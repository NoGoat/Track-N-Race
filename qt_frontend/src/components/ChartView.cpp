#include "ChartView.h"
#include "../ChartGraphicsBackend.h"
#include "../SessionModel.h"

#include <QComboBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFontMetricsF>
#include <QFrame>
#include <QLabel>
#include <QLocale>
#include <QMatrix4x4>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QResizeEvent>
#include <QRegion>
#include <QSettings>
#include <QStyleOptionComboBox>
#include <QStylePainter>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QRhiWidget>
#include <rhi/qrhi.h>
#include <rhi/qshader.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

namespace {
constexpr int kGap = ChartView::PanelGap;
constexpr int kHeader = 34;
constexpr int kControlH = 26;
constexpr int kSidePad = 8;
constexpr int kControlTextInset = 6;
constexpr int kTitleControlGap = 8;
constexpr int kLapControlW = 54;
constexpr int kAxisTextGap = 5;
constexpr int kAxisLaneGap = 6;
constexpr int kPlotEdgePad = 4;
constexpr qsizetype kMaxPoints = 750000;
constexpr qsizetype kCompactAt = 65536;
const QColor kAxis(150, 150, 150, 130);
const QColor kGrid(150, 150, 150, 40);

class InsetComboBox final : public QComboBox {
public:
    using QComboBox::QComboBox;

protected:
    void paintEvent(QPaintEvent*) override {
        QStylePainter painter(this);
        QStyleOptionComboBox option;
        initStyleOption(&option);
        painter.drawComplexControl(QStyle::CC_ComboBox, option);
        option.rect.adjust(kControlTextInset, 0, 0, 0);
        painter.drawControl(QStyle::CE_ComboBoxLabel, option);
    }
};

// Keep CPU keys in double precision for binary search and midpoint selection.
// GPU coordinates are relative to a retained per-series origin.
struct Point { double x; float y; };
struct GpuPoint { float x, y; };
struct Segment { GpuPoint a, b; };
static_assert(sizeof(Segment) == sizeof(float) * 4);

struct Axis {
    ChartView::Side side = ChartView::Side::Left;
    double lo = 0, hi = 1;
    QColor color;
    bool inherit = true, visible = true, grid = false, time = false, distance = false;
    char format = 'f'; int precision = 0, panel = 0;
    int tickSpacePx = 80;
    bool lapBoundaryLabels = false;
    int labelWidth = 1, laneOffset = 0;
    double scale = 1, step = 0;
    QString suffix, timeFormat = "%m:%s";
    QVector<double> ticks, sessionKeys, sessionTimes;
    QStringList labels;
    int lapNum = -1; float lapStart = -1;
    QElapsedTimer fitTimer;
};

struct Series {
    ChartView::SeriesSpec spec;
    int panel = 0, linked = -1;
    bool visible = true;
    std::vector<Point> data;
    qsizetype first = 0, dirty = 0, fitDirty = 0;
    quint64 revision = 1;
    QRect legendHit;
    qsizetype size() const { return qsizetype(data.size()) - first; }
    bool empty() const { return size() <= 0; }
};

struct Band { ChartView::BandSpec spec; int panel = 0; };
struct ReferenceLine { int axis; double value; bool dashed; };

struct PanelDivider {
    QRect rect;
    QFrame::Shape shape = QFrame::VLine;
};

struct Panel {
    bool visible = true, header = false, legend = true;
    QString title;
    QRect outer, plot;
    QComboBox *window = nullptr, *lap = nullptr;
    tnr::GraphSection section = tnr::GraphSection::Count_;
    bool cursorV = false, cursorH = false;
    double cursorX = 0, cursorY = .5;
};

qsizetype lowerBound(const Series& s, double x) {
    auto begin = s.data.begin() + s.first;
    auto it = std::lower_bound(begin, s.data.end(), x,
        [](const Point& p, double key) { return p.x < key; });
    return qsizetype(std::distance(s.data.begin(), it));
}

qsizetype nearest(const Series& s, double x) {
    if (s.empty()) return -1;
    qsizetype i = lowerBound(s, x);
    if (i >= qsizetype(s.data.size())) return qsizetype(s.data.size()) - 1;
    if (i > s.first && x - s.data[size_t(i - 1)].x <= s.data[size_t(i)].x - x) --i;
    return i;
}

void compact(Series& s) {
    if (s.first < kCompactAt || s.first * 2 < qsizetype(s.data.size())) return;
    s.data.erase(s.data.begin(), s.data.begin() + s.first);
    s.first = s.dirty = s.fitDirty = 0; ++s.revision;
}

double interpolate(const QVector<double>& a, const QVector<double>& b, double x) {
    if (a.isEmpty() || a.size() != b.size()) return x;
    auto it = std::lower_bound(a.begin(), a.end(), x);
    if (it == a.begin()) return b.first();
    if (it == a.end()) return b.last();
    int n = int(std::distance(a.begin(), it)), p = n - 1;
    double span = a[n] - a[p];
    return span > 0 ? b[p] + (b[n] - b[p]) * (x - a[p]) / span : b[n];
}

int msaaSamples() {
    int n = QSettings("TrackNRace", "NativeRecorder").value("ui/chartMsaaSamples", 4).toInt();
    return n == 0 ? 1 : (n == 4 || n == 8 || n == 16 ? n : 4);
}

QVector<ChartView*>& liveCharts() { static QVector<ChartView*> v; return v; }

double niceStep(double span, int targetDivisions = 5) {
    if (!std::isfinite(span) || span <= 0) return 1;
    double raw = span / qMax(1, targetDivisions), p = std::pow(10.0, std::floor(std::log10(raw))), f = raw / p;
    return (f <= 1 ? 1 : f <= 2 ? 2 : f <= 5 ? 5 : 10) * p;
}

// Axis labels use compact, locale-aware numbers. Tooltip precision remains
// explicit, but shares zero normalization and the same decimal separator.
QString numberText(double value, char format, int precision, bool grouped = false,
                   bool compact = false) {
    if (!std::isfinite(value)) return QString::fromUtf8("—");
    precision = qBound(format == 'f' ? 0 : 1, precision, 12);
    if (value == 0 || (format == 'f' && std::abs(value) < .5 * std::pow(10., -precision))) value = 0;
    QLocale locale;
    if (!grouped) locale.setNumberOptions(locale.numberOptions() | QLocale::OmitGroupSeparator);
    QString text = locale.toString(value, format, precision);
    if (compact && format == 'f' && text.contains(locale.decimalPoint())) {
        while (text.endsWith(locale.zeroDigit())) text.chop(locale.zeroDigit().size());
        if (text.endsWith(locale.decimalPoint())) text.chop(locale.decimalPoint().size());
    }
    return text;
}

int decimalPlaces(double value, int limit = 6) {
    value = std::abs(value);
    for (int precision = 0; precision < limit; ++precision) {
        const double scaled = value * std::pow(10., precision);
        if (std::abs(scaled - std::round(scaled)) < 1e-7) return precision;
    }
    return limit;
}

QString timeText(double seconds, int precision = 1, const QString& format = "%m:%s") {
    if (!std::isfinite(seconds)) return QString::fromUtf8("—");
    precision = qBound(0, precision, 6);
    if (format.contains("%z")) precision = 3;
    const qint64 factor = qint64(std::pow(10., precision));
    const double scaled = std::abs(seconds) * factor;
    if (scaled >= double(std::numeric_limits<qint64>::max())) return numberText(seconds, 'g', 6) + " s";
    const qint64 ticks = qint64(std::round(scaled)), whole = ticks / factor;
    const bool hours = format.contains("%h");
    QString result = format;
    result.replace("%h", QString::number(whole / 3600));
    result.replace("%m", hours ? QString::number(whole / 60 % 60).rightJustified(2, '0') : QString::number(whole / 60));
    QString second = QString::number(whole % 60).rightJustified(2, '0');
    const QString fraction = QString::number(ticks % factor).rightJustified(precision, '0');
    if (precision && !format.contains("%z")) second += QLocale().decimalPoint() + fraction;
    result.replace("%s", second);
    result.replace("%z", fraction);
    if (seconds < 0 && ticks) result.prepend(QLocale().negativeSign());
    return result;
}

QString tickText(const Axis& a, double value, double step = 0) {
    if (step <= 0) step = a.step > 0 ? a.step : niceStep(a.hi - a.lo);
    if (a.time && !a.distance)
        return timeText(value, step < 1 ? decimalPlaces(step) : 0, a.timeFormat);
    const double scale = a.distance ? 1 : a.scale;
    const double displayed = value / scale;
    int precision = qMax(a.distance ? 0 : a.precision, decimalPlaces(step / scale));
    // A non-nice upper bound must not become the same integer as its neighbor.
    // Limit extra digits so auto-ranging does not expose floating-point noise.
    precision = qMax(precision, decimalPlaces(displayed, qMin(6, precision + 2)));
    char format = a.distance ? 'f' : a.format;
    if (format == 'f' && (std::abs(displayed) >= 1e9 ||
        (displayed != 0 && std::abs(displayed) < 1e-6))) { format = 'g'; precision = 6; }
    return numberText(displayed, format, precision, false, true) + (a.distance ? " m" : a.suffix);
}

QString mappedTickText(const Axis& a, double value, double step = 0) {
    const int mapped = a.ticks.indexOf(value);
    return mapped >= 0 && mapped < a.labels.size() ? a.labels[mapped] : tickText(a, value, step);
}

QRectF containedHorizontally(QRectF rect, const QRectF& bounds) {
    if (rect.width() > bounds.width()) { rect.setLeft(bounds.left()); rect.setWidth(bounds.width()); }
    else if (rect.left() < bounds.left()) rect.moveLeft(bounds.left());
    else if (rect.right() > bounds.right()) rect.moveRight(bounds.right());
    return rect;
}

QFont chartLabelFont(const QFont& base) {
    QFont result = base;
    result.setPointSizeF(8.0);
    result.setFeature(QFont::Tag("tnum"), 1); // Stable-width digits in proportional UI fonts.
    return result;
}

QVector<double> ticksFor(const Axis& a, int targetDivisions = 5) {
    if (!a.ticks.isEmpty()) {
        QVector<double> out;
        for (double x : a.ticks) if (x >= a.lo && x <= a.hi) out.push_back(x);
        return out;
    }
    const double step = a.step > 0 ? a.step : niceStep(a.hi - a.lo, targetDivisions);
    QVector<double> out;
    if (!std::isfinite(step) || step <= 0) return out;
    const double first = std::ceil(a.lo / step);
    for (int i = 0; i < 64; ++i) {
        const double tick = (first + i) * step; // No accumulated addition error.
        if (!std::isfinite(tick) || tick > a.hi + step * 1e-6) break;
        if (tick < a.lo - step * 1e-6) continue;
        if (!out.isEmpty() && tick <= out.last()) break;
        out.push_back(std::abs(tick) < step * 1e-9 ? 0. : tick);
    }
    return out;
}

QVector<double> yTicksWithUpperBound(const Axis& axis, int plotHeight = 0,
                                     int minimumPixelSpacing = 0) {
    QVector<double> ticks = ticksFor(axis);
    if (!axis.ticks.isEmpty()) return ticks; // Explicit tick lists are authoritative.
    const double tolerance = qMax(1.0, std::abs(axis.hi)) * 1e-9;
    if (!ticks.isEmpty() && std::abs(ticks.last() - axis.hi) <= tolerance) return ticks;

    // The range maximum is a mandatory label. If the preceding nice tick would
    // collide with it, omit that tick rather than hiding or overlapping the bound.
    if (plotHeight > 0 && minimumPixelSpacing > 0 && axis.hi > axis.lo) {
        while (!ticks.isEmpty()) {
            const double pixelGap = (axis.hi - ticks.last()) / (axis.hi - axis.lo) * plotHeight;
            if (pixelGap >= minimumPixelSpacing) break;
            ticks.removeLast();
        }
    }
    ticks.push_back(axis.hi);
    return ticks;
}

int measuredYAxisLabelWidth(const Axis& axis, const QFontMetricsF& metrics) {
    int width = 1;
    for (double tick : yTicksWithUpperBound(axis))
        width = qMax(width, int(std::ceil(metrics.horizontalAdvance(mappedTickText(axis, tick)))) + 2);
    return width;
}

QShader shader(const char* path) {
    QFile f(QString::fromLatin1(path));
    return f.open(QIODevice::ReadOnly) ? QShader::fromSerialized(f.readAll()) : QShader();
}

struct alignas(16) Uniform { float mvp[16]; float color[4]; float stroke[4]; };

ChartView::LineType lineType(const Series& s) {
    return s.spec.step ? ChartView::LineType::Step : s.spec.lineType;
}

int pathSegments(const Series& s) {
    if (lineType(s) != ChartView::LineType::Step) return 1;
    return s.spec.stepLocation == 0 || s.spec.stepLocation == 1 ? 2 : 3;
}

class RhiCanvas final : public QRhiWidget {
public:
    RhiCanvas(QVector<Axis>* a, QVector<Series>* s, QVector<Band>* b,
              QVector<Panel>* p, QVector<int>* order, QWidget* parent)
        : QRhiWidget(parent), axes(a), series(s), bands(b), panels(p), drawOrder(order) {
        setApi(tnr::graphics::activeApi());
        setSampleCount(msaaSamples());
        setMouseTracking(true);
    }
    void applySettings() { if (sampleCount() != msaaSamples()) setSampleCount(msaaSamples()); update(); }

protected:
    void initialize(QRhiCommandBuffer*) override {
        if (device != rhi()) { releaseResources(); device = rhi(); }
        makePipelines();
    }

    void render(QRhiCommandBuffer* cb) override {
        if (!device || !renderTarget() || !linePipe || !nativePipe || !fillPipe) return;
        if (gpu.size() < size_t(series->size())) gpu.resize(size_t(series->size()));
        if (gpuBands.size() < size_t(bands->size())) gpuBands.resize(size_t(bands->size()));
        QRhiResourceUpdateBatch* up = device->nextResourceUpdateBatch();
        struct Draw {
            QRhiBuffer* vb; QRhiShaderResourceBindings* srb;
            quint32 first, count; int panel; bool instanced = false;
        };
        QVector<Draw> fillDraws, lineDraws, bandDraws;
        for (int i = 0; i < bands->size(); ++i) {
            const Band& b = (*bands)[i];
            if (b.spec.axisId < 0 || b.spec.axisId >= axes->size()) continue;
            const int panel = (*axes)[b.spec.axisId].panel;
            if (!drawable(panel)) continue;
            Gpu& g = gpuBands[size_t(i)];
            ensureGpu(g, 4, 0, false);
            if (!g.uploaded) {
                GpuPoint v[] = {{0, float(b.spec.min)}, {1, float(b.spec.min)},
                                {0, float(b.spec.max)}, {1, float(b.spec.max)}};
                up->updateDynamicBuffer(g.line.get(), 0, sizeof(v), v); g.uploaded = 1;
            }
            const Uniform u = uniform(0, b.spec.axisId, b.spec.color, 0, true);
            up->updateDynamicBuffer(g.lineUbo.get(), 0, sizeof(u), &u);
            bandDraws.push_back({g.line.get(), g.lineSrb.get(), 0, 4, panel});
        }
        for (int id : *drawOrder) {
            if (id < 0 || id >= series->size()) continue;
            Series& s = (*series)[id];
            if (!s.visible || s.empty() || !drawable(s.panel) ||
                s.spec.xAxisId < 0 || s.spec.xAxisId >= axes->size() ||
                s.spec.yAxisId < 0 || s.spec.yAxisId >= axes->size()) continue;
            const Axis& x = (*axes)[s.spec.xAxisId];
            const double pad = s.spec.width * .5 * (x.hi - x.lo) / (*panels)[s.panel].plot.width();
            if (s.data.back().x < x.lo - pad || s.data[size_t(s.first)].x > x.hi + pad) continue;
            upload(id, up);
            Gpu& g = gpu[size_t(id)];
            qsizetype begin = lowerBound(s, x.lo - pad); if (begin > s.first) --begin;
            qsizetype end = lowerBound(s, x.hi + pad); if (end < qsizetype(s.data.size())) ++end;
            if (end <= begin) continue;
            const auto type = lineType(s);
            const bool native = type == ChartView::LineType::NativeLine;
            const bool points = type == ChartView::LineType::NativePoint;
            const int segments = pathSegments(s);
            QColor color = s.spec.color;
            color.setAlphaF(color.alphaF() * s.spec.opacity);
            Uniform u = uniform(s.spec.xAxisId, s.spec.yAxisId, color, g.origin);
            const QRect plot = (*panels)[s.panel].plot;
            u.stroke[0] = 2.f / plot.width(); u.stroke[1] = 2.f / plot.height();
            u.stroke[2] = float(s.spec.width * .5); u.stroke[3] = points ? 1.f : 0.f;
            up->updateDynamicBuffer(g.lineUbo.get(), 0, sizeof(u), &u);
            if (s.spec.fill && g.fill) {
                QColor fill = s.spec.fillColor;
                if (!fill.isValid()) { fill = s.spec.color; fill.setAlphaF(fill.alphaF() * .2); }
                fill.setAlphaF(fill.alphaF() * s.spec.opacity);
                u = uniform(s.spec.xAxisId, s.spec.yAxisId, fill, g.origin);
                up->updateDynamicBuffer(g.fillUbo.get(), 0, sizeof(u), &u);
            }
            // Cached finite runs: scrolling does binary searches and draw calls,
            // never a scan of every retained sample. NaNs break lines AND fills.
            auto run = std::lower_bound(g.runs.begin(), g.runs.end(), begin,
                [](const auto& range, qsizetype key) { return range.second <= key; });
            for (; run != g.runs.end() && run->first < end; ++run) {
                const qsizetype first = qMax(begin, run->first), last = qMin(end, run->second);
                const qsizetype intervals = last - first - 1;
                if (s.spec.fill && intervals > 0)
                    fillDraws.push_back({g.fill.get(), g.fillSrb.get(), quint32(first * segments * 2),
                        quint32((intervals * segments + 1) * 2), s.panel});
                if (points && last > first)
                    lineDraws.push_back({g.line.get(), g.lineSrb.get(), quint32(first), quint32(last - first), s.panel, true});
                else if (intervals > 0 && s.spec.width > 0)
                    lineDraws.push_back({g.line.get(), g.lineSrb.get(), quint32(first * (native ? 1 : segments)),
                        quint32(native ? last - first : intervals * segments), s.panel, !native});
            }
        }
        cb->beginPass(renderTarget(), palette().color(QPalette::Window), {1, 0}, up);
        const QSize target = renderTarget()->pixelSize();
        const double ratio = devicePixelRatioF();
        auto viewport = [&](int id) {
            const QRect r = (*panels)[id].plot;
            const double left = r.x() * ratio, right = (r.x() + r.width()) * ratio;
            const double top = r.y() * ratio, bottom = (r.y() + r.height()) * ratio;
            // QRhi viewport/scissor coordinates are bottom-left based on every API.
            const double y = target.height() - bottom;
            // Viewports accept fractions. Only scissors require integer pixels;
            // round those outwards to avoid shaving off fractional edge coverage.
            cb->setViewport(QRhiViewport(float(left), float(y), float(right - left), float(bottom - top)));
            const int x0 = qBound(0, int(std::floor(left)), target.width());
            const int x1 = qBound(x0, int(std::ceil(right)), target.width());
            const int y0 = qBound(0, int(std::floor(y)), target.height());
            const int y1 = qBound(y0, int(std::ceil(target.height() - top)), target.height());
            cb->setScissor(QRhiScissor(x0, y0, x1 - x0, y1 - y0));
        };
        auto draw = [&](const QVector<Draw>& list, bool fill) {
            QRhiGraphicsPipeline* active = nullptr;
            for (const Draw& d : list) {
                auto* pipe = fill ? fillPipe.get() : d.instanced ? linePipe.get() : nativePipe.get();
                if (pipe != active) { cb->setGraphicsPipeline(pipe); active = pipe; }
                viewport(d.panel); cb->setShaderResources(d.srb);
                QRhiCommandBuffer::VertexInput input(d.vb, d.instanced ? d.first * sizeof(Segment) : 0);
                cb->setVertexInput(0, 1, &input);
                if (d.instanced) cb->draw(6, d.count);
                else cb->draw(d.count, 1, d.first);
            }
        };
        draw(bandDraws, true); draw(fillDraws, true); draw(lineDraws, false);
        cb->endPass();
    }

    void releaseResources() override {
        linePipe.reset(); nativePipe.reset(); fillPipe.reset(); templateSrb.reset(); templateUbo.reset();
        gpu.clear(); gpuBands.clear(); device = nullptr;
    }

private:
    struct Gpu {
        std::unique_ptr<QRhiBuffer> line, fill, lineUbo, fillUbo;
        std::unique_ptr<QRhiShaderResourceBindings> lineSrb, fillSrb;
        qsizetype cap = 0, fillCap = 0; quint64 uploaded = 0;
        bool instanced = false;
        double origin = 0;
        std::vector<std::pair<qsizetype, qsizetype>> runs;
        QByteArray lineStaging, fillStaging;
    };

    bool drawable(int panel) const {
        return panel >= 0 && panel < panels->size() && (*panels)[panel].visible && !(*panels)[panel].plot.isEmpty();
    }

    std::unique_ptr<QRhiShaderResourceBindings> makeSrb(std::unique_ptr<QRhiBuffer>& ubo) {
        ubo.reset(device->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(Uniform)));
        ubo->create();
        std::unique_ptr<QRhiShaderResourceBindings> result(device->newShaderResourceBindings());
        result->setBindings({QRhiShaderResourceBinding::uniformBuffer(0,
            QRhiShaderResourceBinding::VertexStage, ubo.get())});
        result->create(); return result;
    }

    void makePipelines() {
        if (!device || !renderTarget()) return;
        if (!templateSrb) templateSrb = makeSrb(templateUbo);
        const QShader vs = shader(":/shaders/chart.vert.qsb"), stroke = shader(":/shaders/chartline.vert.qsb"),
                      fs = shader(":/shaders/chart.frag.qsb");
        if (!vs.isValid() || !stroke.isValid() || !fs.isValid()) { qCritical("[charts] QRhi shaders are missing"); return; }
        auto make = [&](QRhiGraphicsPipeline::Topology topology, bool instanced) {
            std::unique_ptr<QRhiGraphicsPipeline> p(device->newGraphicsPipeline());
            p->setTopology(topology); p->setSampleCount(sampleCount());
            p->setFlags(QRhiGraphicsPipeline::UsesScissor);
            QRhiGraphicsPipeline::TargetBlend blend; blend.enable = true;
            blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
            blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
            p->setTargetBlends({blend});
            p->setShaderStages({{QRhiShaderStage::Vertex, instanced ? stroke : vs}, {QRhiShaderStage::Fragment, fs}});
            QRhiVertexInputLayout layout;
            if (instanced) {
                layout.setBindings({{sizeof(Segment), QRhiVertexInputBinding::PerInstance}});
                layout.setAttributes({{0, 0, QRhiVertexInputAttribute::Float4, 0}});
            } else {
                layout.setBindings({{sizeof(GpuPoint)}});
                layout.setAttributes({{0, 0, QRhiVertexInputAttribute::Float2, 0}});
            }
            p->setVertexInputLayout(layout); p->setShaderResourceBindings(templateSrb.get());
            p->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
            return p->create() ? std::move(p) : std::unique_ptr<QRhiGraphicsPipeline>();
        };
        linePipe = make(QRhiGraphicsPipeline::Triangles, true);
        nativePipe = make(QRhiGraphicsPipeline::LineStrip, false);
        fillPipe = make(QRhiGraphicsPipeline::TriangleStrip, false);
    }

    static qsizetype capacity(qsizetype needed) { qsizetype n = 1024; while (n < needed) n *= 2; return n; }

    void ensureGpu(Gpu& g, qsizetype vertices, qsizetype fillVertices, bool instanced) {
        if (!g.line || g.cap < vertices || g.instanced != instanced) {
            g.cap = capacity(qMax<qsizetype>(1, vertices)); g.instanced = instanced;
            g.line.reset(device->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer,
                int(g.cap * (instanced ? sizeof(Segment) : sizeof(GpuPoint)))));
            g.line->create(); g.uploaded = 0;
        }
        if (!g.lineSrb) g.lineSrb = makeSrb(g.lineUbo);
        if (fillVertices && (!g.fill || g.fillCap < fillVertices)) {
            g.fillCap = capacity(fillVertices);
            g.fill.reset(device->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, int(g.fillCap * sizeof(GpuPoint))));
            g.fill->create(); g.uploaded = 0;
        }
        if (fillVertices && !g.fillSrb) g.fillSrb = makeSrb(g.fillUbo);
    }

    void upload(int id, QRhiResourceUpdateBatch* up) {
        Series& s = (*series)[id]; Gpu& g = gpu[size_t(id)];
        const qsizetype count = qsizetype(s.data.size());
        const auto type = lineType(s);
        const bool native = type == ChartView::LineType::NativeLine;
        const bool points = type == ChartView::LineType::NativePoint;
        const int segments = pathSegments(s);
        const qsizetype pathCount = (count - 1) * segments + 1;
        ensureGpu(g, native || points ? count : pathCount - 1, s.spec.fill ? pathCount * 2 : 0, !native);
        if (g.uploaded == s.revision) return;
        qsizetype dirty = g.uploaded ? qBound<qsizetype>(0, s.dirty, count) : 0;
        if (!dirty) g.origin = s.data.front().x;
        // Append/replace repairs the previous interval too, including its step corners.
        const qsizetype start = dirty ? dirty - 1 : 0;
        g.lineStaging.resize(0); g.fillStaging.resize(0);
        auto point = [&](qsizetype i) { const auto& p = s.data[size_t(i)]; return GpuPoint{float(p.x - g.origin), p.y}; };
        auto appendFill = [&](GpuPoint p) {
            GpuPoint baseline{p.x, float(s.spec.fillBaseline)};
            g.fillStaging.append(reinterpret_cast<const char*>(&baseline), sizeof(baseline));
            g.fillStaging.append(reinterpret_cast<const char*>(&p), sizeof(p));
        };
        auto appendSegment = [&](GpuPoint a, GpuPoint b) {
            Segment segment{a, b};
            g.lineStaging.append(reinterpret_cast<const char*>(&segment), sizeof(segment));
        };
        for (qsizetype i = start; i < count; ++i) {
            const GpuPoint a = point(i);
            if (native) g.lineStaging.append(reinterpret_cast<const char*>(&a), sizeof(a));
            else if (points) appendSegment(a, a);
            if (s.spec.fill) appendFill(a);
            if (i + 1 == count) break;
            const GpuPoint b = point(i + 1);
            GpuPoint path[4] = {a, b, b, b};
            if (segments > 1) {
                // Calculate the transition before conversion to GPU float.
                const double transition = s.data[size_t(i)].x +
                    (s.data[size_t(i + 1)].x - s.data[size_t(i)].x) * s.spec.stepLocation - g.origin;
                const float x = float(transition);
                path[1] = {x, segments == 2 && s.spec.stepLocation == 0 ? b.y : a.y};
                path[2] = segments == 3 ? GpuPoint{x, b.y} : b;
            }
            for (int part = 0; part < segments; ++part) {
                if (!native && !points) appendSegment(path[part], path[part + 1]);
                if (s.spec.fill && part + 1 < segments) appendFill(path[part + 1]);
            }
        }
        const qsizetype lineStart = start * (native || points ? 1 : segments);
        if (!g.lineStaging.isEmpty()) up->updateDynamicBuffer(g.line.get(), quint32(lineStart * (native ? sizeof(GpuPoint) : sizeof(Segment))), g.lineStaging);
        if (!g.fillStaging.isEmpty()) up->updateDynamicBuffer(g.fill.get(), quint32(start * segments * 2 * sizeof(GpuPoint)), g.fillStaging);
        // Preserve runs before the changed suffix and repair the crossing run.
        qsizetype scan = start;
        while (!g.runs.empty() && g.runs.back().first >= start) g.runs.pop_back();
        if (!g.runs.empty() && g.runs.back().second > start) g.runs.back().second = start;
        for (; scan < count;) {
            while (scan < count && !std::isfinite(s.data[size_t(scan)].y)) ++scan;
            const qsizetype first = scan;
            while (scan < count && std::isfinite(s.data[size_t(scan)].y)) ++scan;
            if (scan > first) {
                if (!g.runs.empty() && g.runs.back().second == first) g.runs.back().second = scan;
                else g.runs.emplace_back(first, scan);
            }
        }
        s.dirty = count; g.uploaded = s.revision;
    }

    Uniform uniform(int xAxis, int yAxis, QColor color, double origin, bool band = false) {
        Uniform u{}; QMatrix4x4 m = device->clipSpaceCorrMatrix(), ortho;
        const Axis& y = (*axes)[yAxis];
        if (band) ortho.ortho(0.f, 1.f, float(y.lo), float(y.hi), -1.f, 1.f);
        else {
            const Axis& x = (*axes)[xAxis];
            ortho.ortho(float(x.lo - origin), float(x.hi - origin), float(y.lo), float(y.hi), -1.f, 1.f);
        }
        m *= ortho; std::memcpy(u.mvp, m.constData(), sizeof(u.mvp));
        u.color[0] = color.redF(); u.color[1] = color.greenF();
        u.color[2] = color.blueF(); u.color[3] = color.alphaF(); return u;
    }

    QVector<Axis>* axes; QVector<Series>* series; QVector<Band>* bands;
    QVector<Panel>* panels; QVector<int>* drawOrder;
    QRhi* device = nullptr;
    std::vector<Gpu> gpu, gpuBands;
    std::unique_ptr<QRhiBuffer> templateUbo;
    std::unique_ptr<QRhiShaderResourceBindings> templateSrb;
    std::unique_ptr<QRhiGraphicsPipeline> linePipe, nativePipe, fillPipe;
};

class Overlay final : public QWidget {
public:
    Overlay(QVector<Axis>* a, QVector<Series>* s, QVector<Panel>* p,
            QVector<ReferenceLine>* refs, QVector<ChartView::CursorGuide>* cursors,
            QWidget* parent)
        : QWidget(parent), axes(a), series(s), panels(p), references(refs),
          cursorGuides(cursors) {
        setAttribute(Qt::WA_TranslucentBackground); setAttribute(Qt::WA_NoSystemBackground); setMouseTracking(true);
    }

    void setPanelDividers(const QVector<PanelDivider>& dividers) {
        while (dividerFrames.size() < dividers.size()) {
            auto* line = new QFrame(this);
            line->setFrameShadow(QFrame::Sunken);
            line->setAttribute(Qt::WA_TransparentForMouseEvents);
            dividerFrames.push_back(line);
        }
        for (int i = 0; i < dividerFrames.size(); ++i) {
            QFrame* line = dividerFrames[i];
            const bool used = i < dividers.size();
            line->setVisible(used);
            if (!used) continue;
            line->setFrameShape(dividers[i].shape);
            line->setGeometry(dividers[i].rect);
            line->raise();
        }
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter q(this);
        q.setRenderHint(QPainter::TextAntialiasing);
        QColor background = palette().color(QPalette::Window);
        background.setAlpha(255);
        // The QRhi image is composited beneath this translucent widget AFTER
        // raster painting. Its dark background is therefore not available to
        // Qt's glyph blender: text painted onto transparent pixels falls back
        // to gray/uncorrected alpha blending and looks noticeably thinner than
        // ordinary widgets. Make the chart chrome opaque before painting text
        // (including the native controls), but leave every plot cutout clear.
        QRegion chrome(rect());
        for (const Panel& panel : *panels)
            if (panel.visible && !panel.plot.isEmpty()) chrome -= QRegion(panel.plot);
        q.save();
        q.setClipRegion(chrome);
        q.fillRect(rect(), background);
        q.restore();
        const QTransform toPixels = q.deviceTransform(), fromPixels = toPixels.inverted();
        auto drawText = [&](const QRectF& bounds, int flags, const QString& text) {
            // Legends may sit inside the plot. Give their text the same opaque
            // destination without covering the entire plot or changing weight.
            // Expand to device-pixel boundaries so edge glyph coverage is also
            // blended against an opaque pixel at fractional display scales.
            const QRect pixels = toPixels.mapRect(bounds).toAlignedRect();
            q.fillRect(fromPixels.mapRect(QRectF(pixels)), background);
            q.drawText(bounds, flags, text);
        };
        const QColor text = palette().color(QPalette::Text), muted = palette().color(QPalette::PlaceholderText);
        const QFont smallFont = chartLabelFont(font());
        QFont boldFont = smallFont; boldFont.setBold(true);
        const QFontMetricsF metrics(smallFont, this);
        const qreal labelHeight = std::ceil(metrics.height()) + 2;
        // Snap hairlines in DEVICE pixels; leave text and data at fractional
        // logical coordinates. Cosmetic pens stay one physical pixel wide.
        auto hairline = [&](QPointF a, QPointF b, const QColor& color, bool dashed = false) {
            auto snap = [&](QPointF point) {
                const QPointF physical = toPixels.map(point);
                return fromPixels.map(QPointF(std::floor(physical.x()) + .5, std::floor(physical.y()) + .5));
            };
            QPen pen(color, 1); pen.setCosmetic(true);
            if (dashed) pen.setDashPattern({4, 4});
            q.setPen(pen); q.drawLine(snap(a), snap(b));
        };
        for (Series& s : *series) s.legendHit = {};
        for (int pid = 0; pid < panels->size(); ++pid) {
            Panel& p = (*panels)[pid]; if (!p.visible || p.plot.isEmpty()) continue;
            // QRect::right/bottom are inclusive integer coordinates. QRectF
            // shares the GPU's x+width/y+height edges instead of shifting by 1.
            const QRectF plot(p.plot), outer(p.outer);
            q.save(); q.setClipRect(outer);
            if (p.header) {
                q.setFont(boldFont); q.setPen(muted);
                drawText(QRectF(outer.left() + kSidePad, outer.top(), outer.width() - kSidePad, kHeader),
                           Qt::AlignLeft | Qt::AlignVCenter, p.title);
            }
            for (const Axis& a : *axes) {
                if (!a.visible || a.panel != pid || a.hi <= a.lo) continue;
                const int capacity = a.side == ChartView::Side::Bottom
                    ? qMax(2, int(plot.width()) / qMax(1, a.tickSpacePx)) : 5;
                const double step = a.step > 0 ? a.step : niceStep(a.hi - a.lo, capacity);
                const QColor axisText = a.inherit ? text : a.color;
                const QVector<double> ticks = a.side == ChartView::Side::Bottom
                    ? ticksFor(a, capacity) : yTicksWithUpperBound(a, int(plot.height()), int(std::ceil(labelHeight + 2)));
                q.setFont(smallFont);
                if (a.side == ChartView::Side::Bottom) {
                    hairline(plot.bottomLeft(), plot.bottomRight(), kAxis);
                    auto labelRect = [&](double value, const QString& label) {
                        const double x = plot.left() + (value - a.lo) / (a.hi - a.lo) * plot.width();
                        const qreal width = metrics.horizontalAdvance(label) + 4;
                        return containedHorizontally(QRectF(a.lapBoundaryLabels ? x + 4 : x - width / 2,
                            plot.bottom() + 4, width, labelHeight), outer.adjusted(2, 0, -2, 0));
                    };
                    const QRectF lastRect = ticks.isEmpty() ? QRectF()
                        : labelRect(ticks.last(), mappedTickText(a, ticks.last(), step));
                    qreal previousRight = -std::numeric_limits<qreal>::infinity();
                    QString previousLabel;
                    for (int i = 0; i < ticks.size(); ++i) {
                        const double value = ticks[i];
                        const double x = plot.left() + (value - a.lo) / (a.hi - a.lo) * plot.width();
                        if (a.grid) hairline(QPointF(x, plot.top()), QPointF(x, plot.bottom()), kGrid);
                        hairline(QPointF(x, plot.bottom()), QPointF(x, plot.bottom() + 3), kAxis);
                        const QString label = mappedTickText(a, value, step);
                        const QRectF r = labelRect(value, label);
                        if (label.isEmpty() || label == previousLabel || r.left() < previousRight + 4) continue;
                        if (i > 0 && i + 1 < ticks.size() && r.right() + 4 > lastRect.left()) continue;
                        q.setPen(axisText);
                        drawText(r, (a.lapBoundaryLabels ? Qt::AlignLeft : Qt::AlignHCenter) | Qt::AlignVCenter, label);
                        previousRight = r.right(); previousLabel = label;
                    }
                } else {
                    const bool left = a.side == ChartView::Side::Left;
                    const double ax = left ? plot.left() - a.laneOffset : plot.right() + a.laneOffset;
                    hairline(QPointF(ax, plot.top()), QPointF(ax, plot.bottom()), kAxis);
                    qreal previousBottom = -std::numeric_limits<qreal>::infinity();
                    QString previousLabel;
                    // Top bound wins when a short panel cannot fit every label.
                    for (auto it = ticks.crbegin(); it != ticks.crend(); ++it) {
                        const double y = plot.top() + (a.hi - *it) / (a.hi - a.lo) * plot.height();
                        if (a.grid) hairline(QPointF(plot.left(), y), QPointF(plot.right(), y), kGrid);
                        hairline(QPointF(ax + (left ? -3 : 0), y), QPointF(ax + (left ? 0 : 3), y), kAxis);
                        const QString label = mappedTickText(a, *it, step);
                        QRectF r(left ? ax - kAxisTextGap - a.labelWidth : ax + kAxisTextGap,
                                 y - labelHeight / 2, a.labelWidth, labelHeight);
                        r = containedHorizontally(r, outer.adjusted(2, 0, -2, 0));
                        if (label.isEmpty() || label == previousLabel || r.top() < previousBottom + 2) continue;
                        q.setPen(axisText);
                        drawText(r, (left ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter, label);
                        previousBottom = r.bottom(); previousLabel = label;
                    }
                }
            }
            for (const auto& ref : *references) {
                const Axis& axis = (*axes)[ref.axis];
                if (axis.panel != pid || ref.value < axis.lo || ref.value > axis.hi) continue;
                const double y = plot.top() + plot.height() * (axis.hi - ref.value) / (axis.hi - axis.lo);
                hairline(QPointF(plot.left(), y), QPointF(plot.right(), y), kAxis, ref.dashed);
            }
            int guideAxis = -1;
            for (int i = 0; i < axes->size(); ++i)
                if ((*axes)[i].panel == pid && (*axes)[i].side == ChartView::Side::Bottom) {
                    guideAxis = i;
                    break;
                }
            if (guideAxis >= 0 && !cursorGuides->isEmpty()) {
                const Axis& axis = (*axes)[guideAxis];
                QVector<double> guidePixels(cursorGuides->size(), qQNaN());
                QVector<double> playheadPixels(cursorGuides->size(), qQNaN());
                for (int i = 0; i < cursorGuides->size(); ++i) {
                    const double value = (*cursorGuides)[i].x;
                    if (!std::isfinite(value) || value < axis.lo || value > axis.hi ||
                        axis.hi <= axis.lo) continue;
                    const double pixel = plot.left() +
                        (value - axis.lo) / (axis.hi - axis.lo) * plot.width();
                    guidePixels[i] = playheadPixels[i] = pixel;
                }
                if (guidePixels.size() == 2 && std::isfinite(guidePixels[0]) &&
                    std::isfinite(guidePixels[1])) {
                    const double gap = std::abs(guidePixels[1] - guidePixels[0]);
                    if (gap < 2.0) {
                        guidePixels[0] -= 0.75;
                        guidePixels[1] += 0.75;
                    }
                    constexpr double playheadGap = 11.0;
                    if (gap < playheadGap) {
                        const double midpoint = (playheadPixels[0] + playheadPixels[1]) / 2.0;
                        playheadPixels[0] = midpoint - playheadGap / 2.0;
                        playheadPixels[1] = midpoint + playheadGap / 2.0;
                    }
                }

                q.save();
                q.setClipRect(plot);
                constexpr double halfWidth = 5.0;
                constexpr double playheadHeight = 9.0;
                const double top = plot.top() + 1.0;
                for (int i = 0; i < cursorGuides->size(); ++i) {
                    if (!std::isfinite(guidePixels[i])) continue;
                    const QColor base = (*cursorGuides)[i].color;
                    QColor halo = base; halo.setAlphaF(base.alphaF() * 0.07);
                    QPen haloPen(halo, 7.0, Qt::SolidLine, Qt::RoundCap);
                    q.setPen(haloPen);
                    q.drawLine(QPointF(guidePixels[i], top + playheadHeight),
                               QPointF(guidePixels[i], plot.bottom()));

                    QColor guide = base; guide.setAlphaF(base.alphaF() * 0.58);
                    QPen guidePen(guide, 1.0, Qt::CustomDashLine, Qt::RoundCap);
                    guidePen.setDashPattern({2.0, 4.0});
                    q.setPen(guidePen);
                    q.drawLine(QPointF(guidePixels[i], top + playheadHeight),
                               QPointF(guidePixels[i], plot.bottom()));

                    const double center = playheadPixels[i];
                    QPainterPath playhead;
                    playhead.moveTo(center - halfWidth, top);
                    playhead.lineTo(center + halfWidth, top);
                    playhead.lineTo(center + halfWidth, top + 4.0);
                    playhead.lineTo(guidePixels[i], top + playheadHeight);
                    playhead.lineTo(center - halfWidth, top + 4.0);
                    playhead.closeSubpath();
                    q.fillPath(playhead, base);
                    q.setPen(QPen(background, 1.5, Qt::SolidLine, Qt::SquareCap,
                                  Qt::RoundJoin));
                    q.drawPath(playhead);
                }
                q.restore();
            }
            if (p.legend) {
                q.setFont(smallFont); QVector<int> ids; qreal total = 0;
                for (int i = 0; i < series->size(); ++i) if ((*series)[i].panel == pid && !(*series)[i].spec.name.isEmpty()) {
                    ids.push_back(i); total += 24 + metrics.horizontalAdvance((*series)[i].spec.name);
                }
                qreal x = p.header ? outer.right() - kSidePad - total : plot.center().x() - total / 2;
                const qreal y = p.header ? outer.top() + (kHeader - labelHeight) / 2 : plot.top() + 4;
                for (int id : ids) {
                    Series& s = (*series)[id]; const qreal w = 24 + metrics.horizontalAdvance(s.spec.name);
                    q.setPen(QPen(s.spec.color, 2));
                    q.drawLine(QPointF(x, y + labelHeight / 2), QPointF(x + 12, y + labelHeight / 2));
                    q.setPen(s.visible ? text : muted);
                    drawText(QRectF(x + 16, y, w - 16, labelHeight), Qt::AlignVCenter, s.spec.name);
                    s.legendHit = QRectF(x - 2, y - 3, w, labelHeight + 6).toAlignedRect(); x += w;
                }
            }
            int xAxis = -1;
            for (int i = 0; i < axes->size(); ++i)
                if ((*axes)[i].panel == pid && (*axes)[i].side == ChartView::Side::Bottom) { xAxis = i; break; }
            if (p.cursorV && xAxis >= 0) {
                const Axis& a = (*axes)[xAxis];
                const double x = plot.left() + (p.cursorX - a.lo) / (a.hi - a.lo) * plot.width();
                hairline(QPointF(x, plot.top()), QPointF(x, plot.bottom()), QColor(150,150,150,160));
            }
            if (p.cursorH) {
                const double y = plot.top() + p.cursorY * plot.height();
                hairline(QPointF(plot.left(), y), QPointF(plot.right(), y), QColor(150,150,150,120));
            }
            q.restore();
        }
    }
private:
    QVector<Axis>* axes; QVector<Series>* series; QVector<Panel>* panels;
    QVector<ReferenceLine>* references;
    QVector<ChartView::CursorGuide>* cursorGuides;
    QVector<QFrame*> dividerFrames;
};
} // namespace

struct ChartView::Impl {
    RhiCanvas* canvas = nullptr; Overlay* overlay = nullptr; QLabel* tooltip = nullptr;
    QVector<Axis> axes; QVector<Series> series; QVector<Band> bands; QVector<Panel> panels{Panel{}};
    QVector<ReferenceLine> references;
    QVector<ChartView::CursorGuide> cursorGuides;
    QVector<int> order; QVector<QVector<int>> rows; QVector<int> linkedXAxes; int columns = 1;
    bool explicitRows = false, hover = false, sync = false, secondaryV = true, secondaryH = false;
    QString cursorMode; QPointer<SessionModel> model;
    int navAxis = -1; bool nav = false, dragging = false;
    double navMin = 0, navMax = 1, navSpan = .5, dragMin = 0, dragMax = 1;
    int dragWidth = 1; QPoint dragStart;
    QTimer* hoverTimer = nullptr;
    QPoint hoverPosition;
    bool hoverActive = false;

    QVector<QVector<int>> layoutRows() const {
        if (explicitRows) return rows;
        QVector<QVector<int>> out; QVector<int> row;
        for (int i = 0; i < panels.size(); ++i) if (panels[i].visible) {
            row.push_back(i); if (row.size() == columns) { out.push_back(row); row.clear(); }
        }
        if (!row.isEmpty()) out.push_back(row); return out;
    }
    void geometry(QRect bounds) {
        for (Panel& p : panels) { p.outer = {}; p.plot = {}; }
        const QFont axisFont = chartLabelFont(overlay ? overlay->font() : QFont());
        const QFontMetricsF axisMetrics(axisFont, overlay);
        QVector<PanelDivider> dividers;
        auto lr = layoutRows();
        if (lr.isEmpty()) {
            if (overlay) overlay->setPanelDividers(dividers);
            return;
        }
        int availableH = qMax(1, bounds.height() - kGap * (lr.size() - 1)), y = bounds.top();
        for (int ri = 0; ri < lr.size(); ++ri) {
            int h = availableH / lr.size() + (ri < availableH % lr.size()), availableW = qMax(1, bounds.width() - kGap * (lr[ri].size() - 1)), x = bounds.left();
            for (int ci = 0; ci < lr[ri].size(); ++ci) {
                int w = availableW / lr[ri].size() + (ci < availableW % lr[ri].size()), id = lr[ri][ci];
                if (id >= 0 && id < panels.size()) {
                    Panel& p = panels[id]; p.outer = QRect(x, y, w, h);
                    QVector<int> leftAxes, rightAxes, bottomAxes;
                    for (int axisId = 0; axisId < axes.size(); ++axisId) {
                        const Axis& axis = axes[axisId];
                        if (!axis.visible || axis.panel != id) continue;
                        if (axis.side == ChartView::Side::Left) leftAxes.push_back(axisId);
                        else if (axis.side == ChartView::Side::Right) rightAxes.push_back(axisId);
                        else bottomAxes.push_back(axisId);
                    }
                    auto sideInset = [&](const QVector<int>& sideAxes) {
                        if (sideAxes.isEmpty()) return kPlotEdgePad;
                        int used = 0;
                        for (int index = 0; index < sideAxes.size(); ++index) {
                            Axis& axis = axes[sideAxes[index]];
                            axis.labelWidth = measuredYAxisLabelWidth(axis, axisMetrics);
                            axis.laneOffset = used;
                            used += axis.labelWidth + kAxisTextGap;
                            if (index + 1 < sideAxes.size()) used += kAxisLaneGap;
                        }
                        return used + kPlotEdgePad;
                    };
                    int leftInset = sideInset(leftAxes);
                    int rightInset = sideInset(rightAxes);
                    for (int axisId : bottomAxes) {
                        const Axis& axis = axes[axisId];
                        const int capacity = qMax(2, w / qMax(1, axis.tickSpacePx));
                        const QVector<double> ticks = ticksFor(axis, capacity);
                        if (ticks.isEmpty()) continue;
                        const int firstWidth = int(std::ceil(axisMetrics.horizontalAdvance(mappedTickText(axis, ticks.first()))));
                        const int lastWidth = int(std::ceil(axisMetrics.horizontalAdvance(mappedTickText(axis, ticks.last()))));
                        if (axis.lapBoundaryLabels) {
                            rightInset = qMax(rightInset, lastWidth + kPlotEdgePad);
                        } else {
                            leftInset = qMax(leftInset, firstWidth / 2 + kPlotEdgePad);
                            rightInset = qMax(rightInset, lastWidth / 2 + kPlotEdgePad);
                        }
                    }
                    const int topInset = p.header
                        ? kHeader + kPlotEdgePad
                        : qMax(kPlotEdgePad, int(std::ceil((axisMetrics.height() + 2) / 2)));
                    p.plot = p.outer.adjusted(leftInset, topInset,
                                              -rightInset, -(bottomAxes.isEmpty() ? 4 : int(std::ceil(axisMetrics.height())) + 10));
                    if (p.plot.width() < 8 || p.plot.height() < 8) p.plot = {};
                }
                x += w;
                if (ci + 1 < lr[ri].size()) {
                    dividers.push_back({ QRect(x, y, kGap, h), QFrame::VLine });
                    x += kGap;
                }
            }
            y += h;
            if (ri + 1 < lr.size()) {
                dividers.push_back({ QRect(bounds.left(), y, bounds.width(), kGap), QFrame::HLine });
                y += kGap;
            }
        }
        if (overlay) overlay->setPanelDividers(dividers);
    }
};

ChartView::ChartView(QWidget* parent) : QWidget(parent), d_(std::make_unique<Impl>()) {
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding); setMinimumHeight(120);
    auto* layout = new QVBoxLayout(this); layout->setContentsMargins(0,0,0,0);
    d_->canvas = new RhiCanvas(&d_->axes, &d_->series, &d_->bands, &d_->panels, &d_->order, this);
    layout->addWidget(d_->canvas); d_->overlay = new Overlay(
        &d_->axes, &d_->series, &d_->panels, &d_->references, &d_->cursorGuides, this);
    d_->overlay->installEventFilter(this); d_->overlay->raise(); liveCharts().push_back(this);
    d_->hoverTimer = new QTimer(this);
    d_->hoverTimer->setSingleShot(true);
    d_->hoverTimer->setInterval(16);
    connect(d_->hoverTimer, &QTimer::timeout, this, [this] {
        if (d_->hoverActive && isVisible() && !d_->dragging) updateHover(d_->hoverPosition);
    });
    connect(d_->canvas, &QRhiWidget::renderFailed, this, [] { qCritical("[charts] Rendering failed; select another backend in Settings and restart"); });
}
ChartView::~ChartView() { liveCharts().removeAll(this); }
void ChartView::suspendOpenGlForStyleChange() {}
void ChartView::reapplyRenderSettings() { for (auto* v : liveCharts()) { v->d_->canvas->applySettings(); v->d_->overlay->update(); } }

int ChartView::addAxis(const AxisSpec& s, int panel) {
    panel = qBound(0, panel, d_->panels.size()-1); Axis a; a.side=s.side; a.lo=s.min; a.hi=s.max;
    a.color=s.labelColor; a.inherit=!s.labelColor.isValid(); a.visible=s.visible; a.format=s.numberFormat;
    a.precision=s.precision; a.grid=s.grid; a.tickSpacePx=s.tickSpacePx; a.panel=panel;
    d_->axes.push_back(a); d_->geometry(rect()); return d_->axes.size()-1;
}
int ChartView::addSeries(const SeriesSpec& s) {
    Series v; v.spec=s;
    v.spec.width = std::isfinite(s.width) ? qMax(0., s.width) : 2.;
    v.spec.opacity = std::isfinite(s.opacity) ? qBound(0., s.opacity, 1.) : 1.;
    v.spec.stepLocation = std::isfinite(s.stepLocation) ? qBound(0., s.stepLocation, 1.) : 1.;
    v.spec.fillBaseline = std::isfinite(s.fillBaseline) ? s.fillBaseline : 0.;
    if(s.yAxisId>=0&&s.yAxisId<d_->axes.size())v.panel=d_->axes[s.yAxisId].panel;
    d_->series.push_back(std::move(v)); int id=d_->series.size()-1; d_->order.push_back(id); return id;
}
void ChartView::addBand(const BandSpec& s) { Band b; b.spec=s; if(s.axisId>=0&&s.axisId<d_->axes.size())b.panel=d_->axes[s.axisId].panel; d_->bands.push_back(b); }
void ChartView::addReferenceLine(int axis, double value, bool dashed) {
    if (axis < 0 || axis >= d_->axes.size() || d_->axes[axis].side == Side::Bottom || !std::isfinite(value)) return;
    d_->references.push_back({axis, value, dashed}); requestReplot();
}

void ChartView::setCursorGuides(const QVector<CursorGuide>& guides) {
    if (d_->cursorGuides == guides) return;
    d_->cursorGuides = guides;
    if (d_->overlay && isVisible()) d_->overlay->update();
}

void ChartView::appendPoint(int id, double x, double y) {
    if (id < 0 || id >= d_->series.size() || !std::isfinite(x)) return;
    Series& s = d_->series[id];
    const Point p{x, float(y)}; // Non-finite Y is an explicit gap, including on append.
    if (!s.data.empty() && x < s.data.back().x) {
        auto at = std::lower_bound(s.data.begin() + s.first, s.data.end(), x,
            [](const Point& point, double key) { return point.x < key; });
        const qsizetype i = std::distance(s.data.begin(), at);
        s.data.insert(at, p); s.dirty = qMin(s.dirty, i); s.fitDirty = qMin(s.fitDirty, i);
    } else {
        s.dirty = qMin(s.dirty, qsizetype(s.data.size()));
        s.fitDirty = qMin(s.fitDirty, qsizetype(s.data.size())); s.data.push_back(p);
    }
    while (s.size() > kMaxPoints) ++s.first;
    compact(s); ++s.revision;
}

void ChartView::setSeriesData(int id, const QVector<double>& xs, const QVector<double>& ys) {
    if (id < 0 || id >= d_->series.size() || xs.size() != ys.size()) return;
    Series& s = d_->series[id];
    std::vector<Point> replacement;
    replacement.reserve(size_t(qMin<qsizetype>(kMaxPoints, xs.size())));
    for (qsizetype i = qMax<qsizetype>(0, xs.size() - kMaxPoints); i < xs.size(); ++i)
        if (std::isfinite(xs[i])) replacement.push_back({xs[i], float(ys[i])});
    // Binary search, clipping and hover all require ordered keys. Equal keys
    // retain source order, including vertical edges and explicit gap samples.
    if (!std::is_sorted(replacement.begin(), replacement.end(),
            [](const Point& a, const Point& b) { return a.x < b.x; }))
        std::stable_sort(replacement.begin(), replacement.end(),
            [](const Point& a, const Point& b) { return a.x < b.x; });
    const qsizetype n = qsizetype(replacement.size());
    qsizetype equal = 0;
    while (equal < qMin(s.size(), n)) {
        const auto& old = s.data[size_t(s.first + equal)];
        const auto& next = replacement[size_t(equal)];
        if (old.x != next.x || !(old.y == next.y || (std::isnan(old.y) && std::isnan(next.y)))) break;
        ++equal;
    }
    if (equal == n && n == s.size()) return;
    // Preserve the unchanged prefix and upload just the changed suffix. Checking
    // endpoints alone misses interior corrections (notably analysis delta data).
    s.data.resize(size_t(s.first + equal));
    s.data.insert(s.data.end(), replacement.begin() + equal, replacement.end());
    s.dirty = qMin(s.dirty, s.first + equal);
    s.fitDirty = qMin(s.fitDirty, s.first + equal);
    compact(s); ++s.revision;
}

void ChartView::trimBefore(int id, double x) {
    if (id < 0 || id >= d_->series.size() || !std::isfinite(x)) return;
    Series& s = d_->series[id];
    qsizetype first = lowerBound(s, x);
    if (first > s.first) --first; // Segment crossing the viewport's left edge.
    s.first = first; compact(s);
}
void ChartView::clear(int id){if(id<0||id>=d_->series.size())return;Series&s=d_->series[id];s.data.clear();s.first=s.dirty=s.fitDirty=0;++s.revision;}
void ChartView::clearAll(){for(int i=0;i<d_->series.size();++i)clear(i);}
void ChartView::setSeriesVisible(int id, bool on) {
    if (id < 0 || id >= d_->series.size() || d_->series[id].visible == on) return;
    Series& s = d_->series[id]; s.visible = on;
    if (s.spec.yAxisId >= 0 && s.spec.yAxisId < d_->axes.size()) d_->axes[s.spec.yAxisId].fitTimer.invalidate();
}
bool ChartView::seriesVisible(int id)const{return id>=0&&id<d_->series.size()&&d_->series[id].visible;}
void ChartView::setSeriesColor(int id,const QColor&c){if(id>=0&&id<d_->series.size())d_->series[id].spec.color=c;}
void ChartView::setSeriesName(int id,const QString&n){if(id>=0&&id<d_->series.size())d_->series[id].spec.name=n;}
void ChartView::setSeriesWidth(int id, double width) {
    if (id < 0 || id >= d_->series.size() || !std::isfinite(width)) return;
    d_->series[id].spec.width = qMax(0., width); requestReplot();
}
void ChartView::setSeriesLineType(int id, LineType type) {
    if (id < 0 || id >= d_->series.size()) return;
    Series& s = d_->series[id];
    if (lineType(s) == type) return;
    s.spec.step = false; s.spec.lineType = type; s.dirty = 0; ++s.revision; requestReplot();
}
void ChartView::setSeriesStepLocation(int id, double location) {
    if (id < 0 || id >= d_->series.size() || !std::isfinite(location)) return;
    Series& s = d_->series[id]; location = qBound(0., location, 1.);
    if (s.spec.stepLocation == location) return;
    s.spec.stepLocation = location; s.dirty = 0; ++s.revision; requestReplot();
}
void ChartView::setSeriesFillBaseline(int id, double baseline) {
    if (id < 0 || id >= d_->series.size() || !std::isfinite(baseline)) return;
    Series& s = d_->series[id];
    if (s.spec.fillBaseline == baseline) return;
    s.spec.fillBaseline = baseline; s.dirty = 0; ++s.revision; requestReplot();
}
void ChartView::setSeriesOpacity(int id, double opacity) {
    if (id < 0 || id >= d_->series.size() || !std::isfinite(opacity)) return;
    d_->series[id].spec.opacity = qBound(0., opacity, 1.); requestReplot();
}
void ChartView::setAxisNativeLines(int axis, bool enabled) {
    for (int id = 0; id < d_->series.size(); ++id) {
        const Series& s = d_->series[id];
        if (s.spec.xAxisId != axis) continue;
        const auto type = lineType(s);
        if (type == LineType::Line || type == LineType::NativeLine)
            setSeriesLineType(id, enabled ? LineType::NativeLine : LineType::Line);
    }
}
void ChartView::setSeriesOrder(const QVector<int>& ids){QVector<int>o;for(int id:ids)if(id>=0&&id<d_->series.size()&&!o.contains(id))o.push_back(id);for(int id:d_->order)if(!o.contains(id))o.push_back(id);d_->order=o;}
void ChartView::linkSeriesVisibility(int a,int b){if(a>=0&&a<d_->series.size())d_->series[a].linked=b;}
void ChartView::setAxisVisible(int id,bool on){if(id>=0&&id<d_->axes.size()&&d_->axes[id].visible!=on){d_->axes[id].visible=on;d_->geometry(rect());}}
void ChartView::setAxisColor(int id,const QColor&c){if(id>=0&&id<d_->axes.size()){d_->axes[id].color=c;d_->axes[id].inherit=false;}}
void ChartView::setAxisGridVisible(int id,bool on){if(id>=0&&id<d_->axes.size())d_->axes[id].grid=on;}
void ChartView::setLegendVisible(bool on){if(!d_->panels.isEmpty())d_->panels[0].legend=on;}

int ChartView::addPanel(){d_->panels.push_back(Panel{});return d_->panels.size()-1;}
void ChartView::ensurePanelHeader(int id){if(id>=0&&id<d_->panels.size()){d_->panels[id].header=true;d_->geometry(rect());}}
void ChartView::layoutPanels(int cols){d_->columns=qMax(1,cols);d_->explicitRows=false;applyPanelLayout();}
void ChartView::layoutPanelsRows(const QVector<QVector<int>>&rows){d_->rows=rows;d_->explicitRows=true;for(auto&p:d_->panels)p.visible=false;for(const auto&r:rows)for(int id:r)if(id>=0&&id<d_->panels.size())d_->panels[id].visible=true;applyPanelLayout();}
void ChartView::applyPanelLayout(){d_->geometry(rect());positionPanelChartSettings();requestReplot();}
void ChartView::setPanelVisible(int id,bool on){if(id>=0&&id<d_->panels.size()){d_->panels[id].visible=on;d_->explicitRows=false;applyPanelLayout();}}
void ChartView::setPanelTitle(int id,const QString&t){if(id>=0&&id<d_->panels.size()){ensurePanelHeader(id);d_->panels[id].title=t;requestReplot();}}
void ChartView::setPanelLegendVisible(int id,bool on){if(id>=0&&id<d_->panels.size()){ensurePanelHeader(id);d_->panels[id].legend=on;}}
void ChartView::setAxisTimeTicker(int id,const QString& format){if(id>=0&&id<d_->axes.size()){auto&a=d_->axes[id];a.timeFormat=format;a.time=true;a.lapBoundaryLabels=false;a.ticks.clear();a.labels.clear();}}
void ChartView::setAxisDistanceMode(int id,bool on){if(id>=0&&id<d_->axes.size())d_->axes[id].distance=on;}

void ChartView::syncAxisSessionMap(int id,const LapBlock*lap,float now){
    if(id<0||id>=d_->axes.size())return;Axis&a=d_->axes[id];if(!lap){a.sessionKeys.clear();a.sessionTimes.clear();a.lapNum=-1;a.lapStart=-1;return;}
    bool changed=a.lapNum!=lap->lapNum||a.lapStart!=lap->startSessionTime,rewound=!a.sessionTimes.isEmpty()&&now<a.sessionTimes.last();
    if(changed||rewound){a.sessionKeys.clear();a.sessionTimes.clear();a.lapNum=lap->lapNum;a.lapStart=lap->startSessionTime;}
    float after=a.sessionTimes.isEmpty()?-std::numeric_limits<float>::infinity():float(a.sessionTimes.last());
    auto it=std::upper_bound(lap->progress.begin(),lap->progress.end(),after,[](float v,const LapProgressSample&p){return v<p.t;});
    for(;it!=lap->progress.end()&&it->t<=now;++it){a.sessionKeys.push_back(it->distanceM);a.sessionTimes.push_back(it->t);}
}

void ChartView::bindPanelChartSettings(int id,SessionModel*model,tnr::GraphSection section){
    if(id<0||id>=d_->panels.size()||!model)return;ensurePanelHeader(id);Panel&p=d_->panels[id];p.section=section;d_->model=model;
    if(!p.window){p.window=new InsetComboBox(this);p.window->setFrame(false);p.window->setFixedSize(106,kControlH);p.window->setToolTip("Chart window override");
        connect(p.window,QOverload<int>::of(&QComboBox::activated),this,[this,id](int i){auto&p=d_->panels[id];if(d_->model)d_->model->setChartWindow(p.section,chartWindowFromKey(p.window->itemData(i).toString()));});
        p.lap=new InsetComboBox(this);p.lap->setFrame(false);p.lap->setFixedSize(kLapControlW,kControlH);p.lap->setToolTip("Selected reference lap for this chart");connect(p.lap,QOverload<int>::of(&QComboBox::activated),this,[this,id](int i){auto&p=d_->panels[id];if(d_->model)d_->model->setReferenceLap(p.section,p.lap->itemData(i).toInt());});}
    connect(model,&SessionModel::chartConfigurationChanged,this,&ChartView::refreshPanelChartSettings,Qt::UniqueConnection);connect(model,&SessionModel::lapsChanged,this,&ChartView::refreshPanelChartSettings,Qt::UniqueConnection);refreshPanelChartSettings();
}

void ChartView::refreshPanelChartSettings(){
    if(!d_->model)return;bool coords=d_->model->lapCoordinatesAvailable();for(Panel&p:d_->panels){if(!p.window||p.section==tnr::GraphSection::Count_)continue;
        p.window->blockSignals(true);p.window->clear();
        const ChartWindow values[]={ChartWindow::Seconds15,ChartWindow::Seconds30,ChartWindow::Seconds60,ChartWindow::Seconds120,ChartWindow::Seconds300,ChartWindow::Seconds600,ChartWindow::CurrentLap,ChartWindow::PreviousLap,ChartWindow::FastestLap,ChartWindow::SelectedLap,ChartWindow::StintLaps,ChartWindow::AllLaps};
        for(auto w:values)if(chartWindowIsAvailable(w,coords,d_->model->playbackMode()))p.window->addItem(chartWindowLabel(w),chartWindowKey(w));
        int i=p.window->findData(chartWindowKey(d_->model->effectiveChartWindow(p.section)));p.window->setCurrentIndex(i>=0?i:0);p.window->blockSignals(false);
        int wanted=d_->model->referenceLap(p.section);p.lap->blockSignals(true);p.lap->clear();for(const LapBlock&lap:d_->model->data().laps)if(!lap.progress.isEmpty()||d_->model->playbackCatalogHasLapDistance())p.lap->addItem(QString::number(lap.lapNum),lap.lapNum);i=p.lap->findData(wanted);p.lap->setCurrentIndex(i>=0?i:(p.lap->count()?0:-1));p.lap->blockSignals(false);}positionPanelChartSettings();
}

void ChartView::positionPanelChartSettings(){for(Panel&p:d_->panels){if(!p.window)continue;bool show=p.visible&&!p.outer.isEmpty();p.window->setVisible(show);bool lap=show&&d_->model&&d_->model->playbackMode()&&d_->model->effectiveChartWindow(p.section)==ChartWindow::SelectedLap;p.lap->setVisible(lap);if(!show)continue;QFont f=chartLabelFont(font());f.setBold(true);int title=int(std::ceil(QFontMetricsF(f,d_->overlay).horizontalAdvance(p.title)));int x=p.outer.left()+kSidePad+title+kTitleControlGap,y=p.outer.top()+(kHeader-kControlH)/2;p.window->move(x,y);p.window->raise();if(lap){p.lap->move(x+p.window->width()+3,y);p.lap->raise();}}if(d_->tooltip)d_->tooltip->raise();}

void ChartView::setAxisLabelMap(int id,const QVector<double>&ticks,const QStringList&labels,bool lapBoundaryLabels){if(id>=0&&id<d_->axes.size()){auto&a=d_->axes[id];a.time=false;a.lapBoundaryLabels=lapBoundaryLabels;a.ticks=ticks;a.labels=labels;if(a.side!=Side::Bottom)d_->geometry(rect());}}
void ChartView::setAxisNumberSuffix(int id,double scale,const QString&suffix,double step){if(id>=0&&id<d_->axes.size()){auto&a=d_->axes[id];a.time=false;a.scale=std::isfinite(scale)&&scale>0?scale:1.;a.suffix=suffix;a.step=std::isfinite(step)&&step>0?step:0.;if(a.side!=Side::Bottom)d_->geometry(rect());}}
void ChartView::setHoverReadout(bool on){d_->hover=on;if(on&&!d_->tooltip){d_->tooltip=new QLabel(this);QFont tooltipFont=font();tooltipFont.setFeature(QFont::Tag("tnum"),1);d_->tooltip->setFont(tooltipFont);d_->tooltip->setTextFormat(Qt::RichText);d_->tooltip->setAttribute(Qt::WA_TransparentForMouseEvents);d_->tooltip->hide();applyPaletteText();}if(!on)clearSyncedCursor();}
void ChartView::setCursorSync(bool on,bool v,bool h){bool clear=d_->sync&&(!on||d_->secondaryV!=v||d_->secondaryH!=h);d_->sync=on;d_->secondaryV=v;d_->secondaryH=h;if(clear)clearSyncedCursor();}
void ChartView::setCursorModeKey(const QString&key){if(d_->cursorMode==key)return;d_->cursorMode=key;for(auto*c:liveCharts())c->clearSyncedCursor();}

QString ChartView::showSyncedCursor(double time,double sourceX,bool sourceDistance,double yRatio,ChartView*source,int sourcePanel){
    if(!d_->sync||!d_->hover)return{};if(source!=this){d_->hoverActive=false;d_->hoverTimer->stop();if(d_->tooltip)d_->tooltip->hide();}QString html;
    for(int pid=0;pid<d_->panels.size();++pid){Panel&p=d_->panels[pid];int xid=-1;for(int i=0;i<d_->axes.size();++i)if(d_->axes[i].panel==pid&&d_->axes[i].side==Side::Bottom){xid=i;break;}if(xid<0)continue;const Axis&a=d_->axes[xid];bool target=a.distance;double key=sourceDistance==target?sourceX:target?interpolate(a.sessionTimes,a.sessionKeys,time):time;bool mapped=sourceDistance==target||!target||(!a.sessionTimes.isEmpty()&&time>=a.sessionTimes.first()&&time<=a.sessionTimes.last());
        if(source==this&&pid==sourcePanel)continue;
        if(!p.visible||!mapped||key<a.lo||key>a.hi){if(!(source==this&&pid==sourcePanel))p.cursorV=false;p.cursorH=false;continue;}if(!(source==this&&pid==sourcePanel)){p.cursorX=key;p.cursorV=d_->secondaryV;}p.cursorY=yRatio;p.cursorH=d_->secondaryH&&!(source==this&&pid==sourcePanel);bool any=false;
        for(const Series&s:d_->series){if(s.panel!=pid||!s.visible||s.spec.name.isEmpty()||s.empty())continue;double lo=s.data[size_t(s.first)].x,hi=s.data.back().x;bool endpoint=sourceDistance==target&&key>hi;if(key<lo||(key>hi&&!endpoint))continue;qsizetype at=nearest(s,key);if(at<0||!std::isfinite(s.data[size_t(at)].y))continue;QString value=numberText(s.data[size_t(at)].y,'f',s.spec.tipPrecision,s.spec.tipGroupThousands);if(!s.spec.unit.isEmpty())value+=(s.spec.unit=="%"?"":" ")+s.spec.unit;html+=QString("<div style='color:%1'><b>%2:</b> %3</div>").arg(s.spec.color.name(),s.spec.name.toHtmlEscaped(),value.toHtmlEscaped());any=true;}if(!any&&!(source==this&&pid==sourcePanel)){p.cursorV=p.cursorH=false;}}
    d_->overlay->update();return html;
}
void ChartView::clearSyncedCursor(){d_->hoverActive=false;if(d_->hoverTimer)d_->hoverTimer->stop();for(Panel&p:d_->panels)p.cursorV=p.cursorH=false;if(d_->tooltip)d_->tooltip->hide();if(d_->overlay)d_->overlay->update();}
bool ChartView::seriesKeyRange(int id,double&lo,double&hi)const{if(id<0||id>=d_->series.size()||d_->series[id].empty())return false;const Series&s=d_->series[id];lo=s.data[size_t(s.first)].x;hi=s.data.back().x;return true;}
void ChartView::setXRange(int id,double lo,double hi){
    if(id<0||id>=d_->axes.size()||!std::isfinite(lo)||!std::isfinite(hi)||hi<=lo)return;
    auto apply=[&](int axisId){if(axisId<0||axisId>=d_->axes.size())return;Axis&a=d_->axes[axisId];const int oldWidth=a.labelWidth;a.lo=lo;a.hi=hi;if(a.side!=Side::Bottom){QFont f=chartLabelFont(font());if(measuredYAxisLabelWidth(a,QFontMetricsF(f,d_->overlay))!=oldWidth)d_->geometry(rect());}};
    apply(id);if(d_->linkedXAxes.contains(id))for(int linked:d_->linkedXAxes)if(linked!=id)apply(linked);
}
void ChartView::setAxisRange(int id,double lo,double hi){setXRange(id,lo,hi);}

void ChartView::fitAxisToVisibleSeries(int id, const QVector<int>& ids, double fixedLo,
                                       double fixedHi, bool dynamic, bool expand) {
    if (id < 0 || id >= d_->axes.size() || !std::isfinite(fixedLo) ||
        !std::isfinite(fixedHi) || fixedHi <= fixedLo) return;
    Axis& a = d_->axes[id];
    if (!dynamic && !expand) { setAxisRange(id, fixedLo, fixedHi); return; }
    const bool full = !a.fitTimer.isValid() || a.fitTimer.elapsed() >= 200;
    if (full) a.fitTimer.restart();
    bool found = false;
    double lo = 0, hi = 0;
    for (int sid : ids) {
        if (sid < 0 || sid >= d_->series.size()) continue;
        Series& s = d_->series[sid];
        if (!s.visible || s.empty() || s.spec.xAxisId < 0 || s.spec.xAxisId >= d_->axes.size()) continue;
        const Axis& x = d_->axes[s.spec.xAxisId];
        qsizetype begin = lowerBound(s, x.lo);
        if (begin > s.first) --begin;
        const qsizetype end = qMin(lowerBound(s, x.hi) + 1, qsizetype(s.data.size()));
        if (!full) begin = qMax(begin, s.fitDirty);
        for (qsizetype i = begin; i < end; ++i) {
            const double value = s.data[size_t(i)].y;
            if (!std::isfinite(value)) continue;
            if (!found) { lo = hi = value; found = true; }
            else { lo = qMin(lo, value); hi = qMax(hi, value); }
        }
        s.fitDirty = end;
    }
    if (!found) {
        if (full) setAxisRange(id, fixedLo, fixedHi);
        return;
    }
    if (!dynamic) {
        setAxisRange(id, fixedLo, qMax(fixedHi, qMax(hi, full ? fixedHi : a.hi)));
    } else {
        const double pad = (hi == lo ? std::abs(hi) * .05 + 1 : (hi - lo) * .08);
        // New samples expand immediately; only shrinking waits for the scan.
        setAxisRange(id, full ? lo - pad : qMin(a.lo, lo - pad),
                         full ? hi + pad : qMax(a.hi, hi + pad));
    }
}

static std::pair<double,double> navRange(double min,double max,double minSpan,double lo,double hi){double full=qMax(0.,max-min),span=qBound(qMin(minSpan,full),hi-lo,full);if(span<=0)return{min,max};double lower=qBound(min,lo-(span-(hi-lo))*.5,max-span);return{lower,lower+span};}
void ChartView::setXNavigation(int id,bool on,double min,double max,double span){d_->navAxis=id;d_->nav=on;d_->navMin=min;d_->navMax=qMax(min+.001,max);d_->navSpan=span;if(!on)resetX();}
void ChartView::setLinkedXAxes(const QVector<int>&ids){d_->linkedXAxes=ids;}
void ChartView::zoomX(double factor){if(!d_->nav||d_->navAxis<0||d_->navAxis>=d_->axes.size())return;const Axis&a=d_->axes[d_->navAxis];double c=(a.lo+a.hi)*.5;auto r=navRange(d_->navMin,d_->navMax,d_->navSpan,c+(a.lo-c)*factor,c+(a.hi-c)*factor);setXRange(d_->navAxis,r.first,r.second);requestReplot();}
void ChartView::panX(double f){if(!d_->nav||d_->navAxis<0||d_->navAxis>=d_->axes.size())return;const Axis&a=d_->axes[d_->navAxis];double dx=(a.hi-a.lo)*f;auto r=navRange(d_->navMin,d_->navMax,d_->navSpan,a.lo+dx,a.hi+dx);setXRange(d_->navAxis,r.first,r.second);requestReplot();}
void ChartView::resetX(){if(d_->navAxis>=0&&d_->navAxis<d_->axes.size()){setXRange(d_->navAxis,d_->navMin,d_->navMax);requestReplot();}}

void ChartView::updateHover(const QPoint& position) {
    int pid = -1;
    for (int i = 0; i < d_->panels.size(); ++i)
        if (d_->panels[i].visible && d_->panels[i].plot.contains(position)) { pid = i; break; }
    for (Panel& panel : d_->panels) panel.cursorV = panel.cursorH = false;
    if (pid < 0) { for (auto* chart : liveCharts()) chart->clearSyncedCursor(); return; }
    Panel& panel = d_->panels[pid];
    int xid = -1;
    for (int i = 0; i < d_->axes.size(); ++i)
        if (d_->axes[i].panel == pid && d_->axes[i].side == Side::Bottom) { xid = i; break; }
    if (xid < 0) return;
    const Axis& axis = d_->axes[xid];
    const double key = axis.lo + double(position.x() - panel.plot.left()) / panel.plot.width() * (axis.hi - axis.lo);
    const double yRatio = qBound(0., double(position.y() - panel.plot.top()) / panel.plot.height(), 1.);
    double sampled = key;
    bool covered = false;
    QString rows;
    for (const Series& series : d_->series) {
        if (series.panel != pid || !series.visible || series.spec.name.isEmpty() || series.empty()) continue;
        const qsizetype at = nearest(series, key);
        const Point& point = series.data[size_t(at)];
        if (!std::isfinite(point.y)) continue;
        if (!covered) { sampled = point.x; covered = true; }
        QString value = numberText(point.y, 'f', series.spec.tipPrecision, series.spec.tipGroupThousands);
        if (!series.spec.unit.isEmpty()) value += (series.spec.unit == "%" ? "" : " ") + series.spec.unit;
        rows += QString("<div style='color:%1'><b>%2:</b> %3</div>")
            .arg(series.spec.color.name(), series.spec.name.toHtmlEscaped(), value.toHtmlEscaped());
    }
    if (!covered) {
        for (auto* chart : liveCharts()) chart->clearSyncedCursor();
        d_->hoverActive = true; // A later model update may supply a value here.
        return;
    }
    panel.cursorX = key; panel.cursorV = true;
    QString html = QString("<div style='color:%1'>%2</div>")
        .arg(palette().color(QPalette::ToolTipText).name(),
             axis.distance ? QString("%1 m").arg(qRound(sampled)) : timeText(sampled)) + rows;
    if (d_->sync) {
        const double time = interpolate(axis.sessionKeys, axis.sessionTimes, sampled);
        for (auto* chart : liveCharts()) if (chart->isVisible())
            html += chart->showSyncedCursor(time, key, axis.distance, yRatio, this, pid);
    }
    if (d_->tooltip->text() != html) { d_->tooltip->setText(html); d_->tooltip->adjustSize(); }
    QPoint pos = position + QPoint(14, 14);
    if (pos.x() + d_->tooltip->width() > width()) pos.setX(position.x() - 14 - d_->tooltip->width());
    if (pos.y() + d_->tooltip->height() > height()) pos.setY(position.y() - 14 - d_->tooltip->height());
    pos.setX(qMax(0, pos.x())); pos.setY(qMax(0, pos.y()));
    d_->tooltip->move(pos); d_->tooltip->show(); d_->tooltip->raise(); d_->overlay->update();
}

bool ChartView::eventFilter(QObject*w,QEvent*e){
    if(w!=d_->overlay)return QWidget::eventFilter(w,e);if(e->type()==QEvent::Leave)for(auto*c:liveCharts())c->clearSyncedCursor();
    if(e->type()==QEvent::ContextMenu)return true;
    if(e->type()==QEvent::MouseButtonDblClick){auto*m=static_cast<QMouseEvent*>(e);if(m->button()==Qt::RightButton){for(int pid=0;pid<d_->panels.size();++pid){const Panel&p=d_->panels[pid];if(!p.visible||!p.plot.contains(m->pos()))continue;for(int axisId=0;axisId<d_->axes.size();++axisId){const Axis&a=d_->axes[axisId];if(a.panel!=pid||a.side!=Side::Bottom)continue;const double x=a.lo+double(m->pos().x()-p.plot.left())/qMax(1,p.plot.width())*(a.hi-a.lo);emit inspectionRequested(x,a.distance);return true;}}}}
    if(e->type()==QEvent::MouseButtonRelease){auto*m=static_cast<QMouseEvent*>(e);if(!d_->dragging&&m->button()==Qt::LeftButton)for(Series&s:d_->series)if(s.legendHit.contains(m->pos())){s.visible=!s.visible;if(s.spec.yAxisId>=0&&s.spec.yAxisId<d_->axes.size())d_->axes[s.spec.yAxisId].fitTimer.invalidate();if(s.linked>=0&&s.linked<d_->series.size())d_->series[s.linked].visible=s.visible;requestReplot();return true;}d_->dragging=false;}
    if(e->type()==QEvent::MouseMove){auto*m=static_cast<QMouseEvent*>(e);if(d_->dragging&&d_->navAxis>=0&&d_->navAxis<d_->axes.size()){double dx=-double(m->pos().x()-d_->dragStart.x())*(d_->dragMax-d_->dragMin)/qMax(1,d_->dragWidth);auto r=navRange(d_->navMin,d_->navMax,d_->navSpan,d_->dragMin+dx,d_->dragMax+dx);setXRange(d_->navAxis,r.first,r.second);d_->dragStart=m->pos();d_->dragMin=r.first;d_->dragMax=r.second;requestReplot();return true;}
        if (d_->hover && d_->tooltip) {
            d_->hoverPosition = m->pos(); d_->hoverActive = true;
            if (!d_->hoverTimer->isActive()) d_->hoverTimer->start();
        }
    }
    if (d_->nav && d_->navAxis >= 0 && d_->navAxis < d_->axes.size()) {
        QPointF position;
        if (e->type() == QEvent::Wheel) position = static_cast<QWheelEvent*>(e)->position();
        else if (e->type() == QEvent::MouseButtonDblClick || e->type() == QEvent::MouseButtonPress)
            position = static_cast<QMouseEvent*>(e)->position();
        else return QWidget::eventFilter(w, e);
        const Panel* panel = nullptr;
        for (int pid = 0; pid < d_->panels.size(); ++pid) {
            const Panel& p = d_->panels[pid];
            if (!p.visible || !p.plot.contains(position.toPoint())) continue;
            for (int axis = 0; axis < d_->axes.size(); ++axis)
                if (d_->axes[axis].panel == pid && (axis == d_->navAxis || d_->linkedXAxes.contains(axis))) panel = &p;
        }
        if (!panel) return QWidget::eventFilter(w, e);
        const Axis& a = d_->axes[d_->navAxis];
        if (e->type() == QEvent::Wheel) {
            auto* wheel = static_cast<QWheelEvent*>(e);
            const QPoint pixel = wheel->pixelDelta();
            const QPointF delta = pixel.isNull() ? QPointF(wheel->angleDelta()) * .25 : QPointF(pixel);
            double amount = -(wheel->modifiers() & Qt::AltModifier ? delta.x() : delta.x() + delta.y());
            if (wheel->modifiers() & Qt::ShiftModifier) amount *= 5;
            double lo, hi;
            if (wheel->modifiers() & (Qt::ControlModifier | Qt::MetaModifier)) {
                const double origin = a.lo + (position.x() - panel->plot.left()) / panel->plot.width() * (a.hi - a.lo);
                const double zoom = qBound(-.5, amount * .002, .5);
                lo = a.lo + (a.lo - origin) * zoom; hi = a.hi + (a.hi - origin) * zoom;
            } else {
                const double shift = qBound(-.4, amount / panel->plot.width(), .4) * (a.hi - a.lo);
                lo = a.lo + shift; hi = a.hi + shift;
            }
            const auto range = navRange(d_->navMin, d_->navMax, d_->navSpan, lo, hi);
            setXRange(d_->navAxis, range.first, range.second); requestReplot(); return true;
        }
        auto* mouse = static_cast<QMouseEvent*>(e);
        if (mouse->button() == Qt::LeftButton) {
            if (e->type() == QEvent::MouseButtonDblClick) resetX();
            else {
                d_->dragging = true; d_->dragStart = mouse->pos();
                d_->dragMin = a.lo; d_->dragMax = a.hi; d_->dragWidth = panel->plot.width();
                for (auto* chart : liveCharts()) chart->clearSyncedCursor();
            }
            return true;
        }
    }
    return QWidget::eventFilter(w, e);
}

void ChartView::requestReplot() {
    if (!isVisible() || !d_->canvas || !d_->overlay) return;
    d_->canvas->update(); d_->overlay->update();
    if (d_->hoverActive && !d_->dragging && d_->hoverTimer && !d_->hoverTimer->isActive()) d_->hoverTimer->start();
}
bool ChartView::event(QEvent* event) {
    const bool handled = QWidget::event(event);
    if (d_ && d_->overlay && d_->canvas &&
        (event->type() == QEvent::DevicePixelRatioChange || event->type() == QEvent::ScreenChangeInternal)) {
        d_->geometry(rect());
        positionPanelChartSettings();
        requestReplot();
    }
    return handled;
}
void ChartView::applyPaletteText(){if(d_->tooltip){QColor bg=palette().color(QPalette::Button),fg=palette().color(QPalette::ToolTipText),border=fg;border.setAlpha(90);d_->tooltip->setStyleSheet(QString("background:rgba(%1,%2,%3,%4);color:%5;border:1px solid %6;padding:5px 8px;").arg(bg.red()).arg(bg.green()).arg(bg.blue()).arg(bg.alpha()).arg(fg.name(),border.name()));}}
void ChartView::resizeEvent(QResizeEvent*e){QWidget::resizeEvent(e);d_->overlay->setGeometry(rect());d_->geometry(rect());d_->overlay->raise();positionPanelChartSettings();}
void ChartView::changeEvent(QEvent* e) {
    QWidget::changeEvent(e);
    if (e->type() == QEvent::FontChange || e->type() == QEvent::ApplicationFontChange ||
        e->type() == QEvent::LocaleChange) {
        if (d_->tooltip) {
            QFont tooltipFont = font(); tooltipFont.setFeature(QFont::Tag("tnum"), 1);
            d_->tooltip->setFont(tooltipFont);
        }
        d_->geometry(rect()); positionPanelChartSettings(); requestReplot();
    }
    if (e->type() == QEvent::PaletteChange || e->type() == QEvent::ApplicationPaletteChange) {
        applyPaletteText(); requestReplot();
    }
}
