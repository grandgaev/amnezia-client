package org.amnezia.awg

/**
 * Excludes a socket from the tunnel (VpnService.protect).
 * Called by libwg-go from arbitrary native threads.
 */
fun interface SocketProtector {
    fun protect(fd: Int): Boolean
}

object GoBackend {
    external fun awgGetConfig(handle: Int): String?
    external fun awgGetSocketV4(handle: Int): Int
    external fun awgGetSocketV6(handle: Int): Int
    external fun awgTurnOff(handle: Int)
    external fun awgTurnOn(ifName: String, tunFd: Int, settings: String): Int
    external fun awgVersion(): String

    /**
     * Sets the protector for the sockets that the routing profiles router opens for "direct"
     * traffic, null removes it. The native side keeps a global reference to the protector.
     */
    external fun awgSetSocketProtector(protector: SocketProtector?)
}
