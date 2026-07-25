using System.Collections.ObjectModel;
using System.Runtime.InteropServices;
using Windows.UI;

namespace TrackNRace.Charting;

[StructLayout(LayoutKind.Sequential)]
public readonly record struct ChartPoint(double X, double Y);

public enum ChartAxisOrientation
{
    X,
    Y,
}

public enum ChartAxisSide
{
    Bottom,
    Left,
    Right,
}

public interface IChartTickProvider
{
    IReadOnlyList<double> GetTicks(double minimum, double maximum, double pixelLength);
}

public sealed class FixedChartTickProvider(params double[] ticks) : IChartTickProvider
{
    private readonly double[] _ticks = ticks;

    public IReadOnlyList<double> GetTicks(
        double minimum, double maximum, double pixelLength) =>
        _ticks.Where(value => value >= minimum && value <= maximum).ToArray();
}

public sealed class IntervalChartTickProvider(double interval) : IChartTickProvider
{
    public double Interval { get; } = interval > 0
        ? interval
        : throw new ArgumentOutOfRangeException(nameof(interval));

    public IReadOnlyList<double> GetTicks(
        double minimum, double maximum, double pixelLength)
    {
        if (!double.IsFinite(minimum) || !double.IsFinite(maximum) ||
            maximum <= minimum)
        {
            return [];
        }
        var first = Math.Ceiling(minimum / Interval) * Interval;
        var count = Math.Min(
            2048,
            Math.Max(0, (int)Math.Floor((maximum - first) / Interval) + 1));
        var ticks = new double[count];
        for (var index = 0; index < count; index++)
        {
            ticks[index] = first + index * Interval;
        }
        return ticks;
    }
}

public sealed class ChartAxis
{
    private double _minimum;
    private double _maximum = 1;
    private IChartTickProvider _tickProvider = new IntervalChartTickProvider(.2);
    private Func<double, string> _labelFormatter = value => $"{value:g}";
    private Color _color = Color.FromArgb(255, 107, 114, 128);
    private bool _showGridLines;

    public ChartAxis(
        string key,
        ChartAxisOrientation orientation,
        ChartAxisSide side)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(key);
        if (orientation == ChartAxisOrientation.X && side != ChartAxisSide.Bottom)
        {
            throw new ArgumentException("V1 X axes must use the bottom side.", nameof(side));
        }
        if (orientation == ChartAxisOrientation.Y && side == ChartAxisSide.Bottom)
        {
            throw new ArgumentException("Y axes must use the left or right side.", nameof(side));
        }
        Key = key;
        Orientation = orientation;
        Side = side;
    }

    public string Key { get; }
    public ChartAxisOrientation Orientation { get; }
    public ChartAxisSide Side { get; }

    public double Minimum
    {
        get => _minimum;
        set => Set(ref _minimum, value);
    }

    public double Maximum
    {
        get => _maximum;
        set => Set(ref _maximum, value);
    }

    public IChartTickProvider TickProvider
    {
        get => _tickProvider;
        set => Set(ref _tickProvider, value ?? throw new ArgumentNullException(nameof(value)));
    }

    public Func<double, string> LabelFormatter
    {
        get => _labelFormatter;
        set => Set(ref _labelFormatter, value ?? throw new ArgumentNullException(nameof(value)));
    }

    public Color Color
    {
        get => _color;
        set => Set(ref _color, value);
    }

    public bool ShowGridLines
    {
        get => _showGridLines;
        set => Set(ref _showGridLines, value);
    }

    internal event Action? Changed;

    internal bool HasValidRange =>
        double.IsFinite(Minimum) && double.IsFinite(Maximum) && Maximum > Minimum;

    private void Set<T>(ref T field, T value)
    {
        if (EqualityComparer<T>.Default.Equals(field, value))
        {
            return;
        }
        field = value;
        Changed?.Invoke();
    }
}

public sealed record ChartLineSeriesOptions(
    string Key,
    string XAxisKey,
    string YAxisKey,
    Color Stroke,
    float Thickness = 2,
    float Opacity = 1,
    bool Visible = true,
    int MaximumPointCount = 0,
    double MaximumXSpan = 0);

public sealed record ChartDiagnostics(
    string Adapter,
    long SourcePoints,
    long SubmittedSegments,
    double FrameMilliseconds,
    bool UsedReduction,
    bool UsingWarp);

public sealed class ChartAxisCollection : Collection<ChartAxis>
{
    private readonly GpuChart _owner;

    internal ChartAxisCollection(GpuChart owner) => _owner = owner;

    protected override void InsertItem(int index, ChartAxis item)
    {
        ArgumentNullException.ThrowIfNull(item);
        if (this.Any(axis => axis.Key == item.Key))
        {
            throw new ArgumentException($"Axis key '{item.Key}' already exists.", nameof(item));
        }
        base.InsertItem(index, item);
        item.Changed += _owner.OnAxisChanged;
        _owner.OnAxesChanged();
    }

    protected override void SetItem(int index, ChartAxis item)
    {
        ArgumentNullException.ThrowIfNull(item);
        if (this.Where((_, candidate) => candidate != index)
            .Any(axis => axis.Key == item.Key))
        {
            throw new ArgumentException($"Axis key '{item.Key}' already exists.", nameof(item));
        }
        this[index].Changed -= _owner.OnAxisChanged;
        base.SetItem(index, item);
        item.Changed += _owner.OnAxisChanged;
        _owner.OnAxesChanged();
    }

    protected override void RemoveItem(int index)
    {
        this[index].Changed -= _owner.OnAxisChanged;
        base.RemoveItem(index);
        _owner.OnAxesChanged();
    }

    protected override void ClearItems()
    {
        foreach (var axis in this)
        {
            axis.Changed -= _owner.OnAxisChanged;
        }
        base.ClearItems();
        _owner.OnAxesChanged();
    }
}

public sealed class ChartSeriesCollection : IReadOnlyCollection<ChartLineSeries>
{
    private readonly GpuChart _owner;
    private readonly List<ChartLineSeries> _items = [];

    internal ChartSeriesCollection(GpuChart owner) => _owner = owner;

    public int Count => _items.Count;
    public ChartLineSeries this[string key] =>
        _items.First(series => series.Key == key);

    public ChartLineSeries Add(ChartLineSeriesOptions options)
    {
        ArgumentNullException.ThrowIfNull(options);
        if (_items.Any(series => series.Key == options.Key))
        {
            throw new ArgumentException(
                $"Series key '{options.Key}' already exists.", nameof(options));
        }
        var series = _owner.CreateSeries(options);
        _items.Add(series);
        return series;
    }

    public bool Remove(ChartLineSeries series)
    {
        if (!_items.Remove(series))
        {
            return false;
        }
        series.Dispose();
        _owner.Invalidate();
        return true;
    }

    public void Clear()
    {
        foreach (var series in _items)
        {
            series.Dispose();
        }
        _items.Clear();
        _owner.Invalidate();
    }

    public IEnumerator<ChartLineSeries> GetEnumerator() => _items.GetEnumerator();
    System.Collections.IEnumerator System.Collections.IEnumerable.GetEnumerator() =>
        GetEnumerator();
}
