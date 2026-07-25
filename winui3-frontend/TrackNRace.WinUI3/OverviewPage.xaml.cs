using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Documents;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using System.Diagnostics;
using TrackNRace.Charting;
using UiColor = Windows.UI.Color;

namespace TrackNRace.WinUI3;

internal enum OverviewChartMode
{
    Default,
    CurrentLap,
    PreviousLap,
    FastestLap,
    Compare,
}

public sealed partial class OverviewPage : Page
{
    private const int MaxChartPoints = 750_000;
    private const float RetentionSeconds = 600;
    private static readonly UiColor SpeedColor =
        UiColor.FromArgb(255, 55, 135, 45);
    private static readonly UiColor RpmColor =
        UiColor.FromArgb(255, 196, 22, 42);
    private static readonly UiColor ErsColor =
        UiColor.FromArgb(255, 250, 222, 42);

    private readonly List<OverviewChartPoint> _sessionPoints = [];
    private readonly List<OverviewChartPoint> _currentLapPoints = [];
    private List<OverviewChartPoint> _previousLapPoints = [];
    private List<OverviewChartPoint> _fastestLapPoints = [];
    private int _fastestLapMilliseconds = int.MaxValue;
    private int? _currentLapNumber;

    private readonly List<PlayerStatusData> _statusHistory = [];
    private readonly ChartLineSeries[] _currentSeries = new ChartLineSeries[3];
    private readonly ChartLineSeries[] _referenceSeries = new ChartLineSeries[3];
    private ChartCrosshairTooltipPlugin? _telemetryTooltip;
    private ChartAxis? _timeAxis;
    private ChartAxis? _speedAxis;
    private ChartAxis? _rpmAxis;
    private ChartAxis? _ersAxis;
    private TelemetrySessionStore? _store;
    private int _telemetryCount;
    private long _telemetryEpoch = -1;
    private int _damageCount;
    private long _damageEpoch = -1;
    private int _powerCount;
    private long _powerEpoch = -1;
    private long _timelineRevision = -1;
    private int _windowSeconds;
    private bool _isLoaded;
    private int _refreshQueued;
    private OverviewChartMode _chartMode;
    private long _lastCardRefreshTicks;
    private bool _forceCardRefresh = true;
    private OverviewLapBlock[]? _knownPlaybackLapBlocks;
    private bool? _renderedStatsCompact;
    private bool? _renderedDamageCompact;
    private OverviewTyreDensity? _renderedTyreDensity;
    private double _configuredTickInterval = double.NaN;
    private bool _configuredLapTicks;
    private bool _referenceSeriesVisible;
    private bool _forcePlotRefresh = true;

    public OverviewPage()
    {
        InitializeComponent();
        var app = (App)Application.Current;
        _windowSeconds = app.ChartWindowSeconds;
        ConfigureTelemetryPlot();
        ConfigureTyreCharts(app);
    }

    private void ConfigureTyreCharts(App app)
    {
        SurfaceChart.Configure(TyreChartKind.Surface, app.TyreWearMode, _windowSeconds);
        InnerChart.Configure(TyreChartKind.Inner, app.TyreWearMode, _windowSeconds);
        BrakeChart.Configure(TyreChartKind.Brake, app.TyreWearMode, _windowSeconds);
        WearChart.Configure(TyreChartKind.Wear, app.TyreWearMode, _windowSeconds);
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
            _store.SnapshotChanged += OnStoreChanged;
            _store.TelemetryChanged += OnStoreChanged;
            _store.TyresChanged += OnStoreChanged;
            _store.PowerChanged += OnStoreChanged;
            _store.TimelineReset += OnTimelineReset;
        }
        app.ChartWindowChanged += OnChartWindowChanged;
        app.TyreWearModeChanged += OnTyreWearModeChanged;
        app.OverviewDisplayChanged += OnOverviewDisplayChanged;
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
            _store.SnapshotChanged -= OnStoreChanged;
            _store.TelemetryChanged -= OnStoreChanged;
            _store.TyresChanged -= OnStoreChanged;
            _store.PowerChanged -= OnStoreChanged;
            _store.TimelineReset -= OnTimelineReset;
            _store = null;
        }
        var app = (App)Application.Current;
        app.ChartWindowChanged -= OnChartWindowChanged;
        app.TyreWearModeChanged -= OnTyreWearModeChanged;
        app.OverviewDisplayChanged -= OnOverviewDisplayChanged;
    }

    private void OnStoreChanged() => QueueRefresh();

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

    private void OnTimelineReset(TimelineResetReason reason)
    {
        DispatcherQueue.TryEnqueue(() =>
        {
            ResetHistories();
            RefreshFromStore();
        });
    }

    private void RefreshFromStore()
    {
        if (!_isLoaded || _store is null)
        {
            return;
        }

        var snapshot = _store.OverviewSnapshot;
        var power = _store.ReadPower(_powerCount, _powerEpoch, _timelineRevision);
        if (power.Reset)
        {
            _statusHistory.Clear();
        }
        _statusHistory.AddRange(power.Rows);
        _powerCount = power.TotalCount;
        _powerEpoch = power.BufferEpoch;

        var telemetry = _store.ReadTelemetry(
            _telemetryCount, _telemetryEpoch, _timelineRevision);
        if (telemetry.Reset)
        {
            ResetTelemetryHistories();
            SurfaceChart.ClearData();
            InnerChart.ClearData();
            BrakeChart.ClearData();
        }
        float? playbackLapStart = null;
        if (telemetry.Reset &&
            telemetry.PlaybackLapStart is float lapStart &&
            telemetry.PlaybackLapNumber is int lapNumber)
        {
            _currentLapNumber = lapNumber;
            playbackLapStart = lapStart;
        }
        else
        {
            UpdateLapBoundary(snapshot.Lap);
        }
        AppendTelemetry(telemetry.Samples, playbackLapStart);
        SurfaceChart.AppendTelemetry(telemetry.Samples);
        InnerChart.AppendTelemetry(telemetry.Samples);
        BrakeChart.AppendTelemetry(telemetry.Samples);
        _telemetryCount = telemetry.TotalCount;
        _telemetryEpoch = telemetry.BufferEpoch;

        var damage = _store.ReadDamage(
            _damageCount, _damageEpoch, _timelineRevision);
        if (damage.Reset)
        {
            WearChart.ClearData();
        }
        WearChart.AppendDamage(damage.Rows);
        _damageCount = damage.TotalCount;
        _damageEpoch = damage.BufferEpoch;
        _timelineRevision = snapshot.TimelineRevision;

        var latest = snapshot.LatestTelemetry?.SessionTime ?? 0;
        if (((App)Application.Current).OverviewDisplay.TyreViewMode ==
            OverviewTyreViewMode.Graphs)
        {
            SurfaceChart.RefreshChart(latest);
            InnerChart.RefreshChart(latest);
            BrakeChart.RefreshChart(latest);
            WearChart.RefreshChart(latest);
        }
        if (!ReferenceEquals(_knownPlaybackLapBlocks, snapshot.PlaybackLapBlocks))
        {
            _knownPlaybackLapBlocks = snapshot.PlaybackLapBlocks;
            RefreshComparisonChoices(snapshot);
            if (_chartMode != OverviewChartMode.Default)
            {
                RebuildDisplayedSeries(snapshot);
                _forcePlotRefresh = true;
            }
        }
        if (_forcePlotRefresh || telemetry.Reset || telemetry.Samples.Length > 0)
        {
            RefreshTelemetryPlot(snapshot);
            _forcePlotRefresh = false;
        }

        var now = Stopwatch.GetTimestamp();
        if (_forceCardRefresh ||
            Stopwatch.GetElapsedTime(_lastCardRefreshTicks, now) >= TimeSpan.FromMilliseconds(100))
        {
            RenderStats(snapshot);
            if (((App)Application.Current).OverviewDisplay.TyreViewMode ==
                OverviewTyreViewMode.Cards)
            {
                RenderTyreCards(snapshot);
            }
            RenderDamage(snapshot);
            _lastCardRefreshTicks = now;
            _forceCardRefresh = false;
        }
    }

    private void UpdateLapBoundary(LapRowData? lap)
    {
        if (lap is null || lap.LapNum <= 0)
        {
            return;
        }
        if (_currentLapNumber is null)
        {
            _currentLapNumber = lap.LapNum;
            return;
        }
        if (_currentLapNumber == lap.LapNum)
        {
            return;
        }

        if (_currentLapPoints.Count > 1)
        {
            _previousLapPoints = [.. _currentLapPoints];
            if (lap.LastLapMs > 0 && lap.LastLapMs < _fastestLapMilliseconds)
            {
                _fastestLapMilliseconds = lap.LastLapMs;
                _fastestLapPoints = [.. _currentLapPoints];
            }
        }
        _currentLapPoints.Clear();
        _currentLapNumber = lap.LapNum;
        if (_chartMode != OverviewChartMode.Default)
        {
            RebuildDisplayedSeries(null);
        }
    }

    private void AppendTelemetry(
        TelemetrySample[] samples,
        float? currentLapStart = null)
    {
        var displayedCount = currentLapStart is null ||
            _chartMode == OverviewChartMode.Default
                ? samples.Length
                : samples.Count(sample =>
                    sample.SessionTime >= currentLapStart.Value);
        var speed = new ChartPoint[displayedCount];
        var rpm = new ChartPoint[displayedCount];
        var ers = new ChartPoint[displayedCount];
        var displayedIndex = 0;
        foreach (var sample in samples)
        {
            var point = new OverviewChartPoint(
                sample.SessionTime, sample.SpeedKph, sample.Rpm, ErsAt(sample.SessionTime));
            _sessionPoints.Add(point);
            if (currentLapStart is null ||
                sample.SessionTime >= currentLapStart.Value)
            {
                _currentLapPoints.Add(point);
            }
            if (currentLapStart is not null &&
                _chartMode != OverviewChartMode.Default &&
                sample.SessionTime < currentLapStart.Value)
            {
                continue;
            }
            var origin = _chartMode == OverviewChartMode.Default
                ? 0
                : _currentLapPoints[0].SessionTime;
            var x = point.SessionTime - origin;
            speed[displayedIndex] = new ChartPoint(x, point.SpeedKph);
            rpm[displayedIndex] = new ChartPoint(x, point.Rpm);
            ers[displayedIndex] = new ChartPoint(x, point.ErsPct);
            displayedIndex++;
        }
        _currentSeries[0].Append(speed);
        _currentSeries[1].Append(rpm);
        _currentSeries[2].Append(ers);
        TrimSessionPoints();
        if (_currentLapPoints.Count > MaxChartPoints)
        {
            _currentLapPoints.RemoveRange(
                0, _currentLapPoints.Count - MaxChartPoints);
            if (_chartMode != OverviewChartMode.Default)
            {
                RebuildDisplayedSeries(_store?.OverviewSnapshot);
            }
        }
    }

    private double ErsAt(float sessionTime)
    {
        for (var index = _statusHistory.Count - 1; index >= 0; index--)
        {
            if (_statusHistory[index].SessionTime <= sessionTime)
            {
                return _statusHistory[index].ErsPct;
            }
        }
        return 0;
    }

    private void TrimSessionPoints()
    {
        if (_sessionPoints.Count == 0)
        {
            return;
        }
        var cutoff = _sessionPoints[^1].SessionTime - RetentionSeconds;
        var remove = 0;
        while (remove < _sessionPoints.Count &&
            (_sessionPoints[remove].SessionTime < cutoff ||
             _sessionPoints.Count - remove > MaxChartPoints))
        {
            remove++;
        }
        if (remove < 4096 && _sessionPoints.Count <= MaxChartPoints)
        {
            return;
        }
        _sessionPoints.RemoveRange(0, remove);
    }

    private void RefreshTelemetryPlot(OverviewSnapshot snapshot)
    {
        var current = _chartMode == OverviewChartMode.Default
            ? _sessionPoints
            : _currentLapPoints;
        var hasData = current.Count > 1;
        TelemetryNoData.Visibility = hasData ? Visibility.Collapsed : Visibility.Visible;
        var reference = ReferencePoints(snapshot);
        var showReference = reference.Count > 1;
        if (showReference != _referenceSeriesVisible)
        {
            _referenceSeriesVisible = showReference;
            foreach (var series in _referenceSeries)
            {
                series.Visible = showReference;
            }
        }

        var origin = _chartMode == OverviewChartMode.Default || current.Count == 0
            ? 0
            : current[0].SessionTime;
        var latest = current.Count > 0 ? current[^1].SessionTime - origin : 0;
        var xMin = _chartMode == OverviewChartMode.Default
            ? Math.Max(0, latest - _windowSeconds)
            : 0;
        var referenceEnd = reference.Count > 0
            ? reference[^1].SessionTime - reference[0].SessionTime
            : 0;
        var xMax = _chartMode == OverviewChartMode.Default
            ? Math.Max(_windowSeconds, latest)
            : Math.Max(10, Math.Max(latest, referenceEnd));
        _timeAxis!.Minimum = xMin;
        _timeAxis.Maximum = xMax;
        ConfigureTimeTicks();
        TelemetryPlot.Invalidate();
    }

    private void ConfigureTelemetryPlot()
    {
        TelemetryPlot.Plugins.Add(new ChartBackgroundPlugin());
        _telemetryTooltip = new ChartCrosshairTooltipPlugin(
            BuildTelemetryTooltip);
        TelemetryPlot.Plugins.Add(_telemetryTooltip);
        TelemetryPlot.RenderFailed += error =>
        {
            Debug.WriteLine($"[D3D11Chart] {error}");
            TelemetryNoData.Text = "Chart unavailable";
            TelemetryNoData.Visibility = Visibility.Visible;
        };
        _timeAxis = new ChartAxis(
            "time", ChartAxisOrientation.X, ChartAxisSide.Bottom)
        {
            Minimum = 0,
            Maximum = _windowSeconds,
            ShowGridLines = true,
            Color = UiColor.FromArgb(255, 124, 128, 152),
        };
        _speedAxis = new ChartAxis(
            "speed", ChartAxisOrientation.Y, ChartAxisSide.Left)
        {
            Minimum = 0,
            Maximum = 380,
            TickProvider = new FixedChartTickProvider(0, 95, 190, 285, 380),
            LabelFormatter = value => $"{value:0}",
            Color = SpeedColor,
        };
        _rpmAxis = new ChartAxis(
            "rpm", ChartAxisOrientation.Y, ChartAxisSide.Right)
        {
            Minimum = 0,
            Maximum = 16000,
            TickProvider = new FixedChartTickProvider(0, 4000, 8000, 12000, 16000),
            LabelFormatter = value => value == 0 ? "0" : $"{value / 1000:0}k",
            Color = RpmColor,
        };
        _ersAxis = new ChartAxis(
            "ers", ChartAxisOrientation.Y, ChartAxisSide.Right)
        {
            Minimum = 0,
            Maximum = 100,
            TickProvider = new FixedChartTickProvider(0, 25, 50, 75, 100),
            LabelFormatter = value => $"{value:0}%",
            Color = ErsColor,
        };
        TelemetryPlot.Axes.Add(_timeAxis);
        TelemetryPlot.Axes.Add(_speedAxis);
        TelemetryPlot.Axes.Add(_rpmAxis);
        TelemetryPlot.Axes.Add(_ersAxis);

        var keys = new[] { "speed", "rpm", "ers" };
        var colors = new[] { SpeedColor, RpmColor, ErsColor };
        for (var index = 0; index < 3; index++)
        {
            _currentSeries[index] = TelemetryPlot.Series.Add(
                new ChartLineSeriesOptions(
                    $"current-{keys[index]}",
                    "time",
                    keys[index],
                    colors[index],
                    MaximumPointCount: MaxChartPoints,
                    MaximumXSpan: RetentionSeconds));
            _referenceSeries[index] = TelemetryPlot.Series.Add(
                new ChartLineSeriesOptions(
                    $"reference-{keys[index]}",
                    "time",
                    keys[index],
                    colors[index],
                    Opacity: .35f,
                    Visible: false,
                    MaximumPointCount: MaxChartPoints));
        }
        ConfigureTimeTicks();
#if DEBUG
        AttachRenderProbe();
#endif
    }

    private void ConfigureTimeTicks()
    {
        var lapTicks = _chartMode != OverviewChartMode.Default;
        var currentEnd = _currentLapPoints.Count > 1
            ? _currentLapPoints[^1].SessionTime -
                _currentLapPoints[0].SessionTime
            : 0;
        var reference = ReferencePoints(_store?.OverviewSnapshot);
        var referenceEnd = reference.Count > 1
            ? reference[^1].SessionTime - reference[0].SessionTime
            : 0;
        var interval = _chartMode == OverviewChartMode.Default
            ? _windowSeconds / 6d
            : Math.Max(5, Math.Ceiling(
                Math.Max(currentEnd, referenceEnd) / 30d) * 5);
        // Tick generators allocate and invalidate axis layout. Only replace one
        // when its effective configuration actually changes.
        if (_configuredLapTicks == lapTicks &&
            Math.Abs(_configuredTickInterval - interval) < .001)
        {
            return;
        }
        _configuredLapTicks = lapTicks;
        _configuredTickInterval = interval;
        _timeAxis!.TickProvider = new IntervalChartTickProvider(interval);
        _timeAxis.LabelFormatter = !lapTicks
            ? FormatSessionTime
            : FormatLapTime;
    }

    private void OnChartModeClicked(object sender, RoutedEventArgs args)
    {
        if (sender is not ToggleButton button ||
            button.Tag is not string tag ||
            !Enum.TryParse<OverviewChartMode>(tag, out var mode))
        {
            return;
        }
        _chartMode = mode;
        foreach (var candidate in new[]
                 {
                     DefaultModeButton, CurrentLapModeButton, PreviousLapModeButton,
                     FastestLapModeButton, CompareModeButton,
                 })
        {
            candidate.IsChecked = candidate == button;
        }
        CompareLapComboBox.Visibility =
            mode == OverviewChartMode.Compare ? Visibility.Visible : Visibility.Collapsed;
        RebuildDisplayedSeries(_store?.OverviewSnapshot);
        if (_store is not null)
        {
            RefreshTelemetryPlot(_store.OverviewSnapshot);
        }
    }

    private void OnCompareLapChanged(object sender, SelectionChangedEventArgs args)
    {
        if (_chartMode == OverviewChartMode.Compare && _store is not null)
        {
            RebuildDisplayedSeries(_store.OverviewSnapshot);
            RefreshTelemetryPlot(_store.OverviewSnapshot);
        }
    }

    private void RebuildDisplayedSeries(OverviewSnapshot? snapshot)
    {
        IReadOnlyList<OverviewChartPoint> current = _chartMode == OverviewChartMode.Default
            ? _sessionPoints
            : _currentLapPoints;
        var currentOrigin = _chartMode == OverviewChartMode.Default ||
            _currentLapPoints.Count == 0
                ? 0
                : _currentLapPoints[0].SessionTime;
        ReplaceDisplayedSeries(_currentSeries, current, currentOrigin);

        var reference = ReferencePoints(snapshot);
        ReplaceDisplayedSeries(
            _referenceSeries,
            reference,
            reference.Count > 0 ? reference[0].SessionTime : 0);
        var visible = reference.Count > 1;
        foreach (var series in _referenceSeries)
        {
            series.Visible = visible;
        }
        _referenceSeriesVisible = visible;
    }

    private IReadOnlyList<OverviewChartPoint> ReferencePoints(OverviewSnapshot? snapshot)
    {
        if (snapshot?.IsPlayback == true)
        {
            OverviewLapBlock? block = _chartMode switch
            {
                OverviewChartMode.PreviousLap => PreviousPlaybackBlock(snapshot),
                OverviewChartMode.FastestLap => snapshot.PlaybackLapBlocks.FirstOrDefault(
                    value => value.LapNumber == snapshot.FastestPlaybackLapNumber),
                OverviewChartMode.Compare => CompareLapComboBox.SelectedItem is int lap
                    ? snapshot.PlaybackLapBlocks.FirstOrDefault(value => value.LapNumber == lap)
                    : null,
                _ => null,
            };
            if (block is not null)
            {
                return block.Points;
            }
        }
        return _chartMode switch
        {
            OverviewChartMode.PreviousLap => _previousLapPoints,
            OverviewChartMode.FastestLap => _fastestLapPoints,
            _ => [],
        };
    }

    private static OverviewLapBlock? PreviousPlaybackBlock(OverviewSnapshot snapshot)
    {
        var current = snapshot.Lap?.LapNum ?? int.MaxValue;
        return snapshot.PlaybackLapBlocks
            .Where(value => value.LapNumber < current)
            .OrderByDescending(value => value.LapNumber)
            .FirstOrDefault();
    }

    private void RefreshComparisonChoices(OverviewSnapshot snapshot)
    {
        var laps = snapshot.PlaybackLapBlocks.Select(value => value.LapNumber).ToArray();
        var existing = CompareLapComboBox.Items.Cast<object>().OfType<int>().ToArray();
        if (laps.SequenceEqual(existing))
        {
            CompareModeButton.Visibility =
                laps.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
            return;
        }
        var selected = CompareLapComboBox.SelectedItem as int?;
        CompareLapComboBox.Items.Clear();
        foreach (var lap in laps)
        {
            CompareLapComboBox.Items.Add(lap);
        }
        CompareModeButton.Visibility =
            laps.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
        if (laps.Length > 0)
        {
            CompareLapComboBox.SelectedItem =
                selected is int value && laps.Contains(value) ? value : laps[0];
        }
    }

    private static void ReplaceDisplayedSeries(
        ChartLineSeries[] series,
        IReadOnlyList<OverviewChartPoint> points,
        float origin)
    {
        if (points.Count == 0)
        {
            foreach (var candidate in series)
            {
                candidate.Clear();
            }
            return;
        }

        var speed = new ChartPoint[points.Count];
        var rpm = new ChartPoint[points.Count];
        var ers = new ChartPoint[points.Count];
        for (var index = 0; index < points.Count; index++)
        {
            var point = points[index];
            var x = point.SessionTime - origin;
            speed[index] = new ChartPoint(x, point.SpeedKph);
            rpm[index] = new ChartPoint(x, point.Rpm);
            ers[index] = new ChartPoint(x, point.ErsPct);
        }
        series[0].Replace(speed);
        series[1].Replace(rpm);
        series[2].Replace(ers);
    }

    private void ResetHistories()
    {
        _telemetryCount = 0;
        _telemetryEpoch = -1;
        _damageCount = 0;
        _damageEpoch = -1;
        _powerCount = 0;
        _powerEpoch = -1;
        _timelineRevision = -1;
        _knownPlaybackLapBlocks = null;
        _statusHistory.Clear();
        ResetTelemetryHistories();
        SurfaceChart.ClearData();
        InnerChart.ClearData();
        BrakeChart.ClearData();
        WearChart.ClearData();
    }

    private void ResetTelemetryHistories()
    {
        _sessionPoints.Clear();
        _currentLapPoints.Clear();
        _previousLapPoints.Clear();
        _fastestLapPoints.Clear();
        _fastestLapMilliseconds = int.MaxValue;
        _currentLapNumber = null;
        foreach (var series in _currentSeries.Concat(_referenceSeries))
        {
            series.Clear();
        }
        _referenceSeriesVisible = false;
    }

    private void RenderStats(OverviewSnapshot snapshot)
    {
        var t = snapshot.LatestTelemetry;
        var s = snapshot.Status;
        var lap = snapshot.Lap;
        var damage = snapshot.Damage;
        var fields = ColorFields(t, s);
        var wingKey = snapshot.AeroMode == "slm" ? "slm" : "drs";
        var wingLabel = Label(snapshot, $"{wingKey}.label", wingKey.ToUpperInvariant());
        var wingValue = t is null ? "—" :
            (wingKey == "slm" ? t.Slm : t.Drs) != 0 ? "ON" : "OFF";
        var ersMode = s is null ? "" : Label(snapshot, $"ers.mode.{s.ErsMode}", $"Mode {s.ErsMode}");
        var tyre = s is null ? "—" :
            Label(snapshot, $"tyre.actual.{s.TyreCompound}", $"C{s.TyreCompound}");
        var fuelMix = s?.FuelMix switch { 0 => "Lean", 1 => "Std", 2 => "Rich", 3 => "Max", _ => "" };
        var cards = new[]
        {
            Stat("SPEED", t is null ? "—" : $"{t.SpeedKph}", t is null ? "" : "kph", "", "speed", (double?)null),
            Stat("RPM", t is null ? "—" : $"{t.Rpm:N0}", "", "", "rpm", (double?)null),
            Stat("GEAR", t is null ? "—" : t.Gear == 0 ? "N" : t.Gear < 0 ? "R" : $"{t.Gear}", "", "", "gear", (double?)t?.Gear),
            Stat("THROTTLE", t is null ? "—" : $"{Math.Round(t.Throttle * 100)}", t is null ? "" : "%", "", "throttle", (double?)null),
            Stat("BRAKE", t is null ? "—" : $"{Math.Round(t.Brake * 100)}", t is null ? "" : "%", "", "brake", (double?)null),
            Stat(wingLabel, wingValue, "", wingKey == "drs" && damage?.DrsFault == 1 ? "FAULT" : "", "wing", (double?)(wingKey == "slm" ? t?.Slm : t?.Drs)),
            Stat("ENGINE", t is null ? "—" : $"{t.EngineTemp}", t is null ? "" : "°C", "", "engine", (double?)t?.EngineTemp),
            Stat("ERS", s is null ? "—" : $"{s.ErsPct:0}", s is null ? "" : "%", damage?.ErsFault == 1 ? "FAULT" : ersMode, "ers", (double?)s?.ErsPct),
            Stat("FUEL", s is null ? "—" : $"{s.FuelKg:0.0}", s is null ? "" : "kg", s is null ? "" : $"{s.FuelLaps:+0.0;-0.0;0.0} vs fin", "fuel", (double?)null),
            Stat("POS", lap is null ? "—" : $"P{lap.Position}", "", lap is null ? "" : $"Lap {lap.LapNum}", "", (double?)null),
            Stat("TYRE", tyre, "", s is null ? "" : $"{s.TyreAgeLaps}L · {fuelMix}", "tyre", (double?)null),
        };
        var compact = ((App)Application.Current).OverviewDisplay.CompactStats;
        SyncCards(
            StatsGrid, cards, compact, snapshot, fields, ref _renderedStatsCompact);
    }

    private void RenderDamage(OverviewSnapshot snapshot)
    {
        var d = snapshot.Damage;
        var allCards = new[]
        {
            Damage("TYRE FL", d?.TyreDmgFl), Damage("BRAKE FL", d?.BrakeDmgFl),
            Damage("TYRE FR", d?.TyreDmgFr), Damage("BRAKE FR", d?.BrakeDmgFr),
            Damage("TYRE RL", d?.TyreDmgRl), Damage("BRAKE RL", d?.BrakeDmgRl),
            Damage("TYRE RR", d?.TyreDmgRr), Damage("BRAKE RR", d?.BrakeDmgRr),
            Damage("WING FL", d?.WingFl), Damage("WING FR", d?.WingFr),
            Damage("WING REAR", d?.WingRear), Damage("FLOOR", d?.FloorDamage),
            Damage("DIFFUSER", d?.DiffuserDamage), Damage("SIDEPOD", d?.SidepodDamage),
            Damage("GEARBOX", d?.GearboxDamage), Damage("ENGINE", d?.EngineDamage),
        };
        // Electron's current default layout; all definitions above are ready for
        // the intentionally deferred layout editor.
        var visible = new[] { 8, 9, 10, 11, 12, 14, 15 }.Select(index => allCards[index]).ToArray();
        var compact = ((App)Application.Current).OverviewDisplay.CompactDamage;
        SyncCards(
            DamageGrid,
            visible,
            compact,
            snapshot,
            ColorFields(snapshot.LatestTelemetry, snapshot.Status),
            ref _renderedDamageCompact);
    }

    private static CardValue Stat(
        string label, string value, string unit, string sub, string colorKey, double? colorValue) =>
        new(label, value, unit, sub, colorKey, colorValue, false);

    private static CardValue Damage(string label, int? value) =>
        new(label, value is null ? "—" : value.Value.ToString(), value is null ? "" : "%",
            "", "", value, true);

    private void SyncCards(
        Grid grid,
        IReadOnlyList<CardValue> cards,
        bool compact,
        OverviewSnapshot snapshot,
        IReadOnlyDictionary<string, double> fields,
        ref bool? renderedCompact)
    {
        if (renderedCompact != compact || grid.Children.Count != cards.Count)
        {
            renderedCompact = compact;
            grid.Children.Clear();
            grid.ColumnDefinitions.Clear();
            for (var index = 0; index < cards.Count; index++)
            {
                grid.ColumnDefinitions.Add(new ColumnDefinition
                {
                    Width = new GridLength(1, GridUnitType.Star),
                });
                var wrapper = new Border
                {
                    BorderBrush = DividerBrush(),
                    BorderThickness = index < cards.Count - 1
                        ? new Thickness(0, 0, 1, 0)
                        : new Thickness(0),
                    Child = compact
                        ? CreateCompactCard(cards[index], PrimaryBrush())
                        : CreateNormalCard(cards[index], PrimaryBrush()),
                };
                Grid.SetColumn(wrapper, index);
                grid.Children.Add(wrapper);
            }
        }

        for (var index = 0; index < cards.Count; index++)
        {
            var card = cards[index];
            var brush = card.IsDamage
                ? DamageBrush(card.ColorValue)
                : string.IsNullOrEmpty(card.ColorKey)
                    ? PrimaryBrush()
                    : TelemetryColors.ResolveContext(
                        snapshot.CardColors, card.ColorKey, card.ColorValue, fields,
                        ActualTheme == ElementTheme.Dark, PrimaryBrush());
            var wrapper = (Border)grid.Children[index];
            wrapper.BorderBrush = DividerBrush();
            UpdateCard(wrapper.Child as FrameworkElement, card, brush, compact);
        }
    }

    private FrameworkElement CreateNormalCard(CardValue card, Brush valueBrush)
    {
        var panel = new StackPanel { Padding = new Thickness(12, 9, 12, 8) };
        panel.Children.Add(new TextBlock
        {
            Text = card.Label,
            Style = (Style)Resources["OverviewSectionLabelStyle"],
            TextTrimming = TextTrimming.CharacterEllipsis,
        });
        panel.Children.Add(CreateValueText(card, valueBrush, 21));
        panel.Children.Add(new TextBlock
        {
            Text = card.Sub,
            FontSize = 11,
            FontWeight = Microsoft.UI.Text.FontWeights.SemiBold,
            Foreground = card.Sub == "FAULT" ? NegativeBrush() : SecondaryBrush(),
            TextTrimming = TextTrimming.CharacterEllipsis,
            Visibility = string.IsNullOrEmpty(card.Sub)
                ? Visibility.Collapsed
                : Visibility.Visible,
        });
        return panel;
    }

    private FrameworkElement CreateCompactCard(CardValue card, Brush valueBrush)
    {
        var grid = new Grid
        {
            Padding = new Thickness(10, 6, 10, 6),
            ColumnSpacing = 6,
            ColumnDefinitions =
            {
                new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) },
                new ColumnDefinition { Width = GridLength.Auto },
            },
        };
        grid.Children.Add(new TextBlock
        {
            Text = card.Label,
            Style = (Style)Resources["OverviewSectionLabelStyle"],
            FontSize = 10,
            VerticalAlignment = Microsoft.UI.Xaml.VerticalAlignment.Center,
            TextTrimming = TextTrimming.CharacterEllipsis,
        });
        var value = CreateValueText(card with
        {
            Sub = card.Sub.Replace("Overtake", "OT").Replace(" vs fin", ""),
        }, valueBrush, 15, includeSub: true);
        Grid.SetColumn(value, 1);
        grid.Children.Add(value);
        return grid;
    }

    private TextBlock CreateValueText(
        CardValue card, Brush brush, double fontSize, bool includeSub = false)
    {
        var text = new TextBlock
        {
            VerticalAlignment = Microsoft.UI.Xaml.VerticalAlignment.Center,
            TextTrimming = TextTrimming.CharacterEllipsis,
        };
        text.Inlines.Add(new Run
        {
            Text = card.Value,
            FontSize = fontSize,
            FontWeight = Microsoft.UI.Text.FontWeights.Bold,
            Foreground = brush,
        });
        text.Inlines.Add(new Run
        {
            Text = string.IsNullOrEmpty(card.Unit) ? "" : $" {card.Unit}",
            FontSize = 10,
            Foreground = SecondaryBrush(),
        });
        text.Inlines.Add(new Run
        {
            Text = includeSub && !string.IsNullOrEmpty(card.Sub) ? $"  {card.Sub}" : "",
            FontSize = 10,
            FontWeight = Microsoft.UI.Text.FontWeights.SemiBold,
            Foreground = card.Sub == "FAULT" ? NegativeBrush() : SecondaryBrush(),
        });
        return text;
    }

    private void UpdateCard(
        FrameworkElement? content,
        CardValue card,
        Brush valueBrush,
        bool compact)
    {
        TextBlock label;
        TextBlock value;
        TextBlock? sub = null;
        if (compact && content is Grid compactGrid)
        {
            label = (TextBlock)compactGrid.Children[0];
            value = (TextBlock)compactGrid.Children[1];
        }
        else if (content is StackPanel panel)
        {
            label = (TextBlock)panel.Children[0];
            value = (TextBlock)panel.Children[1];
            sub = (TextBlock)panel.Children[2];
        }
        else
        {
            return;
        }

        label.Text = card.Label;
        var runs = value.Inlines.OfType<Run>().ToArray();
        runs[0].Text = card.Value;
        runs[0].Foreground = valueBrush;
        runs[1].Text = string.IsNullOrEmpty(card.Unit) ? "" : $" {card.Unit}";
        runs[1].Foreground = SecondaryBrush();
        var compactSub = card.Sub.Replace("Overtake", "OT").Replace(" vs fin", "");
        runs[2].Text = compact && !string.IsNullOrEmpty(compactSub)
            ? $"  {compactSub}"
            : "";
        runs[2].Foreground = card.Sub == "FAULT" ? NegativeBrush() : SecondaryBrush();
        if (sub is not null)
        {
            sub.Text = card.Sub;
            sub.Foreground = card.Sub == "FAULT" ? NegativeBrush() : SecondaryBrush();
            sub.Visibility = string.IsNullOrEmpty(card.Sub)
                ? Visibility.Collapsed
                : Visibility.Visible;
        }
    }

    private void RenderTyreCards(OverviewSnapshot snapshot)
    {
        var t = snapshot.LatestTelemetry;
        var d = snapshot.Damage;
        var density = ((App)Application.Current).OverviewDisplay.TyreDensity;
        var cards = new[]
        {
            new TyreValue("Front Left", "FL", t?.TyreTempSurfaceFl, t?.TyreTempInnerFl, t?.BrakeTempFl, d?.TyreWearFl, d?.BlistersFl),
            new TyreValue("Front Right", "FR", t?.TyreTempSurfaceFr, t?.TyreTempInnerFr, t?.BrakeTempFr, d?.TyreWearFr, d?.BlistersFr),
            new TyreValue("Rear Left", "RL", t?.TyreTempSurfaceRl, t?.TyreTempInnerRl, t?.BrakeTempRl, d?.TyreWearRl, d?.BlistersRl),
            new TyreValue("Rear Right", "RR", t?.TyreTempSurfaceRr, t?.TyreTempInnerRr, t?.BrakeTempRr, d?.TyreWearRr, d?.BlistersRr),
        };
        if (_renderedTyreDensity != density ||
            TyreCardsGrid.Children.Count != cards.Length)
        {
            _renderedTyreDensity = density;
            TyreCardsGrid.Children.Clear();
            TyreCardsGrid.ColumnDefinitions.Clear();
            for (var index = 0; index < cards.Length; index++)
            {
                TyreCardsGrid.ColumnDefinitions.Add(new ColumnDefinition
                {
                    Width = new GridLength(1, GridUnitType.Star),
                });
                var border = new Border
                {
                    BorderBrush = DividerBrush(),
                    BorderThickness = index < cards.Length - 1
                        ? new Thickness(0, 0, 1, 0)
                        : new Thickness(0),
                    Child = CreateTyreCard(cards[index], density, snapshot),
                };
                Grid.SetColumn(border, index);
                TyreCardsGrid.Children.Add(border);
            }
        }

        for (var index = 0; index < cards.Length; index++)
        {
            UpdateTyreCard(
                (Border)TyreCardsGrid.Children[index],
                cards[index],
                snapshot);
        }
    }

    private FrameworkElement CreateTyreCard(
        TyreValue tyre, OverviewTyreDensity density, OverviewSnapshot snapshot)
    {
        var metrics = TyreMetrics(tyre, snapshot);
        if (density is OverviewTyreDensity.Compact1 or OverviewTyreDensity.Compact2)
        {
            var card = new Grid();
            if (density == OverviewTyreDensity.Compact1)
            {
                card.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
                card.RowDefinitions.Add(new RowDefinition
                {
                    Height = new GridLength(1, GridUnitType.Star),
                });
                card.Children.Add(new Border
                {
                    BorderBrush = DividerBrush(),
                    BorderThickness = new Thickness(0, 0, 0, 1),
                    Child = new TextBlock
                    {
                        Text = tyre.Name.ToUpperInvariant(),
                        Style = (Style)Resources["OverviewSectionLabelStyle"],
                        FontSize = 10,
                        FontWeight = Microsoft.UI.Text.FontWeights.Bold,
                        CharacterSpacing = 120,
                        HorizontalAlignment =
                            Microsoft.UI.Xaml.HorizontalAlignment.Center,
                        Padding = new Thickness(0, 4, 0, 4),
                    },
                });
            }
            else
            {
                card.RowDefinitions.Add(new RowDefinition
                {
                    Height = new GridLength(1, GridUnitType.Star),
                });
            }
            var metricRow = CreateCompactTyreMetrics(metrics);
            Grid.SetRow(
                metricRow,
                density == OverviewTyreDensity.Compact1 ? 1 : 0);
            card.Children.Add(metricRow);
            return card;
        }
        if (density is OverviewTyreDensity.Compact3 or OverviewTyreDensity.Compact4)
        {
            var grid = new Grid
            {
                Padding = new Thickness(12, 6, 12, 6),
                ColumnDefinitions =
                {
                    new ColumnDefinition { Width = GridLength.Auto },
                    new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) },
                    new ColumnDefinition { Width = GridLength.Auto },
                },
            };
            grid.Children.Add(new TextBlock
            {
                Text = density == OverviewTyreDensity.Compact4
                    ? tyre.Abbreviation
                    : tyre.Name.ToUpperInvariant(),
                Style = (Style)Resources["OverviewSectionLabelStyle"],
                FontSize = 10,
            });
            var values = CreateHorizontalMetrics(
                metrics, density == OverviewTyreDensity.Compact4);
            Grid.SetColumn(values, 2);
            grid.Children.Add(values);
            return grid;
        }

        var dense = density == OverviewTyreDensity.Compact5;
        var stack = new StackPanel
        {
            Padding = dense ? new Thickness(10, 7, 10, 7) : new Thickness(14, 10, 14, 10),
            Spacing = dense ? 1 : 4,
        };
        stack.Children.Add(new TextBlock
        {
            Text = tyre.Name.ToUpperInvariant(),
            Style = (Style)Resources["OverviewSectionLabelStyle"],
            Margin = new Thickness(0, 0, 0, dense ? 1 : 4),
        });
        for (var index = 0; index < 3; index++)
        {
            stack.Children.Add(CreateMetricRow(metrics[index], index, dense ? 11 : 13));
        }
        var wear = metrics[3];
        if (!dense)
        {
            stack.Children.Add(new ProgressBar
            {
                Height = 7,
                Margin = new Thickness(0, 5, 0, 0),
                Maximum = 100,
                Value = tyre.Wear ?? 0,
                Foreground = wear.Brush,
                Tag = "wear-progress",
            });
        }
        stack.Children.Add(CreateMetricRow(
            wear with
            {
                Label = tyre.Blisters is > 0
                    ? $"Wear · {tyre.Blisters}% blisters"
                    : "Wear",
            },
            3,
            dense ? 11 : 12));
        return stack;
    }

    private Grid CreateCompactTyreMetrics(IReadOnlyList<MetricValue> metrics)
    {
        var row = new Grid();
        for (var index = 0; index < metrics.Count; index++)
        {
            row.ColumnDefinitions.Add(new ColumnDefinition
            {
                Width = new GridLength(1, GridUnitType.Star),
            });
            var cell = new Grid
            {
                Padding = new Thickness(12, 4, 12, 4),
                ColumnDefinitions =
                {
                    new ColumnDefinition
                    {
                        Width = new GridLength(1, GridUnitType.Star),
                    },
                    new ColumnDefinition { Width = GridLength.Auto },
                },
            };
            cell.Children.Add(new TextBlock
            {
                Text = metrics[index].Label.ToUpperInvariant(),
                FontSize = 9,
                CharacterSpacing = 50,
                Foreground = SecondaryBrush(),
                VerticalAlignment = Microsoft.UI.Xaml.VerticalAlignment.Center,
            });
            var value = new TextBlock
            {
                Text = metrics[index].Text,
                FontSize = 11,
                FontWeight = Microsoft.UI.Text.FontWeights.Bold,
                Foreground = metrics[index].Brush,
                VerticalAlignment = Microsoft.UI.Xaml.VerticalAlignment.Center,
                Tag = index,
            };
            Grid.SetColumn(value, 1);
            cell.Children.Add(value);

            var wrapper = new Border
            {
                BorderBrush = DividerBrush(),
                BorderThickness = index > 0
                    ? new Thickness(1, 0, 0, 0)
                    : new Thickness(0),
                Child = cell,
            };
            Grid.SetColumn(wrapper, index);
            row.Children.Add(wrapper);
        }
        return row;
    }

    private Grid CreateHorizontalMetrics(IReadOnlyList<MetricValue> metrics, bool showLabels)
    {
        var grid = new Grid { ColumnSpacing = showLabels ? 10 : 16 };
        for (var index = 0; index < metrics.Count; index++)
        {
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            var text = new TextBlock
            {
                VerticalAlignment = Microsoft.UI.Xaml.VerticalAlignment.Center,
                Tag = index,
            };
            if (showLabels)
            {
                text.Inlines.Add(new Run
                {
                    Text = $"{metrics[index].Label.ToUpperInvariant()} ",
                    FontSize = 9,
                    Foreground = SecondaryBrush(),
                });
            }
            text.Inlines.Add(new Run
            {
                Text = metrics[index].Text,
                FontSize = 11,
                FontWeight = Microsoft.UI.Text.FontWeights.Bold,
                Foreground = metrics[index].Brush,
            });
            Grid.SetColumn(text, index);
            grid.Children.Add(text);
        }
        return grid;
    }

    private Grid CreateMetricRow(MetricValue metric, int metricIndex, double fontSize)
    {
        var grid = new Grid
        {
            ColumnDefinitions =
            {
                new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) },
                new ColumnDefinition { Width = GridLength.Auto },
            },
        };
        grid.Children.Add(new TextBlock
        {
            Text = metric.Label,
            FontSize = fontSize,
            Foreground = SecondaryBrush(),
            Tag = $"label-{metricIndex}",
        });
        var value = new TextBlock
        {
            Text = metric.Text,
            FontSize = fontSize,
            FontWeight = Microsoft.UI.Text.FontWeights.Bold,
            Foreground = metric.Brush,
            Tag = metricIndex,
        };
        Grid.SetColumn(value, 1);
        grid.Children.Add(value);
        return grid;
    }

    private void UpdateTyreCard(
        Border wrapper,
        TyreValue tyre,
        OverviewSnapshot snapshot)
    {
        wrapper.BorderBrush = DividerBrush();
        var metrics = TyreMetrics(tyre, snapshot);
        foreach (var text in Descendants<TextBlock>(wrapper))
        {
            if (text.Tag is int metricIndex &&
                metricIndex >= 0 && metricIndex < metrics.Length)
            {
                var valueRun = text.Inlines.OfType<Run>().LastOrDefault();
                if (valueRun is null)
                {
                    text.Text = metrics[metricIndex].Text;
                    text.Foreground = metrics[metricIndex].Brush;
                }
                else
                {
                    valueRun.Text = metrics[metricIndex].Text;
                    valueRun.Foreground = metrics[metricIndex].Brush;
                }
            }
            else if (text.Tag is string tag &&
                tag == "label-3")
            {
                text.Text = tyre.Blisters is > 0
                    ? $"Wear · {tyre.Blisters}% blisters"
                    : "Wear";
            }
        }
        foreach (var progress in Descendants<ProgressBar>(wrapper))
        {
            if (progress.Tag as string == "wear-progress")
            {
                progress.Value = tyre.Wear ?? 0;
                progress.Foreground = metrics[3].Brush;
            }
        }
    }

    private static IEnumerable<T> Descendants<T>(DependencyObject root)
        where T : DependencyObject
    {
        var count = VisualTreeHelper.GetChildrenCount(root);
        for (var index = 0; index < count; index++)
        {
            var child = VisualTreeHelper.GetChild(root, index);
            if (child is T match)
            {
                yield return match;
            }
            foreach (var descendant in Descendants<T>(child))
            {
                yield return descendant;
            }
        }
    }

    private MetricValue[] TyreMetrics(TyreValue tyre, OverviewSnapshot snapshot)
    {
        var dark = ActualTheme == ElementTheme.Dark;
        Brush Resolve(string key, double? value) =>
            value is null
                ? SecondaryBrush()
                : TelemetryColors.Resolve(snapshot.CardColors, key, value, dark, PrimaryBrush());
        return
        [
            new("Surface", tyre.Surface is null ? "—" : $"{tyre.Surface}°C", Resolve("temp.tyre", tyre.Surface)),
            new("Inner", tyre.Inner is null ? "—" : $"{tyre.Inner}°C", Resolve("temp.tyre", tyre.Inner)),
            new("Brake", tyre.Brake is null ? "—" : $"{tyre.Brake}°C", Resolve("temp.brake", tyre.Brake)),
            new("Wear", tyre.Wear is null ? "—" : $"{tyre.Wear:0.0}%", Resolve("wear", tyre.Wear)),
        ];
    }

    private void ApplyDisplaySettings()
    {
        var settings = ((App)Application.Current).OverviewDisplay;
        StatsGrid.MinHeight = settings.CompactStats ? 42 : 72;
        DamageGrid.MinHeight = settings.CompactDamage ? 38 : 64;
        var graphs = settings.TyreViewMode == OverviewTyreViewMode.Graphs;
        TyreCardsGrid.Visibility = graphs ? Visibility.Collapsed : Visibility.Visible;
        TyreGraphsGrid.Visibility = graphs ? Visibility.Visible : Visibility.Collapsed;
        TelemetryChartRow.Height = graphs
            ? new GridLength(13, GridUnitType.Star)
            : new GridLength(1, GridUnitType.Star);
        TyreRow.Height = graphs
            ? new GridLength(7, GridUnitType.Star)
            : new GridLength(settings.TyreDensity switch
            {
                OverviewTyreDensity.Normal => 166,
                OverviewTyreDensity.Compact1 => 62,
                OverviewTyreDensity.Compact2 => 36,
                OverviewTyreDensity.Compact3 or OverviewTyreDensity.Compact4 => 34,
                _ => 120,
            });
        _forceCardRefresh = true;
        QueueRefresh();
    }

    private void OnOverviewDisplayChanged() =>
        DispatcherQueue.TryEnqueue(ApplyDisplaySettings);

    private void OnChartWindowChanged(int seconds)
    {
        _windowSeconds = seconds;
        SurfaceChart.SetWindowSeconds(seconds);
        InnerChart.SetWindowSeconds(seconds);
        BrakeChart.SetWindowSeconds(seconds);
        WearChart.SetWindowSeconds(seconds);
        ConfigureTimeTicks();
        _forcePlotRefresh = true;
        QueueRefresh();
    }

    private void OnTyreWearModeChanged(TyreWearDisplayMode mode)
    {
        WearChart.SetWearMode(mode);
        QueueRefresh();
    }

    private void OnActualThemeChanged(FrameworkElement sender, object args)
    {
        ApplyTheme();
        _forceCardRefresh = true;
        _forcePlotRefresh = true;
        QueueRefresh();
    }

    private void ApplyTheme()
    {
        if (TelemetryPlot is null)
        {
            return;
        }
        var dark = ActualTheme == ElementTheme.Dark;
        var axes = dark
            ? UiColor.FromArgb(255, 124, 128, 152)
            : UiColor.FromArgb(255, 107, 114, 128);
        var grid = dark
            ? UiColor.FromArgb(13, 255, 255, 255)
            : UiColor.FromArgb(12, 0, 0, 0);
        _timeAxis!.Color = axes;
        _speedAxis!.Color = SpeedColor;
        _rpmAxis!.Color = RpmColor;
        _ersAxis!.Color = ErsColor;
        TelemetryPlot.GridColor = grid;
        _telemetryTooltip?.ApplyTheme(dark);
        SurfaceChart.ApplyTheme(dark);
        InnerChart.ApplyTheme(dark);
        BrakeChart.ApplyTheme(dark);
        WearChart.ApplyTheme(dark);
    }

    private static IReadOnlyDictionary<string, double> ColorFields(
        TelemetrySample? telemetry, PlayerStatusData? status) =>
        new Dictionary<string, double>
        {
            ["engine_temp"] = telemetry?.EngineTemp ?? double.NaN,
            ["brake"] = telemetry?.Brake ?? double.NaN,
            ["ers_mode"] = status?.ErsMode ?? double.NaN,
            ["ers_pct"] = status?.ErsPct ?? double.NaN,
            ["fuel_laps"] = status?.FuelLaps ?? double.NaN,
            ["visual_compound"] = status?.VisualCompound ?? double.NaN,
        };

    private static string Label(OverviewSnapshot snapshot, string key, string fallback) =>
        snapshot.Labels.TryGetValue(key, out var value) ? value : fallback;

    private Brush DamageBrush(double? value) =>
        value is null ? PrimaryBrush() :
        value > 0 ? NegativeBrush() :
        new SolidColorBrush(ActualTheme == ElementTheme.Dark
            ? UiColor.FromArgb(255, 55, 135, 45)
            : UiColor.FromArgb(255, 19, 115, 51));

    private Brush PrimaryBrush() => new SolidColorBrush(
        ActualTheme == ElementTheme.Dark
            ? UiColor.FromArgb(255, 255, 255, 255)
            : UiColor.FromArgb(255, 0, 0, 0));
    private Brush SecondaryBrush() => new SolidColorBrush(
        ActualTheme == ElementTheme.Dark
            ? UiColor.FromArgb(255, 160, 168, 184)
            : UiColor.FromArgb(255, 86, 91, 112));
    private Brush DividerBrush() => new SolidColorBrush(
        ActualTheme == ElementTheme.Dark
            ? UiColor.FromArgb(255, 42, 46, 58)
            : UiColor.FromArgb(255, 217, 220, 227));
    private static Brush NegativeBrush() =>
        new SolidColorBrush(UiColor.FromArgb(255, 196, 22, 42));

    private static string FormatSessionTime(double seconds) =>
        $"{(int)(seconds / 60)}:{(int)(seconds % 60):00}";
    private static string FormatLapTime(double seconds) =>
        $"{(int)(seconds / 60)}:{seconds % 60:00.0}";

    private ChartTooltipData? BuildTelemetryTooltip(double x)
    {
        IReadOnlyList<OverviewChartPoint> current =
            _chartMode == OverviewChartMode.Default
                ? _sessionPoints
                : _currentLapPoints;
        var currentOrigin = _chartMode == OverviewChartMode.Default ||
            current.Count == 0
                ? 0
                : current[0].SessionTime;
        var currentPoint = NearestPoint(current, x + currentOrigin);

        var reference = ReferencePoints(_store?.OverviewSnapshot);
        var referenceOrigin = reference.Count == 0
            ? 0
            : reference[0].SessionTime;
        var referencePoint = NearestPoint(reference, x + referenceOrigin);
        if (currentPoint is null && referencePoint is null)
        {
            return null;
        }

        var snappedX = currentPoint is not null
            ? currentPoint.SessionTime - currentOrigin
            : referencePoint!.SessionTime - referenceOrigin;
        var entries = new List<ChartTooltipEntry>(6);
        if (referencePoint is not null)
        {
            AddTooltipEntries(entries, referencePoint, ReferenceTooltipLabel());
        }
        if (currentPoint is not null)
        {
            AddTooltipEntries(
                entries,
                currentPoint,
                referencePoint is null ? null : "CURR");
        }
        return new ChartTooltipData(
            snappedX,
            new ChartTooltipContent(
                _chartMode == OverviewChartMode.Default
                    ? FormatSessionTime(snappedX)
                    : FormatLapTime(snappedX),
                entries));
    }

    private string ReferenceTooltipLabel() =>
        _chartMode switch
        {
            OverviewChartMode.PreviousLap => "PL",
            OverviewChartMode.FastestLap => "FL",
            OverviewChartMode.Compare when CompareLapComboBox.SelectedItem is int lap =>
                $"L{lap}",
            _ => "REF",
        };

    private static void AddTooltipEntries(
        List<ChartTooltipEntry> entries,
        OverviewChartPoint point,
        string? group)
    {
        entries.Add(new ChartTooltipEntry(
            "Speed", $"{point.SpeedKph:N0} kph", SpeedColor, group));
        entries.Add(new ChartTooltipEntry(
            "RPM", $"{point.Rpm:N0}", RpmColor, group));
        entries.Add(new ChartTooltipEntry(
            "ERS", $"{point.ErsPct:N0}%", ErsColor, group));
    }

    private static OverviewChartPoint? NearestPoint(
        IReadOnlyList<OverviewChartPoint> points,
        double sessionTime)
    {
        if (points.Count == 0)
        {
            return null;
        }
        var low = 0;
        var high = points.Count;
        while (low < high)
        {
            var middle = low + ((high - low) / 2);
            if (points[middle].SessionTime < sessionTime)
            {
                low = middle + 1;
            }
            else
            {
                high = middle;
            }
        }
        if (low == 0)
        {
            return points[0];
        }
        if (low == points.Count)
        {
            return points[^1];
        }
        return sessionTime - points[low - 1].SessionTime <=
            points[low].SessionTime - sessionTime
                ? points[low - 1]
                : points[low];
    }

#if DEBUG
    private void AttachRenderProbe()
    {
        var count = 0;
        var total = 0d;
        var maximum = 0d;
        TelemetryPlot.DiagnosticsUpdated += details =>
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
                $"[D3D11Chart] Overview/SpeedRpmErs: avg {total / count:0.00} ms, " +
                $"max {maximum:0.00} ms, {details.SourcePoints:N0} source points, " +
                $"{details.SubmittedSegments:N0} segments, " +
                $"LOD={details.UsedReduction}, WARP={details.UsingWarp}");
            count = 0;
            total = 0;
            maximum = 0;
        };
    }
#endif

    private sealed record CardValue(
        string Label,
        string Value,
        string Unit,
        string Sub,
        string ColorKey,
        double? ColorValue,
        bool IsDamage);

    private sealed record TyreValue(
        string Name,
        string Abbreviation,
        int? Surface,
        int? Inner,
        int? Brake,
        double? Wear,
        int? Blisters);

    private sealed record MetricValue(string Label, string Text, Brush Brush);
}
