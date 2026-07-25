using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;
using Microsoft.UI.Composition;
using Windows.UI;

namespace TrackNRace.Charting;

internal sealed class NativeChartRenderer : IDisposable
{
    private const string NativeLibrary = "TrackNRace.ChartRenderer";

    [StructLayout(LayoutKind.Sequential)]
    private struct NativeDiagnostics
    {
        public ulong SourcePoints;
        public ulong SubmittedSegments;
        public double FrameMilliseconds;
        public int UsedReduction;
        public int UsingWarp;
    }

    private sealed class ChartHandle : SafeHandleZeroOrMinusOneIsInvalid
    {
        private ChartHandle() : base(true) { }
        protected override bool ReleaseHandle()
        {
            NativeMethods.Destroy(handle);
            return true;
        }
    }

    private static class NativeMethods
    {
        [DllImport(NativeLibrary, EntryPoint = "tnr_chart_create",
            CallingConvention = CallingConvention.Cdecl)]
        internal static extern ChartHandle Create();

        [DllImport(NativeLibrary, EntryPoint = "tnr_chart_destroy",
            CallingConvention = CallingConvention.Cdecl)]
        internal static extern void Destroy(nint chart);

        [DllImport(NativeLibrary, EntryPoint = "tnr_chart_attach",
            CallingConvention = CallingConvention.Cdecl)]
        internal static extern int Attach(
            ChartHandle chart, nint compositor, out nint compositionSurface);

        [DllImport(NativeLibrary, EntryPoint = "tnr_chart_resize",
            CallingConvention = CallingConvention.Cdecl)]
        internal static extern int Resize(
            ChartHandle chart, uint width, uint height, float scale);

        [DllImport(NativeLibrary, EntryPoint = "tnr_chart_set_background",
            CallingConvention = CallingConvention.Cdecl)]
        internal static extern void SetBackground(
            ChartHandle chart, float r, float g, float b, float a);

        [DllImport(NativeLibrary, EntryPoint = "tnr_chart_set_grid_color",
            CallingConvention = CallingConvention.Cdecl)]
        internal static extern void SetGridColor(
            ChartHandle chart, float r, float g, float b, float a);

        [DllImport(NativeLibrary, EntryPoint = "tnr_chart_set_x_range",
            CallingConvention = CallingConvention.Cdecl)]
        internal static extern int SetXRange(
            ChartHandle chart, double minimum, double maximum);

        [DllImport(NativeLibrary, EntryPoint = "tnr_chart_set_vertical_grid",
            CallingConvention = CallingConvention.Cdecl)]
        internal static extern unsafe int SetVerticalGrid(
            ChartHandle chart, double* values, nuint count);

        [DllImport(NativeLibrary, EntryPoint = "tnr_chart_add_series",
            CallingConvention = CallingConvention.Cdecl)]
        internal static extern uint AddSeries(
            ChartHandle chart,
            float r, float g, float b, float a,
            float thickness, nuint maximumPoints, double maximumXSpan);

        [DllImport(NativeLibrary, EntryPoint = "tnr_chart_remove_series",
            CallingConvention = CallingConvention.Cdecl)]
        internal static extern int RemoveSeries(ChartHandle chart, uint seriesId);

        [DllImport(NativeLibrary, EntryPoint = "tnr_chart_set_series_style",
            CallingConvention = CallingConvention.Cdecl)]
        internal static extern int SetSeriesStyle(
            ChartHandle chart,
            uint seriesId,
            float r, float g, float b, float a,
            float thickness,
            int visible);

        [DllImport(NativeLibrary, EntryPoint = "tnr_chart_set_series_y_range",
            CallingConvention = CallingConvention.Cdecl)]
        internal static extern int SetSeriesYRange(
            ChartHandle chart, uint seriesId, double minimum, double maximum);

        [DllImport(NativeLibrary, EntryPoint = "tnr_chart_replace_points",
            CallingConvention = CallingConvention.Cdecl)]
        internal static extern unsafe int ReplacePoints(
            ChartHandle chart, uint seriesId, ChartPoint* points, nuint count);

        [DllImport(NativeLibrary, EntryPoint = "tnr_chart_append_points",
            CallingConvention = CallingConvention.Cdecl)]
        internal static extern unsafe int AppendPoints(
            ChartHandle chart, uint seriesId, ChartPoint* points, nuint count);

        [DllImport(NativeLibrary, EntryPoint = "tnr_chart_clear_series",
            CallingConvention = CallingConvention.Cdecl)]
        internal static extern int ClearSeries(ChartHandle chart, uint seriesId);

        [DllImport(NativeLibrary, EntryPoint = "tnr_chart_render",
            CallingConvention = CallingConvention.Cdecl)]
        internal static extern int Render(ChartHandle chart);

        [DllImport(NativeLibrary, EntryPoint = "tnr_chart_get_diagnostics",
            CallingConvention = CallingConvention.Cdecl)]
        internal static extern int GetDiagnostics(
            ChartHandle chart, out NativeDiagnostics diagnostics);

        [DllImport(NativeLibrary, EntryPoint = "tnr_chart_copy_last_error",
            CallingConvention = CallingConvention.Cdecl)]
        internal static extern nuint CopyLastError(
            ChartHandle chart, StringBuilder buffer, nuint size);

        [DllImport(NativeLibrary, EntryPoint = "tnr_chart_copy_adapter_name",
            CallingConvention = CallingConvention.Cdecl)]
        internal static extern nuint CopyAdapterName(
            ChartHandle chart, StringBuilder buffer, nuint size);
    }

    private readonly ChartHandle _handle;
    private bool _disposed;

    public NativeChartRenderer()
    {
        _handle = NativeMethods.Create();
        if (_handle.IsInvalid)
        {
            _handle.Dispose();
            throw new InvalidOperationException("The D3D11 chart renderer could not be created.");
        }
    }

    public ICompositionSurface Attach(Compositor compositor)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        var abi = WinRT.MarshalInspectable<Compositor>.FromManaged(compositor);
        nint surfaceAbi = 0;
        try
        {
            Check(
                NativeMethods.Attach(_handle, abi, out surfaceAbi),
                "attach the composition surface");
            return WinRT.MarshalInterface<ICompositionSurface>.FromAbi(surfaceAbi);
        }
        finally
        {
            if (surfaceAbi != 0)
            {
                WinRT.MarshalInterface<ICompositionSurface>.DisposeAbi(surfaceAbi);
            }
            WinRT.MarshalInspectable<Compositor>.DisposeAbi(abi);
        }
    }

    public void Resize(uint width, uint height, float scale) =>
        Check(NativeMethods.Resize(_handle, width, height, scale), "resize");

    public void SetBackground(Color color)
    {
        var (r, g, b, a) = Components(color, 1);
        NativeMethods.SetBackground(_handle, r, g, b, a);
    }

    public void SetGridColor(Color color)
    {
        var (r, g, b, a) = Components(color, 1);
        NativeMethods.SetGridColor(_handle, r, g, b, a);
    }

    public unsafe void SetXRange(double minimum, double maximum)
    {
        Check(NativeMethods.SetXRange(_handle, minimum, maximum), "set X range");
    }

    public unsafe void SetVerticalGrid(ReadOnlySpan<double> values)
    {
        fixed (double* pointer = values)
        {
            Check(
                NativeMethods.SetVerticalGrid(
                    _handle, pointer, checked((nuint)values.Length)),
                "set vertical grid");
        }
    }

    public uint AddSeries(ChartLineSeriesOptions options)
    {
        var (r, g, b, a) = Components(options.Stroke, options.Opacity);
        var id = NativeMethods.AddSeries(
            _handle, r, g, b, a, options.Thickness,
            checked((nuint)Math.Max(0, options.MaximumPointCount)),
            options.MaximumXSpan);
        if (id == 0)
        {
            throw Error("add series");
        }
        return id;
    }

    public void RemoveSeries(uint id) =>
        Check(NativeMethods.RemoveSeries(_handle, id), "remove series");

    public void SetSeriesStyle(
        uint id, Color stroke, float opacity, float thickness, bool visible)
    {
        var (r, g, b, a) = Components(stroke, opacity);
        Check(
            NativeMethods.SetSeriesStyle(
                _handle, id, r, g, b, a, thickness, visible ? 1 : 0),
            "set series style");
    }

    public void SetSeriesYRange(uint id, double minimum, double maximum) =>
        Check(
            NativeMethods.SetSeriesYRange(_handle, id, minimum, maximum),
            "set series Y range");

    public unsafe void Replace(uint id, ReadOnlySpan<ChartPoint> points)
    {
        fixed (ChartPoint* pointer = points)
        {
            Check(
                NativeMethods.ReplacePoints(
                    _handle, id, pointer, checked((nuint)points.Length)),
                "replace series points");
        }
    }

    public unsafe void Append(uint id, ReadOnlySpan<ChartPoint> points)
    {
        fixed (ChartPoint* pointer = points)
        {
            Check(
                NativeMethods.AppendPoints(
                    _handle, id, pointer, checked((nuint)points.Length)),
                "append series points");
        }
    }

    public void Clear(uint id) =>
        Check(NativeMethods.ClearSeries(_handle, id), "clear series");

    public ChartDiagnostics Render()
    {
        Check(NativeMethods.Render(_handle), "render");
        Check(NativeMethods.GetDiagnostics(_handle, out var value), "read diagnostics");
        return new ChartDiagnostics(
            ReadString(NativeMethods.CopyAdapterName),
            checked((long)value.SourcePoints),
            checked((long)value.SubmittedSegments),
            value.FrameMilliseconds,
            value.UsedReduction != 0,
            value.UsingWarp != 0);
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }
        _disposed = true;
        _handle.Dispose();
        GC.SuppressFinalize(this);
    }

    private static (float R, float G, float B, float A) Components(
        Color color, float opacity) =>
        (color.R / 255f, color.G / 255f, color.B / 255f,
            color.A / 255f * Math.Clamp(opacity, 0, 1));

    private void Check(int result, string operation)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        if (result == 0)
        {
            throw Error(operation);
        }
    }

    private InvalidOperationException Error(string operation)
    {
        var message = ReadString(NativeMethods.CopyLastError);
        return new InvalidOperationException(
            string.IsNullOrWhiteSpace(message)
                ? $"The chart renderer could not {operation}."
                : $"The chart renderer could not {operation}: {message}");
    }

    private delegate nuint CopyString(ChartHandle handle, StringBuilder buffer, nuint size);

    private string ReadString(CopyString copy)
    {
        var buffer = new StringBuilder(512);
        var required = copy(_handle, buffer, checked((nuint)buffer.Capacity));
        if (required < (nuint)buffer.Capacity)
        {
            return buffer.ToString();
        }
        buffer = new StringBuilder(checked((int)required + 1));
        copy(_handle, buffer, checked((nuint)buffer.Capacity));
        return buffer.ToString();
    }
}
