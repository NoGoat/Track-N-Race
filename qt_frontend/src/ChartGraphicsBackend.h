#pragma once

#include <QString>
#include <QVector>

#include <QRhiWidget>

namespace tnr::graphics {

struct BackendInfo {
    QString key;
    QString label;
    QRhiWidget::Api api = QRhiWidget::Api::Null;
};

// Select the backend once, after QApplication exists and before MainWindow
// creates the first QRhiWidget. Qt binds one RHI backend to an entire top-level
// widget hierarchy, so the selected backend is immutable for the process.
// Only the backends tried up to the selected one are probed here.
void initialize();

// Every backend that works on this machine. The first call probes the ones
// initialize() skipped.
const QVector<BackendInfo>& supportedBackends();
QString requestedBackendKey();
QString activeBackendKey();
QString activeBackendLabel();
QRhiWidget::Api activeApi();

// "auto" is always valid. Explicit keys are accepted only when they probe
// successfully on this machine (see supportedBackends()).
bool isSelectableBackend(const QString& key);

} // namespace tnr::graphics
