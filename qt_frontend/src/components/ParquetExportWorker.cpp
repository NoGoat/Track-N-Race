#include "ParquetExportWorker.h"

#include <tnrp/ParquetExport.h>

#include <algorithm>
#include <cmath>

ParquetExportWorker::ParquetExportWorker(QString srcTnrdPath, QString destParentDir, QObject* parent)
    : QObject(parent), src_(std::move(srcTnrdPath)), destDir_(std::move(destParentDir)) {}

void ParquetExportWorker::run() {
    std::string folder;
    std::string err;
    // The library invokes these callbacks from the current (worker) thread.
    // progress() is connected across the thread boundary, so Qt queues it and
    // the slot runs on the GUI thread.
    auto onProgress = [this](double fraction, const std::string& stage) {
        const int pct = static_cast<int>(std::lround(fraction * 100.0));
        emit progress(std::clamp(pct, 0, 100), QString::fromStdString(stage));
    };
    auto isCancelled = [this] { return cancelled_.load(); };

    const bool ok = tnrp::exportTnrdFileToParquet(src_.toStdString(), destDir_.toStdString(),
                                                  &folder, &err, onProgress, isCancelled);
    emit finished(ok, QString::fromStdString(folder), QString::fromStdString(err));
}
