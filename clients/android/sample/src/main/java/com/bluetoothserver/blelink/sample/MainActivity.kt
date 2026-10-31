package com.bluetoothserver.blelink.sample

import android.Manifest
import android.annotation.SuppressLint
import android.app.Activity
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothManager
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.ParcelUuid
import android.text.InputType
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.ScrollView
import android.widget.TextView
import com.bluetoothserver.blelink.BleLinkClient
import com.bluetoothserver.blelink.BleLinkEvent
import com.bluetoothserver.blelink.transport.GattTransport
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch

/**
 * Minimal BleLink v1 client: scan for the BleLink service, connect, send a text or an N-KB blob, show
 * progress and a log. Plain framework views (no AndroidX) so the sample has no dependency besides the library.
 */
@SuppressLint("MissingPermission", "SetTextI18n")
class MainActivity : Activity() {

    private val ui = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    private val main = Handler(Looper.getMainLooper())
    private var client: BleLinkClient? = null
    private var scanning = false
    private val sentAt = HashMap<Int, Long>()
    private val sizes = HashMap<Int, Int>()

    private lateinit var status: TextView
    private lateinit var text: EditText
    private lateinit var kb: EditText
    private lateinit var progress: ProgressBar
    private lateinit var log: TextView
    private lateinit var logScroll: ScrollView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        buildUi()
    }

    override fun onDestroy() {
        stopScan()
        client?.disconnect()
        ui.cancel()
        super.onDestroy()
    }

    // ------------------------------------------------------------------ permissions (README "Permissions")

    private fun requiredPermissions(): Array<String> =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
        } else {
            // API 29/30 need FINE for BLE scan results; 23..28 accept COARSE. Ask for both.
            arrayOf(Manifest.permission.ACCESS_FINE_LOCATION, Manifest.permission.ACCESS_COARSE_LOCATION)
        }

    private fun hasPermissions() = requiredPermissions().all { checkSelfPermission(it) == PackageManager.PERMISSION_GRANTED }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        if (requestCode == REQ_PERMS && hasPermissions()) startScan() else append("permissions denied")
    }

    // ------------------------------------------------------------------ scan + connect

    private fun startScan() {
        if (!hasPermissions()) { requestPermissions(requiredPermissions(), REQ_PERMS); return }
        val adapter = getSystemService(BluetoothManager::class.java)?.adapter
        val scanner = adapter?.bluetoothLeScanner
        if (adapter == null || !adapter.isEnabled || scanner == null) { append("Bluetooth is off or missing"); return }
        if (scanning) return
        scanning = true
        status.text = "scanning for BleLink service..."
        val filter = ScanFilter.Builder().setServiceUuid(ParcelUuid(GattTransport.SERVICE_UUID)).build()
        val settings = ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build()
        scanner.startScan(listOf(filter), settings, scanCallback)
        main.postDelayed({ if (scanning) { stopScan(); status.text = "no BleLink server found" } }, 15_000)
    }

    private fun stopScan() {
        if (!scanning) return
        scanning = false
        getSystemService(BluetoothManager::class.java)?.adapter?.bluetoothLeScanner?.stopScan(scanCallback)
    }

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            main.post { if (scanning) { stopScan(); connect(result.device) } }
        }

        override fun onScanFailed(errorCode: Int) { main.post { scanning = false; append("scan failed: $errorCode") } }
    }

    private fun connect(device: BluetoothDevice) {
        client?.disconnect()
        append("connecting to ${device.address}")
        val c = BleLinkClient(GattTransport(applicationContext, device))
        client = c
        ui.launch { c.state.collect { status.text = "${device.address}: $it" } }
        ui.launch { c.events.collect { onEvent(it) } } // subscribed before connect(): PeerReady is not missed
        c.connect()
    }

    // ------------------------------------------------------------------ send + events

    private fun send(bytes: ByteArray, label: String) {
        val c = client ?: return append("not connected")
        try {
            val id = c.send(bytes)
            sentAt[id] = System.nanoTime(); sizes[id] = bytes.size
            append("#$id queued: $label (${bytes.size} B)")
        } catch (e: IllegalStateException) {
            append("send refused: ${e.message}")
        }
    }

    private fun onEvent(e: BleLinkEvent) {
        when (e) {
            is BleLinkEvent.PeerReady -> append(
                "ready: server maxPacket=${e.peer.maxPacket} maxMessage=${e.peer.maxMessage} window=${e.peer.window}, " +
                    "we send ${e.sendPacketSize}-byte packets" + if (e.reset) " (server reset)" else "",
            )
            is BleLinkEvent.Received -> append("received #${e.msgId}: ${e.data.size} B " + preview(e.data))
            is BleLinkEvent.Sent -> {
                val t = sentAt.remove(e.msgId); val n = sizes.remove(e.msgId) ?: 0
                val rate = if (t != null) " in %.2f s, %.1f KB/s".format(secs(t), n / 1024.0 / maxOf(secs(t), 1e-3)) else ""
                progress.progress = 100
                append("#${e.msgId} ACKed$rate")
            }
            is BleLinkEvent.Failed -> { sentAt.remove(e.msgId); sizes.remove(e.msgId); append("#${e.msgId} failed: ${e.reason} ${e.ackStatus ?: ""}") }
            is BleLinkEvent.Progress -> if (e.kind == BleLinkEvent.Progress.Kind.SENT_TO_STACK && e.total > 0) {
                progress.progress = (e.bytes * 100 / e.total).toInt()
            }
            is BleLinkEvent.Disconnected -> append("disconnected: ${e.reason}")
        }
    }

    private fun secs(startNs: Long) = (System.nanoTime() - startNs) / 1e9
    private fun preview(b: ByteArray) =
        if (b.size <= 64 && b.all { it in 0x20..0x7e }) "\"${String(b, Charsets.US_ASCII)}\"" else ""

    private fun blob(n: Int) = ByteArray(n) { (it * 7 + 3).toByte() }

    // ------------------------------------------------------------------ UI

    private fun buildUi() {
        val root = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; setPadding(32, 32, 32, 32) }
        fun row(vararg views: android.view.View) = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            views.forEach { addView(it, LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f)) }
            root.addView(this, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
        }
        fun button(label: String, onClick: () -> Unit) = Button(this).apply { text = label; setOnClickListener { onClick() } }

        status = TextView(this).apply { text = "idle" }
        root.addView(status)
        row(button("Scan + connect") { startScan() }, button("Disconnect") { stopScan(); client?.disconnect() })
        text = EditText(this).apply { setText("hello from Android"); setSingleLine() }
        row(text, button("Send text") { send(text.text.toString().toByteArray(Charsets.UTF_8), "text") })
        kb = EditText(this).apply { setText("64"); inputType = InputType.TYPE_CLASS_NUMBER; hint = "KB" }
        row(kb, button("Send N KB") {
            val n = kb.text.toString().toIntOrNull()?.coerceIn(0, 16 * 1024) ?: 64
            send(blob(n * 1024), "$n KB blob")
        })
        progress = ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal).apply { max = 100 }
        root.addView(progress, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
        log = TextView(this).apply { setTextIsSelectable(true); typeface = android.graphics.Typeface.MONOSPACE; textSize = 12f }
        logScroll = ScrollView(this).apply { addView(log) }
        root.addView(logScroll, LinearLayout.LayoutParams(MATCH_PARENT, 0, 1f))
        setContentView(root)
    }

    private fun append(line: String) {
        log.append(line + "\n")
        logScroll.post { logScroll.fullScroll(ScrollView.FOCUS_DOWN) }
    }

    private companion object { const val REQ_PERMS = 1 }
}
