#pragma once

#include <QSettings>
#include <QWidget>
#include <optional>
#include <tnrp/Strategy.h>

class QScrollArea;
class QVBoxLayout;
class QColor;

// Presentation-only strategy page. All state and arithmetic live in libtnrp;
// this widget owns only density, palette, layout, scrolling and formatting.
class StrategyPage : public QWidget {
    Q_OBJECT
public:
    explicit StrategyPage(QWidget* parent = nullptr);
    void update(const tnrp::StrategySnapshotRow* snapshot);
    void resetForNewSession();
    void setCompactMode(bool on);

private:
    void rebuild();
    QWidget* makePlan(const QString& title, const tnrp::StrategyPlan& plan,
                      const QColor& accent, QScrollArea** scroll);
    QWidget* makeSidebar();

    QVBoxLayout* root_ = nullptr;
    QScrollArea* conservativeScroll_ = nullptr;
    QScrollArea* aggressiveScroll_ = nullptr;
    QScrollArea* sidebarScroll_ = nullptr;
    std::optional<tnrp::StrategySnapshotRow> snapshot_;
    bool compact_ = false;
    QSettings settings_{"TrackNRace", "NativeRecorder"};
};
