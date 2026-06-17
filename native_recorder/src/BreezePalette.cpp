#include "BreezePalette.h"

#include <QApplication>
#include <QCoreApplication>
#include <QFileInfo>

#ifdef HAVE_BREEZE_KCOLORSCHEME
#include <QStyleHints>
#include <QPalette>
#include <QVariant>
#include <KColorScheme>
#include <KSharedConfig>
#include <KConfig>

namespace {
// Tracks whether *we* currently own the application palette. Only true after we
// apply a Breeze palette, so restoreDefaultPalette() never clobbers a palette
// that another style (Kvantum, windows11, the platform) legitimately set.
bool s_breezeApplied = false;

bool wantsDark(const QString& theme) {
    if (theme == "dark")  return true;
    if (theme == "light") return false;
    // "system": follow the OS colour scheme Qt reports (Qt 6.5+).
    return QApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
}
}

void applyBreezePalette(const QString& theme) {
    // Breeze colour-scheme files are bundled next to the executable by the CMake
    // bundling step (copied from the superbuild's color-schemes data dir).
    const QString file = QCoreApplication::applicationDirPath() + "/color-schemes/"
        + (wantsDark(theme) ? "BreezeDark.colors" : "BreezeLight.colors");
    if (!QFileInfo::exists(file))
        return;  // no scheme bundled — leave the palette as-is rather than guess

    // SimpleConfig keeps KConfig from cascading into a system kdeglobals, so the
    // palette is built purely from our bundled scheme. createApplicationPalette
    // is the same call the KDE platform theme uses, so the result is identical.
    KSharedConfigPtr cfg = KSharedConfig::openConfig(file, KConfig::SimpleConfig);
    QApplication::setPalette(KColorScheme::createApplicationPalette(cfg));
    s_breezeApplied = true;
}

void restoreDefaultPalette() {
    if (!s_breezeApplied)
        return;  // we never hijacked the palette — leave the current style alone
    const QVariant v = qApp->property("defaultPalette");
    if (v.isValid())
        QApplication::setPalette(v.value<QPalette>());
    s_breezeApplied = false;
}

#else  // no bundled KColorScheme — system platform theme owns the palette

void applyBreezePalette(const QString&) {}
void restoreDefaultPalette() {}

#endif
