package com.bluetoothserver.blelink.sample

// Compile check for the usage example in clients/android/README.md. Keep the two in sync.

import android.bluetooth.BluetoothDevice
import android.content.Context
import com.bluetoothserver.blelink.BleLinkClient
import com.bluetoothserver.blelink.BleLinkEvent
import com.bluetoothserver.blelink.transport.GattTransport
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.launch

@Suppress("unused")
suspend fun readmeExample(context: Context, device: BluetoothDevice, scope: CoroutineScope) {
    val client = BleLinkClient(GattTransport(context, device))
    scope.launch {
        client.events.collect { e ->
            when (e) {
                is BleLinkEvent.PeerReady -> println("ready, ${e.sendPacketSize}-byte packets")
                is BleLinkEvent.Received -> println("got ${e.data.size} bytes")
                is BleLinkEvent.Sent -> println("message ${e.msgId} ACKed")
                is BleLinkEvent.Failed -> println("message ${e.msgId} failed: ${e.reason}")
                is BleLinkEvent.Progress -> println("${e.msgId}: ${e.bytes}/${e.total} (${e.kind})")
                is BleLinkEvent.Disconnected -> println("disconnected: ${e.reason}")
            }
        }
    }
    client.connect()                              // GATT connect, MTU 517, CCCD, HELLO
    val server = client.awaitReady()              // suspends until the server's HELLO
    println("server accepts up to ${server.maxMessage} bytes")
    val id = client.send("hello".toByteArray())   // -> msgId; outcome arrives as Sent / Failed
    client.send(ByteArray(256 * 1024))            // any size up to server.maxMessage
    println("queued $id")
    // ... later
    client.disconnect()
}
