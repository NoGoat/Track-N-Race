#pragma once

#include <QByteArray>
#include <optional>

// The paired-display engine state holds the desktop's signing seed, so it is
// kept out of QSettings in plain text: DPAPI (bound to the Windows user) on
// Windows, the Secret Service keyring on Linux. Where neither is available
// the document is stored as is.
namespace PairStateVault {

// The value to store in QSettings for this engine document.
QByteArray seal(const QByteArray& json);

// The engine document behind a QSettings value. std::nullopt when it is
// sealed but cannot be opened right now (keyring locked or missing).
std::optional<QByteArray> open(const QByteArray& stored);

} // namespace PairStateVault
