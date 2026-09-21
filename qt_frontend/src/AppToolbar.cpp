#include "AppToolbar.h"
#include "IconUtils.h"

#include <QApplication>
#include <QAction>
#include <QComboBox>
#include <QEvent>
#include <QFont>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QResizeEvent>
#include <QSizePolicy>
#include <QStyle>
#include <QStyleOptionButton>
#include <QStylePainter>
#include <QTimer>
#include <QToolButton>
#include <cmath>
// TEMP DIAGNOSTIC (icon blur investigation) — remove with the logging block below.
#include <QFile>
#include <QTextStream>
#include <QDir>

// Chart window-size options, shown as a segmented toolbar control.
static const struct { const char* label; ChartWindow window; } kWindowOptions[] = {
    {"15s", ChartWindow::Seconds15}, {"30s", ChartWindow::Seconds30},
    {"1m", ChartWindow::Seconds60}, {"2m", ChartWindow::Seconds120},
    {"5m", ChartWindow::Seconds300}, {"10m", ChartWindow::Seconds600},
    {"Current", ChartWindow::CurrentLap}, {"Previous", ChartWindow::PreviousLap},
    {"Fastest", ChartWindow::FastestLap}, {"Selected", ChartWindow::SelectedLap},
    {"Stint Laps", ChartWindow::StintLaps}, {"All Laps", ChartWindow::AllLaps}
};
static constexpr int kWindowOptionCount = 12;

static constexpr int kToolbarHeight = 44;

namespace {

// Segmented-control button for the toolbar's window-size picker. When checked it
// paints itself as the active QStyle's *default button* — the same blue outline
// the Edit-Layout dialog's on-toggles wear (see ToggleButton in
// EditOverviewLayoutDialog.cpp). QToolButton can't reuse that trick directly:
// the DefaultButton look lives on QStyleOptionButton, which only QPushButton
// feeds the style, so for the checked state we draw a default QPushButton bevel
// + label ourselves (same CE_PushButton + DefaultButton code path the dialog
// hits). Unchecked segments fall through to the normal flat auto-raised look.
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

// No bundled SVG for these — prefer the OS/desktop theme icon (tinted to the
// toolbar foreground on Windows so the monochrome Breeze icons stay visible in
// dark mode), and fall back to Qt's own built-in standard-pixmap icon.
QIcon openRecordingIcon(QWidget* w) {
    return adaptThemeIcon(QIcon::fromTheme(QIcon::ThemeIcon::DocumentOpen),
        w->palette().color(QPalette::WindowText),
        w->style()->standardIcon(QStyle::SP_DialogOpenButton));
}

QIcon editLayoutIcon(QWidget* w) {
    return adaptThemeIcon(QIcon::fromTheme("document-edit"),
        w->palette().color(QPalette::WindowText),
        w->style()->standardIcon(QStyle::SP_FileDialogDetailedView));
}

QIcon settingsIcon(QWidget* w) {
    return adaptThemeIcon(
        QIcon::fromTheme("configure", QIcon::fromTheme("preferences-system")),
        w->palette().color(QPalette::WindowText),
        w->style()->standardIcon(QStyle::SP_FileDialogListView));
}

// "⋯" overflow button — same theme-icon-with-fallback pattern as the icons
// above, ending at the style's horizontal-extension glyph (what Qt's own overflow
// button would use) so it always renders even when the theme lacks an overflow icon.
QIcon overflowIcon(QWidget* w) {
    return adaptThemeIcon(
        QIcon::fromTheme("overflow-menu-symbolic"),
        w->palette().color(QPalette::WindowText),
        w->style()->standardIcon(QStyle::SP_ToolBarHorizontalExtensionButton));
}

QIcon toolbarIcon(QWidget* w, const char* name, QStyle::StandardPixmap fallback) {
    return adaptThemeIcon(QIcon::fromTheme(QString::fromLatin1(name)),
        w->palette().color(QPalette::WindowText), w->style()->standardIcon(fallback));
}

} // namespace

AppToolbar::AppToolbar(const QStringList& pageNames, bool showLabels, QWidget* parent)
    : QToolBar(parent)
{
    setMovable(false);
    setFloatable(false);
    setToolButtonStyle(showLabels ? Qt::ToolButtonTextBesideIcon : Qt::ToolButtonIconOnly);
    setIconSize(QSize(26, 26));
    setContentsMargins(0, 0, 0, 0);
    if (layout()) layout()->setContentsMargins(0, 0, 0, 0);
    if (layout()) layout()->setSpacing(4);
    // Breeze (and other styles) draw the QToolBar's own 1px bottom border across
    // its full width regardless of what the tab bar does — left alone, it shows
    // up as a second line stacked right under our accent underline. QToolBar also
    // reserves its own internal padding/margin around every item regardless of
    // a widget's own size policy — zeroing it here too, since that inset (not
    // anything in the child controls themselves) was the source of unwanted
    // space between the toolbar contents and its true bottom edge.
    setFixedHeight(kToolbarHeight);
    // Sets the toolbar stylesheet (border/margins, + the palette Button shade
    // background — see the method).
    updateColorScheme();

    // Match Electron's header navigation: one non-searchable dropdown showing
    // the active page. Item order remains identical to the owner's page stack,
    // so the selected index can continue to drive QStackedWidget directly.
    QWidget* pageControl = new QWidget;
    auto* pageLayout = new QHBoxLayout(pageControl);
    pageLayout->setContentsMargins(8, 0, 0, 0);
    pageLayout->setSpacing(0);
    pageBtn_ = new QComboBox(pageControl);
    pageBtn_->setFrame(false);
    pageBtn_->addItems(pageNames);
    pageBtn_->setCurrentIndex(0);
    pageBtn_->setToolTip("Page");
    pageLayout->addWidget(pageBtn_);
    pageAct_ = addWidget(pageControl);
    connect(pageBtn_, QOverload<int>::of(&QComboBox::activated),
            this, &AppToolbar::pageSelected);

    // Expanding spacer that pushes the right-hand group over. The session timer
    // rides at the right edge of this spacer (just left of the window segment) so
    // it stays clear of the overflow logic — it's not a standalone toolbar item, so
    // the relayout arithmetic below is untouched and the timer is always visible.
    QWidget* spacer = new QWidget;
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    QHBoxLayout* spacerLay = new QHBoxLayout(spacer);
    spacerLay->setContentsMargins(0, 0, 0, 0);
    spacerLay->setSpacing(0);
    spacerLay->addStretch(1);
    timerLabel_ = new QLabel;
    timerLabel_->setObjectName("sessionTimer");
    timerLabel_->setContentsMargins(8, 0, 8, 0);
    timerLabel_->setAlignment(Qt::AlignVCenter | Qt::AlignRight);
    QFont timerFont = timerLabel_->font();
    timerFont.setBold(true);
    timerLabel_->setFont(timerFont);
    timerLabel_->setToolTip("Session time");
    timerLabel_->hide();   // shown once the first session_time arrives
    spacerLay->addWidget(timerLabel_);
    deltaLabel_ = new QLabel;
    deltaLabel_->setObjectName("sessionDelta");
    deltaLabel_->setContentsMargins(5, 0, 8, 0);
    deltaLabel_->setAlignment(Qt::AlignVCenter | Qt::AlignRight);
    deltaLabel_->setFont(timerFont);
    deltaLabel_->setToolTip("Lap comparison delta");
    deltaLabel_->hide();
    spacerLay->addWidget(deltaLabel_);
    addWidget(spacer);

    // TNRD V6 recordings can project playback onto any recorded participant.
    // Keep the selector independent from the chart/Analyze segment so changing
    // pages never hides it; narrow windows move it into the existing overflow.
    driverBtn_ = new QComboBox(this);
    driverBtn_->setFrame(false);
    driverBtn_->setMinimumContentsLength(14);
    driverBtn_->setMaximumWidth(280);
    driverBtn_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    driverBtn_->setToolTip(
        "Playback driver. Drivers marked Public data only have private status "
        "and damage values hidden by the game.");
    driverAct_ = addWidget(driverBtn_);
    driverAct_->setVisible(false);
    connect(driverBtn_, QOverload<int>::of(&QComboBox::activated),
            this, &AppToolbar::applyPlaybackDriver);

    // Analyze owns a lap-relative X axis, so its navigation controls replace the
    // rolling chart-window selector while that page is active.
    analyzeControls_ = new QWidget;
    auto* analyzeLayout = new QHBoxLayout(analyzeControls_);
    analyzeLayout->setContentsMargins(2, 0, 2, 0);
    analyzeLayout->setSpacing(2);
    auto addAnalyzeButton = [&](QToolButton*& button, const QString& tip, auto signal) {
        button = new QToolButton(analyzeControls_);
        button->setAutoRaise(true);
        button->setFixedSize(32, 32);
        button->setToolTip(tip);
        analyzeLayout->addWidget(button);
        connect(button, &QToolButton::clicked, this, signal);
    };
    addAnalyzeButton(analyzeZoomOut_, "Zoom out", &AppToolbar::analyzeZoomOutRequested);
    addAnalyzeButton(analyzeZoomIn_, "Zoom in", &AppToolbar::analyzeZoomInRequested);
    addAnalyzeButton(analyzePanLeft_, "Pan left", &AppToolbar::analyzePanLeftRequested);
    addAnalyzeButton(analyzePanRight_, "Pan right", &AppToolbar::analyzePanRightRequested);
    addAnalyzeButton(analyzeReset_, "Reset zoom", &AppToolbar::analyzeResetZoomRequested);
    analyzeAct_ = addWidget(analyzeControls_);
    analyzeAct_->setVisible(false);
    setAnalyzeControlsEnabled(false);
    refreshThemedIcons();

    // Window-size selector: a frameless combo box showing the current window,
    // with the options as its dropdown list.
    QWidget* windowTools = new QWidget;
    auto* windowToolsLayout = new QHBoxLayout(windowTools);
    windowToolsLayout->setContentsMargins(0, 0, 0, 0);
    windowToolsLayout->setSpacing(2);
    windowBtn_ = new QComboBox(windowTools);
    windowBtn_->setFrame(false);
    rebuildChartWindowOptions();
    windowBtn_->setToolTip("Chart window");
    connect(windowBtn_, QOverload<int>::of(&QComboBox::activated),
            this, &AppToolbar::applyChartWindow);
    windowToolsLayout->addWidget(windowBtn_);

    referenceLap_ = new QComboBox(windowTools);
    referenceLap_->setFrame(false);
    referenceLap_->setToolTip("Selected reference lap");
    referenceLap_->hide();
    connect(referenceLap_, QOverload<int>::of(&QComboBox::activated), this, [this](int) {
        emit chartReferenceLapChanged(referenceLap_->currentData().toInt());
    });
    windowToolsLayout->addWidget(referenceLap_);

    sectorBtn_ = new QToolButton(windowTools);
    sectorBtn_->setText("S1|S2");
    sectorBtn_->setCheckable(true);
    sectorBtn_->setAutoRaise(true);
    sectorBtn_->setToolTip("Sector Boundaries");
    connect(sectorBtn_, &QToolButton::toggled, this, &AppToolbar::sectorBoundariesChanged);
    windowToolsLayout->addWidget(sectorBtn_);

    syncBtn_ = new QToolButton(windowTools);
    syncBtn_->setText("Sync");
    syncBtn_->setCheckable(true);
    syncBtn_->setAutoRaise(true);
    syncBtn_->setToolTip("Synchronize chart tooltips");
    connect(syncBtn_, &QToolButton::toggled, this, &AppToolbar::cursorSyncChanged);
    windowToolsLayout->addWidget(syncBtn_);
    windowAct_ = addWidget(windowTools);

    openAct_ = addAction(openRecordingIcon(this), "Open File");
    connect(openAct_, &QAction::triggered, this, &AppToolbar::openRecordingRequested);
    editLayoutAct_ = addAction(editLayoutIcon(this), "Edit Layout");
    editLayoutAct_->setEnabled(true); // Default is the Overview page (0)
    connect(editLayoutAct_, &QAction::triggered, this, &AppToolbar::editLayoutRequested);
    settingsAct_ = addAction(settingsIcon(this), "Settings");
    connect(settingsAct_, &QAction::triggered, this, &AppToolbar::settingsRequested);

    // Custom overflow: the toolbar is built from composite custom widgets (page
    // and window dropdowns), which Qt's native QToolBarExtension can't reparent
    // into its popup. Instead we manage it ourselves — relayout() collapses
    // low-priority items into this "⋯" button's menu when the window is too narrow,
    // so the broken native extension never appears.
    overflowMenu_ = new QMenu(this);
    overflowBtn_  = new QToolButton;
    overflowBtn_->setAutoRaise(true);
    overflowBtn_->setIcon(overflowIcon(this));
    overflowBtn_->setPopupMode(QToolButton::InstantPopup);
    overflowBtn_->setMenu(overflowMenu_);
    overflowBtn_->setToolTip("More");
    // Control visibility via the toolbar action so its slot is fully removed when
    // hidden (toggling just the widget leaves a reserved empty slot).
    overflowAct_ = addWidget(overflowBtn_);
    overflowAct_->setVisible(false);
    // Disable Qt's own overflow: its extension button (objectName "qt_toolbar_ext_button")
    // would otherwise flash in/out as items reflow, fighting our ⋯ menu. Keep it
    // permanently hidden via the event filter below.
    extButton_ = findChild<QWidget*>("qt_toolbar_ext_button");
    if (extButton_) {
        extButton_->hide();
        extButton_->installEventFilter(this);
    }
    // Initial pass once the toolbar has a real width (after the window is shown).
    QTimer::singleShot(0, this, [this] {
        relayout();
        // TEMP DIAGNOSTIC (icon blur investigation): log the toolbar's effective icon
        // size + dpr once it's realized, to cross-check against the per-icon requested
        // sizes in tnr_icon_debug.log. Remove along with the engine logging block.
        QFile lf(QCoreApplication::applicationDirPath() + QStringLiteral("/tnr_icon_debug.log"));
        if (lf.open(QIODevice::Append | QIODevice::Text)) {
            QTextStream(&lf)
                << "toolbar iconSize " << iconSize().width() << "x" << iconSize().height()
                << "  devicePixelRatio " << devicePixelRatioF()
                << "  toolButtonStyle " << int(toolButtonStyle()) << "\n";
        }
    });
}

void AppToolbar::setEditLayoutEnabled(bool on) {
    if (editLayoutAct_) editLayoutAct_->setEnabled(on);
}

void AppToolbar::setAnalyzeControlsVisible(bool on) {
    analyzeVisible_ = on;
    relayout();
}

void AppToolbar::setAnalyzeContextWidget(QWidget* widget) {
    if (!widget || analyzeContextAct_) return;
    analyzeContextAct_ = insertWidget(analyzeAct_, widget);
    analyzeContextAct_->setVisible(false);
    relayout();
}

void AppToolbar::setAnalyzeControlsEnabled(bool on) {
    for (auto* button : {analyzeZoomIn_, analyzeZoomOut_, analyzePanLeft_,
                         analyzePanRight_, analyzeReset_})
        if (button) button->setEnabled(on);
}

void AppToolbar::setShowLabels(bool on) {
    setToolButtonStyle(on ? Qt::ToolButtonTextBesideIcon : Qt::ToolButtonIconOnly);
    relayout();   // text-beside-icon changes the icon-action widths
}

void AppToolbar::updateColorScheme() {
    // The toolbar sits on the palette's Button shade (a touch lighter than the
    // window) with a hairline bottom border separating it from the page content
    // below — the same divider style as the tab-bar/pane seam in the Settings
    // window. The border colour is ~30% of the way from the window colour toward
    // the text colour so it stays visible in both light and dark themes; it's
    // recomputed here so it re-themes when the colour scheme changes.
    // (background: palette(button) already neutralises Breeze's OS-scheme "tools
    // area" tint unconditionally, so no style/mode-specific branch is needed.)
    const QColor win = QApplication::palette().color(QPalette::Window);
    const QColor txt = QApplication::palette().color(QPalette::WindowText);
    const QColor borderCol((win.red()   * 7 + txt.red()   * 3) / 10,
                           (win.green() * 7 + txt.green() * 3) / 10,
                           (win.blue()  * 7 + txt.blue()  * 3) / 10);
    setStyleSheet(QString(
        "QToolBar { border: none; border-bottom: 1px solid %1;"
        " margin: 0px; padding: 0px; background: palette(button); }").arg(borderCol.name()));
}

// Toolbar session timer. `sessionTime` is the header session_time carried on
// every packet (live UDP and playback): elapsed seconds since session start.
// Formatted M:SS to mirror the Electron titlebar timer. Guarded on the whole
// second so a burst of same-frame packets doesn't re-set the label needlessly.
void AppToolbar::updateSessionTimer(float sessionTime) {
    if (!timerLabel_ || sessionTime < 0.0f) return;
    const int total = (int)sessionTime;
    if (total == timerSec_ && timerLabel_->isVisible()) return;
    timerSec_ = total;
    timerLabel_->setText(QString("%1:%2")
                             .arg(total / 60)
                             .arg(total % 60, 2, 10, QLatin1Char('0')));
    if (!timerLabel_->isVisible()) timerLabel_->show();
    // The label's footprint changed if it just appeared or grew a digit
    // (e.g. 9:59 → 10:00). Re-run the responsive layout so the reserved timer
    // width stays correct and the toolbar never overflows.
    const int w = timerLabel_->sizeHint().width();
    if (w != timerW_) { timerW_ = w; relayout(); }
}

void AppToolbar::updateSessionDelta(double deltaSeconds) {
    if (!deltaLabel_) return;
    if (!std::isfinite(deltaSeconds)) {
        if (deltaLabel_->isVisible()) {
            deltaLabel_->hide();
            deltaW_ = 0;
            relayout();
        }
        return;
    }

    const QString sign = deltaSeconds >= 0.0 ? QStringLiteral("+")
                                             : QString::fromUtf8("\xE2\x88\x92");
    deltaLabel_->setText(sign + QString::number(std::abs(deltaSeconds), 'f', 3));
    if (deltaSeconds > 0.0)
        deltaLabel_->setStyleSheet(QStringLiteral("color: #C4162A;"));
    else if (deltaSeconds < 0.0)
        deltaLabel_->setStyleSheet(QStringLiteral("color: #37872D;"));
    else
        deltaLabel_->setStyleSheet(QString());
    if (!deltaLabel_->isVisible()) deltaLabel_->show();
    const int w = deltaLabel_->sizeHint().width();
    if (w != deltaW_) { deltaW_ = w; relayout(); }
}

void AppToolbar::resetSessionTimer() {
    timerSec_ = -1;
    timerW_   = 0;
    if (timerLabel_) {
        timerLabel_->clear();
        timerLabel_->hide();
    }
    deltaW_ = 0;
    if (deltaLabel_) {
        deltaLabel_->clear();
        deltaLabel_->hide();
    }
    relayout();   // reclaim the freed timer/delta width for inline items
}

void AppToolbar::resizeEvent(QResizeEvent* e) {
    QToolBar::resizeEvent(e);
    relayout();   // collapse/expand items for the new width
}

void AppToolbar::changeEvent(QEvent* e) {
    QToolBar::changeEvent(e);
    if (e->type() == QEvent::ApplicationPaletteChange || e->type() == QEvent::PaletteChange)
        refreshThemedIcons();
}

bool AppToolbar::eventFilter(QObject* obj, QEvent* e) {
    // Qt's QToolBarLayout re-shows its extension button whenever it thinks items
    // overflow; hide it again every time so only our ⋯ overflow is ever seen.
    if (obj == extButton_ &&
        (e->type() == QEvent::Show || e->type() == QEvent::ShowToParent))
        extButton_->hide();
    return QToolBar::eventFilter(obj, e);
}

void AppToolbar::refreshThemedIcons() {
    // The theme icons are tinted to the palette foreground (on Windows), so
    // rebuild them whenever the palette changes to re-tint for the new mode.
    if (openAct_)       openAct_->setIcon(openRecordingIcon(this));
    if (editLayoutAct_) editLayoutAct_->setIcon(editLayoutIcon(this));
    if (settingsAct_)   settingsAct_->setIcon(settingsIcon(this));
    if (overflowBtn_)   overflowBtn_->setIcon(overflowIcon(this));
    if (analyzeZoomOut_) analyzeZoomOut_->setIcon(toolbarIcon(this, "zoom-out", QStyle::SP_TitleBarMinButton));
    if (analyzeZoomIn_) analyzeZoomIn_->setIcon(toolbarIcon(this, "zoom-in", QStyle::SP_TitleBarMaxButton));
    if (analyzePanLeft_) analyzePanLeft_->setIcon(toolbarIcon(this, "go-previous", QStyle::SP_ArrowLeft));
    if (analyzePanRight_) analyzePanRight_->setIcon(toolbarIcon(this, "go-next", QStyle::SP_ArrowRight));
    if (analyzeReset_) analyzeReset_->setIcon(toolbarIcon(this, "view-refresh", QStyle::SP_BrowserReload));
}

void AppToolbar::applyChartWindow(int idx) {
    if (!windowBtn_ || idx < 0 || idx >= windowBtn_->count()) return;
    const ChartWindow selected = chartWindowFromKey(windowBtn_->itemData(idx).toString());
    window_ = selected;
    emit chartWindowChanged(selected);
    // Reflect the choice on the combo box (the choice may have come from the
    // combo box itself or the overflow menu).
    if (windowBtn_ && windowBtn_->currentIndex() != idx) windowBtn_->setCurrentIndex(idx);
    if (referenceLap_) referenceLap_->setVisible(playback_ && selected == ChartWindow::SelectedLap);
}

void AppToolbar::rebuildChartWindowOptions() {
    if (!windowBtn_) return;
    windowBtn_->blockSignals(true);
    windowBtn_->clear();
    for (int i = 0; i < kWindowOptionCount; ++i) {
        const ChartWindow candidate = kWindowOptions[i].window;
        if (!chartWindowIsAvailable(candidate, lapCoordinatesAvailable_, playback_)) continue;
        windowBtn_->addItem(kWindowOptions[i].label, chartWindowKey(candidate));
    }
    // Electron keeps the persisted value untouched when it is temporarily
    // unavailable, but presents and renders the startup default in its place.
    const ChartWindow displayed = chartWindowIsAvailable(
        window_, lapCoordinatesAvailable_, playback_) ? window_ : ChartWindow::Seconds30;
    const int idx = windowBtn_->findData(chartWindowKey(displayed));
    windowBtn_->setCurrentIndex(idx >= 0 ? idx : 0);
    windowBtn_->blockSignals(false);
}

void AppToolbar::setChartLapAvailability(const QVector<int>& laps, bool playback,
                                         bool lapCoordinatesAvailable)
{
    playback_ = playback;
    lapCoordinatesAvailable_ = lapCoordinatesAvailable;
    rebuildChartWindowOptions();
    const int selected = referenceLap_ ? referenceLap_->currentData().toInt() : 0;
    if (referenceLap_) {
        referenceLap_->blockSignals(true);
        referenceLap_->clear();
        for (int lap : laps) referenceLap_->addItem(QString("Lap %1").arg(lap), lap);
        int idx = referenceLap_->findData(selected);
        if (idx < 0) idx = referenceLap_->findData(1);
        referenceLap_->setCurrentIndex(idx >= 0 ? idx :
            (referenceLap_->count() > 0 ? 0 : -1));
        referenceLap_->blockSignals(false);
        referenceLap_->setVisible(playback &&
            window_ == ChartWindow::SelectedLap);
    }
    if (sectorBtn_) sectorBtn_->setVisible(lapCoordinatesAvailable);
}

void AppToolbar::setChartOptions(ChartWindow window, int referenceLap,
                                 bool sectorBoundaries, bool cursorSync)
{
    window_ = window;
    rebuildChartWindowOptions();
    if (referenceLap_) {
        const int idx = referenceLap_->findData(referenceLap);
        if (idx >= 0) referenceLap_->setCurrentIndex(idx);
        referenceLap_->setVisible(playback_ && window == ChartWindow::SelectedLap);
    }
    if (sectorBtn_) {
        sectorBtn_->blockSignals(true); sectorBtn_->setChecked(sectorBoundaries);
        sectorBtn_->blockSignals(false);
    }
    if (syncBtn_) {
        syncBtn_->blockSignals(true); syncBtn_->setChecked(cursorSync); syncBtn_->blockSignals(false);
    }
}

void AppToolbar::setPlaybackDrivers(const QVector<PlaybackDriverOption>& drivers,
                                    int selectedDriverIndex)
{
    if (!driverBtn_ || !driverAct_) return;
    driverBtn_->blockSignals(true);
    driverBtn_->clear();
    for (const PlaybackDriverOption& driver : drivers)
        driverBtn_->addItem(driver.label, driver.index);
    const int selected = driverBtn_->findData(selectedDriverIndex);
    driverBtn_->setCurrentIndex(selected >= 0 ? selected :
        (driverBtn_->count() > 0 ? 0 : -1));
    driverBtn_->blockSignals(false);
    playbackDriversVisible_ = !drivers.isEmpty();
    relayout();
}

void AppToolbar::clearPlaybackDrivers() {
    if (!driverBtn_ || !driverAct_) return;
    driverBtn_->blockSignals(true);
    driverBtn_->clear();
    driverBtn_->blockSignals(false);
    playbackDriversVisible_ = false;
    driverAct_->setVisible(false);
    relayout();
}

void AppToolbar::applyPlaybackDriver(int idx) {
    if (!driverBtn_ || idx < 0 || idx >= driverBtn_->count()) return;
    if (driverBtn_->currentIndex() != idx) driverBtn_->setCurrentIndex(idx);
    emit playbackDriverChanged(driverBtn_->itemData(idx).toInt());
}

// Collapse low-priority toolbar items into the "⋯" menu when the window is too
// narrow to fit everything, expanding them back as it widens. The compact page
// dropdown remains inline; icon actions and then the chart-window controls move
// into overflow as needed.
void AppToolbar::relayout() {
    if (!overflowAct_ || !pageAct_ || !pageBtn_) return;
    const int avail = width();
    if (avail <= 0) return;
    const int spacing = layout() ? layout()->spacing() : 4;
    // Deliberate slack so we always collapse a little EARLY rather than ever let the
    // toolbar genuinely overflow. A real overflow makes Qt's (suppressed) extension
    // button fight our event filter — show/hide/show — which is the flicker. Showing
    // the ⋯ a few px sooner is harmless; oscillation is not.
    constexpr int kSlack = 40;

    auto actW = [&](QAction* a) -> int {
        QWidget* w = a ? widgetForAction(a) : nullptr;
        return w ? w->sizeHint().width() : 0;
    };
    auto setIconsVisible = [&](bool v) {
        if (openAct_)       openAct_->setVisible(v);
        if (editLayoutAct_) editLayoutAct_->setVisible(v);
        if (settingsAct_)   settingsAct_->setVisible(v);
    };

    // Pure arithmetic from stable sizeHints, so the decision is deterministic —
    // identical for a given width every call, with no overflow oscillation.
    const int wPage  = actW(pageAct_);
    const int wAnalyzeContext = actW(analyzeContextAct_);
    const int wAnalyzeNav = actW(analyzeAct_);
    const int wDriver = playbackDriversVisible_ ? actW(driverAct_) : 0;
    const int wSeg = analyzeVisible_
        ? wAnalyzeContext + wAnalyzeNav + (wAnalyzeContext && wAnalyzeNav ? spacing : 0)
        : actW(windowAct_);
    const int wIcons = actW(openAct_) + actW(editLayoutAct_) + actW(settingsAct_);
    const int wOver  = overflowBtn_->sizeHint().width();
    // The session timer and comparison delta live inside the (otherwise
    // collapsible) spacer, so reserve their visible width. Neither readout joins
    // the overflow menu.
    const int wTimer = ((timerLabel_ && timerLabel_->isVisible())
                            ? timerLabel_->sizeHint().width() : 0)
                     + ((deltaLabel_ && deltaLabel_->isVisible())
                            ? deltaLabel_->sizeHint().width() : 0);
    // Inter-item gaps: 6 toolbar items (page dropdown, spacer, segment, 3 icons)
    // create 5 gaps. The timer adds no gap of its own (it rides in the spacer).
    const int needAll = wPage + wDriver + wSeg + wIcons + wTimer +
        spacing * (playbackDriversVisible_ ? 6 : 5);

    if (avail >= needAll + kSlack) {              // comfortably fits — everything inline
        if (windowAct_) windowAct_->setVisible(!analyzeVisible_);
        if (analyzeContextAct_) analyzeContextAct_->setVisible(analyzeVisible_);
        if (analyzeAct_) analyzeAct_->setVisible(analyzeVisible_);
        if (driverAct_) driverAct_->setVisible(playbackDriversVisible_);
        setIconsVisible(true);
        pageAct_->setVisible(true);
        overflowAct_->setVisible(false);
        return;
    }

    // Need overflow. Collapse icon actions and then the window segment until the
    // always-visible page dropdown plus inline content and ⋯ fit comfortably.
    overflowAct_->setVisible(true);
    const int budget = avail - wOver - spacing - kSlack;
    int inlineW = needAll;
    bool segIn = true, iconsIn = true, analyzeNavIn = true;
    bool driverIn = playbackDriversVisible_;
    if (inlineW > budget && iconsIn) { iconsIn = false; inlineW -= wIcons + spacing; }
    if (inlineW > budget && analyzeVisible_ && analyzeNavIn) {
        analyzeNavIn = false;
        inlineW -= wAnalyzeNav + spacing;
    }
    if (inlineW > budget && segIn && !analyzeVisible_) { segIn = false; inlineW -= wSeg + spacing; }
    if (inlineW > budget && driverIn) { driverIn = false; inlineW -= wDriver + spacing; }

    pageAct_->setVisible(true);
    if (windowAct_) windowAct_->setVisible(!analyzeVisible_ && segIn);
    if (analyzeContextAct_) analyzeContextAct_->setVisible(analyzeVisible_);
    if (analyzeAct_) analyzeAct_->setVisible(analyzeVisible_ && analyzeNavIn);
    if (driverAct_) driverAct_->setVisible(playbackDriversVisible_ && driverIn);
    setIconsVisible(iconsIn);

    // Rebuild the overflow menu from whatever collapsed.
    overflowMenu_->clear();
    if (playbackDriversVisible_ && !driverIn) {
        overflowMenu_->addSection("Playback Driver");
        for (int i = 0; i < driverBtn_->count(); ++i) {
            QAction* driver = overflowMenu_->addAction(driverBtn_->itemText(i));
            driver->setCheckable(true);
            driver->setChecked(i == driverBtn_->currentIndex());
            connect(driver, &QAction::triggered, this,
                    [this, i] { applyPlaybackDriver(i); });
        }
    }
    if (!segIn && !analyzeVisible_) {
        overflowMenu_->addSection("Chart Window");
        for (int i = 0; i < windowBtn_->count(); ++i) {
            QAction* a = overflowMenu_->addAction(windowBtn_->itemText(i));
            a->setCheckable(true);
            a->setChecked(i == windowBtn_->currentIndex());
            connect(a, &QAction::triggered, this, [this, i] { applyChartWindow(i); });
        }
        if (playback_ && window_ == ChartWindow::SelectedLap && referenceLap_) {
            overflowMenu_->addSection("Reference Lap");
            for (int i = 0; i < referenceLap_->count(); ++i) {
                QAction* lap = overflowMenu_->addAction(referenceLap_->itemText(i));
                lap->setCheckable(true);
                lap->setChecked(i == referenceLap_->currentIndex());
                connect(lap, &QAction::triggered, this, [this, i] {
                    referenceLap_->setCurrentIndex(i);
                    emit chartReferenceLapChanged(referenceLap_->itemData(i).toInt());
                });
            }
        }
        if (sectorBtn_ && !sectorBtn_->isHidden()) {
            QAction* sectors = overflowMenu_->addAction("Sector Boundaries");
            sectors->setCheckable(true);
            sectors->setChecked(sectorBtn_->isChecked());
            connect(sectors, &QAction::toggled, sectorBtn_, &QToolButton::setChecked);
        }
        QAction* sync = overflowMenu_->addAction("Synchronize Tooltips");
        sync->setCheckable(true);
        sync->setChecked(syncBtn_ && syncBtn_->isChecked());
        connect(sync, &QAction::toggled, syncBtn_, &QToolButton::setChecked);
    }
    if (analyzeVisible_ && !analyzeNavIn) {
        overflowMenu_->addSection("Analyze navigation");
        QAction* zoomOut = overflowMenu_->addAction("Zoom out");
        connect(zoomOut, &QAction::triggered, this, &AppToolbar::analyzeZoomOutRequested);
        QAction* zoomIn = overflowMenu_->addAction("Zoom in");
        connect(zoomIn, &QAction::triggered, this, &AppToolbar::analyzeZoomInRequested);
        QAction* panLeft = overflowMenu_->addAction("Pan left");
        connect(panLeft, &QAction::triggered, this, &AppToolbar::analyzePanLeftRequested);
        QAction* panRight = overflowMenu_->addAction("Pan right");
        connect(panRight, &QAction::triggered, this, &AppToolbar::analyzePanRightRequested);
        QAction* reset = overflowMenu_->addAction("Reset zoom");
        connect(reset, &QAction::triggered, this, &AppToolbar::analyzeResetZoomRequested);
    }
    if (!iconsIn) {
        overflowMenu_->addSeparator();
        QAction* mo = overflowMenu_->addAction(openRecordingIcon(this), "Open File");
        connect(mo, &QAction::triggered, openAct_, &QAction::trigger);
        QAction* me = overflowMenu_->addAction(editLayoutIcon(this), "Edit Layout");
        me->setEnabled(editLayoutAct_ && editLayoutAct_->isEnabled());
        connect(me, &QAction::triggered, editLayoutAct_, &QAction::trigger);
        QAction* ms = overflowMenu_->addAction(settingsIcon(this), "Settings");
        connect(ms, &QAction::triggered, settingsAct_, &QAction::trigger);
    }
}
