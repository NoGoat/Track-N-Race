package com.nogoat.tracknrace

import android.content.Context
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.os.Build
import android.os.Handler
import android.os.Looper
import androidx.annotation.RequiresApi
import java.net.Inet4Address
import java.net.InetAddress
import java.util.concurrent.Executor

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
        const val RESOLVE_TIMEOUT_MS = 5_000L
        const val RETRY_DELAY_MS = 1_000L
    }

    private val nsd = context.applicationContext.getSystemService(NsdManager::class.java)
    private val main = Handler(Looper.getMainLooper())
    private var discovery: NsdManager.DiscoveryListener? = null

    // API 34+: one live info callback per found service, so a service that never
    // answers (a desktop that left without a goodbye) can't hold up the others.
    // One that hasn't answered within the timeout is forgotten.
    private val watching = HashMap<String, NsdManager.ServiceInfoCallback>()
    private val watchTimeouts = HashMap<String, Runnable>()

    // Before API 34 NsdManager resolves one service at a time, and a resolve for
    // a service that never answers may never call back, so each one is timed out.
    private val pending = ArrayDeque<NsdServiceInfo>()
    private var resolve: Any? = null
    private var timeout: Runnable? = null

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
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
                        watchLocked(manager, serviceInfo)
                    } else {
                        pending.addLast(serviceInfo)
                        resolveNextLocked()
                    }
                }
            }

            override fun onServiceLost(serviceInfo: NsdServiceInfo) {
                synchronized(this@PairDiscovery) {
                    if (discovery !== this) return
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
                        forgetLocked(manager, serviceInfo.serviceName)
                    } else {
                        pending.removeAll { it.serviceName == serviceInfo.serviceName }
                    }
                }
            }
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
        // A resolve still in flight finishes into nothing; the next start() doesn't wait on it.
        resolve = null
        timeout?.let(main::removeCallbacks)
        timeout = null
        nsd?.let { manager ->
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
                watching.keys.toList().forEach { forgetLocked(manager, it) }
            }
            try {
                manager.stopServiceDiscovery(callback)
            } catch (_: RuntimeException) {
                // Already stopped by the system.
            }
        }
    }

    @RequiresApi(Build.VERSION_CODES.UPSIDE_DOWN_CAKE)
    private fun watchLocked(manager: NsdManager, info: NsdServiceInfo) {
        if (info.serviceName in watching) return
        val callback = object : NsdManager.ServiceInfoCallback {
            override fun onServiceInfoCallbackRegistrationFailed(errorCode: Int) {
                synchronized(this@PairDiscovery) {
                    if (watching[info.serviceName] === this) forgetLocked(manager, info.serviceName)
                }
            }

            override fun onServiceUpdated(serviceInfo: NsdServiceInfo) {
                synchronized(this@PairDiscovery) {
                    if (watching[info.serviceName] !== this) return
                    // It answered; keep watching it for address changes.
                    watchTimeouts.remove(info.serviceName)?.let(main::removeCallbacks)
                }
                toService(serviceInfo)?.let(listener::onService)
            }

            override fun onServiceLost() = Unit

            override fun onServiceInfoCallbackUnregistered() = Unit
        }
        try {
            manager.registerServiceInfoCallback(info, Executor { main.post(it) }, callback)
            watching[info.serviceName] = callback
            val timeout = Runnable {
                synchronized(this) {
                    if (watching[info.serviceName] === callback) forgetLocked(manager, info.serviceName)
                }
            }
            watchTimeouts[info.serviceName] = timeout
            main.postDelayed(timeout, RESOLVE_TIMEOUT_MS)
        } catch (_: RuntimeException) {
            // Already watched or rejected; discovery reports it again if it returns.
        }
    }

    /** Stops watching a service; discovery reporting it again starts afresh. */
    @RequiresApi(Build.VERSION_CODES.UPSIDE_DOWN_CAKE)
    private fun forgetLocked(manager: NsdManager, serviceName: String) {
        watchTimeouts.remove(serviceName)?.let(main::removeCallbacks)
        val callback = watching.remove(serviceName) ?: return
        try {
            manager.unregisterServiceInfoCallback(callback)
        } catch (_: RuntimeException) {
            // Registration had already failed or ended.
        }
    }

    @Suppress("DEPRECATION") // resolveService is the only resolver below API 34.
    private fun resolveNextLocked() {
        if (resolve != null) return
        val next = pending.removeFirstOrNull() ?: return
        val manager = nsd ?: return
        val token = Any()
        resolve = token
        timeout = Runnable { finished(token, null) }.also { main.postDelayed(it, RESOLVE_TIMEOUT_MS) }
        try {
            manager.resolveService(next, object : NsdManager.ResolveListener {
                override fun onResolveFailed(serviceInfo: NsdServiceInfo, errorCode: Int) {
                    if (errorCode == NsdManager.FAILURE_ALREADY_ACTIVE) {
                        // A timed-out resolve is still running in the system; try again after it.
                        synchronized(this@PairDiscovery) {
                            if (resolve !== token) return
                            pending.addLast(next)
                        }
                    }
                    finished(token, null)
                }

                override fun onServiceResolved(serviceInfo: NsdServiceInfo) {
                    finished(token, serviceInfo)
                }
            })
        } catch (_: RuntimeException) {
            main.post { finished(token, null) }
        }
    }

    private fun finished(token: Any, info: NsdServiceInfo?) {
        synchronized(this) {
            // Already timed out, superseded, or stopped.
            if (resolve !== token) return
            resolve = null
            timeout?.let(main::removeCallbacks)
            timeout = null
            if (discovery == null) return
            if (pending.isNotEmpty() && info == null) {
                // Space retries out instead of spinning against a busy resolver.
                main.postDelayed({ synchronized(this) { resolveNextLocked() } }, RETRY_DELAY_MS)
            } else {
                resolveNextLocked()
            }
        }
        info?.let(::toService)?.let(listener::onService)
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
