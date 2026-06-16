#include "../MainWindow.h"
#include "../TelemetryChart.h"
#include "../SessionModel.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFrame>
#include <QLabel>
#include <QFont>
#include <QPalette>
#include <QSizePolicy>
#include <QPushButton>
#include <QButtonGroup>
#include <QComboBox>
#include <QSignalBlocker>
#include <QApplication>

// ── UI helpers ────────────────────────────────────────────────────────────

static QFrame* vsep() {
    QFrame* f = new QFrame;
    f->setFrameShape(QFrame::VLine);
    f->setFrameShadow(QFrame::Sunken);
    return f;
}

static QFrame* makeStatCard(const QString& label, const QString& unit, QLabel*& valueOut) {
    QFrame* card = new QFrame;
    card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    QVBoxLayout* cv = new QVBoxLayout(card);
    cv->setContentsMargins(8, 6, 8, 6);
    cv->setSpacing(1);

    QLabel* lbl = new QLabel(label.toUpper());
    QFont lf; lf.setPointSize(7);
    lbl->setFont(lf);
    lbl->setForegroundRole(QPalette::PlaceholderText);

    valueOut = new QLabel("—");
    QFont vf; vf.setPointSize(15); vf.setBold(true);
    valueOut->setFont(vf);

    QLabel* ulbl = new QLabel(unit);
    QFont uf; uf.setPointSize(7);
    ulbl->setFont(uf);
    ulbl->setForegroundRole(QPalette::PlaceholderText);

    cv->addWidget(lbl);
    cv->addWidget(valueOut);
    cv->addWidget(ulbl);
    return card;
}

static QFrame* makeDmgCard(const QString& label, QLabel*& valueOut) {
    QFrame* card = new QFrame;
    card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    QVBoxLayout* cv = new QVBoxLayout(card);
    cv->setContentsMargins(6, 4, 6, 4);
    cv->setSpacing(0);

    QLabel* lbl = new QLabel(label.toUpper());
    QFont lf; lf.setPointSize(6);
    lbl->setFont(lf);
    lbl->setForegroundRole(QPalette::PlaceholderText);

    valueOut = new QLabel("—");
    QFont vf; vf.setPointSize(12); vf.setBold(true);
    valueOut->setFont(vf);

    cv->addWidget(lbl);
    cv->addWidget(valueOut);
    return card;
}

// ── Overview page ─────────────────────────────────────────────────────────

QWidget* MainWindow::buildOverviewTab() {
    QWidget* w = new QWidget;
    QVBoxLayout* vbox = new QVBoxLayout(w);
    vbox->setContentsMargins(8, 8, 8, 8);
    vbox->setSpacing(6);

    // ── Stats row ────────────────────────────────────────────────
    ov_statsFrame_ = new QFrame;
    ov_statsFrame_->setFixedHeight(80);
    QHBoxLayout* sh = new QHBoxLayout(ov_statsFrame_);
    sh->setContentsMargins(0, 0, 0, 0);
    sh->setSpacing(0);

    sh->addWidget(ov_statCardFrame_[OverviewLayout::Speed] =
        makeStatCard("Speed",    "kph", cardSpeed));
    sh->addWidget(vsep());
    sh->addWidget(ov_statCardFrame_[OverviewLayout::Rpm] =
        makeStatCard("RPM",      "",    cardRpm));
    sh->addWidget(vsep());
    sh->addWidget(ov_statCardFrame_[OverviewLayout::Gear] =
        makeStatCard("Gear",     "",    cardGear));
    sh->addWidget(vsep());
    sh->addWidget(ov_statCardFrame_[OverviewLayout::Throttle] =
        makeStatCard("Throttle", "%",   cardThrottle));
    sh->addWidget(vsep());
    sh->addWidget(ov_statCardFrame_[OverviewLayout::Brake] =
        makeStatCard("Brake",    "%",   cardBrake));
    sh->addWidget(vsep());
    sh->addWidget(ov_statCardFrame_[OverviewLayout::Drs] =
        makeStatCard("DRS",      "",    cardDrs));
    sh->addWidget(vsep());
    sh->addWidget(ov_statCardFrame_[OverviewLayout::Ers] =
        makeStatCard("ERS",      "%",   cardErs));
    sh->addWidget(vsep());
    sh->addWidget(ov_statCardFrame_[OverviewLayout::Pos] =
        makeStatCard("Pos",      "",    cardPos));

    vbox->addWidget(ov_statsFrame_);

    ov_sep1_ = new QFrame;
    ov_sep1_->setFrameShape(QFrame::HLine);
    ov_sep1_->setFrameShadow(QFrame::Sunken);
    vbox->addWidget(ov_sep1_);

    // ── Chart mode controls ──────────────────────────────────────
    ov_modeBar_ = new QWidget;
    QHBoxLayout* mb = new QHBoxLayout(ov_modeBar_);
    mb->setContentsMargins(0, 2, 0, 2);
    mb->setSpacing(4);

    auto* modeGroup = new QButtonGroup(ov_modeBar_);
    modeGroup->setExclusive(true);
    struct { const char* label; ChartMode mode; } modes[] = {
        { "Default",      ChartMode::Default     },
        { "Current Lap",  ChartMode::CurrentLap  },
        { "Previous Lap", ChartMode::PreviousLap },
        { "Fastest Lap",  ChartMode::FastestLap  },
        { "Compare",      ChartMode::Compare     },
    };
    // Same flat, underline-on-active style as the page switcher in the toolbar,
    // instead of these being raised/boxed buttons.
    const QString accent = QApplication::palette().color(QPalette::Highlight).name();
    const QString modeBtnStyle = QString(
        "QPushButton { padding: 6px 12px; border: none; background: transparent; }"
        "QPushButton:checked { border-bottom: 2px solid %1; }"
    ).arg(accent);
    for (const auto& m : modes) {
        QPushButton* b = new QPushButton(m.label);
        b->setCheckable(true);
        b->setFlat(true);
        b->setChecked(m.mode == ChartMode::Default);
        b->setStyleSheet(modeBtnStyle);
        modeGroup->addButton(b);
        mb->addWidget(b);
        // Compare needs a loaded recording's laps; disabled until one is opened.
        if (m.mode == ChartMode::Compare) { ov_compareBtn_ = b; b->setEnabled(false); }
        if (m.mode == ChartMode::Default) ov_defaultBtn_ = b;
        ChartMode mode = m.mode;
        connect(b, &QPushButton::clicked, this, [this, mode] {
            if (chart) chart->setMode(mode);
            if (ov_lapCombo_) ov_lapCombo_->setVisible(mode == ChartMode::Compare);
        });
    }

    ov_lapCombo_ = new QComboBox;
    ov_lapCombo_->setVisible(false);
    ov_lapCombo_->setMinimumWidth(70);
    mb->addWidget(ov_lapCombo_);
    mb->addStretch(1);

    // Chart legend, pinned to the right of this row (the chart's own overlay
    // legend is hidden) so it never covers the traces. Coloured swatch + name,
    // sourced from the chart so colours stay in sync.
    for (const auto& e : TelemetryChart::legendEntries()) {
        QLabel* item = new QLabel(
            QString("<span style='color:%1'>●</span> %2").arg(e.color.name(), e.name));
        item->setTextFormat(Qt::RichText);
        mb->addWidget(item);
    }

    vbox->addWidget(ov_modeBar_);

    connect(ov_lapCombo_, &QComboBox::currentIndexChanged, this, [this](int idx) {
        if (chart && idx >= 0)
            chart->setCompareLap(ov_lapCombo_->itemData(idx).toInt());
    });

    // ── Chart ────────────────────────────────────────────────────
    chart = new TelemetryChart;
    chart->setModel(model_);
    vbox->addWidget(chart, 1);

    // Repopulate the compare-lap selector whenever the set of laps changes.
    connect(model_, &SessionModel::lapsChanged, this, [this] {
        if (!ov_lapCombo_) return;
        const int prev = ov_lapCombo_->count() > 0 && ov_lapCombo_->currentIndex() >= 0
            ? ov_lapCombo_->currentData().toInt() : -1;
        QSignalBlocker block(ov_lapCombo_);
        ov_lapCombo_->clear();
        for (const LapBlock& l : model_->data().laps)
            ov_lapCombo_->addItem(QString("Lap %1").arg(l.lapNum), l.lapNum);
        int sel = ov_lapCombo_->findData(prev);
        if (sel < 0 && ov_lapCombo_->count() > 0) sel = 0;   // no prior selection — default to the first lap
        if (sel >= 0) ov_lapCombo_->setCurrentIndex(sel);
        // QSignalBlocker above suppresses currentIndexChanged, so the chart never
        // learns about this selection on its own — sync it explicitly.
        if (chart && sel >= 0) chart->setCompareLap(ov_lapCombo_->itemData(sel).toInt());
    });

    ov_sep2_ = new QFrame;
    ov_sep2_->setFrameShape(QFrame::HLine);
    ov_sep2_->setFrameShadow(QFrame::Sunken);
    vbox->addWidget(ov_sep2_);

    // ── Damage rows ──────────────────────────────────────────────
    ov_dmgFrame_ = new QFrame;
    QVBoxLayout* dv = new QVBoxLayout(ov_dmgFrame_);
    dv->setContentsMargins(0, 0, 0, 0);
    dv->setSpacing(0);

    ov_dmgRowA_ = new QFrame;
    ov_dmgRowA_->setFixedHeight(60);
    QHBoxLayout* ah = new QHBoxLayout(ov_dmgRowA_);
    ah->setContentsMargins(0, 0, 0, 0);
    ah->setSpacing(0);
    ah->addWidget(ov_dmgCardFrame_[OverviewLayout::TyreFl] =
        makeDmgCard("Tyre FL",  dmgTyreFl));   ah->addWidget(vsep());
    ah->addWidget(ov_dmgCardFrame_[OverviewLayout::TyreFr] =
        makeDmgCard("Tyre FR",  dmgTyreFr));   ah->addWidget(vsep());
    ah->addWidget(ov_dmgCardFrame_[OverviewLayout::TyreRl] =
        makeDmgCard("Tyre RL",  dmgTyreRl));   ah->addWidget(vsep());
    ah->addWidget(ov_dmgCardFrame_[OverviewLayout::TyreRr] =
        makeDmgCard("Tyre RR",  dmgTyreRr));   ah->addWidget(vsep());
    ah->addWidget(ov_dmgCardFrame_[OverviewLayout::BrakeFl] =
        makeDmgCard("Brake FL", dmgBrakeFl));  ah->addWidget(vsep());
    ah->addWidget(ov_dmgCardFrame_[OverviewLayout::BrakeFr] =
        makeDmgCard("Brake FR", dmgBrakeFr));  ah->addWidget(vsep());
    ah->addWidget(ov_dmgCardFrame_[OverviewLayout::BrakeRl] =
        makeDmgCard("Brake RL", dmgBrakeRl));  ah->addWidget(vsep());
    ah->addWidget(ov_dmgCardFrame_[OverviewLayout::BrakeRr] =
        makeDmgCard("Brake RR", dmgBrakeRr));

    ov_dmgHdiv_ = new QFrame;
    ov_dmgHdiv_->setFrameShape(QFrame::HLine);
    ov_dmgHdiv_->setFrameShadow(QFrame::Sunken);

    ov_dmgRowB_ = new QFrame;
    ov_dmgRowB_->setFixedHeight(60);
    QHBoxLayout* bh = new QHBoxLayout(ov_dmgRowB_);
    bh->setContentsMargins(0, 0, 0, 0);
    bh->setSpacing(0);
    bh->addWidget(ov_dmgCardFrame_[OverviewLayout::WingFl] =
        makeDmgCard("Wing FL",   dmgWingFl));   bh->addWidget(vsep());
    bh->addWidget(ov_dmgCardFrame_[OverviewLayout::WingFr] =
        makeDmgCard("Wing FR",   dmgWingFr));   bh->addWidget(vsep());
    bh->addWidget(ov_dmgCardFrame_[OverviewLayout::WingRear] =
        makeDmgCard("Wing Rear", dmgWingRear)); bh->addWidget(vsep());
    bh->addWidget(ov_dmgCardFrame_[OverviewLayout::Floor] =
        makeDmgCard("Floor",     dmgFloor));    bh->addWidget(vsep());
    bh->addWidget(ov_dmgCardFrame_[OverviewLayout::Sidepod] =
        makeDmgCard("Sidepod",   dmgSidepod));  bh->addWidget(vsep());
    bh->addWidget(ov_dmgCardFrame_[OverviewLayout::Diffuser] =
        makeDmgCard("Diffuser",  dmgDiffuser)); bh->addWidget(vsep());
    bh->addWidget(ov_dmgCardFrame_[OverviewLayout::Gearbox] =
        makeDmgCard("Gearbox",   dmgGearbox));  bh->addWidget(vsep());
    bh->addWidget(ov_dmgCardFrame_[OverviewLayout::Engine] =
        makeDmgCard("Engine",    dmgEngine));

    dv->addWidget(ov_dmgRowA_);
    dv->addWidget(ov_dmgHdiv_);
    dv->addWidget(ov_dmgRowB_);

    vbox->addWidget(ov_dmgFrame_);

    applyOverviewLayout(loadOverviewLayout());
    return w;
}

OverviewLayout MainWindow::loadOverviewLayout()
{
    OverviewLayout L;
    settings.beginGroup("overviewLayout");
    L.showChart = settings.value("showChart", true).toBool();
    settings.beginGroup("statCards");
    for (int i = 0; i < OverviewLayout::StatCardCount; ++i)
        L.statCards[i] = settings.value(OverviewLayout::statCardKey(i), true).toBool();
    settings.endGroup();
    settings.beginGroup("dmgCards");
    for (int i = 0; i < OverviewLayout::DmgCardCount; ++i) {
        const bool defaultHidden = i < OverviewLayout::WingFl;   // tyre/brake row defaults hidden
        L.dmgCards[i] = settings.value(OverviewLayout::dmgCardKey(i), !defaultHidden).toBool();
    }
    settings.endGroup();
    settings.endGroup();
    return L;
}

void MainWindow::saveOverviewLayout(const OverviewLayout& L)
{
    settings.beginGroup("overviewLayout");
    settings.setValue("showChart", L.showChart);
    settings.beginGroup("statCards");
    for (int i = 0; i < OverviewLayout::StatCardCount; ++i)
        settings.setValue(OverviewLayout::statCardKey(i), L.statCards[i]);
    settings.endGroup();
    settings.beginGroup("dmgCards");
    for (int i = 0; i < OverviewLayout::DmgCardCount; ++i)
        settings.setValue(OverviewLayout::dmgCardKey(i), L.dmgCards[i]);
    settings.endGroup();
    settings.endGroup();
}

void MainWindow::applyOverviewLayout(const OverviewLayout& L)
{
    bool anyStat = false;
    for (int i = 0; i < OverviewLayout::StatCardCount; ++i) {
        if (ov_statCardFrame_[i]) ov_statCardFrame_[i]->setVisible(L.statCards[i]);
        anyStat = anyStat || L.statCards[i];
    }
    if (ov_statsFrame_) ov_statsFrame_->setVisible(anyStat);
    if (ov_sep1_)       ov_sep1_->setVisible(anyStat);

    bool rowAVisible = false, rowBVisible = false;
    for (int i = 0; i < OverviewLayout::DmgCardCount; ++i) {
        if (ov_dmgCardFrame_[i]) ov_dmgCardFrame_[i]->setVisible(L.dmgCards[i]);
        if (i < OverviewLayout::WingFl) rowAVisible = rowAVisible || L.dmgCards[i];
        else                            rowBVisible = rowBVisible || L.dmgCards[i];
    }
    // A row whose cards are all hidden collapses entirely (rather than leaving
    // an empty 60px bar) so the chart's stretch factor can expand into the space.
    if (ov_dmgRowA_) ov_dmgRowA_->setVisible(rowAVisible);
    if (ov_dmgRowB_) ov_dmgRowB_->setVisible(rowBVisible);
    if (ov_dmgHdiv_) ov_dmgHdiv_->setVisible(rowAVisible && rowBVisible);
    if (ov_dmgFrame_) ov_dmgFrame_->setVisible(rowAVisible || rowBVisible);
    if (ov_sep2_)     ov_sep2_->setVisible(rowAVisible || rowBVisible);

    if (chart)       chart->setVisible(L.showChart);
    if (ov_modeBar_) ov_modeBar_->setVisible(L.showChart);
}

void MainWindow::applyAndSaveOverviewLayout(const OverviewLayout& L)
{
    applyOverviewLayout(L);
    saveOverviewLayout(L);
}
