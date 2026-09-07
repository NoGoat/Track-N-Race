package com.tracknrace.android

internal class NativeTelemetry(
    private val listener: Listener,
) {
    interface Listener {
        fun onTelemetryRow(json: String)

        fun onTelemetryBinary(bytes: ByteArray)
    }

    companion object {
        init {
            System.loadLibrary("tracknrace_android")
        }
    }

    fun start(port: Int): String? = nativeStart(port)

    fun stop() = nativeStop()

    fun setRecording(enabled: Boolean, outputDirectory: String) {
        nativeSetRecording(enabled, outputDirectory)
    }

    @Suppress("unused") // Invoked by JNI on libtnrp worker threads.
    private fun onNativeRow(json: String) {
        listener.onTelemetryRow(json)
    }

    @Suppress("unused") // Invoked by JNI on libtnrp worker threads.
    private fun onNativeBinary(bytes: ByteArray) {
        listener.onTelemetryBinary(bytes)
    }

    private external fun nativeStart(port: Int): String?

    private external fun nativeStop()

    private external fun nativeSetRecording(enabled: Boolean, outputDirectory: String)
}
