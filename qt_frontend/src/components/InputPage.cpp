#include "InputPage.h"
#include "InputChartsWidget.h"

#include <QVBoxLayout>

InputPage::InputPage(SessionModel* model, QWidget* parent)
    : QWidget(parent)
{
    QVBoxLayout* vbox = new QVBoxLayout(this);
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(0);

    // Gear / throttle-brake / steering are now panels of one ChartView (a single
    // QRhi render target / repaint) — see InputChartsWidget.
    charts_ = new InputChartsWidget;
    charts_->setModel(model);
    vbox->addWidget(charts_, 1);

    charts_->setPageLayout(pageLayout());
    charts_->setPedalLayout(pedalLayout());
    applyLayout(loadLayout());
}

InputLayout InputPage::loadLayout()
{
    InputLayout L;
    settings_.beginGroup("inputLayout");
    L.showGear = settings_.value("showGear", true).toBool();
    const bool legacyPedals = settings_.value("showInputs", true).toBool();
    L.showAccelerator = settings_.value("showAccelerator", legacyPedals).toBool();
    L.showBrake = settings_.value("showBrake", legacyPedals).toBool();
    L.showSteering = settings_.value("showSteering", true).toBool();
    settings_.endGroup();
    return L;
}

void InputPage::saveLayout(const InputLayout& L)
{
    settings_.beginGroup("inputLayout");
    settings_.setValue("showGear", L.showGear);
    settings_.setValue("showAccelerator", L.showAccelerator);
    settings_.setValue("showBrake", L.showBrake);
    settings_.setValue("showSteering", L.showSteering);
    settings_.endGroup();
}

void InputPage::applyLayout(const InputLayout& L)
{
    if (!charts_) return;
    const InputPedalLayout pedals = pedalLayout();
    charts_->setPedalVisibility(L.showAccelerator, L.showBrake);
    charts_->setSectionVisible(0, L.showGear);
    charts_->setSectionVisible(1, pedals == InputPedalLayout::Combined &&
                                   (L.showAccelerator || L.showBrake));
    charts_->setSectionVisible(2, L.showSteering);
    charts_->setSectionVisible(3, pedals == InputPedalLayout::Combined2 &&
                                   (L.showAccelerator || L.showBrake));
    charts_->setSectionVisible(4, pedals == InputPedalLayout::Split && L.showAccelerator);
    charts_->setSectionVisible(5, pedals == InputPedalLayout::Split && L.showBrake);
}

void InputPage::applyAndSaveLayout(const InputLayout& L)
{
    applyLayout(L);
    saveLayout(L);
    emit layoutChanged();
}

InputPageLayout InputPage::pageLayout() const
{
    return settings_.value("pageLayouts/input", "grid").toString() == "vertical"
        ? InputPageLayout::Vertical : InputPageLayout::Grid;
}

InputPedalLayout InputPage::pedalLayout() const
{
    const QString value = settings_.value("pageLayouts/inputPedals", "combined").toString();
    if (value == "split") return InputPedalLayout::Split;
    if (value == "combined2") return InputPedalLayout::Combined2;
    return InputPedalLayout::Combined;
}

void InputPage::setPageLayout(InputPageLayout layout)
{
    settings_.setValue("pageLayouts/input",
                       layout == InputPageLayout::Vertical ? "vertical" : "grid");
    if (charts_) charts_->setPageLayout(layout);
    emit layoutChanged();
}

void InputPage::setPedalLayout(InputPedalLayout layout)
{
    const char* value = layout == InputPedalLayout::Split ? "split"
        : layout == InputPedalLayout::Combined2 ? "combined2" : "combined";
    settings_.setValue("pageLayouts/inputPedals", value);
    if (charts_) charts_->setPedalLayout(layout);
    applyLayout(loadLayout());
    emit layoutChanged();
}

QVector<tnr::GraphSection> InputPage::chartSections()
{
    const InputLayout layout = loadLayout();
    QVector<tnr::GraphSection> sections;
    if (layout.showGear) sections.append(tnr::GraphSection::InputGear);
    if (pedalLayout() == InputPedalLayout::Split) {
        if (layout.showAccelerator) sections.append(tnr::GraphSection::InputAccelerator);
        if (layout.showBrake) sections.append(tnr::GraphSection::InputBrake);
    } else if (layout.showAccelerator || layout.showBrake) {
        sections.append(pedalLayout() == InputPedalLayout::Combined2
            ? tnr::GraphSection::InputThrottleBrakeOverlay
            : tnr::GraphSection::InputThrottleBrake);
    }
    if (layout.showSteering) sections.append(tnr::GraphSection::InputSteering);
    return sections;
}

void InputPage::setPlaybackMode(bool on, float currentTime)
{
    if (charts_) charts_->setPlaybackMode(on);
    if (on) setCurrentTime(currentTime);
}

void InputPage::setCurrentTime(float t)
{
    if (charts_) charts_->setCurrentTime(t);
}

void InputPage::setWindowSeconds(float secs)
{
    if (charts_) charts_->setWindowSeconds(secs);
}

void InputPage::setGraphSectionTable(int section, bool table)
{
    if (charts_) charts_->setSectionViewMode(section, table);
}
