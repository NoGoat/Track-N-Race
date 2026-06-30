#pragma once

#include <QString>
#include <QColor>
#include <nlohmann/json.hpp>

#include "../Labels.h"
#include "CardColors.h"

// ── Compound label (keyed on actual_compound) ─────────────────────────────
// Names come from the library i18n catalog; unknown codes fall back to an em dash.

inline QString tyreLabel(int compound) {
    switch (compound) {
        case 22: case 16: case 17: case 18: case 19: case 20: case 21:
        case  7: case  8:
            return tnr::Ln("tyre.actual", compound);
        default:
            return "—";
    }
}

// Text color keyed on visual_compound (16=soft, 17=medium, 18=hard, 7=int, 8=wet)
// NOT tyre_compound — visual_compound is fixed; tyre_compound varies by weekend
// Returns invalid QColor for hard → caller leaves text at the OS default color
inline QColor tyreTextColor(int visualCompound) {
    switch (visualCompound) {
        case 16: return QColor("#e8002d"); // Soft   — red
        case 17: return QColor("#ffd700"); // Medium — yellow
        case 18: return {};                // Hard   — OS default
        case  7: return QColor("#39b54a"); // INT    — green
        case  8: return QColor("#4488ff"); // WET    — blue
        default: return {};
    }
}

// ── Temperature / wear color helpers ─────────────────────────────────────
// Thresholds live in the shared library spec (temp.tyre / temp.brake / wear),
// so these stay in lockstep with the Electron app and the stat cards.

inline QColor tyreTempColor(int c)  { return tnr::cardColor("temp.tyre", c); }
inline QColor brakeTempColor(int c) { return tnr::cardColor("temp.brake", c); }
inline QColor wearPctColor(int pct) { return tnr::cardColor("wear", pct); }

// ── Tyre set status helpers ───────────────────────────────────────────────

inline QString setStatusText(const nlohmann::json& s) {
    if (s.value("fitted",    false)) return "FITTED";
    if (s.value("available", false)) return s.value("wear", 0) == 0 ? "NEW" : "USED";
    if (s.value("recommended_session", 0) > 0) return "RESERVED";
    return "RETURNED";
}

inline QColor setStatusColor(const nlohmann::json& s) {
    const std::string st = setStatusText(s).toStdString();
    if (st == "FITTED")   return QColor("#5794F2");
    if (st == "NEW")      return QColor("#37872D");
    if (st == "USED")     return QColor("#d4ad04");
    if (st == "RESERVED") return QColor("#a78bfa");
    return QColor("#484c62"); // RETURNED
}
