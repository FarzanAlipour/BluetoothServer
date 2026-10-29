"""Conformance against docs/protocol-test-vectors.json (both the codec and the session, sender and receiver)."""
from __future__ import annotations

import pytest

from blelink import codec as c
from blelink.session import Delivered
from conftest import drain, payload_rule, ready_receiver, ready_sender

H = bytes.fromhex

FIELD_MAP = {  # vector field name -> dataclass attribute
    "version": "version", "maxPacket": "max_packet", "maxMessage": "max_message", "window": "window",
    "flags": "flags", "timeout": "timeout", "msgId": "msg_id", "totalLen": "total_len", "status": "status",
    "offset": "offset", "reason": "reason", "bytesReceived": "bytes_received", "seq": "seq",
}
TYPE_MAP = {1: c.Hello, 2: c.MsgStart, 3: c.MsgData, 4: c.Ack, 5: c.Resume, 6: c.Cancel, 7: c.Progress,
            8: c.MsgResend}


def test_crc32_cases(vectors):
    for case in vectors["crc32"]["cases"]:
        data = case["inputAscii"].encode() if "inputAscii" in case else H(case["inputHex"])
        assert c.crc32(data) == int(case["crc32"], 16), case
    # incremental update gives the same result
    assert c.crc32(b"56789", c.crc32(b"1234")) == 0xCBF43926


def test_payload_rule_matches_vectors(vectors):
    assert "(i * 7 + 3) mod 256" in vectors["payloadRule"]
    assert c.crc32(payload_rule(1000)) == int(vectors["messageSplits"]["cases"][0]["crc32"], 16)


@pytest.mark.parametrize("idx", range(12))
def test_packet_decode_encode(vectors, idx):
    v = vectors["packets"][idx]
    raw = H(v["hex"])
    assert len(raw) == v["length"]
    p = c.decode(raw)
    f = v["fields"]
    assert isinstance(p, TYPE_MAP[f["type"]]), v["name"]
    for key, attr in FIELD_MAP.items():
        if key in f:
            assert getattr(p, attr) == f[key], (v["name"], key)
    if "crc32" in f:
        assert p.crc == int(f["crc32"], 16)
    if "payloadHex" in f:
        assert p.payload == H(f["payloadHex"])
    assert c.encode(p) == raw, v["name"]


def test_all_packet_vectors_covered(vectors):
    assert len(vectors["packets"]) == 12


def test_info_vector(vectors):
    raw = H(vectors["info"]["hex"])
    h = c.decode_info(raw)
    assert h is not None
    assert (h.version, h.max_packet, h.max_message, h.window, h.flags, h.timeout) == (1, 509, 16 * 1024 * 1024,
                                                                                    4, 1, 0)
    assert c.encode_info(h) == raw
    assert c.decode_info(raw[:9]) is None


def _split_cases(vectors):
    return [(case, None) for case in vectors["messageSplits"]["cases"]]


@pytest.mark.parametrize("which", range(4))
def test_message_split_codec(vectors, which):
    case = vectors["messageSplits"]["cases"][which]
    msg = payload_rule(case["messageLength"])
    pkts = c.split_message(msg, case["msgId"], case["packetSize"], case["firstSeq"])
    assert [p.hex() for p in pkts] == case["packetsHex"]
    assert len(pkts) == case["packetCount"]
    assert all(len(p) <= case["packetSize"] for p in pkts)


@pytest.mark.parametrize("which", range(4))
def test_message_split_session_sender(vectors, which):
    """The session, not just the codec helper, produces exactly the vector packets."""
    case = vectors["messageSplits"]["cases"][which]
    s = ready_sender(case["packetSize"])
    s.next_msg_id = case["msgId"]
    s.send(payload_rule(case["messageLength"]))
    assert [p.hex() for p in drain(s)] == case["packetsHex"]


@pytest.mark.parametrize("which", range(4))
def test_message_split_session_receiver(vectors, which):
    case = vectors["messageSplits"]["cases"][which]
    r = ready_receiver()
    for p in case["packetsHex"]:
        r.on_packet(H(p), 0.0)
    got = [e for e in r.events if isinstance(e, Delivered)]
    assert len(got) == 1 and got[0].msg_id == case["msgId"] and got[0].data == payload_rule(case["messageLength"])
    assert drain(r) == [c.encode(c.Ack(1, case["msgId"], 0))]


@pytest.mark.parametrize("which", range(4))
def test_boundary_cases_20(vectors, which):
    case = vectors["boundaryCases20"]["cases"][which]
    msg = payload_rule(case["messageLength"])
    assert c.crc32(msg) == int(case["crc32"], 16)
    s = ready_sender(20)
    s.next_msg_id = 5
    s.send(msg)
    assert [p.hex() for p in drain(s)] == case["packetsHex"]
    r = ready_receiver()
    for p in case["packetsHex"]:
        r.on_packet(H(p), 0.0)
    assert [e.data for e in r.events if isinstance(e, Delivered)] == [msg]


def test_interleaving_receiver(vectors):
    v = vectors["interleaving"]
    r = ready_receiver()
    for p in v["packetsHex"]:
        r.on_packet(H(p), 0.0)
    got = [e for e in r.events if isinstance(e, Delivered)]
    assert [e.msg_id for e in got] == v["deliveryOrder"]
    assert c.crc32(got[0].data) == int(v["msg11"]["crc32"], 16) and len(got[0].data) == v["msg11"]["length"]
    assert c.crc32(got[1].data) == int(v["msg10"]["crc32"], 16) and len(got[1].data) == v["msg10"]["length"]
    assert [p.hex() for p in drain(r)] == v["ackPacketsHex"]


def test_interleaving_sender_small_overtakes(vectors):
    """Sender side: a small message submitted while a large one is in flight jumps in (round-robin) and the
    per-message packet contents equal the vector's (exact interleaving order is not normative)."""
    v = vectors["interleaving"]
    s = ready_sender(20)
    s.next_msg_id = 10
    s.send(payload_rule(60))
    first = [s.poll_transmit(0.0), s.poll_transmit(0.0)]
    s.send(b"ping")
    rest = drain(s)
    pkts = first + rest
    want = [H(p) for p in v["packetsHex"]]
    assert sorted(p[2:] for p in pkts) == sorted(p[2:] for p in want)
    assert [p[1] for p in pkts] == list(range(1, len(want) + 1))         # seq strictly 1..N
    ping_at = next(i for i, p in enumerate(pkts) if p[2:4] == b"\x0b\x00")
    assert ping_at < len(pkts) - 1                                         # not last: it overtook msg 10


def test_resume_receiver(vectors):
    v = vectors["resume"]
    orig = [H(p) for p in v["originalPacketsHex"]]
    r = ready_receiver()
    for i, p in enumerate(orig):
        if i == v["lostIndex"]:
            continue
        r.on_packet(p, 0.0)
        if p[1] == 4:   # seq 4 reveals the gap
            assert drain(r) == [H(v["resumeHex"])]
    assert not [e for e in r.events if isinstance(e, Delivered)]   # seq 5 was dropped (resync)
    for p in v["senderResendPacketsHex"]:
        r.on_packet(H(p), 0.0)
    got = [e for e in r.events if isinstance(e, Delivered)]
    assert len(got) == 1 and got[0].msg_id == 20 and got[0].data == payload_rule(v["msgLength"])
    # The vector's MSG_RESEND has seq 7 although the sender's last packet was seq 5 (see NOTES.md), so the
    # receiver sees a second gap and sends one more RESUME before its ACK. Both are legal reactions.
    out = [c.decode(p) for p in drain(r)]
    assert isinstance(out[-1], c.Ack) and out[-1].status == 0 and out[-1].msg_id == 20


def test_resume_sender(vectors):
    v = vectors["resume"]
    s = ready_sender(20)
    s.next_msg_id = 20
    s.send(payload_rule(v["msgLength"]))
    sent = drain(s)
    assert [p.hex() for p in sent] == v["originalPacketsHex"]
    s.on_packet(H(v["resumeHex"]), 0.0)
    resent = drain(s)
    want = [H(p) for p in v["senderResendPacketsHex"]]
    # Payload/offset bytes identical; seq continues from our own counter (6, 7, 8 - the vector uses 7, 8, 9).
    assert [p[2:] for p in resent] == [p[2:] for p in want]
    assert [p[0] for p in resent] == [p[0] for p in want]
    assert [p[1] for p in resent] == [6, 7, 8]
