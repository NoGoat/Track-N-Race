#include "../MainWindow.h"
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
        outTable->setAlternatingRowColors(false);
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

    tp_drySetsTable->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    tp_drySetsTable->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    
    tp_wetSetsTable->setMinimumHeight(0);

    hbox->addWidget(leftWidget, 1);

    // Vertical divider
    QFrame* vdiv = new QFrame;
    vdiv->setFrameShape(QFrame::VLine);
    vdiv->setFrameShadow(QFrame::Sunken);
    hbox->addWidget(vdiv);

    // ── Right panel: WheelCards (1×4 vertical, fills height) ────────
    QWidget* right = new QWidget;
    right->setFixedWidth(240);
    QVBoxLayout* rv = new QVBoxLayout(right);
    rv->setContentsMargins(0, 0, 0, 0);
    rv->setSpacing(0);

    static const char* cornerNames[] = { "FRONT LEFT", "FRONT RIGHT", "REAR LEFT", "REAR RIGHT" };

    for (int i = 0; i < 4; ++i) {
        QWidget* card = new QWidget;
        card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        QVBoxLayout* cv = new QVBoxLayout(card);
        cv->setContentsMargins(10, 8, 10, 8);
        cv->setSpacing(2);

        QLabel* title = new QLabel(cornerNames[i]);
        QFont tf; tf.setPointSize(7); tf.setBold(true);
        title->setFont(tf);
        title->setForegroundRole(QPalette::PlaceholderText);
        cv->addWidget(title);

        auto makeRow = [&](const QString& label, QLabel*& valueOut) {
            QWidget* row = new QWidget;
            QHBoxLayout* h = new QHBoxLayout(row);
            h->setContentsMargins(0, 0, 0, 0);
            QLabel* lbl = new QLabel(label);
            QFont lf; lf.setPointSize(8); lbl->setFont(lf);
            lbl->setForegroundRole(QPalette::PlaceholderText);
            valueOut = new QLabel("—");
            QFont vf; vf.setPointSize(8); vf.setBold(true);
            valueOut->setFont(vf);
            valueOut->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            h->addWidget(lbl);
            h->addStretch();
            h->addWidget(valueOut);
            return row;
        };

        cv->addWidget(makeRow("Surface", tp_surfaceTemp[i]));
        cv->addWidget(makeRow("Inner",   tp_innerTemp[i]));
        cv->addWidget(makeRow("Brake",   tp_brakeTemp[i]));
        cv->addWidget(makeRow("Wear",    tp_wearLabel[i]));

        auto* wearBar = new QProgressBar;
        wearBar->setRange(0, 100);
        wearBar->setValue(0);
        wearBar->setTextVisible(false);
        wearBar->setFixedHeight(6);
        wearBar->setStyleSheet(
            "QProgressBar { border: none; background: palette(mid); border-radius: 3px; }"
            "QProgressBar::chunk { background: #73BF69; border-radius: 3px; }"
        );
        tp_wear[i] = wearBar;
        cv->addWidget(wearBar);

        tp_blisters[i] = new QLabel;
        tp_blisters[i]->setVisible(false);
        QFont bf; bf.setPointSize(7);
        tp_blisters[i]->setFont(bf);
        tp_blisters[i]->setForegroundRole(QPalette::PlaceholderText);
        cv->addWidget(tp_blisters[i]);

        rv->addWidget(card, 1);

        if (i < 3) {
            QFrame* hdiv = new QFrame;
            hdiv->setFrameShape(QFrame::HLine);
            hdiv->setFrameShadow(QFrame::Sunken);
            rv->addWidget(hdiv);
        }
    }

    hbox->addWidget(right);
    return w;
}

// ── Tyres page updater ────────────────────────────────────────────────────

void MainWindow::updateTyresPage() {
    if (!tp_surfaceTemp[0]) return;

    static const char* surfKeys[]    = { "tyre_temp_surface_fl", "tyre_temp_surface_fr", "tyre_temp_surface_rl", "tyre_temp_surface_rr" };
    static const char* innerKeys[]   = { "tyre_temp_inner_fl",   "tyre_temp_inner_fr",   "tyre_temp_inner_rl",   "tyre_temp_inner_rr"   };
    static const char* brakeKeys[]   = { "brake_temp_fl",        "brake_temp_fr",        "brake_temp_rl",        "brake_temp_rr"        };
    static const char* wearKeys[]    = { "tyre_wear_fl",         "tyre_wear_fr",         "tyre_wear_rl",         "tyre_wear_rr"         };
    static const char* blisterKeys[] = { "blisters_fl",          "blisters_fr",          "blisters_rl",          "blisters_rr"          };

    for (int i = 0; i < 4; ++i) {
        if (!lastPlayerTelemetryData.empty()) {
            int surf = lastPlayerTelemetryData.value(surfKeys[i], -1);
            if (surf >= 0) {
                tp_surfaceTemp[i]->setText(QString::number(surf) + "°C");
                tp_surfaceTemp[i]->setStyleSheet("color: " + tyreTempColor(surf).name() + "; font-weight: bold;");
            }
            int inner = lastPlayerTelemetryData.value(innerKeys[i], -1);
            if (inner >= 0) {
                tp_innerTemp[i]->setText(QString::number(inner) + "°C");
                tp_innerTemp[i]->setStyleSheet("color: " + tyreTempColor(inner).name() + "; font-weight: bold;");
            }
            int brk = lastPlayerTelemetryData.value(brakeKeys[i], -1);
            if (brk >= 0) {
                tp_brakeTemp[i]->setText(QString::number(brk) + "°C");
                tp_brakeTemp[i]->setStyleSheet("color: " + brakeTempColor(brk).name() + "; font-weight: bold;");
            }
        }

        if (!lastPlayerDamageData.empty()) {
            int wear = lastPlayerDamageData.value(wearKeys[i], -1);
            if (wear >= 0) {
                const QString wearCol = wearPctColor(wear).name();
                tp_wearLabel[i]->setText(QString::number(wear) + "%");
                tp_wearLabel[i]->setStyleSheet("color: " + wearCol + "; font-weight: bold;");
                tp_wear[i]->setValue(wear);
                tp_wear[i]->setStyleSheet(QString(
                    "QProgressBar { border: none; background: palette(mid); border-radius: 3px; }"
                    "QProgressBar::chunk { background: %1; border-radius: 3px; }"
                ).arg(wearCol));
            }
            int blisters = lastPlayerDamageData.value(blisterKeys[i], 0);
            if (blisters > 0) {
                tp_blisters[i]->setText(QString("· %1% blisters").arg(blisters));
                tp_blisters[i]->setVisible(true);
            } else {
                tp_blisters[i]->setVisible(false);
            }
        }
    }
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

    auto sortSets = [](std::vector<nlohmann::json>& vec) {
        std::sort(vec.begin(), vec.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
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

            table->setItem(row, 0, makeItem(QString::number(idx + 1)));

            auto* cmpItem = makeItem(tyreLabel(compound));
            if (cmpFg.isValid()) cmpItem->setForeground(cmpFg);
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
                    "QProgressBar { border: none; background: palette(mid); border-radius: 3px; }"
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
    tp_drySetsTable->setFixedHeight(totalH);
}
