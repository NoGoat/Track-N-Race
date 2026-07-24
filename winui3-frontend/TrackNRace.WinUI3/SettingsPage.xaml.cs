using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace TrackNRace.WinUI3;

public sealed partial class SettingsPage : Page
{
    private bool _isInitialized;

    public SettingsPage()
    {
        InitializeComponent();
        var app = (App)Application.Current;
        ThemeComboBox.SelectedIndex = app.SelectedTheme switch
        {
            ElementTheme.Light => 0,
            ElementTheme.Dark => 1,
            _ => 2,
        };
        BindAddressTextBox.Text = app.TelemetryBindAddress;
        UdpPortNumberBox.Value = app.TelemetryPort;
        ProtocolComboBox.SelectedIndex = (int)app.SelectedProtocol;
        _isInitialized = true;
    }

    private void OnThemeSelectionChanged(object sender, SelectionChangedEventArgs args)
    {
        if (!_isInitialized)
        {
            return;
        }

        if (ThemeComboBox.SelectedItem is not ComboBoxItem selectedItem)
        {
            return;
        }

        var theme = (selectedItem.Tag as string) switch
        {
            "Light" => ElementTheme.Light,
            "Dark" => ElementTheme.Dark,
            _ => ElementTheme.Default,
        };
        ((App)Application.Current).SetTheme(theme);
    }

    private void OnApplyTelemetrySettings(object sender, RoutedEventArgs args)
    {
        if (double.IsNaN(UdpPortNumberBox.Value) ||
            UdpPortNumberBox.Value is < 1 or > ushort.MaxValue)
        {
            ShowTelemetryResult(
                InfoBarSeverity.Error, "Enter a UDP port between 1 and 65535.");
            return;
        }

        var bindAddress = BindAddressTextBox.Text.Trim();
        if (bindAddress.Length == 0)
        {
            ShowTelemetryResult(InfoBarSeverity.Error, "Enter a bind address.");
            return;
        }

        var protocol = ProtocolComboBox.SelectedIndex switch
        {
            1 => TelemetryProtocol.F1_24,
            2 => TelemetryProtocol.F1_25,
            3 => TelemetryProtocol.F1_26,
            _ => TelemetryProtocol.Auto,
        };

        var app = (App)Application.Current;
        if (!app.ApplyTelemetrySettings(
                checked((ushort)UdpPortNumberBox.Value),
                bindAddress,
                protocol,
                out var error))
        {
            ShowTelemetryResult(
                InfoBarSeverity.Error,
                error.Length > 0 ? error : "The UDP listener could not be restarted.");
            return;
        }

        ShowTelemetryResult(InfoBarSeverity.Success, "Telemetry settings applied.");
    }

    private void ShowTelemetryResult(InfoBarSeverity severity, string message)
    {
        TelemetrySettingsInfo.Severity = severity;
        TelemetrySettingsInfo.Message = message;
        TelemetrySettingsInfo.IsOpen = true;
    }
}
