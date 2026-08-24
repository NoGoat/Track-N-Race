#include "EditSessionLayoutDialog.h"
#include "SessionPage.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QPushButton>
#include <QLabel>
#include <QSlider>
#include <QStyleOptionButton>

namespace {
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
}

EditSessionLayoutDialog::EditSessionLayoutDialog(SessionPage* page, QWidget* parent)
    : QDialog(parent), page_(page)
{
    setWindowTitle("Edit Session Layout");
    setWindowFlags(Qt::Dialog);
    setWindowModality(Qt::ApplicationModal);
    layout_ = page_->loadLayout();

    QVBoxLayout* main = new QVBoxLayout(this);
    main->setSizeConstraint(QLayout::SetFixedSize);

    // ── Header ───────────────────────────────────────────────────
    QGroupBox* headerBox = new QGroupBox("Header");
    QHBoxLayout* headerLay = new QHBoxLayout(headerBox);
    
    gpNameBtn_ = new ToggleButton("GP & Circuit");
    gpNameBtn_->setCheckable(true);
    gpNameBtn_->setChecked(layout_.showGpName);
    connect(gpNameBtn_, &QPushButton::toggled, this, &EditSessionLayoutDialog::toggleGpName);
    headerLay->addWidget(gpNameBtn_);

    zonesBtn_ = new ToggleButton("Marshal Zones");
    zonesBtn_->setCheckable(true);
    zonesBtn_->setChecked(layout_.showMarshalZones);
    connect(zonesBtn_, &QPushButton::toggled, this, &EditSessionLayoutDialog::toggleMarshalZones);
    headerLay->addWidget(zonesBtn_);

    timeLeftBtn_ = new ToggleButton("Time Left");
    timeLeftBtn_->setCheckable(true);
    timeLeftBtn_->setChecked(layout_.showTimeLeft);
    connect(timeLeftBtn_, &QPushButton::toggled, this, &EditSessionLayoutDialog::toggleTimeLeft);
    headerLay->addWidget(timeLeftBtn_);

    main->addWidget(headerBox);

    // ── Stats ────────────────────────────────────────────────────
    QGroupBox* statsBox = new QGroupBox("Stats");
    QGridLayout* statsLay = new QGridLayout(statsBox);
    for (int i = 0; i < SessionLayout::StatCardCount; ++i) {
        QPushButton* b = new ToggleButton(SessionLayout::cardLabel(i));
        b->setCheckable(true);
        b->setChecked(layout_.cards[i]);
        connect(b, &QPushButton::toggled, this, [this, i](bool on) { toggleCard(i, on); });
        cardBtns_[i] = b;
        statsLay->addWidget(b, i / 5, i % 5);
    }
    main->addWidget(statsBox);

    // ── Panels (Schematic: Map + Weather on left, Proximity + Events on right) ──
    QGroupBox* panelsBox = new QGroupBox("Panels");
    QVBoxLayout* panelsMainLay = new QVBoxLayout(panelsBox);
    QHBoxLayout* panelsLay = new QHBoxLayout;

    QVBoxLayout* leftCol = new QVBoxLayout;
    mapBtn_ = new ToggleButton("Track Map");
    mapBtn_->setCheckable(true);
    mapBtn_->setChecked(layout_.showMap);
    connect(mapBtn_, &QPushButton::toggled, this, &EditSessionLayoutDialog::toggleMap);
    leftCol->addWidget(mapBtn_);

    weatherBtn_ = new ToggleButton("Weather Forecast Strip");
    weatherBtn_->setCheckable(true);
    weatherBtn_->setChecked(layout_.showWeather);
    connect(weatherBtn_, &QPushButton::toggled, this, &EditSessionLayoutDialog::toggleWeather);
    leftCol->addWidget(weatherBtn_);
    panelsLay->addLayout(leftCol, 100 - layout_.sidebarPct);

    QVBoxLayout* rightCol = new QVBoxLayout;
    proxBtn_ = new ToggleButton("Proximity");
    proxBtn_->setCheckable(true);
    proxBtn_->setChecked(layout_.showProximity);
    connect(proxBtn_, &QPushButton::toggled, this, &EditSessionLayoutDialog::toggleProximity);
    rightCol->addWidget(proxBtn_);

    eventsBtn_ = new ToggleButton("Events Log");
    eventsBtn_->setCheckable(true);
    eventsBtn_->setChecked(layout_.showEvents);
    connect(eventsBtn_, &QPushButton::toggled, this, &EditSessionLayoutDialog::toggleEvents);
    rightCol->addWidget(eventsBtn_);
    panelsLay->addLayout(rightCol, layout_.sidebarPct);

    panelsMainLay->addLayout(panelsLay);

    // Width Slider
    QHBoxLayout* sliderRow = new QHBoxLayout;
    QLabel* widthLbl = new QLabel(QString("Right Panel Width: %1%").arg(layout_.sidebarPct));
    widthLbl->setFixedWidth(160);
    QSlider* widthSlider = new QSlider(Qt::Horizontal);
    widthSlider->setRange(15, 60);
    widthSlider->setValue(layout_.sidebarPct);
    connect(widthSlider, &QSlider::valueChanged, this, [this, widthLbl, panelsLay, leftCol, rightCol](int val) {
        widthLbl->setText(QString("Right Panel Width: %1%").arg(val));
        layout_.sidebarPct = val;
        panelsLay->setStretch(0, 100 - val);
        panelsLay->setStretch(1, val);
        page_->applyAndSaveLayout(layout_);
    });
    sliderRow->addWidget(widthLbl);
    sliderRow->addWidget(widthSlider, 1);
    panelsMainLay->addLayout(sliderRow);

    main->addWidget(panelsBox);

    // ── Close button ─────────────────────────────────────────────
    QHBoxLayout* bottom = new QHBoxLayout;
    bottom->addStretch(1);
    QPushButton* closeBtn = new QPushButton("Close");
    closeBtn->setDefault(true);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    bottom->addWidget(closeBtn);
    main->addLayout(bottom);
}

void EditSessionLayoutDialog::toggleGpName(bool on) {
    layout_.showGpName = on;
    page_->applyAndSaveLayout(layout_);
}

void EditSessionLayoutDialog::toggleMarshalZones(bool on) {
    layout_.showMarshalZones = on;
    page_->applyAndSaveLayout(layout_);
}

void EditSessionLayoutDialog::toggleTimeLeft(bool on) {
    layout_.showTimeLeft = on;
    page_->applyAndSaveLayout(layout_);
}

void EditSessionLayoutDialog::toggleCard(int idx, bool on) {
    layout_.cards[idx] = on;
    page_->applyAndSaveLayout(layout_);
}

void EditSessionLayoutDialog::toggleMap(bool on) {
    layout_.showMap = on;
    page_->applyAndSaveLayout(layout_);
}

void EditSessionLayoutDialog::toggleProximity(bool on) {
    layout_.showProximity = on;
    page_->applyAndSaveLayout(layout_);
}

void EditSessionLayoutDialog::toggleEvents(bool on) {
    layout_.showEvents = on;
    page_->applyAndSaveLayout(layout_);
}

void EditSessionLayoutDialog::toggleWeather(bool on) {
    layout_.showWeather = on;
    page_->applyAndSaveLayout(layout_);
}
