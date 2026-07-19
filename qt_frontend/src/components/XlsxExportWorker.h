#pragma once

#include <QObject>
#include <QString>

// Runs tnrp::exportTnrdFileToXlsx off the GUI thread. Create one, move it onto a
// QThread, and invoke run() (e.g. from the thread's started() signal): progress()
// and finished() are emitted across the thread boundary (queued to the GUI thread),
// so the caller can drive a progress overlay without blocking. Mirrors the Electron
// ExportXlsxWorker (electron-frontend/node_addon/addon.cpp), which runs the same library call
// off the JS thread and marshals {pct, stage} back.
class XlsxExportWorker : public QObject {
    Q_OBJECT

public:
    XlsxExportWorker(QString srcTnrdPath, QString destXlsxPath, QObject* parent = nullptr);

public slots:
    void run();   // do the export; runs once on the worker thread

signals:
    void progress(int pct, const QString& stage);   // 0..100 + human-readable phase
    void finished(bool ok, const QString& error);   // error non-empty only when !ok

private:
    QString src_;
    QString dest_;
};
