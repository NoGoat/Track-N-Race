#pragma once

#include <QSettings>
#include <QWidget>
#include <optional>
#include <tnrp/Strategy.h>
#include "../CompactSettings.h"

class QScrollArea;
class QVBoxLayout;
class QColor;
class QSpinBox;

// Presentation-only strategy page. All state and arithmetic live in libtnrp;
// this widget owns only density, palette, layout, scrolling and formatting.
class StrategyPage : public QWidget {
    Q_OBJECT
public:
    explicit StrategyPage(QWidget* parent = nullptr);
    void update(const tnrp::StrategySnapshotRow* snapshot);
    void resetForNewSession();
    void setCompactMode(bool on);
    void setDensityMode(tnr::DensityMode mode);

signals:
    void minimumStopsChanged(int stops);

protected:
    void changeEvent(QEvent* event) override;

private:
    void rebuild();
    QWidget* makeHeader();
    QWidget* makePlan(const QString& title, const tnrp::StrategyPlan& plan,
                      const QColor& accent, QScrollArea** scroll);
    QWidget* makeSidebar();
    QWidget* makeWaitingSidebar();
    QWidget* withMinimumStops(QWidget* content);

    QVBoxLayout* root_ = nullptr;
    QScrollArea* conservativeScroll_ = nullptr;
    QScrollArea* aggressiveScroll_ = nullptr;
    QScrollArea* sidebarScroll_ = nullptr;
    std::optional<tnrp::StrategySnapshotRow> snapshot_;
    tnr::DensityMode density_ = tnr::DensityMode::Normal;
    int minimumStops_ = 1;
    QWidget* minimumStopsControl_ = nullptr;
    QSpinBox* minimumStopsInput_ = nullptr;
    bool restoreMinimumStopsFocus_ = false;
    QSettings settings_{"TrackNRace", "NativeRecorder"};
};
