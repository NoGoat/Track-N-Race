#include "SettingsDialog.h"
#include "../MainWindow.h"
#include "../CompactSettings.h"
#include "../GraphViewSettings.h"
#include "../IconUtils.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QFrame>
#include <QLabel>
#include <QCheckBox>
#include <QRadioButton>
#include <QComboBox>
#include <QSlider>
#include <QSpinBox>
#include <QLineEdit>
#include <QStyleFactory>
#include <QPushButton>
#include <QFileDialog>
#include <QSizePolicy>
#include <QFont>
#include <QButtonGroup>
#include <QStackedWidget>
#include <QPalette>
#include <QApplication>
#include <QTableWidget>
#include <QHeaderView>
#include <QToolButton>
#include <QStyle>
#include <QTextBrowser>
#include <QClipboard>
#include <QFile>
#include <QTextStream>
#include <QFontDatabase>
#include <iterator>

static QFrame* horizontalSeparator() {
    QFrame* f = new QFrame;
    f->setFrameShape(QFrame::HLine);
    f->setFrameShadow(QFrame::Sunken);
    return f;
}

// Bold label spanning both form columns, for a sub-section header inside a page.
static QLabel* subHeading(const QString& text) {
    QLabel* l = new QLabel(text);
    QFont f = l->font();
    f.setBold(true);
    l->setFont(f);
    return l;
}

SettingsDialog::SettingsDialog(MainWindow* mainWindow, QWidget* parent)
    : QDialog(parent), mainWindow_(mainWindow)
{
    setWindowTitle("Settings");
    // Qt::Dialog gives the plain dialog frame (no minimize/maximize buttons)
    // without the bitwise flag-stripping the old code needed for that. Modal +
    // parented to MainWindow (see the call site) is what actually keeps this
    // above MainWindow and blocks it — the window-type flag alone doesn't.
    setWindowFlags(Qt::Dialog);
    setWindowModality(Qt::ApplicationModal);

    QVBoxLayout* main = new QVBoxLayout(this);
    // A fixed-size top-level layout makes Qt drop the resize handles and the
    // maximize button (same trick as EditOverviewLayoutDialog) — this should
    // behave like a plain modal child dialog, not a maximizable window.
    main->setSizeConstraint(QLayout::SetFixedSize);
    main->setContentsMargins(0, 0, 0, 0);
    main->setSpacing(0);

    // ── Category tabs over a shared content pane ──────────────────
    // Underline tabs matching the main toolbar's page switcher (see the page
    // tabs in MainWindow.cpp): a row of checkable QToolButtons in an exclusive
    // group, each reserving the same border-bottom width (transparent unless
    // checked) so the accent underline only changes colour, never shifts the
    // text. A QStackedWidget holds the page bodies so the shared Close button
    // can live inside the same bordered pane below.
    QStackedWidget* stack = new QStackedWidget;

    struct Page { const char* title; QWidget* widget; };
    const Page pages[] = {
        { "Appearance",    buildAppearancePage()    },
        { "Compact",       buildCompactPage()       },
        { "Graphs",        buildGraphsPage()        },
        { "Recording",     buildRecordingPage()     },
        { "Overview",      buildOverviewPage()      },
        { "Track Map",     buildTrackMapPage()      },
        { "Notifications", buildNotificationsPage() },
        { "Protocol",      buildProtocolPage()      },
    };

    QWidget*     tabBar = new QWidget;
    // Tint the bar a shade lighter than the window (Midlight) so it reads as a
    // distinct surface, separated from the bordered content pane below.
    tabBar->setBackgroundRole(QPalette::Button);
    tabBar->setAutoFillBackground(true);
    QHBoxLayout* tabLay = new QHBoxLayout(tabBar);
    tabLay->setContentsMargins(8, 0, 8, 0);
    tabLay->setSpacing(4);
    QButtonGroup* tabGroup = new QButtonGroup(this);
    tabGroup->setExclusive(true);

    static constexpr int kUnderlineWidth = 2;
    const QString accent = QApplication::palette().color(QPalette::Highlight).name();
    const QString tabBtnStyle = QString(
        "QToolButton { padding: 8px 14px; border: none; background: transparent;"
        " border-bottom: %1px solid transparent; }"
        "QToolButton:checked { border-bottom: %1px solid %2; }"
    ).arg(kUnderlineWidth).arg(accent);

    int tabIndex = 0;
    for (const Page& p : pages) {
        QToolButton* b = new QToolButton;
        b->setText(p.title);
        b->setCheckable(true);
        b->setAutoRaise(true);
        b->setStyleSheet(tabBtnStyle);
        tabGroup->addButton(b, tabIndex++);
        tabLay->addWidget(b);
        stack->addWidget(p.widget);
    }
    tabLay->addStretch(1);
    connect(tabGroup, &QButtonGroup::idClicked, stack, &QStackedWidget::setCurrentIndex);
    static_cast<QToolButton*>(tabGroup->button(0))->setChecked(true);
    stack->setCurrentIndex(0);

    // Pane holding the page stack + the Close button row. A single top border
    // separates it from the tab bar above; the colour is mixed toward the text
    // colour so it actually contrasts with the background (the palette's
    // Mid/Sunken etch is nearly identical to Window in this dark theme, which is
    // why earlier borders were invisible). Scoped by object name so it doesn't
    // bleed onto children.
    QFrame* pane = new QFrame;
    pane->setObjectName("settingsPane");
    const QColor  win = QApplication::palette().color(QPalette::Window);
    const QColor  txt = QApplication::palette().color(QPalette::WindowText);
    // ~30% of the way from the window colour to the text colour: a clearly
    // visible hairline in both light and dark themes.
    const QColor  borderCol((win.red()   * 7 + txt.red()   * 3) / 10,
                            (win.green() * 7 + txt.green() * 3) / 10,
                            (win.blue()  * 7 + txt.blue()  * 3) / 10);
    pane->setStyleSheet(QString(
        "QFrame#settingsPane { background-color: %1; border-top: 1px solid %2; }")
        .arg(win.name(), borderCol.name()));
    QVBoxLayout* paneLay = new QVBoxLayout(pane);
    paneLay->setContentsMargins(0, 0, 0, 0);
    paneLay->setSpacing(0);
    paneLay->addWidget(stack, 1);
    paneLay->addWidget(horizontalSeparator());

    QHBoxLayout* bottom = new QHBoxLayout;
    bottom->setContentsMargins(12, 8, 12, 12);
    QPushButton* aboutBtn = new QPushButton("About");
    aboutBtn->setIcon(adaptThemeIcon(
        QIcon::fromTheme("info-symbolic"),
        palette().color(QPalette::WindowText),
        style()->standardIcon(QStyle::SP_MessageBoxInformation)));
    connect(aboutBtn, &QPushButton::clicked, this, &SettingsDialog::showAboutDialog);
    bottom->addWidget(aboutBtn);
    bottom->addStretch(1);
    QPushButton* closeBtn = new QPushButton("Close");
    closeBtn->setDefault(true);
    closeBtn->setIcon(adaptThemeIcon(
        QIcon::fromTheme("dialog-close-symbolic"),
        palette().color(QPalette::WindowText),
        style()->standardIcon(QStyle::SP_DialogCloseButton)));
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    bottom->addWidget(closeBtn);
    paneLay->addLayout(bottom);

    main->addWidget(tabBar);
    main->addWidget(pane);
}

// Page scaffold: a right-aligned label/control form (the tab supplies the title).
QWidget* SettingsDialog::makePage(QFormLayout*& formOut) {
    QWidget* page = new QWidget;
    QVBoxLayout* v = new QVBoxLayout(page);
    v->setContentsMargins(8, 12, 8, 8);
    v->setSpacing(12);

    QFormLayout* form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight);
    form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
    form->setHorizontalSpacing(18);
    form->setVerticalSpacing(10);
    v->addLayout(form);
    v->addStretch(1);

    formOut = form;
    return page;
}

QWidget* SettingsDialog::buildProtocolPage() {
    QFormLayout* form;
    QWidget* page = makePage(form);

    // Read-only: the format most recently detected from incoming UDP packets,
    // cached by MainWindow::onEngineRow() from protocol_status rows. Shows
    // "—" until the first packet after the engine starts.
    const int detected = mainWindow_->lastDetectedProtocolFormat();
    detectedProtocolLabel_ = new QLabel(detected > 0 ? QString::number(detected) : QStringLiteral("—"));
    form->addRow("Detected Protocol:", detectedProtocolLabel_);

    protocolCombo_ = new QComboBox;
    protocolCombo_->addItem("Auto", "auto");
    protocolCombo_->addItem("2024", "f1_24");
    protocolCombo_->addItem("2025", "f1_25");
    protocolCombo_->addItem("2026", "f1_26");
    const int idx = protocolCombo_->findData(mainWindow_->currentProtocolOverride());
    protocolCombo_->setCurrentIndex(idx >= 0 ? idx : 0);
    connect(protocolCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        mainWindow_->setProtocolOverride(protocolCombo_->currentData().toString());
    });
    form->addRow("Protocol Version Override:", protocolCombo_);

    form->addRow(horizontalSeparator());
    form->addRow(subHeading("Network"));

    // Port + bind address rebind the live UDP socket on edit (see
    // MainWindow::setUdpPort / setUdpBindAddress). editingFinished (focus-out or
    // Enter), not valueChanged, so the socket isn't restarted on every keystroke.
    udpPortSpin_ = new QSpinBox;
    udpPortSpin_->setRange(1, 65535);
    udpPortSpin_->setGroupSeparatorShown(false);   // a port is not a thousands-grouped number
    udpPortSpin_->setValue(mainWindow_->udpPort());
    connect(udpPortSpin_, &QSpinBox::editingFinished, this, [this] {
        mainWindow_->setUdpPort(udpPortSpin_->value());
    });
    form->addRow("UDP Port:", udpPortSpin_);

    udpBindAddressEdit_ = new QLineEdit(mainWindow_->udpBindAddress());
    udpBindAddressEdit_->setPlaceholderText(QStringLiteral("0.0.0.0"));
    udpBindAddressEdit_->setToolTip(
        "Which local network interface to receive telemetry on. "
        "0.0.0.0 listens on all interfaces.");
    connect(udpBindAddressEdit_, &QLineEdit::editingFinished, this, [this] {
        const QString addr = udpBindAddressEdit_->text().trimmed();
        udpBindAddressEdit_->setText(addr);
        mainWindow_->setUdpBindAddress(addr.isEmpty() ? QStringLiteral("0.0.0.0") : addr);
    });
    form->addRow("Bind Address:", udpBindAddressEdit_);

    return page;
}

QWidget* SettingsDialog::buildRecordingPage() {
    QFormLayout* form;
    QWidget* page = makePage(form);

    recordCheck_ = new QCheckBox("Auto-record when a session starts");
    recordCheck_->setChecked(mainWindow_->autoRecordEnabled());
    form->addRow(QString(), recordCheck_);

    QWidget* dirRow = new QWidget;
    QHBoxLayout* dirLay = new QHBoxLayout(dirRow);
    dirLay->setContentsMargins(0, 0, 0, 0);
    const QString dir = mainWindow_->currentOutputDirectory();
    dirLabel_ = new QLabel(dir.isEmpty() ? "No directory selected." : dir);
    dirLabel_->setWordWrap(true);
    dirLabel_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    dirLabel_->setMaximumWidth(280);
    QPushButton* browseBtn = new QPushButton("Browse…");
    browseBtn->setFixedWidth(90);
    dirLay->addWidget(dirLabel_);
    dirLay->addWidget(browseBtn);
    form->addRow("Save to:", dirRow);

    connect(recordCheck_, &QCheckBox::toggled, this, [this](bool on) {
        mainWindow_->setAutoRecord(on);
    });
    connect(browseBtn, &QPushButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(
            this, "Select Output Directory", mainWindow_->currentOutputDirectory());
        if (!dir.isEmpty()) {
            mainWindow_->setOutputDirectory(dir);
            dirLabel_->setText(dir);
        }
    });
    return page;
}

QWidget* SettingsDialog::buildAppearancePage() {
    QFormLayout* form;
    QWidget* page = makePage(form);

    QWidget* themeRow = new QWidget;
    QHBoxLayout* themeLay = new QHBoxLayout(themeRow);
    themeLay->setContentsMargins(0, 0, 0, 0);
    themeLay->setSpacing(14);
    themeSystem_ = new QRadioButton("System default");
    themeLight_  = new QRadioButton("Light");
    themeDark_   = new QRadioButton("Dark");
    themeLay->addWidget(themeSystem_);
    themeLay->addWidget(themeLight_);
    themeLay->addWidget(themeDark_);

    const QString theme = mainWindow_->currentTheme();
    if (theme == "light")     themeLight_->setChecked(true);
    else if (theme == "dark") themeDark_->setChecked(true);
    else                      themeSystem_->setChecked(true);

    form->addRow("Theme:", themeRow);

    // Style selector — lists the QStyles actually available at runtime, so a
    // bundled "Breeze" only appears once its plugin has loaded. "System default"
    // restores Qt's native platform style. Each item's userData is the lowercased
    // QStyleFactory key (what setStyleName/QSettings store); "system" is special.
    styleCombo_ = new QComboBox;
    styleCombo_->addItem("System default", "system");
    const QString curStyle = mainWindow_->currentStyleName();
    for (const QString& key : QStyleFactory::keys()) {
        styleCombo_->addItem(key, key.toLower());
        if (key.compare(curStyle, Qt::CaseInsensitive) == 0)
            styleCombo_->setCurrentIndex(styleCombo_->count() - 1);
    }
    // Connect after populating so the initial setCurrentIndex above doesn't
    // re-trigger an apply during construction.
    connect(styleCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
        mainWindow_->setStyleName(styleCombo_->currentData().toString());
    });
    form->addRow("Style:", styleCombo_);

    toolbarLabelsCheck_ = new QCheckBox("Show button labels in toolbar");
    toolbarLabelsCheck_->setChecked(mainWindow_->toolbarLabelsEnabled());
    form->addRow("Toolbar:", toolbarLabelsCheck_);

    form->addRow(horizontalSeparator());
    form->addRow(subHeading("Graphs"));

    // OpenGL multisampling (MSAA) for the charts: higher = smoother lines but more
    // GPU fill cost. userData is the sample count passed to QCustomPlot::setOpenGl;
    // "Off" (0) disables anti-aliasing entirely (fastest, aliased lines).
    chartMsaaCombo_ = new QComboBox;
    chartMsaaCombo_->addItem("Off", 0);
    for (int s : { 2, 4, 8, 16 })
        chartMsaaCombo_->addItem(QString("%1x").arg(s), s);
    const int curMsaa = mainWindow_->chartMsaaSamples();
    const int msaaIdx = chartMsaaCombo_->findData(curMsaa);
    chartMsaaCombo_->setCurrentIndex(msaaIdx >= 0 ? msaaIdx : chartMsaaCombo_->findData(4));
    // Connect after selecting so the initial index set above doesn't apply.
    connect(chartMsaaCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
        mainWindow_->setChartMsaaSamples(chartMsaaCombo_->currentData().toInt());
    });
    form->addRow("Anti-aliasing:", chartMsaaCombo_);

    form->addRow(horizontalSeparator());
    form->addRow(subHeading("Accessibility"));

    QWidget* contrastRow = new QWidget;
    QHBoxLayout* ch = new QHBoxLayout(contrastRow);
    ch->setContentsMargins(0, 0, 0, 0);
    QSlider* contrastSlider = new QSlider(Qt::Horizontal);
    contrastSlider->setRange(100, 2100);
    contrastSlider->setValue((int)(mainWindow_->contrastThreshold() * 100.0f));
    QLabel* contrastVal = new QLabel(QString::number(mainWindow_->contrastThreshold(), 'f', 2));
    contrastVal->setFixedWidth(40);
    ch->addWidget(contrastSlider);
    ch->addWidget(contrastVal);
    form->addRow("Contrast Threshold:", contrastRow);

    auto applyTheme = [this](bool) {
        QString val = "system";
        if (themeLight_->isChecked())     val = "light";
        else if (themeDark_->isChecked()) val = "dark";
        mainWindow_->setTheme(val);
    };
    connect(themeSystem_, &QRadioButton::toggled, this, applyTheme);
    connect(themeLight_,  &QRadioButton::toggled, this, applyTheme);
    connect(themeDark_,   &QRadioButton::toggled, this, applyTheme);
    connect(toolbarLabelsCheck_, &QCheckBox::toggled, this, [this](bool on) {
        mainWindow_->setToolbarLabels(on);
    });
    connect(contrastSlider, &QSlider::valueChanged, this, [this, contrastVal](int val) {
        float f = val / 100.0f;
        contrastVal->setText(QString::number(f, 'f', 2));
        mainWindow_->setContrastThreshold(f);
    });
    return page;
}

QWidget* SettingsDialog::buildCompactPage() {
    QWidget* page = new QWidget;
    QVBoxLayout* v = new QVBoxLayout(page);
    v->setContentsMargins(8, 12, 8, 8);
    v->setSpacing(10);

    // One control = a muted caption over a Normal/Compact dropdown. (A dropdown, not
    // a checkbox, so a planned "Ultra Compact" option can be added to select sections
    // later without a redesign.) Each page's controls sit side by side in one row.
    auto makeControl = [this](const char* label, tnr::CompactSection s) -> QWidget* {
        QWidget* w = new QWidget;
        QVBoxLayout* cv = new QVBoxLayout(w);
        cv->setContentsMargins(0, 0, 0, 0);
        cv->setSpacing(3);
        QLabel* cap = new QLabel(label);
        cap->setForegroundRole(QPalette::PlaceholderText);
        QComboBox* combo = new QComboBox;
        // The Overview tyre cards have two extra density levels (Ultra Compact 1/2),
        // so that one control gets a 4-way int level; every other section is on/off.
        if (s == tnr::CompactSection::OverviewTyres) {
            combo->addItem("Normal");
            combo->addItem("Compact");
            combo->addItem("Ultra Compact 1");
            combo->addItem("Ultra Compact 2");
            combo->addItem("Ultra Compact 3");
            combo->setCurrentIndex(mainWindow_->tyresCompactLevel());
            connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                    [this](int idx) { mainWindow_->setTyresCompactLevel(idx); });
        } else {
            combo->addItem("Normal");
            combo->addItem("Compact");
            combo->setCurrentIndex(mainWindow_->compactSection(s) ? 1 : 0);
            connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                    [this, s](int idx) { mainWindow_->setCompactSection(s, idx == 1); });
        }
        cv->addWidget(cap);
        cv->addWidget(combo);
        return w;
    };

    struct Row { tnr::CompactSection s; const char* group; const char* label; };
    static const Row rows[] = {
        { tnr::CompactSection::OverviewStats,   "Overview", "Stats row" },
        { tnr::CompactSection::OverviewDamage,  "Overview", "Damage cards" },
        { tnr::CompactSection::OverviewTyres,   "Overview", "Tyre cards" },
        { tnr::CompactSection::SessionCards,    "Session",  "Info cards" },
        { tnr::CompactSection::SessionWeather,  "Session",  "Weather strip" },
        { tnr::CompactSection::SessionHeader,   "Session",  "Header" },
        { tnr::CompactSection::PowerCards,      "Power",    "Cards" },
        { tnr::CompactSection::StrategySummary, "Strategy", "Summary" },
    };

    QString lastGroup;
    QHBoxLayout* rowLay = nullptr;
    for (const Row& r : rows) {
        if (r.group != lastGroup) {
            if (rowLay) rowLay->addStretch(1);
            if (!lastGroup.isEmpty()) v->addWidget(horizontalSeparator());
            v->addWidget(subHeading(r.group));
            QWidget* rowW = new QWidget;
            rowLay = new QHBoxLayout(rowW);
            rowLay->setContentsMargins(0, 0, 0, 0);
            rowLay->setSpacing(20);
            v->addWidget(rowW);
            lastGroup = r.group;
        }
        rowLay->addWidget(makeControl(r.label, r.s));
    }
    if (rowLay) rowLay->addStretch(1);
    v->addStretch(1);
    return page;
}

QWidget* SettingsDialog::buildGraphsPage() {
    QWidget* page = new QWidget;
    QVBoxLayout* v = new QVBoxLayout(page);
    v->setContentsMargins(8, 12, 8, 8);
    v->setSpacing(10);

    // One control = a muted caption over a Chart/Table dropdown, mirroring the
    // Compact page. Each graph can independently show as its chart or as a table
    // of the raw sample values behind it (see GraphTable / the charts widgets).
    auto makeControl = [this](const char* label, tnr::GraphSection s) -> QWidget* {
        QWidget* w = new QWidget;
        QVBoxLayout* cv = new QVBoxLayout(w);
        cv->setContentsMargins(0, 0, 0, 0);
        cv->setSpacing(3);
        QLabel* cap = new QLabel(label);
        cap->setForegroundRole(QPalette::PlaceholderText);
        QComboBox* combo = new QComboBox;
        combo->addItem("Chart");
        combo->addItem("Table");
        combo->setCurrentIndex(mainWindow_->graphView(s) ? 1 : 0);
        connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this, s](int idx) { mainWindow_->setGraphView(s, idx == 1); });
        cv->addWidget(cap);
        cv->addWidget(combo);
        return w;
    };

    struct Row { tnr::GraphSection s; const char* group; const char* label; };
    static const Row rows[] = {
        { tnr::GraphSection::OverviewTelemetry,   "Overview", "Speed / RPM / ERS" },
        { tnr::GraphSection::OverviewTyreSurface, "Overview", "Tyre surface temp" },
        { tnr::GraphSection::OverviewTyreInner,   "Overview", "Tyre inner temp" },
        { tnr::GraphSection::OverviewTyreBrake,   "Overview", "Tyre brake temp" },
        { tnr::GraphSection::OverviewTyreWear,    "Overview", "Tyre wear / life" },
        { tnr::GraphSection::TyreSurface,        "Tyres",    "Surface temp" },
        { tnr::GraphSection::TyreInner,          "Tyres",    "Inner temp" },
        { tnr::GraphSection::TyreBrake,          "Tyres",    "Brake temp" },
        { tnr::GraphSection::TyreWear,           "Tyres",    "Wear / life" },
        { tnr::GraphSection::InputGear,          "Input",    "Gear" },
        { tnr::GraphSection::InputThrottleBrake, "Input",    "Throttle / brake" },
        { tnr::GraphSection::InputSteering,      "Input",    "Steering" },
        { tnr::GraphSection::PowerSplit,         "Power",    "Power split" },
        { tnr::GraphSection::PowerHarvest,       "Power",    "ERS harvest" },
        { tnr::GraphSection::PowerStore,         "Power",    "ERS store" },
        { tnr::GraphSection::PowerFuel,          "Power",    "Fuel" },
        { tnr::GraphSection::MiscGForce,         "Misc",     "G-force" },
        { tnr::GraphSection::MiscRideHeight,     "Misc",     "Ride height" },
    };

    QString lastGroup;
    QHBoxLayout* rowLay = nullptr;
    for (const Row& r : rows) {
        if (r.group != lastGroup) {
            if (rowLay) rowLay->addStretch(1);
            if (!lastGroup.isEmpty()) v->addWidget(horizontalSeparator());
            v->addWidget(subHeading(r.group));
            QWidget* rowW = new QWidget;
            rowLay = new QHBoxLayout(rowW);
            rowLay->setContentsMargins(0, 0, 0, 0);
            rowLay->setSpacing(20);
            v->addWidget(rowW);
            lastGroup = r.group;
        }
        rowLay->addWidget(makeControl(r.label, r.s));
    }
    if (rowLay) rowLay->addStretch(1);

    v->addStretch(1);
    return page;
}

QWidget* SettingsDialog::buildNotificationsPage() {
    QFormLayout* form;
    QWidget* page = makePage(form);

    toastsCheck_ = new QCheckBox("Show event toasts (penalties, flags, fastest lap…)");
    toastsCheck_->setChecked(mainWindow_->toastsEnabled());
    form->addRow(QString(), toastsCheck_);

    toastDurationCombo_ = new QComboBox;
    for (int s : { 2, 3, 5, 8, 10 })
        toastDurationCombo_->addItem(QString("%1s").arg(s), s);
    const int cur = toastDurationCombo_->findData(mainWindow_->toastDurationSecs());
    toastDurationCombo_->setCurrentIndex(cur >= 0 ? cur : 1);   // default 3s
    toastDurationCombo_->setEnabled(toastsCheck_->isChecked());
    form->addRow("Toast duration:", toastDurationCombo_);

    connect(toastsCheck_, &QCheckBox::toggled, this, [this](bool on) {
        mainWindow_->setToastsEnabled(on);
        toastDurationCombo_->setEnabled(on);
    });
    connect(toastDurationCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        mainWindow_->setToastDurationSecs(toastDurationCombo_->currentData().toInt());
    });
    return page;
}

QWidget* SettingsDialog::buildOverviewPage() {
    QFormLayout* form;
    QWidget* page = makePage(form);

    tyreViewCombo_ = new QComboBox;
    tyreViewCombo_->addItem("Cards",  (int)OverviewLayout::TyreCards);
    tyreViewCombo_->addItem("Charts", (int)OverviewLayout::TyreCharts);
    tyreViewCombo_->setCurrentIndex(
        mainWindow_->currentTyreView() == OverviewLayout::TyreCharts ? 1 : 0);
    connect(tyreViewCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        mainWindow_->setTyreView(
            tyreViewCombo_->currentData().toInt() == (int)OverviewLayout::TyreCharts
                ? OverviewLayout::TyreCharts : OverviewLayout::TyreCards);
    });
    form->addRow("Tyre view:", tyreViewCombo_);

    // Whether the tyre graph shows remaining life (100 - wear) or accumulated wear.
    tyreWearModeCombo_ = new QComboBox;
    tyreWearModeCombo_->addItem("Tyre Life", true);
    tyreWearModeCombo_->addItem("Tyre Wear", false);
    tyreWearModeCombo_->setCurrentIndex(mainWindow_->tyreGraphLifeMode() ? 0 : 1);
    connect(tyreWearModeCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        mainWindow_->setTyreGraphLifeMode(tyreWearModeCombo_->currentData().toBool());
    });
    form->addRow("Tyre wear graph:", tyreWearModeCombo_);
    return page;
}

QWidget* SettingsDialog::buildTrackMapPage() {
    QFormLayout* form;
    QWidget* page = makePage(form);

    trackMapLabelsCombo_ = new QComboBox;
    trackMapLabelsCombo_->addItem("Dots & Labels", 0);
    trackMapLabelsCombo_->addItem("Dots Only", 1);
    trackMapLabelsCombo_->addItem("Labels Only", 2);
    trackMapLabelsCombo_->setCurrentIndex(mainWindow_->trackMapLabelMode());

    connect(trackMapLabelsCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        mainWindow_->setTrackMapLabelMode(trackMapLabelsCombo_->currentData().toInt());
    });
    form->addRow("Labels:", trackMapLabelsCombo_);

    // Hide drivers idle for longer than the selected duration (0 = never).
    trackMapIdleCombo_ = new QComboBox;
    trackMapIdleCombo_->addItem("Off", 0);
    for (int s : { 3, 5, 10, 15, 30 })
        trackMapIdleCombo_->addItem(QString("%1s").arg(s), s);
    const int idleIdx = trackMapIdleCombo_->findData(mainWindow_->trackMapIdleTimeout());
    trackMapIdleCombo_->setCurrentIndex(idleIdx >= 0 ? idleIdx : 0);
    connect(trackMapIdleCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        mainWindow_->setTrackMapIdleTimeout(trackMapIdleCombo_->currentData().toInt());
    });
    form->addRow("Hide static drivers:", trackMapIdleCombo_);

    // Per-sector colours vs plain white/black lines.
    trackMapSectorColorsCheck_ = new QCheckBox;
    trackMapSectorColorsCheck_->setChecked(mainWindow_->trackMapSectorColors());
    connect(trackMapSectorColorsCheck_, &QCheckBox::toggled, this, [this](bool on) {
        mainWindow_->setTrackMapSectorColors(on);
    });
    form->addRow("Sector colors:", trackMapSectorColorsCheck_);

    // Track-outline opacity (20–100%); driver dots/labels stay full strength.
    trackMapOpacitySlider_ = new QSlider(Qt::Horizontal);
    trackMapOpacitySlider_->setRange(20, 100);
    trackMapOpacitySlider_->setValue(mainWindow_->trackMapOpacity());
    connect(trackMapOpacitySlider_, &QSlider::valueChanged, this, [this](int v) {
        mainWindow_->setTrackMapOpacity(v);
    });
    form->addRow("Map opacity:", trackMapOpacitySlider_);
    return page;
}

// About: app identity + version, the project license, and attribution for every
// bundled third-party library. Each library exposes its full license text via a
// "View" button (showLicenseText), satisfying the GPL/LGPL notice requirements.
QWidget* SettingsDialog::buildAboutPage() {
    QWidget* page = new QWidget;
    QVBoxLayout* v = new QVBoxLayout(page);
    v->setContentsMargins(8, 12, 8, 8);
    v->setSpacing(8);

    // ── App identity ─────────────────────────────────────────────
    QLabel* name = new QLabel(QApplication::applicationName());
    QFont nameFont = name->font();
    nameFont.setBold(true);
    nameFont.setPointSizeF(nameFont.pointSizeF() + 3.0);
    name->setFont(nameFont);
    v->addWidget(name);

    v->addWidget(new QLabel("Version " + QApplication::applicationVersion()));

    QLabel* desc = new QLabel(
        "Background telemetry recorder and live session viewer for F1 sim racing.");
    desc->setWordWrap(true);
    v->addWidget(desc);

    QLabel* copyright = new QLabel("© 2026 Track N Race");
    v->addWidget(copyright);

    QLabel* repoLink = new QLabel(
        "<a href=\"https://github.com/nogoat/track-n-race\">github.com/nogoat/track-n-race</a>");
    repoLink->setOpenExternalLinks(true);
    v->addWidget(repoLink);

    // Project license row.
    QWidget* licRow = new QWidget;
    QHBoxLayout* licLay = new QHBoxLayout(licRow);
    licLay->setContentsMargins(0, 0, 0, 0);
    licLay->addWidget(new QLabel("Licensed under the GNU General Public License v3."));
    QPushButton* viewProjectLic = new QPushButton("View license");
    connect(viewProjectLic, &QPushButton::clicked, this, [this] {
        showLicenseText("GNU General Public License v3", ":/licenses/GPL-3.0.txt");
    });
    licLay->addWidget(viewProjectLic);
    licLay->addStretch(1);
    v->addWidget(licRow);

    v->addWidget(horizontalSeparator());
    v->addWidget(subHeading("Third-party software"));

    // name, version, license label, copyright holder, homepage URL, short link text, license resource.
    struct Lib { const char* name; QString version; const char* license; const char* copyright;
                 const char* url; const char* linkText; const char* resource; };
    const Lib libs[] = {
        { "Qt",            qVersion(), "LGPL v3", "© The Qt Company Ltd.",                       "https://www.qt.io",                "qt.io",           ":/licenses/LGPL-3.0.txt" },
        { "QCustomPlot",   "2.1.1",    "GPL v3",  "© 2011–2022 Emanuel Eichhammer",         "https://www.qcustomplot.com",      "qcustomplot.com", ":/licenses/GPL-3.0.txt"  },
        { "nlohmann/json", "3.11.3",   "MIT",     "© 2013–2022 Niels Lohmann",              "https://github.com/nlohmann/json", "github.com",      ":/licenses/MIT-nlohmann.txt" },
        { "glaze",         "7.8.3",    "MIT",     "© 2019–present Stephen Berry",           "https://github.com/stephenberry/glaze", "github.com", ":/licenses/MIT-glaze.txt" },
        // The toast notifications are a fork of niklashenning/qt-toast (see the
        // license text, which notes the fork and reproduces the upstream notice).
        { "qt-toast (fork)", "—",      "MIT",     "© 2024 Niklas Henning",                  "https://github.com/niklashenning/qt-toast", "github.com", ":/licenses/MIT-qt-toast.txt" },
        { "zlib",          "1.3.2",    "zlib",    "© 1995–2026 Jean-loup Gailly & Mark Adler", "https://zlib.net",              "zlib.net",        ":/licenses/Zlib.txt"     },
#ifdef BREEZE_BUNDLED
        // KDE is bundled only in --with-breeze builds (BREEZE_BUNDLED). "KDE
        // Frameworks" covers every KF6 module in the breeze6 runtime closure
        // (CoreAddons, Config, GuiAddons, ColorScheme, WindowSystem, IconThemes,
        // Archive, Codecs, ConfigWidgets, I18n, WidgetsAddons) — one row, not
        // eleven, because KDE's Frameworks Licensing Policy requires every module
        // to use the same terms (LGPL 2.1, or 3, or a later KDE e.V.-approved
        // version), so it's genuinely one license, matching KDE apps' own
        // convention (e.g. Kate's About dialog). All ship at 6.27.0.
        //
        // Breeze style (Plasma) is a separate row: it tracks Plasma's own release
        // (6.7.0, not KF6_VERSION) and — unlike the rest of this list — the
        // widget-style plugin itself (kstyle/breezestyle.cpp) is GPL-2.0-or-later,
        // not LGPL; verified against its SPDX header and the GPL-2.0-or-later.txt
        // shipped in the breeze repo's LICENSES/ dir.
        //
        // Breeze Icons is also separate: KDE's policy requires icon files
        // specifically to be LGPL-3.0 (not 2.1-or-later like the rest of
        // Frameworks), and the shipped icon set is a mix of LGPL-2.x/3.0/
        // CC-BY-SA-4.0 from legacy contributions — distinct enough from the code
        // frameworks' licensing to warrant its own line and its own license text.
        { "Breeze style (Plasma)", "6.7.0",  "GPL v2+",    "© 2014 Hugo Pereira Da Costa, The Qt Company Ltd., and KDE contributors", "https://invent.kde.org/plasma/breeze", "invent.kde.org", ":/licenses/GPL-2.0.txt" },
        { "Breeze Icons",          "6.27.0", "LGPL v3",    "© KDE contributors",                         "https://invent.kde.org/frameworks/breeze-icons", "invent.kde.org", ":/licenses/LGPL-3.0.txt" },
        { "KDE Frameworks",        "6.27.0", "LGPL v2.1+", "© KDE contributors",                         "https://develop.kde.org/products/frameworks/",   "develop.kde.org", ":/licenses/LGPL-2.1.txt" },
#endif
    };
    const int libCount = int(std::size(libs));

    // A table keeps the columns aligned and width under control; the long homepage
    // URLs (shown as short host links) were what forced the horizontal scrollbar.
    QTableWidget* table = new QTableWidget(libCount, 6);
    table->setHorizontalHeaderLabels({ "Library", "Version", "License", "Copyright", "Website", "View License" });
    table->verticalHeader()->setVisible(false);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionMode(QAbstractItemView::NoSelection);
    table->setFocusPolicy(Qt::NoFocus);
    table->setShowGrid(false);
    table->setAlternatingRowColors(true);   // match the app's other tables
    table->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    table->setMinimumWidth(860);             // drives the fixed dialog width

    QHeaderView* hdr = table->horizontalHeader();
    hdr->setSectionResizeMode(0, QHeaderView::Stretch);            // Library absorbs slack
    for (int c = 1; c < 6; ++c)
        hdr->setSectionResizeMode(c, QHeaderView::ResizeToContents);

    for (int row = 0; row < libCount; ++row) {
        const Lib& lib = libs[row];

        QTableWidgetItem* nameItem = new QTableWidgetItem(lib.name);
        QFont nf = nameItem->font();
        nf.setBold(true);
        nameItem->setFont(nf);
        table->setItem(row, 0, nameItem);
        table->setItem(row, 1, new QTableWidgetItem(lib.version));
        table->setItem(row, 2, new QTableWidgetItem(lib.license));
        table->setItem(row, 3, new QTableWidgetItem(lib.copyright));

        QLabel* linkLbl = new QLabel(
            QString("<a href=\"%1\">%2</a>").arg(lib.url, lib.linkText));
        linkLbl->setOpenExternalLinks(true);
        linkLbl->setContentsMargins(4, 0, 24, 0);
        // Rich-text labels report a sizeHint a hair too narrow, so ResizeToContents
        // sizes the column just short of the widest link and clips its last glyph
        // ("qcustomplot.com" → "qcustomplot.con"). Pin a plain-text-measured minimum
        // width (+ margins + a little slack) so the text always fits.
        linkLbl->setMinimumWidth(
            linkLbl->fontMetrics().horizontalAdvance(lib.linkText) + 4 + 24 + 6);
        table->setCellWidget(row, 4, linkLbl);

        QToolButton* viewBtn = new QToolButton;
        viewBtn->setAutoRaise(true);
        viewBtn->setCursor(Qt::PointingHandCursor);
        viewBtn->setToolTip("View license");
        viewBtn->setIcon(adaptThemeIcon(
            QIcon::fromTheme("quickview-symbolic"),
            table->palette().color(QPalette::WindowText),
            style()->standardIcon(QStyle::SP_FileDialogContentsView)));
        const QString title    = QString("%1 — %2").arg(lib.name, lib.license);
        const QString resource = lib.resource;
        connect(viewBtn, &QToolButton::clicked, this, [this, title, resource] {
            showLicenseText(title, resource);
        });
        // Centre the icon button in its cell so the alternating row tint shows
        // around it instead of a full-width button.
        QWidget* viewCell = new QWidget;
        QHBoxLayout* viewLay = new QHBoxLayout(viewCell);
        viewLay->setContentsMargins(0, 0, 0, 0);
        viewLay->addStretch(1);
        viewLay->addWidget(viewBtn);
        viewLay->addStretch(1);
        table->setCellWidget(row, 5, viewCell);
    }

    table->resizeRowsToContents();
    // QLabel cell-widgets and plain QTableWidgetItems report slightly different
    // sizeHints, so resizeRowsToContents() can give rows unequal heights. Clamp
    // everything to the tallest row so the grid looks uniform.
    int rowH = 0;
    for (int r = 0; r < libCount; ++r)
        rowH = qMax(rowH, table->rowHeight(r));
    for (int r = 0; r < libCount; ++r)
        table->setRowHeight(r, rowH);
    // Cap the visible height to ~9 rows; the rest (the KDE closure) scrolls.
    const int visibleRows = qMin(libCount, 9);
    int tableHeight = table->horizontalHeader()->height() + 2 * table->frameWidth()
                      + visibleRows * rowH;
    table->setFixedHeight(tableHeight);
    v->addWidget(table);

    v->addStretch(1);   // keep content top-aligned in the taller About modal
    return page;
}

// About lives in its own modal (opened from the Settings footer's About button)
// rather than a tab, so it can be wider/taller than the cramped settings tabs.
void SettingsDialog::showAboutDialog() {
    QDialog dlg(this);
    dlg.setWindowTitle("About " + QApplication::applicationName());
    // Same recipe as the Settings dialog itself: a fixed-size, plain modal frame
    // (no resize handles / maximize button) sized to its content.
    dlg.setWindowFlags(Qt::Dialog);
    dlg.setWindowModality(Qt::ApplicationModal);

    QVBoxLayout* lay = new QVBoxLayout(&dlg);
    lay->setSizeConstraint(QLayout::SetFixedSize);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addWidget(buildAboutPage(), 1);

    lay->addWidget(horizontalSeparator());
    QHBoxLayout* bottom = new QHBoxLayout;
    bottom->setContentsMargins(12, 8, 12, 12);
    bottom->addStretch(1);
    QPushButton* closeBtn = new QPushButton("Close");
    closeBtn->setDefault(true);
    closeBtn->setIcon(adaptThemeIcon(
        QIcon::fromTheme("dialog-close-symbolic"),
        dlg.palette().color(QPalette::WindowText),
        style()->standardIcon(QStyle::SP_DialogCloseButton)));
    connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::accept);
    bottom->addWidget(closeBtn);
    lay->addLayout(bottom);

    dlg.exec();
}

void SettingsDialog::showLicenseText(const QString& title, const QString& resourcePath) {
    QDialog dlg(this);
    dlg.setWindowTitle(title);
    // Plain, fixed-size modal frame, same as the Settings/About modals (the text
    // browser's own scrollbar handles long license texts).
    dlg.setWindowFlags(Qt::Dialog);
    dlg.setWindowModality(Qt::ApplicationModal);
    dlg.setFixedSize(660, 560);

    QVBoxLayout* lay = new QVBoxLayout(&dlg);

    QTextBrowser* browser = new QTextBrowser;
    browser->setOpenExternalLinks(true);
    browser->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));

    QFile f(resourcePath);
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&f);
        browser->setPlainText(in.readAll());
    } else {
        browser->setPlainText("License text could not be loaded (" + resourcePath + ").");
    }
    lay->addWidget(browser);

    QWidget* btnRow = new QWidget;
    QHBoxLayout* btnLay = new QHBoxLayout(btnRow);
    btnLay->setContentsMargins(0, 0, 0, 0);

    QPushButton* copyBtn = new QPushButton("Copy");
    copyBtn->setIcon(adaptThemeIcon(
        QIcon::fromTheme("edit-copy-symbolic"),
        dlg.palette().color(QPalette::WindowText),
        style()->standardIcon(QStyle::SP_FileDialogContentsView)));
    connect(copyBtn, &QPushButton::clicked, browser, [browser] {
        QApplication::clipboard()->setText(browser->toPlainText());
    });

    QPushButton* closeBtn = new QPushButton("Close");
    closeBtn->setDefault(true);
    closeBtn->setIcon(adaptThemeIcon(
        QIcon::fromTheme("dialog-close-symbolic"),
        dlg.palette().color(QPalette::WindowText),
        style()->standardIcon(QStyle::SP_DialogCloseButton)));
    connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::accept);

    btnLay->addWidget(copyBtn);
    btnLay->addStretch(1);
    btnLay->addWidget(closeBtn);
    lay->addWidget(btnRow);

    dlg.exec();
}
