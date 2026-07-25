using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace TrackNRace.WinUI3;

public sealed partial class TyresPage : Page
{
    private TelemetrySessionStore? _store;
    private int _telemetryCount;
    private long _telemetryEpoch = -1;
    private int _damageCount;
    private long _damageEpoch = -1;
    private long _timelineRevision = -1;
    private bool _isLoaded;
    private bool _showingGraphs;
    private int _refreshQueued;

    internal TyresPageViewModel ViewModel { get; } = new();

    public TyresPage()
    {
        InitializeComponent();
        TyreSetsList.ItemContainerTransitions.Clear();
        var app = (App)Application.Current;
        SurfaceChart.Configure(
            TyreChartKind.Surface, app.TyreWearMode, app.ChartWindowSeconds);
        InnerChart.Configure(
            TyreChartKind.Inner, app.TyreWearMode, app.ChartWindowSeconds);
        BrakeChart.Configure(
            TyreChartKind.Brake, app.TyreWearMode, app.ChartWindowSeconds);
        WearChart.Configure(
            TyreChartKind.Wear, app.TyreWearMode, app.ChartWindowSeconds);
    }

    private void OnLoaded(object sender, RoutedEventArgs args)
    {
        if (_isLoaded)
        {
            return;
        }

        _isLoaded = true;
        _showingGraphs = false;
        AllocationRoot.Visibility = Visibility.Visible;
        GraphsRoot.Visibility = Visibility.Collapsed;
        var app = (App)Application.Current;
        _store = app.TelemetryState;
        if (_store is not null)
        {
            _store.TelemetryChanged += OnTelemetryChanged;
            _store.TyresChanged += OnTyresChanged;
            _store.TimelineReset += OnTimelineReset;
        }
        app.ChartWindowChanged += OnChartWindowChanged;
        app.TyreWearModeChanged += OnTyreWearModeChanged;
        ApplyChartTheme();
        RefreshFromStore(includeChartData: false);
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
            _store.TelemetryChanged -= OnTelemetryChanged;
            _store.TyresChanged -= OnTyresChanged;
            _store.TimelineReset -= OnTimelineReset;
            _store = null;
        }
        var app = (App)Application.Current;
        app.ChartWindowChanged -= OnChartWindowChanged;
        app.TyreWearModeChanged -= OnTyreWearModeChanged;
        _showingGraphs = false;
        Interlocked.Exchange(ref _refreshQueued, 0);
    }

    private void OnTelemetryChanged() => QueueRefresh();

    private void OnTyresChanged() => QueueRefresh();

    private void QueueRefresh()
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
                    RefreshFromStore(_showingGraphs);
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
        DispatcherQueue.TryEnqueue(() =>
        {
            if (!_isLoaded)
            {
                return;
            }
            ResetChartData();
            ViewModel.ResetTimeline();
            RefreshFromStore(_showingGraphs);
        });
    }

    private void RefreshFromStore(bool includeChartData)
    {
        if (!_isLoaded || _store is null)
        {
            return;
        }

        var snapshot = _store.TyresSnapshot;
        ViewModel.Apply(snapshot, ActualTheme == ElementTheme.Dark);
        if (!includeChartData)
        {
            return;
        }

        var telemetry = _store.ReadTelemetry(
            _telemetryCount, _telemetryEpoch, _timelineRevision);
        if (telemetry.Reset)
        {
            SurfaceChart.ClearData();
            InnerChart.ClearData();
            BrakeChart.ClearData();
        }
        SurfaceChart.AppendTelemetry(telemetry.Samples);
        InnerChart.AppendTelemetry(telemetry.Samples);
        BrakeChart.AppendTelemetry(telemetry.Samples);
        _telemetryCount = telemetry.TotalCount;
        _telemetryEpoch = telemetry.BufferEpoch;
        _timelineRevision = telemetry.TimelineRevision;

        var damage = _store.ReadDamage(
            _damageCount, _damageEpoch, _timelineRevision);
        if (damage.Reset)
        {
            WearChart.ClearData();
        }
        WearChart.AppendDamage(damage.Rows);
        _damageCount = damage.TotalCount;
        _damageEpoch = damage.BufferEpoch;
        _timelineRevision = damage.TimelineRevision;

        var latest = snapshot.LatestTelemetry?.SessionTime ??
            ((App)Application.Current).Telemetry?.LatestSessionTime ?? 0;
        SurfaceChart.RefreshChart(latest);
        InnerChart.RefreshChart(latest);
        BrakeChart.RefreshChart(latest);
        WearChart.RefreshChart(latest);
    }

    private void OnShowGraphs(object sender, RoutedEventArgs args)
    {
        _showingGraphs = true;
        AllocationRoot.Visibility = Visibility.Collapsed;
        GraphsRoot.Visibility = Visibility.Visible;
        RefreshFromStore(includeChartData: true);
    }

    private void OnShowAllocation(object sender, RoutedEventArgs args)
    {
        _showingGraphs = false;
        GraphsRoot.Visibility = Visibility.Collapsed;
        AllocationRoot.Visibility = Visibility.Visible;
        RefreshFromStore(includeChartData: false);
    }

    private void OnChartWindowChanged(int seconds)
    {
        SurfaceChart.SetWindowSeconds(seconds);
        InnerChart.SetWindowSeconds(seconds);
        BrakeChart.SetWindowSeconds(seconds);
        WearChart.SetWindowSeconds(seconds);
        if (_isLoaded && _showingGraphs)
        {
            RefreshFromStore(includeChartData: true);
        }
    }

    private void OnTyreWearModeChanged(TyreWearDisplayMode mode)
    {
        WearChart.SetWearMode(mode);
        if (_isLoaded && _showingGraphs)
        {
            WearChart.RefreshChart(
                _store?.TyresSnapshot.LatestTelemetry?.SessionTime ??
                ((App)Application.Current).Telemetry?.LatestSessionTime ?? 0);
        }
    }

    private void OnActualThemeChanged(FrameworkElement sender, object args)
    {
        ApplyChartTheme();
        if (_isLoaded)
        {
            ViewModel.ResetTimeline();
            RefreshFromStore(_showingGraphs);
        }
    }

    private void ApplyChartTheme()
    {
        if (SurfaceChart is null)
        {
            return;
        }
        var dark = ActualTheme == ElementTheme.Dark;
        SurfaceChart.ApplyTheme(dark);
        InnerChart.ApplyTheme(dark);
        BrakeChart.ApplyTheme(dark);
        WearChart.ApplyTheme(dark);
    }

    private void ResetChartData()
    {
        SurfaceChart.ClearData();
        InnerChart.ClearData();
        BrakeChart.ClearData();
        WearChart.ClearData();
        _telemetryCount = 0;
        _telemetryEpoch = -1;
        _damageCount = 0;
        _damageEpoch = -1;
        _timelineRevision = -1;
    }
}
