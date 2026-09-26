#pragma once

#include <QJsonObject>
#include <QString>
#include <functional>

namespace tnr::diagnostics {

// Starts a fresh per-launch log and installs the Qt message handler. Call after
// QApplication metadata is set but before application subsystems are created.
void initialize();
void setFatalFlushHandler(std::function<void()> handler);
// The provider runs on the GUI thread once per memory sample. It should return
// only cheap snapshots of already-retained state; diagnostics must never become
// another owner of telemetry history.
void setMemorySnapshotProvider(std::function<QJsonObject()> provider);
void setMemoryLoggingEnabled(bool enabled);
QString failureReport(const QString& heading, const QString& details);
QString directoryPath();
QString logPath();
void shutdown();

}
