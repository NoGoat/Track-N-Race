#pragma once

#include <QString>
#include <QColor>

#include <tnrp/control_rows.h>

#include "../Labels.h"
#include "CardColors.h"

// ── Compound label (keyed on actual_compound) ─────────────────────────────
// Names come from the library i18n catalog; unknown codes fall back to an em dash.

inline QString tyreLabel(int compound) {
    switch (compound) {
        case 22: case 16: case 17: case 18: case 19: case 20: case 21:
        case 11: case 12: case 13: case 14: case 15:
        case  7: case  8:
            return tnr::Ln("tyre.actual", compound);
        default:
            return "—";
    }
}

// F2's actual compounds carry their own fixed colors; the reported visual
// compound is only authoritative for F1 compounds. Returns invalid QColor for
// F1 hard/unknown values so the caller can retain its native text color.
inline QColor tyreTextColor(int actualCompound, int visualCompound) {
    switch (actualCompound) {
        case 11: return QColor("#a855f7"); // Supersoft — purple
        case 12: return QColor("#e8002d"); // Soft      — red
        case 13: return QColor("#ffd700"); // Medium    — yellow
        case 14: return QColor("#ffffff"); // Hard      — white
        case 15: return QColor("#4488ff"); // Wet       — blue
        default: break;
    }
    switch (visualCompound) {
        case 16: return QColor("#e8002d"); // Soft   — red
        case 17: return QColor("#ffd700"); // Medium — yellow
        case 18: return {};                // Hard   — OS default
        case  7: return QColor("#39b54a"); // INT    — green
        case  8: return QColor("#4488ff"); // WET    — blue
        default: return {};
    }
}

inline bool isWetTyreCompound(int actualCompound) {
    return actualCompound == 7 || actualCompound == 8 || actualCompound == 15;
}

inline int dryTyreCompoundOrder(int actualCompound, int visualCompound) {
    if (actualCompound >= 11 && actualCompound <= 14) return actualCompound - 11;
    switch (visualCompound) {
        case 16: return 0;
        case 17: return 1;
        case 18: return 2;
        default: return 3;
    }
}

inline int wetTyreCompoundOrder(int actualCompound) {
    if (actualCompound == 7) return 0;
    if (actualCompound == 8) return 1;
    return 2;
}

// ── Temperature / wear color helpers ─────────────────────────────────────
// Thresholds live in the shared library spec (temp.tyre / temp.brake / wear),
// so these stay in lockstep with the Electron app and the stat cards.

inline QColor tyreTempColor(int c)  { return tnr::cardColor("temp.tyre", c); }
inline QColor brakeTempColor(int c) { return tnr::cardColor("temp.brake", c); }
inline QColor wearPctColor(int pct) { return tnr::cardColor("wear", pct); }

// ── Tyre set status helpers ───────────────────────────────────────────────

inline QString setStatusText(const tnrp::TyreSet& s) {
    if (s.fitted)    return "FITTED";
    if (s.available) return s.wear == 0 ? "NEW" : "USED";
    if (s.recommended_session > 0) return "RESERVED";
    return "RETURNED";
}

inline QColor setStatusColor(const tnrp::TyreSet& s) {
    const std::string st = setStatusText(s).toStdString();
    if (st == "FITTED")   return QColor("#5794F2");
    if (st == "NEW")      return QColor("#37872D");
    if (st == "USED")     return QColor("#d4ad04");
    if (st == "RESERVED") return QColor("#a78bfa");
    return QColor("#484c62"); // RETURNED
}
