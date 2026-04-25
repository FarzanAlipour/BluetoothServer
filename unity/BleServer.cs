using System;
using System.Collections.Concurrent;
using System.Runtime.InteropServices;
using UnityEngine;

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

    public sealed class BleServer : IDisposable
    {
        private const string LibraryName = "BleServer";
        private readonly ConcurrentQueue<Action> mainThreadEvents = new ConcurrentQueue<Action>();
        private readonly ConnectionCallback connectionCallback;
        private readonly ErrorCallback errorCallback;
        // Hold strong references so the GC can't collect the delegates that
        // we hand to native code; key is "service|characteristic".
        private readonly System.Collections.Generic.Dictionary<string, DataCallback> writeDelegates =
            new System.Collections.Generic.Dictionary<string, DataCallback>();
        private readonly System.Collections.Generic.Dictionary<string, ReadCallbackNative> readDelegates =
            new System.Collections.Generic.Dictionary<string, ReadCallbackNative>();
        private readonly System.Collections.Generic.Dictionary<string, byte[]> readBuffers =
            new System.Collections.Generic.Dictionary<string, byte[]>();
        private IntPtr server;
        private bool disposed;

        public event Action<string, bool> ConnectionChanged;
        public event Action<BleError, string> Error;
        public event Action<string, string, string, byte[]> CharacteristicWritten;

        public BleServer()
        {
            connectionCallback = OnConnectionChangedNative;
            errorCallback = OnErrorNative;
            server = Native.BleServer_Create();
            if (server == IntPtr.Zero)
            {
                throw new InvalidOperationException("Failed to create native BLE server.");
            }

            Native.BleServer_SetConnectionCallback(server, connectionCallback);
            Native.BleServer_SetErrorCallback(server, errorCallback);
        }

        public int ApiVersion => Native.BleServer_GetApiVersion();
        public string BuildInfo => Marshal.PtrToStringAnsi(Native.BleServer_GetBuildInfo()) ?? string.Empty;
        public uint Capabilities => Native.BleServer_GetCapabilities(server);

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
            int strLen = Math.Max(0, actual - 1);
            return System.Text.Encoding.UTF8.GetString(buffer, 0, strLen);
        }

        public void LogStatus()
        {
            EnsureAlive();
            Native.BleServer_LogStatus(server);
        }

        public BleError Initialize(BleServerConfig config)
        {
            EnsureAlive();
            return (BleError)Native.BleServer_InitializeEx(server, ref config);
        }

        public BleError AddService(string uuid, bool isPrimary = true)
        {
            EnsureAlive();
            return (BleError)Native.BleServer_AddService(server, uuid, isPrimary);
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

        public BleError SetWriteCallback(string serviceUuid, string characteristicUuid)
        {
            EnsureAlive();
            string key = serviceUuid + "|" + characteristicUuid;
            DataCallback wrapper = (deviceIdPtr, dataPtr, dataLength) =>
            {
                string deviceId = deviceIdPtr == IntPtr.Zero ? string.Empty : Marshal.PtrToStringUTF8(deviceIdPtr);
                byte[] payload = Array.Empty<byte>();
                if (dataPtr != IntPtr.Zero && dataLength > 0)
                {
                    payload = new byte[dataLength];
                    Marshal.Copy(dataPtr, payload, 0, dataLength);
                }
                mainThreadEvents.Enqueue(() => CharacteristicWritten?.Invoke(deviceId, serviceUuid, characteristicUuid, payload));
            };
            writeDelegates[key] = wrapper;
            Native.BleServer_SetCharacteristicWriteCallback(server, serviceUuid, characteristicUuid, wrapper);
            return BleError.Success;
        }

        public BleError SetReadCallback(string serviceUuid, string characteristicUuid, Func<string, byte[]> provider)
        {
            EnsureAlive();
            if (provider == null)
            {
                throw new ArgumentNullException(nameof(provider));
            }

            string key = serviceUuid + "|" + characteristicUuid;
            ReadCallbackNative wrapper = (deviceIdPtr, buffer, bufferLength, userData) =>
            {
                string deviceId = deviceIdPtr == IntPtr.Zero ? string.Empty : Marshal.PtrToStringUTF8(deviceIdPtr);
                byte[] payload = provider(deviceId) ?? Array.Empty<byte>();
                readBuffers[key] = payload;
                if (buffer == IntPtr.Zero || bufferLength <= 0)
                {
                    return payload.Length;
                }
                int copied = Math.Min(bufferLength, payload.Length);
                if (copied > 0)
                {
                    Marshal.Copy(payload, 0, buffer, copied);
                }
                return copied;
            };
            readDelegates[key] = wrapper;
            return (BleError)Native.BleServer_SetCharacteristicReadCallback(server, serviceUuid, characteristicUuid, wrapper, IntPtr.Zero);
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

        public BleError SendLargeMessage(string serviceUuid, string characteristicUuid, byte[] payload)
        {
            EnsureAlive();
            using (var chunker = new BleChunker())
            {
                return chunker.SendMessage(server, serviceUuid, characteristicUuid, payload);
            }
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
            if (server != IntPtr.Zero)
            {
                Native.BleServer_Shutdown(server);
                Native.BleServer_Destroy(server);
                server = IntPtr.Zero;
            }
        }

        private void OnConnectionChangedNative(string deviceId, bool connected)
        {
            mainThreadEvents.Enqueue(() => ConnectionChanged?.Invoke(deviceId, connected));
        }

        private void OnErrorNative(int error, string message)
        {
            mainThreadEvents.Enqueue(() => Error?.Invoke((BleError)error, message));
        }

        private void EnsureAlive()
        {
            if (disposed || server == IntPtr.Zero)
            {
                throw new ObjectDisposedException(nameof(BleServer));
            }
        }

        private delegate void ConnectionCallback([MarshalAs(UnmanagedType.LPUTF8Str)] string deviceId, [MarshalAs(UnmanagedType.I1)] bool connected);
        private delegate void ErrorCallback(int error, [MarshalAs(UnmanagedType.LPUTF8Str)] string message);
        private delegate void DataCallback(IntPtr deviceId, IntPtr data, int dataLength);
        private delegate int ReadCallbackNative(IntPtr deviceId, IntPtr buffer, int bufferLength, IntPtr userData);

        private static class Native
        {
            [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
            public static extern IntPtr BleServer_Create();

            [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
            public static extern void BleServer_Destroy(IntPtr server);

            [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
            public static extern int BleServer_InitializeEx(IntPtr server, ref BleServerConfig config);

            [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
            public static extern void BleServer_Shutdown(IntPtr server);

            [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
            public static extern int BleServer_GetApiVersion();

            [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
            public static extern IntPtr BleServer_GetBuildInfo();

            [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
            public static extern uint BleServer_GetCapabilities(IntPtr server);

            [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
            public static extern int BleServer_GetStatus(IntPtr server, byte[] buffer, ref int bufferLength);

            [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
            public static extern void BleServer_LogStatus(IntPtr server);

            [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
            public static extern int BleServer_AddService(IntPtr server, [MarshalAs(UnmanagedType.LPUTF8Str)] string serviceUuid, [MarshalAs(UnmanagedType.I1)] bool isPrimary);

            [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
            public static extern int BleServer_AddCharacteristicEx(IntPtr server, [MarshalAs(UnmanagedType.LPUTF8Str)] string serviceUuid, ref NativeCharacteristicConfig config);

            [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
            public static extern void BleServer_SetConnectionCallback(IntPtr server, ConnectionCallback callback);

            [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
            public static extern void BleServer_SetErrorCallback(IntPtr server, ErrorCallback callback);

            [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
            public static extern void BleServer_SetCharacteristicWriteCallback(IntPtr server,
                [MarshalAs(UnmanagedType.LPUTF8Str)] string serviceUuid,
                [MarshalAs(UnmanagedType.LPUTF8Str)] string characteristicUuid,
                DataCallback callback);

            [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
            public static extern int BleServer_SetCharacteristicReadCallback(IntPtr server,
                [MarshalAs(UnmanagedType.LPUTF8Str)] string serviceUuid,
                [MarshalAs(UnmanagedType.LPUTF8Str)] string characteristicUuid,
                ReadCallbackNative callback,
                IntPtr userData);

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
            public static extern int BleServer_StartAdvertising(IntPtr server);

            [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
            public static extern int BleServer_StopAdvertising(IntPtr server);
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
            payload = payload ?? Array.Empty<byte>();
            return (BleError)Native.BleChunker_SendMessage(chunker, server, serviceUuid, characteristicUuid, payload, payload.Length);
        }

        public bool ProcessIncoming(byte[] packet, out byte[] completeMessage, out byte[] ack)
        {
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
                ack = new byte[ackLength];
                Array.Copy(ackBuffer, ack, Math.Min(ackBuffer.Length, ackLength));
            }

            int messageSize = Native.BleChunker_GetNextMessageSize(chunker);
            if (messageSize <= 0)
            {
                return result > 0;
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

        public void Dispose()
        {
            if (chunker != IntPtr.Zero)
            {
                Native.BleChunker_Destroy(chunker);
                chunker = IntPtr.Zero;
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
            public static extern int BleChunker_ProcessIncoming(IntPtr chunker, byte[] data, int dataLength, byte[] ackBuffer, ref int ackLength);

            [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
            public static extern int BleChunker_GetNextMessageSize(IntPtr chunker);

            [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
            [return: MarshalAs(UnmanagedType.I1)]
            public static extern bool BleChunker_ReadNextMessage(IntPtr chunker, byte[] completeMessage, ref int completeMessageLength);
        }
    }
}
