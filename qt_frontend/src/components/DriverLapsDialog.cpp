#include "DriverLapsDialog.h"
#include "CardColors.h"

#include <QDialogButtonBox>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QProgressBar>
#include <QScrollBar>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <algorithm>

namespace {

QString lapTimeText(int ms) {
    if (ms <= 0) return QStringLiteral("--:--.---");
    return QStringLiteral("%1:%2.%3")
        .arg(ms / 60000)
        .arg((ms % 60000) / 1000, 2, 10, QLatin1Char('0'))
        .arg(ms % 1000, 3, 10, QLatin1Char('0'));
}

QString sectorText(int ms) {
    if (ms <= 0) return QStringLiteral("—");
    return QStringLiteral("%1.%2").arg(ms / 1000).arg(ms % 1000, 3, 10, QLatin1Char('0'));
}

} // namespace

DriverLapsDialog::DriverLapsDialog(const QString& driverName, int carIdx,
                                   HoldsFastestLap holdsFastestLap, QWidget* parent)
    : QDialog(parent), carIdx_(carIdx), holdsFastestLap_(std::move(holdsFastestLap)) {
    setWindowTitle(driverName + QStringLiteral(" · Lap Times"));
    setAttribute(Qt::WA_DeleteOnClose);
    setModal(true);
    // Electron caps the modal at 75vh; the Qt window plays the viewport.
    // Width: half the window, at most 1100px, as Electron's min(50vw, 1100px).
    const int maxHeight = parent ? parent->window()->height() * 3 / 4 : 560;
    const int width = parent ? std::min(parent->window()->width() / 2, 1100) : 560;
    setMaximumHeight(maxHeight);
    resize(width, std::min(560, maxHeight));

    auto* root = new QVBoxLayout(this);

    table_ = new QTableWidget(0, 5);
    table_->setHorizontalHeaderLabels({"LAP", "TIME", "S1", "S2", "S3"});
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionMode(QAbstractItemView::NoSelection);
    table_->setShowGrid(false);
    table_->setAlternatingRowColors(true);
    table_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    table_->verticalHeader()->setVisible(false);
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
    table_->setColumnWidth(0, 56);
    QFont hf;
    hf.setPointSize(8);
    table_->horizontalHeader()->setFont(hf);
    root->addWidget(table_, 1);

    // The Strategy page's busy bar, until the first read lands.
    loading_ = new QWidget;
    auto* centre = new QVBoxLayout(loading_);
    auto* bar = new QProgressBar;
    bar->setRange(0, 0);
    bar->setTextVisible(false);
    bar->setFixedSize(128, 4);
    centre->addStretch();
    centre->addWidget(bar, 0, Qt::AlignCenter);
    centre->addStretch();
    root->addWidget(loading_, 1);

    empty_ = new QLabel(QStringLiteral("No completed laps for this driver yet."));
    empty_->setAlignment(Qt::AlignCenter);
    empty_->setForegroundRole(QPalette::PlaceholderText);
    root->addWidget(empty_, 1);
    empty_->hide();
    table_->hide();

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);
}

void DriverLapsDialog::apply(const tnrp::DriverLapHistoryRow& history) {
    if (history.car_idx != carIdx_) return;
    loading_->hide();
    if (history.laps.empty()) {
        empty_->show();
        table_->hide();
        return;
    }
    empty_->hide();
    table_->show();

    const QColor fastest = tnr::themed("#BF5FFF", "#7C3BA6");
    const QColor personalBest = tnr::themed("#37872D", "#0D6B2F");
    // Fastest valid time in each sector across this driver's laps.
    int bestSector[3] = {0, 0, 0};
    const int overallSector[3] = {history.overall_best_s1_ms, history.overall_best_s2_ms,
                                  history.overall_best_s3_ms};
    for (const auto& lap : history.laps) {
        const int ms[3] = {lap.s1_ms, lap.s2_ms, lap.s3_ms};
        const bool valid[3] = {lap.s1_valid, lap.s2_valid, lap.s3_valid};
        for (int s = 0; s < 3; ++s)
            if (ms[s] > 0 && valid[s] && (!bestSector[s] || ms[s] < bestSector[s]))
                bestSector[s] = ms[s];
    }
    const bool holdsFastestLap = holdsFastestLap_ && holdsFastestLap_();
    const QColor invalid = tnr::themed("#C4162A", "#C4162A");
    const QColor secondary = palette().color(QPalette::PlaceholderText);
    const bool followBottom = table_->verticalScrollBar() &&
        table_->verticalScrollBar()->value() >= table_->verticalScrollBar()->maximum();

    table_->setRowCount(static_cast<int>(history.laps.size()));
    for (int row = 0; row < static_cast<int>(history.laps.size()); ++row) {
        const auto& lap = history.laps[static_cast<size_t>(row)];
        // Purple when this driver holds the session's fastest lap (the same
        // holder Standings tints), green for their own best.
        const bool isBest = history.best_lap_num && *history.best_lap_num == lap.lap_num;
        const bool isOverallBest = isBest && holdsFastestLap;
        const QString cells[5] = {
            QString::number(lap.lap_num),
            (lap.lap_valid ? QString() : QStringLiteral("INV  ")) + lapTimeText(lap.lap_time_ms),
            sectorText(lap.s1_ms), sectorText(lap.s2_ms), sectorText(lap.s3_ms),
        };
        for (int column = 0; column < 5; ++column) {
            QTableWidgetItem* item = table_->item(row, column);
            if (!item) {
                item = new QTableWidgetItem;
                table_->setItem(row, column, item);
            }
            item->setText(cells[column]);
            item->setTextAlignment(column == 0 ? (Qt::AlignLeft | Qt::AlignVCenter)
                                               : (Qt::AlignRight | Qt::AlignVCenter));
            const int sectorMs = column == 2 ? lap.s1_ms : column == 3 ? lap.s2_ms
                               : column == 4 ? lap.s3_ms : 0;
            // Purple for the session's fastest sector across all drivers, green
            // for this driver's own fastest.
            const bool isOverallSector = column >= 2 && sectorMs > 0 &&
                sectorMs == overallSector[column - 2];
            const bool isBestSector = column >= 2 && sectorMs > 0 &&
                sectorMs == bestSector[column - 2];
            QFont font = item->font();
            font.setBold(column == 1 || isBestSector);
            font.setStrikeOut(column == 1 && !lap.lap_valid);
            item->setFont(font);
            if (column == 1)
                item->setForeground(!lap.lap_valid ? invalid : isOverallBest ? fastest
                                    : isBest ? personalBest : palette().color(QPalette::Text));
            else
                item->setForeground(isOverallSector ? fastest
                                    : isBestSector ? personalBest : secondary);
        }
        table_->setRowHeight(row, 24);
    }
    // Keep the newest lap in view as laps are added, unless the user scrolled up.
    if (followBottom) table_->scrollToBottom();
}
