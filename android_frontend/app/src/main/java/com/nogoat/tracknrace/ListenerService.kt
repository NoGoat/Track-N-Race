package com.nogoat.tracknrace

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder

/**
 * The persistent listener: a started foreground service that carries the
 * telemetry session while the app itself is not on screen. The session
 * cannot be dropped when the app goes to the background — the game keeps
 * sending while a session runs, and a rebound listener loses the laps from
 * before it rejoined, resetting the charts' All Laps history — so
 * TelemetryController starts this from onHostStop instead of suspending its
 * sources. Stopping it suspends the session, exactly as the app's old
 * background behaviour did.
 *
 * The notification is the user's contract: the service keeps everything
 * running underneath it until they tap Stop, swipe the app's task away from
 * recents or force-quit the app.
 * The state text follows the store's source state; discovery stays a
 * foreground feature and is never carried here.
 */
internal class ListenerService : Service() {
    private val controller by lazy { TelemetryHost.controller(this) }
    private val store by lazy { TelemetryHost.store(this) }
    private var lastText: String? = null

    override fun onCreate() {
        super.onCreate()
        val manager = getSystemService(NOTIFICATION_SERVICE) as NotificationManager
        manager.createNotificationChannel(
            NotificationChannel(
                CHANNEL_ID,
                getString(R.string.listener_channel_name),
                // Low: silent and no heads-up; the shadow entry is the point.
                NotificationManager.IMPORTANCE_LOW,
            ).apply { description = getString(R.string.listener_channel_description) },
        )
        // Declared foreground right after startForegroundService(); the
        // onStartCommand work happens after this.
        startForegroundCompat(buildNotification(store.sourceStatus))
        store.setSourceStatusListener { notify(it) }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP) {
            // onDestroy suspends the sources, or hands them back to a host.
            stopSelf()
            return START_NOT_STICKY
        }
        controller.onListenerServiceStarted()
        notify(store.sourceStatus)
        return START_STICKY
    }

    /**
     * The app was swiped away from recents: that closes it, listener
     * included. Stopped here, the sticky service is not scheduled again.
     */
    override fun onTaskRemoved(rootIntent: Intent?) {
        controller.onTaskRemoved()
        stopSelf()
        super.onTaskRemoved(rootIntent)
    }

    override fun onDestroy() {
        store.setSourceStatusListener(null)
        controller.onListenerServiceStopped()
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    private fun startForegroundCompat(notification: Notification) {
        // The manifest declares FOREGROUND_SERVICE_SPECIAL_USE and the
        // specialUse type: a live UDP session fits no typed bucket (dataSync
        // is capped at six hours a day, connectedDevice wants a companion
        // association), and specialUse carries the documented subtype string
        // instead.
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            startForeground(
                NOTIFICATION_ID,
                notification,
                ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE,
            )
        } else {
            startForeground(NOTIFICATION_ID, notification)
        }
    }

    private fun buildNotification(status: SourceStatus): Notification {
        val openApp = PendingIntent.getActivity(
            this,
            0,
            Intent(this, MainActivity::class.java).setFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
            PendingIntent.FLAG_IMMUTABLE,
        )
        val stop = PendingIntent.getService(
            this,
            1,
            Intent(this, ListenerService::class.java).setAction(ACTION_STOP),
            PendingIntent.FLAG_IMMUTABLE,
        )
        val recording = RecordingStorage.preferences(this)
            .getBoolean(RecordingStorage.PREF_RECORDING, false)
        val text = text(status, recording)
        return Notification.Builder(this, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_sensors)
            .setContentTitle(getString(R.string.listener_notification_title))
            .setContentText(text)
            .setContentIntent(openApp)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setCategory(Notification.CATEGORY_SERVICE)
            .addAction(R.drawable.ic_close, getString(R.string.listener_notification_stop), stop)
            .apply { if (recording) setSubText(getString(R.string.listener_notification_recording)) }
            .build()
            .also { lastText = text }
    }

    /** The same vocabulary the dashboard's menu uses for the source. */
    private fun text(status: SourceStatus, recording: Boolean): String = when (status.state) {
        "listening" -> status.detail ?: getString(R.string.listener_state_listening)
        "connected", "paired", "awaiting_approval" ->
            status.detail ?: getString(R.string.listener_state_desktop)
        "connecting" ->
            if (status.detail.isNullOrBlank()) getString(R.string.listener_state_desktop) else getString(R.string.listener_state_connecting, status.detail)
        "reconnecting", "disconnected" ->
            status.detail ?: getString(R.string.listener_state_reconnecting)
        "error" -> status.detail ?: getString(R.string.listener_state_error)
        else -> status.detail ?: getString(R.string.listener_state_listening)
    } + (if (recording) getString(R.string.listener_notification_recording_suffix) else "")

    private fun notify(status: SourceStatus) {
        val text = text(status, RecordingStorage.preferences(this)
            .getBoolean(RecordingStorage.PREF_RECORDING, false))
        if (text == lastText) return
        lastText = text
        (getSystemService(NOTIFICATION_SERVICE) as NotificationManager)
            .notify(NOTIFICATION_ID, buildNotification(status))
    }

    companion object {
        const val ACTION_STOP = "com.nogoat.tracknrace.LISTENER_STOP"
        private const val CHANNEL_ID = "listener"
        private const val NOTIFICATION_ID = 1
    }
}
