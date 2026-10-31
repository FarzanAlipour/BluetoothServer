import Foundation

/// One packet the transport should write to RX.
public struct OutboundPacket: Equatable {
    public var data: Data
    /// HELLO SHOULD use Write With Response (PROTOCOL.md section 1); everything else goes without response.
    public var isHello: Bool
}

/// BleLink v1 session, client (central) side: the protocol state machine with no Bluetooth, threads or timers.
///
/// Threading: NOT thread-safe. Call every method from one serial context (BleLinkClient uses its
/// CoreBluetooth queue). Callbacks (`onEvent`, `onOutputAvailable`) run synchronously inside the call that caused them.
///
/// Driving it (what a transport does):
/// - `start()` after TX notifications are enabled; it queues HELLO.
/// - `receive(_:)` for every TX notification.
/// - When `onOutputAvailable` fires, or the stack signals it can take another write, call `nextPacket()` in a loop
///   while the stack is ready and write each packet. Packets are built at that moment, so a packet that was
///   pulled MUST be written (seq is assigned on pull).
/// - `tick()` periodically (every 0.25 s is fine) for timeouts.
/// - `linkDown()` on disconnect.
public final class BleLinkSession {
    public enum State: Equatable { case closed, opening, ready }

    public let config: BleLinkConfig
    public private(set) var state: State = .closed
    /// The server's receive limits, from its HELLO.
    public private(set) var peer: HelloBody?
    public var onEvent: ((BleLinkEvent) -> Void)?
    /// Called when new output is queued (the transport should start pulling `nextPacket()`).
    public var onOutputAvailable: (() -> Void)?

    private let clock: () -> Double

    // MARK: tx state
    private var txSeq: UInt8 = 0
    private var control: [(UInt8) -> Packet] = []
    private var controlIsHello: [Bool] = []
    /// Next msgId to assign (internal so tests can start at the ids used in the test vectors).
    var nextMsgId: UInt16 = 0
    private var pending: [Outgoing] = []
    private var active: [Outgoing] = []
    private var rr = 0
    private var osLimit = 20

    // MARK: hello state
    private var helloSentAt: Double?
    private var helloAttempts = 0
    private var awaitingHello = false

    // MARK: rx state
    private var expectedSeq: UInt8 = 0
    private var incoming: [Incoming] = []
    private var doneMemory: [(id: UInt16, status: UInt8)] = []
    private var rejectedMemory: [(id: UInt16, status: UInt8)] = []
    private var silentIds: [UInt16] = []
    private var unknownResumeSent: [UInt16: Double] = [:]

    // MARK: flow control pause
    private var blockedSince: Double?

    public init(config: BleLinkConfig = BleLinkConfig(), clock: @escaping () -> Double = { ProcessInfo.processInfo.systemUptime }) {
        self.config = config
        self.clock = clock
    }

    // MARK: - Public API

    /// Bytes per packet this side sends: clamp(min(peer maxPacket, osLimit), 20, 512); 20 before READY (section 5).
    public var packetSize: Int {
        guard state == .ready, let p = peer else { return 20 }
        return min(max(min(p.effectiveMaxPacket, osLimit), 20), 512)
    }

    /// Effective timeout T in seconds: max of both sides (section 10). Before the peer's HELLO, our own value.
    public var timeoutSeconds: Double {
        max(config.hello.timeoutSeconds, peer?.timeoutSeconds ?? 0)
    }

    /// iOS: `peripheral.maximumWriteValueLength(for: .withoutResponse)`. Can be updated at any time;
    /// the next packet built uses it.
    public func setOsLimit(_ limit: Int) { osLimit = max(limit, 20) }

    /// Starts the session (TX notifications must already be enabled): queues HELLO.
    public func start() {
        dropAll(reason: nil)
        state = .opening
        helloAttempts = 0
        queueHello(awaitReply: true)
    }

    /// Client-initiated reset (for example after the MTU grew): fails everything in flight with `.reset`
    /// and re-runs the HELLO exchange.
    public func reset() {
        guard state != .closed else { return }
        dropAll(reason: .reset)
        state = .opening
        helloAttempts = 0
        queueHello(awaitReply: true)
    }

    /// The link is gone: fail everything in flight with `.disconnected`.
    public func linkDown() {
        dropAll(reason: .disconnected)
        state = .closed
        awaitingHello = false
        helloSentAt = nil
    }

    /// Queues a message and returns its msgId. Messages beyond the peer's window wait in a local queue.
    @discardableResult
    public func send(_ data: Data) throws -> UInt16 {
        guard state == .ready, let p = peer else { throw BleLinkError.notReady }
        if p.maxMessage != 0xFFFF_FFFF, data.count > Int(p.maxMessage) { throw BleLinkError.tooLarge(limit: p.maxMessage) }
        if data.count > Int(UInt32.max) - 1 { throw BleLinkError.tooLarge(limit: UInt32.max - 1) }
        let id = nextMsgId
        nextMsgId &+= 1
        pending.append(Outgoing(id: id, data: data, crc: CRC32.checksum(data)))
        promote()
        onOutputAvailable?()
        return id
    }

    /// Cancels an outgoing message (sends CANCEL reason 0 if any packet of it went out). Reports `.failed(.cancelled)`.
    public func cancel(_ msgId: UInt16) {
        if let i = pending.firstIndex(where: { $0.id == msgId }) {
            pending.remove(at: i)
            emit(.failed(msgId: msgId, reason: .cancelled))
        } else if let m = active.first(where: { $0.id == msgId }) {
            finish(m, failure: .cancelled, cancelReason: m.everSent ? 0 : nil)
        }
    }

    /// Number of messages queued or in flight.
    public var outgoingCount: Int { pending.count + active.count }

    /// True if `nextPacket()` would return a packet.
    public var hasOutput: Bool {
        !control.isEmpty || active.contains { $0.phase == .sending } || (!pending.isEmpty && active.count < peerWindow)
    }

    /// The next packet to write, or nil. Control packets (HELLO, ACK, RESUME, CANCEL, PROGRESS) go first,
    /// then data round-robin across in-flight messages.
    public func nextPacket() -> OutboundPacket? {
        let now = clock()
        if !control.isEmpty {
            let make = control.removeFirst()
            let isHello = controlIsHello.removeFirst()
            let seq: UInt8
            if isHello { seq = 0; txSeq = 1; helloSentAt = now } else { seq = txSeq; txSeq &+= 1 }
            return OutboundPacket(data: make(seq).encode(), isHello: isHello)
        }
        guard state == .ready else { return nil }
        promote()
        let sending = active.indices.filter { active[$0].phase == .sending }
        guard !sending.isEmpty else { return nil }
        let idx = sending.first(where: { $0 >= rr }) ?? sending[0]
        rr = idx + 1
        let m = active[idx]
        let ps = packetSize
        let seq = txSeq
        txSeq &+= 1
        let packet: Packet
        if m.needStart {
            let n = min(ps - Packet.startHeader, m.data.count)
            packet = .msgStart(seq: seq, msgId: m.id, totalLen: UInt32(m.data.count), crc32: m.crc, payload: m.slice(0, n))
            m.offset = n
            m.needStart = false
            m.resendAt = nil
        } else if let r = m.resendAt {
            let n = min(ps - Packet.resendHeader, m.data.count - r)
            packet = .msgResend(seq: seq, msgId: m.id, offset: UInt32(r), payload: m.slice(r, n))
            m.offset = r + n
            m.resendAt = nil
        } else {
            let n = min(ps - Packet.dataHeader, m.data.count - m.offset)
            packet = .msgData(seq: seq, msgId: m.id, payload: m.slice(m.offset, n))
            m.offset += n
        }
        m.everSent = true
        m.restartPending = false
        m.lastActivity = now
        if m.offset >= m.data.count { m.phase = .waitAck }
        if m.offset - m.reportedBytes >= max(m.data.count / 100, 1) || m.offset >= m.data.count {
            m.reportedBytes = m.offset
            emit(.progress(MessageProgress(msgId: m.id, direction: .outgoing, bytes: m.offset, total: m.data.count)))
        }
        return OutboundPacket(data: packet.encode(), isHello: false)
    }

    /// Tell the session the transport is (not) able to write. While blocked for longer than T/2 the ACK timers
    /// are paused (section 10).
    public func setFlowBlocked(_ blocked: Bool) {
        let now = clock()
        if blocked {
            if blockedSince == nil { blockedSince = now }
        } else if let since = blockedSince {
            blockedSince = nil
            if now - since > timeoutSeconds / 2 {
                for m in active where m.phase == .waitAck { m.lastActivity = max(m.lastActivity, now) }
            }
        }
    }

    /// Runs timeouts: HELLO resend, ACK timeout (resend whole message), receiver idle timeout (RESUME).
    public func tick() {
        let now = clock()
        let t = timeoutSeconds
        if state == .opening, awaitingHello, let sent = helloSentAt, now - sent >= t {
            if helloAttempts >= config.helloRetries {
                awaitingHello = false
                helloSentAt = nil
                state = .closed
                emit(.helloTimeout)
                return
            }
            helloAttempts += 1
            queueHello(awaitReply: true)
        }
        guard state == .ready else { return }
        if let since = blockedSince, now - since > t / 2 {
            // sender blocked by flow control: timers paused
        } else {
            for m in active where m.phase == .waitAck && now - m.lastActivity >= t {
                if m.timeoutResends >= 3 {
                    finish(m, failure: .timeout, cancelReason: 1)
                } else {
                    m.timeoutResends += 1
                    m.restart(at: 0)
                    m.lastActivity = now
                    onOutputAvailable?()
                }
            }
        }
        for inc in incoming where now - inc.lastActivity >= t {
            if inc.idleResumes >= 3 {
                removeIncoming(inc.id)
                rememberSilent(inc.id)
            } else {
                inc.idleResumes += 1
                inc.resync = true
                inc.mismatchResumeSent = false
                inc.lastActivity = now
                queueControl { .resume(seq: $0, msgId: inc.id, offset: UInt32(inc.received)) }
            }
        }
        unknownResumeSent = unknownResumeSent.filter { now - $0.value < t }
    }

    /// Feed one TX notification.
    public func receive<D: DataProtocol>(_ bytes: D) {
        guard let packet = Packet.decode(bytes) else {
            // Too short for its layout: ignored, but its seq counts.
            if state == .ready, let s = Packet.rawSeq(bytes) { checkSeq(s) }
            return
        }
        if case let .hello(seq, body) = packet {
            handleHello(seq: seq, body)
            return
        }
        guard state == .ready else { return }  // ignored before READY (section 3)
        checkSeq(packet.seq)
        let now = clock()
        switch packet {
        case .hello, .unknown:
            break
        case let .msgStart(_, id, total, crc, payload):
            handleStart(id: id, total: total, crc: crc, payload: payload, now: now)
        case let .msgData(_, id, payload):
            if let inc = findIncoming(id) {
                if !inc.resync { append(inc, payload, now: now) }
            } else {
                handleUnknownData(id, now: now)
            }
        case let .msgResend(_, id, offset, payload):
            if let inc = findIncoming(id) {
                if Int(offset) == inc.received {
                    inc.resync = false
                    inc.mismatchResumeSent = false
                    append(inc, payload, now: now)
                } else {
                    inc.resync = true
                    if !inc.mismatchResumeSent {
                        inc.mismatchResumeSent = true
                        queueControl { .resume(seq: $0, msgId: id, offset: UInt32(inc.received)) }
                    }
                }
            } else {
                handleUnknownData(id, now: now)
            }
        case let .ack(_, id, status):
            guard let m = active.first(where: { $0.id == id }) else { return }
            switch status {
            case 0:
                finish(m, failure: nil, cancelReason: nil)
            case 1:
                if m.crcRetries >= 3 {
                    finish(m, failure: .crc, cancelReason: nil)
                } else {
                    m.crcRetries += 1
                    m.restart(at: 0)
                    m.lastActivity = now
                    onOutputAvailable?()
                }
            case 2: finish(m, failure: .tooLarge, cancelReason: nil)
            case 3: finish(m, failure: .busy, cancelReason: nil)
            default: finish(m, failure: .rejected(status: status), cancelReason: nil)
            }
        case let .resume(_, id, offset):
            guard let m = active.first(where: { $0.id == id }), Int(offset) <= m.data.count else { return }
            if m.restartPending, Int(offset) <= m.restartOffset { return }  // same gap reported twice
            if m.restarts >= 8 {
                finish(m, failure: .linkUnreliable, cancelReason: 1)
                return
            }
            m.restarts += 1
            m.restart(at: Int(offset))
            m.lastActivity = now
            onOutputAvailable?()
        case let .cancel(_, id, _):
            if findIncoming(id) != nil {
                removeIncoming(id)
                rememberSilent(id)
            }
        case let .progress(_, id, n):
            if let m = active.first(where: { $0.id == id }) {
                emit(.progress(MessageProgress(msgId: id, direction: .outgoing, bytes: Int(n), total: m.data.count,
                                               confirmedByPeer: true)))
            }
        }
    }

    // MARK: - HELLO

    private func handleHello(seq: UInt8, _ body: HelloBody) {
        guard body.version >= 1 else { return }  // version 0 is not defined (section 12); see NOTES.md S6
        switch state {
        case .closed:
            return
        case .opening:
            peer = body
            state = .ready
            awaitingHello = false
            helloSentAt = nil
            expectedSeq = seq &+ 1
            emit(.peerReady(peer: body, packetSize: packetSize))
            onOutputAvailable?()
        case .ready:
            // Peer-initiated reset (section 3): drop everything, answer with our HELLO once.
            dropAll(reason: .reset)
            peer = body
            expectedSeq = seq &+ 1
            queueHello(awaitReply: false)
            emit(.peerReady(peer: body, packetSize: packetSize))
        }
    }

    private func queueHello(awaitReply: Bool) {
        let body = config.hello
        // A HELLO restarts our seq counter; anything queued before it belongs to the old session.
        control.removeAll()
        controlIsHello.removeAll()
        control.append { .hello(seq: $0, body) }
        controlIsHello.append(true)
        awaitingHello = awaitReply
        if awaitReply { helloSentAt = nil }
        onOutputAvailable?()
    }

    // MARK: - receiver

    private func checkSeq(_ seq: UInt8) {
        if seq != expectedSeq {
            // Gap: some stack dropped (seq - expectedSeq) packets. Resync every incomplete message that is not
            // already waiting for a MSG_RESEND (section 8.1; NOTES.md S2).
            for inc in incoming where !inc.resync {
                inc.resync = true
                inc.mismatchResumeSent = false
                queueControl { .resume(seq: $0, msgId: inc.id, offset: UInt32(inc.received)) }
            }
        }
        expectedSeq = seq &+ 1
    }

    private func handleStart(id: UInt16, total: UInt32, crc: UInt32, payload: Data, now: Double) {
        unknownResumeSent[id] = nil
        if let st = doneMemory.first(where: { $0.id == id })?.status ?? rejectedMemory.first(where: { $0.id == id })?.status {
            queueControl { .ack(seq: $0, msgId: id, status: st) }
            return
        }
        silentIds.removeAll { $0 == id }
        if findIncoming(id) != nil {
            removeIncoming(id)  // restart (section 8.2)
        } else if config.maxMessage != 0xFFFF_FFFF, total > config.maxMessage {
            reject(id, status: 2)
            return
        } else if incoming.count >= Int(config.hello.window) {
            reject(id, status: 3)
            return
        }
        let inc = Incoming(id: id, total: Int(total), crc: crc, now: now)
        incoming.append(inc)
        append(inc, payload, now: now)
    }

    private func reject(_ id: UInt16, status: UInt8) {
        rejectedMemory.append((id, status))
        if rejectedMemory.count > 16 { rejectedMemory.removeFirst() }
        queueControl { .ack(seq: $0, msgId: id, status: status) }
    }

    private func handleUnknownData(_ id: UInt16, now: Double) {
        if doneMemory.contains(where: { $0.id == id }) || rejectedMemory.contains(where: { $0.id == id })
            || silentIds.contains(id) || unknownResumeSent[id] != nil {
            return
        }
        unknownResumeSent[id] = now
        queueControl { .resume(seq: $0, msgId: id, offset: 0) }
    }

    private func append(_ inc: Incoming, _ payload: Data, now: Double) {
        if inc.received + payload.count > inc.total {
            // Corrupt sender (section 6.3): fail with status 1; not remembered, so a retry with the same id works.
            removeIncoming(inc.id)
            queueControl { .ack(seq: $0, msgId: inc.id, status: 1) }
            return
        }
        inc.lastActivity = now
        inc.idleResumes = 0
        let before = inc.received
        if !payload.isEmpty {
            inc.running.update(payload)
            inc.buffer.append(payload)
            inc.received += payload.count
        }
        if let p = peer, p.wantsProgress, config.wantsProgress, inc.received / 65536 > before / 65536, inc.received < inc.total {
            let id = inc.id, n = UInt32(inc.received)
            queueControl { .progress(seq: $0, msgId: id, bytesReceived: n) }
        }
        if inc.received >= inc.total {
            removeIncoming(inc.id)
            if inc.running.value == inc.crc {
                doneMemory.append((inc.id, 0))
                if doneMemory.count > Int(config.hello.window) { doneMemory.removeFirst() }
                queueControl { .ack(seq: $0, msgId: inc.id, status: 0) }
                emit(.progress(MessageProgress(msgId: inc.id, direction: .incoming, bytes: inc.received, total: inc.total)))
                emit(.received(msgId: inc.id, data: inc.buffer))
            } else {
                queueControl { .ack(seq: $0, msgId: inc.id, status: 1) }
            }
        } else if inc.received - inc.reported >= max(inc.total / 100, 1) {
            inc.reported = inc.received
            emit(.progress(MessageProgress(msgId: inc.id, direction: .incoming, bytes: inc.received, total: inc.total)))
        }
    }

    private func findIncoming(_ id: UInt16) -> Incoming? { incoming.first { $0.id == id } }
    private func removeIncoming(_ id: UInt16) { incoming.removeAll { $0.id == id } }
    private func rememberSilent(_ id: UInt16) {
        silentIds.append(id)
        if silentIds.count > 16 { silentIds.removeFirst() }
    }

    // MARK: - sender

    private var peerWindow: Int { peer?.effectiveWindow ?? 1 }

    private func promote() {
        guard state == .ready else { return }
        while !pending.isEmpty && active.count < peerWindow {
            active.append(pending.removeFirst())
        }
    }

    private func finish(_ m: Outgoing, failure: BleLinkFailure?, cancelReason: UInt8?) {
        active.removeAll { $0 === m }
        if let r = cancelReason {
            let id = m.id
            queueControl { .cancel(seq: $0, msgId: id, reason: r) }
        }
        if let f = failure { emit(.failed(msgId: m.id, reason: f)) } else { emit(.sent(msgId: m.id)) }
        promote()
        if hasOutput { onOutputAvailable?() }
    }

    // MARK: - common

    private func queueControl(_ make: @escaping (UInt8) -> Packet) {
        control.append(make)
        controlIsHello.append(false)
        onOutputAvailable?()
    }

    /// Drops all session state. `reason` nil = silently (fresh start).
    private func dropAll(reason: BleLinkFailure?) {
        let failed = active + pending
        active.removeAll()
        pending.removeAll()
        incoming.removeAll()
        doneMemory.removeAll()
        rejectedMemory.removeAll()
        silentIds.removeAll()
        unknownResumeSent.removeAll()
        control.removeAll()
        controlIsHello.removeAll()
        nextMsgId = 0
        rr = 0
        blockedSince = nil
        peer = nil
        if let r = reason { for m in failed { emit(.failed(msgId: m.id, reason: r)) } }
    }

    private func emit(_ e: BleLinkEvent) { onEvent?(e) }
}

// MARK: - per-message state

final class Outgoing {
    enum Phase { case sending, waitAck }
    let id: UInt16
    let data: Data
    let crc: UInt32
    var phase: Phase = .sending
    var offset = 0
    var needStart = true
    var resendAt: Int?
    var restartPending = false
    var restartOffset = 0
    var everSent = false
    var lastActivity: Double = 0
    var timeoutResends = 0
    var crcRetries = 0
    var restarts = 0
    var reportedBytes = 0

    init(id: UInt16, data: Data, crc: UInt32) {
        self.id = id
        self.data = data
        self.crc = crc
    }

    func slice(_ from: Int, _ count: Int) -> Data {
        let s = data.startIndex + from
        return data.subdata(in: s..<(s + count))
    }

    func restart(at off: Int) {
        phase = .sending
        restartPending = true
        restartOffset = off
        reportedBytes = off
        if off == 0 {
            needStart = true
            resendAt = nil
            offset = 0
        } else {
            needStart = false
            resendAt = off
            offset = off
        }
    }
}

final class Incoming {
    let id: UInt16
    let total: Int
    let crc: UInt32
    var running = CRC32()
    var buffer = Data()
    var received = 0
    var reported = 0
    var resync = false
    var mismatchResumeSent = false
    var lastActivity: Double
    var idleResumes = 0

    init(id: UInt16, total: Int, crc: UInt32, now: Double) {
        self.id = id
        self.total = total
        self.crc = crc
        self.lastActivity = now
    }
}
