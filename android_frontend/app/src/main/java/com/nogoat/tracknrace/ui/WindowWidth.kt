package com.nogoat.tracknrace.ui

import android.content.res.Configuration
import androidx.compose.runtime.Composable
import androidx.compose.ui.platform.LocalConfiguration

/**
 * Material 3's window width classes. Layouts follow the room the window has,
 * so a tablet and a phone held the same way can lay out differently.
 */
internal enum class WidthClass {
    COMPACT,
    MEDIUM,
    EXPANDED,
    LARGE;

    /** A tablet or foldable width: the navigation rail and the wider layouts. */
    val atLeastMedium: Boolean get() = this != COMPACT
}

internal fun widthClassOf(widthDp: Int): WidthClass = when {
    widthDp < 600 -> WidthClass.COMPACT
    widthDp < 840 -> WidthClass.MEDIUM
    widthDp < 1200 -> WidthClass.EXPANDED
    else -> WidthClass.LARGE
}

/** The window's width class. A plain read of the configuration, so a long list does not re-measure for it. */
@Composable
internal fun currentWidthClass(): WidthClass = widthClassOf(LocalConfiguration.current.screenWidthDp)

/** A tablet: the shorter side is at least 600 dp, whichever way it is held. */
internal fun isTablet(configuration: Configuration): Boolean = configuration.smallestScreenWidthDp >= 600

/**
 * Whether landscape is the steering-wheel display, its bars hidden until a
 * tap: the setting when chosen, else on for phones and off for tablets.
 */
internal fun wheelDisplay(setting: Boolean?, configuration: Configuration): Boolean =
    setting ?: !isTablet(configuration)

/** Landscape as the wheel display: the bars hide until a tap. */
internal fun immersiveLandscape(setting: Boolean?, configuration: Configuration): Boolean =
    configuration.orientation == Configuration.ORIENTATION_LANDSCAPE && wheelDisplay(setting, configuration)
