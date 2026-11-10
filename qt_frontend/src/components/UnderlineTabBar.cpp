#include "UnderlineTabBar.h"

#include <QAbstractButton>
#include <QApplication>
#include <QButtonGroup>
#include <QEvent>
#include <QHBoxLayout>
#include <QPainter>
#include <QToolButton>

namespace {
constexpr int kUnderlineWidth = 2;

// `tenths` of the way from the window colour to the text colour.
QColor towardText(int tenths) {
    const QColor win = QApplication::palette().color(QPalette::Window);
    const QColor txt = QApplication::palette().color(QPalette::WindowText);
    return QColor((win.red() * (10 - tenths) + txt.red() * tenths) / 10,
                  (win.green() * (10 - tenths) + txt.green() * tenths) / 10,
                  (win.blue() * (10 - tenths) + txt.blue() * tenths) / 10);
}

// A clearly visible hairline in both light and dark themes (the palette's Mid
// etch is nearly identical to Window in the dark theme).
QColor hairlineColor() {
    return towardText(3);
}

QString tabStyle() {
    return QString(
        "QToolButton { padding: 6px 8px; border: none; background: transparent;"
        " border-bottom: %1px solid transparent; color: %2; }"
        "QToolButton:hover, QToolButton:checked { color: %3; }"
        "QToolButton:checked { border-bottom: %1px solid %4; }"
        "QToolButton:disabled { color: %5; }")
        .arg(kUnderlineWidth)
        .arg(towardText(7).name())
        .arg(QApplication::palette().color(QPalette::WindowText).name())
        .arg(QApplication::palette().color(QPalette::Highlight).name())
        .arg(towardText(4).name());
}
}

UnderlineTabBar::UnderlineTabBar(QWidget* parent) : QWidget(parent) {
    // Tinted a shade lighter than the window so it reads as a distinct surface.
    setBackgroundRole(QPalette::Button);
    setAutoFillBackground(true);
    layout_ = new QHBoxLayout(this);
    layout_->setContentsMargins(0, 0, 0, 0);
    layout_->setSpacing(0);
    group_ = new QButtonGroup(this);
    group_->setExclusive(true);
    connect(group_, &QButtonGroup::idClicked, this, &UnderlineTabBar::tabClicked);
}

int UnderlineTabBar::addTab(const QString& text) {
    auto* tab = new QToolButton(this);
    tab->setText(text);
    tab->setCheckable(true);
    tab->setAutoRaise(true);
    tab->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    tab->setStyleSheet(tabStyle());
    const int index = count();
    group_->addButton(tab, index);
    layout_->addWidget(tab, 1);
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

void UnderlineTabBar::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (group_ && event->type() == QEvent::PaletteChange)
        for (QAbstractButton* tab : group_->buttons()) tab->setStyleSheet(tabStyle());
}

QToolButton* UnderlineTabBar::button(int index) const {
    return qobject_cast<QToolButton*>(group_->button(index));
}
