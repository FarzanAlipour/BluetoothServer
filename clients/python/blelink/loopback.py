"""In-memory transport + fake BleLink server for tests and for trying the CLI without hardware.

``LoopbackServer`` runs a server-role ``Session`` (same spec implementation, other role) and echoes every
delivered message back to the client when ``echo=True`` - the behaviour the CLI's ``echo-test`` expects from the
real test server. ``drop`` lets tests lose packets in either direction to exercise RESUME / timeouts.
"""
from __future__ import annotations

import asyncio
import time
from typing import Callable, Optional

from .session import Delivered, Session

DropFn = Callable[[str, int, bytes], bool]   # (direction "c2s"/"s2c", packet index, raw) -> drop?


class LoopbackServer:
    def __init__(self, *, os_limit: int = 244, max_packet: int = 512, max_message: int = 16 * 1024 * 1024,
                 window: int = 4, flags: int = 1, timeout_units: int = 0, echo: bool = True,
                 drop: Optional[DropFn] = None, clock: Callable[[], float] = time.monotonic, tick: float = 0.01):
        self.session = Session("server", max_packet=max_packet, max_message=max_message, window=window,
                               flags=flags, timeout_units=timeout_units, os_limit=os_limit)
        self.echo = echo
        self.drop = drop
        self.clock = clock
        self.tick = tick
        self.delivered: list[Delivered] = []
        self.client_rx: Optional[Callable[[bytes], None]] = None
        self.counts = {"c2s": 0, "s2c": 0}
        self._task: Optional[asyncio.Task] = None
        self._wake: Optional[asyncio.Event] = None
        self._stopped = False

    def from_client(self, raw: bytes) -> None:
        i = self.counts["c2s"]
        self.counts["c2s"] += 1
        if self.drop and self.drop("c2s", i, raw):
            return
        self.session.on_packet(raw, self.clock())
        self._handle_events()
        if self._wake:
            self._wake.set()

    def _handle_events(self) -> None:
        events, self.session.events = self.session.events, []
        for ev in events:
            if isinstance(ev, Delivered):
                self.delivered.append(ev)
                if self.echo:
                    self.session.send(ev.data)

    async def run(self) -> None:
        self._wake = asyncio.Event()
        # A stop flag, not only cancel(): on Python < 3.12 asyncio.wait_for can swallow a cancellation that
        # races with its inner wait completing, which would leave this loop running forever.
        while not self._stopped:
            self.session.on_timer(self.clock())
            self._handle_events()
            n = 0
            while (raw := self.session.poll_transmit(self.clock())) is not None:
                i = self.counts["s2c"]
                self.counts["s2c"] += 1
                if not (self.drop and self.drop("s2c", i, raw)) and self.client_rx:
                    self.client_rx(raw)
                n += 1
                if n % 16 == 0:
                    await asyncio.sleep(0)
            self._wake.clear()
            try:
                await asyncio.wait_for(self._wake.wait(), self.tick)
            except asyncio.TimeoutError:
                pass


class LoopbackTransport:
    """Client-side transport wired to a ``LoopbackServer``. ``os_limit`` plays ATT_MTU - 3."""

    def __init__(self, server: LoopbackServer, os_limit: int = 244):
        self.server = server
        self.os_limit = os_limit
        self.mtu: Optional[int] = os_limit + 3
        self.writes = 0
        self.writes_with_response = 0
        self.disconnect_cb: Optional[Callable[[], None]] = None
        self.connected = True

    async def start(self, receiver: Callable[[bytes], None], on_disconnect: Callable[[], None]) -> None:
        self.server.client_rx = receiver
        self.disconnect_cb = on_disconnect
        self.server._task = asyncio.create_task(self.server.run())
        await asyncio.sleep(0)

    async def wait_writable(self) -> None:
        await asyncio.sleep(0)

    async def write(self, data: bytes, with_response: bool) -> None:
        if not self.connected:
            raise ConnectionError("disconnected")
        if len(data) > self.os_limit and not with_response:
            raise ValueError(f"packet of {len(data)} bytes exceeds os_limit {self.os_limit}")
        self.writes += 1
        self.writes_with_response += int(with_response)
        self.server.from_client(bytes(data))

    def drop_link(self) -> None:
        """Simulate a link loss."""
        self.connected = False
        self.server.session.on_disconnect()
        if self.disconnect_cb:
            self.disconnect_cb()

    async def close(self) -> None:
        self.connected = False
        self.server._stopped = True
        if self.server._wake:
            self.server._wake.set()
        if self.server._task:
            self.server._task.cancel()
            try:
                await self.server._task
            except (asyncio.CancelledError, Exception):
                pass
