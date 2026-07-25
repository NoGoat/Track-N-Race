using System.Numerics;
using Microsoft.UI.Composition;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Hosting;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Markup;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using Windows.Foundation;
using Windows.UI;

namespace TrackNRace.Charting;

public sealed class ChartLineSeries : IDisposable
{
    private readonly GpuChart _owner;
    private bool _visible;
    private Color _stroke;
    private float _opacity;
    private float _thickness;
    private bool _disposed;

    internal ChartLineSeries(
        GpuChart owner,
        uint nativeId,
        ChartLineSeriesOptions options)
    {
        _owner = owner;
        NativeId = nativeId;
        Key = options.Key;
        XAxisKey = options.XAxisKey;
        YAxisKey = options.YAxisKey;
        _visible = options.Visible;
        _stroke = options.Stroke;
        _opacity = options.Opacity;
        _thickness = options.Thickness;
    }

    public string Key { get; }
    public string XAxisKey { get; }
    public string YAxisKey { get; }
    internal uint NativeId { get; }

    public bool Visible
    {
        get => _visible;
        set
        {
            if (_visible == value) return;
            _visible = value;
            SyncStyle();
        }
    }

    public Color Stroke
    {
        get => _stroke;
        set
        {
            if (_stroke == value) return;
            _stroke = value;
            SyncStyle();
        }
    }

    public float Opacity
    {
        get => _opacity;
        set
        {
            value = Math.Clamp(value, 0, 1);
            if (Math.Abs(_opacity - value) < .0001f) return;
            _opacity = value;
            SyncStyle();
        }
    }

    public float Thickness
    {
        get => _thickness;
        set
        {
            value = Math.Max(.5f, value);
            if (Math.Abs(_thickness - value) < .0001f) return;
            _thickness = value;
            SyncStyle();
        }
    }

    public void Replace(ReadOnlySpan<ChartPoint> points)
    {
        ThrowIfDisposed();
        _owner.Replace(this, points);
    }

    public void Append(ReadOnlySpan<ChartPoint> points)
    {
        ThrowIfDisposed();
        if (points.IsEmpty) return;
        _owner.Append(this, points);
    }

    public void Clear()
    {
        ThrowIfDisposed();
        _owner.Clear(this);
    }

    internal void SyncAxisRange(ChartAxis axis) =>
        _owner.SetSeriesYRange(this, axis.Minimum, axis.Maximum);

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        _owner.RemoveNativeSeries(this);
        GC.SuppressFinalize(this);
    }

    private void SyncStyle()
    {
        ThrowIfDisposed();
        _owner.SetSeriesStyle(this);
    }

    private void ThrowIfDisposed() =>
        ObjectDisposedException.ThrowIf(_disposed, this);
}

public sealed class GpuChart : Grid, IDisposable
{
    private readonly NativeChartRenderer _renderer = new();
    private readonly Grid _layout = new();
    private readonly StackPanel _leftAxes = new()
    {
        Orientation = Orientation.Horizontal,
        HorizontalAlignment = HorizontalAlignment.Right,
    };
    private readonly StackPanel _rightAxes = new()
    {
        Orientation = Orientation.Horizontal,
        HorizontalAlignment = HorizontalAlignment.Left,
    };
    private readonly Grid _plotHost = new()
    {
        Background = new SolidColorBrush(Color.FromArgb(0, 0, 0, 0)),
    };
    private readonly Grid _compositionHost = new() { IsHitTestVisible = false };
    private readonly Canvas _overlay = new() { IsHitTestVisible = false };
    private readonly Grid _bottomAxes = new();
    private readonly Dictionary<ChartAxis, AxisPresenter> _axisPresenters = [];
    private readonly Dictionary<string, Brush> _resourceBrushes = [];
    private readonly Dictionary<string, Style> _themeBackgroundStyles = [];
    private readonly HashSet<string> _failedThemeBackgroundStyles = [];
    private Border? _tooltipElement;
    private readonly ChartPluginContext _pluginContext;
    private bool _loaded;
    private bool _attached;
    private bool _disposed;
    private bool _renderQueued;
    private bool _overlayRenderQueued;
    private bool _axesDirty = true;
    private uint _pixelWidth = 1;
    private uint _pixelHeight = 1;
    private float _scale = 1;
    private ulong _surfaceGeneration;
    private Color _plotBackground = Color.FromArgb(0, 0, 0, 0);
    private Color _gridColor = Color.FromArgb(13, 255, 255, 255);
    private ICompositionSurface? _compositionSurface;
    private CompositionSurfaceBrush? _surfaceBrush;
    private SpriteVisual? _plotVisual;
    private XamlRoot? _subscribedXamlRoot;

    public GpuChart()
    {
        Axes = new ChartAxisCollection(this);
        Series = new ChartSeriesCollection(this);
        Plugins = new ChartPluginCollection(this);
        _pluginContext = new ChartPluginContext(this);

        _layout.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        _layout.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        _layout.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        _layout.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        _layout.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });

        Grid.SetColumn(_leftAxes, 0);
        Grid.SetRow(_leftAxes, 0);
        Grid.SetColumn(_plotHost, 1);
        Grid.SetRow(_plotHost, 0);
        Grid.SetColumn(_rightAxes, 2);
        Grid.SetRow(_rightAxes, 0);
        Grid.SetColumn(_bottomAxes, 1);
        Grid.SetRow(_bottomAxes, 1);

        _plotHost.Children.Add(_compositionHost);
        _plotHost.Children.Add(_overlay);
        _layout.Children.Add(_leftAxes);
        _layout.Children.Add(_plotHost);
        _layout.Children.Add(_rightAxes);
        _layout.Children.Add(_bottomAxes);
        Children.Add(_layout);

        Loaded += OnLoaded;
        Unloaded += OnUnloaded;
        _plotHost.SizeChanged += OnPlotSizeChanged;
        _plotHost.PointerMoved += OnPointerMoved;
        _plotHost.PointerExited += OnPointerExited;
    }

    public ChartAxisCollection Axes { get; }
    public ChartSeriesCollection Series { get; }
    public ChartPluginCollection Plugins { get; }
    public ChartDiagnostics? Diagnostics { get; private set; }
    public event Action<ChartDiagnostics>? DiagnosticsUpdated;
    public event Action<Exception>? RenderFailed;

    public Color PlotBackground
    {
        get => _plotBackground;
        set
        {
            if (_plotBackground == value) return;
            _plotBackground = value;
            Invalidate();
        }
    }

    public Color GridColor
    {
        get => _gridColor;
        set
        {
            if (_gridColor == value) return;
            _gridColor = value;
            Invalidate();
        }
    }

    public void SetTheme(Color plotBackground, Color gridColor)
    {
        _plotBackground = plotBackground;
        _gridColor = gridColor;
        Invalidate();
    }

    public Point DataToPlot(double x, double y, string? yAxisKey = null)
    {
        var xAxis = Axes.FirstOrDefault(axis => axis.Orientation == ChartAxisOrientation.X);
        var yAxis = yAxisKey is null
            ? Axes.FirstOrDefault(axis => axis.Orientation == ChartAxisOrientation.Y)
            : Axes.FirstOrDefault(axis => axis.Key == yAxisKey);
        if (xAxis is null || yAxis is null ||
            !xAxis.HasValidRange || !yAxis.HasValidRange)
        {
            return new Point(double.NaN, double.NaN);
        }
        return new Point(
            (x - xAxis.Minimum) / (xAxis.Maximum - xAxis.Minimum) * _plotHost.ActualWidth,
            (1 - (y - yAxis.Minimum) / (yAxis.Maximum - yAxis.Minimum)) *
                _plotHost.ActualHeight);
    }

    public ChartPoint PlotToData(Point point, string? yAxisKey = null)
    {
        var xAxis = Axes.FirstOrDefault(axis => axis.Orientation == ChartAxisOrientation.X);
        var yAxis = yAxisKey is null
            ? Axes.FirstOrDefault(axis => axis.Orientation == ChartAxisOrientation.Y)
            : Axes.FirstOrDefault(axis => axis.Key == yAxisKey);
        if (xAxis is null || yAxis is null || _plotHost.ActualWidth <= 0 ||
            _plotHost.ActualHeight <= 0)
        {
            return new ChartPoint(double.NaN, double.NaN);
        }
        return new ChartPoint(
            xAxis.Minimum +
                point.X / _plotHost.ActualWidth * (xAxis.Maximum - xAxis.Minimum),
            yAxis.Maximum -
                point.Y / _plotHost.ActualHeight * (yAxis.Maximum - yAxis.Minimum));
    }

    public double DataXToPlot(double x)
    {
        var xAxis = Axes.FirstOrDefault(
            axis => axis.Orientation == ChartAxisOrientation.X);
        return xAxis is null || !xAxis.HasValidRange
            ? double.NaN
            : (x - xAxis.Minimum) / (xAxis.Maximum - xAxis.Minimum) *
                _plotHost.ActualWidth;
    }

    public void Invalidate()
    {
        if (_disposed || !_loaded || _renderQueued)
        {
            return;
        }
        _renderQueued = true;
        if (!DispatcherQueue.TryEnqueue(
            DispatcherQueuePriority.Low,
            RenderCore))
        {
            _renderQueued = false;
        }
    }

    internal ChartLineSeries CreateSeries(ChartLineSeriesOptions options)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        var nativeId = _renderer.AddSeries(options);
        var series = new ChartLineSeries(this, nativeId, options);
        Invalidate();
        return series;
    }

    internal void Replace(ChartLineSeries series, ReadOnlySpan<ChartPoint> points)
    {
        _renderer.Replace(series.NativeId, points);
        Invalidate();
    }

    internal void Append(ChartLineSeries series, ReadOnlySpan<ChartPoint> points)
    {
        _renderer.Append(series.NativeId, points);
        Invalidate();
    }

    internal void Clear(ChartLineSeries series)
    {
        _renderer.Clear(series.NativeId);
        Invalidate();
    }

    internal void SetSeriesStyle(ChartLineSeries series)
    {
        _renderer.SetSeriesStyle(
            series.NativeId,
            series.Stroke,
            series.Opacity,
            series.Thickness,
            series.Visible);
        Invalidate();
    }

    internal void SetSeriesYRange(
        ChartLineSeries series, double minimum, double maximum) =>
        _renderer.SetSeriesYRange(series.NativeId, minimum, maximum);

    internal void RemoveNativeSeries(ChartLineSeries series)
    {
        if (!_disposed)
        {
            _renderer.RemoveSeries(series.NativeId);
        }
    }

    internal void OnAxisChanged()
    {
        Invalidate();
    }

    internal void OnAxesChanged()
    {
        _axesDirty = true;
        Invalidate();
    }

    internal void InvalidateOverlay()
    {
        if (_disposed || !_loaded || _overlayRenderQueued)
        {
            return;
        }
        _overlayRenderQueued = true;
        if (!DispatcherQueue.TryEnqueue(
            DispatcherQueuePriority.Normal,
            () =>
            {
                _overlayRenderQueued = false;
                if (!_disposed && _loaded)
                {
                    RenderPluginOverlays();
                }
            }))
        {
            _overlayRenderQueued = false;
        }
    }

    internal void AttachPlugin(IChartPlugin plugin)
    {
        plugin.Attach(_pluginContext);
        Invalidate();
    }

    internal void DetachPlugin(IChartPlugin plugin)
    {
        plugin.Detach();
        Invalidate();
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        Loaded -= OnLoaded;
        Unloaded -= OnUnloaded;
        _plotHost.SizeChanged -= OnPlotSizeChanged;
        _plotHost.PointerMoved -= OnPointerMoved;
        _plotHost.PointerExited -= OnPointerExited;
        UnsubscribeFromXamlRoot();
        Plugins.Clear();
        ElementCompositionPreview.SetElementChildVisual(
            _compositionHost, null);
        _plotVisual?.Dispose();
        _surfaceBrush?.Dispose();
        _plotVisual = null;
        _surfaceBrush = null;
        _compositionSurface = null;
        Series.Clear();
        _renderer.Dispose();
        GC.SuppressFinalize(this);
    }

    private void OnLoaded(object sender, RoutedEventArgs args)
    {
        _loaded = true;
        try
        {
            if (!_attached)
            {
                var compositor = ElementCompositionPreview
                    .GetElementVisual(_compositionHost)
                    .Compositor;
                _compositionSurface = _renderer.Attach(compositor);
                _surfaceGeneration = _renderer.SurfaceGeneration;
                _surfaceBrush = compositor.CreateSurfaceBrush(
                    _compositionSurface);
                _surfaceBrush.Stretch = CompositionStretch.Fill;
                _plotVisual = compositor.CreateSpriteVisual();
                _plotVisual.Brush = _surfaceBrush;
                ElementCompositionPreview.SetElementChildVisual(
                    _compositionHost, _plotVisual);
                _attached = true;
            }
            SubscribeToXamlRoot();
            ResizeNative();
            Invalidate();
        }
        catch (Exception error)
        {
            RenderFailed?.Invoke(error);
        }
    }

    private void OnUnloaded(object sender, RoutedEventArgs args)
    {
        _loaded = false;
        _renderQueued = false;
        _overlayRenderQueued = false;
        UnsubscribeFromXamlRoot();
    }

    private void OnPlotSizeChanged(object sender, SizeChangedEventArgs args)
    {
        try
        {
            ResizeNative();
            _axesDirty = true;
            Invalidate();
        }
        catch (Exception error)
        {
            RenderFailed?.Invoke(error);
        }
    }

    private void OnXamlRootChanged(
        XamlRoot sender, XamlRootChangedEventArgs args)
    {
        try
        {
            ResizeNative();
            _axesDirty = true;
            Invalidate();
        }
        catch (Exception error)
        {
            RenderFailed?.Invoke(error);
        }
    }

    private void ResizeNative()
    {
        if (!_attached)
        {
            return;
        }
        _scale = (float)(XamlRoot?.RasterizationScale ?? 1);
        _pixelWidth = Math.Max(
            1u, checked((uint)Math.Ceiling(_plotHost.ActualWidth * _scale)));
        _pixelHeight = Math.Max(
            1u, checked((uint)Math.Ceiling(_plotHost.ActualHeight * _scale)));
        var visualSize = new Vector2(
            (float)_plotHost.ActualWidth,
            (float)_plotHost.ActualHeight);
        if (_plotVisual is not null)
        {
            _plotVisual.Size = visualSize;
        }
        _renderer.Resize(_pixelWidth, _pixelHeight, _scale);
    }

    private void SubscribeToXamlRoot()
    {
        var current = XamlRoot;
        if (ReferenceEquals(current, _subscribedXamlRoot))
        {
            return;
        }
        UnsubscribeFromXamlRoot();
        _subscribedXamlRoot = current;
        if (_subscribedXamlRoot is not null)
        {
            _subscribedXamlRoot.Changed += OnXamlRootChanged;
        }
    }

    private void UnsubscribeFromXamlRoot()
    {
        if (_subscribedXamlRoot is not null)
        {
            _subscribedXamlRoot.Changed -= OnXamlRootChanged;
            _subscribedXamlRoot = null;
        }
    }

    private void RenderCore()
    {
        _renderQueued = false;
        if (_disposed || !_loaded || !_attached)
        {
            return;
        }
        try
        {
            if (_axesDirty)
            {
                RebuildAxisHosts();
                _axesDirty = false;
            }
            RenderAxes();
            var xAxis = Axes.FirstOrDefault(
                axis => axis.Orientation == ChartAxisOrientation.X);
            if (xAxis is null || !xAxis.HasValidRange)
            {
                return;
            }

            _renderer.SetBackground(_plotBackground);
            _renderer.SetGridColor(_gridColor);
            _renderer.SetXRange(xAxis.Minimum, xAxis.Maximum);
            var gridTicks = xAxis.ShowGridLines
                ? xAxis.TickProvider.GetTicks(
                    xAxis.Minimum, xAxis.Maximum, _plotHost.ActualWidth).ToArray()
                : [];
            _renderer.SetVerticalGrid(gridTicks);
            var primaryYAxis = Axes.FirstOrDefault(
                axis => axis.Orientation == ChartAxisOrientation.Y &&
                    axis.Side == ChartAxisSide.Left);
            var horizontalGrid = primaryYAxis is { ShowGridLines: true } &&
                primaryYAxis.HasValidRange
                ? primaryYAxis.TickProvider.GetTicks(
                        primaryYAxis.Minimum,
                        primaryYAxis.Maximum,
                        _plotHost.ActualHeight)
                    .Select(value =>
                        (value - primaryYAxis.Minimum) /
                        (primaryYAxis.Maximum - primaryYAxis.Minimum))
                    .ToArray()
                : [];
            _renderer.SetHorizontalGrid(horizontalGrid);

            foreach (var series in Series)
            {
                var yAxis = Axes.FirstOrDefault(axis => axis.Key == series.YAxisKey)
                    ?? throw new InvalidOperationException(
                        $"Series '{series.Key}' references missing Y axis '{series.YAxisKey}'.");
                series.SyncAxisRange(yAxis);
            }

            var layoutContext = new ChartLayoutContext(
                new Rect(0, 0, _plotHost.ActualWidth, _plotHost.ActualHeight),
                _scale);
            foreach (var plugin in Plugins)
            {
                plugin.OnLayout(layoutContext);
            }
            RenderPluginOverlays();

            Diagnostics = _renderer.Render();
            RefreshCompositionSurfaceIfNeeded();
            DiagnosticsUpdated?.Invoke(Diagnostics);
        }
        catch (Exception error)
        {
            RenderFailed?.Invoke(error);
        }
    }

    private void RefreshCompositionSurfaceIfNeeded()
    {
        var generation = _renderer.SurfaceGeneration;
        if (generation == _surfaceGeneration || _surfaceBrush is null)
        {
            return;
        }
        var compositor = ElementCompositionPreview
            .GetElementVisual(_compositionHost)
            .Compositor;
        _compositionSurface = _renderer.Attach(compositor);
        _surfaceBrush.Surface = _compositionSurface;
        _surfaceGeneration = _renderer.SurfaceGeneration;
    }

    private void RebuildAxisHosts()
    {
        _leftAxes.Children.Clear();
        _rightAxes.Children.Clear();
        _bottomAxes.Children.Clear();
        _axisPresenters.Clear();

        foreach (var axis in Axes)
        {
            var presenter = new AxisPresenter(axis);
            _axisPresenters.Add(axis, presenter);
            switch (axis.Side)
            {
                case ChartAxisSide.Left:
                    _leftAxes.Children.Add(presenter);
                    break;
                case ChartAxisSide.Right:
                    _rightAxes.Children.Add(presenter);
                    break;
                case ChartAxisSide.Bottom:
                    _bottomAxes.Children.Add(presenter);
                    break;
            }
        }
    }

    private void RenderAxes()
    {
        var dividerBrush =
            ResolveResourceBrush("DividerStrokeColorDefaultBrush") ??
            new SolidColorBrush(Color.FromArgb(51, 128, 128, 128));
        foreach (var (axis, presenter) in _axisPresenters)
        {
            var length = axis.Orientation == ChartAxisOrientation.X
                ? _plotHost.ActualWidth
                : _plotHost.ActualHeight;
            presenter.Update(length, dividerBrush);
        }
    }

    private void RenderPluginOverlays()
    {
        for (var index = _overlay.Children.Count - 1; index >= 0; index--)
        {
            if (!ReferenceEquals(_overlay.Children[index], _tooltipElement))
            {
                _overlay.Children.RemoveAt(index);
            }
        }
        if (_tooltipElement is not null)
        {
            _tooltipElement.Visibility = Visibility.Collapsed;
        }
        var builder = new ChartOverlayBuilder();
        foreach (var plugin in Plugins)
        {
            plugin.BuildOverlay(builder);
        }
        foreach (var command in builder.Commands)
        {
            switch (command)
            {
                case ChartOverlayLine line:
                    var start = ResolveOverlayPoint(
                        line.Start, line.CoordinateSpace, line.YAxisKey);
                    var end = ResolveOverlayPoint(
                        line.End, line.CoordinateSpace, line.YAxisKey);
                    var overlayLine = new Line
                    {
                        X1 = start.X,
                        Y1 = start.Y,
                        X2 = end.X,
                        Y2 = end.Y,
                        Stroke = new SolidColorBrush(line.Color),
                        StrokeThickness = line.Thickness,
                    };
                    if (line.DashPattern is not null)
                    {
                        foreach (var dash in line.DashPattern)
                        {
                            overlayLine.StrokeDashArray.Add(dash);
                        }
                    }
                    _overlay.Children.Add(overlayLine);
                    break;
                case ChartOverlayText text:
                    var position = ResolveOverlayPoint(
                        text.Position, text.CoordinateSpace, text.YAxisKey);
                    var label = new TextBlock
                    {
                        Text = text.Text,
                        Foreground = new SolidColorBrush(text.Color),
                        FontSize = text.FontSize,
                    };
                    Canvas.SetLeft(label, position.X);
                    Canvas.SetTop(label, position.Y);
                    _overlay.Children.Add(label);
                    break;
                case ChartOverlayMarker marker:
                    var markerPosition = ResolveOverlayPoint(
                        marker.Position,
                        marker.CoordinateSpace,
                        marker.YAxisKey);
                    if (!double.IsFinite(markerPosition.X) ||
                        !double.IsFinite(markerPosition.Y) ||
                        markerPosition.X < 0 ||
                        markerPosition.X > _plotHost.ActualWidth ||
                        markerPosition.Y < 0 ||
                        markerPosition.Y > _plotHost.ActualHeight)
                    {
                        break;
                    }
                    // SVG strokes are centered on the circle's radius. Include
                    // that outward half-stroke on both sides so a 3 px marker
                    // matches Electron's 7.5 px rendered diameter.
                    var diameter =
                        marker.Radius * 2 + marker.StrokeThickness;
                    var dot = new Ellipse
                    {
                        Width = diameter,
                        Height = diameter,
                        Fill = new SolidColorBrush(
                            Color.FromArgb(255, 0, 0, 0)),
                        Stroke = new SolidColorBrush(marker.Color),
                        StrokeThickness = marker.StrokeThickness,
                        IsHitTestVisible = false,
                    };
                    Canvas.SetLeft(dot, markerPosition.X - diameter / 2);
                    Canvas.SetTop(dot, markerPosition.Y - diameter / 2);
                    _overlay.Children.Add(dot);
                    break;
                case ChartOverlayTooltip tooltip:
                    AddTooltip(tooltip);
                    break;
            }
        }
    }

    private void AddTooltip(ChartOverlayTooltip command)
    {
        var content = new StackPanel { Spacing = 2 };
        content.Children.Add(new TextBlock
        {
            Text = command.Content.Header,
            Foreground = new SolidColorBrush(command.SecondaryForeground),
            FontSize = 12,
            Margin = new Thickness(0, 0, 0, 2),
        });
        string? group = null;
        foreach (var entry in command.Content.Entries)
        {
            if (entry.Group is not null && entry.Group != group)
            {
                group = entry.Group;
                content.Children.Add(new TextBlock
                {
                    Text = group,
                    Foreground = new SolidColorBrush(
                        command.SecondaryForeground),
                    FontSize = 10,
                    Margin = new Thickness(0, 3, 0, 0),
                });
            }
            var row = new StackPanel
            {
                Orientation = Orientation.Horizontal,
                Spacing = 4,
            };
            row.Children.Add(new TextBlock
            {
                Text = $"{entry.Label}:",
                Foreground = new SolidColorBrush(entry.Color),
                FontSize = 12,
            });
            row.Children.Add(new TextBlock
            {
                Text = entry.Value,
                Foreground = new SolidColorBrush(command.Foreground),
                FontSize = 12,
            });
            content.Children.Add(row);
        }

        var tooltip = _tooltipElement;
        if (tooltip is null)
        {
            tooltip = new Border
            {
                BorderThickness = new Thickness(1),
                CornerRadius = new CornerRadius(4),
                Padding = new Thickness(10, 6, 10, 6),
                IsHitTestVisible = false,
            };
            Canvas.SetZIndex(tooltip, 1000);
            _tooltipElement = tooltip;
            _overlay.Children.Add(tooltip);
        }
        tooltip.Visibility = Visibility.Visible;
        tooltip.BorderBrush = new SolidColorBrush(command.Border);
        tooltip.Child = content;
        var backgroundStyle = ResolveThemeBackgroundStyle(
            command.BackgroundResourceKey);
        if (backgroundStyle is not null)
        {
            tooltip.ClearValue(Border.BackgroundProperty);
            tooltip.Style = backgroundStyle;
        }
        else
        {
            tooltip.Style = null;
            tooltip.Background = new SolidColorBrush(command.Background);
        }
        const double gap = 16;
        const double padding = 4;
        tooltip.Measure(new Size(
            double.PositiveInfinity,
            double.PositiveInfinity));
        var desired = tooltip.DesiredSize;
        var left = command.Position.X <= _plotHost.ActualWidth / 2
            ? command.Position.X + gap
            : command.Position.X - gap - desired.Width;
        var top = command.Position.Y <= _plotHost.ActualHeight / 2
            ? command.Position.Y + gap
            : command.Position.Y - gap - desired.Height;
        Canvas.SetLeft(
            tooltip,
            Math.Clamp(
                left,
                padding,
                Math.Max(padding, _plotHost.ActualWidth -
                    desired.Width - padding)));
        Canvas.SetTop(
            tooltip,
            Math.Clamp(
                top,
                padding,
                Math.Max(padding, _plotHost.ActualHeight -
                    desired.Height - padding)));
    }

    private Brush? ResolveResourceBrush(string? key)
    {
        if (string.IsNullOrWhiteSpace(key))
        {
            return null;
        }
        if (_resourceBrushes.TryGetValue(key, out var cached))
        {
            return cached;
        }
        var brush =
            BrushFrom(_plotHost.Resources, key) ??
            BrushFrom(Resources, key) ??
            BrushFrom(Application.Current?.Resources, key);
        if (brush is not null)
        {
            _resourceBrushes[key] = brush;
        }
        return brush;
    }

    private static Brush? BrushFrom(ResourceDictionary? resources, string key) =>
        resources is not null &&
        resources.TryGetValue(key, out var value) &&
        value is Brush brush
            ? brush
            : null;

    private Style? ResolveThemeBackgroundStyle(string? key)
    {
        if (string.IsNullOrWhiteSpace(key))
        {
            return null;
        }
        if (_themeBackgroundStyles.TryGetValue(key, out var cached))
        {
            return cached;
        }
        if (_failedThemeBackgroundStyles.Contains(key))
        {
            return null;
        }
        try
        {
            var style = (Style)XamlReader.Load(
                "<Style xmlns=\"http://schemas.microsoft.com/winfx/2006/xaml/presentation\" " +
                "TargetType=\"Border\">" +
                "<Setter Property=\"Background\" Value=\"{ThemeResource " +
                key +
                "}\" /></Style>");
            _themeBackgroundStyles[key] = style;
            return style;
        }
        catch
        {
            _failedThemeBackgroundStyles.Add(key);
            return null;
        }
    }

    private Point ResolveOverlayPoint(
        Point point, ChartCoordinateSpace space, string? yAxisKey) =>
        space == ChartCoordinateSpace.Pixels
            ? point
            : DataToPlot(point.X, point.Y, yAxisKey);

    private void OnPointerMoved(object sender, PointerRoutedEventArgs args)
    {
        if (Plugins.Count == 0) return;
        var pointer = args.GetCurrentPoint(_plotHost);
        var value = new ChartPointerEvent(pointer, pointer.Position, false);
        foreach (var plugin in Plugins)
        {
            plugin.OnPointer(value);
        }
    }

    private void OnPointerExited(object sender, PointerRoutedEventArgs args)
    {
        if (Plugins.Count == 0) return;
        var pointer = args.GetCurrentPoint(_plotHost);
        var value = new ChartPointerEvent(pointer, pointer.Position, true);
        foreach (var plugin in Plugins)
        {
            plugin.OnPointer(value);
        }
    }

    private sealed class AxisPresenter : Canvas
    {
        private readonly ChartAxis _axis;
        private readonly List<TextBlock> _labels = [];
        private readonly Border _line = new();
        private Color _brushColor;
        private SolidColorBrush? _brush;

        public AxisPresenter(ChartAxis axis)
        {
            _axis = axis;
            if (axis.Orientation == ChartAxisOrientation.X)
            {
                Height = 22;
                HorizontalAlignment = HorizontalAlignment.Stretch;
            }
            else
            {
                Width = axis.Side == ChartAxisSide.Left ? 44 : 50;
                VerticalAlignment = VerticalAlignment.Stretch;
            }
            _line.Visibility = axis.Side == ChartAxisSide.Right
                ? Visibility.Collapsed
                : Visibility.Visible;
            Children.Add(_line);
        }

        public void Update(double length, Brush lineBrush)
        {
            if (!_axis.HasValidRange || length <= 0)
            {
                Visibility = Visibility.Collapsed;
                return;
            }
            Visibility = Visibility.Visible;
            var values = _axis.TickProvider.GetTicks(
                _axis.Minimum, _axis.Maximum, length);
            EnsureChildren(values.Count);
            if (_brush is null || _brushColor != _axis.Color)
            {
                _brushColor = _axis.Color;
                _brush = new SolidColorBrush(_brushColor);
            }
            var brush = _brush;
            _line.Background = lineBrush;

            if (_axis.Orientation == ChartAxisOrientation.X)
            {
                _line.Width = length;
                _line.Height = 1;
                Canvas.SetLeft(_line, 0);
                Canvas.SetTop(_line, 0);
            }
            else
            {
                _line.Width = 1;
                _line.Height = length;
                Canvas.SetLeft(
                    _line, _axis.Side == ChartAxisSide.Left ? Width - 1 : 0);
                Canvas.SetTop(_line, 0);
            }

            for (var index = 0; index < values.Count; index++)
            {
                var value = values[index];
                var normalized =
                    (value - _axis.Minimum) / (_axis.Maximum - _axis.Minimum);
                var label = _labels[index];
                label.Text = _axis.LabelFormatter(value);
                label.Foreground = brush;

                if (_axis.Orientation == ChartAxisOrientation.X)
                {
                    var x = normalized * length;
                    label.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
                    Canvas.SetLeft(label, x - label.DesiredSize.Width / 2);
                    Canvas.SetTop(label, 2);
                }
                else
                {
                    var y = (1 - normalized) * length;
                    label.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
                    Canvas.SetLeft(
                        label,
                        _axis.Side == ChartAxisSide.Left
                            ? Math.Max(0, Width - 4 - label.DesiredSize.Width)
                            : 4);
                    Canvas.SetTop(label, y - label.DesiredSize.Height / 2);
                }
            }
        }

        private void EnsureChildren(int count)
        {
            while (_labels.Count < count)
            {
                var label = new TextBlock
                {
                    FontFamily = new FontFamily("Segoe UI Variable Text"),
                    FontSize = 12,
                };
                _labels.Add(label);
                Children.Add(label);
            }
            for (var index = 0; index < _labels.Count; index++)
            {
                var visibility = index < count
                    ? Visibility.Visible
                    : Visibility.Collapsed;
                _labels[index].Visibility = visibility;
            }
        }
    }
}
