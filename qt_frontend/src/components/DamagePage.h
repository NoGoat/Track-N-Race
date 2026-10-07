#pragma once

#include <QSettings>
#include <QWidget>

#include <optional>

#include <tnrp/rows.h>

#include "DamageLayout.h"
#include "../CompactSettings.h"

class CarDamageDiagram;
class QFrame;
class SummaryCard;

// Damage tab (Electron DamagePage.tsx): the streamed driver's car damage and
// power-unit wear — the player live, the driver selector's car in V6
// playback. A status strip, the car wireframe with a callout per part, and
// one row of wear tiles, all from the latest damage row; it draws no history.
// MainWindow feeds the row via update() and the density via setDensityMode().
class DamagePage : public QWidget {
    Q_OBJECT

public:
    explicit DamagePage(QWidget* parent = nullptr);

    // Latest damage row (nullptr = none yet). Missing V6 fields carry
    // kPlaybackMissingInt and show as unavailable, never as 0.
    void update(const DamageRow* damage);
    // Re-resolve the format-dependent wing-fault title (DRS / Rear Wing).
    void refreshTitles();

    // "Edit Layout" dialog reads/writes through these (immediate-apply).
    DamageLayout loadLayout();
    void applyAndSaveLayout(const DamageLayout& layout);
    void setDensityMode(tnr::DensityMode mode);

protected:
    void changeEvent(QEvent* event) override;

private:
    void buildCards();   // (re)populate both card rows at the current density
    void saveLayout(const DamageLayout& layout);
    void applyLayout(const DamageLayout& layout);
    void refresh();

    tnr::DensityMode density_ = tnr::DensityMode::Normal;
    std::optional<DamageRow> damage_;

    QWidget* statusBar_ = nullptr;
    QFrame* statusDivider_ = nullptr;   // below the status strip
    SummaryCard* statusCards_[DamageLayout::StatusCount] = {};
    QFrame* statusSeps_[DamageLayout::StatusCount] = {};   // separator before each card

    CarDamageDiagram* diagram_ = nullptr;
    QWidget* diagramSpacer_ = nullptr;   // keeps the wear row at the bottom while the diagram is hidden

    QWidget* wearBar_ = nullptr;
    QFrame* wearDivider_ = nullptr;     // above the wear row
    SummaryCard* wearCards_[DamageLayout::WearCount] = {};
    QFrame* wearSeps_[DamageLayout::WearCount] = {};

    QSettings settings_{ "TrackNRace", "NativeRecorder" };
};
