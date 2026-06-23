#include "MainWindow.h"
#include "TelemetryChart.h"
#include "components/GearChart.h"
#include "components/InputsChart.h"
#include "components/SteeringChart.h"
#include "components/PowerChart.h"
#include "TnrdPlayer.h"
#include "SessionModel.h"
#include "components/EditOverviewLayoutDialog.h"
#include "components/EditInputLayoutDialog.h"
#include "components/EditPowerLayoutDialog.h"
#include "components/EditMiscLayoutDialog.h"
#include "components/GForceChart.h"
#include "components/RideHeightChart.h"
#include "components/TyreCardsWidget.h"
#include "components/TyreChartsWidget.h"
#include "components/SettingsDialog.h"
#include "components/TrackMapWidget.h"
#include "components/TyreHelpers.h"
#include "components/ToastEvents.h"
#include "components/Toast.h"
#include "BreezePalette.h"
#include "IconUtils.h"

#include <QApplication>
#include <QToolBar>
#include <QMenu>
#include <QStackedWidget>
#include <QComboBox>
#include <QLabel>
#include <QFrame>
#include <QWidget>
#include <QHBoxLayout>
#include <QFont>
#include <QSlider>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QFileDialog>
#include <QMessageBox>
#include <QFileInfo>
#include <QSizePolicy>
#include <QStyleHints>
#include <QCoreApplication>
#include <QUdpSocket>
#include <QTimer>
#include <QPainter>
#include <QImage>
#include <QPixmap>
#include <QIconEngine>
#include <QResizeEvent>
#include <QCloseEvent>
#include <QMoveEvent>
#include <QWindowStateChangeEvent>
#include <QGuiApplication>
#include <QScreen>
#include <QWindow>
#include <QProgressBar>
#include <QStyle>
#include <QAction>
#include <QTabBar>
#include <QToolButton>
#include <QButtonGroup>
#include <QStyleFactory>
#include <QStyleOptionButton>
#include <QStylePainter>
#include <QLocale>

#include <chrono>
#include <ctime>
#include <cstdio>
#include <algorithm>
#include <cctype>

#include "protocols/protocol.h"
#include "protocols/f1_24.h"
#include "protocols/f1_25.h"

// ── Packet IDs ─────────────────────────────────────────────────────────────

static constexpr int PID_MOTION       = 0;
static constexpr int PID_SESSION      = 1;
static constexpr int PID_LAP_DATA     = 2;
static constexpr int PID_EVENT        = 3;
static constexpr int PID_PARTICIPANTS = 4;
static constexpr int PID_CAR_TEL     = 6;
static constexpr int PID_CAR_STATUS  = 7;
static constexpr int PID_CAR_DAMAGE  = 10;
static constexpr int PID_MOTION_EX   = 13;

static constexpr int HEADER_SIZE = 29;

static const std::unordered_set<int> FRAME_SAMPLED = {
    PID_MOTION, PID_CAR_TEL, PID_MOTION_EX
};
static const std::unordered_map<int, int> SLOW_RATE_MS = {
    { PID_SESSION, 0 }, { PID_LAP_DATA, 500 }, { PID_CAR_STATUS, 500 },
    { PID_CAR_DAMAGE, 500 }, { PID_PARTICIPANTS, 5000 }, { PID_EVENT, 0 }
};
static const std::unordered_set<std::string> DEDUPE_TYPES = {
    "session", "tyre_sets", "participants", "all_status", "status", "timing", "damage"
};

// Chart window-size options, shown as a segmented toolbar control.
static const struct { const char* label; float secs; } kWindowOptions[] = {
    {"15s", 15}, {"30s", 30}, {"1m", 60},
    {"2m", 120}, {"5m", 300}, {"10m", 600}
};
static constexpr int kWindowOptionCount = 6;

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

} // namespace

// Removed TintedIconEngine and adaptThemeIcon, now in IconUtils.h

static QIcon playPauseIcon(bool playing, QWidget* w, const QColor& tint) {
    return adaptThemeIcon(
        QIcon::fromTheme(playing ? QIcon::ThemeIcon::MediaPlaybackPause
                                 : QIcon::ThemeIcon::MediaPlaybackStart),
        tint,
        w->style()->standardIcon(playing ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay));
}

// No bundled SVG for these two — prefer the OS/desktop theme icon (tinted to the
// toolbar foreground on Windows so the monochrome Breeze icons stay visible in
// dark mode), and fall back to Qt's own built-in standard-pixmap icon.
static QIcon openRecordingIcon(QWidget* w) {
    return adaptThemeIcon(QIcon::fromTheme(QIcon::ThemeIcon::DocumentOpen),
        w->palette().color(QPalette::WindowText),
        w->style()->standardIcon(QStyle::SP_DialogOpenButton));
}

static QIcon editLayoutIcon(QWidget* w) {
    return adaptThemeIcon(QIcon::fromTheme("document-edit"),
        w->palette().color(QPalette::WindowText),
        w->style()->standardIcon(QStyle::SP_FileDialogDetailedView));
}

static QIcon settingsIcon(QWidget* w) {
    return adaptThemeIcon(
        QIcon::fromTheme("configure", QIcon::fromTheme("preferences-system")),
        w->palette().color(QPalette::WindowText),
        w->style()->standardIcon(QStyle::SP_FileDialogListView));
}

static QIcon closeRecordingIcon(QWidget* w) {
    return adaptThemeIcon(
        QIcon::fromTheme("window-close", QIcon::fromTheme("process-stop")),
        w->palette().color(QPalette::WindowText),
        w->style()->standardIcon(QStyle::SP_DialogCloseButton));
}

static QIcon seekBackwardIcon(QWidget* w, const QColor& tint) {
    return adaptThemeIcon(
        QIcon::fromTheme("media-seek-backward", QIcon::fromTheme("go-previous")),
        tint,
        w->style()->standardIcon(QStyle::SP_MediaSeekBackward));
}

static QIcon seekForwardIcon(QWidget* w, const QColor& tint) {
    return adaptThemeIcon(
        QIcon::fromTheme("media-seek-forward", QIcon::fromTheme("go-next")),
        tint,
        w->style()->standardIcon(QStyle::SP_MediaSeekForward));
}

// "⋯" overflow button — same theme-icon-with-fallback pattern as the toolbar icons
// above, ending at the style's horizontal-extension glyph (what Qt's own overflow
// button would use) so it always renders even when the theme lacks an overflow icon.
static QIcon overflowIcon(QWidget* w) {
    return adaptThemeIcon(
        QIcon::fromTheme("overflow-menu-symbolic"),
        w->palette().color(QPalette::WindowText),
        w->style()->standardIcon(QStyle::SP_ToolBarHorizontalExtensionButton));
}

// QSlider's click behaviour is style-dependent: Windows' native style jumps the
// handle straight to the clicked position, but Linux styles (Breeze, Fusion,
// GTK) treat a groove click as a page step in that direction instead — hence
// the playback bar "jumping by a few seconds" instead of seeking to the click.
// Override to always seek to the clicked position, and ignore the wheel so
// scrolling over the bar doesn't nudge playback either.
class ScrubSlider : public QSlider {
public:
    using QSlider::QSlider;
protected:
    // Handled entirely ourselves rather than delegating to QSlider's built-in
    // press/move handling, whose drag-tracking only engages for clicks that
    // land exactly on the handle — clicking the groove wouldn't let a drag
    // that started there continue to track the cursor.
    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() != Qt::LeftButton) { QSlider::mousePressEvent(e); return; }
        dragging_ = true;
        seekToPos(e->pos().x());
        e->accept();
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (!dragging_) { QSlider::mouseMoveEvent(e); return; }
        seekToPos(e->pos().x());
        e->accept();
    }
    void mouseReleaseEvent(QMouseEvent* e) override {
        if (!dragging_) { QSlider::mouseReleaseEvent(e); return; }
        dragging_ = false;
        e->accept();
    }
    void wheelEvent(QWheelEvent* e) override { e->ignore(); }
private:
    void seekToPos(int x) {
        const double ratio = qBound(0.0, double(x) / qMax(1, width()), 1.0);
        setValue(minimum() + qRound(ratio * (maximum() - minimum())));
    }
    bool dragging_ = false;
};

static void setDmgValue(QLabel* lbl, int val) {
    if (val < 0) { lbl->setText("—"); lbl->setStyleSheet(""); return; }
    lbl->setText(QString::number(val));
    lbl->setStyleSheet(val == 0 ? "color: #37872D;" : "color: #C4162A;");
}

// A geometry "looks maximized" if it (nearly) fills the screen's available area.
// Such a value must never be treated as the windowed/normal size: an earlier bug
// recorded the maximized size as the normal geometry, and restoring to it makes
// un-maximize look like a no-op. Rejecting it (on load and while tracking) both
// clears any already-poisoned saved value and stops it from regenerating.
static bool looksMaximized(const QRect& g, const QScreen* s) {
    if (!g.isValid()) return false;
    const QScreen* scr = s ? s : QGuiApplication::primaryScreen();
    if (!scr) return false;
    const QRect avail = scr->availableGeometry();
    return g.width() >= avail.width() - 12 && g.height() >= avail.height() - 12;
}

// ── Construction ───────────────────────────────────────────────────────────

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle("Track N Race Background Recorder");
    setMinimumSize(480, 520);   // toolbar collapses into its ⋯ menu below ~full width
    // Restore the last windowed size/position, then maximize directly if we were
    // closed maximized — setting the state before the first show maps the window
    // maximized straight away (no small-window flash). Un-maximizing afterwards is
    // corrected back to normalGeometry_ in changeEvent (the WM has no pre-maximize
    // geometry to restore to when we launch already maximized). See closeEvent.
    QRect saved = settings.value("window/normalGeometry").toRect();
    if (!saved.isValid() || looksMaximized(saved, nullptr)) {
        // First run, or a poisoned saved value (the maximized size stored as the
        // windowed geometry) — fall back to a sane centred default so un-maximize
        // always lands on a real window.
        const QScreen* scr = QGuiApplication::primaryScreen();
        const QRect avail = scr ? scr->availableGeometry() : QRect(0, 0, 1280, 800);
        saved = QRect(QPoint(0, 0), QSize(780, 580));
        saved.moveCenter(avail.center());
    }
    normalGeometry_ = saved;
    setGeometry(saved);
    if (settings.value("window/maximized", false).toBool())
        setWindowState(windowState() | Qt::WindowMaximized);

    outputDirectory = settings.value("outputDirectory").toString();
    wantRecord      = settings.value("autoRecord", false).toBool();

    const QString theme = settings.value("theme", "system").toString();
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    if (theme == "light")
        QApplication::styleHints()->setColorScheme(Qt::ColorScheme::Light);
    else if (theme == "dark")
        QApplication::styleHints()->setColorScheme(Qt::ColorScheme::Dark);
#endif

    QToolBar* toolbar = new QToolBar(this);
    toolbar->setMovable(false);
    toolbar->setFloatable(false);
    toolbar->setToolButtonStyle(settings.value("ui/toolbarShowLabels", false).toBool()
        ? Qt::ToolButtonTextBesideIcon : Qt::ToolButtonIconOnly);
    toolbar->setContentsMargins(0, 0, 0, 0);
    if (toolbar->layout()) toolbar->layout()->setContentsMargins(0, 0, 0, 0);
    if (toolbar->layout()) toolbar->layout()->setSpacing(4);
    // Breeze (and other styles) draw the QToolBar's own 1px bottom border across
    // its full width regardless of what the tab bar does — left alone, it shows
    // up as a second line stacked right under our accent underline. QToolBar also
    // reserves its own internal padding/margin around every item regardless of
    // a widget's own size policy — zeroing it here too, since that inset (not
    // anything in the page buttons themselves) was the source of the remaining
    // gap between the active-page underline and the toolbar's true bottom edge.
    static constexpr int kToolbarHeight = 44;
    toolbar->setFixedHeight(kToolbarHeight);
    addToolBar(Qt::TopToolBarArea, toolbar);
    toolbar_ = toolbar;
    // Sets the toolbar stylesheet (border/margins, + a window-colour background
    // when the app's mode differs from the OS — see the method).
    updateToolbarColorScheme();
    // QMainWindow draws its own separator line between the toolbar area and the
    // central widget — a different element again from QToolBar's/QTabBar's own
    // borders, and the most likely source of the line that's survived every fix
    // to those two so far.
    setStyleSheet("QMainWindow::separator { width: 0px; height: 0px; background: transparent; }");

    // Page switcher: plain checkable QToolButtons in an exclusive group, same
    // approach as the window-size segmented control below. QTabBar was tried
    // first, but it has a hardcoded internal paint call (PE_FrameTabBarBase)
    // for the base line under inactive tabs that's supposed to be suppressed by
    // documentMode(true) and isn't, in this style/Qt-version combination — and
    // that line isn't reachable through any stylesheet rule. Plain QToolButtons
    // have no such native "tab base" painting path, so there's nothing for an
    // unselected button to draw beyond what its own (empty) stylesheet says.
    static const char* kPageNames[] = { "Overview", "Standings", "Session", "Tyres", "Input", "Power", "Misc" };
    QWidget* pageTabsWidget = new QWidget;
    pageTabsWidget->setFixedHeight(kToolbarHeight);
    QHBoxLayout* pageTabsLay = new QHBoxLayout(pageTabsWidget);
    pageTabsLay->setContentsMargins(0, 0, 0, 0);
    pageTabsLay->setSpacing(4);
    tb_pageGroup_ = new QButtonGroup(this);
    QButtonGroup* pageGroup = tb_pageGroup_;
    pageGroup->setExclusive(true);
    static constexpr int kPageCount = 7;
    static constexpr int kUnderlineWidth = 2;
    const QString accent = QApplication::palette().color(QPalette::Highlight).name();
    // Every button — checked or not — reserves the same border-bottom width
    // (transparent unless checked), so switching pages only changes its color,
    // never shifts the text within the button's fixed height.
    const QString pageBtnStyle = QString(
        "QToolButton { padding: 0px 14px; border: none; background: transparent;"
        " border-bottom: %1px solid transparent; }"
        "QToolButton:checked { border-bottom: %1px solid %2; }"
    ).arg(kUnderlineWidth).arg(accent);
    for (int i = 0; i < kPageCount; ++i) {
        QToolButton* b = new QToolButton;
        b->setText(kPageNames[i]);
        b->setCheckable(true);
        b->setAutoRaise(true);
        b->setFixedHeight(kToolbarHeight - 2);
        b->setStyleSheet(pageBtnStyle);
        // Pin each tab to its natural width so it can't compress: when space runs
        // out the only way the strip shrinks is by *hiding* a whole tab (handled by
        // relayoutToolbar). Otherwise the buttons and the toolbar both try to shrink
        // at once and the overflow measurement never settles — that was the flicker.
        b->ensurePolished();
        b->setFixedWidth(b->sizeHint().width());
        pageGroup->addButton(b, i);
        pageTabsLay->addWidget(b);
        tb_pageButtons_.push_back(b);
    }
    static_cast<QToolButton*>(pageGroup->button(0))->setChecked(true);   // default: Overview
    toolbar->addWidget(pageTabsWidget);

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
    tb_timerLabel_ = new QLabel;
    tb_timerLabel_->setObjectName("sessionTimer");
    tb_timerLabel_->setContentsMargins(8, 0, 8, 0);
    tb_timerLabel_->setAlignment(Qt::AlignVCenter | Qt::AlignRight);
    QFont timerFont = tb_timerLabel_->font();
    timerFont.setBold(true);
    tb_timerLabel_->setFont(timerFont);
    tb_timerLabel_->setToolTip("Session time");
    tb_timerLabel_->hide();   // shown once the first session_time arrives
    spacerLay->addWidget(tb_timerLabel_);
    toolbar->addWidget(spacer);

    // Window-size segmented control: an exclusive row of checkable QToolButtons
    // drawn edge-to-edge, styled by the active QStyle (so it renders native to
    // whatever platform/theme is running, not a fixed look).
    QWidget* windowSeg = new QWidget;
    tb_windowSeg_ = windowSeg;
    QHBoxLayout* segLay = new QHBoxLayout(windowSeg);
    segLay->setContentsMargins(0, 0, 0, 0);
    segLay->setSpacing(0);
    tb_windowGroup_ = new QButtonGroup(this);
    QButtonGroup* windowGroup = tb_windowGroup_;
    windowGroup->setExclusive(true);
    for (int i = 0; i < kWindowOptionCount; ++i) {
        SegmentButton* b = new SegmentButton;
        b->setText(kWindowOptions[i].label);
        b->setCheckable(true);
        b->setAutoRaise(true);
        windowGroup->addButton(b, i);
        segLay->addWidget(b);
    }
    static_cast<QToolButton*>(windowGroup->button(1))->setChecked(true);   // default 30s
    tb_windowAct_ = toolbar->addWidget(windowSeg);
    connect(windowGroup, &QButtonGroup::idClicked, this, [this](int idx) { applyChartWindow(idx); });

    openAct_ = toolbar->addAction(openRecordingIcon(this), "Open Recording");
    editLayoutAct_ = toolbar->addAction(editLayoutIcon(this), "Edit Layout");
    editLayoutAct_->setEnabled(true); // Default is Overview page (0)
    connect(editLayoutAct_, &QAction::triggered, this, [this] {
        if (currentPage_ == 0) {
            EditOverviewLayoutDialog* dlg = new EditOverviewLayoutDialog(this, this);
            connect(dlg, &QDialog::finished, dlg, &QObject::deleteLater);
            dlg->show();
        } else if (currentPage_ == 4) {
            EditInputLayoutDialog* dlg = new EditInputLayoutDialog(this, this);
            connect(dlg, &QDialog::finished, dlg, &QObject::deleteLater);
            dlg->show();
        } else if (currentPage_ == 5) {
            EditPowerLayoutDialog* dlg = new EditPowerLayoutDialog(this, this);
            connect(dlg, &QDialog::finished, dlg, &QObject::deleteLater);
            dlg->show();
        } else if (currentPage_ == 6) {
            EditMiscLayoutDialog* dlg = new EditMiscLayoutDialog(this, this);
            connect(dlg, &QDialog::finished, dlg, &QObject::deleteLater);
            dlg->show();
        }
    });
    settingsAct_ = toolbar->addAction(settingsIcon(this), "Settings");
    connect(settingsAct_, &QAction::triggered, this, [this] {
        SettingsDialog* dlg = new SettingsDialog(this, this);
        connect(dlg, &QDialog::finished, dlg, &QObject::deleteLater);
        dlg->show();
    });

    // Custom overflow: the toolbar is built from composite custom widgets (page
    // tabs, window-size segment), which Qt's native QToolBarExtension can't reparent
    // into its popup. Instead we manage it ourselves — relayoutToolbar() collapses
    // low-priority items into this "⋯" button's menu when the window is too narrow,
    // so the broken native extension never appears.
    tb_overflowMenu_ = new QMenu(this);
    tb_overflowBtn_  = new QToolButton;
    tb_overflowBtn_->setAutoRaise(true);
    tb_overflowBtn_->setIcon(overflowIcon(this));
    tb_overflowBtn_->setPopupMode(QToolButton::InstantPopup);
    tb_overflowBtn_->setMenu(tb_overflowMenu_);
    tb_overflowBtn_->setToolTip("More");
    // Control visibility via the toolbar action so its slot is fully removed when
    // hidden (toggling just the widget leaves a reserved empty slot).
    tb_overflowAct_ = toolbar->addWidget(tb_overflowBtn_);
    tb_overflowAct_->setVisible(false);
    // Disable Qt's own overflow: its extension button (objectName "qt_toolbar_ext_button")
    // would otherwise flash in/out as items reflow, fighting our ⋯ menu. Keep it
    // permanently hidden via the event filter below.
    tb_extButton_ = toolbar->findChild<QWidget*>("qt_toolbar_ext_button");
    if (tb_extButton_) {
        tb_extButton_->hide();
        tb_extButton_->installEventFilter(this);
    }
    // Initial pass once the toolbar has a real width (after the window is shown).
    QTimer::singleShot(0, this, [this] { relayoutToolbar(); });

    // Lap-aware session model — the chart's single source of truth, fed by both
    // live UDP and playback. Created before the Overview tab so the chart can bind it.
    model_ = new SessionModel(this);

    QStackedWidget* stack = new QStackedWidget(this);
    stack->addWidget(buildOverviewTab());   // index 0
    stack->addWidget(buildStandingsPage()); // index 1
    stack->addWidget(buildSessionPage());   // index 2
    stack->addWidget(buildTyresPage());     // index 3
    stack->addWidget(buildInputPage());     // index 4
    stack->addWidget(buildPowerPage());     // index 5
    stack->addWidget(buildMiscPage());      // index 6

    // Coalesces panel rebuilds to ~30 Hz so bursts of packets can't lock the UI.
    uiRefreshTimer_ = new QTimer(this);
    uiRefreshTimer_->setSingleShot(true);
    uiRefreshTimer_->setInterval(33);
    connect(uiRefreshTimer_, &QTimer::timeout, this, &MainWindow::flushUiRefresh);

    // ── Bottom playback bar ────────────────────────────────────────────────────
    player_ = new TnrdPlayer(this);

    pb_bar_ = new QWidget(this);
    pb_bar_->setAutoFillBackground(true);
    {
        QPalette pal = pb_bar_->palette();
        pal.setColor(QPalette::Window, palette().color(QPalette::Window));
        pb_bar_->setPalette(pal);
    }
    pb_bar_->setFixedHeight(48);
    auto* pbLayout = new QHBoxLayout(pb_bar_);
    pbLayout->setContentsMargins(12, 0, 12, 0);
    pbLayout->setSpacing(10);

    const QColor iconTint = palette().color(QPalette::Text);

    pb_seekBackBtn_ = new QPushButton(pb_bar_);
    pb_seekBackBtn_->setIcon(seekBackwardIcon(this, iconTint));
    pb_seekBackBtn_->setIconSize(QSize(20, 20));
    pb_seekBackBtn_->setFixedSize(34, 34);
    pb_seekBackBtn_->setFlat(true);
    pb_seekBackBtn_->setToolTip("Skip Backward 5s");
    pbLayout->addWidget(pb_seekBackBtn_);

    pb_playBtn_ = new QPushButton(pb_bar_);
    pb_playBtn_->setIcon(playPauseIcon(false, this, iconTint));
    pb_playBtn_->setIconSize(QSize(20, 20));
    pb_playBtn_->setFixedSize(34, 34);
    pb_playBtn_->setFlat(true);
    pbLayout->addWidget(pb_playBtn_);

    pb_seekFwdBtn_ = new QPushButton(pb_bar_);
    pb_seekFwdBtn_->setIcon(seekForwardIcon(this, iconTint));
    pb_seekFwdBtn_->setIconSize(QSize(20, 20));
    pb_seekFwdBtn_->setFixedSize(34, 34);
    pb_seekFwdBtn_->setFlat(true);
    pb_seekFwdBtn_->setToolTip("Skip Forward 5s");
    pbLayout->addWidget(pb_seekFwdBtn_);

    pb_slider_ = new ScrubSlider(Qt::Horizontal, pb_bar_);
    pb_slider_->setRange(0, 1000);
    pb_slider_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    pbLayout->addWidget(pb_slider_);

    pb_timeLabel_ = new QLabel("0:00 / 0:00", pb_bar_);
    pbLayout->addWidget(pb_timeLabel_);

    pb_lapCombo_ = new QComboBox(pb_bar_);
    pb_lapCombo_->addItem("Select Lap...", -1.0f);
    pbLayout->addWidget(pb_lapCombo_);

    pb_speedCombo_ = new QComboBox(pb_bar_);
    pb_speedCombo_->addItem("0.25×", 0.25f);
    pb_speedCombo_->addItem("0.5×",  0.5f);
    pb_speedCombo_->addItem("1×",    1.0f);
    pb_speedCombo_->addItem("2×",    2.0f);
    pb_speedCombo_->addItem("4×",    4.0f);
    pb_speedCombo_->setCurrentIndex(2);
    pbLayout->addWidget(pb_speedCombo_);

    auto* closeRecBtn = new QPushButton(pb_bar_);
    closeRecBtn->setIcon(closeRecordingIcon(this));
    closeRecBtn->setIconSize(QSize(20, 20));
    closeRecBtn->setFixedSize(34, 34);
    closeRecBtn->setFlat(true);
    closeRecBtn->setToolTip("Close Recording");
    pbLayout->addWidget(closeRecBtn);

    pb_bar_->hide();

    pb_sep_ = new QFrame(this);
    pb_sep_->setFrameShape(QFrame::HLine);
    pb_sep_->setFrameShadow(QFrame::Sunken);
    pb_sep_->hide();

    // Stack + separator + playback bar stacked vertically as the central widget
    container_ = new QWidget(this);
    auto* vbox = new QVBoxLayout(container_);
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(0);
    vbox->addWidget(stack);
    vbox->addWidget(pb_sep_);
    vbox->addWidget(pb_bar_);
    setCentralWidget(container_);

    // Event toasts: top-right of the screen, stacked, max a few at once (rest queue).
    // These are process-wide statics on the vendored Toast class, set once.
    Toast::setPosition(ToastPosition::TOP_RIGHT);
    Toast::setMaximumOnScreen(4);
    Toast::setSpacing(10);
    Toast::setOffset(20, 20);

    // Loading overlay (shown over the entire central area while decompressing/indexing)
    loadingOverlay_ = new QWidget(container_);
    loadingOverlay_->setAutoFillBackground(true);
    {
        QPalette pal = loadingOverlay_->palette();
        pal.setColor(QPalette::Window, palette().color(QPalette::Window));
        loadingOverlay_->setPalette(pal);
    }
    auto* ol = new QVBoxLayout(loadingOverlay_);
    ol->setAlignment(Qt::AlignCenter);
    ol->setSpacing(12);
    auto* loadingLabel = new QLabel("Loading recording…", loadingOverlay_);
    loadingLabel->setAlignment(Qt::AlignCenter);
    ol->addWidget(loadingLabel);
    auto* spinner = new QProgressBar(loadingOverlay_);
    spinner->setRange(0, 0);
    spinner->setFixedWidth(300);
    spinner->setTextVisible(false);
    ol->addWidget(spinner);
    loadingOverlay_->hide();

    connect(pageGroup, &QButtonGroup::idClicked, stack, &QStackedWidget::setCurrentIndex);
    connect(pageGroup, &QButtonGroup::idClicked, this, [this](int i) {
        currentPage_ = i;       // refresh the newly-shown page from any pending data
        editLayoutAct_->setEnabled(i == 0 || i == 4 || i == 5 || i == 6);
        flushUiRefresh();
        relayoutToolbar();      // the active tab is kept inline — re-evaluate overflow
    });

    // Helper for formatting session time as M:SS
    auto fmtTime = [](float s) -> QString {
        int m   = (int)s / 60;
        int sec = (int)s % 60;
        return QString("%1:%2").arg(m).arg(sec, 2, 10, QChar('0'));
    };

    connect(openAct_, &QAction::triggered, this, [this] {
        QString path = QFileDialog::getOpenFileName(
            this, "Open Recording", outputDirectory,
            "TNRD Recordings (*.tnrd *.trnd)");
        if (!path.isEmpty()) {
            QMessageBox msgBox(this);
            msgBox.setWindowTitle("Open Session File");
            msgBox.setText("Are you sure you want to open this file?");
            msgBox.setInformativeText("Opening it will stop the event bridge and if you have an session right now with the game, it will be closed.\n\nSelected file: " + QFileInfo(path).fileName());
            msgBox.setIcon(QMessageBox::Warning);
            msgBox.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
            msgBox.setDefaultButton(QMessageBox::No);
            if (msgBox.exec() == QMessageBox::Yes) {
                player_->load(path);
            }
        }
    });

    connect(player_, &TnrdPlayer::loadingStarted, this, [this] {
        loadingOverlay_->setGeometry(container_->rect());
        loadingOverlay_->raise();
        loadingOverlay_->show();
    });

    connect(player_, &TnrdPlayer::loadFailed, this, [this] {
        loadingOverlay_->hide();
        QMessageBox::warning(this, "Load Failed", "Could not open the recording file.");
    });

    connect(player_, &TnrdPlayer::loaded, this, [this](const nlohmann::json& hdr) {
        loadingOverlay_->hide();
        inPlayback_ = true;
        // Clear any frozen live value; the first replayed packet sets it afresh.
        resetSessionTimer();
        // Hand the chart the whole pre-scanned session; it now drives off currentTime.
        if (model_) model_->load(player_->takeScannedData());
        
        if (pb_lapCombo_) {
            pb_lapCombo_->blockSignals(true);
            pb_lapCombo_->clear();
            pb_lapCombo_->addItem("Select Lap...", -1.0f);
            if (model_) {
                for (const auto& lap : model_->data().laps) {
                    pb_lapCombo_->addItem(QString("Lap %1").arg(lap.lapNum), lap.startSessionTime);
                }
            }
            pb_lapCombo_->setCurrentIndex(0);
            pb_lapCombo_->blockSignals(false);
        }
        if (chart) { chart->setPlaybackMode(true); chart->setCurrentTime(player_->currentTime()); }
        if (gearChart_) { gearChart_->setPlaybackMode(true); gearChart_->setCurrentTime(player_->currentTime()); }
        if (inputsChart_) { inputsChart_->setPlaybackMode(true); inputsChart_->setCurrentTime(player_->currentTime()); }
        if (steeringChart_) { steeringChart_->setPlaybackMode(true); steeringChart_->setCurrentTime(player_->currentTime()); }
        if (pp_splitChart) { pp_splitChart->setPlaybackMode(true); pp_splitChart->setCurrentTime(player_->currentTime()); }
        if (pp_harvestChart) { pp_harvestChart->setPlaybackMode(true); pp_harvestChart->setCurrentTime(player_->currentTime()); }
        if (pp_storeChart) { pp_storeChart->setPlaybackMode(true); pp_storeChart->setCurrentTime(player_->currentTime()); }
        if (pp_fuelChart) { pp_fuelChart->setPlaybackMode(true); pp_fuelChart->setCurrentTime(player_->currentTime()); }
        if (gforceChart_) { gforceChart_->setPlaybackMode(true); gforceChart_->setCurrentTime(player_->currentTime()); }
        if (rideHeightChart_) { rideHeightChart_->setPlaybackMode(true); rideHeightChart_->setCurrentTime(player_->currentTime()); }
        if (ov_tyreCharts_) { ov_tyreCharts_->setPlaybackMode(true); ov_tyreCharts_->setCurrentTime(player_->currentTime()); }
        if (ov_compareBtn_) ov_compareBtn_->setEnabled(true);
        closeActiveStream();
        pb_sep_->show();
        pb_bar_->show();
        pb_playBtn_->setIcon(playPauseIcon(false, this, palette().color(QPalette::Text)));
        pbLastPlaying_ = false;
        pb_slider_->setValue(0);
        pb_speedCombo_->setCurrentIndex(2); // reset to 1×
        player_->setSpeed(1.0f);
        QString trackName = QString::fromStdString(hdr.value("track_name", "Unknown"));
        QString sessName  = QString::fromStdString(hdr.value("session_name", "Unknown"));
        setWindowTitle(QString("Track N Race — %1 %2 [Playback]").arg(trackName, sessName));
    });

    connect(player_, &TnrdPlayer::packetReady, this, [this](const nlohmann::json& j) {
        emitLiveData(j);   // panels only — the chart reads the pre-scanned model
    });

    // A seek replays a state snapshot (incl. the session packet); swallow the one
    // safety-car toast that snapshot would otherwise raise — race_events aren't
    // replayed on seek, so those need no special handling.
    connect(player_, &TnrdPlayer::seeked, this, [this] { scSuppressOnce_ = true; });

    connect(player_, &TnrdPlayer::stateChanged, this,
            [this, fmtTime](bool playing, float cur, float total, float /*speed*/) {
        // `cur` is session-relative (for the slider); the model is keyed on absolute
        // session_time, so hand the chart the absolute playhead.
        if (chart) chart->setCurrentTime(player_->currentTime());
        if (gearChart_) gearChart_->setCurrentTime(player_->currentTime());
        if (inputsChart_) inputsChart_->setCurrentTime(player_->currentTime());
        if (steeringChart_) steeringChart_->setCurrentTime(player_->currentTime());
        if (pp_splitChart) pp_splitChart->setCurrentTime(player_->currentTime());
        if (pp_harvestChart) pp_harvestChart->setCurrentTime(player_->currentTime());
        if (pp_storeChart) pp_storeChart->setCurrentTime(player_->currentTime());
        if (pp_fuelChart) pp_fuelChart->setCurrentTime(player_->currentTime());
        if (gforceChart_) gforceChart_->setCurrentTime(player_->currentTime());
        if (rideHeightChart_) rideHeightChart_->setCurrentTime(player_->currentTime());
        if (ov_tyreCharts_) ov_tyreCharts_->setCurrentTime(player_->currentTime());
        if (playing != pbLastPlaying_) {
            pb_playBtn_->setIcon(playPauseIcon(playing, this, palette().color(QPalette::Text)));
            pbLastPlaying_ = playing;
        }
        if (total > 0.0f) {
            seekerUpdating_ = true;
            pb_slider_->setValue((int)(cur / total * 1000.0f));
            seekerUpdating_ = false;
        }
        pb_timeLabel_->setText(fmtTime(cur) + " / " + fmtTime(total));
        if (model_ && pb_lapCombo_) {
            const LapBlock* currentLap = model_->data().lapAtTime(player_->currentTime());
            if (currentLap) {
                for (int i = 1; i < pb_lapCombo_->count(); ++i) {
                    if (pb_lapCombo_->itemData(i).toFloat() == currentLap->startSessionTime) {
                        if (pb_lapCombo_->currentIndex() != i) {
                            pb_lapCombo_->blockSignals(true);
                            pb_lapCombo_->setCurrentIndex(i);
                            pb_lapCombo_->blockSignals(false);
                        }
                        break;
                    }
                }
            } else {
                if (pb_lapCombo_->currentIndex() != 0) {
                    pb_lapCombo_->blockSignals(true);
                    pb_lapCombo_->setCurrentIndex(0);
                    pb_lapCombo_->blockSignals(false);
                }
            }
        }
    });

    connect(player_, &TnrdPlayer::finished, this, [this] {
        pb_playBtn_->setIcon(playPauseIcon(false, this, palette().color(QPalette::Text)));
        pbLastPlaying_ = false;
    });

    connect(pb_seekBackBtn_, &QPushButton::clicked, this, [this] {
        player_->seekToTime(player_->currentTime() - 5.0f);
    });

    connect(pb_playBtn_, &QPushButton::clicked, this, [this] {
        if (player_->isPlaying()) player_->pause();
        else player_->play();
    });

    connect(pb_seekFwdBtn_, &QPushButton::clicked, this, [this] {
        player_->seekToTime(player_->currentTime() + 5.0f);
    });

    connect(pb_slider_, &QSlider::valueChanged, this, [this](int val) {
        if (!seekerUpdating_)
            player_->seek(val / 1000.0f);
    });

    connect(pb_speedCombo_, &QComboBox::currentIndexChanged, this, [this](int idx) {
        player_->setSpeed(pb_speedCombo_->itemData(idx).toFloat());
    });

    connect(pb_lapCombo_, &QComboBox::currentIndexChanged, this, [this](int idx) {
        if (idx > 0) {
            float targetTime = pb_lapCombo_->itemData(idx).toFloat();
            if (targetTime >= 0) {
                player_->seekToTime(targetTime);
            }
        }
    });

    connect(closeRecBtn, &QPushButton::clicked, this, [this] {
        player_->close();
        inPlayback_ = false;
        // Drop the playback timer value; live packets (if any) repopulate it.
        resetSessionTimer();
        if (model_) model_->clear();
        if (chart) { chart->setPlaybackMode(false); chart->setMode(ChartMode::Default); }
        if (gearChart_) gearChart_->setPlaybackMode(false);
        if (inputsChart_) inputsChart_->setPlaybackMode(false);
        if (steeringChart_) steeringChart_->setPlaybackMode(false);
        if (gforceChart_) gforceChart_->setPlaybackMode(false);
        if (rideHeightChart_) rideHeightChart_->setPlaybackMode(false);
        if (ov_tyreCharts_) ov_tyreCharts_->setPlaybackMode(false);
        if (ov_compareBtn_) ov_compareBtn_->setEnabled(false);
        if (ov_defaultBtn_) ov_defaultBtn_->setChecked(true);
        if (ov_lapCombo_) ov_lapCombo_->setVisible(false);
        pb_sep_->hide();
        pb_bar_->hide();
        setWindowTitle("Track N Race Background Recorder");
    });

    udpSocket = new QUdpSocket(this);
    bool bound = udpSocket->bind(QHostAddress::AnyIPv4, 20777,
                                  QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint);
    if (!bound)
        QMessageBox::critical(this, "UDP Error",
            "Failed to bind to UDP port 20777.\n"
            "Is another telemetry tool or Track-N-Race already open?");

    connect(udpSocket, &QUdpSocket::readyRead, this, &MainWindow::onDatagramReady);

    connect(this, &MainWindow::telemetryUpdated,
            this, [this](float speed, int rpm, int gear,
                         float throttle, float brake, float steering, bool drs, int engineTemp) {
        cardSpeed->setText(QString::number((int)speed));
        cardSpeed->setStyleSheet("color: #37872D; font-weight: bold;");
        cardRpm->setText(QLocale().toString(rpm));
        cardRpm->setStyleSheet("color: #C4162A; font-weight: bold;");
        cardGear->setText(gear <= 0 ? (gear < 0 ? "R" : "N") : QString::number(gear));
        QString gearColor = gear <= 2 ? "#5794F2" : (gear <= 4 ? "#FADE2A" : (gear <= 6 ? "#c47d0e" : "#C4162A"));
        cardGear->setStyleSheet(QString("color: %1; font-weight: bold;").arg(gearColor));
        cardThrottle->setText(QString::number((int)(throttle * 100)));
        cardThrottle->setStyleSheet("color: #37872D; font-weight: bold;");
        cardBrake->setText(QString::number((int)(brake * 100)));
        cardBrake->setStyleSheet(brake > 0.05 ? "color: #C4162A; font-weight: bold;" : "font-weight: bold;");
        cardDrs->setText(drs ? "ON" : "OFF");
        cardDrs->setStyleSheet(drs ? "color: #37872D; font-weight: bold;"
                                   : "color: gray; font-weight: bold;");
        cardEngine->setText(QString::number(engineTemp));
        cardEngine->setStyleSheet(engineTemp > 112 ? "color: #C4162A; font-weight: bold;"
                                                   : "font-weight: bold;");
    });

    connect(this, &MainWindow::statusUpdated,
            this, [this](float ersPct, int ersMode, float fuelKg, float fuelLaps,
                         int tyreCompound, int tyreAgeLaps, int fuelMix, int visualCompound) {
        static const char* FUEL_MIX[]  = { "Lean", "Std", "Rich", "Max" };

        cardErs->setText(QString::number((int)ersPct));
        QString ersColor = ersMode == 3 ? "#C4162A" : (ersPct < 20 ? "#FADE2A" : "#5794F2");
        cardErs->setStyleSheet(QString("color: %1; font-weight: bold;").arg(ersColor));
        ovErsMode_ = ersMode;
        refreshErsSub();

        cardFuel->setText(QString::number(fuelKg, 'f', 1));
        QString fuelColor = fuelLaps > 1 ? "#37872D" : (fuelLaps >= 0 ? "#FADE2A" : "#C4162A");
        cardFuel->setStyleSheet(QString("color: %1; font-weight: bold;").arg(fuelColor));
        cardFuelSub->setText(QString("%1%2 vs fin")
            .arg(fuelLaps >= 0 ? "+" : "").arg(fuelLaps, 0, 'f', 1));

        cardTyre->setText(tyreLabel(tyreCompound));
        const QColor tc = tyreTextColor(visualCompound);
        cardTyre->setStyleSheet(tc.isValid()
            ? QString("color: %1; font-weight: bold;").arg(tc.name())
            : "font-weight: bold;");
        cardTyreSub->setText(QString("%1L · %2")
            .arg(tyreAgeLaps).arg(fuelMix >= 0 && fuelMix < 4 ? FUEL_MIX[fuelMix] : ""));
    });

    connect(this, &MainWindow::lapUpdated,
            this, [this](int pos, int lapNum) {
        cardPos->setText("P" + QString::number(pos));
        cardPosSub->setText("Lap " + QString::number(lapNum));
    });

    connect(this, &MainWindow::telemetryUpdated,
            chart, [](float, int, int, float, float, float, bool, int) {
        // chart is updated directly from processPacket
    });

    connect(this, &MainWindow::damageUpdated, this,
        [this](int tfl, int tfr, int trl, int trr,
               int bfl, int bfr, int brl, int brr,
               int wfl, int wfr, int wr,
               int fl, int sp, int diff, int gb, int eng,
               int drsFault, int ersFault) {
            setDmgValue(dmgTyreFl,   tfl); setDmgValue(dmgTyreFr,   tfr);
            setDmgValue(dmgTyreRl,   trl); setDmgValue(dmgTyreRr,   trr);
            setDmgValue(dmgBrakeFl,  bfl); setDmgValue(dmgBrakeFr,  bfr);
            setDmgValue(dmgBrakeRl,  brl); setDmgValue(dmgBrakeRr,  brr);
            setDmgValue(dmgWingFl,   wfl); setDmgValue(dmgWingFr,   wfr);
            setDmgValue(dmgWingRear,  wr); setDmgValue(dmgFloor,     fl);
            setDmgValue(dmgSidepod,   sp); setDmgValue(dmgDiffuser, diff);
            setDmgValue(dmgGearbox,   gb); setDmgValue(dmgEngine,   eng);

            cardDrsSub->setText(drsFault == 1 ? "FAULT" : "");
            cardDrsSub->setStyleSheet(drsFault == 1 ? "color: #C4162A;" : "");
            ovErsFault_ = (ersFault == 1);
            refreshErsSub();
        });
}

void MainWindow::refreshErsSub() {
    if (!cardErsSub) return;
    static const char* ERS_MODES[] = { "None", "Auto", "Hotlap", "Overtake" };
    if (ovErsFault_) {
        cardErsSub->setText("FAULT");
        cardErsSub->setStyleSheet("color: #C4162A;");
    } else {
        cardErsSub->setText(ovErsMode_ >= 0 && ovErsMode_ < 4 ? ERS_MODES[ovErsMode_] : "");
        cardErsSub->setStyleSheet("");
    }
}

MainWindow::~MainWindow() {
    closeActiveStream();
}

void MainWindow::resizeEvent(QResizeEvent* e) {
    QMainWindow::resizeEvent(e);
    scheduleNormalGeometryCapture();
    if (loadingOverlay_ && loadingOverlay_->isVisible() && container_)
        loadingOverlay_->setGeometry(container_->rect());
    // Keep any visible toasts pinned to the content area's top-right corner.
    Toast::updateAllPositions();
    relayoutToolbar();   // collapse/expand toolbar items for the new width
}

void MainWindow::moveEvent(QMoveEvent* e) {
    QMainWindow::moveEvent(e);
    scheduleNormalGeometryCapture();
}

// Cache the windowed geometry, but DEFERRED to the next event-loop turn. A resize
// from a maximize/minimize can arrive while isMaximized()/isMinimized() still read
// false (the state flag updates a beat later); reading the state synchronously here
// would record the maximized size as the windowed geometry — the root cause of the
// "maximized geometry gets stored" bug. By the time the deferred slot runs, the
// state is settled, so we only ever record a genuine windowed rect. Coalesced via
// captureScheduled_ so a burst of resize/move events triggers one capture.
void MainWindow::scheduleNormalGeometryCapture() {
    if (captureScheduled_) return;
    captureScheduled_ = true;
    QTimer::singleShot(0, this, [this] {
        captureScheduled_ = false;
        if (isVisible() && !isMaximized() && !isMinimized() && !isFullScreen()
            && !looksMaximized(geometry(), screen()))
            normalGeometry_ = geometry();
    });
}

bool MainWindow::eventFilter(QObject* obj, QEvent* e) {
    // Expose on our backing QWindow: the platform tells us when the window becomes
    // fully obscured (e.g. covered by another window) or visible again. On X11 and
    // Windows isExposed() flips with occlusion; on Wayland occlusion isn't reported
    // so this just no-ops there (minimize/hide still handled via changeEvent).
    if (obj == windowHandle() && e->type() == QEvent::Expose)
        updateRenderingState();
    // Qt's QToolBarLayout re-shows its extension button whenever it thinks items
    // overflow; hide it again every time so only our ⋯ overflow is ever seen.
    if (obj == tb_extButton_ &&
        (e->type() == QEvent::Show || e->type() == QEvent::ShowToParent))
        tb_extButton_->hide();
    return QMainWindow::eventFilter(obj, e);
}

void MainWindow::showEvent(QShowEvent* e) {
    QMainWindow::showEvent(e);
    // The backing QWindow only exists once the widget has been shown — install the
    // expose filter on first show so we can detect occlusion thereafter.
    if (!windowFilterHooked_) {
        if (QWindow* wh = windowHandle()) {
            wh->installEventFilter(this);
            windowFilterHooked_ = true;
        }
    }
    updateRenderingState();
}

void MainWindow::hideEvent(QHideEvent* e) {
    QMainWindow::hideEvent(e);
    updateRenderingState();
}

void MainWindow::closeEvent(QCloseEvent* e) {
    // normalGeometry_ always holds the windowed bounds (never the maximized rect),
    // so persist it as-is plus the maximized flag. Neither carries the minimized
    // state, so quitting minimized still reopens at the normal/maximized size.
    if (normalGeometry_.isValid())
        settings.setValue("window/normalGeometry", normalGeometry_);
    settings.setValue("window/maximized", isMaximized());
    QMainWindow::closeEvent(e);
}

// Computes whether the window is actually being displayed, and pauses/resumes the
// rendering subsystems accordingly. Recording (.tnrd writing) is unaffected.
void MainWindow::updateRenderingState() {
    const QWindow* wh = windowHandle();
    const bool active = isVisible()
                     && !(windowState() & Qt::WindowMinimized)
                     && (!wh || wh->isExposed());
    if (active != renderingActive_)
        setRenderingActive(active);
}

void MainWindow::setRenderingActive(bool on) {
    renderingActive_ = on;
    if (on) {
        // Resume: let the model flush the data it kept ingesting while paused (one
        // emit rebuilds the charts from full history), wake the track map, and
        // rebuild the visible page from the dirty flags accumulated while paused.
        if (model_)    model_->setLiveFlushActive(true);
        if (trackMap_) trackMap_->setRenderingActive(true);
        flushUiRefresh();
    } else {
        // Pause: stop every timer-driven repaint. Data ingest (recordRow,
        // ingestForModel) and the UDP path keep running so nothing is lost.
        if (uiRefreshTimer_) uiRefreshTimer_->stop();
        if (model_)    model_->setLiveFlushActive(false);
        if (trackMap_) trackMap_->setRenderingActive(false);
    }
}

void MainWindow::refreshThemedIcons() {
    // The toolbar theme icons are tinted to the palette foreground (on Windows),
    // so rebuild them whenever the palette changes to re-tint for the new mode.
    if (openAct_)        openAct_->setIcon(openRecordingIcon(this));
    if (editLayoutAct_)  editLayoutAct_->setIcon(editLayoutIcon(this));
    if (settingsAct_)    settingsAct_->setIcon(settingsIcon(this));
    if (tb_overflowBtn_) tb_overflowBtn_->setIcon(overflowIcon(this));
}

void MainWindow::applyChartWindow(int idx) {
    if (idx < 0 || idx >= kWindowOptionCount) return;
    tb_windowIdx_ = idx;
    const float secs = kWindowOptions[idx].secs;
    if (chart) chart->setWindowSeconds(secs);
    if (gearChart_) gearChart_->setWindowSeconds(secs);
    if (inputsChart_) inputsChart_->setWindowSeconds(secs);
    if (steeringChart_) steeringChart_->setWindowSeconds(secs);
    if (pp_splitChart) pp_splitChart->setWindowSeconds(secs);
    if (pp_harvestChart) pp_harvestChart->setWindowSeconds(secs);
    if (pp_storeChart) pp_storeChart->setWindowSeconds(secs);
    if (pp_fuelChart) pp_fuelChart->setWindowSeconds(secs);
    if (gforceChart_) gforceChart_->setWindowSeconds(secs);
    if (rideHeightChart_) rideHeightChart_->setWindowSeconds(secs);
    if (ov_tyreCharts_) ov_tyreCharts_->setWindowSeconds(secs);
    // Keep the inline segment in sync when the choice came from the overflow menu
    // (a programmatic setChecked emits idToggled, not idClicked, so no recursion).
    if (tb_windowGroup_)
        if (auto* b = tb_windowGroup_->button(idx))
            if (!b->isChecked()) b->setChecked(true);
}

// Collapse low-priority toolbar items into the "⋯" menu when the window is too
// narrow to fit everything, expanding them back as it widens. Order of collapse:
// icon actions → window-size segment → page tabs (right-to-left, active tab kept).
void MainWindow::relayoutToolbar() {
    if (!toolbar_ || !tb_overflowAct_ || tb_pageButtons_.empty()) return;
    const int avail = toolbar_->width();
    if (avail <= 0) return;
    const int spacing = toolbar_->layout() ? toolbar_->layout()->spacing() : 4;
    const int n = (int)tb_pageButtons_.size();
    // Deliberate slack so we always collapse a little EARLY rather than ever let the
    // toolbar genuinely overflow. A real overflow makes Qt's (suppressed) extension
    // button fight our event filter — show/hide/show — which is the flicker. Showing
    // the ⋯ a few px sooner is harmless; oscillation is not.
    constexpr int kSlack = 40;

    auto actW = [&](QAction* a) -> int {
        QWidget* w = a ? toolbar_->widgetForAction(a) : nullptr;
        return w ? w->sizeHint().width() : 0;
    };
    auto setIconsVisible = [&](bool v) {
        if (openAct_)        openAct_->setVisible(v);
        if (editLayoutAct_)  editLayoutAct_->setVisible(v);
        if (settingsAct_)    settingsAct_->setVisible(v);
    };

    // Pure arithmetic from stable sizeHints (tabs are fixed-width; the rest are
    // content-sized and don't depend on the live layout), so the decision is
    // deterministic — identical for a given width every call, no oscillation.
    std::vector<int> tabW(n);
    int sumTabs = 0;
    for (int i = 0; i < n; ++i) { tabW[i] = tb_pageButtons_[i]->sizeHint().width(); sumTabs += tabW[i]; }
    const int wSeg   = tb_windowSeg_->sizeHint().width();
    const int wIcons = actW(openAct_) + actW(editLayoutAct_) + actW(settingsAct_);
    const int wOver  = tb_overflowBtn_->sizeHint().width();
    // The session timer lives inside the (otherwise collapsible) spacer, so the
    // spacer can no longer shrink to 0 — it must always reserve the timer's width.
    // The timer is persistent: it never collapses into the overflow menu, so this
    // width stays in the inline budget throughout and is never subtracted off.
    const int wTimer = (tb_timerLabel_ && tb_timerLabel_->isVisible())
                           ? tb_timerLabel_->sizeHint().width() : 0;
    // Inter-item gaps: 6 toolbar items (tab strip, spacer, seg, 3 icons) → 5 gaps,
    // plus the tab strip's own gaps between its n buttons. The timer adds no gap of
    // its own (it rides inside the spacer).
    const int needAll = sumTabs + wSeg + wIcons + wTimer + spacing * 5 + spacing * (n - 1);

    if (avail >= needAll + kSlack) {              // comfortably fits — everything inline
        if (tb_windowAct_) tb_windowAct_->setVisible(true);
        setIconsVisible(true);
        for (auto* b : tb_pageButtons_) b->setVisible(true);
        tb_overflowAct_->setVisible(false);
        return;
    }

    // Need overflow. Collapse units (icons → window segment → tabs R→L, active tab
    // kept) until inline content + ⋯ leaves at least kSlack of headroom.
    tb_overflowAct_->setVisible(true);
    const int budget = avail - wOver - spacing - kSlack;
    int inlineW = needAll;
    bool segIn = true, iconsIn = true;
    std::vector<bool> tabIn(n, true);
    if (inlineW > budget && iconsIn) { iconsIn = false; inlineW -= wIcons + spacing; }
    if (inlineW > budget && segIn)   { segIn   = false; inlineW -= wSeg + spacing; }
    for (int i = n - 1; i >= 0 && inlineW > budget; --i) {
        if (i == currentPage_) continue;          // always keep the active tab inline
        tabIn[i] = false;
        inlineW -= tabW[i] + spacing;
    }

    if (tb_windowAct_) tb_windowAct_->setVisible(segIn);
    setIconsVisible(iconsIn);
    for (int i = 0; i < n; ++i) tb_pageButtons_[i]->setVisible(tabIn[i]);

    // Rebuild the overflow menu from whatever collapsed.
    tb_overflowMenu_->clear();
    for (int i = 0; i < n; ++i) {
        if (tabIn[i]) continue;
        // Tabs are navigation, not state — render as plain buttons (no checkbox).
        QAction* a = tb_overflowMenu_->addAction(tb_pageButtons_[i]->text());
        connect(a, &QAction::triggered, this, [this, i] {
            if (tb_pageGroup_) if (auto* b = tb_pageGroup_->button(i)) b->click();
        });
    }
    if (!segIn) {
        tb_overflowMenu_->addSection("Chart Window");
        for (int i = 0; i < kWindowOptionCount; ++i) {
            QAction* a = tb_overflowMenu_->addAction(kWindowOptions[i].label);
            a->setCheckable(true);
            a->setChecked(i == tb_windowIdx_);
            connect(a, &QAction::triggered, this, [this, i] { applyChartWindow(i); });
        }
    }
    if (!iconsIn) {
        tb_overflowMenu_->addSeparator();
        QAction* mo = tb_overflowMenu_->addAction(openRecordingIcon(this), "Open Recording");
        connect(mo, &QAction::triggered, openAct_, &QAction::trigger);
        QAction* me = tb_overflowMenu_->addAction(editLayoutIcon(this), "Edit Layout");
        me->setEnabled(editLayoutAct_ && editLayoutAct_->isEnabled());
        connect(me, &QAction::triggered, editLayoutAct_, &QAction::trigger);
        QAction* ms = tb_overflowMenu_->addAction(settingsIcon(this), "Settings");
        connect(ms, &QAction::triggered, settingsAct_, &QAction::trigger);
    }
}

void MainWindow::changeEvent(QEvent* e) {
    QMainWindow::changeEvent(e);
    if (e->type() == QEvent::ApplicationPaletteChange || e->type() == QEvent::PaletteChange)
        refreshThemedIcons();
    else if (e->type() == QEvent::WindowStateChange) {
        auto* se = static_cast<QWindowStateChangeEvent*>(e);
        const bool wasMaximized = se->oldState() & Qt::WindowMaximized;
        const bool nowMaximized = isMaximized();

        if (!wasMaximized && nowMaximized) {
            // Entering maximized: normalGeometry_ still holds the geometry from just
            // BEFORE this maximize (tracking never records a maximized-sized rect).
            // Persist it right here so that closing while maximized restores to this
            // windowed geometry — never the maximized one.
            if (normalGeometry_.isValid())
                settings.setValue("window/normalGeometry", normalGeometry_);
        } else if (wasMaximized && !nowMaximized && !isMinimized() && !isFullScreen()
                   && normalGeometry_.isValid()) {
            // Un-maximizing: force our remembered windowed geometry (the WM may
            // otherwise restore a wrong size). Capture first — a WM restore-resize
            // could overwrite the member before the singleShot runs.
            const QRect target = normalGeometry_;
            QTimer::singleShot(0, this, [this, target] { setGeometry(target); });
        }
        updateRenderingState();            // minimize/restore toggles rendering
    }
}

void MainWindow::updateToolbarColorScheme() {
    if (!toolbar_) return;
    // The dark-bar problem is specific to Breeze's "tools area", which colours the
    // toolbar from the *OS* scheme (same source as the titlebar). So only override
    // when (a) the user explicitly picked the Breeze style, AND (b) the app is
    // forced to the opposite mode from the OS — there the native colour clashes.
    // For System Default / any other style, or when the app matches the OS (incl.
    // "system" theme), leave the toolbar to the style/OS.
    const bool breezeStyle = currentStyleName().compare("breeze", Qt::CaseInsensitive) == 0;
    const QString theme = currentTheme();
    const auto os = Qt::ColorScheme(qApp->property("osColorScheme").toInt());
    const bool osDark = (os == Qt::ColorScheme::Dark);
    const bool mismatch = breezeStyle &&
        ((theme == "light" && osDark) || (theme == "dark" && !osDark));
    toolbar_->setStyleSheet(mismatch
        ? "QToolBar { border: none; margin: 0px; padding: 0px; background: palette(button); }"
        : "QToolBar { border: none; margin: 0px; padding: 0px; }");
}

// ── Slots ──────────────────────────────────────────────────────────────────

void MainWindow::setOutputDirectory(const QString& dir) {
    outputDirectory = dir;
    settings.setValue("outputDirectory", dir);
}

void MainWindow::setAutoRecord(bool checked) {
    wantRecord = checked;
    settings.setValue("autoRecord", checked);
    if (!checked) closeActiveStream();
}

void MainWindow::setTheme(const QString& theme) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    if (theme == "light")
        QApplication::styleHints()->setColorScheme(Qt::ColorScheme::Light);
    else if (theme == "dark")
        QApplication::styleHints()->setColorScheme(Qt::ColorScheme::Dark);
    else
        QApplication::styleHints()->setColorScheme(Qt::ColorScheme::Unknown);
#endif
    settings.setValue("theme", theme);
    // Only reload Breeze's light/dark palette if Breeze is the active style; other
    // styles follow the theme via setColorScheme above.
    if (currentStyleName().compare("breeze", Qt::CaseInsensitive) == 0)
        applyBreezePalette(theme);
    // Re-evaluate whether the toolbar needs the window-colour override for the new
    // mode (forced light/dark over an opposite OS scheme).
    updateToolbarColorScheme();
}

void MainWindow::setStyleName(const QString& name) {
    // Swap the active QStyle live — Qt repolishes every existing widget, so no
    // restart is needed. "system" restores the platform default captured at
    // startup (main.cpp), since by now it may already have been overridden.
    const bool breeze = name.compare("breeze", Qt::CaseInsensitive) == 0;
    // Leaving Breeze: give the palette back to the platform *before* the new style
    // polishes, so a style that sets its own palette (e.g. Kvantum) or relies on
    // the platform one (windows11) isn't left wearing Breeze's colours. Restore the
    // platform font and icon theme too, since Breeze swapped in Noto Sans + the
    // bundled Breeze icons.
    if (!breeze) {
        restoreDefaultPalette();
        restoreDefaultFont();
        restoreDefaultIconTheme();
    }

    if (name == "system") {
        const QString def = qApp->property("defaultStyleName").toString();
        if (!def.isEmpty())
            setApplicationStyle(QStyleFactory::create(def),
                                def.compare("breeze", Qt::CaseInsensitive) == 0);
    } else {
        setApplicationStyle(QStyleFactory::create(name), breeze);
    }
    settings.setValue("style", name);
    // Breeze's plugin draws shapes but never sets a palette — apply the KDE one
    // after the style is in place, along with Breeze's default font (Noto Sans)
    // and the bundled Breeze icon theme. (No-ops in non-bundled builds.)
    if (breeze) {
        applyBreezePalette(currentTheme());
        applyBreezeFont();
        applyBreezeIconTheme();
    }
    // The toolbar override only applies under Breeze, so re-evaluate on style change.
    updateToolbarColorScheme();
}

void MainWindow::setToolbarLabels(bool checked) {
    settings.setValue("ui/toolbarShowLabels", checked);
    if (toolbar_) toolbar_->setToolButtonStyle(checked ? Qt::ToolButtonTextBesideIcon : Qt::ToolButtonIconOnly);
    relayoutToolbar();   // text-beside-icon changes the icon-action widths
}

void MainWindow::setContrastThreshold(float val) {
    settings.setValue("ui/contrastThreshold", val);
    dirtyTiming_ = true;
    scheduleUiRefresh();
}

void MainWindow::setTrackMapLabelMode(int mode) {
    settings.setValue("ui/trackMapLabelMode", mode);
    if (trackMap_) {
        trackMap_->setLabelMode(static_cast<TrackMapWidget::LabelMode>(mode));
    }
}

void MainWindow::setTrackMapSectorColors(bool on) {
    settings.setValue("ui/trackMapSectorColors", on);
    if (trackMap_) trackMap_->setSectorColors(on);
}

void MainWindow::setTrackMapOpacity(int pct) {
    settings.setValue("ui/trackMapOpacity", pct);
    if (trackMap_) trackMap_->setMapOpacity(pct / 100.0);
}

void MainWindow::setTrackMapIdleTimeout(int secs) {
    settings.setValue("ui/trackMapIdleTimeout", secs);
    if (trackMap_) trackMap_->setIdleTimeout(secs);
}

void MainWindow::onDatagramReady() {
    if (inPlayback_) {
        while (udpSocket->hasPendingDatagrams())
            udpSocket->readDatagram(nullptr, 0);
        return;
    }
    while (udpSocket->hasPendingDatagrams()) {
        QByteArray dg;
        dg.resize((int)udpSocket->pendingDatagramSize());
        udpSocket->readDatagram(dg.data(), dg.size());
        processPacket(reinterpret_cast<const uint8_t*>(dg.constData()), dg.size());
    }
}

// ── Timestamps ─────────────────────────────────────────────────────────────

std::string MainWindow::getISOTimestamp() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto ms  = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    time_t t = system_clock::to_time_t(now);
    struct tm tmInfo {};
#ifdef _WIN32
    gmtime_s(&tmInfo, &t);
#else
    gmtime_r(&t, &tmInfo);
#endif
    char buf[64]; strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tmInfo);
    char result[80]; snprintf(result, sizeof(result), "%s.%03dZ", buf, (int)ms.count());
    return std::string(result);
}

std::string MainWindow::getFilenameTimestamp() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto ms  = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    time_t t = system_clock::to_time_t(now);
    struct tm tmInfo {};
#ifdef _WIN32
    gmtime_s(&tmInfo, &t);
#else
    gmtime_r(&t, &tmInfo);
#endif
    char buf[64]; strftime(buf, sizeof(buf), "%Y-%m-%dT%H-%M-%S", &tmInfo);
    char result[80]; snprintf(result, sizeof(result), "%s-%03dZ", buf, (int)ms.count());
    return std::string(result);
}

std::string MainWindow::sanitizeName(const std::string& name) {
    std::string r;
    for (unsigned char c : name) r += std::isalnum(c) ? (char)std::tolower(c) : '_';
    return r;
}

// ── gzopen abstraction ─────────────────────────────────────────────────────

gzFile MainWindow::gzOpenPath(const QString& path, const char* mode) {
#ifdef _WIN32
    return gzopen_w(path.toStdWString().c_str(), mode);
#else
    return gzopen(path.toUtf8().constData(), mode);
#endif
}

// ── Stream lifecycle ───────────────────────────────────────────────────────

void MainWindow::closeActiveStream() {
    if (activeGzip) {
        flushBufferToDisk(rollingBuffer);
        gzclose(activeGzip);
        activeGzip = nullptr;
    }
    rollingBuffer.clear();
    currentTrackId     = -1;
    currentSessionType = -1;
    activeGzipPath.clear();
    lastSessionTime = -1.0f;
    dedupeCache.clear();
    resetFastestLapState();
}

void MainWindow::startNewStream(int trackId, int sessionType, int format) {
    closeActiveStream();
    if (!wantRecord || outputDirectory.isEmpty()) return;

    std::string proto = (format == 2024) ? "f1_24" : "f1_25";

    auto itTrack = TRACK_NAMES.find(trackId);
    std::string tName = (itTrack != TRACK_NAMES.end())
        ? sanitizeName(itTrack->second) : "track_" + std::to_string(trackId);

    auto itSess = SESSION_NAMES.find(sessionType);
    std::string sName = (itSess != SESSION_NAMES.end())
        ? sanitizeName(itSess->second) : "session_" + std::to_string(sessionType);

    std::string filename = proto + "_" + std::to_string(trackId) + "_"
                         + tName + "_" + sName + "_" + getFilenameTimestamp() + ".tnrd";

    activeGzipPath = outputDirectory + "/" + QString::fromStdString(filename);
    activeGzip     = gzOpenPath(activeGzipPath, "wb");

    if (activeGzip) {
        nlohmann::json hdr;
        hdr["magic"]        = "TNRD_V1";
        hdr["protocol"]     = format;
        hdr["track_id"]     = trackId;
        hdr["track_name"]   = (itTrack != TRACK_NAMES.end()) ? itTrack->second : "Unknown";
        hdr["session_type"] = sessionType;
        hdr["session_name"] = (itSess != SESSION_NAMES.end()) ? itSess->second : "Unknown";
        hdr["start_time"]   = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::string hl = hdr.dump() + "\n";
        gzwrite(activeGzip, hl.c_str(), (unsigned int)hl.size());

        currentTrackId     = trackId;
        currentSessionType = sessionType;
        lastSessionTime    = -1.0f;
    }
}

// ── Buffer ─────────────────────────────────────────────────────────────────

void MainWindow::flushBufferToDisk(const std::vector<BufferEntry>& entries) {
    if (!activeGzip || entries.empty()) return;
    for (const auto& e : entries)
        gzwrite(activeGzip, e.line.c_str(), (unsigned int)e.line.size());
}

void MainWindow::flushOldBufferEntries() {
    if (lastSessionTime < 0.0f || rollingBuffer.empty()) return;
    float cutoff = lastSessionTime - BUFFER_WINDOW_S;
    size_t flush = 0;
    while (flush < rollingBuffer.size() && rollingBuffer[flush].sessionTime < cutoff)
        flush++;
    if (flush > 0) {
        flushBufferToDisk({rollingBuffer.begin(), rollingBuffer.begin() + (ptrdiff_t)flush});
        rollingBuffer.erase(rollingBuffer.begin(), rollingBuffer.begin() + (ptrdiff_t)flush);
    }
}

// ── Deduplication ──────────────────────────────────────────────────────────

bool MainWindow::isDuplicate(const std::string& type, const nlohmann::json& row) {
    if (!DEDUPE_TYPES.count(type)) return false;
    nlohmann::json clone = row;
    clone.erase("ts"); clone.erase("session_time");
    std::string hash = clone.dump();
    auto it = dedupeCache.find(type);
    if (it != dedupeCache.end() && it->second == hash) return true;
    dedupeCache[type] = hash;
    return false;
}

// ── Flashback / rewind ─────────────────────────────────────────────────────

void MainWindow::truncateTimeline(float newSessionTime) {
    float bufStart = rollingBuffer.empty()
        ? std::numeric_limits<float>::infinity() : rollingBuffer[0].sessionTime;

    if (newSessionTime >= bufStart) {
        rollingBuffer.erase(
            std::remove_if(rollingBuffer.begin(), rollingBuffer.end(),
                [newSessionTime](const BufferEntry& e) { return e.sessionTime > newSessionTime; }),
            rollingBuffer.end());
    } else {
        rollingBuffer.clear();
        if (!activeGzipPath.isEmpty() && activeGzip) {
            gzclose(activeGzip); activeGzip = nullptr;
            std::vector<std::string> kept;
            gzFile in = gzOpenPath(activeGzipPath, "rb");
            if (in) {
                char buf[16384];
                while (gzgets(in, buf, sizeof(buf)) != nullptr) {
                    std::string line(buf);
                    try {
                        nlohmann::json j = nlohmann::json::parse(line);
                        if (!j.contains("session_time") || j["session_time"].get<float>() <= newSessionTime)
                            kept.push_back(line);
                    } catch (...) {}
                }
                gzclose(in);
            }
            gzFile out = gzOpenPath(activeGzipPath, "wb");
            if (out) {
                for (const auto& l : kept) gzwrite(out, l.c_str(), (unsigned int)l.size());
                gzclose(out);
            }
            activeGzip = gzOpenPath(activeGzipPath, "ab");
        }
    }
    dedupeCache.clear();
    lastSessionTime = newSessionTime;
}

// ── Record a row to the rolling buffer ─────────────────────────────────────

void MainWindow::recordRow(const nlohmann::json& row, float sessionTime) {
    if (!activeGzip) return;
    std::string type = row["type"];
    if (isDuplicate(type, row)) return;
    std::string line = row.dump() + "\n";
    float entryTime  = (sessionTime >= 0.0f) ? sessionTime : lastSessionTime;
    rollingBuffer.push_back({line, entryTime});
    if (type == "race_event" && row["code"] == "SEND") { closeActiveStream(); return; }
    flushOldBufferEntries();
}

// ── Live data extraction → signals ─────────────────────────────────────────

void MainWindow::emitLiveData(const nlohmann::json& row) {
    const std::string type = row["type"].get<std::string>();
    // Every packet carries the header session_time; drive the toolbar timer from it.
    if (row.contains("session_time"))
        updateSessionTimer(row["session_time"].get<float>());
    if (type == "telemetry") {
        emit telemetryUpdated(
            row["speed_kph"].get<float>(),
            row["rpm"].get<int>(),
            row["gear"].get<int>(),
            row["throttle"].get<float>(),
            row["brake"].get<float>(),
            row.value("steering", 0.0f),
            row.value("drs", 0) != 0,
            row.value("engine_temp", 0)
        );
        lastPlayerTelemetryData = row;
        dirtyTyres_ = true; scheduleUiRefresh();
    } else if (type == "status") {
        emit statusUpdated(
            row["ers_pct"].get<float>(),
            row["ers_mode"].get<int>(),
            row["fuel_kg"].get<float>(),
            row["fuel_laps"].get<float>(),
            row["tyre_compound"].get<int>(),
            row["tyre_age_laps"].get<int>(),
            row.value("fuel_mix", 0),
            row.value("visual_compound", 0)
        );
        lastPlayerStatusData = row;
        dirtyRacePanel_ = true; dirtyPower_ = true; scheduleUiRefresh();
    } else if (type == "damage") {
        emit damageUpdated(
            row.value("tyre_dmg_fl",   0), row.value("tyre_dmg_fr",   0),
            row.value("tyre_dmg_rl",   0), row.value("tyre_dmg_rr",   0),
            row.value("brake_dmg_fl",  0), row.value("brake_dmg_fr",  0),
            row.value("brake_dmg_rl",  0), row.value("brake_dmg_rr",  0),
            row.value("wing_fl",           0), row.value("wing_fr",           0),
            row.value("wing_rear",         0), row.value("floor_damage",      0),
            row.value("sidepod_damage",    0), row.value("diffuser_damage",   0),
            row.value("gearbox_damage",    0), row.value("engine_damage",     0),
            row.value("drs_fault",         0), row.value("ers_fault",         0)
        );
        lastPlayerDamageData = row;
        dirtyTyres_ = true; scheduleUiRefresh();
    } else if (type == "tyre_sets") {
        lastTyreSetsData = row;
        dirtyTyreSets_ = true; scheduleUiRefresh();
    } else if (type == "lap") {
        emit lapUpdated(row["position"].get<int>(), row["lap_num"].get<int>());
        lastPlayerLapData = row;
        dirtyRacePanel_ = true; scheduleUiRefresh();
    } else if (type == "positions") {
        lastPositionsData = row;
        dirtyTrackMap_ = true; scheduleUiRefresh();
    } else if (type == "session") {
        lastSessionData = row;
        // Safety-car state changes update the persistent banner. SC/VSC/FL show a
        // persistent toast; returning to green (sc=0) silently dismisses it — no
        // "Track Clear" notification, matching the Electron app. Seeks are suppressed
        // via the one-shot flag set in seeked().
        const int sc = row.value("safety_car_status", 0);
        const bool suppress = scSuppressOnce_;
        scSuppressOnce_ = false;
        if (!suppress && sc != lastSafetyCarStatus_) {
            if (auto spec = safetyCarToast(lastSafetyCarStatus_, sc)) {
                showToast(*spec);
            } else if (sc == 0 && m_persistentToast_) {
                m_persistentToast_->hide();
                m_persistentToast_ = nullptr;
            }
        }
        lastSafetyCarStatus_ = sc;
        dirtySession_ = true; dirtyTrackMap_ = true; scheduleUiRefresh();
    } else if (type == "race_event") {
        if (row.value("code", "") == "SSTA") {
            sessionEventLog.clear();
            resetFastestLapState();
            lastSafetyCarStatus_ = 0;
        }
        sessionEventLog.push_back(row);
        // Transient notification for the event, both live and during playback.
        // race_events are never replayed on seek, so scrubbing won't re-fire them.
        if (auto spec = buildToast(row, lastParticipantsData)) showToast(*spec);
        dirtyEvents_ = true; scheduleUiRefresh();
    } else if (type == "timing") {
        lastTimingData = row;
        dirtyTiming_ = true; dirtyProximity_ = true; scheduleUiRefresh();
    } else if (type == "participants") {
        lastParticipantsData = row;
        dirtyTiming_ = true; dirtyTrackMap_ = true; scheduleUiRefresh();
    } else if (type == "all_status") {
        lastAllStatusData = row;
        dirtyTiming_ = true; scheduleUiRefresh();
    } else if (type == "fastest_lap") {
        fastestLapCarIdx_ = row.value("car_idx", -1);
        fastestLapSet_ = true;
        dirtyTiming_ = true; scheduleUiRefresh();
    } else if (type == "session_history_fastest") {
        if (!fastestLapSet_) {
            int carIdx = row.value("car_idx", -1);
            int ms = row.value("best_lap_time_ms", 0);
            sessionHistoryBest_[carIdx] = ms;
            int minMs = std::numeric_limits<int>::max();
            int minIdx = -1;
            for (const auto& kv : sessionHistoryBest_) {
                if (kv.second < minMs) { minMs = kv.second; minIdx = kv.first; }
            }
            if (fastestLapCarIdx_ != minIdx) {
                fastestLapCarIdx_ = minIdx;
                dirtyTiming_ = true; scheduleUiRefresh();
            }
        }
    }
}

void MainWindow::resetFastestLapState() {
    fastestLapCarIdx_ = -1;
    fastestLapSet_ = false;
    sessionHistoryBest_.clear();
}

void MainWindow::showToast(const ToastSpec& spec) {
    if (!settings.value("ui/toastsEnabled", true).toBool()) return;

    // Evict the current persistent toast (SC/VSC/FL) when the incoming event
    // takes over that slot — either by replacing it with a new persistent toast,
    // or by an ending event (Track Clear, Red Flag) that occupies then auto-dismisses.
    if ((spec.persistent || spec.dismissesPersistent) && m_persistentToast_) {
        m_persistentToast_->hide();
        m_persistentToast_ = nullptr;
    }

    // ToolTipBase reads as a raised card distinct from the page background; the
    // per-event accent drives the title, secondary text for the sub line.
    const QColor bg  = palette().color(QPalette::ToolTipBase);
    const QColor sub = palette().color(QPalette::PlaceholderText);

    // Parented to the central content widget so the toast renders inline (a child
    // overlay), not as its own window — required for correct positioning on Wayland.
    Toast* t = new Toast(container_);
    t->setShowIcon(false);
    t->setShowIconSeparator(false);
    t->setShowCloseButton(spec.persistent); // persistent toasts need manual dismissal
    t->setShowDurationBar(false);   // no countdown bar
    t->setFixedWidth(250);          // uniform width across all toasts (long text wraps)
    t->setBorderRadius(3);
    t->setDuration(spec.persistent ? 0 : settings.value("ui/bannerDuration", 3).toInt() * 1000);
    if (spec.persistent) {
        // The vendor's icon assets aren't bundled, so draw the × inline.
        // recolorImage() preserves alpha, so white-on-transparent gets tinted correctly.
        QPixmap closePix(10, 10);
        closePix.fill(Qt::transparent);
        QPainter painter(&closePix);
        painter.setPen(QPen(Qt::white, 1.5, Qt::SolidLine, Qt::RoundCap));
        painter.setRenderHint(QPainter::Antialiasing);
        painter.drawLine(2, 2, 8, 8);
        painter.drawLine(8, 2, 2, 8);
        painter.end();
        t->setCloseButtonIcon(closePix);
        t->setCloseButtonIconColor(sub);

        m_persistentToast_ = t;
        connect(t, &Toast::closed, this, [this, t]() {
            if (m_persistentToast_ == t) m_persistentToast_ = nullptr;
        });
    }
    t->setBackgroundColor(bg);
    t->setTitle(spec.label);
    t->setTitleColor(spec.color);
    if (!spec.sub.isEmpty()) {
        t->setText(spec.sub);
        t->setTextColor(sub);
    }
    t->show();
}

// Toolbar session timer. `sessionTime` is the header session_time carried on
// every packet (live UDP and playback): elapsed seconds since session start.
// Formatted M:SS to mirror the Electron titlebar timer. Guarded on the whole
// second so a burst of same-frame packets doesn't re-set the label needlessly.
void MainWindow::updateSessionTimer(float sessionTime) {
    if (!tb_timerLabel_ || sessionTime < 0.0f) return;
    const int total = (int)sessionTime;
    if (total == tb_timerSec_ && tb_timerLabel_->isVisible()) return;
    tb_timerSec_ = total;
    tb_timerLabel_->setText(QString("%1:%2")
                                .arg(total / 60)
                                .arg(total % 60, 2, 10, QLatin1Char('0')));
    if (!tb_timerLabel_->isVisible()) tb_timerLabel_->show();
    // The label's footprint changed if it just appeared or grew a digit
    // (e.g. 9:59 → 10:00). Re-run the responsive layout so the reserved timer
    // width stays correct and the toolbar never overflows.
    const int w = tb_timerLabel_->sizeHint().width();
    if (w != tb_timerW_) { tb_timerW_ = w; relayoutToolbar(); }
}

void MainWindow::resetSessionTimer() {
    tb_timerSec_ = -1;
    tb_timerW_   = 0;
    if (tb_timerLabel_) {
        tb_timerLabel_->clear();
        tb_timerLabel_->hide();
        relayoutToolbar();   // reclaim the freed width for the inline items
    }
}

void MainWindow::scheduleUiRefresh() {
    // While paused (window hidden/minimized/occluded) packets still set the dirty
    // flags, but we don't run the timer — the visible page is rebuilt once on
    // resume (see setRenderingActive).
    if (!renderingActive_) return;
    if (!uiRefreshTimer_->isActive()) uiRefreshTimer_->start();
}

void MainWindow::flushUiRefresh() {
    // Only rebuild the panels on the visible page; others stay marked dirty and
    // refresh when shown (see the pageCombo switch handler). This keeps hidden
    // pages (e.g. the 20-row standings table) from hitching the visible page's
    // animations on the shared UI thread. Index: 1=Standings 2=Session 3=Tyres.
    switch (currentPage_) {
        case 0:
            if (dirtyTyres_)     { updateTyresPage();        dirtyTyres_     = false; }
            break;
        case 1:
            if (dirtyTiming_)    { updateTimingTable();     dirtyTiming_    = false; }
            if (dirtyRacePanel_) { updateRacePanel();       dirtyRacePanel_ = false; }
            break;
        case 2:
            if (dirtyProximity_) { updateProximityWidget(); dirtyProximity_ = false; }
            if (dirtySession_)   { updateSessionPage();      dirtySession_   = false; }
            if (dirtyEvents_)    { updateSessionEvents();    dirtyEvents_    = false; }
            if (dirtyTrackMap_)  { updateTrackMapPage();     dirtyTrackMap_  = false; }
            break;
        case 3:
            if (dirtyTyres_)     { updateTyresPage();        dirtyTyres_     = false; }
            if (dirtyTyreSets_)  { updateTyreSetsTable();    dirtyTyreSets_  = false; }
            break;
        case 5:
            if (dirtyPower_)     { updatePowerPage();        dirtyPower_     = false; }
            break;
        default:
            break;
    }
}

// ── Central packet router ──────────────────────────────────────────────────

void MainWindow::processPacket(const uint8_t* data, int length) {
    if (length < HEADER_SIZE) return;

    uint16_t format = ReadUInt16(data, 0);
    if (format != 2024 && format != 2025) return;

    PacketHeader hdr;
    hdr.packetFormat   = format;
    hdr.packetId       = data[6];
    hdr.sessionTime    = ReadFloat(data, 15);
    hdr.overallFrameId = ReadUInt32(data, 23);
    hdr.playerCarIndex = data[27];

    uint64_t nowMs = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    if (FRAME_SAMPLED.count(hdr.packetId)) {
        auto it = lastFrameId.find(hdr.packetId);
        if (it != lastFrameId.end() && it->second == hdr.overallFrameId) return;
        lastFrameId[hdr.packetId] = hdr.overallFrameId;
    } else {
        int rateMs = 500;
        auto itL = SLOW_RATE_MS.find(hdr.packetId);
        if (itL != SLOW_RATE_MS.end()) rateMs = itL->second;
        if (rateMs > 0) {
            auto itT = lastSlowMs.find(hdr.packetId);
            if (itT != lastSlowMs.end() && (nowMs - itT->second) < (uint64_t)rateMs) return;
            lastSlowMs[hdr.packetId] = nowMs;
        }
    }

    if (activeGzip && lastSessionTime >= 0.0f && hdr.sessionTime < lastSessionTime - 0.2f)
        truncateTimeline(hdr.sessionTime);
    else if (hdr.sessionTime > lastSessionTime)
        lastSessionTime = hdr.sessionTime;

    if (hdr.packetId == PID_SESSION && length >= 708) {
        int8_t  trackId     = ReadInt8(data, 36);
        uint8_t sessionType = data[35];
        if (wantRecord && (trackId != currentTrackId || sessionType != currentSessionType || !activeGzip))
            startNewStream(trackId, sessionType, format);
    }

    std::vector<nlohmann::json> rows;
    std::string ts = getISOTimestamp();
    if (format == 2024) rows = F1_24::ParsePacket(data, length, hdr, ts);
    else                rows = F1_25::ParsePacket(data, length, hdr, ts);

    for (const auto& row : rows) {
        recordRow(row, hdr.sessionTime);
        emitLiveData(row);
        ingestForModel(row, hdr.sessionTime);
    }
}

// Routes a parsed row into the lap-aware SessionModel. Shared by the live UDP
// path and (for completeness) any streamed source. The model — not the chart —
// owns chart state; the chart re-queries the model on its change signals.
void MainWindow::ingestForModel(const nlohmann::json& row, float sessionTime) {
    if (!model_ || inPlayback_) return;   // playback feeds the model via the load scan
    const std::string rtype = row.value("type", std::string{});
    // A backward jump in session time is an in-game flashback/rewind: drop only the
    // samples newer than the rewind point and keep the rest (NOT a full reset — that
    // wiped all live data). A genuine restart rewinds to ~0, which truncates to empty
    // anyway. Same 0.2s guard as the recording-side truncate above.
    if (rtype == "telemetry" && sessionTime < model_->data().latestTime - 0.2f)
        model_->truncateAfter(sessionTime);
    if (rtype == "telemetry") {
        model_->onTelemetry(sessionTime,
                            row.value("speed_kph", 0.0f),
                            row.value("rpm", 0),
                            row.value("gear", 0),
                            row.value("throttle", 0.0f),
                            row.value("brake", 0.0f),
                            row.value("steering", 0.0f));
        // Combine live tyre temps with last-seen wear from the damage packet.
        model_->onTyre(sessionTime,
            row.value("tyre_temp_surface_fl", 0.0f), row.value("tyre_temp_surface_fr", 0.0f),
            row.value("tyre_temp_surface_rl", 0.0f), row.value("tyre_temp_surface_rr", 0.0f),
            row.value("tyre_temp_inner_fl",   0.0f), row.value("tyre_temp_inner_fr",   0.0f),
            row.value("tyre_temp_inner_rl",   0.0f), row.value("tyre_temp_inner_rr",   0.0f),
            row.value("brake_temp_fl",        0.0f), row.value("brake_temp_fr",        0.0f),
            row.value("brake_temp_rl",        0.0f), row.value("brake_temp_rr",        0.0f),
            lastPlayerDamageData.value("tyre_wear_fl",  0.0f), lastPlayerDamageData.value("tyre_wear_fr",  0.0f),
            lastPlayerDamageData.value("tyre_wear_rl",  0.0f), lastPlayerDamageData.value("tyre_wear_rr",  0.0f));
    }
    else if (rtype == "status")
        model_->onStatus(
            sessionTime,
            row.value("ers_pct", 0.0f),
            row.value("fuel_kg", 0.0f),
            row.value("engine_power_ice_kw", 0.0f),
            row.value("engine_power_mguk_kw", 0.0f),
            row.value("ers_harvested_mguk_j", 0.0f),
            row.value("ers_harvested_mguh_j", 0.0f)
        );
    else if (rtype == "lap")
        model_->onLap(row.value("lap_num", 0),
                     row.value("current_lap_ms", 0),
                     row.value("last_lap_ms", 0),
                     row.value("lap_invalid", false));
    else if (rtype == "motion")
        model_->onMotion(sessionTime,
                         row.value("g_lat", 0.0f),
                         row.value("g_long", 0.0f));
    else if (rtype == "motion_ex")
        model_->onMotionEx(sessionTime,
                           row.value("front_aero_height_mm", 0.0f),
                           row.value("rear_aero_height_mm", 0.0f));
}
