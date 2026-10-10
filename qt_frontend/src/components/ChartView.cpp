#include "ChartView.h"
#include "../ChartGraphicsBackend.h"
#include "../SessionModel.h"
#include "../ChartWindowCombo.h"
#include "../PresentationScheduler.h"
#include "CardColors.h"

#include <QComboBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFontMetricsF>
#include <QFrame>
#include <QJsonArray>
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
#include <QHash>
#include <QPair>
#include <cmath>
#include <cstring>
#include <functional>
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
// Electron TimeChart chrome (TimeChartView/axisPlugin/referenceLines). The
// colours are expressed against the active palette so they follow the theme.
QColor mixColor(const QColor& from, const QColor& to, double amount) {
    return QColor::fromRgbF(from.redF() + (to.redF() - from.redF()) * amount,
                            from.greenF() + (to.greenF() - from.greenF()) * amount,
                            from.blueF() + (to.blueF() - from.blueF()) * amount);
}
QColor chartGridColor() { return tnr::isDarkTheme() ? QColor(255, 255, 255, 10) : QColor(0, 0, 0, 18); }
QColor chartReferenceSolidColor() { return tnr::isDarkTheme() ? QColor(255, 255, 255, 51) : QColor(0, 0, 0, 46); }
QColor chartBorderColor(const QPalette& palette) {
    return mixColor(palette.color(QPalette::Window), palette.color(QPalette::Text),
                    tnr::isDarkTheme() ? .10 : .30);
}
// Crosshairs and the tooltip header use Electron's secondary/axis text colour.
QColor chartAxisTextColor(const QPalette& palette) { return palette.color(QPalette::PlaceholderText); }
constexpr int kTooltipGap = 16, kTooltipPad = 4;

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

// A sample's key is stored as a float offset from a double origin (Series::key):
// offsets up to ~10,000 s or metres keep about a millisecond or millimetre of
// precision, well inside the sample spacing. GPU coordinates are relative to a
// per-upload origin.
//
// Keys live in a KeyColumn that series with identical keys share (Speed and
// RPM, the four tyre corners, ...), so each of them stores 4 bytes a sample
// for its values and the keys are stored once. A series holds keys
// [0, ys.size()) of its column; a sharer that appended ahead may have added
// more. A series that must change its keys takes a private copy first.
struct KeyColumn { double origin = 0; std::vector<float> dx; };
struct GpuPoint { float x, y; };
struct Segment { GpuPoint a, b; };
static_assert(sizeof(Segment) == sizeof(float) * 4);

struct Axis {
    ChartView::Side side = ChartView::Side::Left;
    double lo = 0, hi = 1;
    QColor color;
    bool inherit = true, visible = true, grid = false, time = false, distance = false;
    bool lap = false;   // lap-number axis: sessionKeys are laps, sessionTimes their starts
    char format = 'f'; int precision = 0, panel = 0;
    int tickSpacePx = 80;
    bool lapBoundaryLabels = false;
    bool gridDashed = false;   // Electron gridDash [3, 3]
    int tickMarkPx = 0;        // Electron xTickSize; 0 = no tick marks
    int labelWidth = 1, laneOffset = 0;
    int laneOrder = 0;   // position among same-side axes, innermost first
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
    std::shared_ptr<KeyColumn> keys = std::make_shared<KeyColumn>();
    std::vector<float> ys;
    qsizetype first = 0, dirty = 0, fitDirty = 0;
    quint64 revision = 1;
    QRect legendHit;
    qsizetype count() const { return qsizetype(ys.size()); }
    qsizetype size() const { return count() - first; }
    bool empty() const { return size() <= 0; }
    double key(qsizetype i) const { return keys->origin + double(keys->dx[size_t(i)]); }
    double firstKey() const { return key(first); }
    double lastKey() const { return key(count() - 1); }
    float offset(double x) const { return float(x - keys->origin); }
    bool sharesKeys() const { return keys.use_count() > 1; }
    // Before changing its keys: a private copy of the ones this series holds.
    void ownKeys() {
        if (!sharesKeys()) return;
        auto own = std::make_shared<KeyColumn>();
        own->origin = keys->origin;
        own->dx.assign(keys->dx.begin(), keys->dx.begin() + qsizetype(ys.size()));
        keys = std::move(own);
    }
    // Empty, with a fresh unshared column.
    void reset() {
        std::vector<float>().swap(ys);
        if (sharesKeys()) keys = std::make_shared<KeyColumn>();
        else std::vector<float>().swap(keys->dx);
        first = dirty = fitDirty = 0; ++revision;
    }
};

struct Band { ChartView::BandSpec spec; int panel = 0; };
struct ReferenceLine { int axis; double value; bool dashed; };

struct PanelDivider {
    QRect rect;
    QFrame::Shape shape = QFrame::VLine;
};

struct Panel {
    bool visible = true, header = false, legend = true;
    QString title, note;
    QRect outer, plot;
    QComboBox *window = nullptr, *lap = nullptr;
    QWidget* control = nullptr;   // caller-supplied header control (setPanelHeaderControl)
    int syncOrder = -1;           // < 0: follow the bound section's order
    tnr::GraphSection section = tnr::GraphSection::Count_;
    bool cursorV = false, cursorH = false;
    double cursorX = 0, cursorY = .5;
    // Hover markers (Electron nearestPoint / sync points): a ring on every
    // visible series at the sample nearest dotX. A strict panel (a synced
    // peer) hides a series whose data starts after dotX, but holds its
    // endpoint when the cursor is past its newest sample.
    bool dots = false, dotsStrict = false;
    double dotX = 0;
    std::function<QString(const QVector<double>&)> tooltipExtra;
};

qsizetype lowerBound(const Series& s, double x) {
    const auto begin = s.keys->dx.begin();
    auto it = std::lower_bound(begin + s.first, begin + s.count(), x,
        [&s](float dx, double key) { return s.keys->origin + double(dx) < key; });
    return qsizetype(std::distance(begin, it));
}

qsizetype nearest(const Series& s, double x) {
    if (s.empty()) return -1;
    qsizetype i = lowerBound(s, x);
    if (i >= s.count()) return s.count() - 1;
    if (i > s.first && x - s.key(i - 1) <= s.key(i) - x) --i;
    return i;
}

// Drops the trimmed front once it is large. A shared key column moves only as
// far as every series on it has trimmed, and all of them move together.
void compact(QVector<Series>& all, Series& s) {
    if (s.first < kCompactAt || s.first * 2 < s.count()) return;
    qsizetype cut = s.first;
    if (s.sharesKeys()) {
        for (const Series& other : all)
            if (other.keys == s.keys) cut = qMin(cut, other.first);
        if (cut < kCompactAt) return;
    }
    auto& dx = s.keys->dx;
    dx.erase(dx.begin(), dx.begin() + cut);
    for (Series& other : all) {
        if (other.keys != s.keys) continue;
        other.ys.erase(other.ys.begin(), other.ys.begin() + cut);
        other.first -= cut;
        other.dirty = other.fitDirty = 0; ++other.revision;
    }
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

// Cursor-sync axis kinds, as Electron's ChartCursorAxisKind.
enum AxisKind { TimeAxis = 0, DistanceAxis = 1, LapAxis = 2 };
int axisKind(const Axis& a) { return a.lap ? LapAxis : a.distance ? DistanceAxis : TimeAxis; }

// Session time behind x on this axis. A lap stands for the time it began, or
// NaN when that is not known (it then syncs with other lap axes only).
double axisSessionTime(const Axis& a, double x) {
    if (!a.lap) return interpolate(a.sessionKeys, a.sessionTimes, x);
    for (int i = 0; i < a.sessionKeys.size() && i < a.sessionTimes.size(); ++i)
        if (std::abs(a.sessionKeys[i] - x) < 1e-6) return a.sessionTimes[i];
    return qQNaN();
}

// The lap whose span holds the session time: the last one started by then.
double lapAtSessionTime(const Axis& a, double time) {
    if (!std::isfinite(time)) return qQNaN();
    for (int i = qMin(a.sessionKeys.size(), a.sessionTimes.size()) - 1; i >= 0; --i)
        if (a.sessionTimes[i] <= time) return a.sessionKeys[i];
    return qQNaN();
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

// Shaping a label costs far more than formatting it. Widths are cached per
// font (its key plus the metrics height, which also covers the device DPI),
// so steady-state repaints, relayouts and hover tooltips never re-shape a
// label they have already measured.
class TextWidths {
public:
    static TextWidths& of(const QFont& font, const QPaintDevice* device) {
        static QHash<QString, std::shared_ptr<TextWidths>> all;
        const QFontMetricsF metrics(font, device);
        const QString key = font.key() + QLatin1Char('|') + QString::number(metrics.height());
        std::shared_ptr<TextWidths>& entry = all[key];
        if (!entry) entry = std::make_shared<TextWidths>(metrics);
        return *entry;
    }
    explicit TextWidths(const QFontMetricsF& metrics) : metrics_(metrics) {}
    const QFontMetricsF& metrics() const { return metrics_; }
    qreal advance(const QString& text) {
        const auto it = widths_.constFind(text);
        if (it != widths_.constEnd()) return *it;
        if (widths_.size() >= 4096) widths_.clear();   // bound live tick churn
        const qreal width = metrics_.horizontalAdvance(text);
        widths_.insert(text, width);
        return width;
    }
private:
    QFontMetricsF metrics_;
    QHash<QString, qreal> widths_;
};

int measuredYAxisLabelWidth(const Axis& axis, TextWidths& widths) {
    int width = 1;
    for (double tick : yTicksWithUpperBound(axis))
        width = qMax(width, int(std::ceil(widths.advance(mappedTickText(axis, tick)))) + 2);
    return width;
}

// Bumped whenever chart data or presentation changes. A hover over the same
// snapped sample reuses its tooltip while this is unchanged.
quint64& chartContentGeneration() { static quint64 generation = 0; return generation; }

// Small order-dependent hash used to detect which overlay strips changed.
void hashMix(size_t& h, size_t v) { h ^= v + size_t(0x9e3779b97f4a7c15ULL) + (h << 6) + (h >> 2); }
void hashMix(size_t& h, double v) { quint64 bits; std::memcpy(&bits, &v, sizeof bits); hashMix(h, size_t(bits ^ (bits >> 32))); }
void hashMix(size_t& h, const QString& v) { hashMix(h, size_t(qHash(v))); }
void hashMix(size_t& h, const QRect& v) {
    hashMix(h, size_t(uint(v.x()))); hashMix(h, size_t(uint(v.y())));
    hashMix(h, size_t(uint(v.width()))); hashMix(h, size_t(uint(v.height())));
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
              QVector<Panel>* p, QVector<int>* order, QVector<ReferenceLine>* refs,
              const bool* shared, QWidget* parent)
        : QRhiWidget(parent), axes(a), series(s), bands(b), panels(p), drawOrder(order),
          references(refs), sharedCursor(shared) {
        setApi(tnr::graphics::activeApi());
        // The canvas owns its render target (see makeTarget): QRhiWidget only
        // provides the single-sample texture it composites. Charts never
        // depth-test, so the automatic target's depth-stencil buffer would be
        // waste, and owning the multisample buffer lets a hidden chart free it.
        setAutoRenderTarget(false);
        setMouseTracking(true);
    }
    void applySettings() {
        if (requestedSamples != msaaSamples()) {
            requestedSamples = msaaSamples();
            releaseTarget();
            linePipe.reset(); nativePipe.reset(); fillPipe.reset(); targetPass.reset();
        }
        update();
    }

    // Memory-log accounting: allocated GPU buffer sizes, the CPU-side
    // staging/run caches that persist between uploads, and the render target
    // (QRhiWidget's texture plus the multisample buffer, at 4 bytes per pixel
    // per sample).
    struct Retention {
        quint64 gpuBuffers = 0, gpuBytes = 0, stagingBytes = 0, cacheBytes = 0, targetBytes = 0;
        QSize targetSize; int samples = 1;
    };
    void addRetention(Retention& r) const {
        auto addBuffer = [&](const std::unique_ptr<QRhiBuffer>& buffer) {
            if (!buffer) return;
            ++r.gpuBuffers; r.gpuBytes += buffer->size();
        };
        for (const auto* list : {&gpu, &gpuBands}) {
            r.cacheBytes += quint64(list->capacity()) * sizeof(Gpu);
            for (const Gpu& g : *list) {
                addBuffer(g.line); addBuffer(g.fill); addBuffer(g.lineUbo); addBuffer(g.fillUbo);
                r.cacheBytes += quint64(g.runs.capacity()) * sizeof(g.runs[0]);
                r.stagingBytes += quint64(g.lineStaging.capacity()) + quint64(g.fillStaging.capacity());
            }
        }
        addBuffer(templateUbo);
        r.cacheBytes += quint64(chromeSlots.capacity()) * sizeof(ChromeSlot);
        for (const ChromeSlot& slot : chromeSlots) { addBuffer(slot.vertices); addBuffer(slot.ubo); }
        r.samples = msaa ? msaa->sampleCount() : 1;
        auto pixels = [](QSize s) { return quint64(qMax(0, s.width())) * quint64(qMax(0, s.height())); };
        if (const QRhiTexture* t = colorTexture()) { r.targetSize = t->pixelSize(); r.targetBytes += pixels(t->pixelSize()) * 4; }
        if (msaa) r.targetBytes += pixels(msaa->pixelSize()) * 4 * quint64(msaa->sampleCount());
    }
    QRhi* rhiDevice() const { return device; }

    // Render at `scale` device pixels per logical pixel regardless of the
    // screen (2x screenshots on a 1x display); 0 returns to the screen's ratio.
    void setCaptureScale(qreal scale) {
        setFixedColorBufferSize(scale > 0 ? (QSizeF(size()) * scale).toSize() : QSize());
    }


protected:
    // Device pixels per logical pixel of the target being drawn: the screen's
    // ratio, unless a capture fixed the colour buffer to another scale.
    double renderScale(QSize targetSize) const {
        if (fixedColorBufferSize().isEmpty() || width() <= 0) return devicePixelRatioF();
        return double(targetSize.width()) / width();
    }

    // Called whenever QRhiWidget (re)creates its texture: first show and resize.
    void initialize(QRhiCommandBuffer*) override {
        if (device != rhi()) { releaseResources(); device = rhi(); }
        releaseTarget();   // the colour texture was recreated
    }

    // A hidden chart keeps only QRhiWidget's single-sample texture and its
    // CPU series. The multisample buffer (most of the target's memory) and
    // the vertex buffers are rebuilt from those when it is shown again.
    void hideEvent(QHideEvent* e) override {
        releaseTarget();
        gpu.clear(); gpuBands.clear(); chromeSlots.clear();
        QRhiWidget::hideEvent(e);
    }

    void render(QRhiCommandBuffer* cb) override {
        if (device && !target) makeTarget();
        if (device && target && !linePipe) makePipelines();
        if (!device || !target || !linePipe || !nativePipe || !fillPipe) return;
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
                up->uploadStaticBuffer(g.line.get(), 0, sizeof(v), v); g.uploaded = 1;
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
            // Round points are discs drawn with the chrome above the traces.
            if (lineType(s) == ChartView::LineType::RoundPoint) continue;
            const Axis& x = (*axes)[s.spec.xAxisId];
            const double pad = s.spec.width * .5 * (x.hi - x.lo) / (*panels)[s.panel].plot.width();
            if (s.lastKey() < x.lo - pad || s.firstKey() > x.hi + pad) continue;
            upload(id, up);
            Gpu& g = gpu[size_t(id)];
            qsizetype begin = lowerBound(s, x.lo - pad); if (begin > s.first) --begin;
            qsizetype end = lowerBound(s, x.hi + pad); if (end < s.count()) ++end;
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
        const QSize targetSize = target->pixelSize();
        // Chart chrome on the GPU (Electron's axis-plugin grid, borders,
        // reference lines, crosshairs and nearest-point markers). Geometry is
        // in logical pixels; hairlines snap to device-pixel centres and stay
        // one physical pixel wide, like the cosmetic pens they replace.
        const double dpr = renderScale(targetSize);
        const QSizeF logical(targetSize.width() / dpr, targetSize.height() / dpr);
        const float hair = float(.5 / dpr);
        auto snap = [dpr](double v) { return (std::floor(v * dpr) + .5) / dpr; };
        const QColor gridColor = chartGridColor(), borderColor = chartBorderColor(palette());
        const QColor axisColor = chartAxisTextColor(palette()), background = palette().color(QPalette::Window);
        const qreal labelHeight = std::ceil(TextWidths::of(chartLabelFont(font()), this).metrics().height()) + 2;
        QVector<ChromeBatch> under, over;
        // Batches are addressed by index: appending a batch may reallocate.
        auto batch = [](QVector<ChromeBatch>& list, const QColor& color, float halfWidth,
                        const QRectF& clip, bool strip = false) {
            list.push_back({color, halfWidth, strip, clip, {}, {}});
            return int(list.size() - 1);
        };
        auto segment = [](ChromeBatch& b, QPointF a, QPointF c) {
            b.segments.push_back({{float(a.x()), float(a.y())}, {float(c.x()), float(c.y())}});
        };
        auto dashed = [&](ChromeBatch& b, QPointF a, QPointF c, double on, double off) {
            const QPointF d = c - a;
            const double length = std::hypot(d.x(), d.y());
            if (length <= 0) return;
            if (on <= 0) { segment(b, a, c); return; }
            const QPointF unit = d / length;
            for (double t = 0; t < length; t += on + off)
                segment(b, a + unit * t, a + unit * qMin(length, t + on));
        };
        const QRectF everything(QPointF(0, 0), logical);
        // A filled disc appended to a triangle-strip batch; a repeated vertex
        // pair bridges consecutive discs with degenerate triangles.
        auto appendDisc = [](std::vector<GpuPoint>& strip, double cx, double cy, double radius) {
            constexpr int sides = 24;
            constexpr double turn = 6.283185307179586;
            const auto rim = [&](int i) {
                const double angle = turn * i / sides;
                return GpuPoint{float(cx + radius * std::cos(angle)), float(cy + radius * std::sin(angle))};
            };
            const GpuPoint centre{float(cx), float(cy)};
            if (!strip.empty()) { const GpuPoint last = strip.back(); strip.push_back(last); strip.push_back(rim(0)); }
            for (int i = 0; i <= sides; ++i) {
                strip.push_back(rim(i));
                if (i < sides) strip.push_back(centre);
            }
        };
        QVector<int> sharedPanels;
        for (int pid = 0; pid < panels->size(); ++pid) {
            if (!drawable(pid)) continue;
            const Panel& p = (*panels)[pid];
            const QRectF plot(p.plot), outer(p.outer);
            // Round points (Electron line type 3), in draw order above every
            // trace and below the axes chrome, crosshairs and hover markers.
            for (int id : *drawOrder) {
                if (id < 0 || id >= series->size()) continue;
                const Series& s = (*series)[id];
                if (s.panel != pid || !s.visible || s.empty() ||
                    lineType(s) != ChartView::LineType::RoundPoint ||
                    s.spec.xAxisId < 0 || s.spec.xAxisId >= axes->size() ||
                    s.spec.yAxisId < 0 || s.spec.yAxisId >= axes->size()) continue;
                const Axis& ax = (*axes)[s.spec.xAxisId];
                const Axis& ay = (*axes)[s.spec.yAxisId];
                if (ax.hi <= ax.lo || ay.hi <= ay.lo || s.spec.width <= 0) continue;
                QColor color = s.spec.color;
                color.setAlphaF(color.alphaF() * s.spec.opacity);
                const int discs = batch(over, color, 0, plot, true);
                const double radius = s.spec.width * .5;
                for (qsizetype i = s.first; i < s.count(); ++i) {
                    const float pointY = s.ys[size_t(i)];
                    if (!std::isfinite(pointY)) continue;
                    const double px = plot.left() + (s.key(i) - ax.lo) / (ax.hi - ax.lo) * plot.width();
                    const double py = plot.top() + (ay.hi - pointY) / (ay.hi - ay.lo) * plot.height();
                    if (!std::isfinite(px) || !std::isfinite(py) ||
                        px < plot.left() - radius || px > plot.left() + plot.width() + radius ||
                        py < plot.top() - radius || py > plot.top() + plot.height() + radius) continue;
                    appendDisc(over[discs].vertices, px, py, radius);
                }
            }
            const int grid = batch(under, gridColor, hair, plot);
            for (const Axis& a : *axes) {
                if (!a.visible || !a.grid || a.panel != pid || a.hi <= a.lo) continue;
                const double on = a.gridDashed ? 3 : 0, off = 3;
                if (a.side == ChartView::Side::Bottom) {
                    const int capacity = qMax(2, int(plot.width()) / qMax(1, a.tickSpacePx));
                    for (double tick : ticksFor(a, capacity)) {
                        const double x = snap(plot.left() + (tick - a.lo) / (a.hi - a.lo) * plot.width());
                        dashed(under[grid], {x, plot.top()}, {x, plot.top() + plot.height()}, on, off);
                    }
                } else {
                    for (double tick : yTicksWithUpperBound(a, int(plot.height()), int(std::ceil(labelHeight + 2)))) {
                        const double y = snap(plot.top() + (a.hi - tick) / (a.hi - a.lo) * plot.height());
                        dashed(under[grid], {plot.left(), y}, {plot.left() + plot.width(), y}, on, off);
                    }
                }
            }
            const int border = batch(over, borderColor, hair, outer);
            const double bottom = snap(plot.top() + plot.height()), left = snap(plot.left());
            segment(over[border], {plot.left(), bottom}, {plot.left() + plot.width(), bottom});
            segment(over[border], {left, plot.top()}, {left, plot.top() + plot.height()});
            const int solidRefs = batch(over, chartReferenceSolidColor(), hair, outer);
            const int dashedRefs = batch(over, gridColor, hair, outer);
            for (const ReferenceLine& ref : *references) {
                const Axis& axis = (*axes)[ref.axis];
                if (axis.panel != pid || ref.value < axis.lo || ref.value > axis.hi) continue;
                const double y = snap(plot.top() + plot.height() * (axis.hi - ref.value) / (axis.hi - axis.lo));
                dashed(over[ref.dashed ? dashedRefs : solidRefs], {plot.left(), y}, {plot.left() + plot.width(), y},
                       ref.dashed ? 4 : 0, 4);
            }
            const int cross = batch(over, axisColor, hair, outer);
            int xAxis = -1;
            for (int i = 0; i < axes->size(); ++i)
                if ((*axes)[i].panel == pid && (*axes)[i].side == ChartView::Side::Bottom) { xAxis = i; break; }
            if (p.cursorV && xAxis >= 0 && (*axes)[xAxis].hi > (*axes)[xAxis].lo) {
                const Axis& a = (*axes)[xAxis];
                const double x = plot.left() + (p.cursorX - a.lo) / (a.hi - a.lo) * plot.width();
                if (*sharedCursor) sharedPanels << pid;
                else if (x >= plot.left() && x <= plot.left() + plot.width())
                    dashed(over[cross], {snap(x), plot.top()}, {snap(x), plot.top() + plot.height()}, 2, 1);
            }
            if (p.cursorH) {
                const double y = snap(plot.top() + p.cursorY * plot.height());
                dashed(over[cross], {plot.left(), y}, {plot.left() + plot.width(), y}, 2, 1);
            }
            if (p.dots) {
                // Electron's nearest-point ring: r = 3, stroked in the series
                // colour at its line width and filled with the panel background.
                // All discs share one fill batch drawn before the rings.
                const int fill = batch(over, background, 0, outer, true);
                for (const Series& s : *series) {
                    if (s.panel != pid || !s.visible || s.empty() ||
                        s.spec.xAxisId < 0 || s.spec.xAxisId >= axes->size() ||
                        s.spec.yAxisId < 0 || s.spec.yAxisId >= axes->size()) continue;
                    const Axis& ax = (*axes)[s.spec.xAxisId];
                    const Axis& ay = (*axes)[s.spec.yAxisId];
                    if (ax.hi <= ax.lo || ay.hi <= ay.lo) continue;
                    if (s.spec.hoverSnap == ChartView::HoverSnap::None) continue;
                    if (p.dotsStrict && p.dotX < s.firstKey() - 1e-6) continue;
                    qsizetype at = -1;
                    if (s.spec.hoverSnap == ChartView::HoverSnap::Next) {
                        at = lowerBound(s, p.dotX);
                        if (at >= s.count()) continue;
                    } else {
                        at = nearest(s, p.dotX);
                    }
                    if (at < 0) continue;
                    const float pointY = s.ys[size_t(at)];
                    if (!std::isfinite(pointY)) continue;
                    const double px = plot.left() + (s.key(at) - ax.lo) / (ax.hi - ax.lo) * plot.width();
                    const double py = plot.top() + (ay.hi - pointY) / (ay.hi - ay.lo) * plot.height();
                    if (!std::isfinite(px) || !std::isfinite(py) ||
                        px < plot.left() - .5 || px > plot.left() + plot.width() + .5 ||
                        py < plot.top() - .5 || py > plot.top() + plot.height() + .5) continue;
                    QColor stroke = s.spec.color;
                    stroke.setAlphaF(stroke.alphaF() * s.spec.opacity);
                    const int ring = batch(over, stroke, float(qMax(1.0, s.spec.width) * .5), outer);
                    constexpr int sides = 24;
                    constexpr double radius = 3.0, turn = 6.283185307179586;
                    const auto rim = [&](int i) {
                        const double angle = turn * i / sides;
                        return GpuPoint{float(px + radius * std::cos(angle)), float(py + radius * std::sin(angle))};
                    };
                    const GpuPoint centre{float(px), float(py)};
                    // Disc as one triangle strip (rim, centre, rim, ...); a
                    // repeated vertex pair bridges discs with degenerate triangles.
                    std::vector<GpuPoint>& disc = over[fill].vertices;
                    if (!disc.empty()) { const GpuPoint last = disc.back(); disc.push_back(last); disc.push_back(rim(0)); }
                    for (int i = 0; i <= sides; ++i) {
                        disc.push_back(rim(i));
                        if (i < sides) disc.push_back(centre);
                    }
                    for (int i = 0; i < sides; ++i) over[ring].segments.push_back({rim(i), rim(i + 1)});
                }
            }
        }
        // Electron's stacked Analyze chart is one TimeChart: its vertical
        // crosshair crosses every panel and the gaps between them.
        if (!sharedPanels.isEmpty()) {
            const Panel& first = (*panels)[sharedPanels.first()];
            double top = first.plot.top(), bottom = first.plot.top() + first.plot.height(), x = qQNaN();
            for (int pid : sharedPanels) {
                const QRectF plot((*panels)[pid].plot);
                top = qMin(top, plot.top()); bottom = qMax(bottom, plot.top() + plot.height());
            }
            for (const Axis& a : *axes)
                if (a.panel == sharedPanels.first() && a.side == ChartView::Side::Bottom && a.hi > a.lo) {
                    const QRectF plot(first.plot);
                    x = plot.left() + (first.cursorX - a.lo) / (a.hi - a.lo) * plot.width();
                    break;
                }
            if (std::isfinite(x)) {
                const int shared = batch(over, axisColor, hair, everything);
                dashed(over[shared], {snap(x), top}, {snap(x), bottom}, 2, 1);
            }
        }
        const QVector<ChromeDraw> underDraws = uploadChrome(under, 0, logical, up);
        const QVector<ChromeDraw> overDraws = uploadChrome(over, under.size(), logical, up);
        cb->beginPass(target.get(), palette().color(QPalette::Window), {1, 0}, up);
        const double ratio = dpr;
        auto viewport = [&](int id) {
            const QRect r = (*panels)[id].plot;
            const double left = r.x() * ratio, right = (r.x() + r.width()) * ratio;
            const double top = r.y() * ratio, bottom = (r.y() + r.height()) * ratio;
            // QRhi viewport/scissor coordinates are bottom-left based on every API.
            const double y = targetSize.height() - bottom;
            // Viewports accept fractions. Only scissors require integer pixels;
            // round those outwards to avoid shaving off fractional edge coverage.
            cb->setViewport(QRhiViewport(float(left), float(y), float(right - left), float(bottom - top)));
            const int x0 = qBound(0, int(std::floor(left)), targetSize.width());
            const int x1 = qBound(x0, int(std::ceil(right)), targetSize.width());
            const int y0 = qBound(0, int(std::floor(y)), targetSize.height());
            const int y1 = qBound(y0, int(std::ceil(targetSize.height() - top)), targetSize.height());
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
        // Chrome uses the whole target as its viewport, clipped per batch.
        auto drawChrome = [&](const QVector<ChromeDraw>& list) {
            QRhiGraphicsPipeline* active = nullptr;
            for (const ChromeDraw& d : list) {
                auto* pipe = d.strip ? fillPipe.get() : linePipe.get();
                if (pipe != active) { cb->setGraphicsPipeline(pipe); active = pipe; }
                cb->setViewport(QRhiViewport(0, 0, float(targetSize.width()), float(targetSize.height())));
                const double left = d.clip.left() * ratio, right = (d.clip.left() + d.clip.width()) * ratio;
                const double top = d.clip.top() * ratio, bottom = (d.clip.top() + d.clip.height()) * ratio;
                const int x0 = qBound(0, int(std::floor(left)), targetSize.width());
                const int x1 = qBound(x0, int(std::ceil(right)), targetSize.width());
                const int y0 = qBound(0, int(std::floor(targetSize.height() - bottom)), targetSize.height());
                const int y1 = qBound(y0, int(std::ceil(targetSize.height() - top)), targetSize.height());
                cb->setScissor(QRhiScissor(x0, y0, x1 - x0, y1 - y0));
                cb->setShaderResources(d.srb);
                QRhiCommandBuffer::VertexInput input(d.vb, 0);
                cb->setVertexInput(0, 1, &input);
                if (d.strip) cb->draw(d.count);
                else cb->draw(6, d.count);
            }
        };
        drawChrome(underDraws);   // grid behind the traces, as Electron's grid canvas
        draw(bandDraws, true); draw(fillDraws, true); draw(lineDraws, false);
        drawChrome(overDraws);
        cb->endPass();
    }

    void releaseResources() override {
        linePipe.reset(); nativePipe.reset(); fillPipe.reset(); templateSrb.reset(); templateUbo.reset();
        releaseTarget(); targetPass.reset();
        gpu.clear(); gpuBands.clear(); chromeSlots.clear(); device = nullptr;
    }

private:
    void releaseTarget() { target.reset(); msaa.reset(); }

    // Colour-only render target: a multisample buffer resolving into
    // QRhiWidget's texture, or the texture itself without MSAA. Pipelines
    // survive while the new target's render pass stays compatible.
    void makeTarget() {
        releaseTarget();
        QRhiTexture* texture = colorTexture();
        if (!device || !texture) {
            qWarning("[charts] No colour texture for the chart render target");
            return;
        }
        int samples = 1;
        for (int supported : device->supportedSampleCounts())
            if (supported <= requestedSamples) samples = qMax(samples, supported);
        QRhiColorAttachment color(texture);
        if (samples > 1) {
            msaa.reset(device->newRenderBuffer(QRhiRenderBuffer::Color, texture->pixelSize(), samples,
                                               {}, texture->format()));
            if (!msaa->create()) {
                qWarning("[charts] Creating the chart multisample buffer failed");
                msaa.reset(); samples = 1;
            } else {
                color = QRhiColorAttachment(msaa.get());
                color.setResolveTexture(texture);
            }
        }
        target.reset(device->newTextureRenderTarget(QRhiTextureRenderTargetDescription(color)));
        std::unique_ptr<QRhiRenderPassDescriptor> pass(target->newCompatibleRenderPassDescriptor());
        if (!targetPass || !linePipe || pipelineSamples != samples || !pass->isCompatible(targetPass.get())) {
            linePipe.reset(); nativePipe.reset(); fillPipe.reset();
            targetPass = std::move(pass);
            pipelineSamples = samples;
        }
        target->setRenderPassDescriptor(targetPass.get());
        if (!target->create()) {
            qWarning("[charts] Creating the chart render target failed");
            releaseTarget();
        }
    }

    struct Gpu {
        std::unique_ptr<QRhiBuffer> line, fill, lineUbo, fillUbo;
        std::unique_ptr<QRhiShaderResourceBindings> lineSrb, fillSrb;
        qsizetype cap = 0, fillCap = 0; quint64 uploaded = 0;
        bool instanced = false;
        double origin = 0;
        std::vector<std::pair<qsizetype, qsizetype>> runs;
        QByteArray lineStaging, fillStaging;
    };

    // One retained vertex buffer + uniform per chrome batch slot.
    struct ChromeBatch {
        QColor color; float halfWidth = .5f; bool strip = false; QRectF clip;
        std::vector<Segment> segments; std::vector<GpuPoint> vertices;
    };
    struct ChromeDraw { QRhiBuffer* vb; QRhiShaderResourceBindings* srb; quint32 count; bool strip; QRectF clip; };
    struct ChromeSlot {
        std::unique_ptr<QRhiBuffer> vertices, ubo;
        std::unique_ptr<QRhiShaderResourceBindings> srb;
        quint32 capacity = 0;
    };

    QVector<ChromeDraw> uploadChrome(const QVector<ChromeBatch>& list, int firstSlot, const QSizeF& logical,
                                     QRhiResourceUpdateBatch* up) {
        QVector<ChromeDraw> draws;
        QMatrix4x4 m = device->clipSpaceCorrMatrix(), ortho;
        ortho.ortho(0.f, float(logical.width()), float(logical.height()), 0.f, -1.f, 1.f);
        m *= ortho;
        for (int i = 0; i < list.size(); ++i) {
            const ChromeBatch& b = list[i];
            const size_t count = b.strip ? b.vertices.size() : b.segments.size();
            if (!count) continue;
            const quint32 bytes = quint32(b.strip ? count * sizeof(GpuPoint) : count * sizeof(Segment));
            const size_t index = size_t(firstSlot + i);
            if (chromeSlots.size() <= index) chromeSlots.resize(index + 1);
            ChromeSlot& slot = chromeSlots[index];
            if (!slot.srb) slot.srb = makeSrb(slot.ubo);
            if (!slot.vertices || slot.capacity < bytes) {
                quint32 capacity = 256;
                while (capacity < bytes) capacity <<= 1;
                slot.capacity = capacity;
                slot.vertices.reset(device->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, capacity));
                slot.vertices->create();
            }
            up->updateDynamicBuffer(slot.vertices.get(), 0, bytes,
                b.strip ? static_cast<const void*>(b.vertices.data()) : static_cast<const void*>(b.segments.data()));
            Uniform u{};
            std::memcpy(u.mvp, m.constData(), sizeof(u.mvp));
            u.color[0] = b.color.redF(); u.color[1] = b.color.greenF();
            u.color[2] = b.color.blueF(); u.color[3] = b.color.alphaF();
            u.stroke[0] = float(2.0 / logical.width()); u.stroke[1] = float(2.0 / logical.height());
            u.stroke[2] = b.halfWidth; u.stroke[3] = 0.f;
            up->updateDynamicBuffer(slot.ubo.get(), 0, sizeof(u), &u);
            draws.push_back({slot.vertices.get(), slot.srb.get(), quint32(count), b.strip, b.clip});
        }
        return draws;
    }

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
        if (!device || !target) return;
        if (!templateSrb) templateSrb = makeSrb(templateUbo);
        const QShader vs = shader(":/shaders/chart.vert.qsb"), stroke = shader(":/shaders/chartline.vert.qsb"),
                      fs = shader(":/shaders/chart.frag.qsb");
        if (!vs.isValid() || !stroke.isValid() || !fs.isValid()) { qCritical("[charts] QRhi shaders are missing"); return; }
        auto make = [&](QRhiGraphicsPipeline::Topology topology, bool instanced) {
            std::unique_ptr<QRhiGraphicsPipeline> p(device->newGraphicsPipeline());
            p->setTopology(topology); p->setSampleCount(pipelineSamples);
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
            p->setRenderPassDescriptor(targetPass.get());
            if (p->create()) return p;
            qWarning("[charts] Creating a chart pipeline failed");
            return std::unique_ptr<QRhiGraphicsPipeline>();
        };
        linePipe = make(QRhiGraphicsPipeline::Triangles, true);
        nativePipe = make(QRhiGraphicsPipeline::LineStrip, false);
        fillPipe = make(QRhiGraphicsPipeline::TriangleStrip, false);
    }

    static qsizetype capacity(qsizetype needed) { qsizetype n = 1024; while (n < needed) n *= 2; return n; }

    // Series vertices are Static: written once, then only appended to. With
    // Dynamic buffers Qt's D3D11 backend keeps a full CPU copy of each buffer
    // and re-uploads all of it on every change, and the driver keeps more
    // copies: end-of-race All Laps playback measured 1,002 MB with Dynamic and
    // 698 MB with Static on the Input page.
    void ensureGpu(Gpu& g, qsizetype vertices, qsizetype fillVertices, bool instanced) {
        if (!g.line || g.cap < vertices || g.instanced != instanced) {
            g.cap = capacity(qMax<qsizetype>(1, vertices)); g.instanced = instanced;
            g.line.reset(device->newBuffer(QRhiBuffer::Static, QRhiBuffer::VertexBuffer,
                int(g.cap * (instanced ? sizeof(Segment) : sizeof(GpuPoint)))));
            g.line->create(); g.uploaded = 0;
        }
        if (!g.lineSrb) g.lineSrb = makeSrb(g.lineUbo);
        if (fillVertices && (!g.fill || g.fillCap < fillVertices)) {
            g.fillCap = capacity(fillVertices);
            g.fill.reset(device->newBuffer(QRhiBuffer::Static, QRhiBuffer::VertexBuffer, int(g.fillCap * sizeof(GpuPoint))));
            g.fill->create(); g.uploaded = 0;
        }
        if (fillVertices && !g.fillSrb) g.fillSrb = makeSrb(g.fillUbo);
    }

    void upload(int id, QRhiResourceUpdateBatch* up) {
        Series& s = (*series)[id]; Gpu& g = gpu[size_t(id)];
        const qsizetype count = s.count();
        const auto type = lineType(s);
        const bool native = type == ChartView::LineType::NativeLine;
        const bool points = type == ChartView::LineType::NativePoint;
        const int segments = pathSegments(s);
        const qsizetype pathCount = (count - 1) * segments + 1;
        ensureGpu(g, native || points ? count : pathCount - 1, s.spec.fill ? pathCount * 2 : 0, !native);
        if (g.uploaded == s.revision) return;
        qsizetype dirty = g.uploaded ? qBound<qsizetype>(0, s.dirty, count) : 0;
        if (!dirty) g.origin = s.key(0);
        // Append/replace repairs the previous interval too, including its step corners.
        const qsizetype start = dirty ? dirty - 1 : 0;
        g.lineStaging.resize(0); g.fillStaging.resize(0);
        auto point = [&](qsizetype i) { return GpuPoint{float(s.key(i) - g.origin), s.ys[size_t(i)]}; };
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
                const double transition = s.key(i) +
                    (s.key(i + 1) - s.key(i)) * s.spec.stepLocation - g.origin;
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
        if (!g.lineStaging.isEmpty()) up->uploadStaticBuffer(g.line.get(), quint32(lineStart * (native ? sizeof(GpuPoint) : sizeof(Segment))), g.lineStaging);
        if (!g.fillStaging.isEmpty()) up->uploadStaticBuffer(g.fill.get(), quint32(start * segments * 2 * sizeof(GpuPoint)), g.fillStaging);
        // The update batch holds its own reference to the bytes. Appends are a
        // few points, so only a full rebuild leaves a large staging buffer:
        // drop it rather than keep a CPU copy of the whole series.
        constexpr qsizetype kKeepStaging = 64 * 1024;
        if (g.lineStaging.capacity() > kKeepStaging) g.lineStaging = QByteArray();
        if (g.fillStaging.capacity() > kKeepStaging) g.fillStaging = QByteArray();
        // Preserve runs before the changed suffix and repair the crossing run.
        qsizetype scan = start;
        while (!g.runs.empty() && g.runs.back().first >= start) g.runs.pop_back();
        if (!g.runs.empty() && g.runs.back().second > start) g.runs.back().second = start;
        for (; scan < count;) {
            while (scan < count && !std::isfinite(s.ys[size_t(scan)])) ++scan;
            const qsizetype first = scan;
            while (scan < count && std::isfinite(s.ys[size_t(scan)])) ++scan;
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
    QVector<ReferenceLine>* references; const bool* sharedCursor;
    QRhi* device = nullptr;
    std::vector<ChromeSlot> chromeSlots;
    std::vector<Gpu> gpu, gpuBands;
    std::unique_ptr<QRhiBuffer> templateUbo;
    std::unique_ptr<QRhiShaderResourceBindings> templateSrb;
    // Declared before the pipelines, which are built against targetPass, so
    // the pipelines are destroyed first.
    std::unique_ptr<QRhiRenderBuffer> msaa;
    std::unique_ptr<QRhiTextureRenderTarget> target;
    std::unique_ptr<QRhiRenderPassDescriptor> targetPass;
    int requestedSamples = msaaSamples();
    int pipelineSamples = 1;
    std::unique_ptr<QRhiGraphicsPipeline> linePipe, nativePipe, fillPipe;
};

// Raster chrome above the QRhi canvas: panel titles, legends, axis labels,
// x tick marks and the map playheads. Grid, borders, reference lines,
// crosshairs and hover markers are drawn by RhiCanvas. Repaints are limited
// to the strips whose inputs changed (see invalidateChanged()).
class Overlay final : public QWidget {
public:
    Overlay(QVector<Axis>* a, QVector<Series>* s, QVector<Panel>* p,
            QVector<ChartView::CursorGuide>* cursors, QWidget* parent)
        : QWidget(parent), axes(a), series(s), panels(p), cursorGuides(cursors) {
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

    // Schedule a repaint of only the chrome strips whose inputs changed since
    // the last call. A scrolling time axis dirties just its x-label strip;
    // titles, legends and unchanged y labels keep their composited pixels.
    void invalidateChanged() {
        size_t global = 0;
        const QPalette pal = palette();
        hashMix(global, size_t(pal.color(QPalette::Window).rgba()));
        hashMix(global, size_t(pal.color(QPalette::Text).rgba()));
        hashMix(global, size_t(pal.color(QPalette::PlaceholderText).rgba()));
        hashMix(global, font().key());
        hashMix(global, devicePixelRatioF());
        hashMix(global, size_t(panels->size()));
        if (global != globalKey || scheduled.size() != panels->size()) {
            globalKey = global;
            scheduled.resize(panels->size());
            for (int pid = 0; pid < panels->size(); ++pid) scheduled[pid] = keysFor(pid);
            update();
            return;
        }
        QRegion dirty;
        for (int pid = 0; pid < panels->size(); ++pid) {
            const PanelKeys keys = keysFor(pid);
            PanelKeys& old = scheduled[pid];
            if (keys.visible != old.visible || keys.outer != old.outer || keys.plot != old.plot) {
                dirty += old.outer; dirty += keys.outer;
            } else if (keys.visible) {
                for (int part = 0; part < PartCount; ++part)
                    if (keys.part[part] != old.part[part]) dirty += partRect(keys, part);
            }
            old = keys;
        }
        if (!dirty.isEmpty()) update(dirty);
    }

    // Forget the scheduled state so the next invalidateChanged() repaints all.
    void invalidateAll() { globalKey = 0; update(); }

protected:
    void paintEvent(QPaintEvent* event) override {
        const QRegion region = event->region();
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
        TextWidths& widths = TextWidths::of(smallFont, this);
        const qreal labelHeight = std::ceil(widths.metrics().height()) + 2;
        // Snap hairlines in DEVICE pixels; cosmetic pens stay one physical pixel wide.
        auto hairline = [&](QPointF a, QPointF b, const QColor& color) {
            auto snap = [&](QPointF point) {
                const QPointF physical = toPixels.map(point);
                return fromPixels.map(QPointF(std::floor(physical.x()) + .5, std::floor(physical.y()) + .5));
            };
            QPen pen(color, 1); pen.setCosmetic(true);
            q.setPen(pen); q.drawLine(snap(a), snap(b));
        };
        const QColor axisColor = chartAxisTextColor(palette());
        for (int pid = 0; pid < panels->size(); ++pid) {
            Panel& p = (*panels)[pid]; if (!p.visible || p.plot.isEmpty()) continue;
            if (!region.intersects(p.outer)) continue;
            PanelKeys bounds; bounds.outer = p.outer; bounds.plot = p.plot;
            const bool paintHeader = region.intersects(partRect(bounds, Header));
            const bool paintPlot = region.intersects(partRect(bounds, PlotArea));
            // QRect::right/bottom are inclusive integer coordinates. QRectF
            // shares the GPU's x+width/y+height edges instead of shifting by 1.
            const QRectF plot(p.plot), outer(p.outer);
            q.save(); q.setClipRect(outer);
            if (p.header && paintHeader) {
                q.setFont(boldFont); q.setPen(muted);
                drawText(QRectF(outer.left() + kSidePad, outer.top(), outer.width() - kSidePad, kHeader),
                           Qt::AlignLeft | Qt::AlignVCenter, p.title);
            }
            for (const Axis& a : *axes) {
                if (!a.visible || a.panel != pid || a.hi <= a.lo) continue;
                const Part part = a.side == ChartView::Side::Bottom ? Bottom
                    : a.side == ChartView::Side::Left ? LeftGutter : RightGutter;
                if (!region.intersects(partRect(bounds, part))) continue;
                const int capacity = a.side == ChartView::Side::Bottom
                    ? qMax(2, int(plot.width()) / qMax(1, a.tickSpacePx)) : 5;
                const double step = a.step > 0 ? a.step : niceStep(a.hi - a.lo, capacity);
                const QColor axisText = a.inherit ? text : a.color;
                const QVector<double> ticks = a.side == ChartView::Side::Bottom
                    ? ticksFor(a, capacity) : yTicksWithUpperBound(a, int(plot.height()), int(std::ceil(labelHeight + 2)));
                q.setFont(smallFont);
                if (a.side == ChartView::Side::Bottom) {
                    auto labelRect = [&](double value, const QString& label) {
                        const double x = plot.left() + (value - a.lo) / (a.hi - a.lo) * plot.width();
                        const qreal width = widths.advance(label) + 4;
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
                        if (a.tickMarkPx > 0)
                            hairline(QPointF(x, plot.bottom()), QPointF(x, plot.bottom() + a.tickMarkPx), axisColor);
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
                    qreal previousBottom = -std::numeric_limits<qreal>::infinity();
                    QString previousLabel;
                    // Top bound wins when a short panel cannot fit every label.
                    for (auto it = ticks.crbegin(); it != ticks.crend(); ++it) {
                        const double y = plot.top() + (a.hi - *it) / (a.hi - a.lo) * plot.height();
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
            int guideAxis = -1;
            for (int i = 0; i < axes->size(); ++i)
                if ((*axes)[i].panel == pid && (*axes)[i].side == ChartView::Side::Bottom) {
                    guideAxis = i;
                    break;
                }
            if (paintPlot && guideAxis >= 0 && !cursorGuides->isEmpty()) {
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
            // Legend hit boxes are rebuilt whenever the legend's strip repaints,
            // so a partial repaint elsewhere keeps the last valid ones.
            const bool legendArea = p.header ? paintHeader : paintPlot;
            if (legendArea)
                for (Series& s : *series) if (s.panel == pid) s.legendHit = {};
            if (p.legend && legendArea) {
                q.setFont(smallFont); QVector<int> ids; qreal total = 0;
                for (int i = 0; i < series->size(); ++i) if ((*series)[i].panel == pid && !(*series)[i].spec.name.isEmpty()) {
                    ids.push_back(i); total += 24 + widths.advance((*series)[i].spec.name);
                }
                // Electron's secondary-coloured note after the key (e.g. "resets each lap").
                const qreal noteW = p.note.isEmpty() ? 0 : 8 + widths.advance(p.note);
                total += noteW;
                qreal x = p.header ? outer.right() - kSidePad - total : plot.center().x() - total / 2;
                const qreal y = p.header ? outer.top() + (kHeader - labelHeight) / 2 : plot.top() + 4;
                for (int id : ids) {
                    Series& s = (*series)[id]; const qreal w = 24 + widths.advance(s.spec.name);
                    q.setPen(QPen(s.spec.color, 2));
                    q.drawLine(QPointF(x, y + labelHeight / 2), QPointF(x + 12, y + labelHeight / 2));
                    q.setPen(s.visible ? text : muted);
                    drawText(QRectF(x + 16, y, w - 16, labelHeight), Qt::AlignVCenter, s.spec.name);
                    s.legendHit = QRectF(x - 2, y - 3, w, labelHeight + 6).toAlignedRect(); x += w;
                }
                if (noteW > 0) {
                    q.setPen(muted);
                    drawText(QRectF(x + 8, y, noteW - 8, labelHeight), Qt::AlignVCenter, p.note);
                }
            }
            q.restore();
        }
    }

private:
    enum Part { Header, LeftGutter, RightGutter, Bottom, PlotArea, PartCount };
    struct PanelKeys {
        bool visible = false;
        QRect outer, plot;
        size_t part[PartCount] = {};
    };

    // The strip of a panel each part paints into. Strips overlap at the
    // corners; paintEvent() repaints every part that meets the dirty region.
    static QRect partRect(const PanelKeys& k, int part) {
        const QRect& o = k.outer; const QRect& p = k.plot;
        switch (part) {
        case Header: return QRect(o.left(), o.top(), o.width(), qMax(0, p.top() - o.top()));
        case LeftGutter: return QRect(o.left(), o.top(), qMax(0, p.left() - o.left()), o.height());
        case RightGutter: return QRect(p.right() + 1, o.top(), qMax(0, o.right() - p.right()), o.height());
        case Bottom: return QRect(o.left(), p.bottom() + 1, o.width(), qMax(0, o.bottom() - p.bottom()));
        default: return p;
        }
    }

    static size_t axisKey(const Axis& a) {
        size_t h = 0;
        hashMix(h, size_t(a.visible)); hashMix(h, size_t(a.side)); hashMix(h, a.lo); hashMix(h, a.hi);
        hashMix(h, size_t(a.inherit)); hashMix(h, size_t(a.color.rgba())); hashMix(h, size_t(a.format));
        hashMix(h, size_t(a.precision)); hashMix(h, size_t(a.tickSpacePx)); hashMix(h, size_t(a.lapBoundaryLabels));
        hashMix(h, size_t(a.tickMarkPx)); hashMix(h, size_t(a.labelWidth)); hashMix(h, size_t(a.laneOffset));
        hashMix(h, a.scale); hashMix(h, a.step); hashMix(h, a.suffix); hashMix(h, a.timeFormat);
        hashMix(h, size_t(a.time)); hashMix(h, size_t(a.distance));
        for (double tick : a.ticks) hashMix(h, tick);
        for (const QString& label : a.labels) hashMix(h, label);
        return h;
    }

    PanelKeys keysFor(int pid) const {
        PanelKeys k;
        const Panel& p = (*panels)[pid];
        k.visible = p.visible && !p.plot.isEmpty();
        if (!k.visible) return k;
        k.outer = p.outer; k.plot = p.plot;
        size_t legend = 0;
        hashMix(legend, size_t(p.legend)); hashMix(legend, p.note);
        for (const Series& s : *series) {
            if (s.panel != pid || s.spec.name.isEmpty()) continue;
            hashMix(legend, s.spec.name); hashMix(legend, size_t(s.spec.color.rgba())); hashMix(legend, size_t(s.visible));
        }
        hashMix(k.part[Header], size_t(p.header)); hashMix(k.part[Header], p.title);
        hashMix(k.part[p.header ? Header : PlotArea], legend);
        int guideAxis = -1;
        for (int i = 0; i < axes->size(); ++i) {
            const Axis& a = (*axes)[i];
            if (a.panel != pid) continue;
            const Part part = a.side == ChartView::Side::Bottom ? Bottom
                : a.side == ChartView::Side::Left ? LeftGutter : RightGutter;
            hashMix(k.part[part], axisKey(a));
            if (guideAxis < 0 && a.side == ChartView::Side::Bottom) guideAxis = i;
        }
        if (guideAxis >= 0) {
            hashMix(k.part[PlotArea], (*axes)[guideAxis].lo); hashMix(k.part[PlotArea], (*axes)[guideAxis].hi);
            for (const ChartView::CursorGuide& guide : *cursorGuides) {
                hashMix(k.part[PlotArea], guide.x); hashMix(k.part[PlotArea], size_t(guide.color.rgba()));
            }
        }
        return k;
    }

    QVector<Axis>* axes; QVector<Series>* series; QVector<Panel>* panels;
    QVector<ChartView::CursorGuide>* cursorGuides;
    QVector<QFrame*> dividerFrames;
    size_t globalKey = 0;
    QVector<PanelKeys> scheduled;
};

// Hover tooltip (Electron TOOLTIP_STYLE) in its own frameless, translucent
// tool window: moving it never repaints the main window or its QRhi charts.
// The 0 4px 16px rgba(0,0,0,.3) shadow is rendered once per box size into a
// cached image instead of a per-frame QGraphicsEffect. Content lines are laid
// out like the Electron markup (line-height 1.5, collapsing margins,
// border-top separators, real opacity for the comparison section).
class TooltipWidget final : public QWidget {
public:
    explicit TooltipWidget(QWidget* parent)
        : QWidget(parent, Qt::ToolTip | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint |
                          Qt::WindowTransparentForInput | Qt::WindowDoesNotAcceptFocus) {
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        // Breeze's ShadowHelper gives every Qt::ToolTip window a KWin shadow
        // around its full rect, i.e. around our transparent shadow margin,
        // doubling up with the cached shadow. Opt out before first polish.
        setProperty("_KDE_NET_WM_SKIP_SHADOW", true);
        hide();
    }

    void setTheme(const QFont& base, const QColor& background, const QColor& text, const QColor& border) {
        font_ = base; font_.setFeature(QFont::Tag("tnum"), 1);
        background_ = background; text_ = text; border_ = border;
        relayout(true);
    }

    void setContent(const ChartView::TooltipContent& content) {
        if (content == content_) return;
        content_ = content;
        relayout(false);
    }

    QSize boxSize() const { return box_; }
    // Place the bordered box (not its shadow margin) at a global position.
    void moveBoxTo(const QPoint& globalTopLeft) { move(globalTopLeft - QPoint(kShadowSide, kShadowTop)); }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.fillRect(rect(), Qt::transparent);
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        p.drawImage(0, 0, shadow_);
        p.setRenderHint(QPainter::Antialiasing);
        p.translate(kShadowSide, kShadowTop);
        p.setPen(QPen(border_, 1));
        p.setBrush(background_);
        p.drawRoundedRect(QRectF(0, 0, box_.width(), box_.height()).adjusted(.5, .5, -.5, -.5), 4, 4);
        p.setRenderHint(QPainter::TextAntialiasing);
        const qreal left = kBorder + kPadX, right = box_.width() - kBorder - kPadX;
        qreal y = kBorder + kPadY;
        int previousMargin = 0;
        for (int i = 0; i < content_.size(); ++i) {
            const ChartView::TooltipLine& line = content_[i];
            y += i == 0 ? line.marginTop : qMax(previousMargin, line.marginTop);
            p.setOpacity(line.opacity);
            if (line.ruleAbove) {
                QPen rule(border_, 1); rule.setCosmetic(true);
                p.setPen(rule);
                p.drawLine(QPointF(left, y + .5), QPointF(right, y + .5));
                y += 1 + line.paddingTop;
            }
            const QFont font = lineFont(line);
            TextWidths& widths = TextWidths::of(font, this);
            const QFontMetricsF& metrics = widths.metrics();
            const qreal lineHeight = std::round(line.pixelSize * kLineHeight);
            const qreal baseline = y + (lineHeight - metrics.ascent() - metrics.descent()) / 2 + metrics.ascent();
            p.setFont(font);
            qreal x = left;
            for (const ChartView::TooltipRun& run : line.runs) {
                p.setPen(run.color.isValid() ? run.color : text_);
                p.drawText(QPointF(x, baseline), run.text);
                x += widths.advance(run.text);
            }
            p.setOpacity(1);
            y += lineHeight;
            previousMargin = line.marginBottom;
        }
    }

private:
    static constexpr int kBorder = 1, kPadX = 10, kPadY = 6;
    static constexpr qreal kLineHeight = 1.5;   // Tailwind preflight line-height
    // CSS blur 16px with a 4px downward offset: 16px of shadow on each side,
    // 12px above the box and 20px below it.
    static constexpr int kShadowSide = 16, kShadowTop = 12, kShadowBottom = 20;

    QFont lineFont(const ChartView::TooltipLine& line) const {
        QFont font = font_;
        font.setPixelSize(line.pixelSize);
        return font;
    }

    void relayout(bool themeChanged) {
        qreal width = 0, height = 0;
        int previousMargin = 0;
        for (int i = 0; i < content_.size(); ++i) {
            const ChartView::TooltipLine& line = content_[i];
            height += i == 0 ? line.marginTop : qMax(previousMargin, line.marginTop);
            if (line.ruleAbove) height += 1 + line.paddingTop;
            TextWidths& widths = TextWidths::of(lineFont(line), this);
            qreal lineWidth = 0;
            for (const ChartView::TooltipRun& run : line.runs) lineWidth += widths.advance(run.text);
            width = qMax(width, lineWidth);
            height += std::round(line.pixelSize * kLineHeight);
            previousMargin = line.marginBottom;
        }
        if (!content_.isEmpty()) height += content_.last().marginBottom;
        const QSize box(int(std::ceil(width)) + 2 * (kBorder + kPadX),
                        int(std::ceil(height)) + 2 * (kBorder + kPadY));
        if (box != box_ || themeChanged || shadow_.isNull()) {
            box_ = box;
            renderShadow();
            resize(box_ + QSize(2 * kShadowSide, kShadowTop + kShadowBottom));
        }
        update();
    }

    // Gaussian-like blur (three box passes, sigma ~ 8 px) of the box shape.
    void renderShadow() {
        const QSize size = box_ + QSize(2 * kShadowSide, kShadowTop + kShadowBottom);
        const int w = size.width(), h = size.height();
        std::vector<int> alpha(size_t(w) * size_t(h), 0), scratch(alpha.size());
        constexpr int kShadowAlpha = 77;   // rgba(0,0,0,0.3)
        const int boxLeft = kShadowSide, boxTop = kShadowTop + 4;
        for (int y = boxTop; y < qMin(h, boxTop + box_.height()); ++y)
            for (int x = boxLeft; x < qMin(w, boxLeft + box_.width()); ++x)
                alpha[size_t(y) * w + x] = kShadowAlpha;
        constexpr int radius = 8;   // three passes of width 17: sigma ~ 8 px (CSS blur 16px)
        auto pass = [&](bool horizontal) {
            const int outer = horizontal ? h : w, inner = horizontal ? w : h;
            for (int o = 0; o < outer; ++o) {
                auto at = [&](std::vector<int>& v, int i) -> int& {
                    return horizontal ? v[size_t(o) * w + i] : v[size_t(i) * w + o];
                };
                int sum = 0;
                for (int i = -radius; i <= radius; ++i) sum += (i >= 0 && i < inner) ? at(alpha, i) : 0;
                for (int i = 0; i < inner; ++i) {
                    at(scratch, i) = sum / (2 * radius + 1);
                    const int add = i + radius + 1, drop = i - radius;
                    if (add < inner) sum += at(alpha, add);
                    if (drop >= 0) sum -= at(alpha, drop);
                }
            }
            alpha.swap(scratch);
        };
        for (int i = 0; i < 3; ++i) { pass(true); pass(false); }
        shadow_ = QImage(size, QImage::Format_ARGB32_Premultiplied);
        for (int y = 0; y < h; ++y) {
            auto* line = reinterpret_cast<QRgb*>(shadow_.scanLine(y));
            for (int x = 0; x < w; ++x) line[x] = qRgba(0, 0, 0, alpha[size_t(y) * w + x]);
        }
    }

    ChartView::TooltipContent content_;
    QFont font_;
    QColor background_, text_, border_;
    QSize box_;
    QImage shadow_;
};
} // namespace

struct ChartView::Impl {
    RhiCanvas* canvas = nullptr; Overlay* overlay = nullptr;
    // Lives on the top-level window, like Electron's portal tooltip, so it
    // can extend past this chart's bounds.
    QPointer<TooltipWidget> tooltip;
    bool sharedCursor = false;
    // Tooltip for the last hovered snapped sample; reused while the pointer
    // stays on that sample and no chart content changed (Electron does the same).
    struct HoverCache {
        bool valid = false, sync = false;
        int panel = -1;
        double sampled = 0;
        quint64 generation = 0;
        ChartView::TooltipContent content;
    } hoverCache;
    QVector<Axis> axes; QVector<Series> series; QVector<Band> bands; QVector<Panel> panels{Panel{}};
    QVector<ReferenceLine> references;
    QVector<ChartView::CursorGuide> cursorGuides;
    QVector<int> order; QVector<QVector<int>> rows; QVector<int> linkedXAxes; int columns = 1;
    bool explicitRows = false, hover = false, sync = false, secondaryV = true, secondaryH = false;
    bool alignedInsets = false;   // panels in a column share their widest gutters
    QString cursorMode; QPointer<SessionModel> model;
    int navAxis = -1; bool nav = false, dragging = false;
    double navMin = 0, navMax = 1, navSpan = .5, dragMin = 0, dragMax = 1;
    int dragWidth = 1; QPoint dragStart;
    QTimer* hoverTimer = nullptr;
    QPoint hoverPosition;
    bool hoverActive = false;
    bool releaseSeriesWhenHidden = false;

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
        TextWidths& axisWidths = TextWidths::of(axisFont, overlay);
        const QFontMetricsF& axisMetrics = axisWidths.metrics();
        QVector<PanelDivider> dividers;
        auto lr = layoutRows();
        if (lr.isEmpty()) {
            if (overlay) overlay->setPanelDividers(dividers);
            return;
        }
        QHash<int, QMargins> insets;   // per panel, for the stacked pass below
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
                    auto sideInset = [&](QVector<int> sideAxes) {
                        if (sideAxes.isEmpty()) return kPlotEdgePad;
                        std::stable_sort(sideAxes.begin(), sideAxes.end(), [&](int a, int b) {
                            return axes[a].laneOrder < axes[b].laneOrder;
                        });
                        int used = 0;
                        for (int index = 0; index < sideAxes.size(); ++index) {
                            Axis& axis = axes[sideAxes[index]];
                            axis.labelWidth = measuredYAxisLabelWidth(axis, axisWidths);
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
                        const int firstWidth = int(std::ceil(axisWidths.advance(mappedTickText(axis, ticks.first()))));
                        const int lastWidth = int(std::ceil(axisWidths.advance(mappedTickText(axis, ticks.last()))));
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
                    const int bottomInset = bottomAxes.isEmpty() ? 4 : int(std::ceil(axisMetrics.height())) + 10;
                    insets.insert(id, QMargins(leftInset, topInset, rightInset, bottomInset));
                    p.plot = p.outer.adjusted(leftInset, topInset, -rightInset, -bottomInset);
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
        const bool singleColumn = std::all_of(lr.cbegin(), lr.cend(), [&](const QVector<int>& row) {
            return row.size() == 1 && row.first() >= 0 && row.first() < panels.size();
        });
        if (alignedInsets && lr.size() > 1 && singleColumn) {
            // Electron's stacked AnalyzeTimeChart: one plot area (top padding
            // and x-axis strip taken once) split into equal slices, each minus
            // a 10 px gap after it. Panels squash as more are added; they never
            // drop out, unlike the generic split's per-panel insets.
            constexpr int kStackedGap = 10;
            const int n = lr.size();
            const int top = bounds.top() + insets.value(lr.first().first()).top();
            const int bottom = bounds.bottom() + 1 - insets.value(lr.last().first()).bottom();
            const double span = qMax(0, bottom - top) / double(n);
            auto sliceTop = [&](int index) { return top + int(std::lround(index * span)); };
            dividers.clear();
            for (int ri = 0; ri < n; ++ri) {
                const int id = lr[ri].first();
                Panel& p = panels[id];
                const QMargins m = insets.value(id);
                const bool lastRow = ri + 1 == n;
                const int plotTop = sliceTop(ri);
                const int plotBottom = qMax(plotTop + 1, sliceTop(ri + 1) - (lastRow ? 0 : kStackedGap));
                p.outer = QRect(QPoint(bounds.left(), ri == 0 ? bounds.top() : plotTop),
                                QPoint(bounds.right(), lastRow ? bounds.bottom() : sliceTop(ri + 1) - 1));
                p.plot = QRect(QPoint(bounds.left() + m.left(), plotTop),
                               QPoint(bounds.right() - m.right(), plotBottom - 1));
                if (p.plot.width() < 8) p.plot = {};
                if (!lastRow && sliceTop(ri + 1) > plotBottom)
                    dividers.push_back({ QRect(bounds.left(), plotBottom, bounds.width(),
                                               sliceTop(ri + 1) - plotBottom), QFrame::HLine });
            }
        }
        if (alignedInsets) {
            // Stacked panels read as one chart only when their plot areas share
            // edges, so every panel in a column takes the column's widest gutters.
            QHash<int, QPair<int, int>> widest;   // outer.left -> (left, right) inset
            for (const Panel& p : panels) {
                if (!p.visible || p.plot.isEmpty()) continue;
                QPair<int, int>& inset = widest[p.outer.left()];
                inset.first = qMax(inset.first, p.plot.left() - p.outer.left());
                inset.second = qMax(inset.second, p.outer.right() - p.plot.right());
            }
            for (Panel& p : panels) {
                if (!p.visible || p.plot.isEmpty()) continue;
                const QPair<int, int> inset = widest.value(p.outer.left());
                p.plot.setLeft(p.outer.left() + inset.first);
                p.plot.setRight(p.outer.right() - inset.second);
                if (p.plot.width() < 8) p.plot = {};
            }
        }
        if (overlay) overlay->setPanelDividers(dividers);
    }
};

ChartView::ChartView(QWidget* parent) : QWidget(parent), d_(std::make_unique<Impl>()) {
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding); setMinimumHeight(120);
    auto* layout = new QVBoxLayout(this); layout->setContentsMargins(0,0,0,0);
    d_->canvas = new RhiCanvas(&d_->axes, &d_->series, &d_->bands, &d_->panels, &d_->order,
                               &d_->references, &d_->sharedCursor, this);
    // Release the next chart batch once this canvas has submitted its frame —
    // the Qt counterpart of Electron's requestAnimationFrame pacing.
    connect(d_->canvas, &QRhiWidget::frameSubmitted, this,
            [] { PresentationScheduler::instance().chartFrameSubmitted(); });
    layout->addWidget(d_->canvas); d_->overlay = new Overlay(
        &d_->axes, &d_->series, &d_->panels, &d_->cursorGuides, this);
    d_->overlay->installEventFilter(this); d_->overlay->raise(); liveCharts().push_back(this);
    d_->hoverTimer = new QTimer(this);
    d_->hoverTimer->setSingleShot(true);
    d_->hoverTimer->setInterval(16);
    connect(d_->hoverTimer, &QTimer::timeout, this, [this] {
        if (d_->hoverActive && isVisible() && !d_->dragging) updateHover(d_->hoverPosition);
    });
    connect(d_->canvas, &QRhiWidget::renderFailed, this, [] { qCritical("[charts] Rendering failed; select another backend in Settings and restart"); });
}
ChartView::~ChartView() { liveCharts().removeAll(this); delete d_->tooltip.data(); }
void ChartView::suspendOpenGlForStyleChange() {}
QJsonObject ChartView::retentionDiagnostics() {
    quint64 charts = 0, visibleCharts = 0, buffers = 0, rows = 0, cpuBytes = 0, gpuBuffers = 0, gpuBytes = 0;
    quint64 seriesBytes = 0, stagingBytes = 0, targetBytes = 0;
    QJsonArray perChart;
    QRhi* rhi = nullptr;
    for (const ChartView* view : liveCharts()) {
        ++charts;
        quint64 viewRows = 0, viewSeriesBytes = 0;
        for (const Series& s : view->d_->series) {
            ++buffers;
            viewRows += quint64(qMax<qsizetype>(0, s.size()));
            viewSeriesBytes += quint64(s.ys.capacity()) * sizeof(float) +
                quint64(s.keys->dx.capacity()) * sizeof(float) / quint64(qMax<long>(1, s.keys.use_count()));
        }
        RhiCanvas::Retention r;
        view->d_->canvas->addRetention(r);
        if (!rhi) rhi = view->d_->canvas->rhiDevice();
        const bool visible = view->isVisible();
        if (visible) ++visibleCharts;
        rows += viewRows; seriesBytes += viewSeriesBytes; stagingBytes += r.stagingBytes;
        cpuBytes += viewSeriesBytes + r.stagingBytes + r.cacheBytes;
        gpuBuffers += r.gpuBuffers; gpuBytes += r.gpuBytes; targetBytes += r.targetBytes;
        // Owner chain names the page and section without each chart naming itself.
        QStringList owners;
        for (const QObject* o = view; o && owners.size() < 3; o = o->parent()) {
            const QString cls = QString::fromLatin1(o->metaObject()->className());
            if (cls.startsWith(QLatin1Char('Q')) && o != view) continue;
            owners << (o->objectName().isEmpty() ? cls : cls + QLatin1Char('#') + o->objectName());
        }
        QJsonObject chart;
        chart["owner"] = owners.join(QStringLiteral(" < "));
        chart["visible"] = visible;
        chart["series"] = double(view->d_->series.size());
        chart["rows"] = double(viewRows);
        chart["series_bytes"] = double(viewSeriesBytes);
        chart["staging_bytes"] = double(r.stagingBytes);
        chart["gpu_buffer_bytes"] = double(r.gpuBytes);
        chart["render_target_bytes"] = double(r.targetBytes);
        chart["render_target"] = QStringLiteral("%1x%2x%3").arg(r.targetSize.width()).arg(r.targetSize.height()).arg(r.samples);
        perChart.append(chart);
    }
    QJsonObject result;
    result["chart_count"] = double(charts);
    result["visible_chart_count"] = double(visibleCharts);
    result["buffer_count"] = double(buffers);
    result["rows"] = double(rows);
    result["cpu_bytes"] = double(cpuBytes);
    result["series_bytes"] = double(seriesBytes);
    result["staging_bytes"] = double(stagingBytes);
    result["gpu_buffers"] = double(gpuBuffers);
    result["gpu_buffer_bytes"] = double(gpuBytes);
    // The dev-tools RAM viewer reads Electron's key for the chart GPU row.
    result["gpu_texture_bytes"] = double(gpuBytes + targetBytes);
    result["render_target_bytes"] = double(targetBytes);
    if (rhi) {
        const QRhiStats stats = rhi->statistics();
        QJsonObject rhiJson;
        rhiJson["backend"] = QString::fromLatin1(rhi->backendName());
        rhiJson["block_count"] = double(stats.blockCount);
        rhiJson["alloc_count"] = double(stats.allocCount);
        rhiJson["used_bytes"] = double(stats.usedBytes);
        rhiJson["unused_bytes"] = double(stats.unusedBytes);
        rhiJson["total_usage_bytes"] = double(stats.totalUsageBytes);
        result["rhi"] = rhiJson;
    }
    result["charts"] = perChart;
    return result;
}

void ChartView::reapplyRenderSettings() { for (auto* v : liveCharts()) { v->d_->canvas->applySettings(); v->d_->overlay->update(); } }
void ChartView::setCaptureScale(qreal scale) { for (auto* v : liveCharts()) if (v->isVisible()) v->d_->canvas->setCaptureScale(scale); }

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
    if (d_->overlay && isVisible()) d_->overlay->invalidateChanged();
}

void ChartView::appendPoint(int id, double x, double y) {
    if (id < 0 || id >= d_->series.size() || !std::isfinite(x)) return;
    Series& s = d_->series[id];
    if (s.ys.empty()) {
        // Starting over: share a column that another series has just begun at
        // the same key (they are appended to in turn), else start a new one.
        bool adopted = false;
        for (Series& other : d_->series) {
            if (&other == &s || other.keys == s.keys || other.count() != 1 ||
                other.keys->dx.size() != 1 || other.key(0) != x) continue;
            s.keys = other.keys;
            adopted = true;
            break;
        }
        if (!adopted) {
            if (s.sharesKeys()) s.keys = std::make_shared<KeyColumn>();
            s.keys->dx.clear();
            s.keys->origin = x;
        }
    }
    const float dx = s.offset(x);
    const float value = float(y);   // Non-finite Y is an explicit gap, including on append.
    if (!s.ys.empty() && x < s.lastKey()) {
        s.ownKeys();
        auto& keys = s.keys->dx;
        keys.resize(s.ys.size());
        auto at = std::lower_bound(keys.begin() + s.first, keys.end(), x,
            [&s](float key, double value) { return s.keys->origin + double(key) < value; });
        const qsizetype i = std::distance(keys.begin(), at);
        keys.insert(at, dx);
        s.ys.insert(s.ys.begin() + i, value);
        s.dirty = qMin(s.dirty, i); s.fitDirty = qMin(s.fitDirty, i);
    } else {
        const size_t n = s.ys.size();
        s.dirty = qMin(s.dirty, qsizetype(n));
        s.fitDirty = qMin(s.fitDirty, qsizetype(n));
        // A sharer may already have appended this key; otherwise add it.
        if (s.keys->dx.size() > n) {
            // Otherwise this series' keys diverge here: keep only its own (a
            // column it now owns alone may still end in a key a former sharer added).
            if (s.keys->dx[n] != dx) { s.ownKeys(); s.keys->dx.resize(n); s.keys->dx.push_back(dx); }
        } else {
            s.keys->dx.push_back(dx);
        }
        s.ys.push_back(value);
    }
    while (s.size() > kMaxPoints) ++s.first;
    compact(d_->series, s); ++s.revision;
}

void ChartView::setSeriesData(int id, const QVector<double>& xs, const QVector<double>& ys) {
    if (id < 0 || id >= d_->series.size() || xs.size() != ys.size()) return;
    Series& s = d_->series[id];
    struct Keyed { double x; double y; };
    std::vector<Keyed> replacement;
    replacement.reserve(size_t(qMin<qsizetype>(kMaxPoints, xs.size())));
    for (qsizetype i = qMax<qsizetype>(0, xs.size() - kMaxPoints); i < xs.size(); ++i)
        if (std::isfinite(xs[i])) replacement.push_back({xs[i], ys[i]});
    // Binary search, clipping and hover all require ordered keys. Equal keys
    // retain source order, including vertical edges and explicit gap samples.
    if (!std::is_sorted(replacement.begin(), replacement.end(),
            [](const Keyed& a, const Keyed& b) { return a.x < b.x; }))
        std::stable_sort(replacement.begin(), replacement.end(),
            [](const Keyed& a, const Keyed& b) { return a.x < b.x; });
    const qsizetype n = qsizetype(replacement.size());
    qsizetype equal = 0;
    // Compare in stored form against the current origin, so identical input
    // keeps its prefix.
    while (equal < qMin(s.size(), n)) {
        const size_t at = size_t(s.first + equal);
        const float oldY = s.ys[at];
        const float nextY = float(replacement[size_t(equal)].y);
        if (s.keys->dx[at] != s.offset(replacement[size_t(equal)].x) ||
            !(oldY == nextY || (std::isnan(oldY) && std::isnan(nextY)))) break;
        ++equal;
    }
    if (equal == n && n == s.size()) return;
    // Preserve the unchanged prefix and upload just the changed suffix. Checking
    // endpoints alone misses interior corrections (notably analysis delta data).
    s.ownKeys();
    const size_t keep = size_t(s.first + equal);
    s.keys->dx.resize(keep);
    s.ys.resize(keep);
    if (s.ys.empty() && n > 0) s.keys->origin = replacement.front().x;
    s.keys->dx.reserve(keep + size_t(n - equal));
    s.ys.reserve(keep + size_t(n - equal));
    for (qsizetype i = equal; i < n; ++i) {
        s.keys->dx.push_back(s.offset(replacement[size_t(i)].x));
        s.ys.push_back(float(replacement[size_t(i)].y));
    }
    // Series given the same keys (Speed and RPM, tyre corners) share one column.
    for (const Series& other : d_->series) {
        if (&other == &s || other.keys == s.keys || other.count() != s.count() ||
            other.keys->origin != s.keys->origin || other.keys->dx.size() != s.keys->dx.size()) continue;
        if (std::memcmp(other.keys->dx.data(), s.keys->dx.data(), s.keys->dx.size() * sizeof(float)) != 0) continue;
        s.keys = other.keys;
        break;
    }
    s.dirty = qMin(s.dirty, s.first + equal);
    s.fitDirty = qMin(s.fitDirty, s.first + equal);
    compact(d_->series, s); ++s.revision;
}

void ChartView::trimBefore(int id, double x) {
    if (id < 0 || id >= d_->series.size() || !std::isfinite(x)) return;
    Series& s = d_->series[id];
    qsizetype first = lowerBound(s, x);
    if (first > s.first) --first; // Segment crossing the viewport's left edge.
    s.first = first; compact(d_->series, s);
}
void ChartView::clear(int id){if(id<0||id>=d_->series.size())return;d_->series[id].reset();}
void ChartView::clearAll(){for(int i=0;i<d_->series.size();++i)clear(i);}
void ChartView::setReleaseSeriesWhenHidden(bool on) { d_->releaseSeriesWhenHidden = on; }
void ChartView::hideEvent(QHideEvent* e) {
    QWidget::hideEvent(e);
    if (!d_->releaseSeriesWhenHidden) return;
    bool released = false;
    for (Series& s : d_->series) {
        if (s.ys.empty()) continue;
        s.reset();   // frees the allocations, not just the sizes
        released = true;
    }
    if (released) emit seriesReleased();
}
void ChartView::setSeriesVisible(int id, bool on) {
    if (id < 0 || id >= d_->series.size() || d_->series[id].visible == on) return;
    Series& s = d_->series[id]; s.visible = on; ++chartContentGeneration();
    if (s.spec.yAxisId >= 0 && s.spec.yAxisId < d_->axes.size()) d_->axes[s.spec.yAxisId].fitTimer.invalidate();
}
bool ChartView::seriesVisible(int id)const{return id>=0&&id<d_->series.size()&&d_->series[id].visible;}
void ChartView::setSeriesColor(int id,const QColor&c){if(id>=0&&id<d_->series.size())d_->series[id].spec.color=c;}
void ChartView::setSeriesFillColor(int id,const QColor&c){if(id>=0&&id<d_->series.size())d_->series[id].spec.fillColor=c;}
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
void ChartView::setAxisSide(int id,Side side,int laneOrder){if(id<0||id>=d_->axes.size())return;Axis&a=d_->axes[id];if(a.side==side&&a.laneOrder==laneOrder)return;a.side=side;a.laneOrder=laneOrder;d_->geometry(rect());requestReplot();}
void ChartView::setPanelInsetsAligned(bool on){if(d_->alignedInsets==on)return;d_->alignedInsets=on;applyPanelLayout();}
void ChartView::setAxisVisible(int id,bool on){if(id>=0&&id<d_->axes.size()&&d_->axes[id].visible!=on){d_->axes[id].visible=on;d_->geometry(rect());}}
void ChartView::setAxisColor(int id,const QColor&c){if(id>=0&&id<d_->axes.size()){d_->axes[id].color=c;d_->axes[id].inherit=false;}}
void ChartView::setAxisGridVisible(int id,bool on){if(id>=0&&id<d_->axes.size())d_->axes[id].grid=on;}
void ChartView::setAxisGridStyle(int id, bool dashed, int tickMarkPx) {
    if (id < 0 || id >= d_->axes.size()) return;
    d_->axes[id].gridDashed = dashed; d_->axes[id].tickMarkPx = qMax(0, tickMarkPx); requestReplot();
}
void ChartView::setLegendVisible(bool on){if(!d_->panels.isEmpty())d_->panels[0].legend=on;}

int ChartView::addPanel(){d_->panels.push_back(Panel{});return d_->panels.size()-1;}
void ChartView::ensurePanelHeader(int id){if(id>=0&&id<d_->panels.size()){d_->panels[id].header=true;d_->geometry(rect());}}
void ChartView::layoutPanels(int cols){d_->columns=qMax(1,cols);d_->explicitRows=false;applyPanelLayout();}
void ChartView::layoutPanelsRows(const QVector<QVector<int>>&rows){d_->rows=rows;d_->explicitRows=true;for(auto&p:d_->panels)p.visible=false;for(const auto&r:rows)for(int id:r)if(id>=0&&id<d_->panels.size())d_->panels[id].visible=true;applyPanelLayout();}
void ChartView::applyPanelLayout(){d_->geometry(rect());positionPanelChartSettings();requestReplot();}
void ChartView::setPanelVisible(int id,bool on){if(id>=0&&id<d_->panels.size()){d_->panels[id].visible=on;d_->explicitRows=false;applyPanelLayout();}}
void ChartView::setPanelTitle(int id,const QString&t){if(id>=0&&id<d_->panels.size()){ensurePanelHeader(id);d_->panels[id].title=t;positionPanelChartSettings();requestReplot();}}
void ChartView::setPanelNote(int id,const QString&n){if(id>=0&&id<d_->panels.size()){ensurePanelHeader(id);d_->panels[id].note=n;requestReplot();}}
// A hidden legend needs no header row, so only a shown one reserves it.
void ChartView::setPanelLegendVisible(int id,bool on){if(id>=0&&id<d_->panels.size()){if(on)ensurePanelHeader(id);d_->panels[id].legend=on;}}
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
        p.window->blockSignals(true);
        // Same grouped Laps / Time list as the toolbar (heading rows carry no data).
        populateChartWindowCombo(p.window,coords,d_->model->playbackMode());
        int i=p.window->findData(chartWindowKey(d_->model->effectiveChartWindow(p.section)));
        if(i<0)i=p.window->findData(chartWindowKey(ChartWindow::Seconds30));
        p.window->setCurrentIndex(i);p.window->blockSignals(false);
        int wanted=d_->model->referenceLap(p.section);p.lap->blockSignals(true);p.lap->clear();for(const LapBlock&lap:d_->model->data().laps)if(!lap.progress.isEmpty()||d_->model->playbackCatalogHasLapDistance())p.lap->addItem(QString::number(lap.lapNum),lap.lapNum);i=p.lap->findData(wanted);p.lap->setCurrentIndex(i>=0?i:(p.lap->count()?0:-1));p.lap->blockSignals(false);}positionPanelChartSettings();
}

void ChartView::setPanelHeaderControl(int id, QWidget* control) {
    if (id < 0 || id >= d_->panels.size() || !control) return;
    ensurePanelHeader(id);
    Panel& p = d_->panels[id];
    if (p.control && p.control != control) p.control->deleteLater();
    control->setParent(this);
    p.control = control;
    positionPanelChartSettings();
}

void ChartView::setPanelSyncOrder(int id, int order) {
    if (id >= 0 && id < d_->panels.size()) d_->panels[id].syncOrder = order;
}

void ChartView::positionPanelChartSettings(){for(Panel&p:d_->panels){
    if(p.control){
        // Caller controls sit after the title, as the chart-window selector does.
        const bool show=p.visible&&!p.outer.isEmpty();p.control->setVisible(show);
        if(show){QFont f=chartLabelFont(font());f.setBold(true);const int title=int(std::ceil(TextWidths::of(f,d_->overlay).advance(p.title)));
            const int h=qMin(kControlH,qMax(1,p.control->sizeHint().height()));p.control->resize(p.control->sizeHint().width(),h);
            p.control->move(p.outer.left()+kSidePad+title+kTitleControlGap,p.outer.top()+(kHeader-h)/2);p.control->raise();}
    }
    if(!p.window)continue;bool show=p.visible&&!p.outer.isEmpty();p.window->setVisible(show);bool lap=show&&d_->model&&d_->model->playbackMode()&&d_->model->effectiveChartWindow(p.section)==ChartWindow::SelectedLap;p.lap->setVisible(lap);if(!show)continue;QFont f=chartLabelFont(font());f.setBold(true);int title=int(std::ceil(TextWidths::of(f,d_->overlay).advance(p.title)));int x=p.outer.left()+kSidePad+title+kTitleControlGap,y=p.outer.top()+(kHeader-kControlH)/2;p.window->move(x,y);p.window->raise();if(lap){p.lap->move(x+p.window->width()+3,y);p.lap->raise();}}}

void ChartView::setAxisLabelMap(int id,const QVector<double>&ticks,const QStringList&labels,bool lapBoundaryLabels){if(id>=0&&id<d_->axes.size()){auto&a=d_->axes[id];a.time=false;a.lapBoundaryLabels=lapBoundaryLabels;a.ticks=ticks;a.labels=labels;if(a.side!=Side::Bottom)d_->geometry(rect());}}
void ChartView::setAxisLapMap(int id, const QVector<double>& laps, const QVector<double>& lapStarts) {
    if (id < 0 || id >= d_->axes.size() || laps.size() != lapStarts.size()) return;
    Axis& a = d_->axes[id];
    a.lap = true; a.distance = false;
    a.sessionKeys = laps; a.sessionTimes = lapStarts;
}
void ChartView::setAxisNumberSuffix(int id,double scale,const QString&suffix,double step){if(id>=0&&id<d_->axes.size()){auto&a=d_->axes[id];a.time=false;a.scale=std::isfinite(scale)&&scale>0?scale:1.;a.suffix=suffix;a.step=std::isfinite(step)&&step>0?step:0.;if(a.side!=Side::Bottom)d_->geometry(rect());}}
ChartView::TooltipLine ChartView::tooltipTextLine(const QString& text, const QColor& color) {
    TooltipLine line;
    line.runs = {{text, color}};
    return line;
}

// Electron row: <div><span style="color:series">Name</span>: value</div>.
ChartView::TooltipLine ChartView::tooltipValueLine(const QString& name, const QColor& nameColor,
                                                   const QString& value) {
    TooltipLine line;
    line.runs = {{name, nameColor}, {QStringLiteral(": ") + value, QColor()}};
    return line;
}

// Electron formatChartDeltaTooltip: margin-top 5 px, coloured "Delta", 3 decimals.
ChartView::TooltipLine ChartView::tooltipDeltaLine(double delta, const QColor& positive,
                                                   const QColor& negative) {
    TooltipLine line;
    line.runs = {{QStringLiteral("Delta"), delta >= 0 ? positive : negative},
                 {QString(": %1%2 s").arg(delta >= 0 ? QStringLiteral("+") : QString())
                      .arg(delta, 0, 'f', 3), QColor()}};
    line.marginTop = 5;
    return line;
}

void ChartView::setHoverReadout(bool on) {
    d_->hover = on;
    if (on && !d_->tooltip) {
        d_->tooltip = new TooltipWidget(window());
        applyPaletteText();
    }
    if (!on) clearSyncedCursor();
}
void ChartView::setCursorSync(bool on,bool v,bool h){++chartContentGeneration();bool clear=d_->sync&&(!on||d_->secondaryV!=v||d_->secondaryH!=h);d_->sync=on;d_->secondaryV=v;d_->secondaryH=h;if(clear)clearSyncedCursor();}
void ChartView::setCursorModeKey(const QString&key){if(d_->cursorMode==key)return;++chartContentGeneration();d_->cursorMode=key;for(auto*c:liveCharts())c->clearSyncedCursor();}
void ChartView::setPanelsShareCursor(bool on) {
    if (d_->sharedCursor == on) return;
    d_->sharedCursor = on; ++chartContentGeneration(); clearSyncedCursor();
}

namespace {
// Electron's per-page cursor-sync order (PowerBreakdownChart, TyreTrendCharts,
// InputsChart, GearChart, SteeringChart, GForceChart, RideHeightChart,
// SpeedRpmTimeChart). Ties keep registration order.
int cursorSyncOrder(tnr::GraphSection section) {
    using S = tnr::GraphSection;
    switch (section) {
    case S::OverviewTelemetry: return 10;
    case S::OverviewTyreSurface: return 20;
    case S::OverviewTyreInner: return 30;
    case S::OverviewTyreBrake: return 40;
    case S::OverviewTyreWear: return 50;
    case S::TyreSurface: return 10;
    case S::TyreInner: return 20;
    case S::TyreBrake: return 30;
    case S::TyreWear: return 40;
    case S::InputGear: return 10;
    case S::InputThrottleBrake: case S::InputThrottleBrakeOverlay: case S::InputAccelerator: return 20;
    case S::InputBrake: return 30;
    case S::InputSteering: return 40;
    case S::PowerSplit: return 10;
    case S::PowerHarvest: return 20;
    case S::PowerStore: return 30;
    case S::PowerFuel: return 40;
    case S::MiscGForce: case S::MiscGLateral: return 10;
    case S::MiscGLongitudinal: return 20;
    case S::MiscRideHeight: case S::MiscRideFront: return 30;
    case S::MiscRideRear: return 40;
    default: return 100;
    }
}

// Electron formatChartComparisonTooltip heading: secondary colour, border-top,
// margin-top 5 px, padding-top 4 px.
ChartView::TooltipLine tooltipSection(const QString& label, const QColor& muted) {
    ChartView::TooltipLine line = ChartView::tooltipTextLine(label, muted);
    line.marginTop = 5; line.ruleAbove = true; line.paddingTop = 4;
    return line;
}

ChartView::TooltipContent faded(ChartView::TooltipContent lines) {
    for (ChartView::TooltipLine& line : lines) line.opacity *= .35;   // Electron opacity:0.35
    return lines;
}
}

struct ChartView::PanelTooltip {
    TooltipContent current;     // series rows plus the panel's extra row
    TooltipContent comparison;  // comparison-lap rows plus extra row, at 35% opacity
    QString comparisonLabel;    // "Previous lap" / "Fastest lap" / "Reference lap"
    QString syncLabel;          // as above; "Reference lap N" names the lap
    TooltipContent delta;
    bool any = false;
};

struct ChartView::SyncedSample { int order = 100; TooltipContent current, comparison; QString comparisonLabel; };

QVector<ChartView::SyncedSample> ChartView::showSyncedCursor(double time, double sourceX, int sourceKind,
                                                             double yRatio, ChartView* source, int sourcePanel,
                                                             bool buildContent) {
    QVector<SyncedSample> out;
    if (!d_->sync || !d_->hover) return out;
    if (source != this) {
        d_->hoverActive = false; d_->hoverTimer->stop();
        d_->hoverCache.valid = false;
        if (d_->tooltip) d_->tooltip->hide();
    }
    const QColor muted = tooltipMutedColor();
    for (int pid = 0; pid < d_->panels.size(); ++pid) {
        Panel& p = d_->panels[pid];
        int xid = -1;
        for (int i = 0; i < d_->axes.size(); ++i)
            if (d_->axes[i].panel == pid && d_->axes[i].side == Side::Bottom) { xid = i; break; }
        if (xid < 0) continue;
        const Axis& a = d_->axes[xid];
        const bool isSource = source == this && pid == sourcePanel;
        const int target = axisKind(a);
        // Same-kind axes share x; lap axes match a time through the lap
        // started by then; a distance axis maps through its lap's progress.
        double key = time;
        bool mapped = true;
        if (sourceKind == target) key = sourceX;
        else if (target == DistanceAxis) {
            key = interpolate(a.sessionTimes, a.sessionKeys, time);
            mapped = !a.sessionTimes.isEmpty() && time >= a.sessionTimes.first() && time <= a.sessionTimes.last();
        } else if (target == LapAxis) key = lapAtSessionTime(a, time);
        // The source panel keeps the crosshairs and markers updateHover() set.
        if (!isSource) { p.cursorV = p.cursorH = false; p.dots = false; }
        if (!p.visible || !mapped || !std::isfinite(key) || key < a.lo || key > a.hi) continue;
        // The hovered panel samples like an unsynced hover; peers only inside
        // their own data range (Electron's coverage gate).
        // Without content (a reused tooltip) only the cheap coverage test runs.
        PanelTooltip tip;
        TooltipContent custom;
        const bool customRows = customSyncedRows(pid, key, custom);
        if (customRows) tip.any = !custom.isEmpty();
        else if (buildContent) tip = panelTooltip(pid, key, !isSource, sourceKind == target);
        else tip.any = panelHasValue(pid, key, !isSource, sourceKind == target);
        if (!tip.any) continue;
        if (!isSource) {
            p.cursorX = key; p.cursorV = d_->secondaryV;
            p.cursorY = yRatio; p.cursorH = d_->secondaryH;
            p.dotX = key; p.dots = true; p.dotsStrict = true;
        }
        if (!buildContent) continue;
        SyncedSample sample;
        sample.order = p.syncOrder >= 0 ? p.syncOrder : cursorSyncOrder(p.section);
        if (customRows) {
            sample.current = custom;
            out.push_back(sample);
            continue;
        }
        // Electron formatRow: the chart title, then that chart's value rows.
        TooltipContent title;
        if (!p.title.isEmpty()) {
            title << tooltipTextLine(p.title, muted);
            title.last().marginTop = 3;
        }
        sample.current = title + tip.current;
        if (!tip.comparison.isEmpty()) {
            sample.comparison = faded(title) + tip.comparison;
            sample.comparisonLabel = tip.syncLabel;
        }
        out.push_back(sample);
    }
    d_->canvas->update();
    return out;
}
void ChartView::clearSyncedCursor() {
    d_->hoverActive = false;
    d_->hoverCache.valid = false;
    if (d_->hoverTimer) d_->hoverTimer->stop();
    for (Panel& p : d_->panels) p.cursorV = p.cursorH = p.dots = false;
    if (d_->tooltip) d_->tooltip->hide();
    if (d_->canvas) d_->canvas->update();
}

bool ChartView::panelHasValue(int pid, double key, bool strictRange, bool allowEndpoint) const {
    for (const Series& s : d_->series) {
        if (s.panel != pid || s.spec.name.isEmpty() || !s.visible || s.empty()) continue;
        if (strictRange) {
            const double lo = s.firstKey(), hi = s.lastKey();
            if (key < lo || (key > hi && !allowEndpoint)) continue;
        }
        const qsizetype at = nearest(s, key);
        if (at >= 0 && std::isfinite(s.ys[size_t(at)])) return true;
    }
    return false;
}
bool ChartView::seriesKeyRange(int id,double&lo,double&hi)const{if(id<0||id>=d_->series.size()||d_->series[id].empty())return false;const Series&s=d_->series[id];lo=s.firstKey();hi=s.lastKey();return true;}
void ChartView::setXRange(int id,double lo,double hi){
    if(id<0||id>=d_->axes.size()||!std::isfinite(lo)||!std::isfinite(hi)||hi<=lo)return;
    // An unchanged range skips the label re-measure; a changed one only
    // relayouts when the widest y label actually changes width.
    auto apply=[&](int axisId){if(axisId<0||axisId>=d_->axes.size())return false;Axis&a=d_->axes[axisId];if(a.lo==lo&&a.hi==hi)return false;const int oldWidth=a.labelWidth;a.lo=lo;a.hi=hi;if(a.side!=Side::Bottom){if(measuredYAxisLabelWidth(a,TextWidths::of(chartLabelFont(font()),d_->overlay))!=oldWidth)d_->geometry(rect());}return true;};
    bool changed=apply(id);if(d_->linkedXAxes.contains(id))for(int linked:d_->linkedXAxes)if(linked!=id)changed=apply(linked)||changed;
    if(changed&&id==d_->navAxis)navigationRangeChanged();
}
void ChartView::setAxisRange(int id,double lo,double hi){setXRange(id,lo,hi);}

bool ChartView::visibleSeriesRange(const QVector<int>& ids, double& lo, double& hi) const {
    bool found = false;
    for (int sid : ids) {
        if (sid < 0 || sid >= d_->series.size()) continue;
        const Series& s = d_->series[sid];
        if (!s.visible || s.empty() || s.spec.xAxisId < 0 || s.spec.xAxisId >= d_->axes.size()) continue;
        const Axis& x = d_->axes[s.spec.xAxisId];
        qsizetype begin = lowerBound(s, x.lo);
        if (begin > s.first) --begin;
        const qsizetype end = qMin(lowerBound(s, x.hi) + 1, s.count());
        for (qsizetype i = begin; i < end; ++i) {
            const double value = s.ys[size_t(i)];
            if (!std::isfinite(value)) continue;
            if (!found) { lo = hi = value; found = true; }
            else { lo = qMin(lo, value); hi = qMax(hi, value); }
        }
    }
    return found;
}

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
        const qsizetype end = qMin(lowerBound(s, x.hi) + 1, s.count());
        if (!full) begin = qMax(begin, s.fitDirty);
        for (qsizetype i = begin; i < end; ++i) {
            const double value = s.ys[size_t(i)];
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

void ChartView::setPanelTooltipExtra(int id, std::function<QString(const QVector<double>&)> extra) {
    if (id >= 0 && id < d_->panels.size()) d_->panels[id].tooltipExtra = std::move(extra);
}

ChartView::PanelTooltip ChartView::panelTooltip(int pid, double key, bool strictRange,
                                                bool allowEndpoint) const {
    PanelTooltip out;
    if (pid < 0 || pid >= d_->panels.size()) return out;
    const Panel& panel = d_->panels[pid];
    const QColor muted = tooltipMutedColor();
    const auto sampleAt = [&](const Series& s) -> double {
        if (s.empty()) return qQNaN();
        if (strictRange) {
            const double lo = s.firstKey(), hi = s.lastKey();
            if (key < lo || (key > hi && !allowEndpoint)) return qQNaN();
        }
        const qsizetype at = nearest(s, key);
        return at < 0 ? qQNaN() : s.ys[size_t(at)];
    };
    // A missing value prints as an em dash, like Electron's NaN replacement.
    const auto valueText = [](const Series& s, double value) {
        QString text = numberText(value, 'f', s.spec.tipPrecision, s.spec.tipGroupThousands);
        if (!s.spec.unit.isEmpty())
            text += (s.spec.unit == "%" || !s.spec.unitSpace ? "" : " ") + s.spec.unit;
        return text;
    };

    TooltipContent rows, comparisonRows;
    QVector<double> values, comparisonValues;
    bool anyComparison = false;
    for (const Series& s : d_->series) {
        if (s.panel != pid || s.spec.name.isEmpty()) continue;
        const double value = sampleAt(s);
        values.push_back(value);
        double reference = qQNaN();
        if (s.linked >= 0 && s.linked < d_->series.size() && d_->series[s.linked].visible)
            reference = sampleAt(d_->series[s.linked]);
        comparisonValues.push_back(reference);
        if (!s.visible) continue;
        rows << tooltipValueLine(s.spec.name, s.spec.color, valueText(s, value));
        comparisonRows << tooltipValueLine(s.spec.name, s.spec.color, valueText(s, reference));
        out.any = out.any || std::isfinite(value);
        anyComparison = anyComparison || std::isfinite(reference);
    }
    if (!out.any) return out;
    // Electron tooltipDetails rows use the axis colour.
    const auto extra = [&](const QVector<double>& sample) {
        TooltipContent lines;
        const QString text = panel.tooltipExtra ? panel.tooltipExtra(sample) : QString();
        if (!text.isEmpty()) lines << tooltipTextLine(text, muted);
        return lines;
    };
    out.current = rows + extra(values);

    // Comparison-lap section + lap delta (Previous / Fastest / Selected windows).
    const SessionModel* model = d_->model;
    if (!model || panel.section == tnr::GraphSection::Count_) return out;
    const ChartWindow window = model->effectiveChartWindow(panel.section);
    if (!chartWindowIsComparison(window)) return out;
    if (anyComparison) {
        out.comparisonLabel = window == ChartWindow::PreviousLap ? QStringLiteral("Previous lap")
            : window == ChartWindow::FastestLap ? QStringLiteral("Fastest lap")
            : QStringLiteral("Reference lap");
        out.syncLabel = out.comparisonLabel;
        const int referenceLapNum = model->referenceLap(panel.section);
        if (window == ChartWindow::SelectedLap && referenceLapNum > 0)
            out.syncLabel += QStringLiteral(" %1").arg(referenceLapNum);
        out.comparison = faded(comparisonRows + extra(comparisonValues));
    }
    const Axis* axis = nullptr;
    for (const Axis& a : d_->axes) if (a.panel == pid && a.side == Side::Bottom) { axis = &a; break; }
    if (!axis || !axis->distance || axis->lapNum < 0 || axis->sessionKeys.size() < 2 ||
        key < axis->sessionKeys.first() || key > axis->sessionKeys.last()) return out;
    const LapBlock* reference = model->chartReferenceLap(
        window, model->referenceLap(panel.section), axis->lapStart + 0.001f);
    if (!reference || reference->progress.size() < 2 ||
        key < reference->progress.first().distanceM || key > reference->progress.last().distanceM)
        return out;
    const double delta = (interpolate(axis->sessionKeys, axis->sessionTimes, key) - axis->lapStart)
        - (model->data().timeAtDistance(reference, key) - reference->startSessionTime);
    if (!std::isfinite(delta)) return out;
    out.delta << tooltipDeltaLine(delta, QColor("#C4162A"), QColor("#37872D"));
    return out;
}

bool ChartView::customTooltip(int, double, TooltipContent&) const { return false; }
bool ChartView::customSyncedRows(int, double, TooltipContent&) const { return false; }

bool ChartView::hoverSample(int pid, double key, double& sampled) const {
    for (const Series& series : d_->series) {
        if (series.panel != pid || !series.visible || series.spec.name.isEmpty() || series.empty()) continue;
        const qsizetype at = nearest(series, key);
        if (!std::isfinite(series.ys[size_t(at)])) continue;
        sampled = series.key(at);
        return true;
    }
    return false;
}

double ChartView::seriesValueAt(int id, double key) const {
    if (id < 0 || id >= d_->series.size() || d_->series[id].empty()) return qQNaN();
    const Series& s = d_->series[id];
    const qsizetype at = nearest(s, key);
    return at < 0 ? qQNaN() : double(s.ys[size_t(at)]);
}

QColor ChartView::tooltipTextColor() const { return palette().color(QPalette::Text); }
QColor ChartView::tooltipMutedColor() const { return chartAxisTextColor(palette()); }

void ChartView::updateHover(const QPoint& position) {
    int pid = -1;
    for (int i = 0; i < d_->panels.size(); ++i)
        if (d_->panels[i].visible && d_->panels[i].plot.contains(position)) { pid = i; break; }
    for (Panel& panel : d_->panels) panel.cursorV = panel.cursorH = panel.dots = false;
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
    if (!hoverSample(pid, key, sampled)) {
        for (auto* chart : liveCharts()) chart->clearSyncedCursor();
        d_->hoverActive = true; // A later model update may supply a value here.
        return;
    }
    // Electron's crosshair plugin: both lines at the pointer, plus a marker
    // on every visible series at its sample nearest the pointer. A shared
    // cursor treats the visible panels as one stacked chart.
    for (int i = 0; i < d_->panels.size(); ++i) {
        Panel& p = d_->panels[i];
        if (i != pid && (!d_->sharedCursor || !p.visible || p.plot.isEmpty())) continue;
        p.cursorX = key; p.cursorV = true;
        p.dotX = key; p.dots = true; p.dotsStrict = false;
    }
    panel.cursorY = yRatio; panel.cursorH = true;

    TooltipContent content;
    // A synchronized chart builds its rows through customSyncedRows instead.
    if (!d_->sync && customTooltip(pid, key, content)) {
        if (content.isEmpty()) { if (d_->tooltip) d_->tooltip->hide(); }
        else showTooltip(content, position);
        d_->canvas->update();
        return;
    }
    const int sourceKind = axisKind(axis);
    const quint64 generation = chartContentGeneration();
    auto& cache = d_->hoverCache;
    const bool reuse = cache.valid && cache.panel == pid && cache.sampled == sampled &&
                       cache.sync == d_->sync && cache.generation == generation;
    if (reuse) {
        // Same snapped sample: move every crosshair, keep the tooltip text.
        if (d_->sync) {
            const double time = axisSessionTime(axis, sampled);
            for (auto* chart : liveCharts()) if (chart->isVisible())
                chart->showSyncedCursor(time, key, sourceKind, yRatio, this, pid, false);
        }
        if (cache.content.isEmpty()) { if (d_->tooltip) d_->tooltip->hide(); }
        else showTooltip(cache.content, position);
        d_->canvas->update();
        return;
    }
    const QColor muted = tooltipMutedColor();
    // Live charts format the header as m:ss (Electron fmtTime floors); lap
    // charts name the lap.
    const QString xText = axis.lap ? QStringLiteral("L%1").arg(qRound(sampled))
                        : axis.distance ? QString("%1 m").arg(qRound(sampled))
                                        : timeText(std::floor(sampled), 0);
    content << tooltipTextLine(xText, muted);
    content.last().marginBottom = 4;
    if (d_->sync) {
        // Electron chartCursorSync: every participant's rows by page order,
        // then the comparison fragments grouped under one heading per lap.
        const double time = axisSessionTime(axis, sampled);
        QVector<SyncedSample> samples;
        for (auto* chart : liveCharts()) if (chart->isVisible())
            samples += chart->showSyncedCursor(time, key, sourceKind, yRatio, this, pid);
        std::stable_sort(samples.begin(), samples.end(),
            [](const SyncedSample& a, const SyncedSample& b) { return a.order < b.order; });
        QStringList labels;
        QHash<QString, TooltipContent> groups;
        bool any = false;
        for (const SyncedSample& sample : samples) {
            content += sample.current;
            any = any || !sample.current.isEmpty();
            if (sample.comparison.isEmpty()) continue;
            if (!groups.contains(sample.comparisonLabel)) labels << sample.comparisonLabel;
            groups[sample.comparisonLabel] += sample.comparison;
        }
        for (const QString& label : labels)
            content += TooltipContent{tooltipSection(label, muted)} + groups.value(label);
        if (!any && labels.isEmpty()) content.clear();
    } else {
        const PanelTooltip tip = panelTooltip(pid, key, false, false);
        content += tip.current;
        if (!tip.comparison.isEmpty())
            content += TooltipContent{tooltipSection(tip.comparisonLabel, muted)} + tip.comparison;
        content += tip.delta;
    }
    cache.valid = true; cache.panel = pid; cache.sampled = sampled;
    cache.sync = d_->sync; cache.generation = generation; cache.content = content;
    if (content.isEmpty()) { if (d_->tooltip) d_->tooltip->hide(); }
    else showTooltip(content, position);
    d_->canvas->update();
}

// Electron useChartTooltip placement: 16 px from the pointer, on the side
// with more room in the window, clamped 4 px inside the window edges.
void ChartView::showTooltip(const TooltipContent& content, const QPoint& position) {
    if (!d_->tooltip) return;
    QWidget* host = window();
    if (d_->tooltip->parentWidget() != host) d_->tooltip->setParent(host, d_->tooltip->windowFlags());
    d_->tooltip->setContent(content);
    const QPoint anchor = mapTo(host, position);
    const QSize box = d_->tooltip->boxSize();
    const int w = box.width(), h = box.height();
    const int preferredLeft = anchor.x() <= host->width() / 2 ? anchor.x() + kTooltipGap : anchor.x() - kTooltipGap - w;
    const int preferredTop = anchor.y() <= host->height() / 2 ? anchor.y() + kTooltipGap : anchor.y() - kTooltipGap - h;
    const int left = qMax(kTooltipPad, qMin(preferredLeft, qMax(kTooltipPad, host->width() - w - kTooltipPad)));
    const int top = qMax(kTooltipPad, qMin(preferredTop, qMax(kTooltipPad, host->height() - h - kTooltipPad)));
    d_->tooltip->moveBoxTo(host->mapToGlobal(QPoint(left, top)));
    if (!d_->tooltip->isVisible()) d_->tooltip->show();
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
    ++chartContentGeneration();
    if (!isVisible() || !d_->canvas || !d_->overlay) return;
    d_->canvas->update(); d_->overlay->invalidateChanged();
    if (d_->hoverActive && !d_->dragging && d_->hoverTimer && !d_->hoverTimer->isActive()) d_->hoverTimer->start();
}
bool ChartView::event(QEvent* event) {
    const bool handled = QWidget::event(event);
    // The tooltip lives on the window; do not leave it behind on a page switch.
    if (event->type() == QEvent::Hide && d_) clearSyncedCursor();
    if (d_ && d_->overlay && d_->canvas &&
        (event->type() == QEvent::DevicePixelRatioChange || event->type() == QEvent::ScreenChangeInternal)) {
        d_->geometry(rect());
        positionPanelChartSettings();
        d_->overlay->invalidateAll();
        requestReplot();
    }
    return handled;
}
// Tooltip colours follow the Qt palette; the shape matches Electron.
void ChartView::applyPaletteText() {
    if (!d_->tooltip) return;
    const QPalette pal = palette();
    d_->tooltip->setTheme(font(), pal.color(QPalette::Window), pal.color(QPalette::Text), chartBorderColor(pal));
}
void ChartView::resizeEvent(QResizeEvent*e){QWidget::resizeEvent(e);d_->overlay->setGeometry(rect());d_->geometry(rect());d_->overlay->raise();positionPanelChartSettings();}
void ChartView::changeEvent(QEvent* e) {
    QWidget::changeEvent(e);
    if (e->type() == QEvent::FontChange || e->type() == QEvent::ApplicationFontChange ||
        e->type() == QEvent::LocaleChange) {
        applyPaletteText();
        d_->geometry(rect()); positionPanelChartSettings(); d_->overlay->invalidateAll(); requestReplot();
    }
    if (e->type() == QEvent::PaletteChange || e->type() == QEvent::ApplicationPaletteChange) {
        applyPaletteText(); d_->overlay->invalidateAll(); requestReplot();
    }
}

