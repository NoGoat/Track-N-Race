#include "GraphTable.h"
#include "ChartView.h"

#include <QGridLayout>
#include <QHeaderView>
#include <QLayoutItem>
#include <QScrollBar>
#include <QTableWidgetItem>
#include <algorithm>

GraphTable::GraphTable(const QStringList& headers, QWidget* parent)
    : QTableWidget(parent)
{
    cols_ = headers.size();
    setColumnCount(cols_);
    setHorizontalHeaderLabels(headers);

    // Read-only, no selection/editing — this is a data readout, not an input.
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setSelectionMode(QAbstractItemView::NoSelection);
    setFocusPolicy(Qt::NoFocus);
    setAlternatingRowColors(true);
    setShowGrid(false);
    setWordWrap(false);

    verticalHeader()->setVisible(false);
    verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    verticalHeader()->setDefaultSectionSize(18);   // compact rows
    horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    horizontalHeader()->setHighlightSections(false);
}

QString GraphTable::fmtTime(float t) {
    if (t < 0) t = 0;
    const int totalTenths = (int)(t * 10.0f + 0.5f);
    const int mins   = totalTenths / 600;
    const int secs   = (totalTenths / 10) % 60;
    const int tenths = totalTenths % 10;
    return QString("%1:%2.%3")
        .arg(mins)
        .arg(secs, 2, 10, QChar('0'))
        .arg(tenths);
}

void GraphTable::beginRebuild() {
    pending_.clear();
}

void GraphTable::addRow(const QStringList& cols) {
    pending_.append(cols);
}

void GraphTable::endRebuild() {
    // Preserve the "stick to newest" behaviour: only auto-scroll to the bottom if
    // the user was already there (or the table was empty). If they scrolled up to
    // read history, leave the viewport where it is.
    QScrollBar* sb = verticalScrollBar();
    const bool stickToBottom = sb->value() >= sb->maximum();

    const int n = pending_.size();
    setRowCount(n);
    // pending_ is newest-first; render reversed so the newest sample is the last row.
    for (int r = 0; r < n; ++r) {
        const QStringList& cols = pending_[n - 1 - r];
        for (int c = 0; c < cols_; ++c) {
            QTableWidgetItem* it = item(r, c);
            if (!it) {
                it = new QTableWidgetItem;
                it->setTextAlignment(c == 0 ? (Qt::AlignLeft | Qt::AlignVCenter)
                                            : (Qt::AlignRight | Qt::AlignVCenter));
                setItem(r, c, it);
            }
            it->setText(c < cols.size() ? cols[c] : QString());
        }
    }

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
    // Clear stretch from a previous, possibly larger, layout before re-setting it.
    for (int i = 0; i < sectionCount; ++i) {
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

    chart->layoutPanelsRows(anyChart ? chartRows : QVector<QVector<int>>{});
    chart->setVisible(anyChart);
    if (anyChart) outer->addWidget(chart, 0, 0, (int)rows.size(), maxCols);

    // Overlay each table on its natural cell (on top of the spanning chart). A lone
    // section in a row spans the full width, matching ChartView's single-panel row.
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
}
