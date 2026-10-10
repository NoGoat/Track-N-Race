package com.tracknrace.android

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.Base64
import java.security.KeyStore
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

/**
 * The paired desktop: where it was last seen, its pinned identity key and this
 * phone's reconnect token. The token is encrypted with an Android Keystore key
 * that never leaves the device, so a backup or copy of the preferences cannot
 * be used to impersonate this phone.
 */
internal object PairingStore {
    data class SavedDesktop(
        val serverId: String,
        val name: String,
        val host: String,
        val port: Int,
        val identityKey: String,
        val token: String,
    )

    private const val KEY_ALIAS = "tnr_pairing_token"
    private const val GCM_TAG_BITS = 128

    private const val PREF_SERVER_ID = "pairing.server_id"
    private const val PREF_SERVER_NAME = "pairing.server_name"
    private const val PREF_HOST = "pairing.host"
    private const val PREF_PORT = "pairing.port"
    private const val PREF_IDENTITY_KEY = "pairing.identity_key"
    private const val PREF_SEALED_TOKEN = "pairing.token_sealed"
    // Protocol 2 kept the token in plain text and pinned no desktop key.
    private const val PREF_LEGACY_TOKEN = "pairing.token"

    @Volatile
    private var cached: SavedDesktop? = null

    @Volatile
    private var loaded = false

    private fun preferences(context: Context) = RecordingStorage.preferences(context)

    @Synchronized
    fun load(context: Context): SavedDesktop? {
        if (loaded) return cached
        val preferences = preferences(context)
        if (preferences.contains(PREF_LEGACY_TOKEN)) {
            // That pairing cannot be resumed securely: pair again.
            preferences.edit()
                .remove(PREF_LEGACY_TOKEN)
                .putString(PairedTelemetryClient.PREF_SOURCE, PairedTelemetryClient.SOURCE_DIRECT)
                .apply()
        }
        val sealed = preferences.getString(PREF_SEALED_TOKEN, "").orEmpty()
        val identityKey = preferences.getString(PREF_IDENTITY_KEY, "").orEmpty()
        val serverId = preferences.getString(PREF_SERVER_ID, "").orEmpty()
        val token = if (sealed.isNotEmpty()) unseal(sealed) else null
        cached = if (token != null && identityKey.isNotEmpty() && serverId.isNotEmpty()) {
            SavedDesktop(
                serverId = serverId,
                name = preferences.getString(PREF_SERVER_NAME, "Desktop") ?: "Desktop",
                host = preferences.getString(PREF_HOST, "").orEmpty(),
                port = preferences.getInt(PREF_PORT, 20779),
                identityKey = identityKey,
                token = token,
            )
        } else {
            // A restored backup has the ciphertext but not the Keystore key.
            if (sealed.isNotEmpty()) {
                clearPreferences(context)
                preferences.edit()
                    .putString(PairedTelemetryClient.PREF_SOURCE, PairedTelemetryClient.SOURCE_DIRECT)
                    .apply()
            }
            null
        }
        loaded = true
        return cached
    }

    @Synchronized
    fun save(context: Context, desktop: SavedDesktop) {
        val sealed = seal(desktop.token)
        preferences(context).edit()
            .putString(PREF_SERVER_ID, desktop.serverId)
            .putString(PREF_SERVER_NAME, desktop.name)
            .putString(PREF_HOST, desktop.host)
            .putInt(PREF_PORT, desktop.port)
            .putString(PREF_IDENTITY_KEY, desktop.identityKey)
            .putString(PREF_SEALED_TOKEN, sealed)
            .apply()
        cached = desktop
        loaded = true
    }

    /** The desktop moved (new IP or port); its identity is unchanged. */
    @Synchronized
    fun updateEndpoint(context: Context, host: String, port: Int) {
        val current = load(context) ?: return
        if (current.host == host && current.port == port) return
        preferences(context).edit()
            .putString(PREF_HOST, host)
            .putInt(PREF_PORT, port)
            .apply()
        cached = current.copy(host = host, port = port)
    }

    @Synchronized
    fun updateName(context: Context, name: String) {
        val current = load(context) ?: return
        if (name.isEmpty() || current.name == name) return
        preferences(context).edit().putString(PREF_SERVER_NAME, name).apply()
        cached = current.copy(name = name)
    }

    @Synchronized
    fun clear(context: Context) {
        clearPreferences(context)
        cached = null
        loaded = true
    }

    private fun clearPreferences(context: Context) {
        preferences(context).edit()
            .remove(PREF_SERVER_ID)
            .remove(PREF_SERVER_NAME)
            .remove(PREF_HOST)
            .remove(PREF_PORT)
            .remove(PREF_IDENTITY_KEY)
            .remove(PREF_SEALED_TOKEN)
            .remove(PREF_LEGACY_TOKEN)
            .apply()
    }

    private fun keystoreKey(): SecretKey {
        val keyStore = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        (keyStore.getKey(KEY_ALIAS, null) as? SecretKey)?.let { return it }
        val generator = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore")
        generator.init(
            KeyGenParameterSpec.Builder(
                KEY_ALIAS,
                KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT,
            )
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setKeySize(256)
                .build(),
        )
        return generator.generateKey()
    }

    private fun seal(value: String): String {
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.ENCRYPT_MODE, keystoreKey())
        val iv = cipher.iv
        val ciphertext = cipher.doFinal(value.toByteArray(Charsets.UTF_8))
        return Base64.encodeToString(byteArrayOf(iv.size.toByte()) + iv + ciphertext, Base64.NO_WRAP)
    }

    private fun unseal(value: String): String? = try {
        val bytes = Base64.decode(value, Base64.NO_WRAP)
        val ivLength = bytes[0].toInt()
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(
            Cipher.DECRYPT_MODE,
            keystoreKey(),
            GCMParameterSpec(GCM_TAG_BITS, bytes, 1, ivLength),
        )
        String(cipher.doFinal(bytes, 1 + ivLength, bytes.size - 1 - ivLength), Charsets.UTF_8)
    } catch (error: Exception) {
        null
    }
}
