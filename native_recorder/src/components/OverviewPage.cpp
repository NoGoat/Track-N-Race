#include "../MainWindow.h"
#include "../TelemetryChart.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFrame>
#include <QLabel>
#include <QFont>
#include <QPalette>
#include <QSizePolicy>

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

    // ── Chart ────────────────────────────────────────────────────
    chart = new TelemetryChart;
    vbox->addWidget(chart, 1);

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
