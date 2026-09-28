#pragma once

#include <QSettings>
#include <QWidget>
#include <array>
#include <optional>
#include <vector>
#include <tnrp/Strategy.h>
#include "../CompactSettings.h"

class QFrame;
class QLabel;
class QProgressBar;
class QScrollArea;
class QTableWidget;
class QVBoxLayout;
class QColor;
class QSpinBox;

// Presentation-only strategy page. All state and arithmetic live in libtnrp;
// this widget owns only density, palette, layout, scrolling and formatting.
class StrategyPage : public QWidget {
    Q_OBJECT
public:
    explicit StrategyPage(QWidget* parent = nullptr);
    // rebuilding: a playback seek is waiting for Strategy's rebuilt snapshot.
    void update(const tnrp::StrategySnapshotRow* snapshot, bool rebuilding = false);
    void resetForNewSession();
    void setCompactMode(bool on);
    void setDensityMode(tnr::DensityMode mode);

signals:
    void minimumStopsChanged(int stops);

protected:
    void changeEvent(QEvent* event) override;

private:
    struct StintView {
        QFrame* rule = nullptr;
        QWidget* card = nullptr;
        QLabel* title = nullptr;
        QLabel* counts = nullptr;
        QTableWidget* table = nullptr;
    };
    struct PlanView {
        QWidget* frame = nullptr;
        QLabel* heading = nullptr;
        QScrollArea* scroll = nullptr;
        QVBoxLayout* stints = nullptr;   // stint cards, then a trailing stretch
        std::vector<StintView> items;
    };
    struct CornerView {
        QLabel* value = nullptr;
        QProgressBar* bar = nullptr;
    };

    // rebuild() creates the widget tree (construction, density and palette
    // changes only); refresh() writes snapshot_ into the existing widgets so
    // live updates never replace the page or reset its scroll positions.
    void rebuild();
    void refresh();
    QWidget* buildHeader();
    QWidget* buildBody();
    QWidget* buildPlan(PlanView& view);
    QWidget* buildSidebar();
    void refreshHeader(const tnrp::StrategySnapshotRow* s);
    void refreshPlan(PlanView& view, const QString& title, const tnrp::StrategyPlan& plan, const QColor& accent);
    void refreshSidebar(const tnrp::StrategySnapshotRow* s);

    QVBoxLayout* pageLayout_ = nullptr;
    QWidget* content_ = nullptr;
    // Header
    QLabel* lapValue_ = nullptr;
    QLabel* lapDistance_ = nullptr;      // spacious only
    QLabel* compoundChip_ = nullptr;
    QLabel* wearValue_ = nullptr;
    QLabel* wearDetail_ = nullptr;
    QProgressBar* wearBar_ = nullptr;
    QLabel* limitingTyre_ = nullptr;     // spacious only
    QLabel* cliffValue_ = nullptr;
    QLabel* cliffRemaining_ = nullptr;   // spacious only
    // Body
    QLabel* nonRace_ = nullptr;
    QWidget* body_ = nullptr;
    QWidget* pending_ = nullptr;
    QFrame* planRule_ = nullptr;
    PlanView defensive_;
    PlanView attacking_;
    // Sidebar
    QScrollArea* sidebarScroll_ = nullptr;
    QWidget* neutralisationSection_ = nullptr;
    QLabel* neutralisation_ = nullptr;
    QWidget* callSection_ = nullptr;
    QLabel* call_ = nullptr;
    QLabel* pitWindow_ = nullptr;
    QWidget* weatherSection_ = nullptr;
    QLabel* weather_ = nullptr;
    QWidget* rivalsSection_ = nullptr;
    QVBoxLayout* rivalsList_ = nullptr;
    std::vector<QLabel*> rivals_;
    QLabel* tyreCondition_ = nullptr;
    std::array<CornerView, 4> corners_{};
    QWidget* alertsSection_ = nullptr;
    QVBoxLayout* alertsList_ = nullptr;
    std::vector<QLabel*> alerts_;

    std::optional<tnrp::StrategySnapshotRow> snapshot_;
    // The displayed snapshot as JSON without its clock fields, so snapshots
    // that differ only in session_time/data_age_s skip the refresh.
    std::string renderedKey_;
    bool rebuilding_ = false;
    tnr::DensityMode density_ = tnr::DensityMode::Normal;
    int minimumStops_ = 1;
    QWidget* minimumStopsControl_ = nullptr;
    QSpinBox* minimumStopsInput_ = nullptr;
    bool restoreMinimumStopsFocus_ = false;
    QSettings settings_{"TrackNRace", "NativeRecorder"};
};
