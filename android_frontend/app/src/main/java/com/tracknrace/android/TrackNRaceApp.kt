package com.tracknrace.android

import android.content.res.Configuration
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.NavigationBar
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
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.input.pointer.PointerEventPass
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.google.zxing.client.android.Intents
import com.journeyapps.barcodescanner.ScanContract
import com.tracknrace.android.pages.DashboardScreen
import com.tracknrace.android.pages.LicensesScreen
import com.tracknrace.android.pages.PairingScreen
import com.tracknrace.android.pages.SettingsScreen
import com.tracknrace.android.pages.TimingScreen
import com.tracknrace.android.pages.TyresScreen
import kotlinx.coroutines.delay

private enum class AppScreen(val telemetryPage: PairedTelemetryPage) {
    DASHBOARD(PairedTelemetryPage.DASHBOARD),
    TIMING(PairedTelemetryPage.TIMING),
    TYRES(PairedTelemetryPage.TYRES),
    SETTINGS(PairedTelemetryPage.NONE),
    PAIRING(PairedTelemetryPage.NONE),
    LICENSES(PairedTelemetryPage.NONE),
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun TrackNRaceApp(telemetry: TelemetryController) {
    val store = telemetry.store
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

    val isPrimary = screen == AppScreen.DASHBOARD || screen == AppScreen.TIMING || screen == AppScreen.TYRES
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
                            cold = rememberCurrentWhileActive(destinationActive) { store.cold },
                        )
                        AppScreen.TIMING -> TimingScreen(
                            state = rememberCurrentWhileActive(destinationActive) { store.timing },
                            labels = rememberCurrentWhileActive(destinationActive) { store.cold.labels },
                            oneLine = rememberCurrentWhileActive(destinationActive) {
                                store.settings.timingOneLine
                            },
                            active = destinationActive,
                            onRequestParticipants = telemetry::requestParticipants,
                        )
                        AppScreen.TYRES -> TyresScreen(
                            rememberCurrentWhileActive(destinationActive) { store.cold },
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
                    (store.sourceStatus.state == "error" || store.sourceStatus.state == "disconnected")
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
                        onSettings = openSettings,
                        onBack = ::navigateBack,
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
        }
    }
}

/** Avoids recomposing an outgoing AnimatedContent destination with live data. */
@Composable
private fun <T> rememberCurrentWhileActive(active: Boolean, current: () -> T): T =
    if (active) current() else remember { current() }

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun AppTopBar(
    screen: AppScreen,
    onSettings: () -> Unit,
    onBack: () -> Unit,
) {
    val primary = screen == AppScreen.DASHBOARD || screen == AppScreen.TIMING || screen == AppScreen.TYRES
    var overflowOpen by remember { mutableStateOf(false) }
    val title = when (screen) {
        AppScreen.DASHBOARD -> "Dashboard"
        AppScreen.TIMING -> "Timing"
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
        Triple(AppScreen.TIMING, "Timing", R.drawable.ic_standings),
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
