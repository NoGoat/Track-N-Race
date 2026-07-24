using Microsoft.UI.Xaml;
using Windows.Storage;

namespace TrackNRace.WinUI3;

public partial class App : Application
{
    private const string ThemeSettingKey = "AppTheme";

    public MainWindow MainWindow { get; private set; } = null!;
    public ElementTheme SelectedTheme { get; private set; }

    public App()
    {
        InitializeComponent();
        SelectedTheme = ReadSavedTheme();
    }

    public void SetTheme(ElementTheme theme)
    {
        if (theme is not (ElementTheme.Light or ElementTheme.Dark))
        {
            return;
        }

        SelectedTheme = theme;
        ApplicationData.Current.LocalSettings.Values[ThemeSettingKey] = theme.ToString();
        MainWindow?.ApplyTheme(theme);
    }

    private static ElementTheme ReadSavedTheme()
    {
        var value = ApplicationData.Current.LocalSettings.Values[ThemeSettingKey] as string;
        return Enum.TryParse<ElementTheme>(value, out var theme) &&
            theme is ElementTheme.Light or ElementTheme.Dark
                ? theme
                : ElementTheme.Dark;
    }

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        MainWindow = new MainWindow();
        MainWindow.Activate();
    }
}
