#include <QApplication>
#include <QStyle>
#include <QStyleFactory>
#include <QSettings>
#include <QVariant>
#include <QStyleHints>
#include <QIcon>
#include <QImageReader>
#include <QPixmap>
#include <QFont>
#include <QStandardPaths>
#include <QDir>
#include <cstdio>
#include <array>
#include "MainWindow.h"
#include "BreezePalette.h"
#include "IconUtils.h"

// Build a multi-resolution app icon from every frame in the .ico. QIcon(".ico")
// alone only takes the first frame (16x16 here), which the window manager then
// upscales into a pixelated mess for the (HiDPI) titlebar. Adding all frames lets
// Qt pick the right size and downscale the large ones smoothly.
#ifdef Q_OS_LINUX
// In the AppImage, qt.conf restricts Qt's plugin search to the bundled
// AppDir/usr/plugins/, so when "System Default" style is resolved (see
// below), QStyleFactory can't see whatever style is actually installed on
// the host (Breeze, Kvantum, adwaita-qt, ...) and silently falls back to
// Qt's built-in Fusion. Ask the host's own qtpaths for its real Qt plugins
// directory and add it to the search path so "System Default" reflects the
// real system, not just this app's bundled (portable, explicit-choice-only)
// copy of Breeze. No-op in non-bundled builds (Qt already searches the
// system path there) and if qtpaths isn't found on the host.
static void addHostQtPluginPath() {
    QString qtpaths = QStandardPaths::findExecutable("qtpaths6");
    if (qtpaths.isEmpty()) qtpaths = QStandardPaths::findExecutable("qtpaths");
    if (qtpaths.isEmpty()) return;

    // This runs before QApplication is constructed (required — see call site),
    // so there's no event dispatcher yet and QProcess isn't usable. Shell out
    // directly with popen() instead, which has no such dependency.
    QString escaped = qtpaths;
    escaped.replace(QLatin1String("'"), QLatin1String("'\\''"));
    const QString cmd = QLatin1Char('\'') + escaped + QLatin1String("' -query QT_INSTALL_PLUGINS 2>/dev/null");

    FILE* pipe = popen(cmd.toLocal8Bit().constData(), "r");
    if (!pipe) return;
    std::array<char, 512> buf{};
    QString output;
    while (fgets(buf.data(), int(buf.size()), pipe))
        output += QString::fromLocal8Bit(buf.data());
    pclose(pipe);

    const QString hostPlugins = output.trimmed();
    if (hostPlugins.isEmpty() || !QDir(hostPlugins).exists()) return;

    // QApplication::addLibraryPath() here would be a no-op: it's discarded
    // once qt.conf processing runs during QApplication's own construction
    // (confirmed empirically — the path silently vanished from
    // QApplication::libraryPaths() after construction). QT_PLUGIN_PATH is
    // read *during* construction and merges additively with qt.conf's
    // Plugins path, so it survives — same mechanism already used for
    // QT_QPA_PLATFORMTHEME just above.
    QByteArray existing = qgetenv("QT_PLUGIN_PATH");
    QByteArray combined = existing.isEmpty()
        ? hostPlugins.toLocal8Bit()
        : hostPlugins.toLocal8Bit() + ":" + existing;
    qputenv("QT_PLUGIN_PATH", combined);
}
#endif

static QIcon loadAppIcon(const QString& resource) {
    QIcon icon;
    QImageReader reader(resource);
    const int n = reader.imageCount();
    for (int i = 0; i < n; ++i) {
        reader.jumpToImage(i);
        const QImage img = reader.read();
        if (!img.isNull()) icon.addPixmap(QPixmap::fromImage(img));
    }
    if (icon.isNull()) icon = QIcon(resource);   // fallback
    return icon;
}

int main(int argc, char* argv[]) {
#ifdef Q_OS_LINUX
    // On Linux the XDG desktop-portal plugin (bundled as platformthemes/libqxdgdesktopportal.so)
    // gives native file dialogs on both X11 and Wayland. Qt reads QT_QPA_PLATFORMTHEME
    // during QApplication construction, so it must be set before that. If the portal
    // D-Bus service is not available Qt falls back to the built-in file dialog automatically.
    if (qgetenv("QT_QPA_PLATFORMTHEME").isEmpty())
        qputenv("QT_QPA_PLATFORMTHEME", "xdgdesktopportal");

    // Must run before QApplication is constructed: the platform theme plugin
    // (QT_QPA_PLATFORMTHEME, set above) that detects "this is a KDE session,
    // use Breeze" is loaded *during* QApplication's own construction. In the
    // AppImage, qt.conf restricts that construction-time plugin search to the
    // bundle only, so without this the theme plugin can never see the host's
    // real desktop integration and QApplication::style() ends up as "fusion"
    // regardless of what's added to the search path afterwards.
    addHostQtPluginPath();
#endif
    QApplication app(argc, argv);
    app.setApplicationName("Track N Race Background Recorder");
    app.setApplicationVersion(APP_VERSION);   // defined by CMake from PROJECT_VERSION
    app.setOrganizationName("TrackNRace");
    app.setWindowIcon(loadAppIcon(":/icon.ico"));

    // Remember the platform's default style *before* anything can override it, so
    // a later "System default" choice can be restored at runtime (the style's
    // objectName is its lowercased QStyleFactory key, e.g. "breeze"/"windows11").
    app.setProperty("defaultStyleName", QApplication::style()->objectName());

    // Snapshot the pristine platform palette too — restoreDefaultPalette() puts it
    // back when the user switches away from Breeze, so other styles aren't left
    // wearing Breeze's colours.
    app.setProperty("defaultPalette", QVariant::fromValue(app.palette()));

    // Record the OS colour scheme *before* any setColorScheme() override, so the
    // toolbar logic can tell when the app's forced mode differs from the system
    // (see MainWindow::updateToolbarColorScheme).
    app.setProperty("osColorScheme", int(QApplication::styleHints()->colorScheme()));

    // Snapshot the platform font and icon theme too, so leaving Breeze can restore
    // them (Breeze swaps in Noto Sans + the bundled Breeze icon theme). The icon
    // theme is captured *before* setupBreezeIconTheme() so it's the pristine value.
    app.setProperty("defaultFont", QVariant::fromValue(app.font()));
    app.setProperty("defaultIconTheme", QIcon::themeName());

    // Let QStyleFactory discover a Breeze (or any) style plugin bundled next to
    // the executable under plugins/styles/. Harmless when absent — dev builds
    // just keep finding the system styles on the default plugin path.
    QApplication::addLibraryPath(QCoreApplication::applicationDirPath() + "/plugins");

    // Set up the bundled Breeze icon theme: OS theme stays primary on Linux with
    // Breeze as the fallback; on Windows Breeze becomes the icon theme. No-op in
    // non-bundled builds. (Independent of the selected widget style.)
    setupBreezeIconTheme();

    // Apply the saved style before the window is built so the first paint is
    // already correct. Key matches MainWindow's QSettings store; an empty value
    // or "system" leaves Qt's native platform style untouched.
    QSettings settings("TrackNRace", "NativeRecorder");
    const QString styleName = settings.value("style").toString();
    if (!styleName.isEmpty() && styleName != "system") {
        // Wrap Breeze so its monochrome standard button icons (QMessageBox /
        // QDialogButtonBox) get recoloured for the palette — see setApplicationStyle.
        setApplicationStyle(QStyleFactory::create(styleName),
                            styleName.compare("breeze", Qt::CaseInsensitive) == 0);
    }

    // Only Breeze needs its palette applied (its plugin doesn't set one); every
    // other style keeps the platform palette. Breeze also uses its default font
    // (Noto Sans) and the bundled Breeze icon theme. All no-ops in non-bundled
    // builds / for other styles.
    if (styleName.compare("breeze", Qt::CaseInsensitive) == 0) {
        applyBreezePalette(settings.value("theme", "system").toString());
        applyBreezeFont();
        applyBreezeIconTheme();
    }

    MainWindow w;
    w.show();
    return app.exec();
}
