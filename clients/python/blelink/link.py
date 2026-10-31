"""asyncio driver: runs a ``Session`` over a ``Transport`` (bleak for hardware, ``loopback`` for tests).

    transport = await BleakTransport.connect(name="BLE-TestServer")
    link = Link(transport, Session("client"))
    ready = await link.open()            # notifications on, HELLO, wait for the server HELLO
    await link.send(b"hello")            # resolves when the server ACKed (raises SendError otherwise)
    msg = await link.receive()           # next complete, CRC-verified server message
    await link.close()

Flow control (PROTOCOL.md 6.2): the loop calls ``transport.wait_writable()`` before every packet and only then
asks the session for it, so a packet is never produced while the OS queue is full.
"""
from __future__ import annotations

import asyncio
import time
from typing import Callable, Optional, Protocol

from . import codec as c
from .session import Delivered, Failed, OutMessage, Progressed, Ready, Session


class SendError(Exception):
    def __init__(self, message: OutMessage):
        super().__init__(f"message {message.msg_id} failed: {message.reason}")
        self.message = message
        self.reason = message.reason


class LinkClosed(Exception):
    pass


class Transport(Protocol):
    """What the driver needs from a link. One call to ``write`` = one GATT write = one packet."""

    os_limit: int            # ATT_MTU - 3 for write without response
    mtu: Optional[int]       # negotiated ATT MTU if the stack reports it (informational)

    async def start(self, receiver: Callable[[bytes], None], on_disconnect: Callable[[], None]) -> None: ...
    async def wait_writable(self) -> None: ...
    async def write(self, data: bytes, with_response: bool) -> None: ...
    async def close(self) -> None: ...


class Link:
    def __init__(self, transport: Transport, session: Session, *, clock: Callable[[], float] = time.monotonic,
                 tick: float = 0.05, write_with_response: bool = False,
                 on_progress: Optional[Callable[[Progressed], None]] = None):
        self.transport = transport
        self.session = session
        self.clock = clock
        self.tick = tick
        self.write_with_response = write_with_response   # force Write Request for every packet (debug/fallback)
        self.on_progress = on_progress
        self.ready_info: Optional[Ready] = None
        self.received: asyncio.Queue[Delivered] = asyncio.Queue()
        self._ready: Optional[asyncio.Future] = None
        self._wake = asyncio.Event()
        self._task: Optional[asyncio.Task] = None
        self._closed = False
        self._disconnected = False

    # --- public API --------------------------------------------------------------------------------------

    async def open(self, timeout: Optional[float] = None) -> Ready:
        """Enable notifications, send HELLO, wait for the peer's HELLO. Returns the negotiated values."""
        loop = asyncio.get_running_loop()
        self._ready = loop.create_future()
        self.session.set_os_limit(self.transport.os_limit)
        await self.transport.start(self._on_packet, self._on_disconnect)
        self.session.start(self.clock())
        self._task = asyncio.create_task(self._run())
        if timeout is None:
            # HELLO timeout: first try + 3 resends, T each (section 10), plus margin.
            timeout = self.session.timeout_s * 4 + 2
        try:
            return await asyncio.wait_for(asyncio.shield(self._ready), timeout)
        except asyncio.TimeoutError:
            raise LinkClosed("no HELLO from the server") from None

    def submit(self, data: bytes) -> "asyncio.Future[OutMessage]":
        """Queue a message; the future resolves with the finished OutMessage (check ``.ok``)."""
        fut: asyncio.Future = asyncio.get_running_loop().create_future()

        def done(m: OutMessage) -> None:
            if not fut.done():
                fut.set_result(m)

        if self._disconnected:
            raise LinkClosed("disconnected")
        self.session.send(data, on_done=done)
        self._wake.set()
        return fut

    async def send(self, data: bytes) -> OutMessage:
        """Send one message and wait for its ACK. Raises SendError on any failure."""
        m = await self.submit(data)
        if not m.ok:
            raise SendError(m)
        return m

    async def receive(self, timeout: Optional[float] = None) -> Delivered:
        return await asyncio.wait_for(self.received.get(), timeout)

    async def reset(self) -> Ready:
        """Re-send HELLO (section 3); in-flight messages fail with ``reset``."""
        self._ready = asyncio.get_running_loop().create_future()
        self.session.reset(self.clock())
        self._dispatch()
        self._wake.set()
        return await asyncio.wait_for(asyncio.shield(self._ready), self.session.timeout_s * 4 + 2)

    async def close(self) -> None:
        self._closed = True
        self._wake.set()
        if self._task:
            self._task.cancel()
            try:
                await self._task
            except (asyncio.CancelledError, Exception):
                pass
        await self.transport.close()
        if not self._disconnected:
            self._on_disconnect()

    # --- internals ---------------------------------------------------------------------------------------

    def _on_packet(self, raw: bytes) -> None:
        self.session.on_packet(raw, self.clock())
        self._dispatch()
        self._wake.set()

    def _on_disconnect(self) -> None:
        if self._disconnected:
            return
        self._disconnected = True
        self.session.on_disconnect()
        self._dispatch()
        if self._ready and not self._ready.done():
            self._ready.set_exception(LinkClosed("disconnected"))
        self._wake.set()

    def _dispatch(self) -> None:
        events, self.session.events = self.session.events, []
        for ev in events:
            if isinstance(ev, Ready):
                self.ready_info = ev
                if self._ready and not self._ready.done():
                    self._ready.set_result(ev)
            elif isinstance(ev, Delivered):
                self.received.put_nowait(ev)
            elif isinstance(ev, Progressed):
                if self.on_progress:
                    self.on_progress(ev)
            elif isinstance(ev, Failed):
                if self._ready and not self._ready.done():
                    self._ready.set_exception(LinkClosed(ev.reason))

    async def _run(self) -> None:
        try:
            while not self._closed and not self._disconnected:
                self.session.on_timer(self.clock())
                self._dispatch()
                burst = 0
                while not self._closed and not self._disconnected:
                    await self.transport.wait_writable()
                    raw = self.session.poll_transmit(self.clock())
                    if raw is None:
                        break
                    with_response = self.write_with_response or raw[0] == c.HELLO   # section 1: HELLO with response
                    await self.transport.write(raw, with_response)
                    self._dispatch()
                    burst += 1
                    if burst % 8 == 0:
                        await asyncio.sleep(0)   # let notification callbacks run between writes
                        self.session.on_timer(self.clock())
                self._wake.clear()
                try:
                    await asyncio.wait_for(self._wake.wait(), self.tick)
                except asyncio.TimeoutError:
                    pass
        except asyncio.CancelledError:
            raise
        except Exception as exc:  # write failed: the link is gone
            self.session.log(f"transport error: {exc!r}")
            self._on_disconnect()


def throughput(nbytes: int, seconds: float) -> str:
    if seconds <= 0:
        return "n/a"
    return f"{nbytes / seconds / 1024:.1f} KiB/s"
