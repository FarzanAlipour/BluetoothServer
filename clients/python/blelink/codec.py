"""BleLink v1 packet codec (docs/PROTOCOL.md sections 2 and 4).

Pure functions and small dataclasses, no I/O. Written from the spec only; it does not import or mirror
the C++ server code. All integers are little-endian.

    >>> encode(Hello(seq=0, version=1, max_packet=244, max_message=16 * 1024 * 1024, window=4, flags=1,
    ...              timeout=40)).hex()
    '010001f40000000001040128'
"""
from __future__ import annotations

import struct
import zlib
from dataclasses import dataclass
from typing import Optional, Union

PROTOCOL_VERSION = 1

# --- GATT service (section 1) ----------------------------------------------------------------------------
SERVICE_UUID = "7b1e1000-5a6c-4f0e-9d3b-2c1a8e4f6b01"
RX_UUID = "7b1e1001-5a6c-4f0e-9d3b-2c1a8e4f6b01"    # client -> server, write / write without response
TX_UUID = "7b1e1002-5a6c-4f0e-9d3b-2c1a8e4f6b01"    # server -> client, notify
INFO_UUID = "7b1e1003-5a6c-4f0e-9d3b-2c1a8e4f6b01"  # read, 10-byte server limits

# --- Packet types (section 2.3) --------------------------------------------------------------------------
HELLO = 0x01
MSG_START = 0x02
MSG_DATA = 0x03
ACK = 0x04
RESUME = 0x05
CANCEL = 0x06
PROGRESS = 0x07
MSG_RESEND = 0x08

TYPE_NAMES = {HELLO: "HELLO", MSG_START: "MSG_START", MSG_DATA: "MSG_DATA", ACK: "ACK", RESUME: "RESUME",
              CANCEL: "CANCEL", PROGRESS: "PROGRESS", MSG_RESEND: "MSG_RESEND"}

# Header sizes of the data-carrying packets: payload capacity = packetSize - header.
START_HEADER = 12
DATA_HEADER = 4
RESEND_HEADER = 8

# --- Limits (sections 2.2, 4.1) --------------------------------------------------------------------------
MIN_PACKET = 20
MAX_PACKET = 512
NO_MESSAGE_LIMIT = 0xFFFFFFFF
DEFAULT_TIMEOUT_UNITS = 40  # 40 * 0.25 s = 10 s; a HELLO timeout field of 0 means this

FLAG_PROGRESS = 0x01   # wants / sends PROGRESS packets
FLAG_STREAMING = 0x02  # informational: receiver consumes data before the message is complete

# --- ACK status (section 6.3) ----------------------------------------------------------------------------
STATUS_OK = 0
STATUS_CRC = 1
STATUS_TOO_LARGE = 2
STATUS_BUSY = 3
STATUS_REJECTED = 4

# --- CANCEL reason (section 16) --------------------------------------------------------------------------
CANCEL_APP = 0
CANCEL_GAVE_UP = 1
CANCEL_SHUTDOWN = 2


def crc32(data: bytes, running: int = 0) -> int:
    """CRC-32/IEEE 802.3 (zlib). Pass the previous result as ``running`` to update it incrementally."""
    return zlib.crc32(data, running) & 0xFFFFFFFF


def clamp_packet(n: int) -> int:
    """Clamp a packet size into the legal range 20..512 (section 2.2)."""
    return max(MIN_PACKET, min(MAX_PACKET, n))


def timeout_seconds(units: int) -> float:
    """HELLO ``timeout`` field (0.25 s units, 0 = default 10 s) -> seconds."""
    return (units or DEFAULT_TIMEOUT_UNITS) * 0.25


# --- Packet dataclasses ----------------------------------------------------------------------------------
# ``seq`` is the per-direction counter. The session assigns it when the packet is transmitted; encode()
# writes whatever value the dataclass carries.

@dataclass
class Hello:
    seq: int = 0
    version: int = PROTOCOL_VERSION
    max_packet: int = MAX_PACKET
    max_message: int = 16 * 1024 * 1024
    window: int = 4
    flags: int = 0
    timeout: int = 0  # 0.25 s units, 0 = default

    @property
    def timeout_s(self) -> float:
        return timeout_seconds(self.timeout)


@dataclass
class MsgStart:
    seq: int
    msg_id: int
    total_len: int
    crc: int
    payload: bytes = b""


@dataclass
class MsgData:
    seq: int
    msg_id: int
    payload: bytes = b""


@dataclass
class Ack:
    seq: int
    msg_id: int
    status: int = STATUS_OK


@dataclass
class Resume:
    seq: int
    msg_id: int
    offset: int


@dataclass
class Cancel:
    seq: int
    msg_id: int
    reason: int = CANCEL_APP


@dataclass
class Progress:
    seq: int
    msg_id: int
    bytes_received: int


@dataclass
class MsgResend:
    seq: int
    msg_id: int
    offset: int
    payload: bytes = b""


@dataclass
class Unknown:
    """A packet of an unknown type (or too short for its layout). Ignored, but its seq still counts."""
    type: int
    seq: Optional[int]
    raw: bytes


Packet = Union[Hello, MsgStart, MsgData, Ack, Resume, Cancel, Progress, MsgResend, Unknown]

_HELLO = struct.Struct("<BBBHIBBB")  # 12
_START = struct.Struct("<BBHII")     # 12 + payload
_DATA = struct.Struct("<BBH")        # 4 + payload
_ACK = struct.Struct("<BBHB")        # 5 (CANCEL has the same layout)
_RESUME = struct.Struct("<BBHI")     # 8 (PROGRESS and the MSG_RESEND header have the same layout)


def encode(p: Packet) -> bytes:
    """Serialise one packet."""
    s = (p.seq or 0) & 0xFF
    if isinstance(p, Hello):
        return _HELLO.pack(HELLO, s, p.version, p.max_packet, p.max_message, p.window, p.flags, p.timeout)
    if isinstance(p, MsgStart):
        return _START.pack(MSG_START, s, p.msg_id, p.total_len, p.crc) + bytes(p.payload)
    if isinstance(p, MsgData):
        return _DATA.pack(MSG_DATA, s, p.msg_id) + bytes(p.payload)
    if isinstance(p, Ack):
        return _ACK.pack(ACK, s, p.msg_id, p.status)
    if isinstance(p, Resume):
        return _RESUME.pack(RESUME, s, p.msg_id, p.offset)
    if isinstance(p, Cancel):
        return _ACK.pack(CANCEL, s, p.msg_id, p.reason)
    if isinstance(p, Progress):
        return _RESUME.pack(PROGRESS, s, p.msg_id, p.bytes_received)
    if isinstance(p, MsgResend):
        return _RESUME.pack(MSG_RESEND, s, p.msg_id, p.offset) + bytes(p.payload)
    if isinstance(p, Unknown):
        return bytes(p.raw)
    raise TypeError(f"not a packet: {p!r}")


def decode(buf: bytes) -> Packet:
    """Parse one packet. Never raises on malformed input: returns ``Unknown`` instead (section 2.3).

    Trailing bytes after fixed-size packets are ignored (forward compatibility, D13).
    """
    b = bytes(buf)
    if len(b) < 2:
        return Unknown(b[0] if b else -1, None, b)
    t, seq = b[0], b[1]
    try:
        if t == HELLO:
            _, _, ver, mp, mm, win, flags, to = _HELLO.unpack_from(b)
            return Hello(seq, ver, mp, mm, win, flags, to)
        if t == MSG_START:
            _, _, mid, total, crc = _START.unpack_from(b)
            return MsgStart(seq, mid, total, crc, b[_START.size:])
        if t == MSG_DATA:
            _, _, mid = _DATA.unpack_from(b)
            return MsgData(seq, mid, b[_DATA.size:])
        if t == ACK:
            _, _, mid, st = _ACK.unpack_from(b)
            return Ack(seq, mid, st)
        if t == RESUME:
            _, _, mid, off = _RESUME.unpack_from(b)
            return Resume(seq, mid, off)
        if t == CANCEL:
            _, _, mid, reason = _ACK.unpack_from(b)
            return Cancel(seq, mid, reason)
        if t == PROGRESS:
            _, _, mid, n = _RESUME.unpack_from(b)
            return Progress(seq, mid, n)
        if t == MSG_RESEND:
            _, _, mid, off = _RESUME.unpack_from(b)
            return MsgResend(seq, mid, off, b[_RESUME.size:])
    except struct.error:
        pass  # shorter than its fixed layout -> ignored
    return Unknown(t, seq, b)


def encode_info(h: Hello) -> bytes:
    """INFO characteristic value: the HELLO body without type and seq (10 bytes)."""
    return encode(h)[2:]


def decode_info(buf: bytes) -> Optional[Hello]:
    """Parse the INFO characteristic value; None if it is too short."""
    if len(buf) < 10:
        return None
    p = decode(bytes([HELLO, 0]) + bytes(buf))
    return p if isinstance(p, Hello) else None


def split_message(msg: bytes, msg_id: int, packet_size: int, first_seq: int = 1) -> list[bytes]:
    """Frame a whole message as MSG_START + MSG_DATA... at ``packet_size`` (section 2.4).

    Convenience for tests and simple senders; the session uses the same capacities packet by packet.
    """
    if not MIN_PACKET <= packet_size <= MAX_PACKET:
        raise ValueError("packet_size must be in 20..512")
    out = []
    seq = first_seq
    first = msg[: packet_size - START_HEADER]
    out.append(encode(MsgStart(seq, msg_id, len(msg), crc32(msg), first)))
    off = len(first)
    while off < len(msg):
        seq = (seq + 1) & 0xFF
        chunk = msg[off: off + packet_size - DATA_HEADER]
        out.append(encode(MsgData(seq, msg_id, chunk)))
        off += len(chunk)
    return out


def describe(p: Packet) -> str:
    """Short human-readable form for logs."""
    if isinstance(p, Hello):
        return (f"HELLO seq={p.seq} v{p.version} maxPacket={p.max_packet} maxMessage={p.max_message} "
                f"window={p.window} flags=0x{p.flags:02x} timeout={p.timeout_s:g}s")
    if isinstance(p, MsgStart):
        return f"MSG_START seq={p.seq} id={p.msg_id} total={p.total_len} crc={p.crc:08x} n={len(p.payload)}"
    if isinstance(p, MsgData):
        return f"MSG_DATA seq={p.seq} id={p.msg_id} n={len(p.payload)}"
    if isinstance(p, MsgResend):
        return f"MSG_RESEND seq={p.seq} id={p.msg_id} off={p.offset} n={len(p.payload)}"
    if isinstance(p, Ack):
        return f"ACK seq={p.seq} id={p.msg_id} status={p.status}"
    if isinstance(p, Resume):
        return f"RESUME seq={p.seq} id={p.msg_id} off={p.offset}"
    if isinstance(p, Cancel):
        return f"CANCEL seq={p.seq} id={p.msg_id} reason={p.reason}"
    if isinstance(p, Progress):
        return f"PROGRESS seq={p.seq} id={p.msg_id} bytes={p.bytes_received}"
    return f"UNKNOWN type={p.type} seq={p.seq} len={len(p.raw)}"
