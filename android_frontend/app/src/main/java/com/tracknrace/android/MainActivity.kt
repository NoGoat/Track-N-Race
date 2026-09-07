package com.tracknrace.android

import android.content.res.Configuration
import android.os.Build
import android.os.Bundle
import android.view.View
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import com.tracknrace.android.ui.theme.TrackNRaceTheme

class MainActivity : ComponentActivity() {
    private lateinit var telemetry: TelemetryController

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        telemetry = TelemetryController(this, TelemetryStore())
        updateSystemBars()
        setContent {
            TrackNRaceTheme {
                TrackNRaceApp(telemetry)
            }
        }
    }

    override fun onStart() {
        super.onStart()
        telemetry.onHostStart()
    }

    override fun onStop() {
        telemetry.onHostStop()
        super.onStop()
    }

    override fun onDestroy() {
        telemetry.destroy()
        super.onDestroy()
    }

    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        updateSystemBars()
    }

    override fun onResume() {
        super.onResume()
        updateSystemBars()
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) updateSystemBars()
    }

    /** Landscape is an immersive steering-wheel display; portrait uses normal system chrome. */
    @Suppress("DEPRECATION")
    private fun updateSystemBars() {
        val landscape = resources.configuration.orientation == Configuration.ORIENTATION_LANDSCAPE
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            window.attributes = window.attributes.apply {
                layoutInDisplayCutoutMode = if (landscape) {
                    WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
                } else {
                    WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_DEFAULT
                }
            }
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            window.setDecorFitsSystemWindows(!landscape)
            window.insetsController?.let { controller ->
                val bars = WindowInsets.Type.statusBars() or WindowInsets.Type.navigationBars()
                if (landscape) {
                    controller.hide(bars)
                    controller.systemBarsBehavior =
                        WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
                } else {
                    controller.show(bars)
                }
            }
        } else {
            window.decorView.systemUiVisibility = if (landscape) {
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY or
                    View.SYSTEM_UI_FLAG_FULLSCREEN or
                    View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or
                    View.SYSTEM_UI_FLAG_LAYOUT_STABLE or
                    View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN or
                    View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
            } else {
                View.SYSTEM_UI_FLAG_VISIBLE
            }
        }
    }
}
