#include "AnalyzePage.h"
#include "AnalyzeChart.h"
#include "../SessionModel.h"
#include "../Labels.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QCompleter>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>

namespace {
QPushButton* tinyButton(const QString& text,const QString& tip,QWidget* parent){auto*b=new QPushButton(text,parent);b->setToolTip(tip);b->setFixedSize(24,24);b->setFlat(true);return b;}
QColor compoundColor(int c){switch(c){case 16:return QColor("#e10600");case 17:return QColor("#ffd700");case 18:return QColor("#f2f2f2");case 7:return QColor("#39b54a");case 8:return QColor("#4488ff");default:return QColor();}}
}

AnalyzePage::AnalyzePage(SessionModel* model,QWidget* parent):QWidget(parent),model_(model){
    loadSettings();
    auto* root=new QHBoxLayout(this);root->setContentsMargins(0,0,0,0);root->setSpacing(0);
    sidebar_=new QFrame;sidebar_->setFrameShape(QFrame::NoFrame);sidebar_->setFixedWidth(collapsed_?0:288);
    auto* side=new QVBoxLayout(sidebar_);side->setContentsMargins(8,8,8,8);side->setSpacing(8);
    auto* title=new QLabel("ANALYZE\nOverlay configuration");QFont tf=title->font();tf.setBold(true);tf.setPointSize(8);title->setFont(tf);side->addWidget(title);
    addMetric_=new QComboBox;addMetric_->setEditable(true);addMetric_->setInsertPolicy(QComboBox::NoInsert);addMetric_->lineEdit()->setPlaceholderText("Choose a value…");side->addWidget(addMetric_);rebuildMetricPicker();
    fixedMode_=new QCheckBox("Disable playback mode");fixedMode_->setEnabled(false);side->addWidget(fixedMode_);
    auto lapRow=[&](const QString& label,QComboBox*& box){auto*w=new QWidget;auto*l=new QHBoxLayout(w);l->setContentsMargins(0,0,0,0);auto*lab=new QLabel(label);lab->setFixedWidth(82);box=new QComboBox;l->addWidget(lab);l->addWidget(box,1);side->addWidget(w);};
    lapRow("Compare Lap",compareLap_);lapRow("Lap A",lapA_);lapRow("Lap B",lapB_);
    showYAxis_=new QCheckBox("Y-axis values");showYAxis_->setChecked(settings_.value("analyze/showYAxis",true).toBool());side->addWidget(showYAxis_);
    seriesList_=new QListWidget;seriesList_->setFrameShape(QFrame::NoFrame);seriesList_->setDragDropMode(QAbstractItemView::InternalMove);seriesList_->setDefaultDropAction(Qt::MoveAction);side->addWidget(seriesList_,1);
    root->addWidget(sidebar_);
    auto* right=new QWidget;auto* rv=new QVBoxLayout(right);rv->setContentsMargins(0,0,0,0);rv->setSpacing(0);
    auto* bar=new QWidget;bar->setFixedHeight(42);auto* bh=new QHBoxLayout(bar);bh->setContentsMargins(6,4,6,4);
    collapse_=tinyButton(collapsed_?">":"<",collapsed_?"Open Analyze controls":"Collapse Analyze controls",bar);bh->addWidget(collapse_);bh->addSpacing(8);bh->addWidget(new QLabel("ZOOM"));
    zoomOut_=tinyButton("−","Zoom out",bar);zoomIn_=tinyButton("+","Zoom in",bar);panLeft_=tinyButton("←","Pan left",bar);panRight_=tinyButton("→","Pan right",bar);resetZoom_=tinyButton("↺","Reset zoom",bar);
    for(auto*b:{zoomOut_,zoomIn_,panLeft_,panRight_,resetZoom_})bh->addWidget(b);bh->addStretch();rv->addWidget(bar);
    chart_=new AnalyzeChart;chart_->setModel(model_);rv->addWidget(chart_,1);root->addWidget(right,1);
    rebuildSeriesList();refreshLapSelectors();applyState();

    connect(addMetric_,qOverload<int>(&QComboBox::activated),this,[this](int i){QString id=addMetric_->itemData(i).toString();const auto*m=analyzeMetric(id);if(!m)return;series_.push_back({id,m->defaultColor,true});saveSettings();rebuildSeriesList();rebuildMetricPicker();applyState();});
    connect(showYAxis_,&QCheckBox::toggled,this,[this](bool){saveSettings();applyState();});
    connect(fixedMode_,&QCheckBox::toggled,this,[this](bool){applyState();});
    for(auto* b:{compareLap_,lapA_,lapB_})connect(b,qOverload<int>(&QComboBox::currentIndexChanged),this,[this](int){applyState();});
    connect(collapse_,&QPushButton::clicked,this,[this]{collapsed_=!collapsed_;sidebar_->setFixedWidth(collapsed_?0:288);collapse_->setText(collapsed_?">":"<");saveSettings();});
    connect(zoomIn_,&QPushButton::clicked,chart_,&AnalyzeChart::zoomIn);connect(zoomOut_,&QPushButton::clicked,chart_,&AnalyzeChart::zoomOut);connect(panLeft_,&QPushButton::clicked,chart_,&AnalyzeChart::panLeft);connect(panRight_,&QPushButton::clicked,chart_,&AnalyzeChart::panRight);connect(resetZoom_,&QPushButton::clicked,chart_,&AnalyzeChart::resetZoom);
    connect(model_,&SessionModel::lapsChanged,this,&AnalyzePage::refreshLapSelectors);
    connect(seriesList_->model(),&QAbstractItemModel::rowsMoved,this,[this]{QVector<AnalyzeSeriesSetting> next;for(int i=0;i<seriesList_->count();++i){QString id=seriesList_->item(i)->data(Qt::UserRole).toString();auto it=std::find_if(series_.cbegin(),series_.cend(),[&](const auto&s){return s.metricId==id;});if(it!=series_.cend())next<<*it;}series_=next;saveSettings();applyState();QTimer::singleShot(0,this,&AnalyzePage::rebuildSeriesList);});
}

void AnalyzePage::loadSettings(){collapsed_=settings_.value("analyze/collapsed",false).toBool();QJsonDocument doc=QJsonDocument::fromJson(settings_.value("analyze/series").toByteArray());QSet<QString> seen;if(doc.isArray())for(const auto&v:doc.array()){auto o=v.toObject();QString id=o["metricId"].toString();const auto*m=analyzeMetric(id);QColor c(o["color"].toString());if(!m||seen.contains(id))continue;seen.insert(id);series_<<AnalyzeSeriesSetting{id,c.isValid()?c:m->defaultColor,o["visible"].toBool(true)};}if(series_.isEmpty()&&!doc.isArray())for(const char*id:{"speed","rpm","ers"}){const auto*m=analyzeMetric(id);series_<<AnalyzeSeriesSetting{m->id,m->defaultColor,true};}}
void AnalyzePage::saveSettings(){settings_.setValue("analyze/version",1);settings_.setValue("analyze/collapsed",collapsed_);settings_.setValue("analyze/showYAxis",showYAxis_?showYAxis_->isChecked():true);QJsonArray a;for(const auto&s:series_){QJsonObject o;o["metricId"]=s.metricId;o["color"]=s.color.name();o["visible"]=s.visible;a.append(o);}settings_.setValue("analyze/series",QJsonDocument(a).toJson(QJsonDocument::Compact));}

void AnalyzePage::rebuildMetricPicker(){if(!addMetric_)return;QSignalBlocker b(addMetric_);addMetric_->clear();QSet<QString> used;for(const auto&s:series_)used.insert(s.metricId);for(const auto&m:analyzeMetrics())if(!used.contains(m.id))addMetric_->addItem(QString("%1 · %2").arg(m.group,m.label),m.id);addMetric_->setCurrentIndex(-1);if(addMetric_->completer()){addMetric_->completer()->setCaseSensitivity(Qt::CaseInsensitive);addMetric_->completer()->setFilterMode(Qt::MatchContains);}}

void AnalyzePage::rebuildSeriesList(){seriesList_->clear();for(int i=0;i<series_.size();++i){const auto&m=*analyzeMetric(series_[i].metricId);auto*item=new QListWidgetItem;item->setData(Qt::UserRole,m.id);item->setSizeHint(QSize(260,38));seriesList_->addItem(item);auto*w=new QWidget;auto*l=new QHBoxLayout(w);l->setContentsMargins(2,1,2,1);l->setSpacing(3);auto*color=tinyButton("",m.label+" color",w);color->setStyleSheet("background:"+series_[i].color.name()+";border:1px solid palette(mid);border-radius:3px;");auto*label=new QLabel(QString("%1\n%2%3").arg(m.label,m.group,m.unit.isEmpty()?QString():" · "+m.unit));label->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Preferred);auto*up=tinyButton("↑","Move up",w);auto*down=tinyButton("↓","Move down",w);auto*eye=tinyButton(series_[i].visible?"●":"○",series_[i].visible?"Hide series":"Show series",w);auto*reset=tinyButton("↺","Reset color",w);auto*remove=tinyButton("×","Remove metric",w);l->addWidget(color);l->addWidget(label,1);for(auto*b:{up,down,eye,reset,remove})l->addWidget(b);seriesList_->setItemWidget(item,w);up->setEnabled(i>0);down->setEnabled(i+1<series_.size());
        connect(color,&QPushButton::clicked,this,[this,id=m.id]{auto it=std::find_if(series_.begin(),series_.end(),[&](auto&s){return s.metricId==id;});if(it==series_.end())return;QColor c=QColorDialog::getColor(it->color,this,"Select series colour",QColorDialog::DontUseNativeDialog);if(c.isValid()){it->color=c;saveSettings();rebuildSeriesList();applyState();}});
        connect(up,&QPushButton::clicked,this,[this,i]{moveSeries(i,i-1);});connect(down,&QPushButton::clicked,this,[this,i]{moveSeries(i,i+1);});connect(eye,&QPushButton::clicked,this,[this,id=m.id]{for(auto&s:series_)if(s.metricId==id)s.visible=!s.visible;saveSettings();rebuildSeriesList();applyState();});connect(reset,&QPushButton::clicked,this,[this,id=m.id]{for(auto&s:series_)if(s.metricId==id)s.color=analyzeMetric(id)->defaultColor;saveSettings();rebuildSeriesList();applyState();});connect(remove,&QPushButton::clicked,this,[this,id=m.id]{series_.erase(std::remove_if(series_.begin(),series_.end(),[&](const auto&s){return s.metricId==id;}),series_.end());saveSettings();rebuildSeriesList();rebuildMetricPicker();applyState();});}}
void AnalyzePage::moveSeries(int from,int to){if(from<0||to<0||from>=series_.size()||to>=series_.size())return;series_.move(from,to);saveSettings();rebuildSeriesList();applyState();}

void AnalyzePage::refreshLapSelectors(){auto fill=[&](QComboBox*box,bool none){QSignalBlocker guard(box);int old=box->currentData().toInt();box->clear();if(none)box->addItem("None",-1);for(const LapBlock&lap:model_->data().laps){QString compound;int visual=0;for(auto it=lap.sts.crbegin();it!=lap.sts.crend();++it)if(it->tyre_compound>0){compound=tnr::Ln("tyre.actual",it->tyre_compound);visual=it->visual_compound;break;}QString text=QString("%1%2Lap %3%4").arg(compound,compound.isEmpty()?"":" · ").arg(lap.lapNum).arg(lap.lapNum==model_->data().fastestLapNum?" · FL":"");box->addItem(text,lap.lapNum);if(QColor c=compoundColor(visual);c.isValid())box->setItemData(box->count()-1,c,Qt::ForegroundRole);}int idx=box->findData(old);box->setCurrentIndex(idx>=0?idx:0);};fill(compareLap_,true);fill(lapA_,false);fill(lapB_,false);applyState();}
void AnalyzePage::applyState(){const bool fixed=playback_&&fixedMode_->isChecked();fixedMode_->setEnabled(playback_&&!model_->data().laps.isEmpty());compareLap_->setVisible(!fixed);compareLap_->setEnabled(playback_);lapA_->parentWidget()->setVisible(fixed);lapB_->parentWidget()->setVisible(fixed);chart_->setConfig(series_,showYAxis_->isChecked());chart_->setComparisonLap(playback_&&!fixed?compareLap_->currentData().toInt():-1);chart_->setFixedLaps(fixed,lapA_->currentData().toInt(),lapB_->currentData().toInt());const bool nav=fixed&&lapA_->currentData().toInt()>0;for(auto*b:{zoomIn_,zoomOut_,panLeft_,panRight_,resetZoom_})b->setEnabled(nav);}
void AnalyzePage::setPlaybackMode(bool on,float t){playback_=on;chart_->setPlaybackMode(on);chart_->setCurrentTime(t);if(!on)resetPlaybackSelections();fixedMode_->setEnabled(on&&!model_->data().laps.isEmpty());applyState();}
void AnalyzePage::setCurrentTime(float t){chart_->setCurrentTime(t);}
void AnalyzePage::resetPlaybackSelections(){fixedMode_->setChecked(false);compareLap_->setCurrentIndex(0);if(lapA_->count())lapA_->setCurrentIndex(0);if(lapB_->count())lapB_->setCurrentIndex(0);applyState();}
