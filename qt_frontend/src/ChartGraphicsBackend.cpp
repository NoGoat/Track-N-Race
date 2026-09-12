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
    QVector<BackendInfo> supported;
    QString requested = QStringLiteral("auto");
    BackendInfo active;
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

} // namespace

void initialize()
{
    State& s = state();
    if (s.initialized) return;
    s.initialized = true;

    // This order is the application contract. Keep platform-inapplicable APIs
    // in the probe sequence so the preference stays identical on every OS.
    const struct Candidate {
        const char* key;
        const char* label;
        QRhiWidget::Api api;
        bool (*probe)();
    } candidates[] = {
        { "vulkan", "Vulkan", QRhiWidget::Api::Vulkan, probeVulkan },
        { "d3d12", "Direct3D 12", QRhiWidget::Api::Direct3D12, probeD3D12 },
        { "d3d11", "Direct3D 11", QRhiWidget::Api::Direct3D11, probeD3D11 },
        { "metal", "Metal", QRhiWidget::Api::Metal, probeMetal },
        { "opengl", "OpenGL", QRhiWidget::Api::OpenGL, probeOpenGL },
    };

    for (const Candidate& candidate : candidates) {
        if (candidate.probe())
            s.supported.push_back({ QString::fromLatin1(candidate.key),
                                    QString::fromLatin1(candidate.label), candidate.api });
    }
    QStringList supportedNames;
    for (const BackendInfo& backend : s.supported) supportedNames.push_back(backend.label);
    qInfo().noquote() << "[charts] Supported graphics backends:"
                      << (supportedNames.isEmpty() ? QStringLiteral("none")
                                                   : supportedNames.join(QStringLiteral(", ")));

    QSettings settings("TrackNRace", "NativeRecorder");
    s.requested = settings.value("ui/chartGraphicsBackend", "auto").toString().toLower();

    if (s.requested != QLatin1String("auto")) {
        for (const BackendInfo& backend : s.supported) {
            if (backend.key == s.requested) {
                s.active = backend;
                break;
            }
        }
        if (s.active.api == QRhiWidget::Api::Null)
            qWarning().noquote() << "[charts] Requested graphics backend" << s.requested
                                 << "is unavailable; using automatic fallback";
    }
    if (s.active.api == QRhiWidget::Api::Null && !s.supported.isEmpty())
        s.active = s.supported.first();

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
    initialize();
    if (key.compare(QLatin1String("auto"), Qt::CaseInsensitive) == 0) return true;
    for (const BackendInfo& backend : state().supported)
        if (backend.key.compare(key, Qt::CaseInsensitive) == 0) return true;
    return false;
}

} // namespace tnr::graphics
