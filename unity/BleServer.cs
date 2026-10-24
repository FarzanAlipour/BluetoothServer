using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using AOT;

// Unity wrapper for the BleServer native plugin (include/IBleServer.h).
//
// Threading contract
//   Native callbacks arrive on arbitrary native threads. They are NOT forwarded to user code directly:
//   each one is queued and delivered when the main thread calls BleServer.DispatchEvents().
//   Call DispatchEvents() once per frame (e.g. from a MonoBehaviour.Update). Events (ConnectionChanged,
//   Error, CharacteristicWritten) therefore always run on the thread that calls DispatchEvents().
//   Read providers (SetReadCallback) are the one exception: the central waits for the answer, so the
//   provider runs on the native thread and must be fast and thread-safe.
//
// Callback lifetime (GC / IL2CPP)
//   Native callbacks are STATIC methods marked [MonoPInvokeCallback], referenced by static readonly
//   delegate fields: the delegates can never be garbage collected, and IL2CPP/AOT can marshal them.
//   Instance state travels through the native `userData` pointer as a GCHandle that is freed only after
//   BleServer_Destroy returned (the native side delivers nothing after Shutdown/Destroy).
//
// Marshalling
//   Strings are UTF-8 (LPUTF8Str). Byte arrays are passed with explicit lengths. Out-parameters
//   (`ref int length`) follow the native "query required length, then call again" convention.
namespace BluetoothServer.Unity
{
    public enum BleError
    {
        Success = 0,
        NotSupported = 1,
        NotInitialized = 2,
        AlreadyInitialized = 3,
        InvalidParameter = 4,
        ServiceNotFound = 5,
        CharacteristicNotFound = 6,
        BluetoothDisabled = 7,
        AdvertisingFailed = 8,
        Unknown = 9
    }

    [Flags]
    public enum CharacteristicProperty : uint
    {
        Read = 0x02,
        WriteWithoutResponse = 0x04,
        Write = 0x08,
        Notify = 0x10,
        Indicate = 0x20
    }

    [Flags]
    public enum CharacteristicPermission : uint
    {
        Read = 0x01,
        Write = 0x02,
        ReadEncrypted = 0x04,
        WriteEncrypted = 0x08
    }

    // Mirrors BleServerConfigC.
    [StructLayout(LayoutKind.Sequential)]
    public struct BleServerConfig
    {
        public uint size;
        [MarshalAs(UnmanagedType.LPUTF8Str)] public string deviceName;
        [MarshalAs(UnmanagedType.I1)] public bool requiresPairing;
        [MarshalAs(UnmanagedType.I1)] public bool requiresBonding;
        public int maxConnections;

        public static BleServerConfig Create(string name)
        {
            return new BleServerConfig
            {
                size = (uint)Marshal.SizeOf<BleServerConfig>(),
                deviceName = name,
                requiresPairing = false,
                requiresBonding = false,
                maxConnections = 10
            };
        }
    }

    // Mirrors BleCharacteristicConfigC.
    [StructLayout(LayoutKind.Sequential)]
    internal struct NativeCharacteristicConfig
    {
        public uint size;
        [MarshalAs(UnmanagedType.LPUTF8Str)] public string uuid;
        public uint properties;
        public uint permissions;
        public IntPtr initialValue;
        public int initialValueLength;
        public int maxLength;
    }

    // Mirrors BleDescriptorConfigC.
    [StructLayout(LayoutKind.Sequential)]
    internal struct NativeDescriptorConfig
    {
        public uint size;
        [MarshalAs(UnmanagedType.LPUTF8Str)] public string uuid;
        public uint permissions;
        public IntPtr initialValue;
        public int initialValueLength;
    }

    public sealed class BleServer : IDisposable
    {
        private const string LibraryName = "BleServer";

        // ---- native callback signatures (match the *Ex typedefs in IBleServer.h) ----
        private delegate void ConnectionCallbackEx(IntPtr deviceId, [MarshalAs(UnmanagedType.I1)] bool connected, IntPtr userData);
        private delegate void ErrorCallbackEx(int error, IntPtr message, IntPtr userData);
        private delegate void DataCallbackEx(IntPtr deviceId, IntPtr data, int dataLength, IntPtr userData);
        private delegate int ReadCallback(IntPtr deviceId, IntPtr buffer, int bufferLength, IntPtr userData);

        // Static fields: never collected. See "Callback lifetime" above.
        private static readonly ConnectionCallbackEx ConnectionThunk = OnConnectionNative;
        private static readonly ErrorCallbackEx ErrorThunk = OnErrorNative;
        private static readonly DataCallbackEx WriteThunk = OnWriteNative;
        private static readonly ReadCallback ReadThunk = OnReadNative;

        private sealed class WriteBinding
        {
            public BleServer Owner;
            public string Service;
            public string Characteristic;
        }

        private sealed class ReadBinding
        {
            public Func<string, byte[]> Provider;
        }

        private readonly ConcurrentQueue<Action> mainThreadEvents = new ConcurrentQueue<Action>();
        private readonly List<GCHandle> bindingHandles = new List<GCHandle>();
        private readonly object bindingLock = new object();
        private GCHandle selfHandle;
        private BleChunker chunker;
        private IntPtr server;
        private bool disposed;

        public event Action<string, bool> ConnectionChanged;
        public event Action<BleError, string> Error;
        public event Action<string, string, string, byte[]> CharacteristicWritten;

        public BleServer()
        {
            server = Native.BleServer_Create();
            if (server == IntPtr.Zero)
            {
                throw new InvalidOperationException("Failed to create native BLE server.");
            }

            selfHandle = GCHandle.Alloc(this);
            IntPtr self = GCHandle.ToIntPtr(selfHandle);
            Native.BleServer_SetConnectionCallbackEx(server, ConnectionThunk, self);
            Native.BleServer_SetErrorCallbackEx(server, ErrorThunk, self);
        }

        public static int NativeApiVersion => Native.BleServer_GetApiVersion();
        public int ApiVersion => Native.BleServer_GetApiVersion();
        public string BuildInfo => PtrToUtf8(Native.BleServer_GetBuildInfo());
        public uint Capabilities { get { EnsureAlive(); return Native.BleServer_GetCapabilities(server); } }
        public string LastError { get { EnsureAlive(); return PtrToUtf8(Native.BleServer_GetLastError(server)); } }
        public bool IsInitialized { get { EnsureAlive(); return Native.BleServer_IsInitialized(server); } }
        public bool IsAdvertising { get { EnsureAlive(); return Native.BleServer_IsAdvertising(server); } }

        public string GetStatus()
        {
            EnsureAlive();
            int length = 0;
            Native.BleServer_GetStatus(server, null, ref length);
            if (length <= 1)
            {
                return string.Empty;
            }

            var buffer = new byte[length];
            int actual = length;
            int result = Native.BleServer_GetStatus(server, buffer, ref actual);
            if (result != 0)
            {
                return $"GetStatus failed (code {result})";
            }
            return Encoding.UTF8.GetString(buffer, 0, Math.Max(0, actual - 1));
        }

        public void LogStatus()
        {
            EnsureAlive();
            Native.BleServer_LogStatus(server);
        }

        public BleError Initialize(BleServerConfig config)
        {
            EnsureAlive();
            if (config.size == 0)
            {
                config.size = (uint)Marshal.SizeOf<BleServerConfig>();
            }
            return (BleError)Native.BleServer_InitializeEx(server, ref config);
        }

        public void Shutdown()
        {
            EnsureAlive();
            Native.BleServer_Shutdown(server);
        }

        public BleError AddService(string uuid, bool isPrimary = true)
        {
            EnsureAlive();
            return (BleError)Native.BleServer_AddService(server, uuid, isPrimary);
        }

        public BleError RemoveService(string uuid)
        {
            EnsureAlive();
            return (BleError)Native.BleServer_RemoveService(server, uuid);
        }

        public BleError AddCharacteristic(string serviceUuid, string characteristicUuid,
            CharacteristicProperty properties,
            CharacteristicPermission permissions,
            byte[] initialValue = null,
            int maxLength = 512)
        {
            EnsureAlive();

            GCHandle handle = default;
            try
            {
                IntPtr dataPtr = IntPtr.Zero;
                int dataLength = 0;
                if (initialValue != null && initialValue.Length > 0)
                {
                    handle = GCHandle.Alloc(initialValue, GCHandleType.Pinned);
                    dataPtr = handle.AddrOfPinnedObject();
                    dataLength = initialValue.Length;
                }

                var config = new NativeCharacteristicConfig
                {
                    size = (uint)Marshal.SizeOf<NativeCharacteristicConfig>(),
                    uuid = characteristicUuid,
                    properties = (uint)properties,
                    permissions = (uint)permissions,
                    initialValue = dataPtr,
                    initialValueLength = dataLength,
                    maxLength = maxLength
                };

                return (BleError)Native.BleServer_AddCharacteristicEx(server, serviceUuid, ref config);
            }
            finally
            {
                if (handle.IsAllocated)
                {
                    handle.Free();
                }
            }
        }

        public BleError RemoveCharacteristic(string serviceUuid, string characteristicUuid)
        {
            EnsureAlive();
            return (BleError)Native.BleServer_RemoveCharacteristic(server, serviceUuid, characteristicUuid);
        }

        public BleError AddDescriptor(string serviceUuid, string characteristicUuid, string descriptorUuid,
            CharacteristicPermission permissions, byte[] initialValue = null)
        {
            EnsureAlive();

            GCHandle handle = default;
            try
            {
                IntPtr dataPtr = IntPtr.Zero;
                int dataLength = 0;
                if (initialValue != null && initialValue.Length > 0)
                {
                    handle = GCHandle.Alloc(initialValue, GCHandleType.Pinned);
                    dataPtr = handle.AddrOfPinnedObject();
                    dataLength = initialValue.Length;
                }

                var config = new NativeDescriptorConfig
                {
                    size = (uint)Marshal.SizeOf<NativeDescriptorConfig>(),
                    uuid = descriptorUuid,
                    permissions = (uint)permissions,
                    initialValue = dataPtr,
                    initialValueLength = dataLength
                };
                return (BleError)Native.BleServer_AddDescriptor(server, serviceUuid, characteristicUuid, ref config);
            }
            finally
            {
                if (handle.IsAllocated)
                {
                    handle.Free();
                }
            }
        }

        // Registers (or replaces) the write handler of one characteristic. The CharacteristicWritten
        // event is raised from DispatchEvents().
        public BleError SetWriteCallback(string serviceUuid, string characteristicUuid)
        {
            EnsureAlive();
            var binding = new WriteBinding { Owner = this, Service = serviceUuid, Characteristic = characteristicUuid };
            IntPtr userData = Pin(binding);
            return (BleError)Native.BleServer_SetCharacteristicWriteCallbackEx(
                server, serviceUuid, characteristicUuid, WriteThunk, userData);
        }

        // The provider runs on a native thread (see threading contract).
        public BleError SetReadCallback(string serviceUuid, string characteristicUuid, Func<string, byte[]> provider)
        {
            EnsureAlive();
            if (provider == null)
            {
                throw new ArgumentNullException(nameof(provider));
            }

            IntPtr userData = Pin(new ReadBinding { Provider = provider });
            return (BleError)Native.BleServer_SetCharacteristicReadCallback(
                server, serviceUuid, characteristicUuid, ReadThunk, userData);
        }

        public BleError StartAdvertising()
        {
            EnsureAlive();
            return (BleError)Native.BleServer_StartAdvertising(server);
        }

        public BleError StopAdvertising()
        {
            EnsureAlive();
            return (BleError)Native.BleServer_StopAdvertising(server);
        }

        // Sends a message of any size (up to ~243 KB) as chunks. The chunker is kept for the lifetime of
        // the server so lost ACKs can be retransmitted: feed ACKs to Chunker.ProcessIncoming and call
        // RetransmitTimedOut periodically.
        public BleError SendLargeMessage(string serviceUuid, string characteristicUuid, byte[] payload)
        {
            EnsureAlive();
            return Chunker.SendMessage(server, serviceUuid, characteristicUuid, payload);
        }

        public BleChunker Chunker
        {
            get
            {
                EnsureAlive();
                if (chunker == null)
                {
                    chunker = new BleChunker();
                }
                return chunker;
            }
        }

        // Resends the oldest unacknowledged chunk of messages older than timeoutMs.
        // Returns the number of chunks resent, or a BleError code (4 = invalid argument).
        public int RetransmitTimedOut(int timeoutMs)
        {
            EnsureAlive();
            return Chunker.CleanupTimedOut(timeoutMs);
        }

        public BleError UpdateValue(string serviceUuid, string characteristicUuid, byte[] value)
        {
            EnsureAlive();
            value = value ?? Array.Empty<byte>();
            return (BleError)Native.BleServer_UpdateCharacteristicValue(server, serviceUuid, characteristicUuid, value, value.Length);
        }

        public BleError NotifyValue(string serviceUuid, string characteristicUuid, string deviceId = null)
        {
            EnsureAlive();
            return (BleError)Native.BleServer_NotifyCharacteristicChanged(server, serviceUuid, characteristicUuid, deviceId ?? string.Empty);
        }

        public string[] GetConnectedDevices()
        {
            EnsureAlive();
            int count = Native.BleServer_GetConnectedDeviceCount(server);
            if (count <= 0)
            {
                return Array.Empty<string>();
            }

            var devices = new List<string>(count);
            for (int i = 0; i < count; i++)
            {
                int length = 0;
                Native.BleServer_GetConnectedDeviceId(server, i, null, ref length);
                if (length <= 1)
                {
                    continue;
                }
                var buffer = new byte[length];
                int actual = length;
                if (Native.BleServer_GetConnectedDeviceId(server, i, buffer, ref actual) == 0)
                {
                    devices.Add(Encoding.UTF8.GetString(buffer, 0, Math.Max(0, actual - 1)));
                }
            }
            return devices.ToArray();
        }

        public BleError DisconnectDevice(string deviceId)
        {
            EnsureAlive();
            return (BleError)Native.BleServer_DisconnectDevice(server, deviceId);
        }

        // Delivers queued native events on the calling (main) thread. Call once per frame.
        public void DispatchEvents()
        {
            while (mainThreadEvents.TryDequeue(out var action))
            {
                action();
            }
        }

        public void Dispose()
        {
            if (disposed)
            {
                return;
            }

            disposed = true;
            if (chunker != null)
            {
                chunker.Dispose();
                chunker = null;
            }

            if (server != IntPtr.Zero)
            {
                Native.BleServer_Shutdown(server);   // after this the native side delivers no callbacks
                Native.BleServer_Destroy(server);
                server = IntPtr.Zero;
            }

            // Only now is it safe to release the handles that native userData pointers refer to.
            lock (bindingLock)
            {
                foreach (var handle in bindingHandles)
                {
                    if (handle.IsAllocated)
                    {
                        handle.Free();
                    }
                }
                bindingHandles.Clear();
            }
            if (selfHandle.IsAllocated)
            {
                selfHandle.Free();
            }
        }

        private IntPtr Pin(object binding)
        {
            var handle = GCHandle.Alloc(binding);
            lock (bindingLock)
            {
                bindingHandles.Add(handle);
            }
            return GCHandle.ToIntPtr(handle);
        }

        private void EnsureAlive()
        {
            if (disposed || server == IntPtr.Zero)
            {
                throw new ObjectDisposedException(nameof(BleServer));
            }
        }

        private static string PtrToUtf8(IntPtr ptr)
        {
            return ptr == IntPtr.Zero ? string.Empty : (Marshal.PtrToStringUTF8(ptr) ?? string.Empty);
        }

        private static T Target<T>(IntPtr userData) where T : class
        {
            return userData == IntPtr.Zero ? null : GCHandle.FromIntPtr(userData).Target as T;
        }

        // ---- native callback thunks: static, exception-safe, never touch Unity APIs ----
        [MonoPInvokeCallback(typeof(ConnectionCallbackEx))]
        private static void OnConnectionNative(IntPtr deviceId, bool connected, IntPtr userData)
        {
            try
            {
                var self = Target<BleServer>(userData);
                if (self == null) return;
                string id = PtrToUtf8(deviceId);
                self.mainThreadEvents.Enqueue(() => self.ConnectionChanged?.Invoke(id, connected));
            }
            catch (Exception) { /* never let an exception cross into native code */ }
        }

        [MonoPInvokeCallback(typeof(ErrorCallbackEx))]
        private static void OnErrorNative(int error, IntPtr message, IntPtr userData)
        {
            try
            {
                var self = Target<BleServer>(userData);
                if (self == null) return;
                string text = PtrToUtf8(message);
                self.mainThreadEvents.Enqueue(() => self.Error?.Invoke((BleError)error, text));
            }
            catch (Exception) { }
        }

        [MonoPInvokeCallback(typeof(DataCallbackEx))]
        private static void OnWriteNative(IntPtr deviceId, IntPtr data, int dataLength, IntPtr userData)
        {
            try
            {
                var binding = Target<WriteBinding>(userData);
                if (binding == null) return;
                string id = PtrToUtf8(deviceId);
                var payload = Array.Empty<byte>();
                if (data != IntPtr.Zero && dataLength > 0)
                {
                    payload = new byte[dataLength];
                    Marshal.Copy(data, payload, 0, dataLength);   // copy: the native buffer dies after return
                }
                var owner = binding.Owner;
                owner.mainThreadEvents.Enqueue(() =>
                    owner.CharacteristicWritten?.Invoke(id, binding.Service, binding.Characteristic, payload));
            }
            catch (Exception) { }
        }

        [MonoPInvokeCallback(typeof(ReadCallback))]
        private static int OnReadNative(IntPtr deviceId, IntPtr buffer, int bufferLength, IntPtr userData)
        {
            try
            {
                var binding = Target<ReadBinding>(userData);
                if (binding == null) return -1;
                byte[] payload = binding.Provider(PtrToUtf8(deviceId)) ?? Array.Empty<byte>();
                if (buffer == IntPtr.Zero || bufferLength <= 0)
                {
                    return payload.Length;               // size query
                }
                int copied = Math.Min(bufferLength, payload.Length);
                if (copied > 0)
                {
                    Marshal.Copy(payload, 0, buffer, copied);
                }
                return copied;
            }
            catch (Exception)
            {
                return -1;
            }
        }

        private static class Native
        {
            [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
            public static extern IntPtr BleServer_Create();

            [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
            public static extern void BleServer_Destroy(IntPtr server);

            [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
            public static extern int BleServer_Initialize(IntPtr server, [MarshalAs(UnmanagedType.LPUTF8Str)] string deviceName, [MarshalAs(UnmanagedType.I1)] bool requiresPairing);

            [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
            public static extern int BleServer_InitializeEx(IntPtr server, ref BleServerConfig config);

            [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
            public static extern void BleServer_Shutdown(IntPtr server);

            [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
            [return: MarshalAs(UnmanagedType.I1)]
            public static extern bool BleServer_IsInitialized(IntPtr server);

            [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
            public static extern int BleServer_GetApiVersion();

            [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
            public static extern IntPtr BleServer_GetBuildInfo();

            [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
            public static extern IntPtr BleServer_GetLastError(IntPtr server);

            [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
            public static extern uint BleServer_GetCapabilities(IntPtr server);

            [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
            public static extern int BleServer_GetStatus(IntPtr server, byte[] buffer, ref int bufferLength);

            [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
            public static extern void BleServer_LogStatus(IntPtr server);

            [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
            public static extern int BleServer_AddService(IntPtr server, [MarshalAs(UnmanagedType.LPUTF8Str)] string serviceUuid, [MarshalAs(UnmanagedType.I1)] bool isPrimary);

            [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
            public static extern int BleServer_RemoveService(IntPtr server, [MarshalAs(UnmanagedType.LPUTF8Str)] string serviceUuid);

            [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
            public static extern int BleServer_AddCharacteristic(IntPtr server,
                [MarshalAs(UnmanagedType.LPUTF8Str)] string serviceUuid,
                [MarshalAs(UnmanagedType.LPUTF8Str)] string characteristicUuid,
                uint properties,
                uint permissions);

            [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
            public static extern int BleServer_AddCharacteristicEx(IntPtr server, [MarshalAs(UnmanagedType.LPUTF8Str)] string serviceUuid, ref NativeCharacteristicConfig config);

            [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
            public static extern int BleServer_RemoveCharacteristic(IntPtr server,
                [MarshalAs(UnmanagedType.LPUTF8Str)] string serviceUuid,
                [MarshalAs(UnmanagedType.LPUTF8Str)] string characteristicUuid);

            [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
            public static extern int BleServer_AddDescriptor(IntPtr server,
                [MarshalAs(UnmanagedType.LPUTF8Str)] string serviceUuid,
                [MarshalAs(UnmanagedType.LPUTF8Str)] string characteristicUuid,
                ref NativeDescriptorConfig config);

            // The non-Ex callback variants carry no userData; this wrapper only uses the Ex ones.
            [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
            public static extern void BleServer_SetConnectionCallback(IntPtr server, IntPtr callback);

            [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
            public static extern void BleServer_SetConnectionCallbackEx(IntPtr server, ConnectionCallbackEx callback, IntPtr userData);

            [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
            public static extern void BleServer_SetErrorCallback(IntPtr server, IntPtr callback);

            [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
            public static extern void BleServer_SetErrorCallbackEx(IntPtr server, ErrorCallbackEx callback, IntPtr userData);

            [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
            public static extern int BleServer_SetCharacteristicReadCallback(IntPtr server,
                [MarshalAs(UnmanagedType.LPUTF8Str)] string serviceUuid,
                [MarshalAs(UnmanagedType.LPUTF8Str)] string characteristicUuid,
                ReadCallback callback,
                IntPtr userData);

            [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
            public static extern void BleServer_SetCharacteristicWriteCallback(IntPtr server,
                [MarshalAs(UnmanagedType.LPUTF8Str)] string serviceUuid,
                [MarshalAs(UnmanagedType.LPUTF8Str)] string characteristicUuid,
                IntPtr callback);

            [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
            public static extern int BleServer_SetCharacteristicWriteCallbackEx(IntPtr server,
                [MarshalAs(UnmanagedType.LPUTF8Str)] string serviceUuid,
                [MarshalAs(UnmanagedType.LPUTF8Str)] string characteristicUuid,
                DataCallbackEx callback,
                IntPtr userData);

            [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
            public static extern int BleServer_StartAdvertising(IntPtr server);

            [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
            public static extern int BleServer_StopAdvertising(IntPtr server);

            [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
            [return: MarshalAs(UnmanagedType.I1)]
            public static extern bool BleServer_IsAdvertising(IntPtr server);

            [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
            public static extern int BleServer_UpdateCharacteristicValue(IntPtr server,
                [MarshalAs(UnmanagedType.LPUTF8Str)] string serviceUuid,
                [MarshalAs(UnmanagedType.LPUTF8Str)] string characteristicUuid,
                byte[] value,
                int valueLength);

            [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
            public static extern int BleServer_NotifyCharacteristicChanged(IntPtr server,
                [MarshalAs(UnmanagedType.LPUTF8Str)] string serviceUuid,
                [MarshalAs(UnmanagedType.LPUTF8Str)] string characteristicUuid,
                [MarshalAs(UnmanagedType.LPUTF8Str)] string deviceId);

            [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
            public static extern int BleServer_GetConnectedDeviceCount(IntPtr server);

            [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
            public static extern int BleServer_GetConnectedDeviceId(IntPtr server, int index, byte[] buffer, ref int bufferLength);

            [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
            public static extern int BleServer_DisconnectDevice(IntPtr server, [MarshalAs(UnmanagedType.LPUTF8Str)] string deviceId);
        }
    }

    public sealed class BleChunker : IDisposable
    {
        private const string LibraryName = "BleServer";
        private IntPtr chunker;

        public BleChunker()
        {
            chunker = Native.BleChunker_Create();
            if (chunker == IntPtr.Zero)
            {
                throw new InvalidOperationException("Failed to create native BLE chunker.");
            }
        }

        public BleError SendMessage(IntPtr server, string serviceUuid, string characteristicUuid, byte[] payload)
        {
            EnsureAlive();
            payload = payload ?? Array.Empty<byte>();
            return (BleError)Native.BleChunker_SendMessage(chunker, server, serviceUuid, characteristicUuid, payload, payload.Length);
        }

        // Feeds one received packet (data chunk or ACK). `ack` is the ACK to send back for a data chunk.
        public bool ProcessIncoming(byte[] packet, out byte[] completeMessage, out byte[] ack)
        {
            EnsureAlive();
            completeMessage = null;
            ack = null;
            if (packet == null)
            {
                return false;
            }

            var ackBuffer = new byte[13];
            var ackLength = ackBuffer.Length;
            int result = Native.BleChunker_ProcessIncoming(chunker, packet, packet.Length, ackBuffer, ref ackLength);
            if (ackLength > 0)
            {
                ack = new byte[Math.Min(ackBuffer.Length, ackLength)];
                Array.Copy(ackBuffer, ack, ack.Length);
            }

            int messageSize = Native.BleChunker_GetNextMessageSize(chunker);
            if (messageSize < 0 || (messageSize == 0 && result <= 0))
            {
                return false;
            }

            var buffer = new byte[messageSize];
            var bufferLength = buffer.Length;
            if (Native.BleChunker_ReadNextMessage(chunker, buffer, ref bufferLength))
            {
                completeMessage = buffer;
                return true;
            }

            return false;
        }

        // Retransmits the oldest unacknowledged chunk of every message older than timeoutMs.
        public int CleanupTimedOut(int timeoutMs)
        {
            EnsureAlive();
            return Native.BleChunker_CleanupTimedOut(chunker, timeoutMs);
        }

        public void Dispose()
        {
            if (chunker != IntPtr.Zero)
            {
                Native.BleChunker_Destroy(chunker);
                chunker = IntPtr.Zero;
            }
        }

        private void EnsureAlive()
        {
            if (chunker == IntPtr.Zero)
            {
                throw new ObjectDisposedException(nameof(BleChunker));
            }
        }

        private static class Native
        {
            [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
            public static extern IntPtr BleChunker_Create();

            [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
            public static extern void BleChunker_Destroy(IntPtr chunker);

            [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
            public static extern int BleChunker_SendMessage(IntPtr chunker, IntPtr server, [MarshalAs(UnmanagedType.LPUTF8Str)] string serviceUuid, [MarshalAs(UnmanagedType.LPUTF8Str)] string characteristicUuid, byte[] data, int dataLength);

            [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
            [return: MarshalAs(UnmanagedType.I1)]
            public static extern bool BleChunker_ProcessReceivedData(IntPtr chunker, byte[] data, int dataLength, byte[] completeMessage, ref int completeMessageLength);

            [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
            public static extern int BleChunker_ProcessIncoming(IntPtr chunker, byte[] data, int dataLength, byte[] ackBuffer, ref int ackLength);

            [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
            public static extern int BleChunker_GetNextMessageSize(IntPtr chunker);

            [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
            [return: MarshalAs(UnmanagedType.I1)]
            public static extern bool BleChunker_ReadNextMessage(IntPtr chunker, byte[] completeMessage, ref int completeMessageLength);

            [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
            public static extern int BleChunker_CleanupTimedOut(IntPtr chunker, int timeoutMs);
        }
    }
}
