package com.tracknrace.android

import android.os.Bundle
import android.view.View
import android.widget.FrameLayout
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import com.google.android.material.appbar.MaterialToolbar
import com.google.android.material.color.DynamicColors
import com.journeyapps.barcodescanner.CaptureActivity
import com.journeyapps.barcodescanner.DecoratedBarcodeView

/** QR scanner host locked to portrait independently of the dashboard orientation. */
class PortraitCaptureActivity : CaptureActivity() {
    override fun initializeContent(): DecoratedBarcodeView {
        DynamicColors.applyToActivityIfAvailable(this)
        setContentView(R.layout.activity_qr_scanner)
        return findViewById(R.id.zxing_barcode_scanner)
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        findViewById<MaterialToolbar>(R.id.qr_scanner_app_bar).setNavigationOnClickListener { finish() }
        applySafetyInsets()
    }

    private fun applySafetyInsets() {
        val root = findViewById<View>(R.id.qr_scanner_root)
        val topScrim = findViewById<View>(R.id.qr_scanner_top_scrim)
        val appBar = findViewById<View>(R.id.qr_scanner_app_bar)
        val guideCard = findViewById<View>(R.id.qr_scanner_guide_card)
        val density = resources.displayMetrics.density
        val appBarHeight = (64 * density).toInt()
        val scrimHeight = (136 * density).toInt()
        val horizontalMargin = (16 * density).toInt()
        val bottomMargin = (20 * density).toInt()

        ViewCompat.setOnApplyWindowInsetsListener(root) { _, insets ->
            val systemBars = insets.getInsets(WindowInsetsCompat.Type.systemBars())
            val cutout = insets.getInsets(WindowInsetsCompat.Type.displayCutout())
            val safeLeft = maxOf(systemBars.left, cutout.left)
            val safeTop = maxOf(systemBars.top, cutout.top)
            val safeRight = maxOf(systemBars.right, cutout.right)
            val safeBottom = maxOf(systemBars.bottom, cutout.bottom)

            appBar.layoutParams = appBar.layoutParams.apply {
                height = appBarHeight + safeTop
            }
            appBar.setPaddingRelative(safeLeft, safeTop, safeRight, 0)
            topScrim.layoutParams = topScrim.layoutParams.apply {
                height = scrimHeight + safeTop
            }
            guideCard.layoutParams = (guideCard.layoutParams as FrameLayout.LayoutParams).apply {
                marginStart = horizontalMargin + safeLeft
                marginEnd = horizontalMargin + safeRight
                this.bottomMargin = bottomMargin + safeBottom
            }

            insets
        }
        ViewCompat.requestApplyInsets(root)
    }
}
