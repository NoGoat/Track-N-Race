#pragma once

#include <QDialog>
#include <QHash>
#include <QStringList>
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
class QListWidget;
class QStackedWidget;
class QWidget;
class QTableWidget;
class QPushButton;
class QTimer;
class QVBoxLayout;

struct TeamColorPresetSetting {
    int id = -1;
    QString name;
    QString color;
    QString group;
};

// Settings, laid out like KDE's configuration dialogs:
//
//  ┌──────────────────┬──────────────────────────────────────────────┐
//  │ 🔍 Search         │  Page Title                                   │
//  │ ▣ Appearance      │  One-line description of what the page does. │
//  │ ▣ Team Colours    │                                              │
//  │ ▣ Density         │  SECTION                                     │
//  │ ▣ Layout          │           Label:  [control]                  │
//  │ ▣ Graphs          │                   muted hint                 │
//  │ …                 │                                              │
//  ├──────────────────┴──────────────────────────────────────────────┤
//  │ [About]                                               [Close]    │
//  └─────────────────────────────────────────────────────────────────┘
//
// Every page is one aligned form of titled sections. Most controls apply
// immediately; the Connection page keeps a draft until Apply & Restart.
class SettingsDialog : public QDialog {
    Q_OBJECT

public:
    explicit SettingsDialog(MainWindow* mainWindow, QWidget* parent = nullptr);

    // Select the Connection page (UDP port, bind address, forwarding).
    void showConnectionPage();

protected:
    void done(int result) override;

private:
    // Each builds and returns a page body (the frame adds its title).
    QWidget* buildAppearancePage();
    QWidget* buildTeamColorsPage();
    QWidget* buildCompactPage();
    QWidget* buildLayoutPage();
    QWidget* buildGraphsPage();
    QWidget* buildYAxisPage();
    QWidget* buildRenderingPage();
    QWidget* buildTrackMapPage();
    QWidget* buildRecordingPage();
    QWidget* buildNotificationsPage();
    QWidget* buildProtocolPage();
    QWidget* buildPairingPage();
    QWidget* buildDebugPage();
    QWidget* buildAboutPage();

    // Shared page body: one right-aligned label/control form, returned via
    // formOut, so label columns line up across all of a page's sections.
    QWidget* makePage(QFormLayout*& formOut);
    // Wraps a body with the page's title and description; `scrolls` wraps the
    // body in a scroll area (pages with their own scrolling pass false).
    QWidget* pageFrame(const QString& title, const QString& description, QWidget* body,
                       bool scrolls);
    // Shows only the categories whose pages mention every word of `query`.
    void filterPages(const QString& query);

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
    void refreshPairingUi();
    void loadTeamColorConfiguration();
    void refreshTeamColorRows();
    void commitTeamColorOverrides();

    enum class UdpApplyState { Idle, Applying, Ok, Error };

    MainWindow*   mainWindow_;
    QLineEdit*    search_                = nullptr;
    QListWidget*  nav_                   = nullptr;
    QStackedWidget* pages_               = nullptr;
    QLabel*       noMatches_             = nullptr;
    QStringList   pageKeywords_;          // lower-cased searchable text per page
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
    QCheckBox*    pairingEnabledCheck_       = nullptr;
    QWidget*      pairingContent_            = nullptr;
    QWidget*      pairingClosed_             = nullptr;
    QWidget*      pairingOpen_               = nullptr;
    QLabel*       pairingQrLabel_            = nullptr;
    QLabel*       pairingCodeLabel_          = nullptr;
    QLabel*       pairingErrorLabel_         = nullptr;
    QTableWidget* pairingDevicesTable_       = nullptr;
    QWidget*      teamColorRows_              = nullptr;
    QVBoxLayout*  teamColorRowsLayout_        = nullptr;
    QHash<int, QVector<TeamColorPresetSetting>> teamColorCatalog_;
    QHash<int, QHash<int, QString>> teamColorOverrides_;
    int teamColorFormat_ = 2026;
};
