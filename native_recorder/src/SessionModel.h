#pragma once

#include <QObject>
#include <QVector>
#include <QTimer>
#include <climits>

// Slim per-sample records — only what the Speed/RPM/ERS chart needs.
struct TelSample { float t = 0; float speed = 0; int rpm = 0; int gear = 0; float throttle = 0; float brake = 0; float steering = 0; };
struct StsSample { float t = 0; float ers = 0; };

// One lap's telemetry/status, plus its timing. Mirrors the Electron
// SpeedRpmLapBlock used by the chart's per-lap and comparison modes.
struct LapBlock {
    int   lapNum = 0;
    float startSessionTime = 0;
    float endSessionTime   = 0;
    int   lapTimeMs = 0;          // last_lap_ms reported when the lap completed
    bool  invalid   = false;
    QVector<TelSample> tel;
    QVector<StsSample> sts;
};

// Plain (non-QObject) core of the session: rolling buffers, per-lap blocks,
// fastest-lap tracking, and the lap-segmentation state machine. Shared by both
// feeders — SessionModel mutates one of these live, and TnrdPlayer builds one on
// its background scan thread — so the lap-detection logic lives in exactly one
// place. Ported from useTelemetry.ts / sessionPlayer.ts.
struct SessionData {
    // Default-view buffers. Live: trimmed to the last ~10 min. Playback: the
    // whole session (so a window can end anywhere on the slider).
    QVector<TelSample> telBuf;
    QVector<StsSample> stsBuf;

    QVector<LapBlock>  laps;       // completed (and, after load, the final) laps
    LapBlock           curLap;     // in-progress lap (live only)
    int   curLapNum     = -1;
    float lapStartTime  = 0;

    int   fastestLapNum = -1;
    int   fastestLapMs  = INT_MAX;
    float latestTime    = 0;
    bool  trimBuffers   = true;    // live caps to 10 min; playback keeps everything

    static constexpr float BUFFER_S = 600.0f;   // 10 min, matches Electron MAX_ROWS

    void onTelemetry(float t, float speed, int rpm, int gear, float throttle, float brake, float steering);
    void onStatus(float t, float ers);
    void onLap(int lapNum, int currentLapMs, int lastLapMs, bool invalid);
    void onSessionReset(float newTime);          // time went backward → fresh session
    void finalizeOpenLap();                       // push the trailing in-progress lap
    void clear();

    const LapBlock* lapByNum(int n) const;
    const LapBlock* fastestLap() const { return lapByNum(fastestLapNum); }
    const LapBlock* lapAtTime(float t) const;     // lap whose [start,end] spans t

private:
    void finalizeCurrentLap(int lastLapMs);
    void trim();
};

// QObject wrapper: the chart and lap selectors hold a pointer to this, query it,
// and refresh on its signals. Live ingest is throttled to ~30 Hz so the chart
// doesn't rebuild its series 60× a second.
class SessionModel : public QObject {
    Q_OBJECT
public:
    explicit SessionModel(QObject* parent = nullptr);

    // Live ingest.
    void onTelemetry(float t, float speed, int rpm, int gear, float throttle, float brake, float steering);
    void onStatus(float t, float ers);
    void onLap(int lapNum, int currentLapMs, int lastLapMs, bool invalid);
    void onSessionReset(float newTime);
    void clear();

    // Playback: replace everything with a pre-scanned session in one shot.
    void load(SessionData&& data);

    const SessionData& data() const { return d_; }

signals:
    void telemetryAppended();   // throttled — default/current-lap views refresh
    void lapsChanged();         // a lap completed or a session loaded — selectors refresh
    void wasReset();

private:
    SessionData d_;
    QTimer*     flush_ = nullptr;
    bool        telemetryDirty_ = false;
};
