#include "AnalyzePage.h"
#include "AnalyzeChart.h"
#include "ClearableComboBox.h"
#include "AnalyzeMapComparison.h"
#include "TyreHelpers.h"
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
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <array>
#include <cmath>

namespace {
QIcon analyzeIcon(QWidget* w,const char* name,QStyle::StandardPixmap fallback){return adaptThemeIcon(QIcon::fromTheme(QString::fromLatin1(name)),w->palette().color(QPalette::WindowText),w->style()->standardIcon(fallback));}
QPushButton* tinyButton(const QString& tip,const QIcon& icon,QWidget* parent){auto*b=new QPushButton(parent);b->setIcon(icon);b->setToolTip(tip);b->setFixedSize(24,24);b->setFlat(true);return b;}
struct AnalysisSelection {
    bool valid = false;
    bool secondary = false;
    int driverIndex = -1;
    int lapNum = -1;
};
QString driverKey(bool secondary, int driverIndex) {
    return QString("file%1:%2").arg(secondary ? 2 : 1).arg(driverIndex);
}
QString lapKey(bool secondary, int driverIndex, int lapNum) {
    return lapNum > 0
        ? QString("file%1:%2:%3").arg(secondary ? 2 : 1).arg(driverIndex).arg(lapNum)
        : QString();
}
AnalysisSelection parseSelection(const QString& key, bool requireLap) {
    const QStringList parts = key.split(':');
    if (parts.size() != (requireLap ? 3 : 2) ||
        (parts[0] != "file1" && parts[0] != "file2")) return {};
    bool driverOk = false;
    bool lapOk = !requireLap;
    const int driver = parts[1].toInt(&driverOk);
    const int lap = requireLap ? parts[2].toInt(&lapOk) : -1;
    return {driverOk && lapOk && (!requireLap || lap > 0), parts[0] == "file2",
            driver, lap};
}
AnalysisSelection selectedDriver(const QComboBox* box) {
    return box ? parseSelection(box->currentData().toString(), false) : AnalysisSelection{};
}
AnalysisSelection selectedLap(const QComboBox* box) {
    return box ? parseSelection(box->currentData().toString(), true) : AnalysisSelection{};
}
QString resolvedAnalyzeLabel(const QString& value, const QString& fallback) {
    const QString trimmed = value.trimmed();
    return trimmed.isEmpty() ? fallback : trimmed;
}
QString analyzeLapTime(int milliseconds) {
    if (milliseconds <= 0) return QStringLiteral("-");
    return QStringLiteral("%1:%2")
        .arg(milliseconds / 60000)
        .arg((milliseconds % 60000) / 1000.0, 6, 'f', 3, QChar('0'));
}
QString selectionSummary(const QString& driver, const LapBlock* lap) {
    QString compound;
    if (lap) {
        for (auto it = lap->sts.crbegin(); it != lap->sts.crend(); ++it) {
            if (it->tyre_compound <= 0) continue;
            compound = tyreLabel(it->tyre_compound);
            break;
        }
    }
    return QStringLiteral("%1  ·  %2  ·  %3  ·  %4")
        .arg(driver.isEmpty() ? QStringLiteral("-") : driver,
             compound.isEmpty() ? QStringLiteral("-") : compound,
             lap ? QStringLiteral("Lap %1").arg(lap->lapNum) : QStringLiteral("-"),
             lap ? analyzeLapTime(lap->lapTimeMs) : QStringLiteral("-"));
}

template<class T>
void mergeRows(QVector<T>& target,QVector<T>&& source){
    if(target.isEmpty()){target=std::move(source);return;}
    target+=std::move(source);
    std::stable_sort(target.begin(),target.end(),[](const T&a,const T&b){return a.t<b.t;});
    auto last=std::unique(target.begin(),target.end(),[](const T&a,const T&b){return qFuzzyCompare(a.t+1.0f,b.t+1.0f);});
    target.erase(last,target.end());
}
}

class AnalyzeSelectionGroup : public QFrame {
public:
    explicit AnalyzeSelectionGroup(const QString& title, QWidget* parent = nullptr)
        : QFrame(parent), title_(title) {
        setFrameShape(QFrame::StyledPanel);
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(0, 0, 0, 0);
        root->setSpacing(0);
        auto* header = new QWidget(this);
        headerLayout_ = new QHBoxLayout(header);
        headerLayout_->setContentsMargins(7, 3, 4, 3);
        headerLayout_->setSpacing(4);
        caption_ = new QLabel(title_, header);
        caption_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        QFont font = caption_->font();
        font.setBold(true);
        font.setPointSizeF(qMax(7.0, font.pointSizeF() - 1.0));
        caption_->setFont(font);
        headerLayout_->addWidget(caption_, 1);
        toggle_ = new QToolButton(header);
        toggle_->setAutoRaise(true);
        toggle_->setFixedSize(22, 22);
        headerLayout_->addWidget(toggle_);
        root->addWidget(header);

        body_ = new QWidget(this);
        fields_ = new QVBoxLayout(body_);
        fields_->setContentsMargins(7, 6, 7, 7);
        fields_->setSpacing(5);
        root->addWidget(body_);
        connect(toggle_, &QToolButton::clicked, this,
                [this] { setCollapsed(!collapsed_); });
        setCollapsed(false);
    }

    QVBoxLayout* fieldsLayout() const { return fields_; }
    void addHeaderWidget(QWidget* widget) {
        headerLayout_->insertWidget(headerLayout_->count() - 1, widget);
    }
    void setHeaderWidgetVisible(QWidget* widget, bool visible) {
        if (widget) widget->setVisible(visible);
    }
    void setSummary(const QString& summary) {
        summary_ = summary;
        if (collapsed_) caption_->setText(summary_);
        caption_->setToolTip(summary_);
    }

private:
    void setCollapsed(bool collapsed) {
        collapsed_ = collapsed;
        body_->setVisible(!collapsed_);
        caption_->setText(collapsed_ ? summary_ : title_);
        toggle_->setText(collapsed_ ? QStringLiteral("+") : QString::fromUtf8("\u2212"));
        toggle_->setToolTip(collapsed_ ? QStringLiteral("Expand %1").arg(title_)
                                      : QStringLiteral("Collapse %1").arg(title_));
        toggle_->setAccessibleName(toggle_->toolTip());
    }

    QString title_;
    QString summary_{QStringLiteral("-  ·  -  ·  -  ·  -")};
    bool collapsed_ = false;
    QHBoxLayout* headerLayout_ = nullptr;
    QLabel* caption_ = nullptr;
    QToolButton* toggle_ = nullptr;
    QWidget* body_ = nullptr;
    QVBoxLayout* fields_ = nullptr;
};

struct AnalyzePage::FileState {
    struct DriverState {
        int driverIndex = -1;
        QString driverName;
        bool isPlayer = false;
        SessionData data;
        QHash<int, LapBlock> cache;
        QHash<int, uint32_t> installedMasks;
        QHash<int, uint32_t> requestedMasks;

        void setCatalog(int index, const QString& name, bool player,
                        const std::vector<tnrp::LapBlockMeta>& blocks,
                        const std::vector<tnrp::LapMeta>& laps,
                        int fastestLapNum, int trackLengthM) {
            driverIndex = index;
            driverName = name.isEmpty() ? QStringLiteral("Car %1").arg(index) : name;
            isPlayer = player;
            data.clear();
            data.trimBuffers = false;
            data.fastestLapNum = fastestLapNum;
            data.trackLengthM = static_cast<float>(trackLengthM);
            QHash<int, int> times;
            for (const auto& lap : laps) times.insert(lap.lapNum, lap.lapTimeMs);
            data.laps.reserve(static_cast<qsizetype>(blocks.size()));
            for (const auto& source : blocks) {
                LapBlock lap;
                lap.lapNum = source.lapNum;
                lap.startSessionTime = source.startSessionTime;
                lap.endSessionTime = source.endSessionTime;
                lap.lapTimeMs = times.value(source.lapNum);
                lap.tel.reserve(static_cast<qsizetype>(source.telemetry.size()));
                for (const auto& point : source.telemetry)
                    lap.tel.push_back({point.session_time, static_cast<float>(point.speed_kph),
                                       point.rpm, qQNaN(), qQNaN(), qQNaN(), qQNaN()});
                lap.sts.reserve(static_cast<qsizetype>(source.statusHistory.size()));
                for (const auto& point : source.statusHistory)
                    lap.sts.push_back({point.session_time, static_cast<float>(point.ers_pct),
                                       0, 0, 0, 0, 0, point.tyre_compound,
                                       point.visual_compound, 0});
                data.laps.push_back(std::move(lap));
            }
            std::sort(data.laps.begin(), data.laps.end(),
                      [](const LapBlock& a, const LapBlock& b) {
                          return a.startSessionTime < b.startSessionTime;
                      });
        }
    };

    QHash<int, DriverState> drivers;
    QVector<int> driverOrder;
    QVector<QString> lru;
    QString filename;
    QString path;
    QString trackName;
    int trackId=-1;
    uint64_t generation=0;
    bool distanceAvailable=false;

    DriverState* driver(int index) {
        auto it = drivers.find(index);
        return it == drivers.end() ? nullptr : &it.value();
    }
    const DriverState* driver(int index) const {
        auto it = drivers.constFind(index);
        return it == drivers.cend() ? nullptr : &it.value();
    }

    void setCatalog(const tnrp::PlaybackLapBlocksRow& catalog) {
        drivers.clear();
        driverOrder.clear();
        lru.clear();
        distanceAvailable = catalog.lapDistanceAvailable || catalog.deltaAvailable;
        if (!catalog.analysisDrivers.empty()) {
            for (const auto& source : catalog.analysisDrivers) {
                DriverState driverState;
                driverState.setCatalog(
                    source.driverIndex, QString::fromStdString(source.driverName),
                    source.isPlayer, source.blocks, source.laps,
                    source.fastestLapNum, catalog.trackLengthM);
                driverOrder.push_back(source.driverIndex);
                drivers.insert(source.driverIndex, std::move(driverState));
            }
            return;
        }
        DriverState recorded;
        recorded.setCatalog(-1, QStringLiteral("Recorded driver"), true,
                            catalog.blocks, catalog.laps, catalog.fastestLapNum,
                            catalog.trackLengthM);
        driverOrder.push_back(-1);
        drivers.insert(-1, std::move(recorded));
    }
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
    viewMode_=new QComboBox(viewControl);viewMode_->addItem("Graphs","graph");viewMode_->addItem("Split","split");viewMode_->addItem("Map","map");viewMode_->setCurrentIndex(qMax(0,viewMode_->findData(preferredView_)));viewMode_->setFrame(false);viewMode_->setToolTip("Analysis view");
    viewLayout->addWidget(viewLabel);viewLayout->addWidget(viewMode_);toolbarLayout->addWidget(viewControl);

    secondaryFileRow_=new QWidget(toolbarControls_);auto*secondaryLayout=new QHBoxLayout(secondaryFileRow_);secondaryLayout->setContentsMargins(4,0,2,0);secondaryLayout->setSpacing(3);secondaryFileLabel_=new QLabel("No secondary file",secondaryFileRow_);secondaryFileLabel_->setMinimumWidth(0);secondaryFileLabel_->setMaximumWidth(110);secondaryFileLabel_->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);secondaryOpen_=tinyButton("Open Secondary File",analyzeIcon(secondaryFileRow_,"document-open",QStyle::SP_DialogOpenButton),secondaryFileRow_);secondaryClear_=tinyButton("Clear Secondary File",analyzeIcon(secondaryFileRow_,"edit-clear",QStyle::SP_DialogCloseButton),secondaryFileRow_);secondaryClear_->hide();secondaryLayout->addWidget(secondaryFileLabel_,1);secondaryLayout->addWidget(secondaryOpen_);secondaryLayout->addWidget(secondaryClear_);toolbarLayout->addWidget(secondaryFileRow_);

    deltaSummary_=new QWidget(toolbarControls_);auto*deltaLayout=new QHBoxLayout(deltaSummary_);deltaLayout->setContentsMargins(4,0,2,0);deltaLayout->setSpacing(7);
    QFont deltaFont=font();deltaFont.setStyleHint(QFont::Monospace);deltaFont.setFamilies({"Noto Sans Mono","monospace"});
    for(const QString&label:{QStringLiteral("S1"),QStringLiteral("S2"),QStringLiteral("S3"),QStringLiteral("Lap")}){auto*item=new QWidget(deltaSummary_);auto*itemLayout=new QHBoxLayout(item);itemLayout->setContentsMargins(0,0,0,0);itemLayout->setSpacing(3);auto*name=new QLabel(label,item);QFont nameFont=name->font();nameFont.setPointSizeF(qMax(7.0,nameFont.pointSizeF()-1));nameFont.setBold(true);name->setFont(nameFont);auto*value=new QLabel(QString::fromUtf8("—.---"),item);value->setFont(deltaFont);value->setMinimumWidth(value->fontMetrics().horizontalAdvance("+0.000"));itemLayout->addWidget(name);itemLayout->addWidget(value);deltaLayout->addWidget(item);deltaValues_.push_back(value);if(deltaSectorItems_.size()<3)deltaSectorItems_.push_back(item);}
    toolbarLayout->addWidget(deltaSummary_);
    helpButton_=new QToolButton(toolbarControls_);helpButton_->setAutoRaise(true);helpButton_->setIcon(analyzeIcon(this,"help-about",QStyle::SP_MessageBoxQuestion));helpButton_->setToolTip("Analysis controls help");toolbarLayout->addWidget(helpButton_);

    secondaryErrorLabel_=new QLabel;secondaryErrorLabel_->setContentsMargins(10,4,10,4);secondaryErrorLabel_->setWordWrap(true);QPalette errorPalette=secondaryErrorLabel_->palette();errorPalette.setColor(QPalette::WindowText,QColor("#d44252"));secondaryErrorLabel_->setPalette(errorPalette);secondaryErrorLabel_->hide();root->addWidget(secondaryErrorLabel_);

    contentSplitter_=new QSplitter(Qt::Horizontal,this);
    contentSplitter_->setChildrenCollapsible(false);
    contentSplitter_->setHandleWidth(1);

    viewContainer_=new QWidget(contentSplitter_);auto*views=new QHBoxLayout(viewContainer_);views->setContentsMargins(0,0,0,0);views->setSpacing(0);chart_=new AnalyzeChart(viewContainer_);chart_->setModel(model_);viewDivider_=new QFrame(viewContainer_);viewDivider_->setFixedWidth(1);viewDivider_->setStyleSheet("background: palette(mid);");map_=new AnalyzeMapComparison(viewContainer_);connect(map_,&AnalyzeMapComparison::cursorElapsedChanged,chart_,&AnalyzeChart::setMapCursorElapsed);views->addWidget(chart_,1);views->addWidget(viewDivider_);views->addWidget(map_,1);contentSplitter_->addWidget(viewContainer_);

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
    auto*comparisonPanel=new QGroupBox("Lap comparison",sidebar_);auto*comparisonLayout=new QVBoxLayout(comparisonPanel);comparisonLayout->setContentsMargins(6,7,6,7);comparisonLayout->setSpacing(5);
    fixedMode_=new QCheckBox("Fixed laps",comparisonPanel);fixedMode_->setToolTip("Compare two selected laps instead of following playback");fixedMode_->setEnabled(false);comparisonLayout->addWidget(fixedMode_);
    auto addField=[](AnalyzeSelectionGroup*group,const QString&label,QWidget*control){auto*w=new QWidget(group);auto*l=new QHBoxLayout(w);l->setContentsMargins(0,0,0,0);l->setSpacing(6);auto*lab=new QLabel(label,w);lab->setFixedWidth(46);control->setParent(w);l->addWidget(lab);l->addWidget(control,1);group->fieldsLayout()->addWidget(w);};
    auto driverControl=[&](AnalyzeSelectionGroup*group,QComboBox*&box){box=new QComboBox(group);box->setMinimumContentsLength(10);box->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);addField(group,"Driver",box);};
    auto lapControl=[&](AnalyzeSelectionGroup*group,ClearableComboBox*&box){box=new ClearableComboBox(group);box->setMinimumContentsLength(10);box->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);addField(group,"Lap",box);};
    auto labelControl=[&](AnalyzeSelectionGroup*group,QLineEdit*&edit,const QString&placeholder,const QString&value){edit=new QLineEdit(group);edit->setMaxLength(40);edit->setPlaceholderText(placeholder);edit->setText(value);addField(group,"Name",edit);};
    auto colorButton=[&](AnalyzeSelectionGroup*group,const QString&tip){auto*button=new QPushButton(group);button->setFixedSize(28,22);button->setToolTip(tip);button->setAccessibleName(tip);group->addHeaderWidget(button);return button;};

    currentGroup_=new AnalyzeSelectionGroup("Current",comparisonPanel);driverControl(currentGroup_,currentDriver_);currentDriver_->setEnabled(false);currentLapDisplay_=new QLabel(QString::fromUtf8("\u2014"),currentGroup_);addField(currentGroup_,"Lap",currentLapDisplay_);labelControl(currentGroup_,currentLabelEdit_,"Current",currentLabel_);mapCurrentColor_=colorButton(currentGroup_,"Current map color");comparisonLayout->addWidget(currentGroup_);
    compareGroup_=new AnalyzeSelectionGroup("Compare",comparisonPanel);driverControl(compareGroup_,compareDriver_);lapControl(compareGroup_,compareLap_);labelControl(compareGroup_,compareLabelEdit_,"Compare",compareLabel_);mapComparisonColor_=colorButton(compareGroup_,"Comparison map color");comparisonLayout->addWidget(compareGroup_);
    lapAGroup_=new AnalyzeSelectionGroup("Lap A",comparisonPanel);driverControl(lapAGroup_,lapADriver_);lapControl(lapAGroup_,lapA_);labelControl(lapAGroup_,lapALabelEdit_,"Lap A",lapALabel_);mapLapAColor_=colorButton(lapAGroup_,"Lap A map color");comparisonLayout->addWidget(lapAGroup_);
    lapBGroup_=new AnalyzeSelectionGroup("Lap B",comparisonPanel);driverControl(lapBGroup_,lapBDriver_);lapControl(lapBGroup_,lapB_);labelControl(lapBGroup_,lapBLabelEdit_,"Lap B",lapBLabel_);mapLapBColor_=colorButton(lapBGroup_,"Lap B map color");comparisonLayout->addWidget(lapBGroup_);side->addWidget(comparisonPanel);
    auto*metricLabel=new QLabel("Add metric");side->addWidget(metricLabel);
    addMetric_=new QComboBox;addMetric_->setEditable(true);addMetric_->setInsertPolicy(QComboBox::NoInsert);addMetric_->lineEdit()->setPlaceholderText("Choose a value…");side->addWidget(addMetric_);rebuildMetricPicker();
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
    rebuildSeriesList();refreshDriverSelectors();refreshLapSelectors();applyState();

    connect(addMetric_,qOverload<int>(&QComboBox::activated),this,[this](int i){QString id=addMetric_->itemData(i).toString();const auto*m=analyzeMetric(id);if(!m)return;series_.push_back({id,m->defaultColor,QColor(),true,true});saveSettings();rebuildSeriesList();rebuildMetricPicker();applyState();});
    connect(viewMode_,qOverload<int>(&QComboBox::activated),this,[this](int){preferredView_=viewMode_->currentData().toString();saveSettings();applyState();});
    connect(individualGraphs_,&QCheckBox::toggled,this,[this](bool){saveSettings();applyState();});
    connect(syncedTooltip_,&QCheckBox::toggled,this,[this](bool){saveSettings();applyState();});
    connect(sectorBoundaries_,&QCheckBox::toggled,this,[this](bool on){if(!on)sectorDelta_->setChecked(false);saveSettings();applyState();});
    connect(sectorDelta_,&QCheckBox::toggled,this,[this](bool){saveSettings();applyState();});
    connect(showYAxis_,&QCheckBox::toggled,this,[this](bool on){for(auto&s:series_)s.showYAxis=on;saveSettings();rebuildSeriesList();applyState();});
    connect(fixedMode_,&QCheckBox::toggled,this,[this](bool){applyState();});
    for(auto* b:{compareLap_,lapA_,lapB_})connect(b,qOverload<int>(&QComboBox::currentIndexChanged),this,[this](int){applyState();});
    connect(compareDriver_,qOverload<int>(&QComboBox::currentIndexChanged),this,[this](int){if(compareLap_->count())compareLap_->setCurrentIndex(0);refreshLapSelectors();});
    connect(lapADriver_,qOverload<int>(&QComboBox::currentIndexChanged),this,[this](int){if(lapA_->count())lapA_->setCurrentIndex(0);refreshLapSelectors();});
    connect(lapBDriver_,qOverload<int>(&QComboBox::currentIndexChanged),this,[this](int){if(lapB_->count())lapB_->setCurrentIndex(0);refreshLapSelectors();});
    auto setInspectorVisible=[this](bool visible){collapsed_=!visible;sidebar_->setVisible(visible);QSignalBlocker guard(inspectorButton_);inspectorButton_->setChecked(visible);saveSettings();};
    connect(inspectorButton_,&QToolButton::toggled,this,setInspectorVisible);
    connect(collapse_,&QPushButton::clicked,this,[setInspectorVisible]{setInspectorVisible(false);});
    connect(secondaryOpen_,&QPushButton::clicked,this,&AnalyzePage::loadSecondaryFile);
    connect(secondaryClear_,&QPushButton::clicked,this,[this]{clearSecondaryFile(true);});
    connect(helpButton_,&QToolButton::clicked,this,&AnalyzePage::showControlsHelp);
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
    for(auto*button:{mapCurrentColor_,mapLapAColor_})connect(button,&QPushButton::clicked,this,[chooseMapColor]{chooseMapColor(true);});
    for(auto*button:{mapComparisonColor_,mapLapBColor_})connect(button,&QPushButton::clicked,this,[chooseMapColor]{chooseMapColor(false);});
    connect(currentLabelEdit_,&QLineEdit::textChanged,this,[this](const QString&value){currentLabel_=value.left(40);saveSettings();applyState();});
    connect(compareLabelEdit_,&QLineEdit::textChanged,this,[this](const QString&value){compareLabel_=value.left(40);saveSettings();applyState();});
    connect(lapALabelEdit_,&QLineEdit::textChanged,this,[this](const QString&value){lapALabel_=value.left(40);saveSettings();applyState();});
    connect(lapBLabelEdit_,&QLineEdit::textChanged,this,[this](const QString&value){lapBLabel_=value.left(40);saveSettings();applyState();});
    connect(chart_,&ChartView::inspectionRequested,this,&AnalyzePage::inspectMap);
    connect(model_,&SessionModel::lapsChanged,this,&AnalyzePage::refreshLapSelectors);
    connect(model_,&SessionModel::chartConfigurationChanged,this,&AnalyzePage::applyState);
    connect(seriesList_->model(),&QAbstractItemModel::rowsMoved,this,[this]{QVector<AnalyzeSeriesSetting> next;for(int i=0;i<seriesList_->count();++i){QString id=seriesList_->item(i)->data(Qt::UserRole).toString();auto it=std::find_if(series_.cbegin(),series_.cend(),[&](const auto&s){return s.metricId==id;});if(it!=series_.cend())next<<*it;}series_=next;saveSettings();applyState();QTimer::singleShot(0,this,&AnalyzePage::rebuildSeriesList);});
}

AnalyzePage::~AnalyzePage()=default;

void AnalyzePage::setPrimaryRecording(int trackId,const QString& trackName){
    primaryTrackId_=trackId;
    primaryTrackName_=trackName;
    currentPrimaryDriverIndex_=-1;
    ++primaryGeneration_;
    primaryCatalogReady_=false;
    primary_=std::make_unique<FileState>();
    primary_->trackId=trackId;
    primary_->trackName=trackName;
    primary_->generation=primaryGeneration_;
    refreshDriverSelectors();
    refreshLapSelectors();
}

void AnalyzePage::setPrimaryCatalog(const tnrp::PlaybackLapBlocksRow& catalog){
    if(!primary_)primary_=std::make_unique<FileState>();
    if(!primaryCatalogReady_){
        primary_->setCatalog(catalog);
        primaryCatalogReady_=true;
    }
    int current=catalog.playbackDriverIndex;
    if(!primary_->driver(current)){
        for(int index:primary_->driverOrder){
            const auto*driver=primary_->driver(index);
            if(driver&&driver->isPlayer){current=index;break;}
        }
    }
    if(!primary_->driver(current)&&!primary_->driverOrder.isEmpty())
        current=primary_->driverOrder.first();
    currentPrimaryDriverIndex_=current;
    refreshDriverSelectors();
    refreshLapSelectors();
    applyState();
}

void AnalyzePage::loadSettings(){
    collapsed_=settings_.value("analyze/collapsed",false).toBool();preferredView_=settings_.value("analyze/view","graph").toString();if(preferredView_!="graph"&&preferredView_!="split"&&preferredView_!="map")preferredView_="graph";mapCurrent_=QColor(settings_.value("analyze/mapCurrentColor","#5794F2").toString());mapComparison_=QColor(settings_.value("analyze/mapComparisonColor","#C4162A").toString());
    auto loadLabel=[this](const char*key,const QString&legacyDefault){QString value=settings_.value(QString::fromLatin1(key)).toString().left(40);return value==legacyDefault?QString():value;};
    currentLabel_=loadLabel("analyze/currentLabel","Current");compareLabel_=loadLabel("analyze/compareLabel","Compare");lapALabel_=loadLabel("analyze/lapALabel","Lap A");lapBLabel_=loadLabel("analyze/lapBLabel","Lap B");
    const bool defaultAxis=settings_.value("analyze/showYAxis",true).toBool();QJsonDocument doc=QJsonDocument::fromJson(settings_.value("analyze/series").toByteArray());QSet<QString>seen;if(doc.isArray())for(const auto&v:doc.array()){const auto o=v.toObject();const QString id=o["metricId"].toString();if(seen.contains(id))continue;if(id=="delta"){QColor positive(o["color"].toString()),negative(o["negativeColor"].toString());series_<<AnalyzeSeriesSetting{id,positive.isValid()?positive:QColor("#C4162A"),negative.isValid()?negative:QColor("#37872D"),o["visible"].toBool(true),o.contains("showYAxis")?o["showYAxis"].toBool():defaultAxis};seen.insert(id);continue;}const auto*m=analyzeMetric(id);QColor color(o["color"].toString());if(!m)continue;series_<<AnalyzeSeriesSetting{id,color.isValid()?color:m->defaultColor,QColor(),o["visible"].toBool(true),o.contains("showYAxis")?o["showYAxis"].toBool():defaultAxis};seen.insert(id);}if(!doc.isArray())for(const char*id:{"speed","rpm","ers"}){const auto*m=analyzeMetric(id);series_<<AnalyzeSeriesSetting{m->id,m->defaultColor,QColor(),true,defaultAxis};}if(!seen.contains("delta"))series_<<AnalyzeSeriesSetting{"delta",QColor("#C4162A"),QColor("#37872D"),true,defaultAxis};
}
void AnalyzePage::saveSettings(){settings_.setValue("analyze/version",9);settings_.setValue("analyze/collapsed",collapsed_);settings_.setValue("analyze/showYAxis",showYAxis_?showYAxis_->isChecked():true);settings_.setValue("analyze/view",preferredView_);settings_.setValue("analyze/currentLabel",currentLabel_.left(40));settings_.setValue("analyze/compareLabel",compareLabel_.left(40));settings_.setValue("analyze/lapALabel",lapALabel_.left(40));settings_.setValue("analyze/lapBLabel",lapBLabel_.left(40));if(individualGraphs_)settings_.setValue("analyze/individualGraphs",individualGraphs_->isChecked());if(syncedTooltip_)settings_.setValue("analyze/syncedTooltip",syncedTooltip_->isChecked());if(sectorBoundaries_)settings_.setValue("analyze/sectorBoundaries",sectorBoundaries_->isChecked());if(sectorDelta_)settings_.setValue("analyze/sectorDelta",sectorDelta_->isChecked());settings_.setValue("analyze/mapCurrentColor",mapCurrent_.name());settings_.setValue("analyze/mapComparisonColor",mapComparison_.name());QJsonArray a;for(const auto&s:series_){QJsonObject o;o["metricId"]=s.metricId;o["color"]=s.color.name();if(s.metricId=="delta")o["negativeColor"]=s.negativeColor.name();o["visible"]=s.visible;o["showYAxis"]=s.showYAxis;a.append(o);}settings_.setValue("analyze/series",QJsonDocument(a).toJson(QJsonDocument::Compact));}

void AnalyzePage::rebuildMetricPicker(){if(!addMetric_)return;QSignalBlocker b(addMetric_);addMetric_->clear();QSet<QString> used;for(const auto&s:series_)used.insert(s.metricId);for(const auto&m:analyzeMetrics())if(!used.contains(m.id))addMetric_->addItem(QString("%1 · %2").arg(m.group,m.label),m.id);addMetric_->setCurrentIndex(-1);if(addMetric_->completer()){addMetric_->completer()->setCaseSensitivity(Qt::CaseInsensitive);addMetric_->completer()->setFilterMode(Qt::MatchContains);}}

void AnalyzePage::rebuildSeriesList(){seriesList_->clear();deltaStatus_=nullptr;for(int i=0;i<series_.size();++i){const QString id=series_[i].metricId;const auto*m=analyzeMetric(id);const bool delta=id=="delta";if(!delta&&!m)continue;auto*item=new QListWidgetItem;item->setData(Qt::UserRole,id);item->setSizeHint(QSize(260,40));seriesList_->addItem(item);auto*w=new QWidget;auto*l=new QHBoxLayout(w);l->setContentsMargins(2,1,2,1);l->setSpacing(3);auto swatch=[&](QColor c,const QString&tip){auto*b=new QPushButton(w);b->setToolTip(tip);b->setFixedSize(delta?18:24,24);b->setFlat(true);b->setStyleSheet("background:"+c.name()+";border:1px solid palette(mid);border-radius:3px;");return b;};auto*positive=swatch(series_[i].color,delta?"Positive delta color":m->label+" color");QPushButton*negative=delta?swatch(series_[i].negativeColor,"Negative delta color"):nullptr;auto*label=new QLabel(QString("%1\n%2").arg(delta?"Delta":m->label,delta?"Time · + / −":m->group+(m->unit.isEmpty()?QString():" · "+m->unit)));label->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Preferred);if(delta)deltaStatus_=label;auto*up=tinyButton("Move up",analyzeIcon(w,"go-up",QStyle::SP_ArrowUp),w);auto*down=tinyButton("Move down",analyzeIcon(w,"go-down",QStyle::SP_ArrowDown),w);auto*axis=new QPushButton("Y",w);axis->setToolTip(series_[i].showYAxis?"Hide Y-axis":"Show Y-axis");axis->setCheckable(true);axis->setChecked(series_[i].showYAxis);axis->setFixedSize(24,24);axis->setFlat(true);auto*eye=tinyButton(series_[i].visible?"Hide series":"Show series",analyzeIcon(w,series_[i].visible?"view-visible":"view-hidden",QStyle::SP_FileDialogInfoView),w);auto*reset=tinyButton("Reset color",analyzeIcon(w,"view-refresh",QStyle::SP_BrowserReload),w);QPushButton*remove=delta?nullptr:tinyButton("Remove metric",analyzeIcon(w,"edit-delete",QStyle::SP_TrashIcon),w);l->addWidget(positive);if(negative)l->addWidget(negative);l->addWidget(label,1);for(auto*b:{up,down,axis,eye,reset})l->addWidget(b);if(remove)l->addWidget(remove);seriesList_->setItemWidget(item,w);up->setEnabled(i>0);down->setEnabled(i+1<series_.size());
        auto choose=[this,id](bool negative){auto it=std::find_if(series_.begin(),series_.end(),[&](auto&s){return s.metricId==id;});if(it==series_.end())return;QColor&target=negative?it->negativeColor:it->color;QColor c=QColorDialog::getColor(target,this,"Select series colour",QColorDialog::DontUseNativeDialog);if(c.isValid()){target=c;saveSettings();rebuildSeriesList();applyState();}};connect(positive,&QPushButton::clicked,this,[choose]{choose(false);});if(negative)connect(negative,&QPushButton::clicked,this,[choose]{choose(true);});connect(up,&QPushButton::clicked,this,[this,i]{moveSeries(i,i-1);});connect(down,&QPushButton::clicked,this,[this,i]{moveSeries(i,i+1);});connect(axis,&QPushButton::clicked,this,[this,id]{for(auto&s:series_)if(s.metricId==id)s.showYAxis=!s.showYAxis;QSignalBlocker guard(showYAxis_);showYAxis_->setChecked(std::all_of(series_.cbegin(),series_.cend(),[](const auto&s){return s.showYAxis;}));saveSettings();rebuildSeriesList();applyState();});connect(eye,&QPushButton::clicked,this,[this,id]{for(auto&s:series_)if(s.metricId==id)s.visible=!s.visible;saveSettings();rebuildSeriesList();applyState();});connect(reset,&QPushButton::clicked,this,[this,id]{for(auto&s:series_)if(s.metricId==id){if(id=="delta"){s.color=QColor("#C4162A");s.negativeColor=QColor("#37872D");}else s.color=analyzeMetric(id)->defaultColor;}saveSettings();rebuildSeriesList();applyState();});if(remove)connect(remove,&QPushButton::clicked,this,[this,id]{series_.erase(std::remove_if(series_.begin(),series_.end(),[&](const auto&s){return s.metricId==id;}),series_.end());saveSettings();rebuildSeriesList();rebuildMetricPicker();applyState();});}}
void AnalyzePage::moveSeries(int from,int to){if(from<0||to<0||from>=series_.size()||to>=series_.size())return;series_.move(from,to);saveSettings();rebuildSeriesList();applyState();}

void AnalyzePage::refreshDriverSelectors() {
    if (!currentDriver_ || !compareDriver_ || !lapADriver_ || !lapBDriver_) return;
    const QString currentKey = primary_ && primary_->driver(currentPrimaryDriverIndex_)
        ? driverKey(false, currentPrimaryDriverIndex_) : QString();
    {
        QSignalBlocker guard(currentDriver_);
        currentDriver_->clear();
        if (primary_) {
            if (const auto* driver = primary_->driver(currentPrimaryDriverIndex_))
                currentDriver_->addItem(driver->driverName, currentKey);
        }
    }
    auto fill = [&](QComboBox* box) {
        QSignalBlocker guard(box);
        const QString old = box->currentData().toString();
        box->clear();
        auto addFile = [&](const FileState* file, bool second, const QString& title) {
            if (!file || file->driverOrder.isEmpty()) return;
            box->addItem(title, QString());
            if (auto* model = qobject_cast<QStandardItemModel*>(box->model()))
                if (auto* item = model->item(box->count() - 1)) item->setFlags(Qt::NoItemFlags);
            for (int index : file->driverOrder) {
                const auto* driver = file->driver(index);
                if (driver) box->addItem(driver->driverName, driverKey(second, index));
            }
        };
        addFile(primary_.get(), false, "Primary File");
        addFile(secondary_.get(), true, "Secondary File");
        int selected = box->findData(old);
        if (selected < 0) selected = box->findData(currentKey);
        box->setCurrentIndex(selected);
    };
    fill(compareDriver_);
    fill(lapADriver_);
    fill(lapBDriver_);
}

void AnalyzePage::refreshLapSelectors() {
    auto fill = [&](ClearableComboBox* box, QComboBox* driverBox) {
        QSignalBlocker guard(box);
        const QString old = box->currentData().toString();
        box->clear();
        box->addItem("No option selected", QString());
        const AnalysisSelection selection = selectedDriver(driverBox);
        const FileState* file = selection.secondary ? secondary_.get() : primary_.get();
        const FileState::DriverState* driver = file ? file->driver(selection.driverIndex) : nullptr;
        if (driver) {
            const QString title = selection.secondary
                ? driver->driverName
                : QStringLiteral("Primary File · %1").arg(driver->driverName);
            box->addItem(title, QString());
            if (auto* model = qobject_cast<QStandardItemModel*>(box->model()))
                if (auto* item = model->item(box->count() - 1)) item->setFlags(Qt::NoItemFlags);
            for (const LapBlock& lap : driver->data.laps) {
                QString compound;
                QColor compoundColor;
                for (auto it = lap.sts.crbegin(); it != lap.sts.crend(); ++it) {
                    if (it->tyre_compound <= 0) continue;
                    compound = tyreLabel(it->tyre_compound);
                    compoundColor = tyreTextColor(it->tyre_compound, it->visual_compound);
                    break;
                }
                QString lapTime;
                if (lap.lapTimeMs > 0)
                    lapTime = QString(" · %1:%2").arg(lap.lapTimeMs / 60000)
                        .arg((lap.lapTimeMs % 60000) / 1000.0, 6, 'f', 3, QChar('0'));
                const QString text = QString("%1%2Lap %3%4%5")
                    .arg(compound, compound.isEmpty() ? "" : " · ")
                    .arg(lap.lapNum).arg(lapTime)
                    .arg(lap.lapNum == driver->data.fastestLapNum ? " · FL" : "");
                box->addItem(text, lapKey(selection.secondary, selection.driverIndex,
                                           lap.lapNum));
                if (compoundColor.isValid())
                    box->setItemData(box->count() - 1, compoundColor, Qt::ForegroundRole);
            }
        }
        const int idx = box->findData(old);
        box->setCurrentIndex(idx >= 0 ? idx : 0);
    };
    fill(compareLap_, compareDriver_);
    fill(lapA_, lapADriver_);
    fill(lapB_, lapBDriver_);
    applyState();
}

void AnalyzePage::applyState(){
    const bool fixed=playback_&&fixedMode_->isChecked();
    const AnalysisSelection compareSelection=selectedLap(compareLap_);
    const AnalysisSelection aSelection=selectedLap(lapA_);
    const AnalysisSelection bSelection=selectedLap(lapB_);
    auto fileFor=[&](const AnalysisSelection&selection)->const FileState*{
        return selection.secondary?secondary_.get():primary_.get();
    };
    auto distanceFor=[&](const AnalysisSelection&selection){
        const FileState*file=fileFor(selection);return !selection.valid||(file&&file->distanceAvailable);
    };
    const bool distance=model_->lapCoordinatesAvailable()&&
        (fixed?(distanceFor(aSelection)&&distanceFor(bSelection)):distanceFor(compareSelection));
    const bool mismatch=circuitsMismatched();
    if(mismatch&&preferredView_!="graph"){
        preferredView_="graph";
        saveSettings();
    }
    const bool mapAllowed=playback_&&!mismatch;
    QString activeView=viewMode_->currentData().toString();
    if(!mapAllowed&&activeView!="graph"){
        QSignalBlocker guard(viewMode_);
        viewMode_->setCurrentIndex(viewMode_->findData("graph"));
        activeView="graph";
    }
    const bool showMap=mapAllowed&&activeView!="graph";
    const bool showChart=activeView!="map";
    const bool split=showChart&&showMap;

    if(auto*menu=qobject_cast<QStandardItemModel*>(viewMode_->model())){
        if(auto*splitItem=menu->item(viewMode_->findData("split")))splitItem->setEnabled(mapAllowed);
        if(auto*mapItem=menu->item(viewMode_->findData("map")))mapItem->setEnabled(mapAllowed);
    }
    fixedMode_->setEnabled(playback_&&!model_->data().laps.isEmpty());
    secondaryFileRow_->setVisible(playback_);secondaryErrorLabel_->setVisible(playback_&&!secondaryErrorLabel_->text().isEmpty());
    currentGroup_->setVisible(!fixed);compareGroup_->setVisible(!fixed);lapAGroup_->setVisible(fixed);lapBGroup_->setVisible(fixed);
    const int driverCount=(primary_?primary_->driverOrder.size():0)+(secondary_?secondary_->driverOrder.size():0);
    compareDriver_->setEnabled(playback_&&driverCount>1);lapADriver_->setEnabled(playback_&&driverCount>1);lapBDriver_->setEnabled(playback_&&driverCount>1);compareLap_->setEnabled(playback_);
    compareLap_->setClearVisible(compareSelection.valid);lapA_->setClearVisible(aSelection.valid);lapB_->setClearVisible(bSelection.valid);
    individualGraphs_->setEnabled(true);syncedTooltip_->setEnabled(individualGraphs_->isChecked());sectorBoundaries_->setEnabled(!mismatch);sectorDelta_->setEnabled(!mismatch&&sectorBoundaries_->isChecked());
    for(auto*button:{mapCurrentColor_,mapLapAColor_}){button->setVisible(showMap);button->setStyleSheet("background:"+mapCurrent_.name()+";border:1px solid palette(mid);border-radius:3px;");}
    for(auto*button:{mapComparisonColor_,mapLapBColor_}){button->setVisible(showMap);button->setStyleSheet("background:"+mapComparison_.name()+";border:1px solid palette(mid);border-radius:3px;");}
    const bool deltaUnsupported=playback_&&!distance;
    if(deltaStatus_)deltaStatus_->setText(deltaUnsupported?"Delta\nNot supported in this file.":"Delta\nTime · + / −");
    for(int row=0;row<seriesList_->count();++row)if(seriesList_->item(row)->data(Qt::UserRole).toString()=="delta"){
        if(QWidget*card=seriesList_->itemWidget(seriesList_->item(row))){const auto buttons=card->findChildren<QPushButton*>(QString(),Qt::FindDirectChildrenOnly);for(int i=0;i<buttons.size();++i){if(i<2)buttons[i]->setEnabled(!deltaUnsupported);else buttons[i]->setVisible(!deltaUnsupported);}}
        break;
    }

    struct ResolvedLap{const SessionData*data=nullptr;const LapBlock*lap=nullptr;int trackId=-1;};
    auto resolve=[&](const AnalysisSelection&selection){
        ResolvedLap out;if(!selection.valid)return out;const FileState*file=fileFor(selection);if(!file)return out;const auto*driver=file->driver(selection.driverIndex);if(!driver)return out;out.data=&driver->data;out.trackId=file->trackId;auto it=driver->cache.constFind(selection.lapNum);if(it!=driver->cache.cend())out.lap=&it.value();return out;
    };
    ResolvedLap current,comparison;
    if(fixed){current=resolve(aSelection);comparison=resolve(bSelection);}else{current={&model_->data(),model_->chartPrimaryLap(currentTime_),primaryTrackId_};comparison=resolve(compareSelection);}

    auto catalogLap=[&](const AnalysisSelection&selection)->const LapBlock*{
        const FileState*file=fileFor(selection);const auto*driver=file&&selection.valid?file->driver(selection.driverIndex):nullptr;return driver?driver->data.lapByNum(selection.lapNum):nullptr;
    };
    const LapBlock*aSummary=current.lap?current.lap:catalogLap(aSelection);
    const LapBlock*bSummary=comparison.lap?comparison.lap:catalogLap(bSelection);
    const LapBlock*compareSummary=comparison.lap?comparison.lap:catalogLap(compareSelection);
    currentGroup_->setSummary(selectionSummary(currentDriver_->currentText(),fixed?aSummary:current.lap));
    compareGroup_->setSummary(selectionSummary(compareDriver_->currentText(),compareSummary));
    lapAGroup_->setSummary(selectionSummary(lapADriver_->currentText(),aSummary));
    lapBGroup_->setSummary(selectionSummary(lapBDriver_->currentText(),bSummary));
    currentLapDisplay_->setText(current.lap?QStringLiteral("Lap %1  ·  %2").arg(current.lap->lapNum).arg(analyzeLapTime(current.lap->lapTimeMs)):QString::fromUtf8("\u2014"));

    const QString primaryLabel=fixed?resolvedAnalyzeLabel(lapALabel_,"Lap A"):resolvedAnalyzeLabel(currentLabel_,"Current");
    const QString comparisonLabel=fixed?resolvedAnalyzeLabel(lapBLabel_,"Lap B"):resolvedAnalyzeLabel(compareLabel_,"Compare");

    chart_->setConfig(series_,showYAxis_->isChecked());
    chart_->setLabels(primaryLabel,comparisonLabel);
    chart_->setDistanceMode(distance);chart_->setIndividualGraphs(individualGraphs_->isChecked(),syncedTooltip_->isChecked());chart_->setSectorOptions(!mismatch&&sectorBoundaries_->isChecked(),!mismatch&&sectorDelta_->isChecked());
    chart_->setSelectedLaps(fixed,current.data,current.lap,comparison.data,comparison.lap);
    chart_->setMapCursors(split&&fixed,mapCurrent_,mapComparison_);
    const bool compatible=!current.lap||!comparison.lap||current.trackId<0||comparison.trackId<0||current.trackId==comparison.trackId;
    map_->setColors(mapCurrent_,mapComparison_);map_->setLabels(primaryLabel,comparisonLabel);map_->setLaps(current.lap,comparison.lap,fixed,current.trackId>=0?current.trackId:comparison.trackId,compatible);map_->setCurrentTime(currentTime_);
    chart_->setVisible(showChart);viewDivider_->setVisible(split);map_->setVisible(showMap);
    summaryPrimaryData_=current.data;summaryPrimary_=current.lap;summaryComparisonData_=comparison.data;summaryComparison_=comparison.lap;summaryFixed_=fixed;summaryCompatible_=compatible;refreshDeltaSummary();
    emit navigationEnabledChanged(showChart&&fixed&&aSelection.valid);emit dataRequirementsChanged();requestAnalysisLaps();
}

bool AnalyzePage::circuitsMismatched() const {
    return primary_&&secondary_&&primary_->trackId>=0&&secondary_->trackId>=0&&
        primary_->trackId!=secondary_->trackId;
}

void AnalyzePage::refreshDeltaSummary(){
    if(deltaValues_.size()!=4)return;
    const bool showSectors=!circuitsMismatched()&&summaryCompatible_;
    for(QWidget*item:deltaSectorItems_)item->setVisible(showSectors);

    std::array<double,4>values{qQNaN(),qQNaN(),qQNaN(),qQNaN()};
    const LapBlock*current=summaryPrimary_;
    const LapBlock*comparison=summaryComparison_;
    if(current&&comparison&&summaryPrimaryData_&&!current->progress.isEmpty()&&!comparison->progress.isEmpty()){
        const double currentEnd=current->progress.last().distanceM;
        const double comparisonEnd=comparison->progress.last().distanceM;
        const double fullDistance=qMin(currentEnd,comparisonEnd);
        double visibleDistance=fullDistance;
        if(!summaryFixed_){
            if(currentTime_<current->progress.first().t||currentTime_>current->progress.last().t)
                visibleDistance=qQNaN();
            else
                visibleDistance=summaryPrimaryData_->distanceAtTime(current,currentTime_);
        }
        visibleDistance=qMin(visibleDistance,fullDistance);
        auto cumulative=[&](double distance){
            if(!std::isfinite(distance)||distance<0)return qQNaN();
            const double currentTime=summaryPrimaryData_->timeAtDistance(current,distance)-current->startSessionTime;
            const double comparisonTime=summaryComparisonData_
                ? summaryComparisonData_->timeAtDistance(comparison,distance)-comparison->startSessionTime
                : qQNaN();
            return std::isfinite(currentTime)&&std::isfinite(comparisonTime)
                ? currentTime-comparisonTime:qQNaN();
        };
        if(std::isfinite(visibleDistance)&&visibleDistance>=0){
            values[3]=cumulative(visibleDistance);
            std::array<double,2>splits{qQNaN(),qQNaN()};
            auto mergeSplits=[&](const SessionData*owner,const LapBlock*lap){
                if(!owner||!lap)return;
                for(const auto&split:owner->sectorSplits(lap)){
                    const int index=split.sector-1;
                    if(index>=0&&index<2)splits[index]=split.distanceM;
                }
            };
            mergeSplits(summaryComparisonData_,comparison);
            mergeSplits(summaryPrimaryData_,current);
            if(showSectors&&std::isfinite(splits[0])){
                if(visibleDistance>=0)
                    values[0]=cumulative(qMin(visibleDistance,splits[0]));
                if(visibleDistance>=splits[0]&&std::isfinite(splits[1])){
                    const double atStart=cumulative(splits[0]);
                    const double atEnd=cumulative(qMin(visibleDistance,splits[1]));
                    if(std::isfinite(atStart)&&std::isfinite(atEnd))values[1]=atEnd-atStart;
                }
                if(std::isfinite(splits[1])&&visibleDistance>=splits[1]){
                    const double atStart=cumulative(splits[1]);
                    const double atEnd=cumulative(visibleDistance);
                    if(std::isfinite(atStart)&&std::isfinite(atEnd))values[2]=atEnd-atStart;
                }
                if(sectorDelta_->isChecked()){
                    double total=0;bool any=false;
                    for(int i=0;i<3;++i)if(std::isfinite(values[i])){total+=values[i];any=true;}
                    values[3]=any?total:qQNaN();
                }
            }
        }
    }

    QColor positive("#C4162A"),negative("#37872D");
    const auto delta=std::find_if(series_.cbegin(),series_.cend(),[](const auto&setting){return setting.metricId=="delta";});
    if(delta!=series_.cend()){if(delta->color.isValid())positive=delta->color;if(delta->negativeColor.isValid())negative=delta->negativeColor;}
    for(int i=0;i<deltaValues_.size();++i){
        QLabel*label=deltaValues_[i];const double value=values[i];
        if(!std::isfinite(value)){
            label->setText(QString::fromUtf8("—.---"));
            label->setStyleSheet("color: palette(mid);");
            continue;
        }
        const double normalized=std::abs(value)<0.0005?0.0:value;
        label->setText((normalized>0?QStringLiteral("+"):QString())+QString::number(normalized,'f',3));
        label->setStyleSheet(QString("color:%1;").arg(
            normalized>0?positive.name():normalized<0?negative.name():palette().color(QPalette::Mid).name()));
    }
    deltaSummary_->setToolTip(sectorDelta_->isChecked()
        ? "Sector Delta mode · Lap is the sum of the visible sector deltas"
        : "Lap delta at the playback cursor or selected-lap finish");
}

void AnalyzePage::showControlsHelp(){
    QMessageBox help(QMessageBox::Information,"Analysis Controls",QString(),QMessageBox::Close,this);
    help.setTextFormat(Qt::RichText);
    help.setText(
        "<b>Views</b><br>"
        "<b>Graphs</b> shows telemetry plots. <b>Split</b> keeps Graphs and Map "
        "side by side. <b>Map</b> shows the lap comparison map.<br><br>"
        "<b>Display</b><br>"
        "<b>Individual graphs</b> gives each metric its own panel. "
        "<b>Sync tooltips</b> aligns their hover readouts. "
        "<b>Sector boundaries</b> labels S1/S2/S3, and <b>Sector delta</b> "
        "resets delta at each sector. <b>Fixed laps</b> compares two selections.<br><br>"
        "<b>Chart interactions</b><br>"
        "Ctrl/Command + wheel zooms; wheel or click-drag pans. Double left-click "
        "resets the view. Double right-click focuses that point on the map and "
        "opens Split view when needed.");
    help.exec();
}
void AnalyzePage::setPlaybackMode(bool on,float t){playback_=on;currentTime_=t;if(on){const int wanted=viewMode_->findData(preferredView_);if(wanted>=0){QSignalBlocker guard(viewMode_);viewMode_->setCurrentIndex(wanted);}}chart_->setPlaybackMode(on);chart_->setCurrentTime(t);map_->setCurrentTime(t);if(!on){clearSecondaryFile(true);primary_.reset();primaryCatalogReady_=false;currentPrimaryDriverIndex_=-1;primaryTrackId_=-1;primaryTrackName_.clear();refreshDriverSelectors();refreshLapSelectors();resetPlaybackSelections();}fixedMode_->setEnabled(on&&!model_->data().laps.isEmpty());applyState();}
void AnalyzePage::setCurrentTime(float t){currentTime_=t;chart_->setCurrentTime(t);map_->setCurrentTime(t);refreshDeltaSummary();}
void AnalyzePage::setMapAppearance(bool sectorColors,int opacityPercent){map_->setMapAppearance(sectorColors,opacityPercent);}
void AnalyzePage::resetPlaybackSelections(){fixedMode_->setChecked(false);refreshDriverSelectors();for(auto*box:{compareLap_,lapA_,lapB_})if(box->count())box->setCurrentIndex(0);refreshLapSelectors();applyState();}
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
    auto clearSecondarySelection=[](QComboBox*driverBox,QComboBox*lapBox){
        if(selectedDriver(driverBox).secondary){
            QSignalBlocker driverGuard(driverBox);
            QSignalBlocker lapGuard(lapBox);
            driverBox->setCurrentIndex(-1);
            lapBox->setCurrentIndex(0);
        }
    };
    clearSecondarySelection(compareDriver_,compareLap_);
    clearSecondarySelection(lapADriver_,lapA_);
    clearSecondarySelection(lapBDriver_,lapB_);
    auto state=std::make_unique<FileState>();
    state->filename=catalog->filename;
    state->path=catalog->path;
    state->trackName=catalog->trackName;
    state->trackId=catalog->trackId;
    state->generation=catalog->generation;
    state->setCatalog(catalog->laps);
    secondary_=std::move(state);
    secondaryFileLabel_->setText(secondary_->filename);
    secondaryFileLabel_->setToolTip(catalog->path);
    secondaryOpen_->setToolTip("Replace Secondary File");
    secondaryClear_->show();
    secondaryErrorLabel_->clear();
    secondaryErrorLabel_->hide();
    refreshDriverSelectors();
    refreshLapSelectors();
}

void AnalyzePage::clearSecondaryFile(bool clearFixedSelections){
    auto clearSelection=[](QComboBox*driverBox,QComboBox*lapBox){
        if(!selectedDriver(driverBox).secondary)return;
        QSignalBlocker driverGuard(driverBox);
        QSignalBlocker lapGuard(lapBox);
        driverBox->setCurrentIndex(-1);
        lapBox->setCurrentIndex(0);
    };
    clearSelection(compareDriver_,compareLap_);
    clearSelection(lapADriver_,lapA_);
    clearSelection(lapBDriver_,lapB_);
    secondary_.reset();
    secondaryReader_->close();
    secondaryLoading_=false;
    secondaryFileLabel_->setText("No secondary file");
    secondaryFileLabel_->setToolTip({});
    secondaryOpen_->setEnabled(playback_);
    secondaryOpen_->setToolTip("Open Secondary File");
    secondaryClear_->setEnabled(true);
    secondaryClear_->hide();
    if(clearFixedSelections){secondaryErrorLabel_->clear();secondaryErrorLabel_->hide();}
    refreshDriverSelectors();
    refreshLapSelectors();
}

void AnalyzePage::installAnalysisLap(FileState&file,int driverIndex,int lapNum,uint32_t rowTypeMask,const std::shared_ptr<PlaybackHistoryBatch>&batch){
    auto*driver=file.driver(driverIndex);
    if(!driver)return;
    driver->requestedMasks[lapNum]&=~rowTypeMask;
    if(!batch||batch->lapDetails.isEmpty())return;
    const uint32_t payloadMask=batch->rowTypeMask;
    LapBlock detail=batch->lapDetails.first();
    auto it=driver->cache.find(lapNum);
    if(it==driver->cache.end()){
        LapBlock base;
        if(const LapBlock*meta=driver->data.lapByNum(lapNum))base=*meta;
        else{base.lapNum=lapNum;base.startSessionTime=detail.startSessionTime;base.endSessionTime=detail.endSessionTime;}
        it=driver->cache.insert(lapNum,std::move(base));
    }
    LapBlock&cached=it.value();
    uint32_t&installed=driver->installedMasks[lapNum];
    auto install=[&](uint32_t bit,auto&target,auto&source){
        if(!(installed&bit))target=std::move(source);else mergeRows(target,std::move(source));
        installed|=bit;
    };
    if(payloadMask&(1u<<1)){
        install(1u<<1,cached.tel,detail.tel);
        if(cached.tyre.isEmpty())cached.tyre=std::move(detail.tyre);else mergeRows(cached.tyre,std::move(detail.tyre));
    }
    if(payloadMask&(1u<<2))install(1u<<2,cached.sts,detail.sts);
    if(payloadMask&(1u<<3))install(1u<<3,cached.damage,detail.damage);
    if(payloadMask&(1u<<4))install(1u<<4,cached.progress,detail.progress);
    if(payloadMask&(1u<<11))install(1u<<11,cached.motion,detail.motion);
    if(payloadMask&(1u<<12))install(1u<<12,cached.motionEx,detail.motionEx);
    if(payloadMask&(1u<<13))install(1u<<13,cached.positions,detail.positions);

    const QString cacheKey=QString("%1:%2").arg(driverIndex).arg(lapNum);
    file.lru.removeAll(cacheKey);
    file.lru.push_back(cacheKey);
    while(file.lru.size()>6){
        const QString old=file.lru.takeFirst();
        const int separator=old.lastIndexOf(':');
        bool driverOk=false,lapOk=false;
        const int oldDriver=old.left(separator).toInt(&driverOk);
        const int oldLap=old.mid(separator+1).toInt(&lapOk);
        auto*oldState=driverOk&&lapOk?file.driver(oldDriver):nullptr;
        if(!oldState)continue;
        oldState->cache.remove(oldLap);
        oldState->installedMasks.remove(oldLap);
        oldState->requestedMasks.remove(oldLap);
    }
    applyState();
}

void AnalyzePage::installPrimaryLap(uint64_t generation,int driverIndex,int lapNum,uint32_t rowTypeMask,const std::shared_ptr<PlaybackHistoryBatch>&batch){
    if(!primary_||primary_->generation!=generation)return;
    installAnalysisLap(*primary_,driverIndex,lapNum,rowTypeMask,batch);
}

void AnalyzePage::installSecondaryLap(uint64_t generation,int driverIndex,int lapNum,uint32_t rowTypeMask,const std::shared_ptr<PlaybackHistoryBatch>&batch){
    if(!secondary_||secondary_->generation!=generation)return;
    installAnalysisLap(*secondary_,driverIndex,lapNum,rowTypeMask,batch);
}

void AnalyzePage::requestAnalysisLaps(){
    if(!playback_)return;
    QVector<AnalysisSelection> selections;
    if(fixedMode_->isChecked())selections={selectedLap(lapA_),selectedLap(lapB_)};
    else selections={selectedLap(compareLap_)};
    const uint32_t wanted=playbackRowMask();
    QSet<QString> requested;
    for(const AnalysisSelection&selection:selections){
        if(!selection.valid)continue;
        const QString key=lapKey(selection.secondary,selection.driverIndex,selection.lapNum);
        if(requested.contains(key))continue;
        requested.insert(key);
        FileState*file=selection.secondary?secondary_.get():primary_.get();
        if(!file||(selection.secondary&&secondaryLoading_))continue;
        auto*driver=file->driver(selection.driverIndex);
        if(!driver)continue;
        const uint32_t missing=wanted&~driver->installedMasks.value(selection.lapNum)&~driver->requestedMasks.value(selection.lapNum);
        if(!missing)continue;
        driver->requestedMasks[selection.lapNum]|=missing;
        if(selection.secondary)
            secondaryReader_->requestLapData(selection.driverIndex,selection.lapNum,missing);
        else
            emit primaryLapDataRequested(primaryGeneration_,selection.driverIndex,selection.lapNum,missing);
    }
}

uint32_t AnalyzePage::playbackRowMask() const {
    uint32_t mask = 1u << 4;
    const QString view=viewMode_?viewMode_->currentData().toString():QStringLiteral("graph");
    if(playback_&&(view=="map"||view=="split"))mask|=(1u<<13)|(1u<<1)|(1u<<2);
    if(view=="map")return mask;
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
    if(!playback_||circuitsMismatched())return;
    const bool fixed=fixedMode_->isChecked();
    const SessionData*owner=&model_->data();
    const LapBlock*lap=nullptr;
    if(fixed){
        const AnalysisSelection selection=selectedLap(lapA_);
        const FileState*file=selection.secondary?secondary_.get():primary_.get();
        const auto*driver=file&&selection.valid?file->driver(selection.driverIndex):nullptr;
        if(driver){
            owner=&driver->data;
            const auto it=driver->cache.constFind(selection.lapNum);
            if(it!=driver->cache.cend())lap=&it.value();
        }
    }else lap=model_->chartPrimaryLap(currentTime_);
    if(!lap)return;double elapsed=coordinate;if(distanceCoordinate){const double absolute=owner->timeAtDistance(lap,coordinate);elapsed=absolute-lap->startSessionTime;}if(!std::isfinite(elapsed))return;
    if(viewMode_->currentData().toString()=="graph"){
        const int splitIndex=viewMode_->findData("split");preferredView_="split";saveSettings();if(splitIndex>=0){QSignalBlocker guard(viewMode_);viewMode_->setCurrentIndex(splitIndex);}
    }
    applyState();map_->focusElapsed(elapsed);
}

QVector<int> AnalyzePage::requestedPlaybackLaps() const {
    // Explicit comparison laps use the per-file/per-driver analysis cache.
    // The playback history request remains responsible only for the current lap.
    return {};
}
