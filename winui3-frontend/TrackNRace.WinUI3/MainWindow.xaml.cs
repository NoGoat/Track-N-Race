using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media.Imaging;
using Windows.Graphics;

namespace TrackNRace.WinUI3;

public sealed partial class MainWindow : Window
{
    public MainWindow()
    {
        InitializeComponent();
        RootLayout.RequestedTheme = ((App)Application.Current).SelectedTheme;

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
        RootFrame.Navigate(typeof(DashboardPage), pageName);
    }

    public void ApplyTheme(ElementTheme theme)
    {
        RootLayout.RequestedTheme = theme;
        UpdateTitleBarIcon();
    }

    private void OnRootLayoutLoaded(object sender, RoutedEventArgs args)
    {
        UpdateTitleBarIcon();
    }

    private void OnActualThemeChanged(FrameworkElement sender, object args)
    {
        UpdateTitleBarIcon();
    }

    private void UpdateTitleBarIcon()
    {
        var iconName = RootLayout.ActualTheme == ElementTheme.Light
            ? "icon_transparent_light.png"
            : "icon_transparent.png";

        TitleBarIconSource.ImageSource = new BitmapImage(
            new Uri($"ms-appx:///Assets/{iconName}"));
    }
}
