#pragma once

#include <QTableWidget>
#include <QStringList>
#include <QVector>
#include <functional>

class QGridLayout;
class ChartView;

// A read-only raw-values table used to replace a telemetry graph (see the
// per-graph Chart/Table toggle in the Settings "Graphs" tab). One column per
// series plus a leading time column; oldest sample at the top, newest at the
// bottom, holding every sample in the visible window (no row cap — the toolbar's
// time window is the only bound). Auto-scrolls to the newest row unless the user
// has scrolled up to inspect history.
//
// Feeding pattern (called from a chart widget's refresh()):
//   t->beginRebuild();
//   for (samples newest-first, within the window) t->addRow({ GraphTable::fmtTime(s.t), ... });
//   t->endRebuild();
// Callers pass rows newest-first; endRebuild() renders them oldest-first so the
// latest value lands on the last row. Rows are reused between rebuilds.
class GraphTable : public QTableWidget {
    Q_OBJECT
public:
    // headers[0] is the time column label (e.g. "Time"); the rest name each series.
    explicit GraphTable(const QStringList& headers, QWidget* parent = nullptr);

    // Format a session time (seconds) as "m:ss.s".
    static QString fmtTime(float t);

    // Rebuild cycle. Pass rows newest-first; endRebuild() renders them oldest-first
    // (newest last). full() always returns false — kept so existing feed loops that
    // guard on it compile; the visible time window bounds how many rows arrive.
    void beginRebuild();
    void addRow(const QStringList& cols);
    bool full() const { return false; }
    void endRebuild();

private:
    int cols_ = 0;
    QVector<QStringList> pending_;   // rows for the current rebuild, newest-first
};

namespace tnr {

// Lay chart-mode and table-mode sections out in ONE grid so a table occupies the
// same cell its chart would, replacing it in place (rather than stacking below).
//
// `rows` lists the VISIBLE sections in their natural grid order — each inner list
// is one grid row, left-to-right (section index == the ChartView panel id). For
// each section, tableMode[s] selects table vs chart. Chart-mode sections stay in
// the single spanning ChartView (one GL context); table-mode cells become blank
// holes in the chart that the section's GraphTable is overlaid on top of, so the
// two align. `tables[s]` is the section's table (built on demand via ensureTable);
// sectionCount sizes both arrays.
void layoutSectionGrid(QGridLayout* outer, ChartView* chart,
                       const QVector<QVector<int>>& rows, int sectionCount,
                       const bool* tableMode, GraphTable* const* tables,
                       const std::function<void(int)>& ensureTable);

}  // namespace tnr
