#include "AnalysisPanels.h"

#include "../SegmentedControl.h"

#include <QAction>
#include <QCursor>
#include <QDockWidget>
#include <QEnterEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QStyle>
#include <QStyleOptionButton>
#include <QStyleOptionToolButton>
#include <QStylePainter>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

constexpr int kStripWidth = 64;

} // namespace

// ── Grips ───────────────────────────────────────────────────────────────────

void analysisPaintGrip(QPainter& painter, const QRect& rect, const QPalette& palette, bool hot) {
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect, palette.color(QPalette::Window));
    const bool horizontal = rect.width() > rect.height();
    QColor line = palette.color(QPalette::WindowText);
    line.setAlpha(90);
    if (hot) line = palette.color(QPalette::Highlight);
    const QPointF centre = QRectF(rect).center();
    painter.setPen(QPen(line, hot ? 2.0 : 1.0));
    if (horizontal) painter.drawLine(QPointF(rect.left(), centre.y()), QPointF(rect.right() + 1, centre.y()));
    else painter.drawLine(QPointF(centre.x(), rect.top()), QPointF(centre.x(), rect.bottom() + 1));
    painter.restore();
}

AnalysisGripSplitter::AnalysisGripSplitter(Qt::Orientation orientation, QWidget* parent)
    : QSplitter(orientation, parent) {
    setHandleWidth(kAnalysisGripExtent);
}

QSplitterHandle* AnalysisGripSplitter::createHandle() {
    return new AnalysisGripHandle(orientation(), this);
}

AnalysisGripHandle::AnalysisGripHandle(Qt::Orientation orientation, QSplitter* parent)
    : QSplitterHandle(orientation, parent) {
    setToolTip(QStringLiteral("Drag to resize"));
}

void AnalysisGripHandle::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    analysisPaintGrip(painter, rect(), palette(), hovered_ || pressed_);
}

void AnalysisGripHandle::enterEvent(QEnterEvent* event) {
    hovered_ = true;
    update();
    QSplitterHandle::enterEvent(event);
}

void AnalysisGripHandle::leaveEvent(QEvent* event) {
    hovered_ = false;
    update();
    QSplitterHandle::leaveEvent(event);
}

void AnalysisGripHandle::mousePressEvent(QMouseEvent* event) {
    pressed_ = event->button() == Qt::LeftButton;
    update();
    QSplitterHandle::mousePressEvent(event);
}

void AnalysisGripHandle::mouseReleaseEvent(QMouseEvent* event) {
    pressed_ = false;
    update();
    QSplitterHandle::mouseReleaseEvent(event);
}

void analysisPaintSelectedRow(QPainter* painter, const QRect& rect, const QPalette& palette,
                              bool enabled, const QWidget* widget) {
    QStyleOptionButton bevel;
    bevel.state = QStyle::State_Raised | (enabled ? QStyle::State_Enabled : QStyle::State_None);
    bevel.palette = palette;
    bevel.rect = rect.adjusted(1, 1, -1, -1);
    bevel.features = QStyleOptionButton::DefaultButton;
    QStyle* style = widget ? widget->style() : nullptr;
    if (style) style->drawControl(QStyle::CE_PushButtonBevel, &bevel, painter, widget);
}

// ── Toggle button ───────────────────────────────────────────────────────────

QSize AnalysisToggleButton::sizeHint() const {
    return tnrui::withSystemStyleTogglePadding(QToolButton::sizeHint());
}

void AnalysisToggleButton::paintEvent(QPaintEvent* event) {
    // Styles left-align an icon + text label, so the added width has to be split
    // by insetting the label.
    const int inset = toolButtonStyle() == Qt::ToolButtonTextBesideIcon
                          ? tnrui::systemStyleTogglePadding() : 0;
    if (!isChecked() && inset == 0) {
        QToolButton::paintEvent(event);
        return;
    }
    QStylePainter painter(this);
    QStyleOptionToolButton label;
    initStyleOption(&label);
    if (isChecked()) {
        // The segmented controls' selected look: a default push button's bevel...
        QStyleOptionButton bevel;
        bevel.initFrom(this);
        bevel.rect = rect();
        bevel.features = QStyleOptionButton::DefaultButton;
        bevel.state |= QStyle::State_Raised;
        bevel.state &= ~(QStyle::State_On | QStyle::State_Sunken);
        painter.drawControl(QStyle::CE_PushButtonBevel, bevel);
    } else {
        QStyleOptionToolButton panel = label;
        panel.text.clear();
        panel.icon = QIcon();
        painter.drawComplexControl(QStyle::CC_ToolButton, panel);
    }
    // ...with this button's icon and label, laid out as usual.
    label.rect = style()->subControlRect(QStyle::CC_ToolButton, &label, QStyle::SC_ToolButton, this)
                     .adjusted(inset, 0, -inset, 0);
    if (isChecked())
        label.state &= ~(QStyle::State_On | QStyle::State_Sunken);
    painter.drawControl(QStyle::CE_ToolButtonLabel, label);
}

// ── Tool strip ──────────────────────────────────────────────────────────────

AnalysisToolStrip::AnalysisToolStrip(const QString& accessibleName, QWidget* parent)
    : QScrollArea(parent) {
    setAccessibleName(accessibleName);
    setFrameShape(QFrame::NoFrame);
    setWidgetResizable(true);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setBackgroundRole(QPalette::Window);
    body_ = new QWidget(this);
    layout_ = new QVBoxLayout(body_);
    layout_->setContentsMargins(3, 6, 3, 6);
    layout_->setSpacing(2);
    layout_->addStretch(1);
    setWidget(body_);
    setFixedWidth(kStripWidth);
}

void AnalysisToolStrip::addTool(QAction* action) {
    auto* button = new AnalysisToggleButton(body_);
    button->setDefaultAction(action);
    button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    button->setAutoRaise(true);
    const int icon = style()->pixelMetric(QStyle::PM_SmallIconSize, nullptr, this);
    button->setIconSize(QSize(icon, icon));
    QFont stripFont = button->font();
    stripFont.setPointSizeF(qMax(7.0, stripFont.pointSizeF() * 0.85));
    button->setFont(stripFont);
    button->setFixedWidth(kStripWidth - 6);
    button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    layout_->insertWidget(layout_->count() - 1, button, 0, Qt::AlignHCenter);
}

void AnalysisToolStrip::addToolPair(QAction* first, QAction* second) {
    constexpr int kGap = 2;
    auto* row = new QWidget(body_);
    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(kGap);
    const int icon = style()->pixelMetric(QStyle::PM_SmallIconSize, nullptr, this);
    for (QAction* action : {first, second}) {
        auto* button = new AnalysisToggleButton(row);
        button->setDefaultAction(action);
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        button->setAutoRaise(true);
        button->setIconSize(QSize(icon, icon));
        button->setFixedWidth((kStripWidth - 6 - kGap) / 2);
        button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        rowLayout->addWidget(button);
    }
    layout_->insertWidget(layout_->count() - 1, row, 0, Qt::AlignHCenter);
}

void AnalysisToolStrip::addSeparator() {
    auto* line = new QFrame(body_);
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);
    line->setFixedWidth(kStripWidth - 22);
    layout_->insertWidget(layout_->count() - 1, line, 0, Qt::AlignHCenter);
}

void AnalysisToolStrip::addCaption(const QString& text) {
    auto* caption = new QLabel(text, body_);
    QFont stripFont = caption->font();
    stripFont.setPointSizeF(qMax(7.0, stripFont.pointSizeF() * 0.8));
    caption->setFont(stripFont);
    caption->setForegroundRole(QPalette::PlaceholderText);
    caption->setAlignment(Qt::AlignHCenter);
    layout_->insertWidget(layout_->count() - 1, caption, 0, Qt::AlignHCenter);
}

// ── Workspace ───────────────────────────────────────────────────────────────

AnalysisWorkspace::AnalysisWorkspace(QWidget* parent) : QMainWindow(parent) {
    setWindowFlags(Qt::Widget);
    // The app window's style sheet sets every QMainWindow's separators to 0 px,
    // which leaves nothing to grab. This window's own rule wins over it: wide
    // enough to drag, transparent, with the line drawn in event().
    setStyleSheet(QStringLiteral("QMainWindow::separator { width: %1px; height: %1px; background: transparent; }")
                      .arg(kAnalysisGripExtent));
    setMouseTracking(true);
    setAttribute(Qt::WA_Hover);   // separators light up under the pointer
    setDockNestingEnabled(true);
    // No animation: a panel coming back is sized at once, not after a slide.
    setDockOptions(QMainWindow::AllowTabbedDocks |
                   QMainWindow::AllowNestedDocks);
}

QMenu* AnalysisWorkspace::createPopupMenu() {
    auto* menu = new QMenu(this);
    if (fillPanelsMenu) fillPanelsMenu(menu);
    return menu;
}

QVector<QRect> AnalysisWorkspace::separators() const {
    QVector<QRect> panes;
    if (centralWidget() && centralWidget()->isVisible()) panes << centralWidget()->geometry();
    for (const QDockWidget* dock : findChildren<QDockWidget*>(QString(), Qt::FindDirectChildrenOnly))
        if (dock->isVisible() && !dock->isFloating()) panes << dock->geometry();
    QVector<QRect> gaps;
    const int widest = kAnalysisGripExtent + 6;
    for (const QRect& a : panes)
        for (const QRect& b : panes) {
            // b to the right of a, overlapping vertically: a vertical separator.
            const int dx = b.left() - a.right() - 1;
            const int top = qMax(a.top(), b.top());
            const int bottom = qMin(a.bottom(), b.bottom());
            if (dx > 0 && dx <= widest && bottom > top) gaps << QRect(a.right() + 1, top, dx, bottom - top + 1);
            // b below a, overlapping horizontally: a horizontal separator.
            const int dy = b.top() - a.bottom() - 1;
            const int left = qMax(a.left(), b.left());
            const int right = qMin(a.right(), b.right());
            if (dy > 0 && dy <= widest && right > left) gaps << QRect(left, a.bottom() + 1, right - left + 1, dy);
        }
    return gaps;
}

bool AnalysisWorkspace::event(QEvent* event) {
    const bool handled = QMainWindow::event(event);
    switch (event->type()) {
    case QEvent::Paint: {
        QPainter painter(this);
        for (const QRect& gap : separators()) analysisPaintGrip(painter, gap, palette(), gap == hovered_);
        break;
    }
    case QEvent::HoverMove:
    case QEvent::HoverEnter:
    case QEvent::MouseMove:
    case QEvent::HoverLeave:
    case QEvent::Leave: {
        QRect hit;
        if (event->type() != QEvent::HoverLeave && event->type() != QEvent::Leave) {
            const QPoint at = mapFromGlobal(QCursor::pos());
            for (const QRect& gap : separators())
                if (gap.contains(at)) hit = gap;
        }
        if (hit != hovered_) {
            hovered_ = hit;
            update();
        }
        break;
    }
    default:
        break;
    }
    return handled;
}

// ── Edge tabs ───────────────────────────────────────────────────────────────

AnalysisRailButton::AnalysisRailButton(const QString& text, bool rightSide, QWidget* parent)
    : QAbstractButton(parent), rightSide_(rightSide) {
    setText(text);
    setCursor(Qt::PointingHandCursor);
    setToolTip(QStringLiteral("Show %1").arg(text));
    setAccessibleName(QStringLiteral("Show %1").arg(text));
}

QSize AnalysisRailButton::sizeHint() const {
    const QFontMetrics fm(font());
    return {fm.height() + 10, fm.horizontalAdvance(text()) + 24};
}

void AnalysisRailButton::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QPalette pal = palette();
    const QRectF box = QRectF(rect()).adjusted(1.5, 1.5, -1.5, -1.5);
    QColor fill = pal.color(QPalette::Button);
    if (hovered_ || isDown()) fill = pal.color(QPalette::Highlight).darker(hovered_ && !isDown() ? 220 : 180);
    p.setPen(QPen(hovered_ ? pal.color(QPalette::Highlight) : pal.color(QPalette::Mid), 1));
    p.setBrush(fill);
    p.drawRoundedRect(box, 3, 3);
    // Text runs bottom to top on the left edge and top to bottom on the right,
    // so it always reads towards the content.
    p.translate(width() / 2.0, height() / 2.0);
    p.rotate(rightSide_ ? 90 : -90);
    p.setPen(pal.color(QPalette::ButtonText));
    const QRectF text(-height() / 2.0, -width() / 2.0, height(), width());
    p.drawText(text, Qt::AlignCenter, this->text());
}

void AnalysisRailButton::enterEvent(QEnterEvent* event) {
    hovered_ = true;
    update();
    QAbstractButton::enterEvent(event);
}

void AnalysisRailButton::leaveEvent(QEvent* event) {
    hovered_ = false;
    update();
    QAbstractButton::leaveEvent(event);
}

AnalysisDockRail::AnalysisDockRail(bool rightSide, QWidget* parent)
    : QWidget(parent), rightSide_(rightSide) {
    setAccessibleName(rightSide ? QStringLiteral("Closed panels, right edge")
                                : QStringLiteral("Closed panels, left edge"));
    layout_ = new QVBoxLayout(this);
    layout_->setContentsMargins(2, 6, 2, 6);
    layout_->setSpacing(4);
    layout_->addStretch(1);
    hide();
}

void AnalysisDockRail::setDocks(const QVector<QDockWidget*>& closed) {
    for (const QPointer<AnalysisRailButton>& button : buttons_)
        if (button) button->deleteLater();
    buttons_.clear();
    for (QDockWidget* dock : closed) {
        auto* button = new AnalysisRailButton(dock->windowTitle(), rightSide_, this);
        connect(button, &QAbstractButton::clicked, dock, [dock] {
            dock->show();
            dock->raise();
        });
        layout_->insertWidget(layout_->count() - 1, button, 0, Qt::AlignHCenter);
        buttons_.push_back(button);
    }
    setVisible(!closed.isEmpty());
}
