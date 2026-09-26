#include "AnalyzeInputComparison.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QSettings>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {
constexpr int kInset = 8;

QString readingText(double value, bool speed) {
    if (!std::isfinite(value)) return QString::fromUtf8("\u2014");
    if (speed) return QString::number(qRound(value));
    const int gear = qRound(value);
    return gear < 0 ? QStringLiteral("R") : gear == 0 ? QStringLiteral("N")
                                             : QString::number(gear);
}

QLabel* valueLabel(QWidget* parent) {
    auto* label = new QLabel(QString::fromUtf8("\u2014"), parent);
    label->setAlignment(Qt::AlignCenter);
    label->setMinimumWidth(42);
    return label;
}
}

class AnalyzeInputComparison::DualBar : public QWidget {
public:
    explicit DualBar(bool signedValue, QWidget* parent = nullptr)
        : QWidget(parent), signed_(signedValue) {
        setFixedHeight(24);
        setMinimumWidth(132);
    }

    void setReadings(double current, double comparison, const QColor& currentColor,
                     const QColor& comparisonColor, const QString& currentLabel,
                     const QString& comparisonLabel, const QString& fieldLabel) {
        values_[0] = current;
        values_[1] = comparison;
        colors_[0] = currentColor;
        colors_[1] = comparisonColor;
        const auto text = [this](double value) {
            if (!std::isfinite(value)) return QStringLiteral("No data");
            if (signed_) {
                const QString direction = value < 0 ? QStringLiteral("L ")
                    : value > 0 ? QStringLiteral("R ") : QString();
                return direction + QString::number(qRound(std::abs(value) * 100.0)) + "%";
            }
            return QString::number(qRound(value * 100.0)) + "%";
        };
        setToolTip(QStringLiteral("%1: %2\n%3: %4")
                       .arg(currentLabel, text(current), comparisonLabel, text(comparison)));
        setAccessibleName(QStringLiteral("%1. %2: %3. %4: %5")
                              .arg(fieldLabel, currentLabel, text(current),
                                   comparisonLabel, text(comparison)));
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, false);
        const QColor background = palette().color(QPalette::AlternateBase);
        const QColor border = palette().color(QPalette::Mid);
        const QColor text = palette().color(QPalette::Text);
        for (int index = 0; index < 2; ++index) {
            const QRect lane(0, index * 12, width(), 10);
            painter.fillRect(lane, background);
            painter.setPen(border);
            painter.drawRect(lane.adjusted(0, 0, -1, -1));
            const double raw = values_[index];
            if (!std::isfinite(raw)) {
                painter.setPen(text);
                painter.drawText(lane, Qt::AlignCenter, QString::fromUtf8("\u2014"));
                continue;
            }
            const double value = qBound(signed_ ? -1.0 : 0.0, raw, 1.0);
            QRectF fill;
            if (signed_) {
                const double center = lane.width() / 2.0;
                const double span = std::abs(value) * center;
                fill = QRectF(value < 0 ? center - span : center, lane.y(), span,
                              lane.height());
                painter.setPen(border);
                painter.drawLine(qRound(center), lane.top() + 1, qRound(center),
                                 lane.bottom() - 1);
            } else {
                fill = QRectF(lane.x(), lane.y(), value * lane.width(), lane.height());
            }
            painter.fillRect(fill, colors_[index]);
        }
    }

private:
    bool signed_ = false;
    double values_[2]{qQNaN(), qQNaN()};
    QColor colors_[2];
};

AnalyzeInputComparison::AnalyzeInputComparison(QWidget* mapArea)
    : QFrame(mapArea), mapArea_(mapArea) {
    setObjectName(QStringLiteral("analyzeInputComparison"));
    setFrameShape(QFrame::StyledPanel);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setStyleSheet(QStringLiteral(
        "#analyzeInputComparison{background:palette(window);border:1px solid palette(mid);"
        "border-radius:4px;}"));
    setAccessibleName(QStringLiteral("Data Comparison"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(7, 5, 7, 7);
    root->setSpacing(5);
    root->setSizeConstraint(QLayout::SetFixedSize);
    auto* header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(2);
    dragHandle_ = new QToolButton(this);
    dragHandle_->setText(QString::fromUtf8("\u22ee\u22ee  Data Comparison"));
    dragHandle_->setAutoRaise(true);
    dragHandle_->setCursor(Qt::OpenHandCursor);
    dragHandle_->setFocusPolicy(Qt::StrongFocus);
    dragHandle_->setToolTip(
        QStringLiteral("Drag to move. Use arrow keys; hold Shift for fine adjustments."));
    dragHandle_->setAccessibleName(QStringLiteral(
        "Move comparison. Drag or use arrow keys; hold Shift for fine adjustments."));
    collapse_ = new QToolButton(this);
    collapse_->setAutoRaise(true);
    collapse_->setFixedSize(22, 22);
    header->addWidget(dragHandle_, 1);
    header->addWidget(collapse_);
    root->addLayout(header);

    body_ = new QWidget(this);
    auto* bodyLayout = new QVBoxLayout(body_);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(4);
    auto* legend = new QHBoxLayout;
    legend->setContentsMargins(58, 0, 0, 0);
    legend->setSpacing(4);
    currentLegend_ = new QLabel(body_);
    comparisonLegend_ = new QLabel(body_);
    for (QLabel* label : {currentLegend_, comparisonLegend_}) {
        label->setAlignment(Qt::AlignCenter);
        label->setMinimumWidth(64);
        legend->addWidget(label, 1);
    }
    bodyLayout->addLayout(legend);

    auto addBar = [this, bodyLayout](const QString& label, bool signedValue) {
        auto* row = new QHBoxLayout;
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(5);
        auto* title = new QLabel(label, body_);
        title->setFixedWidth(53);
        auto* bar = new DualBar(signedValue, body_);
        row->addWidget(title);
        row->addWidget(bar, 1);
        bodyLayout->addLayout(row);
        return bar;
    };
    steering_ = addBar(QStringLiteral("Steering"), true);
    brake_ = addBar(QStringLiteral("Brake"), false);
    throttle_ = addBar(QStringLiteral("Throttle"), false);
    ers_ = addBar(QStringLiteral("ERS"), false);

    auto addValues = [this, bodyLayout](const QString& title, QLabel*& current,
                                        QLabel*& comparison, const QString& unit) {
        auto* row = new QHBoxLayout;
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(4);
        auto* label = new QLabel(title, body_);
        label->setFixedWidth(53);
        current = valueLabel(body_);
        comparison = valueLabel(body_);
        row->addWidget(label);
        row->addWidget(current, 1);
        row->addWidget(comparison, 1);
        if (!unit.isEmpty()) {
            auto* suffix = new QLabel(unit, body_);
            suffix->setStyleSheet(QStringLiteral("font-size:9px;color:palette(mid);"));
            row->addWidget(suffix);
        }
        bodyLayout->addLayout(row);
    };
    addValues(QStringLiteral("Speed"), currentSpeed_, comparisonSpeed_,
              QStringLiteral("km/h"));
    addValues(QStringLiteral("Gear"), currentGear_, comparisonGear_, {});
    root->addWidget(body_);

    QSettings settings(QStringLiteral("TrackNRace"), QStringLiteral("NativeRecorder"));
    normalizedX_ = qBound(0.0, settings.value("analyze/inputComparisonX", 0.0).toDouble(), 1.0);
    normalizedY_ = qBound(0.0, settings.value("analyze/inputComparisonY", 0.0).toDouble(), 1.0);
    dragHandle_->installEventFilter(this);
    mapArea_->installEventFilter(this);
    connect(collapse_, &QToolButton::clicked, this,
            [this] { setCollapsed(!collapsed_); });
    setLabels(currentLabel_, comparisonLabel_);
    setColors(currentColor_, comparisonColor_);
    setCollapsed(false);
    raise();
}

void AnalyzeInputComparison::setLaps(const LapBlock* current,
                                     const LapBlock* comparison) {
    current_ = current;
    comparison_ = comparison;
    lastCurrentSample_ = nullptr;
    lastComparisonSample_ = nullptr;
    lastCurrentStatus_ = nullptr;
    lastComparisonStatus_ = nullptr;
    refreshReadings(true);
}

void AnalyzeInputComparison::setColors(const QColor& current,
                                       const QColor& comparison) {
    currentColor_ = current;
    comparisonColor_ = comparison;
    currentLegend_->setStyleSheet(QStringLiteral("font-weight:600;color:%1;")
                                      .arg(currentColor_.name()));
    comparisonLegend_->setStyleSheet(QStringLiteral("font-weight:600;color:%1;")
                                         .arg(comparisonColor_.name()));
    currentSpeed_->setStyleSheet(QStringLiteral("font-weight:600;color:%1;")
                                     .arg(currentColor_.name()));
    comparisonSpeed_->setStyleSheet(QStringLiteral("font-weight:600;color:%1;")
                                        .arg(comparisonColor_.name()));
    currentGear_->setStyleSheet(QStringLiteral("font-weight:600;color:%1;")
                                    .arg(currentColor_.name()));
    comparisonGear_->setStyleSheet(QStringLiteral("font-weight:600;color:%1;")
                                       .arg(comparisonColor_.name()));
    refreshReadings(true);
}

void AnalyzeInputComparison::setLabels(const QString& current,
                                       const QString& comparison) {
    currentLabel_ = current.isEmpty() ? QStringLiteral("Current lap") : current;
    comparisonLabel_ = comparison.isEmpty() ? QStringLiteral("Comparison lap") : comparison;
    currentLegend_->setText(currentLabel_);
    currentLegend_->setToolTip(currentLabel_);
    comparisonLegend_->setText(comparisonLabel_);
    comparisonLegend_->setToolTip(comparisonLabel_);
    refreshReadings(true);
}

void AnalyzeInputComparison::setElapsed(double seconds) {
    elapsed_ = seconds;
    if (!collapsed_) refreshReadings();
}

const TelSample* AnalyzeInputComparison::telemetryAt(const LapBlock* lap,
                                                      double elapsed) {
    if (!lap || lap->tel.isEmpty()) return nullptr;
    const double duration = qMax(0.0, double(lap->endSessionTime - lap->startSessionTime));
    const float target = static_cast<float>(lap->startSessionTime +
                                            qBound(0.0, elapsed, duration));
    auto it = std::upper_bound(lap->tel.cbegin(), lap->tel.cend(), target,
                               [](float time, const TelSample& sample) {
                                   return time < sample.t;
                               });
    if (it == lap->tel.cbegin()) return &lap->tel.first();
    return &*(it - 1);
}

const StsSample* AnalyzeInputComparison::statusAt(const LapBlock* lap,
                                                  double elapsed) {
    if (!lap || lap->sts.isEmpty()) return nullptr;
    const double duration = qMax(0.0, double(lap->endSessionTime - lap->startSessionTime));
    const float target = static_cast<float>(lap->startSessionTime +
                                            qBound(0.0, elapsed, duration));
    auto it = std::upper_bound(lap->sts.cbegin(), lap->sts.cend(), target,
                               [](float time, const StsSample& sample) {
                                   return time < sample.t;
                               });
    if (it == lap->sts.cbegin()) return &lap->sts.first();
    return &*(it - 1);
}

void AnalyzeInputComparison::refreshReadings(bool force) {
    const TelSample* current = telemetryAt(current_, elapsed_);
    const TelSample* comparison = telemetryAt(comparison_, elapsed_);
    const StsSample* currentStatus = statusAt(current_, elapsed_);
    const StsSample* comparisonStatus = statusAt(comparison_, elapsed_);
    if (!force && current == lastCurrentSample_ && comparison == lastComparisonSample_ &&
        currentStatus == lastCurrentStatus_ && comparisonStatus == lastComparisonStatus_)
        return;
    lastCurrentSample_ = current;
    lastComparisonSample_ = comparison;
    lastCurrentStatus_ = currentStatus;
    lastComparisonStatus_ = comparisonStatus;
    auto value = [](const TelSample* sample, float TelSample::*field) {
        if (!sample) return qQNaN();
        const double result = sample->*field;
        return std::isfinite(result) ? result : qQNaN();
    };
    const double currentSteering = value(current, &TelSample::steering);
    const double comparisonSteering = value(comparison, &TelSample::steering);
    const double currentBrake = value(current, &TelSample::brake);
    const double comparisonBrake = value(comparison, &TelSample::brake);
    const double currentThrottle = value(current, &TelSample::throttle);
    const double comparisonThrottle = value(comparison, &TelSample::throttle);
    steering_->setReadings(currentSteering, comparisonSteering, currentColor_,
                           comparisonColor_, currentLabel_, comparisonLabel_,
                           QStringLiteral("Steering"));
    brake_->setReadings(currentBrake, comparisonBrake, currentColor_, comparisonColor_,
                        currentLabel_, comparisonLabel_, QStringLiteral("Brake"));
    throttle_->setReadings(currentThrottle, comparisonThrottle, currentColor_,
                           comparisonColor_, currentLabel_, comparisonLabel_,
                           QStringLiteral("Throttle"));
    const auto ersValue = [](const StsSample* sample) {
        if (!sample || !std::isfinite(sample->ers)) return qQNaN();
        return double(sample->ers) / 100.0;
    };
    ers_->setReadings(ersValue(currentStatus), ersValue(comparisonStatus),
                      currentColor_, comparisonColor_, currentLabel_,
                      comparisonLabel_, QStringLiteral("ERS"));

    auto setValue = [](QLabel* label, double reading, bool speed,
                       const QString& sourceLabel, const QString& fieldLabel) {
        const QString text = readingText(reading, speed);
        label->setText(text);
        const QString spoken = std::isfinite(reading)
            ? text + (speed ? QStringLiteral(" km/h") : QString())
            : QStringLiteral("No data");
        label->setAccessibleName(QStringLiteral("%1 %2: %3")
                                     .arg(sourceLabel, fieldLabel, spoken));
        label->setToolTip(QStringLiteral("%1: %2").arg(sourceLabel, spoken));
    };
    setValue(currentSpeed_, value(current, &TelSample::speed), true, currentLabel_,
             QStringLiteral("speed"));
    setValue(comparisonSpeed_, value(comparison, &TelSample::speed), true,
             comparisonLabel_, QStringLiteral("speed"));
    setValue(currentGear_, value(current, &TelSample::gear), false, currentLabel_,
             QStringLiteral("gear"));
    setValue(comparisonGear_, value(comparison, &TelSample::gear), false,
             comparisonLabel_, QStringLiteral("gear"));
}

void AnalyzeInputComparison::setCollapsed(bool collapsed) {
    collapsed_ = collapsed;
    body_->setVisible(!collapsed_);
    collapse_->setText(collapsed_ ? QStringLiteral("+") : QString::fromUtf8("\u2212"));
    collapse_->setToolTip(collapsed_ ? QStringLiteral("Expand data comparison")
                                    : QStringLiteral("Collapse data comparison"));
    collapse_->setAccessibleName(collapse_->toolTip());
    adjustSize();
    updatePosition();
    if (!collapsed_) refreshReadings(true);
}

bool AnalyzeInputComparison::eventFilter(QObject* watched, QEvent* event) {
    if (watched == mapArea_ && event->type() == QEvent::Resize) {
        updatePosition();
    } else if (watched == dragHandle_) {
        if (event->type() == QEvent::MouseButtonPress) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() != Qt::LeftButton) return false;
            dragging_ = true;
            dragHandle_->setCursor(Qt::ClosedHandCursor);
            dragOffset_ = mouse->globalPosition().toPoint() - mapToGlobal(QPoint(0, 0));
            dragHandle_->grabMouse();
            return true;
        }
        if (event->type() == QEvent::MouseMove && dragging_) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            const QPoint desired = mapArea_->mapFromGlobal(mouse->globalPosition().toPoint()) -
                                   dragOffset_;
            moveToPixel(desired, false);
            return true;
        }
        if (event->type() == QEvent::MouseButtonRelease && dragging_) {
            dragging_ = false;
            dragHandle_->setCursor(Qt::OpenHandCursor);
            dragHandle_->releaseMouse();
            savePosition();
            return true;
        }
        if (event->type() == QEvent::KeyPress) {
            auto* key = static_cast<QKeyEvent*>(event);
            const int step = key->modifiers().testFlag(Qt::ShiftModifier) ? 1 : 10;
            QPoint delta;
            if (key->key() == Qt::Key_Left) delta.setX(-step);
            else if (key->key() == Qt::Key_Right) delta.setX(step);
            else if (key->key() == Qt::Key_Up) delta.setY(-step);
            else if (key->key() == Qt::Key_Down) delta.setY(step);
            else return false;
            moveToPixel(pos() + delta, true);
            return true;
        }
    }
    return QFrame::eventFilter(watched, event);
}

void AnalyzeInputComparison::resizeEvent(QResizeEvent* event) {
    QFrame::resizeEvent(event);
    updatePosition();
}

void AnalyzeInputComparison::updatePosition() {
    if (!mapArea_) return;
    const int availableX = qMax(0, mapArea_->width() - width() - 2 * kInset);
    const int availableY = qMax(0, mapArea_->height() - height() - 2 * kInset);
    move(kInset + qRound(normalizedX_ * availableX),
         kInset + qRound(normalizedY_ * availableY));
    raise();
}

void AnalyzeInputComparison::moveToPixel(const QPoint& position, bool persist) {
    const int availableX = qMax(0, mapArea_->width() - width() - 2 * kInset);
    const int availableY = qMax(0, mapArea_->height() - height() - 2 * kInset);
    const int x = qBound(kInset, position.x(), kInset + availableX);
    const int y = qBound(kInset, position.y(), kInset + availableY);
    normalizedX_ = availableX > 0 ? double(x - kInset) / availableX : 0.0;
    normalizedY_ = availableY > 0 ? double(y - kInset) / availableY : 0.0;
    move(x, y);
    raise();
    if (persist) savePosition();
}

void AnalyzeInputComparison::savePosition() {
    QSettings settings(QStringLiteral("TrackNRace"), QStringLiteral("NativeRecorder"));
    settings.setValue("analyze/inputComparisonX", normalizedX_);
    settings.setValue("analyze/inputComparisonY", normalizedY_);
}
