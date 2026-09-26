#pragma once

#include <QImage>
#include <QString>

// Encodes the engine-owned pairing URI as a byte-mode QR code. Pairing
// semantics remain in libtnrp; this helper only turns the public payload into
// pixels for Settings.
QImage pairingQrCodeImage(const QString& payload, int pixelSize = 260);
