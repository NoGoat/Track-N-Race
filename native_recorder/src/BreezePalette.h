#pragma once

#include <QString>

// Real KDE Breeze colours for the bundled Breeze style.
//
// A Qt style only draws shapes/metrics — the colours come from the QPalette,
// which on a real KDE session is set by the KDE platform theme from KColorScheme.
// We don't ship that platform theme, so off-KDE the Breeze plugin would draw its
// shapes over the wrong (Windows/default) palette. These helpers reproduce what
// the platform theme does, but *only* for Breeze — every other style keeps the
// palette the platform (or the style itself, e.g. Kvantum) gives it.
//
// Both are no-ops in builds without the bundled KF6ColorScheme (normal dev
// builds, where the system platform theme already owns the palette) — see the
// HAVE_BREEZE_KCOLORSCHEME guard in BreezePalette.cpp.

// Apply the exact KDE Breeze palette (light/dark per `theme`). Call only when
// Breeze is the selected style, AFTER QApplication::setStyle.
void applyBreezePalette(const QString& theme);

// Undo a previously-applied Breeze palette, restoring the platform's original
// palette (captured at startup into the qApp "defaultPalette" property). Call
// BEFORE switching to a non-Breeze style so that style/platform controls the
// colours again. No-op if Breeze was never applied — so it never disturbs styles
// like windows11/Kvantum that manage their own palette.
void restoreDefaultPalette();
