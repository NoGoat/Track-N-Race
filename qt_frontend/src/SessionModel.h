#pragma once

#include <QObject>
#include <QVector>
#include <QTimer>
#include <QHash>
#include <climits>
#include <cstdint>
#include <tnrp/control_rows.h>
#include "ChartSettings.h"

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
    int tyre_age_laps = 0;
};
struct DamageSample { float t = 0; float wearFl=0, wearFr=0, wearRl=0, wearRr=0; };
struct MotionSample { float t = 0; float g_lat = 0; float g_long = 0; };
struct MotionExSample { float t = 0; float front_aero = 0; float rear_aero = 0; };
struct LapProgressSample { float t = 0; int currentLapMs = 0; float distanceM = 0; int sector = 0; };

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
    QVector<TyreSample> tyre;
    QVector<DamageSample> damage;
    QVector<MotionSample> motion;
    QVector<MotionExSample> motionEx;
    QVector<LapProgressSample> progress;
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
    float trackLengthM  = 0;
    float currentStintStartTime = 0;
    bool  trimBuffers   = true;    // source histories use Electron's 750,000-row cap

    void onTelemetry(float t, float speed, int rpm, int gear, float throttle, float brake, float steering);
    void onStatus(float t, float ers, float fuel_kg, float ice_kw, float mguk_kw, float mguk_harvest_j, float mguh_harvest_j,
                  int tyre_compound = 0, int visual_compound = 0, int tyre_age_laps = 0);
    void onDamage(float t, float wearFl, float wearFr, float wearRl, float wearRr);
    void onMotion(float t, float g_lat, float g_long);
    void onMotionEx(float t, float front_aero, float rear_aero);
    void onTyre(float t,
                float surfFl, float surfFr, float surfRl, float surfRr,
                float innerFl, float innerFr, float innerRl, float innerRr,
                float brakeFl, float brakeFr, float brakeRl, float brakeRr,
                float wearFl,  float wearFr,  float wearRl,  float wearRr);
    void onLap(int lapNum, int currentLapMs, int lastLapMs, bool invalid,
               int driverStatus = -1, bool timedSession = false,
               float sessionTime = -1, float lapDistanceM = 0, int sector = 0, int pitStatus = 0);
    void onSessionReset(float newTime);          // time went backward → fresh session
    void truncateAfter(float newTime);            // in-game rewind → drop samples newer than newTime
    void finalizeOpenLap();                       // push the trailing in-progress lap
    void finishIngestBatch();                     // prune rolling buffers once after a producer batch
    void clear();

    const LapBlock* lapByNum(int n) const;
    LapBlock* lapByNum(int n);
    const LapBlock* fastestLap() const { return lapByNum(fastestLapNum); }
    const LapBlock* lapAtTime(float t) const;     // lap whose [start,end] spans t
    LapBlock* lapAtTime(float t);
    const LapBlock* currentLapAt(float t) const;
    const LapBlock* previousLap(const LapBlock* current) const;
    double distanceAtTime(const LapBlock* lap, float t) const;
    double timeAtDistance(const LapBlock* lap, double distance) const;
    QVector<LapProgressSample> sectorSplits(const LapBlock* lap) const;

private:
    void finalizeCurrentLap(int lastLapMs);
    void trim();
};

// QObject wrapper: the chart and lap selectors hold a pointer to this, query it,
// and refresh on its signals. Multiple row families from one packet are
// coalesced, but the model does not impose a fixed telemetry-rate throttle.
class SessionModel : public QObject {
    Q_OBJECT
public:
    explicit SessionModel(QObject* parent = nullptr);

    // Live ingest.
    void beginIngestBatch();
    void endIngestBatch();
    void onTelemetry(float t, float speed, int rpm, int gear, float throttle, float brake, float steering);
    void onStatus(float t, float ers, float fuel_kg, float ice_kw, float mguk_kw, float mguk_harvest_j, float mguh_harvest_j,
                  int tyre_compound = 0, int visual_compound = 0, int tyre_age_laps = 0);
    void onDamage(float t, float wearFl, float wearFr, float wearRl, float wearRr);
    void onMotion(float t, float g_lat, float g_long);
    void onMotionEx(float t, float front_aero, float rear_aero);
    void onTyre(float t,
                float surfFl, float surfFr, float surfRl, float surfRr,
                float innerFl, float innerFr, float innerRl, float innerRr,
                float brakeFl, float brakeFr, float brakeRl, float brakeRr,
                float wearFl,  float wearFr,  float wearRl,  float wearRr);
    void onLap(int lapNum, int currentLapMs, int lastLapMs, bool invalid,
               int driverStatus = -1, bool timedSession = false,
               float sessionTime = -1, float lapDistanceM = 0, int sector = 0, int pitStatus = 0);
    void onSessionReset(float newTime);
    void truncateAfter(float newTime);
    void clear();

    // Playback control metadata and generation-safe history installation. The
    // catalog is small and complete at load; detailed family histories remain
    // bounded and are populated only when requested by a visible consumer.
    void setPlaybackCatalog(const tnrp::PlaybackLapBlocksRow& catalog);
    void installPlaybackHistory(SessionData&& data,
                                QVector<LapProgressSample>&& progress,
                                QVector<LapBlock>&& lapDetails,
                                bool authoritative, bool isolatedLapRequest,
                                uint32_t rowTypeMask,
                                float historyStart, int requestedLapNum);
    void retainPlaybackHistoryMask(uint32_t rowTypeMask);
    uint32_t missingPlaybackLapMask(int lapNum, uint32_t rowTypeMask) const;
    const LapBlock* playbackLapData(int lapNum) const;
    const LapBlock* chartPrimaryLap(float sessionTime) const;
    const LapBlock* chartReferenceLap(ChartWindow window, int selectedLap,
                                      float sessionTime) const;
    uint64_t playbackDataRevision() const { return playbackDataRevision_; }

    // Pause/resume the per-frame signal flush without affecting ingest. When paused
    // (window hidden/minimized), onTelemetry() etc. keep accumulating samples but
    // no telemetryAppended() is emitted, so charts don't repaint. Resuming forces
    // one flush so charts rebuild from the full retained history (no gap).
    void setLiveFlushActive(bool on);

    const SessionData& data() const { return d_; }

    // Shared ordinary-chart configuration. Per-chart selectors always show their
    // effective value, matching Electron: choosing the global value removes the
    // internal override, and changing a global selector resets every graph to it.
    ChartWindow globalChartWindow() const { return globalWindow_; }
    ChartWindow effectiveChartWindow(tnr::GraphSection section) const;
    void setGlobalChartWindow(ChartWindow window);
    void setChartWindow(tnr::GraphSection section, ChartWindow window);
    int referenceLap(tnr::GraphSection section) const;
    int globalReferenceLap() const { return globalReferenceLap_; }
    void setGlobalReferenceLap(int lapNum);
    void setReferenceLap(tnr::GraphSection section, int lapNum);
    bool playbackMode() const { return playbackMode_; }
    bool playbackCatalogHasLapDistance() const { return playbackLapDistanceAvailable_; }
    // Live always exposes lap-relative modes. For playback, mirror Electron's
    // legacy-recording gate: require an actual lap-distance coordinate stream.
    bool lapCoordinatesAvailable() const;
    void setPlaybackMode(bool on);
    bool sectorBoundaries() const { return sectorBoundaries_; }
    void setSectorBoundaries(bool on);
    bool cursorSync() const { return cursorSync_; }
    void setCursorSync(bool on);
    bool secondaryVerticalCrosshair() const { return secondaryVertical_; }
    bool secondaryHorizontalCrosshair() const { return secondaryHorizontal_; }
    void setSecondaryCrosshairs(bool vertical, bool horizontal);
    bool dynamicYAxis(tnr::GraphSection section) const;
    void setDynamicYAxis(tnr::GraphSection section, bool dynamic);

signals:
    void telemetryAppended();   // coalesced per event-loop pass — default/current-lap views refresh
    void tyreAppended();        // coalesced per event-loop pass — tyre chart views refresh
    void lapsChanged();         // a lap completed or a session loaded — selectors refresh
    void wasReset();
    void chartConfigurationChanged();
private:
    // Coalesces the several per-packet ingest setters into a single
    // telemetryAppended()/tyreAppended() emission per event-loop pass (once per
    // arriving packet — 20..60 Hz — with no fixed cap). See scheduleFlush().
    void scheduleFlush();
    void touchPlaybackLap(int lapNum);

    SessionData d_;
    QVector<LapBlock> playbackCatalogLaps_;
    // Electron keeps these three stores independent. Catalog laps own only
    // immutable selector/boundary metadata; active masks describe seek/window
    // history installed into d_.laps; the indexed cache owns complete requested
    // laps used by Selected/Previous/Fastest and Analyze.
    QHash<int, uint32_t> playbackActiveLapMasks_;
    QHash<int, LapBlock> playbackLapDataCache_;
    QHash<int, uint32_t> playbackLapDataMasks_;
    QVector<int> playbackLapLru_;
    uint32_t playbackHistoryMask_ = 0;
    uint32_t playbackRequestedHistoryMask_ = 0;
    uint64_t playbackDataRevision_ = 0;
    bool        telemetryDirty_ = false;
    bool        tyreDirty_      = false;
    bool        flushScheduled_ = false;   // a singleShot(0) flush is already queued
    int         ingestBatchDepth_ = 0;
    bool        flushActive_    = true;    // emission enabled (paused when hidden)
    ChartWindow globalWindow_ = ChartWindow::Seconds30;
    QVector<int> windowOverrides_;          // -1 = global value, otherwise ChartWindow
    QVector<int> referenceLaps_;            // 0 = global reference lap
    int globalReferenceLap_ = 0;
    QVector<bool> dynamicYAxes_;
    bool playbackMode_ = false;
    bool playbackLapDistanceAvailable_ = false;
    bool sectorBoundaries_ = false;
    bool cursorSync_ = false;
    bool secondaryVertical_ = true;
    bool secondaryHorizontal_ = false;
};
