#include "UpdateChecker.h"

#include <QApplication>
#include <QDateTime>
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QProcess>
#include <QProgressDialog>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

#include <thread>

namespace {
constexpr qint64 kCheckIntervalMs = 24LL * 60 * 60 * 1000;
const QUrl kReleaseApi("https://api.github.com/repos/NoGoat/Track-N-Race/releases/latest");
const QUrl kReleasePage("https://github.com/NoGoat/Track-N-Race/releases/latest");
constexpr const char* kProduct = "qt";

struct Target {
    const char* os;
    const char* arch;
    const char* format;
};

Target platformTarget() {
#if defined(Q_PROCESSOR_ARM_64)
    constexpr const char* arch = "arm64";
#else
    constexpr const char* arch = "x64";
#endif
#if defined(Q_OS_WIN)
    return {"windows", arch, "installer"};
#elif defined(Q_OS_MACOS)
    return {"macos", arch, "dmg"};
#else
    return {"linux", arch, "appimage"};
#endif
}

std::string utf8(const QString& value) { return value.toUtf8().toStdString(); }
QString qstr(const std::string& value) { return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())); }

// A per-user NSIS install of this exact folder (installer.nsi records it).
// The portable zip and dev builds only get the download page.
bool isInstalledWindowsBuild() {
#if defined(Q_OS_WIN)
    QSettings uninstall(
        "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Track-N-Race-Qt",
        QSettings::NativeFormat);
    const QString location = uninstall.value("InstallLocation").toString();
    if (location.isEmpty()) return false;
    const QString installed = QFileInfo(location).canonicalFilePath();
    const QString running = QFileInfo(QCoreApplication::applicationDirPath()).canonicalFilePath();
    return !installed.isEmpty() && installed.compare(running, Qt::CaseInsensitive) == 0;
#else
    return false;
#endif
}

QString downloadDirectory() {
    return QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation)).filePath("TrackNRace-update");
}

// The setup an earlier update ran. While that installer is still finishing (it
// relaunches the app before it exits) its file is locked; the next launch
// removes it instead.
void removeOldDownloads() {
    QDir directory(downloadDirectory());
    for (const QString& name : directory.entryList(QDir::Files)) directory.remove(name);
}

bool isSkipped(const QString& version) {
    QSettings settings("TrackNRace", "NativeRecorder");
    const QString skipped = settings.value("updates/skippedVersion").toString();
    return !skipped.isEmpty() && tnrp::compareVersions(utf8(skipped), utf8(version)) == 0;
}
}

UpdateChecker::UpdateChecker(QWidget* dialogParent)
    : QObject(dialogParent), dialogParent_(dialogParent),
      network_(new QNetworkAccessManager(this)) {}

void UpdateChecker::checkOnStartup() {
    removeOldDownloads();
    QSettings settings("TrackNRace", "NativeRecorder");
    if (!settings.value("updates/enabled", true).toBool()) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!tnrp::updateServiceOverridden() &&
        now - settings.value("updates/lastCheckAt", 0).toLongLong() < kCheckIntervalMs)
        return;
    // Store the attempt first so an outage does not retry on every launch.
    settings.setValue("updates/lastCheckAt", now);

    const QString current = qstr(tnrp::normalizedVersion(utf8(QApplication::applicationVersion())));
    const QString serviceUrl = qstr(tnrp::updateServiceUrl());
    if (serviceUrl.isEmpty()) checkGitHub(current);
    else checkService(serviceUrl, current);
}

void UpdateChecker::checkService(const QString& serviceUrl, const QString& current) {
    const Target target = platformTarget();
    const QUrl url(serviceUrl + qstr(tnrp::updateCheckPath(kProduct, target.os, target.arch, target.format,
                                                           utf8(current))));
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", "Track-N-Race");
    QNetworkReply* reply = network_->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, current, target] {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() == QNetworkReply::NoError && status == 204) return;  // up to date
        if (reply->error() != QNetworkReply::NoError || status != 200) {
            qWarning().noquote() << "[updates] update service check failed, falling back to GitHub:"
                                 << reply->errorString();
            checkGitHub(current);
            return;
        }
        const QByteArray body = reply->readAll();
        tnrp::UpdateOffer offer = tnrp::evaluateUpdate(std::string_view(body.constData(), body.size()), kProduct,
                                                       target.os, target.arch, target.format, utf8(current));
        if (offer.status == "invalid") {
            qWarning().noquote() << "[updates] rejected update service response:" << qstr(offer.error);
            checkGitHub(current);
            return;
        }
        const QString latest = qstr(offer.version);
        if (latest.isEmpty() || tnrp::compareVersions(offer.version, utf8(current)) != 1 || isSkipped(latest))
            return;
        hasPending_ = offer.status == "available";
        pending_ = std::move(offer);
        showOffer(current, latest, hasPending_ && isInstalledWindowsBuild());
    });
}

void UpdateChecker::checkGitHub(const QString& current) {
    QNetworkRequest request(kReleaseApi);
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("User-Agent", "Track-N-Race");
    request.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
    QNetworkReply* reply = network_->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, current] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            qWarning().noquote() << "[updates] update check failed:" << reply->errorString();
            return;
        }
        const QJsonObject body = QJsonDocument::fromJson(reply->readAll()).object();
        if (!body.value("tag_name").isString()) {
            qWarning() << "[updates] latest release did not include a tag name";
            return;
        }
        const QString latest = qstr(tnrp::normalizedVersion(utf8(body.value("tag_name").toString())));
        const auto order = tnrp::compareVersions(utf8(latest), utf8(current));
        if (!order) {
            qWarning().noquote() << "[updates] could not compare versions" << current << latest;
            return;
        }
        if (*order <= 0 || isSkipped(latest)) return;
        hasPending_ = false;
        showOffer(current, latest, false);
    });
}

void UpdateChecker::showOffer(const QString& current, const QString& latest, bool installable) {
    QMessageBox dialog(QMessageBox::Information, "Update Available",
        QString("Track N Race %1 is available.").arg(latest), QMessageBox::NoButton, dialogParent_);
    const QString versions = QString("Installed: %1\nLatest: %2").arg(current, latest);
    dialog.setInformativeText(installable
        ? versions
        : QString("Download opens the GitHub Releases page.\n\n%1").arg(versions));
    QPushButton* skip = dialog.addButton("Skip this version", QMessageBox::DestructiveRole);
    dialog.addButton("Remind me Later", QMessageBox::RejectRole);
    QPushButton* accept = dialog.addButton(installable ? "Update now" : "Download", QMessageBox::AcceptRole);
    dialog.setDefaultButton(accept);
    dialog.exec();
    if (dialog.clickedButton() == skip) {
        QSettings("TrackNRace", "NativeRecorder").setValue("updates/skippedVersion", latest);
    } else if (dialog.clickedButton() == accept) {
        if (installable) startInstall();
        else QDesktopServices::openUrl(kReleasePage);
    }
}

void UpdateChecker::startInstall() {
    if (!hasPending_ || progress_) return;
    const QString directory = downloadDirectory();
    if (!QDir().mkpath(directory)) {
        failInstall("Could not create the download folder.");
        return;
    }
    const QString installer = QDir(directory).filePath(qstr(pending_.fileName));
    const QString partial = installer + ".part";
    QFile::remove(partial);
    download_ = new QFile(partial, this);
    if (!download_->open(QIODevice::WriteOnly)) {
        delete download_;
        download_ = nullptr;
        failInstall("Could not write the download.");
        return;
    }

    const qint64 total = static_cast<qint64>(pending_.size);
    progress_ = new QProgressDialog("Downloading update…", "Cancel", 0, 1000, dialogParent_);
    progress_->setWindowTitle("Updating Track N Race");
    progress_->setWindowModality(Qt::WindowModal);
    progress_->setMinimumDuration(0);
    progress_->setAutoClose(false);
    progress_->setAutoReset(false);
    progress_->setValue(0);

    QNetworkRequest request{QUrl(qstr(pending_.url))};
    request.setRawHeader("User-Agent", "Track-N-Race");
    QNetworkReply* reply = network_->get(request);
    connect(progress_, &QProgressDialog::canceled, reply, &QNetworkReply::abort);
    connect(reply, &QNetworkReply::readyRead, this, [this, reply, total] {
        if (!download_) return;
        download_->write(reply->readAll());
        if (download_->size() > total) reply->abort();
    });
    connect(reply, &QNetworkReply::downloadProgress, this, [this, total](qint64 received, qint64) {
        if (!progress_ || total <= 0) return;
        progress_->setValue(static_cast<int>(qMin<qint64>(1000, received * 1000 / total)));
        progress_->setLabelText(QString("Downloading update… %1 / %2 MB")
            .arg(received / 1048576.0, 0, 'f', 1).arg(total / 1048576.0, 0, 'f', 1));
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, installer, partial, total] {
        reply->deleteLater();
        if (download_) {
            download_->write(reply->readAll());
            download_->close();
        }
        const qint64 written = download_ ? download_->size() : -1;
        delete download_;
        download_ = nullptr;
        if (reply->error() == QNetworkReply::OperationCanceledError && written <= total) {
            QFile::remove(partial);
            if (progress_) progress_->deleteLater();
            progress_ = nullptr;
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            QFile::remove(partial);
            failInstall(written > total ? "The download is larger than the signed size."
                                        : "The download failed: " + reply->errorString());
            return;
        }
        QFile::remove(installer);
        if (!QFile::rename(partial, installer)) {
            QFile::remove(partial);
            failInstall("Could not save the download.");
            return;
        }
        verifyAndLaunch(installer);
    });
}

void UpdateChecker::verifyAndLaunch(const QString& installer) {
    if (progress_) {
        progress_->setLabelText("Verifying…");
        progress_->setCancelButton(nullptr);
        progress_->setValue(1000);
    }
    const std::string path = utf8(installer);
    const std::string sha512 = pending_.sha512;
    const uint64_t size = pending_.size;
    QPointer<UpdateChecker> self(this);
    // Hashing a large installer would stall the UI; do it off the main thread.
    std::thread([self, path, sha512, size, installer] {
        const QString mismatch = qstr(tnrp::verifyUpdateFile(path, sha512, size));
        QMetaObject::invokeMethod(qApp, [self, mismatch, installer] {
            if (!self) return;
            if (!mismatch.isEmpty()) {
                QFile::remove(installer);
                self->failInstall(mismatch);
                return;
            }
            if (self->progress_) self->progress_->setLabelText("Starting installer…");
            // installer.nsi: /S installs silently, /UPDATE waits for this app to
            // exit and relaunches it afterwards.
            if (!QProcess::startDetached(installer, {"/S", "/UPDATE"})) {
                self->failInstall("Could not start the installer.");
                return;
            }
            qInfo().noquote() << "[updates] started the" << qstr(self->pending_.version) << "installer; quitting";
            QTimer::singleShot(250, qApp, &QCoreApplication::quit);
        }, Qt::QueuedConnection);
    }).detach();
}

void UpdateChecker::failInstall(const QString& error) {
    qWarning().noquote() << "[updates] update failed:" << error;
    if (progress_) progress_->deleteLater();
    progress_ = nullptr;
    QMessageBox dialog(QMessageBox::Warning, "Update Failed", "The update could not be installed.",
                       QMessageBox::NoButton, dialogParent_);
    dialog.setInformativeText(error);
    dialog.addButton(QMessageBox::Close);
    QPushButton* download = dialog.addButton("Open Download Page", QMessageBox::AcceptRole);
    dialog.exec();
    if (dialog.clickedButton() == download) QDesktopServices::openUrl(kReleasePage);
}
