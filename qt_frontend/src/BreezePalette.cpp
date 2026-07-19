#include "BreezePalette.h"

#include <QApplication>
#include <QCoreApplication>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QVariant>

#ifdef HAVE_BREEZE_ICONS
#include <QIcon>
#include <breezeicons.h>
#endif

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

void setupBreezeIconTheme() {
#ifdef HAVE_BREEZE_ICONS
    // Mounts the embedded Breeze icon set at :/icons/breeze and sets it as the
    // QIcon fallback theme (only if none is set yet) — so on Linux the OS icon
    // theme stays primary and Breeze fills the gaps.
    BreezeIcons::initIcons();
#ifdef Q_OS_WIN
    // Windows has no system icon theme, so promote Breeze to the primary theme;
    // otherwise QIcon::fromTheme() would have nothing to resolve against.
    if (QIcon::themeName().isEmpty())
        QIcon::setThemeName(QStringLiteral("breeze"));
#endif
#endif
}

// ── Breeze default UI font (Noto Sans) ───────────────────────────────────────
// Unlike the palette, the font isn't tied to KColorScheme, so this is built in
// every configuration. KDE's Breeze default general font is Noto Sans at 10pt.
namespace {
bool s_breezeFontApplied = false;
}

void applyBreezeFont() {
    if (s_breezeFontApplied)
        return;
    // Register the bundled Noto Sans (fonts.qrc) so "Noto Sans" resolves to our copy
    // rather than depending on a system install (which most Windows machines lack).
    // addApplicationFont dedupes by content, so calling this once is enough for all
    // four weights; the family name they register under is what setFont() asks for.
    for (const char* f : { ":/fonts/NotoSans-Regular.ttf", ":/fonts/NotoSans-Bold.ttf",
                           ":/fonts/NotoSans-Italic.ttf",  ":/fonts/NotoSans-BoldItalic.ttf" })
        QFontDatabase::addApplicationFont(QString::fromLatin1(f));
    QApplication::setFont(QFont(QStringLiteral("Noto Sans"), 10));
    s_breezeFontApplied = true;
}

void restoreDefaultFont() {
    if (!s_breezeFontApplied)
        return;  // we never set the Breeze font — leave the current font alone
    const QVariant v = qApp->property("defaultFont");
    if (v.isValid())
        QApplication::setFont(v.value<QFont>());
    s_breezeFontApplied = false;
}

#ifdef HAVE_BREEZE_ICONS

namespace {
bool s_breezeIconThemeApplied = false;
}

void applyBreezeIconTheme() {
    // initIcons() (called once at startup) registered the "breeze" theme under
    // :/icons/breeze; promote it to the *active* theme so QIcon::fromTheme()
    // resolves against the bundled Breeze icons while Breeze is the style.
    QIcon::setThemeName(QStringLiteral("breeze"));
    s_breezeIconThemeApplied = true;
}

void restoreDefaultIconTheme() {
    if (!s_breezeIconThemeApplied)
        return;  // we never switched the icon theme — leave it alone
    QIcon::setThemeName(qApp->property("defaultIconTheme").toString());
    s_breezeIconThemeApplied = false;
}

#else  // no bundled Breeze icons — the platform's icon theme is used as-is

void applyBreezeIconTheme() {}
void restoreDefaultIconTheme() {}

#endif
