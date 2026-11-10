#pragma once

// Small widget helpers shared by first-time setup and the connection check.

#include "../IconUtils.h"
#include "PageUiHelpers.h"

#include <QFont>
#include <QIcon>
#include <QLabel>
#include <QLayout>
#include <QMetaObject>
#include <QPointer>
#include <QStyle>
#include <QThreadPool>
#include <QVBoxLayout>
#include <QWidget>

#include <functional>
#include <utility>

namespace tnrsetup {

inline QFont scaledFont(const QFont& base, double scale, bool bold = false) {
    QFont font = base;
    font.setPointSizeF(qMax(7.0, base.pointSizeF() * scale));
    font.setBold(bold);
    return font;
}

inline QLabel* muted(const QString& text, double scale = 0.92) {
    auto* label = new QLabel(text);
    label->setWordWrap(true);
    label->setForegroundRole(QPalette::PlaceholderText);
    label->setFont(scaledFont(label->font(), scale));
    return label;
}

// Theme icon tinted to the text colour: the bundled Breeze icons are
// monochrome and plain Qt draws them black (see adaptThemeIcon).
inline QIcon themedIcon(const QWidget* widget, const char* name, QStyle::StandardPixmap fallback) {
    return adaptThemeIcon(QIcon::fromTheme(QString::fromLatin1(name)),
                          widget->palette().color(QPalette::WindowText),
                          widget->style()->standardIcon(fallback));
}

// Theme icon in a fixed colour (a check row's status colour). Only the bundled
// Breeze icons need it; other themes' status icons already carry their colours.
inline QIcon tintedIcon(const QWidget* widget, const char* name, QStyle::StandardPixmap fallback,
                 const QColor& tint) {
    const QIcon themed = QIcon::fromTheme(QString::fromLatin1(name));
    if (themed.isNull()) return widget->style()->standardIcon(fallback);
#if defined(Q_OS_WIN) || defined(HAVE_BREEZE_ICONS)
    if (QIcon::themeName() == QLatin1String("breeze"))
        return QIcon(new TintedIconEngine(themed, tint));
#endif
    Q_UNUSED(tint);
    return themed;
}

inline void clearLayout(QLayout* layout) {
    while (QLayoutItem* item = layout->takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        else if (QLayout* child = item->layout()) clearLayout(child);
        delete item;
    }
}

// Rows separated by thin lines, like Electron's border-t lists.
inline void addSeparated(QVBoxLayout* layout, QWidget* row) {
    if (layout->count() > 0) layout->addWidget(tnrui::hline());
    layout->addWidget(row);
}

// Runs `work` on the thread pool and hands its result to `done` on the GUI
// thread, unless `context` is gone by then.
template <typename Result>
inline void runAsync(QObject* context, std::function<Result()> work, std::function<void(Result)> done) {
    QPointer<QObject> guard(context);
    QThreadPool::globalInstance()->start([guard, work = std::move(work), done = std::move(done)] {
        Result result = work();
        if (!guard) return;
        QMetaObject::invokeMethod(guard.data(), [guard, done, result = std::move(result)] {
            if (guard) done(result);
        }, Qt::QueuedConnection);
    });
}

} // namespace tnrsetup
