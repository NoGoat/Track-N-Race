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
class QWidget;

// Modal Settings dialog, opened from its own toolbar icon. A standard QTabWidget
// groups the controls into category tabs (Recording, Appearance, …), each tab a
// label/control form. Immediate-apply, same as EditOverviewLayoutDialog — every
// control updates MainWindow and persists right away, so there's only a Close
// button.
class SettingsDialog : public QDialog {
    Q_OBJECT

public:
    explicit SettingsDialog(MainWindow* mainWindow, QWidget* parent = nullptr);

private:
    // Each builds and returns a self-contained settings tab page.
    QWidget* buildRecordingPage();
    QWidget* buildAppearancePage();
    QWidget* buildNotificationsPage();
    QWidget* buildOverviewPage();
    QWidget* buildTrackMapPage();
    QWidget* buildAboutPage();

    // Shared page scaffold: a page whose body form is returned via formOut.
    QWidget* makePage(QFormLayout*& formOut);

    // Open the standalone About modal (reached from the footer's About button),
    // which hosts the buildAboutPage() content.
    void showAboutDialog();

    // Open a modal viewer showing the full text of an embedded license resource
    // (e.g. ":/licenses/GPL-3.0.txt") — reused by every "View license" button.
    void showLicenseText(const QString& title, const QString& resourcePath);

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
