import Foundation
import XCTest
@testable import BleLinkCore

/// Loads docs/protocol-test-vectors.json from the repository (path relative to this file), or from
/// $BLELINK_VECTORS. The vectors are not copied into the package so they cannot drift from the spec.
enum Vectors {
    static func load() throws -> [String: Any] {
        let path: String
        if let env = ProcessInfo.processInfo.environment["BLELINK_VECTORS"] {
            path = env
        } else {
            var url = URL(fileURLWithPath: #filePath)
            for _ in 0..<5 { url.deleteLastPathComponent() }  // Tests/BleLinkCoreTests/x.swift -> clients/
            path = url.deletingLastPathComponent().appendingPathComponent("docs/protocol-test-vectors.json").path
        }
        let data = try Data(contentsOf: URL(fileURLWithPath: path))
        return try XCTUnwrap(JSONSerialization.jsonObject(with: data) as? [String: Any])
    }
}

func hex(_ s: String) -> Data {
    var d = Data()
    var i = s.startIndex
    while i < s.endIndex {
        let j = s.index(i, offsetBy: 2)
        d.append(UInt8(s[i..<j], radix: 16)!)
        i = j
    }
    return d
}

func hexString(_ d: Data) -> String { d.map { String(format: "%02x", $0) }.joined() }

/// payloadRule from the vectors: byte[i] = (i * 7 + 3) mod 256.
func testPayload(_ n: Int) -> Data { Data((0..<n).map { UInt8(truncatingIfNeeded: $0 &* 7 &+ 3) }) }

final class ManualClock {
    var now: Double = 1000
    func advance(_ s: Double) { now += s }
}

/// A client session driven by hand: captures events, lets the test play the server.
final class Harness {
    let clock = ManualClock()
    let session: BleLinkSession
    var events: [BleLinkEvent] = []

    init(config: BleLinkConfig = BleLinkConfig()) {
        let c = clock
        session = BleLinkSession(config: config, clock: { c.now })
        session.onEvent = { [unowned self] in self.events.append($0) }
    }

    /// start() + pull our HELLO + feed the server's HELLO -> READY.
    func open(serverMaxPacket: UInt16 = 512, osLimit: Int = 512, window: UInt8 = 4, maxMessage: UInt32 = 16 << 20,
              flags: UInt8 = 0, timeout: UInt8 = 0) {
        session.setOsLimit(osLimit)
        session.start()
        let hello = session.nextPacket()
        XCTAssertEqual(hello?.isHello, true)
        session.receive(Packet.hello(seq: 0, HelloBody(maxPacket: serverMaxPacket, maxMessage: maxMessage,
                                                       window: window, flags: flags, timeout: timeout)).encode())
        XCTAssertEqual(session.state, .ready)
    }

    func drain() -> [Data] {
        var out: [Data] = []
        while let p = session.nextPacket() { out.append(p.data) }
        return out
    }

    func drainPackets() -> [Packet] { drain().map { Packet.decode($0)! } }

    var received: [(UInt16, Data)] {
        events.compactMap { if case let .received(id, d) = $0 { return (id, d) } else { return nil } }
    }
    var sent: [UInt16] { events.compactMap { if case let .sent(id) = $0 { return id } else { return nil } } }
    var failed: [(UInt16, BleLinkFailure)] {
        events.compactMap { if case let .failed(id, r) = $0 { return (id, r) } else { return nil } }
    }
}

/// Server-side packet builder with its own seq counter (seq 0 = its HELLO).
struct ServerSeq {
    var seq: UInt8 = 1
    mutating func next() -> UInt8 { defer { seq &+= 1 }; return seq }
}
