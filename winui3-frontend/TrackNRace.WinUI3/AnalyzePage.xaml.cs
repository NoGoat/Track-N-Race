using System.Collections.ObjectModel;
using System.Collections.Specialized;
using System.Numerics;
using System.Runtime.InteropServices;
using Microsoft.UI.Composition;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using TrackNRace.Charting;
using Windows.Foundation;
using Windows.UI;
using Windows.UI.ViewManagement;

namespace TrackNRace.WinUI3;

internal sealed record AnalyzeLapOption(int? LapNumber, string DisplayName);

public sealed partial class AnalyzePage : Page
{
    private const int MaxChartPoints = 750_000;
    private const double ExpandedSidebarWidth = 300;
    private static readonly TimeSpan SidebarAnimationDuration =
        TimeSpan.FromMilliseconds(200);
    private readonly List<TelemetrySample> _telemetry = [];
    private readonly List<PlayerStatusData> _status = [];
    private readonly List<MotionSample> _motion = [];
    private readonly List<MotionExSample> _motionEx = [];
    private readonly List<DamageRowData> _damage = [];
    private readonly Dictionary<string, ChartLineSeries> _currentSeries = [];
    private readonly Dictionary<string, ChartLineSeries> _comparisonSeries = [];
    private readonly Dictionary<string, List<ChartPoint>> _currentPoints = [];
    private readonly Dictionary<string, List<ChartPoint>> _comparisonPoints = [];
    private readonly List<(UIElement Element, ICompositionAnimationBase Animation)>
        _sidebarAnimations = [];
    private readonly HashSet<int> _requestedLaps = [];
    private readonly Flyout _colorFlyout;
    private readonly ColorPicker _colorPicker;
    private AnalyzeMetricItem? _colorTarget;
    private TelemetrySessionStore? _store;
    private ChartAxis? _timeAxis;
    private ChartCrosshairTooltipPlugin? _tooltip;
    private int _telemetryCount;
    private int _statusCount;
    private int _motionCount;
    private int _motionExCount;
    private int _damageCount;
    private long _telemetryEpoch = -1;
    private long _statusEpoch = -1;
    private long _motionEpoch = -1;
    private long _motionExEpoch = -1;
    private long _damageEpoch = -1;
    private long _timelineRevision = -1;
    private float? _playbackLapStart;
    private int? _playbackLapNumber;
    private float _currentOrigin;
    private int? _currentLapNumber;
    private bool _isLoaded;
    private bool _liveSeriesInitialized;
    private bool _sidebarCollapsed;
    private bool _fixedLapMode;
    private bool _restoringConfiguration = true;
    private bool _updatingLapSelectors;
    private bool _updatingColor;
    private double _fullRangeMaximum = 1;
    private int _refreshQueued;
    private int _sidebarAnimationVersion;

    internal ObservableCollection<AnalyzeMetricItem> Metrics { get; } = [];

    public AnalyzePage()
    {
        InitializeComponent();
        var settings = ((App)Application.Current).AnalyzeDisplay;
        _sidebarCollapsed = settings.Collapsed is true;
        YAxisToggle.IsOn = settings.ShowYAxis is not false;
        foreach (var saved in settings.Series ?? [])
        {
            if (AnalyzeMetrics.ById.TryGetValue(saved.MetricId, out var definition))
            {
                AddMetric(
                    definition,
                    saved.Visible is not false,
                    ParseColor(saved.Color, definition.DefaultColor));
            }
        }

        _colorPicker = new ColorPicker
        {
            IsAlphaEnabled = false,
            IsAlphaSliderVisible = false,
            IsAlphaTextInputVisible = false,
            IsColorChannelTextInputVisible = true,
            IsColorPreviewVisible = true,
            IsColorSliderVisible = true,
            IsColorSpectrumVisible = true,
            IsHexInputVisible = true,
            IsMoreButtonVisible = false,
        };
        _colorPicker.ColorChanged += OnMetricColorChanged;
        _colorFlyout = new Flyout { Content = _colorPicker };
        ConfigureChart();
        RefreshMetricChoices();
        ApplySidebarLayout(_sidebarCollapsed);
        CollapseIcon.Glyph = _sidebarCollapsed ? "\uE8A0" : "\uE89F";
        ToolTipService.SetToolTip(
            CollapseSidebarButton,
            _sidebarCollapsed
                ? "Open Analyze controls"
                : "Collapse Analyze controls");
        _restoringConfiguration = false;
        Metrics.CollectionChanged += OnMetricsCollectionChanged;
        UpdateControlState();
    }

    private void OnLoaded(object sender, RoutedEventArgs args)
    {
        if (_isLoaded) return;
        _isLoaded = true;
        var app = (App)Application.Current;
        _store = app.TelemetryState;
        if (_store is not null)
        {
            _store.SnapshotChanged += OnStoreChanged;
            _store.TelemetryChanged += OnStoreChanged;
            _store.MiscChanged += OnStoreChanged;
            _store.PowerChanged += OnStoreChanged;
            _store.TyresChanged += OnStoreChanged;
            _store.AnalyzeLapDataChanged += OnAnalyzeLapDataChanged;
            _store.TimelineReset += OnTimelineReset;
        }
        ApplyTheme();
        RefreshFromStore();
    }

    private void OnUnloaded(object sender, RoutedEventArgs args)
    {
        if (!_isLoaded) return;
        _isLoaded = false;
        Interlocked.Exchange(ref _refreshQueued, 0);
        CancelSidebarAnimation();
        if (_store is not null)
        {
            _store.SnapshotChanged -= OnStoreChanged;
            _store.TelemetryChanged -= OnStoreChanged;
            _store.MiscChanged -= OnStoreChanged;
            _store.PowerChanged -= OnStoreChanged;
            _store.TyresChanged -= OnStoreChanged;
            _store.AnalyzeLapDataChanged -= OnAnalyzeLapDataChanged;
            _store.TimelineReset -= OnTimelineReset;
            _store = null;
        }
    }

    private void ConfigureChart()
    {
        AnalyzePlot.Plugins.Add(new ChartBackgroundPlugin());
        _tooltip = new ChartCrosshairTooltipPlugin(BuildTooltip);
        AnalyzePlot.Plugins.Add(_tooltip);
        AnalyzePlot.RenderFailed += error =>
        {
            NoDataText.Text = "Chart unavailable";
            NoDataText.Visibility = Visibility.Visible;
            System.Diagnostics.Debug.WriteLine($"[AnalyzeChart] {error}");
        };
        ReconfigureMetricSeries();
    }

    private void ReconfigureMetricSeries()
    {
        AnalyzePlot.Series.Clear();
        AnalyzePlot.Axes.Clear();
        _currentSeries.Clear();
        _comparisonSeries.Clear();

        _timeAxis = new ChartAxis(
            "time", ChartAxisOrientation.X, ChartAxisSide.Bottom)
        {
            Minimum = 0,
            Maximum = Math.Max(1, _fullRangeMaximum),
            TickProvider = new IntervalChartTickProvider(10),
            LabelFormatter = FormatLapTime,
            ShowGridLines = true,
        };
        AnalyzePlot.Axes.Add(_timeAxis);

        var visible = Metrics.Where(metric => metric.IsVisible).ToArray();
        var scales = visible
            .GroupBy(metric => metric.Definition.ScaleKey)
            .Select(group => group.First())
            .ToArray();
        for (var index = 0; index < scales.Length; index++)
        {
            var item = scales[index];
            var definition = item.Definition;
            var axis = new ChartAxis(
                definition.ScaleKey,
                ChartAxisOrientation.Y,
                index % 2 == 0 ? ChartAxisSide.Left : ChartAxisSide.Right)
            {
                Minimum = definition.Minimum,
                Maximum = definition.Maximum,
                TickProvider = new FixedChartTickProvider(
                    definition.Minimum,
                    definition.Minimum + (definition.Maximum - definition.Minimum) * .25,
                    definition.Minimum + (definition.Maximum - definition.Minimum) * .5,
                    definition.Minimum + (definition.Maximum - definition.Minimum) * .75,
                    definition.Maximum),
                LabelFormatter = definition.AxisFormat,
                Color = item.Color,
                ShowGridLines = index == 0,
                IsVisible = YAxisToggle?.IsOn != false,
            };
            AnalyzePlot.Axes.Add(axis);
        }

        foreach (var item in visible.Reverse())
        {
            var definition = item.Definition;
            _comparisonSeries[item.Id] = AnalyzePlot.Series.Add(
                new ChartLineSeriesOptions(
                    $"comparison-{item.Id}",
                    "time",
                    definition.ScaleKey,
                    ReferenceColor(item.Color),
                    Thickness: 1.25f,
                    Visible: false,
                    MaximumPointCount: MaxChartPoints));
            _currentSeries[item.Id] = AnalyzePlot.Series.Add(
                new ChartLineSeriesOptions(
                    $"current-{item.Id}",
                    "time",
                    definition.ScaleKey,
                    item.Color,
                    Thickness: 1.75f,
                    MaximumPointCount: MaxChartPoints));
        }
        RebuildAllSeries(resetView: true);
    }

    private void OnStoreChanged() => QueueRefresh();

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

    private void OnAnalyzeLapDataChanged(int lapNumber)
    {
        DispatcherQueue.TryEnqueue(() =>
        {
            _requestedLaps.Remove(lapNumber);
            RebuildAllSeries(resetView: true);
        });
    }

    private void OnTimelineReset(TimelineResetReason reason)
    {
        DispatcherQueue.TryEnqueue(() =>
        {
            ResetReadState();
            if (reason == TimelineResetReason.PlaybackClosed)
            {
                _requestedLaps.Clear();
            }
            QueueRefresh();
        });
    }

    private void RefreshFromStore()
    {
        if (!_isLoaded || _store is null) return;

        var snapshot = _store.OverviewSnapshot;
        var telemetry = _store.ReadTelemetry(
            _telemetryCount, _telemetryEpoch, _timelineRevision);
        if (telemetry.Reset) _telemetry.Clear();
        _telemetry.AddRange(telemetry.Samples);
        _telemetryCount = telemetry.TotalCount;
        _telemetryEpoch = telemetry.BufferEpoch;
        _playbackLapStart = telemetry.PlaybackLapStart;
        _playbackLapNumber = telemetry.PlaybackLapNumber;

        var misc = _store.ReadMisc(
            _motionCount, _motionEpoch,
            _motionExCount, _motionExEpoch,
            _timelineRevision);
        if (misc.Reset)
        {
            _motion.Clear();
            _motionEx.Clear();
        }
        _motion.AddRange(misc.Motion);
        _motionEx.AddRange(misc.MotionEx);
        _motionCount = misc.MotionTotalCount;
        _motionExCount = misc.MotionExTotalCount;
        _motionEpoch = misc.MotionBufferEpoch;
        _motionExEpoch = misc.MotionExBufferEpoch;

        var power = _store.ReadPower(
            _statusCount, _statusEpoch, _timelineRevision);
        if (power.Reset) _status.Clear();
        _status.AddRange(power.Rows);
        _statusCount = power.TotalCount;
        _statusEpoch = power.BufferEpoch;

        var damage = _store.ReadDamage(
            _damageCount, _damageEpoch, _timelineRevision);
        if (damage.Reset) _damage.Clear();
        _damage.AddRange(damage.Rows);
        _damageCount = damage.TotalCount;
        _damageEpoch = damage.BufferEpoch;
        _timelineRevision = snapshot.TimelineRevision;

        RefreshLapChoices();
        if (_fixedLapMode)
        {
            return;
        }

        var lapNumber = _playbackLapNumber ?? snapshot.Lap?.LapNum;
        var sourcesReset =
            telemetry.Reset || misc.Reset || power.Reset || damage.Reset;
        var lapChanged = _currentLapNumber != lapNumber;
        if (!_liveSeriesInitialized || sourcesReset || lapChanged)
        {
            RebuildAllSeries(resetView: false);
            return;
        }

        AppendLiveSeries(
            _currentOrigin,
            telemetry.Samples,
            power.Rows,
            misc.Motion,
            misc.MotionEx,
            damage.Rows);
    }

    private void RebuildAllSeries(bool resetView)
    {
        if (_timeAxis is null) return;
        _currentPoints.Clear();
        _comparisonPoints.Clear();

        var primaryLap = _fixedLapMode
            ? SelectedLapData(LapAComboBox)
            : null;
        var comparisonLap = _fixedLapMode
            ? SelectedLapData(LapBComboBox)
            : SelectedLapData(CompareLapComboBox);
        RequestSelectedLap(LapAComboBox);
        RequestSelectedLap(LapBComboBox);
        RequestSelectedLap(CompareLapComboBox);

        var origin = primaryLap?.StartSessionTime ?? CurrentLapOrigin();
        var currentEnd = 0d;
        var comparisonEnd = 0d;
        foreach (var item in Metrics.Where(metric => metric.IsVisible))
        {
            var current = primaryLap is not null
                ? BuildPoints(item.Definition, primaryLap, primaryLap.StartSessionTime)
                : BuildLivePoints(item.Definition, origin);
            var comparison = comparisonLap is not null
                ? BuildPoints(
                    item.Definition, comparisonLap, comparisonLap.StartSessionTime)
                : [];
            _currentPoints[item.Id] = current;
            _comparisonPoints[item.Id] = comparison;
            if (_currentSeries.TryGetValue(item.Id, out var currentSeries))
            {
                currentSeries.Replace(CollectionsMarshal.AsSpan(current));
            }
            if (_comparisonSeries.TryGetValue(item.Id, out var comparisonSeries))
            {
                comparisonSeries.Replace(CollectionsMarshal.AsSpan(comparison));
                comparisonSeries.Visible = comparison.Count > 1;
            }
            if (current.Count > 0) currentEnd = Math.Max(currentEnd, current[^1].X);
            if (comparison.Count > 0)
            {
                comparisonEnd = Math.Max(comparisonEnd, comparison[^1].X);
            }
        }

        _liveSeriesInitialized = !_fixedLapMode;
        if (_liveSeriesInitialized)
        {
            _currentOrigin = origin;
            _currentLapNumber =
                _playbackLapNumber ?? _store?.OverviewSnapshot.Lap?.LapNum;
        }
        UpdateChartState(currentEnd, comparisonEnd, primaryLap, resetView);
        UpdateControlState();
    }

    private void AppendLiveSeries(
        float origin,
        IReadOnlyList<TelemetrySample> telemetry,
        IReadOnlyList<PlayerStatusData> status,
        IReadOnlyList<MotionSample> motion,
        IReadOnlyList<MotionExSample> motionEx,
        IReadOnlyList<DamageRowData> damage)
    {
        foreach (var item in Metrics.Where(metric => metric.IsVisible))
        {
            IEnumerable<object> rows = item.Definition.Source switch
            {
                AnalyzeMetricSource.Telemetry => telemetry,
                AnalyzeMetricSource.Status => status,
                AnalyzeMetricSource.Motion => motion,
                AnalyzeMetricSource.MotionEx => motionEx,
                AnalyzeMetricSource.Damage => damage,
                _ => [],
            };
            var appended = BuildPoints(item.Definition, rows, origin);
            if (appended.Count == 0)
            {
                continue;
            }
            if (!_currentPoints.TryGetValue(item.Id, out var points))
            {
                points = [];
                _currentPoints[item.Id] = points;
            }
            points.AddRange(appended);
            if (_currentSeries.TryGetValue(item.Id, out var series))
            {
                series.Append(CollectionsMarshal.AsSpan(appended));
            }
        }

        var currentEnd = LastPointX(_currentPoints);
        var comparisonEnd = LastPointX(_comparisonPoints);
        UpdateChartState(
            currentEnd, comparisonEnd, primaryLap: null, resetView: false);
    }

    private void UpdateChartState(
        double currentEnd,
        double comparisonEnd,
        AnalyzeLapData? primaryLap,
        bool resetView)
    {
        if (_timeAxis is null) return;
        _fullRangeMaximum = Math.Max(1, Math.Max(currentEnd, comparisonEnd));
        var interval = _fullRangeMaximum <= 1
            ? .1
            : Math.Max(1, Math.Ceiling(_fullRangeMaximum / 8));
        _timeAxis.TickProvider = new IntervalChartTickProvider(interval);
        if (resetView || !_fixedLapMode ||
            _timeAxis.Maximum <= _timeAxis.Minimum ||
            _timeAxis.Maximum > _fullRangeMaximum + .001)
        {
            ResetChartView();
        }
        NoDataText.Text = _fixedLapMode &&
            LapAComboBox.SelectedItem is AnalyzeLapOption { LapNumber: not null } &&
            primaryLap is null
                ? "Loading lap data…"
                : "Waiting for telemetry";
        NoDataText.Visibility = currentEnd > 0 || comparisonEnd > 0
            ? Visibility.Collapsed
            : Visibility.Visible;
        AnalyzePlot.Invalidate();
    }

    private static double LastPointX(
        IReadOnlyDictionary<string, List<ChartPoint>> points)
    {
        var maximum = 0d;
        foreach (var values in points.Values)
        {
            if (values.Count > 0)
            {
                maximum = Math.Max(maximum, values[^1].X);
            }
        }
        return maximum;
    }

    private List<ChartPoint> BuildLivePoints(
        AnalyzeMetricDefinition definition, float origin)
    {
        IEnumerable<object> rows = definition.Source switch
        {
            AnalyzeMetricSource.Telemetry => _telemetry,
            AnalyzeMetricSource.Status => _status,
            AnalyzeMetricSource.Motion => _motion,
            AnalyzeMetricSource.MotionEx => _motionEx,
            AnalyzeMetricSource.Damage => _damage,
            _ => [],
        };
        return BuildPoints(definition, rows, origin);
    }

    private static List<ChartPoint> BuildPoints(
        AnalyzeMetricDefinition definition,
        AnalyzeLapData lap,
        float origin)
    {
        IEnumerable<object> rows = definition.Source switch
        {
            AnalyzeMetricSource.Telemetry => lap.Telemetry,
            AnalyzeMetricSource.Status => lap.Status,
            AnalyzeMetricSource.Motion => lap.Motion,
            AnalyzeMetricSource.MotionEx => lap.MotionEx,
            AnalyzeMetricSource.Damage => lap.Damage,
            _ => [],
        };
        return BuildPoints(definition, rows, origin);
    }

    private static List<ChartPoint> BuildPoints(
        AnalyzeMetricDefinition definition,
        IEnumerable<object> rows,
        float origin)
    {
        var points = new List<ChartPoint>();
        var lastX = double.NegativeInfinity;
        foreach (var row in rows)
        {
            var sessionTime = SessionTime(row);
            if (sessionTime < origin) continue;
            var x = sessionTime - origin;
            var y = definition.Value(row);
            if (!double.IsFinite(y)) continue;
            if (Math.Abs(x - lastX) < .000001 && points.Count > 0)
            {
                points[^1] = new ChartPoint(x, y);
            }
            else
            {
                points.Add(new ChartPoint(x, y));
                lastX = x;
            }
        }
        return points;
    }

    private float CurrentLapOrigin()
    {
        if (_fixedLapMode) return 0;
        if (_playbackLapStart is float playbackStart) return playbackStart;
        var snapshot = _store?.OverviewSnapshot;
        var latest = snapshot?.LatestTelemetry?.SessionTime ?? 0;
        var elapsed = Math.Max(0, snapshot?.Lap?.CurrentLapMs ?? 0) / 1000f;
        return Math.Max(0, latest - elapsed);
    }

    private AnalyzeLapData? SelectedLapData(ComboBox comboBox) =>
        comboBox.SelectedItem is AnalyzeLapOption { LapNumber: int lapNumber }
            ? _store?.GetAnalyzeLapData(lapNumber)
            : null;

    private void RequestSelectedLap(ComboBox comboBox)
    {
        if (comboBox.SelectedItem is not AnalyzeLapOption { LapNumber: int lapNumber } ||
            _store?.GetAnalyzeLapData(lapNumber) is not null ||
            !_requestedLaps.Add(lapNumber))
        {
            return;
        }
        ((App)Application.Current).Telemetry?.RequestLapData(lapNumber);
    }

    private void RefreshLapChoices()
    {
        var laps = ((App)Application.Current).Telemetry?.PlaybackLaps ?? [];
        var existing = LapAComboBox.ItemsSource as AnalyzeLapOption[];
        if (existing is not null &&
            existing.Select(item => item.LapNumber)
                .SequenceEqual(laps.Select(lap => (int?)lap.LapNumber)))
        {
            return;
        }

        var selectedA = (LapAComboBox.SelectedItem as AnalyzeLapOption)?.LapNumber;
        var selectedB = (LapBComboBox.SelectedItem as AnalyzeLapOption)?.LapNumber;
        var selectedCompare =
            (CompareLapComboBox.SelectedItem as AnalyzeLapOption)?.LapNumber;
        var options = laps
            .Select(lap => new AnalyzeLapOption(lap.LapNumber, $"Lap {lap.LapNumber}"))
            .ToArray();
        var compareOptions = new[]
        {
            new AnalyzeLapOption(null, "None"),
        }.Concat(options).ToArray();

        _updatingLapSelectors = true;
        LapAComboBox.ItemsSource = options;
        LapBComboBox.ItemsSource = options;
        CompareLapComboBox.ItemsSource = compareOptions;
        LapAComboBox.SelectedItem =
            options.FirstOrDefault(item => item.LapNumber == selectedA);
        LapBComboBox.SelectedItem =
            options.FirstOrDefault(item => item.LapNumber == selectedB);
        CompareLapComboBox.SelectedItem =
            compareOptions.FirstOrDefault(item => item.LapNumber == selectedCompare) ??
            compareOptions[0];
        _updatingLapSelectors = false;
        FixedLapModeToggle.IsEnabled = options.Length > 0;
        CompareLapComboBox.IsEnabled = options.Length > 0;
        UpdateControlState();
    }

    private void ResetReadState()
    {
        _telemetry.Clear();
        _status.Clear();
        _motion.Clear();
        _motionEx.Clear();
        _damage.Clear();
        _telemetryCount = _statusCount = _motionCount = _motionExCount = _damageCount = 0;
        _telemetryEpoch = _statusEpoch = _motionEpoch =
            _motionExEpoch = _damageEpoch = -1;
        _timelineRevision = -1;
        _playbackLapStart = null;
        _playbackLapNumber = null;
        _currentLapNumber = null;
        _currentOrigin = 0;
        _liveSeriesInitialized = false;
    }

    private void SaveConfiguration()
    {
        if (_restoringConfiguration)
        {
            return;
        }
        ((App)Application.Current).SetAnalyzeDisplay(
            new AnalyzeDisplaySettings(
                1,
                _sidebarCollapsed,
                YAxisToggle.IsOn,
                Metrics.Select(item => new AnalyzeSeriesDisplaySettings(
                    item.Id,
                    AnalyzeDisplaySettings.ColorHex(item.Color),
                    item.IsVisible)).ToArray()));
    }

    private static Color ParseColor(string value, Color fallback)
    {
        if (value is not { Length: 7 } || value[0] != '#')
        {
            return fallback;
        }
        try
        {
            return Color.FromArgb(
                255,
                Convert.ToByte(value[1..3], 16),
                Convert.ToByte(value[3..5], 16),
                Convert.ToByte(value[5..7], 16));
        }
        catch (FormatException)
        {
            return fallback;
        }
    }

    private void RefreshMetricChoices()
    {
        var selected = Metrics.Select(item => item.Id).ToHashSet(StringComparer.Ordinal);
        AddMetricComboBox.ItemsSource = AnalyzeMetrics.All
            .Where(definition => !selected.Contains(definition.Id))
            .OrderBy(definition => definition.Group)
            .ThenBy(definition => definition.Label)
            .ToArray();
    }

    private void AddMetric(
        AnalyzeMetricDefinition definition,
        bool isVisible = true,
        Color? color = null)
    {
        var item = new AnalyzeMetricItem(definition);
        item.Color = color ?? definition.DefaultColor;
        item.IsVisible = isVisible;
        item.PropertyChanged += (_, args) =>
        {
            if (args.PropertyName is nameof(AnalyzeMetricItem.IsVisible) or
                nameof(AnalyzeMetricItem.Color))
            {
                ReconfigureMetricSeries();
                SaveConfiguration();
            }
        };
        Metrics.Add(item);
    }

    private void OnMetricsCollectionChanged(
        object? sender,
        NotifyCollectionChangedEventArgs args)
    {
        RefreshMetricChoices();
        ReconfigureMetricSeries();
        SaveConfiguration();
    }

    private void OnAddMetricChanged(object sender, SelectionChangedEventArgs args)
    {
        if (AddMetricComboBox.SelectedItem is not AnalyzeMetricDefinition definition)
        {
            return;
        }
        AddMetric(definition);
        AddMetricComboBox.SelectedItem = null;
    }

    private AnalyzeMetricItem? MetricFrom(object sender) =>
        (sender as FrameworkElement)?.DataContext as AnalyzeMetricItem;

    private void OnMoveMetricUp(object sender, RoutedEventArgs args)
    {
        var item = MetricFrom(sender);
        var index = item is null ? -1 : Metrics.IndexOf(item);
        if (index > 0)
        {
            Metrics.Move(index, index - 1);
        }
    }

    private void OnMoveMetricDown(object sender, RoutedEventArgs args)
    {
        var item = MetricFrom(sender);
        var index = item is null ? -1 : Metrics.IndexOf(item);
        if (index >= 0 && index < Metrics.Count - 1)
        {
            Metrics.Move(index, index + 1);
        }
    }

    private void OnToggleMetric(object sender, RoutedEventArgs args)
    {
        if (MetricFrom(sender) is { } item) item.IsVisible = !item.IsVisible;
    }

    private void OnResetMetricColor(object sender, RoutedEventArgs args)
    {
        if (MetricFrom(sender) is { } item) item.Color = item.Definition.DefaultColor;
    }

    private void OnRemoveMetric(object sender, RoutedEventArgs args)
    {
        if (MetricFrom(sender) is not { } item) return;
        Metrics.Remove(item);
    }

    private void OnColorButtonClicked(object sender, RoutedEventArgs args)
    {
        if (sender is not Button button || MetricFrom(sender) is not { } item) return;
        _colorTarget = item;
        _updatingColor = true;
        _colorPicker.Color = item.Color;
        _updatingColor = false;
        _colorFlyout.ShowAt(button);
    }

    private void OnMetricColorChanged(ColorPicker sender, ColorChangedEventArgs args)
    {
        if (!_updatingColor && _colorTarget is not null)
        {
            _colorTarget.Color = args.NewColor;
        }
    }

    private void OnFixedLapModeToggled(object sender, RoutedEventArgs args)
    {
        _fixedLapMode = FixedLapModeToggle.IsOn;
        FixedLapSelectors.Visibility =
            _fixedLapMode ? Visibility.Visible : Visibility.Collapsed;
        CompareLapSelector.Visibility =
            _fixedLapMode ? Visibility.Collapsed : Visibility.Visible;
        RebuildAllSeries(resetView: true);
        UpdateControlState();
    }

    private void OnLapSelectionChanged(object sender, SelectionChangedEventArgs args)
    {
        if (_updatingLapSelectors) return;
        RebuildAllSeries(resetView: true);
    }

    private void OnYAxisToggled(object sender, RoutedEventArgs args)
    {
        if (AnalyzePlot is null) return;
        foreach (var axis in AnalyzePlot.Axes.Where(
            axis => axis.Orientation == ChartAxisOrientation.Y))
        {
            axis.IsVisible = YAxisToggle.IsOn;
        }
        AnalyzePlot.Invalidate();
        SaveConfiguration();
    }

    private void OnCollapseSidebar(object sender, RoutedEventArgs args)
    {
        _sidebarCollapsed = !_sidebarCollapsed;
        AnimateSidebarTo(_sidebarCollapsed ? 0 : ExpandedSidebarWidth);
        CollapseIcon.Glyph = _sidebarCollapsed ? "\uE8A0" : "\uE89F";
        ToolTipService.SetToolTip(
            sender as DependencyObject,
            _sidebarCollapsed ? "Open Analyze controls" : "Collapse Analyze controls");
        SaveConfiguration();
    }

    private void AnimateSidebarTo(double targetWidth)
    {
        CancelSidebarAnimation();
        var collapsing = targetWidth <= 0;
        var oldChartWidth = Math.Max(1, ChartArea.ActualWidth);
        if (!new UISettings().AnimationsEnabled)
        {
            ApplySidebarLayout(collapsing);
            return;
        }

        // Change XAML layout once, then use a FLIP transform to visually bridge
        // the old and new bounds entirely on the compositor thread.
        if (!collapsing)
        {
            SidebarDivider.Visibility = Visibility.Visible;
        }
        Sidebar.Width = ExpandedSidebarWidth;
        SidebarColumn.Width = new GridLength(
            collapsing ? 0 : ExpandedSidebarWidth);
        RootGrid.UpdateLayout();

        var newChartWidth = Math.Max(1, ChartArea.ActualWidth);
        var chartStartX = collapsing
            ? (float)ExpandedSidebarWidth
            : -(float)ExpandedSidebarWidth;
        var sidebarStartX = collapsing
            ? 0
            : -(float)ExpandedSidebarWidth;
        var sidebarTargetX = collapsing
            ? -(float)ExpandedSidebarWidth
            : 0;
        var chartStartScale = (float)(oldChartWidth / newChartWidth);

        Sidebar.Translation = new Vector3(sidebarTargetX, 0, 0);
        SidebarDivider.Translation = Vector3.Zero;
        ChartArea.Translation = Vector3.Zero;
        ChartArea.CenterPoint = Vector3.Zero;
        ChartArea.Scale = Vector3.One;

        var compositor = CompositionTarget.GetCompositorForCurrentThread();
        var easing = compositor.CreateCubicBezierEasingFunction(
            new Vector2(.1f, .9f),
            new Vector2(.2f, 1));
        var version = ++_sidebarAnimationVersion;
        var batch = compositor.CreateScopedBatch(CompositionBatchTypes.Animation);
        StartCompositionAnimation(
            Sidebar, "Translation",
            new Vector3(sidebarStartX, 0, 0),
            new Vector3(sidebarTargetX, 0, 0),
            easing);
        StartCompositionAnimation(
            SidebarDivider, "Translation",
            new Vector3(chartStartX, 0, 0),
            Vector3.Zero,
            easing);
        StartCompositionAnimation(
            ChartArea, "Translation",
            new Vector3(chartStartX, 0, 0),
            Vector3.Zero,
            easing);
        StartCompositionAnimation(
            ChartArea, "Scale",
            new Vector3(chartStartScale, 1, 1),
            Vector3.One,
            easing);
        batch.End();
        batch.Completed += (_, _) =>
        {
            if (version != _sidebarAnimationVersion)
            {
                return;
            }
            DispatcherQueue.TryEnqueue(() => CompleteSidebarAnimation(collapsing));
        };
    }

    private void StartCompositionAnimation(
        UIElement element,
        string target,
        Vector3 from,
        Vector3 to,
        CompositionEasingFunction easing)
    {
        var animation = easing.Compositor.CreateVector3KeyFrameAnimation();
        animation.Target = target;
        animation.Duration = SidebarAnimationDuration;
        animation.InsertKeyFrame(0, from);
        animation.InsertKeyFrame(1, to, easing);
        element.StartAnimation(animation);
        _sidebarAnimations.Add((element, animation));
    }

    private void CompleteSidebarAnimation(bool collapsed)
    {
        CancelSidebarAnimation();
        ApplySidebarLayout(collapsed);
    }

    private void CancelSidebarAnimation()
    {
        _sidebarAnimationVersion++;
        foreach (var (element, animation) in _sidebarAnimations)
        {
            element.StopAnimation(animation);
        }
        _sidebarAnimations.Clear();
    }

    private void ApplySidebarLayout(bool collapsed)
    {
        Sidebar.Translation = Vector3.Zero;
        SidebarDivider.Translation = Vector3.Zero;
        ChartArea.Translation = Vector3.Zero;
        ChartArea.Scale = Vector3.One;
        Sidebar.Width = double.NaN;
        SidebarColumn.Width = new GridLength(
            collapsed ? 0 : ExpandedSidebarWidth);
        SidebarDivider.Visibility = collapsed
            ? Visibility.Collapsed
            : Visibility.Visible;
    }

    private void OnZoomIn(object sender, RoutedEventArgs args) => Zoom(.7);
    private void OnZoomOut(object sender, RoutedEventArgs args) => Zoom(1 / .7);
    private void OnPanLeft(object sender, RoutedEventArgs args) => Pan(-.2);
    private void OnPanRight(object sender, RoutedEventArgs args) => Pan(.2);
    private void OnResetView(object sender, RoutedEventArgs args) => ResetChartView();

    private void Zoom(double factor)
    {
        if (!_fixedLapMode || _timeAxis is null) return;
        var center = (_timeAxis.Minimum + _timeAxis.Maximum) / 2;
        var extent = Math.Clamp(
            (_timeAxis.Maximum - _timeAxis.Minimum) * factor,
            Math.Min(2, _fullRangeMaximum),
            _fullRangeMaximum);
        SetView(center - extent / 2, center + extent / 2);
    }

    private void Pan(double fraction)
    {
        if (!_fixedLapMode || _timeAxis is null) return;
        var delta = (_timeAxis.Maximum - _timeAxis.Minimum) * fraction;
        SetView(_timeAxis.Minimum + delta, _timeAxis.Maximum + delta);
    }

    private void SetView(double minimum, double maximum)
    {
        if (_timeAxis is null) return;
        var extent = Math.Min(_fullRangeMaximum, maximum - minimum);
        minimum = Math.Clamp(minimum, 0, Math.Max(0, _fullRangeMaximum - extent));
        _timeAxis.Minimum = minimum;
        _timeAxis.Maximum = minimum + extent;
        AnalyzePlot.Invalidate();
    }

    private void ResetChartView()
    {
        if (_timeAxis is null) return;
        _timeAxis.Minimum = 0;
        _timeAxis.Maximum = Math.Max(1, _fullRangeMaximum);
        AnalyzePlot.Invalidate();
    }

    private void UpdateControlState()
    {
        var enabled = _fixedLapMode &&
            LapAComboBox?.SelectedItem is AnalyzeLapOption { LapNumber: not null };
        if (ZoomInButton is null) return;
        ZoomInButton.IsEnabled = enabled;
        ZoomOutButton.IsEnabled = enabled;
        PanLeftButton.IsEnabled = enabled;
        PanRightButton.IsEnabled = enabled;
        ResetViewButton.IsEnabled = enabled;
    }

    private ChartTooltipData? BuildTooltip(double x)
    {
        var entries = new List<ChartTooltipEntry>();
        var markers = new List<ChartTooltipMarker>();
        foreach (var item in Metrics.Where(metric => metric.IsVisible))
        {
            AddTooltipRole(item, x, _currentPoints, "CURRENT", item.Color, entries, markers);
            AddTooltipRole(
                item, x, _comparisonPoints, "COMPARE", ReferenceColor(item.Color),
                entries, markers);
        }
        return entries.Count == 0
            ? null
            : new ChartTooltipData(
                x,
                new ChartTooltipContent(FormatLapTime(x), entries),
                markers);
    }

    private static void AddTooltipRole(
        AnalyzeMetricItem item,
        double x,
        IReadOnlyDictionary<string, List<ChartPoint>> source,
        string group,
        Color color,
        List<ChartTooltipEntry> entries,
        List<ChartTooltipMarker> markers)
    {
        if (!source.TryGetValue(item.Id, out var points) || points.Count == 0)
        {
            return;
        }
        var point = Nearest(points, x);
        entries.Add(new ChartTooltipEntry(
            item.Label, item.Definition.Format(point.Y), color, group));
        markers.Add(new ChartTooltipMarker(
            point.X, point.Y, item.Definition.ScaleKey, color));
    }

    private static ChartPoint Nearest(IReadOnlyList<ChartPoint> points, double x)
    {
        var low = 0;
        var high = points.Count;
        while (low < high)
        {
            var middle = low + ((high - low) / 2);
            if (points[middle].X < x)
            {
                low = middle + 1;
            }
            else
            {
                high = middle;
            }
        }
        var index = low;
        if (index == 0) return points[0];
        if (index >= points.Count) return points[^1];
        return x - points[index - 1].X <= points[index].X - x
            ? points[index - 1]
            : points[index];
    }

    private void OnActualThemeChanged(FrameworkElement sender, object args)
    {
        if (AnalyzePlot is null) return;
        ApplyTheme();
        ReconfigureMetricSeries();
    }

    private void ApplyTheme()
    {
        if (_timeAxis is null) return;
        var dark = ActualTheme != ElementTheme.Light;
        _timeAxis.Color = dark
            ? Color.FromArgb(255, 124, 128, 152)
            : Color.FromArgb(255, 107, 114, 128);
        AnalyzePlot.GridColor = dark
            ? Color.FromArgb(10, 255, 255, 255)
            : Color.FromArgb(18, 0, 0, 0);
        _tooltip?.ApplyTheme(dark);
    }

    private Color ReferenceColor(Color color)
    {
        var dark = ActualTheme != ElementTheme.Light;
        var background = dark ? (byte)18 : (byte)255;
        return Color.FromArgb(
            255,
            (byte)Math.Round(color.R * .35 + background * .65),
            (byte)Math.Round(color.G * .35 + background * .65),
            (byte)Math.Round(color.B * .35 + background * .65));
    }

    private static float SessionTime(object row) => row switch
    {
        TelemetrySample value => value.SessionTime,
        PlayerStatusData value => value.SessionTime,
        MotionSample value => value.SessionTime,
        MotionExSample value => value.SessionTime,
        DamageRowData value => value.SessionTime,
        _ => 0,
    };

    private static string FormatLapTime(double seconds)
    {
        var safe = Math.Max(0, seconds);
        return $"{(int)(safe / 60)}:{safe % 60:00.0}";
    }
}
