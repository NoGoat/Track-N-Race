#include "EditOverviewLayoutDialog.h"
#include "OverviewPage.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QPushButton>
#include <QStyleOptionButton>

namespace {

// A checkable button that wears the active QStyle's default-button look (the
// blue Breeze/Fusion outline) while it's toggled on, and draws plain while off.
//
// The DefaultButton feature alone isn't enough here: a checked checkable button
// is drawn sunken (State_On), and the style paints that sunken bevel over the
// default-button frame, hiding it — which is why a naive "set DefaultButton when
// checked" shows nothing on the on state. So we also strip State_On (and force
// State_Raised) for the *drawing only*: isChecked() is untouched, so the toggle
// logic still works; the button just paints raised + outlined when on. It's the
// real native style, not a stylesheet, and it never touches the dialog's actual
// default-button/Enter-key bookkeeping.
class ToggleButton : public QPushButton {
public:
    using QPushButton::QPushButton;

protected:
    void initStyleOption(QStyleOptionButton* option) const override {
        QPushButton::initStyleOption(option);
        if (isChecked()) {
            option->features |= QStyleOptionButton::DefaultButton;
            option->state &= ~QStyle::State_On;
            option->state |= QStyle::State_Raised;
        }
    }
};

} // namespace

EditOverviewLayoutDialog::EditOverviewLayoutDialog(OverviewPage* page, QWidget* parent)
    : QDialog(parent), page_(page)
{
    setWindowTitle("Edit Overview Layout");
    // Qt::Dialog gives the plain dialog frame (no minimize/maximize buttons).
    setWindowFlags(Qt::Dialog);
    // Modal + parented to MainWindow (see the call site) is what keeps this
    // above MainWindow and blocks it now that it's shown via show() rather
    // than exec() — the window-type flag alone doesn't do either.
    setWindowModality(Qt::ApplicationModal);
    layout_ = page_->loadLayout();

    QVBoxLayout* main = new QVBoxLayout(this);
    // A fixed-size top-level layout makes Qt drop the resize handles and the
    // maximize button — this should behave like a plain modal child dialog,
    // not a regular resizable/maximizable window.
    main->setSizeConstraint(QLayout::SetFixedSize);

    // ── Chart ────────────────────────────────────────────────────
    QGroupBox* chartBox = new QGroupBox("Chart");
    QHBoxLayout* chartLay = new QHBoxLayout(chartBox);
    chartBtn_ = new ToggleButton("Speed / RPM / ERS Chart");
    chartBtn_->setCheckable(true);
    chartBtn_->setChecked(layout_.showChart);
    connect(chartBtn_, &QPushButton::toggled, this, &EditOverviewLayoutDialog::toggleChart);
    chartLay->addWidget(chartBtn_);
    main->addWidget(chartBox);

    // ── Tyre section ─────────────────────────────────────────────
    QGroupBox* tyreBox = new QGroupBox("Tyres");
    QVBoxLayout* tyreLay = new QVBoxLayout(tyreBox);

    // Cards row: FL / FR / RL / RR (shown when view = Cards)
    tyreCardsRow_ = new QWidget;
    QHBoxLayout* crh = new QHBoxLayout(tyreCardsRow_);
    crh->setContentsMargins(0, 0, 0, 0);
    for (int i = 0; i < OverviewLayout::TyreCornerCount; ++i) {
        QPushButton* b = new ToggleButton(OverviewLayout::tyreCardLabel(i));
        b->setCheckable(true);
        b->setChecked(layout_.tyreCardVisible[i]);
        connect(b, &QPushButton::toggled, this, [this, i](bool on) { toggleTyreCard(i, on); });
        tyreCardBtns_[i] = b;
        crh->addWidget(b);
    }
    tyreLay->addWidget(tyreCardsRow_);

    // Charts row: Surface / Inner / Brake / Wear (shown when view = Charts)
    tyreChartsRow_ = new QWidget;
    QHBoxLayout* chrh = new QHBoxLayout(tyreChartsRow_);
    chrh->setContentsMargins(0, 0, 0, 0);
    for (int i = 0; i < OverviewLayout::TyreChartCount; ++i) {
        QPushButton* b = new ToggleButton(OverviewLayout::tyreChartLabel(i));
        b->setCheckable(true);
        b->setChecked(layout_.tyreChartVisible[i]);
        connect(b, &QPushButton::toggled, this, [this, i](bool on) { toggleTyreChart(i, on); });
        tyreChartBtns_[i] = b;
        chrh->addWidget(b);
    }
    tyreLay->addWidget(tyreChartsRow_);

    tyreCardsRow_->setVisible(layout_.tyreView  == OverviewLayout::TyreCards);
    tyreChartsRow_->setVisible(layout_.tyreView == OverviewLayout::TyreCharts);
    main->addWidget(tyreBox);

    // ── Stats ────────────────────────────────────────────────────
    QGroupBox* statsBox = new QGroupBox("Stats");
    QGridLayout* statsLay = new QGridLayout(statsBox);
    for (int i = 0; i < OverviewLayout::StatCardCount; ++i) {
        QPushButton* b = new ToggleButton(OverviewLayout::statCardLabel(i));
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
        QPushButton* b = new ToggleButton(OverviewLayout::dmgCardLabel(i));
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
    closeBtn->setDefault(true);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    bottom->addWidget(closeBtn);
    main->addLayout(bottom);
}

void EditOverviewLayoutDialog::toggleChart(bool on)
{
    layout_.showChart = on;
    page_->applyAndSaveLayout(layout_);
}

void EditOverviewLayoutDialog::toggleStat(int idx, bool on)
{
    layout_.statCards[idx] = on;
    page_->applyAndSaveLayout(layout_);
}

void EditOverviewLayoutDialog::toggleDmg(int idx, bool on)
{
    layout_.dmgCards[idx] = on;
    page_->applyAndSaveLayout(layout_);
}

void EditOverviewLayoutDialog::toggleTyreCard(int i, bool on)
{
    layout_.tyreCardVisible[i] = on;
    page_->applyAndSaveLayout(layout_);
}

void EditOverviewLayoutDialog::toggleTyreChart(int i, bool on)
{
    layout_.tyreChartVisible[i] = on;
    page_->applyAndSaveLayout(layout_);
}

