using System.Collections.ObjectModel;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Windows.UI;

namespace TrackNRace.WinUI3;

internal abstract class TyreTableItemViewModel
{
}

internal sealed class TyreSetGroupViewModel : TyreTableItemViewModel
{
    public required string Title { get; init; }
}

internal sealed class TyreSetRowViewModel : TyreTableItemViewModel
{
    public required string SetNumber { get; init; }
    public required string Compound { get; init; }
    public required string Status { get; init; }
    public required string Wear { get; init; }
    public required double WearValue { get; init; }
    public required string Life { get; init; }
    public required string Recommended { get; init; }
    public required string Delta { get; init; }
    public required string AutomationName { get; init; }
    public required Brush CompoundBrush { get; init; }
    public required Brush StatusBrush { get; init; }
    public required Brush StatusBackground { get; init; }
    public required Brush WearBrush { get; init; }
    public required Brush DeltaBrush { get; init; }
    public required double ContentOpacity { get; init; }
    public required Visibility FittedVisibility { get; init; }
}

internal sealed class TyreConditionViewModel : BindableBase
{
    private string _surface = "—";
    private string _inner = "—";
    private string _brake = "—";
    private string _wear = "—";
    private double _wearValue;
    private Brush _surfaceBrush = StandingsColors.Brush("#6F7893");
    private Brush _innerBrush = StandingsColors.Brush("#6F7893");
    private Brush _brakeBrush = StandingsColors.Brush("#6F7893");
    private Brush _wearBrush = StandingsColors.Brush("#6F7893");

    public required string Position { get; init; }
    public string Surface { get => _surface; private set => Set(ref _surface, value); }
    public string Inner { get => _inner; private set => Set(ref _inner, value); }
    public string Brake { get => _brake; private set => Set(ref _brake, value); }
    public string Wear { get => _wear; private set => Set(ref _wear, value); }
    public double WearValue { get => _wearValue; private set => Set(ref _wearValue, value); }
    public Brush SurfaceBrush { get => _surfaceBrush; private set => Set(ref _surfaceBrush, value); }
    public Brush InnerBrush { get => _innerBrush; private set => Set(ref _innerBrush, value); }
    public Brush BrakeBrush { get => _brakeBrush; private set => Set(ref _brakeBrush, value); }
    public Brush WearBrush { get => _wearBrush; private set => Set(ref _wearBrush, value); }

    public void Update(
        int? surface,
        int? inner,
        int? brake,
        double? wear,
        IReadOnlyDictionary<string, CardColorSpecData> colors,
        bool dark)
    {
        var muted = StandingsColors.Muted(dark);
        Surface = surface is int surfaceValue ? $"{surfaceValue}°C" : "—";
        Inner = inner is int innerValue ? $"{innerValue}°C" : "—";
        Brake = brake is int brakeValue ? $"{brakeValue}°C" : "—";
        Wear = wear is double wearValue ? $"{wearValue:0.0}%" : "—";
        WearValue = wear ?? 0;
        SurfaceBrush = surface is null
            ? muted
            : TelemetryColors.Resolve(colors, "temp.tyre", surface, dark, muted);
        InnerBrush = inner is null
            ? muted
            : TelemetryColors.Resolve(colors, "temp.tyre", inner, dark, muted);
        BrakeBrush = brake is null
            ? muted
            : TelemetryColors.Resolve(colors, "temp.brake", brake, dark, muted);
        WearBrush = wear is null
            ? muted
            : TelemetryColors.Resolve(colors, "wear", wear, dark, muted);
    }
}

internal sealed class TyresPageViewModel : BindableBase
{
    private static readonly IReadOnlyDictionary<int, string> SessionLabels =
        new Dictionary<int, string>
        {
            [0] = "—", [1] = "FP1", [2] = "FP2", [3] = "FP3",
            [4] = "Q1", [5] = "Q2", [6] = "Q3", [7] = "Race",
        };
    private static readonly IReadOnlyDictionary<int, int> DrySort =
        new Dictionary<int, int> { [16] = 0, [17] = 1, [18] = 2 };
    private static readonly IReadOnlyDictionary<int, int> WetSort =
        new Dictionary<int, int> { [7] = 0, [8] = 1 };

    private string _fittedSummary = string.Empty;
    private Visibility _fittedSummaryVisibility = Visibility.Collapsed;
    private TyreSetsRowData? _lastTyreSets;
    private IReadOnlyDictionary<string, string>? _lastLabels;
    private IReadOnlyDictionary<string, CardColorSpecData>? _lastColors;
    private int? _lastSessionType;
    private bool? _lastDark;

    public ObservableCollection<TyreTableItemViewModel> Rows { get; } = [];
    public TyreConditionViewModel FrontLeft { get; } = new() { Position = "FRONT LEFT" };
    public TyreConditionViewModel FrontRight { get; } = new() { Position = "FRONT RIGHT" };
    public TyreConditionViewModel RearLeft { get; } = new() { Position = "REAR LEFT" };
    public TyreConditionViewModel RearRight { get; } = new() { Position = "REAR RIGHT" };
    public string FittedSummary { get => _fittedSummary; private set => Set(ref _fittedSummary, value); }
    public Visibility FittedSummaryVisibility
    {
        get => _fittedSummaryVisibility;
        private set => Set(ref _fittedSummaryVisibility, value);
    }

    public void Apply(TyresSnapshot snapshot, bool dark)
    {
        UpdateConditions(snapshot, dark);
        if (!ReferenceEquals(_lastTyreSets, snapshot.TyreSets) ||
            !ReferenceEquals(_lastLabels, snapshot.Labels) ||
            !ReferenceEquals(_lastColors, snapshot.CardColors) ||
            _lastSessionType != snapshot.SessionType ||
            _lastDark != dark)
        {
            RebuildAllocation(snapshot, dark);
            _lastTyreSets = snapshot.TyreSets;
            _lastLabels = snapshot.Labels;
            _lastColors = snapshot.CardColors;
            _lastSessionType = snapshot.SessionType;
            _lastDark = dark;
        }
    }

    public void ResetTimeline()
    {
        _lastTyreSets = null;
        _lastLabels = null;
        _lastColors = null;
        _lastSessionType = null;
        _lastDark = null;
    }

    private void UpdateConditions(TyresSnapshot snapshot, bool dark)
    {
        var telemetry = snapshot.LatestTelemetry;
        var damage = snapshot.LatestDamage;
        var hasTelemetry = telemetry is not null;
        FrontLeft.Update(
            telemetry?.TyreTempSurfaceFl,
            telemetry?.TyreTempInnerFl,
            telemetry?.BrakeTempFl,
            hasTelemetry ? damage?.TyreWearFl : null,
            snapshot.CardColors,
            dark);
        FrontRight.Update(
            telemetry?.TyreTempSurfaceFr,
            telemetry?.TyreTempInnerFr,
            telemetry?.BrakeTempFr,
            hasTelemetry ? damage?.TyreWearFr : null,
            snapshot.CardColors,
            dark);
        RearLeft.Update(
            telemetry?.TyreTempSurfaceRl,
            telemetry?.TyreTempInnerRl,
            telemetry?.BrakeTempRl,
            hasTelemetry ? damage?.TyreWearRl : null,
            snapshot.CardColors,
            dark);
        RearRight.Update(
            telemetry?.TyreTempSurfaceRr,
            telemetry?.TyreTempInnerRr,
            telemetry?.BrakeTempRr,
            hasTelemetry ? damage?.TyreWearRr : null,
            snapshot.CardColors,
            dark);
    }

    private void RebuildAllocation(TyresSnapshot snapshot, bool dark)
    {
        Rows.Clear();
        Rows.Add(new TyreSetGroupViewModel { Title = "DRY SETS (SLICKS)" });

        if (snapshot.TyreSets is null)
        {
            for (var index = 0; index < 13; index++)
            {
                Rows.Add(CreatePlaceholder(index, dark));
            }
            Rows.Add(new TyreSetGroupViewModel { Title = "WET / INTER SETS" });
            for (var index = 0; index < 7; index++)
            {
                Rows.Add(CreatePlaceholder(index, dark));
            }
            FittedSummary = string.Empty;
            FittedSummaryVisibility = Visibility.Collapsed;
        }
        else
        {
            var drySets = snapshot.TyreSets.Sets
                .Where(value => value.ActualCompound is not (7 or 8))
                .OrderBy(value => DrySort.TryGetValue(value.VisualCompound, out var order) ? order : 3)
                .ThenBy(value => value.Idx);
            var wetSets = snapshot.TyreSets.Sets
                .Where(value => value.ActualCompound is 7 or 8)
                .OrderBy(value => WetSort.TryGetValue(value.ActualCompound, out var order) ? order : 2)
                .ThenBy(value => value.Idx);
            foreach (var set in drySets)
            {
                Rows.Add(CreateRow(set, snapshot, dark));
            }
            Rows.Add(new TyreSetGroupViewModel { Title = "WET / INTER SETS" });
            foreach (var set in wetSets)
            {
                Rows.Add(CreateRow(set, snapshot, dark));
            }

            var fitted = snapshot.TyreSets.Sets.FirstOrDefault(value => value.Fitted);
            if (fitted is not null)
            {
                FittedSummary =
                    $"{CompoundLabel(snapshot.Labels, fitted.ActualCompound)}  ·  " +
                    $"{fitted.Wear}% wear  ·  {fitted.LifeSpan}L remaining";
                FittedSummaryVisibility = Visibility.Visible;
            }
            else
            {
                FittedSummary = string.Empty;
                FittedSummaryVisibility = Visibility.Collapsed;
            }
        }

    }

    private static TyreSetRowViewModel CreatePlaceholder(int index, bool dark)
    {
        var muted = StandingsColors.Muted(dark);
        return new TyreSetRowViewModel
        {
            SetNumber = $"#{index + 1}",
            Compound = "—",
            Status = "—",
            Wear = "—",
            WearValue = 0,
            Life = "—",
            Recommended = "—",
            Delta = "—",
            AutomationName = $"Tyre set {index + 1}, no data",
            CompoundBrush = muted,
            StatusBrush = muted,
            StatusBackground = StandingsColors.TransparentBrush,
            WearBrush = muted,
            DeltaBrush = muted,
            ContentOpacity = 0.62,
            FittedVisibility = Visibility.Collapsed,
        };
    }

    private static TyreSetRowViewModel CreateRow(
        TyreSetData set,
        TyresSnapshot snapshot,
        bool dark)
    {
        var status = Status(set, snapshot.SessionType);
        var statusColor = status switch
        {
            "FITTED" => dark ? "#5794F2" : "#0B57D0",
            "NEW" => dark ? "#37872D" : "#137333",
            "USED" => dark ? "#D4AD04" : "#B06000",
            "RESERVED" => dark ? "#A78BFA" : "#6D28D9",
            _ => dark ? "#484C62" : "#565B70",
        };
        var statusBrush = StandingsColors.Brush(statusColor);
        var parsed = ParseColor(statusColor);
        var showDelta = !set.Fitted && set.Available && set.LapDeltaMs != 0;
        var delta = showDelta
            ? $"{(set.LapDeltaMs > 0 ? "+" : string.Empty)}{set.LapDeltaMs / 1000d:0.000}s"
            : "—";
        var deltaBrush = showDelta
            ? StandingsColors.Brush(
                set.LapDeltaMs > 0
                    ? "#C4162A"
                    : dark ? "#37872D" : "#137333")
            : StandingsColors.Muted(dark);
        var wearBrush = TelemetryColors.Resolve(
            snapshot.CardColors,
            "wear",
            set.Wear,
            dark,
            StandingsColors.Muted(dark));

        return new TyreSetRowViewModel
        {
            SetNumber = $"#{set.Idx + 1}",
            Compound = CompoundLabel(snapshot.Labels, set.ActualCompound),
            Status = status,
            Wear = $"{set.Wear}%",
            WearValue = set.Wear,
            Life = $"{set.LifeSpan}/{set.UsableLife}L",
            Recommended = SessionLabels.TryGetValue(
                set.RecommendedSession, out var recommended) ? recommended : "—",
            Delta = delta,
            AutomationName =
                $"Tyre set {set.Idx + 1}, {status}, {set.Wear} percent wear",
            CompoundBrush = StandingsColors.Tyre(set.VisualCompound, dark),
            StatusBrush = statusBrush,
            StatusBackground = new SolidColorBrush(
                Color.FromArgb(24, parsed.R, parsed.G, parsed.B)),
            WearBrush = wearBrush,
            DeltaBrush = deltaBrush,
            ContentOpacity = status == "RETURNED" ? 0.4 : 1,
            FittedVisibility =
                set.Fitted ? Visibility.Visible : Visibility.Collapsed,
        };
    }

    private static string CompoundLabel(
        IReadOnlyDictionary<string, string> labels,
        int compound) =>
        labels.TryGetValue($"tyre.actual.{compound}", out var value)
            ? value
            : compound.ToString();

    private static string Status(TyreSetData set, int? sessionType)
    {
        if (set.Fitted)
        {
            return "FITTED";
        }
        if (set.Available && set.Wear == 0)
        {
            return "NEW";
        }
        if (set.Available && set.Wear > 0)
        {
            return "USED";
        }

        if (sessionType is int type)
        {
            if (set.RecommendedSession > SessionOrder(type))
            {
                return "RESERVED";
            }
        }
        else if (set.RecommendedSession >= 4)
        {
            return "RESERVED";
        }
        return "RETURNED";
    }

    private static int SessionOrder(int sessionType)
    {
        if (sessionType is >= 1 and <= 3)
        {
            return sessionType;
        }
        return sessionType switch
        {
            4 => 3,
            5 or 10 => 4,
            6 or 11 => 5,
            7 or 8 or 9 or 12 or 13 or 14 => 6,
            _ => 7,
        };
    }

    private static Color ParseColor(string value)
    {
        var hex = value.TrimStart('#');
        return uint.TryParse(
            hex,
            System.Globalization.NumberStyles.HexNumber,
            System.Globalization.CultureInfo.InvariantCulture,
            out var packed)
                ? Color.FromArgb(
                    255,
                    (byte)(packed >> 16),
                    (byte)(packed >> 8),
                    (byte)packed)
                : Color.FromArgb(255, 128, 128, 128);
    }
}

public sealed class TyreTableTemplateSelector : DataTemplateSelector
{
    public DataTemplate? SectionTemplate { get; set; }
    public DataTemplate? RowTemplate { get; set; }

    protected override DataTemplate? SelectTemplateCore(object item) =>
        item is TyreSetGroupViewModel ? SectionTemplate : RowTemplate;
}
