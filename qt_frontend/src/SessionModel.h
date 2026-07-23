#pragma once

#include <QObject>
#include <QVector>
#include <QTimer>
#include <climits>

// Slim per-sample records — only what the Speed/RPM/ERS chart needs.
struct TelSample { float t = 0; float speed = 0; int rpm = 0; int gear = 0; float throttle = 0; float brake = 0; float steering = 0; };

struct TyreSample {
    float t = 0;
    float surfFl=0, surfFr=0, surfRl=0, surfRr=0;
    float innerFl=0, innerFr=0, innerRl=0, innerRr=0;
    float brakeFl=0, brakeFr=0, brakeRl=0, brakeRr=0;
    float wearFl=0,  wearFr=0,  wearRl=0,  wearRr=0;
};
struct StsSample {
    float t = 0;
    float ers = 0;
    float fuel_kg = 0;
    float ice_kw = 0;
    float mguk_kw = 0;
    float mguk_harvest_j = 0;
    float mguh_harvest_j = 0;
    int tyre_compound = 0;
    int visual_compound = 0;
};
struct DamageSample { float t = 0; float wearFl=0, wearFr=0, wearRl=0, wearRr=0; };
struct MotionSample { float t = 0; float g_lat = 0; float g_long = 0; };
struct MotionExSample { float t = 0; float front_aero = 0; float rear_aero = 0; };

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
    QVector<MotionSample> motionBuf;
    QVector<MotionExSample> motionExBuf;
    QVector<TyreSample> tyreBuf;
    QVector<DamageSample> damageBuf;

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
    void onStatus(float t, float ers, float fuel_kg, float ice_kw, float mguk_kw, float mguk_harvest_j, float mguh_harvest_j,
                  int tyre_compound = 0, int visual_compound = 0);
    void onDamage(float t, float wearFl, float wearFr, float wearRl, float wearRr);
    void onMotion(float t, float g_lat, float g_long);
    void onMotionEx(float t, float front_aero, float rear_aero);
    void onTyre(float t,
                float surfFl, float surfFr, float surfRl, float surfRr,
                float innerFl, float innerFr, float innerRl, float innerRr,
                float brakeFl, float brakeFr, float brakeRl, float brakeRr,
                float wearFl,  float wearFr,  float wearRl,  float wearRr);
    void onLap(int lapNum, int currentLapMs, int lastLapMs, bool invalid);
    void onSessionReset(float newTime);          // time went backward → fresh session
    void truncateAfter(float newTime);            // in-game rewind → drop samples newer than newTime
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
    void onStatus(float t, float ers, float fuel_kg, float ice_kw, float mguk_kw, float mguk_harvest_j, float mguh_harvest_j,
                  int tyre_compound = 0, int visual_compound = 0);
    void onDamage(float t, float wearFl, float wearFr, float wearRl, float wearRr);
    void onMotion(float t, float g_lat, float g_long);
    void onMotionEx(float t, float front_aero, float rear_aero);
    void onTyre(float t,
                float surfFl, float surfFr, float surfRl, float surfRr,
                float innerFl, float innerFr, float innerRl, float innerRr,
                float brakeFl, float brakeFr, float brakeRl, float brakeRr,
                float wearFl,  float wearFr,  float wearRl,  float wearRr);
    void onLap(int lapNum, int currentLapMs, int lastLapMs, bool invalid);
    void onSessionReset(float newTime);
    void truncateAfter(float newTime);
    void clear();

    // Playback: replace everything with a pre-scanned session in one shot.
    void load(SessionData&& data);

    // Pause/resume the per-frame signal flush without affecting ingest. When paused
    // (window hidden/minimized), onTelemetry() etc. keep accumulating samples but
    // no telemetryAppended() is emitted, so charts don't repaint. Resuming forces
    // one flush so charts rebuild from the full retained history (no gap).
    void setLiveFlushActive(bool on);

    const SessionData& data() const { return d_; }

signals:
    void telemetryAppended();   // coalesced per event-loop pass — default/current-lap views refresh
    void tyreAppended();        // coalesced per event-loop pass — tyre chart views refresh
    void lapsChanged();         // a lap completed or a session loaded — selectors refresh
    void wasReset();

private:
    // Coalesces the several per-packet ingest setters into a single
    // telemetryAppended()/tyreAppended() emission per event-loop pass (once per
    // arriving packet — 20..60 Hz — with no fixed cap). See scheduleFlush().
    void scheduleFlush();

    SessionData d_;
    bool        telemetryDirty_ = false;
    bool        tyreDirty_      = false;
    bool        flushScheduled_ = false;   // a singleShot(0) flush is already queued
    bool        flushActive_    = true;    // emission enabled (paused when hidden)
};
