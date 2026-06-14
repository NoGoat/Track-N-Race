#pragma once

#include <QString>
#include <QColor>
#include <nlohmann/json.hpp>

// ── Compound label (keyed on actual_compound) ─────────────────────────────

inline QString tyreLabel(int compound) {
    switch (compound) {
        case 22: return "C6";
        case 16: return "C5";
        case 17: return "C4";
        case 18: return "C3";
        case 19: return "C2";
        case 20: return "C1";
        case 21: return "C0";
        case  7: return "INT";
        case  8: return "WET";
        default: return "—";
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

inline QColor tyreTempColor(int c) {
    if (c < 60)   return QColor("#5794F2");
    if (c < 80)   return QColor("#d4ad04");
    if (c <= 110) return QColor("#37872D");
    if (c <= 130) return QColor("#c47d0e");
    return QColor("#C4162A");
}

inline QColor brakeTempColor(int c) {
    if (c < 200) return QColor("#37872D");
    if (c < 400) return QColor("#d4ad04");
    if (c < 600) return QColor("#c47d0e");
    return QColor("#C4162A");
}

inline QColor wearPctColor(int pct) {
    if (pct < 20) return QColor("#73BF69");
    if (pct < 40) return QColor("#A8D436");
    if (pct < 60) return QColor("#FADE2A");
    if (pct < 80) return QColor("#FF9830");
    return QColor("#C4162A");
}

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
