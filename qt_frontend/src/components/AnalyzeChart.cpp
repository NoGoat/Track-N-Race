#include "AnalyzeChart.h"
#include "../SessionModel.h"

#include <QPalette>
#include <QShowEvent>
#include <QTimer>
#include <algorithm>
#include <cmath>

namespace {
QColor muted(const QColor& c, const QColor& bg) {
    return QColor(qRound(c.red()*0.35+bg.red()*0.65), qRound(c.green()*0.35+bg.green()*0.65), qRound(c.blue()*0.35+bg.blue()*0.65));
}

template<class V, class F>
void collect(const V& values, float start, float end, QVector<double>& xs, QVector<double>& ys, F value) {
    auto first = std::lower_bound(values.cbegin(), values.cend(), start, [](const auto& s, float t){ return s.t < t; });
    for (auto it=first; it!=values.cend() && it->t<=end; ++it) {
        const double y=value(*it); if (!std::isfinite(y)) continue;
        if (!xs.isEmpty() && qFuzzyCompare(xs.last()+1.0, double(it->t-start)+1.0)) { ys.last()=y; continue; }
        xs.push_back(it->t-start); ys.push_back(y);
    }
}

double telValue(const TelSample& s,const QString& f) {
    if(f=="speed")return s.speed;if(f=="rpm")return s.rpm;if(f=="gear")return s.gear;
    if(f=="throttle")return s.throttle*100.0;if(f=="brake")return s.brake*100.0;return s.steering*100.0;
}
double statusValue(const StsSample& s,const QString& f) {
    if(f=="ers")return s.ers;if(f=="fuel")return s.fuel_kg;if(f=="ice")return s.ice_kw;
    if(f=="mguk")return s.mguk_kw;if(f=="harvest_k")return s.mguk_harvest_j/1000.0;return s.mguh_harvest_j/1000.0;
}
double motionValue(const MotionSample& s,const QString& f){return f=="g_lat"?s.g_lat:s.g_long;}
double motionExValue(const MotionExSample& s,const QString& f){return f=="front"?s.front_aero:s.rear_aero;}
int corner(const QString& f){if(f.endsWith("fl"))return 0;if(f.endsWith("fr"))return 1;if(f.endsWith("rl"))return 2;return 3;}
double tyreValue(const TyreSample& s,const QString& f){
    const int c=corner(f); const float* p=nullptr;
    if(f.startsWith("surface"))p=&s.surfFl;else if(f.startsWith("inner"))p=&s.innerFl;else p=&s.brakeFl;
    return p[c];
}
double damageValue(const DamageSample& s,const QString& f){const float* p=&s.wearFl;const double w=p[corner(f)];return f.startsWith("life")?100.0-w:w;}
float lapStart(const LapBlock& lap){if(!lap.tel.isEmpty())return lap.tel.first().t;if(!lap.sts.isEmpty())return lap.sts.first().t;return lap.startSessionTime;}
float lapEnd(const LapBlock& lap){if(!lap.tel.isEmpty())return lap.tel.last().t;if(!lap.sts.isEmpty())return lap.sts.last().t;return lap.endSessionTime;}
}

AnalyzeChart::AnalyzeChart(QWidget* parent):ChartView(parent) {
    xAxis_=addAxis({Side::Bottom,0,1,QColor(),true,'f',1,false}); setAxisTimeTicker(xAxis_,"%m:%s");
    const auto& metrics=analyzeMetrics();
    QStringList scales;
    for(const auto& m:metrics) if(!scales.contains(m.scaleKey)) scales<<m.scaleKey;
    for(int i=0;i<scales.size();++i){
        const auto mit=std::find_if(metrics.cbegin(),metrics.cend(),[&](const auto& x){return x.scaleKey==scales[i];});
        const auto& m=*mit;
        const Side side=(i%2)?Side::Right:Side::Left;
        int ax=addAxis({side,m.min,m.max,m.defaultColor,false,'f',m.precision,i==0});
        if(m.scaleKey=="rpm")setAxisNumberSuffix(ax,1000.0,"k");
        else if(m.unit=="%")setAxisNumberSuffix(ax,1.0,"%");
        else if(m.scaleKey!="speed"&&!m.unit.isEmpty())setAxisNumberSuffix(ax,1.0,m.unit=="°C"?"°":m.unit);
        axes_.insert(scales[i],ax);
    }
    handles_.resize(metrics.size());
    for(int i=0;i<metrics.size();++i){const auto&m=metrics[i];const int ax=axes_[m.scaleKey];
        handles_[i].comparison=addSeries({"COMPARE · "+m.label,muted(m.defaultColor,palette().color(QPalette::Window)),1.25,xAxis_,ax,m.unit,m.precision,m.id=="rpm",false,QColor(),m.step});
        handles_[i].current=addSeries({"CURRENT · "+m.label,m.defaultColor,1.75,xAxis_,ax,m.unit,m.precision,m.id=="rpm",false,QColor(),m.step});
        setSeriesVisible(handles_[i].comparison,false);setSeriesVisible(handles_[i].current,false);
    }
    setLegendVisible(false);setHoverReadout(true);
    refreshTimer_=new QTimer(this);refreshTimer_->setSingleShot(true);refreshTimer_->setInterval(0);
    connect(refreshTimer_,&QTimer::timeout,this,&AnalyzeChart::refresh);
}

void AnalyzeChart::setModel(SessionModel* m){if(model_)disconnect(model_,nullptr,this,nullptr);model_=m;if(m){connect(m,&SessionModel::telemetryAppended,this,&AnalyzeChart::requestRefresh);connect(m,&SessionModel::tyreAppended,this,&AnalyzeChart::requestRefresh);connect(m,&SessionModel::lapsChanged,this,&AnalyzeChart::requestRefresh);connect(m,&SessionModel::wasReset,this,&AnalyzeChart::requestRefresh);}requestRefresh();}
void AnalyzeChart::setConfig(const QVector<AnalyzeSeriesSetting>& s,bool y){selected_=s;showYAxis_=y;requestRefresh();}
void AnalyzeChart::setPlaybackMode(bool on){playback_=on;requestRefresh();}
void AnalyzeChart::setCurrentTime(float t){currentTime_=t;requestRefresh();}
void AnalyzeChart::setComparisonLap(int n){compareLap_=n;requestRefresh();}
void AnalyzeChart::setFixedLaps(bool e,int a,int b){fixed_=e;lapA_=a;lapB_=b;requestRefresh();}
void AnalyzeChart::showEvent(QShowEvent* e){ChartView::showEvent(e);requestRefresh();}
void AnalyzeChart::requestRefresh(){dirty_=true;if(isVisible()&&!refreshTimer_->isActive())refreshTimer_->start();}

void AnalyzeChart::refresh(){
    if(!model_||!dirty_||!isVisible())return;dirty_=false;const SessionData&d=model_->data();
    const LapBlock* primary=nullptr;const LapBlock* compare=nullptr;float primaryEnd=0;
    if(fixed_){primary=d.lapByNum(lapA_);compare=d.lapByNum(lapB_);if(primary)primaryEnd=lapEnd(*primary);}
    else {const float now=playback_?currentTime_:d.latestTime;primary=playback_?d.lapAtTime(now):(d.curLapNum>=0?&d.curLap:d.lapAtTime(now));primaryEnd=now;compare=d.lapByNum(compareLap_);}
    const auto& defs=analyzeMetrics();QSet<QString> visibleScales;double fullMax=1;
    for(int i=0;i<defs.size();++i){const auto&m=defs[i];auto it=std::find_if(selected_.cbegin(),selected_.cend(),[&](const auto&s){return s.metricId==m.id;});const bool vis=it!=selected_.cend()&&it->visible;
        setSeriesName(handles_[i].current,(fixed_?QString("LAP A · L%1").arg(primary?primary->lapNum:0):QString("CURRENT · L%1").arg(primary?primary->lapNum:0))+" · "+m.label);
        setSeriesName(handles_[i].comparison,(fixed_?QString("LAP B · L%1").arg(compare?compare->lapNum:0):QString("COMPARE · L%1").arg(compare?compare->lapNum:0))+" · "+m.label);
        setSeriesVisible(handles_[i].current,vis&&primary);setSeriesVisible(handles_[i].comparison,vis&&compare);
        const QColor color=it!=selected_.cend()?it->color:m.defaultColor;setSeriesColor(handles_[i].current,color);setSeriesColor(handles_[i].comparison,muted(color,palette().color(QPalette::Window)));
        if(vis)visibleScales.insert(m.scaleKey);
        auto fill=[&](const LapBlock* lap,float end,int sid){QVector<double>xs,ys;if(!lap){clear(sid);return;}const float a=lapStart(*lap),last=lapEnd(*lap),b=qMin(end,last);
            if(b<a){clear(sid);return;}
            switch(m.source){case AnalyzeSource::Telemetry:collect(lap->tel,a,b,xs,ys,[&](const auto&s){return telValue(s,m.field);});break;case AnalyzeSource::Status:collect(lap->sts,a,b,xs,ys,[&](const auto&s){return statusValue(s,m.field);});break;case AnalyzeSource::Motion:collect(d.motionBuf,a,b,xs,ys,[&](const auto&s){return motionValue(s,m.field);});break;case AnalyzeSource::MotionEx:collect(d.motionExBuf,a,b,xs,ys,[&](const auto&s){return motionExValue(s,m.field);});break;case AnalyzeSource::Tyre:collect(d.tyreBuf,a,b,xs,ys,[&](const auto&s){return tyreValue(s,m.field);});break;case AnalyzeSource::Damage:collect(d.damageBuf,a,b,xs,ys,[&](const auto&s){return damageValue(s,m.field);});break;}setSeriesData(sid,xs,ys);if(!xs.isEmpty())fullMax=qMax(fullMax,xs.last());};
        fill(primary,primaryEnd,handles_[i].current);fill(compare,compare?lapEnd(*compare):0,handles_[i].comparison);
    }
    QVector<int> order;for(auto it=selected_.crbegin();it!=selected_.crend();++it)if(it->visible){if(const auto*m=analyzeMetric(it->metricId)){const int idx=int(m-analyzeMetrics().constData());order<<handles_[idx].comparison;}}for(auto it=selected_.crbegin();it!=selected_.crend();++it)if(it->visible){if(const auto*m=analyzeMetric(it->metricId)){const int idx=int(m-analyzeMetrics().constData());order<<handles_[idx].current;}}setSeriesOrder(order);
    QString firstScale;
    for(const auto&s:selected_)if(s.visible){if(const auto*m=analyzeMetric(s.metricId)){firstScale=m->scaleKey;break;}}
    for(auto it=axes_.cbegin();it!=axes_.cend();++it){setAxisVisible(it.value(),showYAxis_&&visibleScales.contains(it.key()));setAxisGridVisible(it.value(),showYAxis_&&it.key()==firstScale);}
    QSet<QString> styledScales;for(const auto&s:selected_)if(s.visible){if(const auto*m=analyzeMetric(s.metricId))if(axes_.contains(m->scaleKey)&&!styledScales.contains(m->scaleKey)){setAxisColor(axes_[m->scaleKey],s.color);styledScales.insert(m->scaleKey);}}
    const bool fixedNavigation=fixed_&&primary;
    setXNavigation(xAxis_,fixedNavigation,0,fullMax,0.5);
    if(fixedNavigation){
        const QString key=QString("%1:%2:%3:%4").arg(lapA_).arg(lapB_).arg(lapStart(*primary),0,'f',3).arg(lapEnd(*primary),0,'f',3);
        if(key!=fixedDomainKey_){fixedDomainKey_=key;resetX();}
    }else{fixedDomainKey_.clear();setXRange(xAxis_,0,fullMax);}
    requestReplot();
}
