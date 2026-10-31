"""Shared helpers: vector loading and a two-session in-memory pump with a fake clock."""
from __future__ import annotations

import json
import sys
from pathlib import Path
from typing import Callable, Optional

import pytest

ROOT = Path(__file__).resolve().parents[3]          # repo root
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))   # clients/python, so `import blelink` works

from blelink import codec as c  # noqa: E402
from blelink.session import Delivered, Session  # noqa: E402

VECTORS_PATH = ROOT / "docs" / "protocol-test-vectors.json"


@pytest.fixture(scope="session")
def vectors() -> dict:
    return json.loads(VECTORS_PATH.read_text())


def payload_rule(n: int) -> bytes:
    """protocol-test-vectors.json payloadRule: byte[i] = (i * 7 + 3) mod 256."""
    return bytes((i * 7 + 3) & 0xFF for i in range(n))


class Pair:
    """A client and a server Session wired back to back. ``drop(direction, raw)`` may lose packets."""

    def __init__(self, client: Optional[Session] = None, server: Optional[Session] = None,
                 drop: Optional[Callable[[str, bytes], bool]] = None):
        self.client = client or Session("client", os_limit=244)
        self.server = server or Session("server", os_limit=244)
        self.drop = drop
        self.now = 0.0
        self.wire: list[tuple[str, bytes]] = []   # every packet transmitted, before drop

    def pump(self, max_packets: int = 1_000_000) -> int:
        """Move packets until both sides are idle. Returns how many were moved."""
        moved = 0
        while moved < max_packets:
            progressed = False
            for name, src, dst in (("c2s", self.client, self.server), ("s2c", self.server, self.client)):
                raw = src.poll_transmit(self.now)
                if raw is None:
                    continue
                progressed = True
                moved += 1
                self.wire.append((name, raw))
                if self.drop and self.drop(name, raw):
                    continue
                dst.on_packet(raw, self.now)
            if not progressed:
                break
        return moved

    def advance(self, seconds: float) -> None:
        self.now += seconds
        self.client.on_timer(self.now)
        self.server.on_timer(self.now)

    def handshake(self) -> None:
        self.client.start(self.now)
        self.pump()

    @staticmethod
    def delivered(s: Session) -> list[Delivered]:
        out = [e for e in s.events if isinstance(e, Delivered)]
        s.events = [e for e in s.events if not isinstance(e, Delivered)]
        return out


def ready_receiver(window: int = 4, max_message: int = 16 * 1024 * 1024, flags: int = c.FLAG_PROGRESS) -> Session:
    """A server-role session that has received a client HELLO and sent its own (its next tx seq is 1)."""
    s = Session("server", window=window, max_message=max_message, flags=flags, os_limit=244)
    s.on_packet(c.encode(c.Hello(0, 1, 512, 16 * 1024 * 1024, 4, 0, 0)), 0.0)
    assert s.poll_transmit(0.0)[0] == c.HELLO
    s.events.clear()
    return s


def ready_sender(packet_size: int, peer_window: int = 4) -> Session:
    """A client-role session that completed the handshake with a peer whose maxPacket = packet_size."""
    s = Session("client", os_limit=packet_size)
    s.start(0.0)
    assert s.poll_transmit(0.0)[0] == c.HELLO
    s.on_packet(c.encode(c.Hello(0, 1, packet_size, 16 * 1024 * 1024, peer_window, 0, 0)), 0.0)
    s.events.clear()
    return s


def drain(s: Session, now: float = 0.0) -> list[bytes]:
    out = []
    while (raw := s.poll_transmit(now)) is not None:
        out.append(raw)
    return out
