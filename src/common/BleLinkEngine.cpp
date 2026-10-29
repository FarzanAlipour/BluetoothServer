// BleLinkEngine.cpp - BleLink v1 session engine (docs/PROTOCOL.md). Pure C++17, no OS headers.
#include "BleLinkEngine.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>

namespace BleServer {
namespace BleLink {

namespace {

constexpr size_t kNpos = std::numeric_limits<size_t>::max();
constexpr uint32_t kMaxTimeoutResends = 3;  // section 10
constexpr uint32_t kMaxCrcResends = 3;      // section 6.3
constexpr uint32_t kMaxRestarts = 8;        // section 8.2
constexpr uint32_t kMaxIdleResumes = 3;     // section 10
constexpr uint32_t kMaxHelloResends = 3;    // section 10
constexpr size_t kMaxUnknownResumed = 64;   // bound for the "RESUME(id, 0) sent once" memory
constexpr size_t kRxInitialReserve = 1u << 20;

uint32_t PeerWindow(const Hello& h) { return h.window == 0 ? 1u : h.window; }

}  // namespace

const char* ToString(FailReason reason) {
    switch (reason) {
    case FailReason::Disconnected: return "disconnected";
    case FailReason::Reset: return "reset";
    case FailReason::Timeout: return "timeout";
    case FailReason::Crc: return "crc";
    case FailReason::TooLarge: return "too_large";
    case FailReason::Busy: return "busy";
    case FailReason::Rejected: return "rejected";
    case FailReason::LinkUnreliable: return "link_unreliable";
    case FailReason::Cancelled: return "cancelled";
    case FailReason::NotReady: return "not_ready";
    case FailReason::Shutdown: return "shutdown";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------------------------------------

struct Engine::TxMsg {
    uint32_t handle = 0;
    uint16_t msgId = 0;
    std::shared_ptr<const std::vector<uint8_t>> data;
    uint32_t crc = 0;
    // Send cursor: the next packet is MSG_START (needStart), MSG_RESEND at nextOffset (needResend) or MSG_DATA.
    bool needStart = true;
    bool needResend = false;
    bool waitAck = false;  // every byte handed to the stack, waiting for the ACK
    uint32_t nextOffset = 0;
    uint32_t sentHigh = 0;  // highest offset ever handed to the stack
    bool anySent = false;
    // Duplicate-RESUME guard (8.2): active from a restart until the next packet of this message is sent.
    bool guardActive = false;
    uint32_t guardOffset = 0;
    uint32_t restarts = 0;
    uint32_t timeoutResends = 0;
    uint32_t crcResends = 0;
    uint64_t lastActivity = 0;

    uint32_t Size() const { return static_cast<uint32_t>(data->size()); }
    bool HasData() const { return needStart || needResend || (!waitAck && nextOffset < Size()); }
};

struct Engine::RxMsg {
    uint16_t msgId = 0;
    uint32_t totalLen = 0;
    uint32_t expectedCrc = 0;
    uint32_t crc = 0;
    std::vector<uint8_t> data;
    bool resync = false;
    bool mismatchResumeSent = false;
    uint32_t idleResumes = 0;
    uint64_t lastActivity = 0;

    uint32_t Received() const { return static_cast<uint32_t>(data.size()); }
};

struct Engine::Control {
    PacketType type = PacketType::Ack;
    uint16_t msgId = 0;
    uint32_t value = 0;  // ACK status, RESUME offset, CANCEL reason, PROGRESS bytes
};

struct Engine::Built {
    enum class Kind { Hello, Control, Data } kind = Kind::Control;
    std::vector<uint8_t> bytes;
    size_t index = 0;  // Data: index into inFlight
    uint32_t endOffset = 0;
};

struct Engine::Session {
    PeerId id;
    bool subscribed = false;
    // HELLO state: READY once our HELLO was sent and the peer's was received (section 3).
    bool helloQueued = false;
    bool helloSent = false;
    bool helloReceived = false;
    bool awaitingHello = false;  // we initiated (client open or reset) and wait for the answer
    uint32_t helloSends = 0;
    uint64_t helloDeadline = kNoDeadline;
    bool ready = false;      // PeerReady reported
    bool everReady = false;  // was READY since the last close (a reset is in progress when !ready)
    Hello peer;
    uint16_t osLimit = 0;
    // seq
    uint8_t txSeq = 0;
    bool haveExpected = false;
    uint8_t expectedSeq = 0;
    std::vector<uint8_t> lastPacket;
    bool haveLast = false;
    // send side
    std::deque<Control> controls;
    std::deque<std::unique_ptr<TxMsg>> queued;
    std::vector<std::unique_ptr<TxMsg>> inFlight;
    size_t rr = 0;
    uint16_t nextMsgId = 0;
    bool busy = false;
    uint64_t busySince = 0;
    uint64_t busyRetryAt = 0;
    uint64_t retryAt = 0;
    uint32_t errorCount = 0;
    // receive side
    std::map<uint16_t, std::unique_ptr<RxMsg>> rx;
    std::deque<std::pair<uint16_t, uint8_t>> done;  // duplicate memory (6.3)
    std::map<uint16_t, uint64_t> unknownResumed;    // msgId -> time RESUME(id, 0) was queued

    bool Ready() const { return helloSent && helloReceived; }
};

// ---------------------------------------------------------------------------------------------------------
// Construction, inputs
// ---------------------------------------------------------------------------------------------------------

Engine::Engine(ITransport& transport, const Config& config, Clock clock)
    : transport_(transport), config_(config), clock_(std::move(clock)) {
    if (!clock_) {
        clock_ = [] {
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                             std::chrono::steady_clock::now().time_since_epoch())
                                             .count());
        };
    }
}

Engine::~Engine() { Stop(); }

uint64_t Engine::Now() const { return clock_(); }

void Engine::Enqueue(Input&& in) {
    if (stopped_.load()) return;
    {
        std::lock_guard<std::mutex> lk(inboxMu_);
        inbox_.push_back(std::move(in));
    }
    inboxCv_.notify_one();
}

void Engine::OnPeerConnected(const PeerId& peer) {
    Input in;
    in.kind = Input::Kind::Connected;
    in.peer = peer;
    Enqueue(std::move(in));
}

void Engine::OnPeerSubscribed(const PeerId& peer) {
    Input in;
    in.kind = Input::Kind::Subscribed;
    in.peer = peer;
    Enqueue(std::move(in));
}

void Engine::OnPeerUnsubscribed(const PeerId& peer) {
    Input in;
    in.kind = Input::Kind::Unsubscribed;
    in.peer = peer;
    Enqueue(std::move(in));
}

void Engine::OnPeerDisconnected(const PeerId& peer) {
    Input in;
    in.kind = Input::Kind::Disconnected;
    in.peer = peer;
    Enqueue(std::move(in));
}

void Engine::OnPacket(const PeerId& peer, const uint8_t* data, size_t len) {
    Input in;
    in.kind = Input::Kind::Packet;
    in.peer = peer;
    if (data != nullptr && len > 0) in.bytes.assign(data, data + len);
    Enqueue(std::move(in));
}

void Engine::OnTransportReady(const PeerId& peer) {
    Input in;
    in.kind = Input::Kind::Ready;
    in.peer = peer;
    Enqueue(std::move(in));
}

void Engine::SetPeerOsLimit(const PeerId& peer, uint16_t osLimit) {
    Input in;
    in.kind = Input::Kind::OsLimit;
    in.peer = peer;
    in.value = osLimit;
    Enqueue(std::move(in));
}

SendOutcome Engine::Send(const PeerId& peer, std::vector<uint8_t> data) {
    SendOutcome out;
    if (stopped_.load()) {
        out.code = SendOutcome::Code::Stopped;
        return out;
    }
    if (data.size() > 0xFFFFFFFFull) {
        out.code = SendOutcome::Code::TooLarge;
        return out;
    }
    {
        std::lock_guard<std::mutex> lk(snapMu_);
        auto it = snapshot_.find(peer);
        if (it == snapshot_.end() || !it->second.ready) {
            out.code = SendOutcome::Code::NotReady;
            return out;
        }
        const uint32_t limit = it->second.peer.maxMessage;
        if (limit != kNoMessageLimit && data.size() > limit) {
            out.code = SendOutcome::Code::TooLarge;
            return out;
        }
    }
    Input in;
    in.kind = Input::Kind::Send;
    in.peer = peer;
    in.crc = Crc32(0, data.data(), data.size());
    in.message = std::make_shared<const std::vector<uint8_t>>(std::move(data));
    in.handle = nextHandle_.fetch_add(1);
    if (in.handle == 0) in.handle = nextHandle_.fetch_add(1);  // 0 is never a valid handle
    out.handle = in.handle;
    Enqueue(std::move(in));
    if (stopped_.load()) {
        out.code = SendOutcome::Code::Stopped;
        out.handle = 0;
    }
    return out;
}

void Engine::Cancel(uint32_t handle) {
    Input in;
    in.kind = Input::Kind::Cancel;
    in.handle = handle;
    Enqueue(std::move(in));
}

void Engine::ResetPeer(const PeerId& peer) {
    Input in;
    in.kind = Input::Kind::Reset;
    in.peer = peer;
    Enqueue(std::move(in));
}

bool Engine::GetPeerInfo(const PeerId& peer, PeerInfo& out) const {
    std::lock_guard<std::mutex> lk(snapMu_);
    auto it = snapshot_.find(peer);
    if (it == snapshot_.end()) return false;
    out = it->second;
    return true;
}

std::vector<PeerId> Engine::Peers() const {
    std::lock_guard<std::mutex> lk(snapMu_);
    std::vector<PeerId> ids;
    ids.reserve(snapshot_.size());
    for (const auto& kv : snapshot_) ids.push_back(kv.first);
    return ids;
}

Hello Engine::LocalHello() const {
    Hello h;
    h.version = kProtocolVersion;
    h.maxPacket = ClampPacketSize(config_.maxPacket);
    h.maxMessage = config_.maxMessage;
    h.window = config_.window == 0 ? 1 : config_.window;
    h.flags = static_cast<uint8_t>((config_.wantProgress ? kFlagProgress : 0) |
                                   (config_.streamingReceiver ? kFlagStreaming : 0));
    h.timeout = config_.timeoutUnits;
    return h;
}

std::vector<uint8_t> Engine::InfoValue() const {
    std::vector<uint8_t> v;
    EncodeInfo(v, LocalHello());
    return v;
}

Stats Engine::GetStats() const {
    std::lock_guard<std::mutex> lk(snapMu_);
    return statsSnapshot_;
}

void Engine::SetEventHandler(EventHandler handler) {
    std::lock_guard<std::mutex> lk(outMu_);
    handler_ = std::move(handler);
}

bool Engine::PollEvent(Event& out) {
    std::lock_guard<std::mutex> lk(outMu_);
    if (handler_ || outbox_.empty()) return false;
    out = std::move(outbox_.front());
    outbox_.pop_front();
    return true;
}

size_t Engine::PendingEventCount() const {
    std::lock_guard<std::mutex> lk(outMu_);
    return outbox_.size();
}

size_t Engine::PendingInputCount() const {
    std::lock_guard<std::mutex> lk(inboxMu_);
    return inbox_.size();
}

// ---------------------------------------------------------------------------------------------------------
// Pump, worker, events
// ---------------------------------------------------------------------------------------------------------

uint64_t Engine::Pump() {
    uint64_t deadline = kNoDeadline;
    {
        std::unique_lock<std::mutex> core(coreMu_);
        const uint64_t now = Now();
        std::deque<Input> batch;
        {
            std::lock_guard<std::mutex> lk(inboxMu_);
            batch.swap(inbox_);
        }
        for (auto& in : batch) ProcessInput(in, now);
        bool more = false;
        for (auto& kv : sessions_) Timers(*kv.second, now);
        for (auto& kv : sessions_) {
            if (FlushSession(*kv.second, now)) more = true;
        }
        if (more) {
            deadline = 0;
        } else {
            for (const auto& kv : sessions_) deadline = std::min(deadline, NextDeadline(*kv.second, now));
        }
        UpdateSnapshot();
        if (!coreEvents_.empty()) {
            std::lock_guard<std::mutex> lk(outMu_);
            for (auto& ev : coreEvents_) outbox_.push_back(std::move(ev));
            coreEvents_.clear();
        }
    }
    Dispatch();
    {
        std::lock_guard<std::mutex> lk(inboxMu_);
        if (!inbox_.empty()) deadline = 0;
    }
    return deadline;
}

void Engine::Emit(Event&& ev) { coreEvents_.push_back(std::move(ev)); }

void Engine::Dispatch() {
    EventHandler handler;
    {
        std::lock_guard<std::mutex> lk(outMu_);
        if (dispatching_ || !handler_) return;
        dispatching_ = true;
        handler = handler_;
    }
    for (;;) {
        Event ev;
        {
            std::lock_guard<std::mutex> lk(outMu_);
            if (outbox_.empty()) {
                dispatching_ = false;
                return;
            }
            ev = std::move(outbox_.front());
            outbox_.pop_front();
        }
        try {
            handler(ev);
        } catch (...) {
            // A throwing handler must not take the engine down; the event is dropped.
        }
    }
}

void Engine::Start() {
    std::lock_guard<std::mutex> lk(workerMu_);
    if (worker_.joinable() || stopped_.load()) return;
    {
        std::lock_guard<std::mutex> ilk(inboxMu_);
        stopWorker_ = false;
    }
    worker_ = std::thread([this] { WorkerLoop(); });
}

void Engine::Stop() {
    std::lock_guard<std::mutex> lk(workerMu_);
    if (!worker_.joinable()) return;
    {
        std::lock_guard<std::mutex> ilk(inboxMu_);
        stopWorker_ = true;
    }
    inboxCv_.notify_all();
    if (worker_.get_id() == std::this_thread::get_id()) {
        worker_.detach();  // Stop() from an event handler: the loop exits after this event
        return;
    }
    worker_.join();
}

void Engine::WorkerLoop() {
    for (;;) {
        const uint64_t wait = Pump();
        std::unique_lock<std::mutex> lk(inboxMu_);
        if (stopWorker_) return;
        if (wait == 0 || !inbox_.empty()) continue;
        auto pred = [this] { return stopWorker_ || !inbox_.empty(); };
        if (wait == kNoDeadline) {
            inboxCv_.wait(lk, pred);
        } else {
            inboxCv_.wait_for(lk, std::chrono::milliseconds(wait), pred);
        }
        if (stopWorker_) return;
    }
}

void Engine::Shutdown() {
    stopped_.store(true);
    Stop();
    {
        std::lock_guard<std::mutex> core(coreMu_);
        for (auto& kv : sessions_) {
            Session& s = *kv.second;
            for (auto& m : s.inFlight) {
                if (s.subscribed && s.Ready() && m->anySent) {
                    std::vector<uint8_t> pkt;
                    EncodeCancel(pkt, s.txSeq, m->msgId, static_cast<uint8_t>(CancelReason::Shutdown));
                    if (transport_.SendPacket(s.id, pkt.data(), pkt.size()) == TransportResult::Sent) {
                        ++s.txSeq;
                        ++stats_.packetsSent;
                    }
                }
            }
            CloseSession(s, FailReason::Shutdown, false);
        }
        {
            std::lock_guard<std::mutex> lk(inboxMu_);
            inbox_.clear();
        }
        UpdateSnapshot();
        std::lock_guard<std::mutex> lk(outMu_);
        for (auto& ev : coreEvents_) outbox_.push_back(std::move(ev));
        coreEvents_.clear();
    }
    Dispatch();
}

void Engine::UpdateSnapshot() {
    std::lock_guard<std::mutex> lk(snapMu_);
    snapshot_.clear();
    for (const auto& kv : sessions_) snapshot_[kv.first] = MakeInfo(*kv.second);
    statsSnapshot_ = stats_;
}

// ---------------------------------------------------------------------------------------------------------
// Sessions
// ---------------------------------------------------------------------------------------------------------

Engine::Session& Engine::GetOrCreate(const PeerId& peer) {
    auto it = sessions_.find(peer);
    if (it != sessions_.end()) return *it->second;
    auto s = std::make_unique<Session>();
    s->id = peer;
    Session& ref = *s;
    sessions_.emplace(peer, std::move(s));
    return ref;
}

Engine::Session* Engine::Find(const PeerId& peer) {
    auto it = sessions_.find(peer);
    return it == sessions_.end() ? nullptr : it->second.get();
}

uint16_t Engine::TxPacketSize(const Session& s) const {
    if (!s.helloReceived) return kMinPacketSize;
    const uint16_t os = s.osLimit != 0 ? s.osLimit : config_.defaultOsLimit;
    return ClampPacketSize(std::min<uint32_t>(ClampPacketSize(s.peer.maxPacket), os));
}

uint32_t Engine::TimeoutMs(const Session& s) const {
    const uint32_t own = TimeoutUnitsToMs(config_.timeoutUnits);
    if (!s.helloReceived) return own;
    return std::max(own, TimeoutUnitsToMs(s.peer.timeout));
}

PeerInfo Engine::MakeInfo(const Session& s) const {
    PeerInfo info;
    info.subscribed = s.subscribed;
    info.ready = s.ready;
    info.txPacketSize = TxPacketSize(s);
    info.osLimit = s.osLimit;
    info.peer = s.peer;
    info.timeoutMs = TimeoutMs(s);
    info.queued = s.queued.size();
    info.inFlight = s.inFlight.size();
    info.receiving = s.rx.size();
    return info;
}

void Engine::EmitFailed(const Session& s, const TxMsg& m, FailReason reason, uint8_t ackStatus) {
    Event ev;
    ev.type = EventType::MessageFailed;
    ev.peer = s.id;
    ev.handle = m.handle;
    ev.msgId = m.msgId;
    ev.reason = reason;
    ev.ackStatus = ackStatus;
    ev.total = m.Size();
    Emit(std::move(ev));
}

// Drops every in-flight message (fail with reason) and all receive state. Queued messages stay queued.
void Engine::ResetSessionData(Session& s, FailReason reason) {
    for (auto& m : s.inFlight) EmitFailed(s, *m, reason, 0);
    s.inFlight.clear();
    s.rr = 0;
    s.nextMsgId = 0;
    s.controls.clear();
    s.rx.clear();
    s.done.clear();
    s.unknownResumed.clear();
}

void Engine::CloseSession(Session& s, FailReason reason, bool forceGone) {
    const bool wasUp = s.ready || s.everReady;
    ResetSessionData(s, reason);
    for (auto& m : s.queued) EmitFailed(s, *m, reason, 0);
    s.queued.clear();
    s.subscribed = false;
    s.helloQueued = s.helloSent = s.helloReceived = s.awaitingHello = false;
    s.helloSends = 0;
    s.helloDeadline = kNoDeadline;
    s.peer = Hello{};
    s.txSeq = 0;
    s.haveExpected = false;
    s.haveLast = false;
    s.lastPacket.clear();
    s.busy = false;
    s.retryAt = 0;
    s.errorCount = 0;
    s.ready = false;
    s.everReady = false;
    if (wasUp || forceGone) {
        Event ev;
        ev.type = EventType::PeerGone;
        ev.peer = s.id;
        ev.reason = reason;
        ev.info = MakeInfo(s);
        Emit(std::move(ev));
    }
}

void Engine::QueueHello(Session& s, bool initiate) {
    s.helloQueued = true;
    s.helloSent = false;
    s.txSeq = 0;
    s.controls.clear();
    s.errorCount = 0;
    if (initiate) {
        s.awaitingHello = true;
        s.helloReceived = false;
        s.helloDeadline = kNoDeadline;
    }
    MaybeReady(s);
}

void Engine::MaybeReady(Session& s) {
    const bool r = s.Ready();
    if (r && !s.ready) {
        s.ready = true;
        s.everReady = true;
        Event ev;
        ev.type = EventType::PeerReady;
        ev.peer = s.id;
        ev.info = MakeInfo(s);
        Emit(std::move(ev));
    } else if (!r && s.ready) {
        s.ready = false;  // reset in progress; PeerReady follows when both HELLOs are through again
    }
}

void Engine::ProcessInput(Input& in, uint64_t now) {
    switch (in.kind) {
    case Input::Kind::Connected:
        GetOrCreate(in.peer);
        break;
    case Input::Kind::Subscribed: {
        Session& s = GetOrCreate(in.peer);
        if (s.subscribed) break;
        s.subscribed = true;
        if (config_.role == Role::Client) {
            s.helloSends = 0;
            QueueHello(s, true);
        }
        break;
    }
    case Input::Kind::Unsubscribed:
        if (Session* s = Find(in.peer)) CloseSession(*s, FailReason::Disconnected, false);
        break;
    case Input::Kind::Disconnected: {
        auto it = sessions_.find(in.peer);
        if (it != sessions_.end()) {
            CloseSession(*it->second, FailReason::Disconnected, false);
            sessions_.erase(it);
        }
        break;
    }
    case Input::Kind::Packet: {
        Session& s = GetOrCreate(in.peer);
        HandlePacket(s, in.bytes.data(), in.bytes.size(), now);
        break;
    }
    case Input::Kind::Ready:
        for (auto& kv : sessions_) {
            Session& s = *kv.second;
            if (!in.peer.empty() && kv.first != in.peer) continue;
            if (!s.busy) continue;
            const uint64_t blocked = now - s.busySince;
            // Sender timers pause while flow control blocked us for longer than T/2 (section 10).
            if (blocked > TimeoutMs(s) / 2) {
                for (auto& m : s.inFlight) {
                    if (m->waitAck) m->lastActivity = std::min(now, m->lastActivity + blocked);
                }
            }
            s.busy = false;
        }
        break;
    case Input::Kind::OsLimit:
        GetOrCreate(in.peer).osLimit = in.value;
        break;
    case Input::Kind::Send: {
        Session* s = Find(in.peer);
        auto m = std::make_unique<TxMsg>();
        m->handle = in.handle;
        m->data = std::move(in.message);
        m->crc = in.crc;
        if (s == nullptr || !s->subscribed || !s->everReady) {
            Event ev;
            ev.type = EventType::MessageFailed;
            ev.peer = in.peer;
            ev.handle = m->handle;
            ev.reason = FailReason::NotReady;
            ev.total = m->Size();
            Emit(std::move(ev));
            break;
        }
        s->queued.push_back(std::move(m));
        break;
    }
    case Input::Kind::Cancel:
        for (auto& kv : sessions_) {
            Session& s = *kv.second;
            for (auto it = s.queued.begin(); it != s.queued.end(); ++it) {
                if ((*it)->handle == in.handle) {
                    EmitFailed(s, **it, FailReason::Cancelled, 0);
                    s.queued.erase(it);
                    return;
                }
            }
            for (size_t i = 0; i < s.inFlight.size(); ++i) {
                if (s.inFlight[i]->handle == in.handle) {
                    if (s.inFlight[i]->anySent) {
                        QueueControl(s, PacketType::Cancel, s.inFlight[i]->msgId,
                                     static_cast<uint8_t>(CancelReason::Application));
                    }
                    EmitFailed(s, *s.inFlight[i], FailReason::Cancelled, 0);
                    RemoveInFlight(s, i);
                    return;
                }
            }
        }
        break;
    case Input::Kind::Reset:
        if (Session* s = Find(in.peer)) {
            if (!s->subscribed) break;
            ResetSessionData(*s, FailReason::Reset);
            s->helloSends = 0;
            QueueHello(*s, true);
        }
        break;
    }
}

// ---------------------------------------------------------------------------------------------------------
// Receive path
// ---------------------------------------------------------------------------------------------------------

void Engine::HandlePacket(Session& s, const uint8_t* data, size_t len, uint64_t now) {
    ++stats_.packetsReceived;
    Packet p;
    const DecodeResult r = Decode(data, len, p);
    if (r == DecodeResult::Empty) return;

    // A packet identical to the previous one, with the previous seq, is a stack duplicate.
    const bool duplicate = s.haveExpected && s.haveLast && p.seq == static_cast<uint8_t>(s.expectedSeq - 1) &&
                           s.lastPacket.size() == len && std::memcmp(s.lastPacket.data(), data, len) == 0;
    if (duplicate && p.type != static_cast<uint8_t>(PacketType::Hello)) {
        ++stats_.duplicatesDropped;
        return;
    }
    s.lastPacket.assign(data, data + len);
    s.haveLast = true;

    if (p.type == static_cast<uint8_t>(PacketType::Hello)) {
        if (r != DecodeResult::Ok) {
            if (s.haveExpected) s.expectedSeq = static_cast<uint8_t>(p.seq + 1);
            return;
        }
        HandleHello(s, p, duplicate);
        return;
    }
    if (!s.haveExpected) return;  // no HELLO yet: nothing to count against
    if (p.seq != s.expectedSeq) {
        ++stats_.seqGaps;
        if (s.Ready()) HandleGap(s);
    }
    s.expectedSeq = static_cast<uint8_t>(p.seq + 1);
    if (r != DecodeResult::Ok) return;  // unknown type or too short: ignored, seq counted
    if (!s.Ready()) return;             // section 3: only HELLO before READY

    switch (static_cast<PacketType>(p.type)) {
    case PacketType::MsgStart: OnMsgStart(s, p, now); break;
    case PacketType::MsgData: OnMsgData(s, p, now); break;
    case PacketType::MsgResend: OnMsgResend(s, p, now); break;
    case PacketType::Ack: OnAck(s, p); break;
    case PacketType::Resume: OnResume(s, p); break;
    case PacketType::Progress: OnProgress(s, p); break;
    case PacketType::Cancel: s.rx.erase(p.msgId); break;
    case PacketType::Hello: break;
    }
}

void Engine::HandleHello(Session& s, const Packet& p, bool duplicate) {
    if (p.hello.version == 0) return;  // not a valid v1+ HELLO
    s.haveExpected = true;
    s.expectedSeq = static_cast<uint8_t>(p.seq + 1);
    if (s.awaitingHello) {
        // The answer to our HELLO (or a simultaneous open): no reset, no answer.
        s.awaitingHello = false;
        s.helloDeadline = kNoDeadline;
        s.helloSends = 0;
        s.helloReceived = true;
        s.peer = p.hello;
        MaybeReady(s);
        return;
    }
    if (duplicate && config_.role == Role::Client) {
        // A client never answers a repeated server HELLO with nothing in between: answering would make
        // the server reset and answer again (ping-pong). See the notes on HELLO duplicates.
        ++stats_.duplicatesDropped;
        return;
    }
    // Peer-initiated open or reset: drop everything in flight and answer with our own HELLO.
    if (s.helloReceived || s.helloSent || s.everReady) ResetSessionData(s, FailReason::Reset);
    s.helloReceived = true;
    s.peer = p.hello;
    QueueHello(s, false);
}

void Engine::HandleGap(Session& s) {
    for (auto& kv : s.rx) {
        RxMsg& m = *kv.second;
        QueueControl(s, PacketType::Resume, m.msgId, m.Received());
        ++stats_.resumesSent;
        m.resync = true;
        m.mismatchResumeSent = false;
    }
}

const std::pair<uint16_t, uint8_t>* Engine::FindDone(const Session& s, uint16_t msgId) const {
    for (const auto& d : s.done) {
        if (d.first == msgId) return &d;
    }
    return nullptr;
}

void Engine::Remember(Session& s, uint16_t msgId, uint8_t status) {
    for (auto it = s.done.begin(); it != s.done.end(); ++it) {
        if (it->first == msgId) {
            s.done.erase(it);
            break;
        }
    }
    s.done.emplace_back(msgId, status);
    // Spec minimum: the last `window` ids. Rejections share the memory, so keep a few spare slots.
    const size_t cap = static_cast<size_t>(LocalHello().window) + 16;
    while (s.done.size() > cap) s.done.pop_front();
}

void Engine::RejectRx(Session& s, uint16_t msgId, AckStatus status, bool remember) {
    s.rx.erase(msgId);
    QueueControl(s, PacketType::Ack, msgId, static_cast<uint8_t>(status));
    ++stats_.rejected;
    if (remember) Remember(s, msgId, static_cast<uint8_t>(status));
}

void Engine::OnMsgStart(Session& s, const Packet& p, uint64_t now) {
    if (const auto* d = FindDone(s, p.msgId)) {
        // Retry after a lost ACK: answer with the stored status, never deliver twice.
        QueueControl(s, PacketType::Ack, p.msgId, d->second);
        ++stats_.duplicatesAcked;
        return;
    }
    s.unknownResumed.erase(p.msgId);
    s.rx.erase(p.msgId);  // a MSG_START for a message being received restarts it
    if (config_.maxMessage != kNoMessageLimit && p.totalLen > config_.maxMessage) {
        RejectRx(s, p.msgId, AckStatus::TooLarge, true);
        return;
    }
    if (s.rx.size() >= LocalHello().window) {
        RejectRx(s, p.msgId, AckStatus::NoMemory, true);
        return;
    }
    if (p.payloadLen > p.totalLen) {
        RejectRx(s, p.msgId, AckStatus::CrcMismatch, false);  // corrupt sender (6.3)
        return;
    }
    auto m = std::make_unique<RxMsg>();
    m->msgId = p.msgId;
    m->totalLen = p.totalLen;
    m->expectedCrc = p.crc;
    m->data.reserve(std::min<size_t>(p.totalLen, kRxInitialReserve));
    m->lastActivity = now;
    RxMsg& ref = *m;
    s.rx.emplace(p.msgId, std::move(m));
    AppendRx(s, ref, p.payload, p.payloadLen, now);
}

void Engine::OnUnknownMsgPacket(Session& s, uint16_t msgId, uint64_t now) {
    if (FindDone(s, msgId) != nullptr) return;  // late packet of a finished or rejected message
    auto it = s.unknownResumed.find(msgId);
    if (it != s.unknownResumed.end()) return;  // RESUME(id, 0) already sent once
    if (s.unknownResumed.size() >= kMaxUnknownResumed) s.unknownResumed.erase(s.unknownResumed.begin());
    s.unknownResumed[msgId] = now;
    QueueControl(s, PacketType::Resume, msgId, 0);
    ++stats_.resumesSent;
}

void Engine::OnMsgData(Session& s, const Packet& p, uint64_t now) {
    auto it = s.rx.find(p.msgId);
    if (it == s.rx.end()) {
        OnUnknownMsgPacket(s, p.msgId, now);
        return;
    }
    RxMsg& m = *it->second;
    if (m.resync) return;  // stale data after a gap: wait for MSG_RESEND
    AppendRx(s, m, p.payload, p.payloadLen, now);
}

void Engine::OnMsgResend(Session& s, const Packet& p, uint64_t now) {
    auto it = s.rx.find(p.msgId);
    if (it == s.rx.end()) {
        OnUnknownMsgPacket(s, p.msgId, now);
        return;
    }
    RxMsg& m = *it->second;
    if (p.offset != m.Received()) {
        m.resync = true;
        if (!m.mismatchResumeSent) {
            m.mismatchResumeSent = true;
            QueueControl(s, PacketType::Resume, m.msgId, m.Received());
            ++stats_.resumesSent;
        }
        return;
    }
    m.resync = false;
    m.mismatchResumeSent = false;
    AppendRx(s, m, p.payload, p.payloadLen, now);
}

void Engine::AppendRx(Session& s, RxMsg& m, const uint8_t* data, size_t n, uint64_t now) {
    m.lastActivity = now;
    if (n > static_cast<size_t>(m.totalLen - m.Received())) {
        RejectRx(s, m.msgId, AckStatus::CrcMismatch, false);  // more bytes than totalLen: corrupt sender
        return;
    }
    const uint32_t before = m.Received();
    if (n > 0) {
        m.data.insert(m.data.end(), data, data + n);
        m.crc = Crc32(m.crc, data, n);
        m.idleResumes = 0;
    }
    const uint32_t after = m.Received();
    if (m.totalLen >= kProgressStep && after / kProgressStep != before / kProgressStep) {
        if ((s.peer.flags & kFlagProgress) != 0) QueueControl(s, PacketType::Progress, m.msgId, after);
        Event ev;
        ev.type = EventType::Progress;
        ev.peer = s.id;
        ev.msgId = m.msgId;
        ev.progressKind = ProgressKind::Receiving;
        ev.bytes = after;
        ev.total = m.totalLen;
        Emit(std::move(ev));
    }
    if (after < m.totalLen) return;

    const uint16_t id = m.msgId;
    if (m.crc != m.expectedCrc) {
        RejectRx(s, id, AckStatus::CrcMismatch, false);  // not remembered: the sender resends with this id
        return;
    }
    QueueControl(s, PacketType::Ack, id, static_cast<uint8_t>(AckStatus::Ok));
    Remember(s, id, static_cast<uint8_t>(AckStatus::Ok));
    Event ev;
    ev.type = EventType::MessageReceived;
    ev.peer = s.id;
    ev.msgId = id;
    ev.data = std::move(m.data);
    ev.total = ev.data.size();
    s.rx.erase(id);  // m is gone from here on
    Emit(std::move(ev));
}

void Engine::OnAck(Session& s, const Packet& p) {
    const size_t i = FindInFlight(s, p.msgId);
    if (i == kNpos) return;
    TxMsg& m = *s.inFlight[i];
    switch (p.status) {
    case static_cast<uint8_t>(AckStatus::Ok): {
        Event ev;
        ev.type = EventType::MessageSent;
        ev.peer = s.id;
        ev.handle = m.handle;
        ev.msgId = m.msgId;
        ev.total = m.Size();
        Emit(std::move(ev));
        RemoveInFlight(s, i);
        return;
    }
    case static_cast<uint8_t>(AckStatus::CrcMismatch):
        if (m.crcResends < kMaxCrcResends) {
            ++m.crcResends;
            ++stats_.crcRetries;
            RestartAt(m, 0);
            return;
        }
        EmitFailed(s, m, FailReason::Crc, p.status);
        break;
    case static_cast<uint8_t>(AckStatus::TooLarge): EmitFailed(s, m, FailReason::TooLarge, p.status); break;
    case static_cast<uint8_t>(AckStatus::NoMemory): EmitFailed(s, m, FailReason::Busy, p.status); break;
    default: EmitFailed(s, m, FailReason::Rejected, p.status); break;
    }
    RemoveInFlight(s, i);
}

void Engine::OnResume(Session& s, const Packet& p) {
    ++stats_.resumesReceived;
    const size_t i = FindInFlight(s, p.msgId);
    if (i == kNpos) return;  // unknown, finished or aborted
    TxMsg& m = *s.inFlight[i];
    if (m.guardActive && p.offset <= m.guardOffset) return;  // same gap, already restarted
    if (p.offset > m.sentHigh) return;                       // never sent that far: bogus
    if (p.offset > 0 && p.offset >= m.Size()) return;        // nothing missing
    if (++m.restarts > kMaxRestarts) {
        QueueControl(s, PacketType::Cancel, m.msgId, static_cast<uint8_t>(CancelReason::GaveUp));
        EmitFailed(s, m, FailReason::LinkUnreliable, 0);
        RemoveInFlight(s, i);
        return;
    }
    ++stats_.restarts;
    RestartAt(m, p.offset);
}

void Engine::OnProgress(Session& s, const Packet& p) {
    const size_t i = FindInFlight(s, p.msgId);
    if (i == kNpos) return;
    Event ev;
    ev.type = EventType::Progress;
    ev.peer = s.id;
    ev.handle = s.inFlight[i]->handle;
    ev.msgId = p.msgId;
    ev.progressKind = ProgressKind::PeerReceived;
    ev.bytes = p.bytesReceived;
    ev.total = s.inFlight[i]->Size();
    Emit(std::move(ev));
}

// ---------------------------------------------------------------------------------------------------------
// Send path
// ---------------------------------------------------------------------------------------------------------

void Engine::QueueControl(Session& s, PacketType type, uint16_t msgId, uint32_t value) {
    Control c;
    c.type = type;
    c.msgId = msgId;
    c.value = value;
    s.controls.push_back(c);
}

void Engine::RestartAt(TxMsg& m, uint32_t offset) {
    m.waitAck = false;
    m.guardActive = true;
    m.guardOffset = offset;
    m.nextOffset = offset;
    m.needStart = offset == 0;
    m.needResend = offset != 0;
}

size_t Engine::FindInFlight(const Session& s, uint16_t msgId) const {
    for (size_t i = 0; i < s.inFlight.size(); ++i) {
        if (s.inFlight[i]->msgId == msgId) return i;
    }
    return kNpos;
}

void Engine::RemoveInFlight(Session& s, size_t index) {
    s.inFlight.erase(s.inFlight.begin() + static_cast<std::ptrdiff_t>(index));
    if (index < s.rr) --s.rr;
    if (s.rr >= s.inFlight.size()) s.rr = 0;
}

bool Engine::BuildNext(Session& s, Built& out) {
    if (s.helloQueued) {
        out.kind = Built::Kind::Hello;
        EncodeHello(out.bytes, 0, LocalHello());
        return true;
    }
    if (!s.Ready()) return false;
    if (!s.controls.empty()) {
        const Control& c = s.controls.front();
        out.kind = Built::Kind::Control;
        switch (c.type) {
        case PacketType::Ack: EncodeAck(out.bytes, s.txSeq, c.msgId, static_cast<uint8_t>(c.value)); break;
        case PacketType::Resume: EncodeResume(out.bytes, s.txSeq, c.msgId, c.value); break;
        case PacketType::Cancel: EncodeCancel(out.bytes, s.txSeq, c.msgId, static_cast<uint8_t>(c.value)); break;
        case PacketType::Progress: EncodeProgress(out.bytes, s.txSeq, c.msgId, c.value); break;
        default: EncodeAck(out.bytes, s.txSeq, c.msgId, static_cast<uint8_t>(c.value)); break;
        }
        return true;
    }
    // Start queued messages while the peer's window allows (6.3).
    while (!s.queued.empty() && s.inFlight.size() < PeerWindow(s.peer)) {
        std::unique_ptr<TxMsg> m = std::move(s.queued.front());
        s.queued.pop_front();
        if (s.peer.maxMessage != kNoMessageLimit && m->Size() > s.peer.maxMessage) {
            EmitFailed(s, *m, FailReason::TooLarge, 0);  // the peer's limit shrank after Send()
            continue;
        }
        m->msgId = s.nextMsgId++;
        s.inFlight.push_back(std::move(m));
    }
    const size_t n = s.inFlight.size();
    if (n == 0) return false;
    const uint16_t ps = TxPacketSize(s);
    for (size_t k = 0; k < n; ++k) {
        const size_t i = (s.rr + k) % n;
        TxMsg& m = *s.inFlight[i];
        if (!m.HasData()) continue;
        const uint8_t* base = m.data->data();
        const uint32_t size = m.Size();
        out.kind = Built::Kind::Data;
        out.index = i;
        if (m.needStart) {
            const uint32_t len = static_cast<uint32_t>(std::min<size_t>(size, StartCapacity(ps)));
            EncodeMsgStart(out.bytes, s.txSeq, m.msgId, size, m.crc, base, len);
            out.endOffset = len;
        } else if (m.needResend) {
            const uint32_t len =
                static_cast<uint32_t>(std::min<size_t>(size - m.nextOffset, ResendCapacity(ps)));
            EncodeMsgResend(out.bytes, s.txSeq, m.msgId, m.nextOffset, base + m.nextOffset, len);
            out.endOffset = m.nextOffset + len;
        } else {
            const uint32_t len = static_cast<uint32_t>(std::min<size_t>(size - m.nextOffset, DataCapacity(ps)));
            EncodeMsgData(out.bytes, s.txSeq, m.msgId, base + m.nextOffset, len);
            out.endOffset = m.nextOffset + len;
        }
        return true;
    }
    return false;
}

void Engine::Commit(Session& s, const Built& b, uint64_t now) {
    ++s.txSeq;
    switch (b.kind) {
    case Built::Kind::Hello:
        s.helloQueued = false;
        s.helloSent = true;
        if (s.awaitingHello) {
            ++s.helloSends;
            s.helloDeadline = now + TimeoutMs(s);
        }
        MaybeReady(s);
        return;
    case Built::Kind::Control:
        s.controls.pop_front();
        return;
    case Built::Kind::Data: break;
    }
    TxMsg& m = *s.inFlight[b.index];
    const uint32_t before = m.nextOffset;
    m.needStart = false;
    m.needResend = false;
    m.nextOffset = b.endOffset;
    m.sentHigh = std::max(m.sentHigh, m.nextOffset);
    m.anySent = true;
    m.guardActive = false;
    m.lastActivity = now;
    if (m.nextOffset >= m.Size()) m.waitAck = true;
    s.rr = (b.index + 1) % s.inFlight.size();
    if (m.Size() >= kProgressStep &&
        (m.nextOffset / kProgressStep != before / kProgressStep || m.nextOffset == m.Size()) &&
        m.nextOffset > before) {
        Event ev;
        ev.type = EventType::Progress;
        ev.peer = s.id;
        ev.handle = m.handle;
        ev.msgId = m.msgId;
        ev.progressKind = ProgressKind::Sending;
        ev.bytes = m.nextOffset;
        ev.total = m.Size();
        Emit(std::move(ev));
    }
}

bool Engine::FlushSession(Session& s, uint64_t now) {
    if (!s.subscribed) return false;
    if (s.busy) {
        if (now < s.busyRetryAt) return false;
        s.busy = false;  // lost-wakeup guard: try again
    }
    if (now < s.retryAt) return false;
    Built b;
    for (uint32_t count = 0; count < config_.maxPacketsPerPump; ++count) {
        if (!BuildNext(s, b)) return false;
        const TransportResult r = transport_.SendPacket(s.id, b.bytes.data(), b.bytes.size());
        if (r == TransportResult::Sent) {
            ++stats_.packetsSent;
            s.errorCount = 0;
            Commit(s, b, now);
            continue;
        }
        if (r == TransportResult::Busy) {
            ++stats_.busy;
            s.busy = true;
            s.busySince = now;
            s.busyRetryAt = now + config_.busyRetryMs;
            return false;
        }
        ++stats_.sendErrors;
        if (++s.errorCount >= std::max<uint32_t>(1, config_.maxSendErrorsPerPacket)) {
            // Give the packet up. Its seq is consumed, so the peer sees a gap and the RESUME / timeout
            // machinery repairs whatever it carried.
            ++stats_.packetsDropped;
            s.errorCount = 0;
            Commit(s, b, now);
        }
        s.retryAt = now + config_.errorBackoffMs;
        return false;
    }
    return true;
}

void Engine::Timers(Session& s, uint64_t now) {
    if (s.awaitingHello && s.helloSent && now >= s.helloDeadline) {
        if (s.helloSends > kMaxHelloResends) {
            const PeerId id = s.id;
            CloseSession(s, FailReason::Timeout, true);
            transport_.RequestDisconnect(id);
            return;
        }
        QueueHello(s, true);  // keeps helloSends
        return;
    }
    if (!s.Ready()) return;
    const uint64_t T = TimeoutMs(s);
    if (!s.busy) {
        for (size_t i = 0; i < s.inFlight.size();) {
            TxMsg& m = *s.inFlight[i];
            if (m.waitAck && now - m.lastActivity >= T) {
                ++stats_.ackTimeouts;
                if (m.timeoutResends >= kMaxTimeoutResends) {
                    QueueControl(s, PacketType::Cancel, m.msgId, static_cast<uint8_t>(CancelReason::GaveUp));
                    EmitFailed(s, m, FailReason::Timeout, 0);
                    RemoveInFlight(s, i);
                    continue;
                }
                ++m.timeoutResends;
                RestartAt(m, 0);
                m.guardActive = false;
            }
            ++i;
        }
    }
    for (auto it = s.rx.begin(); it != s.rx.end();) {
        RxMsg& m = *it->second;
        if (now - m.lastActivity >= T) {
            if (m.idleResumes >= kMaxIdleResumes) {
                it = s.rx.erase(it);  // give up silently (section 10)
                continue;
            }
            ++m.idleResumes;
            m.resync = true;
            m.mismatchResumeSent = false;
            m.lastActivity = now;
            QueueControl(s, PacketType::Resume, m.msgId, m.Received());
            ++stats_.resumesSent;
        }
        ++it;
    }
    for (auto it = s.unknownResumed.begin(); it != s.unknownResumed.end();) {
        if (now - it->second >= T) {
            it = s.unknownResumed.erase(it);
        } else {
            ++it;
        }
    }
}

uint64_t Engine::NextDeadline(const Session& s, uint64_t now) const {
    uint64_t at = kNoDeadline;
    auto consider = [&](uint64_t t) { at = std::min(at, t); };
    if (s.awaitingHello && s.helloSent) consider(s.helloDeadline);
    if (s.subscribed) {
        if (s.busy) consider(s.busyRetryAt);
        if (s.retryAt > now) consider(s.retryAt);
    }
    if (s.Ready()) {
        const uint64_t T = TimeoutMs(s);
        if (!s.busy) {
            for (const auto& m : s.inFlight) {
                if (m->waitAck) consider(m->lastActivity + T);
            }
        }
        for (const auto& kv : s.rx) consider(kv.second->lastActivity + T);
        for (const auto& kv : s.unknownResumed) consider(kv.second + T);
    }
    if (at == kNoDeadline) return kNoDeadline;
    return at <= now ? 0 : at - now;
}

}  // namespace BleLink
}  // namespace BleServer
