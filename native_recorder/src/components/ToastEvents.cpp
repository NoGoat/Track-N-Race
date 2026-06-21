#include "ToastEvents.h"

#include <QRegularExpression>
#include <QStringList>

#include <array>

namespace {

// Last word of a participant's name, by car index — mirrors lastName() in App.tsx.
QString lastName(const nlohmann::json& participants, int idx) {
    if (participants.contains("drivers") && participants["drivers"].is_array()) {
        for (const auto& d : participants["drivers"]) {
            if (d.value("idx", -1) == idx) {
                const QString name = QString::fromStdString(d.value("name", std::string())).trimmed();
                if (name.isEmpty()) break;
                const QStringList parts = name.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
                return parts.isEmpty() ? name : parts.last();
            }
        }
    }
    return QString("Car %1").arg(idx);
}

// "M:SS.sss" — mirrors fmtLap() in App.tsx.
QString fmtLap(double s) {
    const int m = (int)(s / 60.0);
    const double rem = s - m * 60.0;
    return QString("%1:%2").arg(m).arg(rem, 6, 'f', 3, QChar('0'));
}

// Infringement label by index — direct port of INFRINGEMENT_LABELS (App.tsx:133).
const char* infringementLabel(int i) {
    static const std::array<const char*, 49> L = {
        "Blocking by slowing", "Blocking wrong way", "Reversing off start line",
        "Big collision", "Small collision", "Collision — failed to hand back",
        "Collision — attack from rear", "SC delta exceeded", "SC illegal overtake",
        "SC exceeding allowed pace", "Cornering under SC", "SC must pit this lap",
        "SC pit lane curfew", "Pit lane too fast", "Unsafe release",
        "Pit re-entry too slow", "In pit too fast", "Unsafe release",
        "Escape from pit", "Ignoring blue flags", "Ignoring yellow flags",
        "Ignoring drive through", "Too many drive throughs", "DT — serve this lap",
        "DT — serve next lap", "Pit stop failed to serve", "Hanging around",
        "Hang around for SC", "Return to pits", "Tyre regulations",
        "Lap invalidated", "This + next lap invalid", "Lap invalid (no reason)",
        "This + next invalid (no reason)", "This + prev lap invalid",
        "This + prev invalid (no reason)", "Retired", "Black flag timer",
        "Unserved stop-go", "Unserved drive through", "Engine change",
        "Gearbox change", "Parc fermé change", "League grid penalty",
        "Retry penalty", "Illegal time gain", "Mandatory pit stop",
        "Attribute assigned", "Corner cutting",
    };
    return (i >= 0 && i < (int)L.size()) ? L[i] : nullptr;
}

} // namespace

std::optional<ToastSpec> buildToast(const nlohmann::json& event,
                                    const nlohmann::json& participants) {
    const std::string code = event.value("code", std::string());
    const int carIdx = event.value("car_idx", 0);

    if (code == "FTLP")
        return ToastSpec{ "Fastest Lap",
            QString("%1  ·  %2").arg(lastName(participants, carIdx),
                                     fmtLap(event.value("lap_time_s", 0.0))),
            QColor("#BF5FFF") };
    if (code == "DRSE") return ToastSpec{ "DRS Enabled",  {}, QColor("#37872D") };
    if (code == "DRSD") return ToastSpec{ "DRS Disabled", {}, QColor("#8e8e8e") };
    if (code == "RDFL") return ToastSpec{ "Red Flag",     {}, QColor("#e10600") };
    if (code == "PENA") {
        const int pt = event.value("penalty_type", 0);
        struct P { const char* label; const char* color; };
        // Index by penalty_type; type 3 has no mapping (skipped), matching Electron.
        static const std::array<P, 7> M = {{
            { "Drive Through", "#e10600" }, { "Stop Go",  "#e10600" },
            { "Grid Penalty",  "#c47d0e" }, { nullptr,    nullptr   },
            { "Time Penalty",  "#c47d0e" }, { "Warning",  "#ffd700" },
            { "Disqualified",  "#e10600" },
        }};
        if (pt < 0 || pt >= (int)M.size() || !M[pt].label) return std::nullopt;
        QString label = M[pt].label;
        if ((pt == 1 || pt == 4) && event.contains("penalty_time_s") &&
            event["penalty_time_s"].get<double>() > 0)
            label += QString(" %1s").arg(event["penalty_time_s"].get<double>());
        const QString driver = lastName(participants, carIdx);
        const char* infr = event.contains("infringement_type")
            ? infringementLabel(event["infringement_type"].get<int>()) : nullptr;
        const QString sub = (pt == 5 && infr) ? QString("%1  ·  %2").arg(driver, infr) : driver;
        return ToastSpec{ label, sub, QColor(M[pt].color) };
    }
    if (code == "DTSV") return ToastSpec{ "DT Served",     lastName(participants, carIdx), QColor("#a0a8b8") };
    if (code == "SGSV") return ToastSpec{ "SG Served",     lastName(participants, carIdx), QColor("#a0a8b8") };
    if (code == "RTMT") return ToastSpec{ "Retired",       lastName(participants, carIdx), QColor("#a0a8b8") };
    if (code == "RCWN") return ToastSpec{ "Race Winner",   lastName(participants, carIdx), QColor("#FFD700") };
    if (code == "CHQF") return ToastSpec{ "Chequered Flag", {}, QColor("#7a7a7a") };
    if (code == "LGOT") return ToastSpec{ "Lights Out",     {}, QColor("#37872D") };
    if (code == "SSTA") return ToastSpec{ "Session Start",  {}, QColor("#5794F2") };
    if (code == "SEND") return ToastSpec{ "Session End",    {}, QColor("#5794F2") };
    // SCAR (session-packet driven), OVTK, SPTP and anything else: no toast.
    return std::nullopt;
}

std::optional<ToastSpec> safetyCarToast(int oldStatus, int newStatus) {
    if (newStatus == oldStatus) return std::nullopt;
    switch (newStatus) {
        case 1: return ToastSpec{ "Safety Car",         {}, QColor("#ffd700"), true,  true };
        case 2: return ToastSpec{ "Virtual Safety Car", {}, QColor("#ffb347"), true,  true };
        case 3: return ToastSpec{ "Formation Lap",      {}, QColor("#ffd700"), true,  true };
        case 0:  // back to green — caller dismisses the persistent banner silently (no toast)
            return std::nullopt;
        default: return std::nullopt;
    }
}
