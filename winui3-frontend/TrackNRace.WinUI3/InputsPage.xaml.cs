using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using System.Diagnostics;
using TrackNRace.Charting;
using Windows.Foundation;
using UiColor = Windows.UI.Color;

namespace TrackNRace.WinUI3;

public sealed partial class InputsPage : Page
{
    private const int MaxChartPoints = 750_000;
    private const double RetentionSeconds = 600;
    private const double InputAxisMinimum = -1.05;
    private const double InputAxisMaximum = 1.05;
    private static readonly double[] InputAxisTicks =
        [-1, -0.8, -0.6, -0.4, -0.2, 0, 0.2, 0.4, 0.6, 0.8, 1];
    private static readonly UiColor GearColor =
        UiColor.FromArgb(255, 87, 148, 242);
    private static readonly UiColor ThrottleColor =
        UiColor.FromArgb(255, 55, 135, 45);
    private static readonly UiColor BrakeColor =
        UiColor.FromArgb(255, 196, 22, 42);
    private static readonly UiColor SteeringColor =
        UiColor.FromArgb(255, 191, 95, 255);

    private readonly List<double> _times = [];
    private readonly List<double> _gear = [];
    private readonly List<double> _throttle = [];
    private readonly List<double> _brake = [];
    private readonly List<double> _steering = [];
    private ChartLineSeries _gearSeries = null!;
    private ChartLineSeries _throttleSeries = null!;
    private ChartLineSeries _brakeSeries = null!;
    private ChartLineSeries _steeringSeries = null!;
    private ChartAxis _gearTimeAxis = null!;
    private ChartAxis _throttleBrakeTimeAxis = null!;
    private ChartAxis _steeringTimeAxis = null!;
    private ChartAxis _gearAxis = null!;
    private ChartAxis _throttleBrakeAxis = null!;
    private ChartAxis _steeringAxis = null!;
    private ChartCrosshairTooltipPlugin _gearTooltip = null!;
    private ChartCrosshairTooltipPlugin _throttleBrakeTooltip = null!;
    private ChartCrosshairTooltipPlugin _steeringTooltip = null!;
    private InputReferenceLinesPlugin _throttleBrakeReferenceLines = null!;
    private InputReferenceLinesPlugin _steeringReferenceLines = null!;
    private TelemetrySessionStore? _store;
    private int _storeCount;
    private long _bufferEpoch = -1;
    private long _timelineRevision = -1;
    private bool _isLoaded;
    private int _windowSeconds;
    private int _refreshQueued;

    public InputsPage()
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
            _store.TelemetryChanged += OnInputTelemetryChanged;
            _store.TimelineReset += OnTimelineReset;
        }
        app.ChartWindowChanged += OnChartWindowChanged;
        ApplyTheme();
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
            _store.TelemetryChanged -= OnInputTelemetryChanged;
            _store.TimelineReset -= OnTimelineReset;
            _store = null;
        }
        ((App)Application.Current).ChartWindowChanged -= OnChartWindowChanged;
    }

    private void ConfigurePlots()
    {
        _gearTimeAxis = CreateTimeAxis();
        _gearAxis = new ChartAxis(
            "gear", ChartAxisOrientation.Y, ChartAxisSide.Left)
        {
            Minimum = .5,
            Maximum = 8.5,
            TickProvider = new FixedChartTickProvider(1, 2, 3, 4, 5, 6, 7, 8),
            LabelFormatter = value => $"{value:0}",
        };
        ConfigureChart(GearPlot, _gearTimeAxis, _gearAxis, "Input/Gear");
        _gearSeries = GearPlot.Series.Add(new ChartLineSeriesOptions(
            "gear", "time", "gear", GearColor,
            Thickness: 1.5f,
            MaximumPointCount: MaxChartPoints,
            MaximumXSpan: RetentionSeconds));
        _gearTooltip = new ChartCrosshairTooltipPlugin(BuildGearTooltip);
        GearPlot.Plugins.Add(_gearTooltip);

        _throttleBrakeTimeAxis = CreateTimeAxis();
        _throttleBrakeAxis = new ChartAxis(
            "input", ChartAxisOrientation.Y, ChartAxisSide.Left)
        {
            Minimum = InputAxisMinimum,
            Maximum = InputAxisMaximum,
            TickProvider = new FixedChartTickProvider(InputAxisTicks),
            LabelFormatter = value => $"{Math.Abs(value) * 100:0}%",
        };
        ConfigureChart(
            ThrottleBrakePlot,
            _throttleBrakeTimeAxis,
            _throttleBrakeAxis,
            "Input/ThrottleBrake");
        _throttleSeries = ThrottleBrakePlot.Series.Add(
            new ChartLineSeriesOptions(
                "throttle", "time", "input", ThrottleColor,
                Thickness: 1.5f,
                MaximumPointCount: MaxChartPoints,
                MaximumXSpan: RetentionSeconds));
        _brakeSeries = ThrottleBrakePlot.Series.Add(
            new ChartLineSeriesOptions(
                "brake", "time", "input", BrakeColor,
                Thickness: 1.5f,
                MaximumPointCount: MaxChartPoints,
                MaximumXSpan: RetentionSeconds));
        _throttleBrakeReferenceLines = new InputReferenceLinesPlugin(
            _throttleBrakeTimeAxis,
            _throttleBrakeAxis,
            [0]);
        ThrottleBrakePlot.Plugins.Add(_throttleBrakeReferenceLines);
        _throttleBrakeTooltip =
            new ChartCrosshairTooltipPlugin(BuildThrottleBrakeTooltip);
        ThrottleBrakePlot.Plugins.Add(_throttleBrakeTooltip);

        _steeringTimeAxis = CreateTimeAxis();
        _steeringAxis = new ChartAxis(
            "steering", ChartAxisOrientation.Y, ChartAxisSide.Left)
        {
            Minimum = InputAxisMinimum,
            Maximum = InputAxisMaximum,
            TickProvider = new FixedChartTickProvider(InputAxisTicks),
            LabelFormatter = FormatSteering,
        };
        ConfigureChart(
            SteeringPlot,
            _steeringTimeAxis,
            _steeringAxis,
            "Input/Steering");
        _steeringSeries = SteeringPlot.Series.Add(new ChartLineSeriesOptions(
            "steering", "time", "steering", SteeringColor,
            Thickness: 1.5f,
            MaximumPointCount: MaxChartPoints,
            MaximumXSpan: RetentionSeconds));
        _steeringReferenceLines = new InputReferenceLinesPlugin(
            _steeringTimeAxis,
            _steeringAxis,
            [0]);
        SteeringPlot.Plugins.Add(_steeringReferenceLines);
        _steeringTooltip =
            new ChartCrosshairTooltipPlugin(BuildSteeringTooltip);
        SteeringPlot.Plugins.Add(_steeringTooltip);

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

    private void OnInputTelemetryChanged() => QueueRefresh();

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
        var tickProvider = new IntervalChartTickProvider(seconds / 6d);
        foreach (var axis in new[]
                 {
                     _gearTimeAxis,
                     _throttleBrakeTimeAxis,
                     _steeringTimeAxis,
                 })
        {
            axis.TickProvider = tickProvider;
        }
        if (_isLoaded)
        {
            RefreshFromStore();
        }
    }

    private void RefreshFromStore()
    {
        if (!_isLoaded || _store is null)
        {
            return;
        }

        var read = _store.ReadTelemetry(
            _storeCount, _bufferEpoch, _timelineRevision);
        if (read.Reset)
        {
            ClearData();
        }

        var firstNewIndex = _times.Count;
        foreach (var sample in read.Samples)
        {
            _times.Add(sample.SessionTime);
            _gear.Add(sample.Gear);
            _throttle.Add(sample.Throttle);
            _brake.Add(-sample.Brake);
            _steering.Add(sample.Steering);
        }
        AppendNewPoints(firstNewIndex);

        _storeCount = read.TotalCount;
        _bufferEpoch = read.BufferEpoch;
        _timelineRevision = read.TimelineRevision;
        var hasData = _times.Count > 0;
        GearNoData.Visibility = hasData ? Visibility.Collapsed : Visibility.Visible;
        ThrottleBrakeNoData.Visibility =
            hasData ? Visibility.Collapsed : Visibility.Visible;
        SteeringNoData.Visibility =
            hasData ? Visibility.Collapsed : Visibility.Visible;

        var latest = hasData ? _times[^1] : 0;
        var xMin = Math.Max(0, latest - _windowSeconds);
        var xMax = Math.Max(_windowSeconds, latest);
        SetTimeRange(_gearTimeAxis, xMin, xMax);
        SetTimeRange(_throttleBrakeTimeAxis, xMin, xMax);
        SetTimeRange(_steeringTimeAxis, xMin, xMax);
        GearPlot.Invalidate();
        ThrottleBrakePlot.Invalidate();
        SteeringPlot.Invalidate();
    }

    private void AppendNewPoints(int firstNewIndex)
    {
        var count = _times.Count - firstNewIndex;
        if (count <= 0)
        {
            return;
        }

        _gearSeries.Append(BuildHorizontalStepPoints(
            _gear, firstNewIndex, count));
        _throttleSeries.Append(BuildVerticalStepPoints(
            _throttle, firstNewIndex, count));
        _brakeSeries.Append(BuildVerticalStepPoints(
            _brake, firstNewIndex, count));

        var steering = new ChartPoint[count];
        for (var index = 0; index < count; index++)
        {
            var sourceIndex = firstNewIndex + index;
            steering[index] = new ChartPoint(
                _times[sourceIndex], _steering[sourceIndex]);
        }
        _steeringSeries.Append(steering);
    }

    private ChartPoint[] BuildHorizontalStepPoints(
        IReadOnlyList<double> values,
        int firstNewIndex,
        int count)
    {
        var points = new ChartPoint[
            count * 2 - (firstNewIndex == 0 ? 1 : 0)];
        var destination = 0;
        for (var index = firstNewIndex;
             index < firstNewIndex + count;
             index++)
        {
            if (index > 0)
            {
                points[destination++] =
                    new ChartPoint(_times[index], values[index - 1]);
            }
            points[destination++] =
                new ChartPoint(_times[index], values[index]);
        }
        return points;
    }

    private ChartPoint[] BuildVerticalStepPoints(
        IReadOnlyList<double> values,
        int firstNewIndex,
        int count)
    {
        var points = new ChartPoint[
            count * 2 - (firstNewIndex == 0 ? 1 : 0)];
        var destination = 0;
        for (var index = firstNewIndex;
             index < firstNewIndex + count;
             index++)
        {
            if (index > 0)
            {
                points[destination++] =
                    new ChartPoint(_times[index - 1], values[index]);
            }
            points[destination++] =
                new ChartPoint(_times[index], values[index]);
        }
        return points;
    }

    private void ClearData()
    {
        _times.Clear();
        _gear.Clear();
        _throttle.Clear();
        _brake.Clear();
        _steering.Clear();
        _gearSeries.Clear();
        _throttleSeries.Clear();
        _brakeSeries.Clear();
        _steeringSeries.Clear();
        _storeCount = 0;
    }

    private static void SetTimeRange(ChartAxis axis, double minimum, double maximum)
    {
        axis.Minimum = minimum;
        axis.Maximum = maximum;
    }

    private void OnActualThemeChanged(FrameworkElement sender, object args)
    {
        ApplyTheme();
        if (_isLoaded)
        {
            GearPlot.Invalidate();
            ThrottleBrakePlot.Invalidate();
            SteeringPlot.Invalidate();
        }
    }

    private void ApplyTheme()
    {
        if (GearPlot is null)
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
        var centerLine = dark
            ? UiColor.FromArgb(107, 124, 128, 152)
            : UiColor.FromArgb(89, 107, 114, 128);

        foreach (var axis in new[]
                 {
                     _gearTimeAxis,
                     _throttleBrakeTimeAxis,
                     _steeringTimeAxis,
                     _gearAxis,
                     _throttleBrakeAxis,
                     _steeringAxis,
                 })
        {
            axis.Color = axes;
        }
        foreach (var chart in new[]
                 {
                     GearPlot,
                     ThrottleBrakePlot,
                     SteeringPlot,
                 })
        {
            chart.GridColor = grid;
        }
        _throttleBrakeReferenceLines.Color = centerLine;
        _steeringReferenceLines.Color = centerLine;
        _gearTooltip.ApplyTheme(dark);
        _throttleBrakeTooltip.ApplyTheme(dark);
        _steeringTooltip.ApplyTheme(dark);
    }

    private ChartTooltipData? BuildGearTooltip(double x)
    {
        var index = NearestIndex(x);
        if (index < 0)
        {
            return null;
        }
        return new ChartTooltipData(
            _times[index],
            new ChartTooltipContent(
                FormatTime(_times[index]),
                [
                    new ChartTooltipEntry(
                        "Gear", $"{Math.Round(_gear[index])}", GearColor),
                ]),
            [
                new ChartTooltipMarker(
                    _times[index], _gear[index], "gear", GearColor),
            ]);
    }

    private ChartTooltipData? BuildThrottleBrakeTooltip(double x)
    {
        var index = NearestIndex(x);
        if (index < 0)
        {
            return null;
        }
        return new ChartTooltipData(
            _times[index],
            new ChartTooltipContent(
                FormatTime(_times[index]),
                [
                    new ChartTooltipEntry(
                        "Throttle",
                        $"{Math.Round(_throttle[index] * 100)}%",
                        ThrottleColor),
                    new ChartTooltipEntry(
                        "Brake",
                        $"{Math.Round(Math.Abs(_brake[index]) * 100)}%",
                        BrakeColor),
                ]),
            [
                new ChartTooltipMarker(
                    _times[index], _throttle[index], "input", ThrottleColor),
                new ChartTooltipMarker(
                    _times[index], _brake[index], "input", BrakeColor),
            ]);
    }

    private ChartTooltipData? BuildSteeringTooltip(double x)
    {
        var index = NearestIndex(x);
        if (index < 0)
        {
            return null;
        }
        return new ChartTooltipData(
            _times[index],
            new ChartTooltipContent(
                FormatTime(_times[index]),
                [
                    new ChartTooltipEntry(
                        "Steering",
                        FormatSteering(_steering[index]),
                        SteeringColor),
                ]),
            [
                new ChartTooltipMarker(
                    _times[index],
                    _steering[index],
                    "steering",
                    SteeringColor),
            ]);
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

    private static string FormatTime(double seconds)
    {
        var safe = Math.Max(0, seconds);
        return $"{(int)(safe / 60)}:{(int)(safe % 60):00}";
    }

    private static string FormatSteering(double value)
    {
        if (Math.Abs(value) < .005)
        {
            return "0%";
        }
        return $"{Math.Abs(value) * 100:0}% {(value < 0 ? "L" : "R")}";
    }

    private sealed class InputReferenceLinesPlugin(
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
                    YAxisKey: _yAxis.Key));
            }
        }
    }
}
