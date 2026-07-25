using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using ScottPlot;
using ScottPlot.Plottables;
using ScottPlot.WinUI;
using System.Diagnostics;

namespace TrackNRace.WinUI3;

internal enum TyreChartKind
{
    Surface,
    Inner,
    Brake,
    Wear,
}

public sealed partial class TyreChartControl : UserControl
{
    private static readonly LinePattern HoverCrosshairPattern =
        new([2f, 1f], 0, "TyreCrosshair");

    private readonly List<double> _times = [];
    private readonly List<double> _fl = [];
    private readonly List<double> _fr = [];
    private readonly List<double> _rl = [];
    private readonly List<double> _rr = [];
    private readonly List<double> _rawFl = [];
    private readonly List<double> _rawFr = [];
    private readonly List<double> _rawRl = [];
    private readonly List<double> _rawRr = [];
    private readonly SignalXY[] _series = new SignalXY[4];
    private readonly Marker[] _markers = new Marker[4];
    private Crosshair? _crosshair;
    private HorizontalLine? _bottomBoundary;
    private TyreChartKind _kind;
    private TyreWearDisplayMode _wearMode;
    private int _windowSeconds = 30;
    private bool _configured;
    private double _observedMaximum;
    private double _configuredYMinimum = double.NaN;
    private double _configuredYMaximum = double.NaN;

    public TyreChartControl()
    {
        InitializeComponent();
    }

    internal void Configure(
        TyreChartKind kind,
        TyreWearDisplayMode wearMode,
        int windowSeconds)
    {
        if (_configured)
        {
            return;
        }

        _configured = true;
        _kind = kind;
        _wearMode = wearMode;
        _windowSeconds = windowSeconds;
        Heading.Text = kind switch
        {
            TyreChartKind.Surface => "SURFACE TEMP",
            TyreChartKind.Inner => "INNER TEMP",
            TyreChartKind.Brake => "BRAKE TEMP",
            _ => wearMode == TyreWearDisplayMode.Life ? "TYRE LIFE" : "TYRE WEAR",
        };

        var plot = PlotControl.Plot;
        PlotControl.UserInputProcessor.Disable();
        _series[0] = plot.Add.SignalXY(_times, _fl);
        _series[1] = plot.Add.SignalXY(_times, _fr);
        _series[2] = plot.Add.SignalXY(_times, _rl);
        _series[3] = plot.Add.SignalXY(_times, _rr);
        foreach (var series in _series)
        {
            series.LineWidth = 2;
            series.MarkerSize = 0;
            series.ConnectStyle = ConnectStyle.Straight;
        }

        _crosshair = plot.Add.Crosshair(0, 0);
        _crosshair.EnableAutoscale = false;
        _crosshair.IsVisible = false;
        _crosshair.LineWidth = 1;
        _crosshair.LinePattern = HoverCrosshairPattern;
        _crosshair.MarkerSize = 0;

        for (var index = 0; index < _markers.Length; index++)
        {
            _markers[index] = plot.Add.Marker(
                0, 0, MarkerShape.OpenCircle, 7, ScottPlot.Colors.Transparent);
            _markers[index].MarkerLineWidth = 2;
            _markers[index].IsVisible = false;
        }

        var (minimum, maximum) = BaseRange();
        const double insetFraction = .0025;
        _bottomBoundary = plot.Add.HorizontalLine(
            minimum + (maximum - minimum) * insetFraction);
        _bottomBoundary.EnableAutoscale = false;
        _bottomBoundary.LineWidth = .75f;

        plot.Axes.SetLimits(0, windowSeconds, minimum, maximum);
        ConfigureYAxis(minimum, maximum);
        _configuredYMinimum = minimum;
        _configuredYMaximum = maximum;
        ConfigureTimeTicks();
        plot.Axes.Top.IsVisible = false;
        plot.Axes.Right.IsVisible = false;
        plot.Axes.Margins(0, 0, 0, 0);
        plot.Grid.IsVisible = true;
        plot.Grid.YAxisStyle.IsVisible = false;
        plot.Grid.MajorLineWidth = 1;
        plot.Grid.MinorLineWidth = 0;
        StyleAxis(plot.Axes.Left);
        StyleAxis(plot.Axes.Bottom);
#if DEBUG
        AttachRenderProbe();
#endif
    }

    internal void AppendTelemetry(IEnumerable<TelemetrySample> samples)
    {
        if (_kind == TyreChartKind.Wear)
        {
            return;
        }
        foreach (var sample in samples)
        {
            _times.Add(sample.SessionTime);
            switch (_kind)
            {
                case TyreChartKind.Surface:
                    Add(sample.TyreTempSurfaceFl, sample.TyreTempSurfaceFr,
                        sample.TyreTempSurfaceRl, sample.TyreTempSurfaceRr);
                    break;
                case TyreChartKind.Inner:
                    Add(sample.TyreTempInnerFl, sample.TyreTempInnerFr,
                        sample.TyreTempInnerRl, sample.TyreTempInnerRr);
                    break;
                case TyreChartKind.Brake:
                    Add(sample.BrakeTempFl, sample.BrakeTempFr,
                        sample.BrakeTempRl, sample.BrakeTempRr);
                    break;
            }
        }
    }

    internal void AppendDamage(IEnumerable<DamageRowData> rows)
    {
        if (_kind != TyreChartKind.Wear)
        {
            return;
        }
        foreach (var row in rows)
        {
            _times.Add(row.SessionTime);
            _rawFl.Add(row.TyreWearFl);
            _rawFr.Add(row.TyreWearFr);
            _rawRl.Add(row.TyreWearRl);
            _rawRr.Add(row.TyreWearRr);
            Add(
                DisplayWear(row.TyreWearFl),
                DisplayWear(row.TyreWearFr),
                DisplayWear(row.TyreWearRl),
                DisplayWear(row.TyreWearRr));
        }
    }

    internal void SetWearMode(TyreWearDisplayMode mode)
    {
        if (_kind != TyreChartKind.Wear || _wearMode == mode)
        {
            return;
        }
        _wearMode = mode;
        Heading.Text = mode == TyreWearDisplayMode.Life ? "TYRE LIFE" : "TYRE WEAR";
        for (var index = 0; index < _rawFl.Count; index++)
        {
            _fl[index] = DisplayWear(_rawFl[index]);
            _fr[index] = DisplayWear(_rawFr[index]);
            _rl[index] = DisplayWear(_rawRl[index]);
            _rr[index] = DisplayWear(_rawRr[index]);
        }
    }

    internal void SetWindowSeconds(int seconds)
    {
        _windowSeconds = seconds;
        ConfigureTimeTicks();
    }

    internal void ClearData()
    {
        _times.Clear();
        _fl.Clear();
        _fr.Clear();
        _rl.Clear();
        _rr.Clear();
        _rawFl.Clear();
        _rawFr.Clear();
        _rawRl.Clear();
        _rawRr.Clear();
        _observedMaximum = 0;
        _configuredYMinimum = double.NaN;
        _configuredYMaximum = double.NaN;
        HideHover(false);
    }

    internal void RefreshChart(double latestSessionTime)
    {
        var hasData = _times.Count > 1;
        NoData.Visibility = hasData ? Visibility.Collapsed : Visibility.Visible;
        var latest = Math.Max(
            latestSessionTime,
            _times.Count > 0 ? _times[^1] : 0);
        var xMin = Math.Max(0, latest - _windowSeconds);
        var xMax = Math.Max(_windowSeconds, latest);
        var (yMin, baseMaximum) = BaseRange();
        var yMax = _kind switch
        {
            TyreChartKind.Surface or TyreChartKind.Inner =>
                ExpandedMaximum(baseMaximum, 25),
            TyreChartKind.Brake => ExpandedMaximum(baseMaximum, 250),
            _ => baseMaximum,
        };
        PlotControl.Plot.Axes.SetLimitsX(xMin, xMax);
        if (Math.Abs(_configuredYMinimum - yMin) > .001 ||
            Math.Abs(_configuredYMaximum - yMax) > .001)
        {
            PlotControl.Plot.Axes.SetLimitsY(yMin, yMax);
            ConfigureYAxis(yMin, yMax);
            _configuredYMinimum = yMin;
            _configuredYMaximum = yMax;
        }
        PlotControl.Refresh();
    }

    internal void ApplyTheme(bool dark)
    {
        var colorValues = dark
            ? new[] { "#E10600", "#4488FF", "#37872D", "#FFD700" }
            : new[] { "#E10600", "#0B57D0", "#137333", "#B38F00" };
        var legend = new[] { FlLegend, FrLegend, RlLegend, RrLegend };
        var labels = new[] { TooltipFlLabel, TooltipFrLabel, TooltipRlLabel, TooltipRrLabel };
        for (var index = 0; index < 4; index++)
        {
            var color = ScottPlot.Color.FromHex(colorValues[index]);
            _series[index].Color = color;
            _markers[index].Color = color;
            var brush = new SolidColorBrush(ParseColor(colorValues[index]));
            legend[index].Fill = brush;
            labels[index].Foreground = brush;
        }

        var transparent = ScottPlot.Color.FromHex("#00000000");
        var axes = ScottPlot.Color.FromHex(dark ? "#7C8098" : "#6B7280");
        var grid = ScottPlot.Color.FromHex(dark ? "#FFFFFF" : "#000000")
            .WithAlpha(dark ? .05 : .045);
        PlotControl.Plot.FigureBackground.Color = transparent;
        PlotControl.Plot.DataBackground.Color = transparent;
        PlotControl.Plot.Axes.Color(axes);
        PlotControl.Plot.Grid.MajorLineColor = grid;
        if (_bottomBoundary is not null)
        {
            _bottomBoundary.Color = axes;
        }
        if (_crosshair is not null)
        {
            _crosshair.LineColor = axes.WithAlpha(dark ? .7 : .6);
        }
    }

    private void Add(double fl, double fr, double rl, double rr)
    {
        _fl.Add(fl);
        _fr.Add(fr);
        _rl.Add(rl);
        _rr.Add(rr);
        _observedMaximum = Math.Max(
            _observedMaximum,
            Math.Max(Math.Max(fl, fr), Math.Max(rl, rr)));
    }

    private double DisplayWear(double wear) =>
        _wearMode == TyreWearDisplayMode.Life ? 100 - wear : wear;

    private (double Minimum, double Maximum) BaseRange() => _kind switch
    {
        TyreChartKind.Surface or TyreChartKind.Inner => (0, 125),
        TyreChartKind.Brake => (0, 1250),
        _ => (0, 100),
    };

    private double ExpandedMaximum(double baseMaximum, double increment)
    {
        return _observedMaximum <= baseMaximum
            ? baseMaximum
            : Math.Ceiling(_observedMaximum / increment) * increment;
    }

    private void ConfigureYAxis(double minimum, double maximum)
    {
        var interval = _kind switch
        {
            TyreChartKind.Brake => 250,
            TyreChartKind.Wear => 20,
            _ => 25,
        };
        var ticks = new List<double>();
        for (var value = minimum; value <= maximum; value += interval)
        {
            ticks.Add(value);
        }
        var suffix = _kind == TyreChartKind.Wear ? "%" : "°C";
        PlotControl.Plot.Axes.Left.SetTicks(
            ticks.ToArray(),
            ticks.Select(value => $"{value:0}{suffix}").ToArray());
    }

    private void ConfigureTimeTicks()
    {
        PlotControl.Plot.Axes.Bottom.TickGenerator =
            new ScottPlot.TickGenerators.NumericFixedInterval(
                _windowSeconds / 6d)
            {
                LabelFormatter = FormatTime,
            };
    }

    private static void StyleAxis(IAxis axis)
    {
        axis.FrameLineStyle.Width = .75f;
        axis.MajorTickStyle.Length = 3;
        axis.MajorTickStyle.Width = .75f;
        axis.MinorTickStyle.Length = 0;
        axis.TickLabelStyle.FontName = "Segoe UI Variable Text";
        axis.TickLabelStyle.FontSize = 12;
        axis.TickLabelStyle.Bold = false;
    }

    private void OnPointerMoved(object sender, PointerRoutedEventArgs args)
    {
        if (_times.Count == 0)
        {
            return;
        }
        var position = args.GetCurrentPoint(PlotControl).Position;
        var scale = XamlRoot?.RasterizationScale ?? 1;
        var coordinates = PlotControl.Plot.GetCoordinates(
            new Pixel(
                (float)(position.X * scale),
                (float)(position.Y * scale)));
        var index = _times.BinarySearch(coordinates.X);
        if (index < 0)
        {
            index = ~index;
            if (index >= _times.Count)
            {
                index = _times.Count - 1;
            }
            else if (index > 0 &&
                Math.Abs(_times[index - 1] - coordinates.X) <
                Math.Abs(_times[index] - coordinates.X))
            {
                index--;
            }
        }

        TooltipTime.Text = FormatTime(_times[index]);
        TooltipFlValue.Text = $" {FormatValue(_fl[index])}";
        TooltipFrValue.Text = $" {FormatValue(_fr[index])}";
        TooltipRlValue.Text = $" {FormatValue(_rl[index])}";
        TooltipRrValue.Text = $" {FormatValue(_rr[index])}";
        Tooltip.Visibility = Visibility.Visible;
        var left = Math.Clamp(
            position.X + 12,
            4,
            Math.Max(4, PlotControl.ActualWidth - Tooltip.ActualWidth - 8));
        var top = Math.Clamp(
            position.Y + 12,
            4,
            Math.Max(4, PlotControl.ActualHeight - Tooltip.ActualHeight - 8));
        Canvas.SetLeft(Tooltip, left);
        Canvas.SetTop(Tooltip, top);

        if (_crosshair is not null)
        {
            _crosshair.Position = coordinates;
            _crosshair.IsVisible = true;
        }
        var values = new[] { _fl[index], _fr[index], _rl[index], _rr[index] };
        for (var markerIndex = 0; markerIndex < 4; markerIndex++)
        {
            _markers[markerIndex].Position =
                new Coordinates(_times[index], values[markerIndex]);
            _markers[markerIndex].IsVisible = true;
        }
        PlotControl.Refresh();
    }

    private void OnPointerExited(object sender, PointerRoutedEventArgs args) =>
        HideHover(true);

    private void HideHover(bool refresh)
    {
        Tooltip.Visibility = Visibility.Collapsed;
        if (_crosshair is not null)
        {
            _crosshair.IsVisible = false;
        }
        foreach (var marker in _markers)
        {
            if (marker is not null)
            {
                marker.IsVisible = false;
            }
        }
        if (refresh)
        {
            PlotControl.Refresh();
        }
    }

    private string FormatValue(double value) =>
        _kind == TyreChartKind.Wear ? $"{value:0.0}%" : $"{value:0}°C";

    private static string FormatTime(double seconds) =>
        $"{(int)(seconds / 60)}:{(int)(seconds % 60):00}";

    private static Windows.UI.Color ParseColor(string value)
    {
        var packed = Convert.ToUInt32(value.TrimStart('#'), 16);
        return Windows.UI.Color.FromArgb(
            255,
            (byte)(packed >> 16),
            (byte)(packed >> 8),
            (byte)packed);
    }

#if DEBUG
    private void AttachRenderProbe()
    {
        var sampleCount = 0;
        var totalMilliseconds = 0d;
        var maximumMilliseconds = 0d;
        PlotControl.Plot.RenderManager.RenderFinished += (_, details) =>
        {
            if (details.Count <= 1)
            {
                return;
            }
            var milliseconds = details.Elapsed.TotalMilliseconds;
            sampleCount++;
            totalMilliseconds += milliseconds;
            maximumMilliseconds = Math.Max(maximumMilliseconds, milliseconds);
            if (sampleCount < 120)
            {
                return;
            }
            Debug.WriteLine(
                $"[ScottPlot] Tyres/{_kind}: avg " +
                $"{totalMilliseconds / sampleCount:0.00} ms, " +
                $"max {maximumMilliseconds:0.00} ms, " +
                $"{details.Count:N0} total renders");
            sampleCount = 0;
            totalMilliseconds = 0;
            maximumMilliseconds = 0;
        };
    }
#endif
}
