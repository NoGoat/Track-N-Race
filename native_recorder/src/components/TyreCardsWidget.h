#pragma once

#include <QWidget>
#include <QLabel>
#include <QProgressBar>

#include <vector>

#include <tnrp/rows.h>

class QBoxLayout;
class QGridLayout;

class TyreCardsWidget : public QWidget {
    Q_OBJECT
public:
    // Horizontal: 4 cards side by side (Overview page)
    // Vertical:   4 cards stacked in a column (Tyres page)
    explicit TyreCardsWidget(Qt::Orientation orientation = Qt::Horizontal,
                             QWidget* parent = nullptr);

    // nullptr = the row hasn't been seen yet; that section keeps its placeholder.
    void update(const TelemetryRow* telemetry, const DamageRow* damage);
    void setCornerVisible(int i, bool on);

    // Density levels for the Overview tyre cards (the Tyres page always uses Full):
    //   Full          — stacked Surface/Inner/Brake/Wear rows + wear bar (default)
    //   Compact        — corner name centred over a Surface/Inner/Brake/Wear header
    //                    + value row, aligned to the damage row, no wear bar
    //   UltraCompact1  — Compact without the corner-name heading row
    //   UltraCompact2  — one line per corner: name + the four bare values
    //   UltraCompact3  — one line per corner: short name + labelled values
    enum Level { Full = 0, Compact = 1, UltraCompact1 = 2, UltraCompact2 = 3, UltraCompact3 = 4 };

    // Live density switch (Overview page only). Rebuilds the cards at the new level;
    // the Overview page re-applies corner visibility and re-feeds update().
    void setLevel(Level level);

private:
    void updateDividers();
    void buildCards();                          // (re)build the four corners at the current level
    // Compact/UltraCompact1 8-column grid aligned to the damage row. showHeading
    // draws the corner-name title + rule row (Compact); UltraCompact1 drops it.
    void buildCompactGrid(QBoxLayout* outer, bool showHeading);
    // One line per corner (UltraCompact2/3): name + 4 values. abbrev shortens the
    // corner name; showLabels prefixes each value with its metric.
    void buildOneLine(QBoxLayout* outer, bool abbrev, bool showLabels);

    Qt::Orientation orientation_ = Qt::Horizontal;
    Level           level_       = Full;
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
