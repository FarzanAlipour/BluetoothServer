#pragma once
#include "BleMessageChunker.h"
#include <map>
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <cstdint>

// Cross-platform export macro
#ifdef _WIN32
#ifdef BLE_SERVER_EXPORTS
#define BLE_API __declspec(dllexport)
#else
#define BLE_API __declspec(dllimport)
#endif
#else
#define BLE_API __attribute__((visibility("default")))
#endif


namespace BleServer {

    // Forward declarations
    class IBleService;
    class IBleCharacteristic;

    // Enums
    enum class BleError {
        Success = 0,
        NotSupported,
        NotInitialized,
        AlreadyInitialized,
        InvalidParameter,
        ServiceNotFound,
        CharacteristicNotFound,
        BluetoothDisabled,
        AdvertisingFailed,
        Unknown,
        // API v3 (BleLink_*). Older entry points never return these.
        PeerNotReady,     // 10: the peer has no READY BleLink session (unknown, handshaking or gone)
        MessageTooLarge,  // 11: longer than the peer's advertised maxMessage
        BufferTooSmall    // 12: BleLink_PollEvent: the caller's buffer cannot hold the payload
    };

    enum class CharacteristicProperty {
        Read = 0x02,
        Write = 0x08,
        Notify = 0x10,
        Indicate = 0x20,
        WriteWithoutResponse = 0x04
    };

    enum class CharacteristicPermission {
        Read = 0x01,
        Write = 0x02,
        ReadEncrypted = 0x04,
        WriteEncrypted = 0x08
    };

    enum class ServerCapability {
        Advertising = 0x00000001,
        Connections = 0x00000002,
        Read = 0x00000004,
        Write = 0x00000008,
        Notify = 0x00000010,
        Indicate = 0x00000020,
        Descriptors = 0x00000040,
        Pairing = 0x00000080,
        Bonding = 0x00000100,
        Encryption = 0x00000200,
        Disconnect = 0x00000400,
        Chunking = 0x00000800,
        // Per-central transport (see "Per-central transport" below): NotifyPeer with busy/ready flow
        // control, per-central subscription events and max notification payload reporting.
        PeerNotify = 0x00001000
    };

    // Result of IBleServer::NotifyPeer (one notification to one central).
    enum class NotifyStatus {
        Sent = 0,       // the OS accepted the notification for this central
        Busy,           // OS transmit queue full: NOTHING was sent; the ReadyToSend callback follows
        NotSubscribed,  // the central is not (or no longer) subscribed to this characteristic
        TooLarge,       // longer than the central's current max notification payload: nothing sent
        NotSupported,   // this backend has no per-central notify
        Error           // not initialized, unknown characteristic, invalid argument, OS error
    };

    // Callbacks
    using ConnectionCallback = std::function<void(const std::string& deviceId, bool connected)>;
    using CharacteristicReadCallback = std::function<std::vector<uint8_t>(const std::string& deviceId)>;
    using CharacteristicWriteCallback = std::function<void(const std::string& deviceId, const std::vector<uint8_t>& data)>;
    using ErrorCallback = std::function<void(BleError error, const std::string& message)>;
    // A central enabled (subscribed=true) or disabled notifications/indications of one characteristic.
    using SubscriptionCallback = std::function<void(const std::string& serviceUuid,
        const std::string& characteristicUuid, const std::string& deviceId, bool subscribed)>;
    // NotifyPeer may succeed again for deviceId; an empty deviceId means "every central".
    using ReadyToSendCallback = std::function<void(const std::string& deviceId)>;
    // The OS reported (or changed) the largest notification value it accepts for deviceId
    // (ATT_MTU - 3). Never 0.
    using MaxPayloadCallback = std::function<void(const std::string& deviceId, size_t maxPayload)>;

    // Configuration structures
    struct ServerConfig {
        std::string deviceName = "BLE Server";
        bool requiresPairing = false;
        bool requiresBonding = false;
        int maxConnections = 10;
    };

    struct ServiceConfig {
        std::string uuid;
        bool isPrimary = true;
    };

    struct CharacteristicConfig {
        std::string uuid;
        uint32_t properties = 0;  // Combination of CharacteristicProperty flags
        uint32_t permissions = 0;  // Combination of CharacteristicPermission flags
        std::vector<uint8_t> initialValue;
        size_t maxLength = 512;
    };

    struct DescriptorConfig {
        std::string uuid;
        uint32_t permissions = 0;  // Combination of CharacteristicPermission flags
        std::vector<uint8_t> initialValue;
    };

    // Main BLE Server interface
    class BLE_API IBleServer {
    public:
        virtual ~IBleServer() = default;

        // Lifecycle
        virtual BleError Initialize(const ServerConfig& config) = 0;
        virtual void Shutdown() = 0;
        virtual bool IsInitialized() const = 0;

        // Service management
        virtual BleError AddService(const ServiceConfig& config) = 0;
        virtual BleError RemoveService(const std::string& serviceUuid) = 0;
        virtual std::vector<std::string> GetServices() const = 0;

        // Characteristic management
        virtual BleError AddCharacteristic(const std::string& serviceUuid,
            const CharacteristicConfig& config) = 0;
        virtual BleError RemoveCharacteristic(const std::string& serviceUuid,
            const std::string& characteristicUuid) = 0;

        virtual BleError AddDescriptor(const std::string& serviceUuid,
            const std::string& characteristicUuid,
            const DescriptorConfig& config) {
            (void)serviceUuid;
            (void)characteristicUuid;
            (void)config;
            return BleError::NotSupported;
        }

        // Characteristic callbacks
        virtual BleError SetCharacteristicReadCallback(const std::string& serviceUuid,
            const std::string& characteristicUuid,
            CharacteristicReadCallback callback) = 0;
        virtual BleError SetCharacteristicWriteCallback(const std::string& serviceUuid,
            const std::string& characteristicUuid,
            CharacteristicWriteCallback callback) = 0;

        // Advertising
        virtual BleError StartAdvertising() = 0;
        virtual BleError StopAdvertising() = 0;
        virtual bool IsAdvertising() const = 0;

        // Connection management
        virtual void SetConnectionCallback(ConnectionCallback callback) = 0;
        virtual std::vector<std::string> GetConnectedDevices() const = 0;
        virtual BleError DisconnectDevice(const std::string& deviceId) = 0;

        // Data operations
        virtual BleError UpdateCharacteristicValue(const std::string& serviceUuid,
            const std::string& characteristicUuid,
            const std::vector<uint8_t>& value) = 0;
        // Sends the characteristic's current value (as set by UpdateCharacteristicValue) to the
        // subscribed centrals. The value is captured at call time.
        // Flow control: if the OS transmit queue is full (macOS: updateValue returned NO), the
        // backend keeps the captured value in a bounded FIFO and sends it, in call order, once the
        // OS reports room again. Such a call still returns Success: Success means "sent or queued",
        // not "delivered". Queued values are dropped on Shutdown, when the characteristic or its
        // service is removed, and when the last central unsubscribes from the characteristic; on
        // overflow (256 values) the oldest is dropped and reported through the error callback.
        // Backlog start/drain is reported through the error callback with BleError::Success.
        // Backends without OS-level flow control send immediately (unchanged behaviour).
        virtual BleError NotifyCharacteristicChanged(const std::string& serviceUuid,
            const std::string& characteristicUuid,
            const std::string& deviceId = "") = 0;

        // Error handling
        virtual void SetErrorCallback(ErrorCallback callback) = 0;

        // ---- Per-central transport (capability ServerCapability::PeerNotify) ----
        // What a message protocol engine (BleLink, docs/PROTOCOL.md) needs from a backend. Defaults
        // keep older backends compiling; they report NotSupported / nothing.
        //
        // NotifyPeer: sends `data` as ONE notification of the characteristic to ONE subscribed
        // central (not broadcast) and returns at once. Unlike NotifyCharacteristicChanged it does not
        // touch the stored characteristic value and never queues: when the OS cannot take the value
        // it returns Busy and nothing was sent (no partial send, no hidden copy). After a Busy for a
        // central, the backend calls the ReadyToSend callback for that central (or with "" for every
        // central) at least once, unless the central unsubscribes/disconnects or Shutdown() runs.
        // Spurious ReadyToSend calls are allowed: a caller just tries again and may get Busy again.
        //   macOS:   updateValue:forCharacteristic:onSubscribedCentrals:@[central]; NO -> Busy,
        //            peripheralManagerIsReadyToUpdateSubscribers: -> ReadyToSend("").
        //   Windows: NotifyValueAsync(value, GattSubscribedClient); one in flight per central,
        //            a second call -> Busy, completion -> ReadyToSend(deviceId).
        //   Linux:   write() to the AcquireNotify socket of that central; EAGAIN -> Busy,
        //            POLLOUT -> ReadyToSend(deviceId).
        // TooLarge when length exceeds GetMaxNotifyPayload(deviceId) (when it is known).
        virtual NotifyStatus NotifyPeer(const std::string& serviceUuid,
            const std::string& characteristicUuid,
            const std::string& deviceId,
            const uint8_t* data,
            size_t length) {
            (void)serviceUuid;
            (void)characteristicUuid;
            (void)deviceId;
            (void)data;
            (void)length;
            return NotifyStatus::NotSupported;
        }

        // Largest notification value the OS accepts for this central (ATT_MTU - 3), 0 = not known
        // (a caller then uses 20, which is always safe). Also reported through the MaxPayload
        // callback when it first becomes known and whenever it changes.
        virtual size_t GetMaxNotifyPayload(const std::string& deviceId) const {
            (void)deviceId;
            return 0;
        }

        // Centrals currently subscribed to the characteristic (per-central bookkeeping).
        virtual std::vector<std::string> GetSubscribers(const std::string& serviceUuid,
            const std::string& characteristicUuid) const {
            (void)serviceUuid;
            (void)characteristicUuid;
            return {};
        }

        // Callback rules (all three, and the write callback): they run on the backend's BLE thread
        // with no backend lock held, so they may call any IBleServer method (NotifyPeer included).
        // They must not block for long. Writes (with and without response) of one central reach the
        // characteristic's write callback one at a time, in the order the OS delivered them, with
        // that central's deviceId.
        // Subscribe/unsubscribe per characteristic per central. The connection callback keeps its
        // old meaning on every backend.
        virtual void SetSubscriptionCallback(SubscriptionCallback callback) { (void)callback; }
        virtual void SetReadyToSendCallback(ReadyToSendCallback callback) { (void)callback; }
        virtual void SetMaxPayloadCallback(MaxPayloadCallback callback) { (void)callback; }

        // Diagnostics: optional human-readable platform status. Default is
        // empty so platforms can opt in without breaking the ABI.
        virtual std::string DescribeStatus() const {
            return std::string();
        }

        virtual uint32_t GetCapabilities() const {
            return static_cast<uint32_t>(ServerCapability::Advertising) |
                static_cast<uint32_t>(ServerCapability::Connections) |
                static_cast<uint32_t>(ServerCapability::Read) |
                static_cast<uint32_t>(ServerCapability::Write) |
                static_cast<uint32_t>(ServerCapability::Notify) |
                static_cast<uint32_t>(ServerCapability::Indicate) |
                static_cast<uint32_t>(ServerCapability::Chunking);
        }

        // Factory method
        static std::unique_ptr<IBleServer> Create();
    };

} // namespace BleServer

// C API for Unity integration
extern "C" {
    struct BleServerConfigC {
        uint32_t size;
        const char* deviceName;
        bool requiresPairing;
        bool requiresBonding;
        int maxConnections;
    };

    struct BleCharacteristicConfigC {
        uint32_t size;
        const char* uuid;
        uint32_t properties;
        uint32_t permissions;
        const uint8_t* initialValue;
        int initialValueLength;
        int maxLength;
    };

    struct BleDescriptorConfigC {
        uint32_t size;
        const char* uuid;
        uint32_t permissions;
        const uint8_t* initialValue;
        int initialValueLength;
    };

    // Server lifecycle
    BLE_API void* BleServer_Create();
    BLE_API void BleServer_Destroy(void* server);
    BLE_API int BleServer_Initialize(void* server, const char* deviceName, bool requiresPairing);
    BLE_API int BleServer_InitializeEx(void* server, const BleServerConfigC* config);
    BLE_API void BleServer_Shutdown(void* server);
    BLE_API bool BleServer_IsInitialized(void* server);
    BLE_API int BleServer_GetApiVersion();
    BLE_API const char* BleServer_GetBuildInfo();
    BLE_API const char* BleServer_GetLastError(void* server);
    BLE_API uint32_t BleServer_GetCapabilities(void* server);
    BLE_API int BleServer_GetStatus(void* server, char* buffer, int* bufferLength);
    BLE_API void BleServer_LogStatus(void* server);

    // Service management
    BLE_API int BleServer_AddService(void* server, const char* serviceUuid, bool isPrimary);
    BLE_API int BleServer_RemoveService(void* server, const char* serviceUuid);

    // Characteristic management
    BLE_API int BleServer_AddCharacteristic(void* server,
        const char* serviceUuid,
        const char* characteristicUuid,
        uint32_t properties,
        uint32_t permissions);
    BLE_API int BleServer_AddCharacteristicEx(void* server,
        const char* serviceUuid,
        const BleCharacteristicConfigC* config);
    BLE_API int BleServer_RemoveCharacteristic(void* server,
        const char* serviceUuid,
        const char* characteristicUuid);
    BLE_API int BleServer_AddDescriptor(void* server,
        const char* serviceUuid,
        const char* characteristicUuid,
        const BleDescriptorConfigC* config);

    // Callbacks (using function pointers for Unity interop)
    typedef void (*ConnectionCallbackFn)(const char* deviceId, bool connected);
    typedef void (*DataCallbackFn)(const char* deviceId, const uint8_t* data, int dataLength);
    typedef void (*ErrorCallbackFn)(int error, const char* message);
    typedef void (*ConnectionCallbackExFn)(const char* deviceId, bool connected, void* userData);
    typedef int (*ReadCallbackFn)(const char* deviceId, uint8_t* buffer, int bufferLength, void* userData);
    typedef void (*DataCallbackExFn)(const char* deviceId, const uint8_t* data, int dataLength, void* userData);
    typedef void (*ErrorCallbackExFn)(int error, const char* message, void* userData);

    BLE_API void BleServer_SetConnectionCallback(void* server, ConnectionCallbackFn callback);
    BLE_API void BleServer_SetConnectionCallbackEx(void* server, ConnectionCallbackExFn callback, void* userData);
    BLE_API void BleServer_SetErrorCallback(void* server, ErrorCallbackFn callback);
    BLE_API void BleServer_SetErrorCallbackEx(void* server, ErrorCallbackExFn callback, void* userData);
    BLE_API int BleServer_SetCharacteristicReadCallback(void* server,
        const char* serviceUuid,
        const char* characteristicUuid,
        ReadCallbackFn callback,
        void* userData);
    BLE_API void BleServer_SetCharacteristicWriteCallback(void* server,
        const char* serviceUuid,
        const char* characteristicUuid,
        DataCallbackFn callback);
    BLE_API int BleServer_SetCharacteristicWriteCallbackEx(void* server,
        const char* serviceUuid,
        const char* characteristicUuid,
        DataCallbackExFn callback,
        void* userData);

    // Advertising
    BLE_API int BleServer_StartAdvertising(void* server);
    BLE_API int BleServer_StopAdvertising(void* server);
    BLE_API bool BleServer_IsAdvertising(void* server);

    // Data operations
    BLE_API int BleServer_UpdateCharacteristicValue(void* server,
        const char* serviceUuid,
        const char* characteristicUuid,
        const uint8_t* value,
        int valueLength);
    // Returns 0 when the notification was sent OR queued by the backend's flow control (see
    // IBleServer::NotifyCharacteristicChanged); it never means the central has received it.
    BLE_API int BleServer_NotifyCharacteristicChanged(void* server,
        const char* serviceUuid,
        const char* characteristicUuid,
        const char* deviceId);
    BLE_API int BleServer_GetConnectedDeviceCount(void* server);
    BLE_API int BleServer_GetConnectedDeviceId(void* server, int index, char* buffer, int* bufferLength);
    BLE_API int BleServer_DisconnectDevice(void* server, const char* deviceId);

    // ---------------------------------------------------------------- BleLink (API version 3)
    // Messages of any size (0 bytes .. maxMessage) in both directions with every connected central,
    // using the BleLink v1 protocol (docs/PROTOCOL.md): one service with RX (write), TX (notify) and
    // INFO (read) characteristics, per-central sessions, flow control, CRC, loss repair.
    // Contract and notes: docs/BLELINK_ABI.md.
    //
    // Lifecycle: BleLink_Create after BleServer_Initialize and BEFORE BleServer_StartAdvertising (so
    // the service UUID is in the advertisement). At most one link per server. BleServer_Shutdown or
    // BleServer_Destroy shuts the link down (unsent/unacknowledged messages fail with reason Shutdown;
    // those events stay pollable); after a re-Initialize create a new link. BleLink_Destroy removes
    // the service. Every entry point validates its handle (null, unknown, destroyed -> error) and is
    // safe from any thread, also after Shutdown/Destroy; no exception crosses the ABI.
    //
    // Events are POLLED (no native->managed callbacks; fine for IL2CPP): call BleLink_PollEvent
    // from one thread (Unity: the main thread, every frame) until it returns 0. Events queue up
    // without bound until polled. The protocol runs on a native worker thread, so polling rate only
    // affects event latency, not throughput.
    //
    // Do not set your own write/read callbacks on the BleLink characteristics.

#define BLELINK_SERVICE_UUID "7b1e1000-5a6c-4f0e-9d3b-2c1a8e4f6b01"
#define BLELINK_RX_UUID      "7b1e1001-5a6c-4f0e-9d3b-2c1a8e4f6b01"
#define BLELINK_TX_UUID      "7b1e1002-5a6c-4f0e-9d3b-2c1a8e4f6b01"
#define BLELINK_INFO_UUID    "7b1e1003-5a6c-4f0e-9d3b-2c1a8e4f6b01"
#define BLELINK_DEVICE_ID_CAPACITY 256  // bytes incl. NUL; longer device ids are not supported

    // BleLinkConfigC.flags
    enum BleLinkConfigFlags {
        BLELINK_FLAG_NO_PROGRESS = 0x1,  // do not ask peers for PROGRESS packets (HELLO flags bit 0 = 0)
        BLELINK_FLAG_ENCRYPTED = 0x2     // RX/TX/INFO require an encrypted link (ReadEncrypted/WriteEncrypted)
    };

    // All fields 0 = default. `size` = sizeof(BleLinkConfigC) (a smaller size from an older caller:
    // the missing trailing fields take their defaults). Out-of-range values -> InvalidParameter.
    struct BleLinkConfigC {
        uint32_t size;
        uint32_t maxMessage;  // largest message we accept, 1..0x7FFFFFFF; 0 = 16 MiB
        uint32_t window;      // messages a peer may have in flight to us, 1..255; 0 = 4
        uint32_t maxPacket;   // largest packet we accept, 20..512; 0 = 512
        uint32_t timeoutMs;   // idle/ACK timeout we need, 1..63750 (rounded up to 250 ms); 0 = 10 s
        uint32_t flags;       // BleLinkConfigFlags; unknown bits -> InvalidParameter
    };

    enum BleLinkEventType {
        BLELINK_EVENT_NONE = 0,
        BLELINK_EVENT_PEER_READY = 1,        // session READY (also after a reset); info is set
        BLELINK_EVENT_PEER_GONE = 2,         // session left READY; reason is set
        BLELINK_EVENT_MESSAGE_RECEIVED = 3,  // payload = the verified message; wireMsgId is set
        BLELINK_EVENT_MESSAGE_SENT = 4,      // the peer ACKed messageId
        BLELINK_EVENT_MESSAGE_FAILED = 5,    // messageId failed; reason (and ackStatus) set
        BLELINK_EVENT_PROGRESS = 6           // progressKind, bytes, total
    };

    enum BleLinkFailReason {
        BLELINK_REASON_NONE = 0,
        BLELINK_REASON_DISCONNECTED = 1,
        BLELINK_REASON_RESET = 2,
        BLELINK_REASON_TIMEOUT = 3,
        BLELINK_REASON_CRC = 4,
        BLELINK_REASON_TOO_LARGE = 5,
        BLELINK_REASON_BUSY = 6,
        BLELINK_REASON_REJECTED = 7,
        BLELINK_REASON_LINK_UNRELIABLE = 8,
        BLELINK_REASON_CANCELLED = 9,
        BLELINK_REASON_NOT_READY = 10,
        BLELINK_REASON_SHUTDOWN = 11
    };

    enum BleLinkProgressKind {
        BLELINK_PROGRESS_NONE = 0,
        BLELINK_PROGRESS_SENDING = 1,        // outgoing: bytes handed to the OS
        BLELINK_PROGRESS_PEER_RECEIVED = 2,  // outgoing: bytes the peer reported (PROGRESS packet)
        BLELINK_PROGRESS_RECEIVING = 3       // incoming: bytes received so far
    };

    // 56 bytes, every field 4 bytes. `size` = sizeof(BleLinkPeerInfoC).
    struct BleLinkPeerInfoC {
        uint32_t size;
        uint32_t subscribed;    // 1 = notifications on TX enabled
        uint32_t ready;         // 1 = session READY (Send allowed)
        uint32_t txPacketSize;  // packet size we send with (20..512)
        uint32_t osLimit;       // OS max notification payload (ATT_MTU - 3), 0 = not reported yet
        uint32_t peerVersion;   // the peer's HELLO, once received
        uint32_t peerMaxPacket;
        uint32_t peerMaxMessage;
        uint32_t peerWindow;
        uint32_t peerFlags;
        uint32_t timeoutMs;     // effective T
        uint32_t queued;        // our messages to this peer not started yet
        uint32_t inFlight;      // our messages started, not ACKed
        uint32_t receiving;     // incomplete incoming messages
    };

    // 368 bytes, no implicit padding (offsets in docs/BLELINK_ABI.md). `size` = sizeof(BleLinkEventC).
    struct BleLinkEventC {
        uint32_t size;
        int32_t type;          // BleLinkEventType
        uint64_t bytes;        // PROGRESS
        uint64_t total;        // PROGRESS
        uint32_t messageId;    // outgoing messages: the id returned by BleLink_Send; 0 for incoming
        uint32_t wireMsgId;    // protocol msgId (per session and direction)
        int32_t reason;        // BleLinkFailReason (PEER_GONE, MESSAGE_FAILED)
        int32_t ackStatus;     // MESSAGE_FAILED from a peer ACK: its status (1 crc, 2 too large, 3 busy, ...)
        int32_t progressKind;  // BleLinkProgressKind
        int32_t dataLength;    // payload bytes of this event (MESSAGE_RECEIVED), else 0
        BleLinkPeerInfoC info; // PEER_READY: negotiated values
        uint32_t deviceIdLength;                    // bytes, without NUL
        char deviceId[BLELINK_DEVICE_ID_CAPACITY];  // UTF-8, NUL-terminated
        uint32_t reserved;
    };

    // Registers the BleLink service on an initialized server. NULL config = defaults. Returns the link
    // handle, or NULL (reason in BleServer_GetLastError(server)): invalid server/config, server not
    // initialized, a link already exists, backend without per-central notify, service registration failed.
    BLE_API void* BleLink_Create(void* server, const BleLinkConfigC* config);
    // Shuts the link down (pending messages fail, peers get CANCEL best effort), removes the service
    // and frees the handle. Second call / NULL / stale handle: no-op. Unpolled events are discarded.
    BLE_API void BleLink_Destroy(void* link);
    // Queues one message (copied) for one READY peer. *outMsgId (optional) = id used by the
    // MESSAGE_SENT / MESSAGE_FAILED / PROGRESS events, never 0. Returns 0 or a BleError:
    // InvalidParameter, PeerNotReady (10), MessageTooLarge (11), NotInitialized (link shut down).
    BLE_API int BleLink_Send(void* link, const char* deviceId, const uint8_t* data, int dataLength,
        uint32_t* outMsgId);
    // Aborts an outgoing message (MESSAGE_FAILED reason CANCELLED follows unless it already finished).
    // Unknown ids are ignored. Returns 0 or InvalidParameter.
    BLE_API int BleLink_Cancel(void* link, uint32_t msgId);
    // Payload size of the next event (>= 0), -1 when no event is pending, -4 (InvalidParameter) for a
    // bad handle. Does not remove the event.
    BLE_API int BleLink_GetEventPayloadSize(void* link);
    // Takes the next event: fills *ev (ev->size must be >= sizeof(BleLinkEventC)) and copies its payload
    // into buffer. Returns 1 = event taken, 0 = no event, -12 (BufferTooSmall) = the payload needs
    // ev->dataLength bytes, the event stays queued (ev is filled), -4 = invalid handle/argument.
    // buffer may be NULL when bufferLength is 0.
    BLE_API int BleLink_PollEvent(void* link, BleLinkEventC* ev, uint8_t* buffer, int bufferLength);
    // Current session info for a peer (info->size must be >= sizeof(BleLinkPeerInfoC)). Returns 0,
    // PeerNotReady (unknown peer) or InvalidParameter.
    BLE_API int BleLink_GetPeerInfo(void* link, const char* deviceId, BleLinkPeerInfoC* info);
    // Number of peers with a session (READY or not) and their ids (query-length convention, like
    // BleServer_GetConnectedDeviceId).
    BLE_API int BleLink_GetPeerCount(void* link);
    BLE_API int BleLink_GetPeerId(void* link, int index, char* buffer, int* bufferLength);

    // ---------------------------------------------------------------- Message chunking (DEPRECATED)
    // DEPRECATED since api 3: use BleLink_* above. The chunker uses fixed 256-byte chunks (needs
    // ATT_MTU >= 259), broadcasts to every central, has no flow control and ACKs every chunk. It keeps
    // working unchanged. Define BLE_WARN_DEPRECATED_CHUNKER to get compiler warnings for its uses.
#if defined(BLE_WARN_DEPRECATED_CHUNKER)
#define BLE_CHUNKER_DEPRECATED [[deprecated("BleChunker_* is deprecated: use BleLink_*")]]
#else
#define BLE_CHUNKER_DEPRECATED
#endif
    BLE_CHUNKER_DEPRECATED BLE_API void* BleChunker_Create();
    BLE_CHUNKER_DEPRECATED BLE_API void BleChunker_Destroy(void* chunker);
    BLE_CHUNKER_DEPRECATED BLE_API int BleChunker_SendMessage(void* chunker, void* server,
        const char* serviceUuid,
        const char* characteristicUuid,
        const uint8_t* data,
        int dataLength);
    BLE_CHUNKER_DEPRECATED BLE_API bool BleChunker_ProcessReceivedData(void* chunker,
        const uint8_t* data,
        int dataLength,
        uint8_t* completeMessage,
        int* completeMessageLength);
    BLE_CHUNKER_DEPRECATED BLE_API int BleChunker_ProcessIncoming(void* chunker,
        const uint8_t* data,
        int dataLength,
        uint8_t* ackBuffer,
        int* ackLength);
    BLE_CHUNKER_DEPRECATED BLE_API int BleChunker_GetNextMessageSize(void* chunker);
    BLE_CHUNKER_DEPRECATED BLE_API bool BleChunker_ReadNextMessage(void* chunker,
        uint8_t* completeMessage,
        int* completeMessageLength);
    BLE_CHUNKER_DEPRECATED BLE_API int BleChunker_CleanupTimedOut(void* chunker, int timeoutMs);
}