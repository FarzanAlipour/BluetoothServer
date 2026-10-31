import Foundation

/// This side's receive limits, sent in HELLO (PROTOCOL.md 4.1).
public struct BleLinkConfig: Equatable {
    /// Largest packet this side accepts (20...512). CoreBluetooth hands notifications of any negotiated size, so 512.
    public var maxPacket: UInt16 = 512
    /// Largest message this side accepts. Messages are reassembled in memory, so keep it bounded.
    public var maxMessage: UInt32 = 16 * 1024 * 1024
    /// Concurrent incoming messages accepted (>= 1).
    public var window: UInt8 = 4
    /// Ask the peer for PROGRESS packets (and send them ourselves when the peer asks).
    public var wantsProgress: Bool = true
    /// Idle timeout this side needs, 0.25 s units, 0 = 10 s.
    public var timeout: UInt8 = 0
    /// HELLO resends before giving up (section 10: 3).
    public var helloRetries: Int = 3

    public init() {}

    public var hello: HelloBody {
        HelloBody(version: 1, maxPacket: UInt16(min(max(Int(maxPacket), 20), 512)), maxMessage: maxMessage,
                  window: max(window, 1), flags: wantsProgress ? HelloBody.flagProgress : 0, timeout: timeout)
    }
}

/// Why a sent message failed (PROTOCOL.md 6.3, 10, 11, 18).
public enum BleLinkFailure: Equatable, CustomStringConvertible {
    case disconnected
    case reset
    case timeout
    case crc
    case tooLarge
    case busy
    case rejected(status: UInt8)
    case linkUnreliable
    case cancelled

    public var description: String {
        switch self {
        case .disconnected: return "disconnected"
        case .reset: return "reset"
        case .timeout: return "timeout"
        case .crc: return "crc"
        case .tooLarge: return "too_large"
        case .busy: return "busy"
        case let .rejected(s): return "rejected(\(s))"
        case .linkUnreliable: return "link_unreliable"
        case .cancelled: return "cancelled"
        }
    }
}

public enum TransferDirection: Equatable {
    case outgoing
    case incoming
}

public struct MessageProgress: Equatable {
    public var msgId: UInt16
    public var direction: TransferDirection
    /// Outgoing: bytes handed to the stack (or, if `confirmedByPeer`, bytes the peer reported via PROGRESS).
    /// Incoming: bytes received so far.
    public var bytes: Int
    public var total: Int
    public var confirmedByPeer: Bool

    public init(msgId: UInt16, direction: TransferDirection, bytes: Int, total: Int, confirmedByPeer: Bool = false) {
        self.msgId = msgId
        self.direction = direction
        self.bytes = bytes
        self.total = total
        self.confirmedByPeer = confirmedByPeer
    }

    public var fraction: Double { total == 0 ? 1 : Double(bytes) / Double(total) }
}

/// Everything the library reports to the application.
public enum BleLinkEvent {
    /// HELLO exchange finished (also after a reset); `peer` are the server's receive limits.
    case peerReady(peer: HelloBody, packetSize: Int)
    /// A complete, CRC-verified message from the server.
    case received(msgId: UInt16, data: Data)
    /// The server ACKed our message with status 0.
    case sent(msgId: UInt16)
    case failed(msgId: UInt16, reason: BleLinkFailure)
    case progress(MessageProgress)
    /// No server HELLO after all retries; the transport disconnects.
    case helloTimeout
    /// Connection gone (transport level). `error` is CoreBluetooth's, if any.
    case disconnected(error: Error?)
}

public enum BleLinkError: Error, Equatable, CustomStringConvertible {
    case notReady
    case tooLarge(limit: UInt32)
    case bluetoothUnavailable(String)
    case serviceNotFound
    case connectFailed(String)
    case helloTimeout
    case disconnected

    public var description: String {
        switch self {
        case .notReady: return "session not ready (no HELLO exchange yet)"
        case let .tooLarge(l): return "message larger than the peer's maxMessage \(l)"
        case let .bluetoothUnavailable(s): return "Bluetooth unavailable: \(s)"
        case .serviceNotFound: return "BleLink service or characteristics not found"
        case let .connectFailed(s): return "connect failed: \(s)"
        case .helloTimeout: return "no HELLO from the server"
        case .disconnected: return "disconnected"
        }
    }
}
