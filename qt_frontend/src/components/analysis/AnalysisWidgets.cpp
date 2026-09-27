#include "AnalysisWidgets.h"

#include <QColorDialog>
#include <QEvent>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QStyleOption>
#include <QStylePainter>

#include <cmath>

namespace analysis {

QString formatLapTime(int milliseconds) {
    if (milliseconds <= 0) return QString::fromUtf8("—");
    return QStringLiteral("%1:%2")
        .arg(milliseconds / 60000)
        .arg((milliseconds % 60000) / 1000.0, 6, 'f', 3, QChar('0'));
}

QString formatDelta(double seconds) {
    if (!std::isfinite(seconds)) return QString::fromUtf8("—.---");
    const double value = std::abs(seconds) < 0.0005 ? 0.0 : seconds;
    return (value > 0 ? QStringLiteral("+") : QString()) + QString::number(value, 'f', 3);
}

QIcon dotIcon(const QColor& fill, const QWidget* context, int size) {
    const qreal ratio = context ? context->devicePixelRatioF() : 1.0;
    QPixmap pixmap(QSize(size, size) * ratio);
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    if (fill.isValid()) {
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        const QColor rim = context ? context->palette().color(QPalette::Mid) : fill.darker(160);
        painter.setPen(QPen(rim, 1.0));
        painter.setBrush(fill);
        painter.drawEllipse(QRectF(0.5, 0.5, size - 1.0, size - 1.0));
    }
    return QIcon(pixmap);
}

} // namespace analysis

// ── Colour button ───────────────────────────────────────────────────────────

AnalysisColorButton::AnalysisColorButton(const QString& dialogTitle, QWidget* parent)
    : QToolButton(parent), dialogTitle_(dialogTitle) {
    setToolButtonStyle(Qt::ToolButtonIconOnly);
    setAutoRaise(false);
    setFocusPolicy(Qt::StrongFocus);
    connect(this, &QToolButton::clicked, this, [this] {
        const QColor picked = QColorDialog::getColor(color_, this, dialogTitle_,
                                                     QColorDialog::DontUseNativeDialog);
        if (!picked.isValid() || picked == color_) return;
        setColor(picked);
        emit colorPicked(picked);
    });
}

void AnalysisColorButton::setColor(const QColor& color) {
    color_ = color;
    const QString name = color_.isValid() ? color_.name().toUpper() : QStringLiteral("none");
    setToolTip(QStringLiteral("%1 (%2)").arg(dialogTitle_, name));
    setAccessibleName(dialogTitle_);
    setAccessibleDescription(name);
    update();
}

void AnalysisColorButton::paintEvent(QPaintEvent*) {
    // Draw the native button bevel, then the colour inset within its content rect.
    QStylePainter painter(this);
    QStyleOptionToolButton option;
    initStyleOption(&option);
    option.text.clear();
    option.icon = QIcon();
    painter.drawComplexControl(QStyle::CC_ToolButton, option);

    const QRect content = style()->subControlRect(QStyle::CC_ToolButton, &option,
                                                  QStyle::SC_ToolButton, this)
                              .adjusted(5, 5, -5, -5);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(palette().color(isEnabled() ? QPalette::Active : QPalette::Disabled,
                                   QPalette::Mid));
    QColor fill = color_;
    if (!isEnabled()) fill.setAlphaF(0.35);
    painter.setBrush(fill);
    painter.drawRoundedRect(QRectF(content).adjusted(0.5, 0.5, -0.5, -0.5), 2, 2);
}

// ── Delta readout ───────────────────────────────────────────────────────────

AnalysisDeltaReadout::AnalysisDeltaReadout(QWidget* parent) : QWidget(parent) {
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(6, 0, 6, 0);
    layout->setSpacing(14);

    QFont valueFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    valueFont.setPointSizeF(font().pointSizeF());
    const int valueWidth = QFontMetrics(valueFont).horizontalAdvance(QStringLiteral("+00.000"));

    const char* names[] = {"S1", "S2", "S3", "Lap"};
    for (int index = 0; index < 4; ++index) {
        auto* cell = new QWidget(this);
        auto* cellLayout = new QHBoxLayout(cell);
        cellLayout->setContentsMargins(0, 0, 0, 0);
        cellLayout->setSpacing(5);
        auto* caption = new QLabel(QString::fromLatin1(names[index]), cell);
        QFont captionFont = caption->font();
        captionFont.setBold(true);
        captionFont.setPointSizeF(qMax(7.0, captionFont.pointSizeF() * 0.85));
        caption->setFont(captionFont);
        caption->setForegroundRole(QPalette::PlaceholderText);
        auto* value = new QLabel(cell);
        value->setFont(valueFont);
        value->setMinimumWidth(valueWidth);
        value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        cellLayout->addWidget(caption);
        cellLayout->addWidget(value);
        layout->addWidget(cell);
        values_.push_back(value);
        if (index < 3) sectorCells_.push_back(cell);
    }
    seconds_.fill(qQNaN());
    setToolTip(QStringLiteral("Time delta of the primary lap against the comparison lap. "
                              "Positive is slower."));
    repaintValues();
}

void AnalysisDeltaReadout::setColors(const QColor& slower, const QColor& faster) {
    slower_ = slower;
    faster_ = faster;
    repaintValues();
}

void AnalysisDeltaReadout::setSectorsVisible(bool visible) {
    for (QWidget* cell : sectorCells_) cell->setVisible(visible);
}

void AnalysisDeltaReadout::setValues(const std::array<double, 4>& seconds) {
    seconds_ = seconds;
    repaintValues();
}

void AnalysisDeltaReadout::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange) repaintValues();
}

void AnalysisDeltaReadout::repaintValues() {
    const QColor neutral = palette().color(QPalette::PlaceholderText);
    for (int index = 0; index < values_.size(); ++index) {
        QLabel* label = values_[index];
        const double value = seconds_[static_cast<size_t>(index)];
        label->setText(analysis::formatDelta(value));
        const bool known = std::isfinite(value) && std::abs(value) >= 0.0005;
        const QColor color = !known ? neutral : value > 0 ? slower_ : faster_;
        // Called at playback rate: only touch the palette when the colour changes.
        if (label->palette().color(QPalette::WindowText) == color) continue;
        QPalette labelPalette = label->palette();
        labelPalette.setColor(QPalette::WindowText, color);
        label->setPalette(labelPalette);
    }
}
