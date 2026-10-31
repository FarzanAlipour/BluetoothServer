"""bleak transport for the BleLink service (PROTOCOL.md section 1).

Imported lazily by the CLI so the codec/session tests run without bleak installed.

Flow control with bleak 3.0 (PROTOCOL.md 6.2), per backend:

* WinRT (Windows): ``write_gatt_char(response=False)`` awaits ``WriteValueWithResultAsync``; the await is the
  backpressure.
* BlueZ (Linux): the write without response is a D-Bus ``WriteValue`` call (type "command"); awaiting the reply
  bounds the queue.
* CoreBluetooth (macOS): bleak does not wait for ``canSendWriteWithoutResponse`` (its
  ``peripheralIsReadyToSendWriteWithoutResponse`` delegate is a no-op). ``wait_writable`` therefore polls
  ``CBPeripheral.canSendWriteWithoutResponse()`` through bleak's private backend object. If that attribute
  is missing (bleak changed), it falls back to no wait: CoreBluetooth then drops writes silently, the server
  sees a seq gap and RESUME repairs it (section 8). ``--with-response`` is the lossless fallback.
"""
from __future__ import annotations

import asyncio
import logging
from typing import Callable, Optional

from bleak import BleakClient, BleakScanner

from . import codec as c

log = logging.getLogger("blelink.ble")


class BleakTransport:
    def __init__(self, client: "BleakClient", log_fn: Callable[[str], None] = print):
        self.client = client
        self.log = log_fn
        svc = client.services.get_service(c.SERVICE_UUID)
        if svc is None:
            raise RuntimeError(f"BleLink service {c.SERVICE_UUID} not found on {client.address}")
        self.rx = svc.get_characteristic(c.RX_UUID)
        self.tx = svc.get_characteristic(c.TX_UUID)
        self.info = svc.get_characteristic(c.INFO_UUID)
        if self.rx is None or self.tx is None:
            raise RuntimeError("BleLink RX/TX characteristics missing")
        self._peripheral = getattr(getattr(client, "_backend", None), "_peripheral", None)
        self._can_send = getattr(self._peripheral, "canSendWriteWithoutResponse", None)
        self._on_disconnect: Optional[Callable[[], None]] = None
        self.flow_control = "CoreBluetooth canSendWriteWithoutResponse" if self._can_send else "awaited write"

    # --- discovery / connect -----------------------------------------------------------------------------

    @classmethod
    async def connect(cls, name: Optional[str] = None, address: Optional[str] = None, scan_timeout: float = 10.0,
                      log_fn: Callable[[str], None] = print) -> "BleakTransport":
        if address:
            device = await BleakScanner.find_device_by_address(address, timeout=scan_timeout)
        else:
            want = name

            def match(dev, adv) -> bool:
                names = {dev.name, adv.local_name}
                if want:
                    return want in names
                return c.SERVICE_UUID in [u.lower() for u in (adv.service_uuids or [])]

            device = await BleakScanner.find_device_by_filter(match, timeout=scan_timeout)
        if device is None:
            raise RuntimeError(f"no device found (name={name!r}, address={address!r}) within {scan_timeout:g} s")
        log_fn(f"found {device.name or '?'} [{device.address}]")
        holder: dict = {}

        def disconnected(_client) -> None:
            t = holder.get("t")
            if t and t._on_disconnect:
                t._on_disconnect()

        client = BleakClient(device, disconnected_callback=disconnected)
        await client.connect()
        try:
            t = cls(client, log_fn)
        except Exception:
            await client.disconnect()
            raise
        holder["t"] = t
        return t

    # --- Transport protocol ------------------------------------------------------------------------------

    @property
    def mtu(self) -> Optional[int]:
        try:
            return self.client.mtu_size
        except Exception:
            return None

    @property
    def os_limit(self) -> int:
        """ATT_MTU - 3 for write without response, as bleak reports it (BlueZ < 5.62 always says 20)."""
        return self.rx.max_write_without_response_size

    async def wait_for_mtu(self, timeout: float = 3.0) -> int:
        """Some stacks report 20 until the MTU exchange completed; wait a little for a larger value."""
        deadline = asyncio.get_running_loop().time() + timeout
        while self.os_limit <= c.MIN_PACKET and asyncio.get_running_loop().time() < deadline:
            await asyncio.sleep(0.25)
        return self.os_limit

    async def read_info(self) -> Optional[c.Hello]:
        if self.info is None:
            return None
        return c.decode_info(bytes(await self.client.read_gatt_char(self.info)))

    async def start(self, receiver: Callable[[bytes], None], on_disconnect: Callable[[], None]) -> None:
        self._on_disconnect = on_disconnect
        await self.client.start_notify(self.tx, lambda _ch, data: receiver(bytes(data)))

    async def wait_writable(self) -> None:
        if self._can_send is None:
            return
        while self.client.is_connected and not self._can_send():
            await asyncio.sleep(0.001)

    async def write(self, data: bytes, with_response: bool) -> None:
        await self.client.write_gatt_char(self.rx, data, response=with_response)

    async def close(self) -> None:
        try:
            if self.client.is_connected:
                try:
                    await self.client.stop_notify(self.tx)
                except Exception:
                    pass
                await self.client.disconnect()
        except Exception as exc:  # already gone
            log.debug("disconnect: %r", exc)
