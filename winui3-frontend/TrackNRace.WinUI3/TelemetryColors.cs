using Microsoft.UI.Xaml.Media;
using Windows.UI;

namespace TrackNRace.WinUI3;

internal static class TelemetryColors
{
    public static Brush Resolve(
        IReadOnlyDictionary<string, CardColorSpecData> specs,
        string key,
        double? value,
        bool dark,
        Brush? fallback = null)
    {
        return ResolveContext(specs, key, value, null, dark, fallback);
    }

    public static Brush ResolveContext(
        IReadOnlyDictionary<string, CardColorSpecData> specs,
        string key,
        double? value,
        IReadOnlyDictionary<string, double>? fields,
        bool dark,
        Brush? fallback = null)
    {
        if (string.IsNullOrEmpty(key) || !specs.TryGetValue(key, out var spec))
        {
            return fallback ?? new SolidColorBrush(
                dark ? Color.FromArgb(255, 255, 255, 255) : Color.FromArgb(255, 0, 0, 0));
        }

        var token = spec.Default;
        foreach (var rule in spec.Rules)
        {
            double? candidate = rule.On == "self"
                ? value
                : fields is not null && fields.TryGetValue(rule.On, out var fieldValue)
                    ? fieldValue
                    : null;
            if (candidate is null)
            {
                continue;
            }

            var matches = rule.Op switch
            {
                "lt" => candidate < rule.Value,
                "lte" => candidate <= rule.Value,
                "gt" => candidate > rule.Value,
                "gte" => candidate >= rule.Value,
                "eq" => Math.Abs(candidate.Value - rule.Value) < 0.0001,
                _ => false,
            };
            if (matches)
            {
                token = rule.Color;
                break;
            }
        }
        return new SolidColorBrush(TokenColor(token, dark));
    }

    public static Color TokenColor(string token, bool dark) => token switch
    {
        "pos" => dark
            ? Color.FromArgb(255, 115, 191, 105)
            : Color.FromArgb(255, 19, 115, 51),
        "neg" => Color.FromArgb(255, 225, 6, 0),
        "warn" or "warnAlt" => dark
            ? Color.FromArgb(255, 255, 215, 0)
            : Color.FromArgb(255, 183, 149, 11),
        "info" => dark
            ? Color.FromArgb(255, 87, 148, 242)
            : Color.FromArgb(255, 11, 87, 208),
        "ice" => dark
            ? Color.FromArgb(255, 87, 148, 242)
            : Color.FromArgb(255, 11, 87, 208),
        "mguk" => dark
            ? Color.FromArgb(255, 250, 222, 42)
            : Color.FromArgb(255, 176, 96, 0),
        "fuel" => dark
            ? Color.FromArgb(255, 240, 165, 0)
            : Color.FromArgb(255, 194, 100, 0),
        "wear1" => dark
            ? Color.FromArgb(255, 80, 200, 120)
            : Color.FromArgb(255, 30, 130, 70),
        "wear3" => dark
            ? Color.FromArgb(255, 251, 146, 60)
            : Color.FromArgb(255, 194, 80, 10),
        _ => dark
            ? Color.FromArgb(255, 255, 255, 255)
            : Color.FromArgb(255, 0, 0, 0),
    };
}
