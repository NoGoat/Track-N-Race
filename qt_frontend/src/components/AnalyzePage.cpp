#include "AnalyzePage.h"
#include "AnalyzeChart.h"
#include "ClearableComboBox.h"
#include "AnalyzeMapComparison.h"
#include "../AnalysisFileReader.h"
#include "../SessionModel.h"
#include "../TnrdPlayer.h"
#include "../Labels.h"
#include "../IconUtils.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QCompleter>
#include <QFrame>
#include <QFileDialog>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStandardItemModel>
#include <QStackedWidget>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace {
QIcon analyzeIcon(QWidget* w,const char* name,QStyle::StandardPixmap fallback){return adaptThemeIcon(QIcon::fromTheme(QString::fromLatin1(name)),w->palette().color(QPalette::WindowText),w->style()->standardIcon(fallback));}
QPushButton* tinyButton(const QString& tip,const QIcon& icon,QWidget* parent){auto*b=new QPushButton(parent);b->setIcon(icon);b->setToolTip(tip);b->setFixedSize(24,24);b->setFlat(true);return b;}
QString lapKey(bool secondary,int lapNum){return lapNum>0?QString("file%1:%2").arg(secondary?2:1).arg(lapNum):QString();}
bool secondaryChoice(const QComboBox* box){return box&&box->currentData().toString().startsWith("file2:");}
int selectedLap(const QComboBox* box){if(!box)return -1;const QString key=box->currentData().toString();const int colon=key.indexOf(':');bool ok=false;const int lap=colon>=0?key.sliced(colon+1).toInt(&ok):-1;return ok?lap:-1;}

template<class T>
void mergeRows(QVector<T>& target,QVector<T>&& source){
    if(target.isEmpty()){target=std::move(source);return;}
    target+=std::move(source);
    std::stable_sort(target.begin(),target.end(),[](const T&a,const T&b){return a.t<b.t;});
    auto last=std::unique(target.begin(),target.end(),[](const T&a,const T&b){return qFuzzyCompare(a.t+1.0f,b.t+1.0f);});
    target.erase(last,target.end());
}
}

struct AnalyzePage::SecondaryState {
    SessionData data;
    QHash<int,LapBlock> cache;
    QHash<int,uint32_t> installedMasks;
    QHash<int,uint32_t> requestedMasks;
    QVector<int> lru;
    QString filename;
    QString trackName;
    int trackId=-1;
    uint64_t generation=0;
    bool distanceAvailable=false;
};

AnalyzePage::AnalyzePage(SessionModel* model,QWidget* parent):QWidget(parent),model_(model){
    loadSettings();
    auto* root=new QVBoxLayout(this);root->setContentsMargins(0,0,0,0);root->setSpacing(0);

    // MainWindow reparents this composite into the application's existing top
    // toolbar. AnalyzePage continues to own the state and behavior of its
    // controls; AppToolbar only owns their placement and responsive visibility.
    toolbarControls_=new QWidget;
    toolbarControls_->setObjectName("analyzeToolbarControls");
    auto* toolbarLayout=new QHBoxLayout(toolbarControls_);toolbarLayout->setContentsMargins(2,0,2,0);toolbarLayout->setSpacing(4);
    inspectorButton_=new QToolButton(toolbarControls_);inspectorButton_->setAutoRaise(true);inspectorButton_->setCheckable(true);inspectorButton_->setChecked(!collapsed_);inspectorButton_->setIcon(analyzeIcon(this,"view-list-details",QStyle::SP_FileDialogDetailedView));inspectorButton_->setToolTip("Show or hide metric and display settings");toolbarLayout->addWidget(inspectorButton_);

    auto* viewControl=new QWidget(toolbarControls_);
    auto* viewLayout=new QHBoxLayout(viewControl);viewLayout->setContentsMargins(4,0,4,0);viewLayout->setSpacing(5);
    auto* viewLabel=new QLabel("View",viewControl);
    viewMode_=new QComboBox(viewControl);viewMode_->addItem("Graphs","graph");viewMode_->addItem("Map","map");viewMode_->setCurrentIndex(preferredView_=="map"?1:0);viewMode_->setFrame(false);viewMode_->setToolTip("Analysis view");
    viewLayout->addWidget(viewLabel);viewLayout->addWidget(viewMode_);toolbarLayout->addWidget(viewControl);

    secondaryFileRow_=new QWidget(toolbarControls_);auto*secondaryLayout=new QHBoxLayout(secondaryFileRow_);secondaryLayout->setContentsMargins(4,0,2,0);secondaryLayout->setSpacing(3);secondaryFileLabel_=new QLabel("No secondary file",secondaryFileRow_);secondaryFileLabel_->setMinimumWidth(0);secondaryFileLabel_->setMaximumWidth(110);secondaryFileLabel_->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);secondaryOpen_=tinyButton("Open Secondary File",analyzeIcon(secondaryFileRow_,"document-open",QStyle::SP_DialogOpenButton),secondaryFileRow_);secondaryClear_=tinyButton("Clear Secondary File",analyzeIcon(secondaryFileRow_,"edit-clear",QStyle::SP_DialogCloseButton),secondaryFileRow_);secondaryClear_->hide();secondaryLayout->addWidget(secondaryFileLabel_,1);secondaryLayout->addWidget(secondaryOpen_);secondaryLayout->addWidget(secondaryClear_);toolbarLayout->addWidget(secondaryFileRow_);

    secondaryErrorLabel_=new QLabel;secondaryErrorLabel_->setContentsMargins(10,4,10,4);secondaryErrorLabel_->setWordWrap(true);QPalette errorPalette=secondaryErrorLabel_->palette();errorPalette.setColor(QPalette::WindowText,QColor("#d44252"));secondaryErrorLabel_->setPalette(errorPalette);secondaryErrorLabel_->hide();root->addWidget(secondaryErrorLabel_);

    contentSplitter_=new QSplitter(Qt::Horizontal,this);
    contentSplitter_->setChildrenCollapsible(false);
    contentSplitter_->setHandleWidth(1);

    viewStack_=new QStackedWidget(contentSplitter_);chart_=new AnalyzeChart;chart_->setModel(model_);map_=new AnalyzeMapComparison;viewStack_->addWidget(chart_);viewStack_->addWidget(map_);contentSplitter_->addWidget(viewStack_);

    sidebar_=new QFrame(contentSplitter_);sidebar_->setFrameShape(QFrame::NoFrame);sidebar_->setMinimumWidth(300);sidebar_->setMaximumWidth(420);
    auto* side=new QVBoxLayout(sidebar_);side->setContentsMargins(12,10,12,10);side->setSpacing(10);
    auto* header=new QWidget;
    auto* headerLayout=new QHBoxLayout(header);headerLayout->setContentsMargins(0,0,0,0);headerLayout->setSpacing(4);
    auto* title=new QLabel("Metrics & Display");QFont titleFont=title->font();titleFont.setBold(true);title->setFont(titleFont);
    collapse_=tinyButton("Hide metric settings",analyzeIcon(header,"window-close",QStyle::SP_DockWidgetCloseButton),header);
    headerLayout->addWidget(title);headerLayout->addStretch();headerLayout->addWidget(collapse_);
    side->addWidget(header);
    auto*displayGroup=new QGroupBox("Graph display",sidebar_);auto*displayLayout=new QVBoxLayout(displayGroup);displayLayout->setContentsMargins(8,8,8,8);displayLayout->setSpacing(5);
    auto*modeRow=new QWidget(displayGroup);auto*modeLayout=new QHBoxLayout(modeRow);modeLayout->setContentsMargins(0,0,0,0);modeLayout->setSpacing(6);individualGraphs_=new QCheckBox("Individual graphs");individualGraphs_->setChecked(settings_.value("analyze/individualGraphs",false).toBool());syncedTooltip_=new QCheckBox("Sync tooltips");syncedTooltip_->setChecked(settings_.value("analyze/syncedTooltip",false).toBool());modeLayout->addWidget(individualGraphs_);modeLayout->addWidget(syncedTooltip_);displayLayout->addWidget(modeRow);
    auto*sectorRow=new QWidget(displayGroup);auto*sectorLayout=new QHBoxLayout(sectorRow);sectorLayout->setContentsMargins(0,0,0,0);sectorLayout->setSpacing(6);sectorBoundaries_=new QCheckBox("Sector boundaries");sectorBoundaries_->setChecked(settings_.value("analyze/sectorBoundaries",false).toBool());sectorDelta_=new QCheckBox("Sector delta");sectorDelta_->setChecked(sectorBoundaries_->isChecked()&&settings_.value("analyze/sectorDelta",false).toBool());sectorLayout->addWidget(sectorBoundaries_);sectorLayout->addWidget(sectorDelta_);displayLayout->addWidget(sectorRow);
    showYAxis_=new QCheckBox("Show Y-axis values");showYAxis_->setChecked(std::all_of(series_.cbegin(),series_.cend(),[](const auto&s){return s.showYAxis;}));displayLayout->addWidget(showYAxis_);side->addWidget(displayGroup);
    auto*comparisonGroup=new QGroupBox("Lap comparison",sidebar_);auto*comparisonLayout=new QVBoxLayout(comparisonGroup);comparisonLayout->setContentsMargins(8,8,8,8);comparisonLayout->setSpacing(5);
    fixedMode_=new QCheckBox("Fixed laps",comparisonGroup);fixedMode_->setToolTip("Compare two selected laps instead of following playback");fixedMode_->setEnabled(false);comparisonLayout->addWidget(fixedMode_);
    auto lapControl=[&](const QString&label,ClearableComboBox*&box){auto*w=new QWidget(comparisonGroup);auto*l=new QHBoxLayout(w);l->setContentsMargins(0,0,0,0);l->setSpacing(6);auto*lab=new QLabel(label,w);lab->setMinimumWidth(54);box=new ClearableComboBox(w);box->setMinimumContentsLength(10);box->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);box->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Preferred);l->addWidget(lab);l->addWidget(box,1);comparisonLayout->addWidget(w);};
    lapControl("Compare",compareLap_);lapControl("Lap A",lapA_);lapControl("Lap B",lapB_);side->addWidget(comparisonGroup);
    auto*metricLabel=new QLabel("Add metric");side->addWidget(metricLabel);
    addMetric_=new QComboBox;addMetric_->setEditable(true);addMetric_->setInsertPolicy(QComboBox::NoInsert);addMetric_->lineEdit()->setPlaceholderText("Choose a value…");side->addWidget(addMetric_);rebuildMetricPicker();
    auto*mapColors=new QGroupBox("Map colors",sidebar_);auto*mapColorLayout=new QHBoxLayout(mapColors);mapColorLayout->setContentsMargins(8,8,8,8);mapColorLayout->setSpacing(4);mapCurrentColor_=new QPushButton("Current lap");mapComparisonColor_=new QPushButton("Comparison");mapColorLayout->addWidget(mapCurrentColor_);mapColorLayout->addWidget(mapComparisonColor_);side->addWidget(mapColors);mapColors->setObjectName("analyzeMapColors");
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
    contentSplitter_->addWidget(sidebar_);
    contentSplitter_->setStretchFactor(0,1);contentSplitter_->setStretchFactor(1,0);
    contentSplitter_->setSizes({900,340});
    sidebar_->setVisible(!collapsed_);
    root->addWidget(contentSplitter_,1);
    secondaryReader_=new AnalysisFileReader(this);
    rebuildSeriesList();refreshLapSelectors();applyState();

    connect(addMetric_,qOverload<int>(&QComboBox::activated),this,[this](int i){QString id=addMetric_->itemData(i).toString();const auto*m=analyzeMetric(id);if(!m)return;series_.push_back({id,m->defaultColor,QColor(),true,true});saveSettings();rebuildSeriesList();rebuildMetricPicker();applyState();});
    connect(viewMode_,qOverload<int>(&QComboBox::activated),this,[this](int){preferredView_=viewMode_->currentData().toString();saveSettings();applyState();});
    connect(individualGraphs_,&QCheckBox::toggled,this,[this](bool){saveSettings();applyState();});
    connect(syncedTooltip_,&QCheckBox::toggled,this,[this](bool){saveSettings();applyState();});
    connect(sectorBoundaries_,&QCheckBox::toggled,this,[this](bool on){if(!on)sectorDelta_->setChecked(false);saveSettings();applyState();});
    connect(sectorDelta_,&QCheckBox::toggled,this,[this](bool){saveSettings();applyState();});
    connect(showYAxis_,&QCheckBox::toggled,this,[this](bool on){for(auto&s:series_)s.showYAxis=on;saveSettings();rebuildSeriesList();applyState();});
    connect(fixedMode_,&QCheckBox::toggled,this,[this](bool){applyState();});
    for(auto* b:{compareLap_,lapA_,lapB_})connect(b,qOverload<int>(&QComboBox::currentIndexChanged),this,[this](int){applyState();});
    auto setInspectorVisible=[this](bool visible){collapsed_=!visible;sidebar_->setVisible(visible);QSignalBlocker guard(inspectorButton_);inspectorButton_->setChecked(visible);saveSettings();};
    connect(inspectorButton_,&QToolButton::toggled,this,setInspectorVisible);
    connect(collapse_,&QPushButton::clicked,this,[setInspectorVisible]{setInspectorVisible(false);});
    connect(secondaryOpen_,&QPushButton::clicked,this,&AnalyzePage::loadSecondaryFile);
    connect(secondaryClear_,&QPushButton::clicked,this,[this]{clearSecondaryFile(true);});
    connect(secondaryReader_,&AnalysisFileReader::catalogLoaded,this,[this](const std::shared_ptr<AnalysisFileCatalog>& catalog){
        secondaryLoading_=false;secondaryOpen_->setEnabled(true);secondaryClear_->setEnabled(true);
        if(primaryTrackId_>=0&&catalog->trackId>=0&&catalog->trackId!=primaryTrackId_){
            QMessageBox warning(QMessageBox::Warning,"Circuit Mismatch","The Secondary File was recorded on a different circuit.",QMessageBox::NoButton,this);
            warning.setInformativeText("Lap comparisons may not align correctly. You can cancel or load the file anyway.\n\nLoaded File: "+(primaryTrackName_.isEmpty()?QString("Circuit %1").arg(primaryTrackId_):primaryTrackName_)+"\nSelected File: "+(catalog->trackName.isEmpty()?QString("Circuit %1").arg(catalog->trackId):catalog->trackName));
            auto*loadAnyway=warning.addButton("Load Anyway",QMessageBox::AcceptRole);warning.addButton(QMessageBox::Cancel);warning.exec();
            if(warning.clickedButton()!=loadAnyway){secondaryReader_->rejectLoaded(catalog->generation);secondaryFileLabel_->setText(secondary_?secondary_->filename:"No secondary file");return;}
        }
        secondaryReader_->acceptLoaded(catalog->generation);applySecondaryFile(catalog);
    });
    connect(secondaryReader_,&AnalysisFileReader::loadFailed,this,[this](const QString& reason){secondaryLoading_=false;clearSecondaryFile(false);secondaryErrorLabel_->setText(reason.isEmpty()?"The recording could not be opened.":reason);secondaryErrorLabel_->show();});
    connect(secondaryReader_,&AnalysisFileReader::lapDataReady,this,&AnalyzePage::installSecondaryLap);
    auto chooseMapColor=[this](bool current){QColor initial=current?mapCurrent_:mapComparison_;QColor color=QColorDialog::getColor(initial,this,current?"Current map color":"Comparison map color",QColorDialog::DontUseNativeDialog);if(!color.isValid())return;if(current)mapCurrent_=color;else mapComparison_=color;saveSettings();applyState();};
    connect(mapCurrentColor_,&QPushButton::clicked,this,[chooseMapColor]{chooseMapColor(true);});connect(mapComparisonColor_,&QPushButton::clicked,this,[chooseMapColor]{chooseMapColor(false);});
    connect(chart_,&ChartView::inspectionRequested,this,&AnalyzePage::inspectMap);
    connect(model_,&SessionModel::lapsChanged,this,&AnalyzePage::refreshLapSelectors);
    connect(model_,&SessionModel::chartConfigurationChanged,this,&AnalyzePage::applyState);
    connect(seriesList_->model(),&QAbstractItemModel::rowsMoved,this,[this]{QVector<AnalyzeSeriesSetting> next;for(int i=0;i<seriesList_->count();++i){QString id=seriesList_->item(i)->data(Qt::UserRole).toString();auto it=std::find_if(series_.cbegin(),series_.cend(),[&](const auto&s){return s.metricId==id;});if(it!=series_.cend())next<<*it;}series_=next;saveSettings();applyState();QTimer::singleShot(0,this,&AnalyzePage::rebuildSeriesList);});
}

AnalyzePage::~AnalyzePage()=default;

void AnalyzePage::setPrimaryRecording(int trackId,const QString& trackName){primaryTrackId_=trackId;primaryTrackName_=trackName;}

void AnalyzePage::loadSettings(){
    collapsed_=settings_.value("analyze/collapsed",false).toBool();preferredView_=settings_.value("analyze/view","graph").toString()=="map"?"map":"graph";mapCurrent_=QColor(settings_.value("analyze/mapCurrentColor","#5794F2").toString());mapComparison_=QColor(settings_.value("analyze/mapComparisonColor","#C4162A").toString());const bool defaultAxis=settings_.value("analyze/showYAxis",true).toBool();QJsonDocument doc=QJsonDocument::fromJson(settings_.value("analyze/series").toByteArray());QSet<QString>seen;if(doc.isArray())for(const auto&v:doc.array()){const auto o=v.toObject();const QString id=o["metricId"].toString();if(seen.contains(id))continue;if(id=="delta"){QColor positive(o["color"].toString()),negative(o["negativeColor"].toString());series_<<AnalyzeSeriesSetting{id,positive.isValid()?positive:QColor("#C4162A"),negative.isValid()?negative:QColor("#37872D"),o["visible"].toBool(true),o.contains("showYAxis")?o["showYAxis"].toBool():defaultAxis};seen.insert(id);continue;}const auto*m=analyzeMetric(id);QColor color(o["color"].toString());if(!m)continue;series_<<AnalyzeSeriesSetting{id,color.isValid()?color:m->defaultColor,QColor(),o["visible"].toBool(true),o.contains("showYAxis")?o["showYAxis"].toBool():defaultAxis};seen.insert(id);}if(!doc.isArray())for(const char*id:{"speed","rpm","ers"}){const auto*m=analyzeMetric(id);series_<<AnalyzeSeriesSetting{m->id,m->defaultColor,QColor(),true,defaultAxis};}if(!seen.contains("delta"))series_<<AnalyzeSeriesSetting{"delta",QColor("#C4162A"),QColor("#37872D"),true,defaultAxis};
}
void AnalyzePage::saveSettings(){settings_.setValue("analyze/version",7);settings_.setValue("analyze/collapsed",collapsed_);settings_.setValue("analyze/showYAxis",showYAxis_?showYAxis_->isChecked():true);settings_.setValue("analyze/view",preferredView_);if(individualGraphs_)settings_.setValue("analyze/individualGraphs",individualGraphs_->isChecked());if(syncedTooltip_)settings_.setValue("analyze/syncedTooltip",syncedTooltip_->isChecked());if(sectorBoundaries_)settings_.setValue("analyze/sectorBoundaries",sectorBoundaries_->isChecked());if(sectorDelta_)settings_.setValue("analyze/sectorDelta",sectorDelta_->isChecked());settings_.setValue("analyze/mapCurrentColor",mapCurrent_.name());settings_.setValue("analyze/mapComparisonColor",mapComparison_.name());QJsonArray a;for(const auto&s:series_){QJsonObject o;o["metricId"]=s.metricId;o["color"]=s.color.name();if(s.metricId=="delta")o["negativeColor"]=s.negativeColor.name();o["visible"]=s.visible;o["showYAxis"]=s.showYAxis;a.append(o);}settings_.setValue("analyze/series",QJsonDocument(a).toJson(QJsonDocument::Compact));}

void AnalyzePage::rebuildMetricPicker(){if(!addMetric_)return;QSignalBlocker b(addMetric_);addMetric_->clear();QSet<QString> used;for(const auto&s:series_)used.insert(s.metricId);for(const auto&m:analyzeMetrics())if(!used.contains(m.id))addMetric_->addItem(QString("%1 · %2").arg(m.group,m.label),m.id);addMetric_->setCurrentIndex(-1);if(addMetric_->completer()){addMetric_->completer()->setCaseSensitivity(Qt::CaseInsensitive);addMetric_->completer()->setFilterMode(Qt::MatchContains);}}

void AnalyzePage::rebuildSeriesList(){seriesList_->clear();deltaStatus_=nullptr;for(int i=0;i<series_.size();++i){const QString id=series_[i].metricId;const auto*m=analyzeMetric(id);const bool delta=id=="delta";if(!delta&&!m)continue;auto*item=new QListWidgetItem;item->setData(Qt::UserRole,id);item->setSizeHint(QSize(260,40));seriesList_->addItem(item);auto*w=new QWidget;auto*l=new QHBoxLayout(w);l->setContentsMargins(2,1,2,1);l->setSpacing(3);auto swatch=[&](QColor c,const QString&tip){auto*b=new QPushButton(w);b->setToolTip(tip);b->setFixedSize(delta?18:24,24);b->setFlat(true);b->setStyleSheet("background:"+c.name()+";border:1px solid palette(mid);border-radius:3px;");return b;};auto*positive=swatch(series_[i].color,delta?"Positive delta color":m->label+" color");QPushButton*negative=delta?swatch(series_[i].negativeColor,"Negative delta color"):nullptr;auto*label=new QLabel(QString("%1\n%2").arg(delta?"Delta":m->label,delta?"Time · + / −":m->group+(m->unit.isEmpty()?QString():" · "+m->unit)));label->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Preferred);if(delta)deltaStatus_=label;auto*up=tinyButton("Move up",analyzeIcon(w,"go-up",QStyle::SP_ArrowUp),w);auto*down=tinyButton("Move down",analyzeIcon(w,"go-down",QStyle::SP_ArrowDown),w);auto*axis=new QPushButton("Y",w);axis->setToolTip(series_[i].showYAxis?"Hide Y-axis":"Show Y-axis");axis->setCheckable(true);axis->setChecked(series_[i].showYAxis);axis->setFixedSize(24,24);axis->setFlat(true);auto*eye=tinyButton(series_[i].visible?"Hide series":"Show series",analyzeIcon(w,series_[i].visible?"view-visible":"view-hidden",QStyle::SP_FileDialogInfoView),w);auto*reset=tinyButton("Reset color",analyzeIcon(w,"view-refresh",QStyle::SP_BrowserReload),w);QPushButton*remove=delta?nullptr:tinyButton("Remove metric",analyzeIcon(w,"edit-delete",QStyle::SP_TrashIcon),w);l->addWidget(positive);if(negative)l->addWidget(negative);l->addWidget(label,1);for(auto*b:{up,down,axis,eye,reset})l->addWidget(b);if(remove)l->addWidget(remove);seriesList_->setItemWidget(item,w);up->setEnabled(i>0);down->setEnabled(i+1<series_.size());
        auto choose=[this,id](bool negative){auto it=std::find_if(series_.begin(),series_.end(),[&](auto&s){return s.metricId==id;});if(it==series_.end())return;QColor&target=negative?it->negativeColor:it->color;QColor c=QColorDialog::getColor(target,this,"Select series colour",QColorDialog::DontUseNativeDialog);if(c.isValid()){target=c;saveSettings();rebuildSeriesList();applyState();}};connect(positive,&QPushButton::clicked,this,[choose]{choose(false);});if(negative)connect(negative,&QPushButton::clicked,this,[choose]{choose(true);});connect(up,&QPushButton::clicked,this,[this,i]{moveSeries(i,i-1);});connect(down,&QPushButton::clicked,this,[this,i]{moveSeries(i,i+1);});connect(axis,&QPushButton::clicked,this,[this,id]{for(auto&s:series_)if(s.metricId==id)s.showYAxis=!s.showYAxis;QSignalBlocker guard(showYAxis_);showYAxis_->setChecked(std::all_of(series_.cbegin(),series_.cend(),[](const auto&s){return s.showYAxis;}));saveSettings();rebuildSeriesList();applyState();});connect(eye,&QPushButton::clicked,this,[this,id]{for(auto&s:series_)if(s.metricId==id)s.visible=!s.visible;saveSettings();rebuildSeriesList();applyState();});connect(reset,&QPushButton::clicked,this,[this,id]{for(auto&s:series_)if(s.metricId==id){if(id=="delta"){s.color=QColor("#C4162A");s.negativeColor=QColor("#37872D");}else s.color=analyzeMetric(id)->defaultColor;}saveSettings();rebuildSeriesList();applyState();});if(remove)connect(remove,&QPushButton::clicked,this,[this,id]{series_.erase(std::remove_if(series_.begin(),series_.end(),[&](const auto&s){return s.metricId==id;}),series_.end());saveSettings();rebuildSeriesList();rebuildMetricPicker();applyState();});}}
void AnalyzePage::moveSeries(int from,int to){if(from<0||to<0||from>=series_.size()||to>=series_.size())return;series_.move(from,to);saveSettings();rebuildSeriesList();applyState();}

void AnalyzePage::refreshLapSelectors(){auto fill=[&](QComboBox*box){QSignalBlocker guard(box);const QString old=box->currentData().toString();box->clear();box->addItem("No option selected",QString());auto addGroup=[&](const QString&title,const SessionData&data,bool second){box->addItem(title);if(auto*m=qobject_cast<QStandardItemModel*>(box->model()))if(auto*item=m->item(box->count()-1))item->setFlags(Qt::NoItemFlags);for(const LapBlock&lap:data.laps){QString compound;for(auto it=lap.sts.crbegin();it!=lap.sts.crend();++it)if(it->tyre_compound>0){compound=tnr::Ln("tyre.actual",it->tyre_compound);break;}QString lapTime;if(lap.lapTimeMs>0)lapTime=QString(" · %1:%2").arg(lap.lapTimeMs/60000).arg((lap.lapTimeMs%60000)/1000.0,6,'f',3,QChar('0'));QString text=QString("%1%2Lap %3%4%5").arg(compound,compound.isEmpty()?"":" · ").arg(lap.lapNum).arg(lapTime).arg(lap.lapNum==data.fastestLapNum?" · FL":"");box->addItem(text,lapKey(second,lap.lapNum));}};addGroup("Primary File",model_->data(),false);if(secondary_)addGroup("Secondary File",secondary_->data,true);const int idx=box->findData(old);box->setCurrentIndex(idx>=0?idx:0);};fill(compareLap_);fill(lapA_);fill(lapB_);applyState();}
void AnalyzePage::applyState(){
    const bool fixed=playback_&&fixedMode_->isChecked();
    const bool compareSecond=!fixed&&secondaryChoice(compareLap_);
    const bool aSecond=fixed&&secondaryChoice(lapA_);
    const bool bSecond=fixed&&secondaryChoice(lapB_);
    const bool usesSecond=compareSecond||aSecond||bSecond;
    const bool distance=model_->lapCoordinatesAvailable()&&(!usesSecond||(secondary_&&secondary_->distanceAvailable));
    if(!playback_&&viewMode_->currentData().toString()=="map"){QSignalBlocker guard(viewMode_);viewMode_->setCurrentIndex(viewMode_->findData("graph"));}
    const bool showMap=playback_&&viewMode_->currentData().toString()=="map";

    if(auto*menu=qobject_cast<QStandardItemModel*>(viewMode_->model()))
        if(auto*mapItem=menu->item(viewMode_->findData("map")))mapItem->setEnabled(playback_);
    fixedMode_->setEnabled(playback_&&!model_->data().laps.isEmpty());
    secondaryFileRow_->setVisible(playback_);secondaryErrorLabel_->setVisible(playback_&&!secondaryErrorLabel_->text().isEmpty());
    compareLap_->parentWidget()->setVisible(!fixed);compareLap_->setEnabled(playback_);
    lapA_->parentWidget()->setVisible(fixed);lapB_->parentWidget()->setVisible(fixed);
    compareLap_->setClearVisible(selectedLap(compareLap_)>0);lapA_->setClearVisible(selectedLap(lapA_)>0);lapB_->setClearVisible(selectedLap(lapB_)>0);
    individualGraphs_->setEnabled(true);syncedTooltip_->setEnabled(individualGraphs_->isChecked());sectorBoundaries_->setEnabled(true);sectorDelta_->setEnabled(sectorBoundaries_->isChecked());
    if(auto*colors=findChild<QWidget*>("analyzeMapColors"))colors->setVisible(showMap);
    mapCurrentColor_->setStyleSheet("background:"+mapCurrent_.name()+";border:1px solid palette(mid);border-radius:3px;");
    mapComparisonColor_->setStyleSheet("background:"+mapComparison_.name()+";border:1px solid palette(mid);border-radius:3px;");
    const bool deltaUnsupported=playback_&&!distance;
    if(deltaStatus_)deltaStatus_->setText(deltaUnsupported?"Delta\nNot supported in this file.":"Delta\nTime · + / −");
    for(int row=0;row<seriesList_->count();++row)if(seriesList_->item(row)->data(Qt::UserRole).toString()=="delta"){
        if(QWidget*card=seriesList_->itemWidget(seriesList_->item(row))){const auto buttons=card->findChildren<QPushButton*>(QString(),Qt::FindDirectChildrenOnly);for(int i=0;i<buttons.size();++i){if(i<2)buttons[i]->setEnabled(!deltaUnsupported);else buttons[i]->setVisible(!deltaUnsupported);}}
        break;
    }

    chart_->setConfig(series_,showYAxis_->isChecked());
    chart_->setSecondarySource(secondary_?&secondary_->data:nullptr,secondary_?&secondary_->cache:nullptr);
    chart_->setDistanceMode(distance);chart_->setIndividualGraphs(individualGraphs_->isChecked(),syncedTooltip_->isChecked());chart_->setSectorOptions(sectorBoundaries_->isChecked(),sectorDelta_->isChecked());
    chart_->setComparisonLap(playback_&&!fixed?selectedLap(compareLap_):-1,compareSecond);
    chart_->setFixedLaps(fixed,selectedLap(lapA_),aSecond,selectedLap(lapB_),bSecond);

    const SessionData&primaryData=model_->data();
    auto resolve=[&](int lapNum,bool second)->const LapBlock*{
        if(lapNum<=0)return nullptr;
        if(second){if(!secondary_)return nullptr;auto it=secondary_->cache.constFind(lapNum);return it==secondary_->cache.cend()?nullptr:&it.value();}
        return playback_?model_->playbackLapData(lapNum):primaryData.lapByNum(lapNum);
    };
    const LapBlock*current=nullptr;const LapBlock*comparison=nullptr;bool currentSecond=false,comparisonSecond=false;
    if(fixed){currentSecond=aSecond;comparisonSecond=bSecond;current=resolve(selectedLap(lapA_),aSecond);comparison=resolve(selectedLap(lapB_),bSecond);}
    else{current=model_->chartPrimaryLap(currentTime_);comparisonSecond=compareSecond;comparison=resolve(selectedLap(compareLap_),compareSecond);}
    const int currentTrack=currentSecond&&secondary_?secondary_->trackId:primaryTrackId_;
    const int comparisonTrack=comparisonSecond&&secondary_?secondary_->trackId:primaryTrackId_;
    const bool compatible=!current||!comparison||currentTrack<0||comparisonTrack<0||currentTrack==comparisonTrack;
    map_->setColors(mapCurrent_,mapComparison_);map_->setLaps(current,comparison,fixed,currentTrack>=0?currentTrack:comparisonTrack,compatible);map_->setCurrentTime(currentTime_);
    viewStack_->setCurrentWidget(showMap?static_cast<QWidget*>(map_):static_cast<QWidget*>(chart_));
    emit navigationEnabledChanged(!showMap&&fixed&&selectedLap(lapA_)>0);emit dataRequirementsChanged();requestSecondaryLaps();
}
void AnalyzePage::setPlaybackMode(bool on,float t){playback_=on;currentTime_=t;if(on){const int wanted=viewMode_->findData(preferredView_);if(wanted>=0){QSignalBlocker guard(viewMode_);viewMode_->setCurrentIndex(wanted);}}chart_->setPlaybackMode(on);chart_->setCurrentTime(t);map_->setCurrentTime(t);if(!on){clearSecondaryFile(true);primaryTrackId_=-1;primaryTrackName_.clear();resetPlaybackSelections();}fixedMode_->setEnabled(on&&!model_->data().laps.isEmpty());applyState();}
void AnalyzePage::setCurrentTime(float t){currentTime_=t;chart_->setCurrentTime(t);map_->setCurrentTime(t);}
void AnalyzePage::setMapAppearance(bool sectorColors,int opacityPercent){map_->setMapAppearance(sectorColors,opacityPercent);}
void AnalyzePage::resetPlaybackSelections(){fixedMode_->setChecked(false);compareLap_->setCurrentIndex(0);if(lapA_->count())lapA_->setCurrentIndex(0);if(lapB_->count())lapB_->setCurrentIndex(0);applyState();}
void AnalyzePage::zoomIn(){chart_->zoomIn();}
void AnalyzePage::zoomOut(){chart_->zoomOut();}
void AnalyzePage::panLeft(){chart_->panLeft();}
void AnalyzePage::panRight(){chart_->panRight();}
void AnalyzePage::resetZoom(){chart_->resetZoom();}

void AnalyzePage::loadSecondaryFile(){
    const QString path=QFileDialog::getOpenFileName(this,"Open Secondary File",settings_.value("outputDirectory").toString(),"TNRD Recordings (*.tnrd *.trnd)");
    if(path.isEmpty())return;
    secondaryLoading_=true;secondaryErrorLabel_->clear();secondaryErrorLabel_->hide();secondaryFileLabel_->setText("Loading file");secondaryOpen_->setEnabled(false);secondaryClear_->setEnabled(false);secondaryReader_->load(path);
}

void AnalyzePage::applySecondaryFile(const std::shared_ptr<AnalysisFileCatalog>& catalog){
    if(!catalog)return;
    auto reinterpretAsPrimary=[&](QComboBox*box,bool clear){if(!secondaryChoice(box))return;const int lap=selectedLap(box);if(clear){box->setCurrentIndex(0);return;}const int primary=box->findData(lapKey(false,lap));box->setCurrentIndex(primary>=0?primary:0);};
    reinterpretAsPrimary(compareLap_,true);reinterpretAsPrimary(lapA_,false);reinterpretAsPrimary(lapB_,false);
    auto state=std::make_unique<SecondaryState>();state->filename=catalog->filename;state->trackName=catalog->trackName;state->trackId=catalog->trackId;state->generation=catalog->generation;state->distanceAvailable=catalog->laps.lapDistanceAvailable||catalog->laps.deltaAvailable;state->data.trimBuffers=false;state->data.fastestLapNum=catalog->laps.fastestLapNum;state->data.trackLengthM=static_cast<float>(catalog->laps.trackLengthM);QHash<int,int>times;for(const auto&lap:catalog->laps.laps)times.insert(lap.lapNum,lap.lapTimeMs);for(const auto&source:catalog->laps.blocks){LapBlock lap;lap.lapNum=source.lapNum;lap.startSessionTime=source.startSessionTime;lap.endSessionTime=source.endSessionTime;lap.lapTimeMs=times.value(source.lapNum);for(const auto&point:source.statusHistory)lap.sts.push_back({point.session_time,static_cast<float>(point.ers_pct),0,0,0,0,0,point.tyre_compound,point.visual_compound,0});state->data.laps.push_back(std::move(lap));}std::sort(state->data.laps.begin(),state->data.laps.end(),[](const LapBlock&a,const LapBlock&b){return a.startSessionTime<b.startSessionTime;});chart_->setSecondarySource(nullptr,nullptr);secondary_=std::move(state);secondaryFileLabel_->setText(secondary_->filename);secondaryFileLabel_->setToolTip(catalog->path);secondaryOpen_->setToolTip("Replace Secondary File");secondaryClear_->show();secondaryErrorLabel_->clear();secondaryErrorLabel_->hide();refreshLapSelectors();
}

void AnalyzePage::clearSecondaryFile(bool clearFixedSelections){
    auto reset=[&](QComboBox*box,bool fixed){if(!secondaryChoice(box))return;const int lap=selectedLap(box);if(fixed&&!clearFixedSelections){const int primary=box->findData(lapKey(false,lap));box->setCurrentIndex(primary>=0?primary:0);}else box->setCurrentIndex(0);};
    reset(compareLap_,false);reset(lapA_,true);reset(lapB_,true);chart_->setSecondarySource(nullptr,nullptr);secondary_.reset();secondaryReader_->close();secondaryLoading_=false;secondaryFileLabel_->setText("No secondary file");secondaryFileLabel_->setToolTip({});secondaryOpen_->setEnabled(playback_);secondaryOpen_->setToolTip("Open Secondary File");secondaryClear_->setEnabled(true);secondaryClear_->hide();if(clearFixedSelections){secondaryErrorLabel_->clear();secondaryErrorLabel_->hide();}refreshLapSelectors();
}

void AnalyzePage::installSecondaryLap(uint64_t generation,int lapNum,uint32_t rowTypeMask,const std::shared_ptr<PlaybackHistoryBatch>& batch){
    if(!secondary_||secondary_->generation!=generation)return;secondary_->requestedMasks[lapNum]&=~rowTypeMask;if(!batch||batch->lapDetails.isEmpty()){requestSecondaryLaps();return;}const uint32_t payloadMask=batch->rowTypeMask;LapBlock detail=std::move(batch->lapDetails.first());auto it=secondary_->cache.find(lapNum);if(it==secondary_->cache.end()){LapBlock base;if(const LapBlock*meta=secondary_->data.lapByNum(lapNum))base=*meta;else{base.lapNum=lapNum;base.startSessionTime=detail.startSessionTime;base.endSessionTime=detail.endSessionTime;}it=secondary_->cache.insert(lapNum,std::move(base));}LapBlock&cached=it.value();uint32_t&installed=secondary_->installedMasks[lapNum];auto install=[&](uint32_t bit,auto&target,auto&source){if(!(installed&bit))target=std::move(source);else mergeRows(target,std::move(source));installed|=bit;};if(payloadMask&(1u<<1)){install(1u<<1,cached.tel,detail.tel);if(cached.tyre.isEmpty())cached.tyre=std::move(detail.tyre);else mergeRows(cached.tyre,std::move(detail.tyre));}if(payloadMask&(1u<<2))install(1u<<2,cached.sts,detail.sts);if(payloadMask&(1u<<3))install(1u<<3,cached.damage,detail.damage);if(payloadMask&(1u<<4))install(1u<<4,cached.progress,detail.progress);if(payloadMask&(1u<<11))install(1u<<11,cached.motion,detail.motion);if(payloadMask&(1u<<12))install(1u<<12,cached.motionEx,detail.motionEx);if(payloadMask&(1u<<13))install(1u<<13,cached.positions,detail.positions);secondary_->lru.removeAll(lapNum);secondary_->lru.push_back(lapNum);while(secondary_->lru.size()>6){const int old=secondary_->lru.takeFirst();secondary_->cache.remove(old);secondary_->installedMasks.remove(old);secondary_->requestedMasks.remove(old);}chart_->setSecondarySource(&secondary_->data,&secondary_->cache);applyState();
}

void AnalyzePage::requestSecondaryLaps(){
    if(!secondary_||secondaryLoading_)return;QSet<int>laps;if(fixedMode_->isChecked()){if(secondaryChoice(lapA_)&&selectedLap(lapA_)>0)laps.insert(selectedLap(lapA_));if(secondaryChoice(lapB_)&&selectedLap(lapB_)>0)laps.insert(selectedLap(lapB_));}else if(secondaryChoice(compareLap_)&&selectedLap(compareLap_)>0)laps.insert(selectedLap(compareLap_));const uint32_t wanted=playbackRowMask();for(int lap:laps){const uint32_t missing=wanted&~secondary_->installedMasks.value(lap)&~secondary_->requestedMasks.value(lap);if(!missing)continue;secondary_->requestedMasks[lap]|=missing;secondaryReader_->requestLapData(lap,missing);}
}

uint32_t AnalyzePage::playbackRowMask() const {
    if (playback_ && viewMode_ && viewMode_->currentData().toString() == "map")
        return (1u << 4) | (1u << 13);
    uint32_t mask = 1u << 4;
    for (const AnalyzeSeriesSetting& setting : series_) {
        if (!setting.visible) continue;
        const AnalyzeMetric* metric = analyzeMetric(setting.metricId);
        if (!metric) continue;
        switch (metric->source) {
            case AnalyzeSource::Telemetry:
            case AnalyzeSource::Tyre: mask |= 1u << 1; break;
            case AnalyzeSource::Status: mask |= 1u << 2; break;
            case AnalyzeSource::Damage: mask |= 1u << 3; break;
            case AnalyzeSource::Motion: mask |= 1u << 11; break;
            case AnalyzeSource::MotionEx: mask |= 1u << 12; break;
        }
    }
    return mask;
}

void AnalyzePage::inspectMap(double coordinate,bool distanceCoordinate){
    if(!playback_)return;
    const bool fixed=fixedMode_->isChecked();const bool second=fixed&&secondaryChoice(lapA_);const int lapNum=fixed?selectedLap(lapA_):-1;
    const SessionData*owner=&model_->data();const LapBlock*lap=nullptr;
    if(second&&secondary_){owner=&secondary_->data;auto it=secondary_->cache.constFind(lapNum);if(it!=secondary_->cache.cend())lap=&it.value();}
    else lap=fixed?model_->playbackLapData(lapNum):model_->chartPrimaryLap(currentTime_);
    if(!lap)return;double elapsed=coordinate;if(distanceCoordinate){const double absolute=owner->timeAtDistance(lap,coordinate);elapsed=absolute-lap->startSessionTime;}if(!std::isfinite(elapsed))return;
    const int mapIndex=viewMode_->findData("map");preferredView_="map";saveSettings();if(mapIndex>=0){QSignalBlocker guard(viewMode_);viewMode_->setCurrentIndex(mapIndex);}applyState();map_->focusElapsed(elapsed);
}

QVector<int> AnalyzePage::requestedPlaybackLaps() const {
    QVector<int> result;
    if (!playback_) return result;
    if (fixedMode_->isChecked()) {
        if (!secondaryChoice(lapA_)&&selectedLap(lapA_)>0) result.push_back(selectedLap(lapA_));
        if (!secondaryChoice(lapB_)&&selectedLap(lapB_)>0&&!result.contains(selectedLap(lapB_)))
            result.push_back(selectedLap(lapB_));
    } else if (!secondaryChoice(compareLap_)&&selectedLap(compareLap_)>0) {
        result.push_back(selectedLap(compareLap_));
    }
    return result;
}
