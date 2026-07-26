namespace TrackNRace.WinUI3;

internal sealed record AnalyzeSeriesDisplaySettings(
    string MetricId,
    string Color,
    bool? Visible);

internal sealed record AnalyzeDisplaySettings(
    int Version,
    bool? Collapsed,
    bool? ShowYAxis,
    AnalyzeSeriesDisplaySettings[]? Series)
{
    public static AnalyzeDisplaySettings Default => new(
        1,
        Collapsed: false,
        ShowYAxis: true,
        Series:
        [
            DefaultSeries("speed"),
            DefaultSeries("rpm"),
            DefaultSeries("ers"),
        ]);

    public static AnalyzeDisplaySettings Sanitize(
        AnalyzeDisplaySettings? settings)
    {
        if (settings?.Series is null)
        {
            return Default;
        }

        var seen = new HashSet<string>(StringComparer.Ordinal);
        var series = new List<AnalyzeSeriesDisplaySettings>();
        foreach (var item in settings.Series)
        {
            if (item is null ||
                !AnalyzeMetrics.ById.TryGetValue(item.MetricId ?? "", out var definition) ||
                !seen.Add(definition.Id))
            {
                continue;
            }
            series.Add(item with
            {
                MetricId = definition.Id,
                Color = IsHexColor(item.Color)
                    ? item.Color.ToUpperInvariant()
                    : ColorHex(definition.DefaultColor),
                Visible = item.Visible is not false,
            });
        }

        return new AnalyzeDisplaySettings(
            1,
            settings.Collapsed is true,
            settings.ShowYAxis is not false,
            series.ToArray());
    }

    private static AnalyzeSeriesDisplaySettings DefaultSeries(string metricId)
    {
        var definition = AnalyzeMetrics.ById[metricId];
        return new AnalyzeSeriesDisplaySettings(
            metricId,
            ColorHex(definition.DefaultColor),
            Visible: true);
    }

    internal static string ColorHex(Windows.UI.Color color) =>
        $"#{color.R:X2}{color.G:X2}{color.B:X2}";

    private static bool IsHexColor(string? value)
    {
        if (value is not { Length: 7 } || value[0] != '#')
        {
            return false;
        }
        for (var index = 1; index < value.Length; index++)
        {
            if (!Uri.IsHexDigit(value[index]))
            {
                return false;
            }
        }
        return true;
    }
}
