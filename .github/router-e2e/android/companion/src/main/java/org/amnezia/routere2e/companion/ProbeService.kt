package org.amnezia.routere2e.companion

import android.app.Service
import android.content.Intent
import android.os.Bundle
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.Message
import android.os.Messenger
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.Socket
import kotlin.concurrent.thread

class ProbeService : Service() {
    private val messenger = Messenger(object : Handler(Looper.getMainLooper()) {
        override fun handleMessage(message: Message) {
            val replyTo = message.replyTo ?: return
            val operation = message.data.getString("operation") ?: return
            val host = message.data.getString("host") ?: return
            val port = message.data.getInt("port")
            thread(name = "router-e2e-companion") {
                val result = runCatching {
                    when (operation) {
                        "resolve" -> InetAddress.getAllByName(host)
                            .joinToString(",") { it.hostAddress ?: "" }
                        "http" -> request(
                            host,
                            port,
                        )
                        else -> error("unknown operation")
                    }
                }
                val response = Message.obtain(null, if (result.isSuccess) 1 else 2)
                response.data = Bundle().apply {
                    result.onSuccess { putString("result", it) }
                    result.onFailure { putString("error", it.javaClass.simpleName) }
                }
                replyTo.send(response)
            }
        }
    })

    override fun onBind(intent: Intent?): IBinder = messenger.binder

    private fun request(host: String, port: Int): String {
        Socket().use { socket ->
            socket.connect(InetSocketAddress(host, port), 10_000)
            socket.soTimeout = 10_000
            socket.getOutputStream().bufferedWriter(Charsets.US_ASCII).use { output ->
                output.write("GET / HTTP/1.0\r\nHost: $host\r\nConnection: close\r\n\r\n")
                output.flush()
                val response = socket.getInputStream().bufferedReader().readText()
                return response.substringAfter("\r\n\r\n").trim()
            }
        }
    }
}
