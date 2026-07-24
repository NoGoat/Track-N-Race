using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media.Imaging;
using Windows.Graphics;
using Windows.Storage.Pickers;

namespace TrackNRace.WinUI3;

public sealed partial class MainWindow : Window
{
    private readonly DispatcherTimer _sessionTimer = new()
    {
        Interval = TimeSpan.FromMilliseconds(100),
    };
    private int _displayedSessionSecond = -1;
    private string? _loadedTnrdPath;
    private IReadOnlyList<PlaybackLap>? _displayedLaps;
    private bool _updatingPlaybackControls;
    private long _lastSliderSeekMs;

    public MainWindow()
    {
        InitializeComponent();
        var app = (App)Application.Current;
        RootLayout.RequestedTheme = app.SelectedTheme;
        ChartWindowComboBox.SelectedIndex = ChartWindowIndex(app.ChartWindowSeconds);
        _updatingPlaybackControls = true;
        PlaybackSpeedComboBox.SelectedIndex = 2;
        _updatingPlaybackControls = false;
        _sessionTimer.Tick += OnSessionTimerTick;
        _sessionTimer.Start();

        ExtendsContentIntoTitleBar = true;
        AppWindow.TitleBar.PreferredHeightOption = TitleBarHeightOption.Tall;
        SetTitleBar(AppTitleBar);
        AppWindow.SetIcon(Path.Combine(AppContext.BaseDirectory, "Assets", "AppIcon.ico"));
        if (AppWindow.Presenter is OverlappedPresenter presenter)
        {
            presenter.PreferredMinimumWidth = 900;
            presenter.PreferredMinimumHeight = 640;
        }

        AppWindow.Resize(new SizeInt32(1440, 920));
        Closed += (_, _) =>
        {
            _sessionTimer.Stop();
            app.ShutdownTelemetry();
        };
        RootLayout.Loaded += OnRootLayoutLoaded;
        RootLayout.ActualThemeChanged += OnActualThemeChanged;

        AppNavigationView.SelectedItem = OverviewNavigationItem;
        NavigateToDashboard("Overview");
    }

    private void OnNavigationSelectionChanged(
        NavigationView sender,
        NavigationViewSelectionChangedEventArgs args)
    {
        if (args.IsSettingsSelected)
        {
            if (RootFrame.CurrentSourcePageType != typeof(SettingsPage))
            {
                RootFrame.Navigate(typeof(SettingsPage));
            }
            return;
        }

        if (args.SelectedItemContainer?.Tag is string pageName)
        {
            NavigateToDashboard(pageName);
        }
    }

    private void NavigateToDashboard(string pageName)
    {
        if (pageName == "Standings")
        {
            if (RootFrame.CurrentSourcePageType != typeof(StandingsPage))
            {
                RootFrame.Navigate(typeof(StandingsPage));
            }
            return;
        }

        if (pageName == "Session")
        {
            if (RootFrame.CurrentSourcePageType != typeof(SessionPage))
            {
                RootFrame.Navigate(typeof(SessionPage));
            }
            return;
        }

        RootFrame.Navigate(typeof(DashboardPage), pageName);
    }

    private async void OnLoadTnrdClicked(object sender, TappedRoutedEventArgs args)
    {
        var picker = new FileOpenPicker
        {
            SuggestedStartLocation = PickerLocationId.DocumentsLibrary,
            ViewMode = PickerViewMode.List,
        };
        picker.FileTypeFilter.Add(".tnrd");
        WinRT.Interop.InitializeWithWindow.Initialize(
            picker, WinRT.Interop.WindowNative.GetWindowHandle(this));

        var file = await picker.PickSingleFileAsync();
        if (file is null)
        {
            return;
        }

        var telemetry = ((App)Application.Current).Telemetry;
        if (telemetry is null)
        {
            await ShowTnrdLoadErrorAsync("The telemetry engine is unavailable.");
            return;
        }

        if (_loadedTnrdPath is not null)
        {
            telemetry.CloseRecording();
            ResetClosedRecordingState();
        }

        ShowRecordingLoading(file.Path, file.Name);
        try
        {
            var result = await Task.Run(() =>
            {
                var loaded = telemetry.TryLoadRecording(file.Path, out var error);
                return (Loaded: loaded, Error: error);
            });

            if (!result.Loaded)
            {
                ShowLoadRecordingButton();
                RecordingLoadOverlay.Visibility = Visibility.Collapsed;
                await ShowTnrdLoadErrorAsync(
                    result.Error.Length > 0
                        ? result.Error
                        : "The selected file could not be loaded.");
                return;
            }

            ToolTipService.SetToolTip(
                LoadTnrdNavigationItem,
                $"Loaded {file.Name} · click to choose another recording");
            ShowLoadedRecording(file.Path, file.Name);
        }
        catch (Exception exception)
        {
            ShowLoadRecordingButton();
            RecordingLoadOverlay.Visibility = Visibility.Collapsed;
            await ShowTnrdLoadErrorAsync(exception.Message);
        }
        finally
        {
            RecordingLoadOverlay.Visibility = Visibility.Collapsed;
        }
    }

    private void ShowRecordingLoading(string path, string fileName)
    {
        LoadedFileNameText.Text = fileName;
        LoadedFileNameText.Visibility = Visibility.Visible;
        ToolTipService.SetToolTip(LoadedFileNameText, path);
        LoadTnrdNavigationItem.IsEnabled = false;
        RecordingLoadOverlay.Visibility = Visibility.Visible;
    }

    private void ShowLoadRecordingButton()
    {
        LoadedFileNameText.Text = string.Empty;
        LoadedFileNameText.Visibility = Visibility.Collapsed;
        ToolTipService.SetToolTip(LoadedFileNameText, null);
        LoadTnrdNavigationItem.IsEnabled = true;
        ToolTipService.SetToolTip(
            LoadTnrdNavigationItem, "Load Session Data File (.tnrd)");
    }

    private void ShowLoadedRecording(string path, string fileName)
    {
        _loadedTnrdPath = path;
        LoadedFileNameText.Text = fileName;
        LoadedFileNameText.Visibility = Visibility.Visible;
        ToolTipService.SetToolTip(LoadedFileNameText, path);
        LoadTnrdNavigationItem.IsEnabled = true;
        PlaybackBar.Visibility = Visibility.Visible;
        RefreshPlaybackControls();
    }

    private async Task ShowTnrdLoadErrorAsync(string message)
    {
        var dialog = new ContentDialog
        {
            XamlRoot = RootLayout.XamlRoot,
            Title = "Couldn’t load recording",
            Content = message,
            CloseButtonText = "Close",
        };
        await dialog.ShowAsync();
    }

    private void OnChartWindowSelectionChanged(
        object sender,
        SelectionChangedEventArgs args)
    {
        var seconds = ChartWindowComboBox.SelectedIndex switch
        {
            0 => 15,
            2 => 60,
            3 => 120,
            4 => 300,
            5 => 600,
            _ => 30,
        };
        ((App)Application.Current).SetChartWindow(seconds);
    }

    private void OnSessionTimerTick(object? sender, object args)
    {
        if (PlaybackBar.Visibility == Visibility.Visible)
        {
            RefreshPlaybackControls();
        }

        var telemetry = ((App)Application.Current).Telemetry;
        var sessionTime = telemetry?.LatestSessionTime;
        if (sessionTime is null)
        {
            if (_displayedSessionSecond != -1)
            {
                _displayedSessionSecond = -1;
                SessionTimerText.Text = "--:--";
            }
            return;
        }

        var totalSeconds = Math.Max(0, (int)sessionTime.Value);
        if (totalSeconds == _displayedSessionSecond)
        {
            return;
        }

        _displayedSessionSecond = totalSeconds;
        SessionTimerText.Text =
            $"{totalSeconds / 60}:{totalSeconds % 60:00}";
    }

    private void RefreshPlaybackControls()
    {
        var telemetry = ((App)Application.Current).Telemetry;
        var state = telemetry?.CurrentPlaybackState;
        if (telemetry is null || state is null)
        {
            return;
        }

        _updatingPlaybackControls = true;
        try
        {
            var progress = state.TotalTime > 0
                ? Math.Clamp(state.CurrentTime / state.TotalTime, 0, 1)
                : 0;
            PlaybackProgressSlider.Value = progress;
            PlaybackCurrentTimeText.Text = FormatPlaybackTime(state.CurrentTime);
            PlaybackTotalTimeText.Text = FormatPlaybackTime(state.TotalTime);

            PlayPauseIcon.Glyph = state.IsPlaying ? "\uE769" : "\uE768";
            ToolTipService.SetToolTip(
                PlayPauseButton,
                state.IsPlaying ? "Pause" : "Play");

            var speedIndex = SpeedIndex(state.Speed);
            if (PlaybackSpeedComboBox.SelectedIndex != speedIndex)
            {
                PlaybackSpeedComboBox.SelectedIndex = speedIndex;
            }

            var laps = telemetry.PlaybackLaps;
            if (!ReferenceEquals(laps, _displayedLaps))
            {
                _displayedLaps = laps;
                PlaybackLapComboBox.ItemsSource = laps;
                PlaybackLapComboBox.Visibility =
                    laps.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
            }

            PlaybackLap? currentLap = null;
            var absoluteTime = state.StartTime + state.CurrentTime;
            foreach (var lap in laps)
            {
                if (absoluteTime >= lap.StartSessionTime &&
                    absoluteTime <= lap.EndSessionTime)
                {
                    currentLap = lap;
                    break;
                }
            }
            if (!Equals(PlaybackLapComboBox.SelectedItem, currentLap))
            {
                PlaybackLapComboBox.SelectedItem = currentLap;
            }
        }
        finally
        {
            _updatingPlaybackControls = false;
        }
    }

    private void OnSeekBackwardClicked(object sender, RoutedEventArgs args)
    {
        SeekRelative(-5);
    }

    private void OnSeekForwardClicked(object sender, RoutedEventArgs args)
    {
        SeekRelative(5);
    }

    private void SeekRelative(float seconds)
    {
        var telemetry = ((App)Application.Current).Telemetry;
        var state = telemetry?.CurrentPlaybackState;
        if (telemetry is null || state is null || state.TotalTime <= 0)
        {
            return;
        }

        telemetry.Seek(Math.Clamp(
            (state.CurrentTime + seconds) / state.TotalTime, 0, 1));
    }

    private void OnPlayPauseClicked(object sender, RoutedEventArgs args)
    {
        var telemetry = ((App)Application.Current).Telemetry;
        var state = telemetry?.CurrentPlaybackState;
        if (telemetry is null || state is null)
        {
            return;
        }

        if (state.IsPlaying)
        {
            telemetry.Pause();
        }
        else
        {
            telemetry.Play();
        }
    }

    private void OnPlaybackSliderValueChanged(
        object sender,
        Microsoft.UI.Xaml.Controls.Primitives.RangeBaseValueChangedEventArgs args)
    {
        if (_updatingPlaybackControls)
        {
            return;
        }

        var now = Environment.TickCount64;
        if (now - _lastSliderSeekMs >= 100)
        {
            _lastSliderSeekMs = now;
            ((App)Application.Current).Telemetry?.Seek((float)args.NewValue);
        }
    }

    private void OnPlaybackSliderPointerReleased(
        object sender,
        Microsoft.UI.Xaml.Input.PointerRoutedEventArgs args)
    {
        if (!_updatingPlaybackControls)
        {
            ((App)Application.Current).Telemetry?.Seek(
                (float)PlaybackProgressSlider.Value);
        }
    }

    private void OnPlaybackSpeedSelectionChanged(
        object sender,
        SelectionChangedEventArgs args)
    {
        if (_updatingPlaybackControls)
        {
            return;
        }

        var speed = PlaybackSpeedComboBox.SelectedIndex switch
        {
            0 => 0.25f,
            1 => 0.5f,
            3 => 2.0f,
            4 => 4.0f,
            _ => 1.0f,
        };
        ((App)Application.Current).Telemetry?.SetPlaybackSpeed(speed);
    }

    private void OnPlaybackLapSelectionChanged(
        object sender,
        SelectionChangedEventArgs args)
    {
        if (_updatingPlaybackControls ||
            PlaybackLapComboBox.SelectedItem is not PlaybackLap lap)
        {
            return;
        }

        var telemetry = ((App)Application.Current).Telemetry;
        var state = telemetry?.CurrentPlaybackState;
        if (telemetry is null || state is null || state.TotalTime <= 0)
        {
            return;
        }

        telemetry.Seek(Math.Clamp(
            (lap.StartSessionTime - state.StartTime) / state.TotalTime, 0, 1));
    }

    private async void OnExportRecordingClicked(object sender, RoutedEventArgs args)
    {
        if (_loadedTnrdPath is null)
        {
            return;
        }

        var picker = new FileSavePicker
        {
            SuggestedStartLocation = PickerLocationId.DocumentsLibrary,
            SuggestedFileName = Path.GetFileNameWithoutExtension(_loadedTnrdPath),
        };
        picker.FileTypeChoices.Add(
            "Excel workbook", new List<string> { ".xlsx" });
        WinRT.Interop.InitializeWithWindow.Initialize(
            picker, WinRT.Interop.WindowNative.GetWindowHandle(this));

        var destination = await picker.PickSaveFileAsync();
        if (destination is null)
        {
            return;
        }

        var telemetry = ((App)Application.Current).Telemetry;
        if (telemetry is null)
        {
            await ShowTnrdLoadErrorAsync("The telemetry engine is unavailable.");
            return;
        }

        ExportRecordingButton.IsEnabled = false;
        try
        {
            var sourcePath = _loadedTnrdPath;
            var result = await Task.Run(() =>
            {
                var exported = telemetry.TryExportRecording(
                    sourcePath, destination.Path, out var error);
                return (Exported: exported, Error: error);
            });
            if (!result.Exported)
            {
                await ShowTnrdLoadErrorAsync(
                    result.Error.Length > 0
                        ? result.Error
                        : "The recording could not be exported.");
            }
        }
        finally
        {
            ExportRecordingButton.IsEnabled = true;
        }
    }

    private void OnCloseRecordingClicked(object sender, RoutedEventArgs args)
    {
        ((App)Application.Current).Telemetry?.CloseRecording();
        ResetClosedRecordingState();
    }

    private void ResetClosedRecordingState()
    {
        _loadedTnrdPath = null;
        _displayedLaps = null;
        PlaybackBar.Visibility = Visibility.Collapsed;
        ShowLoadRecordingButton();
        _displayedSessionSecond = -1;
        SessionTimerText.Text = "--:--";
    }

    private static string FormatPlaybackTime(float seconds)
    {
        var safe = Math.Max(0, seconds);
        return $"{(int)(safe / 60)}:{safe % 60:00.000}";
    }

    private static int SpeedIndex(float speed)
    {
        if (Math.Abs(speed - 0.25f) < 0.01f) return 0;
        if (Math.Abs(speed - 0.5f) < 0.01f) return 1;
        if (Math.Abs(speed - 2.0f) < 0.01f) return 3;
        if (Math.Abs(speed - 4.0f) < 0.01f) return 4;
        return 2;
    }

    private static int ChartWindowIndex(int seconds) => seconds switch
    {
        15 => 0,
        60 => 2,
        120 => 3,
        300 => 4,
        600 => 5,
        _ => 1,
    };

    public void ApplyTheme(ElementTheme theme)
    {
        RootLayout.RequestedTheme = theme;
        UpdateTitleBarIcon();
    }

    private void OnRootLayoutLoaded(object sender, RoutedEventArgs args)
    {
        UpdateTitleBarTheme();
        UpdateTitleBarIcon();
    }

    private void OnActualThemeChanged(FrameworkElement sender, object args)
    {
        UpdateTitleBarTheme();
        UpdateTitleBarIcon();
    }

    private void UpdateTitleBarTheme()
    {
        if (!AppWindowTitleBar.IsCustomizationSupported())
        {
            return;
        }

        var titleBar = AppWindow.TitleBar;
        titleBar.PreferredTheme = RootLayout.ActualTheme == ElementTheme.Dark
            ? TitleBarTheme.Dark
            : TitleBarTheme.Light;
        titleBar.ButtonBackgroundColor = Microsoft.UI.Colors.Transparent;
        titleBar.ButtonInactiveBackgroundColor = Microsoft.UI.Colors.Transparent;
    }

    private void UpdateTitleBarIcon()
    {
        var iconName = RootLayout.ActualTheme == ElementTheme.Light
            ? "icon_transparent_light.png"
            : "icon_transparent.png";

        TitleBarIconImage.Source = new BitmapImage(
            new Uri($"ms-appx:///Assets/{iconName}"));
    }
}
