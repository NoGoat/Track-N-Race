using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using System.Diagnostics;
using TrackNRace.Charting;
using Windows.Foundation;
using UiColor = Windows.UI.Color;

namespace TrackNRace.WinUI3;

public sealed partial class MiscPage : Page
{
    private const int MaxChartPoints = 750_000;
    private const double RetentionSeconds = 600;
    private const double InitialRideLower = 0;
    private const double InitialRideUpper = 50;
    private const double RideLowerPadding = 2;
    private const double RideUpperPadding = 5;
    private static readonly double[] GForceTicks = [-6, -4, -2, 0, 2, 4, 6];
    private static readonly UiColor LateralColor =
        UiColor.FromArgb(255, 240, 165, 0);
    private static readonly UiColor LongitudinalColor =
        UiColor.FromArgb(255, 87, 148, 242);
    private static readonly UiColor FrontColor =
        UiColor.FromArgb(255, 115, 191, 105);
    private static readonly UiColor RearColor =
        UiColor.FromArgb(255, 184, 119, 219);

    private readonly List<double> _motionTimes = [];
    private readonly List<double> _lateral = [];
    private readonly List<double> _longitudinal = [];
    private readonly List<double> _motionExTimes = [];
    private readonly List<double> _frontHeight = [];
    private readonly List<double> _rearHeight = [];
    private ChartLineSeries _lateralSeries = null!;
    private ChartLineSeries _longitudinalSeries = null!;
    private ChartLineSeries _frontSeries = null!;
    private ChartLineSeries _rearSeries = null!;
    private ChartAxis _gForceTimeAxis = null!;
    private ChartAxis _rideHeightTimeAxis = null!;
    private ChartAxis _gForceAxis = null!;
    private ChartAxis _rideHeightAxis = null!;
    private ChartCrosshairTooltipPlugin _gForceTooltip = null!;
    private ChartCrosshairTooltipPlugin _rideHeightTooltip = null!;
    private ReferenceLinesPlugin _gForceReferenceLines = null!;
    private TelemetrySessionStore? _store;
    private int _motionCount;
    private int _motionExCount;
    private long _motionBufferEpoch = -1;
    private long _motionExBufferEpoch = -1;
    private long _timelineRevision = -1;
    private bool _isLoaded;
    private int _windowSeconds;
    private int _refreshQueued;
    private double _rideLower = InitialRideLower;
    private double _rideUpper = InitialRideUpper;

    public MiscPage()
    {
        InitializeComponent();
        _windowSeconds = ((App)Application.Current).ChartWindowSeconds;
        ConfigurePlots();
    }

    private void OnLoaded(object sender, RoutedEventArgs args)
    {
        if (_isLoaded)
        {
            return;
        }

        _isLoaded = true;
        var app = (App)Application.Current;
        _store = app.TelemetryState;
        if (_store is not null)
        {
            _store.MiscChanged += OnMiscChanged;
            _store.TimelineReset += OnTimelineReset;
        }
        app.ChartWindowChanged += OnChartWindowChanged;
        app.MiscDisplayChanged += OnMiscDisplayChanged;
        ApplyTheme();
        ApplyDisplaySettings();
        RefreshFromStore();
    }

    private void OnUnloaded(object sender, RoutedEventArgs args)
    {
        if (!_isLoaded)
        {
            return;
        }

        _isLoaded = false;
        if (_store is not null)
        {
            _store.MiscChanged -= OnMiscChanged;
            _store.TimelineReset -= OnTimelineReset;
            _store = null;
        }
        var app = (App)Application.Current;
        app.ChartWindowChanged -= OnChartWindowChanged;
        app.MiscDisplayChanged -= OnMiscDisplayChanged;
    }

    private void ConfigurePlots()
    {
        _gForceTimeAxis = CreateTimeAxis();
        _gForceAxis = new ChartAxis(
            "g-force", ChartAxisOrientation.Y, ChartAxisSide.Left)
        {
            Minimum = -6,
            Maximum = 6,
            TickProvider = new FixedChartTickProvider(GForceTicks),
            LabelFormatter = value => $"{value:0}g",
        };
        ConfigureChart(
            GForcePlot, _gForceTimeAxis, _gForceAxis, "Misc/GForce");
        _lateralSeries = AddSeries(
            GForcePlot, "lateral", "g-force", LateralColor);
        _longitudinalSeries = AddSeries(
            GForcePlot, "longitudinal", "g-force", LongitudinalColor);
        _gForceReferenceLines = new ReferenceLinesPlugin(
            _gForceTimeAxis, _gForceAxis, [0, 4, -4]);
        GForcePlot.Plugins.Add(_gForceReferenceLines);
        _gForceTooltip =
            new ChartCrosshairTooltipPlugin(BuildGForceTooltip);
        GForcePlot.Plugins.Add(_gForceTooltip);

        _rideHeightTimeAxis = CreateTimeAxis();
        _rideHeightAxis = new ChartAxis(
            "ride-height", ChartAxisOrientation.Y, ChartAxisSide.Left)
        {
            Minimum = _rideLower,
            Maximum = _rideUpper,
            TickProvider = CreateRideHeightTicks(),
            LabelFormatter = value => $"{value:0}mm",
        };
        ConfigureChart(
            RideHeightPlot,
            _rideHeightTimeAxis,
            _rideHeightAxis,
            "Misc/RideHeight");
        _frontSeries = AddSeries(
            RideHeightPlot, "front", "ride-height", FrontColor);
        _rearSeries = AddSeries(
            RideHeightPlot, "rear", "ride-height", RearColor);
        _rideHeightTooltip =
            new ChartCrosshairTooltipPlugin(BuildRideHeightTooltip);
        RideHeightPlot.Plugins.Add(_rideHeightTooltip);

        ApplyTheme();
    }

    private ChartAxis CreateTimeAxis() =>
        new("time", ChartAxisOrientation.X, ChartAxisSide.Bottom)
        {
            Minimum = 0,
            Maximum = _windowSeconds,
            TickProvider = new IntervalChartTickProvider(_windowSeconds / 6d),
            LabelFormatter = FormatTime,
            ShowGridLines = true,
        };

    private static ChartLineSeries AddSeries(
        GpuChart chart,
        string name,
        string axisKey,
        UiColor color) =>
        chart.Series.Add(new ChartLineSeriesOptions(
            name,
            "time",
            axisKey,
            color,
            Thickness: 1.5f,
            MaximumPointCount: MaxChartPoints,
            MaximumXSpan: RetentionSeconds));

    private static void ConfigureChart(
        GpuChart chart,
        ChartAxis timeAxis,
        ChartAxis valueAxis,
        string diagnosticsName)
    {
        chart.Plugins.Add(new ChartBackgroundPlugin());
        chart.Axes.Add(timeAxis);
        chart.Axes.Add(valueAxis);
        chart.RenderFailed += error =>
            Debug.WriteLine($"[D3D11Chart] {diagnosticsName}: {error}");
#if DEBUG
        AttachRenderProbe(chart, diagnosticsName);
#endif
    }

#if DEBUG
    private static void AttachRenderProbe(GpuChart chart, string name)
    {
        var count = 0;
        var total = 0d;
        var maximum = 0d;
        chart.DiagnosticsUpdated += details =>
        {
            var milliseconds = details.FrameMilliseconds;
            count++;
            total += milliseconds;
            maximum = Math.Max(maximum, milliseconds);
            if (count < 120)
            {
                return;
            }
            Debug.WriteLine(
                $"[D3D11Chart] {name}: avg {total / count:0.00} ms, " +
                $"max {maximum:0.00} ms, {details.SourcePoints:N0} source points, " +
                $"{details.SubmittedSegments:N0} segments, " +
                $"LOD={details.UsedReduction}, WARP={details.UsingWarp}");
            count = 0;
            total = 0;
            maximum = 0;
        };
    }
#endif

    private void OnMiscChanged() => QueueRefresh();

    private void OnTimelineReset(TimelineResetReason reason) => QueueRefresh();

    private void QueueRefresh()
    {
        if (!_isLoaded || Interlocked.Exchange(ref _refreshQueued, 1) != 0)
        {
            return;
        }
        DispatcherQueue.TryEnqueue(() =>
        {
            Interlocked.Exchange(ref _refreshQueued, 0);
            RefreshFromStore();
        });
    }

    private void OnChartWindowChanged(int seconds)
    {
        _windowSeconds = seconds;
        _gForceTimeAxis.TickProvider =
            new IntervalChartTickProvider(seconds / 6d);
        _rideHeightTimeAxis.TickProvider =
            new IntervalChartTickProvider(seconds / 6d);
        if (_isLoaded)
        {
            RefreshFromStore();
        }
    }

    private void OnMiscDisplayChanged()
    {
        DispatcherQueue.TryEnqueue(ApplyDisplaySettings);
    }

    private void ApplyDisplaySettings()
    {
        var settings = ((App)Application.Current).MiscDisplay;
        GForceSection.Visibility =
            settings.ShowGForce ? Visibility.Visible : Visibility.Collapsed;
        RideHeightSection.Visibility =
            settings.ShowRideHeight ? Visibility.Visible : Visibility.Collapsed;
        GForceRow.Height =
            settings.ShowGForce ? new GridLength(1, GridUnitType.Star) : new GridLength(0);
        RideHeightRow.Height =
            settings.ShowRideHeight ? new GridLength(1, GridUnitType.Star) : new GridLength(0);
        var showDivider = settings.ShowGForce && settings.ShowRideHeight;
        SectionDivider.Visibility =
            showDivider ? Visibility.Visible : Visibility.Collapsed;
        SectionDividerRow.Height = new GridLength(showDivider ? 1 : 0);
    }

    private void RefreshFromStore()
    {
        if (!_isLoaded || _store is null)
        {
            return;
        }

        var read = _store.ReadMisc(
            _motionCount,
            _motionBufferEpoch,
            _motionExCount,
            _motionExBufferEpoch,
            _timelineRevision);
        if (read.Reset)
        {
            ClearData();
        }

        AppendMotion(read.Motion);
        AppendMotionEx(read.MotionEx);
        _motionCount = read.MotionTotalCount;
        _motionExCount = read.MotionExTotalCount;
        _motionBufferEpoch = read.MotionBufferEpoch;
        _motionExBufferEpoch = read.MotionExBufferEpoch;
        _timelineRevision = read.TimelineRevision;

        GForceNoData.Visibility =
            _motionTimes.Count > 0 ? Visibility.Collapsed : Visibility.Visible;
        RideHeightNoData.Visibility =
            _motionExTimes.Count > 0 ? Visibility.Collapsed : Visibility.Visible;
        UpdateTimeRange(_gForceTimeAxis, _motionTimes);
        UpdateTimeRange(_rideHeightTimeAxis, _motionExTimes);
        GForcePlot.Invalidate();
        RideHeightPlot.Invalidate();
    }

    private void AppendMotion(IReadOnlyList<MotionSample> samples)
    {
        if (samples.Count == 0)
        {
            return;
        }
        var lateral = new ChartPoint[samples.Count];
        var longitudinal = new ChartPoint[samples.Count];
        for (var index = 0; index < samples.Count; index++)
        {
            var sample = samples[index];
            _motionTimes.Add(sample.SessionTime);
            _lateral.Add(sample.LateralG);
            _longitudinal.Add(sample.LongitudinalG);
            lateral[index] =
                new ChartPoint(sample.SessionTime, sample.LateralG);
            longitudinal[index] =
                new ChartPoint(sample.SessionTime, sample.LongitudinalG);
        }
        _lateralSeries.Append(lateral);
        _longitudinalSeries.Append(longitudinal);
    }

    private void AppendMotionEx(IReadOnlyList<MotionExSample> samples)
    {
        if (samples.Count == 0)
        {
            return;
        }
        var front = new ChartPoint[samples.Count];
        var rear = new ChartPoint[samples.Count];
        var rangeChanged = false;
        for (var index = 0; index < samples.Count; index++)
        {
            var sample = samples[index];
            _motionExTimes.Add(sample.SessionTime);
            _frontHeight.Add(sample.FrontAeroHeightMm);
            _rearHeight.Add(sample.RearAeroHeightMm);
            front[index] =
                new ChartPoint(sample.SessionTime, sample.FrontAeroHeightMm);
            rear[index] =
                new ChartPoint(sample.SessionTime, sample.RearAeroHeightMm);
            var minimum = Math.Min(
                sample.FrontAeroHeightMm, sample.RearAeroHeightMm);
            var maximum = Math.Max(
                sample.FrontAeroHeightMm, sample.RearAeroHeightMm);
            if (minimum < _rideLower)
            {
                _rideLower = minimum - RideLowerPadding;
                rangeChanged = true;
            }
            if (maximum > _rideUpper)
            {
                _rideUpper = maximum + RideUpperPadding;
                rangeChanged = true;
            }
        }
        _frontSeries.Append(front);
        _rearSeries.Append(rear);
        if (rangeChanged)
        {
            _rideHeightAxis.Minimum = _rideLower;
            _rideHeightAxis.Maximum = _rideUpper;
            _rideHeightAxis.TickProvider = CreateRideHeightTicks();
        }
    }

    private FixedChartTickProvider CreateRideHeightTicks()
    {
        var step = (_rideUpper - _rideLower) / 4;
        var ticks = new double[5];
        for (var index = 0; index < ticks.Length; index++)
        {
            ticks[index] = Math.Round(_rideLower + (step * index));
        }
        return new FixedChartTickProvider(ticks);
    }

    private void ClearData()
    {
        _motionTimes.Clear();
        _lateral.Clear();
        _longitudinal.Clear();
        _motionExTimes.Clear();
        _frontHeight.Clear();
        _rearHeight.Clear();
        _lateralSeries.Clear();
        _longitudinalSeries.Clear();
        _frontSeries.Clear();
        _rearSeries.Clear();
        _motionCount = 0;
        _motionExCount = 0;
        _rideLower = InitialRideLower;
        _rideUpper = InitialRideUpper;
        _rideHeightAxis.Minimum = _rideLower;
        _rideHeightAxis.Maximum = _rideUpper;
        _rideHeightAxis.TickProvider = CreateRideHeightTicks();
    }

    private void UpdateTimeRange(ChartAxis axis, IReadOnlyList<double> times)
    {
        var latest = times.Count > 0 ? times[^1] : 0;
        axis.Minimum = Math.Max(0, latest - _windowSeconds);
        axis.Maximum = Math.Max(_windowSeconds, latest);
    }

    private void OnActualThemeChanged(FrameworkElement sender, object args)
    {
        ApplyTheme();
        if (_isLoaded)
        {
            GForcePlot.Invalidate();
            RideHeightPlot.Invalidate();
        }
    }

    private void ApplyTheme()
    {
        if (GForcePlot is null)
        {
            return;
        }

        var dark = ActualTheme == ElementTheme.Dark;
        var axes = dark
            ? UiColor.FromArgb(255, 124, 128, 152)
            : UiColor.FromArgb(255, 107, 114, 128);
        var grid = dark
            ? UiColor.FromArgb(10, 255, 255, 255)
            : UiColor.FromArgb(18, 0, 0, 0);
        var reference = dark
            ? UiColor.FromArgb(107, 124, 128, 152)
            : UiColor.FromArgb(89, 107, 114, 128);
        foreach (var axis in new[]
                 {
                     _gForceTimeAxis,
                     _rideHeightTimeAxis,
                     _gForceAxis,
                     _rideHeightAxis,
                 })
        {
            axis.Color = axes;
        }
        foreach (var chart in new[] { GForcePlot, RideHeightPlot })
        {
            chart.GridColor = grid;
        }
        _gForceReferenceLines.Color = reference;
        _gForceTooltip.ApplyTheme(dark);
        _rideHeightTooltip.ApplyTheme(dark);
    }

    private ChartTooltipData? BuildGForceTooltip(double x)
    {
        var index = NearestIndex(_motionTimes, x);
        if (index < 0)
        {
            return null;
        }
        return new ChartTooltipData(
            _motionTimes[index],
            new ChartTooltipContent(
                FormatTime(_motionTimes[index]),
                [
                    new ChartTooltipEntry(
                        "Lateral", $"{_lateral[index]:0.00} g", LateralColor),
                    new ChartTooltipEntry(
                        "Longitudinal",
                        $"{_longitudinal[index]:0.00} g",
                        LongitudinalColor),
                ]),
            [
                new ChartTooltipMarker(
                    _motionTimes[index],
                    _lateral[index],
                    "g-force",
                    LateralColor),
                new ChartTooltipMarker(
                    _motionTimes[index],
                    _longitudinal[index],
                    "g-force",
                    LongitudinalColor),
            ]);
    }

    private ChartTooltipData? BuildRideHeightTooltip(double x)
    {
        var index = NearestIndex(_motionExTimes, x);
        if (index < 0)
        {
            return null;
        }
        return new ChartTooltipData(
            _motionExTimes[index],
            new ChartTooltipContent(
                FormatTime(_motionExTimes[index]),
                [
                    new ChartTooltipEntry(
                        "Front", $"{_frontHeight[index]:0.0} mm", FrontColor),
                    new ChartTooltipEntry(
                        "Rear", $"{_rearHeight[index]:0.0} mm", RearColor),
                ]),
            [
                new ChartTooltipMarker(
                    _motionExTimes[index],
                    _frontHeight[index],
                    "ride-height",
                    FrontColor),
                new ChartTooltipMarker(
                    _motionExTimes[index],
                    _rearHeight[index],
                    "ride-height",
                    RearColor),
            ]);
    }

    private static int NearestIndex(List<double> times, double sessionTime)
    {
        if (times.Count == 0)
        {
            return -1;
        }
        var candidate = times.BinarySearch(sessionTime);
        if (candidate < 0)
        {
            candidate = ~candidate;
        }
        if (candidate >= times.Count)
        {
            return times.Count - 1;
        }
        if (candidate > 0 &&
            sessionTime - times[candidate - 1] <=
            times[candidate] - sessionTime)
        {
            candidate--;
        }
        return candidate;
    }

    private static string FormatTime(double seconds)
    {
        var safe = Math.Max(0, seconds);
        return $"{(int)(safe / 60)}:{(int)(safe % 60):00}";
    }

    private sealed class ReferenceLinesPlugin(
        ChartAxis xAxis,
        ChartAxis yAxis,
        IReadOnlyList<double> values) : IChartPlugin
    {
        private readonly ChartAxis _xAxis = xAxis;
        private readonly ChartAxis _yAxis = yAxis;
        private readonly IReadOnlyList<double> _values = values;
        private ChartPluginContext? _context;
        private UiColor _color;

        public UiColor Color
        {
            get => _color;
            set
            {
                if (_color == value)
                {
                    return;
                }
                _color = value;
                _context?.InvalidateOverlay();
            }
        }

        public void Attach(ChartPluginContext context) => _context = context;

        public void Detach() => _context = null;

        public void BuildOverlay(ChartOverlayBuilder builder)
        {
            foreach (var value in _values)
            {
                builder.Add(new ChartOverlayLine(
                    new Point(_xAxis.Minimum, value),
                    new Point(_xAxis.Maximum, value),
                    Color,
                    value == 0 ? 1 : .75,
                    YAxisKey: _yAxis.Key,
                    DashPattern: value == 0 ? null : [4, 4]));
            }
        }
    }
}
