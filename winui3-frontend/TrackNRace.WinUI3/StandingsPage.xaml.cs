using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Animation;
using Windows.UI;
using Windows.UI.ViewManagement;

namespace TrackNRace.WinUI3;

public sealed partial class StandingsPage : Page
{
    private static readonly StandingsSnapshot EmptySnapshot = new(
        null,
        null,
        null,
        null,
        null,
        null,
        new Dictionary<string, string>(),
        0,
        0);

    private readonly UISettings _uiSettings = new();
    private TelemetrySessionStore? _store;
    private bool _isLoaded;
    private bool _animationsEnabled;
    private int _refreshQueued;

    internal StandingsPageViewModel ViewModel { get; } = new();

    public StandingsPage()
    {
        InitializeComponent();
    }

    private void OnLoaded(object sender, RoutedEventArgs args)
    {
        if (_isLoaded)
        {
            return;
        }

        _isLoaded = true;
        _store = ((App)Application.Current).TelemetryState;
        if (_store is not null)
        {
            _store.SnapshotChanged += OnSnapshotChanged;
            _store.TimelineReset += OnTimelineReset;
        }
        _uiSettings.AnimationsEnabledChanged += OnAnimationsEnabledChanged;
        UpdateAnimationPreference();
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
            _store.SnapshotChanged -= OnSnapshotChanged;
            _store.TimelineReset -= OnTimelineReset;
            _store = null;
        }
        _uiSettings.AnimationsEnabledChanged -= OnAnimationsEnabledChanged;
    }

    private void OnSnapshotChanged()
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

        DispatcherQueue.TryEnqueue(() =>
        {
            if (!_isLoaded)
            {
                return;
            }
            ViewModel.ResetTimeline();
            RefreshFromStore();
        });
    }

    private void RefreshFromStore()
    {
        var app = (App)Application.Current;
        var previousIndices = _animationsEnabled
            ? CaptureRowIndices()
            : null;
        var changes = ViewModel.Apply(
            _store?.Snapshot ?? EmptySnapshot,
            app.SelectedDriverIndex,
            ActualTheme == ElementTheme.Dark);

        var selectedRow = app.SelectedDriverIndex is int selectedIndex
            ? ViewModel.FindRow(selectedIndex)
            : null;
        if (!ReferenceEquals(StandingsList.SelectedItem, selectedRow))
        {
            StandingsList.SelectedItem = selectedRow;
        }

        if (_animationsEnabled && changes.Count > 0)
        {
            DispatcherQueue.TryEnqueue(
                DispatcherQueuePriority.Low,
                () => AnimatePositionChanges(changes, previousIndices!));
        }
    }

    private void OnStandingItemClicked(object sender, ItemClickEventArgs args)
    {
        if (args.ClickedItem is not StandingsRowViewModel row || row.IsPlaceholder)
        {
            return;
        }

        var app = (App)Application.Current;
        app.SelectedDriverIndex =
            app.SelectedDriverIndex == row.CarIndex ? null : row.CarIndex;
        RefreshFromStore();
    }

    private void OnActualThemeChanged(FrameworkElement sender, object args)
    {
        if (_isLoaded)
        {
            RefreshFromStore();
        }
    }

    private void OnAnimationsEnabledChanged(
        UISettings sender,
        UISettingsAnimationsEnabledChangedEventArgs args)
    {
        if (_isLoaded)
        {
            DispatcherQueue.TryEnqueue(UpdateAnimationPreference);
        }
    }

    private void UpdateAnimationPreference()
    {
        _animationsEnabled = _uiSettings.AnimationsEnabled;
        // ListView's built-in collection transition re-animates the entire
        // presenter for ObservableCollection.Move. Standings supplies its own
        // row-scoped FLIP animation, so no container transition may remain.
        StandingsList.ItemContainerTransitions.Clear();
        if (!_animationsEnabled)
        {
            foreach (var row in ViewModel.Rows)
            {
                if (StandingsList.ContainerFromItem(row) is ListViewItem container)
                {
                    container.RenderTransform = null;
                }
            }
        }
    }

    private Dictionary<int, int> CaptureRowIndices()
    {
        var indices = new Dictionary<int, int>();
        for (var index = 0; index < ViewModel.Rows.Count; index++)
        {
            var row = ViewModel.Rows[index];
            if (row.IsPlaceholder)
            {
                continue;
            }

            indices[row.CarIndex] = index;
        }
        return indices;
    }

    private void AnimatePositionChanges(
        IReadOnlyList<PositionChange> changes,
        IReadOnlyDictionary<int, int> previousIndices)
    {
        if (!_isLoaded || !_animationsEnabled)
        {
            return;
        }

        StandingsList.UpdateLayout();
        foreach (var change in changes)
        {
            var row = ViewModel.FindRow(change.CarIndex);
            if (row is null ||
                StandingsList.ContainerFromItem(row) is not ListViewItem container ||
                container.ContentTemplateRoot is not FrameworkElement templateRoot ||
                templateRoot.FindName("PositionFlashOverlay") is not Border overlay)
            {
                continue;
            }

            if (previousIndices.TryGetValue(change.CarIndex, out var previousIndex))
            {
                var currentIndex = ViewModel.Rows.IndexOf(row);
                var rowHeight = templateRoot.ActualHeight > 0
                    ? templateRoot.ActualHeight
                    : 34;
                var offset = (previousIndex - currentIndex) * rowHeight;
                if (Math.Abs(offset) > 0.5)
                {
                    var transform = new TranslateTransform();
                    container.RenderTransform = transform;
                    var moveAnimation = new DoubleAnimation
                    {
                        From = offset,
                        To = 0,
                        Duration = new Duration(TimeSpan.FromMilliseconds(400)),
                        EnableDependentAnimation = true,
                        EasingFunction = new CubicEase
                        {
                            EasingMode = EasingMode.EaseOut,
                        },
                    };
                    Storyboard.SetTarget(moveAnimation, transform);
                    Storyboard.SetTargetProperty(moveAnimation, "Y");
                    var moveStoryboard = new Storyboard();
                    moveStoryboard.Children.Add(moveAnimation);
                    moveStoryboard.Begin();
                }
            }

            overlay.Background = new SolidColorBrush(
                change.Gained
                    ? Color.FromArgb(56, 34, 197, 94)
                    : Color.FromArgb(46, 239, 68, 68));
            overlay.Opacity = 1;

            var animation = new DoubleAnimation
            {
                To = 0,
                Duration = new Duration(TimeSpan.FromSeconds(1)),
                EnableDependentAnimation = true,
            };
            Storyboard.SetTarget(animation, overlay);
            Storyboard.SetTargetProperty(animation, "Opacity");
            var storyboard = new Storyboard();
            storyboard.Children.Add(animation);
            storyboard.Begin();
        }
    }
}
