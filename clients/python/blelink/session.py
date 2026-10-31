"""BleLink v1 session: a sans-IO state machine for one side of one connection (docs/PROTOCOL.md 3-11, 16).

The session does no I/O and reads no clock. The caller (``blelink.link`` for asyncio + bleak, or a test)
drives it:

* ``start(now)``               client: queue HELLO (section 3, 14.1)
* ``on_packet(raw, now)``      every received write/notification, one packet each
* ``poll_transmit(now)``       next packet to write, or None. Call it only when the OS can take a packet
                               (flow control, section 6.2); the session stamps its timers at that moment.
* ``on_timer(now)``            ACK, idle and HELLO timeouts (section 10); call it periodically
* ``send(data)``               queue a message, returns an ``OutMessage`` whose ``on_done`` fires once
* ``on_disconnect()``          the link is gone (section 11)
* ``events``                   list of ``Delivered``/``Ready``/``Progressed``/``Failed`` the caller drains

Both roles use the same class. ``role="client"`` sends the first HELLO; ``role="server"`` answers it. The
server role exists so tests can run a full client<->server conversation without hardware; the real server is the
C++ implementation.

Written from the spec only.
"""
from __future__ import annotations

from collections import OrderedDict, deque
from dataclasses import dataclass, field
from typing import Callable, Optional

from . import codec as c

# Failure reasons (section 18).
DISCONNECTED = "disconnected"
RESET = "reset"
TIMEOUT = "timeout"
CRC = "crc"
TOO_LARGE = "too_large"
BUSY = "busy"
REJECTED = "rejected"
LINK_UNRELIABLE = "link_unreliable"
CANCELLED = "cancelled"

MAX_TIMEOUT_RESENDS = 3   # section 10: ACK timeout -> at most 3 resends
MAX_CRC_RESENDS = 3       # section 6.3: status 1 -> at most 3 resends
MAX_RESTARTS = 8          # section 8.2: at most 8 RESUME restarts per message
MAX_HELLO_RESENDS = 3     # section 10
MAX_IDLE_RESUMES = 3      # section 10: receiver gives up after 3 RESUMEs with no progress
PROGRESS_STEP = 65536     # section 16

CLOSED, OPENING, READY = "CLOSED", "OPENING", "READY"


# --- events ----------------------------------------------------------------------------------------------

@dataclass
class Ready:
    """Handshake finished (or redone after a reset). ``peer`` is the peer's HELLO."""
    peer: c.Hello
    tx_packet_size: int
    timeout_s: float


@dataclass
class Delivered:
    """A complete, CRC-verified message from the peer (completion order, section 6.4)."""
    msg_id: int
    data: bytes
    at: float = 0.0


@dataclass
class Progressed:
    msg_id: int
    bytes_received: int


@dataclass
class Failed:
    """Session-level failure: HELLO never answered (the caller should disconnect)."""
    reason: str


@dataclass
class Stats:
    packets_tx: int = 0
    packets_rx: int = 0
    bytes_tx: int = 0
    bytes_rx: int = 0
    seq_gaps: int = 0
    resumes_tx: int = 0
    resumes_rx: int = 0
    restarts: int = 0
    timeout_resends: int = 0
    crc_resends: int = 0
    duplicates: int = 0
    ignored: int = 0


# --- outgoing / incoming message state -------------------------------------------------------------------

QUEUED, SENDING, WAIT_ACK, DONE, FAILED = "QUEUED", "SENDING", "WAIT_ACK", "DONE", "FAILED"


@dataclass(eq=False)
class OutMessage:
    data: bytes
    msg_id: int = -1
    crc: int = 0
    state: str = QUEUED
    offset: int = 0                 # next byte to send
    need_start: bool = True         # next packet is MSG_START
    need_resend: bool = False       # next packet is MSG_RESEND(offset)
    last_tx: float = 0.0
    timeout_resends: int = 0
    crc_resends: int = 0
    restarts: int = 0
    restart_offset: Optional[int] = None
    sent_since_restart: bool = True
    fully_sent: bool = False        # every byte went out at least once (an ACK OK is then legitimate)
    reason: Optional[str] = None    # failure reason when state == FAILED
    status: Optional[int] = None    # ACK status that ended it
    on_done: Optional[Callable[["OutMessage"], None]] = None
    started_at: Optional[float] = None
    finished_at: Optional[float] = None

    @property
    def ok(self) -> bool:
        return self.state == DONE

    @property
    def finished(self) -> bool:
        return self.state in (DONE, FAILED)


@dataclass(eq=False)
class _InMessage:
    msg_id: int
    total: int
    crc: int
    buf: bytearray = field(default_factory=bytearray)
    running_crc: int = 0
    resync: bool = False
    resend_mismatch_sent: bool = False
    idle_resumes: int = 0
    last_activity: float = 0.0

    @property
    def received(self) -> int:
        return len(self.buf)


class Session:
    def __init__(self, role: str = "client", *, max_packet: int = c.MAX_PACKET, max_message: int = 16 * 1024 * 1024,
                 window: int = 4, flags: int = c.FLAG_PROGRESS, timeout_units: int = 0, os_limit: int = c.MIN_PACKET,
                 log: Optional[Callable[[str], None]] = None):
        if role not in ("client", "server"):
            raise ValueError("role must be 'client' or 'server'")
        if not 1 <= window <= 255:
            raise ValueError("window must be 1..255")
        self.role = role
        self.local = c.Hello(0, c.PROTOCOL_VERSION, c.clamp_packet(max_packet), max_message, window, flags & 0x03,
                             timeout_units)
        self.os_limit = os_limit
        self.log = log or (lambda _msg: None)
        self.events: list = []
        self.stats = Stats()
        self.next_msg_id = 0       # section 7; tests may set it to reproduce vectors
        self._now = 0.0            # time of the last call that passed ``now``
        self._reset_state()
        self.state = CLOSED

    # --- derived values ----------------------------------------------------------------------------------

    @property
    def tx_packet_size(self) -> int:
        """packetSize for packets we send (section 5); 20 until the peer's HELLO arrived."""
        if self.peer is None:
            return c.MIN_PACKET
        return c.clamp_packet(min(self.peer.max_packet, self.os_limit))

    @property
    def timeout_s(self) -> float:
        """T = max(timeout_A, timeout_B) (section 4.2); our own value before the handshake."""
        own = c.timeout_seconds(self.local.timeout)
        return max(own, self.peer.timeout_s) if self.peer else own

    @property
    def peer_window(self) -> int:
        return max(1, self.peer.window) if self.peer else 1

    def in_flight(self) -> list[OutMessage]:
        return [m for m in self._out if m.state in (SENDING, WAIT_ACK)]

    def pending(self) -> int:
        """Messages queued or in flight."""
        return len(self._out)

    def set_os_limit(self, os_limit: int) -> None:
        """OS maximum payload per write (ATT_MTU - 3). May change after an MTU exchange (section 5)."""
        self.os_limit = os_limit

    # --- lifecycle ---------------------------------------------------------------------------------------

    def _reset_state(self) -> None:
        self.peer: Optional[c.Hello] = None
        self._tx_seq = 0
        self._rx_expected: Optional[int] = None
        self._control: deque = deque()          # packets that go before data (HELLO, ACK, RESUME, CANCEL, ...)
        self._out: list[OutMessage] = []        # queued + in flight, submission order
        self._rr = 0                            # round-robin cursor
        self._in: dict[int, _InMessage] = {}
        self._done: OrderedDict[int, int] = OrderedDict()   # completed msgId -> status (duplicate memory)
        self._ignored: OrderedDict[int, None] = OrderedDict()  # ids whose packets we drop silently
        self._resumed_unknown: dict[int, float] = {}        # id -> time of the RESUME(id, 0) we sent
        self._hello_pending = False
        self._hello_sent_at = 0.0
        self._hello_resends = 0

    def start(self, now: float) -> None:
        """Client: begin the handshake (call after TX notifications are enabled)."""
        if self.role != "client":
            raise RuntimeError("only the client sends the first HELLO")
        self.state = OPENING
        self._queue_hello(now)

    def reset(self, now: float) -> None:
        """Re-send HELLO while READY (for example after an MTU change): resets both sides (section 3)."""
        self._now = now
        self._fail_in_flight(RESET)
        self._discard_incoming()
        self.peer = None
        self.state = OPENING
        self._queue_hello(now)

    def on_disconnect(self) -> None:
        """Disconnect: every queued or in-flight message fails with ``disconnected`` (section 11)."""
        for m in list(self._out):
            self._finish(m, FAILED, DISCONNECTED)
        self._reset_state()
        self.next_msg_id = 0       # a new connection is a new session (section 11)
        self.state = CLOSED

    def _queue_hello(self, now: float) -> None:
        self._control.clear()
        self._control.append(c.Hello(0, *self._hello_fields()))
        self._hello_pending = True
        self._hello_sent_at = now
        self._hello_resends = 0

    def _hello_fields(self):
        h = self.local
        return h.version, h.max_packet, h.max_message, h.window, h.flags, h.timeout

    # --- application API ---------------------------------------------------------------------------------

    def send(self, data: bytes, on_done: Optional[Callable[[OutMessage], None]] = None) -> OutMessage:
        """Queue a message. It starts as soon as the session is READY and the peer's window has room."""
        m = OutMessage(bytes(data), on_done=on_done)
        m.crc = c.crc32(m.data)
        if len(m.data) > 0xFFFFFFFF:
            self._finish(m, FAILED, TOO_LARGE)
            return m
        self._out.append(m)
        return m

    def cancel(self, m: OutMessage) -> None:
        """Application cancel: sends CANCEL(reason 0) if the message had started."""
        if m.finished:
            return
        if m.state in (SENDING, WAIT_ACK):
            self._control.append(c.Cancel(0, m.msg_id, c.CANCEL_APP))
        self._finish(m, FAILED, CANCELLED)

    # --- transmit ----------------------------------------------------------------------------------------

    def poll_transmit(self, now: float) -> Optional[bytes]:
        """Return the next packet to write, or None if there is nothing to send right now."""
        self._now = now
        pkt = self._next_packet(now)
        if pkt is None:
            return None
        raw = c.encode(pkt)
        self.stats.packets_tx += 1
        self.stats.bytes_tx += len(raw)
        if isinstance(pkt, c.Resume):
            self.stats.resumes_tx += 1
        self.log("tx " + c.describe(pkt))
        return raw

    def _assign_seq(self, pkt):
        if isinstance(pkt, c.Hello):
            pkt.seq = 0
            self._tx_seq = 1
        else:
            pkt.seq = self._tx_seq
            self._tx_seq = (self._tx_seq + 1) & 0xFF
        return pkt

    def _next_packet(self, now: float):
        if self._control:
            pkt = self._control.popleft()
            if isinstance(pkt, c.Hello):
                self._hello_sent_at = now
            return self._assign_seq(pkt)
        if self.state != READY:
            return None
        self._admit(now)
        sending = [m for m in self._out if m.state == SENDING]
        if not sending:
            return None
        m = sending[self._rr % len(sending)]
        self._rr = (self._rr + 1) % max(1, len(sending))
        return self._assign_seq(self._data_packet(m, now))

    def _admit(self, now: float) -> None:
        """Start queued messages while the peer's window has room (section 6.3), in submission order."""
        active = len(self.in_flight())
        for m in self._out:
            if active >= self.peer_window:
                break
            if m.state != QUEUED:
                continue
            if self.peer and self.peer.max_message != c.NO_MESSAGE_LIMIT and len(m.data) > self.peer.max_message:
                self._finish(m, FAILED, TOO_LARGE)   # section 4.2: MUST NOT start it
                continue
            m.msg_id = self.next_msg_id
            self.next_msg_id = (self.next_msg_id + 1) & 0xFFFF
            m.state = SENDING
            m.started_at = now
            active += 1

    def _data_packet(self, m: OutMessage, now: float):
        p = self.tx_packet_size
        total = len(m.data)
        if m.need_start:
            n = min(p - c.START_HEADER, total)
            pkt = c.MsgStart(0, m.msg_id, total, m.crc, m.data[:n])
            m.offset = n
            m.need_start = False
            m.need_resend = False
        elif m.need_resend:
            n = min(p - c.RESEND_HEADER, total - m.offset)
            pkt = c.MsgResend(0, m.msg_id, m.offset, m.data[m.offset:m.offset + n])
            m.offset += n
            m.need_resend = False
        else:
            n = min(p - c.DATA_HEADER, total - m.offset)
            pkt = c.MsgData(0, m.msg_id, m.data[m.offset:m.offset + n])
            m.offset += n
        m.last_tx = now
        m.sent_since_restart = True
        if m.offset >= total:
            m.state = WAIT_ACK
            m.fully_sent = True
        return pkt

    # --- receive -----------------------------------------------------------------------------------------

    def on_packet(self, raw: bytes, now: float) -> None:
        self._now = now
        pkt = c.decode(raw)
        self.stats.packets_rx += 1
        self.stats.bytes_rx += len(raw)
        self.log("rx " + c.describe(pkt))
        if pkt.seq is None:
            self.stats.ignored += 1
            return
        if isinstance(pkt, c.Hello):
            self._on_hello(pkt, now)
            return
        gap = self._count_seq(pkt.seq)
        if self.state != READY:
            self.stats.ignored += 1        # section 3: anything but HELLO before READY is ignored
            return
        if gap:
            self._on_gap(gap, now)
        handler = {
            c.MsgStart: self._on_start, c.MsgData: self._on_data, c.MsgResend: self._on_resend,
            c.Ack: self._on_ack, c.Resume: self._on_resume, c.Cancel: self._on_cancel,
            c.Progress: self._on_progress,
        }.get(type(pkt))
        if handler is None:
            self.stats.ignored += 1        # unknown type / malformed: ignored, seq counted (section 2.3)
            return
        handler(pkt, now)

    def _count_seq(self, seq: int) -> int:
        if self._rx_expected is None:
            self._rx_expected = (seq + 1) & 0xFF
            return 0
        gap = (seq - self._rx_expected) & 0xFF
        self._rx_expected = (seq + 1) & 0xFF
        return gap

    def _on_hello(self, h: c.Hello, now: float) -> None:
        if h.version < 1:
            self.log("peer HELLO with version 0 ignored")
            return
        self._rx_expected = 1
        was_ready = self.state == READY
        if self._hello_pending:
            # The answer to our HELLO (first handshake, or a reset we initiated).
            self._hello_pending = False
        elif self.role == "server" or was_ready:
            # Peer-initiated (re)start: reset and answer once (section 3).
            if was_ready:
                self._fail_in_flight(RESET)
                self._discard_incoming()
            self._control.clear()
            self._control.append(c.Hello(0, *self._hello_fields()))
        else:
            self.log("unexpected HELLO ignored")
            return
        self.peer = h
        self.state = READY
        self.events.append(Ready(h, self.tx_packet_size, self.timeout_s))

    def _on_gap(self, gap: int, now: float) -> None:
        """Section 8.1: RESUME every incomplete message and put it into resync."""
        self.stats.seq_gaps += 1
        self.log(f"seq gap: {gap} packet(s) lost")
        for m in self._in.values():
            self._control.append(c.Resume(0, m.msg_id, m.received))
            m.resync = True
            m.resend_mismatch_sent = False
            m.last_activity = now

    def _remember_done(self, msg_id: int, status: int) -> None:
        self._done[msg_id] = status
        self._done.move_to_end(msg_id)
        while len(self._done) > self.local.window:
            self._done.popitem(last=False)

    def _ignore(self, msg_id: int) -> None:
        self._ignored[msg_id] = None
        self._ignored.move_to_end(msg_id)
        while len(self._ignored) > 64:
            self._ignored.popitem(last=False)

    def _on_start(self, p: c.MsgStart, now: float) -> None:
        mid = p.msg_id
        self._resumed_unknown.pop(mid, None)
        if mid in self._done and mid not in self._in:
            # Retry after a lost ACK: re-ACK with the stored status, do not deliver twice (section 6.3).
            self.stats.duplicates += 1
            self._control.append(c.Ack(0, mid, self._done[mid]))
            self._ignore(mid)
            return
        self._ignored.pop(mid, None)
        if mid in self._in:
            del self._in[mid]                # MSG_START for an active id restarts it (section 8.2)
        elif p.total_len > self.local.max_message and self.local.max_message != c.NO_MESSAGE_LIMIT:
            self._early_ack(mid, c.STATUS_TOO_LARGE)
            return
        elif len(self._in) >= self.local.window:
            self._early_ack(mid, c.STATUS_BUSY)
            return
        m = _InMessage(mid, p.total_len, p.crc, last_activity=now)
        self._in[mid] = m
        self._append(m, p.payload, now)

    def _early_ack(self, mid: int, status: int) -> None:
        self._control.append(c.Ack(0, mid, status))
        self._ignore(mid)

    def _on_data(self, p: c.MsgData, now: float) -> None:
        m = self._in.get(p.msg_id)
        if m is None:
            self._unknown_data(p.msg_id, now)
            return
        if m.resync:
            return                            # stale post-gap data, wait for MSG_RESEND (section 8.1)
        self._append(m, p.payload, now)

    def _on_resend(self, p: c.MsgResend, now: float) -> None:
        m = self._in.get(p.msg_id)
        if m is None:
            self._unknown_data(p.msg_id, now)
            return
        if p.offset != m.received:
            if not m.resend_mismatch_sent:
                self._control.append(c.Resume(0, m.msg_id, m.received))
                m.resend_mismatch_sent = True
            m.resync = True
            return
        m.resync = False
        m.resend_mismatch_sent = False
        self._append(m, p.payload, now)

    def _unknown_data(self, mid: int, now: float) -> None:
        if mid in self._ignored or mid in self._done:
            return
        sent = self._resumed_unknown.get(mid)
        if sent is not None and now - sent < self.timeout_s:
            return
        self._resumed_unknown[mid] = now      # MSG_START was lost: ask once for a restart (section 8.1)
        self._control.append(c.Resume(0, mid, 0))

    def _append(self, m: _InMessage, payload: bytes, now: float) -> None:
        if m.received + len(payload) > m.total:
            self._complete(m, c.STATUS_CRC)   # corrupt sender (section 6.3)
            return
        before = m.received
        m.buf += payload
        m.running_crc = c.crc32(payload, m.running_crc)
        m.last_activity = now
        if payload:
            m.idle_resumes = 0
        if (self.peer and self.peer.flags & c.FLAG_PROGRESS and m.total >= PROGRESS_STEP
                and m.received < m.total and m.received // PROGRESS_STEP > before // PROGRESS_STEP):
            self._control.append(c.Progress(0, m.msg_id, m.received))
        if m.received == m.total:
            self._complete(m, c.STATUS_OK if m.running_crc == m.crc else c.STATUS_CRC)

    def _complete(self, m: _InMessage, status: int) -> None:
        del self._in[m.msg_id]
        self._control.append(c.Ack(0, m.msg_id, status))
        if status == c.STATUS_OK:
            self._remember_done(m.msg_id, status)
            self.events.append(Delivered(m.msg_id, bytes(m.buf), self._now))
        else:
            # Not delivered, so it cannot be delivered twice: drop its stale packets, but let a MSG_START with
            # the same id restart it (a sender retrying the CRC failure with the same msgId).
            self._ignore(m.msg_id)

    def _on_cancel(self, p: c.Cancel, now: float) -> None:
        if self._in.pop(p.msg_id, None) is not None:
            self.log(f"peer cancelled message {p.msg_id} (reason {p.reason})")
            self._ignore(p.msg_id)

    # --- sender-side reactions ---------------------------------------------------------------------------

    def _find_out(self, mid: int) -> Optional[OutMessage]:
        for m in self._out:
            if m.msg_id == mid and m.state in (SENDING, WAIT_ACK):
                return m
        return None

    def _on_ack(self, p: c.Ack, now: float) -> None:
        m = self._find_out(p.msg_id)
        if m is None:
            return
        m.status = p.status
        if p.status == c.STATUS_OK:
            if m.fully_sent:
                # WAIT_ACK, or SENDING again after an ACK-timeout resend: the receiver answered the repeated
                # MSG_START with its remembered ACK (section 8.3). Stop sending the rest.
                self._finish(m, DONE)
            else:
                # ACK OK before we ever sent the last byte: the receiver has a stale completion for this id.
                self._finish(m, FAILED, REJECTED)
            return
        if p.status == c.STATUS_CRC and m.fully_sent and m.crc_resends < MAX_CRC_RESENDS:
            m.crc_resends += 1
            m.fully_sent = False
            self.stats.crc_resends += 1
            # New msgId for the CRC retry: a receiver that keeps status 1 in its duplicate memory (6.3) would
            # otherwise answer the retry with the stored ACK 1 forever. See clients/python/NOTES.md.
            m.msg_id = self.next_msg_id
            self.next_msg_id = (self.next_msg_id + 1) & 0xFFFF
            m.restarts = 0
            m.timeout_resends = 0
            self._restart(m, 0)
            return
        reason = {c.STATUS_CRC: CRC, c.STATUS_TOO_LARGE: TOO_LARGE, c.STATUS_BUSY: BUSY}.get(p.status, REJECTED)
        self._finish(m, FAILED, reason)

    def _on_resume(self, p: c.Resume, now: float) -> None:
        self.stats.resumes_rx += 1
        m = self._find_out(p.msg_id)
        if m is None:
            return                             # unknown, finished or aborted: ignore (section 8.2)
        total = len(m.data)
        if p.offset > total or (total > 0 and p.offset == total):
            return                             # nothing to resend from there
        if (m.restart_offset is not None and not m.sent_since_restart and p.offset <= m.restart_offset):
            return                             # duplicate RESUME from the same gap (section 8.2)
        m.restarts += 1
        self.stats.restarts += 1
        if m.restarts > MAX_RESTARTS:
            self._control.append(c.Cancel(0, m.msg_id, c.CANCEL_GAVE_UP))
            self._finish(m, FAILED, LINK_UNRELIABLE)
            return
        self._restart(m, p.offset)

    def _restart(self, m: OutMessage, offset: int) -> None:
        m.state = SENDING
        m.offset = offset
        m.need_start = offset == 0
        m.need_resend = offset > 0
        m.restart_offset = offset
        m.sent_since_restart = False

    def _on_progress(self, p: c.Progress, now: float) -> None:
        if self._find_out(p.msg_id) is not None:
            self.events.append(Progressed(p.msg_id, p.bytes_received))

    # --- timers ------------------------------------------------------------------------------------------

    def on_timer(self, now: float) -> None:
        self._now = now
        t = self.timeout_s
        if self._hello_pending and not self._control_has_hello() and now - self._hello_sent_at >= t:
            if self._hello_resends >= MAX_HELLO_RESENDS:
                self._hello_pending = False
                self.state = CLOSED
                self.events.append(Failed("hello_timeout"))
                for m in list(self._out):
                    self._finish(m, FAILED, TIMEOUT)
                return
            self._hello_resends += 1
            self._hello_sent_at = now
            self._control.append(c.Hello(0, *self._hello_fields()))
        if self.state != READY:
            return
        for m in list(self._out):
            if m.state == WAIT_ACK and now - m.last_tx >= t:
                if m.timeout_resends >= MAX_TIMEOUT_RESENDS:
                    self._control.append(c.Cancel(0, m.msg_id, c.CANCEL_GAVE_UP))
                    self._finish(m, FAILED, TIMEOUT)
                    continue
                m.timeout_resends += 1
                self.stats.timeout_resends += 1
                self.log(f"ACK timeout for message {m.msg_id}: resend {m.timeout_resends}/{MAX_TIMEOUT_RESENDS}")
                self._restart(m, 0)
        for m in list(self._in.values()):
            if now - m.last_activity >= t:
                if m.idle_resumes >= MAX_IDLE_RESUMES:
                    self.log(f"message {m.msg_id}: no progress after {MAX_IDLE_RESUMES} RESUMEs, discarded")
                    del self._in[m.msg_id]
                    continue
                m.idle_resumes += 1
                m.resync = True
                m.resend_mismatch_sent = False
                m.last_activity = now
                self._control.append(c.Resume(0, m.msg_id, m.received))

    def _control_has_hello(self) -> bool:
        return any(isinstance(p, c.Hello) for p in self._control)

    # --- helpers -----------------------------------------------------------------------------------------

    def _finish(self, m: OutMessage, state: str, reason: Optional[str] = None) -> None:
        if m.finished:
            return
        m.state = state
        m.reason = reason
        m.finished_at = self._now
        if m in self._out:
            self._out.remove(m)
        if m.on_done:
            m.on_done(m)

    def _fail_in_flight(self, reason: str) -> None:
        for m in self.in_flight():
            self._finish(m, FAILED, reason)

    def _discard_incoming(self) -> None:
        self._in.clear()
        self._done.clear()
        self._ignored.clear()
        self._resumed_unknown.clear()
