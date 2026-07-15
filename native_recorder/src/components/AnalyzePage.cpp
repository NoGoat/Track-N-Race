#include "AnalyzePage.h"
#include "AnalyzeChart.h"
#include "PageUiHelpers.h"
#include "../SessionModel.h"
#include "../Labels.h"
#include "../IconUtils.h"

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
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>

namespace {
QIcon analyzeIcon(QWidget* w,const char* name,QStyle::StandardPixmap fallback){return adaptThemeIcon(QIcon::fromTheme(QString::fromLatin1(name)),w->palette().color(QPalette::WindowText),w->style()->standardIcon(fallback));}
QPushButton* tinyButton(const QString& tip,const QIcon& icon,QWidget* parent){auto*b=new QPushButton(parent);b->setIcon(icon);b->setToolTip(tip);b->setFixedSize(24,24);b->setFlat(true);return b;}
}

AnalyzePage::AnalyzePage(SessionModel* model,QWidget* parent):QWidget(parent),model_(model){
    loadSettings();
    auto* root=new QHBoxLayout(this);root->setContentsMargins(0,0,0,0);root->setSpacing(0);
    sidebar_=new QFrame;sidebar_->setFrameShape(QFrame::NoFrame);sidebar_->setFixedWidth(collapsed_?0:288);
    auto* side=new QVBoxLayout(sidebar_);side->setContentsMargins(8,8,8,8);side->setSpacing(8);
    auto* header=new QWidget;
    auto* headerLayout=new QHBoxLayout(header);headerLayout->setContentsMargins(0,0,0,0);headerLayout->setSpacing(4);
    auto* title=new QLabel("Overlay Configuration");
    collapse_=tinyButton("Collapse Analyze controls",analyzeIcon(header,"go-previous",QStyle::SP_ArrowLeft),header);
    headerLayout->addWidget(title);headerLayout->addStretch();headerLayout->addWidget(collapse_);
    side->addWidget(header);
    addMetric_=new QComboBox;addMetric_->setEditable(true);addMetric_->setInsertPolicy(QComboBox::NoInsert);addMetric_->lineEdit()->setPlaceholderText("Choose a value…");side->addWidget(addMetric_);rebuildMetricPicker();
    fixedMode_=new QCheckBox("Disable playback mode");fixedMode_->setEnabled(false);side->addWidget(fixedMode_);
    auto lapRow=[&](const QString& label,QComboBox*& box,bool clearable=false){auto*w=new QWidget;auto*l=new QHBoxLayout(w);l->setContentsMargins(0,0,0,0);l->setSpacing(4);auto*lab=new QLabel(label);lab->setFixedWidth(82);box=new QComboBox;l->addWidget(lab);l->addWidget(box,1);if(clearable){compareClear_=tinyButton("Clear comparison",analyzeIcon(w,"edit-clear",QStyle::SP_DialogCloseButton),w);l->addWidget(compareClear_);}side->addWidget(w);};
    lapRow("Compare Lap",compareLap_,true);lapRow("Lap A",lapA_);lapRow("Lap B",lapB_);
    showYAxis_=new QCheckBox("Y-axis values");showYAxis_->setChecked(settings_.value("analyze/showYAxis",true).toBool());side->addWidget(showYAxis_);
    seriesList_=new QListWidget;
    seriesList_->setFrameShape(QFrame::NoFrame);
    seriesList_->setFocusPolicy(Qt::NoFocus);
    seriesList_->setDragDropMode(QAbstractItemView::InternalMove);
    seriesList_->setDefaultDropAction(Qt::MoveAction);
    // QListWidget normally paints its viewport with the palette's darker Base
    // colour and gives the current row a menu-like selection highlight. These
    // rows are configuration cards, not choices: keep selection internally for
    // drag/drop, but never paint it, and let the sidebar show through.
    seriesList_->viewport()->setAutoFillBackground(false);
    seriesList_->setStyleSheet(
        "QListWidget { background: transparent; border: none; outline: none; }"
        "QListWidget::item { background: transparent; border: none; }"
        "QListWidget::item:hover, QListWidget::item:selected, "
        "QListWidget::item:selected:active, QListWidget::item:selected:!active {"
        " background: transparent; color: palette(text); }");
    side->addWidget(seriesList_,1);
    root->addWidget(sidebar_);
    // Use the same style-painted sunken divider as the rest of the native UI.
    // Hard-coding palette(mid) here produces an almost-black line in Breeze.
    root->addWidget(tnrui::vline());
    auto* right=new QWidget;auto* rv=new QVBoxLayout(right);rv->setContentsMargins(0,0,0,0);rv->setSpacing(0);
    collapsedBar_=new QWidget;collapsedBar_->setFixedHeight(collapsed_?34:0);auto* bh=new QHBoxLayout(collapsedBar_);bh->setContentsMargins(6,3,6,3);
    expand_=tinyButton("Open Analyze controls",analyzeIcon(collapsedBar_,"go-next",QStyle::SP_ArrowRight),collapsedBar_);expand_->setVisible(collapsed_);bh->addWidget(expand_);bh->addStretch();rv->addWidget(collapsedBar_);
    chart_=new AnalyzeChart;chart_->setModel(model_);rv->addWidget(chart_,1);root->addWidget(right,1);
    rebuildSeriesList();refreshLapSelectors();applyState();

    connect(addMetric_,qOverload<int>(&QComboBox::activated),this,[this](int i){QString id=addMetric_->itemData(i).toString();const auto*m=analyzeMetric(id);if(!m)return;series_.push_back({id,m->defaultColor,true});saveSettings();rebuildSeriesList();rebuildMetricPicker();applyState();});
    connect(showYAxis_,&QCheckBox::toggled,this,[this](bool){saveSettings();applyState();});
    connect(fixedMode_,&QCheckBox::toggled,this,[this](bool){applyState();});
    for(auto* b:{compareLap_,lapA_,lapB_})connect(b,qOverload<int>(&QComboBox::currentIndexChanged),this,[this](int){applyState();});
    auto toggleSidebar=[this]{collapsed_=!collapsed_;sidebar_->setFixedWidth(collapsed_?0:288);collapsedBar_->setFixedHeight(collapsed_?34:0);expand_->setVisible(collapsed_);saveSettings();};
    connect(collapse_,&QPushButton::clicked,this,toggleSidebar);
    connect(expand_,&QPushButton::clicked,this,toggleSidebar);
    connect(compareClear_,&QPushButton::clicked,this,[this]{compareLap_->setCurrentIndex(0);});
    connect(model_,&SessionModel::lapsChanged,this,&AnalyzePage::refreshLapSelectors);
    connect(seriesList_->model(),&QAbstractItemModel::rowsMoved,this,[this]{QVector<AnalyzeSeriesSetting> next;for(int i=0;i<seriesList_->count();++i){QString id=seriesList_->item(i)->data(Qt::UserRole).toString();auto it=std::find_if(series_.cbegin(),series_.cend(),[&](const auto&s){return s.metricId==id;});if(it!=series_.cend())next<<*it;}series_=next;saveSettings();applyState();QTimer::singleShot(0,this,&AnalyzePage::rebuildSeriesList);});
}

void AnalyzePage::loadSettings(){collapsed_=settings_.value("analyze/collapsed",false).toBool();QJsonDocument doc=QJsonDocument::fromJson(settings_.value("analyze/series").toByteArray());QSet<QString> seen;if(doc.isArray())for(const auto&v:doc.array()){auto o=v.toObject();QString id=o["metricId"].toString();const auto*m=analyzeMetric(id);QColor c(o["color"].toString());if(!m||seen.contains(id))continue;seen.insert(id);series_<<AnalyzeSeriesSetting{id,c.isValid()?c:m->defaultColor,o["visible"].toBool(true)};}if(series_.isEmpty()&&!doc.isArray())for(const char*id:{"speed","rpm","ers"}){const auto*m=analyzeMetric(id);series_<<AnalyzeSeriesSetting{m->id,m->defaultColor,true};}}
void AnalyzePage::saveSettings(){settings_.setValue("analyze/version",1);settings_.setValue("analyze/collapsed",collapsed_);settings_.setValue("analyze/showYAxis",showYAxis_?showYAxis_->isChecked():true);QJsonArray a;for(const auto&s:series_){QJsonObject o;o["metricId"]=s.metricId;o["color"]=s.color.name();o["visible"]=s.visible;a.append(o);}settings_.setValue("analyze/series",QJsonDocument(a).toJson(QJsonDocument::Compact));}

void AnalyzePage::rebuildMetricPicker(){if(!addMetric_)return;QSignalBlocker b(addMetric_);addMetric_->clear();QSet<QString> used;for(const auto&s:series_)used.insert(s.metricId);for(const auto&m:analyzeMetrics())if(!used.contains(m.id))addMetric_->addItem(QString("%1 · %2").arg(m.group,m.label),m.id);addMetric_->setCurrentIndex(-1);if(addMetric_->completer()){addMetric_->completer()->setCaseSensitivity(Qt::CaseInsensitive);addMetric_->completer()->setFilterMode(Qt::MatchContains);}}

void AnalyzePage::rebuildSeriesList(){seriesList_->clear();for(int i=0;i<series_.size();++i){const auto&m=*analyzeMetric(series_[i].metricId);auto*item=new QListWidgetItem;item->setData(Qt::UserRole,m.id);item->setSizeHint(QSize(260,38));seriesList_->addItem(item);auto*w=new QWidget;auto*l=new QHBoxLayout(w);l->setContentsMargins(2,1,2,1);l->setSpacing(3);auto*color=new QPushButton(w);color->setToolTip(m.label+" color");color->setFixedSize(24,24);color->setFlat(true);color->setStyleSheet("background:"+series_[i].color.name()+";border:1px solid palette(mid);border-radius:3px;");auto*label=new QLabel(QString("%1\n%2%3").arg(m.label,m.group,m.unit.isEmpty()?QString():" · "+m.unit));label->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Preferred);auto*up=tinyButton("Move up",analyzeIcon(w,"go-up",QStyle::SP_ArrowUp),w);auto*down=tinyButton("Move down",analyzeIcon(w,"go-down",QStyle::SP_ArrowDown),w);auto*eye=tinyButton(series_[i].visible?"Hide series":"Show series",analyzeIcon(w,series_[i].visible?"view-visible":"view-hidden",QStyle::SP_FileDialogInfoView),w);auto*reset=tinyButton("Reset color",analyzeIcon(w,"view-refresh",QStyle::SP_BrowserReload),w);auto*remove=tinyButton("Remove metric",analyzeIcon(w,"edit-delete",QStyle::SP_TrashIcon),w);l->addWidget(color);l->addWidget(label,1);for(auto*b:{up,down,eye,reset,remove})l->addWidget(b);seriesList_->setItemWidget(item,w);up->setEnabled(i>0);down->setEnabled(i+1<series_.size());
        connect(color,&QPushButton::clicked,this,[this,id=m.id]{auto it=std::find_if(series_.begin(),series_.end(),[&](auto&s){return s.metricId==id;});if(it==series_.end())return;QColor c=QColorDialog::getColor(it->color,this,"Select series colour",QColorDialog::DontUseNativeDialog);if(c.isValid()){it->color=c;saveSettings();rebuildSeriesList();applyState();}});
        connect(up,&QPushButton::clicked,this,[this,i]{moveSeries(i,i-1);});connect(down,&QPushButton::clicked,this,[this,i]{moveSeries(i,i+1);});connect(eye,&QPushButton::clicked,this,[this,id=m.id]{for(auto&s:series_)if(s.metricId==id)s.visible=!s.visible;saveSettings();rebuildSeriesList();applyState();});connect(reset,&QPushButton::clicked,this,[this,id=m.id]{for(auto&s:series_)if(s.metricId==id)s.color=analyzeMetric(id)->defaultColor;saveSettings();rebuildSeriesList();applyState();});connect(remove,&QPushButton::clicked,this,[this,id=m.id]{series_.erase(std::remove_if(series_.begin(),series_.end(),[&](const auto&s){return s.metricId==id;}),series_.end());saveSettings();rebuildSeriesList();rebuildMetricPicker();applyState();});}}
void AnalyzePage::moveSeries(int from,int to){if(from<0||to<0||from>=series_.size()||to>=series_.size())return;series_.move(from,to);saveSettings();rebuildSeriesList();applyState();}

void AnalyzePage::refreshLapSelectors(){auto fill=[&](QComboBox*box){QSignalBlocker guard(box);int old=box->currentData().toInt();box->clear();box->addItem("No option selected",-1);for(const LapBlock&lap:model_->data().laps){QString compound;for(auto it=lap.sts.crbegin();it!=lap.sts.crend();++it)if(it->tyre_compound>0){compound=tnr::Ln("tyre.actual",it->tyre_compound);break;}QString text=QString("%1%2Lap %3%4").arg(compound,compound.isEmpty()?"":" · ").arg(lap.lapNum).arg(lap.lapNum==model_->data().fastestLapNum?" · FL":"");box->addItem(text,lap.lapNum);}int idx=box->findData(old);box->setCurrentIndex(idx>=0?idx:0);};fill(compareLap_);fill(lapA_);fill(lapB_);applyState();}
void AnalyzePage::applyState(){const bool fixed=playback_&&fixedMode_->isChecked();fixedMode_->setEnabled(playback_&&!model_->data().laps.isEmpty());compareLap_->parentWidget()->setVisible(!fixed);compareLap_->setEnabled(playback_);compareClear_->setVisible(!fixed);compareClear_->setEnabled(playback_&&compareLap_->currentData().toInt()>0);lapA_->parentWidget()->setVisible(fixed);lapB_->parentWidget()->setVisible(fixed);chart_->setConfig(series_,showYAxis_->isChecked());chart_->setComparisonLap(playback_&&!fixed?compareLap_->currentData().toInt():-1);chart_->setFixedLaps(fixed,lapA_->currentData().toInt(),lapB_->currentData().toInt());emit navigationEnabledChanged(fixed&&lapA_->currentData().toInt()>0);}
void AnalyzePage::setPlaybackMode(bool on,float t){playback_=on;chart_->setPlaybackMode(on);chart_->setCurrentTime(t);if(!on)resetPlaybackSelections();fixedMode_->setEnabled(on&&!model_->data().laps.isEmpty());applyState();}
void AnalyzePage::setCurrentTime(float t){chart_->setCurrentTime(t);}
void AnalyzePage::resetPlaybackSelections(){fixedMode_->setChecked(false);compareLap_->setCurrentIndex(0);if(lapA_->count())lapA_->setCurrentIndex(0);if(lapB_->count())lapB_->setCurrentIndex(0);applyState();}
void AnalyzePage::zoomIn(){chart_->zoomIn();}
void AnalyzePage::zoomOut(){chart_->zoomOut();}
void AnalyzePage::panLeft(){chart_->panLeft();}
void AnalyzePage::panRight(){chart_->panRight();}
void AnalyzePage::resetZoom(){chart_->resetZoom();}
