#include "Diagnostics.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QMutex>
#include <QMutexLocker>
#include <QOperatingSystemVersion>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTextStream>
#include <QTimer>
#include <QtGlobal>

#include <cstdio>
#include <cstdlib>
#include <exception>

#ifdef Q_OS_WIN
#include <windows.h>
#include <psapi.h>
#elif defined(Q_OS_MACOS)
#include <mach/mach.h>
#elif !defined(Q_OS_LINUX)
#include <sys/resource.h>
#endif

namespace {
QFile logFile;
QFile ramUsageFile;
QMutex logMutex;
std::function<void()> fatalFlush;
std::function<QJsonObject()> memorySnapshotProvider;
QTimer* memoryTimer = nullptr;
QElapsedTimer memoryElapsed;
QString diagnosticsDirectory;
bool ramUsageLogOpened = false;
bool handlingMessage = false;
std::terminate_handler previousTerminate = nullptr;

void appendLine(const QString& level, const QString& message);

double jsonNumber(quint64 value) {
    return static_cast<double>(value);
}

QJsonObject processMemorySnapshot() {
    QJsonObject process;
    process["category"] = QStringLiteral("process");
    process["pid"] = jsonNumber(QCoreApplication::applicationPid());

#ifdef Q_OS_WIN
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) {
        process["resident_bytes"] = jsonNumber(counters.WorkingSetSize);
        process["peak_resident_bytes"] = jsonNumber(counters.PeakWorkingSetSize);
        process["private_bytes"] = jsonNumber(counters.PrivateUsage);
        process["pagefile_bytes"] = jsonNumber(counters.PagefileUsage);
    }
#elif defined(Q_OS_MACOS)
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
        process["resident_bytes"] = jsonNumber(info.resident_size);
        process["virtual_bytes"] = jsonNumber(info.virtual_size);
    }
#elif defined(Q_OS_LINUX)
    QFile status(QStringLiteral("/proc/self/status"));
    if (status.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const auto readKb = [](const QByteArray& line, const QByteArray& key) -> double {
            if (!line.startsWith(key)) return -1.0;
            const QList<QByteArray> fields = line.mid(key.size()).simplified().split(' ');
            bool ok = false;
            const quint64 kb = fields.isEmpty() ? 0 : fields.front().toULongLong(&ok);
            return ok ? jsonNumber(kb * 1024ULL) : -1.0;
        };
        while (!status.atEnd()) {
            const QByteArray line = status.readLine();
            const struct { const char* source; const char* target; } fields[] = {
                { "VmRSS:", "resident_bytes" },
                { "VmHWM:", "peak_resident_bytes" },
                { "VmSize:", "virtual_bytes" },
                { "RssAnon:", "anonymous_resident_bytes" },
            };
            for (const auto& field : fields) {
                const double value = readKb(line, field.source);
                if (value >= 0.0) process[field.target] = value;
            }
        }
    }
#else
    struct rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0)
        process["peak_resident_bytes"] = jsonNumber(usage.ru_maxrss * 1024ULL);
#endif
    return process;
}

void writeMemorySample() {
    if (!ramUsageFile.isOpen()) return;

    QJsonObject frontend;
    try {
        if (memorySnapshotProvider) frontend = memorySnapshotProvider();
    } catch (const std::exception& error) {
        frontend["snapshot_error"] = QString::fromUtf8(error.what());
    } catch (...) {
        frontend["snapshot_error"] = QStringLiteral("Unknown snapshot failure");
    }

    QJsonObject sample;
    sample["timestamp"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    sample["elapsed_ms"] = jsonNumber(memoryElapsed.isValid() ? memoryElapsed.elapsed() : 0);
    const QJsonObject process = processMemorySnapshot();
    sample["process"] = process;
    sample["frontend_retained"] = frontend;
    sample["category_count"] = 2;

    QJsonArray categories;
    categories.append(process);
    QJsonObject frontendCategory = frontend;
    frontendCategory["category"] = QStringLiteral("frontend_retained");
    categories.append(frontendCategory);
    sample["categories"] = categories;

    const QByteArray line = QJsonDocument(sample).toJson(QJsonDocument::Compact) + '\n';
    if (ramUsageFile.write(line) != line.size() || !ramUsageFile.flush())
        appendLine("ERROR", QStringLiteral("unable to write RAM usage log: %1")
                                .arg(ramUsageFile.errorString()));
}

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
    diagnosticsDirectory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
        + "/launch-diagnostics";
    QDir previous(diagnosticsDirectory);
    if (previous.exists()) previous.removeRecursively();
    QDir().mkpath(diagnosticsDirectory);
    logFile.setFileName(diagnosticsDirectory + "/main.log");
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

void setMemorySnapshotProvider(std::function<QJsonObject()> provider) {
    memorySnapshotProvider = std::move(provider);
}

void setMemoryLoggingEnabled(bool enabled) {
    if (!enabled) {
        if (memoryTimer) memoryTimer->stop();
        if (ramUsageFile.isOpen()) ramUsageFile.close();
        return;
    }

    if (!memoryTimer) {
        memoryTimer = new QTimer(QCoreApplication::instance());
        memoryTimer->setInterval(1000);
        QObject::connect(memoryTimer, &QTimer::timeout, &writeMemorySample);
    }
    if (!ramUsageFile.isOpen()) {
        ramUsageFile.setFileName(diagnosticsDirectory + "/ram_usage.log");
        const QIODevice::OpenMode mode = QIODevice::WriteOnly | QIODevice::Text |
            (ramUsageLogOpened ? QIODevice::Append : QIODevice::Truncate);
        if (!ramUsageFile.open(mode)) {
            appendLine("ERROR", QStringLiteral("unable to open RAM usage log: %1")
                                    .arg(ramUsageFile.errorString()));
            return;
        }
        ramUsageLogOpened = true;
    }
    memoryElapsed.restart();
    writeMemorySample();
    memoryTimer->start();
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

QString directoryPath() { return diagnosticsDirectory; }

QString logPath() { return logFile.fileName(); }

void shutdown() {
    setMemoryLoggingEnabled(false);
    memorySnapshotProvider = {};
    appendLine("INFO", "process exit");
    setFatalFlushHandler({});
    qInstallMessageHandler(nullptr);
    if (previousTerminate) std::set_terminate(previousTerminate);
    QMutexLocker lock(&logMutex);
    if (logFile.isOpen()) logFile.close();
}

}
