using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using ScottPlot;
using ScottPlot.Plottables;
using ScottPlot.WinUI;
using System.Diagnostics;

namespace TrackNRace.WinUI3;

public sealed partial class InputsPage : Page
{
    private static readonly ScottPlot.Color GearColor =
        ScottPlot.Color.FromHex("#5794F2");
    private static readonly ScottPlot.Color ThrottleColor =
        ScottPlot.Color.FromHex("#37872D");
    private static readonly ScottPlot.Color BrakeColor =
        ScottPlot.Color.FromHex("#C4162A");
    private static readonly ScottPlot.Color SteeringColor =
        ScottPlot.Color.FromHex("#BF5FFF");

    private readonly List<double> _times = [];
    private readonly List<double> _gear = [];
    private readonly List<double> _throttle = [];
    private readonly List<double> _brake = [];
    private readonly List<double> _steering = [];
    private TelemetrySessionStore? _store;
    private int _storeCount;
    private long _bufferEpoch = -1;
    private long _timelineRevision = -1;
    private bool _isLoaded;
    private int _windowSeconds;

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
            _store.InputTelemetryChanged += OnInputTelemetryChanged;
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
            _store.InputTelemetryChanged -= OnInputTelemetryChanged;
            _store.TimelineReset -= OnTimelineReset;
            _store = null;
        }
        ((App)Application.Current).ChartWindowChanged -= OnChartWindowChanged;
    }

    private void ConfigurePlots()
    {
        var gear = GearPlot.Plot.Add.SignalXY(_times, _gear);
        StyleSeries(gear, GearColor, ConnectStyle.StepHorizontal);
        gear.LineWidth = 2;
        ConfigurePlot(
            GearPlot,
            0.5,
            8.5,
            [1, 2, 3, 4, 5, 6, 7, 8],
            value => $"{value:0}");

        var throttle = ThrottleBrakePlot.Plot.Add.SignalXY(_times, _throttle);
        StyleSeries(throttle, ThrottleColor, ConnectStyle.StepVertical);

        var brake = ThrottleBrakePlot.Plot.Add.SignalXY(_times, _brake);
        StyleSeries(brake, BrakeColor, ConnectStyle.StepVertical);
        ConfigurePlot(
            ThrottleBrakePlot,
            -1,
            1,
            [-1, -0.5, 0, 0.5, 1],
            value => $"{Math.Abs(value) * 100:0}%");

        var steering = SteeringPlot.Plot.Add.SignalXY(_times, _steering);
        StyleSeries(steering, SteeringColor, ConnectStyle.Straight);
        ConfigurePlot(
            SteeringPlot,
            -1,
            1,
            [-1, -0.5, 0, 0.5, 1],
            FormatSteering);

#if DEBUG
        AttachRenderProbe(GearPlot, "Input/Gear");
        AttachRenderProbe(ThrottleBrakePlot, "Input/ThrottleBrake");
        AttachRenderProbe(SteeringPlot, "Input/Steering");
#endif
        ApplyTheme();
    }

#if DEBUG
    private static void AttachRenderProbe(WinUIPlot control, string name)
    {
        var sampleCount = 0;
        var totalMilliseconds = 0d;
        var maximumMilliseconds = 0d;
        control.Plot.RenderManager.RenderFinished += (_, details) =>
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
                $"[ScottPlot] {name}: avg " +
                $"{totalMilliseconds / sampleCount:0.00} ms, " +
                $"max {maximumMilliseconds:0.00} ms, " +
                $"{details.Count:N0} total renders");
            sampleCount = 0;
            totalMilliseconds = 0;
            maximumMilliseconds = 0;
        };
    }
#endif

    private static void StyleSeries(
        SignalXY series,
        ScottPlot.Color color,
        ConnectStyle connectStyle)
    {
        series.Color = color;
        series.LineWidth = 1.5f;
        series.MarkerSize = 0;
        series.ConnectStyle = connectStyle;
    }

    private static void ConfigurePlot(
        WinUIPlot plotControl,
        double yMin,
        double yMax,
        double[] yTicks,
        Func<double, string> yFormatter)
    {
        var plot = plotControl.Plot;
        plotControl.UserInputProcessor.Disable();
        plot.Axes.SetLimits(0, 30, yMin, yMax);
        plot.Axes.Left.SetTicks(
            yTicks,
            yTicks.Select(yFormatter).ToArray());
        plot.Axes.Bottom.TickGenerator =
            new ScottPlot.TickGenerators.NumericAutomatic
            {
                LabelFormatter = FormatTime,
                TargetTickCount = 6,
            };
        plot.Axes.Top.IsVisible = false;
        plot.Axes.Right.IsVisible = false;
        plot.Axes.Margins(0, 0, 0, 0);
    }

    private void OnInputTelemetryChanged()
    {
        if (!_isLoaded)
        {
            return;
        }

        DispatcherQueue.TryEnqueue(RefreshFromStore);
    }

    private void OnTimelineReset(TimelineResetReason reason)
    {
        if (!_isLoaded)
        {
            return;
        }

        DispatcherQueue.TryEnqueue(RefreshFromStore);
    }

    private void OnChartWindowChanged(int seconds)
    {
        _windowSeconds = seconds;
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

        var read = _store.ReadInputTelemetry(
            _storeCount, _bufferEpoch, _timelineRevision);
        if (read.Reset)
        {
            ClearData();
        }

        foreach (var sample in read.Samples)
        {
            _times.Add(sample.SessionTime);
            _gear.Add(sample.Gear);
            _throttle.Add(sample.Throttle);
            _brake.Add(-sample.Brake);
            _steering.Add(sample.Steering);
        }

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
        SetPlotLimits(GearPlot, xMin, xMax, 0.5, 8.5);
        SetPlotLimits(ThrottleBrakePlot, xMin, xMax, -1, 1);
        SetPlotLimits(SteeringPlot, xMin, xMax, -1, 1);
        GearPlot.Refresh();
        ThrottleBrakePlot.Refresh();
        SteeringPlot.Refresh();
    }

    private void ClearData()
    {
        _times.Clear();
        _gear.Clear();
        _throttle.Clear();
        _brake.Clear();
        _steering.Clear();
        _storeCount = 0;
        HideTooltips();
    }

    private static void SetPlotLimits(
        WinUIPlot plotControl,
        double xMin,
        double xMax,
        double yMin,
        double yMax)
    {
        plotControl.Plot.Axes.SetLimits(xMin, xMax, yMin, yMax);
    }

    private void OnActualThemeChanged(FrameworkElement sender, object args)
    {
        ApplyTheme();
        if (_isLoaded)
        {
            GearPlot.Refresh();
            ThrottleBrakePlot.Refresh();
            SteeringPlot.Refresh();
        }
    }

    private void ApplyTheme()
    {
        if (GearPlot is null)
        {
            return;
        }

        var dark = ActualTheme == ElementTheme.Dark;
        var transparent = ScottPlot.Color.FromHex("#00000000");
        var axes = ScottPlot.Color.FromHex(dark ? "#7C8098" : "#6B7280");
        var grid = ScottPlot.Color.FromHex(dark ? "#FFFFFF" : "#000000")
            .WithAlpha(dark ? .08 : .07);
        foreach (var control in new[]
                 {
                     GearPlot, ThrottleBrakePlot, SteeringPlot,
                 })
        {
            var plot = control.Plot;
            plot.FigureBackground.Color = transparent;
            plot.DataBackground.Color = transparent;
            plot.Axes.Color(axes);
            plot.Grid.MajorLineColor = grid;
            plot.Grid.MinorLineColor = grid.WithAlpha(.5);
        }
    }

    private void OnGearPointerMoved(
        object sender,
        PointerRoutedEventArgs args)
    {
        if (!TryGetNearestIndex(GearPlot, args, out var index, out var position))
        {
            return;
        }
        GearTooltipText.Text =
            $"{FormatTime(_times[index])}\nGear {Math.Round(_gear[index])}";
        ShowTooltip(GearTooltip, GearPlot, position);
    }

    private void OnThrottleBrakePointerMoved(
        object sender,
        PointerRoutedEventArgs args)
    {
        if (!TryGetNearestIndex(
                ThrottleBrakePlot, args, out var index, out var position))
        {
            return;
        }
        ThrottleBrakeTooltipText.Text =
            $"{FormatTime(_times[index])}\n" +
            $"Throttle: {Math.Round(_throttle[index] * 100)}%\n" +
            $"Brake: {Math.Round(Math.Abs(_brake[index]) * 100)}%";
        ShowTooltip(ThrottleBrakeTooltip, ThrottleBrakePlot, position);
    }

    private void OnSteeringPointerMoved(
        object sender,
        PointerRoutedEventArgs args)
    {
        if (!TryGetNearestIndex(
                SteeringPlot, args, out var index, out var position))
        {
            return;
        }
        SteeringTooltipText.Text =
            $"{FormatTime(_times[index])}\n" +
            $"Steering: {FormatSteering(_steering[index])}";
        ShowTooltip(SteeringTooltip, SteeringPlot, position);
    }

    private bool TryGetNearestIndex(
        WinUIPlot plot,
        PointerRoutedEventArgs args,
        out int index,
        out Windows.Foundation.Point position)
    {
        position = args.GetCurrentPoint(plot).Position;
        index = -1;
        if (_times.Count == 0)
        {
            return false;
        }

        var scale = XamlRoot?.RasterizationScale ?? 1;
        var coordinates = plot.Plot.GetCoordinates(
            new Pixel(
                (float)(position.X * scale),
                (float)(position.Y * scale)));
        var candidate = _times.BinarySearch(coordinates.X);
        if (candidate < 0)
        {
            candidate = ~candidate;
        }
        if (candidate >= _times.Count)
        {
            candidate = _times.Count - 1;
        }
        if (candidate > 0 &&
            Math.Abs(_times[candidate - 1] - coordinates.X) <
            Math.Abs(_times[candidate] - coordinates.X))
        {
            candidate--;
        }
        index = candidate;
        return true;
    }

    private static void ShowTooltip(
        Border tooltip,
        FrameworkElement plot,
        Windows.Foundation.Point position)
    {
        tooltip.Visibility = Visibility.Visible;
        var left = Math.Clamp(
            position.X + 12,
            4,
            Math.Max(4, plot.ActualWidth - tooltip.ActualWidth - 8));
        var top = Math.Clamp(
            position.Y + 12,
            4,
            Math.Max(4, plot.ActualHeight - tooltip.ActualHeight - 8));
        Canvas.SetLeft(tooltip, left);
        Canvas.SetTop(tooltip, top);
    }

    private void OnPlotPointerExited(
        object sender,
        PointerRoutedEventArgs args) =>
        HideTooltips();

    private void HideTooltips()
    {
        GearTooltip.Visibility = Visibility.Collapsed;
        ThrottleBrakeTooltip.Visibility = Visibility.Collapsed;
        SteeringTooltip.Visibility = Visibility.Collapsed;
    }

    private static string FormatTime(double seconds)
    {
        var safe = Math.Max(0, seconds);
        return $"{(int)(safe / 60)}:{(int)(safe % 60):00}";
    }

    private static string FormatSteering(double value)
    {
        if (Math.Abs(value) < 0.005)
        {
            return "0%";
        }
        return $"{Math.Abs(value) * 100:0}% {(value < 0 ? "L" : "R")}";
    }
}
