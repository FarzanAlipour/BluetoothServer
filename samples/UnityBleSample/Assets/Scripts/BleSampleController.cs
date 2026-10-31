using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.Text;
using System.Threading;
using BluetoothServer.Unity;
using UnityEngine;
using Debug = UnityEngine.Debug;

namespace BleSample
{
    // Sample BLE peripheral for manual testing with nRF Connect (see samples/UnityBleSample/README.md).
    // Threading contract (unity/README.md): DispatchEvents() runs once per frame in Update(); Dispose()
    // runs in OnDestroy/OnApplicationQuit. The read provider is the only code that runs on a native
    // thread and therefore never touches Unity APIs.
    public sealed class BleSampleController : MonoBehaviour
    {
        private const int MaxLogLines = 300;
        private const float HeartbeatSeconds = 1f;

        private readonly List<string> logs = new List<string>();
        private readonly HashSet<string> connected = new HashSet<string>();
        private readonly Stopwatch uptime = Stopwatch.StartNew();   // thread-safe to read
        private BleServer server;
        private Vector2 logScroll;
        private bool advertising;
        private bool largeSent;
        private int heartbeatCounter;
        private int manualCounter;
        private int largeCounter;
        private float nextHeartbeat;
        private float nextRetransmit;
        private int readCounter;          // incremented on the native thread (Interlocked)
        private int readsLogged;

        private void Start()
        {
            // Keep ticking (DispatchEvents, heartbeat) while the window is unfocused: the BLE client is
            // another device, so the Unity window is usually in the background during a test.
            Application.runInBackground = true;
            CreateServer();
        }

        private void Update()
        {
            if (server == null)
            {
                return;
            }

            server.DispatchEvents();

            int reads = Volatile.Read(ref readCounter);
            if (reads != readsLogged)
            {
                readsLogged = reads;
                Log($"Read served (total {reads})");
            }

            if (connected.Count > 0 && Time.unscaledTime >= nextHeartbeat)
            {
                nextHeartbeat = Time.unscaledTime + HeartbeatSeconds;
                heartbeatCounter++;
                Notify("heartbeat #" + heartbeatCounter, quiet: true);
            }

            if (largeSent && Time.unscaledTime >= nextRetransmit)
            {
                nextRetransmit = Time.unscaledTime + 1f;
                server.RetransmitTimedOut(2000);
            }
        }

        private void OnDestroy()
        {
            DisposeServer();
        }

        private void OnApplicationQuit()
        {
            DisposeServer();
        }

        // ---- server setup ----

        private void CreateServer()
        {
            try
            {
                server = new BleServer();
                server.ConnectionChanged += OnConnectionChanged;
                server.Error += OnServerError;
                server.CharacteristicWritten += OnCharacteristicWritten;

                Log($"Native plugin API v{server.ApiVersion}: {server.BuildInfo}");
                BleError init = server.Initialize(BleServerConfig.Create(BleSampleGatt.AdvertisedName));
                Log($"Initialize: {init}");
                if (init != BleError.Success)
                {
                    Log("Initialize failed. " + SetupHint());
                    return;
                }

                BuildGatt();
            }
            catch (DllNotFoundException ex)
            {
                Log("Native plugin not found: " + ex.Message);
                Log("Build it first: build_plugin.sh (macOS/Linux) or build_plugin.ps1 (Windows).");
                DisposeServer();
            }
            catch (Exception ex)
            {
                Log($"Create/init exception: {ex.GetType().Name}: {ex.Message}");
                DisposeServer();
            }
        }

        private void BuildGatt()
        {
            string svc = BleSampleGatt.ServiceUuid;
            Check("AddService", server.AddService(svc));

            const CharacteristicPermission rw = CharacteristicPermission.Read | CharacteristicPermission.Write;

            Check("Add Read", server.AddCharacteristic(svc, BleSampleGatt.ReadUuid,
                CharacteristicProperty.Read, CharacteristicPermission.Read, Encoding.UTF8.GetBytes("unread")));
            Check("Read callback", server.SetReadCallback(svc, BleSampleGatt.ReadUuid, ReadProvider));

            Check("Add Write", server.AddCharacteristic(svc, BleSampleGatt.WriteUuid,
                CharacteristicProperty.Write | CharacteristicProperty.WriteWithoutResponse, CharacteristicPermission.Write));
            Check("Write callback", server.SetWriteCallback(svc, BleSampleGatt.WriteUuid));

            Check("Add Notify", server.AddCharacteristic(svc, BleSampleGatt.NotifyUuid,
                CharacteristicProperty.Read | CharacteristicProperty.Notify, CharacteristicPermission.Read,
                Encoding.UTF8.GetBytes("heartbeat #0")));

            Check("Add Echo", server.AddCharacteristic(svc, BleSampleGatt.EchoUuid,
                CharacteristicProperty.Write | CharacteristicProperty.WriteWithoutResponse | CharacteristicProperty.Notify,
                rw));
            Check("Echo callback", server.SetWriteCallback(svc, BleSampleGatt.EchoUuid));

            Check("Add Large", server.AddCharacteristic(svc, BleSampleGatt.LargeUuid,
                CharacteristicProperty.Write | CharacteristicProperty.WriteWithoutResponse | CharacteristicProperty.Notify,
                rw));
            Check("Large callback", server.SetWriteCallback(svc, BleSampleGatt.LargeUuid));
        }

        // Runs on a NATIVE thread while the central waits: no Unity API, no allocation-heavy work.
        private byte[] ReadProvider(string deviceId)
        {
            int n = Interlocked.Increment(ref readCounter);
            double seconds = uptime.Elapsed.TotalSeconds;
            return Encoding.UTF8.GetBytes(string.Format(CultureInfo.InvariantCulture, "uptime={0:F1}s read#{1}", seconds, n));
        }

        // ---- events (main thread, via DispatchEvents) ----

        private void OnConnectionChanged(string deviceId, bool isConnected)
        {
            if (isConnected) connected.Add(deviceId); else connected.Remove(deviceId);
            Log($"Central {(isConnected ? "connected" : "disconnected")}: {deviceId}");
        }

        private void OnServerError(BleError error, string message)
        {
            Log($"Native error {error}: {message}");
        }

        private void OnCharacteristicWritten(string deviceId, string serviceUuid, string characteristicUuid, byte[] data)
        {
            string hex = ToHex(data);
            string text = ToUtf8(data);

            if (Same(characteristicUuid, BleSampleGatt.EchoUuid))
            {
                Log($"Echo write {data.Length} B hex={hex} text=\"{text}\" -> notify back");
                if (server.UpdateValue(BleSampleGatt.ServiceUuid, BleSampleGatt.EchoUuid, data) == BleError.Success)
                {
                    Check("Echo notify", server.NotifyValue(BleSampleGatt.ServiceUuid, BleSampleGatt.EchoUuid));
                }
            }
            else if (Same(characteristicUuid, BleSampleGatt.LargeUuid))
            {
                bool complete = server.Chunker.ProcessIncoming(data, out byte[] message, out byte[] ack);
                Log($"Large char write {data.Length} B hex={hex} (chunker: {(complete ? "message complete, " + message.Length + " B" : "packet consumed")})");
                if (ack != null && ack.Length > 0 && server.UpdateValue(BleSampleGatt.ServiceUuid, BleSampleGatt.LargeUuid, ack) == BleError.Success)
                {
                    server.NotifyValue(BleSampleGatt.ServiceUuid, BleSampleGatt.LargeUuid);
                }
            }
            else
            {
                Log($"Write {data.Length} B hex={hex} text=\"{text}\"");
            }
        }

        // ---- actions (buttons) ----

        private void ToggleAdvertising()
        {
            if (advertising)
            {
                BleError r = server.StopAdvertising();
                advertising = false;
                Log("Stop advertising: " + r);
            }
            else
            {
                BleError r = server.StartAdvertising();
                advertising = r == BleError.Success;
                Log($"Start advertising as \"{BleSampleGatt.AdvertisedName}\": {r}");
                if (!advertising) Log(SetupHint());
            }
        }

        private void Notify(string text, bool quiet)
        {
            byte[] value = Encoding.UTF8.GetBytes(text);
            BleError update = server.UpdateValue(BleSampleGatt.ServiceUuid, BleSampleGatt.NotifyUuid, value);
            BleError notify = update == BleError.Success
                ? server.NotifyValue(BleSampleGatt.ServiceUuid, BleSampleGatt.NotifyUuid)
                : update;
            if (!quiet || notify != BleError.Success)
            {
                Log($"Notify \"{text}\": {notify}");
            }
        }

        private void SendNotifyNow()
        {
            manualCounter++;
            Notify("manual #" + manualCounter, quiet: false);
        }

        private void SendLarge()
        {
            largeCounter++;
            var sb = new StringBuilder();
            sb.Append("BoostanBLE large message #").Append(largeCounter).Append(": ");
            while (sb.Length < 1000)
            {
                sb.Append("0123456789abcdefghijklmnopqrstuvwxyz ");
            }
            byte[] payload = Encoding.UTF8.GetBytes(sb.ToString());
            BleError r = server.SendLargeMessage(BleSampleGatt.ServiceUuid, BleSampleGatt.LargeUuid, payload);
            largeSent = largeSent || r == BleError.Success;
            Log($"Send large message ({payload.Length} B, ~{(payload.Length + 242) / 243} chunks): {r}");
        }

        // ---- UI (IMGUI: no extra packages needed) ----

        private void OnGUI()
        {
            const int pad = 20;
            GUI.skin.label.wordWrap = true;
            GUI.skin.label.fontSize = 14;
            GUI.skin.button.fontSize = 14;
            GUILayout.BeginArea(new Rect(pad, pad, Screen.width - 2 * pad, Screen.height - 2 * pad), GUI.skin.box);

            GUILayout.Label("BoostanBLE Sample (BLE GATT server)", GUI.skin.box);
            GUILayout.Label(StatusLine());

            bool ready = server != null && server.IsInitialized;
            GUILayout.BeginHorizontal();
            GUI.enabled = ready;
            if (GUILayout.Button(advertising ? "Stop advertising" : "Start advertising", GUILayout.Height(36)))
            {
                ToggleAdvertising();
            }
            if (GUILayout.Button("Send notify", GUILayout.Height(36)))
            {
                SendNotifyNow();
            }
            if (GUILayout.Button("Send large message", GUILayout.Height(36)))
            {
                SendLarge();
            }
            GUI.enabled = true;
            if (GUILayout.Button("Clear log", GUILayout.Height(36)))
            {
                logs.Clear();
            }
            GUILayout.EndHorizontal();

            GUILayout.Space(6);
            logScroll = GUILayout.BeginScrollView(logScroll, GUI.skin.box);
            foreach (string line in logs)
            {
                GUILayout.Label(line);
            }
            GUILayout.EndScrollView();

            GUILayout.EndArea();
        }

        private string StatusLine()
        {
            if (server == null)
            {
                return "Status: server not created (see log)";
            }
            string devices = connected.Count == 0 ? "none" : string.Join(", ", connected);
            return $"Status: initialized={server.IsInitialized}  advertising={advertising}  connected={devices}";
        }

        // ---- helpers ----

        private void DisposeServer()
        {
            if (server == null)
            {
                return;
            }
            BleServer s = server;
            server = null;
            advertising = false;
            connected.Clear();
            try
            {
                s.ConnectionChanged -= OnConnectionChanged;
                s.Error -= OnServerError;
                s.CharacteristicWritten -= OnCharacteristicWritten;
                s.Dispose();
            }
            catch (Exception ex)
            {
                Debug.LogWarning("BLE dispose: " + ex.Message);
            }
        }

        private void Check(string what, BleError result)
        {
            if (result != BleError.Success)
            {
                Log($"{what}: {result}");
            }
        }

        private static bool Same(string a, string b)
        {
            return string.Equals(a, b, StringComparison.OrdinalIgnoreCase);
        }

        private static string SetupHint()
        {
            switch (Application.platform)
            {
                case RuntimePlatform.OSXEditor:
                case RuntimePlatform.OSXPlayer:
                    return "macOS: allow Bluetooth for Unity/Unity Hub (System Settings > Privacy & Security > Bluetooth) and make sure Bluetooth is on.";
                case RuntimePlatform.WindowsEditor:
                case RuntimePlatform.WindowsPlayer:
                    return "Windows: Bluetooth must be on and the adapter must support the LE peripheral role.";
                default:
                    return "Linux: bluetoothd (BlueZ >= 5.50) must run, Bluetooth unblocked (rfkill), and the user allowed on the system D-Bus.";
            }
        }

        public static string ToHex(byte[] data)
        {
            return data == null || data.Length == 0 ? "<empty>" : BitConverter.ToString(data).Replace('-', ' ');
        }

        public static string ToUtf8(byte[] data)
        {
            if (data == null || data.Length == 0)
            {
                return string.Empty;
            }
            return new UTF8Encoding(false, false).GetString(data);   // invalid bytes become U+FFFD
        }

        private void Log(string message)
        {
            string line = $"[{DateTime.Now:HH:mm:ss}] {message}";
            logs.Add(line);
            Debug.Log("[BleSample] " + message);
            while (logs.Count > MaxLogLines)
            {
                logs.RemoveAt(0);
            }
            logScroll.y = float.MaxValue;
        }
    }
}
