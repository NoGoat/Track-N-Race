package com.tracknrace.android.ui.theme

import android.os.Build
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Shapes
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.dynamicDarkColorScheme
import androidx.compose.material3.dynamicLightColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp

private val FallbackLight = lightColorScheme(
    primary = Color(0xff9c423d),
    onPrimary = Color.White,
    secondary = Color(0xff5d5f7d),
    tertiary = Color(0xff775a2b),
    surface = Color(0xfffff8f6),
    surfaceContainer = Color(0xfff6e8e5),
)

private val FallbackDark = darkColorScheme(
    primary = Color(0xffffb3ad),
    onPrimary = Color(0xff5f1312),
    secondary = Color(0xffc5c4e9),
    tertiary = Color(0xffe8c17d),
    surface = Color(0xff171211),
    surfaceContainer = Color(0xff241e1d),
)

private val AppShapes = Shapes(
    extraSmall = RoundedCornerShape(4.dp),
    small = RoundedCornerShape(8.dp),
    medium = RoundedCornerShape(12.dp),
    large = RoundedCornerShape(20.dp),
    extraLarge = RoundedCornerShape(28.dp),
)

@Composable
internal fun TrackNRaceTheme(content: @Composable () -> Unit) {
    val context = LocalContext.current
    val dark = isSystemInDarkTheme()
    val colors = when {
        Build.VERSION.SDK_INT >= Build.VERSION_CODES.S && dark -> dynamicDarkColorScheme(context)
        Build.VERSION.SDK_INT >= Build.VERSION_CODES.S -> dynamicLightColorScheme(context)
        dark -> FallbackDark
        else -> FallbackLight
    }
    val typography = MaterialTheme.typography.copy(
        displayLarge = MaterialTheme.typography.displayLarge.tabular(),
        displayMedium = MaterialTheme.typography.displayMedium.tabular(),
        headlineLarge = MaterialTheme.typography.headlineLarge.tabular(),
        headlineMedium = MaterialTheme.typography.headlineMedium.tabular(),
        titleLarge = MaterialTheme.typography.titleLarge.tabular(),
        bodyLarge = MaterialTheme.typography.bodyLarge.tabular(),
        bodyMedium = MaterialTheme.typography.bodyMedium.tabular(),
        labelLarge = MaterialTheme.typography.labelLarge.tabular(),
    )
    MaterialTheme(
        colorScheme = colors,
        typography = typography,
        shapes = AppShapes,
        content = content,
    )
}

private fun TextStyle.tabular() = copy(
    fontFamily = FontFamily.SansSerif,
    fontFeatureSettings = "tnum",
)
