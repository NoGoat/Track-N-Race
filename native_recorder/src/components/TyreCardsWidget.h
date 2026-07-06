#pragma once

#include <QWidget>
#include <QLabel>
#include <QProgressBar>

#include <vector>

#include <nlohmann/json.hpp>

class QBoxLayout;
class QGridLayout;

class TyreCardsWidget : public QWidget {
    Q_OBJECT
public:
    // Horizontal: 4 cards side by side (Overview page)
    // Vertical:   4 cards stacked in a column (Tyres page)
    explicit TyreCardsWidget(Qt::Orientation orientation = Qt::Horizontal,
                             QWidget* parent = nullptr);

    void update(const nlohmann::json& telemetry, const nlohmann::json& damage);
    void setCornerVisible(int i, bool on);

    // Compact single-line-ish redesign — corner name centred over a Surface/Inner/
    // Brake/Wear header + value row, no wear bar. Only the Overview page enables it;
    // the Tyres page keeps the full stacked layout.
    void setCompactMode(bool on);

private:
    void updateDividers();
    void buildCards();                       // (re)build the four corners at the current density
    void buildCompactGrid(QBoxLayout* outer);   // compact 8-column grid aligned to the damage row

    Qt::Orientation orientation_ = Qt::Horizontal;
    bool            compact_     = false;
    bool            cornerVisible_[4] = { true, true, true, true };   // logical (not realized) visibility
    std::vector<QWidget*> compactCorner_[4];   // per-corner grid widgets, toggled by setCornerVisible
    QGridLayout*  compactGrid_ = nullptr;      // the compact grid, so hidden corners' columns can collapse
    QWidget*      cards_[4]     = {};
    QFrame*       dividers_[3]  = {};
    QLabel*       surfaceTemp_[4] = {};
    QLabel*       innerTemp_[4]   = {};
    QLabel*       brakeTemp_[4]   = {};
    QLabel*       wearLabel_[4]   = {};
    QProgressBar* wear_[4]        = {};
    QLabel*       blisters_[4]    = {};
};
