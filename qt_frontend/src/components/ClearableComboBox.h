#pragma once

#include "../IconUtils.h"

#include <QComboBox>
#include <QResizeEvent>
#include <QStyle>
#include <QToolButton>

// A combo box with an optional clear action inset between its text and native
// dropdown arrow. Keep this shared by every clearable selector so the action
// does not consume separate layout space beside the field.
class ClearableComboBox : public QComboBox {
public:
    explicit ClearableComboBox(QWidget* parent = nullptr) : QComboBox(parent) {
        clearButton_ = new QToolButton(this);
        clearButton_->setIcon(adaptThemeIcon(
            QIcon::fromTheme("window-close"),
            palette().color(QPalette::WindowText),
            style()->standardIcon(QStyle::SP_DialogCloseButton)));
        clearButton_->setCursor(Qt::PointingHandCursor);
        clearButton_->setFocusPolicy(Qt::NoFocus);
        clearButton_->setToolTip("Clear selection");
        clearButton_->setStyleSheet(
            "QToolButton {"
            "  border: none; background: transparent; font-weight: bold; font-size: 16px;"
            "  color: #888; padding: 0px; margin: 0px;"
            "}"
            "QToolButton:hover { color: palette(text); }");
        connect(clearButton_, &QToolButton::clicked, this, [this] {
            setCurrentIndex(0);
        });
        clearButton_->hide();
    }

    void setClearVisible(bool visible) {
        clearButton_->setVisible(visible);
    }

protected:
    void resizeEvent(QResizeEvent* event) override {
        QComboBox::resizeEvent(event);
        constexpr int arrowWidth = 24;
        constexpr int buttonSize = 18;
        clearButton_->setGeometry(
            width() - arrowWidth - buttonSize,
            (height() - buttonSize) / 2,
            buttonSize,
            buttonSize);
    }

private:
    QToolButton* clearButton_ = nullptr;
};
