#include "AnalyzeMetrics.h"

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
            M("ride-front","Motion","Front Ride Height",AnalyzeSource::MotionEx,"front","#73BF69","ride-height",-2,20,"mm",1),
            M("ride-rear","Motion","Rear Ride Height",AnalyzeSource::MotionEx,"rear","#B877DB","ride-height",-2,20,"mm",1),
            M("power-ice","Power","ICE Power",AnalyzeSource::Status,"ice","#5794F2","power",0,1000,"kW",1),
            M("power-mguk","Power","MGU-K Power",AnalyzeSource::Status,"mguk","#FADE2A","power",0,1000,"kW",1),
            M("harvest-mguk","Power","MGU-K Harvest",AnalyzeSource::Status,"harvest_k","#37872D","harvest",0,2000,"kJ",1),
            M("harvest-mguh","Power","MGU-H Harvest",AnalyzeSource::Status,"harvest_h","#C4162A","harvest",0,2000,"kJ",1),
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
        return out;
    }();
    return v;
}

const AnalyzeMetric* analyzeMetric(const QString& id) {
    for (const auto& m : analyzeMetrics()) if (m.id == id) return &m;
    return nullptr;
}
