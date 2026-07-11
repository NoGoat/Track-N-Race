#include "GraphTable.h"
#include "ChartView.h"

#include <QAbstractTableModel>
#include <QFrame>
#include <QGridLayout>
#include <QHeaderView>
#include <QLayoutItem>
#include <QScrollBar>
#include <algorithm>

// Model behind GraphTable's QTableView. Rows are stored as raw doubles (row-major,
// in the order callers feed them — i.e. newest-first) and a cell is formatted only
// when the view asks for it, so only the on-screen rows are ever turned into
// strings. This is what keeps a 10-minute window cheap: the old QTableWidget built
// an item object for every cell of every row on each rebuild.
class GraphTableModel : public QAbstractTableModel {
public:
    GraphTableModel(QVector<GraphTable::Column> cols, QObject* parent)
        : QAbstractTableModel(parent), cols_(std::move(cols)) {}

    int rowCount(const QModelIndex& p = QModelIndex()) const override {
        return p.isValid() ? 0 : rows_;
    }
    int columnCount(const QModelIndex& p = QModelIndex()) const override {
        return p.isValid() ? 0 : int(cols_.size());
    }

    QVariant headerData(int section, Qt::Orientation o, int role) const override {
        if (o == Qt::Horizontal && role == Qt::DisplayRole &&
            section >= 0 && section < cols_.size())
            return cols_[section].header;
        return QVariant();
    }

    QVariant data(const QModelIndex& idx, int role) const override {
        if (!idx.isValid()) return QVariant();
        if (role == Qt::TextAlignmentRole)
            return int(idx.column() == 0 ? (Qt::AlignLeft  | Qt::AlignVCenter)
                                         : (Qt::AlignRight | Qt::AlignVCenter));
        if (role != Qt::DisplayRole) return QVariant();
        // Display runs oldest-first (row 0 at the top); storage is newest-first.
        const int src = rows_ - 1 - idx.row();
        if (src < 0 || src >= rows_) return QVariant();
        return format(data_[src * cols_.size() + idx.column()], cols_[idx.column()].fmt);
    }

    // ── Feed (driven by GraphTable::beginRebuild/addRow/endRebuild) ──
    void begin() { fill_ = 0; }
    void add(const double* vals, int n) {
        const int c = int(cols_.size());
        const int base = fill_ * c;
        if (base + c > data_.size()) data_.resize(base + c);
        for (int i = 0; i < c; ++i) data_[base + i] = (i < n) ? vals[i] : 0.0;
        ++fill_;
    }
    // Reconcile the committed row count with the new one (structural change only).
    // The surviving rows' *values* also shift as the window slides; the view repaints
    // those itself (GraphTable::endRebuild calls viewport()->update()), which only
    // paints/formats the on-screen rows.
    void commit() {
        const int newN = fill_, oldN = rows_;
        if (newN > oldN)      { beginInsertRows(QModelIndex(), oldN, newN - 1); rows_ = newN; endInsertRows(); }
        else if (newN < oldN) { beginRemoveRows(QModelIndex(), newN, oldN - 1); rows_ = newN; endRemoveRows(); }
    }

private:
    static QString format(double v, GraphTable::Fmt f) {
        switch (f) {
            case GraphTable::Time:   return GraphTable::fmtTime(float(v));
            case GraphTable::Fixed0: return QString::number(v, 'f', 0);
            case GraphTable::Fixed1: return QString::number(v, 'f', 1);
            case GraphTable::Fixed2: return QString::number(v, 'f', 2);
        }
        return QString();
    }

    QVector<GraphTable::Column> cols_;
    QVector<double> data_;   // row-major, capacity reused across rebuilds
    int rows_ = 0;           // committed row count (== model rowCount)
    int fill_ = 0;           // rows written so far in the current rebuild cycle
};

GraphTable::GraphTable(const QVector<Column>& columns, QWidget* parent)
    : QTableView(parent)
{
    model_ = new GraphTableModel(columns, this);
    setModel(model_);

    // Read-only, no selection/editing — this is a data readout, not an input.
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setSelectionMode(QAbstractItemView::NoSelection);
    setFocusPolicy(Qt::NoFocus);
    // Edge to edge: no frame border, so the table fills its cell with no inset (the
    // platform style otherwise gives QTableView a 2px sunken frame). The viewport is
    // still painted opaque with palette(base) by the style, so overlay tables (placed
    // on top of a chart cell) don't let the chart bleed through.
    setFrameShape(QFrame::NoFrame);
    // No alternating row colours: as new samples stream in, rows shift up every
    // frame, so the zebra banding crawls/flickers against the scrolling data. A flat
    // background keeps the readout stable while it updates.
    setAlternatingRowColors(false);
    setShowGrid(false);
    setWordWrap(false);

    verticalHeader()->setVisible(false);
    verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    verticalHeader()->setDefaultSectionSize(18);   // compact, uniform-height rows
    horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    horizontalHeader()->setHighlightSections(false);
}

QString GraphTable::fmtTime(float t) {
    if (t < 0) t = 0;
    const int totalMs = (int)(t * 1000.0f + 0.5f);
    const int mins   = totalMs / 60000;
    const int secs   = (totalMs / 1000) % 60;
    const int millis = totalMs % 1000;
    return QString("%1:%2.%3")
        .arg(mins)
        .arg(secs, 2, 10, QChar('0'))
        .arg(millis, 3, 10, QChar('0'));
}

void GraphTable::beginRebuild() { model_->begin(); }

void GraphTable::addRowImpl(const double* values, int n) { model_->add(values, n); }

void GraphTable::endRebuild() {
    // Preserve the "stick to newest" behaviour: only auto-scroll to the bottom if
    // the user was already there (or the table was empty). If they scrolled up to
    // read history, leave the viewport where it is.
    QScrollBar* sb = verticalScrollBar();
    const bool stickToBottom = sb->value() >= sb->maximum();
    model_->commit();
    // Row values shift every frame as the window slides even when the row count is
    // unchanged; repaint the viewport so those cells refresh. Only the visible rows
    // are actually painted (and formatted), so this stays O(on-screen rows).
    viewport()->update();
    if (stickToBottom) scrollToBottom();
}

void tnr::layoutSectionGrid(QGridLayout* outer, ChartView* chart,
                            const QVector<QVector<int>>& rows, int sectionCount,
                            const bool* tableMode, GraphTable* const* tables,
                            const std::function<void(int)>& ensureTable)
{
    if (!outer || !chart) return;

    // Detach the current items (deleting a QWidgetItem does NOT delete its widget,
    // so chart_ and the tables survive and get re-placed below).
    while (QLayoutItem* it = outer->takeAt(0)) delete it;
    // Delete the separator lines from the previous all-tables layout — takeAt drops
    // their layout items but not the widgets, so remove them explicitly (they're the
    // only "gtsep"-named children; the GraphTables, though also QFrames, are not).
    if (QWidget* host = outer->parentWidget())
        for (QFrame* sep : host->findChildren<QFrame*>(QStringLiteral("gtsep"), Qt::FindDirectChildrenOnly))
            delete sep;
    // Clear stretch from a previous, possibly larger, layout before re-setting it.
    // The all-tables layout interleaves separator tracks, so clear twice the sections.
    for (int i = 0; i < 2 * sectionCount; ++i) {
        outer->setRowStretch(i, 0);
        outer->setColumnStretch(i, 0);
    }
    // Hide every table up front; the ones actually placed are re-shown below.
    for (int s = 0; s < sectionCount; ++s)
        if (tables[s]) tables[s]->setVisible(false);

    int maxCols = 1;
    for (const QVector<int>& r : rows) maxCols = std::max<int>(maxCols, r.size());

    // Build the ChartView's panel rows: chart-mode sections keep their id (== panel
    // id); table-mode cells become -1 spacers so the real panels land in the right
    // cells while the spanning chart reserves the blank areas the tables overlay.
    QVector<QVector<int>> chartRows;
    chartRows.reserve(rows.size());
    bool anyChart = false;
    for (const QVector<int>& r : rows) {
        QVector<int> cr;
        cr.reserve(r.size());
        for (int s : r) {
            if (tableMode[s]) { cr.append(-1); }
            else              { cr.append(s); anyChart = true; }
        }
        chartRows.append(cr);
    }

    // The inter-panel gap only exists so overlay tables line up with the chart's
    // panels. With no chart (every section is a table), there's nothing to align to,
    // so drop it — the tables tile flush against the separator lines placed below.
    outer->setSpacing(anyChart ? ChartView::PanelGap : 0);

    chart->layoutPanelsRows(anyChart ? chartRows : QVector<QVector<int>>{});
    // Reparent (addWidget) BEFORE setVisible: on the first layout the chart has
    // never been parented, so setVisible(true) while it's still top-level makes Qt
    // briefly show it as its own window — a tiny window that flashes on screen
    // during startup. Adding it to the grid first gives it a (hidden) parent, so
    // setVisible only affects it in place.
    if (anyChart) outer->addWidget(chart, 0, 0, (int)rows.size(), maxCols);
    chart->setVisible(anyChart);

    if (anyChart) {
        // Chart present: overlay each table on its natural cell (on top of the
        // spanning chart), aligned by the panel gap. A lone section spans full width.
        for (int r = 0; r < rows.size(); ++r) {
            const QVector<int>& row = rows[r];
            for (int c = 0; c < row.size(); ++c) {
                const int s = row[c];
                if (!tableMode[s]) continue;
                ensureTable(s);
                GraphTable* t = tables[s];
                if (!t) continue;
                if (row.size() == 1) outer->addWidget(t, r, 0, 1, maxCols);
                else                 outer->addWidget(t, r, c);
                t->setVisible(true);
                t->raise();   // sit above the spanning chart underneath
            }
        }
        for (int r = 0; r < rows.size(); ++r) outer->setRowStretch(r, 1);
        for (int c = 0; c < maxCols; ++c)     outer->setColumnStretch(c, 1);
    } else {
        // All tables: tile them on even (content) tracks and drop a QFrame Sunken
        // separator — identical to the dividers between the tyre cards — on the odd
        // tracks between adjacent tables. Separator tracks carry no stretch, so the
        // tables stay flush against the lines; outer edges get no separator.
        auto sepLine = [](QFrame::Shape shape) {
            QFrame* f = new QFrame;
            f->setObjectName(QStringLiteral("gtsep"));
            f->setFrameShape(shape);
            f->setFrameShadow(QFrame::Sunken);
            return f;
        };
        const int gCols = 2 * maxCols - 1;
        for (int r = 0; r < rows.size(); ++r) {
            const QVector<int>& row = rows[r];
            const int gr = 2 * r;
            if (row.size() == 1) {
                ensureTable(row[0]);
                if (GraphTable* t = tables[row[0]]) {
                    outer->addWidget(t, gr, 0, 1, gCols);   // spans full width
                    t->setVisible(true); t->raise();
                }
            } else {
                for (int c = 0; c < row.size(); ++c) {
                    ensureTable(row[c]);
                    GraphTable* t = tables[row[c]];
                    if (!t) continue;
                    outer->addWidget(t, gr, 2 * c);
                    t->setVisible(true); t->raise();
                    if (c < row.size() - 1)                 // vertical divider to the right
                        outer->addWidget(sepLine(QFrame::VLine), gr, 2 * c + 1);
                }
            }
            if (r < rows.size() - 1)                        // horizontal divider below the row
                outer->addWidget(sepLine(QFrame::HLine), gr + 1, 0, 1, gCols);
        }
        for (int r = 0; r < rows.size(); ++r) outer->setRowStretch(2 * r, 1);
        for (int c = 0; c < maxCols; ++c)     outer->setColumnStretch(2 * c, 1);
    }
}
