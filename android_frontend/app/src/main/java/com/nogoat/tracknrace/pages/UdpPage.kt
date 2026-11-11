package com.nogoat.tracknrace.pages

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.Button
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.toMutableStateList
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import com.nogoat.tracknrace.AndroidSettings
import com.nogoat.tracknrace.DEFAULT_UDP_PORT
import com.nogoat.tracknrace.MAX_UDP_FORWARD_TARGETS
import com.nogoat.tracknrace.R
import com.nogoat.tracknrace.UdpForwardTarget
import com.nogoat.tracknrace.isIpv4Address
import java.net.Inet4Address
import java.net.NetworkInterface

// ── Design ──────────────────────────────────────────────────────────────────
// The desktop's Settings ▸ Network listener settings, in the Settings page's
// groups: the port, then forwarding with one row per destination. Edits are a
// draft until Apply, as on the desktop, since applying rebinds the listener.

// Android keeps the ports below 1024 from apps.
private const val MIN_LISTEN_PORT = 1024

/** A destination as typed; its port is text until it is applied. */
private data class ForwardDraft(val address: String, val port: String)

@Composable
internal fun UdpScreen(
    settings: AndroidSettings,
    onApply: (port: Int, forwardingEnabled: Boolean, forwardTargets: List<UdpForwardTarget>) -> Unit,
) {
    var portText by remember { mutableStateOf(settings.udpPort.toString()) }
    var forwarding by remember { mutableStateOf(settings.udpForwardingEnabled) }
    val drafts = remember {
        settings.udpForwardTargets.map { ForwardDraft(it.address, it.port.toString()) }.toMutableStateList()
    }
    val ownAddresses = remember { ownIpv4Addresses() }

    val port = portText.toIntOrNull()
    val portValid = port != null && port in MIN_LISTEN_PORT..65535
    // Forwarding back to this listener would loop every packet.
    fun draftValid(draft: ForwardDraft): Boolean {
        val address = draft.address.trim()
        val targetPort = draft.port.toIntOrNull() ?: return false
        return isIpv4Address(address) && targetPort in 1..65535 &&
            !(targetPort == port && (address.startsWith("127.") || address == "0.0.0.0" || address in ownAddresses))
    }
    val targetsValid = !forwarding || drafts.all(::draftValid)
    val targets = drafts.mapNotNull { draft ->
        draft.port.toIntOrNull()?.let { UdpForwardTarget(draft.address.trim(), it) }
    }
    val dirty = port != settings.udpPort || forwarding != settings.udpForwardingEnabled ||
        targets.size != drafts.size || targets != settings.udpForwardTargets

    LazyColumn(
        Modifier.fillMaxSize(),
        contentPadding = PaddingValues(horizontal = 16.dp, vertical = 8.dp),
    ) {
        item {
            SettingsGroup("Listener") {
                OutlinedTextField(
                    value = portText,
                    onValueChange = { value -> portText = value.filter(Char::isDigit).take(5) },
                    modifier = Modifier.fillMaxWidth().padding(16.dp),
                    singleLine = true,
                    isError = !portValid,
                    label = { Text("UDP port") },
                    supportingText = {
                        Text(
                            if (portValid) {
                                "The port the game sends telemetry to · default $DEFAULT_UDP_PORT"
                            } else {
                                "Enter a port from $MIN_LISTEN_PORT to 65535"
                            },
                        )
                    },
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                )
            }
        }
        item {
            SettingsGroup("Forwarding") {
                SettingsSwitchRow(
                    title = "UDP forward mode",
                    supporting = "Forward every received packet unchanged to the destinations below",
                    checked = forwarding,
                    onCheckedChange = { forwarding = it },
                )
                if (forwarding) {
                    GroupDivider()
                    Column(
                        Modifier.fillMaxWidth().padding(start = 16.dp, end = 4.dp, top = 12.dp),
                        verticalArrangement = Arrangement.spacedBy(8.dp),
                    ) {
                        if (drafts.isEmpty()) {
                            Text(
                                "No forwarding destinations yet.",
                                style = MaterialTheme.typography.bodyMedium,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                            )
                        }
                        drafts.forEachIndexed { index, draft ->
                            Row(
                                verticalAlignment = Alignment.CenterVertically,
                                horizontalArrangement = Arrangement.spacedBy(8.dp),
                            ) {
                                val invalid = !draftValid(draft)
                                OutlinedTextField(
                                    value = draft.address,
                                    onValueChange = { value ->
                                        drafts[index] = draft.copy(
                                            address = value.filter { it.isDigit() || it == '.' }.take(15),
                                        )
                                    },
                                    modifier = Modifier.weight(1f),
                                    singleLine = true,
                                    isError = invalid,
                                    label = { Text("IPv4 address") },
                                    placeholder = { Text("192.168.1.50") },
                                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal),
                                )
                                OutlinedTextField(
                                    value = draft.port,
                                    onValueChange = { value ->
                                        drafts[index] = draft.copy(port = value.filter(Char::isDigit).take(5))
                                    },
                                    modifier = Modifier.width(104.dp),
                                    singleLine = true,
                                    isError = invalid,
                                    label = { Text("Port") },
                                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                                )
                                IconButton(onClick = { drafts.removeAt(index) }) {
                                    Icon(painterResource(R.drawable.ic_delete), "Remove destination ${index + 1}")
                                }
                            }
                        }
                        if (!targetsValid) {
                            Text(
                                "Enter valid IPv4 destinations and ports. Forwarding back to this listener would create a packet loop.",
                                style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.error,
                            )
                        }
                    }
                    Row(
                        Modifier.fillMaxWidth().padding(start = 4.dp, end = 16.dp, top = 4.dp, bottom = 4.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        TextButton(
                            onClick = { drafts.add(ForwardDraft("", DEFAULT_UDP_PORT.toString())) },
                            enabled = drafts.size < MAX_UDP_FORWARD_TARGETS,
                        ) {
                            Icon(painterResource(R.drawable.ic_add), null)
                            Text("Add destination", Modifier.padding(start = 8.dp))
                        }
                        Text(
                            "${drafts.size}/$MAX_UDP_FORWARD_TARGETS",
                            Modifier.weight(1f),
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            textAlign = TextAlign.End,
                        )
                    }
                }
            }
        }
        item {
            Column(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 16.dp)) {
                Button(
                    onClick = { if (port != null) onApply(port, forwarding, targets) },
                    enabled = dirty && portValid && targetsValid,
                ) {
                    Text("Apply")
                }
                Text(
                    "Used while the source is Direct UDP. Applying rebinds the listener; the live session carries on.",
                    Modifier.padding(top = 8.dp),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
    }
}

/** This device's IPv4 addresses, so a destination can't be the listener itself. */
private fun ownIpv4Addresses(): Set<String> = try {
    NetworkInterface.getNetworkInterfaces().asSequence()
        .flatMap { it.inetAddresses.asSequence() }
        .filterIsInstance<Inet4Address>()
        .mapNotNull { it.hostAddress }
        .toSet()
} catch (_: Exception) {
    emptySet()
}
