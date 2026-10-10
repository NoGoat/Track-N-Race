#include "SettingsDialog.h"
#include "../ChartGraphicsBackend.h"
#include "../Diagnostics.h"
#include "../MainWindow.h"
#include "../CompactSettings.h"
#include "../GraphViewSettings.h"
#include "../IconUtils.h"
#include "../PresentationScheduler.h"
#include "AnalyzeMetrics.h"
#include "PairingQrCode.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGridLayout>
#include <QFrame>
#include <QGroupBox>
#include <QLabel>
#include <QCheckBox>
#include <QRadioButton>
#include <QComboBox>
#include <QColorDialog>
#include <QSlider>
#include <QSpinBox>
#include <QLineEdit>
#include <QStyleFactory>
#include <QPushButton>
#include <QStyleOptionButton>
#include <QStylePainter>
#include <QFileDialog>
#include <QSizePolicy>
#include <QFont>
#include <QButtonGroup>
#include <QStackedWidget>
#include <QScrollArea>
#include <QListWidget>
#include <QPalette>
#include <QApplication>
#include <QDesktopServices>
#include <QAbstractItemView>
#include <QTableWidget>
#include <QHeaderView>
#include <QIcon>
#include <QMessageBox>
#include <QToolButton>
#include <QStyle>
#include <QTextBrowser>
#include <QClipboard>
#include <QFile>
#include <QTextStream>
#include <QFontDatabase>
#include <QRegularExpression>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QSignalBlocker>
#include <QTimer>
#include <QUrl>
#include <QAbstractButton>
#include <algorithm>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <utility>

static QFrame* horizontalSeparator() {
    QFrame* f = new QFrame;
    f->setFrameShape(QFrame::HLine);
    f->setFrameShadow(QFrame::Sunken);
    return f;
}

static QFrame* verticalSeparator() {
    QFrame* f = new QFrame;
    f->setFrameShape(QFrame::VLine);
    f->setFrameShadow(QFrame::Sunken);
    return f;
}

// Segmented-control button, identical to the toolbar's window-size picker (see
// SegmentButton in AppToolbar.cpp). When checked it paints itself as the active
// style's *default button* — the same blue outline the on-toggles wear — which a
// QToolButton can't get for free: the DefaultButton look lives on
// QStyleOptionButton, which only QPushButton feeds the style, so for the checked
// state we draw a default QPushButton bevel + label ourselves. Unchecked
// segments fall through to the normal flat auto-raised look.
namespace {
class SegmentButton : public QToolButton {
public:
    using QToolButton::QToolButton;

protected:
    void paintEvent(QPaintEvent* e) override {
        if (!isChecked()) { QToolButton::paintEvent(e); return; }
        QStylePainter p(this);
        QStyleOptionButton opt;
        opt.initFrom(this);
        opt.rect = rect();
        opt.text = text();
        opt.features = QStyleOptionButton::DefaultButton;
        opt.state |= QStyle::State_Raised;
        opt.state &= ~(QStyle::State_On | QStyle::State_Sunken);
        p.drawControl(QStyle::CE_PushButton, opt);
    }
};

// An exclusive row of SegmentButtons; ids are the given values.
QWidget* segmented(QButtonGroup*& groupOut, std::initializer_list<std::pair<const char*, int>> options) {
    auto* row = new QWidget;
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* group = new QButtonGroup(row);
    group->setExclusive(true);
    for (const auto& option : options) {
        auto* button = new SegmentButton(row);
        button->setText(QString::fromUtf8(option.first));
        button->setCheckable(true);
        button->setAutoRaise(true);
        group->addButton(button, option.second);
        layout->addWidget(button);
    }
    groupOut = group;
    return row;
}

// Muted, wrapping explanation shown under a control.
QLabel* hint(const QString& text) {
    auto* label = new QLabel(text);
    label->setWordWrap(true);
    label->setForegroundRole(QPalette::PlaceholderText);
    QFont font = label->font();
    font.setPointSizeF(qMax(7.0, font.pointSizeF() * 0.92));
    label->setFont(font);
    return label;
}

// A bold section title spanning the form. Sections after the first get room
// above them, so a page reads as a few clear groups rather than one long list.
void addSection(QFormLayout* form, const QString& title) {
    if (form->rowCount() > 0) form->addItem(new QSpacerItem(0, 14, QSizePolicy::Minimum, QSizePolicy::Fixed));
    auto* heading = new QLabel(title);
    QFont font = heading->font();
    font.setBold(true);
    heading->setFont(font);
    form->addRow(heading);
}

// Lower-cased text of everything a page shows, for the settings search.
QString searchableText(const QWidget* page) {
    QStringList parts;
    for (const QLabel* label : page->findChildren<QLabel*>()) parts << label->text();
    for (const QAbstractButton* button : page->findChildren<QAbstractButton*>()) parts << button->text();
    for (const QGroupBox* box : page->findChildren<QGroupBox*>()) parts << box->title();
    for (const QComboBox* combo : page->findChildren<QComboBox*>())
        for (int i = 0; i < combo->count(); ++i) parts << combo->itemText(i);
    return parts.join(QLatin1Char(' ')).toLower();
}

QIcon categoryIcon(const QWidget* widget, std::initializer_list<const char*> names,
                   QStyle::StandardPixmap fallback) {
    QIcon icon;
    for (const char* name : names) {
        icon = QIcon::fromTheme(QString::fromLatin1(name));
        if (!icon.isNull()) break;
    }
    return adaptThemeIcon(icon, widget->palette().color(QPalette::WindowText),
                          widget->style()->standardIcon(fallback));
}
} // namespace

// Bold label spanning both form columns, for a sub-section header inside a page.
static QLabel* subHeading(const QString& text) {
    QLabel* l = new QLabel(text);
    QFont f = l->font();
    f.setBold(true);
    l->setFont(f);
    return l;
}

static bool isIpv4Address(const QString& value) {
    static const QRegularExpression octet(QStringLiteral("^[0-9]{1,3}$"));
    const QStringList parts = value.trimmed().split('.', Qt::KeepEmptyParts);
    if (parts.size() != 4) return false;
    for (const QString& part : parts) {
        if (!octet.match(part).hasMatch() || part.toInt() > 255) return false;
    }
    return true;
}

SettingsDialog::SettingsDialog(MainWindow* mainWindow, QWidget* parent)
    : QDialog(parent), mainWindow_(mainWindow)
{
    setWindowTitle("Settings");
    // Qt::Dialog gives the plain dialog frame (no minimize/maximize buttons).
    // Modal + parented to MainWindow (see the call site) is what keeps this
    // above MainWindow and blocks it — the window-type flag alone doesn't.
    setWindowFlags(Qt::Dialog);
    setWindowModality(Qt::ApplicationModal);
    setMinimumSize(880, 560);

    auto* main = new QVBoxLayout(this);
    main->setContentsMargins(0, 0, 0, 0);
    main->setSpacing(0);

    // ── Categories ────────────────────────────────────────────────────────
    struct Page {
        const char* title;
        const char* description;
        const char* icon;          // theme icon, with an alternative name
        const char* altIcon;
        QStyle::StandardPixmap fallback;
        QWidget* body;
        bool scrolls;
    };
    const Page pages[] = {
        {"Appearance", "Theme, widget style, tyre display, toolbar and motion.",
         "preferences-desktop-theme", "preferences-desktop-color", QStyle::SP_DesktopIcon,
         buildAppearancePage(), true},
        {"Team Colours", "Colours used for each team's drivers throughout the app.",
         "preferences-desktop-color", "color-management", QStyle::SP_DesktopIcon,
         buildTeamColorsPage(), false},
        {"Layout", "How each page arranges its charts, and the shared tooltip's crosshair.",
         "view-grid", "view-list-icons", QStyle::SP_FileDialogListView,
         buildLayoutPage(), true},
        {"Graphs", "Show each graph as its chart or as a table of the samples behind it.",
         "office-chart-line", "labplot-xy-curve", QStyle::SP_FileDialogContentsView,
         buildGraphsPage(), true},
        {"Y Axis Behavior", "Keep each graph's value axis at its fixed range, or let it follow the values.",
         "transform-move-vertical", "distribute-vertical", QStyle::SP_ArrowUp,
         buildYAxisPage(), true},
        {"Density", "How tightly each part of the interface is packed.",
         "zoom-fit-best", "view-list-details", QStyle::SP_FileDialogDetailedView,
         buildCompactPage(), true},
        {"Rendering", "The graphics API, anti-aliasing and frame rates of the telemetry graphs.",
         "video-display", "preferences-desktop-display", QStyle::SP_ComputerIcon,
         buildRenderingPage(), true},
        {"Track Map", "Driver markers and the look of the circuit outline.",
         "map-flat", "globe", QStyle::SP_DriveNetIcon,
         buildTrackMapPage(), true},
        {"Recording", "Saving sessions to disk as they are driven.",
         "media-record", "document-save", QStyle::SP_DialogSaveButton,
         buildRecordingPage(), true},
        {"Notifications", "Pop-up event messages and update checks.",
         "preferences-desktop-notification", "notifications", QStyle::SP_MessageBoxInformation,
         buildNotificationsPage(), true},
        {"Connection", "The telemetry format and how the game's UDP data is received and forwarded.",
         "network-wired", "preferences-system-network", QStyle::SP_DriveNetIcon,
         buildProtocolPage(), true},
        {"Paired Devices", "Android displays that receive telemetry from this computer.",
         "smartphone", "phone", QStyle::SP_ComputerIcon,
         buildPairingPage(), true},
        {"Diagnostics", "Extra logging for troubleshooting.",
         "tools-report-bug", "debug-run", QStyle::SP_MessageBoxWarning,
         buildDebugPage(), true},
    };

    // Left: search over a category list.
    auto* navColumn = new QWidget;
    navColumn->setFixedWidth(220);
    auto* navLayout = new QVBoxLayout(navColumn);
    navLayout->setContentsMargins(10, 10, 10, 10);
    navLayout->setSpacing(8);
    search_ = new QLineEdit;
    search_->setPlaceholderText("Search settings");
    search_->setClearButtonEnabled(true);
    search_->addAction(categoryIcon(this, {"edit-find"}, QStyle::SP_FileDialogContentsView),
                       QLineEdit::LeadingPosition);
    navLayout->addWidget(search_);
    nav_ = new QListWidget;
    nav_->setFrameShape(QFrame::NoFrame);
    nav_->setIconSize(QSize(22, 22));
    nav_->setUniformItemSizes(true);
    nav_->setSpacing(1);
    nav_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    nav_->viewport()->setAutoFillBackground(false);
    navLayout->addWidget(nav_, 1);
    noMatches_ = hint("No settings match your search.");
    noMatches_->setAlignment(Qt::AlignHCenter);
    noMatches_->hide();
    navLayout->addWidget(noMatches_);

    // Right: the selected page.
    pages_ = new QStackedWidget;
    for (const Page& page : pages) {
        auto* item = new QListWidgetItem(categoryIcon(this, {page.icon, page.altIcon}, page.fallback),
                                         QString::fromUtf8(page.title), nav_);
        item->setSizeHint(QSize(0, 34));
        item->setToolTip(QString::fromUtf8(page.description));
        pageKeywords_ << (QString::fromUtf8(page.title) + QLatin1Char(' ') +
                          QString::fromUtf8(page.description) + QLatin1Char(' ') +
                          searchableText(page.body)).toLower();
        pages_->addWidget(pageFrame(QString::fromUtf8(page.title),
                                    QString::fromUtf8(page.description), page.body,
                                    page.scrolls));
    }
    connect(nav_, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row >= 0) pages_->setCurrentIndex(row);
    });
    connect(search_, &QLineEdit::textChanged, this, &SettingsDialog::filterPages);

    auto* body = new QHBoxLayout;
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);
    body->addWidget(navColumn);
    body->addWidget(verticalSeparator());
    body->addWidget(pages_, 1);
    main->addLayout(body, 1);

    // ── Footer ────────────────────────────────────────────────────────────
    main->addWidget(horizontalSeparator());
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
    main->addLayout(bottom);

    // Reopen at the last size and page.
    const QSettings settings("TrackNRace", "NativeRecorder");
    if (!restoreGeometry(settings.value("settingsDialog/geometry").toByteArray()))
        resize(1040, 700);
    const int lastPage = settings.value("settingsDialog/page", 0).toInt();
    nav_->setCurrentRow(qBound(0, lastPage, nav_->count() - 1));
}

void SettingsDialog::showConnectionPage() {
    const QList<QListWidgetItem*> items = nav_->findItems(QStringLiteral("Connection"), Qt::MatchExactly);
    if (!items.isEmpty()) nav_->setCurrentItem(items.first());
}

void SettingsDialog::done(int result) {
    QSettings settings("TrackNRace", "NativeRecorder");
    settings.setValue("settingsDialog/geometry", saveGeometry());
    settings.setValue("settingsDialog/page", nav_->currentRow());
    QDialog::done(result);
}

QWidget* SettingsDialog::pageFrame(const QString& title, const QString& description,
                                   QWidget* body, bool scrolls) {
    auto* frame = new QWidget;
    auto* layout = new QVBoxLayout(frame);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* header = new QWidget(frame);
    auto* headerLayout = new QVBoxLayout(header);
    headerLayout->setContentsMargins(24, 18, 24, 14);
    headerLayout->setSpacing(4);
    auto* heading = new QLabel(title, header);
    QFont headingFont = heading->font();
    headingFont.setPointSizeF(headingFont.pointSizeF() * 1.4);
    headingFont.setBold(true);
    heading->setFont(headingFont);
    headerLayout->addWidget(heading);
    auto* summary = hint(description);
    summary->setFont(font());
    headerLayout->addWidget(summary);
    layout->addWidget(header);

    if (scrolls) {
        auto* scroll = new QScrollArea(frame);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setWidget(body);
        scroll->viewport()->setAutoFillBackground(false);
        body->setAutoFillBackground(false);
        layout->addWidget(scroll, 1);
    } else {
        layout->addWidget(body, 1);
    }
    return frame;
}

void SettingsDialog::filterPages(const QString& query) {
    const QStringList words = query.toLower().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    int firstVisible = -1;
    for (int row = 0; row < nav_->count(); ++row) {
        const QString& text = pageKeywords_.value(row);
        const bool match = std::all_of(words.cbegin(), words.cend(),
                                       [&](const QString& word) { return text.contains(word); });
        nav_->item(row)->setHidden(!match);
        if (match && firstVisible < 0) firstVisible = row;
    }
    noMatches_->setVisible(firstVisible < 0);
    if (firstVisible >= 0 && nav_->item(nav_->currentRow()) &&
        nav_->item(nav_->currentRow())->isHidden())
        nav_->setCurrentRow(firstVisible);
}

// Page body: one right-aligned label/control form for all of a page's sections.
QWidget* SettingsDialog::makePage(QFormLayout*& formOut) {
    QWidget* page = new QWidget;
    QVBoxLayout* v = new QVBoxLayout(page);
    v->setContentsMargins(24, 4, 24, 20);
    v->setSpacing(0);

    QFormLayout* form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
    form->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);
    form->setRowWrapPolicy(QFormLayout::DontWrapRows);
    form->setHorizontalSpacing(12);
    form->setVerticalSpacing(8);
    v->addLayout(form);
    v->addStretch(1);

    formOut = form;
    return page;
}

QWidget* SettingsDialog::buildAppearancePage() {
    QFormLayout* form;
    QWidget* page = makePage(form);

    addSection(form, "Look");
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

    // Lists the QStyles actually available at runtime, so a bundled "Breeze"
    // only appears once its plugin has loaded. Each item's userData is the
    // lowercased QStyleFactory key (what setStyleName stores); "system" is special.
    styleCombo_ = new QComboBox;
    styleCombo_->addItem("System default", "system");
    const QString curStyle = mainWindow_->currentStyleName();
    for (const QString& key : QStyleFactory::keys()) {
        styleCombo_->addItem(key, key.toLower());
        if (key.compare(curStyle, Qt::CaseInsensitive) == 0)
            styleCombo_->setCurrentIndex(styleCombo_->count() - 1);
    }
    // Connected after populating so the initial selection doesn't re-apply.
    connect(styleCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
        mainWindow_->setStyleName(styleCombo_->currentData().toString());
    });
    form->addRow("Widget style:", styleCombo_);

    addSection(form, "Tyres");
    tyreViewCombo_ = new QComboBox;
    tyreViewCombo_->addItem("Cards",  (int)OverviewLayout::TyreCards);
    tyreViewCombo_->addItem("Graphs", (int)OverviewLayout::TyreCharts);
    tyreViewCombo_->setCurrentIndex(mainWindow_->currentTyreView() == OverviewLayout::TyreCharts ? 1 : 0);
    connect(tyreViewCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
        mainWindow_->setTyreView(tyreViewCombo_->currentData().toInt() == (int)OverviewLayout::TyreCharts
                                     ? OverviewLayout::TyreCharts : OverviewLayout::TyreCards);
    });
    form->addRow("Tyre view:", tyreViewCombo_);
    form->addRow(QString(), hint("How tyre data is displayed in the Overview tab."));
    // Whether the tyre graph shows remaining life (100 - wear) or accumulated wear.
    tyreWearModeCombo_ = new QComboBox;
    tyreWearModeCombo_->addItem("Tyre life", true);
    tyreWearModeCombo_->addItem("Tyre wear", false);
    tyreWearModeCombo_->setCurrentIndex(mainWindow_->tyreGraphLifeMode() ? 0 : 1);
    connect(tyreWearModeCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
        mainWindow_->setTyreGraphLifeMode(tyreWearModeCombo_->currentData().toBool());
    });
    form->addRow("Tyre wear graph:", tyreWearModeCombo_);
    form->addRow(QString(), hint("Whether graphs show remaining tyre life or accumulated wear."));

    addSection(form, "Toolbar");
    toolbarLabelsCheck_ = new QCheckBox("Show text beside toolbar icons");
    toolbarLabelsCheck_->setChecked(mainWindow_->toolbarLabelsEnabled());
    form->addRow("Button labels:", toolbarLabelsCheck_);

    auto* deltaUpdates = new QComboBox;
    deltaUpdates->addItem("Realtime", 0);
    deltaUpdates->addItem("Every 250 ms", 250);
    deltaUpdates->addItem("Every 500 ms", 500);
    deltaUpdates->addItem("Every second", 1000);
    const int deltaIndex = deltaUpdates->findData(mainWindow_->deltaUpdateInterval());
    deltaUpdates->setCurrentIndex(deltaIndex >= 0 ? deltaIndex : 0);
    connect(deltaUpdates, &QComboBox::currentIndexChanged, this,
            [this, deltaUpdates] { mainWindow_->setDeltaUpdateInterval(
                deltaUpdates->currentData().toInt()); });
    form->addRow("Lap delta refresh:", deltaUpdates);
    form->addRow(QString(), hint("How often the toolbar's lap-comparison delta updates."));

    addSection(form, "Motion & Accessibility");
    auto* reduceAnimations = new QCheckBox("Reduce animations");
    reduceAnimations->setChecked(mainWindow_->reduceAnimations());
    connect(reduceAnimations, &QCheckBox::toggled,
            mainWindow_, &MainWindow::setReduceAnimations);
    form->addRow("Motion:", reduceAnimations);
    form->addRow(QString(), hint("Turns off toast fades and track-map interpolation. "
                                 "Live data is not affected."));

    QWidget* contrastRow = new QWidget;
    QHBoxLayout* contrastLayout = new QHBoxLayout(contrastRow);
    contrastLayout->setContentsMargins(0, 0, 0, 0);
    QSlider* contrastSlider = new QSlider(Qt::Horizontal);
    contrastSlider->setRange(100, 2100);
    contrastSlider->setValue((int)(mainWindow_->contrastThreshold() * 100.0f));
    contrastSlider->setMinimumWidth(260);
    QLabel* contrastVal = new QLabel(QString::number(mainWindow_->contrastThreshold(), 'f', 2));
    contrastVal->setMinimumWidth(40);
    contrastLayout->addWidget(contrastSlider, 1);
    contrastLayout->addWidget(contrastVal);
    form->addRow("Contrast threshold:", contrastRow);

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
        const float f = val / 100.0f;
        contrastVal->setText(QString::number(f, 'f', 2));
        mainWindow_->setContrastThreshold(f);
    });
    return page;
}

QWidget* SettingsDialog::buildCompactPage() {
    QFormLayout* form;
    QWidget* page = makePage(form);

    // Every section's segmented control, so the "Set all" buttons can update
    // both the persisted state and what is shown.
    struct Ctl { tnr::CompactSection s; QButtonGroup* group; };
    auto controls = std::make_shared<QList<Ctl>>();

    // Ordinary sections expose Compact / Normal / Spacious; the three Electron
    // integer controls keep their exact specialised levels.
    auto makeControl = [this, controls](tnr::CompactSection s) -> QWidget* {
        QButtonGroup* group = nullptr;
        QWidget* control = nullptr;
        if (s == tnr::CompactSection::OverviewTyres) {
            control = segmented(group, {{"Spacious", 6}, {"Normal", 0}, {"Compact 1", 1},
                                        {"Compact 2", 2}, {"Compact 3", 3}, {"Compact 4", 4},
                                        {"Compact 5", 5}});
            group->button(mainWindow_->tyresCompactLevel())->setChecked(true);
            connect(group, &QButtonGroup::idClicked, this,
                    [this](int level) { mainWindow_->setTyresCompactLevel(level); });
        } else if (s == tnr::CompactSection::SessionWeather) {
            control = segmented(group, {{"Spacious", 4}, {"Normal", 0}, {"Compact 1", 1},
                                        {"Compact 2", 2}, {"Compact 3", 3}});
            group->button(mainWindow_->weatherCompactLevel())->setChecked(true);
            connect(group, &QButtonGroup::idClicked, this,
                    [this](int level) { mainWindow_->setWeatherCompactLevel(level); });
        } else if (s == tnr::CompactSection::SessionHeader) {
            control = segmented(group, {{"Spacious", 3}, {"Normal", 0}, {"Compact 1", 1},
                                        {"Compact 2", 2}});
            group->button(mainWindow_->headerCompactLevel())->setChecked(true);
            connect(group, &QButtonGroup::idClicked, this,
                    [this](int level) { mainWindow_->setHeaderCompactLevel(level); });
        } else {
            control = segmented(group, {{"Compact", static_cast<int>(tnr::DensityMode::Compact)},
                                        {"Normal", static_cast<int>(tnr::DensityMode::Normal)},
                                        {"Spacious", static_cast<int>(tnr::DensityMode::Spacious)}});
            group->button(static_cast<int>(mainWindow_->densitySection(s)))->setChecked(true);
            connect(group, &QButtonGroup::idClicked, this,
                    [this, s](int mode) { mainWindow_->setDensitySection(
                        s, static_cast<tnr::DensityMode>(mode)); });
        }
        controls->push_back({s, group});
        return control;
    };

    // Bulk actions first, so the common case is one click.
    addSection(form, "All Sections");
    auto* bulk = new QWidget;
    auto* bulkLayout = new QHBoxLayout(bulk);
    bulkLayout->setContentsMargins(0, 0, 0, 0);
    bulkLayout->setSpacing(6);
    const struct { const char* label; tnr::DensityMode mode; } bulkActions[] = {
        {"Compact", tnr::DensityMode::Compact},
        {"Normal", tnr::DensityMode::Normal},
        {"Spacious", tnr::DensityMode::Spacious},
    };
    auto setAll = [this, controls](tnr::DensityMode mode) {
        for (const Ctl& c : *controls) {
            if (c.s == tnr::CompactSection::OverviewTyres) {
                const int lvl = mode == tnr::DensityMode::Spacious ? 6
                    : mode == tnr::DensityMode::Compact ? 1 : 0;
                mainWindow_->setTyresCompactLevel(lvl);
                c.group->button(lvl)->setChecked(true);
            } else if (c.s == tnr::CompactSection::SessionWeather) {
                const int lvl = mode == tnr::DensityMode::Spacious ? 4
                    : mode == tnr::DensityMode::Compact ? 1 : 0;
                mainWindow_->setWeatherCompactLevel(lvl);
                c.group->button(lvl)->setChecked(true);
            } else if (c.s == tnr::CompactSection::SessionHeader) {
                const int lvl = mode == tnr::DensityMode::Spacious ? 3
                    : mode == tnr::DensityMode::Compact ? 1 : 0;
                mainWindow_->setHeaderCompactLevel(lvl);
                c.group->button(lvl)->setChecked(true);
            } else {
                mainWindow_->setDensitySection(c.s, mode);
                c.group->button(static_cast<int>(mode))->setChecked(true);
            }
        }
    };
    for (const auto& action : bulkActions) {
        auto* button = new QPushButton(action.label);
        connect(button, &QPushButton::clicked, this, [setAll, mode = action.mode] { setAll(mode); });
        bulkLayout->addWidget(button);
    }
    form->addRow("Set all to:", bulk);

    struct Row { tnr::CompactSection s; const char* group; const char* label; };
    static const Row rows[] = {
        { tnr::CompactSection::OverviewStats,     "Overview",  "Stats row:" },
        { tnr::CompactSection::OverviewDamage,    "Overview",  "Damage cards:" },
        { tnr::CompactSection::OverviewTyres,     "Overview",  "Tyre cards:" },
        { tnr::CompactSection::StandingsTable,    "Standings", "Timing tower:" },
        { tnr::CompactSection::StandingsTiming,   "Standings", "Timing card:" },
        { tnr::CompactSection::StandingsErs,      "Standings", "Energy recovery card:" },
        { tnr::CompactSection::StandingsStrategy, "Standings", "Strategy card:" },
        { tnr::CompactSection::SessionCards,      "Session",   "Info cards:" },
        { tnr::CompactSection::SessionProximity,  "Session",   "Proximity:" },
        { tnr::CompactSection::SessionEvents,     "Session",   "Events:" },
        { tnr::CompactSection::SessionWeather,    "Session",   "Weather strip:" },
        { tnr::CompactSection::SessionHeader,     "Session",   "Header:" },
        { tnr::CompactSection::PowerCards,        "Power",     "Power cards:" },
        { tnr::CompactSection::StrategySummary,   "Strategy",  "Summary header:" },
        { tnr::CompactSection::TrendsSummary,     "Trends",    "Summary cards:" },
        { tnr::CompactSection::DamageSummary,     "Damage",    "Summary cards:" },
        { tnr::CompactSection::PlaybackBar,       "Playback",  "Playback bar:" },
    };
    QString lastGroup;
    for (const Row& row : rows) {
        if (lastGroup != QLatin1String(row.group)) {
            addSection(form, row.group);
            lastGroup = row.group;
        }
        form->addRow(row.label, makeControl(row.s));
    }
    return page;
}

QWidget* SettingsDialog::buildRenderingPage() {
    QFormLayout* form;
    QWidget* page = makePage(form);

    addSection(form, "Rendering");
    chartBackendCombo_ = new QComboBox;
    chartBackendCombo_->addItem(
        QString("Automatic (currently %1)").arg(tnr::graphics::activeBackendLabel()), "auto");
    for (const tnr::graphics::BackendInfo& backend : tnr::graphics::supportedBackends())
        chartBackendCombo_->addItem(backend.label, backend.key);
    const int backendIndex = chartBackendCombo_->findData(mainWindow_->chartGraphicsBackend());
    chartBackendCombo_->setCurrentIndex(backendIndex >= 0 ? backendIndex : 0);
    connect(chartBackendCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
        mainWindow_->setChartGraphicsBackend(chartBackendCombo_->currentData().toString());
    });
    form->addRow("Graphics API:", chartBackendCombo_);
    form->addRow(QString(), hint("Takes effect after restarting the app."));

    // Portable QRhi multisampling: higher values smooth lines but increase fill cost.
    chartMsaaCombo_ = new QComboBox;
    chartMsaaCombo_->addItem("Off", 0);
    for (int s : { 4, 8, 16 })
        chartMsaaCombo_->addItem(QString("%1×").arg(s), s);
    const int msaaIdx = chartMsaaCombo_->findData(mainWindow_->chartMsaaSamples());
    chartMsaaCombo_->setCurrentIndex(msaaIdx >= 0 ? msaaIdx : chartMsaaCombo_->findData(4));
    connect(chartMsaaCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
        mainWindow_->setChartMsaaSamples(chartMsaaCombo_->currentData().toInt());
    });
    form->addRow("Anti-aliasing:", chartMsaaCombo_);

    auto populateFrameRates = [](QComboBox* combo) {
        combo->addItem("Paused", 0);
        combo->addItem("1 FPS", 1);
        combo->addItem("10 FPS", 10);
        combo->addItem("30 FPS", 30);
        combo->addItem("60 FPS", 60);
        combo->addItem("120 FPS", 120);
        combo->addItem("Match display", PresentationScheduler::MatchDisplay);
    };
    chartFpsInFocusCombo_ = new QComboBox;
    populateFrameRates(chartFpsInFocusCombo_);
    int fpsIdx = chartFpsInFocusCombo_->findData(mainWindow_->chartFpsInFocus());
    chartFpsInFocusCombo_->setCurrentIndex(
        fpsIdx >= 0 ? fpsIdx : chartFpsInFocusCombo_->findData(PresentationScheduler::MatchDisplay));
    connect(chartFpsInFocusCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
        mainWindow_->setChartFpsInFocus(chartFpsInFocusCombo_->currentData().toInt());
    });
    form->addRow("Frame rate, focused:", chartFpsInFocusCombo_);

    chartFpsOutOfFocusCombo_ = new QComboBox;
    populateFrameRates(chartFpsOutOfFocusCombo_);
    fpsIdx = chartFpsOutOfFocusCombo_->findData(mainWindow_->chartFpsOutOfFocus());
    chartFpsOutOfFocusCombo_->setCurrentIndex(
        fpsIdx >= 0 ? fpsIdx : chartFpsOutOfFocusCombo_->findData(30));
    connect(chartFpsOutOfFocusCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
        mainWindow_->setChartFpsOutOfFocus(chartFpsOutOfFocusCombo_->currentData().toInt());
    });
    form->addRow("Frame rate, in background:", chartFpsOutOfFocusCombo_);
    form->addRow(QString(), hint("Maximum graph frame rates while the window is focused and "
                                 "while it is not."));

    return page;
}

// Layout: how each app page arranges its charts (Electron's "Layout" category).
QWidget* SettingsDialog::buildLayoutPage() {
    QFormLayout* form;
    QWidget* page = makePage(form);

    auto layoutCombo = [this](MainWindow::Page target) {
        auto* arrangement = new QComboBox;
        arrangement->addItem("Grid", false);
        arrangement->addItem("Vertical", true);
        arrangement->setCurrentIndex(mainWindow_->verticalChartLayout(target) ? 1 : 0);
        connect(arrangement, &QComboBox::currentIndexChanged, this, [this, target, arrangement](int) {
            mainWindow_->setVerticalChartLayout(target, arrangement->currentData().toBool());
        });
        return arrangement;
    };

    addSection(form, "Inputs");
    form->addRow("Chart layout:", layoutCombo(MainWindow::Input));
    form->addRow(QString(), hint("Arrange the Input page as a grid or a vertical stack. Every chart "
                                 "keeps its own selectable horizontal axis."));
    auto* pedals = new QComboBox;
    pedals->addItem("Combined", "combined");
    pedals->addItem("Combined 2", "combined2");
    pedals->addItem("Split", "split");
    pedals->setCurrentIndex(qMax(0, pedals->findData(mainWindow_->inputPedalLayout())));
    connect(pedals, &QComboBox::currentIndexChanged, this, [this, pedals](int) {
        mainWindow_->setInputPedalLayout(pedals->currentData().toString());
    });
    form->addRow("Pedal charts:", pedals);
    form->addRow(QString(), hint("Combined uses a signed centre line; Combined 2 overlays both inputs "
                                 "from 0–100%; Split uses two independent charts."));

    addSection(form, "Misc");
    for (bool gForce : {true, false}) {
        auto* mode = new QComboBox;
        mode->addItem("Combined", false);
        mode->addItem("Split", true);
        mode->setCurrentIndex(mainWindow_->miscSplitLayout(gForce) ? 1 : 0);
        connect(mode, &QComboBox::currentIndexChanged, this, [this, gForce, mode](int) {
            mainWindow_->setMiscSplitLayout(gForce, mode->currentData().toBool());
        });
        form->addRow(gForce ? "G-force charts:" : "Ride height charts:", mode);
        form->addRow(QString(), hint(gForce
            ? "Show lateral and longitudinal G-force together or as two aligned charts."
            : "Show front and rear ride height together or as two aligned charts."));
    }

    addSection(form, "Power");
    form->addRow("Chart layout:", layoutCombo(MainWindow::Power));
    form->addRow(QString(), hint("Arrange the Power charts as a 2×2 grid or an aligned vertical stack."));

    addSection(form, "Tyres");
    form->addRow("Chart layout:", layoutCombo(MainWindow::Tyres));
    form->addRow(QString(), hint("Arrange the Tyres charts as a 2×2 grid or an aligned vertical stack."));

    addSection(form, "Trends");
    auto* trends = new QComboBox;
    trends->addItem("Separate", "separate");
    trends->addItem("Combined", "combined");
    trends->addItem("Combined (without Recharge)", "combinedNoRecharge");
    trends->addItem("Bars", "bars");
    trends->setCurrentIndex(qMax(0, trends->findData(mainWindow_->trendsChartLayout())));
    connect(trends, &QComboBox::currentIndexChanged, this, [this, trends](int) {
        mainWindow_->setTrendsChartLayout(trends->currentData().toString());
    });
    form->addRow("Chart layout:", trends);
    form->addRow(QString(), hint("Show lap times, ERS usage, recharge and tyre wear as separate charts, "
                                 "or together on one chart with a scale for each unit, with or without "
                                 "recharge, or as three bars per lap: average tyre wear, ERS used and lap time."));

    addSection(form, "Shared Tooltip");
    auto* vertical = new QCheckBox("Secondary vertical crosshair");
    auto* horizontal = new QCheckBox("Secondary horizontal crosshair");
    vertical->setChecked(mainWindow_->chartSecondaryVerticalCrosshair());
    horizontal->setChecked(mainWindow_->chartSecondaryHorizontalCrosshair());
    connect(vertical, &QCheckBox::toggled, this, [this, horizontal](bool on) {
        mainWindow_->setChartSecondaryCrosshairs(on, horizontal->isChecked());
    });
    connect(horizontal, &QCheckBox::toggled, this, [this, vertical](bool on) {
        mainWindow_->setChartSecondaryCrosshairs(vertical->isChecked(), on);
    });
    form->addRow("Show:", vertical);
    form->addRow(QString(), horizontal);
    form->addRow(QString(), hint("Draw the cursor lines on synchronized secondary charts. The hovered "
                                 "chart always keeps its normal crosshair."));
    return page;
}

// Graphs: each graph as its chart (or card) or as a table of its samples
// (Electron's "Graphs" category).
QWidget* SettingsDialog::buildGraphsPage() {
    QFormLayout* form;
    QWidget* page = makePage(form);

    struct Ctl { tnr::GraphSection s; QButtonGroup* group; };
    auto controls = std::make_shared<QList<Ctl>>();

    // One button whose label is the action it performs: "Set All Table" while
    // any graph is a chart, otherwise "Set All Chart".
    auto* setAll = new QPushButton;
    auto anyChart = [this, controls] {
        for (const Ctl& c : *controls)
            if (!mainWindow_->graphView(c.s)) return true;
        return false;
    };
    auto refreshSetAll = [setAll, anyChart] { setAll->setText(anyChart() ? "Set All Table" : "Set All Chart"); };
    form->addRow(QString(), setAll);

    struct Row { tnr::GraphSection s; const char* label; bool card; };
    struct Group { const char* title; QVector<Row> rows; };
    const Group groups[] = {
        {"Overview", {
            { tnr::GraphSection::OverviewTelemetry,   "Speed / RPM / ERS:", false },
            { tnr::GraphSection::OverviewTyreSurface, "Tyre surface temp:", false },
            { tnr::GraphSection::OverviewTyreInner,   "Tyre inner temp:", false },
            { tnr::GraphSection::OverviewTyreBrake,   "Brake temp:", false },
            { tnr::GraphSection::OverviewTyreWear,    "Tyre wear / life:", false },
            { tnr::GraphSection::OverviewTyreCardFL,  "Tyre card FL:", true },
            { tnr::GraphSection::OverviewTyreCardFR,  "Tyre card FR:", true },
            { tnr::GraphSection::OverviewTyreCardRL,  "Tyre card RL:", true },
            { tnr::GraphSection::OverviewTyreCardRR,  "Tyre card RR:", true }}},
        {"Tyres", {
            { tnr::GraphSection::TyreSurface, "Tyre surface temp:", false },
            { tnr::GraphSection::TyreInner,   "Tyre inner temp:", false },
            { tnr::GraphSection::TyreBrake,   "Brake temp:", false },
            { tnr::GraphSection::TyreWear,    "Tyre wear / life:", false },
            { tnr::GraphSection::TyreCardFL,  "Tyre card FL:", true },
            { tnr::GraphSection::TyreCardFR,  "Tyre card FR:", true },
            { tnr::GraphSection::TyreCardRL,  "Tyre card RL:", true },
            { tnr::GraphSection::TyreCardRR,  "Tyre card RR:", true }}},
        {"Input", {
            { tnr::GraphSection::InputGear,                 "Gear:", false },
            { tnr::GraphSection::InputThrottleBrake,        "Accelerator / brake:", false },
            { tnr::GraphSection::InputThrottleBrakeOverlay, "Accelerator / brake (Combined 2):", false },
            { tnr::GraphSection::InputAccelerator,          "Accelerator (Split):", false },
            { tnr::GraphSection::InputBrake,                "Brake (Split):", false },
            { tnr::GraphSection::InputSteering,             "Steering:", false }}},
        {"Power", {
            { tnr::GraphSection::PowerSplit,   "Power:", false },
            { tnr::GraphSection::PowerHarvest, "ERS harvest:", false },
            { tnr::GraphSection::PowerStore,   "ERS store:", false },
            { tnr::GraphSection::PowerFuel,    "Fuel history:", false }}},
        {"Misc", {
            { tnr::GraphSection::MiscGForce,        "G-force:", false },
            { tnr::GraphSection::MiscGLateral,      "G-force — lateral (Split):", false },
            { tnr::GraphSection::MiscGLongitudinal, "G-force — longitudinal (Split):", false },
            { tnr::GraphSection::MiscRideHeight,    "Ride height:", false },
            { tnr::GraphSection::MiscRideFront,     "Ride height — front (Split):", false },
            { tnr::GraphSection::MiscRideRear,      "Ride height — rear (Split):", false }}},
    };
    for (const Group& group : groups) {
        addSection(form, group.title);
        for (const Row& row : group.rows) {
            QButtonGroup* view = nullptr;
            form->addRow(row.label, segmented(view, {{row.card ? "Card" : "Chart", 0}, {"Table", 1}}));
            view->button(mainWindow_->graphView(row.s) ? 1 : 0)->setChecked(true);
            connect(view, &QButtonGroup::idClicked, this, [this, s = row.s, refreshSetAll](int id) {
                mainWindow_->setGraphView(s, id == 1);
                refreshSetAll();
            });
            controls->push_back({row.s, view});
        }
    }
    connect(setAll, &QPushButton::clicked, this, [this, controls, anyChart, refreshSetAll] {
        const bool table = anyChart();
        for (const Ctl& c : *controls) {
            mainWindow_->setGraphView(c.s, table);
            c.group->button(table ? 1 : 0)->setChecked(true);
        }
        refreshSetAll();
    });
    refreshSetAll();
    return page;
}

// Y Axis Behavior: whether a graph's value axis keeps its fixed range or
// follows the values on screen (Electron's "Y Axis Behavior" category).
QWidget* SettingsDialog::buildYAxisPage() {
    QFormLayout* form;
    QWidget* page = makePage(form);

    // An empty scale is a live chart section; otherwise an Analysis axis scale.
    struct Ctl { tnr::GraphSection s; QString scale; QButtonGroup* group; };
    auto controls = std::make_shared<QList<Ctl>>();

    auto* setAll = new QPushButton;
    auto isDynamic = [this](const Ctl& c) {
        return c.scale.isEmpty() ? mainWindow_->chartDynamicYAxis(c.s)
                                 : mainWindow_->chartAnalysisDynamicYAxis(c.scale);
    };
    auto setDynamic = [this](const Ctl& c, bool dynamic) {
        if (c.scale.isEmpty()) mainWindow_->setChartDynamicYAxis(c.s, dynamic);
        else mainWindow_->setChartAnalysisDynamicYAxis(c.scale, dynamic);
    };
    auto anyDynamic = [controls, isDynamic] {
        for (const Ctl& c : *controls)
            if (isDynamic(c)) return true;
        return false;
    };
    auto refreshSetAll = [setAll, anyDynamic] { setAll->setText(anyDynamic() ? "Set All Fixed" : "Set All Dynamic"); };
    form->addRow(QString(), setAll);

    struct Row { tnr::GraphSection s; const char* label; const char* fixedRange; };
    struct Group { const char* title; QVector<Row> rows; };
    const char* tyreTemp = "0–125°C; expands above 125°C when needed";
    const char* brakeTemp = "0–1250°C; expands above 1250°C when needed";
    const char* wear = "Always 0–100%";
    const Group groups[] = {
        {"Overview", {
            { tnr::GraphSection::OverviewTyreSurface, "Surface temp:", tyreTemp },
            { tnr::GraphSection::OverviewTyreInner,   "Inner temp:", tyreTemp },
            { tnr::GraphSection::OverviewTyreBrake,   "Brake temp:", brakeTemp },
            { tnr::GraphSection::OverviewTyreWear,    "Tyre wear / life:", wear }}},
        {"Tyres", {
            { tnr::GraphSection::TyreSurface, "Surface temp:", tyreTemp },
            { tnr::GraphSection::TyreInner,   "Inner temp:", tyreTemp },
            { tnr::GraphSection::TyreBrake,   "Brake temp:", brakeTemp },
            { tnr::GraphSection::TyreWear,    "Tyre wear / life:", wear }}},
        {"Power", {
            { tnr::GraphSection::PowerHarvest, "ERS harvest:",
              "0–4000/8000 kJ by Formula; expands above when needed" }}},
    };
    for (const Group& group : groups) {
        addSection(form, group.title);
        for (const Row& row : group.rows) {
            QButtonGroup* axis = nullptr;
            form->addRow(row.label, segmented(axis, {{"Fixed", 0}, {"Dynamic", 1}}));
            form->addRow(QString(), hint(QStringLiteral("Fixed: %1").arg(QString::fromUtf8(row.fixedRange))));
            axis->button(mainWindow_->chartDynamicYAxis(row.s) ? 1 : 0)->setChecked(true);
            connect(axis, &QButtonGroup::idClicked, this, [this, s = row.s, refreshSetAll](int id) {
                mainWindow_->setChartDynamicYAxis(s, id == 1);
                refreshSetAll();
            });
            controls->push_back({row.s, QString(), axis});
        }
    }
    addSection(form, "Analysis");
    for (const AnalyzeScale& scale : analyzeScales()) {
        QButtonGroup* axis = nullptr;
        form->addRow(scale.label, segmented(axis, {{"Fixed", 0}, {"Dynamic", 1}}));
        form->addRow(QString(), hint(QStringLiteral("Fixed: %1").arg(scale.fixedRange)));
        const Ctl ctl{tnr::GraphSection::Count_, scale.key, axis};
        axis->button(isDynamic(ctl) ? 1 : 0)->setChecked(true);
        connect(axis, &QButtonGroup::idClicked, this, [setDynamic, ctl, refreshSetAll](int id) {
            setDynamic(ctl, id == 1);
            refreshSetAll();
        });
        controls->push_back(ctl);
    }
    connect(setAll, &QPushButton::clicked, this, [controls, anyDynamic, setDynamic, refreshSetAll] {
        const bool dynamic = !anyDynamic();
        for (const Ctl& c : *controls) {
            setDynamic(c, dynamic);
            c.group->button(dynamic ? 1 : 0)->setChecked(true);
        }
        refreshSetAll();
    });
    refreshSetAll();
    return page;
}

QWidget* SettingsDialog::buildTrackMapPage() {
    QFormLayout* form;
    QWidget* page = makePage(form);

    addSection(form, "Drivers");
    trackMapLabelsCombo_ = new QComboBox;
    trackMapLabelsCombo_->addItem("Dots and labels", 0);
    trackMapLabelsCombo_->addItem("Dots only", 1);
    trackMapLabelsCombo_->addItem("Labels only", 2);
    trackMapLabelsCombo_->setCurrentIndex(mainWindow_->trackMapLabelMode());
    connect(trackMapLabelsCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
        mainWindow_->setTrackMapLabelMode(trackMapLabelsCombo_->currentData().toInt());
    });
    form->addRow("Markers:", trackMapLabelsCombo_);

    // Hide drivers idle for longer than the selected duration (0 = never).
    trackMapIdleCombo_ = new QComboBox;
    trackMapIdleCombo_->addItem("Never", 0);
    for (int s : { 3, 5, 10, 15, 30 })
        trackMapIdleCombo_->addItem(QString("After %1 s").arg(s), s);
    const int idleIdx = trackMapIdleCombo_->findData(mainWindow_->trackMapIdleTimeout());
    trackMapIdleCombo_->setCurrentIndex(idleIdx >= 0 ? idleIdx : 0);
    connect(trackMapIdleCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
        mainWindow_->setTrackMapIdleTimeout(trackMapIdleCombo_->currentData().toInt());
    });
    form->addRow("Hide stationary drivers:", trackMapIdleCombo_);

    addSection(form, "Circuit");
    trackMapSectorColorsCheck_ = new QCheckBox("Colour each sector");
    trackMapSectorColorsCheck_->setChecked(mainWindow_->trackMapSectorColors());
    connect(trackMapSectorColorsCheck_, &QCheckBox::toggled, this, [this](bool on) {
        mainWindow_->setTrackMapSectorColors(on);
    });
    form->addRow("Outline:", trackMapSectorColorsCheck_);

    // Track-outline opacity (20–100%); driver dots/labels stay full strength.
    auto* opacityRow = new QWidget;
    auto* opacityLayout = new QHBoxLayout(opacityRow);
    opacityLayout->setContentsMargins(0, 0, 0, 0);
    trackMapOpacitySlider_ = new QSlider(Qt::Horizontal);
    trackMapOpacitySlider_->setRange(20, 100);
    trackMapOpacitySlider_->setMinimumWidth(260);
    trackMapOpacitySlider_->setValue(mainWindow_->trackMapOpacity());
    auto* opacityValue = new QLabel(QStringLiteral("%1%").arg(mainWindow_->trackMapOpacity()));
    opacityValue->setMinimumWidth(40);
    opacityLayout->addWidget(trackMapOpacitySlider_, 1);
    opacityLayout->addWidget(opacityValue);
    connect(trackMapOpacitySlider_, &QSlider::valueChanged, this, [this, opacityValue](int v) {
        opacityValue->setText(QStringLiteral("%1%").arg(v));
        mainWindow_->setTrackMapOpacity(v);
    });
    form->addRow("Outline opacity:", opacityRow);
    form->addRow(QString(), hint("Driver markers always stay fully opaque."));
    return page;
}

QWidget* SettingsDialog::buildRecordingPage() {
    QFormLayout* form;
    QWidget* page = makePage(form);

    addSection(form, "Recording");
    recordCheck_ = new QCheckBox("Start recording when a session starts");
    recordCheck_->setChecked(mainWindow_->autoRecordEnabled());
    form->addRow("Automatic:", recordCheck_);

    QWidget* dirRow = new QWidget;
    QHBoxLayout* dirLay = new QHBoxLayout(dirRow);
    dirLay->setContentsMargins(0, 0, 0, 0);
    const QString dir = mainWindow_->currentOutputDirectory();
    dirLabel_ = new QLabel(dir.isEmpty() ? "No folder selected" : dir);
    dirLabel_->setWordWrap(true);
    dirLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    dirLabel_->setMinimumWidth(260);
    dirLabel_->setMaximumWidth(420);
    QPushButton* browseBtn = new QPushButton("Change…");
    browseBtn->setIcon(adaptThemeIcon(QIcon::fromTheme("document-open-folder"),
                                      palette().color(QPalette::WindowText),
                                      style()->standardIcon(QStyle::SP_DirOpenIcon)));
    dirLay->addWidget(dirLabel_, 1);
    dirLay->addWidget(browseBtn);
    form->addRow("Save to:", dirRow);

    connect(recordCheck_, &QCheckBox::toggled, this, [this](bool on) {
        mainWindow_->setAutoRecord(on);
    });
    connect(browseBtn, &QPushButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(
            this, "Select Output Directory", mainWindow_->lastDialogDirectory());
        if (!dir.isEmpty()) {
            mainWindow_->rememberDialogDirectory(dir, true);
            mainWindow_->setOutputDirectory(dir);
            dirLabel_->setText(dir);
        }
    });

    addSection(form, "Drivers to Save");
    struct Category { const char* key; const char* label; };
    for (const Category& category : { Category{"practice", "Practice Sessions:"},
                                      Category{"qualifying", "Qualifying Sessions:"},
                                      Category{"race", "Race Sessions:"},
                                      Category{"time_trial", "Time Trial Sessions:"} }) {
        auto* combo = new QComboBox;
        combo->addItem("All Drivers", QStringLiteral("all_drivers"));
        combo->addItem("Driver Only", QStringLiteral("driver_only"));
        combo->addItem("Both", QStringLiteral("both"));
        combo->addItem("Ask", QStringLiteral("ask"));
        const QString key = QString::fromLatin1(category.key);
        const int current = combo->findData(mainWindow_->recordingScope(key));
        combo->setCurrentIndex(current >= 0 ? current : 0);
        form->addRow(category.label, combo);
        connect(combo, &QComboBox::currentIndexChanged, this, [this, combo, key](int) {
            mainWindow_->setRecordingScope(key, combo->currentData().toString());
        });
    }
    return page;
}

QWidget* SettingsDialog::buildNotificationsPage() {
    QFormLayout* form;
    QWidget* page = makePage(form);

    addSection(form, "Event Notifications");
    toastsCheck_ = new QCheckBox("Show pop-ups for penalties, flags, fastest laps…");
    toastsCheck_->setChecked(mainWindow_->toastsEnabled());
    form->addRow("Pop-ups:", toastsCheck_);

    toastDurationCombo_ = new QComboBox;
    for (int s : { 2, 3, 5, 8, 10 })
        toastDurationCombo_->addItem(QString("%1 seconds").arg(s), s);
    const int cur = toastDurationCombo_->findData(mainWindow_->toastDurationSecs());
    toastDurationCombo_->setCurrentIndex(cur >= 0 ? cur : 1);   // default 3s
    toastDurationCombo_->setEnabled(toastsCheck_->isChecked());
    form->addRow("Keep visible for:", toastDurationCombo_);

    connect(toastsCheck_, &QCheckBox::toggled, this, [this](bool on) {
        mainWindow_->setToastsEnabled(on);
        toastDurationCombo_->setEnabled(on);
    });
    connect(toastDurationCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
        mainWindow_->setToastDurationSecs(toastDurationCombo_->currentData().toInt());
    });

    addSection(form, "Updates");
    auto* updates = new QCheckBox("Check GitHub for a new version at startup");
    updates->setChecked(mainWindow_->updateChecksEnabled());
    connect(updates, &QCheckBox::toggled,
            mainWindow_, &MainWindow::setUpdateChecksEnabled);
    form->addRow("Updates:", updates);
    form->addRow(QString(), hint("Checks at most once every 24 hours."));
    return page;
}

QWidget* SettingsDialog::buildPairingPage() {
    QWidget* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(24, 4, 24, 20);
    layout->setSpacing(12);

    pairingEnabledCheck_ = new QCheckBox("Enable paired mode", page);
    QFont enabledFont = pairingEnabledCheck_->font();
    enabledFont.setBold(true);
    pairingEnabledCheck_->setFont(enabledFont);
    layout->addWidget(pairingEnabledCheck_);
    auto* description = new QLabel(
        "Let Android displays discover this computer and receive decoded "
        "telemetry over the local network.", page);
    description->setWordWrap(true);
    description->setStyleSheet("color: palette(placeholder-text);");
    layout->addWidget(description);

    pairingContent_ = new QWidget(page);
    auto* content = new QVBoxLayout(pairingContent_);
    content->setContentsMargins(0, 4, 0, 0);
    content->setSpacing(12);

    auto* pairBox = new QGroupBox("Add an Android display", pairingContent_);
    auto* pairBoxLayout = new QVBoxLayout(pairBox);
    pairBoxLayout->setContentsMargins(12, 12, 12, 12);

    pairingClosed_ = new QWidget(pairBox);
    auto* closedLayout = new QHBoxLayout(pairingClosed_);
    closedLayout->setContentsMargins(0, 0, 0, 0);
    auto* closedText = new QLabel(
        "Opens discovery, QR, and matching-code pairing for two minutes.",
        pairingClosed_);
    closedText->setWordWrap(true);
    auto* pairButton = new QPushButton("Pair a device", pairingClosed_);
    closedLayout->addWidget(closedText, 1);
    closedLayout->addWidget(pairButton);
    pairBoxLayout->addWidget(pairingClosed_);

    pairingOpen_ = new QWidget(pairBox);
    auto* openLayout = new QHBoxLayout(pairingOpen_);
    openLayout->setContentsMargins(0, 0, 0, 0);
    openLayout->setSpacing(18);
    pairingQrLabel_ = new QLabel(pairingOpen_);
    pairingQrLabel_->setFixedSize(260, 260);
    pairingQrLabel_->setAlignment(Qt::AlignCenter);
    pairingQrLabel_->setStyleSheet(
        "background: white; color: #333; border: 1px solid palette(mid); "
        "border-radius: 6px;");
    openLayout->addWidget(pairingQrLabel_);
    auto* codeColumn = new QWidget(pairingOpen_);
    auto* codeLayout = new QVBoxLayout(codeColumn);
    codeLayout->setContentsMargins(0, 10, 0, 10);
    auto* codeHeading = new QLabel("MATCHING CODE", codeColumn);
    QFont headingFont = codeHeading->font();
    headingFont.setBold(true);
    headingFont.setLetterSpacing(QFont::AbsoluteSpacing, 1.5);
    codeHeading->setFont(headingFont);
    pairingCodeLabel_ = new QLabel(codeColumn);
    QFont codeFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    codeFont.setPointSize(24);
    codeFont.setBold(true);
    codeFont.setLetterSpacing(QFont::AbsoluteSpacing, 5);
    pairingCodeLabel_->setFont(codeFont);
    pairingCodeLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto* instructions = new QLabel(
        "Scan the QR in Android Settings, or select this desktop and enter "
        "the same code.", codeColumn);
    instructions->setWordWrap(true);
    auto* cancelButton = new QPushButton("Cancel pairing", codeColumn);
    codeLayout->addWidget(codeHeading);
    codeLayout->addWidget(pairingCodeLabel_);
    codeLayout->addSpacing(6);
    codeLayout->addWidget(instructions);
    codeLayout->addStretch(1);
    codeLayout->addWidget(cancelButton, 0, Qt::AlignLeft);
    openLayout->addWidget(codeColumn, 1);
    pairBoxLayout->addWidget(pairingOpen_);

    pairingPending_ = new QWidget(pairBox);
    auto* pendingLayout = new QHBoxLayout(pairingPending_);
    pendingLayout->setContentsMargins(0, 0, 0, 0);
    pairingPendingLabel_ = new QLabel(pairingPending_);
    pairingPendingLabel_->setWordWrap(true);
    auto* denyButton = new QPushButton("Deny", pairingPending_);
    auto* allowButton = new QPushButton("Allow", pairingPending_);
    pendingLayout->addWidget(pairingPendingLabel_, 1);
    pendingLayout->addWidget(denyButton);
    pendingLayout->addWidget(allowButton);
    pairBoxLayout->addWidget(pairingPending_);
    content->addWidget(pairBox);

    auto* devicesHeading = subHeading("Saved devices");
    content->addWidget(devicesHeading);
    pairingDevicesTable_ = new QTableWidget(pairingContent_);
    pairingDevicesTable_->setColumnCount(3);
    pairingDevicesTable_->setHorizontalHeaderLabels({"Device", "State", QString()});
    pairingDevicesTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    pairingDevicesTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    pairingDevicesTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    pairingDevicesTable_->verticalHeader()->hide();
    pairingDevicesTable_->setSelectionMode(QAbstractItemView::NoSelection);
    pairingDevicesTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    pairingDevicesTable_->setShowGrid(false);
    pairingDevicesTable_->setMinimumHeight(120);
    pairingDevicesTable_->setMaximumHeight(190);
    content->addWidget(pairingDevicesTable_);
    layout->addWidget(pairingContent_);

    pairingErrorLabel_ = new QLabel(page);
    pairingErrorLabel_->setWordWrap(true);
    pairingErrorLabel_->setStyleSheet("color: #d44252;");
    layout->addWidget(pairingErrorLabel_);
    layout->addStretch(1);

    connect(pairingEnabledCheck_, &QCheckBox::toggled, this,
            [this](bool enabled) { mainWindow_->setPairServiceEnabled(enabled); });
    connect(pairButton, &QPushButton::clicked,
            mainWindow_, &MainWindow::openPairingWindow);
    connect(cancelButton, &QPushButton::clicked,
            mainWindow_, &MainWindow::closePairingWindow);
    connect(allowButton, &QPushButton::clicked, this,
            [this] { mainWindow_->respondToPairing(true); });
    connect(denyButton, &QPushButton::clicked, this,
            [this] { mainWindow_->respondToPairing(false); });
    connect(mainWindow_, &MainWindow::pairServiceStateChanged,
            this, &SettingsDialog::refreshPairingUi);
    refreshPairingUi();
    return page;
}

void SettingsDialog::refreshPairingUi() {
    if (!pairingEnabledCheck_) return;
    const PairServiceState& state = mainWindow_->pairServiceState();
    {
        QSignalBlocker guard(pairingEnabledCheck_);
        pairingEnabledCheck_->setChecked(state.enabled);
    }
    pairingContent_->setVisible(state.enabled);
    const bool pending = !state.pendingDeviceId.isEmpty();
    pairingPending_->setVisible(pending);
    pairingClosed_->setVisible(!pending && !state.pairingOpen);
    pairingOpen_->setVisible(!pending && state.pairingOpen);
    pairingPendingLabel_->setText(QStringLiteral(
        "<b>Allow %1?</b><br>This phone entered the pairing code. "
        "Only allow a phone you recognise.")
        .arg(state.pendingDeviceName.toHtmlEscaped()));
    pairingCodeLabel_->setText(state.matchingCode.size() == 8
        ? state.matchingCode.left(4) + QLatin1Char(' ') + state.matchingCode.mid(4)
        : state.matchingCode);
    if (state.pairingOpen && !state.qrPayload.isEmpty()) {
        const QImage qr = pairingQrCodeImage(state.qrPayload);
        if (!qr.isNull()) {
            pairingQrLabel_->setPixmap(QPixmap::fromImage(qr));
            pairingQrLabel_->setText({});
        } else {
            pairingQrLabel_->setPixmap({});
            pairingQrLabel_->setText("QR payload is too long");
        }
    } else {
        pairingQrLabel_->setPixmap({});
        pairingQrLabel_->setText("QR unavailable");
    }

    pairingDevicesTable_->clearSpans();
    pairingDevicesTable_->clearContents();
    if (state.devices.isEmpty()) {
        pairingDevicesTable_->setRowCount(1);
        auto* empty = new QTableWidgetItem("No Android devices paired.");
        empty->setFlags(Qt::NoItemFlags);
        pairingDevicesTable_->setItem(0, 0, empty);
        pairingDevicesTable_->setSpan(0, 0, 1, 3);
    } else {
        pairingDevicesTable_->setRowCount(state.devices.size());
        for (int row = 0; row < state.devices.size(); ++row) {
            const PairDeviceState& device = state.devices[row];
            auto* name = new QTableWidgetItem(
                device.name.isEmpty() ? QStringLiteral("Android display") : device.name);
            auto* status = new QTableWidgetItem(device.connected ? "●  Connected" : "●  Offline");
            status->setForeground(device.connected ? QColor("#4ade80")
                                                   : palette().color(QPalette::PlaceholderText));
            pairingDevicesTable_->setItem(row, 0, name);
            pairingDevicesTable_->setItem(row, 1, status);
            auto* remove = new QPushButton("Remove", pairingDevicesTable_);
            connect(remove, &QPushButton::clicked, this,
                    [this, id = device.id] { mainWindow_->removePairDevice(id); });
            pairingDevicesTable_->setCellWidget(row, 2, remove);
        }
    }
    pairingDevicesTable_->resizeRowsToContents();
    pairingErrorLabel_->setText(state.error);
    pairingErrorLabel_->setVisible(!state.error.isEmpty());
}

QWidget* SettingsDialog::buildProtocolPage() {
    QFormLayout* form;
    QWidget* page = makePage(form);
    addSection(form, "Telemetry Format");

    // Read-only: the format most recently detected from incoming UDP packets,
    // cached by MainWindow::onEngineRow() from protocol_status rows. Shows
    // "—" until the first packet after the engine starts.
    const int detected = mainWindow_->lastDetectedProtocolFormat();
    detectedProtocolLabel_ = new QLabel(detected > 0 ? QString::number(detected) : QStringLiteral("—"));
    form->addRow("Detected format:", detectedProtocolLabel_);

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
    form->addRow("Use format:", protocolCombo_);

    protocolWarningLabel_ = new QLabel;
    protocolWarningLabel_->setWordWrap(true);
    protocolWarningLabel_->setContentsMargins(12, 10, 12, 10);
    protocolWarningLabel_->setStyleSheet(
        "background-color: rgba(225, 6, 0, 0.08); border: 1px solid rgba(225, 6, 0, 0.35);"
        " border-radius: 6px; color: #e35b57;");
    form->addRow(protocolWarningLabel_);
    updateProtocolWarning(mainWindow_->detectedProtocolWarningFormat(),
                          mainWindow_->forcedProtocolWarningFormat());
    connect(mainWindow_, &MainWindow::protocolWarningChanged,
            this, &SettingsDialog::updateProtocolWarning);

    addSection(form, "Network");

    // Network controls are a local draft. Nothing is persisted or restarted
    // until Apply & Restart is pressed.
    udpPortSpin_ = new QSpinBox;
    udpPortSpin_->setRange(1, 65535);
    udpPortSpin_->setGroupSeparatorShown(false);   // a port is not a thousands-grouped number
    udpPortSpin_->setValue(mainWindow_->udpPort());
    form->addRow("UDP port:", udpPortSpin_);

    udpBindAddressEdit_ = new QLineEdit(mainWindow_->udpBindAddress());
    udpBindAddressEdit_->setPlaceholderText(QStringLiteral("0.0.0.0"));
    udpBindAddressEdit_->setToolTip(
        "Which local network interface to receive telemetry on. "
        "0.0.0.0 listens on all interfaces.");
    form->addRow("Listen on:", udpBindAddressEdit_);

    udpForwardingCheck_ = new QCheckBox("Forward every received packet unchanged");
    udpForwardingCheck_->setChecked(mainWindow_->udpForwardingEnabled());
    form->addRow("Forwarding:", udpForwardingCheck_);

    udpForwardEditor_ = new QWidget;
    auto* forwardLayout = new QVBoxLayout(udpForwardEditor_);
    forwardLayout->setContentsMargins(0, 0, 0, 0);
    forwardLayout->setSpacing(6);
    auto* forwardHeader = new QHBoxLayout;
    auto* forwardTitle = new QLabel("Forwarding channels (up to 15)");
    QFont forwardTitleFont = forwardTitle->font();
    forwardTitleFont.setBold(true);
    forwardTitle->setFont(forwardTitleFont);
    udpAddForwardTarget_ = new QPushButton("Add channel");
    forwardHeader->addWidget(forwardTitle);
    forwardHeader->addStretch(1);
    forwardHeader->addWidget(udpAddForwardTarget_);
    forwardLayout->addLayout(forwardHeader);

    udpForwardTargets_ = new QTableWidget(0, 3);
    udpForwardTargets_->setHorizontalHeaderLabels({"IPv4 destination", "Port", QString()});
    udpForwardTargets_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    udpForwardTargets_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    udpForwardTargets_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    udpForwardTargets_->verticalHeader()->setVisible(false);
    udpForwardTargets_->setSelectionMode(QAbstractItemView::NoSelection);
    udpForwardTargets_->setFocusPolicy(Qt::NoFocus);
    udpForwardTargets_->setFixedHeight(170);
    forwardLayout->addWidget(udpForwardTargets_);

    udpForwardValidation_ = new QLabel(
        "Enter valid IPv4 destinations and ports. Forwarding back to this listener "
        "would create a packet loop.");
    udpForwardValidation_->setWordWrap(true);
    udpForwardValidation_->setStyleSheet("color: #e35b57;");
    forwardLayout->addWidget(udpForwardValidation_);
    form->addRow(udpForwardEditor_);

    for (const UdpForwardTargetSetting& target : mainWindow_->udpForwardTargets())
        addForwardTargetRow(target.address, target.port);

    QWidget* applyRow = new QWidget;
    auto* applyLayout = new QHBoxLayout(applyRow);
    applyLayout->setContentsMargins(0, 0, 0, 0);
    udpApplyStatus_ = new QLabel;
    udpApplyStatus_->setWordWrap(true);
    udpApplyButton_ = new QPushButton("Apply & Restart");
    applyLayout->addWidget(udpApplyStatus_, 1);
    applyLayout->addWidget(udpApplyButton_);
    form->addRow(applyRow);

    udpStatusResetTimer_ = new QTimer(this);
    udpStatusResetTimer_->setSingleShot(true);
    udpStatusResetTimer_->setInterval(2500);
    connect(udpStatusResetTimer_, &QTimer::timeout, this, [this] {
        udpApplyState_ = UdpApplyState::Idle;
        udpApplyError_.clear();
        refreshNetworkDraftUi();
    });
    connect(udpPortSpin_, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [this](int) { refreshNetworkDraftUi(); });
    connect(udpBindAddressEdit_, &QLineEdit::textChanged, this,
            [this](const QString&) { refreshNetworkDraftUi(); });
    connect(udpForwardingCheck_, &QCheckBox::toggled, this,
            [this](bool) { refreshNetworkDraftUi(); });
    connect(udpAddForwardTarget_, &QPushButton::clicked, this,
            [this] { addForwardTargetRow(); });
    connect(udpApplyButton_, &QPushButton::clicked,
            this, &SettingsDialog::applyNetworkDraft);
    refreshNetworkDraftUi();

    return page;
}

void SettingsDialog::updateProtocolWarning(int detectedFormat, int forcedFormat) {
    if (!protocolWarningLabel_) return;
    const bool visible = detectedFormat > 0 && forcedFormat > 0;
    protocolWarningLabel_->setVisible(visible);
    if (visible) {
        protocolWarningLabel_->setText(
            QString("Protocol mismatch detected\nReceiving %1 packets — override is set to %2")
                .arg(detectedFormat).arg(forcedFormat));
    } else {
        protocolWarningLabel_->clear();
    }
}

void SettingsDialog::addForwardTargetRow(const QString& address, int port) {
    if (!udpForwardTargets_ || udpForwardTargets_->rowCount() >= 15) return;

    const int row = udpForwardTargets_->rowCount();
    udpForwardTargets_->insertRow(row);
    auto* addressEdit = new QLineEdit(address);
    addressEdit->setPlaceholderText("192.168.1.100");
    auto* portSpin = new QSpinBox;
    portSpin->setRange(1, 65535);
    portSpin->setGroupSeparatorShown(false);
    portSpin->setValue(port);
    auto* removeButton = new QToolButton;
    removeButton->setText(QStringLiteral("×"));
    removeButton->setToolTip(QString("Remove forwarding channel %1").arg(row + 1));
    udpForwardTargets_->setCellWidget(row, 0, addressEdit);
    udpForwardTargets_->setCellWidget(row, 1, portSpin);
    udpForwardTargets_->setCellWidget(row, 2, removeButton);

    connect(addressEdit, &QLineEdit::textChanged, this,
            [this](const QString&) { refreshNetworkDraftUi(); });
    connect(portSpin, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [this](int) { refreshNetworkDraftUi(); });
    connect(removeButton, &QToolButton::clicked, this,
            [this, addressEdit] { removeForwardTargetRow(addressEdit); });
    refreshNetworkDraftUi();
}

void SettingsDialog::removeForwardTargetRow(QWidget* addressEditor) {
    if (!udpForwardTargets_) return;
    for (int row = 0; row < udpForwardTargets_->rowCount(); ++row) {
        if (udpForwardTargets_->cellWidget(row, 0) == addressEditor) {
            udpForwardTargets_->removeRow(row);
            break;
        }
    }
    refreshNetworkDraftUi();
}

QVector<UdpForwardTargetSetting> SettingsDialog::forwardTargetDraft() const {
    QVector<UdpForwardTargetSetting> targets;
    if (!udpForwardTargets_) return targets;
    targets.reserve(udpForwardTargets_->rowCount());
    for (int row = 0; row < udpForwardTargets_->rowCount(); ++row) {
        const auto* address = qobject_cast<QLineEdit*>(udpForwardTargets_->cellWidget(row, 0));
        const auto* port = qobject_cast<QSpinBox*>(udpForwardTargets_->cellWidget(row, 1));
        if (address && port) targets.push_back({address->text(), port->value()});
    }
    return targets;
}

bool SettingsDialog::networkDraftValid() const {
    if (!udpForwardingCheck_ || !udpForwardingCheck_->isChecked()) return true;
    const QString listenerAddress = udpBindAddressEdit_->text().trimmed();
    const int listenerPort = udpPortSpin_->value();
    for (const UdpForwardTargetSetting& target : forwardTargetDraft()) {
        const QString address = target.address.trimmed();
        if (!isIpv4Address(address) || target.port < 1 || target.port > 65535 ||
            (target.port == listenerPort &&
             (address.startsWith("127.") || address == listenerAddress))) {
            return false;
        }
    }
    return true;
}

bool SettingsDialog::networkDraftDirty() const {
    return udpPortSpin_->value() != mainWindow_->udpPort() ||
           udpBindAddressEdit_->text() != mainWindow_->udpBindAddress() ||
           udpForwardingCheck_->isChecked() != mainWindow_->udpForwardingEnabled() ||
           forwardTargetDraft() != mainWindow_->udpForwardTargets();
}

void SettingsDialog::refreshNetworkDraftUi() {
    if (!udpApplyButton_) return;
    const bool forwarding = udpForwardingCheck_->isChecked();
    udpForwardEditor_->setVisible(forwarding);
    const bool valid = networkDraftValid();
    udpForwardValidation_->setVisible(forwarding && !valid);
    udpAddForwardTarget_->setEnabled(udpForwardTargets_->rowCount() < 15);
    udpApplyButton_->setEnabled(valid && udpApplyState_ != UdpApplyState::Applying);

    // Mark only invalid destination fields, while retaining the single shared
    // explanation below the channel table.
    const QString listenerAddress = udpBindAddressEdit_->text().trimmed();
    const int listenerPort = udpPortSpin_->value();
    for (int row = 0; row < udpForwardTargets_->rowCount(); ++row) {
        auto* address = qobject_cast<QLineEdit*>(udpForwardTargets_->cellWidget(row, 0));
        auto* port = qobject_cast<QSpinBox*>(udpForwardTargets_->cellWidget(row, 1));
        if (!address || !port) continue;
        const QString destination = address->text().trimmed();
        const bool rowValid = isIpv4Address(destination) &&
            !(port->value() == listenerPort &&
              (destination.startsWith("127.") || destination == listenerAddress));
        const QString invalidStyle = rowValid ? QString() : QStringLiteral("border: 1px solid #c62828;");
        address->setStyleSheet(invalidStyle);
        port->setStyleSheet(invalidStyle);
    }

    QString status;
    QString color;
    switch (udpApplyState_) {
        case UdpApplyState::Applying:
            status = QStringLiteral("Restarting…");
            udpApplyButton_->setText(QStringLiteral("Restarting…"));
            break;
        case UdpApplyState::Ok:
            status = QStringLiteral("Listener restarted successfully");
            color = QStringLiteral("#4caf50");
            udpApplyButton_->setText(QStringLiteral("Applied"));
            break;
        case UdpApplyState::Error:
            status = udpApplyError_;
            color = QStringLiteral("#e35b57");
            udpApplyButton_->setText(QStringLiteral("Apply & Restart"));
            break;
        case UdpApplyState::Idle: {
            const bool dirty = networkDraftDirty();
            status = dirty
                ? QStringLiteral("Unsaved changes")
                : QStringLiteral("Restart the listener to apply changes");
            if (dirty) color = QStringLiteral("#d49b2e");
            udpApplyButton_->setText(QStringLiteral("Apply & Restart"));
            break;
        }
    }
    udpApplyStatus_->setText(status);
    udpApplyStatus_->setStyleSheet(color.isEmpty() ? QString() : "color: " + color + ";");
}

void SettingsDialog::applyNetworkDraft() {
    if (!networkDraftValid() || udpApplyState_ == UdpApplyState::Applying) return;
    udpStatusResetTimer_->stop();
    udpApplyState_ = UdpApplyState::Applying;
    udpApplyError_.clear();
    refreshNetworkDraftUi();

    // Defer the synchronous stop/start by one event-loop turn so "Restarting…"
    // can paint before the listener is recreated.
    QTimer::singleShot(0, this, [this] {
        QVector<UdpForwardTargetSetting> targets = forwardTargetDraft();
        for (int row = 0; row < targets.size(); ++row) {
            targets[row].address = targets[row].address.trimmed();
            if (auto* address = qobject_cast<QLineEdit*>(udpForwardTargets_->cellWidget(row, 0)))
                address->setText(targets[row].address);
        }
        const QString error = mainWindow_->applyUdpConfiguration(
            udpPortSpin_->value(), udpBindAddressEdit_->text(),
            udpForwardingCheck_->isChecked(), targets);
        if (error.isEmpty()) {
            udpApplyState_ = UdpApplyState::Ok;
        } else {
            udpApplyState_ = UdpApplyState::Error;
            udpApplyError_ = error;
        }
        refreshNetworkDraftUi();
        udpStatusResetTimer_->start();
    });
}

QWidget* SettingsDialog::buildTeamColorsPage() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(24, 4, 24, 16);
    layout->setSpacing(10);

    auto* headingRow = new QHBoxLayout;
    auto* formatLabel = new QLabel(QStringLiteral("Game:"), page);
    headingRow->addWidget(formatLabel);
    auto* formatControl = new QWidget(page);
    auto* formatLayout = new QHBoxLayout(formatControl);
    formatLayout->setContentsMargins(0, 0, 0, 0);
    formatLayout->setSpacing(0);
    auto* formatGroup = new QButtonGroup(formatControl);
    formatGroup->setExclusive(true);
    for (const int format : {2024, 2025, 2026}) {
        auto* button = new SegmentButton(formatControl);
        button->setText(QString::number(format));
        button->setCheckable(true);
        button->setAutoRaise(true);
        button->setMinimumWidth(62);
        formatGroup->addButton(button, format);
        formatLayout->addWidget(button);
        if (format == teamColorFormat_) button->setChecked(true);
    }
    connect(formatGroup, &QButtonGroup::idClicked, this, [this](int format) {
        teamColorFormat_ = format;
        refreshTeamColorRows();
    });
    headingRow->addWidget(formatControl);
    headingRow->addStretch(1);
    layout->addLayout(headingRow);

    auto* scroll = new QScrollArea(page);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setMinimumHeight(280);
    teamColorRows_ = new QWidget(scroll);
    teamColorRowsLayout_ = new QVBoxLayout(teamColorRows_);
    teamColorRowsLayout_->setContentsMargins(0, 0, 6, 0);
    teamColorRowsLayout_->setSpacing(10);
    scroll->setWidget(teamColorRows_);
    layout->addWidget(scroll, 1);

    loadTeamColorConfiguration();
    refreshTeamColorRows();
    return page;
}

void SettingsDialog::loadTeamColorConfiguration() {
    teamColorCatalog_.clear();
    teamColorOverrides_.clear();
    if (!mainWindow_) return;

    QJsonParseError error;
    const QJsonDocument catalogDocument = QJsonDocument::fromJson(
        mainWindow_->teamColorCatalogJson(), &error);
    if (error.error == QJsonParseError::NoError && catalogDocument.isObject()) {
        const QJsonObject root = catalogDocument.object();
        for (const int format : {2024, 2025, 2026}) {
            const QJsonArray teams = root.value(QString::number(format)).toArray();
            QVector<TeamColorPresetSetting> parsed;
            parsed.reserve(teams.size());
            for (const QJsonValue& value : teams) {
                const QJsonObject object = value.toObject();
                const int id = object.value("id").toInt(-1);
                const QString name = object.value("name").toString();
                const QString color = object.value("color").toString().toUpper();
                const QString group = object.value("group").toString();
                if (id < 0 || name.isEmpty() || !QColor(color).isValid()) continue;
                parsed.push_back({id, name, color,
                                  group.isEmpty() ? QStringLiteral("Teams") : group});
            }
            if (!parsed.isEmpty()) teamColorCatalog_.insert(format, parsed);
        }
    }

    error = {};
    const QJsonDocument overridesDocument = QJsonDocument::fromJson(
        mainWindow_->teamColorOverridesJson(), &error);
    if (error.error != QJsonParseError::NoError || !overridesDocument.isObject()) return;
    const QJsonObject root = overridesDocument.object();
    for (const int format : {2024, 2025, 2026}) {
        const QJsonObject teams = root.value(QString::number(format)).toObject();
        QHash<int, QString> parsed;
        for (auto it = teams.constBegin(); it != teams.constEnd(); ++it) {
            bool ok = false;
            const int id = it.key().toInt(&ok);
            if (ok && it.value().isString()) parsed.insert(id, it.value().toString());
        }
        if (!parsed.isEmpty()) teamColorOverrides_.insert(format, parsed);
    }
}

void SettingsDialog::commitTeamColorOverrides() {
    QJsonObject root;
    for (const int format : {2024, 2025, 2026}) {
        const auto formatIt = teamColorOverrides_.constFind(format);
        if (formatIt == teamColorOverrides_.cend() || formatIt->isEmpty()) continue;
        QJsonObject teams;
        for (auto it = formatIt->constBegin(); it != formatIt->constEnd(); ++it)
            teams.insert(QString::number(it.key()), it.value());
        root.insert(QString::number(format), teams);
    }
    mainWindow_->setTeamColorOverridesJson(
        QJsonDocument(root).toJson(QJsonDocument::Compact));
    QTimer::singleShot(0, this, [this] {
        loadTeamColorConfiguration();
        refreshTeamColorRows();
    });
}

void SettingsDialog::refreshTeamColorRows() {
    if (!teamColorRowsLayout_) return;
    while (QLayoutItem* item = teamColorRowsLayout_->takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }

    const QVector<TeamColorPresetSetting> teams = teamColorCatalog_.value(teamColorFormat_);
    if (teams.isEmpty()) {
        auto* unavailable = new QLabel(QStringLiteral("Team color catalog unavailable."),
                                       teamColorRows_);
        unavailable->setAlignment(Qt::AlignCenter);
        unavailable->setStyleSheet(QStringLiteral("color:palette(placeholder-text);padding:36px;"));
        teamColorRowsLayout_->addWidget(unavailable);
        teamColorRowsLayout_->addStretch(1);
        return;
    }

    const bool supportsLivery = teamColorFormat_ != 2024;
    int first = 0;
    while (first < teams.size()) {
        int end = first + 1;
        while (end < teams.size() && teams[end].group == teams[first].group) ++end;

        auto* groupBox = new QGroupBox(teams[first].group, teamColorRows_);
        auto* groupLayout = new QVBoxLayout(groupBox);
        groupLayout->setContentsMargins(10, 8, 10, 10);
        groupLayout->setSpacing(5);
        auto* groupActions = new QHBoxLayout;
        groupActions->setContentsMargins(0, 0, 0, 2);
        groupActions->addStretch(1);

        int liveryCount = 0;
        bool hasOverrides = false;
        for (int index = first; index < end; ++index) {
            const QString override = teamColorOverrides_.value(teamColorFormat_)
                                         .value(teams[index].id);
            hasOverrides |= !override.isEmpty();
            if (override == QStringLiteral("livery")) ++liveryCount;
        }
        if (supportsLivery) {
            auto* source = new QComboBox(groupBox);
            source->addItem(QStringLiteral("Fixed colors"), QStringLiteral("fixed"));
            source->addItem(QStringLiteral("Livery colors"), QStringLiteral("livery"));
            source->setAccessibleName(
                QStringLiteral("Color source for %1").arg(teams[first].group));
            source->setPlaceholderText(QStringLiteral("Mixed sources"));
            if (liveryCount == 0) source->setCurrentIndex(0);
            else if (liveryCount == end - first) source->setCurrentIndex(1);
            else source->setCurrentIndex(-1);
            connect(source, &QComboBox::currentIndexChanged, this,
                    [this, source, teams, first, end](int index) {
                if (index < 0) return;
                const bool livery = source->currentData().toString() == "livery";
                auto& overrides = teamColorOverrides_[teamColorFormat_];
                for (int teamIndex = first; teamIndex < end; ++teamIndex) {
                    const int id = teams[teamIndex].id;
                    if (livery) overrides[id] = QStringLiteral("livery");
                    else if (overrides.value(id) == QStringLiteral("livery"))
                        overrides.remove(id);
                }
                if (overrides.isEmpty()) teamColorOverrides_.remove(teamColorFormat_);
                commitTeamColorOverrides();
            });
            groupActions->addWidget(source);
        }
        auto* resetGroup = new QPushButton(QStringLiteral("Reset group"), groupBox);
        resetGroup->setEnabled(hasOverrides);
        resetGroup->setToolTip(hasOverrides ? QStringLiteral("Reset section to presets")
                                            : QStringLiteral("Section is using preset colors"));
        connect(resetGroup, &QPushButton::clicked, this,
                [this, teams, first, end] {
            auto& overrides = teamColorOverrides_[teamColorFormat_];
            for (int index = first; index < end; ++index)
                overrides.remove(teams[index].id);
            if (overrides.isEmpty()) teamColorOverrides_.remove(teamColorFormat_);
            commitTeamColorOverrides();
        });
        groupActions->addWidget(resetGroup);
        groupLayout->addLayout(groupActions);

        for (int index = first; index < end; ++index) {
            const TeamColorPresetSetting team = teams[index];
            const QString override = teamColorOverrides_.value(teamColorFormat_)
                                         .value(team.id);
            const bool useLivery = supportsLivery && override == QStringLiteral("livery");
            const QString color = override.startsWith('#') ? override : team.color;

            if (index > first) groupLayout->addWidget(horizontalSeparator());
            auto* row = new QWidget(groupBox);
            auto* rowLayout = new QGridLayout(row);
            rowLayout->setContentsMargins(2, 3, 2, 3);
            rowLayout->setHorizontalSpacing(9);
            rowLayout->setVerticalSpacing(1);
            auto* name = new QLabel(team.name, row);
            QFont nameFont = name->font();
            nameFont.setBold(true);
            name->setFont(nameFont);
            auto* preset = new QLabel(
                useLivery ? QStringLiteral("Uses each car's game livery color")
                          : QStringLiteral("Preset %1").arg(team.color), row);
            preset->setStyleSheet(QStringLiteral("color:palette(placeholder-text);font-size:10px;"));
            rowLayout->addWidget(name, 0, 0);
            rowLayout->addWidget(preset, 1, 0);
            rowLayout->setColumnStretch(0, 1);

            if (supportsLivery) {
                auto* source = new QComboBox(row);
                source->addItem(QStringLiteral("Fixed"), QStringLiteral("fixed"));
                source->addItem(QStringLiteral("Livery"), QStringLiteral("livery"));
                source->setCurrentIndex(useLivery ? 1 : 0);
                source->setAccessibleName(
                    QStringLiteral("Color source for %1").arg(team.name));
                connect(source, &QComboBox::currentIndexChanged, this,
                        [this, source, team](int) {
                    auto& overrides = teamColorOverrides_[teamColorFormat_];
                    if (source->currentData().toString() == QStringLiteral("livery"))
                        overrides[team.id] = QStringLiteral("livery");
                    else if (overrides.value(team.id) == QStringLiteral("livery"))
                        overrides.remove(team.id);
                    if (overrides.isEmpty()) teamColorOverrides_.remove(teamColorFormat_);
                    commitTeamColorOverrides();
                });
                rowLayout->addWidget(source, 0, 1, 2, 1);
            }

            auto* picker = new QPushButton(row);
            picker->setFixedSize(38, 28);
            picker->setEnabled(!useLivery);
            picker->setAccessibleName(QStringLiteral("%1 color picker").arg(team.name));
            picker->setStyleSheet(useLivery
                ? QStringLiteral("background:palette(alternate-base);border:1px solid palette(mid);")
                : QStringLiteral("background:%1;border:1px solid palette(mid);border-radius:4px;")
                      .arg(color));
            rowLayout->addWidget(picker, 0, 2, 2, 1);
            auto* hex = new QLabel(useLivery ? QStringLiteral("Livery") : color, row);
            hex->setMinimumWidth(62);
            QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
            mono.setPointSizeF(qMax(7.0, mono.pointSizeF() - 1.0));
            hex->setFont(mono);
            rowLayout->addWidget(hex, 0, 3, 2, 1);
            connect(picker, &QPushButton::clicked, this, [this, team, color] {
                const QColor selected = QColorDialog::getColor(
                    QColor(color), this, QStringLiteral("Select %1 color").arg(team.name),
                    QColorDialog::DontUseNativeDialog);
                if (!selected.isValid()) return;
                const QString normalized = selected.name(QColor::HexRgb).toUpper();
                auto& overrides = teamColorOverrides_[teamColorFormat_];
                if (normalized == team.color.toUpper()) overrides.remove(team.id);
                else overrides[team.id] = normalized;
                if (overrides.isEmpty()) teamColorOverrides_.remove(teamColorFormat_);
                commitTeamColorOverrides();
            });

            auto* reset = new QPushButton(QStringLiteral("Reset"), row);
            reset->setEnabled(!override.isEmpty());
            reset->setToolTip(QStringLiteral("Reset to preset"));
            connect(reset, &QPushButton::clicked, this, [this, team] {
                auto& overrides = teamColorOverrides_[teamColorFormat_];
                overrides.remove(team.id);
                if (overrides.isEmpty()) teamColorOverrides_.remove(teamColorFormat_);
                commitTeamColorOverrides();
            });
            rowLayout->addWidget(reset, 0, 4, 2, 1);
            groupLayout->addWidget(row);
        }
        teamColorRowsLayout_->addWidget(groupBox);
        first = end;
    }
    teamColorRowsLayout_->addStretch(1);
}

QWidget* SettingsDialog::buildDebugPage() {
    QWidget* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(24, 4, 24, 20);
    layout->setSpacing(16);

    const auto addDiagnosticToggle = [this, layout](
        const QString& title, const QString& description, const QString& warning,
        bool checked, auto setter) {
        auto* section = new QWidget;
        auto* row = new QHBoxLayout(section);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(18);
        auto* text = new QWidget(section);
        auto* textLayout = new QVBoxLayout(text);
        textLayout->setContentsMargins(0, 0, 0, 0);
        textLayout->setSpacing(3);
        auto* heading = new QLabel(title, text);
        QFont headingFont = heading->font();
        headingFont.setBold(true);
        heading->setFont(headingFont);
        auto* detail = new QLabel(description, text);
        detail->setWordWrap(true);
        detail->setStyleSheet(QStringLiteral("color:palette(placeholder-text);"));
        textLayout->addWidget(heading);
        textLayout->addWidget(detail);
        if (!warning.isEmpty()) {
            auto* warningLabel = new QLabel(warning, text);
            warningLabel->setWordWrap(true);
            warningLabel->setStyleSheet(QStringLiteral("color:#d68a22;"));
            textLayout->addWidget(warningLabel);
        }
        auto* toggle = new QCheckBox(section);
        toggle->setChecked(checked);
        toggle->setAccessibleName(title);
        connect(toggle, &QCheckBox::toggled, this, setter);
        row->addWidget(text, 1);
        row->addWidget(toggle, 0, Qt::AlignTop);
        layout->addWidget(section);
    };

    addDiagnosticToggle(
        QStringLiteral("Additional logging"),
        QStringLiteral("Enable detailed native telemetry pipeline, playback, pairing, Qt renderer, and performance diagnostics. Startup and fatal-error logging remain enabled."),
        QStringLiteral("Development instrumentation is fully applied the next time the app starts."),
        mainWindow_->additionalLoggingEnabled(),
        [this](bool enabled) { mainWindow_->setAdditionalLoggingEnabled(enabled); });
    layout->addWidget(horizontalSeparator());
    addDiagnosticToggle(
        QStringLiteral("Memory log"),
        QStringLiteral("Sample the Qt process and retained telemetry once per second into launch-diagnostics/ram_usage.log. This can be changed while the app is running."),
        QString(), mainWindow_->memoryLogEnabled(),
        [this](bool enabled) { mainWindow_->setMemoryLogEnabled(enabled); });
    layout->addStretch(1);
    return page;
}

// About: app identity + version, project links, creator/non-affiliation details,
// the project license, and attribution for every bundled third-party library.
// Each library exposes its full license text via a "View" button
// (showLicenseText), satisfying the GPL/LGPL notice requirements.
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

    QWidget* links = new QWidget;
    QHBoxLayout* linksLayout = new QHBoxLayout(links);
    linksLayout->setContentsMargins(0, 0, 0, 0);
    linksLayout->setSpacing(16);

    QLabel* websiteLink = new QLabel(
        "<a href=\"https://track-n-race.com\">Official website</a>");
    websiteLink->setOpenExternalLinks(true);
    linksLayout->addWidget(websiteLink);

    QLabel* repoLink = new QLabel(
        "<a href=\"https://github.com/nogoat/track-n-race\">GitHub repository</a>");
    repoLink->setOpenExternalLinks(true);
    linksLayout->addWidget(repoLink);
    QPushButton* diagnosticsButton = new QPushButton("Launch diagnostics");
    diagnosticsButton->setToolTip("Open launch diagnostics folder");
    diagnosticsButton->setIcon(adaptThemeIcon(
        QIcon::fromTheme("folder-open-symbolic"),
        palette().color(QPalette::WindowText),
        style()->standardIcon(QStyle::SP_DirOpenIcon)));
    connect(diagnosticsButton, &QPushButton::clicked, this, [this] {
        const QString directory = tnr::diagnostics::directoryPath();
        if (!QDesktopServices::openUrl(QUrl::fromLocalFile(directory))) {
            QMessageBox::critical(
                this, "Unable to Open Launch Diagnostics",
                QStringLiteral("Track N Race could not open the launch-diagnostics folder.\n\n%1")
                    .arg(directory));
        }
    });
    linksLayout->addWidget(diagnosticsButton);
    linksLayout->addStretch(1);
    v->addWidget(links);

    QLabel* creator = new QLabel("Created by NoGoat");
    QFont creatorFont = creator->font();
    creatorFont.setBold(true);
    creator->setFont(creatorFont);
    v->addWidget(creator);

    QLabel* nonAffiliation = new QLabel(
        "Track N Race is not affiliated with, endorsed by, or associated with EA, "
        "Codemasters, Formula One, the FIA, the drivers or the teams participating "
        "in Formula One.");
    nonAffiliation->setWordWrap(true);
    v->addWidget(nonAffiliation);

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
        { "glaze",         "7.8.3",    "MIT",     "© 2019–present Stephen Berry",           "https://github.com/stephenberry/glaze", "github.com", ":/licenses/MIT-glaze.txt" },
        // The toast notifications are a fork of niklashenning/qt-toast (see the
        // license text, which notes the fork and reproduces the upstream notice).
        { "qt-toast (fork)", "—",      "MIT",     "© 2024 Niklas Henning",                  "https://github.com/niklashenning/qt-toast", "github.com", ":/licenses/MIT-qt-toast.txt" },
        { "zlib",          "1.3.2",    "zlib",    "© 1995–2026 Jean-loup Gailly & Mark Adler", "https://zlib.net",              "zlib.net",        ":/licenses/Zlib.txt"     },
        { "Zstandard",     "1.5.7",    "BSD 3-Clause", "© Meta Platforms, Inc. and affiliates", "https://facebook.github.io/zstd/", "facebook.github.io", ":/licenses/BSD-3-Clause-Zstandard.txt" },
        // libxlsxwriter powers the "Export to Excel" action; linked in every build.
        { "libxlsxwriter", "1.2.4",    "BSD 2-Clause", "© 2014–2026 John McNamara",         "https://libxlsxwriter.github.io", "libxlsxwriter.github.io", ":/licenses/BSD-2-Clause-libxlsxwriter.txt" },
        // libsodium and CPace encrypt and authenticate the paired-display link.
        { "libsodium",     "1.0.20",   "ISC",     "© 2013–2026 Frank Denis",                "https://libsodium.org",         "libsodium.org",   ":/licenses/ISC-libsodium.txt" },
        { "CPace",         "—",        "BSD 2-Clause", "© 2020–2021 Frank Denis",           "https://github.com/jedisct1/cpace", "github.com",   ":/licenses/BSD-2-Clause-cpace.txt" },
        // Noto Sans is bundled (fonts.qrc) in every build as the Breeze UI font, so
        // it's credited here unconditionally — not under BREEZE_BUNDLED.
        { "Noto Sans",     "—",        "OFL 1.1", "© The Noto Project Authors",             "https://fonts.google.com/noto/specimen/Noto+Sans", "fonts.google.com", ":/licenses/OFL-1.1-Noto.txt" },
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
        // Some proportional fonts clip the last glyph. Pin a plain-text-measured minimum
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
