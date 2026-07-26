using System.Text.Json;
using Microsoft.UI.Xaml;
using Windows.Storage;

namespace TrackNRace.WinUI3;

public partial class App : Application
{
    private const string ThemeSettingKey = "AppTheme";
    private const string BackdropSettingKey = "WindowBackdrop";
    private const string TransparentNavigationContentSettingKey =
        "TransparentNavigationContent";
    private const string UdpPortSettingKey = "TelemetryUdpPort";
    private const string BindAddressSettingKey = "TelemetryBindAddress";
    private const string ProtocolSettingKey = "TelemetryProtocol";
    private const string ChartWindowSettingKey = "ChartWindowSeconds";
    private const string TyreWearDisplayModeKey = "TyreWearDisplayMode";
    private const string SessionCompactHeaderKey = "SessionCompactHeader";
    private const string SessionCompactCardsKey = "SessionCompactCards";
    private const string SessionWeatherDensityKey = "SessionWeatherDensity";
    private const string SessionSectorColorsKey = "SessionSectorColors";
    private const string SessionMapDimmedKey = "SessionMapDimmed";
    private const string SessionDriverModeKey = "SessionDriverMode";
    private const string SessionStaleTimeoutKey = "SessionStaleTimeoutSeconds";
    private const string SessionReduceAnimationsKey = "SessionReduceAnimations";
    private const string PowerCompactCardsKey = "PowerCompactCards";
    private const string MiscShowGForceKey = "MiscShowGForce";
    private const string MiscShowRideHeightKey = "MiscShowRideHeight";
    private const string OverviewCompactStatsKey = "OverviewCompactStats";
    private const string OverviewCompactDamageKey = "OverviewCompactDamage";
    private const string OverviewTyreDensityKey = "OverviewTyreDensity";
    private const string OverviewTyreViewModeKey = "OverviewTyreViewMode";
    private const string AnalyzeDisplayKey = "AnalyzeDisplay";

    public MainWindow MainWindow { get; private set; } = null!;
    public ElementTheme SelectedTheme { get; private set; }
    public WindowBackdrop SelectedBackdrop { get; private set; }
    public bool IsNavigationContentTransparent { get; private set; }
    public TelemetryEngine? Telemetry { get; private set; }
    internal TelemetrySessionStore? TelemetryState { get; private set; }
    public string TelemetryStartError { get; private set; } = string.Empty;
    public ushort TelemetryPort { get; private set; }
    public string TelemetryBindAddress { get; private set; }
    public TelemetryProtocol SelectedProtocol { get; private set; }
    public int ChartWindowSeconds { get; private set; }
    public TyreWearDisplayMode TyreWearMode { get; private set; }
    public SessionDisplaySettings SessionDisplay { get; private set; }
    public PowerDisplaySettings PowerDisplay { get; private set; }
    public MiscDisplaySettings MiscDisplay { get; private set; }
    public OverviewDisplaySettings OverviewDisplay { get; private set; }
    internal AnalyzeDisplaySettings AnalyzeDisplay { get; private set; }
    public event Action<int>? ChartWindowChanged;
    public event Action<TyreWearDisplayMode>? TyreWearModeChanged;
    public event Action? SessionDisplayChanged;
    public event Action? PowerDisplayChanged;
    public event Action? MiscDisplayChanged;
    public event Action? OverviewDisplayChanged;
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
        SelectedBackdrop = ReadSavedBackdrop();
        IsNavigationContentTransparent =
            ApplicationData.Current.LocalSettings.Values[
                TransparentNavigationContentSettingKey] is true;
        TelemetryPort = ReadUdpPort();
        TelemetryBindAddress =
            ApplicationData.Current.LocalSettings.Values[BindAddressSettingKey] as string
            ?? "0.0.0.0";
        SelectedProtocol = ReadProtocol();
        ChartWindowSeconds = ReadChartWindow();
        TyreWearMode = ReadTyreWearMode();
        SessionDisplay = ReadSessionDisplay();
        PowerDisplay = ReadPowerDisplay();
        MiscDisplay = ReadMiscDisplay();
        OverviewDisplay = ReadOverviewDisplay();
        AnalyzeDisplay = ReadAnalyzeDisplay();
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

    public void SetBackdrop(WindowBackdrop backdrop)
    {
        if (!Enum.IsDefined(backdrop))
        {
            return;
        }

        SelectedBackdrop = backdrop;
        ApplicationData.Current.LocalSettings.Values[BackdropSettingKey] =
            backdrop.ToString();
        MainWindow?.ApplyBackdrop(backdrop);
    }

    public void SetNavigationContentTransparent(bool isTransparent)
    {
        if (isTransparent == IsNavigationContentTransparent)
        {
            return;
        }

        IsNavigationContentTransparent = isTransparent;
        ApplicationData.Current.LocalSettings.Values[
            TransparentNavigationContentSettingKey] = isTransparent;
        MainWindow?.ApplyNavigationContentTransparency(isTransparent);
    }

    private static ElementTheme ReadSavedTheme()
    {
        var value = ApplicationData.Current.LocalSettings.Values[ThemeSettingKey] as string;
        return Enum.TryParse<ElementTheme>(value, out var theme) &&
            theme is ElementTheme.Default or ElementTheme.Light or ElementTheme.Dark
                ? theme
                : ElementTheme.Default;
    }

    private static WindowBackdrop ReadSavedBackdrop()
    {
        var value =
            ApplicationData.Current.LocalSettings.Values[BackdropSettingKey] as string;
        return Enum.TryParse<WindowBackdrop>(value, out var backdrop) &&
            Enum.IsDefined(backdrop)
                ? backdrop
                : WindowBackdrop.Mica;
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

    public void SetTyreWearMode(TyreWearDisplayMode mode)
    {
        if (mode is not (TyreWearDisplayMode.Life or TyreWearDisplayMode.Wear) ||
            mode == TyreWearMode)
        {
            return;
        }

        TyreWearMode = mode;
        ApplicationData.Current.LocalSettings.Values[TyreWearDisplayModeKey] =
            mode.ToString();
        TyreWearModeChanged?.Invoke(mode);
    }

    public void SetSessionDisplay(SessionDisplaySettings settings)
    {
        settings = settings with
        {
            StaleCarTimeoutSeconds = Math.Clamp(settings.StaleCarTimeoutSeconds, 0, 120),
        };
        if (settings == SessionDisplay)
        {
            return;
        }

        SessionDisplay = settings;
        var values = ApplicationData.Current.LocalSettings.Values;
        values[SessionCompactHeaderKey] = settings.CompactHeader;
        values[SessionCompactCardsKey] = settings.CompactCards;
        values[SessionWeatherDensityKey] = settings.WeatherDensity.ToString();
        values[SessionSectorColorsKey] = settings.SectorColors;
        values[SessionMapDimmedKey] = settings.MapDimmed;
        values[SessionDriverModeKey] = settings.DriverDisplayMode.ToString();
        values[SessionStaleTimeoutKey] = settings.StaleCarTimeoutSeconds;
        values[SessionReduceAnimationsKey] = settings.ReduceAnimations;
        SessionDisplayChanged?.Invoke();
    }

    public void SetPowerDisplay(PowerDisplaySettings settings)
    {
        if (settings == PowerDisplay)
        {
            return;
        }

        PowerDisplay = settings;
        ApplicationData.Current.LocalSettings.Values[PowerCompactCardsKey] =
            settings.CompactCards;
        PowerDisplayChanged?.Invoke();
    }

    public void SetMiscDisplay(MiscDisplaySettings settings)
    {
        if (settings == MiscDisplay)
        {
            return;
        }

        MiscDisplay = settings;
        var values = ApplicationData.Current.LocalSettings.Values;
        values[MiscShowGForceKey] = settings.ShowGForce;
        values[MiscShowRideHeightKey] = settings.ShowRideHeight;
        MiscDisplayChanged?.Invoke();
    }

    public void SetOverviewDisplay(OverviewDisplaySettings settings)
    {
        if (!Enum.IsDefined(settings.TyreDensity))
        {
            settings = settings with { TyreDensity = OverviewTyreDensity.Normal };
        }
        if (!Enum.IsDefined(settings.TyreViewMode))
        {
            settings = settings with { TyreViewMode = OverviewTyreViewMode.Cards };
        }
        if (settings == OverviewDisplay)
        {
            return;
        }

        OverviewDisplay = settings;
        var values = ApplicationData.Current.LocalSettings.Values;
        values[OverviewCompactStatsKey] = settings.CompactStats;
        values[OverviewCompactDamageKey] = settings.CompactDamage;
        values[OverviewTyreDensityKey] = settings.TyreDensity.ToString();
        values[OverviewTyreViewModeKey] = settings.TyreViewMode.ToString();
        OverviewDisplayChanged?.Invoke();
    }

    internal void SetAnalyzeDisplay(AnalyzeDisplaySettings settings)
    {
        AnalyzeDisplay = AnalyzeDisplaySettings.Sanitize(settings);
        ApplicationData.Current.LocalSettings.Values[AnalyzeDisplayKey] =
            JsonSerializer.Serialize(AnalyzeDisplay);
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

    private static TyreWearDisplayMode ReadTyreWearMode()
    {
        var value =
            ApplicationData.Current.LocalSettings.Values[TyreWearDisplayModeKey] as string;
        return Enum.TryParse<TyreWearDisplayMode>(value, out var mode) &&
            mode is TyreWearDisplayMode.Life or TyreWearDisplayMode.Wear
                ? mode
                : TyreWearDisplayMode.Life;
    }

    private static SessionDisplaySettings ReadSessionDisplay()
    {
        var values = ApplicationData.Current.LocalSettings.Values;
        var weatherText = values[SessionWeatherDensityKey] as string;
        var driverText = values[SessionDriverModeKey] as string;
        var weather = Enum.TryParse<SessionWeatherDensity>(weatherText, out var parsedWeather)
            ? parsedWeather
            : SessionWeatherDensity.Normal;
        var driver = Enum.TryParse<TrackDriverDisplayMode>(driverText, out var parsedDriver)
            ? parsedDriver
            : TrackDriverDisplayMode.DotsAndLabels;
        var timeout = values[SessionStaleTimeoutKey] is int storedTimeout
            ? Math.Clamp(storedTimeout, 0, 120)
            : 10;
        return new SessionDisplaySettings(
            values[SessionCompactHeaderKey] is true,
            values[SessionCompactCardsKey] is true,
            weather,
            values[SessionSectorColorsKey] is true,
            values[SessionMapDimmedKey] is true,
            driver,
            timeout,
            values[SessionReduceAnimationsKey] is true);
    }

    private static PowerDisplaySettings ReadPowerDisplay()
    {
        var values = ApplicationData.Current.LocalSettings.Values;
        return new PowerDisplaySettings(
            values[PowerCompactCardsKey] is true);
    }

    private static MiscDisplaySettings ReadMiscDisplay()
    {
        var values = ApplicationData.Current.LocalSettings.Values;
        return new MiscDisplaySettings(
            values[MiscShowGForceKey] is not false,
            values[MiscShowRideHeightKey] is not false);
    }

    private static OverviewDisplaySettings ReadOverviewDisplay()
    {
        var values = ApplicationData.Current.LocalSettings.Values;
        var density = Enum.TryParse<OverviewTyreDensity>(
                values[OverviewTyreDensityKey] as string, out var parsedDensity) &&
            Enum.IsDefined(parsedDensity)
                ? parsedDensity
                : OverviewTyreDensity.Normal;
        var viewMode = Enum.TryParse<OverviewTyreViewMode>(
                values[OverviewTyreViewModeKey] as string, out var parsedViewMode) &&
            Enum.IsDefined(parsedViewMode)
                ? parsedViewMode
                : OverviewTyreViewMode.Cards;
        return new OverviewDisplaySettings(
            values[OverviewCompactStatsKey] is true,
            values[OverviewCompactDamageKey] is true,
            density,
            viewMode);
    }

    private static AnalyzeDisplaySettings ReadAnalyzeDisplay()
    {
        var json =
            ApplicationData.Current.LocalSettings.Values[AnalyzeDisplayKey] as string;
        if (string.IsNullOrWhiteSpace(json))
        {
            return AnalyzeDisplaySettings.Default;
        }
        try
        {
            return AnalyzeDisplaySettings.Sanitize(
                JsonSerializer.Deserialize<AnalyzeDisplaySettings>(json));
        }
        catch (JsonException)
        {
            return AnalyzeDisplaySettings.Default;
        }
        catch (NotSupportedException)
        {
            return AnalyzeDisplaySettings.Default;
        }
    }
}
