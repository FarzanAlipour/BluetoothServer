"""asyncio Link + CLI over the in-memory loopback transport (no hardware, no bleak)."""
from __future__ import annotations

import asyncio

import pytest

from blelink import cli
from blelink import codec as c
from blelink.link import Link, SendError
from blelink.loopback import LoopbackServer, LoopbackTransport
from blelink.session import Session
from conftest import payload_rule


def run(coro, timeout=60):
    return asyncio.run(asyncio.wait_for(coro, timeout))


async def _open(os_limit=244, **server_kw):
    server = LoopbackServer(os_limit=os_limit, **server_kw)
    t = LoopbackTransport(server, os_limit=os_limit)
    link = Link(t, Session("client", os_limit=os_limit), tick=0.01)
    ready = await link.open()
    return link, t, server, ready


def test_link_echo_round_trip_large():
    async def go():
        link, t, server, ready = await _open(os_limit=182)
        assert ready.tx_packet_size == 182
        data = payload_rule(300_000)
        m = await link.send(data)
        echo = await link.receive(timeout=20)
        await link.close()
        return m, echo, t
    m, echo, t = run(go())
    assert m.ok and echo.data == payload_rule(300_000)
    assert t.writes_with_response == 1          # only the HELLO (PROTOCOL.md section 1)


def test_link_repairs_drops():
    drop = lambda d, i, raw: d == "c2s" and raw[0] == c.MSG_DATA and i % 50 == 49   # noqa: E731

    async def go():
        link, t, server, _ = await _open(os_limit=20, drop=drop)
        msgs = [payload_rule(n) for n in (3000, 1, 700)]
        results = await asyncio.gather(*(link.send(d) for d in msgs))
        echoes = [await link.receive(timeout=20) for _ in msgs]
        stats = link.session.stats
        await link.close()
        return msgs, results, echoes, stats, server
    msgs, results, echoes, stats, server = run(go())
    assert all(m.ok for m in results)
    assert sorted(e.data for e in echoes) == sorted(msgs)
    assert server.session.stats.seq_gaps > 0 and stats.restarts > 0


def test_link_disconnect_mid_transfer_fails_send():
    async def go():
        link, t, server, _ = await _open(os_limit=20)
        fut = link.submit(payload_rule(200_000))
        await asyncio.sleep(0.05)
        t.drop_link()
        m = await fut
        with pytest.raises(Exception):
            link.submit(b"x")
        await link.close()
        return m
    m = run(go())
    assert not m.ok and m.reason == "disconnected"


def test_link_send_raises_on_rejection():
    async def go():
        link, t, server, _ = await _open(max_message=100)
        try:
            await link.send(payload_rule(101))
        except SendError as exc:
            return exc.reason
        finally:
            await link.close()
    assert run(go()) == "too_large"


@pytest.mark.parametrize("argv", [
    ["info"],
    ["send-text", "hello wörld"],
    ["send-blob", "64k"],
    ["echo-test", "0", "1", "20", "244", "4k", "100k"],
    ["interleave", "128k", "16", "--verify-echo"],
    ["soak", "60", "--max-size", "2k", "--verify-echo"],
    ["--loopback-drop-every", "40", "--loopback-os-limit", "20", "echo-test", "5k"],
    ["--with-response", "send-blob", "10k"],
])
def test_cli_loopback_pass(argv, capsys):
    assert cli.main(["--loopback"] + argv) == cli.EXIT_PASS
    out = capsys.readouterr().out
    assert out.strip().splitlines()[-1].startswith("RESULT: PASS")
    assert "HELLO received" in out and "packetSize" in out and "stats:" in out


def test_cli_loopback_fail_exit_code(capsys):
    # The loopback server accepts 16 MiB; 17M is refused before it is started -> FAIL, exit 1.
    assert cli.main(["--loopback", "send-blob", "17M"]) == cli.EXIT_FAIL
    assert capsys.readouterr().out.strip().splitlines()[-1].startswith("RESULT: FAIL")


def test_parse_size():
    assert cli.parse_size("512") == 512 and cli.parse_size("4k") == 4096 and cli.parse_size("1M") == 1 << 20
    with pytest.raises(Exception):
        cli.parse_size("abc")


def test_test_payload_matches_vector_rule():
    assert cli.test_payload(1000) == payload_rule(1000)
