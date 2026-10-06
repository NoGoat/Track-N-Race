#include "AnalyzeMetrics.h"

#include <QLocale>
#include <cmath>

namespace {
AnalyzeMetric M(const char* id, const char* group, const char* label, AnalyzeSource source,
                const char* field, const char* color, const char* scale, double min, double max,
                const char* unit, int precision = 0, bool step = false) {
    return {id, group, label, source, field, QColor(color), scale, min, max, unit, precision, step};
}
}

const QVector<AnalyzeMetric>& analyzeMetrics() {
    static const QVector<AnalyzeMetric> v = [] {
        QVector<AnalyzeMetric> out = {
            M("speed","Driving","Speed",AnalyzeSource::Telemetry,"speed","#37872D","speed",0,380,"km/h"),
            M("rpm","Driving","RPM",AnalyzeSource::Telemetry,"rpm","#C4162A","rpm",0,16000,"rpm",0),
            M("gear","Driving","Gear",AnalyzeSource::Telemetry,"gear","#5794F2","gear",0.5,8.5,"",0,true),
            M("throttle","Driving","Throttle",AnalyzeSource::Telemetry,"throttle","#37872D","input-positive",0,100,"%",0,true),
            M("brake","Driving","Brake",AnalyzeSource::Telemetry,"brake","#C4162A","input-positive",0,100,"%",0,true),
            M("steering","Driving","Steering",AnalyzeSource::Telemetry,"steering","#BF5FFF","input-signed",-100,100,"%"),
            M("ers","Driving","ERS",AnalyzeSource::Status,"ers","#FADE2A","percent",0,100,"%",1),
            M("g-lateral","Motion","Lateral G",AnalyzeSource::Motion,"g_lat","#F0A500","g-force",-6,6,"g",2),
            M("g-longitudinal","Motion","Longitudinal G",AnalyzeSource::Motion,"g_long","#5794F2","g-force",-6,6,"g",2),
            M("ride-front","Motion","Front Ride Height",AnalyzeSource::MotionEx,"front","#73BF69","ride-height",0,50,"mm",1),
            M("ride-rear","Motion","Rear Ride Height",AnalyzeSource::MotionEx,"rear","#B877DB","ride-height",0,50,"mm",1),
            M("power-ice","Power","ICE Power",AnalyzeSource::Status,"ice","#5794F2","power",0,500,"kW",1),
            M("power-mguk","Power","MGU-K Power",AnalyzeSource::Status,"mguk","#FADE2A","power",0,500,"kW",1),
            M("harvest-mguk","Power","MGU-K Harvest",AnalyzeSource::Status,"harvest_k","#37872D","harvest",0,4000,"kJ",1),
            M("harvest-mguh","Power","MGU-H Harvest",AnalyzeSource::Status,"harvest_h","#C4162A","harvest",0,4000,"kJ",1),
            M("fuel","Power","Fuel",AnalyzeSource::Status,"fuel","#F0A500","fuel",0,110,"kg",2),
        };
        const struct { const char* key; const char* label; const char* color; } corners[] = {
            {"fl","FL","#e10600"},{"fr","FR","#4488ff"},{"rl","RL","#37872D"},{"rr","RR","#ffd700"}
        };
        for (const auto& c : corners) {
            out << M(qPrintable(QString("surface-%1").arg(c.key)),"Tyres",qPrintable(QString("Surface Temp %1").arg(c.label)),AnalyzeSource::Tyre,qPrintable(QString("surface_%1").arg(c.key)),c.color,"tyre-temp",0,125,"°C",1)
                << M(qPrintable(QString("inner-%1").arg(c.key)),"Tyres",qPrintable(QString("Inner Temp %1").arg(c.label)),AnalyzeSource::Tyre,qPrintable(QString("inner_%1").arg(c.key)),c.color,"tyre-temp",0,125,"°C",1)
                << M(qPrintable(QString("brake-temp-%1").arg(c.key)),"Tyres",qPrintable(QString("Brake Temp %1").arg(c.label)),AnalyzeSource::Tyre,qPrintable(QString("brake_%1").arg(c.key)),c.color,"brake-temp",0,1250,"°C",1)
                << M(qPrintable(QString("wear-%1").arg(c.key)),"Tyres",qPrintable(QString("Tyre Wear %1").arg(c.label)),AnalyzeSource::Damage,qPrintable(QString("wear_%1").arg(c.key)),c.color,"percent",0,100,"%",1)
                << M(qPrintable(QString("life-%1").arg(c.key)),"Tyres",qPrintable(QString("Tyre Life %1").arg(c.label)),AnalyzeSource::Damage,qPrintable(QString("life_%1").arg(c.key)),c.color,"percent",0,100,"%",1);
        }
        // Whole-car baselines: the mean of whichever corners report.
        out << M("wear-avg","Tyres","Average Tyre Wear",AnalyzeSource::Damage,"wear_avg","#FF780A","percent",0,100,"%",1)
            << M("life-avg","Tyres","Average Tyre Life",AnalyzeSource::Damage,"life_avg","#73BF69","percent",0,100,"%",1);
        return out;
    }();
    return v;
}

const AnalyzeMetric* analyzeMetric(const QString& id) {
    for (const auto& m : analyzeMetrics()) if (m.id == id) return &m;
    return nullptr;
}

const QVector<AnalyzeScale>& analyzeScales() {
    // Qt stores throttle, brake and steering as percentages (Electron 0..1).
    static const QVector<AnalyzeScale> scales{
        {"speed", "Speed:", "Always 0–380 km/h", 0, 380},
        {"rpm", "RPM:", "Always 0–16,000 rpm", 0, 16000},
        {"gear", "Gear:", "Always gears 1–8", 0.5, 8.5},
        {"input-positive", "Throttle / brake:", "Always 0–100%", 0, 100},
        {"input-signed", "Steering:", "Always −100–100%", -100, 100},
        {"percent", "ERS / tyre wear / life:", "Always 0–100%", 0, 100},
        {"g-force", "G-force:", "Always −6–6 g", -6, 6},
        {"ride-height", "Ride height:", "0–50 mm; expands when needed", 0, 50, true, 2, 5, true},
        {"power", "ICE / MGU-K power:", "0–500 kW; expands above 500 kW when needed", 0, 500, true},
        {"harvest", "MGU-K / MGU-H harvest:", "0–4000/8000 kJ by Formula; expands above when needed", 0, 4000, true},
        {"fuel", "Fuel:", "Always 0 kg to the session's fuel load + 1 kg", 0, 110},
        {"tyre-temp", "Surface / inner temp:", "0–125°C; expands above 125°C when needed", 0, 125, true},
        {"brake-temp", "Brake temp:", "0–1250°C; expands above 1250°C when needed", 0, 1250, true},
    };
    return scales;
}

const AnalyzeScale* analyzeScale(const QString& key) {
    for (const AnalyzeScale& scale : analyzeScales()) if (scale.key == key) return &scale;
    return nullptr;
}

QString analyzeFormatValue(const AnalyzeMetric& metric, double value) {
    if (!std::isfinite(value)) return QString::fromUtf8("—");
    // Qt stores throttle/brake/steering as percentages; Electron as 0..1.
    if (metric.id == "rpm") {
        QLocale locale;
        return locale.toString(qint64(std::llround(value))) + " rpm";
    }
    if (metric.id == "gear") return QString("Gear %1").arg(std::llround(value));
    if (metric.id == "throttle" || metric.id == "brake") return QString("%1%").arg(std::llround(value));
    if (metric.id == "steering")
        return QString("%1%2%").arg(value < 0 ? "L " : value > 0 ? "R " : "").arg(std::llround(std::abs(value)));
    const QString number = QString::number(value, 'f', metric.precision);
    if (metric.id == "speed") return number + " km/h";
    return metric.unit == "%" ? number + "%" : number + " " + metric.unit;
}

const QStringList& analyzeMetricGroups() {
    static const QStringList groups{QStringLiteral("Driving"), QStringLiteral("Motion"),
                                    QStringLiteral("Power"), QStringLiteral("Tyres")};
    return groups;
}

const QVector<AnalyzeTyreCorner>& analyzeTyreCorners() {
    static const QVector<AnalyzeTyreCorner> corners{
        {QStringLiteral("fl"), QStringLiteral("FL")}, {QStringLiteral("fr"), QStringLiteral("FR")},
        {QStringLiteral("rl"), QStringLiteral("RL")}, {QStringLiteral("rr"), QStringLiteral("RR")}};
    return corners;
}

const QVector<AnalyzeTyreRow>& analyzeTyreRows() {
    static const QVector<AnalyzeTyreRow> rows{
        {QStringLiteral("surface"), QStringLiteral("Surface Temp"), QStringLiteral("Surface"), QColor("#FF9830")},
        {QStringLiteral("inner"), QStringLiteral("Inner Temp"), QStringLiteral("Inner"), QColor("#B877DB")},
        {QStringLiteral("brake-temp"), QStringLiteral("Brake Temp"), QStringLiteral("Brake"), QColor("#F2495C")},
        {QStringLiteral("wear"), QStringLiteral("Tyre Wear"), QStringLiteral("T.Wear"), QColor("#8AB8FF")},
        {QStringLiteral("life"), QStringLiteral("Tyre Life"), QStringLiteral("T.Life"), QColor("#73BF69")},
    };
    return rows;
}

const AnalyzeTyreRow* analyzeCombinedRow(const QString& seriesId) {
    for (const AnalyzeTyreRow& row : analyzeTyreRows())
        if (row.combinedId() == seriesId) return &row;
    return nullptr;
}

const AnalyzeTyreRow* analyzeCornerRow(const QString& metricId, QString* cornerKey) {
    for (const AnalyzeTyreRow& row : analyzeTyreRows()) {
        for (const AnalyzeTyreCorner& corner : analyzeTyreCorners()) {
            if (metricId != row.idPrefix + QLatin1Char('-') + corner.key) continue;
            if (cornerKey) *cornerKey = corner.key;
            return &row;
        }
    }
    return nullptr;
}

const AnalyzeMetric* analyzeScaleMetric(const QString& seriesId) {
    if (const AnalyzeTyreRow* row = analyzeCombinedRow(seriesId))
        return analyzeMetric(row->idPrefix + QLatin1Char('-') + analyzeTyreCorners().first().key);
    return analyzeMetric(seriesId);
}

QStringList analyzeSeriesConflicts(const QString& seriesId) {
    QStringList conflicts;
    if (const AnalyzeTyreRow* row = analyzeCombinedRow(seriesId)) {
        for (const AnalyzeTyreCorner& corner : analyzeTyreCorners())
            conflicts << row->idPrefix + QLatin1Char('-') + corner.key;
    } else if (const AnalyzeTyreRow* row = analyzeCornerRow(seriesId)) {
        conflicts << row->combinedId();
    }
    return conflicts;
}

QStringList analyzeSanitizeCorners(const QStringList& corners) {
    QStringList out;
    for (const AnalyzeTyreCorner& corner : analyzeTyreCorners())
        if (corners.contains(corner.key)) out << corner.key;
    return out;
}

bool analyzeSeriesHasLines(const AnalyzeSeriesSetting& setting) {
    return !analyzeCombinedRow(setting.metricId) || !setting.corners.isEmpty();
}

QColor analyzeSeriesLineColor(const AnalyzeSeriesSetting& setting, const QString& memberId) {
    QString key;
    const AnalyzeTyreRow* row = analyzeCornerRow(memberId, &key);
    if (!row || row->combinedId() != setting.metricId) return setting.color;
    const QColor custom = setting.cornerColors.value(key);
    if (custom.isValid()) return custom;
    const AnalyzeMetric* member = analyzeMetric(memberId);
    return member ? member->defaultColor : setting.color;
}
