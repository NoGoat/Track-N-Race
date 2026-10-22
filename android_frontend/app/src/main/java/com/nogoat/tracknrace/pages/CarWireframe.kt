package com.nogoat.tracknrace.pages

import android.content.Context
import android.graphics.Matrix
import android.graphics.Path
import android.graphics.RectF
import android.util.Xml
import androidx.compose.ui.graphics.vector.PathParser
import androidx.compose.ui.graphics.asAndroidPath
import org.xmlpull.v1.XmlPullParser

/**
 * The desktop's car wireframe (assets/car/f1-car-wireframe.svg, shared with
 * Electron and Qt), read into paths for the Damage tab. Only what that file
 * uses is understood: groups with translate/matrix transforms, paths, rounded
 * rects and ellipses. The car is kept in its authored nose-up frame, so the
 * top-level rotation that turns it nose-right on the desktop is skipped.
 */
internal class CarWireframe(val parts: List<Part>, val bounds: RectF) {
    enum class Kind {
        /** A body panel: filled with the card colour so hidden lines drop out, then outlined. */
        SOLID,
        /** A thin inner line: panel joints, flaps, sidewalls. */
        DETAIL,
        /** Suspension. */
        TUBE,
    }

    /** One drawn shape; [zone] names its damage zone, e.g. `z-fw-l` or `t-rr`. */
    class Part(val zone: String?, val path: Path, val kind: Kind, val strokeWidth: Float)
}

internal fun loadCarWireframe(context: Context): CarWireframe? = runCatching {
    context.assets.open("car/f1-car-wireframe.svg").use { input ->
        val parser = Xml.newPullParser()
        parser.setFeature(XmlPullParser.FEATURE_PROCESS_NAMESPACES, false)
        parser.setInput(input, null)
        val parts = mutableListOf<CarWireframe.Part>()

        // One entry per open <g>: its accumulated transform, zone, class and
        // stroke width, and whether it (or a parent) is skipped.
        class Group(val matrix: Matrix, val zone: String?, val tube: Boolean, val strokeWidth: Float, val skip: Boolean)
        val stack = ArrayDeque<Group>()
        stack.addLast(Group(Matrix(), null, false, 2.4f, false))

        while (parser.next() != XmlPullParser.END_DOCUMENT) {
            if (parser.eventType == XmlPullParser.END_TAG && parser.name == "g") {
                stack.removeLast()
                continue
            }
            if (parser.eventType != XmlPullParser.START_TAG) continue
            val parent = stack.last()
            val id = parser.getAttributeValue(null, "id")
            val cls = parser.getAttributeValue(null, "class")
            val strokeWidth = parser.getAttributeValue(null, "stroke-width")?.toFloatOrNull()
            when (parser.name) {
                "g" -> {
                    val matrix = Matrix(parent.matrix)
                    // #car only rotates the nose-up drawing for the desktop.
                    if (id != "car") parseTransform(parser.getAttributeValue(null, "transform"))?.let { matrix.preConcat(it) }
                    val zone = id?.takeIf { it.startsWith("z-") || it.startsWith("t-") } ?: parent.zone
                    stack.addLast(
                        Group(
                            matrix,
                            zone,
                            parent.tube || cls == "tube",
                            strokeWidth ?: parent.strokeWidth,
                            parent.skip || id == "anchors",
                        ),
                    )
                }
                "path", "rect", "ellipse" -> {
                    if (parent.skip) continue
                    val path = when (parser.name) {
                        "path" -> PathParser().parsePathString(parser.getAttributeValue(null, "d") ?: continue).toPath().asAndroidPath()
                        "rect" -> Path().apply {
                            val x = parser.float("x")
                            val y = parser.float("y")
                            val rx = parser.float("rx")
                            addRoundRect(RectF(x, y, x + parser.float("width"), y + parser.float("height")), rx, rx, Path.Direction.CW)
                        }
                        else -> Path().apply {
                            val cx = parser.float("cx")
                            val cy = parser.float("cy")
                            val rx = parser.float("rx")
                            val ry = parser.float("ry")
                            addOval(RectF(cx - rx, cy - ry, cx + rx, cy + ry), Path.Direction.CW)
                        }
                    }
                    path.transform(parent.matrix)
                    val kind = when {
                        cls == "solid" -> CarWireframe.Kind.SOLID
                        cls == "detail" -> CarWireframe.Kind.DETAIL
                        parent.tube -> CarWireframe.Kind.TUBE
                        else -> CarWireframe.Kind.DETAIL
                    }
                    parts += CarWireframe.Part(parent.zone, path, kind, strokeWidth ?: parent.strokeWidth)
                }
            }
        }
        val bounds = RectF()
        val partBounds = RectF()
        parts.forEachIndexed { index, part ->
            part.path.computeBounds(partBounds, true)
            if (index == 0) bounds.set(partBounds) else bounds.union(partBounds)
        }
        CarWireframe(parts, bounds)
    }
}.getOrNull()

private fun XmlPullParser.float(name: String): Float = getAttributeValue(null, name)?.toFloatOrNull() ?: 0f

/** `translate(x y)` or `matrix(a b c d e f)`, as the wireframe uses them. */
private fun parseTransform(value: String?): Matrix? {
    if (value.isNullOrBlank()) return null
    val numbers = value.substringAfter('(').substringBefore(')')
        .split(' ', ',').filter { it.isNotBlank() }.map { it.toFloat() }
    return Matrix().apply {
        when {
            value.startsWith("translate") -> setTranslate(numbers[0], numbers.getOrElse(1) { 0f })
            value.startsWith("matrix") && numbers.size == 6 -> setValues(
                floatArrayOf(numbers[0], numbers[2], numbers[4], numbers[1], numbers[3], numbers[5], 0f, 0f, 1f),
            )
            else -> return null
        }
    }
}
