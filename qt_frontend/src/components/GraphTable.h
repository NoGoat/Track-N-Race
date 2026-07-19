#pragma once

#include <QTableView>
#include <QString>
#include <QVector>
#include <functional>

class QGridLayout;
class ChartView;
class GraphTableModel;

// A read-only raw-values table used to replace a telemetry graph (see the
// per-graph Chart/Table toggle in the Settings "Graphs" tab). One column per
// series plus a leading time column; oldest sample at the top, newest at the
// bottom, holding every sample in the visible window (no row cap — the toolbar's
// time window is the only bound). Auto-scrolls to the newest row unless the user
// has scrolled up to inspect history.
//
// It's a QTableView over a lightweight model that stores rows as raw numbers and
// formats a cell only when it's actually on screen — so only the handful of
// visible rows are ever painted or turned into strings. (The previous QTableWidget
// built an item object for every cell of every row on each rebuild, which collapsed
// on a 10-minute window of streaming data.)
//
// Feeding pattern (called from a chart widget's refresh(), newest sample first):
//   t->beginRebuild();
//   for (samples newest-first, within the window) t->addRow(s.t, valueA, valueB);
//   t->endRebuild();
// The first value of every row is the session time (seconds); each column's Fmt
// (set at construction) decides how its raw value renders. Rows are fed newest-
// first; the table shows them oldest-first (newest last).
class GraphTable : public QTableView {
    Q_OBJECT
public:
    // How a column's raw value renders: Time -> "m:ss.mmm"; FixedN -> N decimals.
    enum Fmt { Time, Fixed0, Fixed1, Fixed2 };
    struct Column { QString header; Fmt fmt; };

    explicit GraphTable(const QVector<Column>& columns, QWidget* parent = nullptr);

    // Replace the visible schema in place (used when protocol capabilities
    // enable/disable a chart series).
    void setColumns(const QVector<Column>& columns);

    // Format a session time (seconds) as "m:ss.mmm".
    static QString fmtTime(float t);

    // Rebuild cycle — see the class comment. Pass each row's values (session time
    // first) as numbers; they're stored raw and formatted lazily. full() always
    // returns false (the time window is the only bound); kept so existing feed loops
    // that guard on it compile.
    void beginRebuild();
    template <typename... Ts>
    void addRow(Ts... values) {
        const double v[] = { static_cast<double>(values)... };
        addRowImpl(v, int(sizeof...(values)));
    }
    bool full() const { return false; }
    void endRebuild();

private:
    void addRowImpl(const double* values, int n);
    GraphTableModel* model_ = nullptr;
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
