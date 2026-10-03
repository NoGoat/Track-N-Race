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

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>

#ifdef Q_OS_WIN
#include <windows.h>
#include <psapi.h>
#elif defined(Q_OS_MACOS)
#include <mach/mach.h>
#include <malloc/malloc.h>
#elif defined(Q_OS_LINUX)
#include <malloc.h>
#else
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

// One OS-level process row in Electron's app.getAppMetrics() shape. Qt runs
// in a single process, so the row list always has exactly one entry.
struct ProcessMemory {
    QJsonObject row;          // Electron "processes[]" entry
    QJsonObject details;      // extra OS counters for main_runtime_memory
    double workingSetKb = 0;
    double privateKb = -1;    // < 0: not reported on this platform (as Electron)
};

ProcessMemory processMemorySnapshot() {
    ProcessMemory out;

#ifdef Q_OS_WIN
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) {
        out.workingSetKb = jsonNumber(counters.WorkingSetSize) / 1024.0;
        out.privateKb = jsonNumber(counters.PrivateUsage) / 1024.0;
        out.details["working_set_bytes"] = jsonNumber(counters.WorkingSetSize);
        out.details["peak_working_set_bytes"] = jsonNumber(counters.PeakWorkingSetSize);
        out.details["private_bytes"] = jsonNumber(counters.PrivateUsage);
        out.details["pagefile_bytes"] = jsonNumber(counters.PagefileUsage);
        out.details["peak_pagefile_bytes"] = jsonNumber(counters.PeakPagefileUsage);
    }
    DWORD handles = 0;
    if (GetProcessHandleCount(GetCurrentProcess(), &handles))
        out.details["handle_count"] = jsonNumber(handles);
    out.details["gdi_objects"] = jsonNumber(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS));
    out.details["user_objects"] = jsonNumber(GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS));
#elif defined(Q_OS_MACOS)
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
        out.workingSetKb = jsonNumber(info.resident_size) / 1024.0;
        out.details["working_set_bytes"] = jsonNumber(info.resident_size);
        out.details["peak_working_set_bytes"] = jsonNumber(info.resident_size_max);
        out.details["virtual_bytes"] = jsonNumber(info.virtual_size);
    }
    // phys_footprint is the process's own memory as Activity Monitor reports it.
    task_vm_info_data_t vm{};
    mach_msg_type_number_t vmCount = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO,
                  reinterpret_cast<task_info_t>(&vm), &vmCount) == KERN_SUCCESS) {
        out.privateKb = jsonNumber(vm.phys_footprint) / 1024.0;
        out.details["private_bytes"] = jsonNumber(vm.phys_footprint);
    }
#elif defined(Q_OS_LINUX)
    QFile status(QStringLiteral("/proc/self/status"));
    if (status.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const auto readKb = [](const QByteArray& line, const QByteArray& key) -> double {
            if (!line.startsWith(key)) return -1.0;
            const QList<QByteArray> fields = line.mid(key.size()).simplified().split(' ');
            bool ok = false;
            const quint64 kb = fields.isEmpty() ? 0 : fields.front().toULongLong(&ok);
            return ok ? jsonNumber(kb) : -1.0;
        };
        while (!status.atEnd()) {
            const QByteArray line = status.readLine();
            const struct { const char* source; const char* target; } fields[] = {
                { "VmRSS:", "working_set_bytes" },
                { "VmHWM:", "peak_working_set_bytes" },
                { "VmSize:", "virtual_bytes" },
                { "RssAnon:", "anonymous_resident_bytes" },
                { "VmSwap:", "swap_bytes" },
            };
            for (const auto& field : fields) {
                const double kb = readKb(line, field.source);
                if (kb < 0.0) continue;
                out.details[field.target] = kb * 1024.0;
                if (qstrcmp(field.source, "VmRSS:") == 0) out.workingSetKb = kb;
            }
        }
    }
#else
    struct rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0)
        out.details["peak_working_set_bytes"] = jsonNumber(usage.ru_maxrss * 1024ULL);
#endif

    out.row["pid"] = jsonNumber(QCoreApplication::applicationPid());
    out.row["type"] = QStringLiteral("Qt");
    out.row["process_type"] = QStringLiteral("Qt");
    out.row["name"] = QStringLiteral("Track N Race (Qt)");
    out.row["working_set_kb"] = out.workingSetKb;
    out.row["private_kb"] = out.privateKb >= 0.0 ? QJsonValue(out.privateKb) : QJsonValue();
    return out;
}

// Native allocator counters: the Qt build's counterpart of Electron's Node/V8
// heap section. Only what the platform exposes cheaply is reported.
QJsonObject allocatorSnapshot() {
    QJsonObject heap;
#if defined(Q_OS_MACOS)
    malloc_statistics_t stats{};
    malloc_zone_statistics(nullptr, &stats);
    heap["source"] = QStringLiteral("malloc_zone_statistics");
    heap["blocks_in_use"] = jsonNumber(stats.blocks_in_use);
    heap["size_in_use_bytes"] = jsonNumber(stats.size_in_use);
    heap["max_size_in_use_bytes"] = jsonNumber(stats.max_size_in_use);
    heap["size_allocated_bytes"] = jsonNumber(stats.size_allocated);
#elif defined(Q_OS_LINUX) && defined(__GLIBC__) && \
    (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
    const struct mallinfo2 info = mallinfo2();
    heap["source"] = QStringLiteral("mallinfo2");
    heap["arena_bytes"] = jsonNumber(info.arena);
    heap["mmap_bytes"] = jsonNumber(info.hblkhd);
    heap["in_use_bytes"] = jsonNumber(info.uordblks);
    heap["free_bytes"] = jsonNumber(info.fordblks);
    heap["releasable_bytes"] = jsonNumber(info.keepcost);
#elif defined(Q_OS_WIN)
    heap["source"] = QStringLiteral("GetProcessHeaps");
    heap["process_heaps"] = jsonNumber(GetProcessHeaps(0, nullptr));
#else
    heap["source"] = QStringLiteral("unavailable");
#endif
    return heap;
}

double finiteNumber(const QJsonValue& value) {
    const double number = value.toDouble(0.0);
    return std::isfinite(number) ? number : 0.0;
}

void appendWithCategory(QJsonArray& categories, const QString& category, const QJsonObject& body) {
    QJsonObject entry{{"category", category}};
    for (auto it = body.begin(); it != body.end(); ++it) entry.insert(it.key(), it.value());
    categories.append(entry);
}

// Mirrors the Electron main-process ram_usage.log sample (diagnostics.ts) so
// both frontends' logs can be read by the same tooling. The provider returns
// the telemetry_data body { mode, attribution_scope, main, renderer }: "main"
// is native-engine retention and "renderer" is Qt UI retention.
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

    const ProcessMemory process = processMemorySnapshot();
    QJsonArray processes;
    processes.append(process.row);

    const QJsonObject mainRetention = frontend.value("main").toObject();
    const QJsonObject rendererRetention = frontend.value("renderer").toObject();
    const double retainedBytes = finiteNumber(mainRetention.value("retained_bytes")) +
        finiteNumber(rendererRetention.value("estimated_retained_bytes"));
    const QDateTime rendererSampledAt = QDateTime::fromString(
        rendererRetention.value("sampled_at").toString(), Qt::ISODateWithMs);

    QJsonObject telemetryData;
    telemetryData["type"] = QStringLiteral("TelemetryData");
    telemetryData["name"] = QStringLiteral("Application-held telemetry");
    telemetryData["mode"] = frontend.value("mode").toString(QStringLiteral("unknown"));
    telemetryData["estimated_retained_bytes"] = retainedBytes;
    telemetryData["estimated_retained_kb"] = retainedBytes / 1024.0;
    telemetryData["already_included_in_process_totals"] = true;
    telemetryData["attribution_scope"] = frontend.value("attribution_scope");
    telemetryData["renderer_sample_age_ms"] = rendererSampledAt.isValid()
        ? QJsonValue(jsonNumber(qMax<qint64>(0, rendererSampledAt.msecsTo(QDateTime::currentDateTimeUtc()))))
        : QJsonValue();
    telemetryData["main"] = frontend.contains("main") ? QJsonValue(mainRetention) : QJsonValue();
    telemetryData["renderer"] = frontend.contains("renderer") ? QJsonValue(rendererRetention) : QJsonValue();
    if (frontend.contains("snapshot_error"))
        telemetryData["snapshot_error"] = frontend.value("snapshot_error");

    QJsonObject runtime;
    runtime["type"] = QStringLiteral("MainRuntimeMemory");
    runtime["name"] = QStringLiteral("Qt process native runtime");
    runtime["pid"] = process.row.value("pid");
    runtime["already_included_in_process_totals"] = true;
    runtime["process"] = process.details;
    runtime["heap"] = allocatorSnapshot();

    QJsonObject sample;
    sample["timestamp"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    sample["elapsed_ms"] = jsonNumber(memoryElapsed.isValid() ? memoryElapsed.elapsed() : 0);
    sample["total_working_set_kb"] = process.workingSetKb;
    sample["total_private_kb"] = process.privateKb >= 0.0 ? QJsonValue(process.privateKb) : QJsonValue();
    sample["process_count"] = processes.size();
    sample["processes"] = processes;
    sample["category_count"] = processes.size() + 2;

    QJsonArray categories;
    appendWithCategory(categories, QStringLiteral("process"), process.row);
    appendWithCategory(categories, QStringLiteral("telemetry_data"), telemetryData);
    appendWithCategory(categories, QStringLiteral("main_runtime_memory"), runtime);
    sample["categories"] = categories;
    sample["telemetry_data"] = telemetryData;
    sample["main_runtime_memory"] = runtime;

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
