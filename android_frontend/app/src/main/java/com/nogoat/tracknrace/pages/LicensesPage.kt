package com.nogoat.tracknrace.pages

import androidx.annotation.RawRes
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.expandVertically
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.shrinkVertically
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.rotate
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalUriHandler
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.nogoat.tracknrace.R

// ── Design ──────────────────────────────────────────────────────────────────
// The Electron app's Attribution view in Material 3: the components grouped
// by where they ship, one card each. Tapping a card's header shows its licence
// text in a scrolling panel; one is open at a time, as on the desktop. The
// homepage link is its own 48 dp button so it can't be hit by an expand tap.

private enum class AttributionCategory(val label: String) {
    TRACK_N_RACE("Track N Race"),
    APP("Application libraries"),
    NATIVE("Native telemetry engine"),
}

private class Attribution(
    val name: String,
    val version: String,
    val license: String,
    val homepage: String,
    @param:RawRes val licenseText: Int,
    val category: AttributionCategory,
    val badge: String? = null,
)

private val ATTRIBUTIONS = listOf(
    // The app itself
    Attribution("Track N Race", "", "GPL-3.0", "https://github.com/NoGoat/Track-N-Race", R.raw.track_n_race, AttributionCategory.TRACK_N_RACE),
    // Application libraries
    Attribution("Jetpack Compose", "BOM 2026.09.00", "Apache-2.0", "https://developer.android.com/jetpack/compose", R.raw.apache_2_0, AttributionCategory.APP),
    Attribution("AndroidX Activity", "1.13.0", "Apache-2.0", "https://developer.android.com/jetpack/androidx/releases/activity", R.raw.apache_2_0, AttributionCategory.APP),
    Attribution("Material Components for Android", "1.13.0", "Apache-2.0", "https://github.com/material-components/material-components-android", R.raw.apache_2_0, AttributionCategory.APP),
    Attribution("Kotlin", "2.3.21", "Apache-2.0", "https://kotlinlang.org", R.raw.apache_2_0, AttributionCategory.APP),
    Attribution("OkHttp", "5.3.0", "Apache-2.0", "https://square.github.io/okhttp/", R.raw.apache_2_0, AttributionCategory.APP),
    Attribution("ZXing Android Embedded", "4.3.0", "Apache-2.0", "https://github.com/journeyapps/zxing-android-embedded", R.raw.apache_2_0, AttributionCategory.APP),
    // Native telemetry engine
    Attribution("zlib", "1.3.2", "Zlib", "https://zlib.net", R.raw.zlib, AttributionCategory.NATIVE),
    Attribution("Zstandard", "1.5.7", "BSD-3-Clause", "https://facebook.github.io/zstd/", R.raw.zstandard, AttributionCategory.NATIVE),
    Attribution("Glaze", "7.8.3", "MIT", "https://github.com/stephenberry/glaze", R.raw.glaze, AttributionCategory.NATIVE),
    Attribution("libxlsxwriter", "1.2.4", "BSD-2-Clause", "https://libxlsxwriter.github.io", R.raw.libxlsxwriter, AttributionCategory.NATIVE),
    Attribution("libsodium", "1.0.20", "ISC", "https://libsodium.org", R.raw.libsodium, AttributionCategory.NATIVE),
    Attribution("CPace", "41701e7", "BSD-2-Clause", "https://github.com/jedisct1/cpace", R.raw.cpace, AttributionCategory.NATIVE),
)

@Composable
internal fun LicensesScreen() {
    var expanded by rememberSaveable { mutableStateOf<String?>(null) }
    LazyColumn(
        Modifier.fillMaxSize(),
        contentPadding = PaddingValues(horizontal = 16.dp, vertical = 8.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        item {
            Text(
                "Track N Race is built with these open-source components. Thank you to their authors.",
                Modifier.padding(horizontal = 16.dp, vertical = 8.dp),
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        for (category in AttributionCategory.entries) {
            val group = ATTRIBUTIONS.filter { it.category == category }
            if (group.isEmpty()) continue
            item(key = category.name) {
                Text(
                    category.label,
                    Modifier.padding(start = 16.dp, top = 16.dp),
                    style = MaterialTheme.typography.titleSmall,
                    color = MaterialTheme.colorScheme.primary,
                )
            }
            items(group, key = { it.name }) { item ->
                AttributionCard(
                    item = item,
                    expanded = expanded == item.name,
                    onToggle = { expanded = if (expanded == item.name) null else item.name },
                )
            }
        }
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun AttributionCard(item: Attribution, expanded: Boolean, onToggle: () -> Unit) {
    val colors = MaterialTheme.colorScheme
    val uriHandler = LocalUriHandler.current
    val chevron by animateFloatAsState(if (expanded) 180f else 0f, label = "chevron")
    Surface(
        Modifier.fillMaxWidth(),
        shape = MaterialTheme.shapes.large,
        color = colors.surfaceContainerLow,
    ) {
        Column {
            Row(
                Modifier
                    .fillMaxWidth()
                    .heightIn(min = 64.dp)
                    .clickable(role = Role.Button, onClick = onToggle)
                    .semantics { stateDescription = if (expanded) "License shown" else "License hidden" }
                    .padding(start = 16.dp, end = 4.dp, top = 10.dp, bottom = 10.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Icon(
                    painterResource(R.drawable.ic_expand_more),
                    contentDescription = null,
                    modifier = Modifier.rotate(chevron),
                    tint = colors.onSurfaceVariant,
                )
                // Name over its tags, so a long name and three tags still fit
                // a phone's width without truncating either.
                Column(
                    Modifier.weight(1f).padding(start = 12.dp),
                    verticalArrangement = Arrangement.spacedBy(6.dp),
                ) {
                    Text(item.name, style = MaterialTheme.typography.titleSmall, color = colors.onSurface)
                    FlowRow(
                        horizontalArrangement = Arrangement.spacedBy(6.dp),
                        verticalArrangement = Arrangement.spacedBy(4.dp),
                    ) {
                        item.badge?.let { Tag(it, colors.tertiaryContainer, colors.onTertiaryContainer) }
                        if (item.version.isNotEmpty()) Tag(item.version, colors.secondaryContainer, colors.onSecondaryContainer)
                        Tag(item.license, colors.surfaceContainerHighest, colors.onSurfaceVariant)
                    }
                }
                IconButton(
                    onClick = { runCatching { uriHandler.openUri(item.homepage) } },
                    modifier = Modifier.semantics { contentDescription = "Open ${item.name} website" },
                ) {
                    Icon(painterResource(R.drawable.ic_open_in_new), contentDescription = null, tint = colors.onSurfaceVariant)
                }
            }
            AnimatedVisibility(
                visible = expanded,
                enter = expandVertically() + fadeIn(),
                exit = shrinkVertically() + fadeOut(),
            ) {
                LicenseText(item.licenseText)
            }
        }
    }
}

@Composable
private fun Tag(text: String, container: Color, content: Color) {
    Text(
        text,
        Modifier
            .background(container, MaterialTheme.shapes.small)
            .padding(horizontal = 8.dp, vertical = 2.dp),
        style = MaterialTheme.typography.labelMedium,
        fontWeight = FontWeight.SemiBold,
        color = content,
        maxLines = 1,
    )
}

/** The licence, read when first shown, in a panel that scrolls on its own. */
@Composable
private fun LicenseText(@RawRes resource: Int) {
    val context = LocalContext.current
    val text = remember(resource) {
        context.resources.openRawResource(resource).bufferedReader().use { it.readText() }.trim()
    }
    Column {
        HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
        Box(
            Modifier
                .fillMaxWidth()
                .heightIn(max = 320.dp)
                .background(MaterialTheme.colorScheme.surfaceContainerHighest.copy(alpha = 0.5f))
                .verticalScroll(rememberScrollState())
                .padding(16.dp),
        ) {
            Text(
                text,
                style = MaterialTheme.typography.bodySmall,
                fontFamily = FontFamily.Monospace,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}
