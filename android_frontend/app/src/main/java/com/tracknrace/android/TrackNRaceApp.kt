package com.tracknrace.android

import com.tracknrace.android.pages.staticLayer
import android.Manifest
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.RadioButton
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.NavigationBarItemDefaults
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import com.tracknrace.android.ui.immersiveLandscape
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.derivedStateOf
import androidx.compose.runtime.structuralEqualityPolicy
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.input.pointer.PointerEventPass
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.google.zxing.client.android.Intents
import com.journeyapps.barcodescanner.ScanContract
import androidx.compose.foundation.gestures.detectTapGestures
import com.tracknrace.android.pages.PlayerStrip
import com.tracknrace.android.pages.PlayerSheet
import com.tracknrace.android.pages.PlayerStripHeight
import com.tracknrace.android.pages.PlayerSheetWidthLandscape
import com.tracknrace.android.pages.AnalysisScreen
import com.tracknrace.android.pages.AppMenuSheet
import com.tracknrace.android.pages.MenuDriver
import com.tracknrace.android.pages.MenuSource
import com.tracknrace.android.pages.DashboardScreen
import com.tracknrace.android.pages.GraphWindowSheet
import com.tracknrace.android.pages.windowPillText
import com.tracknrace.android.pages.GraphsScreen
import com.tracknrace.android.pages.LicensesScreen
import com.tracknrace.android.pages.PARTICIPANTS_RETRY_DELAY_MS
import com.tracknrace.android.pages.PairingScreen
import com.tracknrace.android.pages.RecordingsSheet
import com.tracknrace.android.pages.SettingsScreen
import com.tracknrace.android.pages.TimingScreen
import com.tracknrace.android.pages.TyresScreen
import com.tracknrace.android.pages.tyreDisplay
import kotlinx.coroutines.delay

private enum class AppScreen(val telemetryPage: PairedTelemetryPage, val isPrimary: Boolean = true) {
    DASHBOARD(PairedTelemetryPage.DASHBOARD),
    TIMING(PairedTelemetryPage.TIMING),
    TYRES(PairedTelemetryPage.TYRES),
    GRAPHS(PairedTelemetryPage.GRAPHS_POWER),
    ANALYSIS(PairedTelemetryPage.ANALYSIS),
    SETTINGS(PairedTelemetryPage.NONE, isPrimary = false),
    PAIRING(PairedTelemetryPage.NONE, isPrimary = false),
    LICENSES(PairedTelemetryPage.NONE, isPrimary = false),
}

// Matched to the roster retry: both recover a control row the desktop already
// caches, so asking more often would not make it arrive sooner.
private const val DRIVER_RESTRICTION_RETRY_DELAY_MS = 3_000L

@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun TrackNRaceApp(telemetry: TelemetryController) {
    val store = telemetry.store
    var screen by rememberSaveable { mutableStateOf(AppScreen.DASHBOARD) }
    var settingsReturn by rememberSaveable { mutableStateOf(AppScreen.DASHBOARD) }
    val snackbar = remember { SnackbarHostState() }
    val message = store.message
    // Landscape as the wheel display hides the bars until a tap; a tablet
    // keeps them unless the Wheel display setting is on.
    val configuration = LocalConfiguration.current
    val landscape = immersiveLandscape(store.settings.wheelDisplay, configuration)
    var landscapeChromeVisible by remember { mutableStateOf(false) }
    var landscapeChromeInteraction by remember { mutableIntStateOf(0) }

    // Recordings on the phone: the sheet that lists them, and what it needs.
    var recordingsOpen by rememberSaveable { mutableStateOf(false) }
    var foldersVersion by remember { mutableIntStateOf(0) }
    var driverPickerOpen by remember { mutableStateOf(false) }
    var graphWindowOpen by remember { mutableStateOf(false) }
    // The Tyres page's Charts tab is showing: the app bar offers the chart window there too.
    var tyreChartsShown by rememberSaveable { mutableStateOf(false) }
    var menuOpen by remember { mutableStateOf(false) }
    val localRecording = store.localRecording
    // The recording player stays a sliver until tapped open.
    var playerOpen by rememberSaveable { mutableStateOf(false) }
    LaunchedEffect(localRecording) {
        if (localRecording == null) playerOpen = false
    }
    val appContext = LocalContext.current.applicationContext
    val folderLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.StartActivityForResult(),
    ) { result ->
        result.data?.data?.let { uri ->
            if (RecordingLibrary.addFolder(appContext, uri)) foldersVersion++
            else store.showMessage("Could not keep access to that folder")
        }
    }
    val directoryLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.StartActivityForResult(),
    ) { result ->
        val data = result.data
        data?.data?.let { uri -> telemetry.acceptRecordingDirectory(uri, data.flags) }
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

    // Android 17 local network access: the controller asks through this
    // launcher on launch, on returning to the app, and before pairing.
    val localNetworkLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestPermission(),
    ) { granted -> telemetry.onLocalNetworkResult(granted) }
    DisposableEffect(telemetry, localNetworkLauncher) {
        telemetry.localNetworkRequester = {
            localNetworkLauncher.launch(Manifest.permission.ACCESS_LOCAL_NETWORK)
        }
        // onHostStart ran before this first composition, so ask here once.
        if (!telemetry.hasLocalNetworkAccess()) telemetry.localNetworkRequester?.invoke()
        onDispose { telemetry.localNetworkRequester = null }
    }

    LaunchedEffect(message?.id) {
        message?.let { snackbar.showSnackbar(it.text) }
    }
    LaunchedEffect(screen) {
        telemetry.setActivePage(screen.telemetryPage)
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

    val isPrimary = screen.isPrimary
    val paired = store.settings.source == PairedTelemetryClient.SOURCE_PAIRED

    // derivedStateOf keeps this root from recomposing on every timing row: it
    // only invalidates when the name or the roster-missing flag actually changes.
    val selectedDriverName by remember(store) { derivedStateOf(structuralEqualityPolicy()) { store.selectedDriverName } }
    val driverRestricted by remember(store) { derivedStateOf(structuralEqualityPolicy()) { store.selectedDriverRestricted } }
    val restrictionMissing by remember(store) {
        derivedStateOf(structuralEqualityPolicy()) { store.needsDriverRestrictionRefresh() }
    }
    // The desktop states the restriction on every driver change and caches it
    // for the subscription snapshot, so a gap means the frame was lost or the
    // socket dropped. Ask again until it answers.
    LaunchedEffect(paired, restrictionMissing) {
        if (!paired || !restrictionMissing) return@LaunchedEffect
        while (true) {
            delay(DRIVER_RESTRICTION_RETRY_DELAY_MS)
            telemetry.requestDriverRestriction()
        }
    }
    // The cold state also carries the lap clock, ERS and fuel, which change
    // several times a second; the labels almost never do.
    val coldLabels by remember(store) { derivedStateOf(structuralEqualityPolicy()) { store.cold.labels } }
    // Only what the Tyres page shows, as it shows it: the lap clock, and
    // tyre-set updates that change no shown value, recompose nothing.
    val tyreRows by remember(store) {
        derivedStateOf(structuralEqualityPolicy()) { tyreDisplay(store.cold) }
    }
    val rosterMissing by remember(store) {
        derivedStateOf(structuralEqualityPolicy()) { store.timing.needsParticipantsRefresh() }
    }
    // The Timing page retries a missing roster itself. Dashboard, Tyres and
    // Analysis and Graphs share the same subscription bit now, so give them the same retry.
    LaunchedEffect(paired, rosterMissing, screen) {
        if (!paired || !rosterMissing ||
            (screen != AppScreen.DASHBOARD && screen != AppScreen.TYRES && screen != AppScreen.ANALYSIS &&
                screen != AppScreen.GRAPHS)
        ) return@LaunchedEffect
        while (true) {
            delay(PARTICIPANTS_RETRY_DELAY_MS)
            telemetry.requestParticipants()
        }
    }

    val openSettings = {
        settingsReturn = if (isPrimary) screen else AppScreen.DASHBOARD
        screen = AppScreen.SETTINGS
    }
    BackHandler(enabled = !isPrimary) {
        navigateBack()
    }
    BackHandler(enabled = playerOpen && isPrimary) {
        playerOpen = false
    }

    Scaffold(
        topBar = {
            if (!landscape) {
                AppTopBar(
                    screen = screen,
                    showsGraphWindow = screen == AppScreen.GRAPHS || (screen == AppScreen.TYRES && tyreChartsShown),
                    onBack = ::navigateBack,
                    onAnalysisSettings = { telemetry.analysis.settingsOpen = true },
                    graphWindow = telemetry.graphs.globalWindow.takeIf(telemetry.graphs::available) ?: GraphWindow.DEFAULT,
                    graphReferenceLap = telemetry.graphs.globalLap(),
                    onGraphWindow = { graphWindowOpen = true },
                    onMenu = { menuOpen = true },
                )
            }
        },
        bottomBar = {
            if (!landscape && isPrimary) {
                Column {
                    // Room for the player's strip, which is drawn with the page
                    // so the open player can take its place.
                    if (localRecording != null) Spacer(Modifier.height(PlayerStripHeight))
                    AppNavigationBar(selected = screen, onSelected = { screen = it })
                }
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
                        // After the page has handled the touch, so a tap that a tab,
                        // chip or button took does not also bring up the bars.
                        val event = awaitPointerEvent(PointerEventPass.Final)
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
                                        // A control's tap only keeps bars that are already up.
                                        if (!change.isConsumed) landscapeChromeVisible = true
                                        if (landscapeChromeVisible) landscapeChromeInteraction += 1
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
        // Where the player's strip and sheet sit: on the navigation bar in
        // portrait, on the screen's edge in landscape.
        val playerBottom = if (landscape) 0.dp else (padding.calculateBottomPadding() - PlayerStripHeight).coerceAtLeast(0.dp)
        val playerRoom = if (landscape && isPrimary && localRecording != null) PlayerStripHeight else 0.dp
        Box(Modifier.fillMaxSize().then(revealChromeModifier)) {
            // Page content consumes Scaffold's safe-area padding. Landscape
            // app-bar surfaces are separate siblings, so their backgrounds
            // can still draw edge-to-edge behind the cutout.
            Box(Modifier.fillMaxSize().padding(padding).padding(bottom = playerRoom)) {
                AnimatedContent(
                    targetState = screen,
                    modifier = Modifier.fillMaxSize(),
                    label = "App page",
                ) { targetScreen ->
                    val destinationActive = targetScreen == screen
                    when (targetScreen) {
                        // AnimatedContent keeps the outgoing destination alive
                        // until its Material fade-through finishes. Freeze its
                        // snapshot state and stop work as soon as it is inactive.
                        AppScreen.DASHBOARD -> DashboardScreen(
                            store = store,
                            active = destinationActive,
                            // Each dashboard tab asks the desktop for its own data.
                            onPage = telemetry::setActivePage,
                        )
                        AppScreen.TIMING -> TimingScreen(
                            // Read by the page itself: reading it here would recompose
                            // this whole switcher on every timing row.
                            state = { store.timing },
                            labels = rememberCurrentWhileActive(destinationActive) { coldLabels },
                            oneLine = rememberCurrentWhileActive(destinationActive) {
                                store.settings.timingOneLine
                            },
                            active = destinationActive,
                            onRequestParticipants = telemetry::requestParticipants,
                        )
                        AppScreen.TYRES -> TyresScreen(
                            display = { tyreRows },
                            store = store,
                            graphs = telemetry.graphs,
                            active = destinationActive,
                            // The allocation and the charts each ask the desktop for their own data.
                            onPage = telemetry::setActivePage,
                            onChartsShown = { tyreChartsShown = it },
                        )
                        AppScreen.GRAPHS -> GraphsScreen(
                            store = store,
                            graphs = telemetry.graphs,
                            active = destinationActive,
                            // Each graph tab asks the desktop for its own data.
                            onPage = telemetry::setActivePage,
                        )
                        AppScreen.ANALYSIS -> AnalysisScreen(
                            store = store,
                            analysis = telemetry.analysis,
                            active = destinationActive,
                        )
                        AppScreen.SETTINGS -> SettingsScreen(
                            settings = rememberCurrentWhileActive(destinationActive) { store.settings },
                            onSource = telemetry::setSource,
                            onPairing = { screen = AppScreen.PAIRING },
                            onRecording = telemetry::setRecording,
                            onTimingOneLine = telemetry::setTimingOneLine,
                            onThemeMode = telemetry::setThemeMode,
                            onWheelDisplay = telemetry::setWheelDisplay,
                            onChooseDirectory = { directoryLauncher.launch(telemetry.recordingDirectoryIntent()) },
                            onDefaultDirectory = telemetry::useDefaultRecordingDirectory,
                            onForgetDesktop = telemetry::forgetDesktop,
                            onLicenses = { screen = AppScreen.LICENSES },
                        )
                        AppScreen.PAIRING -> PairingScreen(
                            telemetry = telemetry,
                            store = store,
                            active = destinationActive,
                            onScanQr = {
                                telemetry.onQrScannerLaunching()
                                try {
                                    qrLauncher.launch(telemetry.qrScanOptions())
                                } catch (error: RuntimeException) {
                                    telemetry.onQrScannerFinished()
                                    store.showMessage(error.message ?: "Could not open the QR scanner")
                                }
                            },
                            onDone = { screen = AppScreen.SETTINGS },
                        )
                        AppScreen.LICENSES -> LicensesScreen()
                    }
                }

                val disconnected = store.settings.source == PairedTelemetryClient.SOURCE_PAIRED &&
                    (store.sourceStatus.state == "error" || store.sourceStatus.state == "disconnected" ||
                        store.sourceStatus.state == "reconnecting")
                AnimatedVisibility(
                    visible = disconnected && screen != AppScreen.PAIRING,
                    modifier = Modifier.align(Alignment.BottomCenter),
                    // Alpha-only motion stays in the draw phase and does not
                    // relayout the page beneath this overlay.
                    enter = fadeIn(),
                    exit = fadeOut(),
                ) {
                    Surface(
                        modifier = Modifier.padding(12.dp),
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

            // The hidden player: on the navigation bar in portrait; along the
            // screen's edge in landscape, under the navigation bar when that is up.
            if (isPrimary && localRecording != null && !playerOpen) {
                PlayerStrip(
                    store,
                    onPlay = telemetry::playbackPlay,
                    onPause = telemetry::playbackPause,
                    onOpen = {
                        playerOpen = true
                        landscapeChromeVisible = false
                    },
                    modifier = Modifier.align(Alignment.BottomCenter).padding(bottom = playerBottom),
                )
            }
            if (landscape) {
                AnimatedVisibility(
                    visible = landscapeChromeVisible,
                    modifier = Modifier.align(Alignment.TopCenter),
                    enter = fadeIn(),
                    exit = fadeOut(),
                ) {
                    AppTopBar(
                        screen = screen,
                        showsGraphWindow = screen == AppScreen.GRAPHS || (screen == AppScreen.TYRES && tyreChartsShown),
                        onBack = ::navigateBack,
                        onAnalysisSettings = { telemetry.analysis.settingsOpen = true },
                        graphWindow = telemetry.graphs.globalWindow.takeIf(telemetry.graphs::available) ?: GraphWindow.DEFAULT,
                        graphReferenceLap = telemetry.graphs.globalLap(),
                        onGraphWindow = { graphWindowOpen = true },
                        onMenu = { menuOpen = true },
                    )
                }
                AnimatedVisibility(
                    visible = landscapeChromeVisible && isPrimary,
                    modifier = Modifier.align(Alignment.BottomCenter),
                    enter = fadeIn(),
                    exit = fadeOut(),
                ) {
                    AppNavigationBar(selected = screen, onSelected = { screen = it })
                }
            }
            if (playerOpen && isPrimary && localRecording != null) {
                // Over the page: a tap outside it hides it, and is not passed on.
                Box(Modifier.fillMaxSize().pointerInput(Unit) { detectTapGestures { playerOpen = false } })
                RecordingPlayer(
                    telemetry,
                    onHide = { playerOpen = false },
                    modifier = Modifier.align(Alignment.BottomCenter).then(
                        if (landscape) {
                            Modifier.width(PlayerSheetWidthLandscape)
                        } else {
                            // On the navigation bar, in the strip's place.
                            Modifier.fillMaxWidth().padding(bottom = playerBottom)
                        },
                    ),
                )
            }
        }
    }

    if (recordingsOpen) {
        RecordingsSheet(
            openTitle = localRecording,
            foldersVersion = foldersVersion,
            onOpen = { file ->
                recordingsOpen = false
                telemetry.openRecording(file)
            },
            onAddFolder = { folderLauncher.launch(RecordingLibrary.folderPickerIntent()) },
            onRemoveFolder = { folder ->
                RecordingLibrary.removeFolder(appContext, folder)
                foldersVersion++
            },
            onDismiss = { recordingsOpen = false },
        )
    }
    if (menuOpen) {
        val status = store.sourceStatus
        AppMenuSheet(
            source = when {
                localRecording != null -> MenuSource.Recording(localRecording)
                paired -> MenuSource.Desktop(store.settings.desktopName, status.state, status.detail)
                else -> MenuSource.Direct
            },
            driver = selectedDriverName?.takeIf { paired || localRecording != null }?.let { name ->
                MenuDriver(name, driverRestricted, onPick = { driverPickerOpen = true }.takeIf { localRecording != null })
            },
            onOpenRecordings = { recordingsOpen = true },
            onCloseRecording = telemetry::closeRecording,
            onReconnect = telemetry::reconnect,
            onSettings = openSettings,
            onDismiss = { menuOpen = false },
        )
    }
    if (graphWindowOpen) {
        GraphWindowSheet(
            title = "Chart window",
            selected = telemetry.graphs.globalWindow,
            options = GraphWindow.entries.filter(telemetry.graphs::available),
            onSelect = { window ->
                telemetry.graphs.selectGlobalWindow(window)
                graphWindowOpen = false
            },
            onDismiss = { graphWindowOpen = false },
            laps = store.playbackCatalog,
            referenceLap = telemetry.graphs.globalLap(),
            onReferenceLap = telemetry.graphs::selectGlobalReferenceLap,
        )
    }
    if (driverPickerOpen && localRecording != null) {
        DriverPicker(
            store = store,
            onPick = { index ->
                driverPickerOpen = false
                telemetry.playbackSetDriver(index)
            },
            onDismiss = { driverPickerOpen = false },
        )
    }
}

@Composable
private fun RecordingPlayer(telemetry: TelemetryController, onHide: () -> Unit, modifier: Modifier) {
    PlayerSheet(
        store = telemetry.store,
        onPlay = telemetry::playbackPlay,
        onPause = telemetry::playbackPause,
        onSeek = telemetry::playbackSeek,
        onSpeed = telemetry::playbackSetSpeed,
        onClose = telemetry::closeRecording,
        onHide = onHide,
        modifier = modifier,
    )
}

/**
 * The recording's drivers in running order, as the desktop's driver selector:
 * the chosen driver's own data is what every page shows.
 */
@Composable
private fun DriverPicker(store: TelemetryStore, onPick: (Int) -> Unit, onDismiss: () -> Unit) {
    val timing = store.timing
    val selected = store.selectedDriverIndex
    val positions = timing.cars.associate { it.index to it.position }
    val drivers = timing.drivers.values.sortedWith(
        compareBy({ positions[it.index]?.takeIf { position -> position > 0 } ?: Int.MAX_VALUE }, { it.index }),
    )
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Driver") },
        text = {
            if (drivers.isEmpty()) {
                Text("The recording has not named its drivers yet.")
            } else {
                LazyColumn(Modifier.selectableGroup()) {
                    items(drivers, key = { it.index }) { driver ->
                        Row(
                            Modifier
                                .fillMaxWidth()
                                .height(52.dp)
                                .clip(MaterialTheme.shapes.small)
                                .selectable(selected = driver.index == selected, role = Role.RadioButton) { onPick(driver.index) },
                            verticalAlignment = Alignment.CenterVertically,
                        ) {
                            RadioButton(selected = driver.index == selected, onClick = null)
                            Spacer(Modifier.width(12.dp))
                            Text(
                                positions[driver.index]?.takeIf { it > 0 }?.let { "P$it" } ?: "",
                                style = MaterialTheme.typography.labelLarge,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                                modifier = Modifier.width(36.dp),
                            )
                            Text(driver.name, style = MaterialTheme.typography.bodyLarge, maxLines = 1, overflow = TextOverflow.Ellipsis)
                        }
                    }
                }
            }
        },
        confirmButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}

/** Avoids recomposing an outgoing AnimatedContent destination with live data. */
@Composable
private fun <T> rememberCurrentWhileActive(active: Boolean, current: () -> T): T =
    if (active) current() else remember { current() }

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun AppTopBar(
    screen: AppScreen,
    /** The page shows charts that follow the global window. */
    showsGraphWindow: Boolean,
    onBack: () -> Unit,
    onAnalysisSettings: () -> Unit,
    /** The Graphs page's global window. */
    graphWindow: GraphWindow,
    /** The lap a global Selected window compares against. */
    graphReferenceLap: Int,
    onGraphWindow: () -> Unit,
    /** Opens the app's menu sheet: the source, the driver, recordings and settings. */
    onMenu: () -> Unit,
) {
    val primary = screen.isPrimary
    val title = when (screen) {
        AppScreen.DASHBOARD -> "Dashboard"
        AppScreen.TIMING -> "Timing"
        AppScreen.TYRES -> "Tyres"
        AppScreen.GRAPHS -> "Charts"
        AppScreen.ANALYSIS -> "Analysis"
        AppScreen.SETTINGS -> "Settings"
        AppScreen.PAIRING -> "Pair desktop"
        AppScreen.LICENSES -> "Licenses"
    }
    TopAppBar(
        modifier = Modifier.staticLayer(),
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
                if (showsGraphWindow) {
                    // The window every chart follows unless it picks its own.
                    TextButton(onClick = onGraphWindow) {
                        Icon(painterResource(R.drawable.ic_timer), contentDescription = null, Modifier.size(18.dp))
                        Spacer(Modifier.width(6.dp))
                        Text(windowPillText(graphWindow, narrow = false, referenceLap = graphReferenceLap), maxLines = 1)
                        Icon(painterResource(R.drawable.ic_expand_more), contentDescription = "Chart window", Modifier.size(18.dp))
                    }
                }
                if (screen == AppScreen.ANALYSIS) {
                    IconButton(onClick = onAnalysisSettings) {
                        Icon(painterResource(R.drawable.ic_tune), contentDescription = "Chart settings")
                    }
                }
                IconButton(onClick = onMenu) {
                    Icon(painterResource(R.drawable.ic_more), contentDescription = "Menu")
                }
            }
        },
    )
}

@Composable
private fun AppNavigationBar(selected: AppScreen, onSelected: (AppScreen) -> Unit) {
    val destinations = listOf(
        Triple(AppScreen.DASHBOARD, "Dashboard", R.drawable.ic_overview),
        Triple(AppScreen.TIMING, "Timing", R.drawable.ic_standings),
        Triple(AppScreen.GRAPHS, "Charts", R.drawable.ic_graphs),
        Triple(AppScreen.TYRES, "Tyres", R.drawable.ic_tyres),
        Triple(AppScreen.ANALYSIS, "Analysis", R.drawable.ic_analysis),
    )
    NavigationBar(
        modifier = Modifier.staticLayer(),
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
