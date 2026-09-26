#include "AnalyzeChart.h"
#include "../SessionModel.h"
#include "../PresentationScheduler.h"

#include <QPalette>
#include <QShowEvent>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
QColor muted(const QColor& c, const QColor& bg) {
    return QColor(qRound(c.red()*0.35+bg.red()*0.65), qRound(c.green()*0.35+bg.green()*0.65), qRound(c.blue()*0.35+bg.blue()*0.65));
}

template<class V, class F, class X>
void collect(const V& values, float start, float end, QVector<double>& xs, QVector<double>& ys, F value, X coordinate) {
    auto first = std::lower_bound(values.cbegin(), values.cend(), start, [](const auto& s, float t){ return s.t < t; });
    for (auto it=first; it!=values.cend() && it->t<=end; ++it) {
        const double x=coordinate(*it),y=value(*it); if (!std::isfinite(x)) continue;
        if (!xs.isEmpty() && qFuzzyCompare(xs.last()+1.0,x+1.0)) { ys.last()=y; continue; }
        xs.push_back(x); ys.push_back(y);
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
double elapsedAtDistance(const LapBlock*lap,double distance){if(!lap||lap->progress.isEmpty())return qQNaN();const auto&p=lap->progress;auto it=std::lower_bound(p.cbegin(),p.cend(),distance,[](const auto&s,double d){return s.distanceM<d;});if(it==p.cbegin())return distance>=it->distanceM?it->currentLapMs/1000.0:qQNaN();if(it==p.cend())return distance<=p.last().distanceM?p.last().currentLapMs/1000.0:qQNaN();const auto&a=*(it-1);const auto&b=*it;const double span=b.distanceM-a.distanceM,r=span>0?(distance-a.distanceM)/span:1;return (a.currentLapMs+(b.currentLapMs-a.currentLapMs)*r)/1000.0;}
QVector<LapProgressSample> resolvedSectorSplits(const SessionData*primaryData,const LapBlock*primary,const SessionData*comparisonData,const LapBlock*comparison){
    QVector<LapProgressSample> result(2);QVector<bool>found(2,false);
    auto merge=[&](const SessionData*owner,const LapBlock*lap){if(!owner||!lap)return;for(const auto&split:owner->sectorSplits(lap)){const int index=split.sector-1;if(index>=0&&index<2){result[index]=split;found[index]=true;}}};
    merge(comparisonData,comparison);merge(primaryData,primary);QVector<LapProgressSample> compact;for(int i=0;i<2;++i)if(found[i])compact<<result[i];return compact;
}
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
    deltaAxis_=addAxis({Side::Right,-0.5,0.5,QColor("#C4162A"),false,'f',1,false});setAxisNumberSuffix(deltaAxis_,1.0,"s");
    deltaHandles_.comparison=addSeries({"DELTA −",QColor("#37872D"),1.75,xAxis_,deltaAxis_,"s",3});
    deltaHandles_.current=addSeries({"DELTA +",QColor("#C4162A"),1.75,xAxis_,deltaAxis_,"s",3});setSeriesVisible(deltaHandles_.comparison,false);setSeriesVisible(deltaHandles_.current,false);
    stacked_.resize(metrics.size());
    for(int i=0;i<metrics.size();++i){const auto&m=metrics[i];auto&h=stacked_[i];h.panel=addPanel();h.xAxis=addAxis({Side::Bottom,0,1,QColor(),true,'f',1,false},h.panel);setAxisTimeTicker(h.xAxis,"%m:%s");h.yAxis=addAxis({Side::Left,m.min,m.max,m.defaultColor,true,'f',m.precision,true},h.panel);if(m.scaleKey=="rpm")setAxisNumberSuffix(h.yAxis,1000.0,"k");else if(m.unit=="%")setAxisNumberSuffix(h.yAxis,1.0,"%");else if(!m.unit.isEmpty())setAxisNumberSuffix(h.yAxis,1.0,m.unit=="°C"?"°":m.unit);h.comparison=addSeries({"COMPARE · "+m.label,muted(m.defaultColor,palette().color(QPalette::Window)),1.25,h.xAxis,h.yAxis,m.unit,m.precision,m.id=="rpm",false,QColor(),m.step});h.current=addSeries({"CURRENT · "+m.label,m.defaultColor,1.75,h.xAxis,h.yAxis,m.unit,m.precision,m.id=="rpm",false,QColor(),m.step});setPanelTitle(h.panel,m.label);setPanelLegendVisible(h.panel,false);}
    stackedDelta_.panel=addPanel();stackedDelta_.xAxis=addAxis({Side::Bottom,0,1,QColor(),true,'f',1,false},stackedDelta_.panel);setAxisTimeTicker(stackedDelta_.xAxis,"%m:%s");stackedDelta_.yAxis=addAxis({Side::Left,-0.5,0.5,QColor("#C4162A"),true,'f',1,true},stackedDelta_.panel);setAxisNumberSuffix(stackedDelta_.yAxis,1.0,"s");stackedDelta_.comparison=addSeries({"DELTA −",QColor("#37872D"),1.75,stackedDelta_.xAxis,stackedDelta_.yAxis,"s",3});stackedDelta_.current=addSeries({"DELTA +",QColor("#C4162A"),1.75,stackedDelta_.xAxis,stackedDelta_.yAxis,"s",3});setPanelTitle(stackedDelta_.panel,"Delta");setPanelLegendVisible(stackedDelta_.panel,false);
    layoutPanelsRows({{0}});setCursorModeKey("analysis");
    setLegendVisible(false);setHoverReadout(true);
}

void AnalyzeChart::setModel(SessionModel* m){if(model_)disconnect(model_,nullptr,this,nullptr);model_=m;if(m){connect(m,&SessionModel::telemetryAppended,this,&AnalyzeChart::requestRefresh);connect(m,&SessionModel::tyreAppended,this,&AnalyzeChart::requestRefresh);connect(m,&SessionModel::lapsChanged,this,&AnalyzeChart::requestRefresh);connect(m,&SessionModel::wasReset,this,&AnalyzeChart::requestRefresh);}requestRefresh();}
void AnalyzeChart::setConfig(const QVector<AnalyzeSeriesSetting>& s,bool y){selected_=s;showYAxis_=y;requestRefresh();}
void AnalyzeChart::setPlaybackMode(bool on){playback_=on;requestRefresh();}
void AnalyzeChart::setCurrentTime(float t){currentTime_=t;requestRefresh();}
void AnalyzeChart::setDistanceMode(bool on){if(distanceMode_==on)return;distanceMode_=on;setAxisDistanceMode(xAxis_,on);refreshMapCursorGuides();requestRefresh();}
void AnalyzeChart::setIndividualGraphs(bool on,bool synced){individualGraphs_=on;syncedTooltip_=synced;setCursorSync(on&&synced,true,false);requestRefresh();}
void AnalyzeChart::setSectorOptions(bool boundaries,bool delta){sectorBoundaries_=boundaries;sectorDelta_=boundaries&&delta;requestRefresh();}
void AnalyzeChart::setLabels(const QString&current,const QString&comparison){currentLabel_=current;comparisonLabel_=comparison;requestRefresh();}
void AnalyzeChart::setMapCursors(bool visible,const QColor&currentColor,const QColor&comparisonColor){mapCursorsVisible_=visible;mapCurrentColor_=currentColor;mapComparisonColor_=comparisonColor;refreshMapCursorGuides();}
void AnalyzeChart::setMapCursorElapsed(double elapsedSeconds){if(!mapCursorsVisible_)return;mapCursorElapsed_=elapsedSeconds;refreshMapCursorGuides();}
void AnalyzeChart::setSelectedLaps(bool fixed,
                                   const SessionData* primaryData, const LapBlock* primary,
                                   const SessionData* comparisonData, const LapBlock* comparison) {
    fixed_ = fixed;
    selectedPrimaryData_ = primaryData;
    selectedPrimary_ = primary;
    selectedComparisonData_ = comparisonData;
    selectedComparison_ = comparison;
    refreshMapCursorGuides();
    requestRefresh();
}
void AnalyzeChart::refreshMapCursorGuides(){
    QVector<CursorGuide> guides;
    if(!mapCursorsVisible_||!fixed_){setCursorGuides(guides);return;}
    auto append=[&](const SessionData*owner,const LapBlock*lap,const QColor&color){
        if(!owner||!lap)return;
        const double duration=qMax(0.0,double(lap->endSessionTime-lap->startSessionTime));
        const double elapsed=qBound(0.0,mapCursorElapsed_,duration);
        double x=elapsed;
        if(distanceMode_)x=owner->distanceAtTime(lap,float(lap->startSessionTime+elapsed));
        if(std::isfinite(x))guides.push_back({x,color});
    };
    append(selectedPrimaryData_,selectedPrimary_,mapCurrentColor_);
    append(selectedComparisonData_,selectedComparison_,mapComparisonColor_);
    setCursorGuides(guides);
}
void AnalyzeChart::showEvent(QShowEvent* e){ChartView::showEvent(e);requestRefresh();}
void AnalyzeChart::requestRefresh(){dirty_=true;if(!isVisible())return;PresentationScheduler::instance().request(this,[this]{refresh();},PresentationScheduler::Policy::Chart);}

void AnalyzeChart::refresh(){
    if(!model_||!dirty_||!isVisible())return;dirty_=false;const SessionData&d=model_->data();
    const LapBlock*primary=nullptr,*compare=nullptr;const SessionData*primaryData=&d,*compareData=&d;float primaryEnd=0;
    if(fixed_){primary=selectedPrimary_;primaryData=selectedPrimaryData_;compare=selectedComparison_;compareData=selectedComparisonData_;if(primary)primaryEnd=lapEnd(*primary);}else{const float now=playback_?currentTime_:d.latestTime;primary=playback_?model_->chartPrimaryLap(now):(d.curLapNum>=0?&d.curLap:d.lapAtTime(now));primaryEnd=now;compare=selectedComparison_;if(selectedComparisonData_)compareData=selectedComparisonData_;}
    const auto&defs=analyzeMetrics();double fullMax=distanceMode_?qMax(1.0,double(d.trackLengthM)):1.0;if(distanceMode_){if(primary&&!primary->progress.isEmpty())fullMax=qMax(fullMax,double(primary->progress.last().distanceM));if(compare&&!compare->progress.isEmpty())fullMax=qMax(fullMax,double(compare->progress.last().distanceM));}else{if(primary)fullMax=qMax(fullMax,double(qMin(primaryEnd,lapEnd(*primary))-lapStart(*primary)));if(compare)fullMax=qMax(fullMax,double(lapEnd(*compare)-lapStart(*compare)));}
    QSet<QString>shownScales;QString firstScale;QVector<int>order,activePanels,activeXAxes;
    auto settingFor=[&](const QString&id)->const AnalyzeSeriesSetting*{auto it=std::find_if(selected_.cbegin(),selected_.cend(),[&](const auto&s){return s.metricId==id;});return it==selected_.cend()?nullptr:&*it;};
    for(int i=0;i<defs.size();++i){const auto&m=defs[i];const auto*s=settingFor(m.id);const bool vis=s&&s->visible;const QColor color=s?s->color:m.defaultColor;const QString currentName=QString("%1 · L%2 · %3").arg(currentLabel_).arg(primary?primary->lapNum:0).arg(m.label);const QString compareName=QString("%1 · L%2 · %3").arg(comparisonLabel_).arg(compare?compare->lapNum:0).arg(m.label);
        for(int id:{handles_[i].current,stacked_[i].current}){setSeriesName(id,currentName);setSeriesColor(id,color);}for(int id:{handles_[i].comparison,stacked_[i].comparison}){setSeriesName(id,compareName);setSeriesColor(id,muted(color,palette().color(QPalette::Window)));}
        QVector<double>px,py,cx,cy;auto gather=[&](const LapBlock*lap,const SessionData*owner,float end,QVector<double>&xs,QVector<double>&ys){if(!lap||!owner)return;const float a=lapStart(*lap),b=qMin(end,lapEnd(*lap));if(b<a)return;auto coordinate=[&](const auto&row){if(!distanceMode_)return double(row.t-a);if(lap->progress.isEmpty()||row.t<lap->progress.first().t||row.t>lap->progress.last().t)return qQNaN();return owner->distanceAtTime(lap,row.t);};switch(m.source){case AnalyzeSource::Telemetry:collect(lap->tel,a,b,xs,ys,[&](const auto&r){return telValue(r,m.field);},coordinate);break;case AnalyzeSource::Status:collect(lap->sts,a,b,xs,ys,[&](const auto&r){return statusValue(r,m.field);},coordinate);break;case AnalyzeSource::Motion:collect(lap->motion,a,b,xs,ys,[&](const auto&r){return motionValue(r,m.field);},coordinate);break;case AnalyzeSource::MotionEx:collect(lap->motionEx,a,b,xs,ys,[&](const auto&r){return motionExValue(r,m.field);},coordinate);break;case AnalyzeSource::Tyre:collect(lap->tyre,a,b,xs,ys,[&](const auto&r){return tyreValue(r,m.field);},coordinate);break;case AnalyzeSource::Damage:collect(lap->damage,a,b,xs,ys,[&](const auto&r){return damageValue(r,m.field);},coordinate);break;}};gather(primary,primaryData,primaryEnd,px,py);gather(compare,compareData,compare?lapEnd(*compare):0,cx,cy);for(int id:{handles_[i].current,stacked_[i].current})setSeriesData(id,px,py);for(int id:{handles_[i].comparison,stacked_[i].comparison})setSeriesData(id,cx,cy);if(!px.isEmpty())fullMax=qMax(fullMax,px.last());if(!cx.isEmpty())fullMax=qMax(fullMax,cx.last());
        setSeriesVisible(handles_[i].current,!individualGraphs_&&vis&&primary);setSeriesVisible(handles_[i].comparison,!individualGraphs_&&vis&&compare);setSeriesVisible(stacked_[i].current,individualGraphs_&&vis&&primary);setSeriesVisible(stacked_[i].comparison,individualGraphs_&&vis&&compare);setAxisVisible(stacked_[i].yAxis,individualGraphs_&&vis&&s->showYAxis);setAxisColor(stacked_[i].yAxis,color);if(vis&&individualGraphs_){activePanels<<stacked_[i].panel;activeXAxes<<stacked_[i].xAxis;}
    }
    QVector<double>dx,dp,dn;double deltaRange=.5;const auto*delta=settingFor("delta");const bool showDelta=delta&&delta->visible&&distanceMode_&&primary&&compare;
    if(showDelta){
        double currentMaxDistance=primary->progress.isEmpty()?0.0:double(primary->progress.last().distanceM);
        if(playback_&&!fixed_){
            const auto&progress=primary->progress;
            const double cursorDistance=!progress.isEmpty()&&currentTime_>=progress.first().t&&currentTime_<=progress.last().t?primaryData->distanceAtTime(primary,currentTime_):qQNaN();
            currentMaxDistance=std::isfinite(cursorDistance)?cursorDistance:0.0;
        }
        const double maxDistance=qMin(currentMaxDistance,compare->progress.isEmpty()?0.0:double(compare->progress.last().distanceM));
        QVector<double>sectorStarts;if(sectorBoundaries_&&sectorDelta_)for(const auto&p:resolvedSectorSplits(primaryData,primary,compareData,compare))sectorStarts<<p.distanceM;
        double previousX=0,previousValue=0;bool havePrevious=false;
        auto appendDelta=[&](double x,double value){
            if(!std::isfinite(value)){dx<<x;dp<<qQNaN();dn<<qQNaN();havePrevious=false;return;}
            if(havePrevious&&std::signbit(previousValue)!=std::signbit(value)&&previousValue!=0&&value!=0){const double zeroX=previousX+(x-previousX)*std::abs(previousValue)/(std::abs(previousValue)+std::abs(value));dx<<zeroX;dp<<0.0;dn<<0.0;}
            dx<<x;dp<<(value>=0?value:qQNaN());dn<<(value<=0?value:qQNaN());previousX=x;previousValue=value;havePrevious=true;deltaRange=qMax(deltaRange,std::ceil(std::abs(value)*10.0)/10.0);
        };
        double lastStoredX=-std::numeric_limits<double>::infinity();
        for(const auto&p:primary->progress){
            const double x=p.distanceM;if(x<0||x>maxDistance)continue;
            if(!sectorStarts.isEmpty())for(double boundary:sectorStarts)if(boundary>lastStoredX&&boundary<=x){double priorStart=0;for(double prior:sectorStarts){if(prior>=boundary)break;priorStart=prior;}const double ca=elapsedAtDistance(primary,boundary),cb=elapsedAtDistance(compare,boundary),pa=priorStart>0?elapsedAtDistance(primary,priorStart):0,pb=priorStart>0?elapsedAtDistance(compare,priorStart):0;appendDelta(boundary,(ca-pa)-(cb-pb));appendDelta(boundary,qQNaN());appendDelta(boundary,0);lastStoredX=boundary;}
            const double a=elapsedAtDistance(primary,x),b=elapsedAtDistance(compare,x);double ab=0,bb=0;if(!sectorStarts.isEmpty()){double start=0;for(double split:sectorStarts)if(split<=x)start=split;if(start>0){ab=elapsedAtDistance(primary,start);bb=elapsedAtDistance(compare,start);}}appendDelta(x,(a-ab)-(b-bb));lastStoredX=x;
        }
    }
    const QColor positive=delta&&delta->color.isValid()?delta->color:QColor("#C4162A"),negative=delta&&delta->negativeColor.isValid()?delta->negativeColor:QColor("#37872D");for(int id:{deltaHandles_.current,stackedDelta_.current}){setSeriesData(id,dx,dp);setSeriesColor(id,positive);}for(int id:{deltaHandles_.comparison,stackedDelta_.comparison}){setSeriesData(id,dx,dn);setSeriesColor(id,negative);}for(int axis:{deltaAxis_,stackedDelta_.yAxis})setAxisRange(axis,-deltaRange,deltaRange);setAxisColor(deltaAxis_,positive);setAxisColor(stackedDelta_.yAxis,positive);setSeriesVisible(deltaHandles_.current,!individualGraphs_&&showDelta);setSeriesVisible(deltaHandles_.comparison,!individualGraphs_&&showDelta);setSeriesVisible(stackedDelta_.current,individualGraphs_&&showDelta);setSeriesVisible(stackedDelta_.comparison,individualGraphs_&&showDelta);setAxisVisible(stackedDelta_.yAxis,individualGraphs_&&showDelta&&delta->showYAxis);if(individualGraphs_&&showDelta){activePanels<<stackedDelta_.panel;activeXAxes<<stackedDelta_.xAxis;}
    for(auto it=selected_.crbegin();it!=selected_.crend();++it)if(it->visible){if(it->metricId=="delta"&&showDelta){order<<deltaHandles_.comparison<<deltaHandles_.current;}else if(const auto*m=analyzeMetric(it->metricId)){const int i=int(m-defs.constData());order<<handles_[i].comparison<<handles_[i].current;}}setSeriesOrder(order);
    for(const auto&s:selected_)if(s.visible&&s.showYAxis){if(s.metricId=="delta"){setAxisVisible(deltaAxis_,!individualGraphs_&&showDelta);continue;}if(const auto*m=analyzeMetric(s.metricId))if(!shownScales.contains(m->scaleKey)){shownScales.insert(m->scaleKey);if(firstScale.isEmpty())firstScale=m->scaleKey;setAxisColor(axes_[m->scaleKey],s.color);}}for(auto it=axes_.cbegin();it!=axes_.cend();++it){setAxisVisible(it.value(),!individualGraphs_&&shownScales.contains(it.key()));setAxisGridVisible(it.value(),!individualGraphs_&&it.key()==firstScale);}if(!shownScales.contains("delta"))setAxisVisible(deltaAxis_,!individualGraphs_&&showDelta&&delta&&delta->showYAxis);
    QVector<double>ticks;QStringList labels;if(sectorBoundaries_&&primary){const QVector<LapProgressSample>splits=resolvedSectorSplits(primaryData,primary,compareData,compare);const double fullLapEnd=distanceMode_?qMax(double(primaryData->trackLengthM),compareData?double(compareData->trackLengthM):0.0):qMax(double(lapEnd(*primary)-lapStart(*primary)),compare?double(lapEnd(*compare)-lapStart(*compare)):0.0);ticks<<0;labels<<"";for(const auto&split:splits){const double value=distanceMode_?split.distanceM:split.currentLapMs/1000.0;if(value>0&&value<fullMax){ticks<<value;labels<<QString("S%1").arg(split.sector);}}ticks<<fullMax;labels<<(std::abs(fullLapEnd-fullMax)<1e-3?"S3":"");}
    QVector<int>allX{xAxis_};for(const auto&h:stacked_)allX<<h.xAxis;allX<<stackedDelta_.xAxis;for(int axis:allX){setAxisDistanceMode(axis,distanceMode_);if(sectorBoundaries_&&primary)setAxisLabelMap(axis,ticks,labels);else{setAxisTimeTicker(axis,"%m:%s");setAxisDistanceMode(axis,distanceMode_);}}
    activePanels.clear();activeXAxes.clear();if(individualGraphs_)for(const auto&s:selected_)if(s.visible){if(s.metricId=="delta"){if(showDelta){activePanels<<stackedDelta_.panel;activeXAxes<<stackedDelta_.xAxis;}}else if(const auto*m=analyzeMetric(s.metricId)){const int i=int(m-defs.constData());activePanels<<stacked_[i].panel;activeXAxes<<stacked_[i].xAxis;}}
    setAxisVisible(xAxis_,!individualGraphs_);for(const auto&h:stacked_)setAxisVisible(h.xAxis,false);setAxisVisible(stackedDelta_.xAxis,false);if(individualGraphs_&&!activeXAxes.isEmpty())setAxisVisible(activeXAxes.last(),true);
    QVector<QVector<int>>rows;if(individualGraphs_){for(int panel:activePanels)rows.push_back({panel});if(rows.isEmpty())rows.push_back({0});}else rows={{0}};QStringList panelKeys;for(int panel:activePanels)panelKeys<<QString::number(panel);const QString layoutKey=QString::number(individualGraphs_)+":"+panelKeys.join(',');if(layoutKey!=panelLayoutKey_){panelLayoutKey_=layoutKey;layoutPanelsRows(rows);}activeXAxes.prepend(xAxis_);setLinkedXAxes(individualGraphs_?activeXAxes:QVector<int>{xAxis_});
    const int navAxis=individualGraphs_&&!activeXAxes.isEmpty()&&activeXAxes.size()>1?activeXAxes[1]:xAxis_;const bool fixedNavigation=fixed_&&primary;setXNavigation(navAxis,fixedNavigation,0,fullMax,distanceMode_?25.0:0.5);if(fixedNavigation){const QString key=QString("%1:%2:%3:%4:%5:%6").arg(distanceMode_).arg(individualGraphs_).arg(primary?primary->lapNum:-1).arg(compare?compare->lapNum:-1).arg(lapStart(*primary),0,'f',3).arg(lapEnd(*primary),0,'f',3);if(key!=fixedDomainKey_){fixedDomainKey_=key;resetX();}}else{fixedDomainKey_.clear();setXRange(navAxis,0,fullMax);}requestReplot();
}
