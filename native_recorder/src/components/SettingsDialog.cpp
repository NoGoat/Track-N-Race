#include "SettingsDialog.h"
#include "../MainWindow.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QFrame>
#include <QLabel>
#include <QCheckBox>
#include <QRadioButton>
#include <QComboBox>
#include <QSlider>
#include <QStyleFactory>
#include <QPushButton>
#include <QFileDialog>
#include <QSizePolicy>
#include <QFont>
#include <QTabBar>
#include <QStackedWidget>
#include <QPalette>
#include <QApplication>
#include <QTableWidget>
#include <QHeaderView>
#include <QTextBrowser>
#include <QDialogButtonBox>
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
    // A QTabBar + QStackedWidget (rather than a QTabWidget) so the shared Close
    // button can live inside the same bordered pane as the tab pages — a
    // QTabWidget has no shared footer area.
    QTabBar*        tabBar = new QTabBar;
    QStackedWidget* stack  = new QStackedWidget;
    tabBar->setExpanding(true);   // tabs stretch to fill the full width
    tabBar->setDrawBase(true);    // native base line divides the bar from the pane

    struct Page { const char* title; QWidget* widget; };
    const Page pages[] = {
        { "Recording",     buildRecordingPage()     },
        { "Appearance",    buildAppearancePage()    },
        { "Notifications", buildNotificationsPage() },
        { "Overview",      buildOverviewPage()      },
        { "Track Map",     buildTrackMapPage()      },
    };
    for (const Page& p : pages) {
        tabBar->addTab(p.title);
        stack->addWidget(p.widget);
    }
    connect(tabBar, &QTabBar::currentChanged, stack, &QStackedWidget::setCurrentIndex);
    tabBar->setCurrentIndex(0);

    // Bordered pane holding the page stack + the Close button row, so the button
    // sits inside the frame for visual consistency with the content.
    QFrame* pane = new QFrame;
    pane->setFrameShape(QFrame::StyledPanel);
    // Fill the pane with the same colour the style paints behind the selected
    // tab (QPalette::Window) so the bar and pane read as one seamless surface.
    pane->setBackgroundRole(QPalette::Window);
    pane->setAutoFillBackground(true);
    QVBoxLayout* paneLay = new QVBoxLayout(pane);
    paneLay->setContentsMargins(0, 0, 0, 0);
    paneLay->setSpacing(0);
    paneLay->addWidget(stack, 1);
    paneLay->addWidget(horizontalSeparator());

    QHBoxLayout* bottom = new QHBoxLayout;
    bottom->setContentsMargins(12, 8, 12, 12);
    QPushButton* aboutBtn = new QPushButton("About");
    connect(aboutBtn, &QPushButton::clicked, this, &SettingsDialog::showAboutDialog);
    bottom->addWidget(aboutBtn);
    bottom->addStretch(1);
    QPushButton* closeBtn = new QPushButton("Close");
    closeBtn->setDefault(true);
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

    // name, version, license label, homepage URL, short link text, license resource.
    struct Lib { const char* name; QString version; const char* license;
                 const char* url; const char* linkText; const char* resource; };
    const Lib libs[] = {
        { "Qt",            qVersion(), "LGPL v3",    "https://www.qt.io",                "qt.io",         ":/licenses/LGPL-3.0.txt" },
        { "QCustomPlot",   "2.1.1",    "GPL v3",     "https://www.qcustomplot.com",      "qcustomplot.com", ":/licenses/GPL-3.0.txt" },
        { "nlohmann/json", "3.11.3",   "MIT",        "https://github.com/nlohmann/json", "github.com",    ":/licenses/MIT.txt"      },
        { "zlib",          "1.3.1",    "zlib",       "https://zlib.net",                 "zlib.net",      ":/licenses/Zlib.txt"     },
        { "KDE Frameworks / Breeze", "6", "LGPL v2.1+", "https://kde.org",               "kde.org",       ":/licenses/LGPL-2.1.txt" },
    };
    const int libCount = int(std::size(libs));

    // A table keeps the columns aligned and width under control; the long homepage
    // URLs (shown as short host links) were what forced the horizontal scrollbar.
    QTableWidget* table = new QTableWidget(libCount, 5);
    table->setHorizontalHeaderLabels({ "Library", "Version", "License", "Website", QString() });
    table->verticalHeader()->setVisible(false);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionMode(QAbstractItemView::NoSelection);
    table->setFocusPolicy(Qt::NoFocus);
    table->setShowGrid(false);
    table->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    QHeaderView* hdr = table->horizontalHeader();
    hdr->setSectionResizeMode(0, QHeaderView::Stretch);            // Library absorbs slack
    for (int c = 1; c < 5; ++c)
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

        QLabel* linkLbl = new QLabel(
            QString("<a href=\"%1\">%2</a>").arg(lib.url, lib.linkText));
        linkLbl->setOpenExternalLinks(true);
        linkLbl->setContentsMargins(4, 0, 8, 0);
        table->setCellWidget(row, 3, linkLbl);

        QPushButton* viewBtn = new QPushButton("View");
        const QString title    = QString("%1 — %2").arg(lib.name, lib.license);
        const QString resource = lib.resource;
        connect(viewBtn, &QPushButton::clicked, this, [this, title, resource] {
            showLicenseText(title, resource);
        });
        table->setCellWidget(row, 4, viewBtn);
    }

    table->resizeRowsToContents();
    // Show every row without an inner scrollbar (header + rows + a little slack).
    int tableHeight = table->horizontalHeader()->height() + 2 * table->frameWidth();
    for (int row = 0; row < libCount; ++row)
        tableHeight += table->rowHeight(row);
    table->setFixedHeight(tableHeight);
    v->addWidget(table);

    // KDE/Breeze ships only with the optional bundled Breeze style.
    QLabel* breezeNote = new QLabel(
        "KDE Frameworks / Breeze are included only when the optional Breeze style is bundled.");
    breezeNote->setWordWrap(true);
    QFont noteFont = breezeNote->font();
    noteFont.setItalic(true);
    breezeNote->setFont(noteFont);
    v->addWidget(breezeNote);

    v->addStretch(1);   // keep content top-aligned in the taller About modal
    return page;
}

// About lives in its own modal (opened from the Settings footer's About button)
// rather than a tab, so it can be wider/taller than the cramped settings tabs.
void SettingsDialog::showAboutDialog() {
    QDialog dlg(this);
    dlg.setWindowTitle("About " + QApplication::applicationName());
    dlg.setWindowModality(Qt::ApplicationModal);
    dlg.resize(680, 600);

    QVBoxLayout* lay = new QVBoxLayout(&dlg);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addWidget(buildAboutPage(), 1);

    lay->addWidget(horizontalSeparator());
    QHBoxLayout* bottom = new QHBoxLayout;
    bottom->setContentsMargins(12, 8, 12, 12);
    bottom->addStretch(1);
    QPushButton* closeBtn = new QPushButton("Close");
    closeBtn->setDefault(true);
    connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::accept);
    bottom->addWidget(closeBtn);
    lay->addLayout(bottom);

    dlg.exec();
}

void SettingsDialog::showLicenseText(const QString& title, const QString& resourcePath) {
    QDialog dlg(this);
    dlg.setWindowTitle(title);
    dlg.setWindowModality(Qt::ApplicationModal);
    dlg.resize(660, 560);

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

    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::accept);
    lay->addWidget(buttons);

    dlg.exec();
}
