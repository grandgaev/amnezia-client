package org.amnezia.routere2e

import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.net.ConnectivityManager
import android.net.NetworkCapabilities
import android.net.VpnService
import android.os.Bundle
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.Message
import android.os.Messenger
import android.util.Log
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import java.io.ByteArrayOutputStream
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.Socket
import java.security.SecureRandom
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit
import org.json.JSONArray
import org.json.JSONObject
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class RouterE2ETest {
    private val targetContext = InstrumentationRegistry.getInstrumentation().targetContext
    private val testContext = InstrumentationRegistry.getInstrumentation().context
    private val states = LinkedBlockingQueue<Int>()
    private val serviceConnected = java.util.concurrent.CountDownLatch(1)
    private var service: Messenger? = null
    @Volatile private var backendFailed = false

    private val reply = Messenger(object : Handler(Looper.getMainLooper()) {
        override fun handleMessage(message: Message) {
            when (message.what) {
                EVENT_STATUS_CHANGED, EVENT_STATUS -> states.offer(message.data.getInt("state"))
                EVENT_ERROR -> backendFailed = true
            }
        }
    })

    private val serviceConnection = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName?, binder: IBinder?) {
            service = Messenger(binder)
            send(ACTION_REGISTER, Bundle().apply { putString("CLIENT_NAME", "router-e2e") })
            serviceConnected.countDown()
        }

        override fun onServiceDisconnected(name: ComponentName?) {
            service = null
        }
    }

    private val companionConnected = java.util.concurrent.CountDownLatch(1)
    private var companion: Messenger? = null
    private val companionConnection = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName?, binder: IBinder?) {
            companion = Messenger(binder)
            companionConnected.countDown()
        }

        override fun onServiceDisconnected(name: ComponentName?) {
            companion = null
        }
    }

    @Before
    fun bindCompanion() {
        val intent = Intent().setComponent(ComponentName(COMPANION_PACKAGE, COMPANION_SERVICE))
        assertTrue(testContext.bindService(intent, companionConnection, Context.BIND_AUTO_CREATE))
        assertTrue("companion service did not bind", companionConnected.await(10, TimeUnit.SECONDS))
    }

    @After
    fun cleanup() {
        runCatching {
            if (service != null) {
                send(ACTION_DISCONNECT)
                awaitState(STATE_DISCONNECTED, 10)
                send(ACTION_UNREGISTER)
                targetContext.unbindService(serviceConnection)
            }
        }
        runCatching { testContext.unbindService(companionConnection) }
        targetContext.stopService(vpnIntent())
    }

    @Test
    fun productionVpnServiceRoutesDirectProxyBlockSwitchAndPerApp() {
        val credentials = JSONObject(
            testContext.assets.open("routing-lab.json").bufferedReader().use { it.readText() },
        )
        validateLab(credentials)

        awaitVpnTransport(expected = false)
        val baseline = awaitBaselineDns()
        assertTrue("example.com did not resolve before the VPN started", baseline.isNotBlank())
        assertTrue("baseline DNS returned a local address", baseline.split(',').any {
            val address = InetAddress.getByName(it)
            !address.isLoopbackAddress && !address.isAnyLocalAddress && !address.isSiteLocalAddress
        })
        Log.i(TAG, "PASS: example.com resolved before the VPN started")
        assertNull("the emulator did not grant VPN consent", VpnService.prepare(targetContext))

        val firstConfig = clientConfig(credentials, switchAction = "direct")
        targetContext.startForegroundService(vpnIntent().putExtra("VPN_CONFIG", firstConfig.toString()))
        assertTrue(
            targetContext.bindService(vpnIntent(), serviceConnection, Context.BIND_AUTO_CREATE),
        )
        assertTrue("production VPN service did not bind", serviceConnected.await(10, TimeUnit.SECONDS))
        awaitState(STATE_CONNECTED, 90)
        awaitVpnTransport(expected = true)
        Log.i(TAG, "PASS: production AwgService connected with a VPN transport")

        assertEquals("default proxy action did not use the tunnel", "10.77.0.4", httpRequest("10.77.0.1", 18080, "10.77.0.1"))
        Log.i(TAG, "PASS: an unmatched IP followed the default proxy action")

        val proxyAddress = requireSingleAddress(dnsQueryA("proxy.android.test"), "10.77.0.1")
        assertEquals("10.77.0.4", httpRequest(proxyAddress, 18080, "proxy.android.test"))
        Log.i(TAG, "PASS: encrypted proxy traffic reached the server from 10.77.0.4")

        val proxiedPublicSource = httpRequest(PUBLIC_SERVER, PUBLIC_HTTP_PORT, PUBLIC_SERVER)
        val companionSource = companionProbe("http", PUBLIC_SERVER, PUBLIC_HTTP_PORT)
        assertTrue(companionSource.isNotBlank())
        assertTrue(proxiedPublicSource.isNotBlank())
        assertFalse("default proxy and excluded traffic used the same source", proxiedPublicSource == companionSource)
        assertFalse("excluded companion application used the tunnel", companionSource == "10.77.0.4")
        Log.i(TAG, "PASS: per-application exclusion kept companion traffic outside the tunnel")

        val directAddress = requireSingleAddress(dnsQueryA("direct.android.test"), PUBLIC_SERVER)
        val directSource = httpRequest(directAddress, PUBLIC_HTTP_PORT, "direct.android.test")
        assertTrue(directSource.isNotBlank())
        assertFalse("direct traffic used the tunnel address", directSource == "10.77.0.4")
        assertEquals("direct and excluded traffic did not use the same physical source", companionSource, directSource)
        Log.i(TAG, "PASS: direct traffic left through a protected router socket")

        val blocked = dnsQueryA("example.com")
        assertEquals("block rule did not return NXDOMAIN", 3, blocked.rcode)
        assertTrue(blocked.addresses.isEmpty())
        Log.i(TAG, "PASS: block rule returned NXDOMAIN for the real control domain")

        val switchAddress = requireSingleAddress(dnsQueryA("switch.android.test"), PUBLIC_SERVER)
        val switchSource = httpRequest(switchAddress, PUBLIC_HTTP_PORT, "switch.android.test")
        assertEquals("initial switch rule was not direct", companionSource, switchSource)

        states.clear()
        send(ACTION_DISCONNECT)
        awaitState(STATE_DISCONNECTED, 20)
        awaitVpnTransport(expected = false)
        states.clear()
        send(
            ACTION_CONNECT,
            Bundle().apply {
                putString("VPN_CONFIG", clientConfig(credentials, switchAction = "block").toString())
            },
        )
        awaitState(STATE_CONNECTED, 90)
        awaitVpnTransport(expected = true)
        val switched = dnsQueryA("switch.android.test")
        assertEquals("new routing profile did not replace the previous direct rule", 3, switched.rcode)
        assertTrue(switched.addresses.isEmpty())
        Log.i(TAG, "PASS: restarted production service applied the replacement routing profile")

        val previousNetworkHandle = targetContext.getSystemService(ConnectivityManager::class.java)
            .activeNetwork?.networkHandle ?: error("VPN network was unavailable before queued reconnect")
        states.clear()
        send(ACTION_DISCONNECT)
        send(
            ACTION_CONNECT,
            Bundle().apply {
                putString("VPN_CONFIG", clientConfig(credentials, switchAction = "direct").toString())
            },
        )
        awaitState(STATE_CONNECTED, 90)
        awaitVpnTransport(expected = true, previousNetworkHandle = previousNetworkHandle)
        val queuedSwitchAddress = requireSingleAddress(dnsQueryA("switch.android.test"), PUBLIC_SERVER)
        val queuedSwitchSource = httpRequest(queuedSwitchAddress, PUBLIC_HTTP_PORT, "switch.android.test")
        assertEquals("queued replacement profile was not applied", companionSource, queuedSwitchSource)
        Log.i(TAG, "PASS: a connection queued during shutdown applied its replacement routing profile")

        states.clear()
        send(ACTION_DISCONNECT)
        awaitState(STATE_DISCONNECTED, 20)
        awaitVpnTransport(expected = false)
    }

    private fun vpnIntent() = Intent().setComponent(ComponentName(TARGET_PACKAGE, TARGET_SERVICE))

    private fun send(action: Int, data: Bundle = Bundle.EMPTY) {
        val message = Message.obtain(null, action)
        message.data = data
        message.replyTo = reply
        service?.send(message) ?: error("production service messenger is unavailable")
    }

    private fun awaitState(expected: Int, timeoutSeconds: Long) {
        val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(timeoutSeconds)
        while (System.nanoTime() < deadline) {
            check(!backendFailed) { "production VPN service reported an error" }
            send(ACTION_STATUS)
            val state = states.poll(500, TimeUnit.MILLISECONDS)
            if (state == expected) {
                states.clear()
                return
            }
        }
        error("timed out waiting for production VPN state $expected")
    }

    private fun companionProbe(operation: String, host: String, port: Int): String {
        val responses = LinkedBlockingQueue<Message>()
        val callback = Messenger(object : Handler(Looper.getMainLooper()) {
            override fun handleMessage(message: Message) {
                responses.offer(Message.obtain(message))
            }
        })
        val message = Message.obtain(null, 1)
        message.data = Bundle().apply {
            putString("operation", operation)
            putString("host", host)
            putInt("port", port)
        }
        message.replyTo = callback
        companion?.send(message) ?: error("companion service messenger is unavailable")
        val response = responses.poll(20, TimeUnit.SECONDS) ?: error("companion probe timed out")
        if (response.what != 1) {
            throw CompanionProbeException(response.data.getString("error") ?: "UnknownError")
        }
        return response.data.getString("result") ?: ""
    }

    private fun awaitBaselineDns(): String {
        val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(30)
        while (true) {
            try {
                return companionProbe("resolve", "example.com", 0)
            } catch (error: CompanionProbeException) {
                if (error.errorType != "UnknownHostException" || System.nanoTime() >= deadline) {
                    throw error
                }
                Log.i(TAG, "Waiting for the emulator's baseline DNS")
                Thread.sleep(1_000)
            }
        }
    }

    private class CompanionProbeException(val errorType: String) :
        IllegalStateException("companion probe failed: $errorType")

    private fun awaitVpnTransport(expected: Boolean, previousNetworkHandle: Long? = null) {
        val connectivity = targetContext.getSystemService(ConnectivityManager::class.java)
        repeat(40) {
            val network = connectivity.activeNetwork
            val isVpn = network != null && connectivity.getNetworkCapabilities(network)
                ?.hasTransport(NetworkCapabilities.TRANSPORT_VPN) == true
            if (isVpn == expected && (previousNetworkHandle == null || network?.networkHandle != previousNetworkHandle)) {
                return
            }
            Thread.sleep(250)
        }
        error("VPN transport state did not become $expected")
    }

    private fun validateLab(credentials: JSONObject) {
        assertEquals(PUBLIC_SERVER, credentials.getString("serverIpv4AddrIn"))
        assertEquals(51832, credentials.getInt("serverPort"))
        assertEquals("10.77.0.4", credentials.getString("deviceIpv4Address").substringBefore('/'))
        for (key in listOf("privateKey", "serverPublicKey", "Jc", "Jmin", "Jmax", "S1", "S2", "H1", "H2", "H3", "H4")) {
            assertTrue("isolated Android lab configuration is incomplete", credentials.has(key))
        }
    }

    private fun clientConfig(credentials: JSONObject, switchAction: String): JSONObject {
        val deviceAddress = credentials.getString("deviceIpv4Address").substringBefore('/') + "/32"
        val awg = JSONObject()
            .put("client_ip", deviceAddress)
            .put("hostName", credentials.getString("serverIpv4AddrIn"))
            .put("port", credentials.getInt("serverPort"))
            .put("client_priv_key", credentials.getString("privateKey"))
            .put("server_pub_key", credentials.getString("serverPublicKey"))
            .put("allowed_ips", JSONArray().put("0.0.0.0/0"))
            .put("mtu", credentials.optString("deviceMTU", "1376"))
            .put("persistent_keep_alive", credentials.optString("persistentKeepalive", "25"))
            .put("isObfuscationEnabled", true)
        if (credentials.optString("serverPskKey").isNotEmpty()) {
            awg.put("psk_key", credentials.getString("serverPskKey"))
        }
        for (key in TRANSPORT_KEYS) {
            if (credentials.has(key)) awg.put(key, credentials.get(key))
        }

        val routing = JSONObject()
            .put("version", 1)
            .put("defaultAction", "proxy")
            .put("bypass", "os")
            .put("rejectDirectIpv6", true)
            .put("rejectProxyIpv6", true)
            .put(
                "rules",
                JSONArray()
                    .put(JSONObject().put("action", "block").put("domains", JSONArray().put("full:example.com")))
                    .put(JSONObject().put("action", "direct").put("domains", JSONArray().put("full:direct.android.test")))
                    .put(JSONObject().put("action", switchAction).put("domains", JSONArray().put("full:switch.android.test"))),
            )
            .put(
                "dns",
                JSONObject()
                    .put("listen", JSONArray().put("198.18.0.53"))
                    .put("remote", JSONArray().put("1.1.1.1"))
                    .put("direct", JSONArray().put("8.8.8.8"))
                    .put(
                        "hosts",
                        JSONObject()
                            .put("proxy.android.test", JSONArray().put("10.77.0.1"))
                            .put("direct.android.test", JSONArray().put(PUBLIC_SERVER))
                            .put("switch.android.test", JSONArray().put(PUBLIC_SERVER)),
                    ),
            )

        return JSONObject()
            .put("protocol", "awg")
            .put("dns1", "198.18.0.53")
            .put("dns2", "198.18.0.53")
            .put("splitTunnelType", 0)
            .put("splitTunnelSites", JSONArray())
            .put("appSplitTunnelType", 2)
            .put("splitTunnelApps", JSONArray().put(COMPANION_PACKAGE))
            .put("awg_config_data", awg)
            .put("routing_config", routing)
    }

    private fun httpRequest(address: String, port: Int, host: String): String {
        Socket().use { socket ->
            socket.connect(InetSocketAddress(address, port), 10_000)
            socket.soTimeout = 10_000
            val output = socket.getOutputStream().bufferedWriter(Charsets.US_ASCII)
            output.write("GET / HTTP/1.0\r\nHost: $host\r\nConnection: close\r\n\r\n")
            output.flush()
            val response = socket.getInputStream().bufferedReader().readText()
            return response.substringAfter("\r\n\r\n").trim()
        }
    }

    private data class DnsAnswer(val rcode: Int, val addresses: List<String>)

    private fun requireSingleAddress(answer: DnsAnswer, expected: String): String {
        assertEquals(0, answer.rcode)
        assertEquals(listOf(expected), answer.addresses)
        return answer.addresses.single()
    }

    private fun dnsQueryA(name: String): DnsAnswer {
        val id = SecureRandom().nextInt(0x10000)
        val query = ByteArrayOutputStream().apply {
            write(byteArrayOf((id ushr 8).toByte(), id.toByte(), 1, 0, 0, 1, 0, 0, 0, 0, 0, 0))
            for (label in name.split('.')) {
                val bytes = label.toByteArray(Charsets.US_ASCII)
                write(bytes.size)
                write(bytes)
            }
            write(byteArrayOf(0, 0, 1, 0, 1))
        }.toByteArray()
        val response = ByteArray(4096)
        DatagramSocket().use { socket ->
            socket.soTimeout = 10_000
            socket.send(DatagramPacket(query, query.size, InetAddress.getByName("198.18.0.53"), 53))
            val packet = DatagramPacket(response, response.size)
            socket.receive(packet)
            return parseDnsAnswer(response.copyOf(packet.length), id)
        }
    }

    private fun parseDnsAnswer(packet: ByteArray, expectedId: Int): DnsAnswer {
        require(packet.size >= 12 && u16(packet, 0) == expectedId && (u16(packet, 2) and 0x8000) != 0) {
            "invalid DNS response"
        }
        val rcode = u16(packet, 2) and 0x0f
        var offset = 12
        repeat(u16(packet, 4)) {
            offset = skipDnsName(packet, offset) + 4
        }
        val addresses = mutableListOf<String>()
        repeat(u16(packet, 6)) {
            offset = skipDnsName(packet, offset)
            require(offset + 10 <= packet.size) { "truncated DNS answer" }
            val type = u16(packet, offset)
            val recordClass = u16(packet, offset + 2)
            val length = u16(packet, offset + 8)
            offset += 10
            require(offset + length <= packet.size) { "truncated DNS record" }
            if (type == 1 && recordClass == 1 && length == 4) {
                addresses += InetAddress.getByAddress(packet.copyOfRange(offset, offset + 4)).hostAddress ?: ""
            }
            offset += length
        }
        return DnsAnswer(rcode, addresses)
    }

    private fun skipDnsName(packet: ByteArray, start: Int): Int {
        var offset = start
        while (true) {
            require(offset < packet.size) { "truncated DNS name" }
            val length = packet[offset].toInt() and 0xff
            if ((length and 0xc0) == 0xc0) return offset + 2
            offset++
            if (length == 0) return offset
            offset += length
        }
    }

    private fun u16(packet: ByteArray, offset: Int): Int =
        ((packet[offset].toInt() and 0xff) shl 8) or (packet[offset + 1].toInt() and 0xff)

    companion object {
        private const val TARGET_PACKAGE = "org.amnezia.vpn"
        private const val TAG = "RouterE2E"
        private const val TARGET_SERVICE = "org.amnezia.vpn.AwgService"
        private const val COMPANION_PACKAGE = "org.amnezia.routere2e.companion"
        private const val COMPANION_SERVICE = "$COMPANION_PACKAGE.ProbeService"
        private const val PUBLIC_SERVER = "82.40.38.137"
        private const val PUBLIC_HTTP_PORT = 31882

        private const val ACTION_REGISTER = 0
        private const val ACTION_UNREGISTER = 1
        private const val ACTION_CONNECT = 2
        private const val ACTION_DISCONNECT = 3
        private const val ACTION_STATUS = 4
        private const val EVENT_STATUS_CHANGED = 0
        private const val EVENT_STATUS = 1
        private const val EVENT_ERROR = 3
        private const val STATE_DISCONNECTED = 0
        private const val STATE_CONNECTED = 1

        private val TRANSPORT_KEYS = listOf(
            "Jc", "Jmin", "Jmax", "S1", "S2", "S3", "S4", "H1", "H2", "H3", "H4",
            "I1", "I2", "I3", "I4", "I5", "HeaderProtectionKey", "ContentPaddingAddition",
            "RekeyAfterTime", "RekeyTimeout", "RejectAfterTime", "KeepaliveTimeout",
            "MaxHandshakeAttempts", "RandomTrailers", "DisableCookies",
        )
    }
}
