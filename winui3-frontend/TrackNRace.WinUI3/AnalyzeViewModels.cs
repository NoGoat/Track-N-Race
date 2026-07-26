using System.ComponentModel;
using Microsoft.UI.Xaml.Media;
using Windows.UI;

namespace TrackNRace.WinUI3;

internal enum AnalyzeMetricSource
{
    Telemetry,
    Status,
    Motion,
    MotionEx,
    Damage,
}

internal sealed record AnalyzeMetricDefinition(
    string Id,
    string Group,
    string Label,
    string Unit,
    string ScaleKey,
    double Minimum,
    double Maximum,
    Color DefaultColor,
    AnalyzeMetricSource Source,
    Func<object, double> Value,
    Func<double, string> Format,
    Func<double, string> AxisFormat)
{
    public string Detail => string.IsNullOrEmpty(Unit) ? Group : $"{Group}  ·  {Unit}";
}

internal sealed class AnalyzeMetricItem : INotifyPropertyChanged
{
    private Color _color;
    private bool _isVisible = true;

    public AnalyzeMetricItem(AnalyzeMetricDefinition definition)
    {
        Definition = definition;
        _color = definition.DefaultColor;
    }

    public AnalyzeMetricDefinition Definition { get; }
    public string Id => Definition.Id;
    public string Label => Definition.Label;
    public string Detail => Definition.Detail;
    public string VisibilityGlyph => IsVisible ? "\uE890" : "\uED1A";

    public Color Color
    {
        get => _color;
        set
        {
            if (_color == value) return;
            _color = value;
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(Color)));
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(ColorBrush)));
        }
    }

    public Brush ColorBrush => new SolidColorBrush(Color);

    public bool IsVisible
    {
        get => _isVisible;
        set
        {
            if (_isVisible == value) return;
            _isVisible = value;
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(IsVisible)));
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(VisibilityGlyph)));
        }
    }

    public event PropertyChangedEventHandler? PropertyChanged;
}

internal static class AnalyzeMetrics
{
    private static readonly Color Steering = Color.FromArgb(255, 191, 95, 255);
    private static readonly Color Green = Color.FromArgb(255, 55, 135, 45);
    private static readonly Color Red = Color.FromArgb(255, 196, 22, 42);
    private static readonly Color Blue = Color.FromArgb(255, 87, 148, 242);
    private static readonly Color Yellow = Color.FromArgb(255, 250, 222, 42);
    private static readonly Color Orange = Color.FromArgb(255, 240, 165, 0);

    private static string Number(double value) => $"{value:0}";
    private static string Percent(double value) => $"{value:0}%";
    private static string Percent01(double value) => $"{value * 100:0}%";
    private static string WithUnit(double value, string unit, int decimals = 0) =>
        $"{value.ToString($"F{decimals}")} {unit}";

    private static AnalyzeMetricDefinition Telemetry(
        string id, string group, string label, string unit, string scale,
        double min, double max, Color color,
        Func<TelemetrySample, double> value,
        Func<double, string>? format = null,
        Func<double, string>? axis = null) =>
        new(id, group, label, unit, scale, min, max, color,
            AnalyzeMetricSource.Telemetry, row => value((TelemetrySample)row),
            format ?? (v => WithUnit(v, unit)),
            axis ?? Number);

    private static AnalyzeMetricDefinition Status(
        string id, string group, string label, string unit, string scale,
        double min, double max, Color color,
        Func<PlayerStatusData, double> value,
        Func<double, string>? format = null,
        Func<double, string>? axis = null) =>
        new(id, group, label, unit, scale, min, max, color,
            AnalyzeMetricSource.Status, row => value((PlayerStatusData)row),
            format ?? (v => WithUnit(v, unit, 1)),
            axis ?? Number);

    private static AnalyzeMetricDefinition Motion(
        string id, string label, Color color, Func<MotionSample, double> value) =>
        new(id, "Motion", label, "g", "g-force", -6, 6, color,
            AnalyzeMetricSource.Motion, row => value((MotionSample)row),
            v => WithUnit(v, "g", 2), v => $"{v:0}g");

    private static AnalyzeMetricDefinition MotionEx(
        string id, string label, Color color, Func<MotionExSample, double> value) =>
        new(id, "Motion", label, "mm", "ride-height", -2, 20, color,
            AnalyzeMetricSource.MotionEx, row => value((MotionExSample)row),
            v => WithUnit(v, "mm", 1), v => $"{v:0}mm");

    private static AnalyzeMetricDefinition Damage(
        string id, string label, Color color, Func<DamageRowData, double> value,
        bool life = false) =>
        new(id, "Tyres", label, "%", "percent", 0, 100, color,
            AnalyzeMetricSource.Damage,
            row =>
            {
                var wear = value((DamageRowData)row);
                return life ? 100 - wear : wear;
            },
            v => $"{v:0.0}%", Percent);

    public static readonly IReadOnlyList<AnalyzeMetricDefinition> All = Build();
    public static readonly IReadOnlyDictionary<string, AnalyzeMetricDefinition> ById =
        All.ToDictionary(value => value.Id, StringComparer.Ordinal);

    private static IReadOnlyList<AnalyzeMetricDefinition> Build()
    {
        var metrics = new List<AnalyzeMetricDefinition>
        {
            Telemetry("speed", "Driving", "Speed", "km/h", "speed", 0, 380, Green,
                row => row.SpeedKph),
            Telemetry("rpm", "Driving", "RPM", "rpm", "rpm", 0, 16000, Red,
                row => row.Rpm,
                value => $"{value:0} rpm",
                value => value == 0 ? "0" : $"{value / 1000:0}k"),
            Telemetry("gear", "Driving", "Gear", "", "gear", .5, 8.5, Blue,
                row => row.Gear,
                value => $"Gear {value:0}", Number),
            Telemetry("throttle", "Driving", "Throttle", "%", "input-positive", 0, 1, Green,
                row => row.Throttle, Percent01, Percent01),
            Telemetry("brake", "Driving", "Brake", "%", "input-positive", 0, 1, Red,
                row => row.Brake, Percent01, Percent01),
            Telemetry("steering", "Driving", "Steering", "%", "input-signed", -1, 1, Steering,
                row => row.Steering,
                value => $"{(value < 0 ? "L " : value > 0 ? "R " : "")}{Math.Abs(value) * 100:0}%",
                value => $"{value * 100:0}%"),
            Status("ers", "Driving", "ERS", "%", "percent", 0, 100, Yellow,
                row => row.ErsPct, value => $"{value:0.0}%", Percent),
            Motion("g-lateral", "Lateral G", Orange, row => row.LateralG),
            Motion("g-longitudinal", "Longitudinal G", Blue, row => row.LongitudinalG),
            MotionEx("ride-front", "Front Ride Height", Color.FromArgb(255, 115, 191, 105),
                row => row.FrontAeroHeightMm),
            MotionEx("ride-rear", "Rear Ride Height", Color.FromArgb(255, 184, 119, 219),
                row => row.RearAeroHeightMm),
            Status("power-ice", "Power", "ICE Power", "kW", "power", 0, 1000, Blue,
                row => row.EnginePowerIceKw, value => WithUnit(value, "kW", 1), value => $"{value:0}kW"),
            Status("power-mguk", "Power", "MGU-K Power", "kW", "power", 0, 1000, Yellow,
                row => row.EnginePowerMgukKw, value => WithUnit(value, "kW", 1), value => $"{value:0}kW"),
            Status("harvest-mguk", "Power", "MGU-K Harvest", "kJ", "harvest", 0, 2000, Green,
                row => row.ErsHarvestedMgukJ / 1000d, value => WithUnit(value, "kJ", 1), value => $"{value:0}kJ"),
            Status("harvest-mguh", "Power", "MGU-H Harvest", "kJ", "harvest", 0, 2000, Red,
                row => row.ErsHarvestedMguhJ / 1000d, value => WithUnit(value, "kJ", 1), value => $"{value:0}kJ"),
            Status("fuel", "Power", "Fuel", "kg", "fuel", 0, 110, Orange,
                row => row.FuelKg, value => WithUnit(value, "kg", 2), value => $"{value:0}kg"),
        };

        var corners = new[]
        {
            ("fl", "FL", Color.FromArgb(255, 225, 6, 0)),
            ("fr", "FR", Color.FromArgb(255, 68, 136, 255)),
            ("rl", "RL", Green),
            ("rr", "RR", Color.FromArgb(255, 255, 215, 0)),
        };
        foreach (var (key, label, color) in corners)
        {
            metrics.Add(Telemetry($"surface-{key}", "Tyres", $"Surface Temp {label}", "°C",
                "tyre-temp", 0, 125, color, row => Corner(row, key, "surface"),
                value => WithUnit(value, "°C", 1), value => $"{value:0}°"));
            metrics.Add(Telemetry($"inner-{key}", "Tyres", $"Inner Temp {label}", "°C",
                "tyre-temp", 0, 125, color, row => Corner(row, key, "inner"),
                value => WithUnit(value, "°C", 1), value => $"{value:0}°"));
            metrics.Add(Telemetry($"brake-temp-{key}", "Tyres", $"Brake Temp {label}", "°C",
                "brake-temp", 0, 1250, color, row => Corner(row, key, "brake"),
                value => WithUnit(value, "°C", 1), value => $"{value:0}°"));
            metrics.Add(Damage($"wear-{key}", $"Tyre Wear {label}", color,
                row => Corner(row, key)));
            metrics.Add(Damage($"life-{key}", $"Tyre Life {label}", color,
                row => Corner(row, key), life: true));
        }
        return metrics;
    }

    private static double Corner(TelemetrySample row, string corner, string kind) =>
        (corner, kind) switch
        {
            ("fl", "surface") => row.TyreTempSurfaceFl,
            ("fr", "surface") => row.TyreTempSurfaceFr,
            ("rl", "surface") => row.TyreTempSurfaceRl,
            ("rr", "surface") => row.TyreTempSurfaceRr,
            ("fl", "inner") => row.TyreTempInnerFl,
            ("fr", "inner") => row.TyreTempInnerFr,
            ("rl", "inner") => row.TyreTempInnerRl,
            ("rr", "inner") => row.TyreTempInnerRr,
            ("fl", "brake") => row.BrakeTempFl,
            ("fr", "brake") => row.BrakeTempFr,
            ("rl", "brake") => row.BrakeTempRl,
            _ => row.BrakeTempRr,
        };

    private static double Corner(DamageRowData row, string corner) => corner switch
    {
        "fl" => row.TyreWearFl,
        "fr" => row.TyreWearFr,
        "rl" => row.TyreWearRl,
        _ => row.TyreWearRr,
    };
}
