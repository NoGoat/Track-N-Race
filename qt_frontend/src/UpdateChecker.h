#pragma once

#include <QObject>
#include <QString>

#include <tnrp/Updates.h>

class QFile;
class QNetworkAccessManager;
class QProgressDialog;
class QWidget;

// Startup update check. Asks the update service for the newest signed release
// (tnrp/Updates.h verifies it); without the service, falls back to the GitHub
// release tag. A Windows installer build can then download the new setup,
// verify it against the signed SHA-512, run it silently and quit; the setup
// relaunches the app. Other builds only open the download page.
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
    void verifyAndLaunch(const QString& installer);
    void failInstall(const QString& error);

    QWidget* dialogParent_ = nullptr;
    QNetworkAccessManager* network_ = nullptr;
    tnrp::UpdateOffer pending_;
    bool hasPending_ = false;
    QProgressDialog* progress_ = nullptr;
    QFile* download_ = nullptr;
};
