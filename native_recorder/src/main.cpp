#include <QApplication>
#include "MainWindow.h"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("Track N Race Background Recorder");
    app.setOrganizationName("TrackNRace");
    app.setWindowIcon(QIcon(":/icon.ico"));

    MainWindow w;
    w.show();
    return app.exec();
}
