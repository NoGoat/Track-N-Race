#include "ToastEvents.h"
#include "../Labels.h"

#include <QRegularExpression>
#include <QStringList>

namespace {

// Last word of a participant's name, by car index — mirrors lastName() in App.tsx.
QString lastName(const tnrp::ParticipantsRow* participants, int idx) {
    if (participants) {
        for (const tnrp::Driver& d : participants->drivers) {
            if (d.idx == idx) {
                const QString name = QString::fromStdString(d.name).trimmed();
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

QString enumLabel(const QString& group, int id) {
    const QString key = group + QChar('.') + QString::number(id);
    const QString label = tnr::L(key);
    return label == key ? QString() : label;
}

} // namespace

ToastSpec raceLeaderToast(int carIdx, const tnrp::ParticipantsRow* participants) {
    return ToastSpec{QStringLiteral("New Race Leader"), lastName(participants, carIdx),
                     QColor("#5794F2"), false, false};
}

std::optional<ToastSpec> buildToast(const tnrp::RaceEventRow& event,
                                    const tnrp::ParticipantsRow* participants) {
    const std::string& code = event.code;
    const int carIdx = event.car_idx.value_or(0);

    if (code == "FTLP")
        return ToastSpec{ "Fastest Lap",
            QString("%1  ·  %2").arg(lastName(participants, carIdx),
                                     fmtLap(event.lap_time_s.value_or(0.0f))),
            QColor("#BF5FFF") };
    if (code == "SCAR") {
        // Electron: SC/VSC ending in Resume Race queues a transient green banner;
        // every other phase lives in the persistent banner (safetyCarBanner).
        const int type = event.safety_car_type.value_or(0);
        if (event.event_type.value_or(-1) == 3 && (type == 1 || type == 2))
            return ToastSpec{ "Resume Race", {}, QColor("#37872D") };
        return std::nullopt;
    }
    // Electron's banner wording is fixed English, Straight Line Mode included.
    if (code == "DRSE") return ToastSpec{ "DRS Enabled",  {}, QColor("#37872D") };
    if (code == "DRSD") return ToastSpec{ "DRS Disabled", {}, QColor("#8e8e8e") };
    if (code == "RDFL") return ToastSpec{ "Red Flag",     {}, QColor("#e10600") };
    if (code == "PENA") {
        const int pt = event.penalty_type.value_or(0);
        QString label = enumLabel(QStringLiteral("penalty"), pt);
        if (label.isEmpty()) return std::nullopt;
        const QColor color = pt == 5 ? QColor("#ffd700")
                           : (pt == 2 || pt == 4) ? QColor("#c47d0e")
                           : QColor("#e10600");
        if ((pt == 1 || pt == 4) && event.penalty_time_s && *event.penalty_time_s > 0)
            label += QString(" %1s").arg(*event.penalty_time_s);
        const QString driver = lastName(participants, carIdx);
        const QString infr = event.infringement_type
            ? enumLabel(QStringLiteral("infringe"), *event.infringement_type) : QString();
        const QString sub = (pt == 5 && !infr.isEmpty()) ? QString("%1  ·  %2").arg(driver, infr) : driver;
        return ToastSpec{ label, sub, color };
    }
    if (code == "DTSV") return ToastSpec{ "DT Served",     lastName(participants, carIdx), QColor("#a0a8b8") };
    if (code == "SGSV") return ToastSpec{ "SG Served",     lastName(participants, carIdx), QColor("#a0a8b8") };
    if (code == "RTMT") return ToastSpec{ "Retired",       lastName(participants, carIdx), QColor("#a0a8b8") };
    if (code == "RCWN") return ToastSpec{ "Race Winner",   lastName(participants, carIdx), QColor("#FFD700") };
    if (code == "CHQF") return ToastSpec{ "Chequered Flag", {}, QColor("#7a7a7a") };
    if (code == "LGOT") return ToastSpec{ "Lights Out",     {}, QColor("#37872D") };
    if (code == "SSTA") return ToastSpec{ "Session Start",  {}, QColor("#5794F2") };
    if (code == "SEND") return ToastSpec{ "Session End",    {}, QColor("#5794F2") };
    // OVTK, SPTP and anything else: no toast.
    return std::nullopt;
}

QString safetyCarTypeLabel(int type) {
    switch (type) {
        case 2:  return QStringLiteral("Virtual Safety Car");
        case 3:  return QStringLiteral("Formation Lap");
        default: return QStringLiteral("Safety Car");
    }
}

QString safetyCarActionLabel(int type, int action) {
    if ((type == 1 || type == 2) && action == 0) return {};
    if ((type == 1 || type == 2) && action == 1) return QStringLiteral("Ending");
    switch (action) {
        case 0:  return QStringLiteral("Deployed");
        case 1:  return QStringLiteral("Returning");
        case 2:  return QStringLiteral("Returned");
        case 3:  return QStringLiteral("Resume Race");
        default: return {};
    }
}

std::optional<ToastSpec> safetyCarBanner(std::optional<int> safetyCarStatus,
                                         const tnrp::RaceEventRow* latestScar) {
    if (!safetyCarStatus) return std::nullopt;

    // Session packets can keep the formation-lap status after recording starts
    // while the first SCAR is already its terminal Resume Race, so the latest
    // event transition wins over the status (also after playback seeks).
    const std::optional<int> action = latestScar ? latestScar->event_type : std::nullopt;
    if (action && (*action == 2 || *action == 3)) return std::nullopt;

    const int status = latestScar && action && (*action == 0 || *action == 1)
        ? latestScar->safety_car_type.value_or(*safetyCarStatus)
        : *safetyCarStatus;
    if (status == 0) return std::nullopt;

    const QColor color = status == 2 ? QColor("#ffb347") : QColor("#ffd700");
    return ToastSpec{ safetyCarTypeLabel(status), safetyCarActionLabel(status, action.value_or(0)),
                      color, true, true };
}
