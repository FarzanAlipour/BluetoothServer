import XCTest
@testable import BleLinkCore

/// Session behaviour (PROTOCOL.md sections 3-10, 16) with the test playing the server.
final class SessionTests: XCTestCase {
    // MARK: handshake

    func testHelloFirstAndNotReadyBefore() throws {
        let h = Harness()
        XCTAssertThrowsError(try h.session.send(Data([1]))) { XCTAssertEqual($0 as? BleLinkError, .notReady) }
        h.session.start()
        XCTAssertEqual(h.session.packetSize, 20)
        let p = try XCTUnwrap(h.session.nextPacket())
        XCTAssertTrue(p.isHello)
        guard case let .hello(seq, body) = Packet.decode(p.data)! else { return XCTFail() }
        XCTAssertEqual(seq, 0)
        XCTAssertEqual(body, BleLinkConfig().hello)
        XCTAssertNil(h.session.nextPacket())
        XCTAssertThrowsError(try h.session.send(Data([1])))  // still OPENING
    }

    func testPacketsBeforeHelloAreIgnored() {
        let h = Harness()
        h.session.start()
        _ = h.session.nextPacket()
        h.session.receive(Packet.msgStart(seq: 1, msgId: 0, totalLen: 1, crc32: CRC32.checksum(Data([9])), payload: Data([9])).encode())
        XCTAssertTrue(h.received.isEmpty)
        XCTAssertNil(h.session.nextPacket())
    }

    func testHelloTimeoutResendsThreeTimesThenGivesUp() {
        let h = Harness()
        h.session.start()
        XCTAssertEqual(h.drain().count, 1)
        for _ in 0..<3 {
            h.clock.advance(10)
            h.session.tick()
            let out = h.drainPackets()
            XCTAssertEqual(out.count, 1)
            guard case .hello(0, _) = out[0] else { return XCTFail("expected HELLO resend") }
        }
        h.clock.advance(10)
        h.session.tick()
        XCTAssertEqual(h.session.state, .closed)
        XCTAssertTrue(h.events.contains { if case .helloTimeout = $0 { return true } else { return false } })
    }

    func testEffectiveTimeoutIsMaxOfBothSides() {
        let h = Harness()
        h.open(timeout: 120)  // server needs 30 s
        XCTAssertEqual(h.session.timeoutSeconds, 30)
        let h2 = Harness()
        h2.open(timeout: 4)  // server 1 s, ours default 10 s
        XCTAssertEqual(h2.session.timeoutSeconds, 10)
    }

    // MARK: sending

    func testSmallMessageAckOk() throws {
        let h = Harness()
        h.open()
        XCTAssertEqual(try h.session.send(Data("hello".utf8)), 0)
        XCTAssertEqual(try h.session.send(Data()), 1)
        let out = h.drainPackets()
        XCTAssertEqual(out.count, 2)
        guard case let .msgStart(1, 0, 5, crc, payload) = out[0] else { return XCTFail("\(out)") }
        XCTAssertEqual(crc, 0x3610_A686)
        XCTAssertEqual(payload, Data("hello".utf8))
        guard case .msgStart(2, 1, 0, 0, _) = out[1] else { return XCTFail("empty message \(out[1])") }
        var s = ServerSeq()
        h.session.receive(Packet.ack(seq: s.next(), msgId: 1, status: 0).encode())
        h.session.receive(Packet.ack(seq: s.next(), msgId: 0, status: 0).encode())
        XCTAssertEqual(h.sent, [1, 0])
        XCTAssertEqual(h.session.outgoingCount, 0)
    }

    func testTooLargeForPeerThrows() {
        let h = Harness()
        h.open(maxMessage: 10)
        XCTAssertThrowsError(try h.session.send(Data(count: 11))) { XCTAssertEqual($0 as? BleLinkError, .tooLarge(limit: 10)) }
        XCTAssertNoThrow(try h.session.send(Data(count: 10)))
    }

    func testPeerWindowLimitsMessagesInFlight() throws {
        let h = Harness()
        h.open(window: 1)
        try h.session.send(Data("a".utf8))
        try h.session.send(Data("b".utf8))
        XCTAssertEqual(h.drain().count, 1, "window 1: second message waits for the first ACK")
        h.session.receive(Packet.ack(seq: 1, msgId: 0, status: 0).encode())
        let out = h.drainPackets()
        guard out.count == 1, case .msgStart(_, 1, _, _, _) = out[0] else { return XCTFail("\(out)") }
    }

    func testSenderInterleavingMatchesVector() throws {
        let v = try Vectors.load()
        let iv = try XCTUnwrap(v["interleaving"] as? [String: Any])
        let h = Harness()
        h.open(serverMaxPacket: 20)
        h.session.nextMsgId = 10
        try h.session.send(testPayload(60))
        var out = [h.session.nextPacket()!.data, h.session.nextPacket()!.data]
        try h.session.send(Data("ping".utf8))  // small message jumps in after the 2nd packet
        out += h.drain()
        XCTAssertEqual(out.map(hexString), iv["packetsHex"] as! [String])
    }

    func testAckStatusesMapToFailures() throws {
        let cases: [(UInt8, BleLinkFailure)] = [(2, .tooLarge), (3, .busy), (4, .rejected(status: 4)), (9, .rejected(status: 9))]
        for (status, failure) in cases {
            let h = Harness()
            h.open()
            let id = try h.session.send(testPayload(100))
            _ = h.drain()
            h.session.receive(Packet.ack(seq: 1, msgId: id, status: status).encode())
            XCTAssertEqual(h.failed.map { $0.1 }, [failure], "status \(status)")
            XCTAssertEqual(h.drain(), [], "no retry, no CANCEL for status \(status)")
        }
    }

    func testEarlyNonZeroAckStopsSending() throws {
        let h = Harness()
        h.open(serverMaxPacket: 20)
        let id = try h.session.send(testPayload(200))
        _ = h.session.nextPacket()  // MSG_START only
        h.session.receive(Packet.ack(seq: 1, msgId: id, status: 3).encode())
        XCTAssertEqual(h.drain(), [])
        XCTAssertEqual(h.failed.map { $0.1 }, [.busy])
    }

    func testCrcAckResendsThreeTimesThenFails() throws {
        let h = Harness()
        h.open()
        let id = try h.session.send(Data("x".utf8))
        _ = h.drain()
        var s = ServerSeq()
        for _ in 0..<3 {
            h.session.receive(Packet.ack(seq: s.next(), msgId: id, status: 1).encode())
            let out = h.drainPackets()
            guard out.count == 1, case .msgStart(_, id, 1, _, _) = out[0] else { return XCTFail("\(out)") }
        }
        h.session.receive(Packet.ack(seq: s.next(), msgId: id, status: 1).encode())
        XCTAssertEqual(h.failed.map { $0.1 }, [.crc])
        XCTAssertEqual(h.drain(), [])
    }

    func testAckTimeoutResendsSameIdThenFailsWithCancel() throws {
        let h = Harness()
        h.open()
        let id = try h.session.send(Data("x".utf8))
        _ = h.drain()
        h.clock.advance(9.9)
        h.session.tick()
        XCTAssertEqual(h.drain(), [], "not yet")
        for _ in 0..<3 {
            h.clock.advance(10)
            h.session.tick()
            let out = h.drainPackets()
            guard out.count == 1, case .msgStart(_, id, _, _, _) = out[0] else { return XCTFail("\(out)") }
        }
        h.clock.advance(10)
        h.session.tick()
        XCTAssertEqual(h.failed.map { $0.1 }, [.timeout])
        let out = h.drainPackets()
        guard out.count == 1, case .cancel(_, id, 1) = out[0] else { return XCTFail("expected CANCEL reason 1, \(out)") }
    }

    func testAckTimersPausedWhileFlowBlocked() throws {
        let h = Harness()
        h.open()
        try h.session.send(Data("x".utf8))
        _ = h.drain()
        h.session.setFlowBlocked(true)
        h.clock.advance(30)
        h.session.tick()
        XCTAssertEqual(h.drain(), [], "blocked longer than T/2: timers paused")
        h.session.setFlowBlocked(false)
        h.clock.advance(5)
        h.session.tick()
        XCTAssertEqual(h.drain(), [], "timer restarted at unblock")
    }

    func testResumeDuplicatesIgnoredAndRestartLimit() throws {
        let h = Harness()
        h.open(serverMaxPacket: 20)
        let id = try h.session.send(testPayload(200))
        _ = h.drain()
        var s = ServerSeq()
        h.session.receive(Packet.resume(seq: s.next(), msgId: id, offset: 24).encode())
        h.session.receive(Packet.resume(seq: s.next(), msgId: id, offset: 24).encode())  // same gap, ignored
        let out = h.drainPackets()
        guard case .msgResend(_, id, 24, _) = out[0] else { return XCTFail("\(out)") }
        XCTAssertEqual(out.filter { if case .msgResend = $0 { return true } else { return false } }.count, 1)
        // RESUME(0) -> full restart with MSG_START
        h.session.receive(Packet.resume(seq: s.next(), msgId: id, offset: 0).encode())
        guard case .msgStart(_, id, 200, _, _) = h.drainPackets().first! else { return XCTFail() }
        // 2 restarts so far; 6 more allowed, the 9th fails
        for _ in 0..<6 {
            h.session.receive(Packet.resume(seq: s.next(), msgId: id, offset: 8).encode())
            XCTAssertFalse(h.drain().isEmpty)
        }
        h.session.receive(Packet.resume(seq: s.next(), msgId: id, offset: 8).encode())
        XCTAssertEqual(h.failed.map { $0.1 }, [.linkUnreliable])
        let last = h.drainPackets()
        guard last.count == 1, case .cancel(_, id, 1) = last[0] else { return XCTFail("\(last)") }
        // RESUME for a finished message: ignored
        h.session.receive(Packet.resume(seq: s.next(), msgId: id, offset: 0).encode())
        XCTAssertEqual(h.drain(), [])
    }

    func testCancelSendsCancelReason0() throws {
        let h = Harness()
        h.open(serverMaxPacket: 20)
        let id = try h.session.send(testPayload(100))
        _ = h.session.nextPacket()
        h.session.cancel(id)
        XCTAssertEqual(h.failed.map { $0.1 }, [.cancelled])
        let out = h.drainPackets()
        guard out.count == 1, case .cancel(_, id, 0) = out[0] else { return XCTFail("\(out)") }
        // queued, never sent: no CANCEL on the wire
        let h2 = Harness()
        h2.open(window: 1)
        try h2.session.send(Data([1]))
        let queued = try h2.session.send(Data([2]))
        _ = h2.drain()
        h2.session.cancel(queued)
        XCTAssertEqual(h2.drain(), [])
    }

    func testOutgoingProgressFromPeer() throws {
        let h = Harness()
        h.open(serverMaxPacket: 512)
        let id = try h.session.send(testPayload(200_000))
        _ = h.drain()
        h.session.receive(Packet.progress(seq: 1, msgId: id, bytesReceived: 65536).encode())
        XCTAssertTrue(h.events.contains {
            if case let .progress(p) = $0 { return p.confirmedByPeer && p.bytes == 65536 && p.total == 200_000 }
            return false
        })
    }

    // MARK: reset / disconnect

    func testPeerHelloWhileReadyResets() throws {
        let h = Harness()
        h.open()
        try h.session.send(Data("x".utf8))
        _ = h.drain()
        h.session.receive(Packet.msgStart(seq: 1, msgId: 0, totalLen: 50, crc32: 0, payload: Data(count: 8)).encode())
        h.session.receive(Packet.hello(seq: 0, HelloBody(maxPacket: 100, maxMessage: 1000, window: 2, flags: 0, timeout: 0)).encode())
        XCTAssertEqual(h.failed.map { $0.1 }, [.reset])
        XCTAssertEqual(h.session.peer?.maxPacket, 100)
        let out = h.drain()
        XCTAssertEqual(out.count, 1, "answer once with our own HELLO")
        XCTAssertEqual(out.first?.prefix(2), Data([0x01, 0x00]))
        XCTAssertEqual(try h.session.send(Data("y".utf8)), 0, "msgIds restart")
        XCTAssertEqual(h.drain().first?.prefix(2), Data([0x02, 0x01]), "seq restarts at 1 after HELLO")
        // the partial incoming message was discarded: its next DATA is unknown -> RESUME(0)
        h.session.receive(Packet.msgData(seq: 1, msgId: 0, payload: Data(count: 4)).encode())
        guard case .resume(_, 0, 0) = h.drainPackets().first! else { return XCTFail() }
    }

    func testClientResetFailsInFlightAndRedoesHello() throws {
        let h = Harness()
        h.open()
        try h.session.send(Data("x".utf8))
        h.session.reset()
        XCTAssertEqual(h.session.state, .opening)
        XCTAssertEqual(h.failed.map { $0.1 }, [.reset])
        let out = h.drain()
        XCTAssertEqual(out.count, 1)
        XCTAssertEqual(out[0][0], 0x01)
        h.session.receive(Packet.hello(seq: 0, HelloBody(maxPacket: 244, maxMessage: 1000, window: 4, flags: 0, timeout: 0)).encode())
        XCTAssertEqual(h.session.state, .ready)
        XCTAssertEqual(h.drain(), [], "the server's answer is not answered again")
    }

    func testLinkDownFailsEverythingWithDisconnected() throws {
        let h = Harness()
        h.open(window: 1)
        try h.session.send(Data([1]))
        try h.session.send(Data([2]))
        _ = h.drain()
        h.session.linkDown()
        XCTAssertEqual(h.failed.map { $0.1 }, [.disconnected, .disconnected])
        XCTAssertEqual(h.session.state, .closed)
        XCTAssertNil(h.session.nextPacket())
    }

    // MARK: receiving

    private func start(_ seq: UInt8, _ id: UInt16, _ data: Data, crc: UInt32? = nil) -> Data {
        Packet.msgStart(seq: seq, msgId: id, totalLen: UInt32(data.count), crc32: crc ?? CRC32.checksum(data), payload: data).encode()
    }

    func testDuplicateStartAfterCompletionIsReAckedNotRedelivered() throws {
        let h = Harness()
        h.open()
        h.session.receive(start(1, 30, Data("abc".utf8)))
        h.session.receive(start(2, 30, Data("abc".utf8)))  // retry after a lost ACK
        XCTAssertEqual(h.received.count, 1)
        XCTAssertEqual(h.drain().map(hexString), ["04011e0000", "04021e0000"])
    }

    func testIncomingCrcMismatchAcksStatus1() {
        let h = Harness()
        h.open()
        h.session.receive(start(1, 4, Data("abc".utf8), crc: 0x1234))
        XCTAssertTrue(h.received.isEmpty)
        XCTAssertEqual(h.drain().map(hexString), ["0401040001"])
        // the sender's resend (same id) is accepted, since a CRC failure is not remembered as done
        h.session.receive(start(2, 4, Data("abc".utf8)))
        XCTAssertEqual(h.received.map { $0.1 }, [Data("abc".utf8)])
    }

    func testIncomingTooLargeAndWindowExceeded() {
        var c = BleLinkConfig()
        c.maxMessage = 100
        c.window = 1
        let h = Harness(config: c)
        h.open()
        h.session.receive(Packet.msgStart(seq: 1, msgId: 1, totalLen: 101, crc32: 0, payload: Data()).encode())
        XCTAssertEqual(h.drain().map(hexString), ["0401010002"])
        h.session.receive(Packet.msgStart(seq: 2, msgId: 2, totalLen: 50, crc32: 0, payload: Data(count: 8)).encode())
        h.session.receive(Packet.msgStart(seq: 3, msgId: 3, totalLen: 50, crc32: 0, payload: Data(count: 8)).encode())
        XCTAssertEqual(h.drain().map(hexString), ["0402030003"], "second concurrent message: busy")
        // following MSG_DATA for the rejected ids: ignored silently, no RESUME
        h.session.receive(Packet.msgData(seq: 4, msgId: 3, payload: Data(count: 4)).encode())
        h.session.receive(Packet.msgData(seq: 5, msgId: 1, payload: Data(count: 4)).encode())
        XCTAssertEqual(h.drain(), [])
    }

    func testOverlongPayloadFailsWithStatus1() {
        let h = Harness()
        h.open()
        h.session.receive(Packet.msgStart(seq: 1, msgId: 6, totalLen: 10, crc32: 0, payload: Data(count: 8)).encode())
        h.session.receive(Packet.msgData(seq: 2, msgId: 6, payload: Data(count: 5)).encode())
        XCTAssertEqual(h.drain().map(hexString), ["0401060001"])
    }

    func testUnknownMsgIdDataGetsOneResume0() {
        let h = Harness()
        h.open()
        h.session.receive(Packet.msgData(seq: 1, msgId: 9, payload: Data(count: 4)).encode())
        h.session.receive(Packet.msgData(seq: 2, msgId: 9, payload: Data(count: 4)).encode())
        h.session.receive(Packet.msgResend(seq: 3, msgId: 9, offset: 4, payload: Data(count: 4)).encode())
        XCTAssertEqual(h.drain().map(hexString), ["0501090000000000"])
    }

    func testResendWithWrongOffsetResumesOnce() {
        let h = Harness()
        h.open()
        let msg = testPayload(40)
        h.session.receive(Packet.msgStart(seq: 1, msgId: 2, totalLen: 40, crc32: CRC32.checksum(msg), payload: msg.prefix(8)).encode())
        h.session.receive(Packet.msgResend(seq: 2, msgId: 2, offset: 20, payload: msg[20..<30]).encode())
        h.session.receive(Packet.msgResend(seq: 3, msgId: 2, offset: 20, payload: msg[20..<30]).encode())
        XCTAssertEqual(h.drain().map(hexString), ["0501020008000000"])
        h.session.receive(Packet.msgResend(seq: 4, msgId: 2, offset: 8, payload: msg[8..<40]).encode())
        XCTAssertEqual(h.received.map { $0.1 }, [msg])
    }

    func testUnknownTypeAndTrailingBytesIgnoredSeqCounted() throws {
        let h = Harness()
        h.open()
        let id = try h.session.send(Data("x".utf8))
        _ = h.drain()
        h.session.receive(Packet.msgStart(seq: 1, msgId: 5, totalLen: 30, crc32: 0, payload: Data(count: 8)).encode())
        h.session.receive(Data([0x7F, 2, 0xAA]))  // unknown type, seq 2
        h.session.receive(Data([0x04, 3]))  // ACK too short: ignored, seq 3 still counts
        h.session.receive(Packet.ack(seq: 4, msgId: id, status: 0).encode() + Data([0xEE, 0xEE]))  // trailing bytes
        XCTAssertEqual(h.sent, [id])
        XCTAssertEqual(h.drain(), [], "no gap was detected")
    }

    func testSeqWrapIsNotAGap() {
        let h = Harness()
        h.open()
        h.session.receive(Packet.msgStart(seq: 1, msgId: 1, totalLen: 300, crc32: 0, payload: Data()).encode())
        for s in 2...255 { h.session.receive(Packet.msgData(seq: UInt8(s), msgId: 1, payload: Data([0])).encode()) }
        h.session.receive(Packet.msgData(seq: 0, msgId: 1, payload: Data([0])).encode())
        h.session.receive(Packet.msgData(seq: 1, msgId: 1, payload: Data([0])).encode())
        XCTAssertEqual(h.drain(), [])
    }

    func testCancelFromPeerFreesIncoming() {
        let h = Harness()
        h.open()
        h.session.receive(Packet.msgStart(seq: 1, msgId: 3, totalLen: 30, crc32: 0, payload: Data(count: 8)).encode())
        h.session.receive(Packet.cancel(seq: 2, msgId: 3, reason: 0).encode())
        h.session.receive(Packet.msgData(seq: 3, msgId: 3, payload: Data(count: 4)).encode())
        h.clock.advance(60)
        h.session.tick()
        XCTAssertEqual(h.drain(), [], "no ACK, no RESUME after CANCEL")
    }

    func testIdleTimeoutResumesThreeTimesThenDiscards() {
        let h = Harness()
        h.open()
        h.session.receive(Packet.msgStart(seq: 1, msgId: 8, totalLen: 30, crc32: 0, payload: Data(count: 8)).encode())
        for _ in 0..<3 {
            h.clock.advance(10)
            h.session.tick()
            XCTAssertEqual(h.drainPackets().count, 1)
        }
        h.clock.advance(10)
        h.session.tick()
        XCTAssertEqual(h.drain(), [])
        h.clock.advance(10)
        h.session.tick()
        XCTAssertEqual(h.drain(), [], "receiver sends nothing more")
    }

    func testIncomingProgressSentWhenPeerAsks() {
        let h = Harness()
        h.open(flags: HelloBody.flagProgress)
        let msg = testPayload(70_000)
        h.session.receive(Packet.msgStart(seq: 1, msgId: 1, totalLen: 70_000, crc32: CRC32.checksum(msg), payload: msg.prefix(500)).encode())
        var off = 500
        var s: UInt8 = 2
        while off < msg.count {
            let n = min(508, msg.count - off)
            h.session.receive(Packet.msgData(seq: s, msgId: 1, payload: msg[off..<(off + n)]).encode())
            off += n
            s &+= 1
        }
        let out = h.drainPackets()
        XCTAssertEqual(out.count, 2)
        guard case .progress(_, 1, let n) = out[0], n >= 65536, n < 65536 + 508 else { return XCTFail("\(out)") }
        guard case .ack(_, 1, 0) = out[1] else { return XCTFail("\(out)") }
        XCTAssertEqual(h.received.map { $0.1 }, [msg])
    }

    func testNoProgressWhenPeerDidNotAsk() {
        let h = Harness()
        h.open(flags: 0)
        let msg = testPayload(70_000)
        h.session.receive(Packet.msgStart(seq: 1, msgId: 1, totalLen: 70_000, crc32: CRC32.checksum(msg), payload: msg.prefix(500)).encode())
        h.session.receive(Packet.msgData(seq: 2, msgId: 1, payload: msg[500...]).encode())
        XCTAssertEqual(h.drainPackets().count, 1)
    }
}
