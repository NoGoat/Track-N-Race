using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Navigation;
using Microsoft.UI.Xaml;
using System.Text.Json;

namespace TrackNRace.WinUI3;

public sealed partial class DashboardPage : Page
{
    private readonly DispatcherTimer _statusTimer = new()
    {
        Interval = TimeSpan.FromMilliseconds(250),
    };

    public DashboardPage()
    {
        InitializeComponent();
        _statusTimer.Tick += (_, _) => RefreshEngineStatus();
        Loaded += OnLoaded;
        Unloaded += OnUnloaded;
    }

    protected override void OnNavigatedTo(NavigationEventArgs args)
    {
        base.OnNavigatedTo(args);
        PageTitle.Text = args.Parameter as string ?? "Overview";
    }

    private void OnLoaded(object sender, RoutedEventArgs args)
    {
        RefreshEngineStatus();
        _statusTimer.Start();
    }

    private void OnUnloaded(object sender, RoutedEventArgs args)
    {
        _statusTimer.Stop();
    }

    private void RefreshEngineStatus()
    {
        var app = (App)Application.Current;
        var telemetry = app.Telemetry;
        if (telemetry is null || app.TelemetryStartError.Length > 0)
        {
            EngineInfo.Severity = InfoBarSeverity.Error;
            EngineStatusText.Text = app.TelemetryStartError.Length > 0
                ? app.TelemetryStartError
                : "The telemetry engine is unavailable.";
            PacketCountText.Text = string.Empty;
            return;
        }

        EngineInfo.Severity = InfoBarSeverity.Success;
        EngineStatusText.Text =
            $"Listening on {app.TelemetryBindAddress}:{app.TelemetryPort} · " +
            ProtocolDescription(telemetry.ProtocolStatusJson);
        PacketCountText.Text =
            $"{telemetry.RowCount:N0} JSON rows · " +
            $"{telemetry.BinaryBatchCount:N0} packed telemetry batches";
    }

    private static string ProtocolDescription(string? statusJson)
    {
        if (statusJson is null)
        {
            return "protocol auto-detection";
        }

        try
        {
            using var document = JsonDocument.Parse(statusJson);
            var root = document.RootElement;
            if (root.TryGetProperty("active_format", out var active) &&
                active.ValueKind == JsonValueKind.Number &&
                active.TryGetInt32(out var format))
            {
                return $"F1 {format}";
            }
        }
        catch (JsonException)
        {
        }

        return "protocol auto-detection";
    }
}
