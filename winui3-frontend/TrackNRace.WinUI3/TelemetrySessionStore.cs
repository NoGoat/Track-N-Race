using System.Text.Json;

namespace TrackNRace.WinUI3;

internal enum TimelineResetReason
{
    Seek,
    SessionEnded,
    PlaybackClosed,
}

internal sealed record TimingCarData
{
    public int Idx { get; init; }
    public int Position { get; init; }
    public int LapNum { get; init; }
    public int CurrentLapMs { get; init; }
    public int LastLapMs { get; init; }
    public int S1Ms { get; init; }
    public int S2Ms { get; init; }
    public int GapMs { get; init; }
    public int PitStatus { get; init; }
    public bool LapInvalid { get; init; }
    public int PenaltiesS { get; init; }
    public int NumDtPens { get; init; }
    public int NumSgPens { get; init; }
    public int Sector { get; init; }
    public int ResultStatus { get; init; }
    public int DriverStatus { get; init; }
}

internal sealed record TimingRowData
{
    public string Ts { get; init; } = string.Empty;
    public float SessionTime { get; init; }
    public int PlayerIdx { get; init; }
    public TimingCarData[] Cars { get; init; } = [];
}

internal sealed record DriverData
{
    public int Idx { get; init; }
    public string Name { get; init; } = string.Empty;
    public int TeamId { get; init; }
    public int RaceNumber { get; init; }
    public bool Ai { get; init; }
    public string LiveryColor { get; init; } = string.Empty;
}

internal sealed record ParticipantsRowData
{
    public DriverData[] Drivers { get; init; } = [];
}

internal sealed record CarStatusData
{
    public int Idx { get; init; }
    public int FuelMix { get; init; }
    public int FrontBrakeBias { get; init; }
    public double FuelKg { get; init; }
    public double FuelLaps { get; init; }
    public bool DrsAllowed { get; init; }
    public int TyreCompound { get; init; }
    public int VisualCompound { get; init; }
    public int TyreAgeLaps { get; init; }
    public int ErsJ { get; init; }
    public double ErsPct { get; init; }
    public int ErsMode { get; init; }
    public int ErsDeployedJ { get; init; }
}

internal sealed record AllStatusRowData
{
    public CarStatusData[] Cars { get; init; } = [];
}

internal sealed record PlayerStatusData
{
    public float SessionTime { get; init; }
    public int FuelMix { get; init; }
    public int FrontBrakeBias { get; init; }
    public double FuelKg { get; init; }
    public double FuelLaps { get; init; }
    public bool DrsAllowed { get; init; }
    public int TyreCompound { get; init; }
    public int VisualCompound { get; init; }
    public int TyreAgeLaps { get; init; }
    public int ErsJ { get; init; }
    public double ErsPct { get; init; }
    public int ErsMode { get; init; }
    public int ErsDeployedJ { get; init; }

    public CarStatusData AsCarStatus(int carIndex) => new()
    {
        Idx = carIndex,
        FuelMix = FuelMix,
        FrontBrakeBias = FrontBrakeBias,
        FuelKg = FuelKg,
        FuelLaps = FuelLaps,
        DrsAllowed = DrsAllowed,
        TyreCompound = TyreCompound,
        VisualCompound = VisualCompound,
        TyreAgeLaps = TyreAgeLaps,
        ErsJ = ErsJ,
        ErsPct = ErsPct,
        ErsMode = ErsMode,
        ErsDeployedJ = ErsDeployedJ,
    };
}

internal sealed record LapRowData
{
    public string Ts { get; init; } = string.Empty;
    public float SessionTime { get; init; }
    public int LastLapMs { get; init; }
    public int CurrentLapMs { get; init; }
    public int S1Ms { get; init; }
    public int S2Ms { get; init; }
    public int Position { get; init; }
    public int LapNum { get; init; }
    public int PitStatus { get; init; }
    public int NumPitStops { get; init; }
    public int Sector { get; init; }
    public bool LapInvalid { get; init; }
    public int PenaltiesS { get; init; }
}

internal sealed record StandingsSnapshot(
    TimingRowData? Timing,
    ParticipantsRowData? Participants,
    AllStatusRowData? AllStatus,
    LapRowData? Lap,
    PlayerStatusData? PlayerStatus,
    int? FastestLapCarIndex,
    IReadOnlyDictionary<string, string> Labels,
    long Revision,
    long TimelineRevision);

internal sealed class TelemetrySessionStore : IDisposable
{
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNameCaseInsensitive = true,
        PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower,
    };

    private static readonly IReadOnlyDictionary<string, string> FallbackLabels =
        new Dictionary<string, string>(StringComparer.Ordinal)
        {
            ["tyre.actual.7"] = "INT",
            ["tyre.actual.8"] = "WET",
            ["tyre.actual.16"] = "C5",
            ["tyre.actual.17"] = "C4",
            ["tyre.actual.18"] = "C3",
            ["tyre.actual.19"] = "C2",
            ["tyre.actual.20"] = "C1",
            ["tyre.actual.21"] = "C0",
            ["tyre.actual.22"] = "C6",
            ["ers.mode.0"] = "None",
            ["ers.mode.1"] = "Auto",
            ["ers.mode.2"] = "Hotlap",
            ["ers.mode.3"] = "Overtake",
            ["drs.label"] = "DRS",
        };

    private readonly object _gate = new();
    private readonly TelemetryEngine _engine;
    private readonly Dictionary<int, long> _sessionHistoryBest = [];
    private TimingRowData? _timing;
    private ParticipantsRowData? _participants;
    private AllStatusRowData? _allStatus;
    private LapRowData? _lap;
    private PlayerStatusData? _playerStatus;
    private int? _fastestLapCarIndex;
    private IReadOnlyDictionary<string, string> _labels = FallbackLabels;
    private bool _hasExplicitFastestLap;
    private bool _isPlayback;
    private long _revision;
    private long _timelineRevision;
    private bool _disposed;

    public TelemetrySessionStore(TelemetryEngine engine)
    {
        _engine = engine;
        _engine.RowReceived += OnRowReceived;
        _engine.SeekFlushReceived += OnSeekFlushReceived;
        if (_engine.ProtocolStatusJson is { Length: > 0 } protocolStatus)
        {
            OnRowReceived(protocolStatus);
        }
    }

    public event Action? SnapshotChanged;
    public event Action<TimelineResetReason>? TimelineReset;

    public StandingsSnapshot Snapshot
    {
        get
        {
            lock (_gate)
            {
                return SnapshotLocked();
            }
        }
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        _engine.RowReceived -= OnRowReceived;
        _engine.SeekFlushReceived -= OnSeekFlushReceived;
    }

    private void OnRowReceived(string json)
    {
        if (_disposed)
        {
            return;
        }

        try
        {
            using var document = JsonDocument.Parse(json);
            var root = document.RootElement;
            if (!root.TryGetProperty("type", out var typeElement))
            {
                return;
            }

            var type = typeElement.GetString();
            var changed = type switch
            {
                "timing" => SetTiming(Deserialize<TimingRowData>(json)),
                "participants" => SetParticipants(Deserialize<ParticipantsRowData>(json)),
                "all_status" => SetAllStatus(Deserialize<AllStatusRowData>(json)),
                "lap" => SetLap(Deserialize<LapRowData>(json)),
                "status" => SetPlayerStatus(Deserialize<PlayerStatusData>(json)),
                "fastest_lap" => SetExplicitFastest(root),
                "session_history_fastest" => SetSessionHistoryFastest(root),
                "protocol_status" => SetProtocolLabels(root),
                "playback_lap_blocks" => SetPlayback(true),
                "playback_loaded" => SetPlaybackLoaded(root),
                "playback_close" => Reset(TimelineResetReason.PlaybackClosed),
                "race_event" => HandleRaceEvent(root),
                _ => false,
            };

            if (changed)
            {
                SnapshotChanged?.Invoke();
            }
        }
        catch (JsonException)
        {
        }
        catch (NotSupportedException)
        {
        }
    }

    private void OnSeekFlushReceived(
        byte[] binary,
        string coldJson,
        float currentLapStart,
        int lapNumber)
    {
        if (_disposed)
        {
            return;
        }

        lock (_gate)
        {
            _timing = null;
            _allStatus = null;
            _lap = null;
            _playerStatus = null;
            _timelineRevision++;
            _revision++;
        }

        TimelineReset?.Invoke(TimelineResetReason.Seek);

        var start = 0;
        while (start < coldJson.Length)
        {
            var end = coldJson.IndexOf('\n', start);
            if (end < 0)
            {
                end = coldJson.Length;
            }

            if (end > start)
            {
                OnRowReceived(coldJson[start..end]);
            }

            start = end + 1;
        }

        SnapshotChanged?.Invoke();
    }

    private static T? Deserialize<T>(string json) =>
        JsonSerializer.Deserialize<T>(json, JsonOptions);

    private bool SetTiming(TimingRowData? value) =>
        SetValue(value, data => _timing = data);

    private bool SetParticipants(ParticipantsRowData? value) =>
        SetValue(value, data => _participants = data);

    private bool SetAllStatus(AllStatusRowData? value) =>
        SetValue(value, data => _allStatus = data);

    private bool SetLap(LapRowData? value) =>
        SetValue(value, data => _lap = data);

    private bool SetPlayerStatus(PlayerStatusData? value) =>
        SetValue(value, data => _playerStatus = data);

    private bool SetValue<T>(T? value, Action<T> setter)
        where T : class
    {
        if (value is null)
        {
            return false;
        }

        lock (_gate)
        {
            setter(value);
            _revision++;
        }
        return true;
    }

    private bool SetExplicitFastest(JsonElement root)
    {
        if (!root.TryGetProperty("car_idx", out var carIndex) ||
            !carIndex.TryGetInt32(out var value))
        {
            return false;
        }

        lock (_gate)
        {
            _fastestLapCarIndex = value;
            _hasExplicitFastestLap = true;
            _revision++;
        }
        return true;
    }

    private bool SetSessionHistoryFastest(JsonElement root)
    {
        if (!root.TryGetProperty("car_idx", out var carIndexElement) ||
            !carIndexElement.TryGetInt32(out var carIndex) ||
            !root.TryGetProperty("best_lap_time_ms", out var lapTimeElement) ||
            !lapTimeElement.TryGetInt64(out var lapTime) ||
            lapTime <= 0)
        {
            return false;
        }

        lock (_gate)
        {
            if (_hasExplicitFastestLap)
            {
                return false;
            }

            _sessionHistoryBest[carIndex] = lapTime;
            _fastestLapCarIndex = _sessionHistoryBest.MinBy(pair => pair.Value).Key;
            _revision++;
        }
        return true;
    }

    private bool SetProtocolLabels(JsonElement root)
    {
        if (!root.TryGetProperty("labels", out var labelsElement) ||
            labelsElement.ValueKind != JsonValueKind.Object)
        {
            return false;
        }

        var labels = new Dictionary<string, string>(FallbackLabels, StringComparer.Ordinal);
        foreach (var property in labelsElement.EnumerateObject())
        {
            if (property.Value.ValueKind == JsonValueKind.String)
            {
                labels[property.Name] = property.Value.GetString() ?? property.Name;
            }
        }

        lock (_gate)
        {
            _labels = labels;
            _revision++;
        }
        return true;
    }

    private bool SetPlayback(bool value)
    {
        lock (_gate)
        {
            _isPlayback = value;
        }
        return false;
    }

    private bool SetPlaybackLoaded(JsonElement root)
    {
        var loaded = root.TryGetProperty("ok", out var ok) && ok.ValueKind == JsonValueKind.True;
        return SetPlayback(loaded);
    }

    private bool HandleRaceEvent(JsonElement root)
    {
        if (!root.TryGetProperty("code", out var code) ||
            !code.ValueEquals("SEND"))
        {
            return false;
        }

        lock (_gate)
        {
            if (_isPlayback)
            {
                return false;
            }
        }

        return Reset(TimelineResetReason.SessionEnded);
    }

    private bool Reset(TimelineResetReason reason)
    {
        lock (_gate)
        {
            _timing = null;
            _participants = null;
            _allStatus = null;
            _lap = null;
            _playerStatus = null;
            _fastestLapCarIndex = null;
            _sessionHistoryBest.Clear();
            _hasExplicitFastestLap = false;
            _isPlayback = reason != TimelineResetReason.PlaybackClosed && _isPlayback;
            _timelineRevision++;
            _revision++;
        }

        TimelineReset?.Invoke(reason);
        return true;
    }

    private StandingsSnapshot SnapshotLocked() => new(
        _timing,
        _participants,
        _allStatus,
        _lap,
        _playerStatus,
        _fastestLapCarIndex,
        _labels,
        _revision,
        _timelineRevision);
}
