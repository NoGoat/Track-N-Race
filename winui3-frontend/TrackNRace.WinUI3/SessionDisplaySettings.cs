namespace TrackNRace.WinUI3;

public enum SessionWeatherDensity
{
    Normal,
    CompactWithIcons,
    CompactWithoutIcons,
}

public enum TrackDriverDisplayMode
{
    Dots,
    DotsAndLabels,
    Labels,
}

public sealed record SessionDisplaySettings(
    bool CompactHeader = false,
    bool CompactCards = false,
    SessionWeatherDensity WeatherDensity = SessionWeatherDensity.Normal,
    bool SectorColors = false,
    bool MapDimmed = false,
    TrackDriverDisplayMode DriverDisplayMode = TrackDriverDisplayMode.DotsAndLabels,
    int StaleCarTimeoutSeconds = 10,
    bool ReduceAnimations = false);
