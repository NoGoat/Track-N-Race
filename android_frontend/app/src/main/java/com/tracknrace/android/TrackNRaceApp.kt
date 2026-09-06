package com.tracknrace.android

import android.content.res.Configuration
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInVertically
import androidx.compose.animation.slideOutVertically
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilterChip
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.ListItem
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.NavigationBarItemDefaults
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.withFrameNanos
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.input.pointer.PointerEventPass
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.journeyapps.barcodescanner.ScanContract
import kotlinx.coroutines.delay
import kotlin.math.roundToInt

private enum class AppScreen { DASHBOARD, TYRES, SETTINGS, PAIRING, LICENSES }
private enum class TyreColumn { SET, COMPOUND, STATUS, WEAR, LIFE, RECOMMENDED, DELTA }

@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun TrackNRaceApp(telemetry: TelemetryController) {
    val store = telemetry.store
    val context = LocalContext.current
    var screen by rememberSaveable { mutableStateOf(AppScreen.DASHBOARD) }
    var settingsReturn by rememberSaveable { mutableStateOf(AppScreen.DASHBOARD) }
    val snackbar = remember { SnackbarHostState() }
    val message = store.message
    val landscape = LocalConfiguration.current.orientation == Configuration.ORIENTATION_LANDSCAPE
    var landscapeChromeVisible by remember { mutableStateOf(false) }
    var landscapeChromeInteraction by remember { mutableIntStateOf(0) }

    val directoryLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.StartActivityForResult(),
    ) { result ->
        val data = result.data
        data?.data?.let { uri -> telemetry.acceptRecordingDirectory(uri, data.flags) }
    }
    val qrLauncher = rememberLauncherForActivityResult(ScanContract()) { result ->
        result.contents?.takeIf(String::isNotEmpty)?.let(telemetry::pairQr)
    }

    LaunchedEffect(message?.id) {
        message?.let { snackbar.showSnackbar(it.text) }
    }
    LaunchedEffect(landscape) {
        if (landscape) landscapeChromeVisible = false
    }
    LaunchedEffect(landscape, landscapeChromeVisible, landscapeChromeInteraction) {
        if (landscape && landscapeChromeVisible) {
            delay(3_000)
            landscapeChromeVisible = false
        }
    }

    fun navigateBack() {
        screen = when (screen) {
            AppScreen.PAIRING, AppScreen.LICENSES -> AppScreen.SETTINGS
            AppScreen.SETTINGS -> settingsReturn
            else -> screen
        }
    }

    val isPrimary = screen == AppScreen.DASHBOARD || screen == AppScreen.TYRES
    val openSettings = {
        settingsReturn = if (isPrimary) screen else AppScreen.DASHBOARD
        screen = AppScreen.SETTINGS
    }
    BackHandler(enabled = !isPrimary) {
        navigateBack()
    }

    Scaffold(
        topBar = {
            if (!landscape) {
                AppTopBar(
                    screen = screen,
                    onSettings = openSettings,
                    onBack = ::navigateBack,
                )
            }
        },
        bottomBar = {
            if (!landscape && isPrimary) {
                AppNavigationBar(selected = screen, onSelected = { screen = it })
            }
        },
        snackbarHost = { SnackbarHost(snackbar) },
        containerColor = MaterialTheme.colorScheme.background,
    ) { padding ->
        val revealChromeModifier = if (landscape) {
            Modifier.pointerInput(Unit) {
                awaitPointerEventScope {
                    var trackedPointerId: Long? = null
                    var downPosition = Offset.Zero
                    var movedBeyondTapSlop = false
                    while (true) {
                        val event = awaitPointerEvent(PointerEventPass.Initial)
                        event.changes.forEach { change ->
                            if (trackedPointerId == null && change.pressed && !change.previousPressed) {
                                trackedPointerId = change.id.value
                                downPosition = change.position
                                movedBeyondTapSlop = false
                            }
                            if (change.id.value == trackedPointerId) {
                                if ((change.position - downPosition).getDistance() > viewConfiguration.touchSlop) {
                                    movedBeyondTapSlop = true
                                }
                                if (!change.pressed && change.previousPressed) {
                                    if (!movedBeyondTapSlop) {
                                        landscapeChromeVisible = true
                                        landscapeChromeInteraction += 1
                                    }
                                    trackedPointerId = null
                                }
                            }
                        }
                    }
                }
            }
        } else {
            Modifier
        }
        Box(Modifier.fillMaxSize().then(revealChromeModifier)) {
            // Page content consumes Scaffold's safe-area padding. Landscape
            // app-bar surfaces are separate siblings, so their backgrounds
            // can still draw edge-to-edge behind the cutout.
            Box(Modifier.fillMaxSize().padding(padding)) {
                when (screen) {
                    AppScreen.DASHBOARD -> DashboardScreen(store)
                    AppScreen.TYRES -> TyresScreen(store.cold)
                    AppScreen.SETTINGS -> SettingsScreen(
                        settings = store.settings,
                        onSource = telemetry::setSource,
                        onPairing = { screen = AppScreen.PAIRING },
                        onRecording = telemetry::setRecording,
                        onChooseDirectory = { directoryLauncher.launch(telemetry.recordingDirectoryIntent()) },
                        onDefaultDirectory = telemetry::useDefaultRecordingDirectory,
                        onForgetDesktop = telemetry::forgetDesktop,
                        onLicenses = { screen = AppScreen.LICENSES },
                    )
                    AppScreen.PAIRING -> PairingScreen(
                        telemetry = telemetry,
                        store = store,
                        onScanQr = { qrLauncher.launch(telemetry.qrScanOptions()) },
                        onDone = { screen = AppScreen.SETTINGS },
                    )
                    AppScreen.LICENSES -> LicensesScreen()
                }

                val disconnected = store.settings.source == PairedTelemetryClient.SOURCE_PAIRED &&
                    (store.sourceStatus.state == "error" || store.sourceStatus.state == "disconnected")
                if (disconnected && screen != AppScreen.PAIRING) {
                    Surface(
                        modifier = Modifier.align(Alignment.BottomCenter).padding(12.dp),
                        shape = MaterialTheme.shapes.large,
                        color = MaterialTheme.colorScheme.errorContainer,
                        tonalElevation = 6.dp,
                    ) {
                        Row(
                            modifier = Modifier.padding(start = 16.dp, end = 8.dp, top = 6.dp, bottom = 6.dp),
                            verticalAlignment = Alignment.CenterVertically,
                        ) {
                            Text(
                                store.sourceStatus.detail ?: "Desktop disconnected",
                                modifier = Modifier.weight(1f),
                                color = MaterialTheme.colorScheme.onErrorContainer,
                                maxLines = 2,
                            )
                            TextButton(onClick = telemetry::reconnect) { Text("Reconnect") }
                        }
                    }
                }
            }

            if (landscape) {
                AnimatedVisibility(
                    visible = landscapeChromeVisible,
                    modifier = Modifier.align(Alignment.TopCenter),
                    enter = fadeIn() + slideInVertically { -it },
                    exit = fadeOut() + slideOutVertically { -it },
                ) {
                    AppTopBar(
                        screen = screen,
                        onSettings = openSettings,
                        onBack = ::navigateBack,
                    )
                }
                if (isPrimary) {
                    AnimatedVisibility(
                        visible = landscapeChromeVisible,
                        modifier = Modifier.align(Alignment.BottomCenter),
                        enter = fadeIn() + slideInVertically { it },
                        exit = fadeOut() + slideOutVertically { it },
                    ) {
                        AppNavigationBar(selected = screen, onSelected = { screen = it })
                    }
                }
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun AppTopBar(
    screen: AppScreen,
    onSettings: () -> Unit,
    onBack: () -> Unit,
) {
    val primary = screen == AppScreen.DASHBOARD || screen == AppScreen.TYRES
    var overflowOpen by remember { mutableStateOf(false) }
    val title = when (screen) {
        AppScreen.DASHBOARD -> "Dashboard"
        AppScreen.TYRES -> "Tyres"
        AppScreen.SETTINGS -> "Settings"
        AppScreen.PAIRING -> "Pair desktop"
        AppScreen.LICENSES -> "Open-source licences"
    }
    TopAppBar(
        colors = TopAppBarDefaults.topAppBarColors(
            containerColor = MaterialTheme.colorScheme.surfaceContainer,
        ),
        navigationIcon = {
            if (!primary) {
                IconButton(onClick = onBack) {
                    Icon(
                        painterResource(R.drawable.ic_arrow_back),
                        contentDescription = "Back",
                    )
                }
            }
        },
        title = {
            Text(
                title,
                style = MaterialTheme.typography.titleLarge,
                fontWeight = FontWeight.SemiBold,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
        },
        actions = {
            if (primary) {
                Box {
                    IconButton(onClick = { overflowOpen = true }) {
                        Icon(painterResource(R.drawable.ic_more), contentDescription = "More options")
                    }
                    DropdownMenu(
                        expanded = overflowOpen,
                        onDismissRequest = { overflowOpen = false },
                    ) {
                        DropdownMenuItem(
                            text = { Text("Settings") },
                            leadingIcon = {
                                Icon(painterResource(R.drawable.ic_settings), contentDescription = null)
                            },
                            onClick = {
                                overflowOpen = false
                                onSettings()
                            },
                        )
                    }
                }
            }
        },
    )
}

@Composable
private fun AppNavigationBar(selected: AppScreen, onSelected: (AppScreen) -> Unit) {
    val destinations = listOf(
        Triple(AppScreen.DASHBOARD, "Dashboard", R.drawable.ic_overview),
        Triple(AppScreen.TYRES, "Tyres", R.drawable.ic_tyres),
    )
    NavigationBar(
        containerColor = MaterialTheme.colorScheme.surfaceContainer,
        tonalElevation = 3.dp,
    ) {
        destinations.forEach { (screen, label, icon) ->
            NavigationBarItem(
                selected = selected == screen,
                onClick = { onSelected(screen) },
                icon = { Icon(painterResource(icon), contentDescription = null) },
                label = { Text(label, maxLines = 1) },
                alwaysShowLabel = true,
                colors = NavigationBarItemDefaults.colors(
                    selectedIconColor = MaterialTheme.colorScheme.onSecondaryContainer,
                    selectedTextColor = MaterialTheme.colorScheme.onSurface,
                    indicatorColor = MaterialTheme.colorScheme.secondaryContainer,
                ),
            )
        }
    }
}

@Composable
private fun DashboardScreen(store: TelemetryStore) {
    val hot = rememberFrameSample(store)
    val cold = store.cold
    val landscape = LocalConfiguration.current.orientation == Configuration.ORIENTATION_LANDSCAPE
    Surface(color = Color(0xff07090c), modifier = Modifier.fillMaxSize()) {
        Column(
            Modifier.fillMaxSize().padding(if (landscape) 10.dp else 8.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            RpmLights(hot.revLightsBitValue, Modifier.fillMaxWidth().height(18.dp))
            if (landscape) {
                LandscapeDashboard(hot, cold, Modifier.weight(1f))
            } else {
                PortraitDashboard(hot, cold, Modifier.weight(1f))
            }
        }
    }
}

@Composable
private fun rememberFrameSample(store: TelemetryStore): HotTelemetry {
    var displayed by remember(store) { mutableStateOf(store.latestHot()) }
    LaunchedEffect(store) {
        while (true) {
            withFrameNanos { }
            val latest = store.latestHot()
            if (latest != displayed) displayed = latest
        }
    }
    return displayed
}

@Composable
private fun RpmLights(bitValue: Int?, modifier: Modifier = Modifier) {
    val off = Color(0xff26313b)
    val green = Color(0xff32d583)
    val red = Color(0xffff4d5e)
    val purple = Color(0xffc875ff)
    Canvas(modifier.semantics { contentDescription = "RPM shift lights" }) {
        val gap = 5.dp.toPx()
        val lightWidth = (size.width - gap * 14) / 15
        val radius = CornerRadius(4.dp.toPx())
        repeat(15) { index ->
            val lit = bitValue != null && bitValue and (1 shl index) != 0
            val active = when {
                index < 5 -> green
                index < 10 -> red
                else -> purple
            }
            drawRoundRect(
                color = if (lit) active else off,
                topLeft = Offset(index * (lightWidth + gap), 0f),
                size = Size(lightWidth, size.height),
                cornerRadius = radius,
            )
        }
    }
}

@Composable
private fun LandscapeDashboard(hot: HotTelemetry, cold: DashboardColdState, modifier: Modifier) {
    Row(modifier, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
        Column(Modifier.weight(0.72f).fillMaxHeight(), verticalArrangement = Arrangement.spacedBy(10.dp)) {
            MetricCard("POSITION", if (cold.position > 0) "P${cold.position}" else "—", Color(0xff43a5ff), Modifier.weight(1f))
            MetricCard("LAP", lapLabel(cold), Color.White, Modifier.weight(1f))
            MetricCard(if (cold.lapInvalid) "CURRENT • INVALID" else "CURRENT LAP", formatTime(cold.currentLapMs), if (cold.lapInvalid) Color(0xffff4d5e) else Color.White, Modifier.weight(1f))
        }
        CenterReadout(hot, cold, Modifier.weight(1.65f).fillMaxHeight())
        Column(Modifier.weight(0.72f).fillMaxHeight(), verticalArrangement = Arrangement.spacedBy(10.dp)) {
            MetricCard("ERS", "${cold.ersPercent}%", if (cold.ersPercent < 20) Color(0xfff5b942) else Color(0xff32d583), Modifier.weight(1f))
            MetricCard("FUEL", if (cold.fuelLaps > 0) "%.1f LAPS".format(cold.fuelLaps) else "—", Color.White, Modifier.weight(1f))
            MetricCard("LAST LAP", formatTime(cold.lastLapMs), Color.White, Modifier.weight(1f))
        }
    }
}

@Composable
private fun PortraitDashboard(hot: HotTelemetry, cold: DashboardColdState, modifier: Modifier) {
    Column(modifier, verticalArrangement = Arrangement.spacedBy(8.dp)) {
        CenterReadout(hot, cold, Modifier.fillMaxWidth().weight(1.8f))
        Row(
            Modifier.fillMaxWidth().weight(1f),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            MetricCard("POSITION", if (cold.position > 0) "P${cold.position}" else "—", Color(0xff43a5ff), Modifier.weight(1f).fillMaxHeight())
            MetricCard("LAP", lapLabel(cold), Color.White, Modifier.weight(1f).fillMaxHeight())
        }
        Row(
            Modifier.fillMaxWidth().weight(1f),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            MetricCard("ERS", "${cold.ersPercent}%", Color(0xff32d583), Modifier.weight(1f).fillMaxHeight())
            MetricCard("FUEL", if (cold.fuelLaps > 0) "%.1f LAPS".format(cold.fuelLaps) else "—", Color.White, Modifier.weight(1f).fillMaxHeight())
        }
        Row(
            Modifier.fillMaxWidth().weight(1f),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            MetricCard("CURRENT LAP", formatTime(cold.currentLapMs), if (cold.lapInvalid) Color(0xffff4d5e) else Color.White, Modifier.weight(1f).fillMaxHeight())
            MetricCard("LAST LAP", formatTime(cold.lastLapMs), Color.White, Modifier.weight(1f).fillMaxHeight())
        }
    }
}

@Composable
private fun CenterReadout(hot: HotTelemetry, cold: DashboardColdState, modifier: Modifier = Modifier) {
    val landscape = LocalConfiguration.current.orientation == Configuration.ORIENTATION_LANDSCAPE
    val gearSize = if (landscape) 82.sp else 70.sp
    val speedSize = if (landscape) 52.sp else 42.sp
    Card(
        modifier,
        colors = CardDefaults.cardColors(containerColor = Color(0xff10151b)),
        border = CardDefaults.outlinedCardBorder().copy(width = 1.dp),
    ) {
        Column(
            Modifier.fillMaxSize().padding(if (landscape) 16.dp else 12.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.SpaceBetween,
        ) {
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                DashboardPill(
                    label = cold.labels["ui.overview.drs"] ?: cold.aeroMode.uppercase(),
                    active = if (cold.aeroMode == "slm") hot.slm > 0 else hot.drs > 0,
                )
                DashboardPill(tyreLabel(cold), active = false, tone = tyreTone(cold.tyreCompound))
            }
            Row(verticalAlignment = Alignment.Bottom, horizontalArrangement = Arrangement.Center) {
                Text(
                    gearLabel(hot.gear),
                    color = Color.White,
                    fontFamily = FontFamily.Monospace,
                    fontWeight = FontWeight.Black,
                    fontSize = gearSize,
                    lineHeight = gearSize,
                )
                Spacer(Modifier.width(if (landscape) 24.dp else 16.dp))
                Column(horizontalAlignment = Alignment.End) {
                    Text(
                        hot.speedKph.toString(),
                        color = Color.White,
                    fontFamily = FontFamily.Monospace,
                    fontWeight = FontWeight.Bold,
                    fontSize = speedSize,
                    lineHeight = speedSize,
                    )
                    Text(
                        "KM/H  •  ${hot.rpm} RPM",
                        color = Color(0xff8f9aa6),
                        fontFamily = FontFamily.Monospace,
                        fontWeight = FontWeight.SemiBold,
                        fontSize = 12.sp,
                    )
                }
            }
            Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
                PedalBar("BRK", hot.brake, Color(0xffff4d5e))
                PedalBar("THR", hot.throttle, Color(0xff32d583))
            }
        }
    }
}

@Composable
private fun DashboardPill(label: String, active: Boolean, tone: Color = Color(0xff8f9aa6)) {
    val color = if (active) Color(0xff32d583) else tone
    Surface(color = color.copy(alpha = 0.17f), shape = MaterialTheme.shapes.extraLarge) {
        Text(
            label,
            modifier = Modifier.padding(horizontal = 12.dp, vertical = 6.dp),
            color = color,
            fontWeight = FontWeight.Bold,
            fontSize = 12.sp,
        )
    }
}

@Composable
private fun PedalBar(label: String, value: Float, color: Color) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Text(label, color = Color(0xff8f9aa6), fontWeight = FontWeight.Bold, fontSize = 11.sp, modifier = Modifier.width(34.dp))
        LinearProgressIndicator(
            progress = { value.coerceIn(0f, 1f) },
            modifier = Modifier.fillMaxWidth().height(8.dp).clip(MaterialTheme.shapes.extraLarge),
            color = color,
            trackColor = Color(0xff26313b),
            strokeCap = StrokeCap.Round,
        )
    }
}

@Composable
private fun MetricCard(label: String, value: String, valueColor: Color, modifier: Modifier = Modifier) {
    Card(
        modifier,
        colors = CardDefaults.cardColors(containerColor = Color(0xff10151b)),
    ) {
        Column(
            Modifier.fillMaxSize().padding(10.dp),
            verticalArrangement = Arrangement.SpaceBetween,
        ) {
            Text(label, color = Color(0xff8f9aa6), fontSize = 10.sp, fontWeight = FontWeight.Bold)
            Text(
                value,
                color = valueColor,
                fontFamily = FontFamily.Monospace,
                fontSize = 19.sp,
                fontWeight = FontWeight.Bold,
                maxLines = 1,
            )
        }
    }
}

@Composable
private fun TyresScreen(cold: DashboardColdState) {
    val dry = cold.tyreSets.filter { it.actualCompound !in setOf(7, 8) }
        .sortedWith(compareBy({ dryOrder(it.visualCompound) }, { it.index }))
    val wet = cold.tyreSets.filter { it.actualCompound in setOf(7, 8) }
        .sortedWith(compareBy({ wetOrder(it.actualCompound) }, { it.index }))
    BoxWithConstraints(Modifier.fillMaxSize()) {
        val showTable = maxWidth >= 600.dp
        LazyColumn(
            Modifier.fillMaxSize(),
            contentPadding = PaddingValues(
                horizontal = if (showTable) 16.dp else 12.dp,
                vertical = 12.dp,
            ),
            verticalArrangement = Arrangement.spacedBy(16.dp),
        ) {
            item {
                TyreTableSection(
                    title = "Dry sets (slicks)",
                    sets = dry,
                    cold = cold,
                    showTable = showTable,
                )
            }
            item {
                TyreTableSection(
                    title = "Wet / intermediate sets",
                    sets = wet,
                    cold = cold,
                    showTable = showTable,
                )
            }
        }
    }
}

@Composable
private fun TyreTableSection(
    title: String,
    sets: List<TyreSetEntry>,
    cold: DashboardColdState,
    showTable: Boolean,
) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        shape = MaterialTheme.shapes.large,
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.surfaceContainerLow,
        ),
    ) {
        Column {
            Row(
                modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 14.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(
                    title,
                    modifier = Modifier.weight(1f),
                    style = MaterialTheme.typography.titleSmall,
                    color = MaterialTheme.colorScheme.onSurface,
                    fontWeight = FontWeight.SemiBold,
                )
                Surface(
                    shape = MaterialTheme.shapes.extraLarge,
                    color = MaterialTheme.colorScheme.secondaryContainer,
                    contentColor = MaterialTheme.colorScheme.onSecondaryContainer,
                ) {
                    Text(
                        sets.size.toString(),
                        modifier = Modifier.padding(horizontal = 9.dp, vertical = 3.dp),
                        style = MaterialTheme.typography.labelMedium,
                        fontWeight = FontWeight.SemiBold,
                    )
                }
            }
            HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
            if (sets.isEmpty()) {
                Text(
                    "Waiting for tyre allocation data…",
                    modifier = Modifier.fillMaxWidth().padding(horizontal = 20.dp, vertical = 28.dp),
                    textAlign = TextAlign.Center,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    style = MaterialTheme.typography.bodyMedium,
                )
            } else if (showTable) {
                TyreTableHeader()
                sets.forEach { set ->
                    HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
                    TyreTableRow(set, cold)
                }
            } else {
                sets.forEachIndexed { index, set ->
                    if (index > 0) {
                        HorizontalDivider(
                            color = MaterialTheme.colorScheme.outlineVariant,
                        )
                    }
                    CompactTyreRow(set, cold)
                }
            }
        }
    }
}

@Composable
private fun TyreTableHeader() {
    Row(
        modifier = Modifier.fillMaxWidth().padding(horizontal = 10.dp, vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        TyreTableText("#", tyreColumnModifier(TyreColumn.SET), header = true)
        TyreTableText("Tyre", tyreColumnModifier(TyreColumn.COMPOUND), header = true)
        TyreTableText("Status", tyreColumnModifier(TyreColumn.STATUS), header = true)
        TyreTableText("Wear", tyreColumnModifier(TyreColumn.WEAR), header = true)
        TyreTableText("Life", tyreColumnModifier(TyreColumn.LIFE), header = true)
        TyreTableText("Rec", tyreColumnModifier(TyreColumn.RECOMMENDED), header = true)
        TyreTableText("Δ lap", tyreColumnModifier(TyreColumn.DELTA), header = true, align = TextAlign.End)
    }
}

@Composable
private fun CompactTyreRow(set: TyreSetEntry, cold: DashboardColdState) {
    val status = tyreStatus(set, cold.sessionType)
    val compound = cold.labels["tyre.actual.${set.actualCompound}"] ?: compoundLabel(set.actualCompound)
    val wearColor = tyreWearColor(set.wear)
    val primaryText = MaterialTheme.colorScheme.onSurface
    val secondaryText = MaterialTheme.colorScheme.onSurfaceVariant
    Surface(
        color = Color.Transparent,
        contentColor = primaryText,
    ) {
        Row(
            modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 13.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            CompactTyreIdentity(
                label = compound,
                accent = compoundColor(set.visualCompound),
                setNumber = set.index + 1,
                status = status,
                numberColor = secondaryText,
            )
            Column(modifier = Modifier.weight(1f)) {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text(
                        "${set.lifeSpan}/${set.usableLife} L  ·  Rec ${sessionLabel(set.recommendedSession)}",
                        modifier = Modifier.weight(1f),
                        color = secondaryText,
                        style = MaterialTheme.typography.bodySmall,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                    Spacer(Modifier.width(8.dp))
                    Text(
                        lapDelta(set),
                        color = tyreDeltaColor(set),
                        style = MaterialTheme.typography.titleSmall,
                        fontWeight = FontWeight.SemiBold,
                    )
                    Spacer(Modifier.width(6.dp))
                    Text(
                        "Δ lap",
                        color = secondaryText,
                        style = MaterialTheme.typography.labelSmall,
                    )
                }
                Spacer(Modifier.height(5.dp))
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text("Wear", color = secondaryText, style = MaterialTheme.typography.labelSmall)
                    Spacer(Modifier.weight(1f))
                    Text(
                        "${set.wear.roundToInt()}%",
                        color = wearColor,
                        style = MaterialTheme.typography.labelMedium,
                        fontWeight = FontWeight.SemiBold,
                    )
                }
                Spacer(Modifier.height(4.dp))
                LinearProgressIndicator(
                    progress = { (set.wear / 100f).coerceIn(0f, 1f) },
                    modifier = Modifier.fillMaxWidth().height(6.dp).clip(MaterialTheme.shapes.extraLarge),
                    color = wearColor,
                    trackColor = MaterialTheme.colorScheme.surfaceContainerHighest,
                    strokeCap = StrokeCap.Round,
                )
            }
        }
    }
}

@Composable
private fun TyreTableRow(set: TyreSetEntry, cold: DashboardColdState) {
    val status = tyreStatus(set, cold.sessionType)
    val compound = cold.labels["tyre.actual.${set.actualCompound}"] ?: compoundLabel(set.actualCompound)
    val wearColor = tyreWearColor(set.wear)
    val rowTextColor = MaterialTheme.colorScheme.onSurface
    Surface(color = Color.Transparent, contentColor = rowTextColor) {
        Row(
            modifier = Modifier.fillMaxWidth().height(48.dp).padding(horizontal = 10.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            TyreTableText("${set.index + 1}", tyreColumnModifier(TyreColumn.SET), color = rowTextColor)
            TyreTableText(
                compound,
                tyreColumnModifier(TyreColumn.COMPOUND),
                color = compoundColor(set.visualCompound),
                weight = FontWeight.Black,
            )
            Box(tyreColumnModifier(TyreColumn.STATUS), contentAlignment = Alignment.Center) {
                TyreStatusPill(status)
            }
            Row(
                modifier = tyreColumnModifier(TyreColumn.WEAR).padding(horizontal = 8.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                LinearProgressIndicator(
                    progress = { (set.wear / 100f).coerceIn(0f, 1f) },
                    modifier = Modifier.weight(1f).height(6.dp).clip(MaterialTheme.shapes.extraLarge),
                    color = wearColor,
                    trackColor = MaterialTheme.colorScheme.surfaceContainerHighest,
                    strokeCap = StrokeCap.Round,
                )
                Text(
                    "${set.wear.roundToInt()}%",
                    modifier = Modifier.width(40.dp),
                    color = wearColor,
                    style = MaterialTheme.typography.labelMedium,
                    fontWeight = FontWeight.SemiBold,
                    textAlign = TextAlign.End,
                )
            }
            TyreTableText("${set.lifeSpan}/${set.usableLife} L", tyreColumnModifier(TyreColumn.LIFE), color = rowTextColor)
            TyreTableText(sessionLabel(set.recommendedSession), tyreColumnModifier(TyreColumn.RECOMMENDED), color = rowTextColor)
            TyreTableText(
                lapDelta(set),
                tyreColumnModifier(TyreColumn.DELTA),
                color = tyreDeltaColor(set),
                weight = FontWeight.SemiBold,
                align = TextAlign.End,
            )
        }
    }
}

@Composable
private fun CompactTyreIdentity(
    label: String,
    accent: Color,
    setNumber: Int,
    status: String,
    numberColor: Color,
) {
    val compoundTextColor = when (accent) {
        Color.Unspecified -> MaterialTheme.colorScheme.primary
        Color(0xffeceff1) -> MaterialTheme.colorScheme.outline
        else -> accent
    }
    Column(
        modifier = Modifier.width(88.dp),
        verticalArrangement = Arrangement.spacedBy(2.dp),
    ) {
        Row(
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Text(
                setNumber.toString(),
                color = numberColor,
                style = MaterialTheme.typography.labelMedium,
                fontWeight = FontWeight.SemiBold,
            )
            Text(
                label,
                color = compoundTextColor,
                style = MaterialTheme.typography.titleSmall,
                fontWeight = FontWeight.Bold,
                maxLines = 1,
            )
        }
        Spacer(Modifier.height(2.dp))
        TyreStatusPill(status)
    }
}

@Composable
private fun TyreStatusPill(status: String) {
    val (containerColor, contentColor) = when (status) {
        "FITTED" -> MaterialTheme.colorScheme.primary to MaterialTheme.colorScheme.onPrimary
        "NEW" -> MaterialTheme.colorScheme.secondaryContainer to MaterialTheme.colorScheme.onSecondaryContainer
        "USED" -> MaterialTheme.colorScheme.primaryContainer to MaterialTheme.colorScheme.onPrimaryContainer
        else -> MaterialTheme.colorScheme.surfaceContainerHighest to MaterialTheme.colorScheme.onSurfaceVariant
    }
    Surface(
        modifier = Modifier.width(78.dp),
        shape = MaterialTheme.shapes.small,
        color = containerColor,
        contentColor = contentColor,
    ) {
        Text(
            status.lowercase().replaceFirstChar { it.titlecase() },
            modifier = Modifier.fillMaxWidth().padding(horizontal = 6.dp, vertical = 3.dp),
            style = MaterialTheme.typography.labelSmall,
            fontWeight = FontWeight.SemiBold,
            textAlign = TextAlign.Center,
            maxLines = 1,
        )
    }
}

@Composable
private fun tyreWearColor(wear: Float): Color = when {
    wear >= 75f -> MaterialTheme.colorScheme.error
    wear >= 50f -> MaterialTheme.colorScheme.tertiary
    else -> MaterialTheme.colorScheme.primary
}

@Composable
private fun tyreDeltaColor(set: TyreSetEntry): Color = when {
    set.lapDeltaMs > 0 -> MaterialTheme.colorScheme.error
    set.lapDeltaMs < 0 -> MaterialTheme.colorScheme.primary
    else -> MaterialTheme.colorScheme.onSurfaceVariant
}

private fun RowScope.tyreColumnModifier(column: TyreColumn): Modifier {
    return when (column) {
        TyreColumn.SET -> Modifier.width(40.dp)
        TyreColumn.COMPOUND -> Modifier.width(50.dp)
        TyreColumn.STATUS -> Modifier.width(104.dp)
        TyreColumn.WEAR -> Modifier.weight(1f)
        TyreColumn.LIFE -> Modifier.width(72.dp)
        TyreColumn.RECOMMENDED -> Modifier.width(58.dp)
        TyreColumn.DELTA -> Modifier.width(84.dp)
    }
}

@Composable
private fun TyreTableText(
    text: String,
    modifier: Modifier,
    header: Boolean = false,
    color: Color = Color.Unspecified,
    weight: FontWeight = FontWeight.Normal,
    align: TextAlign = TextAlign.Center,
) {
    Text(
        text,
        modifier = modifier.padding(horizontal = 2.dp),
        color = if (color == Color.Unspecified) {
            if (header) MaterialTheme.colorScheme.onSurfaceVariant else MaterialTheme.colorScheme.onSurface
        } else {
            color
        },
        style = (if (header) MaterialTheme.typography.labelSmall else MaterialTheme.typography.bodySmall).copy(
            fontFamily = FontFamily.SansSerif,
            fontFeatureSettings = "tnum",
        ),
        fontWeight = if (header) FontWeight.Bold else weight,
        textAlign = align,
        maxLines = 1,
        overflow = TextOverflow.Ellipsis,
    )
}

@Composable
private fun SettingsScreen(
    settings: AndroidSettings,
    onSource: (String) -> Unit,
    onPairing: () -> Unit,
    onRecording: (Boolean) -> Unit,
    onChooseDirectory: () -> Unit,
    onDefaultDirectory: () -> Unit,
    onForgetDesktop: () -> Unit,
    onLicenses: () -> Unit,
) {
    LazyColumn(
        Modifier.fillMaxSize(),
        contentPadding = PaddingValues(16.dp),
        verticalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        item {
            SettingsSection(
                title = "Telemetry source",
                supporting = "Choose where this phone receives its live timing data.",
            ) {
                Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                    FilterChip(
                        selected = settings.source == PairedTelemetryClient.SOURCE_DIRECT,
                        onClick = { onSource(PairedTelemetryClient.SOURCE_DIRECT) },
                        label = { Text("Direct UDP") },
                    )
                    FilterChip(
                        selected = settings.source == PairedTelemetryClient.SOURCE_PAIRED,
                        onClick = {
                            if (settings.hasSavedDesktop) onSource(PairedTelemetryClient.SOURCE_PAIRED)
                            else onPairing()
                        },
                        label = { Text("Paired desktop") },
                    )
                }
                HorizontalDivider()
                SettingsRow(
                    title = if (settings.hasSavedDesktop) settings.desktopName else "Pair a desktop",
                    supporting = if (settings.hasSavedDesktop) {
                        "Manage or replace the saved desktop"
                    } else {
                        "Receive live or playback telemetry from Track N Race desktop"
                    },
                    onClick = onPairing,
                )
                if (settings.hasSavedDesktop) {
                    TextButton(onClick = onForgetDesktop) {
                        Text("Forget paired desktop", color = MaterialTheme.colorScheme.error)
                    }
                }
            }
        }
        item {
            SettingsSection(
                title = "Recording",
                supporting = "Capture complete sessions as portable TNRD V5 files.",
            ) {
                ListItem(
                    headlineContent = { Text("Record telemetry sessions") },
                    supportingContent = {
                        Text(if (settings.recordingEnabled) "Recording is enabled" else "Off by default")
                    },
                    trailingContent = {
                        Switch(checked = settings.recordingEnabled, onCheckedChange = onRecording)
                    },
                    colors = ListItemDefaults.colors(containerColor = Color.Transparent),
                )
                HorizontalDivider()
                SettingsRow(
                    title = "Recording folder",
                    supporting = settings.recordingDirectory.ifEmpty { "App Documents folder" },
                    onClick = onChooseDirectory,
                )
                if (settings.usingCustomDirectory) {
                    TextButton(onClick = onDefaultDirectory) {
                        Text("Use the app Documents folder")
                    }
                }
            }
        }
        item {
            SettingsSection(
                title = "About",
                supporting = "Track N Race Android · Native telemetry dashboard",
            ) {
                SettingsRow(
                    "Open-source licences",
                    "Libraries, notices, and software licences",
                    onLicenses,
                )
            }
        }
    }
}

@Composable
private fun SettingsSection(
    title: String,
    supporting: String,
    content: @Composable () -> Unit,
) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceContainerLow),
        shape = MaterialTheme.shapes.extraLarge,
    ) {
        Column(
            Modifier.fillMaxWidth().padding(20.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Column(verticalArrangement = Arrangement.spacedBy(3.dp)) {
                Text(title, style = MaterialTheme.typography.titleLarge, fontWeight = FontWeight.SemiBold)
                Text(
                    supporting,
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            content()
        }
    }
}

@Composable
private fun SettingsRow(title: String, supporting: String, onClick: () -> Unit) {
    ListItem(
        headlineContent = { Text(title) },
        supportingContent = { Text(supporting, maxLines = 2, overflow = TextOverflow.Ellipsis) },
        trailingContent = { Icon(painterResource(R.drawable.ic_chevron_right), null) },
        colors = ListItemDefaults.colors(containerColor = Color.Transparent),
        modifier = Modifier.fillMaxWidth().clickable(onClick = onClick),
    )
}

@Composable
private fun PairingScreen(
    telemetry: TelemetryController,
    store: TelemetryStore,
    onScanQr: () -> Unit,
    onDone: () -> Unit,
) {
    var selectedId by remember { mutableStateOf("") }
    var code by remember { mutableStateOf("") }
    val desktops = store.discoveredDesktops
    val selected = desktops.firstOrNull { it.serverId == selectedId }

    DisposableEffect(Unit) {
        telemetry.startDiscovery()
        onDispose(telemetry::stopDiscovery)
    }
    LaunchedEffect(desktops.size) {
        if (selectedId.isEmpty() && desktops.isNotEmpty()) selectedId = desktops.first().serverId
    }
    LaunchedEffect(store.settings.hasSavedDesktop, store.pairingBusy) {
        if (store.settings.hasSavedDesktop && !store.pairingBusy) onDone()
    }

    LazyColumn(
        Modifier.fillMaxSize(),
        contentPadding = PaddingValues(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        item {
            Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.primaryContainer)) {
                Column(Modifier.fillMaxWidth().padding(20.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                    Text("Connect to Track N Race desktop", style = MaterialTheme.typography.headlineSmall)
                    Text("Scan the QR shown by the desktop app, or select a nearby desktop and enter its matching code.")
                    Button(onClick = onScanQr, enabled = !store.pairingBusy) { Text("Scan desktop QR") }
                }
            }
        }
        item { SectionTitle("Nearby desktops") }
        if (desktops.isEmpty()) {
            item { Text("Searching on this network…", color = MaterialTheme.colorScheme.onSurfaceVariant) }
        } else {
            items(desktops, key = { it.serverId }) { desktop ->
                Card(
                    Modifier
                        .fillMaxWidth()
                        .selectable(
                            selected = desktop.serverId == selectedId,
                            onClick = { selectedId = desktop.serverId },
                        ),
                    colors = CardDefaults.cardColors(
                        containerColor = if (desktop.serverId == selectedId) {
                            MaterialTheme.colorScheme.secondaryContainer
                        } else {
                            MaterialTheme.colorScheme.surfaceContainer
                        },
                    ),
                ) {
                    Row(Modifier.fillMaxWidth().padding(12.dp), verticalAlignment = Alignment.CenterVertically) {
                        RadioButton(selected = desktop.serverId == selectedId, onClick = null)
                        Column(Modifier.padding(start = 8.dp)) {
                            Text(desktop.name, fontWeight = FontWeight.SemiBold)
                            Text("${desktop.host}:${desktop.port}", style = MaterialTheme.typography.bodySmall)
                        }
                    }
                }
            }
        }
        item {
            OutlinedTextField(
                value = code,
                onValueChange = { value -> code = value.filter(Char::isDigit).take(6) },
                modifier = Modifier.fillMaxWidth(),
                label = { Text("6-digit matching code") },
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.NumberPassword),
                singleLine = true,
            )
        }
        item {
            Button(
                onClick = { telemetry.pairCode(selected, code) },
                modifier = Modifier.fillMaxWidth(),
                enabled = selected != null && code.isNotEmpty() && !store.pairingBusy,
            ) {
                Text(if (store.pairingBusy) "Connecting…" else "Pair selected desktop")
            }
        }
    }
}

@Composable
private fun LicensesScreen() {
    val context = LocalContext.current
    val notices = remember {
        listOf(R.raw.third_party_notices, R.raw.apache_2_0).joinToString("\n\n") { resource ->
            context.resources.openRawResource(resource).bufferedReader().use { it.readText() }
        }
    }
    Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(18.dp)) {
        Text(notices, style = MaterialTheme.typography.bodySmall, fontFamily = FontFamily.Monospace)
    }
}

@Composable
private fun SectionTitle(text: String, modifier: Modifier = Modifier) {
    Text(
        text.uppercase(),
        modifier = modifier.padding(vertical = 8.dp),
        style = MaterialTheme.typography.labelLarge,
        color = MaterialTheme.colorScheme.primary,
        fontWeight = FontWeight.Bold,
    )
}

private fun lapLabel(cold: DashboardColdState): String = when {
    cold.lapNumber <= 0 -> "—"
    cold.totalLaps > 0 -> "${cold.lapNumber} / ${cold.totalLaps}"
    else -> cold.lapNumber.toString()
}

private fun formatTime(milliseconds: Int): String {
    if (milliseconds <= 0) return "—"
    val minutes = milliseconds / 60_000
    val seconds = milliseconds / 1_000 % 60
    val millis = milliseconds % 1_000
    return "%d:%02d.%03d".format(minutes, seconds, millis)
}

private fun gearLabel(gear: Int): String = when {
    gear < 0 -> "R"
    gear == 0 -> "N"
    else -> gear.toString()
}

private fun tyreLabel(cold: DashboardColdState): String {
    val compound = when (cold.tyreCompound) {
        16 -> "SOFT"
        17 -> "MED"
        18 -> "HARD"
        7 -> "INTER"
        8 -> "WET"
        else -> "TYRE"
    }
    return if (cold.tyreAgeLaps > 0) "$compound ${cold.tyreAgeLaps}L" else "$compound —"
}

private fun tyreTone(compound: Int): Color = when (compound) {
    16 -> Color(0xffff4d5e)
    17 -> Color(0xfff5b942)
    7 -> Color(0xff32d583)
    8 -> Color(0xff43a5ff)
    else -> Color(0xffd8dee6)
}

private fun dryOrder(compound: Int) = when (compound) { 16 -> 0; 17 -> 1; 18 -> 2; else -> 3 }
private fun wetOrder(compound: Int) = when (compound) { 7 -> 0; 8 -> 1; else -> 2 }

private fun compoundLabel(compound: Int) = when (compound) {
    7 -> "INT"; 8 -> "WET"; 16 -> "C5"; 17 -> "C4"; 18 -> "C3"
    19 -> "C2"; 20 -> "C1"; 21 -> "C0"; 22 -> "C6"; else -> compound.toString()
}

private fun compoundColor(compound: Int) = when (compound) {
    16 -> Color(0xffe53935); 17 -> Color(0xffffc107); 18 -> Color(0xffeceff1)
    7 -> Color(0xff43a047); 8 -> Color(0xff1e88e5); else -> Color.Unspecified
}

private fun tyreStatus(set: TyreSetEntry, sessionType: Int?): String = when {
    set.fitted -> "FITTED"
    set.available && set.wear == 0f -> "NEW"
    set.available -> "USED"
    sessionType != null && set.recommendedSession > sessionOrder(sessionType) -> "RESERVED"
    sessionType == null && set.recommendedSession >= 4 -> "RESERVED"
    else -> "RETURNED"
}

private fun sessionOrder(sessionType: Int) = when {
    sessionType in 1..3 -> sessionType
    sessionType == 4 -> 3
    sessionType == 5 || sessionType == 10 -> 4
    sessionType == 6 || sessionType == 11 -> 5
    sessionType in 7..9 || sessionType in 12..14 -> 6
    else -> 7
}

private fun sessionLabel(session: Int) = when (session) {
    1 -> "FP1"; 2 -> "FP2"; 3 -> "FP3"; 4 -> "Q1"; 5 -> "Q2"; 6 -> "Q3"; 7 -> "Race"; else -> "—"
}

private fun lapDelta(set: TyreSetEntry): String {
    if (set.fitted || !set.available || set.lapDeltaMs == 0) return "—"
    val seconds = set.lapDeltaMs / 1000.0
    return "%+.3fs".format(seconds)
}
