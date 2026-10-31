"""``blelink-client``: BleLink v1 test client (hardware test tool and example usage of the library).

    blelink-client --name BLE-TestServer send-text "hello"
    blelink-client --name BLE-TestServer send-blob 1M
    blelink-client --name BLE-TestServer echo-test 0 1 20 244 4k 64k 1M
    blelink-client --name BLE-TestServer interleave 256k 16
    blelink-client --name BLE-TestServer soak 500 --max-size 8k --verify-echo
    blelink-client --loopback echo-test 1 1M          # no hardware: in-memory fake server

Exit code: 0 = PASS, 1 = FAIL (a check failed), 2 = could not connect / handshake failed / bad usage.
The last line of output is always ``RESULT: PASS ...`` or ``RESULT: FAIL ...`` (scripts grep for it).

echo-test, interleave --verify-echo and soak --verify-echo need a server that sends every message it receives
back as a new message (the BleLink echo mode of ble_test_server).
"""
from __future__ import annotations

import argparse
import asyncio
import random
import sys
import time
from typing import Awaitable, Callable, Optional

from . import codec as c
from .link import Link, LinkClosed, SendError, throughput
from .session import Progressed, Session

EXIT_PASS, EXIT_FAIL, EXIT_ERROR = 0, 1, 2


def parse_size(text: str) -> int:
    """'512', '4k', '64K', '1M', '1.5m' -> bytes (k = 1024)."""
    t = text.strip().lower()
    mult = 1
    if t.endswith(("k", "m")):
        mult = 1024 if t[-1] == "k" else 1024 * 1024
        t = t[:-1]
    try:
        n = int(float(t) * mult)
    except ValueError:
        raise argparse.ArgumentTypeError(f"not a size: {text!r}") from None
    if n < 0 or n > 0xFFFFFFFF:
        raise argparse.ArgumentTypeError(f"size out of range: {text!r}")
    return n


def test_payload(n: int, seed: int = 0) -> bytes:
    """Deterministic payload: byte[i] = (i * 7 + 3 + seed) mod 256 (the vectors' payloadRule for seed 0)."""
    block = bytes(((i * 7 + 3 + seed) & 0xFF) for i in range(256))
    reps = n // 256 + 1
    return (block * reps)[:n]


class Reporter:
    def __init__(self, verbosity: int):
        self.verbosity = verbosity
        self.failures: list[str] = []

    def info(self, msg: str) -> None:
        print(msg, flush=True)

    def debug(self, msg: str) -> None:
        if self.verbosity >= 1:
            print("  " + msg, flush=True)

    def packet(self, msg: str) -> None:
        if self.verbosity >= 2:
            print("    " + msg, flush=True)

    def fail(self, msg: str) -> None:
        self.failures.append(msg)
        print("FAIL: " + msg, flush=True)


def op_timeout(args, size: int) -> float:
    """How long one message (and its echo) may take: generous, so slow links (20-byte packets) still pass."""
    if args.op_timeout:
        return args.op_timeout
    return 30.0 + size / 1000.0


# --- commands -------------------------------------------------------------------------------------------
# Each returns a one-line summary for the RESULT line; failures are recorded on the reporter.

async def cmd_info(link: Link, args, rep: Reporter) -> str:
    return "handshake ok"


async def cmd_send_text(link: Link, args, rep: Reporter) -> str:
    data = args.text.encode("utf-8")
    t0 = time.monotonic()
    try:
        m = await asyncio.wait_for(link.send(data), op_timeout(args, len(data)))
    except (SendError, asyncio.TimeoutError, LinkClosed) as exc:
        rep.fail(f"send-text: {describe_exc(exc)}")
        return "send-text"
    dt = time.monotonic() - t0
    rep.info(f"send-text: {len(data)} B, msgId {m.msg_id}, ACK OK after {dt * 1000:.1f} ms")
    return f"send-text {len(data)} B in {dt * 1000:.1f} ms"


async def cmd_send_blob(link: Link, args, rep: Reporter) -> str:
    data = test_payload(args.size)
    t0 = time.monotonic()
    try:
        m = await asyncio.wait_for(link.send(data), op_timeout(args, len(data)))
    except (SendError, asyncio.TimeoutError, LinkClosed) as exc:
        rep.fail(f"send-blob {args.size}: {describe_exc(exc)}")
        return f"send-blob {args.size} B"
    dt = time.monotonic() - t0
    rep.info(f"send-blob: {len(data)} B, msgId {m.msg_id}, crc {m.crc:08x}, ACK OK after {dt:.3f} s "
             f"= {throughput(len(data), dt)}")
    return f"send-blob {len(data)} B in {dt:.3f} s = {throughput(len(data), dt)}"


async def _echo_one(link: Link, data: bytes, args, rep: Reporter, label: str) -> Optional[float]:
    """Send ``data``, wait for its ACK and for the server's echo. Returns the round trip time or None."""
    t0 = time.monotonic()
    limit = op_timeout(args, len(data))
    try:
        await asyncio.wait_for(link.send(data), limit)
        t_ack = time.monotonic() - t0
        echo = await link.receive(timeout=max(1.0, limit - t_ack))
    except (SendError, asyncio.TimeoutError, LinkClosed) as exc:
        rep.fail(f"{label}: {describe_exc(exc, 'no echo')}")
        return None
    rtt = time.monotonic() - t0
    if echo.data != data:
        rep.fail(f"{label}: echo differs (sent {len(data)} B, got {len(echo.data)} B, "
                 f"first difference at {first_diff(data, echo.data)})")
        return None
    rep.info(f"{label}: ok, ACK {t_ack * 1000:.1f} ms, echo {rtt * 1000:.1f} ms, "
             f"{throughput(2 * len(data), rtt)} both ways")
    return rtt


async def cmd_echo_test(link: Link, args, rep: Reporter) -> str:
    total_bytes, total_time, passed = 0, 0.0, 0
    for i, size in enumerate(args.sizes):
        rtt = await _echo_one(link, test_payload(size, seed=i), args, rep, f"echo {size} B")
        if rtt is not None:
            passed += 1
            total_bytes += 2 * size
            total_time += rtt
    return (f"echo-test {passed}/{len(args.sizes)} sizes ok, {total_bytes} B moved in {total_time:.3f} s "
            f"= {throughput(total_bytes, total_time)}")


async def cmd_interleave(link: Link, args, rep: Reporter) -> str:
    """A large message with a small one started right after it (PROTOCOL.md 15.3)."""
    window = link.session.peer_window
    if window < 2:
        rep.info(f"interleave: server window is {window}; messages will be sent one after the other")
    big, small = test_payload(args.large, seed=1), test_payload(args.small, seed=2)
    order: list[str] = []
    t0 = time.monotonic()
    f_big, f_small = link.submit(big), link.submit(small)
    f_big.add_done_callback(lambda _f: order.append("large"))
    f_small.add_done_callback(lambda _f: order.append("small"))
    try:
        m_big, m_small = await asyncio.wait_for(asyncio.gather(f_big, f_small), op_timeout(args, args.large))
    except asyncio.TimeoutError:
        rep.fail("interleave: timed out waiting for the ACKs")
        return "interleave"
    dt = time.monotonic() - t0
    for name, m in (("large", m_big), ("small", m_small)):
        if not m.ok:
            rep.fail(f"interleave: {name} message failed: {m.reason}")
    rep.info(f"interleave: ACK order {order} (small first expected when window >= 2), {dt:.3f} s")
    if window >= 2 and order and order[0] != "small" and args.large > 4 * args.small:
        rep.info("interleave: note: the small message did not overtake the large one")
    if args.verify_echo and not rep.failures:
        got = []
        try:
            for _ in range(2):
                got.append((await link.receive(timeout=op_timeout(args, args.large))).data)
        except asyncio.TimeoutError:
            rep.fail(f"interleave: only {len(got)} of 2 echoes arrived")
        else:
            if sorted(got, key=len) != sorted([big, small], key=len):
                rep.fail("interleave: echoes differ from what was sent")
            else:
                rep.info(f"interleave: echo order {['small' if g == small else 'large' for g in got]}")
    return f"interleave {args.large}+{args.small} B in {dt:.3f} s = {throughput(args.large + args.small, dt)}"


async def cmd_soak(link: Link, args, rep: Reporter) -> str:
    """N messages of random size (seeded), all submitted at once: the session keeps the window full."""
    rnd = random.Random(args.seed)
    msgs = [test_payload(rnd.randint(0, args.max_size), seed=i) for i in range(args.count)]
    total = sum(len(m) for m in msgs)
    t0 = time.monotonic()
    futs = [link.submit(m) for m in msgs]
    limit = op_timeout(args, total)
    try:
        results = await asyncio.wait_for(asyncio.gather(*futs), limit)
    except asyncio.TimeoutError:
        done = sum(1 for f in futs if f.done() and f.result().ok)
        rep.fail(f"soak: timed out after {limit:.0f} s with {done}/{len(msgs)} messages ACKed")
        return "soak"
    dt = time.monotonic() - t0
    bad = [m for m in results if not m.ok]
    for m in bad[:10]:
        rep.fail(f"soak: message {m.msg_id} ({len(m.data)} B) failed: {m.reason}")
    if len(bad) > 10:
        rep.fail(f"soak: ... and {len(bad) - 10} more failures")
    rep.info(f"soak: {len(results) - len(bad)}/{len(results)} ACK OK, {total} B in {dt:.3f} s "
             f"= {throughput(total, dt)}")
    if args.verify_echo and not bad:
        want: dict[bytes, int] = {}
        for m in msgs:
            want[m] = want.get(m, 0) + 1
        got = 0
        try:
            while got < len(msgs):
                d = (await link.receive(timeout=op_timeout(args, args.max_size))).data
                if want.get(d, 0) <= 0:
                    rep.fail(f"soak: unexpected echo of {len(d)} B")
                    break
                want[d] -= 1
                got += 1
        except asyncio.TimeoutError:
            rep.fail(f"soak: only {got}/{len(msgs)} echoes arrived")
        else:
            if got == len(msgs):
                rep.info(f"soak: all {got} echoes matched")
    return f"soak {args.count} msgs, {total} B in {dt:.3f} s = {throughput(total, dt)}"


COMMANDS: dict[str, Callable[[Link, argparse.Namespace, Reporter], Awaitable[str]]] = {
    "info": cmd_info, "send-text": cmd_send_text, "send-blob": cmd_send_blob, "echo-test": cmd_echo_test,
    "interleave": cmd_interleave, "soak": cmd_soak,
}


# --- helpers --------------------------------------------------------------------------------------------

def describe_exc(exc: BaseException, timeout_text: str = "timed out") -> str:
    if isinstance(exc, asyncio.TimeoutError):
        return timeout_text
    if isinstance(exc, SendError):
        return f"failed: {exc.reason}"
    return f"{type(exc).__name__}: {exc}"


def first_diff(a: bytes, b: bytes) -> int:
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            return i
    return min(len(a), len(b))


def describe_hello(h: c.Hello) -> str:
    mm = "unlimited" if h.max_message == c.NO_MESSAGE_LIMIT else f"{h.max_message} B"
    flags = [n for bit, n in ((c.FLAG_PROGRESS, "progress"), (c.FLAG_STREAMING, "streaming")) if h.flags & bit]
    return (f"v{h.version} maxPacket {h.max_packet}, maxMessage {mm}, window {h.window}, "
            f"flags {'+'.join(flags) or 'none'}, timeout {h.timeout_s:g} s")


def stats_line(s) -> str:
    return (f"stats: packets tx {s.packets_tx} rx {s.packets_rx}, bytes tx {s.bytes_tx} rx {s.bytes_rx}, "
            f"seq gaps {s.seq_gaps}, RESUME tx {s.resumes_tx} rx {s.resumes_rx}, restarts {s.restarts}, "
            f"ACK-timeout resends {s.timeout_resends}, CRC resends {s.crc_resends}, duplicates {s.duplicates}, "
            f"ignored {s.ignored}")


# --- transports -----------------------------------------------------------------------------------------

async def open_transport(args, rep: Reporter):
    if args.loopback:
        from .loopback import LoopbackServer, LoopbackTransport
        drop = None
        if args.loopback_drop_every:
            every = args.loopback_drop_every
            # Drop every Nth data packet client -> server (simulates a stack queue overflow; exercises RESUME).
            drop = lambda d, i, raw: d == "c2s" and raw[0] in (c.MSG_DATA,) and i % every == every - 1  # noqa: E731
        server = LoopbackServer(os_limit=args.loopback_os_limit, window=args.loopback_window, drop=drop,
                                echo=True)
        rep.info(f"loopback server (os_limit {args.loopback_os_limit}, window {args.loopback_window}"
                 + (f", dropping every {args.loopback_drop_every}th MSG_DATA" if drop else "") + ")")
        return LoopbackTransport(server, os_limit=args.loopback_os_limit)
    try:
        from .ble import BleakTransport
    except ImportError as exc:
        raise RuntimeError(f"bleak is not installed ({exc}); pip install -e clients/python") from None
    t = await BleakTransport.connect(name=None if args.address else args.name, address=args.address,
                                     scan_timeout=args.scan_timeout, log_fn=rep.info)
    os_limit = await t.wait_for_mtu(args.mtu_wait)
    rep.info(f"connected: ATT MTU {t.mtu if t.mtu is not None else '?'}, write-without-response limit "
             f"{os_limit} B, flow control: {t.flow_control}")
    info = None
    try:
        info = await t.read_info()
    except Exception as exc:  # INFO is optional / debugging only (PROTOCOL.md section 1)
        rep.debug(f"INFO read failed: {exc!r}")
    if info:
        rep.info(f"INFO characteristic: {describe_hello(info)}")
    return t


async def run(args) -> int:
    rep = Reporter(args.verbose)
    try:
        transport = await open_transport(args, rep)
    except Exception as exc:
        rep.info(f"RESULT: FAIL could not connect: {exc}")
        return EXIT_ERROR
    session = Session("client", max_packet=args.max_packet, max_message=args.max_message, window=args.window,
                      flags=c.FLAG_PROGRESS, timeout_units=args.timeout_units, os_limit=transport.os_limit,
                      log=rep.packet)

    def on_progress(p: Progressed) -> None:
        rep.debug(f"PROGRESS msgId {p.msg_id}: {p.bytes_received} B received by the server")

    link = Link(transport, session, write_with_response=args.with_response, on_progress=on_progress)
    try:
        ready = await link.open()
    except LinkClosed as exc:
        rep.info(stats_line(session.stats))
        rep.info(f"RESULT: FAIL handshake: {exc}")
        await link.close()
        return EXIT_ERROR
    rep.info(f"HELLO sent:     {describe_hello(session.local)}")
    rep.info(f"HELLO received: {describe_hello(ready.peer)}")
    mtu = getattr(transport, "mtu", None)
    rep.info(f"negotiated: client->server packetSize {ready.tx_packet_size} "
             f"(min(server maxPacket {ready.peer.max_packet}, os limit {transport.os_limit})), "
             f"server->client packetSize <= {session.local.max_packet}, ATT MTU {mtu if mtu else '?'}, "
             f"T = {ready.timeout_s:g} s, writes {'with' if args.with_response else 'without'} response")
    t0 = time.monotonic()
    summary = ""
    try:
        summary = await COMMANDS[args.command](link, args, rep)
    except LinkClosed as exc:
        rep.fail(f"link closed: {exc}")
    finally:
        elapsed = time.monotonic() - t0
        await link.close()
    rep.info(stats_line(session.stats))
    if rep.failures:
        rep.info(f"RESULT: FAIL {summary} ({len(rep.failures)} failure(s); first: {rep.failures[0]})")
        return EXIT_FAIL
    rep.info(f"RESULT: PASS {summary} (total {elapsed:.3f} s, packetSize {ready.tx_packet_size})")
    return EXIT_PASS


# --- argument parsing -----------------------------------------------------------------------------------

def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="blelink-client", description="BleLink v1 reference client / test tool.",
                                epilog="Exit code 0 = PASS, 1 = FAIL, 2 = connect/handshake error.")
    target = p.add_argument_group("target")
    target.add_argument("--name", default="BLE-TestServer", help="advertised name to connect to (default %(default)s)")
    target.add_argument("--address", help="connect by address/UUID instead of name")
    target.add_argument("--scan-timeout", type=float, default=15.0, help="seconds to scan (default %(default)s)")
    target.add_argument("--mtu-wait", type=float, default=3.0,
                        help="seconds to wait for an MTU above 23 before the handshake (default %(default)s)")
    target.add_argument("--loopback", action="store_true", help="no hardware: talk to an in-memory echo server")
    target.add_argument("--loopback-os-limit", type=int, default=244, help=argparse.SUPPRESS)
    target.add_argument("--loopback-window", type=int, default=4, help=argparse.SUPPRESS)
    target.add_argument("--loopback-drop-every", type=int, default=0, help=argparse.SUPPRESS)
    proto = p.add_argument_group("protocol (our HELLO, PROTOCOL.md 4.1)")
    proto.add_argument("--max-packet", type=int, default=c.MAX_PACKET, help="maxPacket we accept (default %(default)s)")
    proto.add_argument("--max-message", type=parse_size, default=64 * 1024 * 1024,
                       help="maxMessage we accept (default 64M)")
    proto.add_argument("--window", type=int, default=4, help="messages we receive concurrently (default %(default)s)")
    proto.add_argument("--timeout-units", type=int, default=0, help="HELLO timeout in 0.25 s units (0 = 10 s)")
    proto.add_argument("--with-response", action="store_true",
                       help="use Write With Response for every packet (slow, lossless fallback)")
    p.add_argument("--op-timeout", type=float, default=0.0, help="seconds per operation (default: size based)")
    p.add_argument("-v", "--verbose", action="count", default=0, help="-v events, -vv every packet")

    sub = p.add_subparsers(dest="command", required=True, metavar="COMMAND")
    sub.add_parser("info", help="connect and handshake only; print the negotiated values")
    s = sub.add_parser("send-text", help="send one UTF-8 text message and wait for its ACK")
    s.add_argument("text")
    s = sub.add_parser("send-blob", help="send SIZE bytes (e.g. 1M) and report throughput")
    s.add_argument("size", type=parse_size)
    s = sub.add_parser("echo-test", help="send each size, wait for the identical echo from the server")
    s.add_argument("sizes", type=parse_size, nargs="+", metavar="SIZE")
    s = sub.add_parser("interleave", help="send a LARGE and a SMALL message concurrently (completion order)")
    s.add_argument("large", type=parse_size)
    s.add_argument("small", type=parse_size)
    s.add_argument("--verify-echo", action="store_true", help="also wait for both echoes")
    s = sub.add_parser("soak", help="send N messages of random size, keep the window full")
    s.add_argument("count", type=int, metavar="N")
    s.add_argument("--max-size", type=parse_size, default=4096, help="largest message (default 4k)")
    s.add_argument("--seed", type=int, default=1)
    s.add_argument("--verify-echo", action="store_true", help="also wait for every echo")
    return p


def main(argv: Optional[list[str]] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if not 1 <= args.window <= 255:
        parser.error("--window must be 1..255")
    if args.command == "soak" and args.count < 1:
        parser.error("soak N must be >= 1")
    try:
        return asyncio.run(run(args))
    except KeyboardInterrupt:
        print("RESULT: FAIL interrupted", flush=True)
        return EXIT_ERROR


if __name__ == "__main__":
    sys.exit(main())
