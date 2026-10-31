package com.bluetoothserver.blelink.transport

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.BluetoothStatusCodes
import android.content.Context
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import java.util.UUID

/**
 * [PacketTransport] on Android's BluetoothGatt (PROTOCOL.md sections 1, 5, 6.2 and 14.3).
 *
 * Connection setup, strictly one GATT operation at a time:
 * connectGatt(LE) -> discoverServices -> requestMtu(517) -> requestConnectionPriority(HIGH)
 * -> setPreferredPhy(2M, API 26+ when the adapter supports it) -> enable TX notifications (CCCD write)
 * -> [PacketTransport.Listener.onLinkReady] with `osLimit = mtu - 3`.
 *
 * Every GATT call and callback runs on one private HandlerThread per connection; binder callbacks are
 * copied and posted there. Writes are queued behind any running operation and the next write is only
 * issued after `onCharacteristicWrite`, which is also the flow-control signal for Write Without Response.
 *
 * Needs BLUETOOTH_CONNECT on API 31+ (the caller checks it; see README "Permissions").
 * After a disconnect (any status, including 133/8) the BluetoothGatt is closed; call [start] again to
 * reconnect with a fresh one, ideally after a short delay.
 */
@SuppressLint("MissingPermission")
class GattTransport(
    private val context: Context,
    private val device: BluetoothDevice,
    private val requestedMtu: Int = 517,
    private val preferLe2M: Boolean = true,
    private val operationTimeoutMs: Long = 10_000,
) : PacketTransport {

    @Volatile private var conn: Connection? = null

    override fun start(listener: PacketTransport.Listener) {
        conn?.close("restarted")
        conn = Connection(listener).also { it.open() }
    }

    override fun write(bytes: ByteArray, withResponse: Boolean) {
        val c = conn
        if (c == null) Log.w(TAG, "write without connection dropped") else c.post { c.enqueueWrite(bytes, withResponse) }
    }

    override fun setHighThroughput(enabled: Boolean) {
        val c = conn ?: return
        c.post { c.setPriority(enabled) }
    }

    override fun close() {
        conn?.close("closed by application")
        conn = null
    }

    /** One BluetoothGatt lifetime. All fields are confined to [thread]. */
    private inner class Connection(private val listener: PacketTransport.Listener) {
        private val thread = HandlerThread("blelink-gatt").apply { start() }
        private val handler = Handler(thread.looper)
        private var gatt: BluetoothGatt? = null
        private var rx: BluetoothGattCharacteristic? = null
        private var tx: BluetoothGattCharacteristic? = null
        private var mtu = 23
        private var linkReady = false
        private var finished = false
        private var highPriority: Boolean? = null

        private val queue = ArrayDeque<Op>()
        private var current: Op? = null
        private val opTimeout = Runnable { current?.let { finish(null, "GATT operation '${it.name}' timed out") } }

        fun post(block: () -> Unit) { handler.post { if (!finished) block() } }

        fun open() = post {
            try {
                val g = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                    device.connectGatt(context, false, callback, BluetoothDevice.TRANSPORT_LE, BluetoothDevice.PHY_LE_1M_MASK, handler)
                } else {
                    device.connectGatt(context, false, callback, BluetoothDevice.TRANSPORT_LE)
                }
                if (g == null) finish(null, "connectGatt returned null") else gatt = g
            } catch (e: SecurityException) {
                finish(null, "missing Bluetooth permission: ${e.message}")
            }
        }

        fun close(reason: String) = handler.post { finish(null, reason) }

        // ------------------------------------------------------------ operation queue

        /** One GATT operation. `run` returns false when the stack refused to start it. */
        private inner class Op(val name: String, val immediate: Boolean = false, val run: (Op) -> Boolean)

        private fun enqueue(op: Op) { queue.addLast(op); next() }

        private fun next() {
            while (current == null && !finished) {
                val op = queue.removeFirstOrNull() ?: return
                current = op
                if (!op.immediate) handler.postDelayed(opTimeout, operationTimeoutMs)
                val started = try { op.run(op) } catch (e: SecurityException) {
                    finish(null, "missing Bluetooth permission: ${e.message}"); return
                }
                if (finished) return
                if (op.immediate) { current = null; continue }
                if (!started) {
                    handler.removeCallbacks(opTimeout)
                    current = null
                    onOpRefused(op)
                }
            }
        }

        private fun completeCurrent() {
            handler.removeCallbacks(opTimeout)
            current = null
            next()
        }

        private fun onOpRefused(op: Op) {
            when (op.name) {
                OP_MTU -> Log.w(TAG, "requestMtu refused; staying at MTU $mtu")
                OP_WRITE -> Unit // handled in the write op itself (busy retry)
                else -> finish(null, "GATT operation '${op.name}' could not be started")
            }
        }

        // ------------------------------------------------------------ setup steps

        private fun onServices(g: BluetoothGatt) {
            val service = g.getService(SERVICE_UUID)
            val r = service?.getCharacteristic(RX_UUID)
            val t = service?.getCharacteristic(TX_UUID)
            if (r == null || t == null) { finish(null, "BleLink service/characteristics not found"); return }
            rx = r; tx = t
            enqueue(Op(OP_MTU) { g.requestMtu(requestedMtu) })
            enqueue(Op("connection priority", immediate = true) { setPriority(true); true })
            if (preferLe2M && Build.VERSION.SDK_INT >= Build.VERSION_CODES.O && adapterSupports2M()) {
                // Result arrives in onPhyUpdate; nothing depends on it (PROTOCOL.md 14.3).
                enqueue(Op("preferred PHY", immediate = true) {
                    g.setPreferredPhy(BluetoothDevice.PHY_LE_2M_MASK, BluetoothDevice.PHY_LE_2M_MASK, BluetoothDevice.PHY_OPTION_NO_PREFERRED)
                    true
                })
            }
            enqueue(Op(OP_CCCD) { enableNotifications(g, t) })
        }

        private fun adapterSupports2M(): Boolean = try {
            val adapter: BluetoothAdapter? = context.getSystemService(BluetoothManager::class.java)?.adapter
            Build.VERSION.SDK_INT >= Build.VERSION_CODES.O && adapter?.isLe2MPhySupported == true
        } catch (e: SecurityException) { false }

        fun setPriority(high: Boolean) {
            val g = gatt ?: return
            if (highPriority == high) return
            highPriority = high
            try {
                g.requestConnectionPriority(
                    if (high) BluetoothGatt.CONNECTION_PRIORITY_HIGH else BluetoothGatt.CONNECTION_PRIORITY_BALANCED,
                )
            } catch (e: SecurityException) {
                finish(null, "missing Bluetooth permission: ${e.message}")
            }
        }

        private fun enableNotifications(g: BluetoothGatt, t: BluetoothGattCharacteristic): Boolean {
            if (!g.setCharacteristicNotification(t, true)) return false
            val cccd = t.getDescriptor(CCCD_UUID) ?: return false
            val value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
            return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                g.writeDescriptor(cccd, value) == BluetoothStatusCodes.SUCCESS
            } else {
                @Suppress("DEPRECATION")
                cccd.value = value
                @Suppress("DEPRECATION")
                g.writeDescriptor(cccd)
            }
        }

        // ------------------------------------------------------------ writes

        fun enqueueWrite(bytes: ByteArray, withResponse: Boolean) {
            if (!linkReady) { listener.onWriteComplete(false); return }
            enqueue(Op(OP_WRITE) { op -> issueWrite(op, bytes, withResponse, attempt = 0); true })
        }

        /**
         * Issues one write while [op] is the current operation. A refusal (the stack is still busy, e.g.
         * ERROR_GATT_WRITE_REQUEST_BUSY on API 33+ or `false` before) is retried every [BUSY_RETRY_MS] up to
         * [BUSY_RETRIES] times; the op stays current so nothing overtakes it. Then the packet is dropped and
         * reported as a failed write (the session's seq/RESUME logic repairs the loss, PROTOCOL.md 8).
         */
        private fun issueWrite(op: Op, bytes: ByteArray, withResponse: Boolean, attempt: Int) {
            if (current !== op || finished) return
            val g = gatt
            val c = rx
            if (g == null || c == null) { finish(null, "write without GATT"); return }
            val type = if (withResponse) BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT else BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
            val ok = try {
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                    g.writeCharacteristic(c, bytes, type) == BluetoothStatusCodes.SUCCESS
                } else {
                    c.writeType = type
                    @Suppress("DEPRECATION")
                    c.value = bytes
                    @Suppress("DEPRECATION")
                    g.writeCharacteristic(c)
                }
            } catch (e: SecurityException) {
                finish(null, "missing Bluetooth permission: ${e.message}"); return
            }
            if (ok) return // onCharacteristicWrite completes the op
            if (attempt >= BUSY_RETRIES) {
                Log.w(TAG, "write refused ${attempt + 1} times; dropping packet")
                completeCurrent()
                listener.onWriteComplete(false)
                return
            }
            handler.postDelayed({ issueWrite(op, bytes, withResponse, attempt + 1) }, BUSY_RETRY_MS)
        }

        // ------------------------------------------------------------ callbacks (binder threads -> handler)

        private val callback = object : BluetoothGattCallback() {
            override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) = post {
                if (status == BluetoothGatt.GATT_SUCCESS && newState == BluetoothProfile.STATE_CONNECTED) {
                    gatt = g
                    enqueue(Op("discoverServices") { g.discoverServices() })
                } else {
                    finish(status, if (newState == BluetoothProfile.STATE_DISCONNECTED) "disconnected (status $status)" else "connection failed (status $status)")
                }
            }

            override fun onServicesDiscovered(g: BluetoothGatt, status: Int) = post {
                if (current?.name == "discoverServices") completeCurrent()
                if (status != BluetoothGatt.GATT_SUCCESS) finish(status, "service discovery failed (status $status)")
                else onServices(g)
            }

            override fun onMtuChanged(g: BluetoothGatt, newMtu: Int, status: Int) = post {
                if (status == BluetoothGatt.GATT_SUCCESS) mtu = newMtu
                if (linkReady) listener.onOsLimitChanged(osLimit())
                if (current?.name == OP_MTU) completeCurrent()
            }

            override fun onDescriptorWrite(g: BluetoothGatt, d: BluetoothGattDescriptor, status: Int) = post {
                if (current?.name != OP_CCCD) return@post
                completeCurrent()
                if (status != BluetoothGatt.GATT_SUCCESS) { finish(status, "enabling TX notifications failed (status $status)"); return@post }
                linkReady = true
                listener.onLinkReady(osLimit())
            }

            override fun onCharacteristicWrite(g: BluetoothGatt, c: BluetoothGattCharacteristic, status: Int) = post {
                if (current?.name != OP_WRITE) return@post
                completeCurrent()
                listener.onWriteComplete(status == BluetoothGatt.GATT_SUCCESS)
            }

            // API 33+: value passed in; the deprecated overload is also invoked there, so it ignores 33+.
            override fun onCharacteristicChanged(g: BluetoothGatt, c: BluetoothGattCharacteristic, value: ByteArray) {
                if (c.uuid == TX_UUID) { val copy = value.copyOf(); post { listener.onPacket(copy) } }
            }

            @Deprecated("API < 33")
            override fun onCharacteristicChanged(g: BluetoothGatt, c: BluetoothGattCharacteristic) {
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) return
                @Suppress("DEPRECATION")
                val copy = c.value?.copyOf() ?: return
                if (c.uuid == TX_UUID) post { listener.onPacket(copy) }
            }
        }

        private fun osLimit() = (mtu - 3).coerceIn(20, 512)

        /** Ends this connection once: close the gatt, stop the thread, tell the listener. */
        private fun finish(status: Int?, reason: String) {
            if (finished) return
            finished = true
            handler.removeCallbacksAndMessages(null)
            queue.clear(); current = null
            try { gatt?.disconnect(); gatt?.close() } catch (_: SecurityException) { }
            gatt = null
            if (conn === this) conn = null
            listener.onDisconnected(status, reason)
            thread.quitSafely()
        }
    }

    companion object {
        private const val TAG = "BleLink"
        val SERVICE_UUID: UUID = UUID.fromString("7b1e1000-5a6c-4f0e-9d3b-2c1a8e4f6b01")
        val RX_UUID: UUID = UUID.fromString("7b1e1001-5a6c-4f0e-9d3b-2c1a8e4f6b01")
        val TX_UUID: UUID = UUID.fromString("7b1e1002-5a6c-4f0e-9d3b-2c1a8e4f6b01")
        val INFO_UUID: UUID = UUID.fromString("7b1e1003-5a6c-4f0e-9d3b-2c1a8e4f6b01")
        val CCCD_UUID: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")

        private const val OP_MTU = "requestMtu"
        private const val OP_CCCD = "enable TX notifications"
        private const val OP_WRITE = "write"
        private const val BUSY_RETRIES = 50
        private const val BUSY_RETRY_MS = 10L
    }
}
