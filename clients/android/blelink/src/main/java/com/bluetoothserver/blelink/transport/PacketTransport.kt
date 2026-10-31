package com.bluetoothserver.blelink.transport

/**
 * The link under a BleLink session: one packet per RX write, one packet per TX notification
 * (PROTOCOL.md section 1). [GattTransport] implements it on Android's BluetoothGatt; tests use a fake.
 *
 * Contract:
 *  - [start] connects and prepares the link (service discovery, MTU, TX CCCD). When the link can carry
 *    packets it calls [Listener.onLinkReady] exactly once per connection.
 *  - [write] is called with at most ONE write outstanding: the caller waits for
 *    [Listener.onWriteComplete] before the next [write] (PROTOCOL.md 6.2, Android row).
 *  - Listener callbacks may arrive on any thread, but never concurrently with each other.
 *  - After [Listener.onDisconnected] the transport is finished; connect again with a new [start].
 */
interface PacketTransport {

    interface Listener {
        /** TX notifications are enabled. `osLimit` = largest packet one write may carry (ATT_MTU - 3). */
        fun onLinkReady(osLimit: Int)

        /** The MTU changed after [onLinkReady] (some stacks renegotiate). */
        fun onOsLimitChanged(osLimit: Int)

        /** One TX notification. The array is owned by the receiver. */
        fun onPacket(bytes: ByteArray)

        /** The stack took (or refused) the last [write]. The next write may be issued now. */
        fun onWriteComplete(success: Boolean)

        /** The link is gone (or could not be set up). `status` is the platform status code, if any. */
        fun onDisconnected(status: Int?, reason: String)
    }

    fun start(listener: Listener)

    /** Writes one packet to RX. `withResponse` selects Write Request vs Write Command. */
    fun write(bytes: ByteArray, withResponse: Boolean)

    /** Throughput hint: true while messages are in flight, false when idle (connection priority). */
    fun setHighThroughput(enabled: Boolean) {}

    /** Disconnects and releases the link. Calls [Listener.onDisconnected] if it was connected. */
    fun close()
}
