#include "Diagnostics.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QMutex>
#include <QMutexLocker>
#include <QOperatingSystemVersion>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTextStream>
#include <QtGlobal>

#include <cstdio>
#include <cstdlib>
#include <exception>

namespace {
QFile logFile;
QMutex logMutex;
std::function<void()> fatalFlush;
bool handlingMessage = false;
std::terminate_handler previousTerminate = nullptr;

void runFatalFlush() {
    std::function<void()> handler;
    {
        QMutexLocker lock(&logMutex);
        handler = fatalFlush;
    }
    if (handler) handler();
}

const char* levelName(QtMsgType type) {
    switch (type) {
        case QtDebugMsg: return "DEBUG";
        case QtInfoMsg: return "INFO";
        case QtWarningMsg: return "WARN";
        case QtCriticalMsg: return "ERROR";
        case QtFatalMsg: return "FATAL";
    }
    return "INFO";
}

void appendLine(const QString& level, const QString& message) {
    QMutexLocker lock(&logMutex);
    if (!logFile.isOpen() || handlingMessage) return;
    handlingMessage = true;
    QTextStream stream(&logFile);
    stream << QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)
           << " [" << QCoreApplication::applicationPid() << "] "
           << level.leftJustified(5) << ' ' << message << '\n';
    stream.flush();
    handlingMessage = false;
}

void messageHandler(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    QString detail = message;
    if (context.file)
        detail += QString(" (%1:%2)").arg(QString::fromUtf8(context.file)).arg(context.line);
    appendLine(QString::fromLatin1(levelName(type)), detail);
    std::fprintf(stderr, "%s\n", qUtf8Printable(message));
    if (type == QtFatalMsg) {
        runFatalFlush();
        std::abort();
    }
}
}

namespace tnr::diagnostics {

void initialize() {
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
        + "/launch-diagnostics";
    QDir previous(directory);
    if (previous.exists()) previous.removeRecursively();
    QDir().mkpath(directory);
    logFile.setFileName(directory + "/main.log");
    logFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text);
    qInstallMessageHandler(messageHandler);
    previousTerminate = std::set_terminate([] {
        appendLine("FATAL", "unhandled C++ exception / std::terminate");
        runFatalFlush();
        std::abort();
    });

    appendLine("INFO", QString("launch log: %1").arg(logFile.fileName()));
    appendLine("INFO", "previous launch diagnostics deleted");
    appendLine("INFO", QString("app version: %1").arg(QApplication::applicationVersion()));
    appendLine("INFO", QString("Qt: %1").arg(QString::fromLatin1(qVersion())));
    appendLine("INFO", QString("platform: %1 %2").arg(QSysInfo::productType(), QSysInfo::currentCpuArchitecture()));
    appendLine("INFO", QString("OS: %1").arg(QOperatingSystemVersion::current().name()));
    appendLine("INFO", QString("kernel: %1 %2").arg(QSysInfo::kernelType(), QSysInfo::kernelVersion()));
    appendLine("INFO", QString("executable: %1").arg(QCoreApplication::applicationFilePath()));
    appendLine("INFO", QString("working directory: %1").arg(QDir::currentPath()));
    appendLine("INFO", QString("command line: %1").arg(QCoreApplication::arguments().join(' ')));
    appendLine("INFO", QString("locale: %1").arg(QLocale().name()));
}

void setFatalFlushHandler(std::function<void()> handler) {
    QMutexLocker lock(&logMutex);
    fatalFlush = std::move(handler);
}

QString failureReport(const QString& heading, const QString& details) {
    const QString report = QString(
        "%1\nApp version: %2\nQt: %3\nPlatform: %4 %5\nOS: %6\nLog: %7\n\n%8")
        .arg(heading, QApplication::applicationVersion(), QString::fromLatin1(qVersion()),
             QSysInfo::productType(), QSysInfo::currentCpuArchitecture(),
             QOperatingSystemVersion::current().name(), logFile.fileName(), details);
    QString logMessage = report;
    appendLine("FATAL", logMessage.replace('\n', " | "));
    return report;
}

QString logPath() { return logFile.fileName(); }

void shutdown() {
    appendLine("INFO", "process exit");
    setFatalFlushHandler({});
    qInstallMessageHandler(nullptr);
    if (previousTerminate) std::set_terminate(previousTerminate);
    QMutexLocker lock(&logMutex);
    if (logFile.isOpen()) logFile.close();
}

}
