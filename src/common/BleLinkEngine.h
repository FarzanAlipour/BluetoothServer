// BleLinkEngine.h - platform-independent BleLink v1 session engine (docs/PROTOCOL.md).
//
// One Engine serves every peer of one side (a GATT server with N centrals, or a client with one server).
// Per peer it runs the session state machine (HELLO negotiation, seq counters, packet size), a sender
// (window, round-robin interleaving, RESUME restart, ACK timeout, CRC retry) and a receiver (reassembly,
// CRC-32, seq-gap -> RESUME, limits, idle timeout, duplicate memory).
//
// Threading model
//   - Backend inputs (OnPacket, OnTransportReady, OnPeer*, SetPeerOsLimit) and app inputs (Send, Cancel,
//     ResetPeer) are thread-safe and never block: they copy into an inbox under a short mutex and return.
//     They are safe to call from a BLE stack callback thread, and from inside ITransport::SendPacket.
//   - All protocol work happens in Pump(), on the engine's worker thread (Start()) or on a thread the C layer
//     drives. ITransport methods are only called from Pump()/Shutdown() (with the engine's core lock held,
//     so they must not call Pump() themselves; calling the backend inputs above is fine).
//   - Sending is peek/commit: a packet is built from the current state, and the state (seq, offsets, queues)
//     only advances when the transport returned Sent. Busy and Error leave nothing half-sent behind.
//   - Events are delivered after Pump() released its locks: to the EventHandler if one is set, otherwise
//     they stay queued for PollEvent(). No user callback ever runs while an engine lock is held, so a handler
//     may call any engine method (Send, GetPeerInfo, even Pump).
#pragma once

#include "BleLinkCodec.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace BleServer {
namespace BleLink {

using PeerId = std::string;

enum class TransportResult {
    Sent,   // the stack accepted the packet
    Busy,   // flow control: try again after OnTransportReady()
    Error,  // the packet could not be sent (engine retries after a short backoff, then drops it)
};

// The engine's only view of the BLE backend.
class ITransport {
public:
    virtual ~ITransport() = default;
    // One packet = one notification (server) or one write (client). Must not block.
    virtual TransportResult SendPacket(const PeerId& peer, const uint8_t* data, size_t len) = 0;
    // Called when the HELLO handshake failed after all retries (section 10). Default: nothing.
    virtual void RequestDisconnect(const PeerId& /*peer*/) {}
};

enum class Role {
    Server,  // waits for the client's HELLO, answers it
    Client,  // sends HELLO when subscribed to the peer's TX, retries it on timeout
};

enum class FailReason {
    Disconnected,
    Reset,
    Timeout,
    Crc,
    TooLarge,
    Busy,
    Rejected,
    LinkUnreliable,
    Cancelled,
    NotReady,
    Shutdown,
};
const char* ToString(FailReason reason);

enum class EventType {
    PeerReady,        // session READY (also after every reset); info = negotiated values
    PeerGone,         // session left READY (disconnect, unsubscribe, handshake failure); reason set
    MessageReceived,  // msgId = peer's wire id, data = verified message
    MessageSent,      // ACK status 0 received for handle/msgId
    MessageFailed,    // reason set; ackStatus set when the failure came from an ACK
    Progress,         // progressKind, bytes, total
};

enum class ProgressKind {
    Sending,       // our bytes accepted by the stack (outgoing message)
    PeerReceived,  // PROGRESS packet from the receiving peer (outgoing message)
    Receiving,     // bytes received of an incoming message
};

struct PeerInfo {
    bool subscribed = false;
    bool ready = false;
    uint16_t txPacketSize = kMinPacketSize;  // packet size we send with
    uint16_t osLimit = 0;                     // OS-reported limit (0 = unknown, engine uses Config.defaultOsLimit)
    Hello peer;                               // peer HELLO (raw values) once received
    uint32_t timeoutMs = 0;                   // effective T
    size_t queued = 0;                        // outgoing messages not yet started
    size_t inFlight = 0;                      // outgoing messages started, not ACKed
    size_t receiving = 0;                     // incoming messages incomplete
};

struct Event {
    EventType type = EventType::PeerReady;
    PeerId peer;
    uint32_t handle = 0;   // outgoing messages: engine-wide handle returned by Send()
    uint16_t msgId = 0;    // wire msgId
    std::vector<uint8_t> data;
    FailReason reason = FailReason::Disconnected;
    uint8_t ackStatus = 0;
    ProgressKind progressKind = ProgressKind::Sending;
    uint64_t bytes = 0;
    uint64_t total = 0;
    PeerInfo info;
};

struct Config {
    Role role = Role::Server;
    // Our receive-side limits, advertised in HELLO and enforced.
    uint16_t maxPacket = kMaxPacketSize;
    uint32_t maxMessage = 16u * 1024u * 1024u;
    uint8_t window = 4;
    bool wantProgress = true;
    bool streamingReceiver = false;  // informational flag only
    uint8_t timeoutUnits = 0;        // 0.25 s units, 0 = 10 s
    // OS limit used for a peer until SetPeerOsLimit() reports one (20 = always safe).
    uint16_t defaultOsLimit = kMinPacketSize;
    // Fairness: packets sent per peer per Pump() before other work is looked at.
    uint32_t maxPacketsPerPump = 64;
    uint32_t errorBackoffMs = 50;
    uint32_t maxSendErrorsPerPacket = 3;
    // A Busy transport is retried after this long even without OnTransportReady (lost wakeup guard).
    uint32_t busyRetryMs = 250;
};

struct SendOutcome {
    enum class Code { Ok, NotReady, TooLarge, Stopped };
    Code code = Code::Ok;
    uint32_t handle = 0;  // valid when code == Ok
};

struct Stats {
    uint64_t packetsSent = 0;
    uint64_t packetsReceived = 0;
    uint64_t busy = 0;
    uint64_t sendErrors = 0;
    uint64_t packetsDropped = 0;     // given up after maxSendErrorsPerPacket errors (peer sees a seq gap)
    uint64_t seqGaps = 0;
    uint64_t duplicatesDropped = 0;  // identical packet with the previous seq (stack duplicate)
    uint64_t resumesSent = 0;
    uint64_t resumesReceived = 0;
    uint64_t restarts = 0;
    uint64_t ackTimeouts = 0;
    uint64_t crcRetries = 0;
    uint64_t duplicatesAcked = 0;
    uint64_t rejected = 0;  // ACKs with status != 0 sent by us
};

constexpr uint64_t kNoDeadline = ~uint64_t(0);

class Engine {
public:
    using Clock = std::function<uint64_t()>;  // monotonic milliseconds
    using EventHandler = std::function<void(const Event&)>;

    // The transport must outlive the engine. An empty clock uses std::chrono::steady_clock.
    Engine(ITransport& transport, const Config& config, Clock clock = {});
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // ---- Backend inputs (thread-safe, non-blocking) ----
    void OnPeerConnected(const PeerId& peer);
    void OnPeerSubscribed(const PeerId& peer);    // TX notifications enabled; packets may flow
    void OnPeerUnsubscribed(const PeerId& peer);  // back to LINKED, session closed
    void OnPeerDisconnected(const PeerId& peer);
    void OnPacket(const PeerId& peer, const uint8_t* data, size_t len);
    void OnTransportReady(const PeerId& peer = PeerId());  // empty = every peer
    void SetPeerOsLimit(const PeerId& peer, uint16_t osLimit);

    // ---- Application API (thread-safe, non-blocking) ----
    // Checks readiness and the peer's maxMessage against the last snapshot (updated by every Pump()).
    SendOutcome Send(const PeerId& peer, std::vector<uint8_t> data);
    void Cancel(uint32_t handle);
    void ResetPeer(const PeerId& peer);  // sends HELLO, fails in-flight messages with Reset
    bool GetPeerInfo(const PeerId& peer, PeerInfo& out) const;
    std::vector<PeerId> Peers() const;
    Hello LocalHello() const;
    std::vector<uint8_t> InfoValue() const;  // INFO characteristic value
    Stats GetStats() const;

    // Handler runs on the pumping thread with no engine lock held. Set it before Start().
    void SetEventHandler(EventHandler handler);
    bool PollEvent(Event& out);  // when no handler is set
    size_t PendingEventCount() const;
    size_t PendingInputCount() const;

    // ---- Driving ----
    // Processes inputs, timers and sends; then delivers events. Returns the ms until the next timer,
    // 0 when more work is ready now, kNoDeadline when nothing is scheduled. Safe from any thread; serialized.
    uint64_t Pump();
    void Start();  // worker thread that pumps on inputs and timers
    void Stop();   // joins the worker (idempotent)
    // Fails every message with Shutdown (in-flight peers get CANCEL reason 2, best effort) and rejects
    // further Send()s and backend inputs. Stops the worker.
    void Shutdown();

private:
    struct Input {
        enum class Kind {
            Connected,
            Subscribed,
            Unsubscribed,
            Disconnected,
            Packet,
            Ready,
            OsLimit,
            Send,
            Cancel,
            Reset,
        };
        Kind kind = Kind::Packet;
        PeerId peer;
        std::vector<uint8_t> bytes;                          // Packet
        std::shared_ptr<const std::vector<uint8_t>> message;  // Send
        uint32_t handle = 0;                                  // Send, Cancel
        uint32_t crc = 0;                                     // Send (computed on the caller's thread)
        uint16_t value = 0;                                   // OsLimit
    };
    struct TxMsg;
    struct RxMsg;
    struct Control;
    struct Built;
    struct Session;

    bool Enqueue(Input&& in);  // false = engine stopped, input dropped
    void ProcessInput(Input& in, uint64_t now);
    Session& GetOrCreate(const PeerId& peer);
    Session* Find(const PeerId& peer);

    // Session lifecycle
    void CloseSession(Session& s, FailReason reason, bool forceGone);
    void ResetSessionData(Session& s, FailReason reason);
    void QueueHello(Session& s, bool initiate);
    void MaybeReady(Session& s);
    uint16_t TxPacketSize(const Session& s) const;
    uint32_t TimeoutMs(const Session& s) const;
    PeerInfo MakeInfo(const Session& s) const;

    // Receive path
    void HandlePacket(Session& s, const uint8_t* data, size_t len, uint64_t now);
    void HandleHello(Session& s, const Packet& p, bool duplicate);
    void HandleGap(Session& s);
    void OnMsgStart(Session& s, const Packet& p, uint64_t now);
    void OnMsgData(Session& s, const Packet& p, uint64_t now);
    void OnMsgResend(Session& s, const Packet& p, uint64_t now);
    void OnUnknownMsgPacket(Session& s, uint16_t msgId, uint64_t now);
    void AppendRx(Session& s, RxMsg& m, const uint8_t* data, size_t n, uint64_t now);
    void RejectRx(Session& s, uint16_t msgId, AckStatus status, bool remember);
    void Remember(Session& s, uint16_t msgId, uint8_t status);
    const std::pair<uint16_t, uint8_t>* FindDone(const Session& s, uint16_t msgId) const;
    void OnAck(Session& s, const Packet& p);
    void OnResume(Session& s, const Packet& p);
    void OnProgress(Session& s, const Packet& p);

    // Send path
    void QueueControl(Session& s, PacketType type, uint16_t msgId, uint32_t value);
    void RestartAt(TxMsg& m, uint32_t offset);
    void RemoveInFlight(Session& s, size_t index);
    void EmitFailed(const Session& s, const TxMsg& m, FailReason reason, uint8_t ackStatus);
    size_t FindInFlight(const Session& s, uint16_t msgId) const;  // npos when absent
    bool FlushSession(Session& s, uint64_t now);  // true = stopped by the per-pump packet cap
    bool BuildNext(Session& s, Built& out);
    void Commit(Session& s, const Built& b, uint64_t now);
    void Timers(Session& s, uint64_t now);
    uint64_t NextDeadline(const Session& s, uint64_t now) const;
    void UpdateSnapshot();

    void Emit(Event&& ev);
    void Dispatch();
    void WorkerLoop();
    uint64_t Now() const;

    ITransport& transport_;
    const Config config_;
    Clock clock_;

    // Inbox (inputs) and worker wakeup
    mutable std::mutex inboxMu_;
    std::condition_variable inboxCv_;
    std::deque<Input> inbox_;
    bool stopWorker_ = false;
    std::thread worker_;
    std::mutex workerMu_;  // guards worker_ start/join

    // Core state, touched only with coreMu_ held
    std::mutex coreMu_;
    std::map<PeerId, std::unique_ptr<Session>> sessions_;
    std::vector<Event> coreEvents_;
    Stats stats_;

    // Snapshot for Send()/GetPeerInfo() from other threads
    mutable std::mutex snapMu_;
    std::map<PeerId, PeerInfo> snapshot_;
    Stats statsSnapshot_;

    std::atomic<uint32_t> nextHandle_{1};
    std::atomic<bool> stopped_{false};

    // Outbox
    mutable std::mutex outMu_;
    std::deque<Event> outbox_;
    EventHandler handler_;
    bool dispatching_ = false;  // guarded by outMu_
};

}  // namespace BleLink
}  // namespace BleServer
