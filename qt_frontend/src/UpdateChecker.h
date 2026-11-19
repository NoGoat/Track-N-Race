#pragma once

#include <QObject>
#include <QString>

#include <tnrp/Updates.h>

class QFile;
class QNetworkAccessManager;
class QProgressDialog;
class QWidget;

// Startup update check. Asks the update service for the newest signed release
// on every start (tnrp/Updates.h verifies it); without the service, falls back
// to the GitHub release tag at most once a day. "Remind me later" hides offers
// for a day. A Windows installer build can then download the new setup and
// verify it against the signed SHA-512; "Restart to update" reports the update
// to the service, runs the setup silently and quits, and the setup relaunches
// the app. Other builds only open the download page.
class UpdateChecker final : public QObject {
    Q_OBJECT
public:
    explicit UpdateChecker(QWidget* dialogParent);
    void checkOnStartup();

private:
    void checkService(const QString& serviceUrl, const QString& current);
    void checkGitHub(const QString& current);
    void showOffer(const QString& current, const QString& latest, bool installable);
    void startInstall();
    void verifyDownload(const QString& installer);
    void offerRestart(const QString& installer);
    void restartToUpdate(const QString& installer);
    void launchInstaller(const QString& installer);
    void failInstall(const QString& error);

    QWidget* dialogParent_ = nullptr;
    QNetworkAccessManager* network_ = nullptr;
    tnrp::UpdateOffer pending_;
    bool hasPending_ = false;
    // Where "Restart to update" reports the update (tnrp::updateInstallPath).
    QString pendingCountUrl_;
    QProgressDialog* progress_ = nullptr;
    QFile* download_ = nullptr;
};
