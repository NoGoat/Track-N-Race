#pragma once

#include "AnalysisRecording.h"

#include <QColor>
#include <QString>
#include <QVector>
#include <QWidget>

class AnalysisLapSummary;
class QComboBox;
class QLineEdit;
class QToolButton;

struct AnalysisDriverChoice {
    AnalysisDriverRef ref;
    QString name;
};

struct AnalysisLapChoice {
    int lapNum = -1;
    int lapTimeMs = 0;
    QString compound;
    QColor compoundColor;
    bool fastest = false;
};

// One side of a comparison, as one collapsible section:
//
//   v [colour]  NoGoat · C3 · Lap 58 · 1:12.148
//     Driver  [ NoGoat                 v ]
//     Lap     [ C3 · Lap 58 · 1:12.148 v ]
//     Name    [ Lap A                    ]
//
// The header's swatch picks the lap's colour; the summary stays readable while
// the fields are collapsed. A "following" slot mirrors the playback cursor: its
// Driver and Lap are read only and only the name and colour are editable.
class AnalysisLapSlot : public QWidget {
    Q_OBJECT
public:
    enum class Mode { Selectable, FollowsPlayback };

    // `title` names the slot in tooltips and dialogs.
    AnalysisLapSlot(const QString& title, const QString& defaultLabel, Mode mode,
                    QWidget* parent = nullptr);

    bool isExpanded() const { return expanded_; }
    void setExpanded(bool expanded);

    // Selectable slots. Keeps the current driver when still offered, otherwise
    // selects `fallback`. Headers separate the recordings when both are open.
    void setDriverChoices(const QVector<AnalysisDriverChoice>& primary,
                          const QVector<AnalysisDriverChoice>& secondary,
                          const AnalysisDriverRef& fallback);
    AnalysisDriverRef driver() const;
    void setDriverSelectable(bool selectable);
    // Keeps the current lap when still offered, otherwise selects none.
    void setLapChoices(const QVector<AnalysisLapChoice>& laps);
    void setLapSelectable(bool selectable);
    int lap() const;
    void clearLap();
    // Programmatic picks (screenshot tour); false when not offered. Selecting
    // a driver does not emit driverActivated, so the owner refreshes laps.
    bool selectDriver(const AnalysisDriverRef& ref);
    bool selectLap(int lapNum);
    // Drops both driver and lap, e.g. when their recording is closed.
    void resetSelection();
    AnalysisLapRef selection() const { return {driver(), lap()}; }

    // Following slots.
    void setFollowed(const QString& driverName, const AnalysisLapChoice& lap,
                     const QString& runningTime = {});

    QString label() const;           // as typed; may be empty
    QString resolvedLabel() const;   // falls back to the default label
    void setLabel(const QString& label);
    QColor color() const { return color_; }
    void setColor(const QColor& color);

signals:
    void driverActivated();
    void lapActivated();
    void labelEdited(const QString& label);
    void colorPicked(const QColor& color);
    // The user opened or closed this section.
    void expandedChanged();

private:
    Mode mode_;
    QString title_;
    QString defaultLabel_;
    QColor color_;
    bool expanded_ = true;
    QString followedDriver_;
    QWidget* header_ = nullptr;
    QToolButton* arrow_ = nullptr;
    QToolButton* swatch_ = nullptr;
    AnalysisLapSummary* summary_ = nullptr;
    QWidget* body_ = nullptr;
    QComboBox* driverBox_ = nullptr;
    QComboBox* lapBox_ = nullptr;
    QLineEdit* labelEdit_ = nullptr;

    void refreshSwatch();
    void refreshSummary();
};
