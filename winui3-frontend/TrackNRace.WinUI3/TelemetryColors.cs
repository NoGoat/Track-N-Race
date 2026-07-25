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
        if (string.IsNullOrEmpty(key) || !specs.TryGetValue(key, out var spec))
        {
            return fallback ?? new SolidColorBrush(
                dark ? Color.FromArgb(255, 255, 255, 255) : Color.FromArgb(255, 0, 0, 0));
        }

        var token = spec.Default;
        foreach (var rule in spec.Rules)
        {
            if (value is null || rule.On != "self")
            {
                continue;
            }

            var matches = rule.Op switch
            {
                "lt" => value < rule.Value,
                "lte" => value <= rule.Value,
                "gt" => value > rule.Value,
                "gte" => value >= rule.Value,
                "eq" => Math.Abs(value.Value - rule.Value) < 0.0001,
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
        "mguk" => dark
            ? Color.FromArgb(255, 0, 208, 255)
            : Color.FromArgb(255, 0, 112, 168),
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
