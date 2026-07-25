using TrackNRace.WinUI3;

if (args.Length != 1)
{
    Console.Error.WriteLine("Usage: WinUISeekRegression <recording.tnrd>");
    return 2;
}

using var engine = new TelemetryEngine();
using var store = new TelemetrySessionStore(engine);

var seekResets = 0;
var snapshotNotifications = 0;
var telemetryNotifications = 0;
var powerNotifications = 0;
store.TimelineReset += reason =>
{
    if (reason == TimelineResetReason.Seek)
    {
        Interlocked.Increment(ref seekResets);
    }
};
store.SnapshotChanged += () => Interlocked.Increment(ref snapshotNotifications);
store.TelemetryChanged += () => Interlocked.Increment(ref telemetryNotifications);
store.PowerChanged += () => Interlocked.Increment(ref powerNotifications);

if (!engine.TryLoadRecording(Path.GetFullPath(args[0]), out var error))
{
    Console.Error.WriteLine(error);
    return 1;
}

await WaitUntil(
    () => engine.CurrentPlaybackState is { TotalTime: > 0 },
    "initial playback state");

ResetCounters();
engine.Seek(0.50f);
await WaitForSeek(0.50f);
ValidateCommittedState(0.50f);
Require(seekResets == 1, $"single seek committed {seekResets} resets");
Require(powerNotifications < 20,
    $"single seek published {powerNotifications} power updates instead of one transaction");

ResetCounters();
engine.Seek(0.20f);
engine.Seek(0.80f);
engine.Seek(0.35f);
await WaitForSeek(0.35f);
ValidateCommittedState(0.35f);
Require(seekResets <= 2,
    $"coalesced seek burst committed {seekResets} intermediate timelines");
Require(powerNotifications < 40,
    $"coalesced seek burst published {powerNotifications} power updates");

engine.Play();
await WaitUntil(
    () => engine.CurrentPlaybackState is { IsPlaying: true },
    "playback to start");
ResetCounters();
engine.Seek(0.60f);
engine.Seek(0.10f);
engine.Seek(0.70f);
await WaitUntil(() =>
{
    var state = engine.CurrentPlaybackState;
    var requested = state?.TotalTime * 0.70f ?? 0;
    return state is not null &&
        state.CurrentTime >= requested &&
        state.CurrentTime < requested + 1;
}, "playing seek to 70%");
engine.Pause();
await WaitUntil(
    () => engine.CurrentPlaybackState is { IsPlaying: false },
    "playback to pause");
ValidateCommittedPlayhead();
Require(seekResets <= 2,
    $"playing seek burst committed {seekResets} intermediate timelines");
Require(powerNotifications < 40,
    $"playing seek burst published {powerNotifications} power updates");

Console.WriteLine(
    $"PASS: final seek={engine.CurrentPlaybackState!.CurrentTime:F3}s, " +
    $"resets={seekResets}, snapshots={snapshotNotifications}, " +
    $"telemetry={telemetryNotifications}, power={powerNotifications}");
return 0;

void ResetCounters()
{
    Interlocked.Exchange(ref seekResets, 0);
    Interlocked.Exchange(ref snapshotNotifications, 0);
    Interlocked.Exchange(ref telemetryNotifications, 0);
    Interlocked.Exchange(ref powerNotifications, 0);
}

async Task WaitForSeek(float percentage)
{
    await WaitUntil(() =>
    {
        var state = engine.CurrentPlaybackState;
        return state is not null &&
            Math.Abs(state.CurrentTime - state.TotalTime * percentage) < 0.1f;
    }, $"seek to {percentage:P0}");
}

void ValidateCommittedState(float percentage)
{
    var state = engine.CurrentPlaybackState
        ?? throw new InvalidOperationException("Playback state disappeared.");
    var target = state.StartTime + state.TotalTime * percentage;
    ValidateStoreAt(target);
    Require(engine.LatestSessionTime is float clock &&
        Math.Abs(clock - target) < 0.1f,
        "session clock is not sourced from the committed playback state");
}

void ValidateCommittedPlayhead()
{
    var state = engine.CurrentPlaybackState
        ?? throw new InvalidOperationException("Playback state disappeared.");
    var target = state.StartTime + state.CurrentTime;
    ValidateStoreAt(target);
    Require(engine.LatestSessionTime is float clock &&
        Math.Abs(clock - target) < 0.1f,
        "session clock diverged from the active playback state");
}

void ValidateStoreAt(float target)
{
    var overview = store.OverviewSnapshot;
    var telemetry = store.ReadTelemetry(-1, -1, -1);

    Require(telemetry.Samples.Length > 0, "seek produced no telemetry");
    Require(telemetry.PlaybackLapNumber is > 0, "seek resolved no current lap");
    Require(telemetry.PlaybackLapStart is float lapStart && lapStart <= target,
        "seek resolved an invalid lap boundary");
    Require(telemetry.Samples[^1].SessionTime <= target + 0.1f,
        "telemetry advanced beyond the committed playhead");
    Require(overview.Status is null || overview.Status.SessionTime <= target + 0.1f,
        "status advanced beyond the committed playhead");
    Require(overview.Damage is null || overview.Damage.SessionTime <= target + 0.1f,
        "damage advanced beyond the committed playhead");
    Require(overview.Lap is null ||
        overview.Lap.LapNum == telemetry.PlaybackLapNumber,
        "displayed lap does not match the seek-resolved lap");
}

static async Task WaitUntil(Func<bool> condition, string operation)
{
    using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(15));
    try
    {
        while (!condition())
        {
            await Task.Delay(10, timeout.Token);
        }
    }
    catch (OperationCanceledException)
    {
        throw new TimeoutException($"Timed out waiting for {operation}.");
    }
}

static void Require(bool condition, string message)
{
    if (!condition)
    {
        throw new InvalidOperationException(message);
    }
}
