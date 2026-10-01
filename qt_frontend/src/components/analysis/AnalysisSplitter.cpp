#include "AnalysisSplitter.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>

namespace {
QColor mix(const QColor& base, const QColor& over, double amount) {
    return QColor::fromRgbF(base.redF() + (over.redF() - base.redF()) * amount,
                            base.greenF() + (over.greenF() - base.greenF()) * amount,
                            base.blueF() + (over.blueF() - base.blueF()) * amount);
}
}

// The "52 / 48" chip shown over the divider. Transparent to the mouse, so it
// never gets in the way of the drag it is describing. It lives on the window,
// not the splitter: a QSplitter adopts every child widget as another pane.
class AnalysisSplitBadge : public QWidget {
public:
    explicit AnalysisSplitBadge(QWidget* parent) : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        QFont badgeFont = font();
        badgeFont.setPointSizeF(qMax(7.0, badgeFont.pointSizeF() * 0.8));
        setFont(badgeFont);
        hide();
    }

    void setText(const QString& text) {
        text_ = text;
        const QFontMetrics metrics(font());
        resize(metrics.horizontalAdvance(text_) + 12, metrics.height() + 4);
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QColor panel = palette().color(QPalette::Window);
        const QColor text = palette().color(QPalette::WindowText);
        QPainterPath path;
        path.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
        painter.fillPath(path, panel);
        painter.setPen(mix(panel, text, 0.2));
        painter.drawPath(path);
        painter.setPen(palette().color(QPalette::PlaceholderText));
        painter.drawText(rect(), Qt::AlignCenter, text_);
    }

private:
    QString text_;
};

AnalysisSplitter::AnalysisSplitter(QWidget* parent) : QSplitter(Qt::Horizontal, parent) {
    connect(this, &QSplitter::splitterMoved, this, &AnalysisSplitter::onMoved);
}

AnalysisSplitter::~AnalysisSplitter() {
    delete badge_.data();   // owned by the window, which may outlive the splitter
}

QSplitterHandle* AnalysisSplitter::createHandle() {
    return new AnalysisSplitterHandle(orientation(), this);
}

bool AnalysisSplitter::bothVisible() const {
    return count() == 2 && !widget(0)->isHidden() && !widget(1)->isHidden();
}

void AnalysisSplitter::setRatio(double ratio) {
    ratio_ = qBound(kMinRatio, ratio, kMaxRatio);
    applyRatio();
}

void AnalysisSplitter::applyRatio() {
    if (!bothVisible()) return;
    const int total = qMax(2, width() - handleWidth());
    const int first = qRound(total * ratio_);
    setSizes({first, total - first});
    refreshBadge();
}

void AnalysisSplitter::resizeEvent(QResizeEvent* event) {
    QSplitter::resizeEvent(event);
    applyRatio();   // keep the split as a percentage of the width
}

void AnalysisSplitter::onMoved() {
    if (!bothVisible()) return;
    const QList<int> parts = sizes();
    const int total = parts.value(0) + parts.value(1);
    if (total <= 0) return;
    const double moved = double(parts.value(0)) / total;
    ratio_ = qBound(kMinRatio, moved, kMaxRatio);
    if (!qFuzzyCompare(ratio_ + 1.0, moved + 1.0)) applyRatio();   // held at the limit
    refreshBadge();
    emit ratioChanged(ratio_);
}

void AnalysisSplitter::resetToDefault() {
    setRatio(kDefaultRatio);
    emit ratioChanged(ratio_);
}

void AnalysisSplitter::setHovered(bool hovered) {
    hovered_ = hovered;
    refreshBadge();
}

void AnalysisSplitter::setDragging(bool dragging) {
    dragging_ = dragging;
    refreshBadge();
}

void AnalysisSplitter::refreshBadge() {
    const bool shown = (hovered_ || dragging_) && bothVisible();
    if (!shown) {
        if (badge_) badge_->hide();
        return;
    }
    QWidget* host = window();
    if (!badge_ || badge_->parentWidget() != host) {
        if (badge_) {   // re-parented window: retire the old badge, QPointer drops it
            badge_->hide();
            badge_->deleteLater();
        }
        badge_ = new AnalysisSplitBadge(host);
    }
    const int graphs = qRound(ratio_ * 100.0);
    badge_->setText(QStringLiteral("%1 / %2").arg(graphs).arg(100 - graphs));
    // Centred on the handle, just below the top of the split.
    QSplitterHandle* divider = handle(1);
    const QPoint centre = divider->mapTo(host, QPoint(divider->width() / 2, 0));
    badge_->move(centre.x() - badge_->width() / 2, mapTo(host, QPoint(0, 8)).y());
    badge_->show();
    badge_->raise();
}

// ── Handle ──────────────────────────────────────────────────────────────────

AnalysisSplitterHandle::AnalysisSplitterHandle(Qt::Orientation orientation, AnalysisSplitter* parent)
    : QSplitterHandle(orientation, parent), owner_(parent) {
    setToolTip(QStringLiteral("Drag to resize · Double-click for 50 / 50"));
}

void AnalysisSplitterHandle::enterEvent(QEnterEvent* event) {
    owner_->setHovered(true);
    QSplitterHandle::enterEvent(event);
}

void AnalysisSplitterHandle::leaveEvent(QEvent* event) {
    owner_->setHovered(false);
    QSplitterHandle::leaveEvent(event);
}

void AnalysisSplitterHandle::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) owner_->setDragging(true);
    QSplitterHandle::mousePressEvent(event);
}

void AnalysisSplitterHandle::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) owner_->setDragging(false);
    QSplitterHandle::mouseReleaseEvent(event);
}

void AnalysisSplitterHandle::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        owner_->resetToDefault();
        return;
    }
    QSplitterHandle::mouseDoubleClickEvent(event);
}
