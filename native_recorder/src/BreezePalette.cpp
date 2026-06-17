#include "BreezePalette.h"

#include <QApplication>
#include <QStyle>
#include <QStyleHints>
#include <QCoreApplication>
#include <QFileInfo>

#ifdef HAVE_BREEZE_KCOLORSCHEME
#include <KColorScheme>
#include <KSharedConfig>
#include <KConfig>

namespace {
bool wantsDark(const QString& theme) {
    if (theme == "dark")  return true;
    if (theme == "light") return false;
    // "system": follow the OS colour scheme Qt reports (Qt 6.5+).
    return QApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
}
}

void applyBreezePalette(const QString& styleName, const QString& theme) {
    if (styleName.compare("breeze", Qt::CaseInsensitive) != 0) {
        // Leaving Breeze: hand the palette back to the active style's default.
        if (QStyle* s = QApplication::style())
            QApplication::setPalette(s->standardPalette());
        return;
    }

    // The Breeze colour-scheme files are bundled next to the executable (copied
    // from the superbuild's share/color-schemes by the CMake bundling step).
    const QString file = QCoreApplication::applicationDirPath() + "/color-schemes/"
        + (wantsDark(theme) ? "BreezeDark.colors" : "BreezeLight.colors");
    if (!QFileInfo::exists(file))
        return;  // no scheme bundled — leave the palette as-is rather than guess

    // SimpleConfig keeps KConfig from cascading into a system kdeglobals, so the
    // palette is built purely from our bundled scheme. createApplicationPalette
    // is the same call the KDE platform theme uses, so the result is identical.
    KSharedConfigPtr cfg = KSharedConfig::openConfig(file, KConfig::SimpleConfig);
    QApplication::setPalette(KColorScheme::createApplicationPalette(cfg));
}

#else  // no bundled KColorScheme — system platform theme owns the palette

void applyBreezePalette(const QString&, const QString&) {}

#endif
