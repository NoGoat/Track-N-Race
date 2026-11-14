package com.nogoat.tracknrace.pages

import android.Manifest
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.provider.Settings
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.core.FastOutSlowInEasing
import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.LinearOutSlowInEasing
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.GenericShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.FilterChip
import androidx.compose.material3.FilterChipDefaults
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.ListItem
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.derivedStateOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.runtime.structuralEqualityPolicy
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.clipRect
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.graphics.luminance
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.colorResource
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.withStyle
import androidx.compose.ui.text.input.KeyboardCapitalization
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import com.google.zxing.client.android.Intents
import com.journeyapps.barcodescanner.ScanContract
import com.nogoat.tracknrace.DiscoveredDesktop
import com.nogoat.tracknrace.PairedTelemetryClient
import com.nogoat.tracknrace.R
import com.nogoat.tracknrace.TelemetryController
import com.nogoat.tracknrace.TelemetryStore
import com.nogoat.tracknrace.TrackNames
import com.nogoat.tracknrace.sessionName
import com.nogoat.tracknrace.ui.theme.ThemeMode
import kotlin.math.PI
import kotlin.math.cos
import kotlin.math.min
import kotlin.math.roundToInt
import kotlin.math.sin

/**
 * First-time setup, in the Android setup-wizard pattern: each step is an icon,
 * a large headline, a line of description and its content, with the
 * secondary action bottom left and the primary button bottom right. Back is
 * the system gesture. The Source choice branches: Paired goes to pairing,
 * Direct to the game's settings and the permissions explanation; both then
 * check that telemetry arrives.
 */
private enum class SetupStep { WELCOME, THEME, SOURCE, PAIR, DIRECT, ACCESS, CHECK, RECORDINGS, DONE }

// The desktop's code alphabet: no 0/O or 1/I/L to confuse.
private const val CODE_ALPHABET = "ABCDEFGHJKMNPQRSTUVWXYZ23456789"
private const val CODE_LENGTH = 8

@Composable
internal fun FirstTimeSetupScreen(telemetry: TelemetryController) {
    val store = telemetry.store
    val settings = store.settings
    val context = LocalContext.current
    val snackbar = remember { SnackbarHostState() }

    // The steps visited, oldest first, so Back retraces a branch.
    var historyText by rememberSaveable { mutableStateOf(SetupStep.WELCOME.name) }
    val history = historyText.split(',').map(SetupStep::valueOf)
    val step = history.last()
    fun go(next: SetupStep) { historyText = (history + next).joinToString(",") { it.name } }
    fun back() { if (history.size > 1) historyText = history.dropLast(1).joinToString(",") { it.name } }
    /** Back to [target] if it was visited, else on to it. */
    fun returnTo(target: SetupStep) {
        val index = history.lastIndexOf(target)
        historyText = if (index >= 0) {
            history.take(index + 1).joinToString(",") { it.name }
        } else {
            (history + target).joinToString(",") { it.name }
        }
    }
    BackHandler(enabled = history.size > 1) { back() }

    val message = store.message
    LaunchedEffect(message?.id) { message?.let { snackbar.showSnackbar(it.text) } }

    // Pairing and discovery ask for local network access through this; setup
    // never asks on its own at launch, only on the step that needs it.
    val localNetworkLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestPermission(),
    ) { granted -> telemetry.onLocalNetworkResult(granted) }
    DisposableEffect(telemetry, localNetworkLauncher) {
        telemetry.localNetworkRequester = {
            localNetworkLauncher.launch(Manifest.permission.ACCESS_LOCAL_NETWORK)
        }
        onDispose { telemetry.localNetworkRequester = null }
    }
    // Allow access: the system dialogs in turn, then the check.
    val accessLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions(),
    ) { results ->
        results[Manifest.permission.ACCESS_LOCAL_NETWORK]?.let(telemetry::onLocalNetworkResult)
        go(SetupStep.CHECK)
    }
    val qrLauncher = rememberLauncherForActivityResult(ScanContract()) { result ->
        telemetry.onQrScannerFinished()
        val contents = result.contents
        if (!contents.isNullOrEmpty()) {
            telemetry.pairQr(contents)
        } else if (result.originalIntent?.hasExtra(Intents.Scan.MISSING_CAMERA_PERMISSION) == true) {
            store.showMessage("Camera permission is required to scan the desktop QR code")
        }
    }
    val directoryLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.StartActivityForResult(),
    ) { result ->
        val data = result.data
        data?.data?.let { uri -> telemetry.acceptRecordingDirectory(uri, data.flags) }
    }

    val paired = settings.source == PairedTelemetryClient.SOURCE_PAIRED

    Scaffold(
        snackbarHost = { SnackbarHost(snackbar) },
        containerColor = MaterialTheme.colorScheme.surface,
    ) { padding ->
        AnimatedContent(
            targetState = step,
            modifier = Modifier.fillMaxSize().padding(padding),
            label = "Setup step",
        ) { target ->
            when (target) {
                SetupStep.WELCOME -> WelcomeStep(onStart = { go(SetupStep.THEME) })
                SetupStep.THEME -> ThemeStep(
                    selected = settings.themeMode,
                    onSelect = telemetry::setThemeMode,
                    onSkip = { go(SetupStep.SOURCE) },
                    onNext = { go(SetupStep.SOURCE) },
                )
                SetupStep.SOURCE -> SourceStep(
                    initialPaired = paired,
                    onSkip = { go(SetupStep.RECORDINGS) },
                    onNext = { choosePaired ->
                        when {
                            choosePaired && settings.hasSavedDesktop -> {
                                telemetry.setSource(PairedTelemetryClient.SOURCE_PAIRED)
                                go(SetupStep.CHECK)
                            }
                            choosePaired -> go(SetupStep.PAIR)
                            else -> {
                                if (paired) telemetry.setSource(PairedTelemetryClient.SOURCE_DIRECT)
                                go(SetupStep.DIRECT)
                            }
                        }
                    },
                )
                SetupStep.PAIR -> PairStep(
                    telemetry = telemetry,
                    store = store,
                    onScanQr = {
                        telemetry.onQrScannerLaunching()
                        try {
                            qrLauncher.launch(telemetry.qrScanOptions())
                        } catch (error: RuntimeException) {
                            telemetry.onQrScannerFinished()
                            store.showMessage(error.message ?: "Could not open the QR scanner")
                        }
                    },
                    onPaired = { go(SetupStep.CHECK) },
                )
                SetupStep.DIRECT -> DirectStep(
                    port = settings.udpPort,
                    onSkip = { go(SetupStep.RECORDINGS) },
                    onNext = { port ->
                        if (port != settings.udpPort) {
                            telemetry.setUdpSettings(port, settings.udpForwardingEnabled, settings.udpForwardTargets)
                        }
                        if (missingAccess(context, telemetry).isEmpty()) go(SetupStep.CHECK) else go(SetupStep.ACCESS)
                    },
                )
                SetupStep.ACCESS -> AccessStep(
                    localNetworkAllowed = telemetry.hasLocalNetworkAccess(),
                    notificationsAllowed = notificationsAllowed(context),
                    onNoThanks = { go(SetupStep.CHECK) },
                    onAllow = {
                        val missing = missingAccess(context, telemetry)
                        if (missing.isEmpty()) go(SetupStep.CHECK) else accessLauncher.launch(missing.toTypedArray())
                    },
                )
                SetupStep.CHECK -> CheckStep(
                    store = store,
                    paired = paired,
                    onGameSettings = { returnTo(SetupStep.DIRECT) },
                    onWifiSettings = {
                        runCatching { context.startActivity(Intent(Settings.ACTION_WIFI_SETTINGS)) }
                    },
                    onSkip = { go(SetupStep.RECORDINGS) },
                    onNext = { go(SetupStep.RECORDINGS) },
                )
                SetupStep.RECORDINGS -> RecordingsStep(
                    enabled = settings.recordingEnabled,
                    folder = settings.recordingDirectory.ifEmpty { "App Documents folder" },
                    onEnabled = telemetry::setRecording,
                    onFolder = { directoryLauncher.launch(telemetry.recordingDirectoryIntent()) },
                    onSkip = { go(SetupStep.DONE) },
                    onNext = { go(SetupStep.DONE) },
                )
                SetupStep.DONE -> DoneStep(
                    theme = themeLabel(settings.themeMode),
                    connection = when {
                        paired && settings.hasSavedDesktop -> "Paired with ${settings.desktopName}"
                        else -> "From the game, UDP port ${settings.udpPort}"
                    },
                    recordings = when {
                        paired -> "Not on this phone in Paired mode"
                        settings.recordingEnabled -> "On · ${settings.recordingDirectory.ifEmpty { "App Documents folder" }}"
                        else -> "Off"
                    },
                    onTheme = { returnTo(SetupStep.THEME) },
                    onConnection = { returnTo(SetupStep.SOURCE) },
                    onRecordings = { returnTo(SetupStep.RECORDINGS) },
                    onStart = telemetry::completeSetup,
                )
            }
        }
    }
}

private fun notificationsAllowed(context: Context): Boolean =
    Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU ||
        context.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) == PackageManager.PERMISSION_GRANTED

/** The permissions Direct mode still needs: local network on Android 17, notifications on 13+. */
private fun missingAccess(context: Context, telemetry: TelemetryController): List<String> = buildList {
    if (!telemetry.hasLocalNetworkAccess()) add(Manifest.permission.ACCESS_LOCAL_NETWORK)
    if (!notificationsAllowed(context)) add(Manifest.permission.POST_NOTIFICATIONS)
}

private fun themeLabel(mode: ThemeMode): String = when (mode) {
    ThemeMode.SYSTEM -> "System"
    ThemeMode.LIGHT -> "Light"
    ThemeMode.DARK -> "Dark"
}

// ── The frame every step shares ─────────────────────────────────────────────

@Composable
private fun SetupPage(
    icon: Int?,
    title: String,
    description: String,
    secondary: String?,
    onSecondary: () -> Unit,
    primary: String,
    onPrimary: () -> Unit,
    primaryEnabled: Boolean = true,
    content: @Composable ColumnScope.() -> Unit,
) {
    Column(Modifier.fillMaxSize()) {
        Column(
            Modifier
                .weight(1f)
                .fillMaxWidth()
                .verticalScroll(rememberScrollState()),
        ) {
            Column(Modifier.padding(start = 24.dp, end = 24.dp, top = 36.dp)) {
                if (icon != null) {
                    Icon(
                        painterResource(icon),
                        contentDescription = null,
                        tint = MaterialTheme.colorScheme.primary,
                        modifier = Modifier.size(32.dp),
                    )
                    Spacer(Modifier.height(20.dp))
                }
                Text(title, style = MaterialTheme.typography.headlineLarge)
                Spacer(Modifier.height(16.dp))
                Text(
                    description,
                    style = MaterialTheme.typography.bodyLarge,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            content()
            Spacer(Modifier.height(16.dp))
        }
        SetupFooter(secondary, onSecondary, primary, onPrimary, primaryEnabled)
    }
}

@Composable
private fun SetupFooter(
    secondary: String?,
    onSecondary: () -> Unit,
    primary: String,
    onPrimary: () -> Unit,
    primaryEnabled: Boolean,
) {
    Row(
        Modifier
            .fillMaxWidth()
            .padding(start = 12.dp, end = 24.dp, top = 16.dp, bottom = 24.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        if (secondary != null) TextButton(onClick = onSecondary) { Text(secondary) }
        Spacer(Modifier.weight(1f))
        Button(onClick = onPrimary, enabled = primaryEnabled) { Text(primary) }
    }
}

/** A flat M3 list item, full width, in the setup's 24 dp margins. */
@Composable
private fun SetupRow(
    title: String,
    supporting: String?,
    leading: Int? = null,
    trailing: (@Composable () -> Unit)? = null,
    onClick: (() -> Unit)? = null,
) {
    ListItem(
        headlineContent = { Text(title) },
        supportingContent = supporting?.let { { Text(it) } },
        leadingContent = leading?.let {
            { Icon(painterResource(it), contentDescription = null, tint = MaterialTheme.colorScheme.onSurfaceVariant) }
        },
        trailingContent = trailing,
        colors = ListItemDefaults.colors(containerColor = Color.Transparent),
        modifier = (if (onClick != null) Modifier.clickable(onClick = onClick) else Modifier).padding(horizontal = 8.dp),
    )
}

@Composable
private fun InfoBox(lead: String, text: String) {
    Row(
        Modifier
            .padding(horizontal = 24.dp, vertical = 8.dp)
            .fillMaxWidth()
            .clip(RoundedCornerShape(16.dp))
            .background(MaterialTheme.colorScheme.surfaceContainerLow)
            .padding(16.dp),
        horizontalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        Icon(
            painterResource(R.drawable.ic_info),
            contentDescription = null,
            tint = MaterialTheme.colorScheme.primary,
            modifier = Modifier.size(22.dp),
        )
        val leadColor = MaterialTheme.colorScheme.onSurface
        Text(
            buildAnnotatedString {
                withStyle(SpanStyle(color = leadColor)) { append(lead) }
                append(' ')
                append(text)
            },
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

@Composable
private fun SectionLabel(text: String) {
    Text(
        text,
        modifier = Modifier.padding(start = 24.dp, end = 24.dp, top = 24.dp, bottom = 4.dp),
        style = MaterialTheme.typography.titleSmall,
        color = MaterialTheme.colorScheme.primary,
    )
}

/** M3 Expressive's scalloped "cookie" shape. */
private val CookieShape = GenericShape { size, _ ->
    val cx = size.width / 2f
    val cy = size.height / 2f
    val radius = min(cx, cy) * 0.93f
    val amplitude = min(cx, cy) * 0.07f
    val steps = 240
    for (i in 0..steps) {
        val angle = i / steps.toDouble() * 2 * PI
        val r = radius + amplitude * cos(9 * angle)
        val x = (cx + r * cos(angle)).toFloat()
        val y = (cy + r * sin(angle)).toFloat()
        if (i == 0) moveTo(x, y) else lineTo(x, y)
    }
    close()
}

// ── 1 Welcome ───────────────────────────────────────────────────────────────

@Composable
private fun WelcomeStep(onStart: () -> Unit) {
    Column(Modifier.fillMaxSize()) {
        Column(
            Modifier
                .weight(1f)
                .fillMaxWidth()
                .verticalScroll(rememberScrollState())
                .padding(horizontal = 32.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.Center,
        ) {
            Box(
                Modifier
                    .size(200.dp)
                    .background(MaterialTheme.colorScheme.primaryContainer, CookieShape),
                contentAlignment = Alignment.Center,
            ) {
                // The launcher icon, as its adaptive layers draw it.
                Box(
                    Modifier
                        .size(112.dp)
                        .clip(CircleShape)
                        .background(colorResource(R.color.ic_launcher_background)),
                    contentAlignment = Alignment.Center,
                ) {
                    Image(
                        painterResource(R.mipmap.ic_launcher_foreground),
                        contentDescription = "Track N Race",
                        modifier = Modifier.size(168.dp),
                    )
                }
            }
            Spacer(Modifier.height(40.dp))
            Text(
                "Welcome to Track N Race",
                style = MaterialTheme.typography.displaySmall,
                textAlign = TextAlign.Center,
            )
            Spacer(Modifier.height(16.dp))
            Text(
                "Live telemetry, timing and lap analysis from F1 25 and F1 24, on your phone.",
                style = MaterialTheme.typography.bodyLarge,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                textAlign = TextAlign.Center,
            )
        }
        SetupFooter(null, {}, "Get started", onStart, true)
    }
}

// ── 2 Theme ─────────────────────────────────────────────────────────────────

private class PreviewColors(
    val background: Color,
    val surface: Color,
    val text: Color,
    val muted: Color,
    val accent: Color,
    val pill: Color,
)

private val DarkPreview = PreviewColors(
    Color(0xFF0D131E), Color(0xFF1A202B), Color(0xFFDCE2F2), Color(0xFF4A5263), Color(0xFFA4C9FF), Color(0xFF3D4565),
)
private val LightPreview = PreviewColors(
    Color(0xFFF9F9FF), Color(0xFFE6EAF4), Color(0xFF191C20), Color(0xFFB9BFCB), Color(0xFF3A5F94), Color(0xFFD6E3FF),
)

@Composable
private fun ThemeStep(selected: ThemeMode, onSelect: (ThemeMode) -> Unit, onSkip: () -> Unit, onNext: () -> Unit) {
    SetupPage(
        icon = R.drawable.ic_brightness_medium,
        title = "Choose a theme",
        description = "You can change this any time in Settings.",
        secondary = "Skip",
        onSecondary = onSkip,
        primary = "Next",
        onPrimary = onNext,
    ) {
        Row(
            Modifier
                .padding(start = 24.dp, end = 24.dp, top = 40.dp)
                .fillMaxWidth()
                .selectableGroup(),
            horizontalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            ThemeMode.entries.forEach { mode ->
                val isSelected = mode == selected
                Column(
                    Modifier
                        .weight(1f)
                        .clip(RoundedCornerShape(20.dp))
                        .selectable(selected = isSelected, role = Role.RadioButton) { onSelect(mode) }
                        .padding(vertical = 4.dp),
                    horizontalAlignment = Alignment.CenterHorizontally,
                    verticalArrangement = Arrangement.spacedBy(12.dp),
                ) {
                    ThemePreview(
                        mode = mode,
                        modifier = Modifier
                            .width(100.dp)
                            .height(204.dp)
                            .border(
                                width = if (isSelected) 3.dp else 1.dp,
                                color = if (isSelected) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.outlineVariant,
                                shape = RoundedCornerShape(20.dp),
                            )
                            .clip(RoundedCornerShape(20.dp)),
                    )
                    RadioButton(selected = isSelected, onClick = null)
                    Text(themeLabel(mode), style = MaterialTheme.typography.bodyLarge)
                }
            }
        }
    }
}

/** A small phone showing the Dashboard in a theme; System is half light, half dark. */
@Composable
private fun ThemePreview(mode: ThemeMode, modifier: Modifier) {
    Canvas(modifier) {
        when (mode) {
            ThemeMode.LIGHT -> drawPreview(LightPreview)
            ThemeMode.DARK -> drawPreview(DarkPreview)
            ThemeMode.SYSTEM -> {
                drawPreview(DarkPreview)
                clipRect(right = size.width / 2f) { drawPreview(LightPreview) }
            }
        }
    }
}

private fun DrawScope.drawPreview(colors: PreviewColors) {
    val u = size.width / 100f
    drawRect(colors.background)
    fun bar(x: Float, y: Float, w: Float, h: Float, color: Color) =
        drawRoundRect(color, Offset(x * u, y * u), Size(w * u, h * u), CornerRadius(h * u / 2f))
    bar(10f, 18f, 44f, 7f, colors.text)
    drawRoundRect(colors.surface, Offset(10f * u, 36f * u), Size(80f * u, 40f * u), CornerRadius(10f * u))
    bar(18f, 46f, 22f, 4f, colors.muted)
    bar(18f, 56f, 34f, 10f, colors.accent)
    val line = Path().apply {
        val points = listOf(0f to 30f, 10f to 28f, 16f to 12f, 22f to 10f, 30f to 24f, 38f to 26f, 46f to 8f, 54f to 6f, 60f to 20f, 70f to 22f, 80f to 10f)
        points.forEachIndexed { index, (x, y) ->
            val px = (10f + x) * u
            val py = (88f + y) * u
            if (index == 0) moveTo(px, py) else lineTo(px, py)
        }
    }
    drawPath(line, colors.accent, style = Stroke(width = 2f * u, join = StrokeJoin.Round))
    bar(10f, 140f, 80f, 5f, colors.muted)
    bar(10f, 152f, 60f, 5f, colors.muted)
    drawRect(colors.surface, Offset(0f, size.height - 28f * u), Size(size.width, 28f * u))
    bar(12f, (size.height / u) - 20f, 22f, 11f, colors.pill)
    drawCircle(colors.muted, 3f * u, Offset(50f * u, size.height - 15f * u))
    drawCircle(colors.muted, 3f * u, Offset(75f * u, size.height - 15f * u))
}

// ── 3 Source ────────────────────────────────────────────────────────────────

@Composable
private fun SourceStep(initialPaired: Boolean, onSkip: () -> Unit, onNext: (paired: Boolean) -> Unit) {
    var choosePaired by rememberSaveable { mutableStateOf(initialPaired) }
    SetupPage(
        icon = R.drawable.ic_link,
        title = "How will you connect?",
        description = "Track N Race can get telemetry from your PC, or straight from the game.",
        secondary = "Skip",
        onSecondary = onSkip,
        primary = "Next",
        onPrimary = { onNext(choosePaired) },
    ) {
        Column(Modifier.padding(top = 32.dp).selectableGroup()) {
            SourceOption(
                selected = choosePaired,
                icon = R.drawable.ic_desktop,
                title = "From my PC",
                supporting = "Track N Race on your PC passes the game on to this phone. Live or playback.",
                onSelect = { choosePaired = true },
            )
            SourceOption(
                selected = !choosePaired,
                icon = R.drawable.ic_videogame,
                title = "From the game",
                supporting = "The game sends straight to this phone over Wi-Fi. No PC needed.",
                onSelect = { choosePaired = false },
            )
        }
        Text(
            "Not sure? Choose From the game. You can pair a PC later in Settings.",
            modifier = Modifier.padding(start = 24.dp, end = 24.dp, top = 24.dp),
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

@Composable
private fun SourceOption(selected: Boolean, icon: Int, title: String, supporting: String, onSelect: () -> Unit) {
    val colors = MaterialTheme.colorScheme
    Row(
        Modifier
            .fillMaxWidth()
            .heightIn(min = 88.dp)
            .background(if (selected) colors.surfaceContainerLow else Color.Transparent)
            .selectable(selected = selected, role = Role.RadioButton, onClick = onSelect)
            .padding(horizontal = 24.dp, vertical = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        Box(
            Modifier
                .size(48.dp)
                .background(if (selected) colors.primaryContainer else colors.surfaceContainerHigh, CircleShape),
            contentAlignment = Alignment.Center,
        ) {
            Icon(
                painterResource(icon),
                contentDescription = null,
                tint = if (selected) colors.onPrimaryContainer else colors.onSurfaceVariant,
            )
        }
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text(title, style = MaterialTheme.typography.bodyLarge)
            Text(supporting, style = MaterialTheme.typography.bodyMedium, color = colors.onSurfaceVariant)
        }
        RadioButton(selected = selected, onClick = null)
    }
}

// ── 4a Pair with your PC ────────────────────────────────────────────────────

@Composable
private fun PairStep(
    telemetry: TelemetryController,
    store: TelemetryStore,
    onScanQr: () -> Unit,
    onPaired: () -> Unit,
) {
    var codeFor by remember { mutableStateOf<String?>(null) }
    var codeDialog by remember { mutableStateOf(false) }
    var observedPairing by remember { mutableStateOf(store.pairingSuccessId) }
    val desktops = store.discoveredDesktops

    // Discovery asks for local network access first, where Android needs it.
    DisposableEffect(Unit) {
        telemetry.startDiscovery()
        onDispose { telemetry.stopDiscovery() }
    }
    LaunchedEffect(store.pairingSuccessId) {
        if (store.pairingSuccessId != observedPairing) {
            observedPairing = store.pairingSuccessId
            codeDialog = false
            onPaired()
        }
    }

    SetupPage(
        icon = R.drawable.ic_qr_code_scanner,
        title = "Pair with your PC",
        description = "On your PC, open Track N Race › Settings › Paired Devices › Pair a device, then scan the code.",
        secondary = "Enter code instead",
        onSecondary = {
            codeFor = desktops.firstOrNull()?.serverId
            codeDialog = true
        },
        primary = "Scan QR code",
        onPrimary = onScanQr,
        primaryEnabled = !store.pairingBusy,
    ) {
        PcQrIllustration(Modifier.padding(start = 24.dp, end = 24.dp, top = 28.dp))
        Text(
            "The camera is only used to read the code.",
            modifier = Modifier.fillMaxWidth().padding(start = 24.dp, end = 24.dp, top = 12.dp),
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            textAlign = TextAlign.Center,
        )
        if (store.pairingBusy) {
            PairingProgress(store)
        }
        SectionLabel("Nearby")
        if (desktops.isEmpty()) {
            Row(
                Modifier.padding(horizontal = 24.dp, vertical = 12.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                CircularProgressIndicator(Modifier.size(18.dp), strokeWidth = 2.dp)
                Text(
                    "Searching on this network…",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
        desktops.forEach { desktop ->
            SetupRow(
                title = desktop.name,
                supporting = "${desktop.host} · tap to enter its code",
                leading = R.drawable.ic_desktop,
                trailing = { Icon(painterResource(R.drawable.ic_chevron_right), contentDescription = null) },
                onClick = {
                    codeFor = desktop.serverId
                    codeDialog = true
                },
            )
        }
    }

    if (codeDialog) {
        CodeDialog(
            desktops = desktops,
            initialServerId = codeFor,
            store = store,
            onPair = telemetry::pairCode,
            onDismiss = { codeDialog = false },
        )
    }
}

@Composable
private fun PairingProgress(store: TelemetryStore) {
    Row(
        Modifier.padding(start = 24.dp, end = 24.dp, top = 16.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        CircularProgressIndicator(Modifier.size(18.dp), strokeWidth = 2.dp)
        Text(
            if (store.sourceStatus.state == "awaiting_approval") {
                "Allow this phone on your PC to finish pairing."
            } else {
                "Connecting…"
            },
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.primary,
        )
    }
}

@Composable
private fun CodeDialog(
    desktops: List<DiscoveredDesktop>,
    initialServerId: String?,
    store: TelemetryStore,
    onPair: (DiscoveredDesktop?, String) -> Unit,
    onDismiss: () -> Unit,
) {
    var selectedId by remember { mutableStateOf(initialServerId) }
    var code by remember { mutableStateOf("") }
    LaunchedEffect(desktops.size) {
        if (selectedId == null && desktops.isNotEmpty()) selectedId = desktops.first().serverId
    }
    val selected = desktops.firstOrNull { it.serverId == selectedId }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Enter the matching code") },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                Text(
                    "Pick your PC, then type the 8-character code shown under its QR.",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                if (desktops.isEmpty()) {
                    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                        CircularProgressIndicator(Modifier.size(18.dp), strokeWidth = 2.dp)
                        Text("Searching on this network…", style = MaterialTheme.typography.bodyMedium)
                    }
                }
                Column(Modifier.selectableGroup()) {
                    desktops.forEach { desktop ->
                        Row(
                            Modifier
                                .fillMaxWidth()
                                .heightIn(min = 48.dp)
                                .clip(MaterialTheme.shapes.small)
                                .selectable(selected = desktop.serverId == selectedId, role = Role.RadioButton) {
                                    selectedId = desktop.serverId
                                },
                            verticalAlignment = Alignment.CenterVertically,
                        ) {
                            RadioButton(selected = desktop.serverId == selectedId, onClick = null)
                            Spacer(Modifier.width(12.dp))
                            Column {
                                Text(desktop.name, style = MaterialTheme.typography.bodyLarge)
                                Text(
                                    desktop.host,
                                    style = MaterialTheme.typography.bodySmall,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                                )
                            }
                        }
                    }
                }
                OutlinedTextField(
                    value = code,
                    onValueChange = { value -> code = value.uppercase().filter { it in CODE_ALPHABET }.take(CODE_LENGTH) },
                    label = { Text("8-character matching code") },
                    singleLine = true,
                    keyboardOptions = KeyboardOptions(
                        capitalization = KeyboardCapitalization.Characters,
                        autoCorrectEnabled = false,
                        keyboardType = KeyboardType.Ascii,
                    ),
                    modifier = Modifier.fillMaxWidth(),
                )
                if (store.pairingBusy) PairingProgress(store)
            }
        },
        confirmButton = {
            TextButton(
                onClick = { onPair(selected, code) },
                enabled = selected != null && code.length == CODE_LENGTH && !store.pairingBusy,
            ) { Text("Pair") }
        },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}

/** A PC screen showing a pairing QR code. */
@Composable
private fun PcQrIllustration(modifier: Modifier) {
    val colors = MaterialTheme.colorScheme
    Box(
        modifier
            .fillMaxWidth()
            .height(196.dp)
            .clip(RoundedCornerShape(28.dp))
            .background(colors.surfaceContainerLow)
            .semantics { contentDescription = "A PC screen showing the pairing QR code" },
        contentAlignment = Alignment.TopCenter,
    ) {
        Canvas(Modifier.padding(top = 24.dp).width(196.dp).height(146.dp)) {
            val frame = colors.outlineVariant
            val screenH = 124.dp.toPx()
            drawRoundRect(colors.surfaceContainerHigh, size = Size(size.width, screenH), cornerRadius = CornerRadius(10.dp.toPx()))
            drawRoundRect(
                frame,
                size = Size(size.width, screenH),
                cornerRadius = CornerRadius(10.dp.toPx()),
                style = Stroke(3.dp.toPx()),
            )
            drawRect(frame, Offset(size.width / 2f - 32.dp.toPx(), screenH), Size(64.dp.toPx(), 14.dp.toPx()))
            drawRoundRect(
                frame,
                Offset(size.width / 2f - 50.dp.toPx(), screenH + 14.dp.toPx()),
                Size(100.dp.toPx(), 6.dp.toPx()),
                CornerRadius(3.dp.toPx()),
            )
            // The code: a light square with an 11 × 11 pattern and three finders.
            val qr = 92.dp.toPx()
            val origin = Offset(size.width / 2f - qr / 2f, 16.dp.toPx())
            drawRoundRect(Color(0xFFE8EAEE), origin, Size(qr, qr), CornerRadius(6.dp.toPx()))
            val inset = 6.dp.toPx()
            val module = (qr - 2 * inset) / 11f
            QrModules.forEachIndexed { index, on ->
                if (!on) return@forEachIndexed
                val row = index / 11
                val column = index % 11
                drawRect(
                    Color(0xFF11161D),
                    Offset(origin.x + inset + column * module, origin.y + inset + row * module),
                    Size(module, module),
                )
            }
        }
    }
}

private val QrModules: List<Boolean> = run {
    var seed = 7
    fun random(): Float {
        seed = (seed * 9301 + 49297) % 233280
        return seed / 233280f
    }
    fun finder(row: Int, column: Int, row0: Int, column0: Int): Boolean? {
        val y = row - row0
        val x = column - column0
        if (y < 0 || x < 0 || y > 3 || x > 3) return null
        return y == 0 || x == 0 || y == 3 || x == 3 || (y in 1..2 && x in 1..2)
    }
    List(121) { index ->
        val row = index / 11
        val column = index % 11
        finder(row, column, 0, 0) ?: finder(row, column, 0, 7) ?: finder(row, column, 7, 0) ?: (random() > 0.5f)
    }
}

// ── 4b Point the game at this phone ─────────────────────────────────────────

@Composable
private fun DirectStep(port: Int, onSkip: () -> Unit, onNext: (port: Int) -> Unit) {
    val context = LocalContext.current
    val addresses = rememberDeviceAddresses()
    val address = addresses.firstOrNull()
    var f124 by rememberSaveable { mutableStateOf(false) }
    var portText by rememberSaveable { mutableStateOf(port.toString()) }
    val portValue = portText.toIntOrNull()?.takeIf { it in 1..65535 }

    SetupPage(
        icon = R.drawable.ic_wifi,
        title = "Point the game at this phone",
        description = "In the game, open Settings › Telemetry Settings and enter these.",
        secondary = "Skip",
        onSecondary = onSkip,
        primary = "Next",
        onPrimary = { portValue?.let(onNext) },
        primaryEnabled = portValue != null,
    ) {
        Row(
            Modifier.padding(start = 24.dp, end = 24.dp, top = 24.dp).selectableGroup(),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            FilterChip(
                selected = !f124,
                onClick = { f124 = false },
                label = { Text("F1 25 + 2026 pack") },
                leadingIcon = if (!f124) {
                    { Icon(painterResource(R.drawable.ic_check), null, Modifier.size(FilterChipDefaults.IconSize)) }
                } else null,
            )
            FilterChip(
                selected = f124,
                onClick = { f124 = true },
                label = { Text("F1 24") },
                leadingIcon = if (f124) {
                    { Icon(painterResource(R.drawable.ic_check), null, Modifier.size(FilterChipDefaults.IconSize)) }
                } else null,
            )
        }

        ListItem(
            overlineContent = { Text("UDP IP Address") },
            headlineContent = {
                Text(
                    address ?: "Not on Wi-Fi",
                    style = MaterialTheme.typography.titleLarge,
                    color = if (address != null) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.error,
                )
            },
            supportingContent = if (addresses.size > 1) {
                { Text("Also: ${addresses.drop(1).joinToString()}") }
            } else null,
            trailingContent = if (address != null) {
                {
                    IconButton(onClick = { copyToClipboard(context, "UDP IP Address", address) }) {
                        Icon(painterResource(R.drawable.ic_content_copy), contentDescription = "Copy IP address")
                    }
                }
            } else null,
            colors = ListItemDefaults.colors(containerColor = Color.Transparent),
            modifier = Modifier.padding(top = 8.dp, start = 8.dp),
        )
        OutlinedTextField(
            value = portText,
            onValueChange = { value -> portText = value.filter(Char::isDigit).take(5) },
            label = { Text("UDP Port") },
            supportingText = { Text(if (portValue == null) "Enter a port from 1 to 65535." else "Must match the game. The default is 20777.") },
            isError = portValue == null,
            singleLine = true,
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
            textStyle = MaterialTheme.typography.titleLarge,
            modifier = Modifier.fillMaxWidth().padding(horizontal = 24.dp, vertical = 8.dp),
        )
        HorizontalDivider(Modifier.padding(horizontal = 24.dp, vertical = 8.dp), color = MaterialTheme.colorScheme.outlineVariant)
        Column(Modifier.padding(horizontal = 24.dp)) {
            Row {
                SettingValue("UDP Telemetry", "On", Modifier.weight(1f))
                SettingValue("Broadcast Mode", "Off", Modifier.weight(1f))
            }
            Row {
                SettingValue("Send Rate", "60 Hz", Modifier.weight(1f))
                SettingValue("UDP Format", if (f124) "2024" else "2026", Modifier.weight(1f))
            }
        }
    }
}

@Composable
private fun SettingValue(label: String, value: String, modifier: Modifier) {
    Column(modifier.heightIn(min = 56.dp).padding(vertical = 6.dp), verticalArrangement = Arrangement.Center) {
        Text(label, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
        Text(value, style = MaterialTheme.typography.bodyLarge)
    }
}

private fun copyToClipboard(context: Context, label: String, text: String) {
    context.getSystemService(ClipboardManager::class.java)?.setPrimaryClip(ClipData.newPlainText(label, text))
}

// ── 5 Allow access ──────────────────────────────────────────────────────────

@Composable
private fun AccessStep(
    localNetworkAllowed: Boolean,
    notificationsAllowed: Boolean,
    onNoThanks: () -> Unit,
    onAllow: () -> Unit,
) {
    SetupPage(
        icon = R.drawable.ic_verified_user,
        title = "Allow access",
        description = "Track N Race needs these to receive the game on this phone.",
        secondary = "No thanks",
        onSecondary = onNoThanks,
        primary = "Allow",
        onPrimary = onAllow,
    ) {
        NotificationIllustration(Modifier.padding(start = 24.dp, end = 24.dp, top = 28.dp))
        Spacer(Modifier.height(16.dp))
        SetupRow(
            title = "Local network",
            supporting = "Lets the game’s packets reach this phone. Required.",
            leading = R.drawable.ic_wifi,
            trailing = if (localNetworkAllowed) {
                { AllowedMark() }
            } else null,
        )
        SetupRow(
            title = "Notifications",
            supporting = "Keeps the recorder running with the screen off. Recommended.",
            leading = R.drawable.ic_notifications,
            trailing = if (notificationsAllowed) {
                { AllowedMark() }
            } else null,
        )
        InfoBox(
            lead = "Only for the recorder.",
            text = "The notification shows while the recorder is running. Track N Race has no notification service and will never send you any other notifications.",
        )
    }
}

@Composable
private fun AllowedMark() {
    Text("Allowed", style = MaterialTheme.typography.labelLarge, color = MaterialTheme.colorScheme.primary)
}

/** What the recorder's notification looks like. */
@Composable
private fun NotificationIllustration(modifier: Modifier) {
    val colors = MaterialTheme.colorScheme
    Box(
        modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(28.dp))
            .background(colors.surfaceContainerLow)
            .padding(horizontal = 20.dp, vertical = 28.dp)
            .semantics(mergeDescendants = true) {},
    ) {
        Row(
            Modifier
                .fillMaxWidth()
                .clip(RoundedCornerShape(20.dp))
                .background(colors.surfaceContainerHigh)
                .padding(horizontal = 16.dp, vertical = 14.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Box(
                Modifier.size(32.dp).clip(CircleShape).background(colorResource(R.color.ic_launcher_background)),
                contentAlignment = Alignment.Center,
            ) {
                Image(painterResource(R.mipmap.ic_launcher_foreground), contentDescription = null, modifier = Modifier.size(48.dp))
            }
            Column {
                Text("Track N Race · now", style = MaterialTheme.typography.bodySmall, color = colors.onSurfaceVariant)
                Text("Recorder running", style = MaterialTheme.typography.bodyLarge)
            }
        }
    }
}

// ── 6 Waiting for the game / your PC ────────────────────────────────────────

@Composable
private fun CheckStep(
    store: TelemetryStore,
    paired: Boolean,
    onGameSettings: () -> Unit,
    onWifiSettings: () -> Unit,
    onSkip: () -> Unit,
    onNext: () -> Unit,
) {
    val settings = store.settings
    val status = store.sourceStatus
    // The session row names the track: telemetry is coming in. Only these
    // fields are read, so the lap clock does not recompose the step.
    val session by remember(store) {
        derivedStateOf(structuralEqualityPolicy()) {
            val cold = store.cold
            if (cold.trackId < 0) null else ArrivedSession(
                track = TrackNames[cold.trackId] ?: "Track ${cold.trackId}",
                session = sessionName(cold.sessionType, cold.labels),
                format = cold.protocolYear,
            )
        }
    }
    val arrived = session
    val address = rememberDeviceAddresses().firstOrNull()

    val title = when {
        arrived != null && paired -> "Your PC is sending"
        arrived != null -> "Telemetry is coming in"
        paired -> "Waiting for your PC"
        else -> "Waiting for the game"
    }
    val description = when {
        arrived != null && paired -> "This phone is showing the same session as your PC. You’re ready to go."
        arrived != null -> "Track N Race can see your session. You’re ready to go."
        paired -> "Start a session in the game, or play a recording on your PC. This moves on by itself."
        else -> "Start any session in the game: practice, time trial or race. This moves on by itself."
    }

    SetupPage(
        icon = if (paired) R.drawable.ic_desktop else R.drawable.ic_sensors,
        title = title,
        description = description,
        secondary = if (arrived == null) "Skip" else null,
        onSecondary = onSkip,
        primary = "Next",
        onPrimary = onNext,
        primaryEnabled = arrived != null,
    ) {
        val statusText = when {
            arrived != null -> "Receiving ${arrived.track}"
            status.state == "error" -> status.detail ?: "Not receiving"
            paired -> "Connected to ${settings.desktopName}"
            else -> "Listening on ${address ?: "this phone"} : ${settings.udpPort}"
        }
        Column(
            Modifier
                .padding(start = 24.dp, end = 24.dp, top = 28.dp)
                .fillMaxWidth()
                .height(252.dp)
                .clip(RoundedCornerShape(28.dp))
                .background(MaterialTheme.colorScheme.surfaceContainerLow)
                .semantics {
                    liveRegion = LiveRegionMode.Polite
                    contentDescription = statusText
                },
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.Center,
        ) {
            if (arrived != null) ArrivedBadge() else MorphingIndicator()
            Spacer(Modifier.height(22.dp))
            when {
                arrived != null -> {
                    Text(
                        if (paired) "Live from ${settings.desktopName}" else arrived.format?.let { "Format $it" } ?: "From the game",
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    Text(
                        listOfNotNull(arrived.track, arrived.session).joinToString(" · "),
                        style = MaterialTheme.typography.titleLarge,
                    )
                }
                status.state == "error" -> Text(
                    status.detail ?: "Not receiving",
                    modifier = Modifier.padding(horizontal = 24.dp),
                    style = MaterialTheme.typography.bodyLarge,
                    color = MaterialTheme.colorScheme.error,
                    textAlign = TextAlign.Center,
                )
                paired -> {
                    Text(
                        if (status.state == "connected") "Connected to" else "Connecting to",
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    Text(settings.desktopName, style = MaterialTheme.typography.titleLarge)
                }
                else -> {
                    Text("Listening on", style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    Text("${address ?: "this phone"} : ${settings.udpPort}", style = MaterialTheme.typography.titleLarge)
                }
            }
        }

        if (arrived == null) {
            SectionLabel("Nothing yet?")
            if (paired) {
                SetupRow(
                    title = "The game sends to your PC",
                    supporting = "The game’s UDP IP should be your PC’s address, not this phone’s.",
                    leading = R.drawable.ic_videogame,
                )
                SetupRow(
                    title = "No game running?",
                    supporting = "Play a recording on your PC to test the link.",
                    leading = R.drawable.ic_play,
                )
            } else {
                SetupRow(
                    title = "Check the game’s settings",
                    supporting = "UDP IP ${address ?: "—"} · Port ${settings.udpPort}",
                    leading = R.drawable.ic_videogame,
                    trailing = { Icon(painterResource(R.drawable.ic_chevron_right), contentDescription = null) },
                    onClick = onGameSettings,
                )
                SetupRow(
                    title = "Same network as the game",
                    supporting = if (address != null) "This phone is at $address" else "This phone isn’t on Wi-Fi",
                    leading = R.drawable.ic_wifi,
                    trailing = { Icon(painterResource(R.drawable.ic_chevron_right), contentDescription = null) },
                    onClick = onWifiSettings,
                )
            }
        } else {
            Row(Modifier.padding(start = 24.dp, end = 24.dp, top = 20.dp)) {
                if (paired) {
                    SettingValue("PC", settings.desktopName, Modifier.weight(1f))
                } else {
                    SettingValue("Format", arrived.format?.toString() ?: "—", Modifier.weight(1f))
                }
                SettingValue("Session", arrived.session ?: "—", Modifier.weight(1f))
            }
        }
    }
}

private data class ArrivedSession(val track: String, val session: String?, val format: Int?)

/** A shape that morphs as it turns, on a soft ripple: M3 Expressive's loading indicator. */
@Composable
private fun MorphingIndicator() {
    val transition = rememberInfiniteTransition(label = "Setup indicator")
    val rotation by transition.animateFloat(
        initialValue = 0f,
        targetValue = 360f,
        animationSpec = infiniteRepeatable(tween(2400, easing = LinearEasing)),
        label = "Rotation",
    )
    val corner by transition.animateFloat(
        initialValue = 50f,
        targetValue = 22f,
        animationSpec = infiniteRepeatable(tween(1200, easing = FastOutSlowInEasing), RepeatMode.Reverse),
        label = "Corner",
    )
    val ripple by transition.animateFloat(
        initialValue = 0f,
        targetValue = 1f,
        animationSpec = infiniteRepeatable(tween(2000, easing = LinearOutSlowInEasing)),
        label = "Ripple",
    )
    val container = MaterialTheme.colorScheme.primaryContainer
    val shape = MaterialTheme.colorScheme.primary
    Box(Modifier.size(112.dp), contentAlignment = Alignment.Center) {
        Box(
            Modifier
                .size(112.dp)
                .graphicsLayer {
                    val scale = 0.7f + 0.75f * ripple
                    scaleX = scale
                    scaleY = scale
                    alpha = 0.55f * (1f - ripple)
                }
                .background(container, CircleShape),
        )
        Box(Modifier.size(112.dp).background(container, CircleShape))
        Box(
            Modifier
                .size(52.dp)
                .graphicsLayer {
                    rotationZ = rotation
                    val scale = 0.86f + 0.14f * ((corner - 22f) / 28f)
                    scaleX = scale
                    scaleY = scale
                }
                .background(shape, RoundedCornerShape(corner.roundToInt())),
        )
    }
}

@Composable
private fun ArrivedBadge() {
    val dark = MaterialTheme.colorScheme.surface.luminance() < 0.5f
    val fill = if (dark) Color(0xFF8FD6A0) else Color(0xFF1E6B3A)
    val mark = if (dark) Color(0xFF0E2A18) else Color.White
    Box(Modifier.size(112.dp).background(fill, CookieShape), contentAlignment = Alignment.Center) {
        Icon(painterResource(R.drawable.ic_check), contentDescription = null, tint = mark, modifier = Modifier.size(48.dp))
    }
}

// ── 7 Save your sessions? ───────────────────────────────────────────────────

@Composable
private fun RecordingsStep(
    enabled: Boolean,
    folder: String,
    onEnabled: (Boolean) -> Unit,
    onFolder: () -> Unit,
    onSkip: () -> Unit,
    onNext: () -> Unit,
) {
    SetupPage(
        icon = R.drawable.ic_recording,
        title = "Save your sessions?",
        description = "Recordings let you replay a session and compare laps in Analysis later.",
        secondary = "Skip",
        onSecondary = onSkip,
        primary = "Next",
        onPrimary = onNext,
    ) {
        Spacer(Modifier.height(24.dp))
        ListItem(
            headlineContent = { Text("Record sessions") },
            supportingContent = { Text("Saves complete sessions as TNRD files") },
            trailingContent = { Switch(checked = enabled, onCheckedChange = null) },
            colors = ListItemDefaults.colors(containerColor = Color.Transparent),
            modifier = Modifier
                .toggleable(value = enabled, role = Role.Switch, onValueChange = onEnabled)
                .padding(horizontal = 8.dp),
        )
        ListItem(
            headlineContent = { Text("Save to") },
            supportingContent = { Text(folder) },
            trailingContent = { Icon(painterResource(R.drawable.ic_chevron_right), contentDescription = null) },
            colors = ListItemDefaults.colors(containerColor = Color.Transparent),
            modifier = Modifier
                .clickable(enabled = enabled, onClick = onFolder)
                .graphicsLayer { alpha = if (enabled) 1f else 0.38f }
                .padding(horizontal = 8.dp),
        )
        Spacer(Modifier.height(8.dp))
        InfoBox(
            lead = "Only in Direct mode.",
            text = "This phone records when the game sends straight to it. In Paired mode nothing is saved on this phone; your PC records the session.",
        )
        Text(
            "You can change this later in Settings › Recording.",
            modifier = Modifier.padding(start = 24.dp, end = 24.dp, top = 8.dp),
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

// ── 8 You're all set ────────────────────────────────────────────────────────

@Composable
private fun DoneStep(
    theme: String,
    connection: String,
    recordings: String,
    onTheme: () -> Unit,
    onConnection: () -> Unit,
    onRecordings: () -> Unit,
    onStart: () -> Unit,
) {
    Column(Modifier.fillMaxSize()) {
        Column(
            Modifier
                .weight(1f)
                .fillMaxWidth()
                .verticalScroll(rememberScrollState()),
        ) {
            Column(Modifier.padding(start = 24.dp, end = 24.dp, top = 36.dp)) {
                Box(
                    Modifier.size(96.dp).background(MaterialTheme.colorScheme.primaryContainer, CookieShape),
                    contentAlignment = Alignment.Center,
                ) {
                    Icon(
                        painterResource(R.drawable.ic_check),
                        contentDescription = null,
                        tint = MaterialTheme.colorScheme.onPrimaryContainer,
                        modifier = Modifier.size(40.dp),
                    )
                }
                Spacer(Modifier.height(24.dp))
                Text("You’re all set", style = MaterialTheme.typography.headlineLarge)
                Spacer(Modifier.height(16.dp))
                Text(
                    "Here’s what you chose. You can change any of it in Settings.",
                    style = MaterialTheme.typography.bodyLarge,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            Spacer(Modifier.height(24.dp))
            SummaryRow("Theme", theme, R.drawable.ic_brightness_medium, onTheme)
            SummaryRow("Connection", connection, R.drawable.ic_link, onConnection)
            SummaryRow("Recordings", recordings, R.drawable.ic_recording, onRecordings)
        }
        SetupFooter(null, {}, "Start", onStart, true)
    }
}

@Composable
private fun SummaryRow(title: String, value: String, icon: Int, onChange: () -> Unit) {
    SetupRow(
        title = title,
        supporting = value,
        leading = icon,
        trailing = { TextButton(onClick = onChange) { Text("Change") } },
    )
}
