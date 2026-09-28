#include "AnalysisInputCard.h"

#include <QHelpEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSettings>
#include <QToolTip>

#include <algorithm>
#include <cmath>

namespace {

// Geometry in logical pixels, matching Electron's .analyze-input-comparison.
constexpr int kWidth = 252;
constexpr int kHeader = 32;
constexpr int kPadTop = 8;
constexpr int kPadSide = 10;
constexpr int kPadBottom = 10;
constexpr int kRowHeight = 16;
constexpr int kRowGap = 6;
constexpr int kLabelWidth = 72;
constexpr int kLabelGap = 8;
constexpr int kUnitWidth = 34;
constexpr int kValueGap = 6;
constexpr int kLaneHeight = 4;
constexpr int kLaneGap = 2;
constexpr int kInset = 8;        // distance kept from the map's edges
constexpr int kRows = 6;

const char* const kNames[kRows] = {"Steering", "Brake", "Throttle", "ERS", "Speed", "Gear"};

QColor mix(const QColor& base, const QColor& over, double amount) {
    return QColor::fromRgbF(base.redF() + (over.redF() - base.redF()) * amount,
                            base.greenF() + (over.greenF() - base.greenF()) * amount,
                            base.blueF() + (over.blueF() - base.blueF()) * amount);
}

// The application's UI font, at the card's compact sizes.
QFont cardFont(const QFont& base, int pixelSize, QFont::Weight weight = QFont::Normal) {
    QFont font = base;
    font.setPixelSize(pixelSize);
    font.setWeight(weight);
    return font;
}

bool same(double a, double b) {
    return (std::isnan(a) && std::isnan(b)) || a == b;
}

template <class Sample>
const Sample* sampleAt(const QVector<Sample>& samples, const LapBlock& lap, double elapsed) {
    if (samples.isEmpty()) return nullptr;
    const double duration = qMax(0.0, double(lap.endSessionTime - lap.startSessionTime));
    const float target = static_cast<float>(lap.startSessionTime + qBound(0.0, elapsed, duration));
    const auto it = std::upper_bound(samples.cbegin(), samples.cend(), target,
                                     [](float time, const Sample& s) { return time < s.t; });
    return it == samples.cbegin() ? &samples.first() : &*(it - 1);
}

} // namespace

AnalysisInputCard::AnalysisInputCard(QWidget* area) : QWidget(area), area_(area) {
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAccessibleName(QStringLiteral("Data Comparison"));
    setAccessibleDescription(QStringLiteral("Drag the header or use the arrow keys to move it; "
                                            "hold Shift for fine steps. Return collapses it."));
    for (Readings& lap : readings_) lap.fill(qQNaN());
    labels_ = {QStringLiteral("Current lap"), QStringLiteral("Comparison lap")};

    const QSettings settings(QStringLiteral("TrackNRace"), QStringLiteral("NativeRecorder"));
    position_ = QPointF(qBound(0.0, settings.value("analyze/inputComparisonX", 0.0).toDouble(), 1.0),
                        qBound(0.0, settings.value("analyze/inputComparisonY", 0.0).toDouble(), 1.0));
    collapsed_ = settings.value("analyze/inputComparisonCollapsed", false).toBool();

    area_->installEventFilter(this);
    resize(sizeHint());
    place();
}

QSize AnalysisInputCard::sizeHint() const {
    if (collapsed_) return {kWidth, kHeader};
    return {kWidth, kHeader + 1 + kPadTop + kRows * kRowHeight + (kRows - 1) * kRowGap + kPadBottom};
}

void AnalysisInputCard::setIdentity(const QString& currentLabel, const QColor& currentColor,
                                    const QString& comparisonLabel, const QColor& comparisonColor) {
    labels_ = {currentLabel.isEmpty() ? QStringLiteral("Current lap") : currentLabel,
               comparisonLabel.isEmpty() ? QStringLiteral("Comparison lap") : comparisonLabel};
    colors_ = {currentColor, comparisonColor};
    update();
}

void AnalysisInputCard::setReadings(const LapBlock* current, const LapBlock* comparison,
                                    double elapsed) {
    const LapBlock* laps[2] = {current, comparison};
    bool changed = false;
    for (int lap = 0; lap < 2; ++lap) {
        Readings next;
        next.fill(qQNaN());
        if (const LapBlock* block = laps[lap]) {
            if (const TelSample* tel = sampleAt(block->tel, *block, elapsed)) {
                next[Steering] = tel->steering;
                next[Brake] = tel->brake;
                next[Throttle] = tel->throttle;
                next[Speed] = tel->speed;
                next[Gear] = tel->gear;
            }
            if (const StsSample* sts = sampleAt(block->sts, *block, elapsed))
                next[Ers] = sts->ers / 100.0;
        }
        for (double& value : next)
            if (!std::isfinite(value)) value = qQNaN();
        for (int channel = 0; channel < ChannelCount; ++channel) {
            if (same(next[channel], readings_[lap][channel])) continue;
            changed = true;
            break;
        }
        readings_[lap] = next;
    }
    if (changed && !collapsed_) update();
}

// ── Geometry ────────────────────────────────────────────────────────────────

QRect AnalysisInputCard::toggleRect() const {
    return {width() - 6 - 24, 4, 24, 24};
}

QRect AnalysisInputCard::rowRect(int row) const {
    const int top = kHeader + 1 + kPadTop + row * (kRowHeight + kRowGap);
    return {kPadSide, top, width() - 2 * kPadSide, kRowHeight};
}

QRect AnalysisInputCard::fieldRect(int row) const {
    const QRect full = rowRect(row);
    const int left = full.left() + kLabelWidth + kLabelGap;
    return {left, full.top(), full.right() + 1 - left, full.height()};
}

QRectF AnalysisInputCard::laneRect(int row, int lap) const {
    // 1 px border + 2 px padding around two 4 px lanes with a 2 px gap.
    const QRectF inner = QRectF(fieldRect(row)).adjusted(3, 3, -3, -3);
    return {inner.left(), inner.top() + lap * (kLaneHeight + kLaneGap), inner.width(),
            double(kLaneHeight)};
}

QString AnalysisInputCard::readingText(Channel channel, int lap, bool spoken) const {
    const double value = readings_[lap][channel];
    if (std::isnan(value)) return spoken ? QStringLiteral("No data") : QString::fromUtf8("—");
    if (channel == Speed) {
        const QString text = QString::number(qRound(value));
        return spoken ? text + QStringLiteral(" km/h") : text;
    }
    if (channel == Gear) {
        const int gear = qRound(value);
        return gear < 0 ? QStringLiteral("R") : gear == 0 ? QStringLiteral("N") : QString::number(gear);
    }
    const bool signedValue = channel == Steering;
    const double clamped = qBound(signedValue ? -1.0 : 0.0, value, 1.0);
    const int percent = qRound(std::abs(clamped) * 100.0);
    const QString side = !signedValue || percent == 0 ? QString()
                       : clamped < 0 ? QStringLiteral("L ") : QStringLiteral("R ");
    return side + QString::number(percent) + QLatin1Char('%');
}

// ── Painting ────────────────────────────────────────────────────────────────

void AnalysisInputCard::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const QPalette& pal = palette();
    const QColor panel = pal.color(QPalette::Window);
    const QColor text = pal.color(QPalette::WindowText);
    const QColor secondary = pal.color(QPalette::PlaceholderText);
    const QColor border = mix(panel, text, 0.14);
    const QColor input = pal.color(QPalette::Base);
    const QColor muted = mix(panel, text, 0.35);

    QPainterPath card;
    card.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);
    painter.fillPath(card, panel);
    painter.setPen(QPen(border, 1));
    painter.drawPath(card);

    // Header: grip, caption, collapse button.
    const int headerMid = kHeader / 2;
    painter.setPen(Qt::NoPen);
    painter.setBrush(secondary);
    for (int column = 0; column < 2; ++column)
        for (int row = -1; row <= 1; ++row)
            painter.drawEllipse(QPointF(12.5 + column * 4, headerMid + row * 4), 1.0, 1.0);

    QFont header = cardFont(font(), 9, QFont::DemiBold);
    header.setLetterSpacing(QFont::AbsoluteSpacing, 0.72);
    painter.setFont(header);
    painter.setPen(dragging_ ? text : secondary);
    painter.drawText(QRect(27, 0, toggleRect().left() - 27, kHeader), Qt::AlignLeft | Qt::AlignVCenter,
                     QStringLiteral("DATA COMPARISON"));

    const QRect toggle = toggleRect();
    if (buttonHovered_) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(mix(panel, text, 0.08));
        painter.drawRoundedRect(toggle, 4, 4);
    }
    const QPointF centre = QRectF(toggle).center();
    painter.setPen(QPen(buttonHovered_ ? text : secondary, 1.5, Qt::SolidLine, Qt::RoundCap));
    painter.drawLine(centre - QPointF(4.5, 0), centre + QPointF(4.5, 0));
    if (collapsed_) painter.drawLine(centre - QPointF(0, 4.5), centre + QPointF(0, 4.5));

    if (hasFocus()) {
        painter.setPen(QPen(pal.color(QPalette::Highlight), 2));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 7, 7);
    }
    if (collapsed_) return;

    painter.setPen(QPen(border, 1));
    painter.drawLine(QPointF(0.5, kHeader + 0.5), QPointF(width() - 0.5, kHeader + 0.5));

    const QFont labelFont = cardFont(font(), 11);
    const QFont valueFont = cardFont(font(), 11, QFont::DemiBold);
    const QFont unitFont = cardFont(font(), 9);
    const QFont missingFont = cardFont(font(), 8);

    for (int row = 0; row < kRows; ++row) {
        const QRect full = rowRect(row);
        const QRect field = fieldRect(row);
        painter.setFont(labelFont);
        painter.setPen(secondary);
        painter.drawText(QRect(full.left(), full.top(), kLabelWidth, full.height()),
                         Qt::AlignLeft | Qt::AlignVCenter, QString::fromLatin1(kNames[row]));

        const auto channel = static_cast<Channel>(row);
        if (channel == Speed || channel == Gear) {
            // Two value columns in the laps' colours, then the unit.
            const int columnWidth = (field.width() - kUnitWidth - 2 * kValueGap) / 2;
            painter.setFont(valueFont);
            for (int lap = 0; lap < 2; ++lap) {
                const bool known = !std::isnan(readings_[lap][channel]);
                painter.setPen(known && colors_[lap].isValid() ? colors_[lap] : muted);
                painter.drawText(QRect(field.left() + lap * (columnWidth + kValueGap), field.top(),
                                       columnWidth, field.height()),
                                 Qt::AlignLeft | Qt::AlignVCenter, readingText(channel, lap, false));
            }
            if (channel == Speed) {
                painter.setFont(unitFont);
                painter.setPen(secondary);
                painter.drawText(QRect(field.right() + 1 - kUnitWidth, field.top(), kUnitWidth,
                                       field.height()),
                                 Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("km/h"));
            }
            continue;
        }

        // One bordered gauge holding a thin lane per lap.
        const bool signedValue = channel == Steering;
        const QRectF box = QRectF(field).adjusted(0.5, 0.5, -0.5, -0.5);
        painter.setPen(QPen(border, 1));
        painter.setBrush(input);
        painter.drawRoundedRect(box, 3, 3);
        for (int lap = 0; lap < 2; ++lap) {
            const QRectF lane = laneRect(row, lap);
            const double raw = readings_[lap][channel];
            if (std::isnan(raw)) {
                painter.setFont(missingFont);
                painter.setPen(muted);
                painter.drawText(lane.adjusted(0, -3, 0, 3), Qt::AlignCenter, QString::fromUtf8("—"));
                continue;
            }
            const double value = qBound(signedValue ? -1.0 : 0.0, raw, 1.0);
            const double half = lane.width() / 2.0;
            const QRectF fill = signedValue
                ? QRectF(lane.left() + half + qMin(0.0, value) * half, lane.top(),
                         std::abs(value) * half, lane.height())
                : QRectF(lane.left(), lane.top(), value * lane.width(), lane.height());
            if (fill.width() <= 0) continue;
            painter.setPen(Qt::NoPen);
            painter.setBrush(colors_[lap].isValid() ? colors_[lap] : text);
            painter.drawRoundedRect(fill, 1, 1);
        }
        if (signedValue) {
            painter.setPen(QPen(muted, 1));
            const double x = std::floor(box.center().x()) + 0.5;
            painter.drawLine(QPointF(x, box.top() + 1), QPointF(x, box.bottom() - 1));
        }
    }
}

// ── Interaction ─────────────────────────────────────────────────────────────

bool AnalysisInputCard::event(QEvent* event) {
    if (event->type() == QEvent::ToolTip) {
        const auto* help = static_cast<QHelpEvent*>(event);
        const QPoint pos = help->pos();
        QString tip;
        if (toggleRect().contains(pos)) {
            tip = collapsed_ ? QStringLiteral("Expand data comparison")
                             : QStringLiteral("Collapse data comparison");
        } else if (pos.y() < kHeader) {
            tip = QStringLiteral("Drag to move · Arrow keys to adjust position");
        } else if (!collapsed_) {
            for (int row = 0; row < kRows; ++row) {
                if (!rowRect(row).contains(pos)) continue;
                const auto channel = static_cast<Channel>(row);
                tip = QStringLiteral("%1: %2\n%3: %4")
                          .arg(labels_[0], readingText(channel, 0, true),
                               labels_[1], readingText(channel, 1, true));
                break;
            }
        }
        if (tip.isEmpty()) QToolTip::hideText();
        else QToolTip::showText(help->globalPos(), tip, this);
        return true;
    }
    return QWidget::event(event);
}

bool AnalysisInputCard::eventFilter(QObject* watched, QEvent* event) {
    if (watched == area_ && event->type() == QEvent::Resize) place();
    return QWidget::eventFilter(watched, event);
}

void AnalysisInputCard::mousePressEvent(QMouseEvent* event) {
    const QPoint pos = event->position().toPoint();
    if (event->button() == Qt::LeftButton && pos.y() < kHeader && !toggleRect().contains(pos)) {
        dragging_ = true;
        dragOffset_ = pos;
        dragStart_ = position_;
        setCursor(Qt::ClosedHandCursor);
        update();
    }
    event->accept();
}

void AnalysisInputCard::mouseMoveEvent(QMouseEvent* event) {
    const QPoint pos = event->position().toPoint();
    if (dragging_) {
        moveTo(mapToParent(pos) - dragOffset_);
        return;
    }
    const bool hovered = toggleRect().contains(pos);
    if (hovered != buttonHovered_) {
        buttonHovered_ = hovered;
        update(toggleRect());
    }
    setCursor(pos.y() < kHeader && !hovered ? Qt::OpenHandCursor : Qt::ArrowCursor);
}

void AnalysisInputCard::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) return;
    if (dragging_) {
        dragging_ = false;
        setCursor(Qt::OpenHandCursor);
        savePosition();
        update();
        return;
    }
    if (toggleRect().contains(event->position().toPoint())) setCollapsed(!collapsed_);
}

void AnalysisInputCard::leaveEvent(QEvent* event) {
    if (buttonHovered_) {
        buttonHovered_ = false;
        update(toggleRect());
    }
    QWidget::leaveEvent(event);
}

void AnalysisInputCard::keyPressEvent(QKeyEvent* event) {
    if (dragging_ && event->key() == Qt::Key_Escape) {
        dragging_ = false;
        position_ = dragStart_;
        setCursor(Qt::ArrowCursor);
        place();
        update();
        return;
    }
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        setCollapsed(!collapsed_);
        return;
    }
    const int step = event->modifiers().testFlag(Qt::ShiftModifier) ? 1 : 10;
    QPoint delta;
    switch (event->key()) {
    case Qt::Key_Left: delta.setX(-step); break;
    case Qt::Key_Right: delta.setX(step); break;
    case Qt::Key_Up: delta.setY(-step); break;
    case Qt::Key_Down: delta.setY(step); break;
    default: QWidget::keyPressEvent(event); return;
    }
    moveTo(pos() + delta);
    savePosition();
}

// ── Placement ───────────────────────────────────────────────────────────────

void AnalysisInputCard::setCollapsed(bool collapsed) {
    collapsed_ = collapsed;
    resize(sizeHint());
    place();
    update();
    QSettings(QStringLiteral("TrackNRace"), QStringLiteral("NativeRecorder"))
        .setValue("analyze/inputComparisonCollapsed", collapsed_);
}

void AnalysisInputCard::place() {
    // Position is a fraction of the free space, so the card keeps its relative
    // spot (and never leaves the map) as the map resizes or the card collapses.
    const int freeX = qMax(0, area_->width() - width() - 2 * kInset);
    const int freeY = qMax(0, area_->height() - height() - 2 * kInset);
    move(kInset + qRound(position_.x() * freeX), kInset + qRound(position_.y() * freeY));
    raise();
}

void AnalysisInputCard::moveTo(const QPoint& topLeft) {
    const int freeX = qMax(0, area_->width() - width() - 2 * kInset);
    const int freeY = qMax(0, area_->height() - height() - 2 * kInset);
    position_ = QPointF(freeX > 0 ? qBound(0.0, double(topLeft.x() - kInset) / freeX, 1.0) : 0.0,
                        freeY > 0 ? qBound(0.0, double(topLeft.y() - kInset) / freeY, 1.0) : 0.0);
    place();
}

void AnalysisInputCard::savePosition() const {
    QSettings settings(QStringLiteral("TrackNRace"), QStringLiteral("NativeRecorder"));
    settings.setValue("analyze/inputComparisonX", position_.x());
    settings.setValue("analyze/inputComparisonY", position_.y());
}
