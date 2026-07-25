using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using System.Diagnostics;
using TrackNRace.Charting;
using UiColor = Windows.UI.Color;

namespace TrackNRace.WinUI3;

public sealed partial class PowerPage : Page
{
    private const int MaxChartPoints = 750_000;
    private const double RetentionSeconds = 600;
    private const double DefaultHarvestUpper = 8000;
    private static readonly UiColor IceColor =
        UiColor.FromArgb(255, 87, 148, 242);
    private static readonly UiColor MgukColor =
        UiColor.FromArgb(255, 250, 222, 42);
    private static readonly UiColor HarvestMgukColor =
        UiColor.FromArgb(255, 55, 135, 45);
    private static readonly UiColor HarvestMguhColor =
        UiColor.FromArgb(255, 196, 22, 42);
    private static readonly UiColor FuelColor =
        UiColor.FromArgb(255, 240, 165, 0);
    private static readonly UiColor NeutralColor =
        UiColor.FromArgb(255, 160, 168, 184);

    private readonly List<double> _times = [];
    private readonly List<double> _ice = [];
    private readonly List<double> _mguk = [];
    private readonly List<double> _harvestMguk = [];
    private readonly List<double> _harvestMguh = [];
    private readonly List<double> _ers = [];
    private readonly List<double> _fuel = [];

    private ChartLineSeries _iceSeries = null!;
    private ChartLineSeries _mgukSeries = null!;
    private ChartLineSeries _harvestMgukSeries = null!;
    private ChartLineSeries _harvestMguhSeries = null!;
    private ChartLineSeries _ersSeries = null!;
    private ChartLineSeries _fuelSeries = null!;
    private ChartAxis _powerTimeAxis = null!;
    private ChartAxis _harvestTimeAxis = null!;
    private ChartAxis _ersTimeAxis = null!;
    private ChartAxis _fuelTimeAxis = null!;
    private ChartAxis _powerAxis = null!;
    private ChartAxis _harvestAxis = null!;
    private ChartAxis _ersAxis = null!;
    private ChartAxis _fuelAxis = null!;
    private ChartCrosshairTooltipPlugin _powerTooltip = null!;
    private ChartCrosshairTooltipPlugin _harvestTooltip = null!;
    private ChartCrosshairTooltipPlugin _ersTooltip = null!;
    private ChartCrosshairTooltipPlugin _fuelTooltip = null!;
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
        _powerTimeAxis = CreateTimeAxis();
        _powerAxis = CreateValueAxis(
            "power", 0, 1000,
            [0, 250, 500, 750, 1000],
            value => $"{value:0}kW");
        ConfigureChart(
            PowerSplitPlot, _powerTimeAxis, _powerAxis, "Power/Split");
        _iceSeries = AddSeries(
            PowerSplitPlot, "ice", "power", IceColor);
        _mgukSeries = AddSeries(
            PowerSplitPlot, "mguk", "power", MgukColor);
        _powerTooltip =
            new ChartCrosshairTooltipPlugin(BuildPowerTooltip);
        PowerSplitPlot.Plugins.Add(_powerTooltip);

        _harvestTimeAxis = CreateTimeAxis();
        _harvestAxis = CreateValueAxis(
            "harvest", 0, DefaultHarvestUpper,
            [0, 2000, 4000, 6000, 8000],
            value => $"{value:0}kJ");
        ConfigureChart(
            ErsHarvestPlot,
            _harvestTimeAxis,
            _harvestAxis,
            "Power/Harvest");
        _harvestMgukSeries = AddSeries(
            ErsHarvestPlot, "harvest-mguk", "harvest", HarvestMgukColor);
        _harvestMguhSeries = AddSeries(
            ErsHarvestPlot, "harvest-mguh", "harvest", HarvestMguhColor);
        _harvestTooltip =
            new ChartCrosshairTooltipPlugin(BuildHarvestTooltip);
        ErsHarvestPlot.Plugins.Add(_harvestTooltip);

        _ersTimeAxis = CreateTimeAxis();
        _ersAxis = CreateValueAxis(
            "ers", 0, 100,
            [0, 25, 50, 75, 100],
            value => $"{value:0}%");
        ConfigureChart(ErsStorePlot, _ersTimeAxis, _ersAxis, "Power/Store");
        _ersSeries = AddSeries(
            ErsStorePlot, "ers", "ers", IceColor);
        _ersTooltip = new ChartCrosshairTooltipPlugin(BuildErsTooltip);
        ErsStorePlot.Plugins.Add(_ersTooltip);

        _fuelTimeAxis = CreateTimeAxis();
        _fuelAxis = CreateValueAxis(
            "fuel", 0, 1,
            [0, .25, .5, .75, 1],
            value => $"{value:0.0}kg");
        ConfigureChart(FuelPlot, _fuelTimeAxis, _fuelAxis, "Power/Fuel");
        _fuelSeries = AddSeries(
            FuelPlot, "fuel", "fuel", FuelColor);
        _fuelTooltip = new ChartCrosshairTooltipPlugin(BuildFuelTooltip);
        FuelPlot.Plugins.Add(_fuelTooltip);

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

    private static ChartAxis CreateValueAxis(
        string key,
        double minimum,
        double maximum,
        double[] ticks,
        Func<double, string> formatter) =>
        new(key, ChartAxisOrientation.Y, ChartAxisSide.Left)
        {
            Minimum = minimum,
            Maximum = maximum,
            TickProvider = new FixedChartTickProvider(ticks),
            LabelFormatter = formatter,
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

    private static ChartLineSeries AddSeries(
        GpuChart chart,
        string key,
        string yAxisKey,
        UiColor color) =>
        chart.Series.Add(new ChartLineSeriesOptions(
            key,
            "time",
            yAxisKey,
            color,
            Thickness: 1.5f,
            MaximumPointCount: MaxChartPoints,
            MaximumXSpan: RetentionSeconds));

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

    private void OnPowerChanged() => QueueRefresh();

    private void OnTimelineReset(TimelineResetReason reason) => QueueRefresh();

    private void QueueRefresh()
    {
        if (!_isLoaded || Interlocked.Exchange(ref _refreshQueued, 1) != 0)
        {
            return;
        }
        if (!DispatcherQueue.TryEnqueue(() =>
            {
                Interlocked.Exchange(ref _refreshQueued, 0);
                RefreshFromStore();
            }))
        {
            Interlocked.Exchange(ref _refreshQueued, 0);
        }
    }

    private void OnChartWindowChanged(int seconds)
    {
        _windowSeconds = seconds;
        var ticks = new IntervalChartTickProvider(seconds / 6d);
        foreach (var axis in TimeAxes())
        {
            axis.TickProvider = ticks;
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

        var firstNewIndex = _times.Count;
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
        AppendNewPoints(firstNewIndex);

        _storeCount = read.TotalCount;
        _bufferEpoch = read.BufferEpoch;
        _timelineRevision = read.TimelineRevision;
        _hasMguh = snapshot.HasMguh;
        _harvestMguhSeries.Visible = _hasMguh;
        MguhLegend.Visibility = _hasMguh
            ? Visibility.Visible
            : Visibility.Collapsed;

        UpdateAxisRanges(snapshot);
        RenderCards(snapshot);
        var hasData = _times.Count > 0;
        foreach (var emptyState in EmptyStates())
        {
            emptyState.Visibility =
                hasData ? Visibility.Collapsed : Visibility.Visible;
        }

        var latest = hasData ? _times[^1] : 0;
        var xMin = Math.Max(0, latest - _windowSeconds);
        var xMax = Math.Max(_windowSeconds, latest);
        foreach (var axis in TimeAxes())
        {
            SetRange(axis, xMin, xMax);
        }
        InvalidatePlots();
    }

    private void AppendNewPoints(int firstNewIndex)
    {
        var count = _times.Count - firstNewIndex;
        if (count <= 0)
        {
            return;
        }
        _iceSeries.Append(BuildPoints(_ice, firstNewIndex, count));
        _mgukSeries.Append(BuildPoints(_mguk, firstNewIndex, count));
        _harvestMgukSeries.Append(
            BuildPoints(_harvestMguk, firstNewIndex, count));
        _harvestMguhSeries.Append(
            BuildPoints(_harvestMguh, firstNewIndex, count));
        _ersSeries.Append(BuildPoints(_ers, firstNewIndex, count));
        _fuelSeries.Append(BuildPoints(_fuel, firstNewIndex, count));
    }

    private ChartPoint[] BuildPoints(
        IReadOnlyList<double> values,
        int firstNewIndex,
        int count)
    {
        var points = new ChartPoint[count];
        for (var index = 0; index < count; index++)
        {
            var sourceIndex = firstNewIndex + index;
            points[index] =
                new ChartPoint(_times[sourceIndex], values[sourceIndex]);
        }
        return points;
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
            _harvestAxis.Maximum = _harvestUpper;
            _harvestAxis.TickProvider = new FixedChartTickProvider(
                Enumerable.Range(0, 5)
                    .Select(index => index * _harvestUpper / 4)
                    .ToArray());
        }

        var fuelUpper = snapshot.FuelUpperLimit ??
            (_fuel.Count > 0 ? Math.Max(1, _fuel[0] + 1) : 1);
        if (Math.Abs(fuelUpper - _fuelUpper) > .0001)
        {
            _fuelUpper = Math.Max(1, fuelUpper);
            _fuelAxis.Maximum = _fuelUpper;
            _fuelAxis.TickProvider = new FixedChartTickProvider(
                Enumerable.Range(0, 5)
                    .Select(index => index * _fuelUpper / 4)
                    .ToArray());
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
        foreach (var series in AllSeries())
        {
            series.Clear();
        }
        _storeCount = 0;
        _harvestUpper = DefaultHarvestUpper;
        _fuelUpper = 1;
        _harvestAxis.Maximum = DefaultHarvestUpper;
        _harvestAxis.TickProvider =
            new FixedChartTickProvider(0, 2000, 4000, 6000, 8000);
        _fuelAxis.Maximum = 1;
        _fuelAxis.TickProvider =
            new FixedChartTickProvider(0, .25, .5, .75, 1);
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
                            VerticalAlignment = VerticalAlignment.Center,
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
                ? HorizontalAlignment.Left
                : HorizontalAlignment.Right,
            VerticalAlignment = VerticalAlignment.Center,
        };
        Grid.SetColumn(block, gridColumn);
        return block;
    }

    private void OnActualThemeChanged(FrameworkElement sender, object args)
    {
        ApplyTheme();
        if (_isLoaded)
        {
            RenderCards(_store?.PowerSnapshot);
            InvalidatePlots();
        }
    }

    private void ApplyTheme()
    {
        if (PowerSplitPlot is null)
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
        foreach (var axis in TimeAxes().Concat(ValueAxes()))
        {
            axis.Color = axes;
        }
        foreach (var chart in AllPlots())
        {
            chart.GridColor = grid;
        }
        foreach (var tooltip in Tooltips())
        {
            tooltip.ApplyTheme(dark);
        }
    }

    private ChartTooltipData? BuildPowerTooltip(double x)
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
                        "ICE", $"{_ice[index]:0.0} kW", IceColor),
                    new ChartTooltipEntry(
                        "MGU-K", $"{_mguk[index]:0.0} kW", MgukColor),
                    new ChartTooltipEntry(
                        "Total",
                        $"{_ice[index] + _mguk[index]:0.0} kW",
                        NeutralColor),
                ]),
            [
                new ChartTooltipMarker(
                    _times[index], _ice[index], "power", IceColor),
                new ChartTooltipMarker(
                    _times[index], _mguk[index], "power", MgukColor),
            ]);
    }

    private ChartTooltipData? BuildHarvestTooltip(double x)
    {
        var index = NearestIndex(x);
        if (index < 0)
        {
            return null;
        }
        var entries = new List<ChartTooltipEntry>
        {
            new("MGU-K", $"{_harvestMguk[index]:0.0} kJ", HarvestMgukColor),
        };
        var markers = new List<ChartTooltipMarker>
        {
            new(
                _times[index],
                _harvestMguk[index],
                "harvest",
                HarvestMgukColor),
        };
        if (_hasMguh)
        {
            entries.Add(new ChartTooltipEntry(
                "MGU-H", $"{_harvestMguh[index]:0.0} kJ", HarvestMguhColor));
            markers.Add(new ChartTooltipMarker(
                _times[index],
                _harvestMguh[index],
                "harvest",
                HarvestMguhColor));
        }
        var total = _harvestMguk[index] +
            (_hasMguh ? _harvestMguh[index] : 0);
        entries.Add(new ChartTooltipEntry(
            "Total", $"{total:0.0} kJ", NeutralColor));
        return new ChartTooltipData(
            _times[index],
            new ChartTooltipContent(FormatTime(_times[index]), entries),
            markers);
    }

    private ChartTooltipData? BuildErsTooltip(double x)
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
                        "ERS", $"{_ers[index]:0.0}%", IceColor),
                    new ChartTooltipEntry(
                        "Stored",
                        $"{_ers[index] / 100 * 4:0.00} / 4.00 MJ",
                        NeutralColor),
                ]),
            [
                new ChartTooltipMarker(
                    _times[index], _ers[index], "ers", IceColor),
            ]);
    }

    private ChartTooltipData? BuildFuelTooltip(double x)
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
                        "Fuel", $"{_fuel[index]:0.00} kg", FuelColor),
                ]),
            [
                new ChartTooltipMarker(
                    _times[index], _fuel[index], "fuel", FuelColor),
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

    private ChartAxis[] TimeAxes() =>
        [_powerTimeAxis, _harvestTimeAxis, _ersTimeAxis, _fuelTimeAxis];

    private ChartAxis[] ValueAxes() =>
        [_powerAxis, _harvestAxis, _ersAxis, _fuelAxis];

    private GpuChart[] AllPlots() =>
        [PowerSplitPlot, ErsHarvestPlot, ErsStorePlot, FuelPlot];

    private ChartLineSeries[] AllSeries() =>
        [
            _iceSeries,
            _mgukSeries,
            _harvestMgukSeries,
            _harvestMguhSeries,
            _ersSeries,
            _fuelSeries,
        ];

    private TextBlock[] EmptyStates() =>
        [PowerSplitNoData, ErsHarvestNoData, ErsStoreNoData, FuelNoData];

    private ChartCrosshairTooltipPlugin[] Tooltips() =>
        [_powerTooltip, _harvestTooltip, _ersTooltip, _fuelTooltip];

    private void InvalidatePlots()
    {
        foreach (var plot in AllPlots())
        {
            plot.Invalidate();
        }
    }

    private static void SetRange(
        ChartAxis axis,
        double minimum,
        double maximum)
    {
        axis.Minimum = minimum;
        axis.Maximum = maximum;
    }

    private static string FormatTime(double seconds)
    {
        var safe = Math.Max(0, seconds);
        return $"{(int)(safe / 60)}:{(int)(safe % 60):00}";
    }

    private Brush PrimaryBrush() => new SolidColorBrush(
        ActualTheme == ElementTheme.Dark
            ? UiColor.FromArgb(255, 255, 255, 255)
            : UiColor.FromArgb(255, 0, 0, 0));

    private Brush DividerBrush() => new SolidColorBrush(
        ActualTheme == ElementTheme.Dark
            ? UiColor.FromArgb(255, 42, 46, 58)
            : UiColor.FromArgb(255, 217, 220, 227));

}
