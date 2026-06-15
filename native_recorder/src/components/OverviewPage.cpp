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
    QFrame* statsFrame = new QFrame;
    statsFrame->setFixedHeight(80);
    QHBoxLayout* sh = new QHBoxLayout(statsFrame);
    sh->setContentsMargins(0, 0, 0, 0);
    sh->setSpacing(0);

    sh->addWidget(makeStatCard("Speed",    "kph", cardSpeed));
    sh->addWidget(vsep());
    sh->addWidget(makeStatCard("RPM",      "",    cardRpm));
    sh->addWidget(vsep());
    sh->addWidget(makeStatCard("Gear",     "",    cardGear));
    sh->addWidget(vsep());
    sh->addWidget(makeStatCard("Throttle", "%",   cardThrottle));
    sh->addWidget(vsep());
    sh->addWidget(makeStatCard("Brake",    "%",   cardBrake));
    sh->addWidget(vsep());
    sh->addWidget(makeStatCard("DRS",      "",    cardDrs));
    sh->addWidget(vsep());
    sh->addWidget(makeStatCard("ERS",      "%",   cardErs));
    sh->addWidget(vsep());
    sh->addWidget(makeStatCard("Pos",      "",    cardPos));

    vbox->addWidget(statsFrame);

    auto* sep1 = new QFrame;
    sep1->setFrameShape(QFrame::HLine);
    sep1->setFrameShadow(QFrame::Sunken);
    vbox->addWidget(sep1);

    // ── Chart mode controls ──────────────────────────────────────
    QWidget* modeBar = new QWidget;
    QHBoxLayout* mb = new QHBoxLayout(modeBar);
    mb->setContentsMargins(0, 2, 0, 2);
    mb->setSpacing(4);

    auto* modeGroup = new QButtonGroup(modeBar);
    modeGroup->setExclusive(true);
    struct { const char* label; ChartMode mode; } modes[] = {
        { "Default",      ChartMode::Default     },
        { "Current Lap",  ChartMode::CurrentLap  },
        { "Previous Lap", ChartMode::PreviousLap },
        { "Fastest Lap",  ChartMode::FastestLap  },
        { "Compare",      ChartMode::Compare     },
    };
    for (const auto& m : modes) {
        QPushButton* b = new QPushButton(m.label);
        b->setCheckable(true);
        b->setChecked(m.mode == ChartMode::Default);
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

    vbox->addWidget(modeBar);

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
        if (sel >= 0) ov_lapCombo_->setCurrentIndex(sel);
        else if (ov_lapCombo_->count() > 0) ov_lapCombo_->setCurrentIndex(ov_lapCombo_->count() - 1);
    });

    auto* sep2 = new QFrame;
    sep2->setFrameShape(QFrame::HLine);
    sep2->setFrameShadow(QFrame::Sunken);
    vbox->addWidget(sep2);

    // ── Damage rows ──────────────────────────────────────────────
    QFrame* dmgFrame = new QFrame;
    QVBoxLayout* dv = new QVBoxLayout(dmgFrame);
    dv->setContentsMargins(0, 0, 0, 0);
    dv->setSpacing(0);

    QFrame* rowA = new QFrame;
    rowA->setFixedHeight(60);
    QHBoxLayout* ah = new QHBoxLayout(rowA);
    ah->setContentsMargins(0, 0, 0, 0);
    ah->setSpacing(0);
    ah->addWidget(makeDmgCard("Tyre FL",  dmgTyreFl));   ah->addWidget(vsep());
    ah->addWidget(makeDmgCard("Tyre FR",  dmgTyreFr));   ah->addWidget(vsep());
    ah->addWidget(makeDmgCard("Tyre RL",  dmgTyreRl));   ah->addWidget(vsep());
    ah->addWidget(makeDmgCard("Tyre RR",  dmgTyreRr));   ah->addWidget(vsep());
    ah->addWidget(makeDmgCard("Brake FL", dmgBrakeFl));  ah->addWidget(vsep());
    ah->addWidget(makeDmgCard("Brake FR", dmgBrakeFr));  ah->addWidget(vsep());
    ah->addWidget(makeDmgCard("Brake RL", dmgBrakeRl));  ah->addWidget(vsep());
    ah->addWidget(makeDmgCard("Brake RR", dmgBrakeRr));

    QFrame* hdiv = new QFrame;
    hdiv->setFrameShape(QFrame::HLine);
    hdiv->setFrameShadow(QFrame::Sunken);

    QFrame* rowB = new QFrame;
    rowB->setFixedHeight(60);
    QHBoxLayout* bh = new QHBoxLayout(rowB);
    bh->setContentsMargins(0, 0, 0, 0);
    bh->setSpacing(0);
    bh->addWidget(makeDmgCard("Wing FL",   dmgWingFl));   bh->addWidget(vsep());
    bh->addWidget(makeDmgCard("Wing FR",   dmgWingFr));   bh->addWidget(vsep());
    bh->addWidget(makeDmgCard("Wing Rear", dmgWingRear)); bh->addWidget(vsep());
    bh->addWidget(makeDmgCard("Floor",     dmgFloor));    bh->addWidget(vsep());
    bh->addWidget(makeDmgCard("Sidepod",   dmgSidepod));  bh->addWidget(vsep());
    bh->addWidget(makeDmgCard("Diffuser",  dmgDiffuser)); bh->addWidget(vsep());
    bh->addWidget(makeDmgCard("Gearbox",   dmgGearbox));  bh->addWidget(vsep());
    bh->addWidget(makeDmgCard("Engine",    dmgEngine));

    dv->addWidget(rowA);
    dv->addWidget(hdiv);
    dv->addWidget(rowB);

    vbox->addWidget(dmgFrame);
    return w;
}
