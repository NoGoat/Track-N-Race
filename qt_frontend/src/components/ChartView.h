#pragma once

#include <QWidget>
#include <QColor>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>
#include <memory>

class SessionModel;
struct LapBlock;
namespace tnr { enum class GraphSection; }

// Generic, backend-agnostic time-series chart widget.
//
// Wraps the purpose-built QRhi telemetry renderer but exposes none of its
// types — the backend lives entirely behind a pimpl in ChartView.cpp. Charts
// are declared by config:
// add some axes, add some series bound to those axes, then push data. Like the
// Electron TimeChart path, every visible retained point is submitted to the
// renderer; ChartView does not perform per-pixel decimation.
class ChartView : public QWidget {
    Q_OBJECT

public:
    enum class Side { Bottom, Left, Right };
    enum class LineType { Line, Step, NativeLine, NativePoint };

    // Gap (px) left between panels — also the divider channel width. Exposed so a
    // caller laying tables out alongside the chart (see GraphTable's
    // layoutSectionGrid) can use the same spacing and have its cells line up with
    // the chart's internal panel grid.
    static constexpr int PanelGap = 12;

    struct AxisSpec {
        Side    side         = Side::Left;
        double  min          = 0.0;
        double  max          = 1.0;
        QColor  labelColor   = QColor();   // invalid → inherit theme text color
        bool    visible      = true;
        char    numberFormat = 'f';        // numeric format: 'f', 'g', or 'e'
        int     precision    = 0;          // minimum tick precision; extra digits distinguish fractional ticks
        bool    grid         = false;      // draw this axis's (faint) grid lines
        int     tickSpacePx  = 80;         // minimum horizontal space per X tick
    };

    struct SeriesSpec {
        QString name;
        QColor  color = QColor("#888888");
        double  width = 2.0;
        int     xAxisId = -1;            // axis id returned by addAxis()
        int     yAxisId = -1;
        QString unit;                    // appended after the value in the hover tooltip
        int     tipPrecision = 0;        // tooltip value decimals (e.g. 1 for "90.3%")
        bool    tipGroupThousands = false; // tooltip thousands separator (e.g. "11,580")
        bool    fill = false;
        QColor  fillColor = QColor();    // if invalid, uses semi-transparent series color
        bool    step = false;            // compatibility alias for LineType::Step
        double  stepLocation = 1.0;      // 0: start, 0.5: midpoint, 1: end of interval
        double  fillBaseline = 0.0;
        double  opacity = 1.0;           // multiplies line and fill alpha
        LineType lineType = LineType::Line;
        bool    unitSpace = true;        // "12 kW" vs Electron's unspaced "12kW" tooltip style
    };

    struct BandSpec {
        int     axisId = -1;
        double  min = 0.0;
        double  max = 0.0;
        QColor  color;
    };

    // Hover tooltip content, drawn by ChartView itself. Each line mirrors one
    // Electron tooltip <div>: coloured runs of text, CSS-style margins (adjacent
    // margins collapse), an optional 1 px separator above it (border-top plus
    // padding-top) and an opacity for the faded comparison-lap section. A run
    // with an invalid colour uses the tooltip text colour.
    struct TooltipRun {
        QString text; QColor color;
        bool operator==(const TooltipRun&) const = default;
    };
    struct TooltipLine {
        QVector<TooltipRun> runs;
        int pixelSize = 12;
        qreal opacity = 1.0;
        int marginTop = 0, marginBottom = 0;
        bool ruleAbove = false;
        int paddingTop = 0;
        bool operator==(const TooltipLine&) const = default;
    };
    using TooltipContent = QVector<TooltipLine>;
    // Electron's tooltip building blocks: a plain coloured line, a
    // "<coloured name>: value" row, and formatChartDeltaTooltip's delta row.
    static TooltipLine tooltipTextLine(const QString& text, const QColor& color);
    static TooltipLine tooltipValueLine(const QString& name, const QColor& nameColor, const QString& value);
    static TooltipLine tooltipDeltaLine(double deltaSeconds, const QColor& positive, const QColor& negative);

    struct CursorGuide {
        double x = 0.0;
        QColor color;
        bool operator==(const CursorGuide&) const = default;
    };

    explicit ChartView(QWidget* parent = nullptr);
    ~ChartView() override;

    // GPU render settings are read from QSettings and shared by every chart.
    // MSAA can be reapplied live; switching the process-wide RHI backend takes
    // effect on restart because Qt fixes one API per top-level window.
    static void reapplyRenderSettings();

    // Memory-log snapshot across every live chart: retained series samples,
    // CPU staging caches, and allocated QRhi buffer bytes (GUI thread only).
    static QJsonObject retentionDiagnostics();

    // Kept for the existing live style-change call site. QRhiWidget resources do
    // no longer needs a backend-specific pre-repolish teardown.
    static void suspendOpenGlForStyleChange();

    // Declare the chart. addAxis/addSeries return opaque ids used by the data
    // calls below. Series are drawn in creation order (later draws on top).
    // addAxis targets a panel (default panel 0 — see the multi-panel section).
    int  addAxis(const AxisSpec& spec, int panelId = 0);
    int  addSeries(const SeriesSpec& spec);
    void addBand(const BandSpec& spec);
    void addReferenceLine(int yAxisId, double value, bool dashed = true);

    // --- Multi-panel: several charts sharing one QRhi render target ---
    // A ChartView is a single panel (id 0) by default, and every existing chart
    // uses only that. addPanel() adds another axis rect to the same backend, so
    // N charts render in one QRhi render target / command pass instead of N widgets.
    // addAxis(spec, panelId) targets a panel; layoutPanels() arranges them in a
    // row-major grid; setPanelVisible() hides/shows one (reflowing the grid). The
    // per-panel title and colour-key legend live inside the plot.
    int  addPanel();
    void layoutPanels(int columns);
    // Arrange panels into explicit rows (each inner list is the panel ids placed
    // left-to-right in that row). A row with a single panel spans the full width,
    // so this supports asymmetric layouts (e.g. two panels over one wide one).
    // Panels not listed are hidden. Supersedes layoutPanels/setPanelVisible for
    // callers that manage their own arrangement.
    // A negative id reserves an empty (blank) cell in the grid, so a caller can
    // leave a hole where an external widget (e.g. a raw-values table) is overlaid.
    void layoutPanelsRows(const QVector<QVector<int>>& rows);
    void setPanelVisible(int panelId, bool on);
    void setPanelTitle(int panelId, const QString& title);
    void setPanelLegendVisible(int panelId, bool on);
    void setPanelNote(int panelId, const QString& note);   // muted text after the colour key
    void bindPanelChartSettings(int panelId, SessionModel* model, tnr::GraphSection section);
    // Extra tooltip row for a panel (e.g. Electron's "Total: 412.3 kW"), as
    // plain text drawn in the muted axis colour; empty adds nothing. The
    // callback receives the value of every named series in the panel, in
    // creation order (NaN where a series has no sample at the cursor). It is
    // also applied to the comparison-lap section when one is shown.
    void setPanelTooltipExtra(int panelId, std::function<QString(const QVector<double>&)> extra);

    // Data.
    void appendPoint(int seriesId, double x, double y);
    void setSeriesData(int seriesId, const QVector<double>& xs, const QVector<double>& ys);
    void trimBefore(int seriesId, double x);   // retain one predecessor for edge clipping
    void clear(int seriesId);
    void clearAll();

    // Hide a series (and its legend entry) — used to switch between single-lap
    // and two-lap overlay layouts without rebuilding the chart.
    void setSeriesVisible(int seriesId, bool visible);
    bool seriesVisible(int seriesId) const;
    void setSeriesColor(int seriesId, const QColor& color);
    void setSeriesName(int seriesId, const QString& name);
    void setSeriesWidth(int seriesId, double width);
    void setSeriesLineType(int seriesId, LineType type);
    void setSeriesStepLocation(int seriesId, double location);
    void setSeriesFillBaseline(int seriesId, double baseline);
    void setSeriesOpacity(int seriesId, double opacity);
    // Timecharts uses native lines for continuous series in accumulated views.
    // Step and point series retain their configured geometry.
    void setAxisNativeLines(int xAxisId, bool enabled);
    void setSeriesOrder(const QVector<int>& bottomToTop);
    void linkSeriesVisibility(int primarySeriesId, int linkedSeriesId);
    void setAxisVisible(int axisId, bool visible);
    // Moves an axis to another side; laneOrder ranks it among that side's axes
    // (lower sits nearer the plot).
    void setAxisSide(int axisId, Side side, int laneOrder = 0);
    // Panels sharing a column use the column's widest left/right gutters, so
    // stacked plots line up edge to edge. Off by default.
    void setPanelInsetsAligned(bool on);
    void setAxisColor(int axisId, const QColor& color);
    void setAxisGridVisible(int axisId, bool visible);
    // Electron's per-chart axis look: dashed grid lines (Tyre trends use 3/3)
    // and x tick marks below the plot (0 = none, the TimeChart default).
    void setAxisGridStyle(int axisId, bool dashed, int tickMarkPx = 0);

    // Format duration ticks using %h, %m, %s and optional %z (milliseconds).
    // Subsecond zoom adds fractional seconds when needed to distinguish ticks.
    void setAxisTimeTicker(int axisId, const QString& format);
    void setAxisDistanceMode(int axisId, bool distance);
    void syncAxisSessionMap(int axisId, const LapBlock* lap, float currentTime);
    void setAxisLabelMap(int axisId, const QVector<double>& ticks, const QStringList& labels,
                         bool lapBoundaryLabels = false);

    // Format an axis's tick labels as value/scale + suffix, e.g. (1000,"k") turns
    // 16000 into "16k", or (1,"%") turns 80 into "80%". A positive fixedStep forces
    // evenly spaced ticks at that interval (in raw value units, e.g. 2000 for RPM).
    void setAxisNumberSuffix(int axisId, double scale, const QString& suffix, double fixedStep = 0.0);

    // Show/hide the built-in overlay legend (off when an external legend is used).
    void setLegendVisible(bool on);

    // Enable a crosshair + value readout that tracks the cursor across the chart,
    // showing the nearest value of each visible series (and the x as m:ss.s).
    void setHoverReadout(bool on);
    void setCursorSync(bool enabled, bool secondaryVertical, bool secondaryHorizontal);
    void setCursorModeKey(const QString& key);
    // Treat every visible panel as one chart for hovering, like Electron's
    // stacked Analyze chart: one vertical crosshair through all panels and
    // hover markers on every panel's series.
    void setPanelsShareCursor(bool on);
    // Persistent, non-interactive cursors drawn only by the lightweight raster
    // overlay. Updating these never rebuilds or resubmits the GPU traces.
    void setCursorGuides(const QVector<CursorGuide>& guides);

    // Current min/max x (key) of a series. Returns false if the series is empty.
    bool seriesKeyRange(int seriesId, double& lo, double& hi) const;

    // View. requestReplot() coalesces multiple updates in a frame into one paint.
    void setXRange(int axisId, double min, double max);
    void setAxisRange(int axisId, double min, double max);
    void fitAxisToVisibleSeries(int axisId, const QVector<int>& seriesIds,
                                double fixedMin, double fixedMax, bool dynamic,
                                bool expandFixedUpper = false);
    // Min/max of the visible series' samples inside each one's x-axis window.
    // Returns false when none of them has a finite sample there.
    bool visibleSeriesRange(const QVector<int>& seriesIds, double& lo, double& hi) const;
    void setXNavigation(int axisId, bool enabled, double fullMin, double fullMax, double minSpan = 0.5);
    void setLinkedXAxes(const QVector<int>& axisIds);
    void zoomX(double factor);
    void panX(double fraction);
    void resetX();
    void requestReplot();

signals:
    void inspectionRequested(double x, bool distanceCoordinate);

protected:
    // Replace the default tooltip for a hover over panelId at x = key. Return
    // true to use `out` (empty hides the tooltip); false keeps the default.
    virtual bool customTooltip(int panelId, double key, TooltipContent& out) const;
    // The navigation x axis (setXNavigation) changed range: zoom, pan or reset.
    virtual void navigationRangeChanged() {}
    // Value of the sample nearest x = key (NaN when the series is empty).
    double seriesValueAt(int seriesId, double key) const;
    QColor tooltipTextColor() const;
    QColor tooltipMutedColor() const;

    bool event(QEvent* e) override;
    void changeEvent(QEvent* e) override;   // keep label/legend colors in sync with the theme
    void resizeEvent(QResizeEvent* e) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void applyPaletteText();
    void applyPanelLayout();      // (re)place visible panels into the plot's layout grid
    void ensurePanelHeader(int panelId);   // build a panel's title+legend header row
    void refreshPanelChartSettings();
    void positionPanelChartSettings();
    struct PanelTooltip;
    struct SyncedSample;
    // Cursor sync participant (Electron chartCursorSync): moves this chart's
    // crosshairs/markers to the synced position and returns one tooltip
    // fragment per panel that has data there, the source panel included.
    // With buildContent false only the crosshairs/markers move (the source
    // reuses its tooltip for an unchanged snapped sample).
    QVector<SyncedSample> showSyncedCursor(double sessionTime, double sourceAxisX,
                                           bool sourceDistanceAxis, double yRatio,
                                           ChartView* source, int sourcePanel,
                                           bool buildContent = true);
    // Whether any visible named series in the panel has a value at x = key.
    bool panelHasValue(int panelId, double key, bool strictRange, bool allowEndpoint) const;
    void clearSyncedCursor();
    void updateHover(const QPoint& position);
    void showTooltip(const TooltipContent& content, const QPoint& position);
    // One panel's tooltip parts at x = key: the series rows plus extra rows,
    // and — in a Previous/Fastest/Selected lap window — the comparison-lap rows
    // and the lap delta, matching Electron's chart tooltips. strictRange limits
    // sampling to each series' own x range (synced peer panels); allowEndpoint
    // also accepts keys just past a series' last sample.
    PanelTooltip panelTooltip(int panelId, double key, bool strictRange,
                              bool allowEndpoint) const;

    struct Impl;
    std::unique_ptr<Impl> d_;
};
