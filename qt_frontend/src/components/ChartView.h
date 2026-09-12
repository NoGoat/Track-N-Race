#pragma once

#include <QWidget>
#include <QColor>
#include <QString>
#include <QStringList>
#include <QVector>
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
        int     precision    = 0;          // tick label decimal precision
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
        bool    fill = false;            // if true, fills under the curve to zero
        QColor  fillColor = QColor();    // if invalid, uses semi-transparent series color
        bool    step = false;            // if true, use lsStepLeft
    };

    struct BandSpec {
        int     axisId = -1;
        double  min = 0.0;
        double  max = 0.0;
        QColor  color;
    };

    explicit ChartView(QWidget* parent = nullptr);
    ~ChartView() override;

    // GPU render settings are read from QSettings and shared by every chart.
    // MSAA can be reapplied live; switching the process-wide RHI backend takes
    // effect on restart because Qt fixes one API per top-level window.
    static void reapplyRenderSettings();

    // Kept for the existing live style-change call site. QRhiWidget resources do
    // no longer needs a backend-specific pre-repolish teardown.
    static void suspendOpenGlForStyleChange();

    // Declare the chart. addAxis/addSeries return opaque ids used by the data
    // calls below. Series are drawn in creation order (later draws on top).
    // addAxis targets a panel (default panel 0 — see the multi-panel section).
    int  addAxis(const AxisSpec& spec, int panelId = 0);
    int  addSeries(const SeriesSpec& spec);
    void addBand(const BandSpec& spec);

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
    void bindPanelChartSettings(int panelId, SessionModel* model, tnr::GraphSection section);

    // Data.
    void appendPoint(int seriesId, double x, double y);
    void setSeriesData(int seriesId, const QVector<double>& xs, const QVector<double>& ys);
    void trimBefore(int seriesId, double x);   // drop samples with key < x
    void clear(int seriesId);
    void clearAll();

    // Hide a series (and its legend entry) — used to switch between single-lap
    // and two-lap overlay layouts without rebuilding the chart.
    void setSeriesVisible(int seriesId, bool visible);
    bool seriesVisible(int seriesId) const;
    void setSeriesColor(int seriesId, const QColor& color);
    void setSeriesName(int seriesId, const QString& name);
    void setSeriesWidth(int seriesId, double width);
    void setSeriesOrder(const QVector<int>& bottomToTop);
    void linkSeriesVisibility(int primarySeriesId, int linkedSeriesId);
    void setAxisVisible(int axisId, bool visible);
    void setAxisColor(int axisId, const QColor& color);
    void setAxisGridVisible(int axisId, bool visible);

    // Format an axis's tick labels as time (for example m:ss.t).
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

    // Current min/max x (key) of a series. Returns false if the series is empty.
    bool seriesKeyRange(int seriesId, double& lo, double& hi) const;

    // View. requestReplot() coalesces multiple updates in a frame into one paint.
    void setXRange(int axisId, double min, double max);
    void setAxisRange(int axisId, double min, double max);
    void fitAxisToVisibleSeries(int axisId, const QVector<int>& seriesIds,
                                double fixedMin, double fixedMax, bool dynamic,
                                bool expandFixedUpper = false);
    void setXNavigation(int axisId, bool enabled, double fullMin, double fullMax, double minSpan = 0.5);
    void zoomX(double factor);
    void panX(double fraction);
    void resetX();
    void requestReplot();

protected:
    void changeEvent(QEvent* e) override;   // keep label/legend colors in sync with the theme
    void resizeEvent(QResizeEvent* e) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void applyPaletteText();
    void applyPanelLayout();      // (re)place visible panels into the plot's layout grid
    void ensurePanelHeader(int panelId);   // build a panel's title+legend header row
    void refreshPanelChartSettings();
    void positionPanelChartSettings();
    QString showSyncedCursor(double sessionTime, double sourceAxisX,
                             bool sourceDistanceAxis, double yRatio,
                             ChartView* source, int sourcePanel);
    void clearSyncedCursor();

    struct Impl;
    std::unique_ptr<Impl> d_;
};
