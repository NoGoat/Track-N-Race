package com.tracknrace.android

internal class NativePairDiscovery(
    private val listener: Listener,
) {
    interface Listener {
        fun onService(service: Service)
    }

    data class Service(
        val serverId: String,
        val name: String,
        val address: String,
        val port: Int,
        val pairing: Boolean,
    )

    companion object {
        init {
            System.loadLibrary("tracknrace_android")
        }
    }

    fun start(): String? = nativeStart()

    fun stop() = nativeStop()

    @Suppress("unused") // Invoked by JNI on the discovery worker thread.
    private fun onNativeService(
        serverId: String,
        name: String,
        address: String,
        port: Int,
        pairing: Boolean,
    ) {
        listener.onService(Service(serverId, name, address, port, pairing))
    }

    private external fun nativeStart(): String?

    private external fun nativeStop()
}
