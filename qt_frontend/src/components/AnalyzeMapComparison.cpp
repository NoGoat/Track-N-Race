#include "AnalyzeMapComparison.h"

#include "TrackMapWidget.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {
QString clockText(double seconds){const int ms=qMax(0,qRound(seconds*1000));return QString("%1:%2.%3").arg(ms/60000).arg((ms/1000)%60,2,10,QChar('0')).arg((ms%1000)/100);}
}

AnalyzeMapComparison::AnalyzeMapComparison(QWidget* parent):QWidget(parent){
    auto*layout=new QVBoxLayout(this);layout->setContentsMargins(0,0,0,0);layout->setSpacing(0);
    map_=new TrackMapWidget;map_->setControlledMode(true);QSettings settings("TrackNRace","NativeRecorder");map_->setSectorColors(settings.value("ui/trackMapSectorColors",true).toBool());map_->setMapOpacity(settings.value("ui/trackMapOpacity",100).toInt()/100.0);layout->addWidget(map_,1);
    transport_=new QWidget;auto*bar=new QHBoxLayout(transport_);bar->setContentsMargins(6,3,6,3);bar->setSpacing(4);
    back_=new QPushButton;back_->setIcon(style()->standardIcon(QStyle::SP_MediaSeekBackward));back_->setToolTip("Seek backward 5 seconds");play_=new QPushButton;play_->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));play_->setToolTip("Play comparison");forward_=new QPushButton;forward_->setIcon(style()->standardIcon(QStyle::SP_MediaSeekForward));forward_->setToolTip("Seek forward 5 seconds");for(auto*b:{back_,play_,forward_})b->setFixedSize(28,28);
    slider_=new QSlider(Qt::Horizontal);slider_->setRange(0,1000);time_=new QLabel("0:00.0 / 0:00.0");speed_=new QComboBox;for(double value:{0.25,0.5,1.0,2.0,4.0})speed_->addItem(QString::number(value)+QStringLiteral("×"),value);speed_->setCurrentIndex(2);
    bar->addWidget(back_);bar->addWidget(play_);bar->addWidget(forward_);bar->addWidget(slider_,1);bar->addWidget(time_);bar->addWidget(speed_);layout->addWidget(transport_);transport_->hide();
    timer_=new QTimer(this);timer_->setInterval(100);connect(timer_,&QTimer::timeout,this,[this]{if(playing_&&localTime()>=total_){cursor_=total_;playing_=false;clock_.restart();}refreshMarkers();refreshTransport();});
    connect(play_,&QPushButton::clicked,this,[this]{cursor_=localTime();if(!playing_&&cursor_>=total_)cursor_=0;playing_=!playing_&&total_>0;clock_.restart();refreshTransport();});connect(back_,&QPushButton::clicked,this,[this]{setLocalTime(localTime()-5);});connect(forward_,&QPushButton::clicked,this,[this]{setLocalTime(localTime()+5);});connect(slider_,&QSlider::sliderMoved,this,[this](int value){setLocalTime(total_*value/1000.0);});connect(speed_,qOverload<int>(&QComboBox::currentIndexChanged),this,[this](int){cursor_=localTime();speedValue_=speed_->currentData().toDouble();clock_.restart();});clock_.start();
}

void AnalyzeMapComparison::setLaps(const LapBlock* current,const LapBlock* comparison,bool fixedMode,int trackId,bool compatible){const QString next=QString("%1:%2:%3:%4:%5:%6").arg(current?current->lapNum:-1).arg(current?current->positions.size():0).arg(comparison?comparison->lapNum:-1).arg(comparison?comparison->positions.size():0).arg(fixedMode).arg(trackId);if(next!=signature_){signature_=next;current_=current?*current:LapBlock{};comparison_=comparison?*comparison:LapBlock{};hasCurrent_=current;hasComparison_=comparison;total_=qMax(hasCurrent_?double(current_.endSessionTime-current_.startSessionTime):0.0,hasComparison_?double(comparison_.endSessionTime-comparison_.startSessionTime):0.0);cursor_=0;playing_=false;focused_=false;clock_.restart();}fixed_=fixedMode;transport_->setVisible(fixed_);if(fixed_)timer_->start();else timer_->stop();map_->setTrack(compatible?trackId:-1);refreshMarkers();refreshTransport();}
void AnalyzeMapComparison::setColors(const QColor&current,const QColor&comparison){currentColor_=current;comparisonColor_=comparison;refreshMarkers();}
void AnalyzeMapComparison::setMapAppearance(bool sectorColors,int opacityPercent){map_->setSectorColors(sectorColors);map_->setMapOpacity(qBound(0,opacityPercent,100)/100.0);}
void AnalyzeMapComparison::setCurrentTime(float sessionTime){globalTime_=sessionTime;if(!fixed_&&!focused_)refreshMarkers();}
void AnalyzeMapComparison::focusElapsed(double seconds){focused_=true;cursor_=qBound(0.0,seconds,total_);playing_=false;clock_.restart();refreshMarkers();refreshTransport();}
void AnalyzeMapComparison::setLocalTime(double seconds){cursor_=qBound(0.0,seconds,total_);clock_.restart();refreshMarkers();refreshTransport();}
double AnalyzeMapComparison::localTime()const{return playing_?qMin(total_,cursor_+clock_.elapsed()/1000.0*speedValue_):cursor_;}
bool AnalyzeMapComparison::markerAt(const QVector<LapPositionSample>&points,float target,double&x,double&z){if(points.isEmpty())return false;auto it=std::lower_bound(points.cbegin(),points.cend(),target,[](const auto&p,float t){return p.t<t;});if(it==points.cbegin()){x=it->x;z=it->z;return true;}if(it==points.cend()){x=points.last().x;z=points.last().z;return true;}const auto&a=*(it-1);const auto&b=*it;const double span=b.t-a.t,ratio=span>0?(target-a.t)/span:0;x=a.x+(b.x-a.x)*ratio;z=a.z+(b.z-a.z)*ratio;return true;}
void AnalyzeMapComparison::refreshMarkers(){const double elapsed=fixed_||focused_?localTime():(hasCurrent_?globalTime_-current_.startSessionTime:0);QVector<TrackMapWidget::Marker>markers;double x=0,z=0;if(hasCurrent_&&markerAt(current_.positions,current_.startSessionTime+qBound(0.0,elapsed,double(current_.endSessionTime-current_.startSessionTime)),x,z))markers.push_back({x,z,fixed_?"Lap A":"Current",currentColor_});if(hasComparison_&&markerAt(comparison_.positions,comparison_.startSessionTime+qBound(0.0,elapsed,double(comparison_.endSessionTime-comparison_.startSessionTime)),x,z))markers.push_back({x,z,fixed_?"Lap B":"Compare",comparisonColor_});map_->setControlledMarkers(markers);}
void AnalyzeMapComparison::refreshTransport(){const double now=localTime();slider_->blockSignals(true);slider_->setValue(total_>0?qRound(now/total_*1000):0);slider_->blockSignals(false);time_->setText(clockText(now)+" / "+clockText(total_));play_->setIcon(style()->standardIcon(playing_?QStyle::SP_MediaPause:QStyle::SP_MediaPlay));}
