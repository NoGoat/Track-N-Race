#pragma once

#include <QString>
#include <functional>

namespace tnr::diagnostics {

// Starts a fresh per-launch log and installs the Qt message handler. Call after
// QApplication metadata is set but before application subsystems are created.
void initialize();
void setFatalFlushHandler(std::function<void()> handler);
QString failureReport(const QString& heading, const QString& details);
QString logPath();
void shutdown();

}
