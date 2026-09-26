#pragma once

#include <QComboBox>
#include <QFont>
#include <QStandardItemModel>

#include "ChartSettings.h"

// Electron's grouped chart-window picker: a "Laps" section (Current, Previous,
// Fastest, Selected, Stint Laps, All Laps) above a "Time" section (15s–10m),
// each under a non-selectable heading. Options unavailable in the current
// session are omitted. Heading rows carry no item data, so findData() and the
// option keys are unaffected.
inline void populateChartWindowCombo(QComboBox* combo, bool lapCoordinatesAvailable,
                                     bool recordingOpen) {
    static const ChartWindow kLaps[] = {
        ChartWindow::CurrentLap, ChartWindow::PreviousLap, ChartWindow::FastestLap,
        ChartWindow::SelectedLap, ChartWindow::StintLaps, ChartWindow::AllLaps,
    };
    static const ChartWindow kTimes[] = {
        ChartWindow::Seconds15, ChartWindow::Seconds30, ChartWindow::Seconds60,
        ChartWindow::Seconds120, ChartWindow::Seconds300, ChartWindow::Seconds600,
    };
    combo->clear();
    auto* model = qobject_cast<QStandardItemModel*>(combo->model());
    const auto addHeading = [&](const QString& text) {
        combo->addItem(text);
        if (!model) return;
        QStandardItem* item = model->item(combo->count() - 1);
        item->setFlags(item->flags() & ~(Qt::ItemIsSelectable | Qt::ItemIsEnabled));
        QFont font = combo->font();
        font.setBold(true);
        font.setPointSizeF(font.pointSizeF() * 0.85);
        item->setFont(font);
    };
    addHeading(QStringLiteral("Laps"));
    for (ChartWindow w : kLaps)
        if (chartWindowIsAvailable(w, lapCoordinatesAvailable, recordingOpen))
            combo->addItem(chartWindowLabel(w), chartWindowKey(w));
    addHeading(QStringLiteral("Time"));
    for (ChartWindow w : kTimes) combo->addItem(chartWindowLabel(w), chartWindowKey(w));
}
