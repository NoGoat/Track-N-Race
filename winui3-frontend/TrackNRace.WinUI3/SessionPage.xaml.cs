using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using Windows.UI;

namespace TrackNRace.WinUI3;

internal sealed record SessionEventView(string Time, string Label, SolidColorBrush Brush);
internal sealed record EventRenderKey(
    long TimelineRevision,
    int Count,
    RaceEventData? First,
    RaceEventData? Last,
    ParticipantsRowData? Participants,
    ElementTheme Theme);

public sealed partial class SessionPage : Page
{
    private static readonly IReadOnlyDictionary<int, string> SessionTypes =
        new Dictionary<int, string>
        {
            [0] = "Unknown", [1] = "Practice 1", [2] = "Practice 2",
            [3] = "Practice 3", [4] = "Short Practice", [5] = "Qualifying 1",
            [6] = "Qualifying 2", [7] = "Qualifying 3", [8] = "Short Qualifying",
            [9] = "One-Shot Qualifying", [10] = "Sprint Shootout 1",
            [11] = "Sprint Shootout 2", [12] = "Sprint Shootout 3",
            [13] = "Short Sprint Shootout", [14] = "One-Shot Sprint Shootout",
            [15] = "Race", [16] = "Race 2", [17] = "Race 3", [18] = "Time Trial",
        };
    private static readonly string[] WeatherLabels =
        ["Clear", "Light Cloud", "Overcast", "Light Rain", "Heavy Rain", "Storm"];
    private static readonly string[] WeatherGlyphs =
        ["\uE706", "\uE706", "\uE753", "\uE9C8", "\uE9C8", "\uE9C9"];

    private readonly DispatcherTimer _refreshTimer = new()
    {
        Interval = TimeSpan.FromMilliseconds(16),
    };
    private TelemetrySessionStore? _store;
    private SessionSnapshot? _lastColdSnapshot;
    private EventRenderKey? _lastEventKey;
    private SessionDisplaySettings? _lastSettings;
    private bool _dirty = true;
    private bool _isLoaded;
    private bool _mapFullscreen;

    public SessionPage()
    {
        InitializeComponent();
        _refreshTimer.Tick += OnRefreshTick;
    }

    private void OnLoaded(object sender, RoutedEventArgs args)
    {
        if (_isLoaded) return;
        _isLoaded = true;
        var app = (App)Application.Current;
        _store = app.TelemetryState;
        if (_store is not null)
        {
            _store.SnapshotChanged += OnSnapshotChanged;
            _store.TimelineReset += OnTimelineReset;
        }
        app.SessionDisplayChanged += OnSessionDisplayChanged;
        _lastColdSnapshot = null;
        _lastEventKey = null;
        _dirty = true;
        _refreshTimer.Start();
    }

    private void OnUnloaded(object sender, RoutedEventArgs args)
    {
        if (!_isLoaded) return;
        _isLoaded = false;
        _refreshTimer.Stop();
        if (_store is not null)
        {
            _store.SnapshotChanged -= OnSnapshotChanged;
            _store.TimelineReset -= OnTimelineReset;
            _store = null;
        }
        ((App)Application.Current).SessionDisplayChanged -= OnSessionDisplayChanged;
    }

    private void OnSnapshotChanged() => _dirty = true;
    private void OnTimelineReset(TimelineResetReason reason) => _dirty = true;
    private void OnSessionDisplayChanged() => _dirty = true;

    private void OnRefreshTick(object? sender, object args)
    {
        if (!_dirty || !_isLoaded) return;
        _dirty = false;
        var snapshot = _store?.SessionSnapshot;
        if (snapshot is null) return;
        var settings = ((App)Application.Current).SessionDisplay;
        TrackMap.Apply(snapshot, settings);

        var eventKey = CreateEventRenderKey(snapshot);
        if (eventKey != _lastEventKey)
        {
            RenderEvents(snapshot);
            _lastEventKey = eventKey;
        }

        var coldChanged = _lastColdSnapshot is null ||
            !ReferenceEquals(_lastColdSnapshot.Session, snapshot.Session) ||
            !ReferenceEquals(_lastColdSnapshot.Timing, snapshot.Timing) ||
            !ReferenceEquals(_lastColdSnapshot.Participants, snapshot.Participants) ||
            _lastColdSnapshot.AeroMode != snapshot.AeroMode ||
            !ReferenceEquals(_lastSettings, settings);
        if (coldChanged)
        {
            RenderColdContent(snapshot, settings);
            _lastColdSnapshot = snapshot;
            _lastSettings = settings;
        }
    }

    private void RenderColdContent(
        SessionSnapshot snapshot,
        SessionDisplaySettings settings)
    {
        var session = snapshot.Session;
        var details = session is null
            ? ("—", string.Empty)
            : TrackMap.TrackDetails(session.TrackId, snapshot.Labels);
        GrandPrixText.Text = details.Item1;
        CircuitText.Text = details.Item2;
        TimeLeftText.Text = session is null ? "--:--" : FormatTimeLeft(session.SessionTimeLeft);
        RenderMarshalZones(session?.MarshalZones ?? []);
        RenderStats(snapshot, settings);
        RenderWeather(snapshot, settings);
        RenderProximity(snapshot);
        ApplyDensity(settings);
    }

    private EventRenderKey CreateEventRenderKey(SessionSnapshot snapshot)
    {
        var events = snapshot.RaceEvents;
        return new EventRenderKey(
            snapshot.TimelineRevision,
            events.Count,
            events.Count == 0 ? null : events[0],
            events.Count == 0 ? null : events[^1],
            snapshot.Participants,
            ActualTheme);
    }

    private void RenderMarshalZones(IReadOnlyList<MarshalZoneData> zones)
    {
        MarshalZonesGrid.Children.Clear();
        MarshalZonesGrid.ColumnDefinitions.Clear();
        var valid = zones.Where(value => value.Flag != -1).ToArray();
        if (valid.Length == 0)
        {
            MarshalZonesGrid.Children.Add(new TextBlock
            {
                Text = "No zone data",
                FontSize = 10,
                Foreground = SecondaryBrush(),
            });
            return;
        }
        for (var index = 0; index < valid.Length; index++)
        {
            var end = index + 1 < valid.Length ? valid[index + 1].ZoneStart : 1;
            var width = Math.Max(0.005, end - valid[index].ZoneStart);
            MarshalZonesGrid.ColumnDefinitions.Add(new ColumnDefinition
            {
                Width = new GridLength(width, GridUnitType.Star),
            });
            var bar = new Border
            {
                Margin = new Thickness(1, 0, 1, 0),
                CornerRadius = new CornerRadius(2),
                Background = new SolidColorBrush(valid[index].Flag switch
                {
                    1 => Color.FromArgb(255, 0, 200, 83),
                    2 => Color.FromArgb(255, 33, 150, 243),
                    3 => Color.FromArgb(255, 253, 216, 53),
                    _ => ActualTheme == ElementTheme.Dark
                        ? Color.FromArgb(20, 255, 255, 255)
                        : Color.FromArgb(20, 0, 0, 0),
                }),
            };
            Grid.SetColumn(bar, index);
            MarshalZonesGrid.Children.Add(bar);
        }
    }

    private void RenderStats(SessionSnapshot snapshot, SessionDisplaySettings settings)
    {
        StatsGrid.Children.Clear();
        StatsGrid.ColumnDefinitions.Clear();
        var session = snapshot.Session;
        var player = snapshot.Timing?.Cars.FirstOrDefault(value =>
            value.Idx == snapshot.Timing.PlayerIdx && value.ResultStatus == 2);
        var remaining = session is not null && session.TotalLaps > 0 && player is not null
            ? Math.Max(0, session.TotalLaps - player.LapNum + 1).ToString()
            : "—";
        var pitWindow = session is not null && session.PitStopWindowIdealLap > 0
            ? $"L{session.PitStopWindowIdealLap}–{session.PitStopWindowLatestLap}"
            : "—";
        var rejoin = session is not null && session.PitStopRejoinPosition > 0
            ? $"P{session.PitStopRejoinPosition}"
            : "—";
        var values = new[]
        {
            ("TOTAL LAPS", session is { TotalLaps: > 0 } ? session.TotalLaps.ToString() : "—", "", (double?)null, ""),
            ("REMAINING", remaining, "", (double?)null, ""),
            ("PIT SPEED", session?.PitSpeedLimit.ToString() ?? "—", session is null ? "" : "km/h", (double?)null, "session.pitSpeed"),
            ("PIT WINDOW", pitWindow, "", (double?)null, "session.pitWindow"),
            ("REJOIN", rejoin, "", (double?)null, "session.rejoin"),
            ("TRACK TEMP", session is null ? "—" : $"{session.TrackTemp}°C", "", (double?)session?.TrackTemp, "session.trackTemp"),
            ("AIR TEMP", session is null ? "—" : $"{session.AirTemp}°C", "", (double?)session?.AirTemp, "session.airTemp"),
            (settings.CompactCards ? "LENGTH" : "TRACK LENGTH", session is null ? "—" : $"{session.TrackLengthM / 1000.0:0.000}", session is null ? "" : "km", (double?)null, ""),
            (settings.CompactCards ? "TIME" : "TIME OF DAY", session is null ? "—" : FormatTimeOfDay(session.TimeOfDay), "", (double?)null, ""),
        };
        for (var index = 0; index < values.Length; index++)
        {
            StatsGrid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            var item = values[index];
            var valueBrush = CardBrush(snapshot, item.Item5, item.Item4);
            FrameworkElement card;
            if (settings.CompactCards)
            {
                card = new Grid
                {
                    Padding = new Thickness(10, 8, 10, 8),
                    ColumnDefinitions =
                    {
                        new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) },
                        new ColumnDefinition { Width = GridLength.Auto },
                    },
                    Children =
                    {
                        new TextBlock
                        {
                            Text = item.Item1,
                            Style = (Style)Resources["SessionSectionLabelStyle"],
                            FontSize = 10,
                            TextTrimming = TextTrimming.CharacterEllipsis,
                            VerticalAlignment = VerticalAlignment.Center,
                        },
                        CreateInlineValue(item.Item2, item.Item3, 15, valueBrush, 1),
                    },
                };
            }
            else
            {
                card = new StackPanel
                {
                    Padding = new Thickness(12, 10, 12, 10),
                    Children =
                    {
                        new TextBlock
                        {
                            Text = item.Item1,
                            Style = (Style)Resources["SessionSectionLabelStyle"],
                            TextTrimming = TextTrimming.CharacterEllipsis,
                        },
                        CreateInlineValue(item.Item2, item.Item3, 22, valueBrush),
                    },
                };
            }
            var wrapper = new Border
            {
                BorderBrush = DividerBrush(),
                BorderThickness = index < values.Length - 1
                    ? new Thickness(0, 0, 1, 0)
                    : new Thickness(0, 0, 0, 0),
                Child = card,
            };
            Grid.SetColumn(wrapper, index);
            StatsGrid.Children.Add(wrapper);
        }
    }

    private TextBlock CreateInlineValue(
        string value,
        string unit,
        double size,
        Brush brush,
        int gridColumn = 0)
    {
        var block = new TextBlock
        {
            Margin = gridColumn == 0 ? new Thickness(0, 5, 0, 0) : new Thickness(5, 0, 0, 0),
            FontSize = size,
            FontWeight = Microsoft.UI.Text.FontWeights.Bold,
            Foreground = brush,
            Text = string.IsNullOrEmpty(unit) ? value : $"{value} {unit}",
            TextTrimming = TextTrimming.CharacterEllipsis,
            HorizontalAlignment = gridColumn == 0 ? HorizontalAlignment.Left : HorizontalAlignment.Right,
            VerticalAlignment = VerticalAlignment.Center,
        };
        Grid.SetColumn(block, gridColumn);
        return block;
    }

    private void RenderWeather(SessionSnapshot snapshot, SessionDisplaySettings settings)
    {
        WeatherGrid.Children.Clear();
        WeatherGrid.ColumnDefinitions.Clear();
        var session = snapshot.Session;
        var forecast = session?.WeatherForecastSamples
            .Where(value => value.TimeOffset > 0)
            .Take(5)
            .ToArray() ?? [];
        var forecastCardCount = session is null ? 5 : forecast.Length;
        for (var index = 0; index < forecastCardCount + 1; index++)
        {
            WeatherGrid.ColumnDefinitions.Add(new ColumnDefinition
            {
                Width = new GridLength(1, GridUnitType.Star),
            });
        }
        AddWeatherCard(0, "NOW", session?.Weather, null, null,
            session is null ? null : session.ForecastAccuracy == 0 ? "Exact" : "Approx", settings);
        for (var index = 0; index < forecastCardCount; index++)
        {
            var sample = index < forecast.Length ? forecast[index] : null;
            AddWeatherCard(index + 1,
                sample is null ? "—" : $"+{sample.TimeOffset}m",
                sample?.Weather,
                sample?.RainPercentage,
                null,
                null,
                settings);
        }
    }

    private void AddWeatherCard(
        int column,
        string time,
        int? weather,
        int? rain,
        string? unused,
        string? detail,
        SessionDisplaySettings settings)
    {
        var label = weather is int id && id >= 0 && id < WeatherLabels.Length
            ? WeatherLabels[id]
            : "—";
        var glyph = weather is int glyphId && glyphId >= 0 && glyphId < WeatherGlyphs.Length
            ? WeatherGlyphs[glyphId]
            : WeatherGlyphs[2];
        if (settings.WeatherDensity != SessionWeatherDensity.Normal)
        {
            AddCompactWeatherCard(
                column,
                time,
                weather,
                rain,
                label,
                glyph,
                settings.WeatherDensity == SessionWeatherDensity.CompactWithIcons);
            return;
        }
        var panel = new StackPanel
        {
            Padding = new Thickness(8, 9, 8, 7),
            HorizontalAlignment = HorizontalAlignment.Stretch,
            VerticalAlignment = VerticalAlignment.Center,
            Orientation = Orientation.Horizontal,
            Spacing = 8,
        };
        panel.Children.Add(new FontIcon
        {
            FontFamily = new FontFamily("Segoe Fluent Icons"),
            Glyph = glyph,
            FontSize = 27,
            Foreground = WeatherBrush(weather),
        });
        var text = new StackPanel { VerticalAlignment = VerticalAlignment.Center };
        var timeLabel = new TextBlock { Text = time };
        if (time == "NOW")
        {
            timeLabel.Style = (Style)Resources["SessionSectionLabelStyle"];
        }
        else
        {
            timeLabel.FontSize = 9;
            timeLabel.Foreground = SecondaryBrush();
        }
        text.Children.Add(timeLabel);
        text.Children.Add(new TextBlock
        {
            Text = label,
            FontSize = 12,
            FontWeight = Microsoft.UI.Text.FontWeights.SemiBold,
            Foreground = WeatherBrush(weather),
            TextTrimming = TextTrimming.CharacterEllipsis,
        });
        text.Children.Add(new TextBlock
        {
            Text = rain is int percent ? $"{percent}%" : detail ?? "—",
            FontSize = 9,
            Foreground = rain is int value ? RainBrush(value) : SecondaryBrush(),
        });
        panel.Children.Add(text);
        AddWeatherWrapper(column, panel);
    }

    private void AddCompactWeatherCard(
        int column,
        string time,
        int? weather,
        int? rain,
        string label,
        string glyph,
        bool showIcon)
    {
        var card = new Grid
        {
            Padding = showIcon
                ? new Thickness(time == "NOW" ? 12 : 8, 0, time == "NOW" ? 12 : 8, 0)
                : new Thickness(time == "NOW" ? 16 : 8, 0, time == "NOW" ? 16 : 8, 0),
            VerticalAlignment = VerticalAlignment.Stretch,
            ColumnDefinitions =
            {
                new ColumnDefinition { Width = GridLength.Auto },
                new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) },
                new ColumnDefinition { Width = GridLength.Auto },
            },
        };
        var timeLabel = new TextBlock
        {
            Text = time,
            FontSize = showIcon && time != "NOW" ? 12 : 9,
            FontWeight = time == "NOW"
                ? Microsoft.UI.Text.FontWeights.Medium
                : Microsoft.UI.Text.FontWeights.Normal,
            CharacterSpacing = time == "NOW" ? 120 : 0,
            Foreground = SecondaryBrush(),
            VerticalAlignment = VerticalAlignment.Center,
        };
        Grid.SetColumn(timeLabel, 0);
        card.Children.Add(timeLabel);

        if (showIcon)
        {
            var condition = new StackPanel
            {
                Orientation = Orientation.Horizontal,
                Spacing = 8,
                HorizontalAlignment = HorizontalAlignment.Center,
                VerticalAlignment = VerticalAlignment.Center,
            };
            condition.Children.Add(new FontIcon
            {
                FontFamily = new FontFamily("Segoe Fluent Icons"),
                Glyph = glyph,
                FontSize = 26,
                Foreground = WeatherBrush(weather),
            });
            condition.Children.Add(new TextBlock
            {
                Text = label,
                FontSize = 14,
                FontWeight = Microsoft.UI.Text.FontWeights.SemiBold,
                Foreground = weather is null ? SecondaryBrush() : PrimaryBrush(),
                TextTrimming = TextTrimming.CharacterEllipsis,
                VerticalAlignment = VerticalAlignment.Center,
            });
            Grid.SetColumn(condition, 1);
            card.Children.Add(condition);
        }
        else
        {
            var condition = new TextBlock
            {
                Text = label,
                FontSize = 12,
                FontWeight = Microsoft.UI.Text.FontWeights.SemiBold,
                Foreground = WeatherBrush(weather),
                TextTrimming = TextTrimming.CharacterEllipsis,
                HorizontalAlignment = HorizontalAlignment.Center,
                VerticalAlignment = VerticalAlignment.Center,
            };
            Grid.SetColumn(condition, 1);
            card.Children.Add(condition);
        }

        if (rain is int percent)
        {
            var rainLabel = new TextBlock
            {
                Text = $"{percent}%",
                FontSize = showIcon ? 12 : 10,
                FontWeight = Microsoft.UI.Text.FontWeights.Bold,
                Foreground = RainBrush(percent),
                VerticalAlignment = VerticalAlignment.Center,
            };
            Grid.SetColumn(rainLabel, 2);
            card.Children.Add(rainLabel);
        }
        AddWeatherWrapper(column, card);
    }

    private void AddWeatherWrapper(int column, UIElement content)
    {
        var wrapper = new Border
        {
            BorderBrush = DividerBrush(),
            BorderThickness = column < WeatherGrid.ColumnDefinitions.Count - 1
                ? new Thickness(0, 0, 1, 0)
                : new Thickness(0, 0, 0, 0),
            Child = content,
        };
        Grid.SetColumn(wrapper, column);
        WeatherGrid.Children.Add(wrapper);
    }

    private void RenderProximity(SessionSnapshot snapshot)
    {
        ProximityPanel.Children.Clear();
        if (snapshot.Timing is null)
        {
            ProximityPanel.Children.Add(EmptyRailText("No timing data"));
            return;
        }
        var active = snapshot.Timing.Cars
            .Where(value => value.ResultStatus == 2 && value.Position > 0)
            .OrderBy(value => value.Position)
            .ToArray();
        var player = active.FirstOrDefault(value => value.Idx == snapshot.Timing.PlayerIdx);
        if (player is null)
        {
            ProximityPanel.Children.Add(EmptyRailText("No position data"));
            return;
        }
        var rows = new List<(TimingCarData Car, bool Player, int? Delta, bool Ahead)>();
        if (player.Position == 1)
        {
            rows.Add((player, true, null, false));
            foreach (var car in active.Where(value => value.Position is 2 or 3))
                rows.Add((car, false, car.GapMs - player.GapMs, false));
        }
        else if (player.Position == active.Length)
        {
            foreach (var car in active.Where(value => value.Position == player.Position - 2 ||
                value.Position == player.Position - 1))
                rows.Add((car, false, player.GapMs - car.GapMs, true));
            rows.Add((player, true, null, false));
        }
        else
        {
            var ahead = active.FirstOrDefault(value => value.Position == player.Position - 1);
            var behind = active.FirstOrDefault(value => value.Position == player.Position + 1);
            if (ahead is not null) rows.Add((ahead, false, player.GapMs - ahead.GapMs, true));
            rows.Add((player, true, null, false));
            if (behind is not null) rows.Add((behind, false, behind.GapMs - player.GapMs, false));
        }
        foreach (var row in rows)
        {
            var driver = snapshot.Participants?.Drivers.FirstOrDefault(value => value.Idx == row.Car.Idx);
            var color = ParseColor(driver?.LiveryColor, Color.FromArgb(255, 142, 142, 142));
            var grid = new Grid
            {
                Padding = new Thickness(12, 9, 12, 9),
                ColumnSpacing = 9,
                ColumnDefinitions =
                {
                    new ColumnDefinition { Width = GridLength.Auto },
                    new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) },
                    new ColumnDefinition { Width = GridLength.Auto },
                },
            };
            grid.Children.Add(new Border
            {
                Padding = new Thickness(6, 2, 6, 2),
                CornerRadius = new CornerRadius(3),
                Background = new SolidColorBrush(Color.FromArgb(35, color.R, color.G, color.B)),
                Child = new TextBlock
                {
                    Text = $"P{row.Car.Position}",
                    FontSize = 10,
                    FontWeight = Microsoft.UI.Text.FontWeights.Bold,
                    Foreground = new SolidColorBrush(color),
                },
            });
            var name = new TextBlock
            {
                Text = DriverSurname(driver, row.Car.Idx),
                FontSize = 13,
                FontWeight = Microsoft.UI.Text.FontWeights.SemiBold,
                Foreground = row.Player ? PrimaryBrush() : SecondaryBrush(),
                TextTrimming = TextTrimming.CharacterEllipsis,
                VerticalAlignment = VerticalAlignment.Center,
            };
            Grid.SetColumn(name, 1);
            grid.Children.Add(name);
            if (row.Delta is > 0)
            {
                var delta = new TextBlock
                {
                    Text = $"{(row.Ahead ? "-" : "+")}{row.Delta.Value / 1000.0:0.000}",
                    FontSize = 11,
                    Foreground = row.Ahead
                        ? new SolidColorBrush(Color.FromArgb(255, 115, 191, 105))
                        : SecondaryBrush(),
                    VerticalAlignment = VerticalAlignment.Center,
                };
                Grid.SetColumn(delta, 2);
                grid.Children.Add(delta);
            }
            ProximityPanel.Children.Add(grid);
        }
    }

    private void RenderEvents(SessionSnapshot snapshot)
    {
        var events = snapshot.RaceEvents
            .Select(value => FormatEvent(value, snapshot))
            .Where(value => value is not null)
            .Reverse()
            .Cast<SessionEventView>()
            .ToArray();
        EventsList.ItemsSource = events;
        EventsList.Visibility = events.Length == 0 ? Visibility.Collapsed : Visibility.Visible;
        NoEventsText.Visibility = events.Length == 0 ? Visibility.Visible : Visibility.Collapsed;
    }

    private SessionEventView? FormatEvent(RaceEventData value, SessionSnapshot snapshot)
    {
        var dark = ActualTheme == ElementTheme.Dark;
        var gray = dark ? Color.FromArgb(255, 160, 168, 184) : Color.FromArgb(255, 86, 91, 112);
        var blue = dark ? Color.FromArgb(255, 87, 148, 242) : Color.FromArgb(255, 11, 87, 208);
        string Name(int? index) => DriverSurname(
            snapshot.Participants?.Drivers.FirstOrDefault(driver => driver.Idx == (index ?? 0)),
            index ?? 0);
        (string Label, Color Color)? formatted = value.Code switch
        {
            "FTLP" => ($"Fastest Lap — {Name(value.CarIdx)}  {FormatLap(value.LapTimeS ?? 0)}", Color.FromArgb(255, 191, 95, 255)),
            "DRSE" => ("DRS Enabled", Color.FromArgb(255, 55, 135, 45)),
            "DRSD" => ("DRS Disabled", gray),
            "RDFL" => ("Red Flag", Color.FromArgb(255, 225, 6, 0)),
            "CHQF" => ("Chequered Flag", gray),
            "LGOT" => ("Lights Out", Color.FromArgb(255, 55, 135, 45)),
            "SSTA" => ("Session Start", blue),
            "SEND" => ("Session End", blue),
            "RTMT" => ($"Retired — {Name(value.CarIdx)}", gray),
            "RCWN" => ($"Race Winner — {Name(value.CarIdx)}", Color.FromArgb(255, 255, 215, 0)),
            "DTSV" => ($"DT Served — {Name(value.CarIdx)}", gray),
            "SGSV" => ($"SG Served — {Name(value.CarIdx)}", gray),
            "SCAR" => (FormatSafetyCar(value), Color.FromArgb(255, 255, 215, 0)),
            "PENA" => FormatPenalty(value, Name, dark),
            "OVTK" => ($"Overtake — {Name(value.OvertakingCarIdx)} passed {Name(value.BeingOvertakenCarIdx)}", gray),
            _ => null,
        };
        if (formatted is null) return null;
        return new SessionEventView(
            FormatSessionTime(value.SessionTime),
            formatted.Value.Label,
            new SolidColorBrush(formatted.Value.Color));
    }

    private static (string Label, Color Color)? FormatPenalty(
        RaceEventData value,
        Func<int?, string> name,
        bool dark)
    {
        var labels = new Dictionary<int, string>
        {
            [0] = "Drive Through", [1] = "Stop-Go", [2] = "Grid Penalty",
            [4] = "Time Penalty", [5] = "Warning", [6] = "DSQ",
        };
        if (value.PenaltyType is not int type || !labels.TryGetValue(type, out var label))
            return null;
        var time = type is 1 or 4 && value.PenaltyTimeS is > 0 ? $" {value.PenaltyTimeS}s" : "";
        var infringement = Infringement(value.InfringementType);
        var color = type switch
        {
            0 or 1 or 6 => Color.FromArgb(255, 225, 6, 0),
            5 => dark ? Color.FromArgb(255, 255, 215, 0) : Color.FromArgb(255, 183, 149, 11),
            _ => dark ? Color.FromArgb(255, 196, 125, 14) : Color.FromArgb(255, 194, 100, 0),
        };
        return ($"{label}{time} — {name(value.CarIdx)}{(infringement is null ? "" : $" — {infringement}")}", color);
    }

    private void ApplyDensity(SessionDisplaySettings settings)
    {
        SessionHeader.MinHeight = settings.CompactHeader ? 46 : 68;
        TrackHeading.Padding = settings.CompactHeader
            ? new Thickness(20, 7, 20, 7)
            : new Thickness(24, 12, 24, 12);
        GrandPrixText.FontSize = settings.CompactHeader ? 16 : 20;
        CircuitText.Visibility = settings.CompactHeader ? Visibility.Collapsed : Visibility.Visible;
        TimeLeftLabel.Visibility = settings.CompactHeader ? Visibility.Collapsed : Visibility.Visible;
        TimeLeftText.FontSize = settings.CompactHeader ? 20 : 28;
        StatsGrid.MinHeight = settings.CompactCards ? 42 : 72;
        WeatherRow.Height = _mapFullscreen
            ? new GridLength(0)
            : new GridLength(settings.WeatherDensity switch
            {
                SessionWeatherDensity.CompactWithoutIcons => 38,
                SessionWeatherDensity.CompactWithIcons => 58,
                _ => 90,
            });
        RailColumn.Width = _mapFullscreen
            ? new GridLength(0)
            : new GridLength(ActualWidth > 1100 ? 288 : 230);
    }

    private void OnMapFullscreenRequested()
    {
        _mapFullscreen = !_mapFullscreen;
        SessionHeader.Visibility = _mapFullscreen ? Visibility.Collapsed : Visibility.Visible;
        StatsGrid.Visibility = _mapFullscreen ? Visibility.Collapsed : Visibility.Visible;
        RightRail.Visibility = _mapFullscreen ? Visibility.Collapsed : Visibility.Visible;
        WeatherGrid.Visibility = _mapFullscreen ? Visibility.Collapsed : Visibility.Visible;
        WeatherRow.Height = _mapFullscreen ? new GridLength(0) : WeatherRow.Height;
        HeaderRow.Height = _mapFullscreen ? new GridLength(0) : GridLength.Auto;
        StatsRow.Height = _mapFullscreen ? new GridLength(0) : GridLength.Auto;
        RailColumn.Width = _mapFullscreen ? new GridLength(0) : new GridLength(288);
        TrackMap.SetFullscreenState(_mapFullscreen);
        if (!_mapFullscreen)
        {
            ApplyDensity(((App)Application.Current).SessionDisplay);
        }
        _dirty = true;
    }

    private void OnActualThemeChanged(FrameworkElement sender, object args)
    {
        _lastColdSnapshot = null;
        _lastEventKey = null;
        _dirty = true;
    }

    private void OnPageSizeChanged(object sender, SizeChangedEventArgs args)
    {
        if (_isLoaded && !_mapFullscreen)
        {
            ApplyDensity(((App)Application.Current).SessionDisplay);
        }
    }

    private static string Label(SessionSnapshot snapshot, string key, string fallback) =>
        snapshot.Labels.TryGetValue(key, out var value) ? value : fallback;

    private Brush CardBrush(SessionSnapshot snapshot, string key, double? value)
    {
        if (string.IsNullOrEmpty(key) || !snapshot.CardColors.TryGetValue(key, out var spec))
            return PrimaryBrush();
        var token = spec.Default;
        foreach (var rule in spec.Rules)
        {
            if (value is null || rule.On != "self") continue;
            var matches = rule.Op switch
            {
                "lt" => value < rule.Value, "lte" => value <= rule.Value,
                "gt" => value > rule.Value, "gte" => value >= rule.Value,
                "eq" => Math.Abs(value.Value - rule.Value) < 0.0001,
                _ => false,
            };
            if (matches) { token = rule.Color; break; }
        }
        return new SolidColorBrush(TokenColor(token, ActualTheme == ElementTheme.Dark));
    }

    private static Color TokenColor(string token, bool dark) => token switch
    {
        "pos" => dark ? Color.FromArgb(255, 115, 191, 105) : Color.FromArgb(255, 19, 115, 51),
        "neg" => Color.FromArgb(255, 225, 6, 0),
        "warn" or "warnAlt" => dark ? Color.FromArgb(255, 255, 215, 0) : Color.FromArgb(255, 183, 149, 11),
        "info" => dark ? Color.FromArgb(255, 87, 148, 242) : Color.FromArgb(255, 11, 87, 208),
        "mguk" => dark ? Color.FromArgb(255, 0, 208, 255) : Color.FromArgb(255, 0, 112, 168),
        "wear1" => dark ? Color.FromArgb(255, 80, 200, 120) : Color.FromArgb(255, 30, 130, 70),
        "wear3" => dark ? Color.FromArgb(255, 251, 146, 60) : Color.FromArgb(255, 194, 80, 10),
        _ => dark
            ? Color.FromArgb(255, 255, 255, 255)
            : Color.FromArgb(255, 0, 0, 0),
    };

    private static string FormatTimeLeft(int seconds) =>
        seconds <= 0 ? "0:00" : $"{seconds / 60}:{seconds % 60:00}";
    private static string FormatSessionTime(float? seconds) =>
        seconds is null or <= 0 ? "00:00" : $"{(int)(seconds / 60):00}:{(int)(seconds % 60):00}";
    private static string FormatLap(double seconds) =>
        $"{(int)(seconds / 60)}:{seconds % 60:00.000}";
    private static string FormatTimeOfDay(int minutes)
    {
        var hour = minutes / 60 % 24;
        var minute = minutes % 60;
        return $"{(hour % 12 == 0 ? 12 : hour % 12)}:{minute:00} {(hour >= 12 ? "PM" : "AM")}";
    }
    private static string FormatSafetyCar(RaceEventData value)
    {
        var type = value.SafetyCarType switch { 1 => "Safety Car", 2 => "Virtual SC", 3 => "Formation Lap", _ => "SC" };
        var action = value.EventType switch { 0 => "Deployed", 1 => "Returning", 2 => "Returned", 3 => "Resume Race", _ => "" };
        return $"{type} — {action}";
    }
    private static string? Infringement(int? value) => value switch
    {
        0 => "Blocking by slowing", 1 => "Blocking wrong way", 2 => "Reversing off start",
        3 => "Big collision", 4 => "Small collision", 7 => "SC delta exceeded",
        8 => "SC illegal overtake", 9 => "SC exceeding pace", 13 => "Pit lane too fast",
        17 => "Pit lane speeding", 25 => "Corner cutting", 30 => "Lap invalidated",
        _ => null,
    };
    private static string DriverSurname(DriverData? driver, int index) =>
        driver is null || string.IsNullOrWhiteSpace(driver.Name)
            ? $"CAR {index}"
            : driver.Name.Trim().Split(' ', StringSplitOptions.RemoveEmptyEntries).Last().ToUpperInvariant();

    private Brush PrimaryBrush() => new SolidColorBrush(
        ActualTheme == ElementTheme.Dark
            ? Color.FromArgb(255, 255, 255, 255)
            : Color.FromArgb(255, 0, 0, 0));
    private Brush SecondaryBrush() => new SolidColorBrush(
        ActualTheme == ElementTheme.Dark
            ? Color.FromArgb(255, 160, 168, 184)
            : Color.FromArgb(255, 86, 91, 112));
    private Brush DividerBrush() => new SolidColorBrush(
        ActualTheme == ElementTheme.Dark
            ? Color.FromArgb(255, 42, 46, 58)
            : Color.FromArgb(255, 217, 220, 227));
    private TextBlock EmptyRailText(string text) => new()
    {
        Text = text,
        Padding = new Thickness(16, 10, 16, 10),
        FontSize = 11,
        Foreground = SecondaryBrush(),
    };
    private Brush WeatherBrush(int? weather) => new SolidColorBrush(weather switch
    {
        0 => ActualTheme == ElementTheme.Dark ? Color.FromArgb(255, 253, 224, 71) : Color.FromArgb(255, 202, 138, 4),
        1 => Color.FromArgb(255, 251, 146, 60),
        2 => Color.FromArgb(255, 148, 163, 184),
        3 => Color.FromArgb(255, 125, 211, 252),
        4 => Color.FromArgb(255, 37, 99, 235),
        5 => Color.FromArgb(255, 192, 132, 252),
        _ => Color.FromArgb(255, 148, 163, 184),
    });
    private Brush RainBrush(int percentage) =>
        percentage > 50 ? new SolidColorBrush(Color.FromArgb(255, 87, 148, 242)) :
        percentage > 20 ? new SolidColorBrush(Color.FromArgb(255, 115, 191, 105)) :
        SecondaryBrush();
    private static Color ParseColor(string? text, Color fallback)
    {
        if (text is not { Length: 7 } || text[0] != '#' ||
            !byte.TryParse(text.AsSpan(1, 2), System.Globalization.NumberStyles.HexNumber, null, out var r) ||
            !byte.TryParse(text.AsSpan(3, 2), System.Globalization.NumberStyles.HexNumber, null, out var g) ||
            !byte.TryParse(text.AsSpan(5, 2), System.Globalization.NumberStyles.HexNumber, null, out var b))
            return fallback;
        return Color.FromArgb(255, r, g, b);
    }
}
