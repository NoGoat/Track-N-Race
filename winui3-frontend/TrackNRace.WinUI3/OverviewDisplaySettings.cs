namespace TrackNRace.WinUI3;

public enum OverviewTyreDensity
{
    Normal,
    Compact1,
    Compact2,
    Compact3,
    Compact4,
    Compact5,
}

public enum OverviewTyreViewMode
{
    Cards,
    Graphs,
}

public sealed record OverviewDisplaySettings(
    bool CompactStats = false,
    bool CompactDamage = false,
    OverviewTyreDensity TyreDensity = OverviewTyreDensity.Normal,
    OverviewTyreViewMode TyreViewMode = OverviewTyreViewMode.Cards);
