#pragma once

#include <QString>

#include <cstdint>

#include <tnrp/Labels.h>

// Native-side accessor for the library-owned i18n catalog (tnrp/Labels.h). The
// catalog content lives entirely in libtnrp — this only tracks the active packet
// format and resolves keys through tnrp::labelsFor(format), so the recorder's
// labels switch with the game year (e.g. DRS → "Straight Line Mode" in 2026).
//
// The format is updated on the GUI thread (from the protocol_status row live, or
// the .tnrd header during playback); all widget rendering is also on the GUI
// thread, so no locking is needed.
namespace tnr {

class Labels {
public:
    static Labels& instance() {
        static Labels inst;
        return inst;
    }

    void setFormat(uint16_t f) { if (f) format_ = f; }
    uint16_t format() const { return format_; }

    // Resolve a label key (returns the key itself if unknown).
    QString t(const QString& key) const {
        return QString::fromStdString(tnrp::labelsFor(format_).get(key.toStdString()));
    }
    // Convenience for numeric-suffixed enum groups, e.g. tn("tyre.actual", c).
    QString tn(const QString& group, int n) const {
        return t(group + QChar('.') + QString::number(n));
    }

private:
    uint16_t format_ = 2025;   // sane default before any packet/header is seen
};

// Free-function shorthand used at call sites.
inline QString L(const QString& key)               { return Labels::instance().t(key); }
inline QString Ln(const QString& group, int n)     { return Labels::instance().tn(group, n); }

} // namespace tnr
