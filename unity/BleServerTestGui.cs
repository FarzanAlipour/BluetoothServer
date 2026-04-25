using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using UnityEngine;

namespace BluetoothServer.Unity
{
    public sealed class BleServerTestGui : MonoBehaviour
    {
        private const string TestServiceUuid = "FFF0";
        private const string TestCharacteristicUuid = "FFF1";
        private const int MaxLogLines = 200;

        [SerializeField] private string deviceName = "BLETest";
        [SerializeField] private int maxConnections = 5;
        [SerializeField] private bool requiresPairing;
        [SerializeField] private bool requiresBonding;

        private readonly List<string> logs = new List<string>();
        private BleServer server;
        private Vector2 logScroll;
        private bool serviceCreated;
        private bool advertising;
        private string payloadText = "Hello from Unity BLE chunking";

        private void Update()
        {
            server?.DispatchEvents();
        }

        private void OnDestroy()
        {
            ShutdownServer();
        }

        private void OnGUI()
        {
            const int width = 620;
            GUILayout.BeginArea(new Rect(20, 20, width, Screen.height - 40), GUI.skin.box);

            GUILayout.Label("Unity BLE Server Test", GUI.skin.box);
            DrawConfig();
            DrawControls();
            DrawLogs();

            GUILayout.EndArea();
        }

        private void DrawConfig()
        {
            GUILayout.Label("Config");
            deviceName = LabeledTextField("Device Name", deviceName);

            GUILayout.BeginHorizontal();
            GUILayout.Label("Max Connections", GUILayout.Width(130));
            string maxConnectionsText = GUILayout.TextField(maxConnections.ToString(), GUILayout.Width(80));
            if (int.TryParse(maxConnectionsText, out int parsedMaxConnections))
            {
                maxConnections = Mathf.Clamp(parsedMaxConnections, 1, 10);
            }
            GUILayout.EndHorizontal();

            requiresPairing = GUILayout.Toggle(requiresPairing, "Requires Pairing");
            requiresBonding = GUILayout.Toggle(requiresBonding, "Requires Bonding");
        }

        private void DrawControls()
        {
            GUILayout.Space(8);
            GUILayout.Label("Controls");

            GUILayout.BeginHorizontal();
            GUI.enabled = server == null;
            if (GUILayout.Button("Create + Initialize", GUILayout.Height(32)))
            {
                CreateAndInitialize();
            }

            GUI.enabled = server != null && !serviceCreated;
            if (GUILayout.Button("Add Test GATT", GUILayout.Height(32)))
            {
                AddTestGatt();
            }
            GUILayout.EndHorizontal();

            GUILayout.BeginHorizontal();
            GUI.enabled = server != null && serviceCreated && !advertising;
            if (GUILayout.Button("Start Advertising", GUILayout.Height(32)))
            {
                StartAdvertising();
            }

            GUI.enabled = server != null && advertising;
            if (GUILayout.Button("Stop Advertising", GUILayout.Height(32)))
            {
                StopAdvertising();
            }

            GUI.enabled = server != null;
            if (GUILayout.Button("Shutdown", GUILayout.Height(32)))
            {
                ShutdownServer();
            }
            GUILayout.EndHorizontal();

            GUILayout.Space(8);
            GUI.enabled = server != null && serviceCreated;
            payloadText = LabeledTextField("Chunk Payload", payloadText);
            if (GUILayout.Button("Send Chunked Test Message", GUILayout.Height(32)))
            {
                SendChunkedMessage();
            }

            GUI.enabled = true;
            GUILayout.BeginHorizontal();
            if (GUILayout.Button("Log Build Info"))
            {
                LogBuildInfo();
            }

            GUI.enabled = server != null;
            if (GUILayout.Button("Log Status"))
            {
                LogStatus();
            }

            GUI.enabled = true;
            if (GUILayout.Button("Clear Logs"))
            {
                logs.Clear();
            }
            GUILayout.EndHorizontal();
        }

        private void LogStatus()
        {
            if (server == null)
            {
                Log("Server not created");
                return;
            }

            try
            {
                server.LogStatus();
                string status = server.GetStatus();
                if (string.IsNullOrEmpty(status))
                {
                    Log("Status: <empty>");
                    return;
                }

                foreach (string line in status.Split('\n'))
                {
                    Log(line);
                }
            }
            catch (Exception ex)
            {
                Log($"Status exception: {ex.GetType().Name}: {ex.Message}");
            }
        }

        private void DrawLogs()
        {
            GUILayout.Space(8);
            GUILayout.Label("Logs");

            logScroll = GUILayout.BeginScrollView(logScroll, GUI.skin.box, GUILayout.Height(Mathf.Max(220, Screen.height - 430)));
            foreach (string line in logs)
            {
                GUILayout.Label(line);
            }
            GUILayout.EndScrollView();
        }

        private string LabeledTextField(string label, string value)
        {
            GUILayout.BeginHorizontal();
            GUILayout.Label(label, GUILayout.Width(130));
            string nextValue = GUILayout.TextField(value ?? string.Empty);
            GUILayout.EndHorizontal();
            return nextValue;
        }

        private void CreateAndInitialize()
        {
            try
            {
                LogNativePluginPreflight();
                server = new BleServer();
                server.ConnectionChanged += OnConnectionChanged;
                server.Error += OnServerError;
                server.CharacteristicWritten += OnCharacteristicWritten;

                BleServerConfig config = BleServerConfig.Create(deviceName);
                config.maxConnections = maxConnections;
                config.requiresPairing = requiresPairing;
                config.requiresBonding = requiresBonding;

                BleError result = server.Initialize(config);
                Log($"Initialize: {result}");
                LogBuildInfo();

                if (result != BleError.Success)
                {
                    ShutdownServer();
                }
            }
            catch (DllNotFoundException ex)
            {
                Log($"Native plugin not found: {ex.GetType().Name}: {ex.Message}");
                Log("Expected plugin name: BleServer. Copy the native library to Assets/Plugins for this platform.");
                Log("macOS: Assets/Plugins/macOS/libBleServer.dylib");
                Log("Windows: Assets/Plugins/Windows/x86_64/BleServer.dll");
                Log("Linux: Assets/Plugins/Linux/x86_64/libBleServer.so");
                ShutdownServer();
            }
            catch (EntryPointNotFoundException ex)
            {
                Log($"Native symbol missing: {ex.GetType().Name}: {ex.Message}");
                Log("Rebuild and recopy the native plugin; the C# wrapper expects the newer C ABI exports.");
                ShutdownServer();
            }
            catch (Exception ex)
            {
                Log($"Create/init exception: {ex.GetType().Name}: {ex.Message}");
                ShutdownServer();
            }
        }

        private void AddTestGatt()
        {
            BleError serviceResult = server.AddService(TestServiceUuid);
            Log($"Add service {TestServiceUuid}: {serviceResult}");
            if (serviceResult != BleError.Success)
            {
                return;
            }

            byte[] initialValue = Encoding.UTF8.GetBytes("UnityReady");
            BleError characteristicResult = server.AddCharacteristic(
                TestServiceUuid,
                TestCharacteristicUuid,
                CharacteristicProperty.Read | CharacteristicProperty.Write | CharacteristicProperty.Notify,
                CharacteristicPermission.Read | CharacteristicPermission.Write,
                initialValue,
                4096);

            Log($"Add characteristic {TestCharacteristicUuid}: {characteristicResult}");
            serviceCreated = characteristicResult == BleError.Success;

            if (serviceCreated)
            {
                BleError writeCallback = server.SetWriteCallback(TestServiceUuid, TestCharacteristicUuid);
                Log($"Register write callback {TestCharacteristicUuid}: {writeCallback}");

                BleError readCallback = server.SetReadCallback(TestServiceUuid, TestCharacteristicUuid,
                    deviceId => Encoding.UTF8.GetBytes($"UnityReply:{DateTime.Now:HH:mm:ss}"));
                Log($"Register read callback {TestCharacteristicUuid}: {readCallback}");
            }
        }

        private void StartAdvertising()
        {
            BleError result = server.StartAdvertising();
            advertising = result == BleError.Success;
            Log($"Start advertising: {result}");
        }

        private void StopAdvertising()
        {
            BleError result = server.StopAdvertising();
            advertising = false;
            Log($"Stop advertising: {result}");
        }

        private void SendChunkedMessage()
        {
            byte[] payload = Encoding.UTF8.GetBytes(payloadText ?? string.Empty);
            BleError result = server.SendLargeMessage(TestServiceUuid, TestCharacteristicUuid, payload);
            Log($"Send chunked message ({payload.Length} bytes): {result}");
        }

        private void ShutdownServer()
        {
            if (server == null)
            {
                return;
            }

            try
            {
                server.ConnectionChanged -= OnConnectionChanged;
                server.Error -= OnServerError;
                server.CharacteristicWritten -= OnCharacteristicWritten;
                server.Dispose();
                Log("Server shut down");
            }
            catch (Exception ex)
            {
                Log($"Shutdown exception: {ex.Message}");
            }
            finally
            {
                server = null;
                serviceCreated = false;
                advertising = false;
            }
        }

        private void LogBuildInfo()
        {
            if (server == null)
            {
                Log("Server not created");
                return;
            }

            Log($"API version: {server.ApiVersion}");
            Log($"Build info: {server.BuildInfo}");
            Log($"Capabilities: 0x{server.Capabilities:X8}");
        }

        private void LogNativePluginPreflight()
        {
#if UNITY_EDITOR_OSX || UNITY_STANDALONE_OSX
            string pluginPath = Path.Combine(Application.dataPath, "Plugins/macOS/libBleServer.dylib");
#elif UNITY_EDITOR_WIN || UNITY_STANDALONE_WIN
            string pluginPath = Path.Combine(Application.dataPath, "Plugins/Windows/x86_64/BleServer.dll");
#elif UNITY_EDITOR_LINUX || UNITY_STANDALONE_LINUX
            string pluginPath = Path.Combine(Application.dataPath, "Plugins/Linux/x86_64/libBleServer.so");
#else
            string pluginPath = null;
#endif
            if (string.IsNullOrEmpty(pluginPath))
            {
                Log($"Native plugin preflight skipped on unsupported Unity platform: {Application.platform}");
                return;
            }

            bool exists = File.Exists(pluginPath);
            Log($"Native plugin path: {pluginPath}");
            Log($"Native plugin exists: {exists}");
            if (!exists)
            {
                return;
            }

            var info = new FileInfo(pluginPath);
            Log($"Native plugin size: {info.Length} bytes");
            if (info.Length < 1024)
            {
                Log("Plugin file is very small; it may be a broken symlink or placeholder. Recopy a real built binary.");
            }

#if UNITY_EDITOR_OSX || UNITY_STANDALONE_OSX
            Log("If DllNotFoundException still occurs on macOS, check Unity plugin import settings, CPU architecture, and quarantine attributes.");
            Log("Recommended: rebuild after the CMake fix and copy the real libBleServer.dylib, not a symlink.");
#endif
        }

        private void OnConnectionChanged(string deviceId, bool connected)
        {
            Log($"Connection {(connected ? "connected" : "disconnected")}: {deviceId}");
        }

        private void OnServerError(BleError error, string message)
        {
            Log($"Native error {error}: {message}");
        }

        private void OnCharacteristicWritten(string deviceId, string serviceUuid, string characteristicUuid, byte[] data)
        {
            string text = TryDecodeUtf8(data);
            Log($"Write {characteristicUuid} from {deviceId}: {data.Length} bytes" + (text != null ? $" ({text})" : ""));
        }

        private static string TryDecodeUtf8(byte[] data)
        {
            if (data == null || data.Length == 0)
            {
                return null;
            }
            try
            {
                return Encoding.UTF8.GetString(data);
            }
            catch
            {
                return null;
            }
        }

        private void Log(string message)
        {
            string line = $"[{DateTime.Now:HH:mm:ss}] {message}";
            logs.Add(line);
            Debug.Log(line);

            while (logs.Count > MaxLogLines)
            {
                logs.RemoveAt(0);
            }

            logScroll.y = float.MaxValue;
        }
    }
}
