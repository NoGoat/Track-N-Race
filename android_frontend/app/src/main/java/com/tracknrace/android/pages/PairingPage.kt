package com.tracknrace.android.pages

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.LocalContentColor
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import com.tracknrace.android.TelemetryController
import com.tracknrace.android.TelemetryStore

@Composable
internal fun PairingScreen(
    telemetry: TelemetryController,
    store: TelemetryStore,
    onScanQr: () -> Unit,
    onDone: () -> Unit,
    active: Boolean = true,
) {
    var selectedId by remember { mutableStateOf("") }
    var code by remember { mutableStateOf("") }
    var observedPairingSuccessId by remember { mutableStateOf(store.pairingSuccessId) }
    val desktops = store.discoveredDesktops
    val selected = desktops.firstOrNull { it.serverId == selectedId }

    DisposableEffect(active) {
        if (active) telemetry.startDiscovery()
        onDispose {
            if (active) telemetry.stopDiscovery()
        }
    }
    LaunchedEffect(desktops.size, active) {
        if (!active) return@LaunchedEffect
        if (selectedId.isEmpty() && desktops.isNotEmpty()) selectedId = desktops.first().serverId
    }
    LaunchedEffect(store.pairingSuccessId, active) {
        if (!active) return@LaunchedEffect
        if (store.pairingSuccessId != observedPairingSuccessId) {
            observedPairingSuccessId = store.pairingSuccessId
            onDone()
        }
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
            item {
                Row(
                    modifier = Modifier.fillMaxWidth().padding(vertical = 8.dp),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(12.dp),
                ) {
                    CircularProgressIndicator(modifier = Modifier.size(18.dp), strokeWidth = 2.dp)
                    Text("Searching on this network…", color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
            }
        }
        items(desktops, key = { it.serverId }) { desktop ->
            val selectedDesktop = desktop.serverId == selectedId
            Card(
                Modifier
                    .fillMaxWidth()
                    .selectable(
                        selected = selectedDesktop,
                        onClick = { selectedId = desktop.serverId },
                    ),
                colors = CardDefaults.cardColors(
                    containerColor = if (selectedDesktop) {
                        MaterialTheme.colorScheme.secondaryContainer
                    } else {
                        MaterialTheme.colorScheme.surfaceContainer
                    },
                ),
            ) {
                Row(Modifier.fillMaxWidth().padding(12.dp), verticalAlignment = Alignment.CenterVertically) {
                    RadioButton(selected = selectedDesktop, onClick = null)
                    Column(Modifier.padding(start = 8.dp)) {
                        Text(desktop.name, fontWeight = FontWeight.SemiBold)
                        Text("${desktop.host}:${desktop.port}", style = MaterialTheme.typography.bodySmall)
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
                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(10.dp),
                ) {
                    if (store.pairingBusy) {
                        CircularProgressIndicator(
                            modifier = Modifier.size(18.dp),
                            strokeWidth = 2.dp,
                            color = LocalContentColor.current,
                        )
                    }
                    Text(if (store.pairingBusy) "Connecting…" else "Pair selected desktop")
                }
            }
        }
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
