using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Runtime.CompilerServices;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Media;
using Windows.UI;

namespace TrackNRace.WinUI3;

internal abstract class BindableBase : INotifyPropertyChanged
{
    public event PropertyChangedEventHandler? PropertyChanged;

    protected bool Set<T>(
        ref T field,
        T value,
        [CallerMemberName] string propertyName = "")
    {
        if (EqualityComparer<T>.Default.Equals(field, value))
        {
            return false;
        }

        field = value;
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
        return true;
    }

    protected void Raise([CallerMemberName] string propertyName = "") =>
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
}

internal sealed record PositionChange(int CarIndex, bool Gained);

internal sealed class StandingsBadgeViewModel
{
    public required string Text { get; init; }
    public required SolidColorBrush Foreground { get; init; }
    public required SolidColorBrush Background { get; init; }
    public required SolidColorBrush BorderBrush { get; init; }
}

internal sealed class StandingsRowViewModel : BindableBase
{
    private string _positionText = string.Empty;
    private string _raceNumber = string.Empty;
    private string _driverCode = string.Empty;
    private string _driverName = string.Empty;
    private string _lap = string.Empty;
    private string _lastLap = string.Empty;
    private string _gap = string.Empty;
    private string _s1 = string.Empty;
    private string _s2 = string.Empty;
    private string _s3 = string.Empty;
    private string _tyre = string.Empty;
    private string _automationName = string.Empty;
    private SolidColorBrush _positionBrush = StandingsColors.Brush("#8E8E8E");
    private SolidColorBrush _gapBrush = StandingsColors.Brush("#8E8E8E");
    private SolidColorBrush _teamBrush = StandingsColors.Brush("#8E8E8E");
    private SolidColorBrush _tyreBrush = StandingsColors.Brush("#8E8E8E");
    private SolidColorBrush _fastestBrush = StandingsColors.TransparentBrush;
    private Visibility _playerBadgeVisibility = Visibility.Collapsed;
    private Visibility _selectionVisibility = Visibility.Collapsed;
    private double _contentOpacity = 1;
    private IReadOnlyList<StandingsBadgeViewModel> _badges = [];

    public StandingsRowViewModel(int carIndex, bool placeholder = false)
    {
        CarIndex = carIndex;
        IsPlaceholder = placeholder;
    }

    public int CarIndex { get; }
    public bool IsPlaceholder { get; }
    public int Position { get; private set; }
    public string PositionText { get => _positionText; private set => Set(ref _positionText, value); }
    public string RaceNumber { get => _raceNumber; private set => Set(ref _raceNumber, value); }
    public string DriverCode { get => _driverCode; private set => Set(ref _driverCode, value); }
    public string DriverName { get => _driverName; private set => Set(ref _driverName, value); }
    public string Lap { get => _lap; private set => Set(ref _lap, value); }
    public string LastLap { get => _lastLap; private set => Set(ref _lastLap, value); }
    public string Gap { get => _gap; private set => Set(ref _gap, value); }
    public string S1 { get => _s1; private set => Set(ref _s1, value); }
    public string S2 { get => _s2; private set => Set(ref _s2, value); }
    public string S3 { get => _s3; private set => Set(ref _s3, value); }
    public string Tyre { get => _tyre; private set => Set(ref _tyre, value); }
    public string AutomationName { get => _automationName; private set => Set(ref _automationName, value); }
    public SolidColorBrush PositionBrush { get => _positionBrush; private set => Set(ref _positionBrush, value); }
    public SolidColorBrush GapBrush { get => _gapBrush; private set => Set(ref _gapBrush, value); }
    public SolidColorBrush TeamBrush { get => _teamBrush; private set => Set(ref _teamBrush, value); }
    public SolidColorBrush TyreBrush { get => _tyreBrush; private set => Set(ref _tyreBrush, value); }
    public SolidColorBrush FastestBrush { get => _fastestBrush; private set => Set(ref _fastestBrush, value); }
    public Visibility PlayerBadgeVisibility { get => _playerBadgeVisibility; private set => Set(ref _playerBadgeVisibility, value); }
    public Visibility SelectionVisibility { get => _selectionVisibility; private set => Set(ref _selectionVisibility, value); }
    public double ContentOpacity { get => _contentOpacity; private set => Set(ref _contentOpacity, value); }
    public IReadOnlyList<StandingsBadgeViewModel> Badges { get => _badges; private set => Set(ref _badges, value); }

    public void UpdatePlaceholder(int position, bool isDark)
    {
        Position = position;
        PositionText = $"P{position}";
        RaceNumber = "—";
        DriverCode = "—";
        DriverName = string.Empty;
        Lap = "—";
        LastLap = "--:--.---";
        Gap = "—";
        S1 = "—";
        S2 = "—";
        S3 = "—";
        Tyre = "—";
        PositionBrush = StandingsColors.Muted(isDark);
        GapBrush = StandingsColors.Muted(isDark);
        TeamBrush = StandingsColors.Divider(isDark);
        TyreBrush = StandingsColors.Muted(isDark);
        FastestBrush = StandingsColors.TransparentBrush;
        PlayerBadgeVisibility = Visibility.Collapsed;
        SelectionVisibility = Visibility.Collapsed;
        ContentOpacity = 0.62;
        Badges = [];
        AutomationName = $"Position {position}, no timing data";
    }

    public void Update(
        TimingCarData car,
        DriverData? driver,
        CarStatusData? status,
        int s1,
        int s2,
        int s3,
        bool isPlayer,
        bool isFastest,
        bool isSelected,
        bool isDark,
        IReadOnlyDictionary<string, string> labels)
    {
        Position = car.Position;
        PositionText = $"P{car.Position}";
        RaceNumber = driver?.RaceNumber.ToString() ?? string.Empty;
        DriverCode = driver is null ? $"C{car.Idx}" : StandingsFormat.Abbreviate(driver.Name);
        DriverName = driver?.Name ?? string.Empty;
        Lap = car.LapNum.ToString();
        LastLap = StandingsFormat.Milliseconds(car.LastLapMs);
        var resultLabel = ResultLabel(car.ResultStatus);
        Gap = resultLabel ?? StandingsFormat.Gap(car.GapMs, car.Position);
        S1 = StandingsFormat.Sector(s1);
        S2 = StandingsFormat.Sector(s2);
        S3 = StandingsFormat.Sector(s3);
        Tyre = status is null
            ? "—"
            : StandingsFormat.Label(labels, "tyre.actual", status.TyreCompound);
        PositionBrush = StandingsColors.Position(car.Position, isDark);
        GapBrush = resultLabel is null
            ? StandingsColors.Muted(isDark)
            : StandingsColors.Brush("#C4162A");
        TeamBrush = StandingsColors.Brush(driver?.LiveryColor, "#8E8E8E");
        TyreBrush = status is null
            ? StandingsColors.Muted(isDark)
            : StandingsColors.Tyre(status.VisualCompound, isDark);
        FastestBrush = isFastest
            ? StandingsColors.Brush(Color.FromArgb(24, 191, 95, 255))
            : StandingsColors.TransparentBrush;
        PlayerBadgeVisibility = isPlayer ? Visibility.Visible : Visibility.Collapsed;
        SelectionVisibility = isSelected ? Visibility.Visible : Visibility.Collapsed;
        ContentOpacity = 1;
        Badges = CreateBadges(car, isDark);
        AutomationName =
            $"Position {car.Position}, {driver?.Name ?? DriverCode}, lap {car.LapNum}, " +
            $"last lap {LastLap}, gap {Gap}, tyre {Tyre}";
    }

    private static string? ResultLabel(int resultStatus) => resultStatus switch
    {
        4 => "DNF",
        5 => "DSQ",
        7 => "RET",
        _ => null,
    };

    private static IReadOnlyList<StandingsBadgeViewModel> CreateBadges(
        TimingCarData car,
        bool isDark)
    {
        var badges = new List<StandingsBadgeViewModel>(5);
        if (car.PitStatus > 0)
        {
            badges.Add(StandingsColors.Badge(
                car.PitStatus == 1 ? "PIT" : "PIT LANE",
                isDark ? "#FFD700" : "#B7950B"));
        }
        if (car.LapInvalid)
        {
            badges.Add(StandingsColors.Badge("INV", "#C4162A"));
        }
        if (car.PenaltiesS > 0)
        {
            badges.Add(StandingsColors.Badge(
                $"+{car.PenaltiesS}s",
                isDark ? "#C47D0E" : "#B06000"));
        }
        if (car.NumDtPens > 0)
        {
            badges.Add(StandingsColors.Badge(
                $"{(car.NumDtPens > 1 ? $"{car.NumDtPens}× " : string.Empty)}DT",
                "#E10600"));
        }
        if (car.NumSgPens > 0)
        {
            badges.Add(StandingsColors.Badge(
                $"{(car.NumSgPens > 1 ? $"{car.NumSgPens}× " : string.Empty)}SG",
                "#E10600"));
        }
        return badges;
    }
}

internal sealed class StandingsDetailViewModel : BindableBase
{
    private string _driverName = string.Empty;
    private SolidColorBrush _teamBrush = StandingsColors.Brush("#8E8E8E");
    private Visibility _driverVisibility = Visibility.Collapsed;
    private string _lapLabel = "Lap —";
    private string _position = "P—";
    private string _currentLap = "--:--.---";
    private string _lastLap = "--:--.---";
    private string _s1 = "–:––.–––";
    private string _s2 = "–:––.–––";
    private string _s3 = "–:––.–––";
    private string _timingBadge = string.Empty;
    private Visibility _timingBadgeVisibility = Visibility.Collapsed;
    private string _penalty = string.Empty;
    private Visibility _penaltyVisibility = Visibility.Collapsed;
    private string _ersPercent = "—%";
    private double _ersValue;
    private string _ersMode = "—";
    private string _ersEnergy = "— MJ / 4.00 MJ";
    private string _ersDeployed = "— MJ";
    private string _aeroLabel = "DRS";
    private string _aeroState = "—";
    private string _fuel = "—";
    private string _fuelLaps = "— laps vs finish";
    private string _fuelMix = "Mix: —";
    private string _tyre = "—";
    private string _tyreAge = "Age: — laps";
    private string _brakeBias = "Brake bias: —% front";
    private SolidColorBrush _ersBrush = StandingsColors.Brush("#8E8E8E");
    private SolidColorBrush _ersModeBrush = StandingsColors.Brush("#8E8E8E");
    private SolidColorBrush _aeroBrush = StandingsColors.Brush("#8E8E8E");
    private SolidColorBrush _fuelBrush = StandingsColors.Brush("#8E8E8E");
    private SolidColorBrush _tyreBrush = StandingsColors.Brush("#8E8E8E");
    private SolidColorBrush _timingBadgeBrush = StandingsColors.Brush("#FFD700");

    public string DriverName { get => _driverName; private set => Set(ref _driverName, value); }
    public SolidColorBrush TeamBrush { get => _teamBrush; private set => Set(ref _teamBrush, value); }
    public Visibility DriverVisibility { get => _driverVisibility; private set => Set(ref _driverVisibility, value); }
    public string LapLabel { get => _lapLabel; private set => Set(ref _lapLabel, value); }
    public string Position { get => _position; private set => Set(ref _position, value); }
    public string CurrentLap { get => _currentLap; private set => Set(ref _currentLap, value); }
    public string LastLap { get => _lastLap; private set => Set(ref _lastLap, value); }
    public string S1 { get => _s1; private set => Set(ref _s1, value); }
    public string S2 { get => _s2; private set => Set(ref _s2, value); }
    public string S3 { get => _s3; private set => Set(ref _s3, value); }
    public string TimingBadge { get => _timingBadge; private set => Set(ref _timingBadge, value); }
    public Visibility TimingBadgeVisibility { get => _timingBadgeVisibility; private set => Set(ref _timingBadgeVisibility, value); }
    public string Penalty { get => _penalty; private set => Set(ref _penalty, value); }
    public Visibility PenaltyVisibility { get => _penaltyVisibility; private set => Set(ref _penaltyVisibility, value); }
    public string ErsPercent { get => _ersPercent; private set => Set(ref _ersPercent, value); }
    public double ErsValue { get => _ersValue; private set => Set(ref _ersValue, value); }
    public string ErsMode { get => _ersMode; private set => Set(ref _ersMode, value); }
    public string ErsEnergy { get => _ersEnergy; private set => Set(ref _ersEnergy, value); }
    public string ErsDeployed { get => _ersDeployed; private set => Set(ref _ersDeployed, value); }
    public string AeroLabel { get => _aeroLabel; private set => Set(ref _aeroLabel, value); }
    public string AeroState { get => _aeroState; private set => Set(ref _aeroState, value); }
    public string Fuel { get => _fuel; private set => Set(ref _fuel, value); }
    public string FuelLaps { get => _fuelLaps; private set => Set(ref _fuelLaps, value); }
    public string FuelMix { get => _fuelMix; private set => Set(ref _fuelMix, value); }
    public string Tyre { get => _tyre; private set => Set(ref _tyre, value); }
    public string TyreAge { get => _tyreAge; private set => Set(ref _tyreAge, value); }
    public string BrakeBias { get => _brakeBias; private set => Set(ref _brakeBias, value); }
    public SolidColorBrush ErsBrush { get => _ersBrush; private set => Set(ref _ersBrush, value); }
    public SolidColorBrush ErsModeBrush { get => _ersModeBrush; private set => Set(ref _ersModeBrush, value); }
    public SolidColorBrush AeroBrush { get => _aeroBrush; private set => Set(ref _aeroBrush, value); }
    public SolidColorBrush FuelBrush { get => _fuelBrush; private set => Set(ref _fuelBrush, value); }
    public SolidColorBrush TyreBrush { get => _tyreBrush; private set => Set(ref _tyreBrush, value); }
    public SolidColorBrush TimingBadgeBrush { get => _timingBadgeBrush; private set => Set(ref _timingBadgeBrush, value); }

    public void Update(
        StandingsSnapshot snapshot,
        int? selectedIndex,
        PlayerSectorDisplay playerSectors,
        bool isDark)
    {
        var timing = snapshot.Timing;
        var playerIndex = timing?.PlayerIdx ?? -1;
        var selectedCar = timing?.Cars.FirstOrDefault(car => car.Idx == selectedIndex);
        var playerDriver = snapshot.Participants?.Drivers.FirstOrDefault(
            driver => driver.Idx == playerIndex);
        var selectedDriver = snapshot.Participants?.Drivers.FirstOrDefault(
            driver => driver.Idx == selectedIndex) ?? playerDriver;
        var viewingOther = selectedCar is not null && selectedCar.Idx != playerIndex;
        var activeStatus = viewingOther
            ? snapshot.AllStatus?.Cars.FirstOrDefault(status => status.Idx == selectedCar!.Idx)
            : snapshot.PlayerStatus?.AsCarStatus(playerIndex);

        DriverName = selectedDriver?.Name ?? string.Empty;
        DriverVisibility = selectedDriver is null ? Visibility.Collapsed : Visibility.Visible;
        TeamBrush = StandingsColors.Brush(selectedDriver?.LiveryColor, "#8E8E8E");

        if (viewingOther)
        {
            UpdateOtherTiming(selectedCar!);
        }
        else
        {
            UpdatePlayerTiming(snapshot.Lap, playerSectors);
        }

        AeroLabel = StandingsFormat.Label(snapshot.Labels, "drs.label");
        UpdateStatus(activeStatus, snapshot.Labels, isDark);
    }

    private void UpdateOtherTiming(TimingCarData car)
    {
        LapLabel = $"Lap {car.LapNum}";
        Position = $"P{car.Position}";
        CurrentLap = StandingsFormat.Milliseconds(car.CurrentLapMs);
        LastLap = StandingsFormat.Milliseconds(car.LastLapMs);
        S1 = car.Sector >= 1 && car.S1Ms > 0
            ? StandingsFormat.Milliseconds(car.S1Ms)
            : "–:––.–––";
        S2 = car.Sector >= 2 && car.S2Ms > 0
            ? StandingsFormat.Milliseconds(car.S2Ms)
            : "–:––.–––";
        S3 = "–:––.–––";
        SetTimingFlags(car.PitStatus, car.LapInvalid, car.PenaltiesS);
    }

    private void UpdatePlayerTiming(LapRowData? lap, PlayerSectorDisplay sectors)
    {
        if (lap is null)
        {
            LapLabel = "Lap —";
            Position = "P—";
            CurrentLap = "--:--.---";
            LastLap = "--:--.---";
            S1 = "–:––.–––";
            S2 = "–:––.–––";
            S3 = "–:––.–––";
            SetTimingFlags(0, false, 0);
            return;
        }

        LapLabel = $"Lap {lap.LapNum}";
        Position = $"P{lap.Position}";
        CurrentLap = StandingsFormat.Milliseconds(lap.CurrentLapMs);
        LastLap = StandingsFormat.Milliseconds(lap.LastLapMs);
        S1 = sectors.S1Done ? StandingsFormat.Milliseconds(sectors.S1) : "–:––.–––";
        S2 = sectors.S2Done ? StandingsFormat.Milliseconds(sectors.S2) : "–:––.–––";
        S3 = sectors.S3Done ? StandingsFormat.Milliseconds(sectors.S3) : "–:––.–––";
        SetTimingFlags(lap.PitStatus, lap.LapInvalid, lap.PenaltiesS);
    }

    private void SetTimingFlags(int pitStatus, bool invalid, int penalties)
    {
        if (invalid)
        {
            TimingBadge = "INVALID";
            TimingBadgeBrush = StandingsColors.Brush("#C4162A");
            TimingBadgeVisibility = Visibility.Visible;
        }
        else if (pitStatus > 0)
        {
            TimingBadge = pitStatus == 1 ? "Pitting" : "In pit lane";
            TimingBadgeBrush = StandingsColors.Brush("#FFD700");
            TimingBadgeVisibility = Visibility.Visible;
        }
        else
        {
            TimingBadge = string.Empty;
            TimingBadgeVisibility = Visibility.Collapsed;
        }

        Penalty = penalties > 0 ? $"+{penalties}s penalty" : string.Empty;
        PenaltyVisibility = penalties > 0 ? Visibility.Visible : Visibility.Collapsed;
    }

    private void UpdateStatus(
        CarStatusData? status,
        IReadOnlyDictionary<string, string> labels,
        bool isDark)
    {
        if (status is null)
        {
            ErsPercent = "—%";
            ErsValue = 0;
            ErsMode = "—";
            ErsEnergy = "— MJ / 4.00 MJ";
            ErsDeployed = "— MJ";
            AeroState = "—";
            Fuel = "—";
            FuelLaps = "— laps vs finish";
            FuelMix = "Mix: —";
            Tyre = "—";
            TyreAge = "Age: — laps";
            BrakeBias = "Brake bias: —% front";
            ErsBrush = StandingsColors.Muted(isDark);
            ErsModeBrush = StandingsColors.Muted(isDark);
            AeroBrush = StandingsColors.Muted(isDark);
            FuelBrush = StandingsColors.Muted(isDark);
            TyreBrush = StandingsColors.Muted(isDark);
            return;
        }

        var ersPercent = Math.Clamp(status.ErsPct, 0, 100);
        ErsPercent = $"{ersPercent:F1}%";
        ErsValue = ersPercent;
        ErsMode = StandingsFormat.Label(labels, "ers.mode", status.ErsMode);
        ErsEnergy = $"{status.ErsJ / 1_000_000.0:F2} MJ / 4.00 MJ";
        ErsDeployed = $"{status.ErsDeployedJ / 1_000_000.0:F2} MJ";
        AeroState = status.DrsAllowed ? "AVAILABLE" : "LOCKED";
        Fuel = $"{status.FuelKg:F1}";
        FuelLaps = $"{(status.FuelLaps >= 0 ? "+" : string.Empty)}{status.FuelLaps:F1} laps vs finish";
        FuelMix = $"Mix: {StandingsFormat.FuelMix(status.FuelMix)}";
        Tyre = StandingsFormat.Label(labels, "tyre.actual", status.TyreCompound);
        TyreAge = $"Age: {status.TyreAgeLaps} laps";
        BrakeBias = $"Brake bias: {status.FrontBrakeBias}% front";

        ErsBrush = StandingsColors.Ers(ersPercent, isDark);
        ErsModeBrush = StandingsColors.ErsMode(status.ErsMode, isDark);
        AeroBrush = status.DrsAllowed
            ? StandingsColors.Brush(isDark ? "#37872D" : "#137333")
            : StandingsColors.Muted(isDark);
        FuelBrush = StandingsColors.Fuel(status.FuelLaps, isDark);
        TyreBrush = StandingsColors.Tyre(status.VisualCompound, isDark);
    }
}

internal readonly record struct PlayerSectorDisplay(
    int S1,
    int S2,
    int S3,
    bool S1Done,
    bool S2Done,
    bool S3Done);

internal sealed class StandingsPageViewModel
{
    private sealed record FrozenSectors(int S1, int S2, int S3, DateTimeOffset Expires);
    private sealed record SectorSnapshot(int S1, int S2, int Lap);

    private readonly Dictionary<int, StandingsRowViewModel> _rowsByCar = [];
    private readonly Dictionary<int, TimingCarData> _previousCars = [];
    private readonly Dictionary<int, FrozenSectors> _frozenCars = [];
    private readonly Dictionary<int, SectorSnapshot> _carSectorSnapshots = [];
    private string? _lastTimingTimestamp;
    private LapRowData? _previousLap;
    private FrozenSectors? _playerFrozen;
    private SectorSnapshot? _playerSectorSnapshot;
    private string? _lastLapTimestamp;

    public ObservableCollection<StandingsRowViewModel> Rows { get; } = [];
    public StandingsDetailViewModel Detail { get; } = new();

    public IReadOnlyList<PositionChange> Apply(
        StandingsSnapshot snapshot,
        int? selectedIndex,
        bool isDark)
    {
        UpdateTimingTracking(snapshot.Timing);
        var playerSectors = UpdatePlayerTracking(snapshot.Lap);
        var changes = ApplyRows(snapshot, selectedIndex, isDark);
        Detail.Update(snapshot, selectedIndex, playerSectors, isDark);
        return changes;
    }

    public void ResetTimeline()
    {
        _previousCars.Clear();
        _frozenCars.Clear();
        _carSectorSnapshots.Clear();
        _lastTimingTimestamp = null;
        _previousLap = null;
        _playerFrozen = null;
        _playerSectorSnapshot = null;
        _lastLapTimestamp = null;
    }

    public StandingsRowViewModel? FindRow(int carIndex) =>
        _rowsByCar.GetValueOrDefault(carIndex);

    private void UpdateTimingTracking(TimingRowData? timing)
    {
        if (timing is null || timing.Ts == _lastTimingTimestamp)
        {
            return;
        }

        _lastTimingTimestamp = timing.Ts;
        var now = DateTimeOffset.UtcNow;
        foreach (var car in timing.Cars)
        {
            if (car.Sector == 2 && car.S1Ms > 0 && car.S2Ms > 0)
            {
                if (!_carSectorSnapshots.TryGetValue(car.Idx, out var snapshot) ||
                    snapshot.Lap != car.LapNum)
                {
                    _carSectorSnapshots[car.Idx] =
                        new SectorSnapshot(car.S1Ms, car.S2Ms, car.LapNum);
                }
            }

            if (_previousCars.TryGetValue(car.Idx, out var previous) &&
                car.LapNum > previous.LapNum)
            {
                var s3 = 0;
                if (_carSectorSnapshots.TryGetValue(car.Idx, out var snapshot) &&
                    snapshot.Lap == previous.LapNum &&
                    car.LastLapMs > 0)
                {
                    s3 = Math.Max(0, car.LastLapMs - snapshot.S1 - snapshot.S2);
                }
                _frozenCars[car.Idx] = new FrozenSectors(
                    previous.S1Ms, previous.S2Ms, s3, now.AddSeconds(7));
            }

            _previousCars[car.Idx] = car;
        }
    }

    private PlayerSectorDisplay UpdatePlayerTracking(LapRowData? lap)
    {
        if (lap is not null && lap.Ts != _lastLapTimestamp)
        {
            _lastLapTimestamp = lap.Ts;
            var now = DateTimeOffset.UtcNow;
            if (lap.Sector == 2 && lap.S1Ms > 0 && lap.S2Ms > 0 &&
                (_playerSectorSnapshot is null ||
                 _playerSectorSnapshot.Lap != lap.LapNum))
            {
                _playerSectorSnapshot = new SectorSnapshot(
                    lap.S1Ms, lap.S2Ms, lap.LapNum);
            }

            if (_previousLap is not null && lap.LapNum > _previousLap.LapNum)
            {
                var s3 = _playerSectorSnapshot is not null &&
                         _playerSectorSnapshot.Lap == _previousLap.LapNum &&
                         lap.LastLapMs > 0
                    ? Math.Max(
                        0,
                        lap.LastLapMs -
                        _playerSectorSnapshot.S1 -
                        _playerSectorSnapshot.S2)
                    : 0;
                _playerFrozen = new FrozenSectors(
                    _previousLap.S1Ms,
                    _previousLap.S2Ms,
                    s3,
                    now.AddSeconds(7));
            }
            _previousLap = lap;
        }

        if (lap is null)
        {
            return default;
        }

        if (_playerFrozen is not null &&
            DateTimeOffset.UtcNow < _playerFrozen.Expires)
        {
            return new PlayerSectorDisplay(
                _playerFrozen.S1,
                _playerFrozen.S2,
                _playerFrozen.S3,
                _playerFrozen.S1 > 0,
                _playerFrozen.S2 > 0,
                _playerFrozen.S3 > 0);
        }

        return new PlayerSectorDisplay(
            lap.S1Ms,
            lap.S2Ms,
            0,
            lap.Sector >= 1 && lap.S1Ms > 0,
            lap.Sector >= 2 && lap.S2Ms > 0,
            false);
    }

    private IReadOnlyList<PositionChange> ApplyRows(
        StandingsSnapshot snapshot,
        int? selectedIndex,
        bool isDark)
    {
        if (snapshot.Timing is null)
        {
            ShowPlaceholders(isDark);
            return [];
        }

        RemovePlaceholders();
        var timing = snapshot.Timing;
        var drivers = (snapshot.Participants?.Drivers ?? [])
            .ToDictionary(driver => driver.Idx);
        var statuses = (snapshot.AllStatus?.Cars ?? [])
            .ToDictionary(status => status.Idx);
        var activeCars = timing.Cars
            .Where(car => car.Position > 0 && car.ResultStatus >= 1)
            .OrderBy(car => car.Position)
            .ToArray();
        var activeIndices = activeCars.Select(car => car.Idx).ToHashSet();
        var changes = new List<PositionChange>();

        foreach (var obsolete in _rowsByCar.Keys.Where(
                     carIndex => !activeIndices.Contains(carIndex)).ToArray())
        {
            var row = _rowsByCar[obsolete];
            Rows.Remove(row);
            _rowsByCar.Remove(obsolete);
        }

        for (var targetIndex = 0; targetIndex < activeCars.Length; targetIndex++)
        {
            var car = activeCars[targetIndex];
            if (!_rowsByCar.TryGetValue(car.Idx, out var row))
            {
                row = new StandingsRowViewModel(car.Idx);
                _rowsByCar.Add(car.Idx, row);
                Rows.Insert(Math.Min(targetIndex, Rows.Count), row);
            }

            var oldPosition = row.Position;
            var sectors = GetCarSectors(car);
            row.Update(
                car,
                drivers.GetValueOrDefault(car.Idx),
                statuses.GetValueOrDefault(car.Idx),
                sectors.S1,
                sectors.S2,
                sectors.S3,
                car.Idx == timing.PlayerIdx,
                car.Idx == snapshot.FastestLapCarIndex,
                car.Idx == selectedIndex,
                isDark,
                snapshot.Labels);

            var currentIndex = Rows.IndexOf(row);
            if (currentIndex != targetIndex)
            {
                Rows.Move(currentIndex, targetIndex);
            }
            if (oldPosition > 0 && oldPosition != car.Position)
            {
                changes.Add(new PositionChange(car.Idx, car.Position < oldPosition));
            }
        }

        return changes;
    }

    private (int S1, int S2, int S3) GetCarSectors(TimingCarData car)
    {
        if (_frozenCars.TryGetValue(car.Idx, out var frozen) &&
            DateTimeOffset.UtcNow < frozen.Expires)
        {
            return (frozen.S1, frozen.S2, frozen.S3);
        }
        return (car.S1Ms, car.S2Ms, 0);
    }

    private void ShowPlaceholders(bool isDark)
    {
        if (Rows.Count != 20 || Rows.Any(row => !row.IsPlaceholder))
        {
            Rows.Clear();
            _rowsByCar.Clear();
            for (var index = 0; index < 20; index++)
            {
                Rows.Add(new StandingsRowViewModel(-index - 1, placeholder: true));
            }
        }

        for (var index = 0; index < Rows.Count; index++)
        {
            Rows[index].UpdatePlaceholder(index + 1, isDark);
        }
    }

    private void RemovePlaceholders()
    {
        if (Rows.Count > 0 && Rows[0].IsPlaceholder)
        {
            Rows.Clear();
            _rowsByCar.Clear();
        }
    }
}

internal static class StandingsFormat
{
    private static readonly string[] FuelMixes = ["Lean", "Standard", "Rich", "Max power"];

    public static string Milliseconds(int milliseconds)
    {
        if (milliseconds <= 0)
        {
            return "--:--.---";
        }
        var minutes = milliseconds / 60_000;
        var seconds = milliseconds % 60_000 / 1000;
        var remaining = milliseconds % 1000;
        return $"{minutes}:{seconds:00}.{remaining:000}";
    }

    public static string Sector(int milliseconds)
    {
        if (milliseconds <= 0)
        {
            return "—";
        }
        return $"{milliseconds / 1000}.{milliseconds % 1000:000}";
    }

    public static string Gap(int milliseconds, int position)
    {
        if (position == 1)
        {
            return "LEADER";
        }
        if (milliseconds <= 0)
        {
            return "—";
        }

        var seconds = milliseconds / 1000.0;
        if (seconds < 60)
        {
            return $"+{seconds:F3}";
        }
        return $"+{(int)(seconds / 60)}:{seconds % 60:00.000}";
    }

    public static string Abbreviate(string name)
    {
        var last = name.Trim().Split(
            ' ',
            StringSplitOptions.RemoveEmptyEntries).LastOrDefault() ?? string.Empty;
        return last[..Math.Min(3, last.Length)].ToUpperInvariant();
    }

    public static string Label(
        IReadOnlyDictionary<string, string> labels,
        string key) =>
        labels.TryGetValue(key, out var value) ? value : key;

    public static string Label(
        IReadOnlyDictionary<string, string> labels,
        string group,
        int value) =>
        labels.TryGetValue($"{group}.{value}", out var label)
            ? label
            : value.ToString();

    public static string FuelMix(int value) =>
        value >= 0 && value < FuelMixes.Length ? FuelMixes[value] : string.Empty;
}

internal static class StandingsColors
{
    public static SolidColorBrush TransparentBrush { get; } =
        new(Color.FromArgb(0, 0, 0, 0));

    public static SolidColorBrush Brush(string? value, string fallback = "#000000") =>
        new(Parse(value, fallback));

    public static SolidColorBrush Brush(Color color) => new(color);

    public static SolidColorBrush Position(int position, bool isDark) => position switch
    {
        1 => Brush(isDark ? "#FFD700" : "#B7950B"),
        2 => Brush(isDark ? "#C0C0C0" : "#5E6475"),
        3 => Brush(isDark ? "#CD7F32" : "#9C5B23"),
        _ => Brush(isDark ? "#8E8E8E" : "#565B70"),
    };

    public static SolidColorBrush Tyre(int compound, bool isDark) => compound switch
    {
        16 => Brush(isDark ? "#E8002D" : "#C8001A"),
        17 => Brush(isDark ? "#FFD700" : "#9E7A00"),
        18 => Brush(isDark ? "#C8C8C8" : "#555555"),
        7 => Brush(isDark ? "#39B54A" : "#1E7A2E"),
        8 => Brush(isDark ? "#4488FF" : "#1A55BB"),
        _ => Brush(isDark ? "#FFFFFF" : "#202020"),
    };

    public static SolidColorBrush Ers(double percent, bool isDark) =>
        percent > 60
            ? Brush(isDark ? "#5794F2" : "#0B57D0")
            : percent > 30
                ? Brush(isDark ? "#D4AD04" : "#B7950B")
                : Brush("#C4162A");

    public static SolidColorBrush ErsMode(int mode, bool isDark) => mode switch
    {
        0 => Muted(isDark),
        1 => Brush(isDark ? "#5794F2" : "#0B57D0"),
        2 => Brush(isDark ? "#FFD700" : "#B7950B"),
        _ => Brush("#C4162A"),
    };

    public static SolidColorBrush Fuel(double fuelLaps, bool isDark) =>
        fuelLaps > 1
            ? Brush(isDark ? "#37872D" : "#137333")
            : fuelLaps >= 0
                ? Brush(isDark ? "#D4AD04" : "#B06000")
                : Brush("#C4162A");

    public static SolidColorBrush Muted(bool isDark) =>
        Brush(isDark ? "#6F7893" : "#73798C");

    public static SolidColorBrush Divider(bool isDark) =>
        Brush(isDark ? "#34394A" : "#D0D5E0");

    public static StandingsBadgeViewModel Badge(string text, string color)
    {
        var parsed = Parse(color, "#C4162A");
        return new StandingsBadgeViewModel
        {
            Text = text,
            Foreground = new SolidColorBrush(parsed),
            BorderBrush = new SolidColorBrush(parsed),
            Background = new SolidColorBrush(
                Color.FromArgb(26, parsed.R, parsed.G, parsed.B)),
        };
    }

    private static Color Parse(string? value, string fallback)
    {
        var hex = (string.IsNullOrWhiteSpace(value) ? fallback : value)
            .Trim()
            .TrimStart('#');
        if (hex.Length == 6 &&
            uint.TryParse(
                hex,
                System.Globalization.NumberStyles.HexNumber,
                null,
                out var rgb))
        {
            return Color.FromArgb(
                255,
                (byte)(rgb >> 16),
                (byte)(rgb >> 8),
                (byte)rgb);
        }
        return Parse(fallback, "#000000");
    }
}
