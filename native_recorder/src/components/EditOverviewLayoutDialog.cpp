#include "EditOverviewLayoutDialog.h"
#include "../MainWindow.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QPushButton>

EditOverviewLayoutDialog::EditOverviewLayoutDialog(MainWindow* mainWindow, QWidget* parent)
    : QDialog(parent), mainWindow_(mainWindow)
{
    setWindowTitle("Edit Overview Layout");
    // Belt-and-suspenders: some window managers don't infer "no maximize" from
    // a fixed-size layout alone, so drop the hint explicitly too.
    setWindowFlags(windowFlags() & ~Qt::WindowMaximizeButtonHint);
    layout_ = mainWindow_->loadOverviewLayout();

    QVBoxLayout* main = new QVBoxLayout(this);
    // A fixed-size top-level layout makes Qt drop the resize handles and the
    // maximize button — this should behave like a plain modal child dialog,
    // not a regular resizable/maximizable window.
    main->setSizeConstraint(QLayout::SetFixedSize);

    // ── Chart ────────────────────────────────────────────────────
    QGroupBox* chartBox = new QGroupBox("Chart");
    QHBoxLayout* chartLay = new QHBoxLayout(chartBox);
    chartBtn_ = new QPushButton("Speed / RPM / ERS Chart");
    chartBtn_->setCheckable(true);
    chartBtn_->setChecked(layout_.showChart);
    connect(chartBtn_, &QPushButton::toggled, this, &EditOverviewLayoutDialog::toggleChart);
    chartLay->addWidget(chartBtn_);
    main->addWidget(chartBox);

    // ── Stats ────────────────────────────────────────────────────
    QGroupBox* statsBox = new QGroupBox("Stats");
    QGridLayout* statsLay = new QGridLayout(statsBox);
    for (int i = 0; i < OverviewLayout::StatCardCount; ++i) {
        QPushButton* b = new QPushButton(OverviewLayout::statCardLabel(i));
        b->setCheckable(true);
        b->setChecked(layout_.statCards[i]);
        connect(b, &QPushButton::toggled, this, [this, i](bool on) { toggleStat(i, on); });
        statBtns_[i] = b;
        statsLay->addWidget(b, i / 4, i % 4);
    }
    main->addWidget(statsBox);

    // ── Damage ───────────────────────────────────────────────────
    QGroupBox* dmgBox = new QGroupBox("Damage");
    QGridLayout* dmgLay = new QGridLayout(dmgBox);
    for (int i = 0; i < OverviewLayout::DmgCardCount; ++i) {
        QPushButton* b = new QPushButton(OverviewLayout::dmgCardLabel(i));
        b->setCheckable(true);
        b->setChecked(layout_.dmgCards[i]);
        connect(b, &QPushButton::toggled, this, [this, i](bool on) { toggleDmg(i, on); });
        dmgBtns_[i] = b;
        dmgLay->addWidget(b, i / 8, i % 8);
    }
    main->addWidget(dmgBox);

    // ── Close ────────────────────────────────────────────────────
    QHBoxLayout* bottom = new QHBoxLayout;
    bottom->addStretch(1);
    QPushButton* closeBtn = new QPushButton("Close");
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    bottom->addWidget(closeBtn);
    main->addLayout(bottom);
}

void EditOverviewLayoutDialog::toggleChart(bool on)
{
    layout_.showChart = on;
    mainWindow_->applyAndSaveOverviewLayout(layout_);
}

void EditOverviewLayoutDialog::toggleStat(int idx, bool on)
{
    layout_.statCards[idx] = on;
    mainWindow_->applyAndSaveOverviewLayout(layout_);
}

void EditOverviewLayoutDialog::toggleDmg(int idx, bool on)
{
    layout_.dmgCards[idx] = on;
    mainWindow_->applyAndSaveOverviewLayout(layout_);
}
