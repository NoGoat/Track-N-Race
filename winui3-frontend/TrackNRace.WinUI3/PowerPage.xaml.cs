using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using ScottPlot;
using ScottPlot.Plottables;
using ScottPlot.WinUI;
using System.Diagnostics;

namespace TrackNRace.WinUI3;

public sealed partial class PowerPage : Page
{
    private const double DefaultHarvestUpper = 8000;
    private static readonly LinePattern HoverCrosshairPattern =
        new([2f, 1f], 0, "ElectronCrosshair");
    private static readonly ScottPlot.Color IceColor =
        ScottPlot.Color.FromHex("#5794F2");
    private static readonly ScottPlot.Color MgukColor =
        ScottPlot.Color.FromHex("#FADE2A");
    private static readonly ScottPlot.Color HarvestMgukColor =
        ScottPlot.Color.FromHex("#37872D");
    private static readonly ScottPlot.Color HarvestMguhColor =
        ScottPlot.Color.FromHex("#C4162A");
    private static readonly ScottPlot.Color FuelColor =
        ScottPlot.Color.FromHex("#F0A500");

    private readonly List<double> _times = [];
    private readonly List<double> _ice = [];
    private readonly List<double> _mguk = [];
    private readonly List<double> _harvestMguk = [];
    private readonly List<double> _harvestMguh = [];
    private readonly List<double> _ers = [];
    private readonly List<double> _fuel = [];
    private readonly List<HorizontalLine> _bottomBoundaries = [];

    private SignalXY? _harvestMguhSeries;
    private Crosshair? _powerSplitCrosshair;
    private Crosshair? _harvestCrosshair;
    private Crosshair? _ersCrosshair;
    private Crosshair? _fuelCrosshair;
    private Marker? _iceMarker;
    private Marker? _mgukMarker;
    private Marker? _harvestMgukMarker;
    private Marker? _harvestMguhMarker;
    private Marker? _ersMarker;
    private Marker? _fuelMarker;
    private TelemetrySessionStore? _store;
    private int _storeCount;
    private long _bufferEpoch = -1;
    private long _timelineRevision = -1;
    private bool _isLoaded;
    private bool _hasMguh;
    private int _windowSeconds;
    private double _harvestUpper = DefaultHarvestUpper;
    private double _fuelUpper = 1;
    private int _refreshQueued;

    public PowerPage()
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
            _store.PowerChanged += OnPowerChanged;
            _store.TimelineReset += OnTimelineReset;
        }
        app.ChartWindowChanged += OnChartWindowChanged;
        app.PowerDisplayChanged += OnPowerDisplayChanged;
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
            _store.PowerChanged -= OnPowerChanged;
            _store.TimelineReset -= OnTimelineReset;
            _store = null;
        }
        var app = (App)Application.Current;
        app.ChartWindowChanged -= OnChartWindowChanged;
        app.PowerDisplayChanged -= OnPowerDisplayChanged;
    }

    private void ConfigurePlots()
    {
        AddBottomBoundary(PowerSplitPlot, 0, 1000);
        StyleSeries(
            PowerSplitPlot.Plot.Add.SignalXY(_times, _ice),
            IceColor);
        StyleSeries(
            PowerSplitPlot.Plot.Add.SignalXY(_times, _mguk),
            MgukColor);
        ConfigurePlot(
            PowerSplitPlot,
            0,
            1000,
            [0, 250, 500, 750, 1000],
            value => $"{value:0}kW");
        _powerSplitCrosshair = AddHoverCrosshair(PowerSplitPlot);
        _iceMarker = AddHoverMarker(PowerSplitPlot, IceColor);
        _mgukMarker = AddHoverMarker(PowerSplitPlot, MgukColor);

        AddBottomBoundary(ErsHarvestPlot, 0, DefaultHarvestUpper);
        StyleSeries(
            ErsHarvestPlot.Plot.Add.SignalXY(_times, _harvestMguk),
            HarvestMgukColor);
        _harvestMguhSeries =
            ErsHarvestPlot.Plot.Add.SignalXY(_times, _harvestMguh);
        StyleSeries(_harvestMguhSeries, HarvestMguhColor);
        ConfigurePlot(
            ErsHarvestPlot,
            0,
            DefaultHarvestUpper,
            [0, 2000, 4000, 6000, 8000],
            value => $"{value:0}kJ");
        _harvestCrosshair = AddHoverCrosshair(ErsHarvestPlot);
        _harvestMgukMarker =
            AddHoverMarker(ErsHarvestPlot, HarvestMgukColor);
        _harvestMguhMarker =
            AddHoverMarker(ErsHarvestPlot, HarvestMguhColor);

        AddBottomBoundary(ErsStorePlot, 0, 100);
        StyleSeries(
            ErsStorePlot.Plot.Add.SignalXY(_times, _ers),
            IceColor);
        ConfigurePlot(
            ErsStorePlot,
            0,
            100,
            [0, 25, 50, 75, 100],
            value => $"{value:0}%");
        _ersCrosshair = AddHoverCrosshair(ErsStorePlot);
        _ersMarker = AddHoverMarker(ErsStorePlot, IceColor);

        AddBottomBoundary(FuelPlot, 0, 1);
        StyleSeries(
            FuelPlot.Plot.Add.SignalXY(_times, _fuel),
            FuelColor);
        ConfigurePlot(
            FuelPlot,
            0,
            1,
            [0, .25, .5, .75, 1],
            value => $"{value:0.0}kg");
        _fuelCrosshair = AddHoverCrosshair(FuelPlot);
        _fuelMarker = AddHoverMarker(FuelPlot, FuelColor);

#if DEBUG
        AttachRenderProbe(PowerSplitPlot, "Power/Split");
        AttachRenderProbe(ErsHarvestPlot, "Power/Harvest");
        AttachRenderProbe(ErsStorePlot, "Power/Store");
        AttachRenderProbe(FuelPlot, "Power/Fuel");
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
        ScottPlot.Color color)
    {
        series.Color = color;
        series.LineWidth = 2;
        series.MarkerSize = 0;
        series.ConnectStyle = ConnectStyle.Straight;
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

    private void OnPowerChanged()
    {
        if (!_isLoaded || Interlocked.Exchange(ref _refreshQueued, 1) != 0)
        {
            return;
        }

        if (!DispatcherQueue.TryEnqueue(() =>
            {
                Interlocked.Exchange(ref _refreshQueued, 0);
                if (_isLoaded)
                {
                    RefreshFromStore();
                }
            }))
        {
            Interlocked.Exchange(ref _refreshQueued, 0);
        }
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
        foreach (var plot in AllPlots())
        {
            ConfigureTimeTicks(plot, seconds);
        }
        if (_isLoaded)
        {
            RefreshFromStore();
        }
    }

    private void OnPowerDisplayChanged()
    {
        if (_isLoaded)
        {
            RenderCards(_store?.PowerSnapshot);
        }
    }

    private void RefreshFromStore()
    {
        if (!_isLoaded || _store is null)
        {
            return;
        }

        var snapshot = _store.PowerSnapshot;
        var read = _store.ReadPower(
            _storeCount,
            _bufferEpoch,
            _timelineRevision);
        if (read.Reset)
        {
            ClearData();
        }

        foreach (var row in read.Rows)
        {
            _times.Add(row.SessionTime);
            _ice.Add(row.EnginePowerIceKw);
            _mguk.Add(row.EnginePowerMgukKw);
            _harvestMguk.Add(row.ErsHarvestedMgukJ / 1000d);
            _harvestMguh.Add(row.ErsHarvestedMguhJ / 1000d);
            _ers.Add(row.ErsPct);
            _fuel.Add(row.FuelKg);
        }

        _storeCount = read.TotalCount;
        _bufferEpoch = read.BufferEpoch;
        _timelineRevision = read.TimelineRevision;
        _hasMguh = snapshot.HasMguh;
        if (_harvestMguhSeries is not null)
        {
            _harvestMguhSeries.IsVisible = _hasMguh;
        }
        if (!_hasMguh && _harvestMguhMarker is not null)
        {
            _harvestMguhMarker.IsVisible = false;
        }
        MguhLegend.Visibility = _hasMguh
            ? Visibility.Visible
            : Visibility.Collapsed;
        ErsHarvestTooltipMguhRow.Visibility = MguhLegend.Visibility;

        UpdateAxisRanges(snapshot);
        RenderCards(snapshot);
        var hasData = _times.Count > 0;
        foreach (var emptyState in new[]
                 {
                     PowerSplitNoData,
                     ErsHarvestNoData,
                     ErsStoreNoData,
                     FuelNoData,
                 })
        {
            emptyState.Visibility =
                hasData ? Visibility.Collapsed : Visibility.Visible;
        }

        var latest = hasData ? _times[^1] : 0;
        var xMin = Math.Max(0, latest - _windowSeconds);
        var xMax = Math.Max(_windowSeconds, latest);
        SetPlotLimits(PowerSplitPlot, xMin, xMax, 0, 1000);
        SetPlotLimits(ErsHarvestPlot, xMin, xMax, 0, _harvestUpper);
        SetPlotLimits(ErsStorePlot, xMin, xMax, 0, 100);
        SetPlotLimits(FuelPlot, xMin, xMax, 0, _fuelUpper);
        RefreshPlots();
    }

    private void UpdateAxisRanges(PowerSnapshot snapshot)
    {
        var observedHarvest = _harvestMguk.Count > 0
            ? _harvestMguk.Max()
            : 0;
        if (_hasMguh && _harvestMguh.Count > 0)
        {
            observedHarvest = Math.Max(observedHarvest, _harvestMguh.Max());
        }
        if (observedHarvest > _harvestUpper)
        {
            _harvestUpper = Math.Ceiling(observedHarvest / 2000) * 2000;
            var ticks = Enumerable.Range(0, 5)
                .Select(index => index * _harvestUpper / 4)
                .ToArray();
            ErsHarvestPlot.Plot.Axes.Left.SetTicks(
                ticks,
                ticks.Select(value => $"{value:0}kJ").ToArray());
        }

        var fuelUpper = snapshot.FuelUpperLimit ??
            (_fuel.Count > 0 ? Math.Max(1, _fuel[0] + 1) : 1);
        if (Math.Abs(fuelUpper - _fuelUpper) > .0001)
        {
            _fuelUpper = Math.Max(1, fuelUpper);
            var ticks = Enumerable.Range(0, 5)
                .Select(index => index * _fuelUpper / 4)
                .ToArray();
            FuelPlot.Plot.Axes.Left.SetTicks(
                ticks,
                ticks.Select(value => $"{value:0.0}kg").ToArray());
        }
    }

    private void ClearData()
    {
        _times.Clear();
        _ice.Clear();
        _mguk.Clear();
        _harvestMguk.Clear();
        _harvestMguh.Clear();
        _ers.Clear();
        _fuel.Clear();
        _storeCount = 0;
        _harvestUpper = DefaultHarvestUpper;
        _fuelUpper = 1;
        ErsHarvestPlot.Plot.Axes.Left.SetTicks(
            [0, 2000, 4000, 6000, 8000],
            ["0kJ", "2000kJ", "4000kJ", "6000kJ", "8000kJ"]);
        HideTooltips();
    }

    private void RenderCards(PowerSnapshot? snapshot)
    {
        StatsGrid.Children.Clear();
        StatsGrid.ColumnDefinitions.Clear();
        var status = snapshot?.Latest;
        var hasStatus = status is not null;
        var ice = status?.EnginePowerIceKw ?? 0;
        var mguk = status?.EnginePowerMgukKw ?? 0;
        var total = ice + mguk;
        var ers = status?.ErsPct ?? 0;
        var iceSplit = total > 0 ? ice / total * 100 : 0;
        var mgukSplit = total > 0 ? mguk / total * 100 : 0;
        var values = new[]
        {
            ("TOTAL POWER", hasStatus ? $"{total:0}" : "—", hasStatus ? "kW" : "", "power.total", total),
            ("ICE", hasStatus ? $"{ice:0}" : "—", hasStatus ? "kW" : "", "power.ice", ice),
            ("MGU-K", hasStatus ? $"{mguk:0}" : "—", hasStatus ? "kW" : "", "power.mguk", mguk),
            ("SPLIT", hasStatus ? $"{iceSplit:0}:{mgukSplit:0}" : "—", "", "power.split", total),
            ("ERS STORE", hasStatus ? $"{ers / 100 * 4:0.00}" : "—", hasStatus ? "MJ" : "", "power.ers", ers),
            ("ERS %", hasStatus ? $"{ers:0}" : "—", hasStatus ? "%" : "", "power.ers", ers),
            ("FUEL", hasStatus ? $"{status!.FuelKg:0.0}" : "—", hasStatus ? "kg" : "", "power.fuel", status?.FuelKg ?? 0),
        };
        var compact = ((App)Application.Current).PowerDisplay.CompactCards;
        StatsGrid.MinHeight = compact ? 42 : 72;
        for (var index = 0; index < values.Length; index++)
        {
            StatsGrid.ColumnDefinitions.Add(new ColumnDefinition
            {
                Width = new GridLength(1, GridUnitType.Star),
            });
            var item = values[index];
            var valueBrush = snapshot is null
                ? PrimaryBrush()
                : TelemetryColors.Resolve(
                    snapshot.CardColors,
                    item.Item4,
                    item.Item5,
                    ActualTheme == ElementTheme.Dark,
                    PrimaryBrush());
            FrameworkElement card;
            if (compact)
            {
                card = new Grid
                {
                    Padding = new Thickness(10, 8, 10, 8),
                    ColumnDefinitions =
                    {
                        new ColumnDefinition
                        {
                            Width = new GridLength(1, GridUnitType.Star),
                        },
                        new ColumnDefinition { Width = GridLength.Auto },
                    },
                    Children =
                    {
                        new TextBlock
                        {
                            Text = item.Item1,
                            Style = (Style)Resources["PowerHeadingStyle"],
                            FontSize = 10,
                            TextTrimming = TextTrimming.CharacterEllipsis,
                            VerticalAlignment =
                                Microsoft.UI.Xaml.VerticalAlignment.Center,
                        },
                        CreateInlineValue(
                            item.Item2,
                            item.Item3,
                            15,
                            valueBrush,
                            1),
                    },
                };
            }
            else
            {
                card = new StackPanel
                {
                    Padding = new Thickness(12, 10, 12, 10),
                    Children =
                    {
                        new TextBlock
                        {
                            Text = item.Item1,
                            Style = (Style)Resources["PowerHeadingStyle"],
                            TextTrimming = TextTrimming.CharacterEllipsis,
                        },
                        CreateInlineValue(
                            item.Item2,
                            item.Item3,
                            22,
                            valueBrush),
                    },
                };
            }

            var wrapper = new Border
            {
                BorderBrush = DividerBrush(),
                BorderThickness = index < values.Length - 1
                    ? new Thickness(0, 0, 1, 0)
                    : new Thickness(0),
                Child = card,
            };
            Grid.SetColumn(wrapper, index);
            StatsGrid.Children.Add(wrapper);
        }
    }

    private static TextBlock CreateInlineValue(
        string value,
        string unit,
        double size,
        Brush brush,
        int gridColumn = 0)
    {
        var block = new TextBlock
        {
            Margin = gridColumn == 0
                ? new Thickness(0, 5, 0, 0)
                : new Thickness(5, 0, 0, 0),
            FontSize = size,
            FontWeight = Microsoft.UI.Text.FontWeights.Bold,
            Foreground = brush,
            Text = string.IsNullOrEmpty(unit) ? value : $"{value} {unit}",
            TextTrimming = TextTrimming.CharacterEllipsis,
            HorizontalAlignment = gridColumn == 0
                ? Microsoft.UI.Xaml.HorizontalAlignment.Left
                : Microsoft.UI.Xaml.HorizontalAlignment.Right,
            VerticalAlignment = Microsoft.UI.Xaml.VerticalAlignment.Center,
        };
        Grid.SetColumn(block, gridColumn);
        return block;
    }

    private static void SetPlotLimits(
        WinUIPlot plotControl,
        double xMin,
        double xMax,
        double yMin,
        double yMax) =>
        plotControl.Plot.Axes.SetLimits(xMin, xMax, yMin, yMax);

    private void OnActualThemeChanged(FrameworkElement sender, object args)
    {
        ApplyTheme();
        if (_isLoaded)
        {
            RenderCards(_store?.PowerSnapshot);
            RefreshPlots();
        }
    }

    private void ApplyTheme()
    {
        if (PowerSplitPlot is null)
        {
            return;
        }

        var dark = ActualTheme == ElementTheme.Dark;
        var transparent = ScottPlot.Color.FromHex("#00000000");
        var axes = ScottPlot.Color.FromHex(dark ? "#7C8098" : "#6B7280");
        var grid = ScottPlot.Color.FromHex(dark ? "#FFFFFF" : "#000000")
            .WithAlpha(dark ? .05 : .045);
        var crosshair = axes.WithAlpha(dark ? .7 : .6);
        foreach (var boundary in _bottomBoundaries)
        {
            boundary.Color = axes;
        }
        foreach (var hoverCrosshair in new[]
                 {
                     _powerSplitCrosshair,
                     _harvestCrosshair,
                     _ersCrosshair,
                     _fuelCrosshair,
                 })
        {
            if (hoverCrosshair is not null)
            {
                hoverCrosshair.LineColor = crosshair;
            }
        }
        foreach (var control in AllPlots())
        {
            var plot = control.Plot;
            plot.FigureBackground.Color = transparent;
            plot.DataBackground.Color = transparent;
            plot.Axes.Color(axes);
            plot.Grid.MajorLineColor = grid;
        }
    }

    private WinUIPlot[] AllPlots() =>
        [PowerSplitPlot, ErsHarvestPlot, ErsStorePlot, FuelPlot];

    private void RefreshPlots()
    {
        foreach (var plot in AllPlots())
        {
            plot.Refresh();
        }
    }

    private void OnPowerSplitPointerMoved(
        object sender,
        PointerRoutedEventArgs args)
    {
        if (!TryGetNearestIndex(
                PowerSplitPlot,
                args,
                out var index,
                out var position,
                out var coordinates))
        {
            return;
        }

        PowerSplitTooltipTime.Text = FormatTime(_times[index]);
        PowerSplitTooltipIce.Text = $" {_ice[index]:0.0} kW";
        PowerSplitTooltipMguk.Text = $" {_mguk[index]:0.0} kW";
        PowerSplitTooltipTotal.Text =
            $" {_ice[index] + _mguk[index]:0.0} kW";
        ShowTooltip(PowerSplitTooltip, PowerSplitPlot, position);
        ShowCrosshair(_powerSplitCrosshair, coordinates);
        ShowMarker(_iceMarker, _times[index], _ice[index]);
        ShowMarker(_mgukMarker, _times[index], _mguk[index]);
        PowerSplitPlot.Refresh();
    }

    private void OnErsHarvestPointerMoved(
        object sender,
        PointerRoutedEventArgs args)
    {
        if (!TryGetNearestIndex(
                ErsHarvestPlot,
                args,
                out var index,
                out var position,
                out var coordinates))
        {
            return;
        }

        ErsHarvestTooltipTime.Text = FormatTime(_times[index]);
        ErsHarvestTooltipMguk.Text = $" {_harvestMguk[index]:0.0} kJ";
        ErsHarvestTooltipMguh.Text = $" {_harvestMguh[index]:0.0} kJ";
        var total = _harvestMguk[index] +
            (_hasMguh ? _harvestMguh[index] : 0);
        ErsHarvestTooltipTotal.Text = $" {total:0.0} kJ";
        ShowTooltip(ErsHarvestTooltip, ErsHarvestPlot, position);
        ShowCrosshair(_harvestCrosshair, coordinates);
        ShowMarker(
            _harvestMgukMarker,
            _times[index],
            _harvestMguk[index]);
        if (_hasMguh)
        {
            ShowMarker(
                _harvestMguhMarker,
                _times[index],
                _harvestMguh[index]);
        }
        ErsHarvestPlot.Refresh();
    }

    private void OnErsStorePointerMoved(
        object sender,
        PointerRoutedEventArgs args)
    {
        if (!TryGetNearestIndex(
                ErsStorePlot,
                args,
                out var index,
                out var position,
                out var coordinates))
        {
            return;
        }

        ErsStoreTooltipTime.Text = FormatTime(_times[index]);
        ErsStoreTooltipPercent.Text = $" {_ers[index]:0.0}%";
        ErsStoreTooltipEnergy.Text =
            $" {_ers[index] / 100 * 4:0.00} / 4.00 MJ";
        ShowTooltip(ErsStoreTooltip, ErsStorePlot, position);
        ShowCrosshair(_ersCrosshair, coordinates);
        ShowMarker(_ersMarker, _times[index], _ers[index]);
        ErsStorePlot.Refresh();
    }

    private void OnFuelPointerMoved(
        object sender,
        PointerRoutedEventArgs args)
    {
        if (!TryGetNearestIndex(
                FuelPlot,
                args,
                out var index,
                out var position,
                out var coordinates))
        {
            return;
        }

        FuelTooltipTime.Text = FormatTime(_times[index]);
        FuelTooltipValue.Text = $" {_fuel[index]:0.00} kg";
        ShowTooltip(FuelTooltip, FuelPlot, position);
        ShowCrosshair(_fuelCrosshair, coordinates);
        ShowMarker(_fuelMarker, _times[index], _fuel[index]);
        FuelPlot.Refresh();
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
        PowerSplitTooltip.Visibility = Visibility.Collapsed;
        ErsHarvestTooltip.Visibility = Visibility.Collapsed;
        ErsStoreTooltip.Visibility = Visibility.Collapsed;
        FuelTooltip.Visibility = Visibility.Collapsed;
        var refresh = new[]
        {
            _powerSplitCrosshair?.IsVisible == true,
            _harvestCrosshair?.IsVisible == true,
            _ersCrosshair?.IsVisible == true,
            _fuelCrosshair?.IsVisible == true,
        };
        foreach (var plottable in new IPlottable?[]
                 {
                     _powerSplitCrosshair,
                     _harvestCrosshair,
                     _ersCrosshair,
                     _fuelCrosshair,
                     _iceMarker,
                     _mgukMarker,
                     _harvestMgukMarker,
                     _harvestMguhMarker,
                     _ersMarker,
                     _fuelMarker,
                 })
        {
            if (plottable is not null)
            {
                plottable.IsVisible = false;
            }
        }
        var plots = AllPlots();
        for (var index = 0; index < plots.Length; index++)
        {
            if (refresh[index])
            {
                plots[index].Refresh();
            }
        }
    }

    private static string FormatTime(double seconds)
    {
        var safe = Math.Max(0, seconds);
        return $"{(int)(safe / 60)}:{(int)(safe % 60):00}";
    }

    private Brush PrimaryBrush() => new SolidColorBrush(
        ActualTheme == ElementTheme.Dark
            ? Windows.UI.Color.FromArgb(255, 255, 255, 255)
            : Windows.UI.Color.FromArgb(255, 0, 0, 0));

    private Brush DividerBrush() => new SolidColorBrush(
        ActualTheme == ElementTheme.Dark
            ? Windows.UI.Color.FromArgb(255, 42, 46, 58)
            : Windows.UI.Color.FromArgb(255, 217, 220, 227));
}
