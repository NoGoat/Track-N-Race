using System.Buffers.Binary;
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
    public double EnginePowerIceKw { get; init; }
    public double EnginePowerMgukKw { get; init; }
    public int ErsHarvestedMgukJ { get; init; }
    public int ErsHarvestedMguhJ { get; init; }

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

internal sealed record PowerSnapshot(
    PlayerStatusData? Latest,
    IReadOnlyDictionary<string, CardColorSpecData> CardColors,
    bool HasMguh,
    double? FuelUpperLimit,
    long Revision,
    long TimelineRevision);

internal sealed record PowerReadResult(
    PlayerStatusData[] Rows,
    int TotalCount,
    long BufferEpoch,
    long TimelineRevision,
    bool Reset);

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

internal sealed record SessionData
{
    public int Weather { get; init; }
    public int TrackTemp { get; init; }
    public int AirTemp { get; init; }
    public int TrackLengthM { get; init; }
    public int TrackId { get; init; }
    public int SessionType { get; init; }
    public int TotalLaps { get; init; }
    public int SessionTimeLeft { get; init; }
    public int SessionDuration { get; init; }
    public int PitSpeedLimit { get; init; }
    public int PitStopWindowIdealLap { get; init; }
    public int PitStopWindowLatestLap { get; init; }
    public int PitStopRejoinPosition { get; init; }
    public MarshalZoneData[] MarshalZones { get; init; } = [];
    public WeatherForecastData[] WeatherForecastSamples { get; init; } = [];
    public int ForecastAccuracy { get; init; }
    public int TimeOfDay { get; init; }
    public int ActiveAeroTrackStatus { get; init; } = -1;
}

internal sealed record MarshalZoneData
{
    public double ZoneStart { get; init; }
    public int Flag { get; init; }
}

internal sealed record WeatherForecastData
{
    public int TimeOffset { get; init; }
    public int Weather { get; init; }
    public int RainPercentage { get; init; }
}

internal sealed record CarPositionData(int Idx, double X, double Z);
internal sealed record PositionsRowData(int PlayerIdx, CarPositionData[] Cars);
internal sealed record TelemetrySample(
    float SessionTime,
    int Gear,
    float Throttle,
    float Brake,
    double Steering,
    int TyreTempSurfaceRl,
    int TyreTempSurfaceRr,
    int TyreTempSurfaceFl,
    int TyreTempSurfaceFr,
    int TyreTempInnerRl,
    int TyreTempInnerRr,
    int TyreTempInnerFl,
    int TyreTempInnerFr,
    int BrakeTempRl,
    int BrakeTempRr,
    int BrakeTempFl,
    int BrakeTempFr);

internal sealed record TelemetryReadResult(
    TelemetrySample[] Samples,
    int TotalCount,
    long BufferEpoch,
    long TimelineRevision,
    bool Reset);

internal sealed record DamageRowData
{
    public float SessionTime { get; init; }
    public double TyreWearRl { get; init; }
    public double TyreWearRr { get; init; }
    public double TyreWearFl { get; init; }
    public double TyreWearFr { get; init; }
    public int BlistersRl { get; init; }
    public int BlistersRr { get; init; }
    public int BlistersFl { get; init; }
    public int BlistersFr { get; init; }
}

internal sealed record TyreSetData
{
    public int Idx { get; init; }
    public int ActualCompound { get; init; }
    public int VisualCompound { get; init; }
    public int Wear { get; init; }
    public bool Available { get; init; }
    public int RecommendedSession { get; init; }
    public int LifeSpan { get; init; }
    public int UsableLife { get; init; }
    public int LapDeltaMs { get; init; }
    public bool Fitted { get; init; }
}

internal sealed record TyreSetsRowData
{
    public float SessionTime { get; init; }
    public TyreSetData[] Sets { get; init; } = [];
    public int FittedIdx { get; init; }
}

internal sealed record TyresSnapshot(
    TyreSetsRowData? TyreSets,
    TelemetrySample? LatestTelemetry,
    DamageRowData? LatestDamage,
    int? SessionType,
    IReadOnlyDictionary<string, string> Labels,
    IReadOnlyDictionary<string, CardColorSpecData> CardColors,
    long Revision,
    long TimelineRevision);

internal sealed record DamageReadResult(
    DamageRowData[] Rows,
    int TotalCount,
    long BufferEpoch,
    long TimelineRevision,
    bool Reset);

internal sealed record RaceEventData
{
    public float? SessionTime { get; init; }
    public string Code { get; init; } = string.Empty;
    public int? CarIdx { get; init; }
    public double? LapTimeS { get; init; }
    public int? SafetyCarType { get; init; }
    public int? EventType { get; init; }
    public int? PenaltyType { get; init; }
    public int? InfringementType { get; init; }
    public int? PenaltyTimeS { get; init; }
    public int? OvertakingCarIdx { get; init; }
    public int? BeingOvertakenCarIdx { get; init; }
}

internal sealed record CardColorRuleData
{
    public string On { get; init; } = string.Empty;
    public string Op { get; init; } = string.Empty;
    public double Value { get; init; }
    public string Color { get; init; } = string.Empty;
}

internal sealed record CardColorSpecData
{
    public string Default { get; init; } = "neutral";
    public CardColorRuleData[] Rules { get; init; } = [];
}

internal sealed record SessionSnapshot(
    SessionData? Session,
    TimingRowData? Timing,
    ParticipantsRowData? Participants,
    PositionsRowData? Positions,
    IReadOnlyList<RaceEventData> RaceEvents,
    IReadOnlyDictionary<string, string> Labels,
    IReadOnlyDictionary<string, CardColorSpecData> CardColors,
    string AeroMode,
    bool IsPlayback,
    long Revision,
    long TimelineRevision);

internal sealed class TelemetrySessionStore : IDisposable
{
    private const int MaxRaceEvents = 1000;
    private const int MaxHotRows = 750_000;
    private const int HotRowTrimChunk = 4096;
    private const float HotRowRetentionSeconds = 600;
    private const int MaxDamageRows = MaxHotRows;
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
    private IReadOnlyDictionary<string, CardColorSpecData> _cardColors =
        new Dictionary<string, CardColorSpecData>();
    private string _aeroMode = "drs";
    private SessionData? _session;
    private PositionsRowData? _positions;
    private TyreSetsRowData? _tyreSets;
    private DamageRowData? _latestDamage;
    private readonly List<RaceEventData> _raceEvents = [];
    private readonly List<TelemetrySample> _telemetry = [];
    private readonly List<DamageRowData> _damageHistory = [];
    private readonly List<PlayerStatusData> _powerHistory = [];
    private RaceEventData[] _playbackEvents = [];
    private bool _hasExplicitFastestLap;
    private bool _isPlayback;
    private long _revision;
    private long _timelineRevision;
    private long _telemetryBufferEpoch;
    private long _damageBufferEpoch;
    private long _powerBufferEpoch;
    private bool _hasMguh;
    private double? _fuelUpperLimit;
    private double _liveFuelMaximum = double.NegativeInfinity;
    private bool _disposed;

    public TelemetrySessionStore(TelemetryEngine engine)
    {
        _engine = engine;
        _engine.RowReceived += OnRowReceived;
        _engine.BinaryBatchReceived += OnBinaryBatchReceived;
        _engine.SeekFlushReceived += OnSeekFlushReceived;
        if (_engine.ProtocolStatusJson is { Length: > 0 } protocolStatus)
        {
            OnRowReceived(protocolStatus);
        }
    }

    public event Action? SnapshotChanged;
    public event Action? TelemetryChanged;
    public event Action? TyresChanged;
    public event Action? PowerChanged;
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

    public SessionSnapshot SessionSnapshot
    {
        get
        {
            lock (_gate)
            {
                IReadOnlyList<RaceEventData> events = _raceEvents.ToArray();
                if (_isPlayback)
                {
                    var playhead = _engine.LatestSessionTime ?? float.MaxValue;
                    events = _playbackEvents
                        .Where(value => value.SessionTime is null ||
                            value.SessionTime.Value <= playhead)
                        .ToArray();
                }

                return new SessionSnapshot(
                    _session,
                    _timing,
                    _participants,
                    _positions,
                    events,
                    _labels,
                    _cardColors,
                    _aeroMode,
                    _isPlayback,
                    _revision,
                    _timelineRevision);
            }
        }
    }

    public TyresSnapshot TyresSnapshot
    {
        get
        {
            lock (_gate)
            {
                return new TyresSnapshot(
                    _tyreSets,
                    _telemetry.Count > 0 ? _telemetry[^1] : null,
                    _latestDamage,
                    _session?.SessionType,
                    _labels,
                    _cardColors,
                    _revision,
                    _timelineRevision);
            }
        }
    }

    public PowerSnapshot PowerSnapshot
    {
        get
        {
            lock (_gate)
            {
                return new PowerSnapshot(
                    _playerStatus,
                    _cardColors,
                    _hasMguh,
                    _fuelUpperLimit,
                    _revision,
                    _timelineRevision);
            }
        }
    }

    public TelemetryReadResult ReadTelemetry(
        int knownCount,
        long knownBufferEpoch,
        long knownTimelineRevision)
    {
        lock (_gate)
        {
            var reset =
                knownBufferEpoch != _telemetryBufferEpoch ||
                knownTimelineRevision != _timelineRevision ||
                knownCount < 0 ||
                knownCount > _telemetry.Count;
            var start = reset ? 0 : knownCount;
            var samples = new TelemetrySample[
                _telemetry.Count - start];
            _telemetry.CopyTo(
                start, samples, 0, samples.Length);
            return new TelemetryReadResult(
                samples,
                _telemetry.Count,
                _telemetryBufferEpoch,
                _timelineRevision,
                reset);
        }
    }

    public DamageReadResult ReadDamage(
        int knownCount,
        long knownBufferEpoch,
        long knownTimelineRevision)
    {
        lock (_gate)
        {
            var reset =
                knownBufferEpoch != _damageBufferEpoch ||
                knownTimelineRevision != _timelineRevision ||
                knownCount < 0 ||
                knownCount > _damageHistory.Count;
            var start = reset ? 0 : knownCount;
            var rows = new DamageRowData[_damageHistory.Count - start];
            _damageHistory.CopyTo(start, rows, 0, rows.Length);
            return new DamageReadResult(
                rows,
                _damageHistory.Count,
                _damageBufferEpoch,
                _timelineRevision,
                reset);
        }
    }

    public PowerReadResult ReadPower(
        int knownCount,
        long knownBufferEpoch,
        long knownTimelineRevision)
    {
        lock (_gate)
        {
            var reset =
                knownBufferEpoch != _powerBufferEpoch ||
                knownTimelineRevision != _timelineRevision ||
                knownCount < 0 ||
                knownCount > _powerHistory.Count;
            var start = reset ? 0 : knownCount;
            var rows = new PlayerStatusData[_powerHistory.Count - start];
            _powerHistory.CopyTo(start, rows, 0, rows.Length);
            return new PowerReadResult(
                rows,
                _powerHistory.Count,
                _powerBufferEpoch,
                _timelineRevision,
                reset);
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
        _engine.BinaryBatchReceived -= OnBinaryBatchReceived;
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
                "session" => SetSession(Deserialize<SessionData>(json)),
                "tyre_sets" => SetTyreSets(Deserialize<TyreSetsRowData>(json)),
                "damage" => SetDamage(Deserialize<DamageRowData>(json)),
                "positions" => SetPositionsJson(root),
                "status" => SetPlayerStatus(Deserialize<PlayerStatusData>(json)),
                "fastest_lap" => SetExplicitFastest(root),
                "session_history_fastest" => SetSessionHistoryFastest(root),
                "protocol_status" => SetProtocolMetadata(root),
                "playback_lap_blocks" => SetPlaybackMetadata(root),
                "playback_loaded" => SetPlaybackLoaded(root),
                "playback_close" => Reset(TimelineResetReason.PlaybackClosed),
                "race_event" => HandleRaceEvent(root),
                _ => false,
            };

            if (changed)
            {
                SnapshotChanged?.Invoke();
                if (type is "tyre_sets" or "damage" or "session" or "protocol_status")
                {
                    TyresChanged?.Invoke();
                }
                if (type is "status" or "protocol_status" or "playback_lap_blocks")
                {
                    PowerChanged?.Invoke();
                }
            }
        }
        catch (JsonException)
        {
        }
        catch (NotSupportedException)
        {
        }
    }

    private void OnBinaryBatchReceived(ReadOnlySpan<byte> data)
    {
        if (_disposed ||
            !TryDecodeHotBatch(data, out var telemetry, out var positions))
        {
            return;
        }

        var positionsChanged = positions is not null;
        var telemetryChanged = telemetry.Count > 0;
        lock (_gate)
        {
            if (positionsChanged)
            {
                _positions = positions;
                _revision++;
            }
            AppendTelemetryLocked(telemetry);
        }
        if (positionsChanged)
        {
            SnapshotChanged?.Invoke();
        }
        if (telemetryChanged)
        {
            TelemetryChanged?.Invoke();
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
            _positions = null;
            _tyreSets = null;
            _latestDamage = null;
            ClearTelemetryLocked();
            ClearDamageLocked();
            ClearPowerLocked();
            _timelineRevision++;
            _revision++;
        }

        TimelineReset?.Invoke(TimelineResetReason.Seek);
        OnBinaryBatchReceived(binary);

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

    private bool SetPlayerStatus(PlayerStatusData? value)
    {
        if (value is null)
        {
            return false;
        }

        lock (_gate)
        {
            if (_powerHistory.Count > 0 &&
                value.SessionTime < _powerHistory[^1].SessionTime)
            {
                var keep = LowerBoundPower(value.SessionTime);
                if (keep < _powerHistory.Count)
                {
                    _powerHistory.RemoveRange(
                        keep, _powerHistory.Count - keep);
                    _powerBufferEpoch++;
                }
            }

            _playerStatus = value;
            _powerHistory.Add(value);
            if (!_isPlayback &&
                double.IsFinite(value.FuelKg) &&
                value.FuelKg >= 0 &&
                value.FuelKg > _liveFuelMaximum)
            {
                _liveFuelMaximum = value.FuelKg;
                _fuelUpperLimit = value.FuelKg + 1;
            }
            TrimPowerLocked();
            _revision++;
        }
        return true;
    }

    private bool SetSession(SessionData? value) =>
        SetValue(value, data => _session = data);

    private bool SetTyreSets(TyreSetsRowData? value) =>
        SetValue(value, data => _tyreSets = data);

    private bool SetDamage(DamageRowData? value)
    {
        if (value is null)
        {
            return false;
        }

        lock (_gate)
        {
            if (_damageHistory.Count > 0 &&
                value.SessionTime < _damageHistory[^1].SessionTime)
            {
                var keep = LowerBoundDamage(value.SessionTime);
                if (keep < _damageHistory.Count)
                {
                    _damageHistory.RemoveRange(
                        keep, _damageHistory.Count - keep);
                    _damageBufferEpoch++;
                }
            }

            _latestDamage = value;
            _damageHistory.Add(value);
            TrimDamageLocked();
            _revision++;
        }
        return true;
    }

    private bool SetPositionsJson(JsonElement root)
    {
        if (!root.TryGetProperty("player_idx", out var playerElement) ||
            !playerElement.TryGetInt32(out var playerIdx) ||
            !root.TryGetProperty("cars", out var carsElement) ||
            carsElement.ValueKind != JsonValueKind.Array)
        {
            return false;
        }

        var cars = new List<CarPositionData>();
        foreach (var car in carsElement.EnumerateArray())
        {
            if (car.TryGetProperty("idx", out var idxElement) &&
                idxElement.TryGetInt32(out var idx) &&
                car.TryGetProperty("x", out var xElement) &&
                xElement.TryGetDouble(out var x) &&
                car.TryGetProperty("z", out var zElement) &&
                zElement.TryGetDouble(out var z))
            {
                cars.Add(new CarPositionData(idx, x, z));
            }
        }

        lock (_gate)
        {
            _positions = new PositionsRowData(playerIdx, cars.ToArray());
            _revision++;
        }
        return true;
    }

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

    private bool SetProtocolMetadata(JsonElement root)
    {
        var labels = new Dictionary<string, string>(FallbackLabels, StringComparer.Ordinal);
        if (root.TryGetProperty("labels", out var labelsElement) &&
            labelsElement.ValueKind == JsonValueKind.Object)
        {
            foreach (var property in labelsElement.EnumerateObject())
            {
                if (property.Value.ValueKind == JsonValueKind.String)
                {
                    labels[property.Name] = property.Value.GetString() ?? property.Name;
                }
            }
        }

        var cardColors = new Dictionary<string, CardColorSpecData>(StringComparer.Ordinal);
        if (root.TryGetProperty("cardColors", out var colorsElement) &&
            colorsElement.ValueKind == JsonValueKind.Object)
        {
            foreach (var property in colorsElement.EnumerateObject())
            {
                try
                {
                    var spec = property.Value.Deserialize<CardColorSpecData>(JsonOptions);
                    if (spec is not null)
                    {
                        cardColors[property.Name] = spec;
                    }
                }
                catch (JsonException)
                {
                }
            }
        }

        var aeroMode = root.TryGetProperty("aero_mode", out var aeroElement) &&
            aeroElement.ValueKind == JsonValueKind.String
                ? aeroElement.GetString() ?? "drs"
                : "drs";
        var hasMguh = root.TryGetProperty("capabilities", out var capabilities) &&
            capabilities.ValueKind == JsonValueKind.Object &&
            capabilities.TryGetProperty("hasMguh", out var hasMguhElement) &&
            hasMguhElement.ValueKind == JsonValueKind.True;

        lock (_gate)
        {
            _labels = labels;
            _cardColors = cardColors;
            _aeroMode = aeroMode;
            _hasMguh = hasMguh;
            _revision++;
        }
        return true;
    }

    private bool SetPlaybackMetadata(JsonElement root)
    {
        var events = new List<RaceEventData>();
        if (root.TryGetProperty("events", out var eventsElement) &&
            eventsElement.ValueKind == JsonValueKind.Array)
        {
            foreach (var element in eventsElement.EnumerateArray())
            {
                try
                {
                    var value = element.Deserialize<RaceEventData>(JsonOptions);
                    if (value is not null)
                    {
                        events.Add(value);
                    }
                }
                catch (JsonException)
                {
                }
            }
        }

        double? fuelUpperLimit = null;
        if (root.TryGetProperty("initialFuelKg", out var initialFuelElement) &&
            initialFuelElement.TryGetDouble(out var initialFuel) &&
            double.IsFinite(initialFuel) &&
            initialFuel >= 0)
        {
            fuelUpperLimit = initialFuel + 1;
        }

        lock (_gate)
        {
            _isPlayback = true;
            _playbackEvents = events.ToArray();
            _fuelUpperLimit = fuelUpperLimit;
            _liveFuelMaximum = double.NegativeInfinity;
            _revision++;
        }
        return true;
    }

    private bool SetPlaybackLoaded(JsonElement root)
    {
        var loaded = root.TryGetProperty("ok", out var ok) && ok.ValueKind == JsonValueKind.True;
        lock (_gate)
        {
            _isPlayback = loaded;
            if (loaded)
            {
                _tyreSets = null;
                _latestDamage = null;
                _playerStatus = null;
                ClearTelemetryLocked();
                ClearDamageLocked();
                ClearPowerLocked();
                _fuelUpperLimit = null;
                _liveFuelMaximum = double.NegativeInfinity;
            }
        }
        if (loaded)
        {
            TelemetryChanged?.Invoke();
            TyresChanged?.Invoke();
            PowerChanged?.Invoke();
        }
        return false;
    }

    private bool HandleRaceEvent(JsonElement root)
    {
        RaceEventData? raceEvent = null;
        try
        {
            raceEvent = root.Deserialize<RaceEventData>(JsonOptions);
        }
        catch (JsonException)
        {
        }

        if (raceEvent is not null)
        {
            lock (_gate)
            {
                _raceEvents.Add(raceEvent);
                if (_raceEvents.Count > MaxRaceEvents)
                {
                    _raceEvents.RemoveRange(0, _raceEvents.Count - MaxRaceEvents);
                }
                _revision++;
            }
        }

        if (!root.TryGetProperty("code", out var code) || !code.ValueEquals("SEND"))
        {
            return raceEvent is not null;
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
            _session = null;
            _positions = null;
            _tyreSets = null;
            _latestDamage = null;
            ClearTelemetryLocked();
            ClearDamageLocked();
            ClearPowerLocked();
            _fuelUpperLimit = null;
            _liveFuelMaximum = double.NegativeInfinity;
            _fastestLapCarIndex = null;
            _sessionHistoryBest.Clear();
            _raceEvents.Clear();
            _playbackEvents = [];
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

    private void AppendTelemetryLocked(
        IReadOnlyList<TelemetrySample> samples)
    {
        foreach (var sample in samples)
        {
            if (_telemetry.Count > 0 &&
                sample.SessionTime < _telemetry[^1].SessionTime)
            {
                var keep = LowerBoundTelemetry(sample.SessionTime);
                if (keep < _telemetry.Count)
                {
                    _telemetry.RemoveRange(
                        keep, _telemetry.Count - keep);
                    _telemetryBufferEpoch++;
                }
            }

            _telemetry.Add(sample);
        }

        if (_telemetry.Count == 0)
        {
            return;
        }

        var cutoff = _telemetry[^1].SessionTime - HotRowRetentionSeconds;
        var firstValid = LowerBoundTelemetry(cutoff);
        var excess = Math.Max(0, _telemetry.Count - MaxHotRows);
        var removeCount = Math.Max(firstValid, excess);
        if (removeCount >= HotRowTrimChunk || excess > 0)
        {
            _telemetry.RemoveRange(0, removeCount);
            _telemetryBufferEpoch++;
        }
    }

    private int LowerBoundTelemetry(float sessionTime)
    {
        var low = 0;
        var high = _telemetry.Count;
        while (low < high)
        {
            var middle = low + ((high - low) / 2);
            if (_telemetry[middle].SessionTime < sessionTime)
            {
                low = middle + 1;
            }
            else
            {
                high = middle;
            }
        }
        return low;
    }

    private int LowerBoundDamage(float sessionTime)
    {
        var low = 0;
        var high = _damageHistory.Count;
        while (low < high)
        {
            var middle = low + ((high - low) / 2);
            if (_damageHistory[middle].SessionTime < sessionTime)
            {
                low = middle + 1;
            }
            else
            {
                high = middle;
            }
        }
        return low;
    }

    private int LowerBoundPower(float sessionTime)
    {
        var low = 0;
        var high = _powerHistory.Count;
        while (low < high)
        {
            var middle = low + ((high - low) / 2);
            if (_powerHistory[middle].SessionTime < sessionTime)
            {
                low = middle + 1;
            }
            else
            {
                high = middle;
            }
        }
        return low;
    }

    private void TrimDamageLocked()
    {
        if (_damageHistory.Count == 0)
        {
            return;
        }
        var cutoff = _damageHistory[^1].SessionTime - HotRowRetentionSeconds;
        var firstValid = LowerBoundDamage(cutoff);
        var excess = Math.Max(0, _damageHistory.Count - MaxDamageRows);
        var removeCount = Math.Max(firstValid, excess);
        if (removeCount >= HotRowTrimChunk || excess > 0)
        {
            _damageHistory.RemoveRange(0, removeCount);
            _damageBufferEpoch++;
        }
    }

    private void TrimPowerLocked()
    {
        if (_powerHistory.Count == 0)
        {
            return;
        }
        var cutoff = _powerHistory[^1].SessionTime - HotRowRetentionSeconds;
        var firstValid = LowerBoundPower(cutoff);
        if (firstValid >= HotRowTrimChunk)
        {
            _powerHistory.RemoveRange(0, firstValid);
            _powerBufferEpoch++;
        }
    }

    private void ClearTelemetryLocked()
    {
        _telemetry.Clear();
        _telemetryBufferEpoch++;
    }

    private void ClearDamageLocked()
    {
        _damageHistory.Clear();
        _damageBufferEpoch++;
    }

    private void ClearPowerLocked()
    {
        _powerHistory.Clear();
        _powerBufferEpoch++;
    }

    private static bool TryDecodeHotBatch(
        ReadOnlySpan<byte> data,
        out List<TelemetrySample> telemetry,
        out PositionsRowData? latestPositions)
    {
        telemetry = [];
        latestPositions = null;
        var offset = 0;
        while (offset < data.Length)
        {
            var tag = data[offset++];
            switch (tag)
            {
                case 1:
                    if (!TryReadSingle(data, ref offset, out var sessionTime) ||
                        !Advance(data, ref offset, 4) ||
                        !TryReadSByte(data, ref offset, out var gear) ||
                        !Advance(data, ref offset, 1) ||
                        !TryReadSingle(data, ref offset, out var throttle) ||
                        !TryReadSingle(data, ref offset, out var brake) ||
                        !TryReadDouble(data, ref offset, out var steering) ||
                        !TryReadByte(data, ref offset, out var tyreSurfaceRl) ||
                        !TryReadByte(data, ref offset, out var tyreSurfaceRr) ||
                        !TryReadByte(data, ref offset, out var tyreSurfaceFl) ||
                        !TryReadByte(data, ref offset, out var tyreSurfaceFr) ||
                        !TryReadByte(data, ref offset, out var tyreInnerRl) ||
                        !TryReadByte(data, ref offset, out var tyreInnerRr) ||
                        !TryReadByte(data, ref offset, out var tyreInnerFl) ||
                        !TryReadByte(data, ref offset, out var tyreInnerFr) ||
                        !TryReadUInt16(data, ref offset, out var brakeTempRl) ||
                        !TryReadUInt16(data, ref offset, out var brakeTempRr) ||
                        !TryReadUInt16(data, ref offset, out var brakeTempFl) ||
                        !TryReadUInt16(data, ref offset, out var brakeTempFr) ||
                        !Advance(data, ref offset, 3))
                    {
                        telemetry = [];
                        latestPositions = null;
                        return false;
                    }
                    telemetry.Add(new TelemetrySample(
                        sessionTime,
                        gear,
                        throttle,
                        brake,
                        steering,
                        tyreSurfaceRl,
                        tyreSurfaceRr,
                        tyreSurfaceFl,
                        tyreSurfaceFr,
                        tyreInnerRl,
                        tyreInnerRr,
                        tyreInnerFl,
                        tyreInnerFr,
                        brakeTempRl,
                        brakeTempRr,
                        brakeTempFl,
                        brakeTempFr));
                    break;
                case 2:
                    // motion: f32 + 3*f64.
                    if (!Advance(data, ref offset, 28)) return false;
                    break;
                case 4:
                    // motion_ex: f32 + 2*f64.
                    if (!Advance(data, ref offset, 20)) return false;
                    break;
                case 3:
                    if (!Advance(data, ref offset, 2)) return false;
                    var playerIdx = data[offset - 2];
                    var count = data[offset - 1];
                    var byteCount = checked(count * 16);
                    if (offset + byteCount > data.Length) return false;
                    var cars = new CarPositionData[count];
                    for (var index = 0; index < count; index++)
                    {
                        var xBits = BinaryPrimitives.ReadInt64LittleEndian(
                            data.Slice(offset, 8));
                        var zBits = BinaryPrimitives.ReadInt64LittleEndian(
                            data.Slice(offset + 8, 8));
                        offset += 16;
                        cars[index] = new CarPositionData(
                            index,
                            BitConverter.Int64BitsToDouble(xBits),
                            BitConverter.Int64BitsToDouble(zBits));
                    }
                    latestPositions = new PositionsRowData(playerIdx, cars);
                    break;
                default:
                    telemetry = [];
                    latestPositions = null;
                    return false;
            }
        }
        return true;
    }

    private static bool TryReadSByte(
        ReadOnlySpan<byte> data,
        ref int offset,
        out sbyte value)
    {
        if (offset >= data.Length)
        {
            value = 0;
            return false;
        }
        value = unchecked((sbyte)data[offset++]);
        return true;
    }

    private static bool TryReadByte(
        ReadOnlySpan<byte> data,
        ref int offset,
        out byte value)
    {
        if (offset >= data.Length)
        {
            value = 0;
            return false;
        }
        value = data[offset++];
        return true;
    }

    private static bool TryReadUInt16(
        ReadOnlySpan<byte> data,
        ref int offset,
        out ushort value)
    {
        if (offset + sizeof(ushort) > data.Length)
        {
            value = 0;
            return false;
        }
        value = BinaryPrimitives.ReadUInt16LittleEndian(
            data.Slice(offset, sizeof(ushort)));
        offset += sizeof(ushort);
        return true;
    }

    private static bool TryReadSingle(
        ReadOnlySpan<byte> data,
        ref int offset,
        out float value)
    {
        if (offset + sizeof(int) > data.Length)
        {
            value = 0;
            return false;
        }
        value = BitConverter.Int32BitsToSingle(
            BinaryPrimitives.ReadInt32LittleEndian(
                data.Slice(offset, sizeof(int))));
        offset += sizeof(int);
        return true;
    }

    private static bool TryReadDouble(
        ReadOnlySpan<byte> data,
        ref int offset,
        out double value)
    {
        if (offset + sizeof(long) > data.Length)
        {
            value = 0;
            return false;
        }
        value = BitConverter.Int64BitsToDouble(
            BinaryPrimitives.ReadInt64LittleEndian(
                data.Slice(offset, sizeof(long))));
        offset += sizeof(long);
        return true;
    }

    private static bool Advance(ReadOnlySpan<byte> data, ref int offset, int count)
    {
        if (offset + count > data.Length)
        {
            return false;
        }
        offset += count;
        return true;
    }
}
