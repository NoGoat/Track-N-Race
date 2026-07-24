using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using Microsoft.Win32.SafeHandles;

namespace TrackNRace.WinUI3;

public enum TelemetryProtocol
{
    Auto = 0,
    F1_24 = 1,
    F1_25 = 2,
    F1_26 = 3,
}

// The span points into libtnrp-owned memory and is valid only for the duration
// of the callback. Consumers should decode it synchronously, not retain it.
public delegate void BinaryBatchHandler(ReadOnlySpan<byte> data);

public sealed record PlaybackState(
    bool IsPlaying,
    float CurrentTime,
    float TotalTime,
    float Speed,
    float StartTime);

public sealed record PlaybackLap(int LapNumber, float StartSessionTime, float EndSessionTime);

public sealed class TelemetryEngine : IDisposable
{
    private const string NativeLibrary = "TrackNRace.EngineBridge";

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void RowCallback(nint json, nuint length, nint context);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void BinaryCallback(nint data, nuint length, nint context);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void SeekCallback(
        nint data,
        nuint length,
        nint coldJson,
        nuint coldJsonLength,
        float currentLapStart,
        int lapNumber,
        nint context);

    private sealed class EngineHandle : SafeHandleZeroOrMinusOneIsInvalid
    {
        private EngineHandle() : base(true)
        {
        }

        protected override bool ReleaseHandle()
        {
            NativeMethods.Destroy(handle);
            return true;
        }
    }

    private static class NativeMethods
    {
        [DllImport(NativeLibrary, EntryPoint = "tnr_engine_create",
            CallingConvention = CallingConvention.Cdecl)]
        internal static extern EngineHandle Create(
            ushort port,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string bindAddress,
            int protocol,
            RowCallback rowCallback,
            BinaryCallback binaryCallback,
            SeekCallback seekCallback,
            nint context);

        [DllImport(NativeLibrary, EntryPoint = "tnr_engine_destroy",
            CallingConvention = CallingConvention.Cdecl)]
        internal static extern void Destroy(nint handle);

        [DllImport(NativeLibrary, EntryPoint = "tnr_engine_start",
            CallingConvention = CallingConvention.Cdecl)]
        internal static extern int Start(EngineHandle handle);

        [DllImport(NativeLibrary, EntryPoint = "tnr_engine_restart_udp",
            CallingConvention = CallingConvention.Cdecl)]
        internal static extern int RestartUdp(
            EngineHandle handle,
            ushort port,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string bindAddress);

        [DllImport(NativeLibrary, EntryPoint = "tnr_engine_set_protocol",
            CallingConvention = CallingConvention.Cdecl)]
        internal static extern void SetProtocol(EngineHandle handle, int protocol);

        [DllImport(NativeLibrary, EntryPoint = "tnr_engine_set_logging",
            CallingConvention = CallingConvention.Cdecl)]
        internal static extern void SetLogging(
            EngineHandle handle,
            int enabled,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string outputDirectory);

        [DllImport(NativeLibrary, EntryPoint = "tnr_engine_player_load",
            CallingConvention = CallingConvention.Cdecl)]
        internal static extern int PlayerLoad(
            EngineHandle handle,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string path);

        [DllImport(NativeLibrary, EntryPoint = "tnr_engine_player_play",
            CallingConvention = CallingConvention.Cdecl)]
        internal static extern void PlayerPlay(EngineHandle handle);

        [DllImport(NativeLibrary, EntryPoint = "tnr_engine_player_pause",
            CallingConvention = CallingConvention.Cdecl)]
        internal static extern void PlayerPause(EngineHandle handle);

        [DllImport(NativeLibrary, EntryPoint = "tnr_engine_player_seek",
            CallingConvention = CallingConvention.Cdecl)]
        internal static extern void PlayerSeek(EngineHandle handle, float percentage);

        [DllImport(NativeLibrary, EntryPoint = "tnr_engine_player_set_speed",
            CallingConvention = CallingConvention.Cdecl)]
        internal static extern void PlayerSetSpeed(EngineHandle handle, float multiplier);

        [DllImport(NativeLibrary, EntryPoint = "tnr_engine_player_get_lap_data",
            CallingConvention = CallingConvention.Cdecl)]
        internal static extern void PlayerGetLapData(EngineHandle handle, int lapNumber);

        [DllImport(NativeLibrary, EntryPoint = "tnr_engine_player_close",
            CallingConvention = CallingConvention.Cdecl)]
        internal static extern void PlayerClose(EngineHandle handle);

        [DllImport(NativeLibrary, EntryPoint = "tnr_engine_export_xlsx",
            CallingConvention = CallingConvention.Cdecl)]
        internal static extern int ExportXlsx(
            EngineHandle handle,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string sourcePath,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string destinationPath);

        [DllImport(NativeLibrary, EntryPoint = "tnr_engine_copy_last_error",
            CallingConvention = CallingConvention.Cdecl)]
        internal static extern nuint CopyLastError(
            EngineHandle handle, StringBuilder buffer, nuint bufferSize);

        [DllImport(NativeLibrary, EntryPoint = "tnr_engine_copy_create_error",
            CallingConvention = CallingConvention.Cdecl)]
        internal static extern nuint CopyCreateError(StringBuilder buffer, nuint bufferSize);
    }

    // The delegates must remain rooted for as long as native engine threads can
    // call them. SafeHandle releases the engine before these fields can be collected.
    private readonly RowCallback _rowCallback;
    private readonly BinaryCallback _binaryCallback;
    private readonly SeekCallback _seekCallback;
    private readonly EngineHandle _handle;
    private long _rowCount;
    private long _binaryBatchCount;
    private int _sessionTimeBits = int.MinValue;
    private PlaybackState? _playbackState;
    private PlaybackLap[] _playbackLaps = [];
    private bool _disposed;

    public event Action<string>? RowReceived;
    public event BinaryBatchHandler? BinaryBatchReceived;
    public event Action<byte[], string, float, int>? SeekFlushReceived;

    public long RowCount => Interlocked.Read(ref _rowCount);
    public long BinaryBatchCount => Interlocked.Read(ref _binaryBatchCount);
    public float? LatestSessionTime
    {
        get
        {
            var bits = Volatile.Read(ref _sessionTimeBits);
            return bits == int.MinValue ? null : BitConverter.Int32BitsToSingle(bits);
        }
    }
    public string? ProtocolStatusJson { get; private set; }
    public PlaybackState? CurrentPlaybackState => Volatile.Read(ref _playbackState);
    public IReadOnlyList<PlaybackLap> PlaybackLaps => Volatile.Read(ref _playbackLaps);

    public TelemetryEngine(
        ushort port = 20777,
        string bindAddress = "0.0.0.0",
        TelemetryProtocol protocol = TelemetryProtocol.Auto)
    {
        _rowCallback = OnNativeRow;
        _binaryCallback = OnNativeBinary;
        _seekCallback = OnNativeSeek;
        _handle = NativeMethods.Create(
            port, bindAddress, (int)protocol,
            _rowCallback, _binaryCallback, _seekCallback, nint.Zero);

        if (_handle.IsInvalid)
        {
            _handle.Dispose();
            throw new InvalidOperationException(
                ReadError(NativeMethods.CopyCreateError) is { Length: > 0 } error
                    ? error
                    : "The libtnrp bridge could not be created.");
        }
    }

    public bool TryStart(out string error)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        if (NativeMethods.Start(_handle) != 0)
        {
            error = string.Empty;
            return true;
        }

        error = GetLastError();
        return false;
    }

    public bool TryRestartUdp(ushort port, string bindAddress, out string error)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        if (NativeMethods.RestartUdp(_handle, port, bindAddress) != 0)
        {
            error = string.Empty;
            return true;
        }

        error = GetLastError();
        return false;
    }

    public void SetProtocol(TelemetryProtocol protocol)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        NativeMethods.SetProtocol(_handle, (int)protocol);
    }

    public void SetRecording(bool enabled, string outputDirectory)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        NativeMethods.SetLogging(_handle, enabled ? 1 : 0, outputDirectory);
    }

    public bool TryLoadRecording(string path, out string error)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        if (NativeMethods.PlayerLoad(_handle, path) != 0)
        {
            error = string.Empty;
            return true;
        }

        error = GetLastError();
        return false;
    }

    public void Play() => NativeMethods.PlayerPlay(_handle);
    public void Pause() => NativeMethods.PlayerPause(_handle);
    public void Seek(float percentage) => NativeMethods.PlayerSeek(_handle, percentage);
    public void SetPlaybackSpeed(float multiplier) =>
        NativeMethods.PlayerSetSpeed(_handle, multiplier);
    public void RequestLapData(int lapNumber) =>
        NativeMethods.PlayerGetLapData(_handle, lapNumber);
    public void CloseRecording() => NativeMethods.PlayerClose(_handle);

    public bool TryExportRecording(
        string sourcePath, string destinationPath, out string error)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        if (NativeMethods.ExportXlsx(_handle, sourcePath, destinationPath) != 0)
        {
            error = string.Empty;
            return true;
        }

        error = GetLastError();
        return false;
    }

    public string GetLastError() =>
        ReadError((buffer, size) => NativeMethods.CopyLastError(_handle, buffer, size));

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

    private void OnNativeRow(nint json, nuint length, nint context)
    {
        try
        {
            var row = Marshal.PtrToStringUTF8(json, checked((int)length));
            if (row is null)
            {
                return;
            }

            Interlocked.Increment(ref _rowCount);
            UpdateRowMetadata(row);
            RowReceived?.Invoke(row);
        }
        catch
        {
            // Exceptions must never unwind through the native engine thread.
        }
    }

    private unsafe void OnNativeBinary(nint data, nuint length, nint context)
    {
        try
        {
            Interlocked.Increment(ref _binaryBatchCount);
            BinaryBatchReceived?.Invoke(
                new ReadOnlySpan<byte>(data.ToPointer(), checked((int)length)));
        }
        catch
        {
            // Exceptions must never unwind through the native engine thread.
        }
    }

    private void OnNativeSeek(
        nint data,
        nuint length,
        nint coldJson,
        nuint coldJsonLength,
        float currentLapStart,
        int lapNumber,
        nint context)
    {
        try
        {
            var bytes = new byte[checked((int)length)];
            if (bytes.Length > 0)
            {
                Marshal.Copy(data, bytes, 0, bytes.Length);
            }

            var rows = Marshal.PtrToStringUTF8(
                coldJson, checked((int)coldJsonLength)) ?? string.Empty;
            SeekFlushReceived?.Invoke(bytes, rows, currentLapStart, lapNumber);
        }
        catch
        {
            // Exceptions must never unwind through the native playback thread.
        }
    }

    private void UpdateRowMetadata(string json)
    {
        try
        {
            using var document = JsonDocument.Parse(json);
            var root = document.RootElement;
            if (root.TryGetProperty("session_time", out var sessionTime) &&
                sessionTime.TryGetSingle(out var seconds) &&
                seconds >= 0)
            {
                Volatile.Write(
                    ref _sessionTimeBits, BitConverter.SingleToInt32Bits(seconds));
            }

            if (root.TryGetProperty("type", out var type))
            {
                if (type.ValueEquals("protocol_status"))
                {
                    ProtocolStatusJson = json;
                }
                else if (type.ValueEquals("playback_state"))
                {
                    Volatile.Write(ref _playbackState, new PlaybackState(
                        root.GetProperty("playing").GetBoolean(),
                        root.GetProperty("current_time").GetSingle(),
                        root.GetProperty("total_time").GetSingle(),
                        root.GetProperty("speed").GetSingle(),
                        root.GetProperty("start_time").GetSingle()));
                }
                else if (type.ValueEquals("playback_lap_blocks"))
                {
                    var laps = new List<PlaybackLap>();
                    foreach (var block in root.GetProperty("blocks").EnumerateArray())
                    {
                        laps.Add(new PlaybackLap(
                            block.GetProperty("lapNum").GetInt32(),
                            block.GetProperty("startSessionTime").GetSingle(),
                            block.GetProperty("endSessionTime").GetSingle()));
                    }
                    Volatile.Write(ref _playbackLaps, laps.ToArray());
                }
                else if (type.ValueEquals("playback_close"))
                {
                    Volatile.Write(ref _sessionTimeBits, int.MinValue);
                    Volatile.Write(ref _playbackState, null);
                    Volatile.Write(ref _playbackLaps, []);
                }
            }
        }
        catch (JsonException)
        {
        }
    }

    private delegate nuint ErrorCopy(StringBuilder buffer, nuint size);

    private static string ReadError(ErrorCopy copy)
    {
        var buffer = new StringBuilder(2048);
        var required = copy(buffer, (nuint)buffer.Capacity);
        if (required < (nuint)buffer.Capacity)
        {
            return buffer.ToString();
        }

        buffer = new StringBuilder(checked((int)required + 1));
        copy(buffer, (nuint)buffer.Capacity);
        return buffer.ToString();
    }
}
