"""Session behaviour against PROTOCOL.md, driven by a deterministic in-memory pump with a fake clock."""
from __future__ import annotations

from blelink import codec as c
from blelink import session as S
from blelink.session import Failed, Progressed, Ready, Session
from conftest import Pair, drain, payload_rule, ready_receiver, ready_sender


def big(n: int) -> bytes:
    return payload_rule(n)


# --- handshake / limits (sections 3, 4, 5) --------------------------------------------------------------

def test_handshake_and_packet_size_per_direction():
    p = Pair(Session("client", max_packet=100, os_limit=182), Session("server", os_limit=244))
    p.handshake()
    assert p.client.state == p.server.state == S.READY
    assert p.client.tx_packet_size == 182          # min(server maxPacket 512, client os 182)
    assert p.server.tx_packet_size == 100          # min(client maxPacket 100, server os 244)
    assert [type(e) for e in p.client.events] == [Ready]
    assert p.wire[0] == ("c2s", c.encode(c.Hello(0, 1, 100, 16 * 1024 * 1024, 4, c.FLAG_PROGRESS, 0)))
    assert p.wire[1][1][0] == c.HELLO and p.wire[1][1][1] == 0


def test_packet_size_clamped_and_20_before_hello():
    s = Session("client", os_limit=10)
    assert s.tx_packet_size == 20
    s.start(0.0)
    drain(s)
    s.on_packet(c.encode(c.Hello(0, 1, 5000, 100, 1, 0, 0)), 0.0)  # maxPacket out of range -> clamp
    assert s.tx_packet_size == 20
    s.set_os_limit(600)
    assert s.tx_packet_size == 512


def test_timeout_is_max_of_both_sides():
    p = Pair(Session("client", timeout_units=0), Session("server", timeout_units=120))
    p.handshake()
    assert p.client.timeout_s == p.server.timeout_s == 30.0


def test_hello_trailing_bytes_and_newer_version_accepted():
    s = Session("client")
    s.start(0.0)
    drain(s)
    s.on_packet(c.encode(c.Hello(0, 7, 244, 100, 2, 0, 0)) + b"\xff\xff\xff", 0.0)
    assert s.state == S.READY and s.peer.version == 7


def test_packets_before_ready_are_ignored():
    srv = Session("server")
    srv.on_packet(c.encode(c.MsgStart(1, 0, 3, c.crc32(b"abc"), b"abc")), 0.0)
    assert srv.events == [] and drain(srv) == []


def test_hello_timeout_resends_then_fails():
    p = Pair(drop=lambda d, raw: d == "c2s")
    p.handshake()
    for _ in range(5):
        p.advance(10.0)
        p.pump()
    sent = [raw for d, raw in p.wire if d == "c2s" and raw[0] == c.HELLO]
    assert len(sent) == 4                             # first + 3 resends
    assert any(isinstance(e, Failed) and e.reason == "hello_timeout" for e in p.client.events)
    assert p.client.state == S.CLOSED


# --- messages, window, ordering (section 6) -------------------------------------------------------------

def test_small_and_empty_messages_both_directions():
    p = Pair()
    p.handshake()
    a = p.client.send(b"hello")
    e = p.client.send(b"")
    b = p.server.send(b"from server")
    p.pump()
    assert a.ok and e.ok and b.ok
    assert [d.data for d in Pair.delivered(p.server)] == [b"hello", b""]
    assert [d.data for d in Pair.delivered(p.client)] == [b"from server"]


def test_large_message_and_progress():
    p = Pair()
    p.handshake()
    data = big(200_000)
    m = p.client.send(data)
    p.pump()
    assert m.ok
    assert [d.data for d in Pair.delivered(p.server)] == [data]
    prog = [e.bytes_received for e in p.client.events if isinstance(e, Progressed)]
    assert len(prog) == 3 and [b // 65536 for b in prog] == [1, 2, 3]
    assert all(len(raw) <= 244 for _, raw in p.wire)


def test_window_bounds_messages_in_flight():
    p = Pair(server=Session("server", window=2, os_limit=244))
    p.handshake()
    msgs = [p.client.send(big(2000)) for _ in range(5)]
    # Transmit without delivering: only 2 may be started.
    for _ in range(200):
        if p.client.poll_transmit(0.0) is None:
            break
    started = {m.msg_id for m in msgs if m.msg_id >= 0}
    assert len(started) == 2
    assert len(p.client.in_flight()) == 2


def test_window_full_flow_delivers_everything_in_order_of_completion():
    p = Pair(server=Session("server", window=2, os_limit=244))
    p.handshake()
    msgs = [p.client.send(big(n)) for n in (5000, 10, 3000, 1, 0)]
    p.pump()
    assert all(m.ok for m in msgs)
    assert sorted(len(d.data) for d in Pair.delivered(p.server)) == [0, 1, 10, 3000, 5000]


def test_interleaving_small_overtakes_large():
    p = Pair()
    p.handshake()
    large = p.client.send(big(50_000))
    for _ in range(3):
        raw = p.client.poll_transmit(0.0)
        p.server.on_packet(raw, 0.0)
    small = p.client.send(b"ping")
    p.pump()
    assert large.ok and small.ok
    assert [len(d.data) for d in Pair.delivered(p.server)] == [4, 50_000]


def test_too_large_not_started_by_sender():
    p = Pair(server=Session("server", max_message=100, os_limit=244))
    p.handshake()
    m = p.client.send(big(101))
    p.pump()
    assert m.reason == S.TOO_LARGE
    assert not any(raw[0] == c.MSG_START for d, raw in p.wire if d == "c2s")


def test_receiver_early_ack_too_large_and_busy():
    r = ready_receiver(window=1, max_message=100)
    r.on_packet(c.encode(c.MsgStart(1, 7, 1000, 0, b"x")), 0.0)
    assert [c.decode(x) for x in drain(r)] == [c.Ack(1, 7, c.STATUS_TOO_LARGE)]
    r.on_packet(c.encode(c.MsgData(2, 7, b"y")), 0.0)      # ignored silently, no RESUME
    assert drain(r) == []
    r.on_packet(c.encode(c.MsgStart(3, 8, 50, 0, b"a")), 0.0)
    r.on_packet(c.encode(c.MsgStart(4, 9, 50, 0, b"b")), 0.0)   # window 1 already used by 8
    assert [c.decode(x) for x in drain(r)] == [c.Ack(2, 9, c.STATUS_BUSY)]


def test_sender_fails_on_early_ack():
    for status, reason in ((2, S.TOO_LARGE), (3, S.BUSY), (4, S.REJECTED), (77, S.REJECTED)):
        s = ready_sender(20)
        m = s.send(big(100))
        drain(s)
        s.on_packet(c.encode(c.Ack(1, m.msg_id, status)), 0.0)
        assert m.reason == reason and drain(s) == []


def test_crc_mismatch_detected_and_retried():
    r = ready_receiver()
    r.on_packet(c.encode(c.MsgStart(1, 3, 3, 0x12345678, b"abc")), 0.0)
    assert [c.decode(x) for x in drain(r)] == [c.Ack(1, 3, c.STATUS_CRC)]
    assert r.events == []
    # End to end: corrupt one payload byte once; the sender retries and the message arrives once.
    p = Pair()
    p.handshake()
    m = p.client.send(big(1000))
    raw = p.client.poll_transmit(0.0)
    p.server.on_packet(raw[:-1] + bytes([raw[-1] ^ 0xFF]), 0.0)   # corrupted MSG_START payload
    p.pump()
    assert m.ok and m.crc_resends == 1
    assert [d.data for d in Pair.delivered(p.server)] == [big(1000)]


def test_overflowing_payload_fails_with_crc_status():
    r = ready_receiver()
    r.on_packet(c.encode(c.MsgStart(1, 3, 4, 0, b"ab")), 0.0)
    r.on_packet(c.encode(c.MsgData(2, 3, b"cde")), 0.0)
    assert [c.decode(x) for x in drain(r)] == [c.Ack(1, 3, c.STATUS_CRC)]


# --- seq, forward compatibility (section 2) -------------------------------------------------------------

def test_unknown_type_and_short_packets_ignored_but_counted():
    r = ready_receiver()
    r.on_packet(bytes([0x42, 1, 9, 9, 9]), 0.0)          # unknown type, seq 1
    r.on_packet(bytes([c.ACK, 2, 0]), 0.0)               # too short ACK, seq 2
    r.on_packet(c.encode(c.MsgStart(3, 0, 2, c.crc32(b"ok"), b"ok")), 0.0)
    assert r.stats.seq_gaps == 0 and r.stats.ignored == 2
    assert [e.data for e in Pair.delivered(r)] == [b"ok"]
    r.on_packet(c.encode(c.Ack(4, 99, 0)) + b"future", 0.0)   # trailing bytes, unknown id: ignored
    assert r.stats.seq_gaps == 0


def test_seq_wraps_without_gap():
    p = Pair(Session("client", os_limit=20), Session("server", os_limit=20))
    p.handshake()
    m = p.client.send(big(16 * 600))
    p.pump()
    assert m.ok and p.server.stats.seq_gaps == 0
    assert sum(1 for d, _ in p.wire if d == "c2s") > 512


# --- loss and repair (sections 8, 10) -------------------------------------------------------------------

def drop_nth(direction: str, ptype: int, n: int):
    seen = {"i": 0}

    def drop(d, raw):
        if d == direction and raw[0] == ptype:
            seen["i"] += 1
            return seen["i"] == n
        return False
    return drop


def test_lost_data_packet_repaired_by_resume():
    p = Pair(drop=drop_nth("c2s", c.MSG_DATA, 5))
    p.handshake()
    m = p.client.send(big(5000))
    p.pump()
    assert m.ok
    assert [d.data for d in Pair.delivered(p.server)] == [big(5000)]
    assert p.server.stats.seq_gaps == 1 and p.server.stats.resumes_tx >= 1 and p.client.stats.restarts >= 1
    assert any(raw[0] == c.MSG_RESEND for d, raw in p.wire if d == "c2s")


def test_lost_data_with_two_interleaved_messages():
    p = Pair(drop=drop_nth("c2s", c.MSG_DATA, 3))
    p.handshake()
    a = p.client.send(big(3000))
    b = p.client.send(big(2000)[::-1])
    p.pump()
    assert a.ok and b.ok
    assert sorted(d.data for d in Pair.delivered(p.server)) == sorted([big(3000), big(2000)[::-1]])


def test_lost_msg_start_repaired_by_resume_zero():
    p = Pair(drop=drop_nth("c2s", c.MSG_START, 1))
    p.handshake()
    m = p.client.send(big(1000))
    p.pump()
    assert m.ok and [d.data for d in Pair.delivered(p.server)] == [big(1000)]
    resumes = [c.decode(raw) for d, raw in p.wire if d == "s2c" and raw[0] == c.RESUME]
    assert resumes[0].offset == 0


def test_lost_ack_resend_not_delivered_twice():
    for size in (10, 3000):     # single packet and multi packet message
        p = Pair(drop=drop_nth("s2c", c.ACK, 1))
        p.handshake()
        m = p.client.send(big(size))
        p.pump()
        assert not m.finished
        p.advance(10.0)          # ACK timeout T
        p.pump()
        assert m.ok and m.timeout_resends == 1, size
        assert len(Pair.delivered(p.server)) == 1
        assert p.server.stats.duplicates == 1


def test_lost_resume_repaired_by_idle_timeout():
    lose_data, lose_resume = drop_nth("c2s", c.MSG_DATA, 2), drop_nth("s2c", c.RESUME, 1)
    p = Pair(drop=lambda d, raw: lose_data(d, raw) or lose_resume(d, raw))
    p.handshake()
    m = p.client.send(big(2000))
    p.pump()
    assert not m.finished
    for _ in range(4):
        p.advance(10.0)
        p.pump()
        if m.finished:
            break
    assert m.ok and len(Pair.delivered(p.server)) == 1


def test_ack_timeout_gives_up_after_three_resends():
    p = Pair()
    p.handshake()
    p.drop = lambda d, raw: d == "s2c"
    m = p.client.send(b"x")
    p.pump()
    for _ in range(4):
        p.advance(10.0)
        p.pump()
    assert m.reason == S.TIMEOUT and m.timeout_resends == 3
    assert sum(1 for d, raw in p.wire if d == "c2s" and raw[0] == c.MSG_START) == 4
    assert any(raw[0] == c.CANCEL for d, raw in p.wire if d == "c2s")


def test_restart_limit_link_unreliable():
    s = ready_sender(20)
    m = s.send(big(500))
    drain(s)
    for i in range(9):
        s.on_packet(c.encode(c.Resume(1 + i, m.msg_id, 0)), 0.0)
        drain(s)
        if m.finished:
            break
    assert m.reason == S.LINK_UNRELIABLE


def test_duplicate_resume_from_one_gap_restarts_once():
    s = ready_sender(20)
    m = s.send(big(500))
    drain(s)
    s.on_packet(c.encode(c.Resume(1, m.msg_id, 24)), 0.0)
    s.on_packet(c.encode(c.Resume(2, m.msg_id, 24)), 0.0)
    assert m.restarts == 1


def test_resume_for_unknown_id_ignored():
    s = ready_sender(20)
    s.on_packet(c.encode(c.Resume(1, 1234, 0)), 0.0)
    assert drain(s) == []


def test_unknown_data_gets_one_resume_zero():
    r = ready_receiver()
    r.on_packet(c.encode(c.MsgData(1, 5, b"x")), 0.0)
    r.on_packet(c.encode(c.MsgData(2, 5, b"y")), 0.0)
    assert [c.decode(x) for x in drain(r)] == [c.Resume(1, 5, 0)]


def test_resend_with_wrong_offset_asks_again_once():
    r = ready_receiver()
    r.on_packet(c.encode(c.MsgStart(1, 5, 100, 0, b"12345678")), 0.0)
    r.on_packet(c.encode(c.MsgResend(2, 5, 50, b"zz")), 0.0)
    r.on_packet(c.encode(c.MsgResend(3, 5, 60, b"zz")), 0.0)
    assert [c.decode(x) for x in drain(r)] == [c.Resume(1, 5, 8)]


# --- cancel, reset, disconnect (sections 3, 11, 16) -----------------------------------------------------

def test_cancel_mid_message():
    p = Pair()
    p.handshake()
    m = p.client.send(big(5000))
    for _ in range(3):
        p.server.on_packet(p.client.poll_transmit(0.0), 0.0)
    p.client.cancel(m)
    p.pump()
    assert m.reason == S.CANCELLED and Pair.delivered(p.server) == []
    assert any(raw[0] == c.CANCEL for d, raw in p.wire if d == "c2s")
    assert p.server._in == {}


def test_client_reset_fails_in_flight_and_rehandshakes():
    p = Pair()
    p.handshake()
    m = p.client.send(big(5000))
    p.server.on_packet(p.client.poll_transmit(0.0), 0.0)
    p.client.reset(0.0)
    assert m.reason == S.RESET
    p.pump()
    assert p.client.state == p.server.state == S.READY
    m2 = p.client.send(b"after reset")
    p.pump()
    assert m2.ok and [d.data for d in Pair.delivered(p.server)] == [b"after reset"]
    assert sum(1 for d, raw in p.wire if d == "s2c" and raw[0] == c.HELLO) == 2


def test_hello_from_server_while_ready_resets_client():
    p = Pair()
    p.handshake()
    m = p.client.send(big(5000))
    p.client.poll_transmit(0.0)
    p.client.on_packet(c.encode(c.Hello(0, 1, 244, 1000, 1, 0, 0)), 0.0)
    assert m.reason == S.RESET and p.client.peer.window == 1
    assert c.decode(p.client.poll_transmit(0.0)) == c.Hello(0, *p.client._hello_fields())


def test_disconnect_fails_everything_and_restarts_ids():
    p = Pair()
    p.handshake()
    a = p.client.send(big(5000))
    b = p.client.send(b"queued")
    p.client.poll_transmit(0.0)
    p.client.on_disconnect()
    assert a.reason == b.reason == S.DISCONNECTED
    assert p.client.state == S.CLOSED and p.client.next_msg_id == 0


def test_msg_ids_increase_and_wrap():
    s = ready_sender(244, peer_window=255)
    s.next_msg_id = 0xFFFE
    ids = []
    for _ in range(3):
        m = s.send(b"x")
        drain(s)
        ids.append(m.msg_id)
    assert ids == [0xFFFE, 0xFFFF, 0]
