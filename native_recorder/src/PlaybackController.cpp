#include "PlaybackController.h"
#include "TnrdPlayer.h"
#include "SessionModel.h"
#include "IconUtils.h"

#include <QComboBox>
#include <QDateTime>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QSizePolicy>
#include <QSlider>
#include <QStyle>
#include <QWheelEvent>

namespace {

QIcon playPauseIcon(bool playing, QWidget* w, const QColor& tint) {
    return adaptThemeIcon(
        QIcon::fromTheme(playing ? QIcon::ThemeIcon::MediaPlaybackPause
                                 : QIcon::ThemeIcon::MediaPlaybackStart),
        tint,
        w->style()->standardIcon(playing ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay));
}

// No bundled SVG for these — prefer the OS/desktop theme icon (tinted to the
// foreground on Windows so the monochrome Breeze icons stay visible in dark
// mode), and fall back to Qt's own built-in standard-pixmap icon.
QIcon closeRecordingIcon(QWidget* w) {
    return adaptThemeIcon(
        QIcon::fromTheme("window-close", QIcon::fromTheme("process-stop")),
        w->palette().color(QPalette::WindowText),
        w->style()->standardIcon(QStyle::SP_DialogCloseButton));
}

QIcon seekBackwardIcon(QWidget* w, const QColor& tint) {
    return adaptThemeIcon(
        QIcon::fromTheme("media-seek-backward", QIcon::fromTheme("go-previous")),
        tint,
        w->style()->standardIcon(QStyle::SP_MediaSeekBackward));
}

QIcon seekForwardIcon(QWidget* w, const QColor& tint) {
    return adaptThemeIcon(
        QIcon::fromTheme("media-seek-forward", QIcon::fromTheme("go-next")),
        tint,
        w->style()->standardIcon(QStyle::SP_MediaSeekForward));
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
        // The drag's seeks are throttled, so the final position may fall inside a
        // throttle window; signal release so the handler can commit an
        // authoritative seek to the exact drop point.
        emit sliderReleased();
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

// Formats session time as M:SS.
QString fmtTime(float s) {
    int m   = (int)s / 60;
    int sec = (int)s % 60;
    return QString("%1:%2").arg(m).arg(sec, 2, 10, QChar('0'));
}

} // namespace

PlaybackController::PlaybackController(SessionModel* model, QWidget* barParent)
    : QObject(barParent), model_(model)
{
    player_ = new TnrdPlayer(this);

    bar_ = new QWidget(barParent);
    bar_->setAutoFillBackground(true);
    {
        QPalette pal = bar_->palette();
        pal.setColor(QPalette::Window, barParent->palette().color(QPalette::Window));
        bar_->setPalette(pal);
    }
    bar_->setFixedHeight(48);
    auto* pbLayout = new QHBoxLayout(bar_);
    pbLayout->setContentsMargins(12, 0, 12, 0);
    pbLayout->setSpacing(10);

    const QColor iconTint = barParent->palette().color(QPalette::Text);

    seekBackBtn_ = new QPushButton(bar_);
    seekBackBtn_->setIcon(seekBackwardIcon(bar_, iconTint));
    seekBackBtn_->setIconSize(QSize(20, 20));
    seekBackBtn_->setFixedSize(34, 34);
    seekBackBtn_->setFlat(true);
    seekBackBtn_->setToolTip("Skip Backward 5s");
    pbLayout->addWidget(seekBackBtn_);

    playBtn_ = new QPushButton(bar_);
    playBtn_->setIcon(playPauseIcon(false, bar_, iconTint));
    playBtn_->setIconSize(QSize(20, 20));
    playBtn_->setFixedSize(34, 34);
    playBtn_->setFlat(true);
    pbLayout->addWidget(playBtn_);

    seekFwdBtn_ = new QPushButton(bar_);
    seekFwdBtn_->setIcon(seekForwardIcon(bar_, iconTint));
    seekFwdBtn_->setIconSize(QSize(20, 20));
    seekFwdBtn_->setFixedSize(34, 34);
    seekFwdBtn_->setFlat(true);
    seekFwdBtn_->setToolTip("Skip Forward 5s");
    pbLayout->addWidget(seekFwdBtn_);

    slider_ = new ScrubSlider(Qt::Horizontal, bar_);
    slider_->setRange(0, 1000);
    slider_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    pbLayout->addWidget(slider_);

    timeLabel_ = new QLabel("0:00 / 0:00", bar_);
    pbLayout->addWidget(timeLabel_);

    lapCombo_ = new QComboBox(bar_);
    lapCombo_->addItem("Select Lap...", -1.0f);
    pbLayout->addWidget(lapCombo_);

    speedCombo_ = new QComboBox(bar_);
    speedCombo_->addItem("0.25×", 0.25f);
    speedCombo_->addItem("0.5×",  0.5f);
    speedCombo_->addItem("1×",    1.0f);
    speedCombo_->addItem("2×",    2.0f);
    speedCombo_->addItem("4×",    4.0f);
    speedCombo_->setCurrentIndex(2);
    pbLayout->addWidget(speedCombo_);

    // A normal (non-flat) button so it reads as a real "Close" action; icon-only by
    // default, gaining a "Close File" label when the toolbar's labels option is on
    // (see setShowLabels). setShowLabels(false) below sets the compact square size.
    closeRecBtn_ = new QPushButton(bar_);
    closeRecBtn_->setIcon(closeRecordingIcon(bar_));
    closeRecBtn_->setIconSize(QSize(20, 20));
    closeRecBtn_->setToolTip("Close File");
    pbLayout->addWidget(closeRecBtn_);
    setShowLabels(false);

    bar_->hide();

    sep_ = new QFrame(barParent);
    sep_->setFrameShape(QFrame::HLine);
    sep_->setFrameShadow(QFrame::Sunken);
    sep_->hide();

    connect(player_, &TnrdPlayer::loadingStarted, this, &PlaybackController::loadingStarted);
    connect(player_, &TnrdPlayer::loadFailed, this, &PlaybackController::loadFailed);

    connect(player_, &TnrdPlayer::loaded, this, [this](const tnrp::HeaderRow& hdr) {
        // Hand the chart the whole pre-scanned session; it now drives off currentTime.
        if (model_) model_->load(player_->takeScannedData());

        if (lapCombo_) {
            lapCombo_->blockSignals(true);
            lapCombo_->clear();
            lapCombo_->addItem("Select Lap...", -1.0f);
            if (model_) {
                for (const auto& lap : model_->data().laps) {
                    lapCombo_->addItem(QString("Lap %1").arg(lap.lapNum), lap.startSessionTime);
                }
            }
            lapCombo_->setCurrentIndex(0);
            lapCombo_->blockSignals(false);
        }
        sep_->show();
        bar_->show();
        playBtn_->setIcon(playPauseIcon(false, bar_, bar_->palette().color(QPalette::Text)));
        lastPlaying_ = false;
        slider_->setValue(0);
        speedCombo_->setCurrentIndex(2); // reset to 1×
        player_->setSpeed(1.0f);
        emit entered(hdr, player_->currentTime());
    });

    connect(player_, &TnrdPlayer::packetReady, this, &PlaybackController::rowReady);

    connect(player_, &TnrdPlayer::seeked, this, &PlaybackController::seeked);

    connect(player_, &TnrdPlayer::stateChanged, this,
            [this](bool playing, float cur, float total, float /*speed*/) {
        // `cur` is session-relative (for the slider); the model is keyed on absolute
        // session_time, so hand the pages the absolute playhead.
        emit timeChanged(player_->currentTime());
        if (playing != lastPlaying_) {
            playBtn_->setIcon(playPauseIcon(playing, bar_, bar_->palette().color(QPalette::Text)));
            lastPlaying_ = playing;
        }
        if (total > 0.0f) {
            seekerUpdating_ = true;
            slider_->setValue((int)(cur / total * 1000.0f));
            seekerUpdating_ = false;
        }
        timeLabel_->setText(fmtTime(cur) + " / " + fmtTime(total));
        if (model_ && lapCombo_) {
            const LapBlock* currentLap = model_->data().lapAtTime(player_->currentTime());
            if (currentLap) {
                for (int i = 1; i < lapCombo_->count(); ++i) {
                    if (lapCombo_->itemData(i).toFloat() == currentLap->startSessionTime) {
                        if (lapCombo_->currentIndex() != i) {
                            lapCombo_->blockSignals(true);
                            lapCombo_->setCurrentIndex(i);
                            lapCombo_->blockSignals(false);
                        }
                        break;
                    }
                }
            } else {
                if (lapCombo_->currentIndex() != 0) {
                    lapCombo_->blockSignals(true);
                    lapCombo_->setCurrentIndex(0);
                    lapCombo_->blockSignals(false);
                }
            }
        }
    });

    connect(player_, &TnrdPlayer::finished, this, [this] {
        playBtn_->setIcon(playPauseIcon(false, bar_, bar_->palette().color(QPalette::Text)));
        lastPlaying_ = false;
    });

    connect(seekBackBtn_, &QPushButton::clicked, this, [this] {
        player_->seekToTime(player_->currentTime() - 5.0f);
    });

    connect(playBtn_, &QPushButton::clicked, this, [this] {
        if (player_->isPlaying()) player_->pause();
        else player_->play();
    });

    connect(seekFwdBtn_, &QPushButton::clicked, this, [this] {
        player_->seekToTime(player_->currentTime() + 5.0f);
    });

    connect(slider_, &QSlider::valueChanged, this, [this](int val) {
        if (seekerUpdating_) return;
        // Leading-edge throttle: the handle already tracks the cursor via setValue
        // on every drag event, but the seek itself is heavy (per-row disk reads in
        // the reader snapshot + JSON parse of the big 20-car rows). Running it on
        // every mouse-move floods the UI thread and makes scrubbing lag, so cap it
        // to ~10 Hz. The exact drop point is committed on sliderReleased. Mirrors
        // the Electron scrub bar's 100 ms throttle + final seek on release.
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - lastSeekMs_ < 100) return;
        lastSeekMs_ = now;
        player_->seek(val / 1000.0f);
    });

    connect(slider_, &QSlider::sliderReleased, this, [this] {
        // Authoritative final seek: lands the playhead exactly where the drag ended,
        // even if the last move fell inside a throttle window.
        lastSeekMs_ = QDateTime::currentMSecsSinceEpoch();
        player_->seek(slider_->value() / 1000.0f);
    });

    connect(speedCombo_, &QComboBox::currentIndexChanged, this, [this](int idx) {
        player_->setSpeed(speedCombo_->itemData(idx).toFloat());
    });

    connect(lapCombo_, &QComboBox::currentIndexChanged, this, [this](int idx) {
        if (idx > 0) {
            float targetTime = lapCombo_->itemData(idx).toFloat();
            if (targetTime >= 0) {
                player_->seekToTime(targetTime);
            }
        }
    });

    connect(closeRecBtn_, &QPushButton::clicked, this, [this] {
        player_->close();
        if (model_) model_->clear();
        sep_->hide();
        bar_->hide();
        emit exited();
    });
}

void PlaybackController::setShowLabels(bool on) {
    if (!closeRecBtn_) return;
    closeRecBtn_->setText(on ? "Close File" : QString());
    if (on) {
        // Let the button size to icon + label.
        closeRecBtn_->setMinimumSize(0, 34);
        closeRecBtn_->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    } else {
        // Compact square, icon-only.
        closeRecBtn_->setFixedSize(34, 34);
    }
}

void PlaybackController::load(const QString& path) {
    player_->load(path);
}

float PlaybackController::currentTime() const {
    return player_->currentTime();
}
