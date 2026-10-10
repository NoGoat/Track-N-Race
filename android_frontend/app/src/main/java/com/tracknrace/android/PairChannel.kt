package com.tracknrace.android

/**
 * The encrypted channel to a paired desktop. The handshake and frame
 * encryption run in libtnrp (PairCrypto), the same code as the desktop, so
 * the two ends cannot drift apart. Calls are serialised on this object.
 */
internal class PairChannel private constructor() : AutoCloseable {
    enum class Mode(val native: Int) { RESUME(0), QR(1), CODE(2) }

    companion object {
        const val KIND_TEXT = 1
        const val KIND_BINARY = 2

        init {
            System.loadLibrary("tracknrace_android")
        }

        /**
         * Starts a handshake. [identityKey] is the pinned desktop key (resume)
         * or the key from the QR; [serverId] and [code] are for code pairing.
         */
        fun begin(
            mode: Mode,
            serverId: String,
            identityKey: String,
            code: String,
        ): Result<PairChannel> {
            val channel = PairChannel()
            val error = channel.nativeBegin(channel.handle, mode.native, serverId, identityKey, code)
            if (error != null) {
                channel.close()
                return Result.failure(IllegalArgumentException(error))
            }
            return Result.success(channel)
        }
    }

    private var handle: Long = nativeNew()

    @Synchronized
    fun helloJson(): String = if (handle != 0L) nativeHello(handle) else ""

    /** Verifies the desktop's server_hello; returns an error code, or null. */
    @Synchronized
    fun accept(serverHello: String): String? =
        if (handle != 0L) nativeAccept(handle, serverHello) else "invalid_server_hello"

    @Synchronized
    fun identityKey(): String = if (handle != 0L) nativeIdentityKey(handle) else ""

    @Synchronized
    fun serverId(): String = if (handle != 0L) nativeServerId(handle) else ""

    /** Code pairing: proof this phone knew the code. Empty for other modes. */
    @Synchronized
    fun confirmation(): String = if (handle != 0L) nativeConfirmation(handle) else ""

    @Synchronized
    fun seal(text: String): ByteArray? =
        if (handle != 0L) nativeSeal(handle, text.toByteArray(Charsets.UTF_8)) else null

    /** The payload of a sealed frame, with its kind in kindOut[0]; null if forged. */
    @Synchronized
    fun open(frame: ByteArray, kindOut: IntArray): ByteArray? =
        if (handle != 0L) nativeOpen(handle, frame, kindOut) else null

    @Synchronized
    override fun close() {
        if (handle == 0L) return
        nativeFree(handle)
        handle = 0L
    }

    private external fun nativeNew(): Long
    private external fun nativeFree(handle: Long)
    private external fun nativeBegin(
        handle: Long,
        mode: Int,
        serverId: String,
        identityKey: String,
        code: String,
    ): String?
    private external fun nativeHello(handle: Long): String
    private external fun nativeAccept(handle: Long, serverHello: String): String?
    private external fun nativeIdentityKey(handle: Long): String
    private external fun nativeServerId(handle: Long): String
    private external fun nativeConfirmation(handle: Long): String
    private external fun nativeSeal(handle: Long, text: ByteArray): ByteArray?
    private external fun nativeOpen(handle: Long, frame: ByteArray, kindOut: IntArray): ByteArray?
}
