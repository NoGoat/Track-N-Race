#include "AnalysisMapView.h"
#include "AnalysisInputCard.h"

#include "../TrackMapWidget.h"
#include "../../IconUtils.h"

#include <QAction>
#include <QComboBox>
#include <QFontDatabase>
#include <QHideEvent>
#include <QLabel>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QStyle>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {

QIcon themed(const QWidget* widget, const char* name, QStyle::StandardPixmap fallback) {
    return adaptThemeIcon(QIcon::fromTheme(QString::fromLatin1(name)),
                          widget->palette().color(QPalette::WindowText),
                          widget->style()->standardIcon(fallback));
}

QString clockText(double seconds) {
    const int ms = qMax(0, qRound(seconds * 1000));
    return QStringLiteral("%1:%2.%3")
        .arg(ms / 60000)
        .arg((ms / 1000) % 60, 2, 10, QChar('0'))
        .arg((ms % 1000) / 100);
}

bool positionAt(const LapBlock& lap, double elapsed, double& x, double& z) {
    const auto& points = lap.positions;
    if (points.isEmpty()) return false;
    const double duration = qMax(0.0, double(lap.endSessionTime - lap.startSessionTime));
    const float target = static_cast<float>(lap.startSessionTime + qBound(0.0, elapsed, duration));
    const auto it = std::lower_bound(points.cbegin(), points.cend(), target,
                                     [](const LapPositionSample& p, float t) { return p.t < t; });
    if (it == points.cbegin()) { x = it->x; z = it->z; return true; }
    if (it == points.cend()) { x = points.last().x; z = points.last().z; return true; }
    const LapPositionSample& a = *(it - 1);
    const LapPositionSample& b = *it;
    const double span = b.t - a.t;
    const double ratio = span > 0 ? (target - a.t) / span : 0.0;
    x = a.x + (b.x - a.x) * ratio;
    z = a.z + (b.z - a.z) * ratio;
    return true;
}

} // namespace

AnalysisMapView::AnalysisMapView(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // The map fills its area; the Data Comparison card floats over it.
    mapArea_ = new QWidget(this);
    auto* body = new QVBoxLayout(mapArea_);
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);
    map_ = new TrackMapWidget(mapArea_);
    map_->setControlledMode(true);
    const QSettings settings(QStringLiteral("TrackNRace"), QStringLiteral("NativeRecorder"));
    map_->setSectorColors(settings.value("ui/trackMapSectorColors", false).toBool());
    map_->setMapOpacity(settings.value("ui/trackMapOpacity", 100).toInt() / 100.0);
    body->addWidget(map_, 1);
    inputs_ = new AnalysisInputCard(mapArea_);
    root->addWidget(mapArea_, 1);

    transport_ = new QToolBar(QStringLiteral("Lap Replay"), this);
    transport_->setIconSize(QSize(16, 16));
    transport_->setMovable(false);
    back_ = transport_->addAction(themed(this, "media-seek-backward", QStyle::SP_MediaSeekBackward),
                                  QStringLiteral("Back 5 Seconds"));
    play_ = transport_->addAction(themed(this, "media-playback-start", QStyle::SP_MediaPlay),
                                  QStringLiteral("Play"));
    play_->setShortcut(Qt::Key_Space);
    play_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    addAction(play_);   // Space works anywhere in the map view, not only on the toolbar
    forward_ = transport_->addAction(themed(this, "media-seek-forward", QStyle::SP_MediaSeekForward),
                                     QStringLiteral("Forward 5 Seconds"));
    slider_ = new QSlider(Qt::Horizontal, transport_);
    slider_->setRange(0, 1000);
    slider_->setAccessibleName(QStringLiteral("Replay position"));
    transport_->addWidget(slider_);
    time_ = new QLabel(transport_);
    time_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    time_->setContentsMargins(6, 0, 6, 0);
    transport_->addWidget(time_);
    speed_ = new QComboBox(transport_);
    speed_->setToolTip(QStringLiteral("Replay speed"));
    for (double rate : {0.25, 0.5, 1.0, 2.0, 4.0})
        speed_->addItem(QString::number(rate) + QStringLiteral("×"), rate);
    speed_->setCurrentIndex(2);
    transport_->addWidget(speed_);
    root->addWidget(transport_);
    transport_->hide();

    timer_ = new QTimer(this);
    timer_->setInterval(16);
    connect(timer_, &QTimer::timeout, this, [this] {
        if (playing_ && localTime() >= total_) {
            cursor_ = total_;
            setPlaying(false);
        }
        refresh();
        refreshTransport();
    });
    connect(play_, &QAction::triggered, this, [this] {
        if (!playing_) {
            cursor_ = localTime();
            if (cursor_ >= total_) cursor_ = 0;
        }
        setPlaying(!playing_);
    });
    connect(back_, &QAction::triggered, this, [this] { seek(localTime() - 5); });
    connect(forward_, &QAction::triggered, this, [this] { seek(localTime() + 5); });
    connect(slider_, &QSlider::valueChanged, this, [this](int value) {
        seek(total_ * value / 1000.0);
    });
    connect(speed_, &QComboBox::currentIndexChanged, this, [this] {
        cursor_ = localTime();
        rate_ = speed_->currentData().toDouble();
        clock_.restart();
    });
    refreshTransport();
}

void AnalysisMapView::setLaps(const LapBlock* current, const LapBlock* comparison, bool fixedMode,
                              int trackId, bool compatibleCircuit) {
    // Copy the laps only when they actually changed; the page re-applies state often.
    auto describe = [](const LapBlock* lap) {
        return lap ? QStringLiteral("%1/%2/%3/%4/%5")
                         .arg(quintptr(lap)).arg(lap->lapNum).arg(lap->positions.size())
                         .arg(lap->tel.size()).arg(lap->sts.size())
                   : QStringLiteral("-");
    };
    const QString signature = describe(current) + QLatin1Char('|') + describe(comparison) +
                              QLatin1Char('|') + QString::number(fixedMode) +
                              QLatin1Char('|') + QString::number(trackId);
    if (signature != signature_) {
        signature_ = signature;
        current_ = current ? *current : LapBlock{};
        comparison_ = comparison ? *comparison : LapBlock{};
        hasCurrent_ = current;
        hasComparison_ = comparison;
        total_ = qMax(hasCurrent_ ? double(current_.endSessionTime - current_.startSessionTime) : 0.0,
                      hasComparison_ ? double(comparison_.endSessionTime - comparison_.startSessionTime)
                                     : 0.0);
        cursor_ = 0;
        focused_ = false;
        setPlaying(false);
    }
    fixed_ = fixedMode;
    transport_->setVisible(fixed_);
    map_->setTrack(compatibleCircuit ? trackId : -1);
    refresh();
    refreshTransport();
}

void AnalysisMapView::setColors(const QColor& current, const QColor& comparison) {
    currentColor_ = current;
    comparisonColor_ = comparison;
    inputs_->setIdentity(currentLabel_, currentColor_, comparisonLabel_, comparisonColor_);
    refresh();
}

void AnalysisMapView::setLabels(const QString& current, const QString& comparison) {
    currentLabel_ = current;
    comparisonLabel_ = comparison;
    inputs_->setIdentity(currentLabel_, currentColor_, comparisonLabel_, comparisonColor_);
    refresh();
}

void AnalysisMapView::setMapAppearance(bool sectorColors, int opacityPercent) {
    map_->setSectorColors(sectorColors);
    map_->setMapOpacity(qBound(0, opacityPercent, 100) / 100.0);
}

void AnalysisMapView::setAeroMode(bool slm) {
    map_->setAeroMode(slm);
}

void AnalysisMapView::setCurrentTime(float sessionTime) {
    sessionTime_ = sessionTime;
    if (!fixed_ && !focused_) refresh();
}

void AnalysisMapView::focusElapsed(double seconds) {
    focused_ = true;
    cursor_ = qBound(0.0, seconds, total_);
    setPlaying(false);
    refresh();
    refreshTransport();
}

void AnalysisMapView::setReadoutVisible(bool visible) {
    inputs_->setVisible(visible);
    refresh();
}

void AnalysisMapView::hideEvent(QHideEvent* event) {
    setPlaying(false);
    QWidget::hideEvent(event);
}

double AnalysisMapView::localTime() const {
    return playing_ ? qMin(total_, cursor_ + clock_.elapsed() / 1000.0 * rate_) : cursor_;
}

double AnalysisMapView::cursorElapsed() const {
    if (fixed_ || focused_) return localTime();
    return hasCurrent_ ? sessionTime_ - current_.startSessionTime : 0.0;
}

void AnalysisMapView::seek(double seconds) {
    cursor_ = qBound(0.0, seconds, total_);
    clock_.restart();
    refresh();
    refreshTransport();
}

void AnalysisMapView::setPlaying(bool playing) {
    playing_ = playing && total_ > 0;
    clock_.restart();
    if (playing_) timer_->start();
    else timer_->stop();
    play_->setText(playing_ ? QStringLiteral("Pause") : QStringLiteral("Play"));
    play_->setIcon(playing_ ? themed(this, "media-playback-pause", QStyle::SP_MediaPause)
                            : themed(this, "media-playback-start", QStyle::SP_MediaPlay));
    refreshTransport();
}

void AnalysisMapView::refresh() {
    const double elapsed = cursorElapsed();
    emit cursorElapsedChanged(elapsed);

    QVector<TrackMapWidget::Marker> markers;
    double x = 0;
    double z = 0;
    if (hasCurrent_ && positionAt(current_, elapsed, x, z))
        markers.push_back({x, z, currentLabel_, currentColor_});
    if (hasComparison_ && positionAt(comparison_, elapsed, x, z))
        markers.push_back({x, z, comparisonLabel_, comparisonColor_});
    map_->setControlledMarkers(markers);

    if (!inputs_->isHidden())
        inputs_->setReadings(hasCurrent_ ? &current_ : nullptr,
                              hasComparison_ ? &comparison_ : nullptr, elapsed);
}

void AnalysisMapView::refreshTransport() {
    const double now = localTime();
    {
        QSignalBlocker guard(slider_);
        slider_->setValue(total_ > 0 ? qRound(now / total_ * 1000) : 0);
    }
    time_->setText(clockText(now) + QStringLiteral(" / ") + clockText(total_));
    const bool ready = total_ > 0;
    for (QAction* action : {back_, play_, forward_}) action->setEnabled(ready);
    slider_->setEnabled(ready);
}
