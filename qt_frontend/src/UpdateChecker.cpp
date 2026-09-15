#include "UpdateChecker.h"

#include <QApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QStringList>
#include <QUrl>

namespace {
constexpr qint64 kCheckIntervalMs = 24LL * 60 * 60 * 1000;
const QUrl kReleaseApi("https://api.github.com/repos/NoGoat/Track-N-Race/releases/latest");
const QUrl kReleasePage("https://github.com/NoGoat/Track-N-Race/releases/latest");

struct Version {
    int core[3] = {};
    QStringList prerelease;
};

QString normalizedVersion(QString value) {
    value = value.trimmed();
    if (value.startsWith('v', Qt::CaseInsensitive) && value.size() > 1 && value[1].isDigit())
        value.remove(0, 1);
    return value;
}

bool parseVersion(const QString& input, Version& out) {
    static const QRegularExpression expression(
        R"(^v?(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z.-]+))?(?:\+[0-9A-Za-z.-]+)?$)");
    const auto match = expression.match(input.trimmed());
    if (!match.hasMatch()) return false;
    for (int i = 0; i < 3; ++i) out.core[i] = match.captured(i + 1).toInt();
    out.prerelease = match.captured(4).isEmpty()
        ? QStringList{} : match.captured(4).split('.');
    return true;
}

int compareVersions(const QString& leftText, const QString& rightText, bool& valid) {
    Version left, right;
    valid = parseVersion(leftText, left) && parseVersion(rightText, right);
    if (!valid) return 0;
    for (int i = 0; i < 3; ++i)
        if (left.core[i] != right.core[i]) return left.core[i] > right.core[i] ? 1 : -1;
    if (left.prerelease.isEmpty() != right.prerelease.isEmpty())
        return left.prerelease.isEmpty() ? 1 : -1;
    const int count = qMax(left.prerelease.size(), right.prerelease.size());
    for (int i = 0; i < count; ++i) {
        if (i >= left.prerelease.size()) return -1;
        if (i >= right.prerelease.size()) return 1;
        if (left.prerelease[i] == right.prerelease[i]) continue;
        bool leftNumber = false, rightNumber = false;
        const int leftValue = left.prerelease[i].toInt(&leftNumber);
        const int rightValue = right.prerelease[i].toInt(&rightNumber);
        if (leftNumber && rightNumber) return leftValue > rightValue ? 1 : -1;
        if (leftNumber != rightNumber) return leftNumber ? -1 : 1;
        return QString::compare(left.prerelease[i], right.prerelease[i], Qt::CaseSensitive) > 0 ? 1 : -1;
    }
    return 0;
}
}

UpdateChecker::UpdateChecker(QWidget* dialogParent)
    : QObject(dialogParent), dialogParent_(dialogParent),
      network_(new QNetworkAccessManager(this)) {}

void UpdateChecker::checkOnStartup() {
    QSettings settings("TrackNRace", "NativeRecorder");
    if (!settings.value("updates/enabled", true).toBool()) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - settings.value("updates/lastCheckAt", 0).toLongLong() < kCheckIntervalMs) return;
    // Store the attempt first so an outage does not retry on every launch.
    settings.setValue("updates/lastCheckAt", now);

    QNetworkRequest request(kReleaseApi);
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("User-Agent", "Track-N-Race");
    request.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
    QNetworkReply* reply = network_->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
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
        const QString latest = normalizedVersion(body.value("tag_name").toString());
        const QString current = normalizedVersion(QApplication::applicationVersion());
        bool valid = false;
        if (compareVersions(latest, current, valid) <= 0) {
            if (!valid) qWarning().noquote() << "[updates] could not compare versions" << current << latest;
            return;
        }
        QSettings settings("TrackNRace", "NativeRecorder");
        if (normalizedVersion(settings.value("updates/skippedVersion").toString()) == latest) return;

        QMessageBox dialog(QMessageBox::Information, "Update Available",
            QString("Track N Race %1 is available.").arg(latest), QMessageBox::NoButton,
            dialogParent_);
        dialog.setInformativeText(
            QString("Automatic updates are not supported. Download opens the GitHub Releases page.\n\n"
                    "Installed: %1\nLatest: %2").arg(current, latest));
        QPushButton* skip = dialog.addButton("Skip this version", QMessageBox::DestructiveRole);
        dialog.addButton("Remind me Later", QMessageBox::RejectRole);
        QPushButton* download = dialog.addButton("Download", QMessageBox::AcceptRole);
        dialog.exec();
        if (dialog.clickedButton() == skip)
            settings.setValue("updates/skippedVersion", latest);
        else if (dialog.clickedButton() == download)
            QDesktopServices::openUrl(kReleasePage);
    });
}
