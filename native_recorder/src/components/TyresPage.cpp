#include "../MainWindow.h"
#include "TyreCardsWidget.h"
#include "TyreHelpers.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFrame>
#include <QLabel>
#include <QFont>
#include <QPalette>
#include <QSizePolicy>
#include <QTableWidget>
#include <QHeaderView>
#include <QProgressBar>
#include <QTableWidgetItem>

#include <algorithm>
#include <vector>

// ── Tyres page builder ────────────────────────────────────────────────────

QWidget* MainWindow::buildTyresPage() {
    QWidget* w = new QWidget;
    QHBoxLayout* hbox = new QHBoxLayout(w);
    hbox->setContentsMargins(0, 0, 0, 0);
    hbox->setSpacing(0);

    // ── Tyre sets table (left) ───────────────────────────────────
    QWidget* leftWidget = new QWidget;
    QVBoxLayout* leftLayout = new QVBoxLayout(leftWidget);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(0);

    auto createSetsTable = [&](QTableWidget*& outTable, int stretch) {
        outTable = new QTableWidget;
        outTable->setColumnCount(7);
        outTable->setHorizontalHeaderLabels({"#", "COMPOUND", "STATUS", "WEAR", "LIFE", "SESSION", "DELTA"});
        outTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
        outTable->setSelectionMode(QAbstractItemView::NoSelection);
        outTable->setShowGrid(false);
        outTable->setAlternatingRowColors(true);
        outTable->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        outTable->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
        outTable->verticalHeader()->setVisible(false);
        outTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
        outTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
        const int colW[7] = { 36, 92, 72, 0, 52, 84, 72 };
        for (int c = 0; c < 7; ++c)
            if (c != 3) outTable->setColumnWidth(c, colW[c]);
        QFont hf; hf.setPointSize(7);
        outTable->horizontalHeader()->setFont(hf);
        
        leftLayout->addWidget(outTable, stretch);
    };

    createSetsTable(tp_drySetsTable, 0);
    createSetsTable(tp_wetSetsTable, 1);

    tp_drySetsTable->setSizeAdjustPolicy(QAbstractScrollArea::AdjustToContents);
    tp_drySetsTable->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    tp_drySetsTable->setMinimumHeight(0);
    
    tp_wetSetsTable->setMinimumHeight(0);

    hbox->addWidget(leftWidget, 1);

    // Vertical divider
    QFrame* vdiv = new QFrame;
    vdiv->setFrameShape(QFrame::VLine);
    vdiv->setFrameShadow(QFrame::Sunken);
    hbox->addWidget(vdiv);

    // ── Right panel: WheelCards (1×4 vertical, fills height) ────────
    tp_tyreCards_ = new TyreCardsWidget(Qt::Vertical);
    tp_tyreCards_->setFixedWidth(240);

    hbox->addWidget(tp_tyreCards_);
    return w;
}

// ── Tyres page updater ────────────────────────────────────────────────────

void MainWindow::updateTyresPage() {
    if (tp_tyreCards_) tp_tyreCards_->update(lastPlayerTelemetryData, lastPlayerDamageData);
    if (ov_tyreCards_) ov_tyreCards_->update(lastPlayerTelemetryData, lastPlayerDamageData);
}

// ── Tyre sets table updater ───────────────────────────────────────────────

void MainWindow::updateTyreSetsTable() {
    if (!tp_drySetsTable || !tp_wetSetsTable || lastTyreSetsData.empty() || !lastTyreSetsData.contains("sets")) return;

    std::vector<nlohmann::json> drySets;
    std::vector<nlohmann::json> wetSets;
    for (const auto& s : lastTyreSetsData["sets"]) {
        int compound = s.value("actual_compound", 0);
        if (compound != 0) {
            if (compound == 7 || compound == 8) {
                wetSets.push_back(s);
            } else {
                drySets.push_back(s);
            }
        }
    }

    auto statusPriority = [](const nlohmann::json& s) {
        QString st = setStatusText(s);
        if (st == "FITTED")   return 1;
        if (st == "NEW")      return 2;
        if (st == "USED")     return 3;
        if (st == "RESERVED") return 4;
        return 5;
    };

    auto sortSets = [&](std::vector<nlohmann::json>& vec) {
        std::sort(vec.begin(), vec.end(), [&](const nlohmann::json& a, const nlohmann::json& b) {
            int vcA = a.value("visual_compound", 99);
            int vcB = b.value("visual_compound", 99);
            if (vcA != vcB) return vcA < vcB;
            
            int spA = statusPriority(a);
            int spB = statusPriority(b);
            if (spA != spB) return spA < spB;
            
            return a.value("idx", 99) < b.value("idx", 99);
        });
    };
    sortSets(drySets);
    sortSets(wetSets);

    tp_drySetsTable->setRowCount(drySets.size());
    tp_wetSetsTable->setRowCount(wetSets.size());

    static const char* sessionLabels[] = {
        "—", "FP1", "FP2", "FP3", "Short P",
        "Q1", "Q2", "Q3", "Short Q", "1-Shot Q",
        "SS1", "SS2", "SS3", "SS Short", "SS 1-Shot",
        "Race", "Race 2", "Race 3", "Time Trial"
    };

    auto makeItem = [](const QString& text) { return new QTableWidgetItem(text); };

    auto populateTable = [&](QTableWidget* table, const std::vector<nlohmann::json>& setsList) {
        for (int row = 0; row < (int)setsList.size(); ++row) {
            const auto& s = setsList[row];
            int idx        = s.value("idx", 0);
            int compound   = s.value("actual_compound",     0);
            int visual     = s.value("visual_compound",     0);
            int wear       = s.value("wear",                0);
            int lifeSpan   = s.value("life_span",           0);
            int usable     = s.value("usable_life",         0);
            int recSess    = s.value("recommended_session", 0);
            int deltaMs    = s.value("lap_delta_ms",        0);

            QString status    = setStatusText(s);
            QColor  statusCol = setStatusColor(s);
            QColor  cmpFg     = tyreTextColor(visual);

            auto* idxItem = makeItem(QString::number(idx + 1));
            idxItem->setTextAlignment(Qt::AlignCenter);
            table->setItem(row, 0, idxItem);

            auto* cmpItem = makeItem(tyreLabel(compound));
            if (cmpFg.isValid()) cmpItem->setForeground(cmpFg);
            cmpItem->setTextAlignment(Qt::AlignCenter);
            table->setItem(row, 1, cmpItem);

            auto* stItem = makeItem(status);
            stItem->setForeground(statusCol);
            table->setItem(row, 2, stItem);

            {
                const QString wc = wearPctColor(wear).name();
                QWidget* cell = new QWidget;
                cell->setStyleSheet("background: transparent;");
                QHBoxLayout* wh = new QHBoxLayout(cell);
                wh->setContentsMargins(4, 0, 4, 0);
                wh->setSpacing(4);

                auto* bar = new QProgressBar;
                bar->setRange(0, 100);
                bar->setValue(wear);
                bar->setTextVisible(false);
                bar->setFixedHeight(6);
                bar->setStyleSheet(QString(
                    "QProgressBar { border: none; background: rgba(128, 128, 128, 0.3); border-radius: 3px; }"
                    "QProgressBar::chunk { background: %1; border-radius: 3px; }"
                ).arg(wc));

                auto* wearLbl = new QLabel(QString::number(wear) + "%");
                wearLbl->setStyleSheet("color: " + wc + "; font-weight: bold; background: transparent;");
                QFont wf; wf.setPointSize(8);
                wearLbl->setFont(wf);
                wearLbl->setFixedWidth(36);
                wearLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

                wh->addWidget(bar, 1);
                wh->addWidget(wearLbl);
                table->setCellWidget(row, 3, cell);
            }

            QString lifeText = (lifeSpan > 0 || usable > 0)
                ? QString("%1/%2L").arg(lifeSpan).arg(usable) : "—";
            table->setItem(row, 4, makeItem(lifeText));

            int rsIdx = (recSess >= 0 && recSess < 19) ? recSess : 0;
            table->setItem(row, 5, makeItem(sessionLabels[rsIdx]));

            QString deltaText;
            if (deltaMs != 0)
                deltaText = QString("%1%2").arg(deltaMs > 0 ? "+" : "").arg(deltaMs / 1000.0, 0, 'f', 3);
            auto* deltaItem = makeItem(deltaText);
            if (deltaMs > 0)      deltaItem->setForeground(QColor("#C4162A"));
            else if (deltaMs < 0) deltaItem->setForeground(QColor("#37872D"));
            table->setItem(row, 6, deltaItem);

            table->setRowHeight(row, 22);
        }
    };

    populateTable(tp_drySetsTable, drySets);
    populateTable(tp_wetSetsTable, wetSets);

    int hh = tp_drySetsTable->horizontalHeader()->height();
    if (hh <= 0) hh = tp_drySetsTable->horizontalHeader()->sizeHint().height();
    if (hh <= 0) hh = 30; // Better fallback for Breeze

    int totalH = hh + (tp_drySetsTable->frameWidth() * 2);
    for (int r = 0; r < tp_drySetsTable->rowCount(); ++r) {
        totalH += tp_drySetsTable->rowHeight(r);
    }
    tp_drySetsTable->setMaximumHeight(totalH);
}
