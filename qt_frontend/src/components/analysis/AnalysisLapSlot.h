#pragma once

#include "AnalysisRecording.h"

#include <QColor>
#include <QGroupBox>
#include <QString>
#include <QVector>

class AnalysisColorButton;
class QComboBox;
class QLabel;
class QLineEdit;

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

// One side of a comparison: which driver, which lap, and how that lap is named
// and coloured in the legend, tooltips and map. A "following" slot mirrors the
// playback cursor and only its name and colour are editable.
class AnalysisLapSlot : public QGroupBox {
    Q_OBJECT
public:
    enum class Mode { Selectable, FollowsPlayback };

    AnalysisLapSlot(const QString& title, const QString& defaultLabel, Mode mode,
                    QWidget* parent = nullptr);

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
    // Drops both driver and lap, e.g. when their recording is closed.
    void resetSelection();
    AnalysisLapRef selection() const { return {driver(), lap()}; }

    // Following slots.
    void setFollowed(const QString& driverName, const AnalysisLapChoice& lap,
                     const QString& runningTime = {});

    QString label() const;           // as typed; may be empty
    QString resolvedLabel() const;   // falls back to the default label
    void setLabel(const QString& label);
    QColor color() const;
    void setColor(const QColor& color);

signals:
    void driverActivated();
    void lapActivated();
    void labelEdited(const QString& label);
    void colorPicked(const QColor& color);

private:
    Mode mode_;
    QString defaultLabel_;
    QComboBox* driverBox_ = nullptr;
    QComboBox* lapBox_ = nullptr;
    QLabel* followedDriver_ = nullptr;
    QLabel* followedCompound_ = nullptr;
    QLabel* followedLap_ = nullptr;
    QLineEdit* labelEdit_ = nullptr;
    AnalysisColorButton* colorButton_ = nullptr;
};
