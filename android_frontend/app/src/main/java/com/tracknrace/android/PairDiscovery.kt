package com.tracknrace.android

import android.content.Context
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.os.Build
import java.net.Inet4Address
import java.net.InetAddress

/**
 * Finds desktops advertising `_tracknrace-pair._tcp` over DNS-SD (Android's
 * system mDNS stack). The TXT record carries only the protocol version, the
 * desktop's server id and whether pairing is open; identity is proven by the
 * handshake, never by discovery.
 */
internal class PairDiscovery(
    context: Context,
    private val listener: Listener,
) {
    interface Listener {
        fun onService(service: Service)

        fun onDiscoveryError(message: String) = Unit
    }

    data class Service(
        val serverId: String,
        val name: String,
        val address: String,
        val port: Int,
        val pairing: Boolean,
    )

    private companion object {
        const val SERVICE_TYPE = "_tracknrace-pair._tcp"
        const val PROTOCOL_VERSION = "3"
    }

    private val nsd = context.applicationContext.getSystemService(NsdManager::class.java)
    private var discovery: NsdManager.DiscoveryListener? = null

    // Before API 34 NsdManager resolves one service at a time.
    private val pending = ArrayDeque<NsdServiceInfo>()
    private var resolving = false

    @Synchronized
    fun start() {
        if (discovery != null) return
        val manager = nsd ?: run {
            listener.onDiscoveryError("Network service discovery is unavailable")
            return
        }
        val callback = object : NsdManager.DiscoveryListener {
            override fun onDiscoveryStarted(serviceType: String) = Unit

            override fun onDiscoveryStopped(serviceType: String) = Unit

            override fun onStartDiscoveryFailed(serviceType: String, errorCode: Int) {
                synchronized(this@PairDiscovery) {
                    if (discovery === this) discovery = null
                }
                listener.onDiscoveryError("Discovery failed ($errorCode)")
            }

            override fun onStopDiscoveryFailed(serviceType: String, errorCode: Int) = Unit

            override fun onServiceFound(serviceInfo: NsdServiceInfo) {
                synchronized(this@PairDiscovery) {
                    if (discovery !== this) return
                    pending.addLast(serviceInfo)
                    resolveNextLocked()
                }
            }

            override fun onServiceLost(serviceInfo: NsdServiceInfo) = Unit
        }
        discovery = callback
        try {
            manager.discoverServices(SERVICE_TYPE, NsdManager.PROTOCOL_DNS_SD, callback)
        } catch (error: RuntimeException) {
            discovery = null
            listener.onDiscoveryError(error.message ?: "Discovery failed")
        }
    }

    @Synchronized
    fun stop() {
        val callback = discovery ?: return
        discovery = null
        pending.clear()
        try {
            nsd?.stopServiceDiscovery(callback)
        } catch (_: RuntimeException) {
            // Already stopped by the system.
        }
    }

    @Suppress("DEPRECATION") // resolveService is the only resolver below API 34.
    private fun resolveNextLocked() {
        if (resolving) return
        val next = pending.removeFirstOrNull() ?: return
        val manager = nsd ?: return
        resolving = true
        val owner = discovery
        manager.resolveService(next, object : NsdManager.ResolveListener {
            override fun onResolveFailed(serviceInfo: NsdServiceInfo, errorCode: Int) {
                finished(owner, null)
            }

            override fun onServiceResolved(serviceInfo: NsdServiceInfo) {
                finished(owner, serviceInfo)
            }
        })
    }

    private fun finished(owner: NsdManager.DiscoveryListener?, info: NsdServiceInfo?) {
        val service = info?.let(::toService)
        synchronized(this) {
            resolving = false
            if (discovery !== owner || owner == null) return
            resolveNextLocked()
        }
        service?.let(listener::onService)
    }

    private fun toService(info: NsdServiceInfo): Service? {
        val attributes = info.attributes
        fun text(key: String) = attributes[key]?.toString(Charsets.UTF_8).orEmpty()
        if (text("v") != PROTOCOL_VERSION) return null
        val serverId = text("id").takeIf(String::isNotEmpty) ?: return null
        val address = ipv4Of(info) ?: return null
        if (info.port !in 1..65535) return null
        return Service(serverId, info.serviceName, address, info.port, text("pair") == "1")
    }

    @Suppress("DEPRECATION") // NsdServiceInfo.host before API 34.
    private fun ipv4Of(info: NsdServiceInfo): String? {
        val addresses: List<InetAddress> = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            info.hostAddresses
        } else {
            listOfNotNull(info.host)
        }
        return addresses.firstOrNull { it is Inet4Address }?.hostAddress
    }
}
