#pragma once

#include <QDialog>
#include "OverviewLayout.h"

class MainWindow;
class QLabel;
class QCheckBox;
class QRadioButton;
class QComboBox;
class QSlider;
class QFormLayout;

// Modal Settings dialog, opened from its own toolbar icon. A single flat
// label/control form (no sidebar, no boxed groups) — bold section headers
// span the full row, every other row is a right-aligned label beside its
// control, native-preferences style. Immediate-apply, same as
// EditOverviewLayoutDialog — every control updates MainWindow and persists
// right away, so there's only a Close button.
class SettingsDialog : public QDialog {
    Q_OBJECT

public:
    explicit SettingsDialog(MainWindow* mainWindow, QWidget* parent = nullptr);

private:
    void addRecordingSection(QFormLayout* form);
    void addAppearanceSection(QFormLayout* form);
    void addNotificationsSection(QFormLayout* form);
    void addOverviewSection(QFormLayout* form);
    void addTrackMapSection(QFormLayout* form);

    MainWindow*   mainWindow_;
    QLabel*       dirLabel_           = nullptr;
    QCheckBox*    recordCheck_        = nullptr;
    QRadioButton* themeSystem_        = nullptr;
    QRadioButton* themeLight_         = nullptr;
    QRadioButton* themeDark_          = nullptr;
    QComboBox*    styleCombo_         = nullptr;
    QCheckBox*    toolbarLabelsCheck_ = nullptr;
    QCheckBox*    toastsCheck_        = nullptr;
    QComboBox*    toastDurationCombo_ = nullptr;
    QComboBox*    tyreViewCombo_       = nullptr;
    QComboBox*    tyreWearModeCombo_   = nullptr;
    QComboBox*    trackMapLabelsCombo_ = nullptr;
    QComboBox*    trackMapIdleCombo_   = nullptr;
    QCheckBox*    trackMapSectorColorsCheck_ = nullptr;
    QSlider*      trackMapOpacitySlider_     = nullptr;
};
