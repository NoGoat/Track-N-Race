#pragma once

#include <QApplication>
#include <QButtonGroup>
#include <QHBoxLayout>
#include <QSize>
#include <QStyle>
#include <QStyleOptionButton>
#include <QStylePainter>
#include <QToolButton>
#include <QWidget>

#include <initializer_list>
#include <utility>

namespace tnrui {

// Horizontal padding per side for toggle buttons under the platform's default
// style, whose tool buttons hug their label. 0 for any other style; Breeze sizes
// them well itself, even when it is the platform default.
inline int systemStyleTogglePadding() {
    const QString name = QApplication::style()->objectName();
    const bool platformDefault =
        name.compare(qApp->property("defaultStyleName").toString(), Qt::CaseInsensitive) == 0;
    return platformDefault && name.compare(QLatin1String("breeze"), Qt::CaseInsensitive) != 0 ? 8 : 0;
}

inline QSize withSystemStyleTogglePadding(QSize hint) {
    const int padding = systemStyleTogglePadding();
    return padding > 0 ? hint + QSize(padding * 2, 4) : hint;
}

// Segmented-control button, used by the toolbar's toggles and the segmented
// rows below. When checked it paints itself as the active
// style's *default button* — the same blue outline the on-toggles wear — which a
// QToolButton can't get for free: the DefaultButton look lives on
// QStyleOptionButton, which only QPushButton feeds the style, so for the checked
// state we draw a default QPushButton bevel + label ourselves. Unchecked
// segments fall through to the normal flat auto-raised look.
class SegmentButton : public QToolButton {
public:
    using QToolButton::QToolButton;

    QSize sizeHint() const override { return withSystemStyleTogglePadding(QToolButton::sizeHint()); }

protected:
    void paintEvent(QPaintEvent* e) override {
        if (!isChecked()) { QToolButton::paintEvent(e); return; }
        QStylePainter p(this);
        QStyleOptionButton opt;
        opt.initFrom(this);
        opt.rect = rect();
        opt.text = text();
        opt.features = QStyleOptionButton::DefaultButton;
        opt.state |= QStyle::State_Raised;
        opt.state &= ~(QStyle::State_On | QStyle::State_Sunken);
        p.drawControl(QStyle::CE_PushButton, opt);
    }
};

// An exclusive row of SegmentButtons; ids are the given values.
inline QWidget* segmented(QButtonGroup*& groupOut,
                          std::initializer_list<std::pair<const char*, int>> options) {
    auto* row = new QWidget;
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* group = new QButtonGroup(row);
    group->setExclusive(true);
    for (const auto& option : options) {
        auto* button = new SegmentButton(row);
        button->setText(QString::fromUtf8(option.first));
        button->setCheckable(true);
        button->setAutoRaise(true);
        group->addButton(button, option.second);
        layout->addWidget(button);
    }
    groupOut = group;
    return row;
}

} // namespace tnrui
