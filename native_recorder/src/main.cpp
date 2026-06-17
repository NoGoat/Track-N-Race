#include <QApplication>
#include <QStyle>
#include <QStyleFactory>
#include <QSettings>
#include "MainWindow.h"
#include "BreezePalette.h"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("Track N Race Background Recorder");
    app.setOrganizationName("TrackNRace");
    app.setWindowIcon(QIcon(":/icon.ico"));

    // Remember the platform's default style *before* anything can override it, so
    // a later "System default" choice can be restored at runtime (the style's
    // objectName is its lowercased QStyleFactory key, e.g. "breeze"/"windows11").
    app.setProperty("defaultStyleName", QApplication::style()->objectName());

    // Let QStyleFactory discover a Breeze (or any) style plugin bundled next to
    // the executable under plugins/styles/. Harmless when absent — dev builds
    // just keep finding the system styles on the default plugin path.
    QApplication::addLibraryPath(QCoreApplication::applicationDirPath() + "/plugins");

    // Apply the saved style before the window is built so the first paint is
    // already correct. Key matches MainWindow's QSettings store; an empty value
    // or "system" leaves Qt's native platform style untouched.
    QSettings settings("TrackNRace", "NativeRecorder");
    const QString styleName = settings.value("style").toString();
    if (!styleName.isEmpty() && styleName != "system") {
        if (QStyle* s = QStyleFactory::create(styleName))
            QApplication::setStyle(s);
    }

    // Give the (bundled) Breeze style the real KDE palette before first paint;
    // no-op for other styles and in non-bundled builds. See BreezePalette.h.
    applyBreezePalette(styleName.isEmpty() ? "system" : styleName,
                       settings.value("theme", "system").toString());

    MainWindow w;
    w.show();
    return app.exec();
}
