#pragma once

#include <QDialog>
#include <QVector>
#include "OverviewLayout.h"

class MainWindow;
struct UdpForwardTargetSetting;
class QLabel;
class QCheckBox;
class QRadioButton;
class QComboBox;
class QSlider;
class QSpinBox;
class QLineEdit;
class QFormLayout;
class QWidget;
class QTableWidget;
class QPushButton;
class QTimer;

// Modal Settings dialog, opened from its own toolbar icon. Underline tabs
// matching the main toolbar's page switcher group the controls into category
// tabs (Recording, Appearance, …), each tab a label/control form. Most controls
// apply immediately; Network keeps a draft until its own Apply & Restart action.
class SettingsDialog : public QDialog {
    Q_OBJECT

public:
    explicit SettingsDialog(MainWindow* mainWindow, QWidget* parent = nullptr);

private:
    // Each builds and returns a self-contained settings tab page.
    QWidget* buildProtocolPage();
    QWidget* buildRecordingPage();
    QWidget* buildAppearancePage();
    QWidget* buildCompactPage();
    QWidget* buildGraphsPage();
    QWidget* buildYAxisPage();
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

    void updateProtocolWarning(int detectedFormat, int forcedFormat);
    void addForwardTargetRow(const QString& address = QString(), int port = 20777);
    void removeForwardTargetRow(QWidget* addressEditor);
    QVector<UdpForwardTargetSetting> forwardTargetDraft() const;
    bool networkDraftValid() const;
    bool networkDraftDirty() const;
    void refreshNetworkDraftUi();
    void applyNetworkDraft();

    enum class UdpApplyState { Idle, Applying, Ok, Error };

    MainWindow*   mainWindow_;
    QComboBox*    protocolCombo_         = nullptr;
    QLabel*       detectedProtocolLabel_ = nullptr;
    QSpinBox*     udpPortSpin_           = nullptr;
    QLineEdit*    udpBindAddressEdit_    = nullptr;
    QCheckBox*    udpForwardingCheck_    = nullptr;
    QWidget*      udpForwardEditor_      = nullptr;
    QTableWidget* udpForwardTargets_     = nullptr;
    QPushButton*  udpAddForwardTarget_   = nullptr;
    QLabel*       udpForwardValidation_  = nullptr;
    QLabel*       udpApplyStatus_        = nullptr;
    QPushButton*  udpApplyButton_        = nullptr;
    QTimer*       udpStatusResetTimer_   = nullptr;
    UdpApplyState udpApplyState_         = UdpApplyState::Idle;
    QString       udpApplyError_;
    QLabel*       protocolWarningLabel_  = nullptr;
    QLabel*       dirLabel_           = nullptr;
    QCheckBox*    recordCheck_        = nullptr;
    QRadioButton* themeSystem_        = nullptr;
    QRadioButton* themeLight_         = nullptr;
    QRadioButton* themeDark_          = nullptr;
    QComboBox*    styleCombo_         = nullptr;
    QCheckBox*    toolbarLabelsCheck_ = nullptr;
    QComboBox*    chartMsaaCombo_     = nullptr;
    QComboBox*    chartBackendCombo_  = nullptr;
    QComboBox*    chartFpsInFocusCombo_ = nullptr;
    QComboBox*    chartFpsOutOfFocusCombo_ = nullptr;
    QCheckBox*    toastsCheck_        = nullptr;
    QComboBox*    toastDurationCombo_ = nullptr;
    QComboBox*    tyreViewCombo_       = nullptr;
    QComboBox*    tyreWearModeCombo_   = nullptr;
    QComboBox*    trackMapLabelsCombo_ = nullptr;
    QComboBox*    trackMapIdleCombo_   = nullptr;
    QCheckBox*    trackMapSectorColorsCheck_ = nullptr;
    QSlider*      trackMapOpacitySlider_     = nullptr;
};
