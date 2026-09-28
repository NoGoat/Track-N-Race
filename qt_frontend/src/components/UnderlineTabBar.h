#pragma once

#include <QString>
#include <QWidget>

class QButtonGroup;
class QHBoxLayout;
class QToolButton;

// Underline tabs matching the main toolbar's page switcher, as used by the
// Analysis lap modes: a row of checkable tool buttons
// in an exclusive group on a tinted bar. Every button reserves the same
// border-bottom width (transparent unless checked), so the accent underline
// only changes colour and never shifts the text.
class UnderlineTabBar : public QWidget {
    Q_OBJECT
public:
    explicit UnderlineTabBar(QWidget* parent = nullptr);

    int addTab(const QString& text);
    int count() const;
    int currentIndex() const;
    // Selects a tab without emitting tabClicked.
    void setCurrentIndex(int index);
    void setTabEnabled(int index, bool enabled);
    void setTabToolTip(int index, const QString& toolTip);
    // A hairline under the bar, for places where no bordered pane follows it.
    void setBottomBorder(bool on);
    // The bar's own tinted surface (on by default); off lets
    // whatever is behind it show through.
    void setTinted(bool on);

signals:
    // The user picked a tab.
    void tabClicked(int index);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QHBoxLayout* layout_ = nullptr;
    QButtonGroup* group_ = nullptr;
    bool bottomBorder_ = false;

    QToolButton* button(int index) const;
};
