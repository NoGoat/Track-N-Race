using System.Text.Json;
using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using Windows.Foundation;
using Windows.UI;
using XamlPath = Microsoft.UI.Xaml.Shapes.Path;

namespace TrackNRace.WinUI3;

internal sealed record TrackMapPoint(double X, double Y);

internal sealed class TrackMapTransform
{
    public double MinX { get; set; }
    public double MinZ { get; set; }
    public double Scale { get; set; }
    public double OffX { get; set; }
    public double OffZ { get; set; }
}

internal sealed class TrackMapSector
{
    public int Index { get; set; }
    public double[][] Points { get; set; } = [];
}

internal sealed class TrackMapZone
{
    public double[] Start { get; set; } = [];
    public double[] End { get; set; } = [];
}

internal sealed class TrackMapData
{
    public int TrackId { get; set; }
    public string TrackName { get; set; } = string.Empty;
    public string CircuitName { get; set; } = string.Empty;
    public double RotationDeg { get; set; }
    public TrackMapViewBox ViewBox { get; set; } = new();
    public TrackMapTransform Transform { get; set; } = new();
    public TrackMapSector[] Sectors { get; set; } = [];
    public TrackMapZone[] DrsZones { get; set; } = [];
    public TrackMapZone[] SlmDry { get; set; } = [];
    public TrackMapZone[] SlmWet { get; set; } = [];
    public double[][] SpeedTraps { get; set; } = [];
    public double[]? StartFinish { get; set; }
    public double[]? OvertakeDetectionPoint { get; set; }
    public double[]? OvertakeActivationPoint { get; set; }
}

internal sealed class TrackMapViewBox
{
    public double Width { get; set; }
    public double Height { get; set; }
}

internal sealed record TrackChoice(int TrackId, string Name)
{
    public override string ToString() => Name;
}

internal sealed record DriverChoice(int CarIndex, int RaceNumber, string Name)
{
    public override string ToString() => RaceNumber > 0 ? $"{RaceNumber} {Name}" : Name;
}

internal sealed record CarVisual(Ellipse? Dot, Border? Label);

public sealed partial class TrackMapControl : UserControl
{
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNameCaseInsensitive = true,
        PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower,
    };
    private static readonly Color[] DarkSectors =
        [Color.FromArgb(255, 232, 0, 45), Color.FromArgb(255, 0, 144, 208), Color.FromArgb(255, 255, 215, 0)];
    private static readonly Color[] LightSectors =
        [Color.FromArgb(255, 211, 47, 47), Color.FromArgb(255, 13, 71, 161), Color.FromArgb(255, 183, 149, 11)];

    private readonly Dictionary<int, TrackMapData> _maps = [];
    private readonly Dictionary<int, (double X, double Z, long MovedAt)> _lastPositions = [];
    private readonly Dictionary<int, CarVisual> _carVisuals = [];
    private readonly Dictionary<int, TrackMapPoint> _visibleCarPoints = [];
    private readonly CompositeTransform _sceneTransform = new();
    private readonly DispatcherTimer _cameraTimer = new()
    {
        Interval = TimeSpan.FromMilliseconds(16),
    };
    private SessionSnapshot? _snapshot;
    private SessionDisplaySettings _settings = new();
    private ParticipantsRowData? _lastParticipants;
    private IReadOnlyDictionary<string, string>? _lastLabels;
    private TrackMapData? _map;
    private int? _telemetryTrackId;
    private string _telemetryAeroKey = string.Empty;
    private int? _previewTrackId;
    private string? _previewAero;
    private int? _followCarIndex;
    private double _zoom = 4;
    private double _minX;
    private double _minY;
    private double _maxX;
    private double _maxY;
    private bool _updatingControls;
    private bool _isFullscreen;
    private string _lastAeroKey = string.Empty;
    private double _aeroWorldOffset;
    private double _aeroWorldThickness;
    private double _targetScale;
    private double _targetTranslateX;
    private double _targetTranslateY;

    public event Action? FullscreenRequested;

    public TrackMapControl()
    {
        InitializeComponent();
        SceneCanvas.RenderTransform = _sceneTransform;
        _cameraTimer.Tick += OnCameraTick;
        LoadMaps();
#if DEBUG
        DebugControls.Visibility = Visibility.Visible;
#endif
        ActualThemeChanged += (_, _) =>
        {
            CarsCanvas.Children.Clear();
            _carVisuals.Clear();
            RebuildStaticGeometry();
            if (_snapshot is not null)
            {
                UpdateCars(_snapshot);
            }
        };
    }

    internal void Apply(SessionSnapshot snapshot, SessionDisplaySettings settings)
    {
        var staticSettingsChanged =
            settings.SectorColors != _settings.SectorColors ||
            settings.MapDimmed != _settings.MapDimmed;
        var markerSettingsChanged =
            settings.DriverDisplayMode != _settings.DriverDisplayMode;
        _snapshot = snapshot;
        _settings = settings;
        if (_telemetryTrackId != snapshot.Session?.TrackId)
        {
            _telemetryTrackId = snapshot.Session?.TrackId;
            _previewTrackId = null;
        }
        var telemetryAeroKey = $"{snapshot.AeroMode}/{snapshot.Session?.ActiveAeroTrackStatus ?? -1}";
        if (_telemetryAeroKey != telemetryAeroKey)
        {
            _telemetryAeroKey = telemetryAeroKey;
            _previewAero = null;
        }
        var nextTrackId = _previewTrackId ?? snapshot.Session?.TrackId;
        SyncDebugSelections(nextTrackId, snapshot);
        var nextMap = nextTrackId is int id && _maps.TryGetValue(id, out var found)
            ? found
            : null;
        if (!ReferenceEquals(nextMap, _map) || staticSettingsChanged)
        {
            _map = nextMap;
            RebuildStaticGeometry();
        }
        else
        {
            UpdateAeroGeometry();
        }

        if (!ReferenceEquals(_lastParticipants, snapshot.Participants))
        {
            _lastParticipants = snapshot.Participants;
            UpdateDriverChoices(snapshot);
            markerSettingsChanged = true;
        }
        if (!ReferenceEquals(_lastLabels, snapshot.Labels))
        {
            _lastLabels = snapshot.Labels;
            UpdateDebugTrackChoices(snapshot.Labels);
        }
        if (markerSettingsChanged)
        {
            CarsCanvas.Children.Clear();
            _carVisuals.Clear();
        }
        UpdateSceneTransform();
        UpdateCars(snapshot);
    }

    public void SetFullscreenState(bool fullscreen)
    {
        _isFullscreen = fullscreen;
        FullscreenIcon.Glyph = fullscreen ? "\uE73F" : "\uE740";
        ToolTipService.SetToolTip(
            FullscreenButton,
            fullscreen ? "Exit fullscreen map" : "Fullscreen map");
    }

    internal (string TrackName, string CircuitName) TrackDetails(
        int trackId,
        IReadOnlyDictionary<string, string> labels)
    {
        if (!_maps.TryGetValue(trackId, out var map))
        {
            return ($"Track {trackId}", string.Empty);
        }
        var trackKey = $"track.{trackId}.track_name";
        var circuitKey = $"track.{trackId}.circuit_name";
        return (
            labels.TryGetValue(trackKey, out var trackName) ? trackName : map.TrackName,
            labels.TryGetValue(circuitKey, out var circuitName) ? circuitName : map.CircuitName);
    }

    private void LoadMaps()
    {
        var directory = System.IO.Path.Combine(AppContext.BaseDirectory, "Assets", "Maps");
        if (!Directory.Exists(directory))
        {
            return;
        }
        foreach (var path in Directory.EnumerateFiles(directory, "track_*.json"))
        {
            try
            {
                var map = JsonSerializer.Deserialize<TrackMapData>(
                    File.ReadAllText(path), JsonOptions);
                if (map is not null)
                {
                    _maps[map.TrackId] = map;
                }
            }
            catch (JsonException)
            {
            }
        }

#if DEBUG
        UpdateDebugTrackChoices(new Dictionary<string, string>());
#endif
    }

    private void RebuildStaticGeometry()
    {
        StaticCanvas.Children.Clear();
        if (_map is null)
        {
            NoMapText.Visibility = Visibility.Visible;
            return;
        }
        NoMapText.Visibility = Visibility.Collapsed;
        _lastAeroKey = string.Empty;
        _aeroWorldOffset = 0;
        _aeroWorldThickness = 0;

        var allPoints = _map.Sectors
            .SelectMany(value => value.Points)
            .Where(value => value.Length >= 2)
            .Select(value => Rotate(value[0], value[1]))
            .ToArray();
        if (allPoints.Length == 0)
        {
            NoMapText.Visibility = Visibility.Visible;
            return;
        }
        _minX = allPoints.Min(value => value.X);
        _maxX = allPoints.Max(value => value.X);
        _minY = allPoints.Min(value => value.Y);
        _maxY = allPoints.Max(value => value.Y);

        var dark = ActualTheme == ElementTheme.Dark;
        var sectorColors = dark ? DarkSectors : LightSectors;
        var baseColor = dark
            ? Color.FromArgb(255, 255, 255, 255)
            : Color.FromArgb(255, 0, 0, 0);
        foreach (var sector in _map.Sectors)
        {
            var color = _settings.SectorColors
                ? sectorColors[Math.Clamp(sector.Index - 1, 0, 2)]
                : baseColor;
            StaticCanvas.Children.Add(CreatePath(
                sector.Points.Select(value => Rotate(value[0], value[1])),
                color,
                5,
                _settings.MapDimmed ? 0.4 : 1));
        }

        AddTrackMarkers(baseColor, dark);
        UpdateSceneTransform();
    }

    private void AddTrackMarkers(Color baseColor, bool dark)
    {
        if (_map is null) return;
        foreach (var trap in _map.SpeedTraps.Where(value => value.Length >= 2))
        {
            AddCircle(StaticCanvas, Rotate(trap[0], trap[1]), 7,
                dark ? Color.FromArgb(255, 136, 129, 222) : Color.FromArgb(255, 91, 84, 180));
        }
        if (_map.StartFinish is { Length: >= 2 } finish)
        {
            var point = Rotate(finish[0], finish[1]);
            var sector = _map.Sectors.OrderBy(value => value.Index).FirstOrDefault();
            var sectorPoints = sector?.Points
                .Where(value => value.Length >= 2)
                .Select(value => Rotate(value[0], value[1]))
                .ToArray() ?? [];
            var normal = sectorPoints.Length > 1
                ? Perpendicular(sectorPoints, Nearest(sectorPoints, point))
                : new TrackMapPoint(1, 0);
            const double halfLength = 14;
            StaticCanvas.Children.Add(new Line
            {
                X1 = point.X - normal.X * halfLength,
                X2 = point.X + normal.X * halfLength,
                Y1 = point.Y - normal.Y * halfLength,
                Y2 = point.Y + normal.Y * halfLength,
                Stroke = new SolidColorBrush(
                    _settings.SectorColors
                        ? baseColor
                        : Color.FromArgb(255, 232, 0, 45)),
                StrokeThickness = 2.5,
            });
        }
        var colors = dark ? DarkSectors : LightSectors;
        foreach (var sector in _map.Sectors.OrderBy(value => value.Index).Take(2))
        {
            var points = sector.Points
                .Where(value => value.Length >= 2)
                .Select(value => Rotate(value[0], value[1]))
                .ToArray();
            if (points.Length < 2) continue;
            var point = points[^1];
            var normal = Perpendicular(points, points.Length - 1);
            const double halfLength = 10;
            StaticCanvas.Children.Add(new Line
            {
                X1 = point.X - normal.X * halfLength,
                X2 = point.X + normal.X * halfLength,
                Y1 = point.Y - normal.Y * halfLength,
                Y2 = point.Y + normal.Y * halfLength,
                Stroke = new SolidColorBrush(
                    _settings.SectorColors
                        ? baseColor
                        : colors[Math.Clamp(sector.Index, 0, 2)]),
                StrokeThickness = 3,
            });
        }
    }

    private void UpdateAeroGeometry(bool force = false)
    {
        if (_map is null) return;
        var aero = _previewAero ??
            (_snapshot?.AeroMode == "slm"
                ? (_snapshot.Session?.ActiveAeroTrackStatus == 1 ? "SLM-Wet" : "SLM-Dry")
                : "DRS");
        var aeroKey = $"{_map.TrackId}/{aero}";
        if (!force && aeroKey == _lastAeroKey)
        {
            return;
        }
        _lastAeroKey = aeroKey;
        for (var index = StaticCanvas.Children.Count - 1; index >= 0; index--)
        {
            if (StaticCanvas.Children[index] is FrameworkElement element &&
                Equals(element.Tag, "aero"))
            {
                StaticCanvas.Children.RemoveAt(index);
            }
        }

        var zones = aero switch
        {
            "SLM-Dry" => _map.SlmDry,
            "SLM-Wet" => _map.SlmWet,
            _ => _map.DrsZones,
        };
        var color = aero switch
        {
            "SLM-Dry" => Color.FromArgb(255, 70, 227, 150),
            "SLM-Wet" => Color.FromArgb(255, 34, 211, 238),
            _ => Color.FromArgb(255, 57, 181, 74),
        };
        var rawCenterline = _map.Sectors
            .OrderBy(value => value.Index)
            .SelectMany(value => value.Points)
            .Where(value => value.Length >= 2)
            .ToArray();
        var centerline = rawCenterline
            .Select(value => Rotate(value[0], value[1]))
            .ToArray();
        foreach (var zone in zones)
        {
            var rawPoints = SmoothSeam(SliceZone(rawCenterline, zone));
            var points = rawPoints
                .Select(value => Rotate(value[0], value[1]))
                .ToArray();
            var outwardSign = FindOutwardSign(points, centerline);
            var offsetPoints = OffsetPolyline(points, _aeroWorldOffset, outwardSign);
            var path = CreatePath(
                offsetPoints,
                color,
                _aeroWorldThickness,
                1,
                dashed: true);
            path.Tag = "aero";
            StaticCanvas.Children.Add(path);
        }
        if (aero.StartsWith("SLM", StringComparison.Ordinal))
        {
            var markerColor = ActualTheme == ElementTheme.Dark
                ? Color.FromArgb(255, 149, 231, 240)
                : Color.FromArgb(255, 57, 127, 136);
            if (_map.OvertakeDetectionPoint is { Length: >= 2 } detection)
            {
                var marker = AddCircle(
                    StaticCanvas, Rotate(detection[0], detection[1]), 6, markerColor);
                marker.Tag = "aero";
            }
            if (_map.OvertakeActivationPoint is { Length: >= 2 } activation)
            {
                var point = Rotate(activation[0], activation[1]);
                var normal = centerline.Length > 1
                    ? Perpendicular(centerline, Nearest(centerline, point))
                    : new TrackMapPoint(1, 0);
                var halfLength = Math.Max(10, _aeroWorldOffset * 0.55);
                StaticCanvas.Children.Add(new Line
                {
                    Tag = "aero",
                    X1 = point.X - normal.X * halfLength,
                    X2 = point.X + normal.X * halfLength,
                    Y1 = point.Y - normal.Y * halfLength,
                    Y2 = point.Y + normal.Y * halfLength,
                    Stroke = new SolidColorBrush(markerColor),
                    StrokeThickness = _aeroWorldThickness / 2,
                });
            }
        }
    }

    private void UpdateCars(SessionSnapshot snapshot)
    {
        if (_map is null || snapshot.Positions is null) return;
        _visibleCarPoints.Clear();
        foreach (var visual in _carVisuals.Values)
        {
            if (visual.Dot is not null) visual.Dot.Visibility = Visibility.Collapsed;
            if (visual.Label is not null) visual.Label.Visibility = Visibility.Collapsed;
        }
        var now = Environment.TickCount64;
        var timeoutMs = _settings.StaleCarTimeoutSeconds * 1000L;
        foreach (var car in snapshot.Positions.Cars)
        {
            if (car.X == 0 && car.Z == 0) continue;
            if (_lastPositions.TryGetValue(car.Idx, out var previous))
            {
                var moved = previous.X != car.X || previous.Z != car.Z;
                _lastPositions[car.Idx] = moved
                    ? (car.X, car.Z, now)
                    : previous;
                if (car.Idx != snapshot.Positions.PlayerIdx &&
                    timeoutMs > 0 && now - _lastPositions[car.Idx].MovedAt > timeoutMs)
                {
                    continue;
                }
            }
            else
            {
                _lastPositions[car.Idx] = (car.X, car.Z, now);
            }

            var point = RotateRawPosition(car.X, car.Z);
            _visibleCarPoints[car.Idx] = point;
            var driver = snapshot.Participants?.Drivers.FirstOrDefault(value => value.Idx == car.Idx);
            if (!_carVisuals.TryGetValue(car.Idx, out var visual))
            {
                var livery = ParseColor(driver?.LiveryColor, Color.FromArgb(255, 142, 142, 142));
                Ellipse? dot = null;
                Border? label = null;
                if (_settings.DriverDisplayMode is TrackDriverDisplayMode.Dots or
                    TrackDriverDisplayMode.DotsAndLabels)
                {
                    dot = new Ellipse
                    {
                        Width = 14,
                        Height = 14,
                        Fill = new SolidColorBrush(livery),
                    };
                    CarsCanvas.Children.Add(dot);
                }
                if (_settings.DriverDisplayMode is TrackDriverDisplayMode.Labels or
                    TrackDriverDisplayMode.DotsAndLabels)
                {
                    label = new Border
                    {
                        Background = new SolidColorBrush(
                            ActualTheme == ElementTheme.Dark
                                ? Color.FromArgb(230, 18, 22, 35)
                                : Color.FromArgb(235, 250, 250, 252)),
                        BorderBrush = new SolidColorBrush(livery),
                        BorderThickness = new Thickness(3, 0, 0, 0),
                        CornerRadius = new CornerRadius(3),
                        Padding = new Thickness(4, 1, 4, 1),
                        Child = new TextBlock
                        {
                            Text = DriverCode(driver, car.Idx),
                            FontSize = 10,
                            FontWeight = Microsoft.UI.Text.FontWeights.Bold,
                        },
                    };
                    CarsCanvas.Children.Add(label);
                }
                visual = new CarVisual(dot, label);
                _carVisuals[car.Idx] = visual;
            }
            if (visual.Dot is not null)
            {
                visual.Dot.Visibility = Visibility.Visible;
            }
            if (visual.Label is not null)
            {
                visual.Label.Visibility = Visibility.Visible;
            }
        }
        PositionCarVisuals();
    }

    private void UpdateDriverChoices(SessionSnapshot snapshot)
    {
        var current = _followCarIndex;
        var choices = (snapshot.Participants?.Drivers ?? [])
            .Where(value => value.Name.Trim().Length > 0 || value.RaceNumber > 0)
            .OrderBy(value => value.RaceNumber)
            .Select(value => new DriverChoice(value.Idx, value.RaceNumber,
                value.Name.Trim().Split(' ', StringSplitOptions.RemoveEmptyEntries).LastOrDefault()?.ToUpperInvariant() ?? $"C{value.Idx}"))
            .ToArray();
        _updatingControls = true;
        FollowDriverComboBox.ItemsSource = choices;
        FollowDriverComboBox.SelectedItem = choices.FirstOrDefault(value => value.CarIndex == current);
        _updatingControls = false;
    }

    private void UpdateSceneTransform()
    {
        if (_map is null || ActualWidth <= 0 || ActualHeight <= 0) return;
        const double padding = 32;
        var width = Math.Max(1, _maxX - _minX);
        var height = Math.Max(1, _maxY - _minY);
        var baseScale = Math.Max(0.01, Math.Min(
            (ActualWidth - padding * 2) / width,
            (ActualHeight - padding * 2) / height));
        var scale = baseScale;
        var centerX = (_minX + _maxX) / 2;
        var centerY = (_minY + _maxY) / 2;
        var following = TryGetFollowPoint(out var followPoint);
        if (following)
        {
            centerX = followPoint.X;
            centerY = followPoint.Y;
            scale *= _zoom;
        }
        var effectiveZoom = Math.Max(1, scale / baseScale);
        var zoomFactor = Math.Sqrt(effectiveZoom);
        var trackZoomFactor = Math.Pow(effectiveZoom, 0.8);
        var desiredOffsetPixels = Math.Max(
            18 * zoomFactor,
            (5 * trackZoomFactor) / 2 + 8 * zoomFactor);
        var nextWorldOffset = desiredOffsetPixels / scale;
        var nextWorldThickness = 6 * zoomFactor / scale;
        if (Math.Abs(nextWorldOffset - _aeroWorldOffset) > 0.001 ||
            Math.Abs(nextWorldThickness - _aeroWorldThickness) > 0.001)
        {
            _aeroWorldOffset = nextWorldOffset;
            _aeroWorldThickness = nextWorldThickness;
            UpdateAeroGeometry(force: true);
        }
        var translateX = ActualWidth / 2 - centerX * scale;
        var translateY = ActualHeight / 2 - centerY * scale;
        _targetScale = scale;
        _targetTranslateX = translateX;
        _targetTranslateY = translateY;
        if (_settings.ReduceAnimations || _sceneTransform.ScaleX <= 0)
        {
            _sceneTransform.ScaleX = scale;
            _sceneTransform.ScaleY = scale;
            _sceneTransform.TranslateX = translateX;
            _sceneTransform.TranslateY = translateY;
            _cameraTimer.Stop();
            PositionCarVisuals();
        }
        else
        {
            if (following)
            {
                // Match Electron: ease only the zoom and derive pan from the
                // latest followed-car point at the current scale. Easing pan
                // independently makes the camera trail an aging telemetry
                // point and greatly amplifies position steps at 8x/16x.
                CenterCameraOn(followPoint);
                PositionCarVisuals();
            }
            _cameraTimer.Start();
        }
    }

    private bool TryGetFollowPoint(out TrackMapPoint point)
    {
        if (_followCarIndex is int followed &&
            _snapshot?.Positions?.Cars.FirstOrDefault(value => value.Idx == followed) is { } car &&
            (car.X != 0 || car.Z != 0))
        {
            point = RotateRawPosition(car.X, car.Z);
            return true;
        }
        point = new TrackMapPoint(0, 0);
        return false;
    }

    private void CenterCameraOn(TrackMapPoint point)
    {
        _sceneTransform.TranslateX = ActualWidth / 2 - point.X * _sceneTransform.ScaleX;
        _sceneTransform.TranslateY = ActualHeight / 2 - point.Y * _sceneTransform.ScaleY;
    }

    private TrackMapPoint RotateRawPosition(double x, double z)
    {
        if (_map is null) return new TrackMapPoint(0, 0);
        var viewX = (x - _map.Transform.MinX) * _map.Transform.Scale + _map.Transform.OffX;
        var viewY = (z - _map.Transform.MinZ) * _map.Transform.Scale + _map.Transform.OffZ;
        return Rotate(viewX, viewY);
    }

    private TrackMapPoint Rotate(double x, double y)
    {
        if (_map is null || Math.Abs(_map.RotationDeg) < 0.001)
        {
            return new TrackMapPoint(x, y);
        }
        var radians = _map.RotationDeg * Math.PI / 180;
        var cos = Math.Cos(radians);
        var sin = Math.Sin(radians);
        var centerX = _map.ViewBox.Width / 2;
        var centerY = _map.ViewBox.Height / 2;
        var dx = x - centerX;
        var dy = y - centerY;
        return new TrackMapPoint(
            cos * dx - sin * dy + centerX,
            sin * dx + cos * dy + centerY);
    }

    private static XamlPath CreatePath(
        IEnumerable<TrackMapPoint> points,
        Color color,
        double thickness,
        double opacity,
        bool dashed = false)
    {
        var values = points.ToArray();
        var figure = new PathFigure { IsClosed = false, IsFilled = false };
        if (values.Length > 0)
        {
            figure.StartPoint = new Point(values[0].X, values[0].Y);
            for (var index = 1; index < values.Length; index++)
            {
                figure.Segments.Add(new LineSegment
                {
                    Point = new Point(values[index].X, values[index].Y),
                });
            }
        }
        var path = new XamlPath
        {
            Data = new PathGeometry { Figures = { figure } },
            Stroke = new SolidColorBrush(color),
            StrokeThickness = thickness,
            StrokeLineJoin = PenLineJoin.Round,
            StrokeStartLineCap = PenLineCap.Round,
            StrokeEndLineCap = PenLineCap.Round,
            Opacity = opacity,
        };
        if (dashed)
        {
            path.StrokeStartLineCap = PenLineCap.Flat;
            path.StrokeEndLineCap = PenLineCap.Flat;
            path.StrokeLineJoin = PenLineJoin.Miter;
            path.StrokeDashArray = new DoubleCollection { 0.5, 0.5 };
        }
        return path;
    }

    private static Ellipse AddCircle(Canvas canvas, TrackMapPoint point, double radius, Color color)
    {
        var ellipse = new Ellipse
        {
            Width = radius * 2,
            Height = radius * 2,
            Fill = new SolidColorBrush(color),
        };
        Canvas.SetLeft(ellipse, point.X - radius);
        Canvas.SetTop(ellipse, point.Y - radius);
        canvas.Children.Add(ellipse);
        return ellipse;
    }

    private static double[][] SliceZone(double[][] centerline, TrackMapZone zone)
    {
        if (centerline.Length == 0 || zone.Start.Length < 2 || zone.End.Length < 2)
        {
            return [];
        }
        var start = Nearest(centerline, zone.Start);
        var end = Nearest(centerline, zone.End);
        var result = new List<double[]>();
        for (var index = start; ; index = (index + 1) % centerline.Length)
        {
            result.Add(centerline[index]);
            if (index == end || result.Count > centerline.Length) break;
        }
        return result.ToArray();
    }

    private static int Nearest(double[][] points, double[] target)
    {
        var best = 0;
        var distance = double.MaxValue;
        for (var index = 0; index < points.Length; index++)
        {
            var dx = points[index][0] - target[0];
            var dy = points[index][1] - target[1];
            var next = dx * dx + dy * dy;
            if (next < distance)
            {
                best = index;
                distance = next;
            }
        }
        return best;
    }

    private static int Nearest(
        IReadOnlyList<TrackMapPoint> points,
        TrackMapPoint target)
    {
        var best = 0;
        var distance = double.MaxValue;
        for (var index = 0; index < points.Count; index++)
        {
            var dx = points[index].X - target.X;
            var dy = points[index].Y - target.Y;
            var next = dx * dx + dy * dy;
            if (next < distance)
            {
                best = index;
                distance = next;
            }
        }
        return best;
    }

    private static TrackMapPoint Perpendicular(
        IReadOnlyList<TrackMapPoint> points,
        int index)
    {
        var low = Math.Max(0, index - 1);
        var high = Math.Min(points.Count - 1, index + 1);
        var dx = points[high].X - points[low].X;
        var dy = points[high].Y - points[low].Y;
        var length = Math.Sqrt(dx * dx + dy * dy);
        if (length <= 0.000001) length = 1;
        return new TrackMapPoint(-dy / length, dx / length);
    }

    private static TrackMapPoint[] OffsetPolyline(
        IReadOnlyList<TrackMapPoint> points,
        double distance,
        int outwardSign)
    {
        var result = new TrackMapPoint[points.Count];
        for (var index = 0; index < points.Count; index++)
        {
            var normal = Perpendicular(points, index);
            result[index] = new TrackMapPoint(
                points[index].X + normal.X * distance * outwardSign,
                points[index].Y + normal.Y * distance * outwardSign);
        }
        return result;
    }

    private static int FindOutwardSign(
        IReadOnlyList<TrackMapPoint> zone,
        IReadOnlyList<TrackMapPoint> centerline)
    {
        const double probeDistance = 2;
        const int maxProbes = 128;
        var step = Math.Max(1, zone.Count / maxProbes);
        var positiveVotes = 0;
        var negativeVotes = 0;
        for (var index = 0; index < zone.Count; index += step)
        {
            var normal = Perpendicular(zone, index);
            var point = zone[index];
            var positiveInside = PointInPolygon(
                new TrackMapPoint(
                    point.X + normal.X * probeDistance,
                    point.Y + normal.Y * probeDistance),
                centerline);
            var negativeInside = PointInPolygon(
                new TrackMapPoint(
                    point.X - normal.X * probeDistance,
                    point.Y - normal.Y * probeDistance),
                centerline);
            if (positiveInside == negativeInside) continue;
            if (positiveInside) negativeVotes++;
            else positiveVotes++;
        }
        if (positiveVotes != negativeVotes)
        {
            return positiveVotes > negativeVotes ? 1 : -1;
        }

        double twiceArea = 0;
        for (int index = 0, previous = centerline.Count - 1;
             index < centerline.Count;
             previous = index++)
        {
            twiceArea +=
                centerline[previous].X * centerline[index].Y -
                centerline[index].X * centerline[previous].Y;
        }
        return twiceArea >= 0 ? -1 : 1;
    }

    private static bool PointInPolygon(
        TrackMapPoint point,
        IReadOnlyList<TrackMapPoint> polygon)
    {
        var inside = false;
        for (int index = 0, previous = polygon.Count - 1;
             index < polygon.Count;
             previous = index++)
        {
            var current = polygon[index];
            var prior = polygon[previous];
            if ((current.Y > point.Y) != (prior.Y > point.Y) &&
                point.X <
                ((prior.X - current.X) * (point.Y - current.Y)) /
                (prior.Y - current.Y) + current.X)
            {
                inside = !inside;
            }
        }
        return inside;
    }

    private static double[][] SmoothSeam(double[][] points)
    {
        const double seamAngleDegrees = 6;
        const int smoothSpan = 6;
        const int passes = 20;
        if (points.Length < 5) return points;
        var weights = new double[points.Length];
        var any = false;
        for (var index = 1; index < points.Length - 1; index++)
        {
            if (TurnAngleDegrees(
                points[index - 1], points[index], points[index + 1]) <=
                seamAngleDegrees)
            {
                continue;
            }
            for (var nearby = index - smoothSpan;
                 nearby <= index + smoothSpan;
                 nearby++)
            {
                if (nearby <= 0 || nearby >= points.Length - 1) continue;
                var taper = 0.5 * (1 + Math.Cos(
                    Math.PI * Math.Abs(nearby - index) / (smoothSpan + 1)));
                if (taper > weights[nearby])
                {
                    weights[nearby] = taper;
                    any = true;
                }
            }
        }
        if (!any) return points;

        var output = points.Select(value => new[] { value[0], value[1] }).ToArray();
        for (var pass = 0; pass < passes; pass++)
        {
            var next = output.Select(value => new[] { value[0], value[1] }).ToArray();
            for (var index = 1; index < output.Length - 1; index++)
            {
                var weight = weights[index];
                if (weight <= 0) continue;
                var midpointX = (output[index - 1][0] + output[index + 1][0]) / 2;
                var midpointY = (output[index - 1][1] + output[index + 1][1]) / 2;
                next[index][0] =
                    output[index][0] + weight * (midpointX - output[index][0]);
                next[index][1] =
                    output[index][1] + weight * (midpointY - output[index][1]);
            }
            output = next;
        }
        return output;
    }

    private static double TurnAngleDegrees(
        double[] first,
        double[] middle,
        double[] last)
    {
        var firstX = middle[0] - first[0];
        var firstY = middle[1] - first[1];
        var secondX = last[0] - middle[0];
        var secondY = last[1] - middle[1];
        var firstLength = Math.Sqrt(firstX * firstX + firstY * firstY);
        var secondLength = Math.Sqrt(secondX * secondX + secondY * secondY);
        if (firstLength <= 0.000001) firstLength = 1;
        if (secondLength <= 0.000001) secondLength = 1;
        var dot = (firstX * secondX + firstY * secondY) /
            (firstLength * secondLength);
        return Math.Acos(Math.Clamp(dot, -1, 1)) * 180 / Math.PI;
    }

    private static Color ParseColor(string? text, Color fallback)
    {
        if (text is not { Length: 7 } || text[0] != '#' ||
            !byte.TryParse(text.AsSpan(1, 2), System.Globalization.NumberStyles.HexNumber, null, out var r) ||
            !byte.TryParse(text.AsSpan(3, 2), System.Globalization.NumberStyles.HexNumber, null, out var g) ||
            !byte.TryParse(text.AsSpan(5, 2), System.Globalization.NumberStyles.HexNumber, null, out var b))
        {
            return fallback;
        }
        return Color.FromArgb(255, r, g, b);
    }

    private static string DriverCode(DriverData? driver, int carIndex)
    {
        if (driver is null || string.IsNullOrWhiteSpace(driver.Name)) return $"C{carIndex}";
        var part = driver.Name.Trim().Split(' ', StringSplitOptions.RemoveEmptyEntries).Last();
        return part[..Math.Min(3, part.Length)].ToUpperInvariant();
    }

    private void OnMapSizeChanged(object sender, SizeChangedEventArgs args)
    {
        // A transformed Canvas is not clipped to its layout slot by default.
        // Keep every map layer and driver marker inside the map viewport so
        // follow/zoom cannot paint across the Session header, rail, or weather.
        MapRoot.Clip = new RectangleGeometry
        {
            Rect = new Rect(0, 0, args.NewSize.Width, args.NewSize.Height),
        };
        UpdateSceneTransform();
    }

    private void OnCameraTick(object? sender, object args)
    {
        const double easing = 0.12;
        _sceneTransform.ScaleX += (_targetScale - _sceneTransform.ScaleX) * easing;
        _sceneTransform.ScaleY = _sceneTransform.ScaleX;
        var following = TryGetFollowPoint(out var followPoint);
        if (following)
        {
            CenterCameraOn(followPoint);
        }
        else
        {
            _sceneTransform.TranslateX +=
                (_targetTranslateX - _sceneTransform.TranslateX) * easing;
            _sceneTransform.TranslateY +=
                (_targetTranslateY - _sceneTransform.TranslateY) * easing;
        }
        var difference = Math.Abs(_targetScale - _sceneTransform.ScaleX);
        if (!following)
        {
            difference +=
                Math.Abs(_targetTranslateX - _sceneTransform.TranslateX) +
                Math.Abs(_targetTranslateY - _sceneTransform.TranslateY);
        }
        if (difference < 0.1)
        {
            _sceneTransform.ScaleX = _targetScale;
            _sceneTransform.ScaleY = _targetScale;
            if (following)
            {
                CenterCameraOn(followPoint);
            }
            else
            {
                _sceneTransform.TranslateX = _targetTranslateX;
                _sceneTransform.TranslateY = _targetTranslateY;
            }
            _cameraTimer.Stop();
        }
        PositionCarVisuals();
    }

    private void PositionCarVisuals()
    {
        var scale = _sceneTransform.ScaleX;
        if (scale <= 0) return;
        foreach (var pair in _visibleCarPoints)
        {
            if (!_carVisuals.TryGetValue(pair.Key, out var visual)) continue;
            var screenX = pair.Value.X * scale + _sceneTransform.TranslateX;
            var screenY = pair.Value.Y * scale + _sceneTransform.TranslateY;
            if (visual.Dot is not null)
            {
                Canvas.SetLeft(visual.Dot, screenX - 7);
                Canvas.SetTop(visual.Dot, screenY - 7);
            }
            if (visual.Label is not null)
            {
                Canvas.SetLeft(visual.Label, screenX - 19);
                Canvas.SetTop(
                    visual.Label,
                    screenY -
                    (_settings.DriverDisplayMode == TrackDriverDisplayMode.Labels ? 8 : 28));
            }
        }
    }

    private void OnFollowDriverChanged(object sender, SelectionChangedEventArgs args)
    {
        if (_updatingControls) return;
        _followCarIndex = FollowDriverComboBox.SelectedItem is DriverChoice choice
            ? choice.CarIndex
            : null;
        ZoomComboBox.Visibility = _followCarIndex is null ? Visibility.Collapsed : Visibility.Visible;
        ClearFollowButton.Visibility =
            _followCarIndex is null ? Visibility.Collapsed : Visibility.Visible;
        if (ZoomComboBox.SelectedIndex < 0) ZoomComboBox.SelectedIndex = 1;
        UpdateSceneTransform();
    }

    private void OnClearFollowClicked(object sender, RoutedEventArgs args)
    {
        FollowDriverComboBox.SelectedItem = null;
        FollowDriverComboBox.Text = string.Empty;
    }

    private void OnZoomChanged(object sender, SelectionChangedEventArgs args)
    {
        _zoom = ZoomComboBox.SelectedIndex switch { 0 => 2, 2 => 8, 3 => 16, _ => 4 };
        UpdateSceneTransform();
    }

    private void OnFullscreenClicked(object sender, RoutedEventArgs args) => FullscreenRequested?.Invoke();

    private void OnDebugTrackChanged(object sender, SelectionChangedEventArgs args)
    {
#if DEBUG
        if (_updatingControls) return;
        _previewTrackId = DebugTrackComboBox.SelectedItem is TrackChoice choice
            ? choice.TrackId
            : null;
        if (_snapshot is not null) Apply(_snapshot, _settings);
#endif
    }

    private void OnDebugAeroChanged(object sender, SelectionChangedEventArgs args)
    {
#if DEBUG
        if (_updatingControls) return;
        _previewAero = DebugAeroComboBox.SelectedItem as string;
        UpdateAeroGeometry();
#endif
    }

    private void UpdateDebugTrackChoices(IReadOnlyDictionary<string, string> labels)
    {
#if DEBUG
        var selected = _previewTrackId;
        var choices = _maps.Values
            .Select(value => new TrackChoice(
                value.TrackId,
                labels.TryGetValue($"track.{value.TrackId}.track_name", out var label)
                    ? label
                    : value.TrackName))
            .OrderBy(value => value.Name)
            .ToArray();
        _updatingControls = true;
        DebugTrackComboBox.ItemsSource = choices;
        DebugTrackComboBox.SelectedItem =
            choices.FirstOrDefault(value => value.TrackId == selected);
        var currentAero = _previewAero ??
            (_snapshot?.AeroMode == "slm"
                ? (_snapshot.Session?.ActiveAeroTrackStatus == 1 ? "SLM-Wet" : "SLM-Dry")
                : "DRS");
        DebugAeroComboBox.SelectedItem = currentAero;
        _updatingControls = false;
#endif
    }

    private void SyncDebugSelections(int? trackId, SessionSnapshot snapshot)
    {
#if DEBUG
        _updatingControls = true;
        DebugTrackComboBox.SelectedItem =
            (DebugTrackComboBox.ItemsSource as IEnumerable<TrackChoice>)?
                .FirstOrDefault(value => value.TrackId == trackId);
        var aero = _previewAero ??
            (snapshot.AeroMode == "slm"
                ? (snapshot.Session?.ActiveAeroTrackStatus == 1 ? "SLM-Wet" : "SLM-Dry")
                : "DRS");
        DebugAeroComboBox.SelectedItem = aero;
        _updatingControls = false;
#endif
    }
}
