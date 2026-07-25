using Microsoft.UI.Input;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Windows.Foundation;
using Windows.UI;
using XamlBrush = Microsoft.UI.Xaml.Media.Brush;

namespace TrackNRace.Charting;

public enum ChartCoordinateSpace
{
    Plot,
    Pixels,
}

public abstract record ChartOverlayCommand;

public sealed record ChartOverlayLine(
    Point Start,
    Point End,
    Color Color,
    double Thickness = 1,
    ChartCoordinateSpace CoordinateSpace = ChartCoordinateSpace.Plot,
    string? YAxisKey = null,
    IReadOnlyList<double>? DashPattern = null)
    : ChartOverlayCommand;

public sealed record ChartOverlayText(
    Point Position,
    string Text,
    Color Color,
    double FontSize = 12,
    ChartCoordinateSpace CoordinateSpace = ChartCoordinateSpace.Pixels,
    string? YAxisKey = null)
    : ChartOverlayCommand;

public sealed record ChartTooltipEntry(
    string Label,
    string Value,
    Color Color,
    string? Group = null);

public sealed record ChartTooltipContent(
    string Header,
    IReadOnlyList<ChartTooltipEntry> Entries);

public sealed record ChartTooltipMarker(
    double X,
    double Y,
    string YAxisKey,
    Color Color);

public sealed record ChartTooltipData(
    double X,
    ChartTooltipContent Content,
    IReadOnlyList<ChartTooltipMarker>? Markers = null);

public sealed record ChartOverlayMarker(
    Point Position,
    Color Color,
    double Radius = 3,
    double StrokeThickness = 1.5,
    ChartCoordinateSpace CoordinateSpace = ChartCoordinateSpace.Plot,
    string? YAxisKey = null)
    : ChartOverlayCommand;

public sealed record ChartOverlayTooltip(
    Point Position,
    ChartTooltipContent Content,
    Color Background,
    Color Border,
    Color Foreground,
    Color SecondaryForeground,
    string? BackgroundResourceKey = null)
    : ChartOverlayCommand;

public sealed class ChartOverlayBuilder
{
    private readonly List<ChartOverlayCommand> _commands = [];
    public IReadOnlyList<ChartOverlayCommand> Commands => _commands;
    public void Add(ChartOverlayCommand command) =>
        _commands.Add(command ?? throw new ArgumentNullException(nameof(command)));
}

public sealed record ChartLayoutContext(
    Rect PlotBounds,
    double RasterizationScale);

public sealed record ChartPointerEvent(
    PointerPoint Pointer,
    Point PlotPosition,
    bool IsExit);

public sealed class ChartPluginContext
{
    private readonly GpuChart _chart;
    internal ChartPluginContext(GpuChart chart) => _chart = chart;
    public GpuChart Chart => _chart;
    public void Invalidate() => _chart.Invalidate();
    public void InvalidateOverlay() => _chart.InvalidateOverlay();
}

public interface IChartPlugin
{
    void Attach(ChartPluginContext context) { }
    void Detach() { }
    void OnLayout(ChartLayoutContext context) { }
    void OnPointer(ChartPointerEvent pointerEvent) { }
    void BuildOverlay(ChartOverlayBuilder builder) { }
}

public sealed class ChartBackgroundPlugin : IChartPlugin
{
    private ChartPluginContext? _context;
    private XamlBrush? _brushOverride;
    private string _resourceKey = "NavigationViewContentBackground";
    private Color _originalBackground;
    private bool _hasOriginalBackground;

    public XamlBrush? BrushOverride
    {
        get => _brushOverride;
        set
        {
            if (ReferenceEquals(_brushOverride, value)) return;
            _brushOverride = value;
            Apply();
        }
    }

    public string ResourceKey
    {
        get => _resourceKey;
        set
        {
            ArgumentException.ThrowIfNullOrWhiteSpace(value);
            if (_resourceKey == value) return;
            _resourceKey = value;
            Apply();
        }
    }

    public void Attach(ChartPluginContext context)
    {
        ArgumentNullException.ThrowIfNull(context);
        _context = context;
        _originalBackground = context.Chart.PlotBackground;
        _hasOriginalBackground = true;
        context.Chart.Loaded += OnLoaded;
        context.Chart.ActualThemeChanged += OnActualThemeChanged;
        Apply();
    }

    public void Detach()
    {
        if (_context is not null)
        {
            _context.Chart.Loaded -= OnLoaded;
            _context.Chart.ActualThemeChanged -= OnActualThemeChanged;
            if (_hasOriginalBackground)
            {
                _context.Chart.PlotBackground = _originalBackground;
            }
        }
        _hasOriginalBackground = false;
        _context = null;
    }

    private void OnLoaded(object sender, RoutedEventArgs args) => Apply();

    private void OnActualThemeChanged(FrameworkElement sender, object args) =>
        Apply();

    private void Apply()
    {
        if (_context is null)
        {
            return;
        }
        var brush = BrushOverride ?? ResolveNavigationContentBrush();
        _context.Chart.PlotBackground = brush is SolidColorBrush solid
            ? ApplyOpacity(solid.Color, solid.Opacity)
            : FallbackColor();
    }

    private XamlBrush? ResolveNavigationContentBrush()
    {
        if (_context is null)
        {
            return null;
        }

        DependencyObject? current = _context.Chart;
        while (current is not null)
        {
            if (!ReferenceEquals(current, _context.Chart) &&
                BackgroundOf(current) is XamlBrush background &&
                background.Opacity > 0)
            {
                return background;
            }
            if (current is FrameworkElement element &&
                TryGetBrush(element.Resources, ResourceKey, out var local))
            {
                return local;
            }
            current = VisualTreeHelper.GetParent(current);
        }

        var resources = Application.Current?.Resources;
        if (TryGetBrush(resources, ResourceKey, out var navigationBrush) ||
            TryGetBrush(resources, "LayerFillColorDefaultBrush", out navigationBrush) ||
            TryGetBrush(
                resources,
                "ApplicationPageBackgroundThemeBrush",
                out navigationBrush))
        {
            return navigationBrush;
        }
        return null;
    }

    private static XamlBrush? BackgroundOf(DependencyObject element) =>
        element switch
        {
            Border border => border.Background,
            Panel panel => panel.Background,
            Control control => control.Background,
            _ => null,
        };

    private static bool TryGetBrush(
        ResourceDictionary? resources,
        string key,
        out XamlBrush? brush)
    {
        brush = null;
        if (resources is null ||
            !resources.TryGetValue(key, out var value) ||
            value is not XamlBrush found)
        {
            return false;
        }
        brush = found;
        return true;
    }

    private Color FallbackColor() =>
        _context?.Chart.ActualTheme == ElementTheme.Light
            ? Color.FromArgb(128, 255, 255, 255)
            : Color.FromArgb(76, 58, 58, 58);

    private static Color ApplyOpacity(Color color, double opacity) =>
        Color.FromArgb(
            (byte)Math.Round(color.A * Math.Clamp(opacity, 0, 1)),
            color.R,
            color.G,
            color.B);
}

public sealed class ChartCrosshairTooltipPlugin(
    Func<double, ChartTooltipData?> dataProvider) : IChartPlugin
{
    private readonly Func<double, ChartTooltipData?> _dataProvider =
        dataProvider ?? throw new ArgumentNullException(nameof(dataProvider));
    private ChartPluginContext? _context;
    private ChartTooltipData? _data;
    private Point _pointer;
    private Rect _plotBounds;

    public Color CrosshairColor { get; set; } =
        Color.FromArgb(160, 124, 128, 152);
    public Color TooltipBackground { get; set; } =
        Color.FromArgb(248, 18, 20, 31);
    public Color TooltipBorder { get; set; } =
        Color.FromArgb(255, 42, 46, 58);
    public Color TooltipForeground { get; set; } =
        Color.FromArgb(255, 255, 255, 255);
    public Color TooltipSecondaryForeground { get; set; } =
        Color.FromArgb(255, 160, 168, 184);
    public string TooltipBackgroundResourceKey { get; set; } =
        "AcrylicBackgroundFillColorDefaultBrush";

    public void Attach(ChartPluginContext context)
    {
        ArgumentNullException.ThrowIfNull(context);
        _context = context;
    }

    public void Detach()
    {
        _context = null;
        _data = null;
    }

    public void OnLayout(ChartLayoutContext context) =>
        _plotBounds = context.PlotBounds;

    public void OnPointer(ChartPointerEvent pointerEvent)
    {
        if (_context is null)
        {
            return;
        }
        if (pointerEvent.IsExit)
        {
            if (_data is not null)
            {
                _data = null;
                _context.InvalidateOverlay();
            }
            return;
        }

        _pointer = pointerEvent.PlotPosition;
        var x = _context.Chart.PlotToData(_pointer).X;
        _data = double.IsFinite(x) ? _dataProvider(x) : null;
        _context.InvalidateOverlay();
    }

    public void BuildOverlay(ChartOverlayBuilder builder)
    {
        if (_context is null || _data is null ||
            _plotBounds.Width <= 0 || _plotBounds.Height <= 0)
        {
            return;
        }

        var x = Math.Clamp(_pointer.X, 0, _plotBounds.Width);
        var y = Math.Clamp(_pointer.Y, 0, _plotBounds.Height);
        builder.Add(new ChartOverlayLine(
            new Point(x, 0),
            new Point(x, _plotBounds.Height),
            CrosshairColor,
            CoordinateSpace: ChartCoordinateSpace.Pixels,
            DashPattern: [2, 1]));
        builder.Add(new ChartOverlayLine(
            new Point(0, y),
            new Point(_plotBounds.Width, y),
            CrosshairColor,
            CoordinateSpace: ChartCoordinateSpace.Pixels,
            DashPattern: [2, 1]));
        if (_data.Markers is not null)
        {
            foreach (var marker in _data.Markers)
            {
                if (double.IsFinite(marker.X) && double.IsFinite(marker.Y))
                {
                    builder.Add(new ChartOverlayMarker(
                        new Point(marker.X, marker.Y),
                        marker.Color,
                        YAxisKey: marker.YAxisKey));
                }
            }
        }
        builder.Add(new ChartOverlayTooltip(
            new Point(x, y),
            _data.Content,
            TooltipBackground,
            TooltipBorder,
            TooltipForeground,
            TooltipSecondaryForeground,
            TooltipBackgroundResourceKey));
    }

    public void ApplyTheme(bool dark)
    {
        CrosshairColor = dark
            ? Color.FromArgb(160, 124, 128, 152)
            : Color.FromArgb(160, 107, 114, 128);
        TooltipBackground = dark
            ? Color.FromArgb(248, 18, 20, 31)
            : Color.FromArgb(248, 255, 255, 255);
        TooltipBorder = dark
            ? Color.FromArgb(255, 42, 46, 58)
            : Color.FromArgb(255, 208, 213, 222);
        TooltipForeground = dark
            ? Color.FromArgb(255, 255, 255, 255)
            : Color.FromArgb(255, 17, 24, 39);
        TooltipSecondaryForeground = dark
            ? Color.FromArgb(255, 160, 168, 184)
            : Color.FromArgb(255, 86, 91, 112);
        _context?.InvalidateOverlay();
    }
}

public sealed class ChartPluginCollection : System.Collections.ObjectModel.Collection<IChartPlugin>
{
    private readonly GpuChart _owner;
    internal ChartPluginCollection(GpuChart owner) => _owner = owner;

    protected override void InsertItem(int index, IChartPlugin item)
    {
        ArgumentNullException.ThrowIfNull(item);
        base.InsertItem(index, item);
        _owner.AttachPlugin(item);
    }

    protected override void SetItem(int index, IChartPlugin item)
    {
        ArgumentNullException.ThrowIfNull(item);
        _owner.DetachPlugin(this[index]);
        base.SetItem(index, item);
        _owner.AttachPlugin(item);
    }

    protected override void RemoveItem(int index)
    {
        _owner.DetachPlugin(this[index]);
        base.RemoveItem(index);
    }

    protected override void ClearItems()
    {
        foreach (var plugin in this)
        {
            _owner.DetachPlugin(plugin);
        }
        base.ClearItems();
    }
}
