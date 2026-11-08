#pragma once

#include <QLabel>
#include <QPointer>
#include <QString>
#include <QVBoxLayout>
#include <QVector>
#include <QWidget>

#include <array>
#include <optional>

#include <tnrp/NetworkSetup.h>

#include "../CompactSettings.h"

class MainWindow;
class QPushButton;
class QScrollArea;
class SetupStepDots;

// First-time setup, as Electron's FirstTimeSetup.tsx: one step per screen in a
// centred 560px column over the whole window, with Back, the step dots and
// "N of 6", Skip and Continue pinned to the bottom. Continue (and moving away
// with the dots or Back) saves a step's choices; Skip keeps the saved settings.
// Qt leaves out Electron's Theme step.
//
// Steps: Recordings → Density → Connect the game → Check connection →
// Paired mode → Summary. Network adapters and the firewall come from
// tnrp/NetworkSetup.h, shared with Electron.
class FirstTimeSetup : public QWidget {
    Q_OBJECT

public:
    FirstTimeSetup(MainWindow* mainWindow, QWidget* parent);

    // Covers the parent and shows the first step.
    void open();

signals:
    void finished();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    enum Step { Recordings, Density, Game, Check, Pairing, Summary, StepCount };
    enum class Status { Todo, Done, Skipped };

    void showStep(int index);
    void leaveStep(int target, Status outcome);
    void commitStep(Step step);
    void finish();
    void refreshFooter();

    QWidget* buildRecordingsStep();
    QWidget* buildDensityStep();
    QWidget* buildGameStep();
    QWidget* buildCheckStep();
    QWidget* buildPairingStep();
    QWidget* buildSummaryStep();

    // Paired mode: rebuilds the part under the toggle.
    void refreshPairingBody();
    // Recordings: "N GB free on D:" for the chosen folder.
    void refreshFreeSpace();

    bool gameStepValid() const;

    MainWindow* mainWindow_;
    int stepIndex_ = 0;
    std::array<Status, StepCount> statuses_{};

    QScrollArea* scroll_ = nullptr;
    QWidget* stepPage_ = nullptr;
    QPushButton* backButton_ = nullptr;
    QPushButton* skipButton_ = nullptr;
    QPushButton* continueButton_ = nullptr;
    SetupStepDots* dots_ = nullptr;
    QLabel* stepCount_ = nullptr;

    // ── Recordings ──
    bool recordEnabled_ = true;
    QString recordDirectory_;
    std::array<QString, 4> scopes_;   // practice, qualifying, race, time_trial
    QPointer<QLabel> freeSpaceLabel_;

    // ── Density ──
    tnr::DensityMode density_ = tnr::DensityMode::Normal;

    // ── Network ──
    QVector<tnrp::NetworkInterfaceInfo> interfaces_;
    bool interfacesLoaded_ = false;
    QString selectedAddress_;
    bool showAdvanced_ = false;
    bool gameIsF1_24_ = false;
    int port_ = 20777;
    QString bindAddress_;
    QPointer<QLabel> ipValue_;   // the settings list's UDP IP Address

    // ── Paired mode ──
    QPointer<QVBoxLayout> pairingBody_;
};
