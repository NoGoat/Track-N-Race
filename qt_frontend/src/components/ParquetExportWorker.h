#pragma once

#include <QObject>
#include <QString>

#include <atomic>

// Runs tnrp::exportTnrdFileToParquet off the GUI thread. Create one, move it onto
// a QThread, and invoke run() (e.g. from the thread's started() signal): progress()
// and finished() are emitted across the thread boundary (queued to the GUI thread),
// so the caller can drive a progress overlay without blocking. Mirrors the Electron
// ExportParquetWorker (electron-frontend/node_addon/addon.cpp), which runs the same
// library call off the JS thread and marshals {pct, stage} back.
class ParquetExportWorker : public QObject {
    Q_OBJECT

public:
    // destParentDir: the folder the user picked; the library creates
    // "<recording>_parquet" (numbered when taken) inside it.
    ParquetExportWorker(QString srcTnrdPath, QString destParentDir, QObject* parent = nullptr);

    // Thread-safe: asks a running export to stop; finished() then reports
    // ok=false with error "cancelled".
    void cancel() { cancelled_.store(true); }

public slots:
    void run();   // do the export; runs once on the worker thread

signals:
    void progress(int pct, const QString& stage);   // 0..100 + human-readable phase
    // folder: the created export folder when ok; error non-empty only when !ok.
    void finished(bool ok, const QString& folder, const QString& error);

private:
    QString src_;
    QString destDir_;
    std::atomic<bool> cancelled_{false};
};
