#include "ChartView.h"
#include "../ChartGraphicsBackend.h"
#include "../SessionModel.h"

#include <QComboBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFontMetrics>
#include <QFrame>
#include <QLabel>
#include <QLocale>
#include <QMatrix4x4>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QResizeEvent>
#include <QSettings>
#include <QStandardItemModel>
#include <QStyleOptionComboBox>
#include <QStylePainter>
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

struct Point { float x, y; };
static_assert(sizeof(Point) == sizeof(float) * 2);

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
    QString suffix;
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
    qsizetype first = 0, dirty = 0;
    quint64 revision = 1;
    QRect legendHit;
    qsizetype size() const { return qsizetype(data.size()) - first; }
    bool empty() const { return size() <= 0; }
};

struct Band { ChartView::BandSpec spec; int panel = 0; };

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
    auto it = std::lower_bound(begin, s.data.end(), float(x),
        [](const Point& p, float key) { return p.x < key; });
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
    s.first = s.dirty = 0; ++s.revision;
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

QString timeText(double sec) {
    int t = qMax(0, int(std::llround(sec * 10))), whole = t / 10;
    return QString("%1:%2.%3").arg(whole / 60).arg(whole % 60, 2, 10, QChar('0')).arg(t % 10);
}

QString tickText(const Axis& a, double value) {
    if (a.time) return timeText(value);
    return QLocale().toString(value / (a.scale == 0 ? 1 : a.scale), a.format, a.precision) + a.suffix;
}

QString mappedTickText(const Axis& a, double value) {
    const int mapped = a.ticks.indexOf(value);
    return mapped >= 0 && mapped < a.labels.size() ? a.labels[mapped] : tickText(a, value);
}

QRect containedHorizontally(QRect rect, const QRect& bounds) {
    if (rect.width() > bounds.width()) {
        rect.setLeft(bounds.left());
        rect.setWidth(bounds.width());
    } else if (rect.left() < bounds.left()) {
        rect.moveLeft(bounds.left());
    } else if (rect.right() > bounds.right()) {
        rect.moveRight(bounds.right());
    }
    return rect;
}

QVector<double> ticksFor(const Axis& a, int targetDivisions = 5) {
    if (!a.ticks.isEmpty()) {
        QVector<double> out;
        for (double x : a.ticks) if (x >= a.lo && x <= a.hi) out.push_back(x);
        return out;
    }
    double step = a.step > 0 ? a.step : niceStep(a.hi - a.lo, targetDivisions);
    QVector<double> out;
    for (double x = std::ceil(a.lo / step) * step; x <= a.hi + step * 1e-6 && out.size() < 64; x += step)
        out.push_back(x);
    return out;
}

QVector<double> yTicksWithUpperBound(const Axis& axis, int plotHeight = 0,
                                     int minimumPixelSpacing = 0) {
    QVector<double> ticks = ticksFor(axis);
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

int measuredYAxisLabelWidth(const Axis& axis, const QFontMetrics& metrics) {
    int width = 1;
    for (double tick : yTicksWithUpperBound(axis))
        width = qMax(width, metrics.horizontalAdvance(mappedTickText(axis, tick)));
    return width;
}

QShader shader(const char* path) {
    QFile f(QString::fromLatin1(path));
    return f.open(QIODevice::ReadOnly) ? QShader::fromSerialized(f.readAll()) : QShader();
}

struct alignas(16) Uniform { float mvp[16]; float color[4]; };

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
        if (!device || !renderTarget() || !linePipe || !fillPipe) return;
        if (gpu.size() < size_t(series->size())) gpu.resize(size_t(series->size()));
        if (gpuBands.size() < size_t(bands->size())) gpuBands.resize(size_t(bands->size()));
        QRhiResourceUpdateBatch* up = device->nextResourceUpdateBatch();

        struct Draw { QRhiBuffer* vb; QRhiShaderResourceBindings* srb; quint32 first, count; int panel; };
        QVector<Draw> fillDraws, lineDraws, bandDraws;
        for (int i = 0; i < bands->size(); ++i) {
            const Band& b = (*bands)[i];
            if (b.spec.axisId < 0 || b.spec.axisId >= axes->size()) continue;
            int panel = (*axes)[b.spec.axisId].panel;
            if (panel < 0 || panel >= panels->size() || !(*panels)[panel].visible) continue;
            ensureBand(i, up);
            Gpu& g = gpuBands[size_t(i)];
            updateUniform(g, 0, b.spec.axisId, b.spec.color, up, true);
            bandDraws.push_back({g.line.get(), g.lineSrb.get(), 0, 4, panel});
        }
        for (int id : *drawOrder) {
            if (id < 0 || id >= series->size()) continue;
            Series& s = (*series)[id];
            if (!s.visible || s.empty() || s.panel < 0 || s.panel >= panels->size() ||
                !(*panels)[s.panel].visible || s.spec.xAxisId < 0 || s.spec.xAxisId >= axes->size() ||
                s.spec.yAxisId < 0 || s.spec.yAxisId >= axes->size()) continue;
            upload(id, up);
            Gpu& g = gpu[size_t(id)];
            const Axis& x = (*axes)[s.spec.xAxisId];
            qsizetype begin = lowerBound(s, x.lo); if (begin > s.first) --begin;
            qsizetype end = lowerBound(s, x.hi); if (end < qsizetype(s.data.size())) ++end;
            begin = qBound(s.first, begin, qsizetype(s.data.size()));
            end = qBound(begin, end, qsizetype(s.data.size()));
            if (end <= begin) continue;
            updateUniform(g, s.spec.xAxisId, s.spec.yAxisId, s.spec.color, up, false);
            if (s.spec.fill && g.fill) {
                QColor c = s.spec.fillColor.isValid() ? s.spec.fillColor : s.spec.color;
                if (c.alpha() == 255) c.setAlpha(40);
                updateFillUniform(g, s.spec.xAxisId, s.spec.yAxisId, c, up);
                fillDraws.push_back({g.fill.get(), g.fillSrb.get(), quint32(begin * 2),
                                     quint32((end - begin) * 2), s.panel});
            }
            qsizetype first = s.spec.step ? (begin ? begin * 2 - 1 : 0) : begin;
            qsizetype last = s.spec.step ? end * 2 - 1 : end;
            lineDraws.push_back({g.line.get(), g.lineSrb.get(), quint32(first), quint32(last - first), s.panel});
        }

        cb->beginPass(renderTarget(), palette().color(QPalette::Window), {1, 0}, up);
        const QSize target = renderTarget()->pixelSize();
        double sx = double(target.width()) / qMax(1, width()), sy = double(target.height()) / qMax(1, height());
        auto viewport = [&](int id) {
            QRect r = (*panels)[id].plot;
            const int left = qRound(r.x() * sx);
            const int right = qRound((r.x() + r.width()) * sx);
            const int top = qRound(r.y() * sy);
            const int bottom = qRound((r.y() + r.height()) * sy);
            const int x = left;
            // QWidget geometry is top-left based, while QRhi viewports and
            // scissors are always OpenGL-style bottom-left based.  Passing the
            // QWidget y coordinate through directly vertically swapped every
            // row in a multi-panel chart (and made zero-valued samples appear
            // against a different panel's axis).
            const int y = target.height() - bottom;
            const int w = qMax(1, right - left);
            const int h = qMax(1, bottom - top);
            cb->setViewport(QRhiViewport(float(x), float(y), float(w), float(h)));
            cb->setScissor(QRhiScissor(x, y, w, h));
        };
        auto draw = [&](const QVector<Draw>& list, QRhiGraphicsPipeline* pipe) {
            cb->setGraphicsPipeline(pipe);
            for (const Draw& d : list) {
                viewport(d.panel); cb->setShaderResources(d.srb);
                QRhiCommandBuffer::VertexInput input(d.vb, 0);
                cb->setVertexInput(0, 1, &input); cb->draw(d.count, 1, d.first);
            }
        };
        draw(bandDraws, fillPipe.get()); draw(fillDraws, fillPipe.get()); draw(lineDraws, linePipe.get());
        cb->endPass();
    }

    void releaseResources() override {
        linePipe.reset(); fillPipe.reset(); templateSrb.reset(); templateUbo.reset();
        gpu.clear(); gpuBands.clear(); device = nullptr;
    }

private:
    struct Gpu {
        std::unique_ptr<QRhiBuffer> line, fill, lineUbo, fillUbo;
        std::unique_ptr<QRhiShaderResourceBindings> lineSrb, fillSrb;
        qsizetype cap = 0; quint64 uploaded = 0;
    };

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
        QShader vs = shader(":/shaders/chart.vert.qsb"), fs = shader(":/shaders/chart.frag.qsb");
        if (!vs.isValid() || !fs.isValid()) { qCritical("[charts] QRhi shaders are missing"); return; }
        auto make = [&](QRhiGraphicsPipeline::Topology topology) {
            std::unique_ptr<QRhiGraphicsPipeline> p(device->newGraphicsPipeline());
            p->setTopology(topology); p->setSampleCount(sampleCount());
            if (topology == QRhiGraphicsPipeline::LineStrip && device->isFeatureSupported(QRhi::WideLines))
                p->setLineWidth(2.0f);
            p->setFlags(QRhiGraphicsPipeline::UsesScissor);
            QRhiGraphicsPipeline::TargetBlend blend; blend.enable = true;
            blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
            blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
            p->setTargetBlends({blend});
            p->setShaderStages({{QRhiShaderStage::Vertex, vs}, {QRhiShaderStage::Fragment, fs}});
            QRhiVertexInputLayout layout; layout.setBindings({{sizeof(Point)}});
            layout.setAttributes({{0, 0, QRhiVertexInputAttribute::Float2, 0}});
            p->setVertexInputLayout(layout); p->setShaderResourceBindings(templateSrb.get());
            p->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
            return p->create() ? std::move(p) : std::unique_ptr<QRhiGraphicsPipeline>();
        };
        linePipe = make(QRhiGraphicsPipeline::LineStrip);
        fillPipe = make(QRhiGraphicsPipeline::TriangleStrip);
    }

    static qsizetype capacity(qsizetype needed) { qsizetype n = 1024; while (n < needed) n *= 2; return n; }

    void ensureGpu(Gpu& g, qsizetype points, bool wantFill) {
        bool rebuild = !g.line || g.cap < points;
        if (rebuild) {
            g.cap = capacity(qMax<qsizetype>(1, points));
            g.line.reset(device->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, int(g.cap * sizeof(Point))));
            g.line->create(); g.uploaded = 0;
        }
        if (!g.lineSrb) g.lineSrb = makeSrb(g.lineUbo);
        if (wantFill && (!g.fill || rebuild)) {
            g.fill.reset(device->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, int(g.cap * 2 * sizeof(Point))));
            g.fill->create(); g.uploaded = 0;
        }
        if (wantFill && !g.fillSrb) g.fillSrb = makeSrb(g.fillUbo);
    }

    void upload(int id, QRhiResourceUpdateBatch* up) {
        Series& s = (*series)[id]; Gpu& g = gpu[size_t(id)];
        qsizetype count = qsizetype(s.data.size());
        qsizetype vertices = s.spec.step ? qMax<qsizetype>(1, count * 2 - 1) : qMax<qsizetype>(1, count);
        qsizetype oldCap = g.cap; ensureGpu(g, vertices, s.spec.fill);
        if (g.uploaded == s.revision) return;
        qsizetype dirty = oldCap != g.cap ? 0 : qBound<qsizetype>(0, s.dirty, count);
        if (s.spec.step) {
            qsizetype firstVertex = dirty ? dirty * 2 - 1 : 0;
            QByteArray bytes; bytes.reserve(int((count * 2 - 1 - firstVertex) * sizeof(Point)));
            for (qsizetype i = dirty; i < count; ++i) {
                Point cur = s.data[size_t(i)];
                if (!i) bytes.append(reinterpret_cast<const char*>(&cur), sizeof(cur));
                else {
                    Point corner{cur.x, s.data[size_t(i - 1)].y};
                    bytes.append(reinterpret_cast<const char*>(&corner), sizeof(corner));
                    bytes.append(reinterpret_cast<const char*>(&cur), sizeof(cur));
                }
            }
            if (!bytes.isEmpty()) up->updateDynamicBuffer(g.line.get(), quint32(firstVertex * sizeof(Point)), bytes);
        } else if (dirty < count) {
            up->updateDynamicBuffer(g.line.get(), quint32(dirty * sizeof(Point)),
                quint32((count - dirty) * sizeof(Point)), s.data.data() + dirty);
        }
        if (s.spec.fill && dirty < count) {
            QByteArray bytes; bytes.reserve(int((count - dirty) * 2 * sizeof(Point)));
            for (qsizetype i = dirty; i < count; ++i) {
                Point zero{s.data[size_t(i)].x, 0};
                bytes.append(reinterpret_cast<const char*>(&zero), sizeof(zero));
                bytes.append(reinterpret_cast<const char*>(&s.data[size_t(i)]), sizeof(Point));
            }
            up->updateDynamicBuffer(g.fill.get(), quint32(dirty * 2 * sizeof(Point)), bytes);
        }
        s.dirty = count; g.uploaded = s.revision;
    }

    Uniform uniform(int xAxis, int yAxis, QColor color, bool band = false) {
        Uniform u{}; QMatrix4x4 m = device->clipSpaceCorrMatrix(), ortho;
        if (band) {
            const Axis& y = (*axes)[yAxis];
            ortho.ortho(0.f, 1.f, float(y.lo), float(y.hi), -1.f, 1.f);
        }
        else {
            const Axis& x = (*axes)[xAxis]; const Axis& y = (*axes)[yAxis];
            ortho.ortho(float(x.lo), float(x.hi), float(y.lo), float(y.hi), -1.f, 1.f);
        }
        m *= ortho; std::memcpy(u.mvp, m.constData(), sizeof(u.mvp));
        u.color[0] = color.redF(); u.color[1] = color.greenF();
        u.color[2] = color.blueF(); u.color[3] = color.alphaF(); return u;
    }

    void updateUniform(Gpu& g, int x, int y, QColor c, QRhiResourceUpdateBatch* up,
                       bool band) {
        Uniform u = uniform(x, y, c, band);
        up->updateDynamicBuffer(g.lineUbo.get(), 0, sizeof(u), &u);
    }
    void updateFillUniform(Gpu& g, int x, int y, QColor c, QRhiResourceUpdateBatch* up) {
        Uniform u = uniform(x, y, c); up->updateDynamicBuffer(g.fillUbo.get(), 0, sizeof(u), &u);
    }

    void ensureBand(int id, QRhiResourceUpdateBatch* up) {
        Gpu& g = gpuBands[size_t(id)]; ensureGpu(g, 4, false);
        if (g.uploaded) return;
        const Band& b = (*bands)[id];
        Point v[] = {{0, float(b.spec.min)}, {1, float(b.spec.min)},
                     {0, float(b.spec.max)}, {1, float(b.spec.max)}};
        up->updateDynamicBuffer(g.line.get(), 0, sizeof(v), v); g.uploaded = 1;
    }

    QVector<Axis>* axes; QVector<Series>* series; QVector<Band>* bands;
    QVector<Panel>* panels; QVector<int>* drawOrder;
    QRhi* device = nullptr;
    std::vector<Gpu> gpu, gpuBands;
    std::unique_ptr<QRhiBuffer> templateUbo;
    std::unique_ptr<QRhiShaderResourceBindings> templateSrb;
    std::unique_ptr<QRhiGraphicsPipeline> linePipe, fillPipe;
};

class Overlay final : public QWidget {
public:
    Overlay(QVector<Axis>* a, QVector<Series>* s, QVector<Panel>* p, QWidget* parent)
        : QWidget(parent), axes(a), series(s), panels(p) {
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
        QPainter q(this); q.setRenderHint(QPainter::TextAntialiasing);
        QColor text = palette().color(QPalette::Text), muted = palette().color(QPalette::PlaceholderText);
        QFont smallFont = font(); smallFont.setPointSize(8); QFont boldFont = smallFont; boldFont.setBold(true);
        const QFontMetrics smallMetrics(smallFont);
        for (Series& s : *series) s.legendHit = {};
        for (int pid = 0; pid < panels->size(); ++pid) {
            Panel& p = (*panels)[pid]; if (!p.visible || p.plot.isEmpty()) continue;
            q.save();
            q.setClipRect(p.outer);
            if (p.header) { q.setFont(boldFont); q.setPen(muted); q.drawText(p.outer.adjusted(kSidePad, 0, 0, -p.outer.height() + kHeader), Qt::AlignLeft | Qt::AlignVCenter, p.title); }
            for (int aid = 0; aid < axes->size(); ++aid) {
                const Axis& a = (*axes)[aid]; if (!a.visible || a.panel != pid || a.hi <= a.lo) continue;
                const int tickCapacity = a.side == ChartView::Side::Bottom
                    ? qMax(2, p.plot.width() / qMax(1, a.tickSpacePx)) : 5;
                QColor axisText = a.inherit ? text : a.color;
                QVector<double> tv = a.side == ChartView::Side::Bottom
                    ? ticksFor(a, tickCapacity)
                    : yTicksWithUpperBound(a, p.plot.height(), smallMetrics.height() + 2);
                const int labelEvery = a.side == ChartView::Side::Bottom && a.lapBoundaryLabels
                    ? qMax(1, int(std::ceil(double(tv.size()) / tickCapacity))) : 1;
                q.setFont(smallFont);
                if (a.side == ChartView::Side::Bottom) {
                    q.setPen(kAxis); q.drawLine(p.plot.bottomLeft(), p.plot.bottomRight());
                    for (int tickIndex = 0; tickIndex < tv.size(); ++tickIndex) {
                        const double t = tv[tickIndex];
                        int x = p.plot.left() + qRound((t - a.lo) / (a.hi - a.lo) * p.plot.width());
                        if (a.grid) { q.setPen(kGrid); q.drawLine(x, p.plot.top(), x, p.plot.bottom()); }
                        q.setPen(kAxis); q.drawLine(x, p.plot.bottom(), x, p.plot.bottom() + 3);
                        if (tickIndex != 0 && tickIndex != tv.size() - 1 && tickIndex % labelEvery != 0) continue;
                        q.setPen(axisText);
                        const QString label = mappedTickText(a, t);
                        const int labelWidth = smallMetrics.horizontalAdvance(label) + 4;
                        QRect labelRect = a.lapBoundaryLabels
                            ? QRect(x + 4, p.plot.bottom() + 4, labelWidth, 18)
                            : QRect(x - labelWidth / 2, p.plot.bottom() + 4, labelWidth, 18);
                        labelRect = containedHorizontally(labelRect, p.outer.adjusted(2, 0, -2, 0));
                        q.drawText(labelRect,
                            (a.lapBoundaryLabels ? Qt::AlignLeft : Qt::AlignHCenter) | Qt::AlignTop,
                            label);
                    }
                } else {
                    bool left = a.side == ChartView::Side::Left;
                    int ax = left ? p.plot.left() - a.laneOffset : p.plot.right() + a.laneOffset;
                    q.setPen(kAxis); q.drawLine(ax, p.plot.top(), ax, p.plot.bottom());
                    for (double t : tv) {
                        int y = p.plot.bottom() - qRound((t - a.lo) / (a.hi - a.lo) * p.plot.height());
                        if (a.grid) { q.setPen(kGrid); q.drawLine(p.plot.left(), y, p.plot.right(), y); }
                        q.setPen(kAxis); q.drawLine(ax + (left ? -3 : 0), y, ax + (left ? 0 : 3), y); q.setPen(axisText);
                        QRect r = left
                            ? QRect(ax - kAxisTextGap - a.labelWidth, y - 9, a.labelWidth, 18)
                            : QRect(ax + kAxisTextGap, y - 9, a.labelWidth, 18);
                        r = containedHorizontally(r, p.outer.adjusted(2, 0, -2, 0));
                        q.drawText(r, (left ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter, tickText(a, t));
                    }
                }
            }
            if (p.legend) {
                QFontMetrics fm(smallFont); q.setFont(smallFont); QVector<int> ids; int total = 0;
                for (int i = 0; i < series->size(); ++i) if ((*series)[i].panel == pid && !(*series)[i].spec.name.isEmpty()) {
                    ids.push_back(i); total += 24 + fm.horizontalAdvance((*series)[i].spec.name);
                }
                int x = p.header ? p.outer.right() - kSidePad - total : p.plot.center().x() - total / 2;
                int y = p.header ? p.outer.top() + (kHeader - fm.height()) / 2 : p.plot.top() + 4;
                for (int id : ids) {
                    Series& s = (*series)[id]; int w = 24 + fm.horizontalAdvance(s.spec.name);
                    q.setPen(QPen(s.spec.color, 2)); q.drawLine(x, y + fm.height() / 2, x + 12, y + fm.height() / 2);
                    q.setPen(s.visible ? text : muted); q.drawText(x + 16, y, w - 16, fm.height(), Qt::AlignVCenter, s.spec.name);
                    s.legendHit = QRect(x - 2, y - 3, w, fm.height() + 6); x += w;
                }
            }
            int xAxis = -1; for (int i = 0; i < axes->size(); ++i) if ((*axes)[i].panel == pid && (*axes)[i].side == ChartView::Side::Bottom) { xAxis = i; break; }
            if (p.cursorV && xAxis >= 0) {
                const Axis& a = (*axes)[xAxis]; int x = p.plot.left() + qRound((p.cursorX - a.lo) / (a.hi - a.lo) * p.plot.width());
                q.setPen(QColor(150,150,150,160)); q.drawLine(x, p.plot.top(), x, p.plot.bottom());
            }
            if (p.cursorH) { int y = p.plot.top() + qRound(p.cursorY * p.plot.height()); q.setPen(QColor(150,150,150,120)); q.drawLine(p.plot.left(), y, p.plot.right(), y); }
            q.restore();
        }
    }
private:
    QVector<Axis>* axes; QVector<Series>* series; QVector<Panel>* panels;
    QVector<QFrame*> dividerFrames;
};
} // namespace

struct ChartView::Impl {
    RhiCanvas* canvas = nullptr; Overlay* overlay = nullptr; QLabel* tooltip = nullptr;
    QVector<Axis> axes; QVector<Series> series; QVector<Band> bands; QVector<Panel> panels{Panel{}};
    QVector<int> order; QVector<QVector<int>> rows; int columns = 1;
    bool explicitRows = false, hover = false, sync = false, secondaryV = true, secondaryH = false;
    QString cursorMode; QPointer<SessionModel> model;
    int navAxis = -1; bool nav = false, dragging = false;
    double navMin = 0, navMax = 1, navSpan = .5, dragMin = 0, dragMax = 1; QPoint dragStart;

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
        QFont axisFont = overlay ? overlay->font() : QFont();
        axisFont.setPointSize(8);
        const QFontMetrics axisMetrics(axisFont);
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
                        const int firstWidth = axisMetrics.horizontalAdvance(mappedTickText(axis, ticks.first()));
                        const int lastWidth = axisMetrics.horizontalAdvance(mappedTickText(axis, ticks.last()));
                        if (axis.lapBoundaryLabels) {
                            rightInset = qMax(rightInset, lastWidth + kPlotEdgePad);
                        } else {
                            leftInset = qMax(leftInset, firstWidth / 2 + kPlotEdgePad);
                            rightInset = qMax(rightInset, lastWidth / 2 + kPlotEdgePad);
                        }
                    }
                    const int topInset = p.header
                        ? kHeader + kPlotEdgePad
                        : qMax(kPlotEdgePad, (axisMetrics.height() + 1) / 2);
                    p.plot = p.outer.adjusted(leftInset, topInset,
                                              -rightInset, -(bottomAxes.isEmpty() ? 4 : 26));
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
    layout->addWidget(d_->canvas); d_->overlay = new Overlay(&d_->axes, &d_->series, &d_->panels, this);
    d_->overlay->installEventFilter(this); d_->overlay->raise(); liveCharts().push_back(this);
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
    Series v; v.spec=s; if(s.yAxisId>=0&&s.yAxisId<d_->axes.size())v.panel=d_->axes[s.yAxisId].panel;
    d_->series.push_back(std::move(v)); int id=d_->series.size()-1; d_->order.push_back(id); return id;
}
void ChartView::addBand(const BandSpec& s) { Band b; b.spec=s; if(s.axisId>=0&&s.axisId<d_->axes.size())b.panel=d_->axes[s.axisId].panel; d_->bands.push_back(b); }

void ChartView::appendPoint(int id,double x,double y) {
    if(id<0||id>=d_->series.size()||!std::isfinite(x)||!std::isfinite(y))return; Series& s=d_->series[id]; Point p{float(x),float(y)};
    if(!s.data.empty()&&p.x<s.data.back().x){auto at=std::lower_bound(s.data.begin()+s.first,s.data.end(),p.x,[](const Point&a,float k){return a.x<k;});qsizetype i=std::distance(s.data.begin(),at);s.data.insert(at,p);s.dirty=qMin(s.dirty,i);}
    else{s.dirty=qMin(s.dirty,qsizetype(s.data.size()));s.data.push_back(p);}while(s.size()>kMaxPoints)++s.first;compact(s);++s.revision;
}
void ChartView::setSeriesData(int id,const QVector<double>& xs,const QVector<double>& ys) {
    if(id<0||id>=d_->series.size()||xs.size()!=ys.size())return;Series&s=d_->series[id];qsizetype n=qMin<qsizetype>(kMaxPoints,xs.size()),from=xs.size()-n;
    if(s.size()==n&&n&&s.data[size_t(s.first)].x==float(xs[from])&&s.data[size_t(s.first)].y==float(ys[from])&&s.data.back().x==float(xs.last())&&s.data.back().y==float(ys.last()))return;
    s.data.clear();s.data.reserve(size_t(n));for(qsizetype i=from;i<xs.size();++i)if(std::isfinite(xs[i])&&std::isfinite(ys[i]))s.data.push_back({float(xs[i]),float(ys[i])});s.first=s.dirty=0;++s.revision;
}
void ChartView::trimBefore(int id,double x){if(id<0||id>=d_->series.size())return;Series&s=d_->series[id];s.first=lowerBound(s,x);compact(s);}
void ChartView::clear(int id){if(id<0||id>=d_->series.size())return;Series&s=d_->series[id];s.data.clear();s.first=s.dirty=0;++s.revision;}
void ChartView::clearAll(){for(int i=0;i<d_->series.size();++i)clear(i);}
void ChartView::setSeriesVisible(int id,bool on){if(id>=0&&id<d_->series.size())d_->series[id].visible=on;}
bool ChartView::seriesVisible(int id)const{return id>=0&&id<d_->series.size()&&d_->series[id].visible;}
void ChartView::setSeriesColor(int id,const QColor&c){if(id>=0&&id<d_->series.size())d_->series[id].spec.color=c;}
void ChartView::setSeriesName(int id,const QString&n){if(id>=0&&id<d_->series.size())d_->series[id].spec.name=n;}
void ChartView::setSeriesWidth(int id,double w){if(id>=0&&id<d_->series.size())d_->series[id].spec.width=w;}
void ChartView::setSeriesOrder(const QVector<int>& ids){QVector<int>o;for(int id:ids)if(id>=0&&id<d_->series.size()&&!o.contains(id))o.push_back(id);for(int id:d_->order)if(!o.contains(id))o.push_back(id);d_->order=o;}
void ChartView::linkSeriesVisibility(int a,int b){if(a>=0&&a<d_->series.size())d_->series[a].linked=b;}
void ChartView::setAxisVisible(int id,bool on){if(id>=0&&id<d_->axes.size()){d_->axes[id].visible=on;d_->geometry(rect());}}
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
void ChartView::setAxisTimeTicker(int id,const QString&){if(id>=0&&id<d_->axes.size()){auto&a=d_->axes[id];a.time=true;a.lapBoundaryLabels=false;a.ticks.clear();a.labels.clear();}}
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
        const ChartWindow values[]={ChartWindow::Seconds15,ChartWindow::Seconds30,ChartWindow::Seconds60,ChartWindow::Seconds120,ChartWindow::Seconds300,ChartWindow::Seconds600,ChartWindow::CurrentLap,ChartWindow::PreviousLap,ChartWindow::FastestLap,ChartWindow::SelectedLap,ChartWindow::StintLaps,ChartWindow::AllLaps};
        for(auto v:values)p.window->addItem(chartWindowLabel(v),chartWindowKey(v));connect(p.window,QOverload<int>::of(&QComboBox::activated),this,[this,id](int i){auto&p=d_->panels[id];if(d_->model)d_->model->setChartWindow(p.section,chartWindowFromKey(p.window->itemData(i).toString()));});
        p.lap=new InsetComboBox(this);p.lap->setFrame(false);p.lap->setFixedSize(kLapControlW,kControlH);p.lap->setToolTip("Selected reference lap for this chart");connect(p.lap,QOverload<int>::of(&QComboBox::activated),this,[this,id](int i){auto&p=d_->panels[id];if(d_->model)d_->model->setReferenceLap(p.section,p.lap->itemData(i).toInt());});}
    connect(model,&SessionModel::chartConfigurationChanged,this,&ChartView::refreshPanelChartSettings,Qt::UniqueConnection);connect(model,&SessionModel::lapsChanged,this,&ChartView::refreshPanelChartSettings,Qt::UniqueConnection);refreshPanelChartSettings();
}

void ChartView::refreshPanelChartSettings(){
    if(!d_->model)return;bool coords=d_->model->lapCoordinatesAvailable();for(Panel&p:d_->panels){if(!p.window||p.section==tnr::GraphSection::Count_)continue;
        p.window->blockSignals(true);int i=p.window->findData(chartWindowKey(d_->model->effectiveChartWindow(p.section)));p.window->setCurrentIndex(i>=0?i:0);
        if(auto*m=qobject_cast<QStandardItemModel*>(p.window->model()))for(int n=0;n<p.window->count();++n){auto w=chartWindowFromKey(p.window->itemData(n).toString());m->item(n)->setEnabled(!chartWindowIsDistance(w)||(coords&&w!=ChartWindow::SelectedLap)||(coords&&w==ChartWindow::SelectedLap&&d_->model->playbackMode()));}p.window->blockSignals(false);
        int wanted=d_->model->referenceLap(p.section);p.lap->blockSignals(true);p.lap->clear();for(const LapBlock&lap:d_->model->data().laps)if(!lap.progress.isEmpty()||d_->model->playbackCatalogHasLapDistance())p.lap->addItem(QString::number(lap.lapNum),lap.lapNum);i=p.lap->findData(wanted);p.lap->setCurrentIndex(i>=0?i:(p.lap->count()?0:-1));p.lap->blockSignals(false);}positionPanelChartSettings();
}

void ChartView::positionPanelChartSettings(){for(Panel&p:d_->panels){if(!p.window)continue;bool show=p.visible&&!p.outer.isEmpty();p.window->setVisible(show);bool lap=show&&d_->model&&d_->model->playbackMode()&&d_->model->effectiveChartWindow(p.section)==ChartWindow::SelectedLap;p.lap->setVisible(lap);if(!show)continue;QFont f=font();f.setPointSize(8);f.setBold(true);int title=QFontMetrics(f).horizontalAdvance(p.title);int x=p.outer.left()+kSidePad+title+kTitleControlGap,y=p.outer.top()+(kHeader-kControlH)/2;p.window->move(x,y);p.window->raise();if(lap){p.lap->move(x+p.window->width()+3,y);p.lap->raise();}}if(d_->tooltip)d_->tooltip->raise();}

void ChartView::setAxisLabelMap(int id,const QVector<double>&ticks,const QStringList&labels,bool lapBoundaryLabels){if(id>=0&&id<d_->axes.size()){auto&a=d_->axes[id];a.time=false;a.lapBoundaryLabels=lapBoundaryLabels;a.ticks=ticks;a.labels=labels;}}
void ChartView::setAxisNumberSuffix(int id,double scale,const QString&suffix,double step){if(id>=0&&id<d_->axes.size()){auto&a=d_->axes[id];a.time=false;a.scale=scale;a.suffix=suffix;a.step=step;if(a.side!=Side::Bottom)d_->geometry(rect());}}
void ChartView::setHoverReadout(bool on){d_->hover=on;if(on&&!d_->tooltip){d_->tooltip=new QLabel(this);d_->tooltip->setTextFormat(Qt::RichText);d_->tooltip->setAttribute(Qt::WA_TransparentForMouseEvents);d_->tooltip->hide();applyPaletteText();}if(!on)clearSyncedCursor();}
void ChartView::setCursorSync(bool on,bool v,bool h){bool clear=d_->sync&&(!on||d_->secondaryV!=v||d_->secondaryH!=h);d_->sync=on;d_->secondaryV=v;d_->secondaryH=h;if(clear)clearSyncedCursor();}
void ChartView::setCursorModeKey(const QString&key){if(d_->cursorMode==key)return;d_->cursorMode=key;for(auto*c:liveCharts())c->clearSyncedCursor();}

QString ChartView::showSyncedCursor(double time,double sourceX,bool sourceDistance,double yRatio,ChartView*source,int sourcePanel){
    if(!d_->sync||!d_->hover)return{};if(source!=this&&d_->tooltip)d_->tooltip->hide();QString html;QLocale loc;
    for(int pid=0;pid<d_->panels.size();++pid){Panel&p=d_->panels[pid];int xid=-1;for(int i=0;i<d_->axes.size();++i)if(d_->axes[i].panel==pid&&d_->axes[i].side==Side::Bottom){xid=i;break;}if(xid<0)continue;const Axis&a=d_->axes[xid];bool target=a.distance;double key=sourceDistance==target?sourceX:target?interpolate(a.sessionTimes,a.sessionKeys,time):time;bool mapped=sourceDistance==target||!target||(!a.sessionTimes.isEmpty()&&time>=a.sessionTimes.first()&&time<=a.sessionTimes.last());
        if(!p.visible||!mapped||key<a.lo||key>a.hi){if(!(source==this&&pid==sourcePanel))p.cursorV=false;p.cursorH=false;continue;}if(!(source==this&&pid==sourcePanel)){p.cursorX=key;p.cursorV=d_->secondaryV;}p.cursorY=yRatio;p.cursorH=d_->secondaryH&&!(source==this&&pid==sourcePanel);bool any=false;
        for(const Series&s:d_->series){if(s.panel!=pid||!s.visible||s.spec.name.isEmpty()||s.empty())continue;double lo=s.data[size_t(s.first)].x,hi=s.data.back().x;bool endpoint=sourceDistance==target&&key>hi;if(key<lo||(key>hi&&!endpoint))continue;qsizetype at=nearest(s,key);QString value=s.spec.tipGroupThousands?loc.toString(s.data[size_t(at)].y,'f',s.spec.tipPrecision):QString::number(s.data[size_t(at)].y,'f',s.spec.tipPrecision);if(!s.spec.unit.isEmpty())value+=(s.spec.unit=="%"?"":" ")+s.spec.unit;html+=QString("<div style='color:%1'><b>%2:</b> %3</div>").arg(s.spec.color.name(),s.spec.name,value);any=true;}if(!any&&!(source==this&&pid==sourcePanel)){p.cursorV=p.cursorH=false;}}
    requestReplot();return html;
}
void ChartView::clearSyncedCursor(){for(Panel&p:d_->panels)p.cursorV=p.cursorH=false;if(d_->tooltip)d_->tooltip->hide();if(d_->overlay)d_->overlay->update();}
bool ChartView::seriesKeyRange(int id,double&lo,double&hi)const{if(id<0||id>=d_->series.size()||d_->series[id].empty())return false;const Series&s=d_->series[id];lo=s.data[size_t(s.first)].x;hi=s.data.back().x;return true;}
void ChartView::setXRange(int id,double lo,double hi){
    if(id<0||id>=d_->axes.size()||hi<=lo)return;
    Axis&a=d_->axes[id];const int oldWidth=a.labelWidth;a.lo=lo;a.hi=hi;
    if(a.side!=Side::Bottom){QFont f=font();f.setPointSize(8);if(measuredYAxisLabelWidth(a,QFontMetrics(f))!=oldWidth)d_->geometry(rect());}
}
void ChartView::setAxisRange(int id,double lo,double hi){setXRange(id,lo,hi);}

void ChartView::fitAxisToVisibleSeries(int id,const QVector<int>&ids,double fixedLo,double fixedHi,bool dynamic,bool expand){
    if(id<0||id>=d_->axes.size())return;
    Axis&a=d_->axes[id];const int oldWidth=a.labelWidth;
    auto updateGutter=[&]{if(a.side==Side::Bottom)return;QFont f=font();f.setPointSize(8);if(measuredYAxisLabelWidth(a,QFontMetrics(f))!=oldWidth)d_->geometry(rect());};
    if(!dynamic&&!expand){const bool changed=a.lo!=fixedLo||a.hi!=fixedHi;a.lo=fixedLo;a.hi=fixedHi;if(changed)updateGutter();return;}
    if(a.fitTimer.isValid()&&a.fitTimer.elapsed()<200)return;
    a.fitTimer.restart();bool found=false;double lo=0,hi=0;
    for(int sid:ids){if(sid<0||sid>=d_->series.size())continue;const Series&s=d_->series[sid];if(!s.visible||s.empty()||s.spec.xAxisId<0||s.spec.xAxisId>=d_->axes.size())continue;const Axis&x=d_->axes[s.spec.xAxisId];qsizetype begin=lowerBound(s,x.lo),end=qMin(lowerBound(s,x.hi)+1,qsizetype(s.data.size()));for(qsizetype i=begin;i<end;++i){double v=s.data[size_t(i)].y;if(!found){lo=hi=v;found=true;}else{lo=qMin(lo,v);hi=qMax(hi,v);}}}
    if(!dynamic){a.lo=fixedLo;a.hi=expand&&found?qMax(fixedHi,hi):fixedHi;}
    else if(!found){a.lo=fixedLo;a.hi=fixedHi;}
    else{double span=qMax(1.,hi-lo);a.lo=lo-span*.08;a.hi=hi+span*.08;}
    updateGutter();
}

static std::pair<double,double> navRange(double min,double max,double minSpan,double lo,double hi){double full=qMax(0.,max-min),span=qBound(qMin(minSpan,full),hi-lo,full);if(span<=0)return{min,max};double lower=qBound(min,lo-(span-(hi-lo))*.5,max-span);return{lower,lower+span};}
void ChartView::setXNavigation(int id,bool on,double min,double max,double span){d_->navAxis=id;d_->nav=on;d_->navMin=min;d_->navMax=qMax(min+.001,max);d_->navSpan=span;if(!on)resetX();}
void ChartView::zoomX(double factor){if(!d_->nav||d_->navAxis<0||d_->navAxis>=d_->axes.size())return;const Axis&a=d_->axes[d_->navAxis];double c=(a.lo+a.hi)*.5;auto r=navRange(d_->navMin,d_->navMax,d_->navSpan,c+(a.lo-c)*factor,c+(a.hi-c)*factor);setXRange(d_->navAxis,r.first,r.second);requestReplot();}
void ChartView::panX(double f){if(!d_->nav||d_->navAxis<0||d_->navAxis>=d_->axes.size())return;const Axis&a=d_->axes[d_->navAxis];double dx=(a.hi-a.lo)*f;auto r=navRange(d_->navMin,d_->navMax,d_->navSpan,a.lo+dx,a.hi+dx);setXRange(d_->navAxis,r.first,r.second);requestReplot();}
void ChartView::resetX(){if(d_->navAxis>=0&&d_->navAxis<d_->axes.size()){setXRange(d_->navAxis,d_->navMin,d_->navMax);requestReplot();}}

bool ChartView::eventFilter(QObject*w,QEvent*e){
    if(w!=d_->overlay)return QWidget::eventFilter(w,e);if(e->type()==QEvent::Leave)for(auto*c:liveCharts())c->clearSyncedCursor();
    if(e->type()==QEvent::MouseButtonRelease){auto*m=static_cast<QMouseEvent*>(e);if(!d_->dragging&&m->button()==Qt::LeftButton)for(Series&s:d_->series)if(s.legendHit.contains(m->pos())){s.visible=!s.visible;if(s.linked>=0&&s.linked<d_->series.size())d_->series[s.linked].visible=s.visible;requestReplot();return true;}d_->dragging=false;}
    if(e->type()==QEvent::MouseMove){auto*m=static_cast<QMouseEvent*>(e);if(d_->dragging&&d_->navAxis>=0&&d_->navAxis<d_->axes.size()){double dx=-double(m->pos().x()-d_->dragStart.x())*(d_->dragMax-d_->dragMin)/qMax(1,width());auto r=navRange(d_->navMin,d_->navMax,d_->navSpan,d_->dragMin+dx,d_->dragMax+dx);setXRange(d_->navAxis,r.first,r.second);requestReplot();return true;}
        if(d_->hover&&d_->tooltip){int pid=-1;for(int i=0;i<d_->panels.size();++i)if(d_->panels[i].visible&&d_->panels[i].plot.contains(m->pos())){pid=i;break;}for(Panel&p:d_->panels)p.cursorV=false;if(pid<0){d_->tooltip->hide();d_->overlay->update();return false;}Panel&p=d_->panels[pid];int xid=-1;for(int i=0;i<d_->axes.size();++i)if(d_->axes[i].panel==pid&&d_->axes[i].side==Side::Bottom){xid=i;break;}if(xid<0)return false;const Axis&a=d_->axes[xid];double key=a.lo+double(m->pos().x()-p.plot.left())/qMax(1,p.plot.width())*(a.hi-a.lo),sampled=key;bool covered=false;for(const Series&s:d_->series)if(s.panel==pid&&s.visible&&!s.spec.name.isEmpty()&&!s.empty()){qsizetype at=nearest(s,key);sampled=s.data[size_t(at)].x;covered=true;break;}p.cursorX=key;p.cursorV=true;double yr=qBound(0.,double(m->pos().y()-p.plot.top())/qMax(1,p.plot.height()),1.);QString html=QString("<div style='color:%1'>%2</div>").arg(palette().color(QPalette::ToolTipText).name(),a.distance?QString("%1 m").arg(qRound(sampled)):timeText(sampled));QLocale loc;
            for(const Series&s:d_->series)if(s.panel==pid&&s.visible&&!s.spec.name.isEmpty()&&!s.empty()){qsizetype at=nearest(s,key);QString value=s.spec.tipGroupThousands?loc.toString(s.data[size_t(at)].y,'f',s.spec.tipPrecision):QString::number(s.data[size_t(at)].y,'f',s.spec.tipPrecision);if(!s.spec.unit.isEmpty())value+=(s.spec.unit=="%"?"":" ")+s.spec.unit;html+=QString("<div style='color:%1'><b>%2:</b> %3</div>").arg(s.spec.color.name(),s.spec.name,value);}if(d_->sync&&covered){double st=interpolate(a.sessionKeys,a.sessionTimes,sampled);for(auto*c:liveCharts())if(c->isVisible())html+=c->showSyncedCursor(st,key,a.distance,yr,this,pid);}d_->tooltip->setText(html);d_->tooltip->adjustSize();QPoint pos=m->pos()+QPoint(14,14);if(pos.x()+d_->tooltip->width()>width())pos.setX(m->pos().x()-14-d_->tooltip->width());if(pos.y()+d_->tooltip->height()>height())pos.setY(m->pos().y()-14-d_->tooltip->height());d_->tooltip->move(pos);d_->tooltip->show();d_->tooltip->raise();d_->overlay->update();}}
    if(d_->nav&&d_->navAxis>=0&&d_->navAxis<d_->axes.size()){if(e->type()==QEvent::Wheel){auto*x=static_cast<QWheelEvent*>(e);if(x->modifiers()&Qt::ControlModifier)zoomX(std::exp(-x->angleDelta().y()/1200.));else panX(-(x->angleDelta().x()+x->angleDelta().y())/1200.);return true;}if(e->type()==QEvent::MouseButtonDblClick){resetX();return true;}if(e->type()==QEvent::MouseButtonPress){auto*m=static_cast<QMouseEvent*>(e);if(m->button()==Qt::LeftButton){const Axis&a=d_->axes[d_->navAxis];d_->dragging=true;d_->dragStart=m->pos();d_->dragMin=a.lo;d_->dragMax=a.hi;return true;}}}return QWidget::eventFilter(w,e);
}

void ChartView::requestReplot(){if(isVisible()){d_->canvas->update();d_->overlay->update();}}
void ChartView::applyPaletteText(){if(d_->tooltip){QColor bg=palette().color(QPalette::Button),fg=palette().color(QPalette::ToolTipText),border=fg;border.setAlpha(90);d_->tooltip->setStyleSheet(QString("background:rgba(%1,%2,%3,%4);color:%5;border:1px solid %6;padding:5px 8px;").arg(bg.red()).arg(bg.green()).arg(bg.blue()).arg(bg.alpha()).arg(fg.name(),border.name()));}}
void ChartView::resizeEvent(QResizeEvent*e){QWidget::resizeEvent(e);d_->overlay->setGeometry(rect());d_->geometry(rect());d_->overlay->raise();positionPanelChartSettings();}
void ChartView::changeEvent(QEvent*e){QWidget::changeEvent(e);if(e->type()==QEvent::FontChange)d_->geometry(rect());if(e->type()==QEvent::PaletteChange||e->type()==QEvent::ApplicationPaletteChange){applyPaletteText();requestReplot();}}
