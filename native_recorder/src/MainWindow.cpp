#include "MainWindow.h"
#include "TelemetryChart.h"
#include "components/GearChart.h"
#include "components/InputsChart.h"
#include "components/SteeringChart.h"
#include "TnrdPlayer.h"
#include "SessionModel.h"
#include "components/EditOverviewLayoutDialog.h"
#include "components/SettingsDialog.h"
#include "BreezePalette.h"

#include <QApplication>
#include <QToolBar>
#include <QStackedWidget>
#include <QComboBox>
#include <QLabel>
#include <QFrame>
#include <QWidget>
#include <QHBoxLayout>
#include <QSlider>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QFileDialog>
#include <QMessageBox>
#include <QSizePolicy>
#include <QStyleHints>
#include <QCoreApplication>
#include <QUdpSocket>
#include <QTimer>
#include <QSvgRenderer>
#include <QPainter>
#include <QImage>
#include <QPixmap>
#include <QIconEngine>
#include <QResizeEvent>
#include <QProgressBar>
#include <QStyle>
#include <QAction>
#include <QTabBar>
#include <QToolButton>
#include <QButtonGroup>
#include <QStyleFactory>
#include <QStyleOptionButton>
#include <QStylePainter>

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

// ── Damage value helper (used in constructor lambda) ───────────────────────

// Renders an SVG resource and tints it with the given colour.
static QIcon paletteIcon(const QString& resource, const QColor& tint) {
    QSvgRenderer renderer(resource);
    QImage img(24, 24, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    renderer.render(&p);
    p.setCompositionMode(QPainter::CompositionMode_SourceIn);
    p.fillRect(img.rect(), tint);
    p.end();
    return QIcon(QPixmap::fromImage(img));
}

#if defined(Q_OS_WIN)
// Recolours a source theme icon to a fixed tint while keeping it fully scalable.
// It renders the source at the exact size + device-pixel-ratio Qt requests (so it
// stays crisp on HiDPI / fractional display scaling) and tints via SourceIn —
// unlike baking a handful of fixed-size pixmaps, which Qt then scales to whatever
// size the toolbar actually wants, producing blur.
namespace {
class TintedIconEngine : public QIconEngine {
public:
    TintedIconEngine(QIcon src, QColor tint) : src_(std::move(src)), tint_(tint) {}
    QIconEngine* clone() const override { return new TintedIconEngine(src_, tint_); }

    QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State state,
                         qreal scale) override {
        QPixmap pm = src_.pixmap(size, scale, mode, state);  // crisp at device res
        if (pm.isNull()) return pm;
        QImage img = pm.toImage().convertToFormat(QImage::Format_ARGB32_Premultiplied);
        QPainter p(&img);
        p.setCompositionMode(QPainter::CompositionMode_SourceIn);
        p.fillRect(img.rect(), tint_);
        p.end();
        QPixmap out = QPixmap::fromImage(img);
        out.setDevicePixelRatio(scale);
        return out;
    }
    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override {
        return scaledPixmap(size, mode, state, 1.0);
    }
    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode,
               QIcon::State state) override {
        const qreal dpr = painter->device() ? painter->device()->devicePixelRatioF() : 1.0;
        painter->drawPixmap(rect, scaledPixmap(rect.size(), mode, state, dpr));
    }
private:
    QIcon  src_;
    QColor tint_;
};
} // namespace
#endif

// On Windows the bundled Breeze icons are monochrome and won't recolour for dark
// mode (that needs the KDE platform theme, which we don't ship), so they'd render
// black. Wrap the theme icon in an engine that tints it to `tint` while keeping
// it scalable. Everywhere else (KDE recolours them, or no Breeze bundled) the icon
// is returned untouched.
static QIcon adaptThemeIcon(const QIcon& themed, const QColor& tint, const QIcon& fallback) {
    if (themed.isNull()) return fallback;
#if defined(Q_OS_WIN)
    return QIcon(new TintedIconEngine(themed, tint));
#else
    Q_UNUSED(tint);
    return themed;
#endif
}

// Prefer the OS/desktop theme's play/pause icon (e.g. Breeze on KDE) so the
// button matches the rest of the system; our tinted SVG is the fallback where
// no theme icon is available (e.g. Windows, which has no icon theme concept).
static QIcon playPauseIcon(bool playing, const QColor& tint) {
    return adaptThemeIcon(
        QIcon::fromTheme(playing ? QIcon::ThemeIcon::MediaPlaybackPause
                                 : QIcon::ThemeIcon::MediaPlaybackStart),
        tint,
        paletteIcon(playing ? ":/pause.svg" : ":/play.svg", tint));
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

// ── Construction ───────────────────────────────────────────────────────────

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle("Track N Race Background Recorder");
    setMinimumSize(720, 520);
    resize(780, 580);

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
    toolbar->setStyleSheet("QToolBar { border: none; margin: 0px; padding: 0px; }");
    static constexpr int kToolbarHeight = 44;
    toolbar->setFixedHeight(kToolbarHeight);
    addToolBar(Qt::TopToolBarArea, toolbar);
    toolbar_ = toolbar;
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
    QWidget* pageTabsWidget = new QWidget;
    pageTabsWidget->setFixedHeight(kToolbarHeight);
    QHBoxLayout* pageTabsLay = new QHBoxLayout(pageTabsWidget);
    pageTabsLay->setContentsMargins(0, 0, 0, 0);
    pageTabsLay->setSpacing(4);
    QButtonGroup* pageGroup = new QButtonGroup(this);
    pageGroup->setExclusive(true);
    static const char* kPageNames[] = { "Overview", "Standings", "Session", "Tyres", "Input" };
    static constexpr int kPageCount = 5;
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
        pageGroup->addButton(b, i);
        pageTabsLay->addWidget(b);
    }
    static_cast<QToolButton*>(pageGroup->button(0))->setChecked(true);   // default: Overview
    toolbar->addWidget(pageTabsWidget);

    QWidget* spacer = new QWidget;
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar->addWidget(spacer);

    // Window-size segmented control: an exclusive row of checkable QToolButtons
    // drawn edge-to-edge, styled by the active QStyle (so it renders native to
    // whatever platform/theme is running, not a fixed look).
    QWidget* windowSeg = new QWidget;
    QHBoxLayout* segLay = new QHBoxLayout(windowSeg);
    segLay->setContentsMargins(0, 0, 0, 0);
    segLay->setSpacing(0);
    QButtonGroup* windowGroup = new QButtonGroup(this);
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
    toolbar->addWidget(windowSeg);
    connect(windowGroup, &QButtonGroup::idClicked, this, [this](int idx) {
        float secs = kWindowOptions[idx].secs;
        if (chart) chart->setWindowSeconds(secs);
        if (gearChart_) gearChart_->setWindowSeconds(secs);
        if (inputsChart_) inputsChart_->setWindowSeconds(secs);
        if (steeringChart_) steeringChart_->setWindowSeconds(secs);
    });

    toolbar->addSeparator();
    openAct_ = toolbar->addAction(openRecordingIcon(this), "Open Recording");
    editLayoutAct_ = toolbar->addAction(editLayoutIcon(this), "Edit Layout");
    connect(editLayoutAct_, &QAction::triggered, this, [this] {
        // Parented to `this` (not just passed the MainWindow* data pointer) so
        // the window manager ties it to the main window and keeps it above it.
        EditOverviewLayoutDialog* dlg = new EditOverviewLayoutDialog(this, this);
        connect(dlg, &QDialog::finished, dlg, &QObject::deleteLater);
        dlg->show();
    });
    settingsAct_ = toolbar->addAction(settingsIcon(this), "Settings");
    connect(settingsAct_, &QAction::triggered, this, [this] {
        SettingsDialog* dlg = new SettingsDialog(this, this);
        connect(dlg, &QDialog::finished, dlg, &QObject::deleteLater);
        dlg->show();
    });

    // The toolbar is built entirely from custom widgets (page tabs, window-size
    // segment) rather than QActions, which Qt's overflow extension button can't
    // reliably reparent into its popup menu — the button appears but its menu
    // silently fails to populate. Rather than fight that, just never let the
    // window get narrow enough to trigger it.
    setMinimumWidth(qMax(minimumWidth(), toolbar->sizeHint().width()));

    // Lap-aware session model — the chart's single source of truth, fed by both
    // live UDP and playback. Created before the Overview tab so the chart can bind it.
    model_ = new SessionModel(this);

    QStackedWidget* stack = new QStackedWidget(this);
    stack->addWidget(buildOverviewTab());   // index 0
    stack->addWidget(buildStandingsPage()); // index 1
    stack->addWidget(buildSessionPage());   // index 2
    stack->addWidget(buildTyresPage());     // index 3
    stack->addWidget(buildInputPage());     // index 4

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
    pb_playBtn_ = new QPushButton(pb_bar_);
    pb_playBtn_->setIcon(playPauseIcon(false, iconTint));
    pb_playBtn_->setIconSize(QSize(20, 20));
    pb_playBtn_->setFixedSize(34, 34);
    pb_playBtn_->setFlat(true);
    pbLayout->addWidget(pb_playBtn_);

    pb_slider_ = new ScrubSlider(Qt::Horizontal, pb_bar_);
    pb_slider_->setRange(0, 1000);
    pb_slider_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    pbLayout->addWidget(pb_slider_);

    pb_timeLabel_ = new QLabel("0:00 / 0:00", pb_bar_);
    pbLayout->addWidget(pb_timeLabel_);

    pb_speedCombo_ = new QComboBox(pb_bar_);
    pb_speedCombo_->addItem("0.25×", 0.25f);
    pb_speedCombo_->addItem("0.5×",  0.5f);
    pb_speedCombo_->addItem("1×",    1.0f);
    pb_speedCombo_->addItem("2×",    2.0f);
    pb_speedCombo_->addItem("4×",    4.0f);
    pb_speedCombo_->setCurrentIndex(2);
    pbLayout->addWidget(pb_speedCombo_);

    auto* closeRecBtn = new QPushButton("✕ Close", pb_bar_);
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
        flushUiRefresh();
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
        if (!path.isEmpty()) player_->load(path);
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
        // Hand the chart the whole pre-scanned session; it now drives off currentTime.
        if (model_) model_->load(player_->takeScannedData());
        if (chart) { chart->setPlaybackMode(true); chart->setCurrentTime(player_->currentTime()); }
        if (gearChart_) { gearChart_->setPlaybackMode(true); gearChart_->setCurrentTime(player_->currentTime()); }
        if (inputsChart_) { inputsChart_->setPlaybackMode(true); inputsChart_->setCurrentTime(player_->currentTime()); }
        if (steeringChart_) { steeringChart_->setPlaybackMode(true); steeringChart_->setCurrentTime(player_->currentTime()); }
        if (ov_compareBtn_) ov_compareBtn_->setEnabled(true);
        closeActiveStream();
        pb_sep_->show();
        pb_bar_->show();
        pb_playBtn_->setIcon(playPauseIcon(false, palette().color(QPalette::Text)));
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

    connect(player_, &TnrdPlayer::stateChanged, this,
            [this, fmtTime](bool playing, float cur, float total, float /*speed*/) {
        // `cur` is session-relative (for the slider); the model is keyed on absolute
        // session_time, so hand the chart the absolute playhead.
        if (chart) chart->setCurrentTime(player_->currentTime());
        if (gearChart_) gearChart_->setCurrentTime(player_->currentTime());
        if (inputsChart_) inputsChart_->setCurrentTime(player_->currentTime());
        if (steeringChart_) steeringChart_->setCurrentTime(player_->currentTime());
        if (playing != pbLastPlaying_) {
            pb_playBtn_->setIcon(playPauseIcon(playing, palette().color(QPalette::Text)));
            pbLastPlaying_ = playing;
        }
        if (total > 0.0f) {
            seekerUpdating_ = true;
            pb_slider_->setValue((int)(cur / total * 1000.0f));
            seekerUpdating_ = false;
        }
        pb_timeLabel_->setText(fmtTime(cur) + " / " + fmtTime(total));
    });

    connect(player_, &TnrdPlayer::finished, this, [this] {
        pb_playBtn_->setIcon(playPauseIcon(false, palette().color(QPalette::Text)));
        pbLastPlaying_ = false;
    });

    connect(pb_playBtn_, &QPushButton::clicked, this, [this] {
        if (player_->isPlaying()) player_->pause();
        else player_->play();
    });

    connect(pb_slider_, &QSlider::valueChanged, this, [this](int val) {
        if (!seekerUpdating_)
            player_->seek(val / 1000.0f);
    });

    connect(pb_speedCombo_, &QComboBox::currentIndexChanged, this, [this](int idx) {
        player_->setSpeed(pb_speedCombo_->itemData(idx).toFloat());
    });

    connect(closeRecBtn, &QPushButton::clicked, this, [this] {
        player_->close();
        inPlayback_ = false;
        if (model_) model_->clear();
        if (chart) { chart->setPlaybackMode(false); chart->setMode(ChartMode::Default); }
        if (gearChart_) gearChart_->setPlaybackMode(false);
        if (inputsChart_) inputsChart_->setPlaybackMode(false);
        if (steeringChart_) steeringChart_->setPlaybackMode(false);
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
                         float throttle, float brake, float steering, bool drs, int /*eng*/) {
        cardSpeed->setText(QString::number((int)speed));
        cardRpm->setText(QString::number(rpm / 1000.0, 'f', 1) + "k");
        cardGear->setText(gear <= 0 ? "N" : QString::number(gear));
        cardThrottle->setText(QString::number((int)(throttle * 100)));
        cardBrake->setText(QString::number((int)(brake * 100)));
        cardDrs->setText(drs ? "ON" : "OFF");
        cardDrs->setStyleSheet(drs ? "color: #37872D; font-weight: bold;"
                                   : "color: gray; font-weight: bold;");
    });

    connect(this, &MainWindow::statusUpdated,
            this, [this](float ersPct, int, float, float, int, int) {
        cardErs->setText(QString::number((int)ersPct));
    });

    connect(this, &MainWindow::lapUpdated,
            this, [this](int pos, int) {
        cardPos->setText("P" + QString::number(pos));
    });

    connect(this, &MainWindow::telemetryUpdated,
            chart, [](float, int, int, float, float, float, bool, int) {
        // chart is updated directly from processPacket
    });

    connect(this, &MainWindow::damageUpdated, this,
        [this](int tfl, int tfr, int trl, int trr,
               int bfl, int bfr, int brl, int brr,
               int wfl, int wfr, int wr,
               int fl, int sp, int diff, int gb, int eng) {
            setDmgValue(dmgTyreFl,   tfl); setDmgValue(dmgTyreFr,   tfr);
            setDmgValue(dmgTyreRl,   trl); setDmgValue(dmgTyreRr,   trr);
            setDmgValue(dmgBrakeFl,  bfl); setDmgValue(dmgBrakeFr,  bfr);
            setDmgValue(dmgBrakeRl,  brl); setDmgValue(dmgBrakeRr,  brr);
            setDmgValue(dmgWingFl,   wfl); setDmgValue(dmgWingFr,   wfr);
            setDmgValue(dmgWingRear,  wr); setDmgValue(dmgFloor,     fl);
            setDmgValue(dmgSidepod,   sp); setDmgValue(dmgDiffuser, diff);
            setDmgValue(dmgGearbox,   gb); setDmgValue(dmgEngine,   eng);
        });
}

MainWindow::~MainWindow() {
    closeActiveStream();
}

void MainWindow::resizeEvent(QResizeEvent* e) {
    QMainWindow::resizeEvent(e);
    if (loadingOverlay_ && loadingOverlay_->isVisible() && container_)
        loadingOverlay_->setGeometry(container_->rect());
}

void MainWindow::refreshThemedIcons() {
    // The toolbar theme icons are tinted to the palette foreground (on Windows),
    // so rebuild them whenever the palette changes to re-tint for the new mode.
    if (openAct_)       openAct_->setIcon(openRecordingIcon(this));
    if (editLayoutAct_) editLayoutAct_->setIcon(editLayoutIcon(this));
    if (settingsAct_)   settingsAct_->setIcon(settingsIcon(this));
}

void MainWindow::changeEvent(QEvent* e) {
    QMainWindow::changeEvent(e);
    if (e->type() == QEvent::ApplicationPaletteChange || e->type() == QEvent::PaletteChange)
        refreshThemedIcons();
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
}

void MainWindow::setStyleName(const QString& name) {
    // Swap the active QStyle live — Qt repolishes every existing widget, so no
    // restart is needed. "system" restores the platform default captured at
    // startup (main.cpp), since by now it may already have been overridden.
    const bool breeze = name.compare("breeze", Qt::CaseInsensitive) == 0;
    // Leaving Breeze: give the palette back to the platform *before* the new style
    // polishes, so a style that sets its own palette (e.g. Kvantum) or relies on
    // the platform one (windows11) isn't left wearing Breeze's colours.
    if (!breeze) restoreDefaultPalette();

    if (name == "system") {
        const QString def = qApp->property("defaultStyleName").toString();
        if (!def.isEmpty())
            if (QStyle* s = QStyleFactory::create(def)) QApplication::setStyle(s);
    } else if (QStyle* s = QStyleFactory::create(name)) {
        QApplication::setStyle(s);
    }
    settings.setValue("style", name);
    // Breeze's plugin draws shapes but never sets a palette — apply the KDE one
    // after the style is in place. (No-op in non-bundled builds.)
    if (breeze) applyBreezePalette(currentTheme());
}

void MainWindow::setToolbarLabels(bool checked) {
    settings.setValue("ui/toolbarShowLabels", checked);
    if (toolbar_) toolbar_->setToolButtonStyle(checked ? Qt::ToolButtonTextBesideIcon : Qt::ToolButtonIconOnly);
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
            row["tyre_age_laps"].get<int>()
        );
        lastPlayerStatusData = row;
        dirtyRacePanel_ = true; scheduleUiRefresh();
    } else if (type == "damage") {
        emit damageUpdated(
            row.value("tyre_dmg_fl",   0), row.value("tyre_dmg_fr",   0),
            row.value("tyre_dmg_rl",   0), row.value("tyre_dmg_rr",   0),
            row.value("brake_dmg_fl",  0), row.value("brake_dmg_fr",  0),
            row.value("brake_dmg_rl",  0), row.value("brake_dmg_rr",  0),
            row.value("wing_fl",           0), row.value("wing_fr",           0),
            row.value("wing_rear",         0), row.value("floor_damage",      0),
            row.value("sidepod_damage",    0), row.value("diffuser_damage",   0),
            row.value("gearbox_damage",    0), row.value("engine_damage",     0)
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
        dirtySession_ = true; dirtyTrackMap_ = true; scheduleUiRefresh();
    } else if (type == "race_event") {
        if (row.value("code", "") == "SSTA") sessionEventLog.clear();
        sessionEventLog.push_back(row);
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
    }
}

void MainWindow::scheduleUiRefresh() {
    if (!uiRefreshTimer_->isActive()) uiRefreshTimer_->start();
}

void MainWindow::flushUiRefresh() {
    // Only rebuild the panels on the visible page; others stay marked dirty and
    // refresh when shown (see the pageCombo switch handler). This keeps hidden
    // pages (e.g. the 20-row standings table) from hitching the visible page's
    // animations on the shared UI thread. Index: 1=Standings 2=Session 3=Tyres.
    switch (currentPage_) {
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
        default:
            break;  // Overview has no coalesced panels
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
    // A large backward jump in session time means a new session/restart.
    if (rtype == "telemetry" && sessionTime < model_->data().latestTime - 2.0f)
        model_->onSessionReset(sessionTime);
    if (rtype == "telemetry")
        model_->onTelemetry(sessionTime,
                            row.value("speed_kph", 0.0f),
                            row.value("rpm", 0),
                            row.value("gear", 0),
                            row.value("throttle", 0.0f),
                            row.value("brake", 0.0f),
                            row.value("steering", 0.0f));
    else if (rtype == "status")
        model_->onStatus(sessionTime, row.value("ers_pct", 0.0f));
    else if (rtype == "lap")
        model_->onLap(row.value("lap_num", 0),
                     row.value("current_lap_ms", 0),
                     row.value("last_lap_ms", 0),
                     row.value("lap_invalid", false));
}
