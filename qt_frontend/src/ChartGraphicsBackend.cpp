#include "ChartGraphicsBackend.h"

#include <QDebug>
#include <QOffscreenSurface>
#include <QSettings>
#include <QStringList>
#include <QSurfaceFormat>
#include <QVulkanInstance>

#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>

#include <memory>

namespace tnr::graphics {
namespace {

struct State {
    bool initialized = false;
    bool fullyProbed = false;
    QVector<BackendInfo> supported;
    QString requested = QStringLiteral("auto");
    BackendInfo active;
};

struct Candidate {
    const char* key;
    const char* label;
    QRhiWidget::Api api;
    bool (*probe)();
};

State& state()
{
    static State value;
    return value;
}

bool probeVulkan()
{
#if QT_CONFIG(vulkan) && __has_include(<vulkan/vulkan.h>)
    QVulkanInstance instance;
    instance.setExtensions(QRhiVulkanInitParams::preferredInstanceExtensions());
    if (!instance.create()) return false;
    QRhiVulkanInitParams params;
    params.inst = &instance;
    return QRhi::probe(QRhi::Vulkan, &params);
#else
    return false;
#endif
}

bool probeD3D12()
{
#ifdef Q_OS_WIN
    QRhiD3D12InitParams params;
    return QRhi::probe(QRhi::D3D12, &params);
#else
    return false;
#endif
}

bool probeD3D11()
{
#ifdef Q_OS_WIN
    QRhiD3D11InitParams params;
    return QRhi::probe(QRhi::D3D11, &params);
#else
    return false;
#endif
}

bool probeMetal()
{
#if QT_CONFIG(metal)
    QRhiMetalInitParams params;
    return QRhi::probe(QRhi::Metal, &params);
#else
    return false;
#endif
}

bool probeOpenGL()
{
#if QT_CONFIG(opengl)
    QRhiGles2InitParams params;
    std::unique_ptr<QOffscreenSurface> surface(
        QRhiGles2InitParams::newFallbackSurface(QSurfaceFormat::defaultFormat()));
    if (!surface || !surface->isValid()) return false;
    params.fallbackSurface = surface.get();
    return QRhi::probe(QRhi::OpenGLES2, &params);
#else
    return false;
#endif
}

// This order is the application contract for "auto". Platform-inapplicable
// APIs stay in the sequence; they simply fail their probe. Windows prefers
// Direct3D 11: measured on an NVIDIA laptop GPU, Vulkan cost ~155 MB more
// process memory at idle and ~140 MB more after visiting every page, and twice
// the video memory, for identical charts. Direct3D 12 cost more than Vulkan.
const Candidate kCandidates[] = {
#ifdef Q_OS_WIN
    { "d3d11", "Direct3D 11", QRhiWidget::Api::Direct3D11, probeD3D11 },
    { "vulkan", "Vulkan", QRhiWidget::Api::Vulkan, probeVulkan },
    { "d3d12", "Direct3D 12", QRhiWidget::Api::Direct3D12, probeD3D12 },
#else
    { "vulkan", "Vulkan", QRhiWidget::Api::Vulkan, probeVulkan },
    { "d3d12", "Direct3D 12", QRhiWidget::Api::Direct3D12, probeD3D12 },
    { "d3d11", "Direct3D 11", QRhiWidget::Api::Direct3D11, probeD3D11 },
#endif
    { "metal", "Metal", QRhiWidget::Api::Metal, probeMetal },
    { "opengl", "OpenGL", QRhiWidget::Api::OpenGL, probeOpenGL },
};

BackendInfo backendInfo(const Candidate& candidate)
{
    return { QString::fromLatin1(candidate.key), QString::fromLatin1(candidate.label), candidate.api };
}

// Probing creates a real device, which loads that API's driver stack into the
// process, and drivers keep their DLLs and memory pools after the probe device
// is destroyed. Only the Settings list needs every backend, so the full probe
// runs when that list is first requested, not at startup.
void probeAll()
{
    State& s = state();
    if (s.fullyProbed) return;
    s.fullyProbed = true;
    QVector<BackendInfo> supported;
    for (const Candidate& candidate : kCandidates) {
        if (candidate.api == s.active.api) supported.push_back(s.active);
        else if (candidate.probe()) supported.push_back(backendInfo(candidate));
    }
    s.supported = supported;
    QStringList supportedNames;
    for (const BackendInfo& backend : s.supported) supportedNames.push_back(backend.label);
    qInfo().noquote() << "[charts] Supported graphics backends:"
                      << (supportedNames.isEmpty() ? QStringLiteral("none")
                                                   : supportedNames.join(QStringLiteral(", ")));
}

} // namespace

void initialize()
{
    State& s = state();
    if (s.initialized) return;
    s.initialized = true;

    QSettings settings("TrackNRace", "NativeRecorder");
    s.requested = settings.value("ui/chartGraphicsBackend", "auto").toString().toLower();

    if (s.requested != QLatin1String("auto")) {
        for (const Candidate& candidate : kCandidates) {
            if (s.requested != QLatin1String(candidate.key)) continue;
            if (candidate.probe()) s.active = backendInfo(candidate);
            break;
        }
        if (s.active.api == QRhiWidget::Api::Null)
            qWarning().noquote() << "[charts] Requested graphics backend" << s.requested
                                 << "is unavailable; using automatic fallback";
    }
    // Automatic: the first backend in preference order that works.
    for (const Candidate& candidate : kCandidates) {
        if (s.active.api != QRhiWidget::Api::Null) break;
        if (s.requested == QLatin1String(candidate.key)) continue;   // already failed above
        if (candidate.probe()) s.active = backendInfo(candidate);
    }
    if (s.active.api != QRhiWidget::Api::Null) s.supported = { s.active };

    if (s.active.api == QRhiWidget::Api::Null) {
        // QRhiWidget's Null backend still clears and composites correctly. This
        // should only be reached on a machine with no usable graphics API.
        s.active = { QStringLiteral("null"), QStringLiteral("Unavailable"),
                     QRhiWidget::Api::Null };
        qCritical("[charts] No supported graphics backend was found");
    } else {
        qInfo().noquote() << "[charts] Active graphics backend:" << s.active.label
                          << "(requested:" << s.requested + QLatin1Char(')');
    }
}

const QVector<BackendInfo>& supportedBackends()
{
    initialize();
    probeAll();
    return state().supported;
}

QString requestedBackendKey()
{
    initialize();
    return state().requested;
}

QString activeBackendKey()
{
    initialize();
    return state().active.key;
}

QString activeBackendLabel()
{
    initialize();
    return state().active.label;
}

QRhiWidget::Api activeApi()
{
    initialize();
    return state().active.api;
}

bool isSelectableBackend(const QString& key)
{
    if (key.compare(QLatin1String("auto"), Qt::CaseInsensitive) == 0) return true;
    for (const BackendInfo& backend : supportedBackends())
        if (backend.key.compare(key, Qt::CaseInsensitive) == 0) return true;
    return false;
}

} // namespace tnr::graphics
