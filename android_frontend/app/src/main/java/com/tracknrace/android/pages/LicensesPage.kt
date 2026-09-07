package com.tracknrace.android.pages

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import com.tracknrace.android.R

@Composable
internal fun LicensesScreen() {
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
