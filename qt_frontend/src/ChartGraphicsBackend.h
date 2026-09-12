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

// Probe the graphics APIs once, after QApplication exists and before MainWindow
// creates the first QRhiWidget. Qt binds one RHI backend to an entire top-level
// widget hierarchy, so the selected backend is immutable for the process.
void initialize();

const QVector<BackendInfo>& supportedBackends();
QString requestedBackendKey();
QString activeBackendKey();
QString activeBackendLabel();
QRhiWidget::Api activeApi();

// "auto" is always valid. Explicit keys are accepted only when they were
// successfully probed on this machine during initialize().
bool isSelectableBackend(const QString& key);

} // namespace tnr::graphics
