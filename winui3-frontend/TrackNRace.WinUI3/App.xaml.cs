using Microsoft.UI.Xaml;
using Windows.Storage;

namespace TrackNRace.WinUI3;

public partial class App : Application
{
    private const string ThemeSettingKey = "AppTheme";
    private const string UdpPortSettingKey = "TelemetryUdpPort";
    private const string BindAddressSettingKey = "TelemetryBindAddress";
    private const string ProtocolSettingKey = "TelemetryProtocol";
    private const string ChartWindowSettingKey = "ChartWindowSeconds";

    public MainWindow MainWindow { get; private set; } = null!;
    public ElementTheme SelectedTheme { get; private set; }
    public TelemetryEngine? Telemetry { get; private set; }
    internal TelemetrySessionStore? TelemetryState { get; private set; }
    public string TelemetryStartError { get; private set; } = string.Empty;
    public ushort TelemetryPort { get; private set; }
    public string TelemetryBindAddress { get; private set; }
    public TelemetryProtocol SelectedProtocol { get; private set; }
    public int ChartWindowSeconds { get; private set; }
    public event Action<int>? ChartWindowChanged;
    private int _selectedDriverIndex = -1;

    internal int? SelectedDriverIndex
    {
        get
        {
            var value = Volatile.Read(ref _selectedDriverIndex);
            return value < 0 ? null : value;
        }
        set => Volatile.Write(ref _selectedDriverIndex, value ?? -1);
    }

    public App()
    {
        InitializeComponent();
        SelectedTheme = ReadSavedTheme();
        TelemetryPort = ReadUdpPort();
        TelemetryBindAddress =
            ApplicationData.Current.LocalSettings.Values[BindAddressSettingKey] as string
            ?? "0.0.0.0";
        SelectedProtocol = ReadProtocol();
        ChartWindowSeconds = ReadChartWindow();
    }

    public void SetTheme(ElementTheme theme)
    {
        if (theme is not (ElementTheme.Default or ElementTheme.Light or ElementTheme.Dark))
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
            theme is ElementTheme.Default or ElementTheme.Light or ElementTheme.Dark
                ? theme
                : ElementTheme.Default;
    }

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        try
        {
            Telemetry = new TelemetryEngine(
                TelemetryPort, TelemetryBindAddress, SelectedProtocol);
            TelemetryState = new TelemetrySessionStore(Telemetry);
            TelemetryState.TimelineReset += OnTimelineReset;
            if (!Telemetry.TryStart(out var error))
            {
                TelemetryStartError = error;
            }
        }
        catch (Exception exception)
        {
            TelemetryStartError = exception.Message;
        }

        MainWindow = new MainWindow();
        MainWindow.Activate();
    }

    public bool ApplyTelemetrySettings(
        ushort port,
        string bindAddress,
        TelemetryProtocol protocol,
        out string error)
    {
        if (Telemetry is null)
        {
            error = TelemetryStartError.Length > 0
                ? TelemetryStartError
                : "The native telemetry engine is unavailable.";
            return false;
        }

        if (!Telemetry.TryRestartUdp(port, bindAddress, out error))
        {
            TelemetryStartError = error;
            return false;
        }

        Telemetry.SetProtocol(protocol);
        TelemetryPort = port;
        TelemetryBindAddress = bindAddress;
        SelectedProtocol = protocol;
        TelemetryStartError = string.Empty;

        var values = ApplicationData.Current.LocalSettings.Values;
        values[UdpPortSettingKey] = (int)port;
        values[BindAddressSettingKey] = bindAddress;
        values[ProtocolSettingKey] = protocol.ToString();
        return true;
    }

    public void ShutdownTelemetry()
    {
        if (TelemetryState is not null)
        {
            TelemetryState.TimelineReset -= OnTimelineReset;
            TelemetryState.Dispose();
            TelemetryState = null;
        }
        Telemetry?.Dispose();
        Telemetry = null;
    }

    private void OnTimelineReset(TimelineResetReason reason)
    {
        if (reason == TimelineResetReason.PlaybackClosed)
        {
            SelectedDriverIndex = null;
        }
    }

    public void SetChartWindow(int seconds)
    {
        if (seconds is not (15 or 30 or 60 or 120 or 300 or 600) ||
            seconds == ChartWindowSeconds)
        {
            return;
        }

        ChartWindowSeconds = seconds;
        ApplicationData.Current.LocalSettings.Values[ChartWindowSettingKey] = seconds;
        ChartWindowChanged?.Invoke(seconds);
    }

    private static ushort ReadUdpPort()
    {
        var value = ApplicationData.Current.LocalSettings.Values[UdpPortSettingKey];
        return value is int port && port is > 0 and <= ushort.MaxValue
            ? (ushort)port
            : (ushort)20777;
    }

    private static TelemetryProtocol ReadProtocol()
    {
        var value =
            ApplicationData.Current.LocalSettings.Values[ProtocolSettingKey] as string;
        return Enum.TryParse<TelemetryProtocol>(value, out var protocol)
            ? protocol
            : TelemetryProtocol.Auto;
    }

    private static int ReadChartWindow()
    {
        var value = ApplicationData.Current.LocalSettings.Values[ChartWindowSettingKey];
        return value is int seconds &&
            seconds is 15 or 30 or 60 or 120 or 300 or 600
                ? seconds
                : 30;
    }
}
