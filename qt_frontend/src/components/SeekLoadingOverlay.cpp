#include "SeekLoadingOverlay.h"

#include <QEvent>
#include <QFont>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QVariantAnimation>

namespace {
// Electron index.css: .seek-loading-overlay waits 300 ms, then fades in over
// 150 ms; .seek-loading-bar scans every 1.5 s. Loading scrims stay dark in
// every theme, so the colours are fixed rather than palette roles.
constexpr int kDelayMs = 300;
constexpr int kFadeMs = 150;
constexpr int kScanMs = 1500;
const QColor kScrim(0, 0, 0, 230);            // rgba(0, 0, 0, 0.9)
const QColor kLabel(0xee, 0xf0, 0xf6);
const QColor kTrack(255, 255, 255, 41);       // rgba(255, 255, 255, 0.16)
const QColor kFill(0x57, 0x94, 0xF2);
constexpr int kTrackW = 192;                  // w-48
constexpr int kTrackH = 6;                    // h-1.5
constexpr int kLabelGap = 12;                 // mb-3
}

SeekLoadingOverlay::SeekLoadingOverlay(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_NoSystemBackground);
    hide();

    delay_ = new QTimer(this);
    delay_->setSingleShot(true);
    delay_->setInterval(kDelayMs);
    connect(delay_, &QTimer::timeout, this, &SeekLoadingOverlay::reveal);

    fade_ = new QVariantAnimation(this);
    fade_->setDuration(kFadeMs);
    fade_->setStartValue(0.0);
    fade_->setEndValue(1.0);
    fade_->setEasingCurve(QEasingCurve::OutCubic);   // CSS ease-out
    connect(fade_, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
        opacity_ = v.toDouble();
        update();
    });

    scan_ = new QVariantAnimation(this);
    scan_->setDuration(kScanMs);
    scan_->setStartValue(-0.5);
    scan_->setEndValue(1.0);
    scan_->setLoopCount(-1);
    connect(scan_, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
        scanPos_ = v.toDouble();
        update();
    });

    parent->installEventFilter(this);
}

void SeekLoadingOverlay::arm() {
    delay_->start();
}

void SeekLoadingOverlay::disarm() {
    delay_->stop();
    fade_->stop();
    scan_->stop();
    hide();
}

void SeekLoadingOverlay::reveal() {
    setGeometry(parentWidget()->rect());
    opacity_ = 0;
    scanPos_ = -0.5;
    show();
    raise();
    fade_->start();
    scan_->start();
}

bool SeekLoadingOverlay::eventFilter(QObject* watched, QEvent* event) {
    if (watched == parentWidget() && event->type() == QEvent::Resize && isVisible())
        setGeometry(parentWidget()->rect());
    return QWidget::eventFilter(watched, event);
}

void SeekLoadingOverlay::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setOpacity(opacity_);
    p.fillRect(rect(), kScrim);

    QFont font = this->font();
    font.setPixelSize(14);                                   // text-sm
    font.setBold(true);
    font.setCapitalization(QFont::AllUppercase);
    font.setLetterSpacing(QFont::AbsoluteSpacing, 1.4);      // tracking-widest (0.1em)
    p.setFont(font);
    const int labelH = p.fontMetrics().height();

    const int blockH = labelH + kLabelGap + kTrackH;
    const int top = (height() - blockH) / 2;
    p.setPen(kLabel);
    p.drawText(QRect(0, top, width(), labelH), Qt::AlignCenter, QStringLiteral("Loading"));

    const QRectF track((width() - kTrackW) / 2.0, top + labelH + kLabelGap, kTrackW, kTrackH);
    QPainterPath clip;
    clip.addRoundedRect(track, kTrackH / 2.0, kTrackH / 2.0);
    p.setClipPath(clip);
    p.fillRect(track, kTrack);
    const QRectF bar(track.left() + scanPos_ * kTrackW, track.top(), kTrackW / 2.0, kTrackH);
    p.setPen(Qt::NoPen);
    p.setBrush(kFill);
    p.drawRoundedRect(bar, kTrackH / 2.0, kTrackH / 2.0);
}
