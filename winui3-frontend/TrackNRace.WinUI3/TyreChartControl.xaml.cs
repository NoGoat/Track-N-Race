using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using System.Diagnostics;
using TrackNRace.Charting;
using UiColor = Windows.UI.Color;

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
    private const int MaxChartPoints = 750_000;
    private const double RetentionSeconds = 600;
    private static readonly string[] SeriesKeys = ["fl", "fr", "rl", "rr"];
    private static readonly string[] SeriesLabels = ["FL", "FR", "RL", "RR"];

    private readonly List<double> _times = [];
    private readonly List<double> _fl = [];
    private readonly List<double> _fr = [];
    private readonly List<double> _rl = [];
    private readonly List<double> _rr = [];
    private readonly List<double> _rawFl = [];
    private readonly List<double> _rawFr = [];
    private readonly List<double> _rawRl = [];
    private readonly List<double> _rawRr = [];
    private readonly ChartLineSeries[] _series = new ChartLineSeries[4];
    private readonly UiColor[] _seriesColors = new UiColor[4];
    private ChartAxis _timeAxis = null!;
    private ChartAxis _valueAxis = null!;
    private ChartCrosshairTooltipPlugin _tooltip = null!;
    private TyreChartKind _kind;
    private TyreWearDisplayMode _wearMode;
    private int _windowSeconds = 30;
    private bool _configured;
    private double _observedMaximum;
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

        _timeAxis = new ChartAxis(
            "time", ChartAxisOrientation.X, ChartAxisSide.Bottom)
        {
            Minimum = 0,
            Maximum = windowSeconds,
            TickProvider = new IntervalChartTickProvider(windowSeconds / 6d),
            LabelFormatter = FormatTime,
            ShowGridLines = true,
        };
        var (minimum, maximum) = BaseRange();
        _valueAxis = new ChartAxis(
            "value", ChartAxisOrientation.Y, ChartAxisSide.Left)
        {
            Minimum = minimum,
            Maximum = maximum,
            TickProvider = new IntervalChartTickProvider(YInterval()),
            LabelFormatter = FormatAxisValue,
        };
        _configuredYMaximum = maximum;

        PlotControl.Plugins.Add(new ChartBackgroundPlugin());
        PlotControl.Axes.Add(_timeAxis);
        PlotControl.Axes.Add(_valueAxis);
        PlotControl.RenderFailed += error =>
            Debug.WriteLine($"[D3D11Chart] Tyres/{_kind}: {error}");

        for (var index = 0; index < _series.Length; index++)
        {
            _series[index] = PlotControl.Series.Add(new ChartLineSeriesOptions(
                SeriesKeys[index],
                "time",
                "value",
                UiColor.FromArgb(255, 255, 255, 255),
                Thickness: 2,
                MaximumPointCount: MaxChartPoints,
                MaximumXSpan: RetentionSeconds));
        }

        _tooltip = new ChartCrosshairTooltipPlugin(BuildTooltip);
        PlotControl.Plugins.Add(_tooltip);
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

        var firstNewIndex = _times.Count;
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
        AppendSeries(firstNewIndex);
    }

    internal void AppendDamage(IEnumerable<DamageRowData> rows)
    {
        if (_kind != TyreChartKind.Wear)
        {
            return;
        }

        var firstNewIndex = _times.Count;
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
        AppendSeries(firstNewIndex);
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
        ReplaceSeries();
    }

    internal void SetWindowSeconds(int seconds)
    {
        _windowSeconds = seconds;
        _timeAxis.TickProvider = new IntervalChartTickProvider(seconds / 6d);
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
        foreach (var series in _series)
        {
            series.Clear();
        }
        _observedMaximum = 0;
        var (_, maximum) = BaseRange();
        _valueAxis.Maximum = maximum;
        _configuredYMaximum = maximum;
        NoData.Visibility = Visibility.Visible;
    }

    internal void RefreshChart(double latestSessionTime)
    {
        NoData.Visibility =
            _times.Count > 1 ? Visibility.Collapsed : Visibility.Visible;
        var latest = Math.Max(
            latestSessionTime,
            _times.Count > 0 ? _times[^1] : 0);
        _timeAxis.Minimum = Math.Max(0, latest - _windowSeconds);
        _timeAxis.Maximum = Math.Max(_windowSeconds, latest);

        var (_, baseMaximum) = BaseRange();
        var yMaximum = _kind switch
        {
            TyreChartKind.Surface or TyreChartKind.Inner =>
                ExpandedMaximum(baseMaximum, 25),
            TyreChartKind.Brake => ExpandedMaximum(baseMaximum, 250),
            _ => baseMaximum,
        };
        if (Math.Abs(_configuredYMaximum - yMaximum) > .001)
        {
            _valueAxis.Maximum = yMaximum;
            _configuredYMaximum = yMaximum;
        }
        PlotControl.Invalidate();
    }

    internal void ApplyTheme(bool dark)
    {
        var colorValues = dark
            ? new[] { "#E10600", "#4488FF", "#37872D", "#FFD700" }
            : new[] { "#E10600", "#0B57D0", "#137333", "#B38F00" };
        var legend = new[] { FlLegend, FrLegend, RlLegend, RrLegend };
        for (var index = 0; index < _series.Length; index++)
        {
            var color = ParseColor(colorValues[index]);
            _seriesColors[index] = color;
            _series[index].Stroke = color;
            legend[index].Fill = new SolidColorBrush(color);
        }

        var axes = dark
            ? UiColor.FromArgb(255, 124, 128, 152)
            : UiColor.FromArgb(255, 107, 114, 128);
        _timeAxis.Color = axes;
        _valueAxis.Color = axes;
        PlotControl.GridColor = dark
            ? UiColor.FromArgb(13, 255, 255, 255)
            : UiColor.FromArgb(11, 0, 0, 0);
        _tooltip.ApplyTheme(dark);
        PlotControl.Invalidate();
    }

    private void AppendSeries(int firstNewIndex)
    {
        var count = _times.Count - firstNewIndex;
        if (count <= 0)
        {
            return;
        }
        var values = new[] { _fl, _fr, _rl, _rr };
        for (var seriesIndex = 0; seriesIndex < _series.Length; seriesIndex++)
        {
            var points = new ChartPoint[count];
            for (var offset = 0; offset < count; offset++)
            {
                var index = firstNewIndex + offset;
                points[offset] = new ChartPoint(
                    _times[index], values[seriesIndex][index]);
            }
            _series[seriesIndex].Append(points);
        }
    }

    private void ReplaceSeries()
    {
        var values = new[] { _fl, _fr, _rl, _rr };
        for (var seriesIndex = 0; seriesIndex < _series.Length; seriesIndex++)
        {
            var points = new ChartPoint[_times.Count];
            for (var index = 0; index < _times.Count; index++)
            {
                points[index] = new ChartPoint(
                    _times[index], values[seriesIndex][index]);
            }
            _series[seriesIndex].Replace(points);
        }
    }

    private ChartTooltipData? BuildTooltip(double sessionTime)
    {
        var index = NearestIndex(sessionTime);
        if (index < 0)
        {
            return null;
        }

        var values = new[] { _fl[index], _fr[index], _rl[index], _rr[index] };
        return new ChartTooltipData(
            _times[index],
            new ChartTooltipContent(
                FormatTime(_times[index]),
                SeriesLabels.Select((label, seriesIndex) =>
                    new ChartTooltipEntry(
                        label,
                        FormatValue(values[seriesIndex]),
                        _seriesColors[seriesIndex]))
                    .ToArray()),
            SeriesKeys.Select((_, seriesIndex) =>
                new ChartTooltipMarker(
                    _times[index],
                    values[seriesIndex],
                    "value",
                    _seriesColors[seriesIndex]))
                .ToArray());
    }

    private int NearestIndex(double sessionTime)
    {
        if (_times.Count == 0)
        {
            return -1;
        }
        var candidate = _times.BinarySearch(sessionTime);
        if (candidate < 0)
        {
            candidate = ~candidate;
        }
        if (candidate >= _times.Count)
        {
            return _times.Count - 1;
        }
        if (candidate > 0 &&
            sessionTime - _times[candidate - 1] <=
            _times[candidate] - sessionTime)
        {
            candidate--;
        }
        return candidate;
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

    private double YInterval() => _kind switch
    {
        TyreChartKind.Brake => 250,
        TyreChartKind.Wear => 20,
        _ => 25,
    };

    private double ExpandedMaximum(double baseMaximum, double increment) =>
        _observedMaximum <= baseMaximum
            ? baseMaximum
            : Math.Ceiling(_observedMaximum / increment) * increment;

    private string FormatAxisValue(double value) =>
        _kind == TyreChartKind.Wear ? $"{value:0}%" : $"{value:0}°C";

    private string FormatValue(double value) =>
        _kind == TyreChartKind.Wear ? $"{value:0.0}%" : $"{value:0}°C";

    private static string FormatTime(double seconds)
    {
        var safe = Math.Max(0, seconds);
        return $"{(int)(safe / 60)}:{(int)(safe % 60):00}";
    }

    private static UiColor ParseColor(string value)
    {
        var packed = Convert.ToUInt32(value.TrimStart('#'), 16);
        return UiColor.FromArgb(
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
        PlotControl.DiagnosticsUpdated += details =>
        {
            sampleCount++;
            totalMilliseconds += details.FrameMilliseconds;
            maximumMilliseconds = Math.Max(
                maximumMilliseconds, details.FrameMilliseconds);
            if (sampleCount < 120)
            {
                return;
            }
            Debug.WriteLine(
                $"[D3D11Chart] Tyres/{_kind}: avg " +
                $"{totalMilliseconds / sampleCount:0.00} ms, " +
                $"max {maximumMilliseconds:0.00} ms, " +
                $"{details.SourcePoints:N0} source points, " +
                $"{details.SubmittedSegments:N0} segments, " +
                $"LOD={details.UsedReduction}, WARP={details.UsingWarp}");
            sampleCount = 0;
            totalMilliseconds = 0;
            maximumMilliseconds = 0;
        };
    }
#endif
}
