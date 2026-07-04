#include "TyresPage.h"
#include "PageUiHelpers.h"
#include "TyreCardsWidget.h"
#include "TyreChartsWidget.h"
#include "TyreHelpers.h"
#include "../IconUtils.h"

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
#include <QStackedWidget>
#include <QToolButton>
#include <QStyle>
#include <QSettings>

#include <algorithm>
#include <vector>

// ── Tyres page builder ────────────────────────────────────────────────────

TyresPage::TyresPage(SessionModel* model, QWidget* parent)
    : QWidget(parent)
{
    // Allocation view (tyre-set tables + wheel cards) lives in its own widget so
    // the whole thing can be swapped for the graphs view via the stack below.
    QWidget* allocWidget = new QWidget;
    QHBoxLayout* hbox = new QHBoxLayout(allocWidget);
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

    createSetsTable(drySetsTable_, 0);
    createSetsTable(wetSetsTable_, 1);

    drySetsTable_->setSizeAdjustPolicy(QAbstractScrollArea::AdjustToContents);
    drySetsTable_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    drySetsTable_->setMinimumHeight(0);

    wetSetsTable_->setMinimumHeight(0);

    hbox->addWidget(leftWidget, 1);

    // Vertical divider
    hbox->addWidget(tnrui::vline());

    // Small uppercase section caption, matching the app's other panel headers.
    auto capLabel = [](const QString& t) {
        auto* l = new QLabel(t);
        QFont f; f.setPointSize(8); f.setBold(true);
        l->setFont(f);
        l->setForegroundRole(QPalette::PlaceholderText);
        return l;
    };
    // An icon+text toggle button styled like the toolbar's action buttons:
    // auto-raise gives the flat/transparent background with a hover highlight
    // (same as Open Recording / Edit Layout / Settings). toGraphs picks the view.
    auto toggleBtn = [this](const QString& text, const char* iconName,
                            QStyle::StandardPixmap fallback, bool toGraphs) {
        auto* b = new QToolButton;
        b->setAutoRaise(true);
        b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        b->setText(text);
        b->setIcon(adaptThemeIcon(QIcon::fromTheme(iconName),
                   palette().color(QPalette::WindowText), style()->standardIcon(fallback)));
        connect(b, &QToolButton::clicked, this, [this, toGraphs]{ setGraphsShown(toGraphs); });
        return b;
    };

    // ── Right panel: wheel cards under a slim header carrying the Graphs toggle ─
    tyreCards_ = new TyreCardsWidget(Qt::Vertical);
    QWidget* cardsCol = new QWidget;
    cardsCol->setFixedWidth(240);
    QVBoxLayout* cc = new QVBoxLayout(cardsCol);
    cc->setContentsMargins(0, 0, 0, 0);
    cc->setSpacing(0);
    {
        QWidget* hdr = new QWidget;
        QHBoxLayout* hh = new QHBoxLayout(hdr);
        hh->setContentsMargins(8, 4, 8, 4);
        hh->addWidget(capLabel("CONDITIONS"));
        hh->addStretch();
        hh->addWidget(toggleBtn("Graphs", "window-maximize-symbolic",
                                QStyle::SP_TitleBarMaxButton, /*toGraphs=*/true));
        cc->addWidget(hdr);
        cc->addWidget(tnrui::hline());
    }
    cc->addWidget(tyreCards_, 1);
    hbox->addWidget(cardsCol);

    // ── Graphs view: the Overview's tyre charts (2×2 here) under their own header ─
    tyreCharts_ = new TyreChartsWidget(/*grid=*/true);
    tyreCharts_->setModel(model);
    {
        QSettings s{ "TrackNRace", "NativeRecorder" };
        tyreCharts_->setTyreLifeMode(s.value("ui/tyreWearMode", "life").toString() != "wear");
    }
    QWidget* graphsView = new QWidget;
    QVBoxLayout* gv = new QVBoxLayout(graphsView);
    gv->setContentsMargins(0, 0, 0, 0);
    gv->setSpacing(0);
    {
        QWidget* hdr = new QWidget;
        QHBoxLayout* hh = new QHBoxLayout(hdr);
        hh->setContentsMargins(8, 4, 8, 4);
        hh->addWidget(capLabel("TYRE GRAPHS"));
        hh->addStretch();
        // Centred fitted-tyre summary (compound · wear · life), like Electron's
        // expanded graph header. Updated from tyre_sets in updateTyreSets().
        graphsFitted_ = new QLabel;
        graphsFitted_->setTextFormat(Qt::RichText);
        hh->addWidget(graphsFitted_);
        hh->addStretch();
        hh->addWidget(toggleBtn("Allocation", "window-restore-symbolic",
                                QStyle::SP_TitleBarNormalButton, /*toGraphs=*/false));
        gv->addWidget(hdr);
        gv->addWidget(tnrui::hline());
    }
    gv->addWidget(tyreCharts_, 1);

    stack_ = new QStackedWidget;
    stack_->addWidget(allocWidget);   // page 0 — allocation
    stack_->addWidget(graphsView);    // page 1 — graphs

    QVBoxLayout* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addWidget(stack_, 1);

    setGraphsShown(false);   // start on allocation
}

void TyresPage::setGraphsShown(bool on) {
    graphsShown_ = on;
    if (stack_) stack_->setCurrentIndex(on ? 1 : 0);
}

void TyresPage::setPlaybackMode(bool on, float currentTime) {
    if (!tyreCharts_) return;
    tyreCharts_->setPlaybackMode(on);
    if (on) tyreCharts_->setCurrentTime(currentTime);
}

void TyresPage::setCurrentTime(float t) {
    if (tyreCharts_) tyreCharts_->setCurrentTime(t);
}

// ── Tyres page updater ────────────────────────────────────────────────────

void TyresPage::updateTyreCards(const nlohmann::json& telemetry, const nlohmann::json& damage) {
    if (tyreCards_) tyreCards_->update(telemetry, damage);
}

// ── Tyre sets table updater ───────────────────────────────────────────────

void TyresPage::updateTyreSets(const nlohmann::json& tyreSets) {
    if (!drySetsTable_ || !wetSetsTable_ || tyreSets.empty() || !tyreSets.contains("sets")) return;

    // Fitted-tyre summary for the graphs header: compound · wear · life remaining.
    if (graphsFitted_) {
        QString txt;
        for (const auto& s : tyreSets["sets"]) {
            if (!s.value("fitted", false)) continue;
            const int compound = s.value("actual_compound", 0);
            const int visual   = s.value("visual_compound", 0);
            const int wear     = s.value("wear",      0);
            const int life     = s.value("life_span", 0);
            const QColor cmp   = tyreTextColor(visual);
            const QString cmpCol  = (cmp.isValid() ? cmp : palette().color(QPalette::WindowText)).name();
            const QString wearCol = wearPctColor(wear).name();
            const QString secCol  = palette().color(QPalette::PlaceholderText).name();
            txt = "<span style='color:" + cmpCol + ";font-weight:bold;'>" + tyreLabel(compound) + "</span>"
                + " <span style='color:" + secCol + ";'>&middot;</span> "
                + "<span style='color:" + wearCol + ";'>" + QString::number(wear) + "% wear</span>"
                + " <span style='color:" + secCol + ";'>&middot; " + QString::number(life) + "L remaining</span>";
            break;
        }
        graphsFitted_->setText(txt);
    }

    std::vector<nlohmann::json> drySets;
    std::vector<nlohmann::json> wetSets;
    for (const auto& s : tyreSets["sets"]) {
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

    drySetsTable_->setRowCount(drySets.size());
    wetSetsTable_->setRowCount(wetSets.size());

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

    populateTable(drySetsTable_, drySets);
    populateTable(wetSetsTable_, wetSets);

    int hh = drySetsTable_->horizontalHeader()->height();
    if (hh <= 0) hh = drySetsTable_->horizontalHeader()->sizeHint().height();
    if (hh <= 0) hh = 30; // Better fallback for Breeze

    int totalH = hh + (drySetsTable_->frameWidth() * 2);
    for (int r = 0; r < drySetsTable_->rowCount(); ++r) {
        totalH += drySetsTable_->rowHeight(r);
    }
    drySetsTable_->setMaximumHeight(totalH);
}
