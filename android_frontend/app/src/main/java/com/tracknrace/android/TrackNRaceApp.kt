package com.tracknrace.android

import com.tracknrace.android.pages.staticLayer
import android.Manifest
import android.content.ActivityNotFoundException
import android.content.Intent
import android.content.res.Configuration
import android.os.Build
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
import androidx.compose.foundation.layout.widthIn
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
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
import androidx.compose.runtime.Composable
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
import com.tracknrace.android.pages.AnalysisScreen
import com.tracknrace.android.pages.DashboardScreen
import com.tracknrace.android.pages.LicensesScreen
import com.tracknrace.android.pages.PARTICIPANTS_RETRY_DELAY_MS
import com.tracknrace.android.pages.PairingScreen
import com.tracknrace.android.pages.PlayerBar
import com.tracknrace.android.pages.RecordingsSheet
import com.tracknrace.android.pages.SettingsScreen
import com.tracknrace.android.pages.TimingScreen
import com.tracknrace.android.pages.TyresScreen
import com.tracknrace.android.pages.tyreDisplay
import com.tracknrace.android.ui.theme.darkAppColorScheme
import kotlinx.coroutines.delay

private enum class AppScreen(val telemetryPage: PairedTelemetryPage, val isPrimary: Boolean = true) {
    DASHBOARD(PairedTelemetryPage.DASHBOARD),
    TIMING(PairedTelemetryPage.TIMING),
    TYRES(PairedTelemetryPage.TYRES),
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
    val landscape = LocalConfiguration.current.orientation == Configuration.ORIENTATION_LANDSCAPE
    val dashboardBackground = darkAppColorScheme().surface
    var landscapeChromeVisible by remember { mutableStateOf(false) }
    var landscapeChromeInteraction by remember { mutableIntStateOf(0) }

    // Recordings on the phone: the sheet that lists them, and what it needs.
    var recordingsOpen by rememberSaveable { mutableStateOf(false) }
    var foldersVersion by remember { mutableIntStateOf(0) }
    var driverPickerOpen by remember { mutableStateOf(false) }
    val localRecording = store.localRecording
    val appContext = LocalContext.current.applicationContext
    val folderLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.StartActivityForResult(),
    ) { result ->
        result.data?.data?.let { uri ->
            if (RecordingLibrary.addFolder(appContext, uri)) foldersVersion++
            else store.showMessage("Could not keep access to that folder")
        }
    }
    val readPermissionLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestPermission(),
    ) { foldersVersion++ }
    val allowAllFiles = {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            try {
                appContext.startActivity(
                    RecordingLibrary.allFilesAccessIntent(appContext).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
                )
            } catch (_: ActivityNotFoundException) {
                store.showMessage("This phone has no All files access page; add folders instead")
            }
        } else {
            readPermissionLauncher.launch(Manifest.permission.READ_EXTERNAL_STORAGE)
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
    // Analysis share the same subscription bit now, so give them the same retry.
    LaunchedEffect(paired, rosterMissing, screen) {
        if (!paired || !rosterMissing ||
            (screen != AppScreen.DASHBOARD && screen != AppScreen.TYRES && screen != AppScreen.ANALYSIS)
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

    Scaffold(
        topBar = {
            if (!landscape) {
                AppTopBar(
                    screen = screen,
                    driverName = selectedDriverName.takeIf { paired || localRecording != null },
                    driverRestricted = driverRestricted,
                    onSettings = openSettings,
                    onBack = ::navigateBack,
                    onAnalysisSettings = { telemetry.analysis.settingsOpen = true },
                    recordingOpen = localRecording != null,
                    onOpenRecordings = { recordingsOpen = true },
                    onCloseRecording = telemetry::closeRecording,
                    onPickDriver = { driverPickerOpen = true }.takeIf { localRecording != null },
                )
            }
        },
        bottomBar = {
            if (!landscape && isPrimary) {
                Column {
                    if (localRecording != null) RecordingPlayer(telemetry)
                    AppNavigationBar(selected = screen, onSelected = { screen = it })
                }
            }
        },
        snackbarHost = { SnackbarHost(snackbar) },
        // The dashboard is always dark; painting its background here instead
        // of over the theme background saves a full-screen fill every frame.
        containerColor = if (screen == AppScreen.DASHBOARD) dashboardBackground else MaterialTheme.colorScheme.background,
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

            if (landscape) {
                AnimatedVisibility(
                    visible = landscapeChromeVisible,
                    modifier = Modifier.align(Alignment.TopCenter),
                    enter = fadeIn(),
                    exit = fadeOut(),
                ) {
                    AppTopBar(
                        screen = screen,
                        driverName = selectedDriverName.takeIf { paired || localRecording != null },
                        driverRestricted = driverRestricted,
                        onSettings = openSettings,
                        onBack = ::navigateBack,
                        onAnalysisSettings = { telemetry.analysis.settingsOpen = true },
                        recordingOpen = localRecording != null,
                        onOpenRecordings = { recordingsOpen = true },
                        onCloseRecording = telemetry::closeRecording,
                        onPickDriver = { driverPickerOpen = true }.takeIf { localRecording != null },
                    )
                }
                AnimatedVisibility(
                    visible = landscapeChromeVisible && isPrimary,
                    modifier = Modifier.align(Alignment.BottomCenter),
                    enter = fadeIn(),
                    exit = fadeOut(),
                ) {
                    Column {
                        if (localRecording != null) RecordingPlayer(telemetry)
                        AppNavigationBar(selected = screen, onSelected = { screen = it })
                    }
                }
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
            onAllowAccess = allowAllFiles,
            onAddFolder = { folderLauncher.launch(RecordingLibrary.folderPickerIntent()) },
            onRemoveFolder = { folder ->
                RecordingLibrary.removeFolder(appContext, folder)
                foldersVersion++
            },
            onDismiss = { recordingsOpen = false },
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
private fun RecordingPlayer(telemetry: TelemetryController) {
    PlayerBar(
        store = telemetry.store,
        onPlay = telemetry::playbackPlay,
        onPause = telemetry::playbackPause,
        onSeek = telemetry::playbackSeek,
        onSpeed = telemetry::playbackSetSpeed,
        onClose = telemetry::closeRecording,
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
    driverName: String?,
    driverRestricted: Boolean?,
    onSettings: () -> Unit,
    onBack: () -> Unit,
    onAnalysisSettings: () -> Unit,
    recordingOpen: Boolean,
    onOpenRecordings: () -> Unit,
    onCloseRecording: () -> Unit,
    /** Set while a recording plays: the driver name opens the driver picker. */
    onPickDriver: (() -> Unit)?,
) {
    val primary = screen.isPrimary
    var overflowOpen by remember { mutableStateOf(false) }
    val title = when (screen) {
        AppScreen.DASHBOARD -> "Dashboard"
        AppScreen.TIMING -> "Timing"
        AppScreen.TYRES -> "Tyres"
        AppScreen.ANALYSIS -> "Analysis"
        AppScreen.SETTINGS -> "Settings"
        AppScreen.PAIRING -> "Pair desktop"
        AppScreen.LICENSES -> "Open-source licences"
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
                if (driverName != null) {
                    // Matches the desktop driver selector's wording. An unknown
                    // setting says so rather than implying full telemetry.
                    val label = when (driverRestricted) {
                        true -> "$driverName · Public data only"
                        null -> "$driverName · Telemetry access unknown"
                        false -> driverName
                    }
                    if (onPickDriver != null) {
                        TextButton(onClick = onPickDriver, modifier = Modifier.widthIn(max = 220.dp)) {
                            Text(label, maxLines = 1, overflow = TextOverflow.Ellipsis)
                            Icon(painterResource(R.drawable.ic_expand_more), contentDescription = null, Modifier.size(18.dp))
                        }
                    } else {
                        Text(
                            label,
                            modifier = Modifier.widthIn(max = 220.dp),
                            style = MaterialTheme.typography.labelLarge,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                        )
                    }
                }
                if (screen == AppScreen.ANALYSIS) {
                    IconButton(onClick = onAnalysisSettings) {
                        Icon(painterResource(R.drawable.ic_tune), contentDescription = "Chart settings")
                    }
                }
                Box {
                    IconButton(onClick = { overflowOpen = true }) {
                        Icon(painterResource(R.drawable.ic_more), contentDescription = "More options")
                    }
                    // M3 menu: actions first, a divider between groups, then Settings.
                    DropdownMenu(
                        expanded = overflowOpen,
                        onDismissRequest = { overflowOpen = false },
                    ) {
                        DropdownMenuItem(
                            text = { Text("Open recording") },
                            leadingIcon = {
                                Icon(painterResource(R.drawable.ic_folder_open), contentDescription = null)
                            },
                            onClick = {
                                overflowOpen = false
                                onOpenRecordings()
                            },
                        )
                        if (recordingOpen) {
                            DropdownMenuItem(
                                text = { Text("Close recording") },
                                leadingIcon = {
                                    Icon(painterResource(R.drawable.ic_close), contentDescription = null)
                                },
                                onClick = {
                                    overflowOpen = false
                                    onCloseRecording()
                                },
                            )
                        }
                        HorizontalDivider(Modifier.padding(vertical = 8.dp))
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
        Triple(AppScreen.TIMING, "Timing", R.drawable.ic_standings),
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
