#pragma once

#include <QString>

// Applies the *exact* KDE Breeze QPalette to the application when the Breeze
// style is selected, so the bundled Breeze plugin renders with real KDE colours
// instead of the host OS's default palette.
//
// Why this is needed: a Qt style only draws shapes/metrics — the colours come
// from the QPalette, which on a real KDE session is set by the KDE platform
// theme from KColorScheme. We don't ship that platform theme, so off-KDE the
// Breeze plugin would draw its shapes over the wrong (Windows/default) palette.
// Here we reproduce what the platform theme does: feed the bundled Breeze
// colour-scheme file to KColorScheme::createApplicationPalette().
//
// `styleName` is the selected style ("breeze"/"system"/...), `theme` is the
// app's theme setting ("system"/"light"/"dark"). When `styleName` isn't Breeze
// the palette is reset to the active style's standard palette.
//
// This is a no-op in builds without the bundled KF6ColorScheme (i.e. normal dev
// builds, where the system platform theme already handles the palette) — see
// the HAVE_BREEZE_KCOLORSCHEME guard in BreezePalette.cpp.
void applyBreezePalette(const QString& styleName, const QString& theme);
