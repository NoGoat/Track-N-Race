package com.nogoat.tracknrace.pages

import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.nogoat.tracknrace.GraphsController

// ── Design ──────────────────────────────────────────────────────────────────
// The Electron app's Settings ▸ Graphs page for a phone: one switch per Charts
// tab, since a tab shows as one table of all its charts' values, and the
// desktop's set-all action under them.

/** The Settings row's summary: the tabs shown as tables. */
internal fun dataTablesSummary(graphs: GraphsController): String {
    val tables = DATA_TABLE_TABS.filter { graphs.showsTable(it.id) }
    return when (tables.size) {
        0 -> "Every Charts tab shows charts"
        DATA_TABLE_TABS.size -> "Every Charts tab shows a table"
        else -> "Tables on ${tables.joinToString { it.title }}"
    }
}

@Composable
internal fun DataTablesScreen(graphs: GraphsController) {
    LazyColumn(
        Modifier.fillMaxSize(),
        contentPadding = PaddingValues(horizontal = 16.dp, vertical = 8.dp),
    ) {
        item {
            SettingsGroup("Show as a table") {
                DATA_TABLE_TABS.forEachIndexed { index, tab ->
                    if (index > 0) GroupDivider()
                    SettingsSwitchRow(
                        title = tab.title,
                        supporting = tab.contents,
                        checked = graphs.showsTable(tab.id),
                        onCheckedChange = { graphs.setTable(tab.id, it) },
                    )
                }
                GroupDivider()
                val anyChart = DATA_TABLE_TABS.any { !graphs.showsTable(it.id) }
                ActionRow(
                    if (anyChart) "Set all to table" else "Set all to chart",
                    onClick = { DATA_TABLE_TABS.forEach { graphs.setTable(it.id, anyChart) } },
                )
            }
            Text(
                "A table lists every sample of the tab's charts for the chosen window, newest at the bottom.",
                Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 16.dp),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}
