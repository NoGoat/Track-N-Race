#pragma once

#include <QWidget>
#include <QHash>
#include <QSettings>
#include <QPointer>

#include <optional>

#include <tnrp/rows.h>

#include "OverviewLayout.h"

class QComboBox;
class QFrame;
class QLabel;
class QPushButton;
class QShowEvent;
class SessionModel;
class TelemetryChart;
class TyreCardsWidget;
class TyreChartsWidget;
class GraphTable;

// Overview tab — key-driven stat cards, telemetry chart with mode bar and
// compare-lap selector, tyre cards/charts section, and the damage rows.
// Self-contained: owns its widgets, the data cache behind the cards, and the
// layout persistence. MainWindow calls the on*() methods synchronously from
// emitLiveData for every matching row (the cards refresh per packet, not on
// the coalesced dirty-flag tick), and forwards playback state via the setters.
class OverviewPage : public QWidget {
    Q_OBJECT

public:
    explicit OverviewPage(SessionModel* model, QWidget* parent = nullptr);

    // Direct per-row updates — these replaced the old telemetryUpdated /
    // statusUpdated / damageUpdated / lapUpdated MainWindow signals.
    void onTelemetry(const TelemetryRow& row);
    void onStatus(const StatusRow& row);
    void onDamage(const DamageRow& row);
    void onLap(const LapRow& row);
    void flushPending();

    // Refresh the per-corner tyre cards from the latest telemetry + damage rows
    // (nullptr = not yet seen).
    void updateTyreCards(const TelemetryRow* telemetry, const DamageRow* damage);

    // Re-label the stat-card titles from the i18n catalog. Called on format
    // change so the wing card flips DRS ↔ SLM with the active game year.
    void refreshTitles();

    // Playback plumbing — chart, tyre charts, compare/default buttons, lap combo.
    void setPlaybackMode(bool on, float currentTime = 0.0f);
    void setCurrentTime(float t);
    void setWindowSeconds(float secs);

    // Per-graph Chart/Table toggles (driven by the Settings "Graphs" tab via
    // MainWindow). The tyre strip shares the Tyres page's tyre-graph settings.
    void setTelemetryTable(bool table);              // Speed / RPM / ERS chart
    void setTyreGraphTable(int section, bool table); // tyre strip (surf/inner/brake/wear)
    void setCardTable(int corner, bool table);       // per-corner tyre card (FL/FR/RL/RR)

    // "Edit Layout" / Settings dialogs read/write through these (immediate-apply).
    OverviewLayout loadLayout();
    void applyAndSaveLayout(const OverviewLayout& layout);
    OverviewLayout::TyreView currentTyreView();
    void setTyreView(OverviewLayout::TyreView v);
    bool tyreGraphLifeMode() const { return settings_.value("ui/tyreWearMode", "life").toString() != "wear"; }
    void setTyreGraphLifeMode(bool life);

    // Per-section compact density (each rebuilds its row/cards in place). The
    // Settings dialog drives these independently via MainWindow.
    void setStatsCompact(bool on);
    void setDamageCompact(bool on);
    // Tyre cards have four density levels (see TyreCardsWidget::Level): 0 Full,
    // 1 Compact, 2 Ultra Compact 1, 3 Ultra Compact 2.
    void setTyresLevel(int level);

protected:
    void showEvent(QShowEvent* event) override;

private:
    void refreshCards();   // recompute value + colour for every built card
    void refreshDamage();
    void refreshTelemetryTable();   // repopulate the Speed/RPM/ERS raw-values table
    void buildStatCards();     // (re)populate the stats row with cards at the current density
    void buildDamageCards();   // (re)populate both damage rows at the current density
    void saveLayout(const OverviewLayout& layout);
    void applyLayout(const OverviewLayout& layout);

    // Value / sub / title QLabels keyed by card key (speed, rpm, …, drs, …).
    // A card is a { key, label } pair: the title comes from the i18n catalog,
    // the value from a per-key resolver over the latest data cache below.
    QHash<QString, QLabel*> cardValue_;
    QHash<QString, QLabel*> cardSub_;
    QHash<QString, QLabel*> cardTitle_;
    // Latest values the card resolvers read; updated per row by the on*()
    // methods, then refreshCards() recomputes every visible card.
    struct OvCache {
        float speed = 0; int rpm = 0; int gear = 0; float throttle = 0; float brake = 0;
        bool drs = false; bool slm = false; int engineTemp = 0;
        float ersPct = 0; int ersMode = -1; bool ersFault = false; bool drsFault = false;
        float fuelKg = 0; float fuelLaps = 0;
        int tyreCompound = -1; int visualCompound = -1; int tyreAgeLaps = 0; int fuelMix = -1;
        int pos = 0; int lapNum = 0;
    } cache_;
    std::optional<DamageRow> lastDamage_;   // last damage row, replayed after a compact rebuild
    bool cardsDirty_ = true;
    bool damageDirty_ = false;
    bool statsCompact_  = false;  // per-section compact density (ui/compact/overview*)
    bool damageCompact_ = false;
    int  tyresLevel_    = 0;       // TyreCardsWidget::Level (0 Full … 3 Ultra Compact 2)

    TelemetryChart* chart_       = nullptr;
    GraphTable*     telemetryTable_ = nullptr;   // shares chart_'s grid cell (toggled)
    bool            telemetryTableMode_ = false;
    QPointer<SessionModel> model_;            // for the telemetry table's raw values
    float           windowS_     = 30.0f;     // chart window (toolbar default 30s)
    bool            playback_    = false;
    float           currentTime_ = 0.0f;
    QComboBox*      lapCombo_    = nullptr;   // compare-lap selector
    QPushButton*    compareBtn_  = nullptr;   // enabled only while a file is loaded
    QPushButton*    defaultBtn_  = nullptr;   // re-selected when a recording closes
    QWidget*        modeBar_     = nullptr;   // chart-mode row; hidden together with the chart
    QFrame*         statsFrame_  = nullptr;   // stats row container; hidden if all stat cards are hidden
    QFrame*         sep1_        = nullptr;   // separator below the stats row
    QFrame*         sep2_        = nullptr;   // separator above the damage rows
    QFrame*         dmgFrame_    = nullptr;   // damage rows container; hidden if both rows are hidden
    QFrame*         dmgRowA_     = nullptr;   // tyre/brake damage row
    QFrame*         dmgRowB_     = nullptr;   // wing/body damage row
    QFrame*         dmgHdiv_     = nullptr;   // separator between the two damage rows
    // Tyre section
    QFrame*           tyreSep_    = nullptr;
    TyreCardsWidget*  tyreCards_  = nullptr;
    TyreChartsWidget* tyreCharts_ = nullptr;
    QFrame*         statCardFrame_[OverviewLayout::StatCardCount] = {};
    // Separator preceding each stat card (index 0 has none). Tracked so a hidden
    // card's flanking separators can be hidden too — otherwise they stack into a
    // double line between the surrounding visible cards.
    QFrame*         statCardSep_[OverviewLayout::StatCardCount]   = {};
    QFrame*         dmgCardFrame_[OverviewLayout::DmgCardCount]   = {};
    // Separator preceding each damage card (first card of each row has none), so a
    // hidden card's separator is hidden too instead of stacking into a cluster.
    QFrame*         dmgCardSep_[OverviewLayout::DmgCardCount]     = {};
    QLabel*         dmgTyreFl   = nullptr; QLabel* dmgTyreFr  = nullptr;
    QLabel*         dmgTyreRl   = nullptr; QLabel* dmgTyreRr  = nullptr;
    QLabel*         dmgBrakeFl  = nullptr; QLabel* dmgBrakeFr = nullptr;
    QLabel*         dmgBrakeRl  = nullptr; QLabel* dmgBrakeRr = nullptr;
    QLabel*         dmgWingFl   = nullptr; QLabel* dmgWingFr  = nullptr;
    QLabel*         dmgWingRear = nullptr; QLabel* dmgFloor   = nullptr;
    QLabel*         dmgSidepod  = nullptr; QLabel* dmgDiffuser = nullptr;
    QLabel*         dmgGearbox  = nullptr; QLabel* dmgEngine   = nullptr;

    QSettings settings_{ "TrackNRace", "NativeRecorder" };
};
