package com.nogoat.tracknrace

import android.content.Context

/**
 * The process holds one telemetry session, whoever asks for it: MainActivity
 * while it is on screen, ListenerService once it is not. A native engine can
 * outlive both — libtnrp runs its own receive thread, and its sink holds a
 * global reference to the store only while a session exists — so the
 * controller has to live as long as the process instead of dying with an
 * activity. Rebinding the session on every foreground return would drop
 * what a background session streamed and reset the charts' All Laps history.
 */
internal object TelemetryHost {
    @Volatile private var session: Pair<TelemetryStore, TelemetryController>? = null

    /** The store, created with its controller on first use. */
    private fun holder(context: Context): Pair<TelemetryStore, TelemetryController> {
        session?.let { return it }
        synchronized(this) {
            session?.let { return it }
            val store = TelemetryStore()
            val controller = TelemetryController(context.applicationContext, store)
            session = store to controller
            return store to controller
        }
    }

    fun store(context: Context): TelemetryStore = holder(context).first

    fun controller(context: Context): TelemetryController = holder(context).second
}
