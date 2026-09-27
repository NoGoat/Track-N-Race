#pragma once

#include <QFrame>
#include <QString>
#include <QVector>

class AnalysisSeriesModel;
class QCheckBox;
class QLabel;
class QLineEdit;
class QScrollArea;
class QToolButton;
class QWidget;

// "Add Metrics": a popup for choosing what the Analysis graphs chart.
//
//  ┌──────────────────────────────┐
//  │ 🔍 Filter metrics             │   Enter adds the first match not yet charted
//  ├──────────────────────────────┤
//  │ [Speed] [RPM] [Gear] …        │   toggle chips, one block per group
//  │ ───────────────────────────── │
//  │ [Lateral G] …                 │
//  │ ───────────────────────────── │
//  │            COM FL FR RL RR ALL│   per-corner matrix; COM (combined) draws
//  │ Surface     ☐  ☑  ☑  ☐  ☐  ▣ │   the picked corners of a row in one series
//  │ ───────────────────────────── │
//  │ [Average Tyre Wear] …         │
//  └──────────────────────────────┘
//
// Every control edits the series model directly, so the sidebar list and the
// graphs follow as you click; the popup closes on Escape or a click outside.
class AnalysisMetricPicker : public QFrame {
    Q_OBJECT
public:
    explicit AnalysisMetricPicker(AnalysisSeriesModel* model, QWidget* parent = nullptr);

    // Opens beside `anchor`: below it when there is room, otherwise above.
    void popup(const QWidget* anchor);

protected:
    void keyPressEvent(QKeyEvent* event) override;

private:
    struct Chip {
        QString metricId;
        QString haystack;
        QToolButton* button = nullptr;
    };
    struct Section {
        QWidget* widget = nullptr;
        QFrame* separator = nullptr;   // rule above the block
        QVector<int> chips;      // indexes into chips_
        bool tyreMatrix = false;
    };
    struct TyreRow {
        QString idPrefix;
        QString haystack;
        QVector<QWidget*> widgets;   // everything on the row, for filtering
        QLabel* label = nullptr;
        QCheckBox* combined = nullptr;
        QVector<QCheckBox*> corners;
        QCheckBox* all = nullptr;
    };

    AnalysisSeriesModel* model_ = nullptr;
    QLineEdit* filter_ = nullptr;
    QScrollArea* scroll_ = nullptr;
    QWidget* content_ = nullptr;
    QLabel* empty_ = nullptr;
    int wrapWidth_ = 0;
    QVector<Chip> chips_;
    QVector<Section> sections_;
    QVector<TyreRow> tyreRows_;

    QWidget* buildChipSection(const QString& title, const QStringList& metricIds);
    QWidget* buildTyreMatrix();
    void applyFilter();
    void refreshStates();
    void addFirstMatch();
};
