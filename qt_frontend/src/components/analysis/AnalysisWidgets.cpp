#include "AnalysisWidgets.h"

#include <QColorDialog>
#include <QEvent>
#include <QFontDatabase>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QResizeEvent>
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
    grid_ = new QGridLayout(this);
    grid_->setContentsMargins(6, 6, 6, 6);
    grid_->setSpacing(6);

    QFont valueFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    valueFont.setPointSizeF(font().pointSizeF());
    const int valueWidth = QFontMetrics(valueFont).horizontalAdvance(QStringLiteral("+00.000"));

    const char* names[] = {"S1", "S2", "S3", "Lap"};
    for (int index = 0; index < 4; ++index) {
        auto* cell = new QFrame(this);
        cell->setFrameShape(QFrame::StyledPanel);
        auto* cellLayout = new QHBoxLayout(cell);
        cellLayout->setContentsMargins(8, 3, 8, 3);
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
        cellLayout->addStretch(1);
        cellLayout->addWidget(value);
        cells_.push_back(cell);
        values_.push_back(value);
        if (index < 3) sectorCells_.push_back(cell);
    }
    seconds_.fill(qQNaN());
    setToolTip(QStringLiteral("Time delta of the primary lap against the comparison lap. "
                              "Positive is slower."));
    reflow();
    repaintValues();
}

void AnalysisDeltaReadout::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    reflow();
}

// One row of four tiles, or two rows of two when a row would not fit.
void AnalysisDeltaReadout::reflow() {
    int widest = 0;
    int shown = 0;
    for (QWidget* cell : cells_)
        if (!cell->isHidden()) {
            widest = qMax(widest, cell->sizeHint().width());
            ++shown;
        }
    const QMargins m = grid_->contentsMargins();
    const int available = width() - m.left() - m.right();
    const int columns = shown <= 1 || available >= shown * widest + (shown - 1) * grid_->spacing()
        ? qMax(1, shown) : 2;
    if (columns == columns_ && shown == shownCells_) return;
    columns_ = columns;
    shownCells_ = shown;
    for (QWidget* cell : cells_) grid_->removeWidget(cell);
    for (int column = 0; column < 4; ++column) grid_->setColumnStretch(column, 0);
    int index = 0;
    for (QWidget* cell : cells_) {
        if (cell->isHidden()) continue;
        grid_->addWidget(cell, index / columns, index % columns);
        grid_->setColumnStretch(index % columns, 1);
        ++index;
    }
    updateGeometry();
}

void AnalysisDeltaReadout::setColors(const QColor& slower, const QColor& faster) {
    slower_ = slower;
    faster_ = faster;
    repaintValues();
}

void AnalysisDeltaReadout::setSectorsVisible(bool visible) {
    if (!sectorCells_.isEmpty() && sectorCells_.first()->isHidden() == !visible) return;
    for (QWidget* cell : sectorCells_) cell->setVisible(visible);
    columns_ = 0;
    reflow();
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
