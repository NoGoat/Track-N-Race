package com.tracknrace.android

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RectF
import android.util.AttributeSet
import android.util.TypedValue
import com.journeyapps.barcodescanner.ViewfinderView

/** Branded QR framing overlay; camera preview and decoding remain owned by ZXing. */
class QrScannerOverlayView(
    context: Context,
    attrs: AttributeSet,
) : ViewfinderView(context, attrs) {
    private val density = resources.displayMetrics.density
    private val accentColor = TypedValue().let { value ->
        if (context.theme.resolveAttribute(androidx.appcompat.R.attr.colorPrimary, value, true)) {
            if (value.resourceId != 0) context.getColor(value.resourceId) else value.data
        } else {
            context.getColor(R.color.dashboard_accent)
        }
    }
    private val cornerPath = Path()
    private val frameBounds = RectF()

    override fun onDraw(canvas: Canvas) {
        refreshSizes()
        val frame = framingRect ?: return

        paint.reset()
        paint.isAntiAlias = true
        paint.style = Paint.Style.FILL
        paint.color = maskColor
        canvas.drawRect(0f, 0f, width.toFloat(), frame.top.toFloat(), paint)
        canvas.drawRect(0f, frame.top.toFloat(), frame.left.toFloat(), frame.bottom.toFloat(), paint)
        canvas.drawRect(frame.right.toFloat(), frame.top.toFloat(), width.toFloat(), frame.bottom.toFloat(), paint)
        canvas.drawRect(0f, frame.bottom.toFloat(), width.toFloat(), height.toFloat(), paint)

        val left = frame.left.toFloat()
        val top = frame.top.toFloat()
        val right = frame.right.toFloat()
        val bottom = frame.bottom.toFloat()
        val cornerLength = 38f * density
        val cornerRadius = 12f * density

        frameBounds.set(left, top, right, bottom)
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = 1f * density
        paint.color = Color.argb(92, 255, 255, 255)
        canvas.drawRoundRect(frameBounds, cornerRadius, cornerRadius, paint)

        cornerPath.reset()
        cornerPath.moveTo(left, top + cornerLength)
        cornerPath.lineTo(left, top + cornerRadius)
        cornerPath.quadTo(left, top, left + cornerRadius, top)
        cornerPath.lineTo(left + cornerLength, top)

        cornerPath.moveTo(right - cornerLength, top)
        cornerPath.lineTo(right - cornerRadius, top)
        cornerPath.quadTo(right, top, right, top + cornerRadius)
        cornerPath.lineTo(right, top + cornerLength)

        cornerPath.moveTo(right, bottom - cornerLength)
        cornerPath.lineTo(right, bottom - cornerRadius)
        cornerPath.quadTo(right, bottom, right - cornerRadius, bottom)
        cornerPath.lineTo(right - cornerLength, bottom)

        cornerPath.moveTo(left + cornerLength, bottom)
        cornerPath.lineTo(left + cornerRadius, bottom)
        cornerPath.quadTo(left, bottom, left, bottom - cornerRadius)
        cornerPath.lineTo(left, bottom - cornerLength)

        paint.strokeWidth = 5f * density
        paint.strokeCap = Paint.Cap.ROUND
        paint.strokeJoin = Paint.Join.ROUND
        paint.color = accentColor
        canvas.drawPath(cornerPath, paint)
    }
}
