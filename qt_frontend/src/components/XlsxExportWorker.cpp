#include "XlsxExportWorker.h"

#include <tnrp/XlsxExport.h>

#include <algorithm>

XlsxExportWorker::XlsxExportWorker(QString srcTnrdPath, QString destXlsxPath, QObject* parent)
    : QObject(parent), src_(std::move(srcTnrdPath)), dest_(std::move(destXlsxPath)) {}

void XlsxExportWorker::run() {
    std::string err;
    // The library invokes this callback from the current (worker) thread. progress()
    // is connected across the thread boundary, so Qt queues it and the slot runs on
    // the GUI thread. The exporter already bakes its 0..95% build / 100% write bands
    // into done/total, so pct = 100*done/total is the honest overall percentage.
    auto onProgress = [this](size_t done, size_t total, const std::string& stage) {
        const int pct = total > 0 ? static_cast<int>((done * 100) / total) : 0;
        emit progress(std::clamp(pct, 0, 100), QString::fromStdString(stage));
    };

    const bool ok = tnrp::exportTnrdFileToXlsx(src_.toStdString(), dest_.toStdString(),
                                               &err, onProgress);
    emit finished(ok, QString::fromStdString(err));
}
