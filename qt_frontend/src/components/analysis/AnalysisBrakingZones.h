#pragma once

#include <QByteArray>
#include <QColor>
#include <QString>
#include <QWidget>

#include <memory>

namespace tnrp { struct AnalysisMapResult; }

class AnalysisGripSplitter;
class QLabel;
class QScrollArea;
class QTreeWidget;

// The Braking Zones panel: every braking zone of the laps on show (libtnrp's
// braking points, tnrp/AnalysisMap.h) with where it starts and the time gap
// through it, and the selected zone's head-to-head below. Selecting a zone
// puts the map and graph cursors at its braking point.
class AnalysisBrakingZones : public QWidget {
    Q_OBJECT
public:
    explicit AnalysisBrakingZones(QWidget* parent = nullptr);

    // Null while no result has arrived or there are no laps to compare.
    void setResult(std::shared_ptr<const tnrp::AnalysisMapResult> result);
    void setLaps(const QString& currentLabel, const QColor& currentColor,
                 const QString& comparisonLabel, const QColor& comparisonColor);
    // The list | detail split, for the page's settings. Empty or unreadable
    // state keeps the default 60 / 40.
    QByteArray splitState() const;
    void restoreSplitState(const QByteArray& state);

signals:
    // Lap distance (m) of the selected zone's braking point.
    void zoneActivated(double distanceM);
    // The user moved the list | detail split.
    void splitChanged();

protected:
    void changeEvent(QEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    QLabel* summary_ = nullptr;
    AnalysisGripSplitter* sections_ = nullptr;
    bool splitApplied_ = false;   // a saved split, or the default once shown
    QTreeWidget* list_ = nullptr;
    QScrollArea* detailScroll_ = nullptr;
    QLabel* detail_ = nullptr;
    std::shared_ptr<const tnrp::AnalysisMapResult> result_;
    QString currentLabel_;
    QString comparisonLabel_;
    QColor currentColor_;
    QColor comparisonColor_;

    void rebuild();
    void showDetail();
};
