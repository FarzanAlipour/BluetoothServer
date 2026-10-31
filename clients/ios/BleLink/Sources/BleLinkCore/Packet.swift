import Foundation

/// Packet type byte (PROTOCOL.md 2.3).
public enum PacketType: UInt8 {
    case hello = 0x01
    case msgStart = 0x02
    case msgData = 0x03
    case ack = 0x04
    case resume = 0x05
    case cancel = 0x06
    case progress = 0x07
    case msgResend = 0x08
}

/// HELLO body: the receive-side limits of the side that sends it (PROTOCOL.md 4.1).
/// The same 10 bytes are the value of the INFO characteristic.
public struct HelloBody: Equatable {
    public static let flagProgress: UInt8 = 0x01
    public static let flagStreaming: UInt8 = 0x02
    public static let encodedLength = 10

    public var version: UInt8
    public var maxPacket: UInt16
    public var maxMessage: UInt32
    public var window: UInt8
    public var flags: UInt8
    /// 0.25 s units; 0 means the default 40 (10 s).
    public var timeout: UInt8

    public init(version: UInt8 = 1, maxPacket: UInt16, maxMessage: UInt32, window: UInt8, flags: UInt8, timeout: UInt8) {
        self.version = version
        self.maxPacket = maxPacket
        self.maxMessage = maxMessage
        self.window = window
        self.flags = flags
        self.timeout = timeout
    }

    /// maxPacket clamped to 20...512 as section 4.1 requires of the receiver of a HELLO.
    public var effectiveMaxPacket: Int { min(max(Int(maxPacket), 20), 512) }
    /// window, with the illegal value 0 treated as 1 (see NOTES.md, spec issue S5).
    public var effectiveWindow: Int { max(Int(window), 1) }
    /// Timeout in seconds with 0 replaced by the default 40 * 0.25 s.
    public var timeoutSeconds: Double { Double(timeout == 0 ? 40 : timeout) * 0.25 }
    public var wantsProgress: Bool { flags & HelloBody.flagProgress != 0 }
    public var isStreamingReceiver: Bool { flags & HelloBody.flagStreaming != 0 }

    /// Decodes an INFO characteristic value (HELLO body without type and seq). Trailing bytes are ignored.
    public static func decodeInfo<D: DataProtocol>(_ data: D) -> HelloBody? {
        let b = [UInt8](data)
        guard b.count >= encodedLength else { return nil }
        var r = Reader(b, 0)
        return HelloBody(version: r.u8(), maxPacket: r.u16(), maxMessage: r.u32(), window: r.u8(),
                         flags: r.u8(), timeout: r.u8())
    }

    func write(to w: inout [UInt8]) {
        w.append(version); w.appendLE(maxPacket); w.appendLE(maxMessage)
        w.append(window); w.append(flags); w.append(timeout)
    }
}

/// One BleLink packet. `encode()` produces the exact wire bytes; `decode()` follows the rules in 2.3:
/// unknown types and packets shorter than their fixed layout decode to `.unknown` / nil respectively
/// (both MUST be ignored, but their seq still counts), trailing bytes after fixed-size packets are ignored.
public enum Packet: Equatable {
    case hello(seq: UInt8, HelloBody)
    case msgStart(seq: UInt8, msgId: UInt16, totalLen: UInt32, crc32: UInt32, payload: Data)
    case msgData(seq: UInt8, msgId: UInt16, payload: Data)
    case ack(seq: UInt8, msgId: UInt16, status: UInt8)
    case resume(seq: UInt8, msgId: UInt16, offset: UInt32)
    case cancel(seq: UInt8, msgId: UInt16, reason: UInt8)
    case progress(seq: UInt8, msgId: UInt16, bytesReceived: UInt32)
    case msgResend(seq: UInt8, msgId: UInt16, offset: UInt32, payload: Data)
    /// Unknown type: ignored, but the seq is counted.
    case unknown(type: UInt8, seq: UInt8)

    public static let helloLength = 12
    public static let startHeader = 12
    public static let dataHeader = 4
    public static let resendHeader = 8

    public var seq: UInt8 {
        switch self {
        case let .hello(s, _), let .msgStart(s, _, _, _, _), let .msgData(s, _, _), let .ack(s, _, _),
             let .resume(s, _, _), let .cancel(s, _, _), let .progress(s, _, _), let .msgResend(s, _, _, _),
             let .unknown(_, s):
            return s
        }
    }

    public func encode() -> Data {
        var w: [UInt8] = []
        switch self {
        case let .hello(seq, body):
            w = [PacketType.hello.rawValue, seq]; body.write(to: &w)
        case let .msgStart(seq, id, total, crc, payload):
            w = [PacketType.msgStart.rawValue, seq]; w.appendLE(id); w.appendLE(total); w.appendLE(crc)
            w.append(contentsOf: payload)
        case let .msgData(seq, id, payload):
            w = [PacketType.msgData.rawValue, seq]; w.appendLE(id); w.append(contentsOf: payload)
        case let .ack(seq, id, status):
            w = [PacketType.ack.rawValue, seq]; w.appendLE(id); w.append(status)
        case let .resume(seq, id, offset):
            w = [PacketType.resume.rawValue, seq]; w.appendLE(id); w.appendLE(offset)
        case let .cancel(seq, id, reason):
            w = [PacketType.cancel.rawValue, seq]; w.appendLE(id); w.append(reason)
        case let .progress(seq, id, n):
            w = [PacketType.progress.rawValue, seq]; w.appendLE(id); w.appendLE(n)
        case let .msgResend(seq, id, offset, payload):
            w = [PacketType.msgResend.rawValue, seq]; w.appendLE(id); w.appendLE(offset)
            w.append(contentsOf: payload)
        case let .unknown(type, seq):
            w = [type, seq]
        }
        return Data(w)
    }

    /// Returns nil when the packet is too short to carry a header or its fixed layout.
    /// Callers that track seq should still count a short packet's seq if it has >= 2 bytes
    /// (see `Packet.rawSeq`).
    public static func decode<D: DataProtocol>(_ data: D) -> Packet? {
        let b = [UInt8](data)
        guard b.count >= 2 else { return nil }
        let seq = b[1]
        var r = Reader(b, 2)
        guard let type = PacketType(rawValue: b[0]) else { return .unknown(type: b[0], seq: seq) }
        switch type {
        case .hello:
            guard b.count >= helloLength else { return nil }
            return .hello(seq: seq, HelloBody(version: r.u8(), maxPacket: r.u16(), maxMessage: r.u32(),
                                              window: r.u8(), flags: r.u8(), timeout: r.u8()))
        case .msgStart:
            guard b.count >= startHeader else { return nil }
            return .msgStart(seq: seq, msgId: r.u16(), totalLen: r.u32(), crc32: r.u32(), payload: Data(b[startHeader...]))
        case .msgData:
            guard b.count >= dataHeader else { return nil }
            return .msgData(seq: seq, msgId: r.u16(), payload: Data(b[dataHeader...]))
        case .ack:
            guard b.count >= 5 else { return nil }
            return .ack(seq: seq, msgId: r.u16(), status: r.u8())
        case .resume:
            guard b.count >= 8 else { return nil }
            return .resume(seq: seq, msgId: r.u16(), offset: r.u32())
        case .cancel:
            guard b.count >= 5 else { return nil }
            return .cancel(seq: seq, msgId: r.u16(), reason: r.u8())
        case .progress:
            guard b.count >= 8 else { return nil }
            return .progress(seq: seq, msgId: r.u16(), bytesReceived: r.u32())
        case .msgResend:
            guard b.count >= resendHeader else { return nil }
            return .msgResend(seq: seq, msgId: r.u16(), offset: r.u32(), payload: Data(b[resendHeader...]))
        }
    }

    /// seq byte of any packet with at least a 2-byte header, even if `decode` rejects it.
    public static func rawSeq<D: DataProtocol>(_ data: D) -> UInt8? {
        var it = data.makeIterator()
        guard it.next() != nil, let s = it.next() else { return nil }
        return s
    }
}

// MARK: - little-endian helpers

struct Reader {
    let b: [UInt8]
    var i: Int
    init(_ b: [UInt8], _ i: Int) { self.b = b; self.i = i }
    mutating func u8() -> UInt8 { defer { i += 1 }; return b[i] }
    mutating func u16() -> UInt16 { defer { i += 2 }; return UInt16(b[i]) | UInt16(b[i + 1]) << 8 }
    mutating func u32() -> UInt32 {
        defer { i += 4 }
        return UInt32(b[i]) | UInt32(b[i + 1]) << 8 | UInt32(b[i + 2]) << 16 | UInt32(b[i + 3]) << 24
    }
}

extension Array where Element == UInt8 {
    mutating func appendLE(_ v: UInt16) { append(UInt8(v & 0xFF)); append(UInt8(v >> 8)) }
    mutating func appendLE(_ v: UInt32) {
        append(UInt8(v & 0xFF)); append(UInt8((v >> 8) & 0xFF)); append(UInt8((v >> 16) & 0xFF)); append(UInt8(v >> 24))
    }
}
