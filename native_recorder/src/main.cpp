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
#include "MainWindow.h"
#include "BreezePalette.h"
#include "IconUtils.h"

// Build a multi-resolution app icon from every frame in the .ico. QIcon(".ico")
// alone only takes the first frame (16x16 here), which the window manager then
// upscales into a pixelated mess for the (HiDPI) titlebar. Adding all frames lets
// Qt pick the right size and downscale the large ones smoothly.
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
#endif
    QApplication app(argc, argv);
    app.setApplicationName("Track N Race Background Recorder");
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
