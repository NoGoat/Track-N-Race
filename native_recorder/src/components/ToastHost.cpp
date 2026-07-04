#include "ToastHost.h"
#include "Toast.h"
#include "ToastEvents.h"

#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QWidget>

ToastHost::ToastHost(QWidget* container)
    : QObject(container), container_(container)
{
    // Event toasts: top-right of the screen, stacked, max a few at once (rest queue).
    // These are process-wide statics on the vendored Toast class, set once.
    Toast::setPosition(ToastPosition::TOP_RIGHT);
    Toast::setMaximumOnScreen(4);
    Toast::setSpacing(10);
    Toast::setOffset(20, 20);
}

void ToastHost::dismissPersistent() {
    if (persistent_) {
        persistent_->hide();
        persistent_ = nullptr;
    }
}

void ToastHost::show(const ToastSpec& spec) {
    if (!settings_.value("ui/toastsEnabled", true).toBool()) return;

    // Evict the current persistent toast (SC/VSC/FL) when the incoming event
    // takes over that slot — either by replacing it with a new persistent toast,
    // or by an ending event (Track Clear, Red Flag) that occupies then auto-dismisses.
    if ((spec.persistent || spec.dismissesPersistent) && persistent_) {
        persistent_->hide();
        persistent_ = nullptr;
    }

    // ToolTipBase reads as a raised card distinct from the page background; the
    // per-event accent drives the title, secondary text for the sub line.
    const QColor bg  = container_->palette().color(QPalette::ToolTipBase);
    const QColor sub = container_->palette().color(QPalette::PlaceholderText);

    // Parented to the central content widget so the toast renders inline (a child
    // overlay), not as its own window — required for correct positioning on Wayland.
    Toast* t = new Toast(container_);
    t->setShowIcon(false);
    t->setShowIconSeparator(false);
    t->setShowCloseButton(spec.persistent); // persistent toasts need manual dismissal
    t->setShowDurationBar(false);   // no countdown bar
    t->setFixedWidth(250);          // uniform width across all toasts (long text wraps)
    t->setBorderRadius(3);
    t->setDuration(spec.persistent ? 0 : settings_.value("ui/bannerDuration", 3).toInt() * 1000);
    if (spec.persistent) {
        // The vendor's icon assets aren't bundled, so draw the × inline.
        // recolorImage() preserves alpha, so white-on-transparent gets tinted correctly.
        QPixmap closePix(10, 10);
        closePix.fill(Qt::transparent);
        QPainter painter(&closePix);
        painter.setPen(QPen(Qt::white, 1.5, Qt::SolidLine, Qt::RoundCap));
        painter.setRenderHint(QPainter::Antialiasing);
        painter.drawLine(2, 2, 8, 8);
        painter.drawLine(8, 2, 2, 8);
        painter.end();
        t->setCloseButtonIcon(closePix);
        t->setCloseButtonIconColor(sub);

        persistent_ = t;
        connect(t, &Toast::closed, this, [this, t]() {
            if (persistent_ == t) persistent_ = nullptr;
        });
    }
    t->setBackgroundColor(bg);
    t->setTitle(spec.label);
    t->setTitleColor(spec.color);
    if (!spec.sub.isEmpty()) {
        t->setText(spec.sub);
        t->setTextColor(sub);
    }
    t->show();
}
