#include "UnderlineTabBar.h"

#include <QApplication>
#include <QButtonGroup>
#include <QHBoxLayout>
#include <QPainter>
#include <QToolButton>

namespace {
constexpr int kUnderlineWidth = 2;

// ~30% of the way from the window colour to the text colour: a clearly visible
// hairline in both light and dark themes (the palette's Mid etch is nearly
// identical to Window in the dark theme).
QColor hairlineColor() {
    const QColor win = QApplication::palette().color(QPalette::Window);
    const QColor txt = QApplication::palette().color(QPalette::WindowText);
    return QColor((win.red() * 7 + txt.red() * 3) / 10,
                  (win.green() * 7 + txt.green() * 3) / 10,
                  (win.blue() * 7 + txt.blue() * 3) / 10);
}
}

UnderlineTabBar::UnderlineTabBar(QWidget* parent) : QWidget(parent) {
    // Tinted a shade lighter than the window so it reads as a distinct surface.
    setBackgroundRole(QPalette::Button);
    setAutoFillBackground(true);
    layout_ = new QHBoxLayout(this);
    layout_->setContentsMargins(8, 0, 8, 0);
    layout_->setSpacing(4);
    layout_->addStretch(1);
    group_ = new QButtonGroup(this);
    group_->setExclusive(true);
    connect(group_, &QButtonGroup::idClicked, this, &UnderlineTabBar::tabClicked);
}

int UnderlineTabBar::addTab(const QString& text) {
    const QString accent = QApplication::palette().color(QPalette::Highlight).name();
    auto* tab = new QToolButton(this);
    tab->setText(text);
    tab->setCheckable(true);
    tab->setAutoRaise(true);
    tab->setStyleSheet(QString(
        "QToolButton { padding: 8px 14px; border: none; background: transparent;"
        " border-bottom: %1px solid transparent; }"
        "QToolButton:checked { border-bottom: %1px solid %2; }")
        .arg(kUnderlineWidth).arg(accent));
    const int index = count();
    group_->addButton(tab, index);
    layout_->insertWidget(index, tab);   // before the trailing stretch
    if (index == 0) tab->setChecked(true);
    return index;
}

int UnderlineTabBar::count() const {
    return static_cast<int>(group_->buttons().size());
}

int UnderlineTabBar::currentIndex() const {
    return group_->checkedId();
}

void UnderlineTabBar::setCurrentIndex(int index) {
    if (QToolButton* tab = button(index)) tab->setChecked(true);
}

void UnderlineTabBar::setTabEnabled(int index, bool enabled) {
    if (QToolButton* tab = button(index)) tab->setEnabled(enabled);
}

void UnderlineTabBar::setTabToolTip(int index, const QString& toolTip) {
    if (QToolButton* tab = button(index)) tab->setToolTip(toolTip);
}

void UnderlineTabBar::setBottomBorder(bool on) {
    if (bottomBorder_ == on) return;
    bottomBorder_ = on;
    setContentsMargins(0, 0, 0, on ? 1 : 0);
    update();
}

void UnderlineTabBar::setTinted(bool on) {
    setAutoFillBackground(on);
    update();
}

void UnderlineTabBar::paintEvent(QPaintEvent* event) {
    QWidget::paintEvent(event);
    if (!bottomBorder_) return;
    QPainter painter(this);
    painter.fillRect(QRect(0, height() - 1, width(), 1), hairlineColor());
}

QToolButton* UnderlineTabBar::button(int index) const {
    return qobject_cast<QToolButton*>(group_->button(index));
}
