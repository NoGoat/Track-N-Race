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
    private const double InputAxisMinimum = -1.05;
    private const double InputAxisMaximum = 1.05;
    private static readonly double[] InputAxisTicks =
        [-1, -0.8, -0.6, -0.4, -0.2, 0, 0.2, 0.4, 0.6, 0.8, 1];
    private static readonly LinePattern HoverCrosshairPattern =
        new([2f, 1f], 0, "ElectronCrosshair");

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
    private readonly List<HorizontalLine> _bottomBoundaries = [];
    private HorizontalLine? _throttleCenterLine;
    private HorizontalLine? _steeringCenterLine;
    private Crosshair? _gearCrosshair;
    private Crosshair? _throttleBrakeCrosshair;
    private Crosshair? _steeringCrosshair;
    private Marker? _gearHoverMarker;
    private Marker? _throttleHoverMarker;
    private Marker? _brakeHoverMarker;
    private Marker? _steeringHoverMarker;
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
        AddBottomBoundary(GearPlot, 0.5, 8.5);
        var gear = GearPlot.Plot.Add.SignalXY(_times, _gear);
        StyleSeries(gear, GearColor, ConnectStyle.StepHorizontal);
        ConfigurePlot(
            GearPlot,
            0.5,
            8.5,
            [1, 2, 3, 4, 5, 6, 7, 8],
            value => $"{value:0}");
        _gearCrosshair = AddHoverCrosshair(GearPlot);
        _gearHoverMarker = AddHoverMarker(GearPlot, GearColor);

        AddBottomBoundary(
            ThrottleBrakePlot,
            InputAxisMinimum,
            InputAxisMaximum);
        _throttleCenterLine =
            ThrottleBrakePlot.Plot.Add.HorizontalLine(0);
        _throttleCenterLine.LineWidth = 1;

        var throttle = ThrottleBrakePlot.Plot.Add.SignalXY(_times, _throttle);
        StyleSeries(throttle, ThrottleColor, ConnectStyle.StepVertical);

        var brake = ThrottleBrakePlot.Plot.Add.SignalXY(_times, _brake);
        StyleSeries(brake, BrakeColor, ConnectStyle.StepVertical);
        ConfigurePlot(
            ThrottleBrakePlot,
            InputAxisMinimum,
            InputAxisMaximum,
            InputAxisTicks,
            value => $"{Math.Abs(value) * 100:0}%");
        _throttleBrakeCrosshair = AddHoverCrosshair(ThrottleBrakePlot);
        _throttleHoverMarker =
            AddHoverMarker(ThrottleBrakePlot, ThrottleColor);
        _brakeHoverMarker = AddHoverMarker(ThrottleBrakePlot, BrakeColor);

        AddBottomBoundary(
            SteeringPlot,
            InputAxisMinimum,
            InputAxisMaximum);
        _steeringCenterLine = SteeringPlot.Plot.Add.HorizontalLine(0);
        _steeringCenterLine.LineWidth = 1;

        var steering = SteeringPlot.Plot.Add.SignalXY(_times, _steering);
        StyleSeries(steering, SteeringColor, ConnectStyle.Straight);
        ConfigurePlot(
            SteeringPlot,
            InputAxisMinimum,
            InputAxisMaximum,
            InputAxisTicks,
            FormatSteering);
        _steeringCrosshair = AddHoverCrosshair(SteeringPlot);
        _steeringHoverMarker = AddHoverMarker(SteeringPlot, SteeringColor);

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
        series.LineWidth = 2;
        series.MarkerSize = 0;
        series.ConnectStyle = connectStyle;
    }

    private void AddBottomBoundary(
        WinUIPlot plotControl,
        double yMinimum,
        double yMaximum)
    {
        const double insetFraction = .0025;
        var y = yMinimum + (yMaximum - yMinimum) * insetFraction;
        var boundary = plotControl.Plot.Add.HorizontalLine(y);
        boundary.EnableAutoscale = false;
        boundary.LineWidth = .75f;
        _bottomBoundaries.Add(boundary);
    }

    private static Crosshair AddHoverCrosshair(WinUIPlot plotControl)
    {
        var crosshair = plotControl.Plot.Add.Crosshair(0, 0);
        crosshair.EnableAutoscale = false;
        crosshair.IsVisible = false;
        crosshair.LineWidth = 1;
        crosshair.LinePattern = HoverCrosshairPattern;
        crosshair.MarkerSize = 0;
        return crosshair;
    }

    private static Marker AddHoverMarker(
        WinUIPlot plotControl,
        ScottPlot.Color color)
    {
        var marker = plotControl.Plot.Add.Marker(
            0,
            0,
            MarkerShape.OpenCircle,
            7,
            color);
        marker.MarkerLineWidth = 2;
        marker.IsVisible = false;
        return marker;
    }

    private void ConfigurePlot(
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
        ConfigureTimeTicks(plotControl, _windowSeconds);
        plot.Axes.Top.IsVisible = false;
        plot.Axes.Right.IsVisible = false;
        plot.Axes.Margins(0, 0, 0, 0);
        plot.Grid.IsVisible = true;
        plot.Grid.YAxisStyle.IsVisible = false;
        plot.Grid.MajorLineWidth = 1;
        plot.Grid.MinorLineWidth = 0;
        StyleAxis(plot.Axes.Left);
        StyleAxis(plot.Axes.Bottom);
    }

    private static void ConfigureTimeTicks(
        WinUIPlot plotControl,
        int windowSeconds)
    {
        plotControl.Plot.Axes.Bottom.TickGenerator =
            new ScottPlot.TickGenerators.NumericFixedInterval(
                windowSeconds / 6d)
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
        ConfigureTimeTicks(GearPlot, seconds);
        ConfigureTimeTicks(ThrottleBrakePlot, seconds);
        ConfigureTimeTicks(SteeringPlot, seconds);
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
        SetPlotLimits(
            ThrottleBrakePlot,
            xMin,
            xMax,
            InputAxisMinimum,
            InputAxisMaximum);
        SetPlotLimits(
            SteeringPlot,
            xMin,
            xMax,
            InputAxisMinimum,
            InputAxisMaximum);
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
            .WithAlpha(dark ? .05 : .045);
        var centerLine = axes.WithAlpha(dark ? .42 : .35);
        var crosshair = axes.WithAlpha(dark ? .7 : .6);
        if (_throttleCenterLine is not null)
        {
            _throttleCenterLine.Color = centerLine;
        }
        if (_steeringCenterLine is not null)
        {
            _steeringCenterLine.Color = centerLine;
        }
        foreach (var boundary in _bottomBoundaries)
        {
            boundary.Color = axes;
        }
        foreach (var hoverCrosshair in new[]
                 {
                     _gearCrosshair,
                     _throttleBrakeCrosshair,
                     _steeringCrosshair,
                 })
        {
            if (hoverCrosshair is not null)
            {
                hoverCrosshair.LineColor = crosshair;
            }
        }
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
        }
    }

    private void OnGearPointerMoved(
        object sender,
        PointerRoutedEventArgs args)
    {
        if (!TryGetNearestIndex(
                GearPlot,
                args,
                out var index,
                out var position,
                out var pointerCoordinates))
        {
            return;
        }
        GearTooltipTimeText.Text = FormatTime(_times[index]);
        GearTooltipValueRun.Text = $" {Math.Round(_gear[index])}";
        ShowTooltip(GearTooltip, GearPlot, position);
        ShowCrosshair(_gearCrosshair, pointerCoordinates);
        ShowMarker(_gearHoverMarker, _times[index], _gear[index]);
        GearPlot.Refresh();
    }

    private void OnThrottleBrakePointerMoved(
        object sender,
        PointerRoutedEventArgs args)
    {
        if (!TryGetNearestIndex(
                ThrottleBrakePlot,
                args,
                out var index,
                out var position,
                out var pointerCoordinates))
        {
            return;
        }
        ThrottleBrakeTooltipTimeText.Text = FormatTime(_times[index]);
        ThrottleBrakeTooltipThrottleValueRun.Text =
            $" {Math.Round(_throttle[index] * 100)}%";
        ThrottleBrakeTooltipBrakeValueRun.Text =
            $" {Math.Round(Math.Abs(_brake[index]) * 100)}%";
        ShowTooltip(ThrottleBrakeTooltip, ThrottleBrakePlot, position);
        ShowCrosshair(_throttleBrakeCrosshair, pointerCoordinates);
        ShowMarker(_throttleHoverMarker, _times[index], _throttle[index]);
        ShowMarker(_brakeHoverMarker, _times[index], _brake[index]);
        ThrottleBrakePlot.Refresh();
    }

    private void OnSteeringPointerMoved(
        object sender,
        PointerRoutedEventArgs args)
    {
        if (!TryGetNearestIndex(
                SteeringPlot,
                args,
                out var index,
                out var position,
                out var pointerCoordinates))
        {
            return;
        }
        SteeringTooltipTimeText.Text = FormatTime(_times[index]);
        SteeringTooltipValueRun.Text =
            $" {FormatSteering(_steering[index])}";
        ShowTooltip(SteeringTooltip, SteeringPlot, position);
        ShowCrosshair(_steeringCrosshair, pointerCoordinates);
        ShowMarker(_steeringHoverMarker, _times[index], _steering[index]);
        SteeringPlot.Refresh();
    }

    private bool TryGetNearestIndex(
        WinUIPlot plot,
        PointerRoutedEventArgs args,
        out int index,
        out Windows.Foundation.Point position,
        out Coordinates pointerCoordinates)
    {
        position = args.GetCurrentPoint(plot).Position;
        pointerCoordinates = default;
        index = -1;
        if (_times.Count == 0)
        {
            return false;
        }

        var scale = XamlRoot?.RasterizationScale ?? 1;
        pointerCoordinates = plot.Plot.GetCoordinates(
            new Pixel(
                (float)(position.X * scale),
                (float)(position.Y * scale)));
        var candidate = _times.BinarySearch(pointerCoordinates.X);
        if (candidate < 0)
        {
            candidate = ~candidate;
        }
        if (candidate >= _times.Count)
        {
            candidate = _times.Count - 1;
        }
        if (candidate > 0 &&
            Math.Abs(_times[candidate - 1] - pointerCoordinates.X) <
            Math.Abs(_times[candidate] - pointerCoordinates.X))
        {
            candidate--;
        }
        index = candidate;
        return true;
    }

    private static void ShowCrosshair(
        Crosshair? crosshair,
        Coordinates coordinates)
    {
        if (crosshair is null)
        {
            return;
        }
        crosshair.Position = coordinates;
        crosshair.IsVisible = true;
    }

    private static void ShowMarker(
        Marker? marker,
        double x,
        double y)
    {
        if (marker is null)
        {
            return;
        }
        marker.Position = new Coordinates(x, y);
        marker.IsVisible = true;
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
        var refreshGear = _gearCrosshair?.IsVisible == true;
        var refreshThrottleBrake = _throttleBrakeCrosshair?.IsVisible == true;
        var refreshSteering = _steeringCrosshair?.IsVisible == true;
        SetHoverVisibility(_gearCrosshair, false);
        SetHoverVisibility(_throttleBrakeCrosshair, false);
        SetHoverVisibility(_steeringCrosshair, false);
        SetHoverVisibility(_gearHoverMarker, false);
        SetHoverVisibility(_throttleHoverMarker, false);
        SetHoverVisibility(_brakeHoverMarker, false);
        SetHoverVisibility(_steeringHoverMarker, false);
        if (refreshGear)
        {
            GearPlot.Refresh();
        }
        if (refreshThrottleBrake)
        {
            ThrottleBrakePlot.Refresh();
        }
        if (refreshSteering)
        {
            SteeringPlot.Refresh();
        }
    }

    private static void SetHoverVisibility(
        IPlottable? plottable,
        bool isVisible)
    {
        if (plottable is not null)
        {
            plottable.IsVisible = isVisible;
        }
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
