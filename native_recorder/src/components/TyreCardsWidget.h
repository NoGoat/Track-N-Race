#pragma once

#include <QWidget>
#include <QLabel>
#include <QProgressBar>
#include <QPointer>

#include <vector>

#include <tnrp/rows.h>

class QBoxLayout;
class QGridLayout;
class QStackedWidget;
class QTimer;
class SessionModel;
class GraphTable;

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

    // Per-corner Card|Table view (Tyres page, vertical orientation, Full level only).
    // The Table view is a scrolling Time/Surface/Inner/Brake history table for that
    // one corner, so it needs the session buffer + playback window — fed through
    // these, exactly like TyreChartsWidget. No-ops for the Overview strip.
    // The mode itself is driven from the Settings "Graphs" tab via setCornerTable(),
    // mirroring every other graph's Chart/Table toggle (no per-card button).
    void setModel(SessionModel* model);
    void setPlaybackMode(bool on);
    void setCurrentTime(float t);
    void setWindowSeconds(float secs);
    void setCornerTable(int i, bool table);   // Settings-driven Card ⇄ Table swap

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

protected:
    void showEvent(QShowEvent* e) override;     // repopulate table-mode corners when shown

private:
    void updateDividers();
    void buildCards();                          // (re)build the four corners at the current level
    // Compact/UltraCompact1 8-column grid aligned to the damage row. showHeading
    // draws the corner-name title + rule row (Compact); UltraCompact1 drops it.
    void buildCompactGrid(QBoxLayout* outer, bool showHeading);
    // One line per corner (UltraCompact2/3): name + 4 values. abbrev shortens the
    // corner name; showLabels prefixes each value with its metric.
    void buildOneLine(QBoxLayout* outer, bool abbrev, bool showLabels);

    // Per-corner Table view helpers (vertical/Tyres page only).
    void  ensureCornerTable(int i);             // build corner i's GraphTable on demand
    void  requestRefresh();                     // coalesced repaint of table-mode corners
    void  refresh();                            // feed the visible-window samples into the tables
    float currentTime() const;                  // playhead (playback) or latest live sample

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

    // Per-corner Table view state (vertical/Tyres page, Full level). cornerStack_
    // swaps the card body for cornerTable_. All null on the Overview strip and at
    // compact levels. The mode is set externally (Settings), not persisted here.
    QPointer<SessionModel> model_;
    QTimer*         refreshTimer_ = nullptr;
    bool            dirty_        = false;
    bool            playback_     = false;
    float           currentTime_  = 0.0f;
    float           windowS_      = 30.0f;      // view window; matches TyreChartsWidget default
    bool            cornerTableMode_[4] = { false, false, false, false };
    GraphTable*     cornerTable_[4]     = {};
    QStackedWidget* cornerStack_[4]     = {};
};
