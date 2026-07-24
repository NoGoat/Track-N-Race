using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace TrackNRace.WinUI3;

public sealed partial class SettingsPage : Page
{
    private bool _isInitialized;

    public SettingsPage()
    {
        InitializeComponent();
        ThemeRadioButtons.SelectedIndex =
            ((App)Application.Current).SelectedTheme == ElementTheme.Light ? 0 : 1;
        _isInitialized = true;
    }

    private void OnThemeSelectionChanged(object sender, SelectionChangedEventArgs args)
    {
        if (!_isInitialized)
        {
            return;
        }

        var theme = ThemeRadioButtons.SelectedIndex == 0
            ? ElementTheme.Light
            : ElementTheme.Dark;
        ((App)Application.Current).SetTheme(theme);
    }
}
