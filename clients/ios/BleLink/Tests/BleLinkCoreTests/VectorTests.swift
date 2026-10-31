import XCTest
@testable import BleLinkCore

/// Conformance against docs/protocol-test-vectors.json.
final class VectorTests: XCTestCase {
    var v: [String: Any] = [:]

    override func setUpWithError() throws { v = try Vectors.load() }

    func testCRC32() throws {
        let cases = try XCTUnwrap((v["crc32"] as? [String: Any])?["cases"] as? [[String: Any]])
        XCTAssertEqual(cases.count, 4)
        for c in cases {
            let input = (c["inputAscii"] as? String).map { Data($0.utf8) } ?? hex(c["inputHex"] as! String)
            XCTAssertEqual(String(format: "%08x", CRC32.checksum(input)), c["crc32"] as? String, "\(c)")
        }
        // running CRC over pieces equals one-shot
        var r = CRC32()
        r.update(Data("1234".utf8)); r.update(Data("56789".utf8))
        XCTAssertEqual(r.value, 0xCBF4_3926)
    }

    func testSinglePacketsDecodeAndReencode() throws {
        let packets = try XCTUnwrap(v["packets"] as? [[String: Any]])
        XCTAssertEqual(packets.count, 12)
        for p in packets {
            let bytes = hex(p["hex"] as! String)
            let f = p["fields"] as! [String: Any]
            XCTAssertEqual(bytes.count, p["length"] as? Int)
            let pk = try XCTUnwrap(Packet.decode(bytes), "\(p["name"]!)")
            XCTAssertEqual(pk.encode(), bytes, "re-encode \(p["name"]!)")
            XCTAssertEqual(Int(pk.seq), f["seq"] as? Int)
            func i(_ k: String) -> Int { f[k] as! Int }
            switch pk {
            case let .hello(_, b):
                XCTAssertEqual(i("type"), 1)
                XCTAssertEqual(Int(b.version), i("version"))
                XCTAssertEqual(Int(b.maxPacket), i("maxPacket"))
                XCTAssertEqual(Int(b.maxMessage), i("maxMessage"))
                XCTAssertEqual(Int(b.window), i("window"))
                XCTAssertEqual(Int(b.flags), i("flags"))
                XCTAssertEqual(Int(b.timeout), i("timeout"))
            case let .msgStart(_, id, total, crc, payload):
                XCTAssertEqual(i("type"), 2)
                XCTAssertEqual(Int(id), i("msgId"))
                XCTAssertEqual(Int(total), i("totalLen"))
                XCTAssertEqual(String(format: "%08x", crc), f["crc32"] as? String)
                XCTAssertEqual(hexString(payload), f["payloadHex"] as? String)
            case let .msgData(_, id, payload):
                XCTAssertEqual(i("type"), 3)
                XCTAssertEqual(Int(id), i("msgId"))
                XCTAssertEqual(hexString(payload), f["payloadHex"] as? String)
            case let .ack(_, id, status):
                XCTAssertEqual(i("type"), 4)
                XCTAssertEqual(Int(id), i("msgId"))
                XCTAssertEqual(Int(status), i("status"))
            case let .resume(_, id, off):
                XCTAssertEqual(i("type"), 5)
                XCTAssertEqual(Int(id), i("msgId"))
                XCTAssertEqual(Int(off), i("offset"))
            case let .cancel(_, id, reason):
                XCTAssertEqual(i("type"), 6)
                XCTAssertEqual(Int(id), i("msgId"))
                XCTAssertEqual(Int(reason), i("reason"))
            case let .progress(_, id, n):
                XCTAssertEqual(i("type"), 7)
                XCTAssertEqual(Int(id), i("msgId"))
                XCTAssertEqual(Int(n), i("bytesReceived"))
            case let .msgResend(_, id, off, payload):
                XCTAssertEqual(i("type"), 8)
                XCTAssertEqual(Int(id), i("msgId"))
                XCTAssertEqual(Int(off), i("offset"))
                XCTAssertEqual(hexString(payload), f["payloadHex"] as? String)
            case .unknown:
                XCTFail("unknown type in vectors")
            }
        }
    }

    func testClientHelloEncodesLikeVector() {
        var c = BleLinkConfig()
        c.maxPacket = 244; c.maxMessage = 16_777_216; c.window = 4; c.wantsProgress = true; c.timeout = 40
        XCTAssertEqual(hexString(Packet.hello(seq: 0, c.hello).encode()), "010001f40000000001040128")
    }

    func testInfo() throws {
        let info = try XCTUnwrap(v["info"] as? [String: Any])
        let b = try XCTUnwrap(HelloBody.decodeInfo(hex(info["hex"] as! String)))
        XCTAssertEqual(b, HelloBody(version: 1, maxPacket: 509, maxMessage: 16_777_216, window: 4, flags: 1, timeout: 0))
        XCTAssertEqual(b.timeoutSeconds, 10)
    }

    // MARK: message splits: sender produces exactly these bytes, receiver reassembles them

    func testMessageSplitsSender() throws {
        let splits = try XCTUnwrap((v["messageSplits"] as? [String: Any])?["cases"] as? [[String: Any]])
        XCTAssertEqual(splits.count, 4)
        for c in splits {
            let ps = c["packetSize"] as! Int
            let h = Harness()
            h.open(serverMaxPacket: UInt16(ps), osLimit: 512)  // packetSize = min(peer maxPacket, osLimit)
            XCTAssertEqual(h.session.packetSize, ps)
            h.session.nextMsgId = UInt16(c["msgId"] as! Int)
            let msg = testPayload(c["messageLength"] as! Int)
            XCTAssertEqual(String(format: "%08x", CRC32.checksum(msg)), c["crc32"] as? String)
            try h.session.send(msg)
            let out = h.drain().map(hexString)
            XCTAssertEqual(out, c["packetsHex"] as! [String], "packetSize \(ps)")
            XCTAssertEqual(out.count, c["packetCount"] as? Int)
        }
    }

    func testOsLimitBoundsPacketSize() throws {
        let h = Harness()
        h.open(serverMaxPacket: 512, osLimit: 182)
        XCTAssertEqual(h.session.packetSize, 182)
        h.session.setOsLimit(5)  // below the minimum -> 20
        XCTAssertEqual(h.session.packetSize, 20)
    }

    func testMessageSplitsReceiver() throws {
        let splits = try XCTUnwrap((v["messageSplits"] as? [String: Any])?["cases"] as? [[String: Any]])
        for c in splits {
            let h = Harness()
            h.open()
            for p in c["packetsHex"] as! [String] { h.session.receive(hex(p)) }
            XCTAssertEqual(h.received.count, 1)
            XCTAssertEqual(h.received.first?.0, UInt16(c["msgId"] as! Int))
            XCTAssertEqual(h.received.first?.1, testPayload(c["messageLength"] as! Int))
            // our HELLO had seq 0, so our ACK is seq 1
            XCTAssertEqual(h.drain().map(hexString), ["04010201" + "00"])
        }
    }

    func testBoundaryCases20() throws {
        let cases = try XCTUnwrap((v["boundaryCases20"] as? [String: Any])?["cases"] as? [[String: Any]])
        for c in cases {
            let n = c["messageLength"] as! Int
            let expected = c["packetsHex"] as! [String]
            // sender
            let s = Harness()
            s.open(serverMaxPacket: 20)
            s.session.nextMsgId = 5
            try s.session.send(testPayload(n))
            XCTAssertEqual(s.drain().map(hexString), expected, "sender length \(n)")
            // receiver
            let r = Harness()
            r.open()
            for p in expected { r.session.receive(hex(p)) }
            XCTAssertEqual(r.received.map { $0.1 }, [testPayload(n)], "receiver length \(n)")
        }
    }

    func testInterleavingReceiverDeliversInCompletionOrder() throws {
        let iv = try XCTUnwrap(v["interleaving"] as? [String: Any])
        let h = Harness()
        h.open()
        for p in iv["packetsHex"] as! [String] { h.session.receive(hex(p)) }
        XCTAssertEqual(h.received.map { Int($0.0) }, iv["deliveryOrder"] as! [Int])
        XCTAssertEqual(h.received[0].1, Data("ping".utf8))
        XCTAssertEqual(h.received[1].1, testPayload(60))
        XCTAssertEqual(h.drain().map(hexString), iv["ackPacketsHex"] as! [String])
    }

    func testResumeReceiver() throws {
        let rv = try XCTUnwrap(v["resume"] as? [String: Any])
        var orig = (rv["originalPacketsHex"] as! [String]).map(hex)
        orig.remove(at: rv["lostIndex"] as! Int)
        let h = Harness()
        h.open()
        for p in orig { h.session.receive(p) }  // seq 1,2,4,5
        XCTAssertEqual(h.drain().map(hexString), [rv["resumeHex"] as! String], "exactly one RESUME(20, 24)")
        XCTAssertTrue(h.received.isEmpty)
        // The vector's MSG_RESEND has seq 7 (one more gap after seq 5, see NOTES.md S1). The message is already
        // in resync, so the second gap must not produce another RESUME.
        for p in rv["senderResendPacketsHex"] as! [String] { h.session.receive(hex(p)) }
        XCTAssertEqual(h.received.map { $0.0 }, [20])
        XCTAssertEqual(h.received.first?.1, testPayload(60))
        let after = h.drainPackets()
        XCTAssertEqual(after.count, 1)
        guard case .ack(_, 20, 0) = after[0] else { return XCTFail("expected ACK ok, got \(after)") }
    }

    func testResumeSender() throws {
        let rv = try XCTUnwrap(v["resume"] as? [String: Any])
        let h = Harness()
        h.open(serverMaxPacket: 20)
        h.session.nextMsgId = 20
        try h.session.send(testPayload(60))
        XCTAssertEqual(h.drain().map(hexString), rv["originalPacketsHex"] as! [String])
        // server's RESUME: seq 1 from the server's side
        h.session.receive(hex(rv["resumeHex"] as! String))
        let resent = h.drain()
        let expected = (rv["senderResendPacketsHex"] as! [String]).map(hex)
        XCTAssertEqual(resent.count, expected.count)
        // Our next seq is 6 (nothing else was sent); the vector uses 7 (NOTES.md S1). Compare everything but seq.
        for (a, b) in zip(resent, expected) {
            XCTAssertEqual(a[0], b[0])
            XCTAssertEqual(a.dropFirst(2), b.dropFirst(2))
        }
        XCTAssertEqual(resent.map { $0[1] }, [6, 7, 8])
    }
}
