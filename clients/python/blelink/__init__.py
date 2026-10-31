"""BleLink v1 reference client (GATT central side) - see docs/PROTOCOL.md.

Layers, each usable on its own:

* ``blelink.codec``    packet encode/decode, CRC-32, constants (no I/O)
* ``blelink.session``  sans-IO protocol state machine (handshake, framing, window, ACK, RESUME, timeouts)
* ``blelink.link``     asyncio driver: runs a Session over a Transport with flow control
* ``blelink.ble``      bleak Transport (imported lazily; needs ``bleak``)
* ``blelink.loopback`` in-memory Transport + fake server, for tests and ``--loopback``
* ``blelink.cli``      ``blelink-client`` command line tool

Implemented from the spec only; it does not import or mirror the C++ server code.
"""
from .codec import PROTOCOL_VERSION, crc32, decode, encode
from .link import Link, LinkClosed, SendError
from .session import Delivered, OutMessage, Ready, Session

__all__ = ["PROTOCOL_VERSION", "crc32", "decode", "encode", "Link", "LinkClosed", "SendError", "Delivered",
           "OutMessage", "Ready", "Session"]
__version__ = "1.0.0"
