#include <napi.h>
#include <tnrp/AnalysisMap.h>
#include <tnrp/AnalysisMapPaint.h>
#include <tnrp/Capabilities.h>
#include <tnrp/DriverLapHistory.h>
#include <tnrp/Engine.h>
#include <tnrp/Labels.h>
#include <tnrp/CardColors.h>
#include <tnrp/LapDelta.h>
#include <tnrp/MergedLapTimeline.h>
#include <tnrp/NetworkSetup.h>
#include <tnrp/Updates.h>
#include <tnrp/TnrdReader.h>
#include <tnrp/TeamColors.h>
#include <tnrp/TrackMaps.h>
#include <tnrp/ParquetExport.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <deque>
#include <limits>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace Napi;

#define TRACE(msg) do { fprintf(stderr, "[native] " msg "\n"); fflush(stderr); } while (0)

template <typename T>
static void updateAtomicMaximum(std::atomic<T>& target, T value) {
    T current = target.load(std::memory_order_relaxed);
    while (current < value && !target.compare_exchange_weak(
        current, value, std::memory_order_relaxed, std::memory_order_relaxed)) {}
}

static tnrp::TeamColorOverrides readTeamColorOverrides(const Napi::Value& value) {
    tnrp::TeamColorOverrides result;
    if (!value.IsObject()) return result;
    Napi::Object formats = value.As<Napi::Object>();
    for (const uint16_t format : {2024, 2025, 2026}) {
        const std::string formatKey = std::to_string(format);
        Napi::Value teamsValue = formats.Get(formatKey);
        if (!teamsValue.IsObject()) continue;
        Napi::Object teams = teamsValue.As<Napi::Object>();
        Napi::Array ids = teams.GetPropertyNames();
        for (uint32_t i = 0; i < ids.Length(); ++i) {
            Napi::Value idValue = ids.Get(i);
            if (!idValue.IsString()) continue;
            const std::string idText = idValue.As<Napi::String>().Utf8Value();
            char* end = nullptr;
            const unsigned long parsed = std::strtoul(idText.c_str(), &end, 10);
            if (!end || *end != '\0' || parsed > 65535) continue;
            Napi::Value color = teams.Get(idValue);
            if (!color.IsString()) continue;
            result[format][static_cast<uint16_t>(parsed)] =
                color.As<Napi::String>().Utf8Value();
        }
    }
    return tnrp::sanitizeTeamColorOverrides(result);
}

// Runs the (potentially several-second) Parquet export off the JS thread via
// libuv's threadpool, resolving a Promise on completion. Progress (0..100)
// is reported back through AsyncProgressQueueWorker's queue, which safely
// marshals each Send() from the worker thread onto the JS thread as an
// OnProgress() call — that's where the JS progress callback is invoked.
// One progress tick carried across the threadpool→JS boundary: a percentage
// plus the human-readable stage message describing what the export is doing.
struct ExportTick {
    double pct;
    std::string stage;
};

class ExportParquetWorker : public Napi::AsyncProgressQueueWorker<ExportTick> {
public:
    ExportParquetWorker(Napi::Env env, std::string src, std::string dest, Napi::Function progressCb)
        : Napi::AsyncProgressQueueWorker<ExportTick>(env), src_(std::move(src)), dest_(std::move(dest)),
          progressCb_(Napi::Persistent(progressCb)),
          deferred_(Napi::Promise::Deferred::New(env)) {}

    void Execute(const ExecutionProgress& progress) override {   // libuv worker thread — no Napi/JS access here
        ok_ = tnrp::exportTnrdFileToParquet(src_, dest_, &folder_, &error_,
            [&progress](double fraction, const std::string& stage) {
                ExportTick tick;
                tick.pct = 100.0 * fraction;
                tick.stage = stage;
                progress.Send(&tick, 1);
            });
    }
    void OnProgress(const ExportTick* data, size_t count) override {   // back on the JS thread
        if (count == 0 || progressCb_.IsEmpty()) return;
        Napi::HandleScope scope(Env());
        const ExportTick& tick = data[count - 1];
        progressCb_.Call({ Napi::Number::New(Env(), tick.pct), Napi::String::New(Env(), tick.stage) });
    }
    void OnOK() override {
        Napi::HandleScope scope(Env());
        Napi::Object result = Napi::Object::New(Env());
        result.Set("ok", ok_);
        if (ok_) result.Set("folder", folder_);
        else result.Set("error", error_);
        deferred_.Resolve(result);
    }
    void OnError(const Napi::Error& e) override { deferred_.Reject(e.Value()); }
    Napi::Promise GetPromise() { return deferred_.Promise(); }

private:
    std::string src_, dest_, folder_, error_;
    bool ok_ = false;
    Napi::FunctionReference progressCb_;
    Napi::Promise::Deferred deferred_;
};

// Runs Engine::playerLoad (gzip decompress + full index scan — seconds for a
// long session) off the JS thread so the Electron main process stays
// responsive during the load, resolving Promise<{ok, error?}>. The engine is held
// by shared_ptr so a destroy() racing a pending load can't free it out from
// under the worker; `busy` serializes loads (a second concurrent load resolves
// with an explanatory failure instead of racing stopPlaybackThread).
class PlayerLoadWorker : public Napi::AsyncWorker {
public:
    PlayerLoadWorker(Napi::Env env, std::shared_ptr<tnrp::Engine> engine,
                     std::string path, std::shared_ptr<std::atomic<bool>> busy)
        : Napi::AsyncWorker(env), engine_(std::move(engine)), path_(std::move(path)),
          busy_(std::move(busy)), deferred_(Napi::Promise::Deferred::New(env)) {}

    void Execute() override {   // libuv worker thread — no Napi/JS access here
        ok_ = engine_->playerLoad(path_, &error_);
    }
    void OnOK() override {
        busy_->store(false);
        Napi::Object result = Napi::Object::New(Env());
        result.Set("ok", ok_);
        if (!ok_) result.Set("error", error_);
        deferred_.Resolve(result);
    }
    void OnError(const Napi::Error& e) override {
        busy_->store(false);
        deferred_.Reject(e.Value());
    }
    Napi::Promise GetPromise() { return deferred_.Promise(); }

private:
    std::shared_ptr<tnrp::Engine> engine_;
    std::string path_;
    std::shared_ptr<std::atomic<bool>> busy_;
    bool ok_ = false;
    std::string error_;
    Napi::Promise::Deferred deferred_;
};

// Indexed AL and finite-window prefixes are extracted on libuv's worker pool.
// Generation checks in Engine make rapid scrub requests monotonic even if
// workers begin out of order.
class PlayerSeekWorker : public Napi::AsyncWorker {
public:
    PlayerSeekWorker(Napi::Env env, std::shared_ptr<tnrp::Engine> engine,
                     float pct, bool allHistory, uint64_t requestId,
                     uint32_t rowTypeMask, float windowSeconds, double mergedTime)
        : Napi::AsyncWorker(env), engine_(std::move(engine)), pct_(pct),
          allHistory_(allHistory), requestId_(requestId), rowTypeMask_(rowTypeMask),
          windowSeconds_(windowSeconds), mergedTime_(mergedTime) {}
    void Execute() override { engine_->playerSeek(pct_, allHistory_, requestId_, rowTypeMask_, windowSeconds_, mergedTime_); }
private:
    std::shared_ptr<tnrp::Engine> engine_;
    float pct_;
    bool allHistory_;
    uint64_t requestId_;
    uint32_t rowTypeMask_;
    float windowSeconds_;
    double mergedTime_;
};

// A V6 driver change rebuilds the lap catalog and sector metadata from the new
// driver's lap data, which takes long enough to stall the Electron main thread.
// Resolves once the engine has switched; the host orders its follow-up seek
// behind that promise.
class PlayerSetDriverWorker : public Napi::AsyncWorker {
public:
    PlayerSetDriverWorker(Napi::Env env, std::shared_ptr<tnrp::Engine> engine,
                          int driverIndex, bool useRecordedRows)
        : Napi::AsyncWorker(env), engine_(std::move(engine)), driverIndex_(driverIndex),
          useRecordedRows_(useRecordedRows), deferred_(Napi::Promise::Deferred::New(env)) {}
    void Execute() override { engine_->playerSetDriver(driverIndex_, useRecordedRows_); }
    void OnOK() override { deferred_.Resolve(Env().Undefined()); }
    void OnError(const Napi::Error& e) override { deferred_.Reject(e.Value()); }
    Napi::Promise GetPromise() { return deferred_.Promise(); }
private:
    std::shared_ptr<tnrp::Engine> engine_;
    int driverIndex_;
    bool useRecordedRows_;
    Napi::Promise::Deferred deferred_;
};

// Runs a short engine call off the JS thread. Engine calls take the engine
// lock, which playback and seeks also hold, so the main process never waits on
// it. Any rows the call produces arrive through the usual row callback.
class EngineCallWorker : public Napi::AsyncWorker {
public:
    EngineCallWorker(Napi::Env env, std::function<void()> call)
        : Napi::AsyncWorker(env), call_(std::move(call)) {}
    void Execute() override { call_(); }
private:
    std::function<void()> call_;
};

// Engine calls whose order matters (play, pause, speed, lap requests,
// settings) take the same lock, so they cannot run on the JS thread either: a
// seek holds it for its whole extraction and a waiting main process freezes
// every window. EngineCallWorkers start in any order, so these share one
// drain worker that runs them one at a time, in the order they were made.
struct OrderedEngineCalls {
    std::mutex mutex;
    std::deque<std::function<void()>> pending;
    bool draining = false;
};

static void queueOrderedEngineCall(Napi::Env env, const std::shared_ptr<OrderedEngineCalls>& calls,
                                   std::function<void()> call) {
    {
        std::lock_guard<std::mutex> lock(calls->mutex);
        calls->pending.push_back(std::move(call));
        if (calls->draining) return;
        calls->draining = true;
    }
    (new EngineCallWorker(env, [calls] {
        for (;;) {
            std::function<void()> next;
            {
                std::lock_guard<std::mutex> lock(calls->mutex);
                if (calls->pending.empty()) { calls->draining = false; return; }
                next = std::move(calls->pending.front());
                calls->pending.pop_front();
            }
            try {
                next();
            } catch (...) {
                // The rest stay queued for the drain worker the next call starts.
                std::lock_guard<std::mutex> lock(calls->mutex);
                calls->draining = false;
                throw;
            }
        }
    }))->Queue();
}

class PlayerHistoryWorker : public Napi::AsyncWorker {
public:
    PlayerHistoryWorker(Napi::Env env, std::shared_ptr<tnrp::Engine> engine,
                        uint64_t requestId, uint32_t rowTypeMask,
                        float windowSeconds = 0.0f, bool windowRequest = false)
        : Napi::AsyncWorker(env), engine_(std::move(engine)), requestId_(requestId),
          rowTypeMask_(rowTypeMask), windowSeconds_(windowSeconds),
          windowRequest_(windowRequest) {}
    void Execute() override {
        if (windowRequest_) engine_->playerGetWindowData(windowSeconds_, requestId_, rowTypeMask_);
        else engine_->playerGetAllLapsData(requestId_, rowTypeMask_);
    }
private:
    std::shared_ptr<tnrp::Engine> engine_;
    uint64_t requestId_;
    uint32_t rowTypeMask_;
    float windowSeconds_;
    bool windowRequest_;
};

class DataRequirementsWorker : public Napi::AsyncWorker {
public:
    DataRequirementsWorker(Napi::Env env, std::shared_ptr<tnrp::Engine> engine,
                           uint32_t streamMask, uint32_t historyMask,
                           float windowSeconds, uint64_t requestId,
                           std::vector<uint8_t> v6Types,
                           std::vector<uint8_t> v6HistoryTypes)
        : Napi::AsyncWorker(env), engine_(std::move(engine)),
          streamMask_(streamMask), historyMask_(historyMask),
          windowSeconds_(windowSeconds), requestId_(requestId), v6Types_(std::move(v6Types)),
          v6HistoryTypes_(std::move(v6HistoryTypes)) {}
    void Execute() override {
        engine_->setDataRequirements(streamMask_, historyMask_, windowSeconds_,
                                     requestId_, v6Types_, v6HistoryTypes_);
    }
private:
    std::shared_ptr<tnrp::Engine> engine_;
    uint32_t streamMask_;
    uint32_t historyMask_;
    float windowSeconds_;
    uint64_t requestId_;
    std::vector<uint8_t> v6Types_;
    std::vector<uint8_t> v6HistoryTypes_;
};

// File B's rows at its moment on the lap-synced playhead, streamed the way the
// engine streams file A's (Engine::playbackLoop). The engine's merged clock
// sets where B is (Engine::setMergedPlayheadCallback); this keeps a playback
// cursor on its own handle on B's file, reads forward to there with
// pullUntilSplit() every 16 ms, and pushes what it read as one batch per tick.
//
// Like A's seek, a jump (backwards, more than a second ahead, a new driver, a
// restart, a longer history wanted) first sends a flush: the followed car's
// history to the new time as column blocks (the seek flush's format) and the
// newest row of every family at that time; the stream then carries on from
// there. The history starts at the followed car's lap, or, for the families
// B's longer windows show (time windows, Stint and All Laps, Trends), as far
// back as those need, as A's history covers its widest visible window.
// Rows: session, timing, participants and positions for every car; the
// followed car's telemetry, status, damage, lap, motion and tyre sets; and its
// completed laps with their ERS and fuel (lap_trends), at each new lap, as the
// engine pushes A's lap history.
class SecondaryStream {
public:
    struct Batch {
        std::string json;                 // newline-joined rows
        std::vector<uint8_t> binary;      // packed hot rows (decodeBinaryBatch)
        // The followed car's lap at the batch's end, and where it started.
        int lapNum = 0;
        float lapStart = 0.0f;
        float time = 0.0f;
        // A flush: the lap's history from lapStart to `time`, and the longer
        // families' from longFrom; the stream's rows carry on after it.
        bool flush = false;
        std::vector<uint8_t> history;     // column blocks; empty without V6 history
        std::vector<uint8_t> longHistory;
        float longFrom = 0.0f;
        // A recording without column history (before V6) flushes as A's seek
        // does for those: packed hot rows and the rest as JSON, both spans.
        std::vector<uint8_t> historyBinary;
        std::string historyJson;
    };

    SecondaryStream() : thread_([this] { loop(); }) {}
    ~SecondaryStream() {
        run_.store(false);
        if (thread_.joinable()) thread_.join();
        std::lock_guard<std::mutex> lock(callbackMutex_);
        if (hasCallback_) callback_.Release();
    }

    // On the load worker's thread, with B's analysis reader.
    void open(const std::string& path) {
        std::lock_guard<std::mutex> lock(mutex_);
        history_.reset();
        reader_.close();
        // Loaded as the engine loads file A in Electron (binaryPlayback and
        // sparseV6Playback), so pullUntilSplit() hands out the same rows A's
        // playback does: packed hot rows, sparse per-car patches.
        reader_.setBinaryPlayback(true);
        reader_.setSparseV6Playback(true);
        reader_.setPlaybackRowMask(kRowMask, 0.0f);
        tnrp::HeaderRow header;
        loaded_ = reader_.load(path, header);
        hasMguh_ = loaded_ && tnrp::hasMguh(tnrp::presentationFormat(
            header.protocol >= 2024 ? static_cast<uint16_t>(header.protocol) : 2025,
            header.regulations_2026, header.formula));
        if (!loaded_) {
            std::fprintf(stderr, "[analysis] second file stream did not open: %s\n", reader_.lastError().c_str());
        } else {
            std::string error;
            history_ = reader_.openHistoryReader(&error);
            if (!history_ && !error.empty())
                std::fprintf(stderr, "[analysis] second file stream has no column history: %s\n", error.c_str());
            if (driver_ >= 0) reader_.setPlaybackDriver(driver_, false, reader_.startTime());
        }
        cursor_ = NAN;
        trendsLap_ = -1;
        lastLapHistoryJson_.clear();
        lapHistoryForce_ = true;
        // A load starts with no focus car; the one selected is applied again.
        focusPending_ = true;
    }

    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        history_.reset();
        reader_.close();
        loaded_ = false;
        cursor_ = NAN;
        target_.store(NAN);
    }

    // B's session time at the playhead (the engine's merged clock).
    void setTarget(float t) { target_.store(std::isfinite(t) ? t : NAN); }

    // Whether a page shows B's streamed rows; the car followed in B (-1: its
    // recorded player); and the families B's longer windows show (row type
    // bits) with how far back (seconds; -1 the whole race; 0 none). Turning
    // it on, another car or another history starts with a flush.
    void configure(bool enabled, int driver, uint32_t longMask, float longSeconds) {
        std::lock_guard<std::mutex> lock(mutex_);
        longMask &= kHistoryMask;
        if (longMask == 0 || longSeconds == 0.0f) { longMask = 0; longSeconds = 0.0f; }
        if (driver != driver_) {
            driver_ = driver;
            if (loaded_) reader_.setPlaybackDriver(driver, false, std::isfinite(cursor_) ? cursor_ : reader_.startTime());
            cursor_ = NAN;
            trendsLap_ = -1;
        }
        if (longMask != longMask_ || longSeconds != longSeconds_) {
            longMask_ = longMask;
            longSeconds_ = longSeconds;
            cursor_ = NAN;
        }
        if (enabled && !enabled_) cursor_ = NAN;
        enabled_ = enabled;
    }

    // The car selected in B's Standings tower (-1 when none), as
    // Engine::playerSetFocusDriver for A: V6 playback reads a car's private
    // status (ERS, fuel, DRS, brake bias) only for the focus car; the rest of
    // the grid streams lap timing and tyres.
    void setFocusDriver(int driverIndex) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (driverIndex == focusDriver_) return;
        focusDriver_ = driverIndex;
        focusPending_ = true;
    }

    // The car whose lap-times view is open on B (-1 when none), as
    // Engine::setLapHistoryCar for A: its driver_lap_history row is pushed now
    // and again only when it changes.
    void setLapHistoryCar(int carIdx) {
        std::lock_guard<std::mutex> lock(mutex_);
        lapHistoryCar_ = carIdx >= 0 && carIdx < kCarSlots ? carIdx : -1;
        lastLapHistoryJson_.clear();
        lapHistoryForce_ = true;
    }

    void setCallback(Napi::Env env, Napi::Function fn) {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        if (hasCallback_) callback_.Release();
        callback_ = Napi::ThreadSafeFunction::New(env, fn, "analysisStream", 0, 1);
        callback_.Unref(env);
        hasCallback_ = true;
    }

private:
    // Row type bits (Engine.cpp rowTypeOf). The followed car's chart families
    // (telemetry, status, damage, lap, motion, motion_ex), flushed as history
    // on a jump:
    static constexpr uint32_t kHistoryMask = (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 11) | (1u << 12);
    // ...and session, timing, participants, every car's status (the
    // Standings tower's tyres), tyre sets, positions and each car's lap
    // history (session_history_fastest: the fastest-lap holder in recordings
    // without fastest_lap rows, as A's store finds it).
    static constexpr uint32_t kRowMask = kHistoryMask | (1u << 5) | (1u << 7) | (1u << 8) | (1u << 9) | (1u << 10) | (1u << 13) | (1u << 14);
    // Car slots read for the lap-times view, as the engine's (Engine::liveLapHistoryRows_).
    static constexpr int kCarSlots = 24;

    // Under mutex_: the followed car's history of `mask` over [from, to] as column blocks.
    void readHistory(float from, float to, uint32_t mask, std::vector<uint8_t>& out) {
        tnrp::TnrdReader::HistoryRequest request;
        if (!history_ || mask == 0 || !reader_.rangeHistoryRequest(from, to, mask, driver_, {}, request)) return;
        std::string error;
        if (!history_->read(request, out, &error, {})) {
            std::fprintf(stderr, "[analysis] second file flush read failed: %s\n", error.c_str());
            out.clear();
        }
    }

    // Under mutex_: a span's rows for a recording without column history, as
    // the seek flush A's playback uses for those formats.
    void readFlush(float target, float lapStart, uint32_t mask, bool all, float windowSeconds, Batch& out) {
        if (mask == 0) return;
        const auto flush = reader_.seekFlush(target, lapStart, all, mask, windowSeconds, false);
        if (flush.binaryStore && flush.binaryEnd > flush.binaryBegin)
            out.historyBinary.insert(out.historyBinary.end(), flush.binaryStore->begin() + flush.binaryBegin,
                                     flush.binaryStore->begin() + flush.binaryEnd);
        out.historyJson += flush.coldJson;
        if (!out.historyJson.empty() && out.historyJson.back() != '\n') out.historyJson.push_back('\n');
    }

    // Under mutex_: the flush for a jump to `target`.
    void flushTo(float target, Batch& out) {
        out.flush = true;
        const uint32_t lapMask = kHistoryMask & ~longMask_;
        if (longMask_)
            out.longFrom = longSeconds_ < 0.0f ? reader_.startTime()
                : std::max(reader_.startTime(), std::min(out.lapStart, target - longSeconds_));
        if (history_) {
            readHistory(out.lapStart, target, lapMask, out.history);
            if (longMask_) readHistory(out.longFrom, target, longMask_, out.longHistory);
        } else {
            readFlush(target, out.lapStart, lapMask, false, 0.0f, out);
            if (longMask_) readFlush(target, out.lapStart, longMask_, longSeconds_ < 0.0f,
                                     longSeconds_ > 0.0f ? target - out.longFrom : 0.0f, out);
        }
        std::vector<uint8_t> types;
        for (uint8_t type = 1; type < 16; ++type)
            if (kRowMask & (1u << type)) types.push_back(type);
        for (auto& [type, line] : reader_.latestOfTypesTagged(target, types)) {
            out.json += line;
            out.json.push_back('\n');
        }
        reader_.setCursor(target);
    }

    void loop() {
        while (run_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            const float target = target_.load();
            if (!std::isfinite(target)) continue;
            auto batch = std::make_unique<Batch>();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!loaded_ || !enabled_) continue;
                // A new focus car: its private families join the playback lanes
                // from where the stream is, and its status is sent at once, as
                // Engine::playerSetFocusDriver does (v6FocusStatusSnapshot).
                const bool focusChanged = focusPending_;
                if (focusPending_) {
                    focusPending_ = false;
                    reader_.setV6FocusDriver(focusDriver_, std::isfinite(cursor_) ? cursor_ : target);
                }
                const bool jump = !std::isfinite(cursor_) || target < cursor_ || target > cursor_ + 1.0f;
                batch->time = target;
                batch->lapStart = target;
                reader_.currentLapAt(target, batch->lapStart, batch->lapNum);
                if (jump) {
                    flushTo(target, *batch);
                } else if (target > cursor_) {
                    uint32_t seen = 0;
                    reader_.pullUntilSplit(target, batch->json, batch->binary, seen);
                }
                cursor_ = target;
                if (focusChanged) {
                    for (auto& row : reader_.v6FocusStatusSnapshot(target)) {
                        batch->json += row;
                        batch->json.push_back('\n');
                    }
                }
                // The fastest-lap holder, which V6 recordings derive from their
                // lap summaries, as the engine sends A's
                // (Engine::playbackFastestLapRowLocked): on a flush and when it
                // changes. Other formats find it from session_history_fastest.
                tnrp::FastestLapRow fastest;
                if (reader_.v6FastestLapAt(target, fastest) && (jump || fastest.car_idx != fastestCar_)) {
                    fastestCar_ = fastest.car_idx;
                    std::string row;
                    (void)glz::write_json(fastest, row);
                    batch->json += row;
                    batch->json.push_back('\n');
                }
                // The lap-times view's car: its driver_lap_history row, as the
                // engine pushes A's (Engine::lapHistoryRowLocked): when claimed or
                // on a jump, and when the cursor passes a lap end if it changed.
                if (lapHistoryCar_ >= 0 && (jump || lapHistoryForce_ || target >= nextLapHistoryCheck_)) {
                    const bool force = jump || lapHistoryForce_;
                    lapHistoryForce_ = false;
                    nextLapHistoryCheck_ = reader_.v6NextLapEndAfter(target);
                    const auto history = tnrp::buildDriverLapHistory(lapHistoryCar_, kCarSlots,
                        [&](int car, int* stintStartLap) {
                            auto row = reader_.driverLapHistory(car, target);
                            if (stintStartLap) *stintStartLap = row.stint_start_lap;
                            return std::move(row.laps);
                        });
                    std::string row;
                    (void)glz::write_json(history, row);
                    if (force || row != lastLapHistoryJson_) {
                        lastLapHistoryJson_ = row;
                        batch->json += row;
                        batch->json.push_back('\n');
                    }
                }
                // The followed car's completed laps, on a flush and at each new lap.
                if (jump || batch->lapNum != trendsLap_) {
                    trendsLap_ = batch->lapNum;
                    batch->json += reader_.lapTrendsMessage(driver_, target, hasMguh_);
                    batch->json.push_back('\n');
                }
            }
            if (!batch->flush && batch->json.empty() && batch->binary.empty()) continue;
            std::lock_guard<std::mutex> lock(callbackMutex_);
            if (!hasCallback_) continue;
            Batch* data = batch.release();
            const auto status = callback_.NonBlockingCall(data, [](Napi::Env env, Napi::Function cb, Batch* d) {
                if (env != nullptr && cb != nullptr) {
                    try {
                        Napi::Object message = Napi::Object::New(env);
                        message.Set("json", Napi::String::New(env, d->json));
                        message.Set("time", d->time);
                        message.Set("lapNum", d->lapNum);
                        message.Set("lapStart", d->lapStart);
                        if (!d->binary.empty())
                            message.Set("binary", Napi::Buffer<uint8_t>::Copy(env, d->binary.data(), d->binary.size()));
                        if (d->flush) {
                            message.Set("flush", true);
                            if (!d->history.empty())
                                message.Set("history", Napi::Buffer<uint8_t>::Copy(env, d->history.data(), d->history.size()));
                            if (!d->longHistory.empty()) {
                                message.Set("longHistory", Napi::Buffer<uint8_t>::Copy(env, d->longHistory.data(), d->longHistory.size()));
                                message.Set("longFrom", d->longFrom);
                            }
                            if (!d->historyBinary.empty())
                                message.Set("historyBinary", Napi::Buffer<uint8_t>::Copy(env, d->historyBinary.data(), d->historyBinary.size()));
                            if (!d->historyJson.empty())
                                message.Set("historyJson", Napi::String::New(env, d->historyJson));
                        }
                        cb.Call({ message });
                    } catch (const std::exception& e) {
                        std::fprintf(stderr, "[addon] second file stream callback failed: %s\n", e.what());
                    }
                }
                delete d;
            });
            if (status != napi_ok) delete data;
        }
    }

    std::mutex mutex_;
    tnrp::TnrdReader reader_;
    std::unique_ptr<tnrp::TnrdReader::HistoryReader> history_;
    bool loaded_ = false;
    bool enabled_ = false;
    bool hasMguh_ = false;
    int driver_ = -1;
    uint32_t longMask_ = 0;
    float longSeconds_ = 0.0f;
    int trendsLap_ = -1;          // the lap the last lap_trends was sent at
    int fastestCar_ = -2;         // the fastest-lap holder last sent (-2: none)
    int lapHistoryCar_ = -1;      // the lap-times view's car (-1: none)
    int focusDriver_ = -1;        // the car selected in B's tower (-1: none)
    bool focusPending_ = false;   // apply focusDriver_ on the next tick
    bool lapHistoryForce_ = false;  // send its row on the next tick
    float nextLapHistoryCheck_ = std::numeric_limits<float>::infinity();
    std::string lastLapHistoryJson_;
    float cursor_ = NAN;         // the time the stream has delivered through
    std::atomic<float> target_{NAN};
    std::atomic<bool> run_{true};
    std::mutex callbackMutex_;
    Napi::ThreadSafeFunction callback_;
    bool hasCallback_ = false;
    std::thread thread_;          // last: starts once everything above exists
};

struct AnalysisReaderState {
    // File B's rows streamed at its playhead (SecondaryStream), on a handle
    // of its own so its cursor never moves the reader Trends and laps use.
    std::shared_ptr<SecondaryStream> stream = std::make_shared<SecondaryStream>();
    std::mutex mutex;
    tnrp::TnrdReader reader;
    std::atomic<bool> busy{false};
    // The loaded file's regulations decide whether MGU-H counts as recharge.
    bool hasMguh = false;
    // A second handle on the file for its laps as column blocks (the history
    // playback reads on a seek), opened with the file. It has its own lock, so
    // a lap is read while Trends or tyre wear hold `mutex`. Null for
    // recordings without column history (anything but V6).
    std::mutex historyMutex;
    std::unique_ptr<tnrp::TnrdReader::HistoryReader> history;
};

// Loads a second recording for Analysis without replacing or controlling the
// active playback reader.
class AnalysisLoadWorker : public Napi::AsyncWorker {
public:
    AnalysisLoadWorker(Napi::Env env, std::shared_ptr<AnalysisReaderState> state,
                       std::string path)
        : Napi::AsyncWorker(env), state_(std::move(state)), path_(std::move(path)),
          deferred_(Napi::Promise::Deferred::New(env)) {}

    void Execute() override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        tnrp::HeaderRow header;
        state_->reader.setLapStatusSummaries(true);
        {
            std::lock_guard<std::mutex> historyLock(state_->historyMutex);
            state_->history.reset();
        }
        ok_ = state_->reader.load(path_, header);
        if (ok_) {
            {
                std::string historyError;
                auto history = state_->reader.openHistoryReader(&historyError);
                if (!history && !historyError.empty())
                    std::fprintf(stderr, "[analysis] no column history for the second file: %s\n", historyError.c_str());
                std::lock_guard<std::mutex> historyLock(state_->historyMutex);
                state_->history = std::move(history);
            }
            state_->stream->open(path_);
            state_->reader.startErsPeakSweep();
            state_->reader.startTrackCornerSweep();
            blocksJson_ = state_->reader.lapBlocksMessage();
            trackId_ = header.track_id;
            trackName_ = header.track_name;
            state_->hasMguh = tnrp::hasMguh(tnrp::presentationFormat(
                header.protocol >= 2024 ? static_cast<uint16_t>(header.protocol) : 2025,
                header.regulations_2026, header.formula));
        }
        else error_ = state_->reader.lastError();
    }

    void OnOK() override {
        state_->busy.store(false);
        Napi::Object result = Napi::Object::New(Env());
        result.Set("ok", ok_);
        if (ok_) {
            result.Set("blocksJson", blocksJson_);
            result.Set("trackId", trackId_);
            result.Set("trackName", trackName_);
        }
        else result.Set("error", error_);
        deferred_.Resolve(result);
    }

    void OnError(const Napi::Error& e) override {
        state_->busy.store(false);
        deferred_.Reject(e.Value());
    }

    Napi::Promise GetPromise() { return deferred_.Promise(); }

private:
    std::shared_ptr<AnalysisReaderState> state_;
    std::string path_, blocksJson_, trackName_, error_;
    bool ok_ = false;
    int trackId_ = 0;
    Napi::Promise::Deferred deferred_;
};

// Computes a complete lap-to-lap delta off the Electron main thread. The two
// laps may come from the active player or the independent secondary Analysis
// reader; the renderer sends identities only and never performs interpolation.
// Reads one lap's rows off the JS thread, like playerGetLapData does for the
// playback file, so a large lap never stalls the main process. Resolves with
// the playback_lap_data JSON ("" when the lap or file is unavailable).
class AnalysisLapDataWorker : public Napi::AsyncWorker {
public:
    AnalysisLapDataWorker(Napi::Env env, std::shared_ptr<tnrp::Engine> engine,
                          std::shared_ptr<AnalysisReaderState> secondary,
                          int lapNum, uint32_t rowTypeMask, bool useSecondary, int driverIndex)
        : Napi::AsyncWorker(env), engine_(std::move(engine)),
          secondary_(std::move(secondary)), lapNum_(lapNum), rowTypeMask_(rowTypeMask),
          useSecondary_(useSecondary), driverIndex_(driverIndex),
          deferred_(Napi::Promise::Deferred::New(env)) {}

    void Execute() override {
        if (useSecondary_) {
            if (secondary_->busy.load()) return;
            std::lock_guard<std::mutex> lock(secondary_->mutex);
            json_ = secondary_->reader.getLapDataMessage(lapNum_, rowTypeMask_, driverIndex_);
        } else if (engine_) {
            json_ = engine_->playerGetAnalysisLapData(lapNum_, rowTypeMask_, driverIndex_);
        }
    }
    void OnOK() override { deferred_.Resolve(Napi::String::New(Env(), json_)); }
    void OnError(const Napi::Error& e) override { deferred_.Reject(e.Value()); }
    Napi::Promise GetPromise() { return deferred_.Promise(); }

private:
    std::shared_ptr<tnrp::Engine> engine_;
    std::shared_ptr<AnalysisReaderState> secondary_;
    int lapNum_;
    uint32_t rowTypeMask_;
    bool useSecondary_;
    int driverIndex_;
    std::string json_;
    Napi::Promise::Deferred deferred_;
};

// One of the second file's laps as V6 column blocks (columnStore.ts decodes
// them, as it does a seek's history), read off the JS thread. No JSON: the
// lap's rows go from the file's columns straight into the payload. Resolves
// with { lapNum, startSessionTime, endSessionTime, history } where history is
// the V6H1 payload, or with null when the file has no column history (not V6)
// or no such lap; the caller then uses the JSON lap.
class AnalysisLapHistoryWorker : public Napi::AsyncWorker {
public:
    AnalysisLapHistoryWorker(Napi::Env env, std::shared_ptr<AnalysisReaderState> state,
                             int lapNum, uint32_t rowTypeMask, int driverIndex,
                             std::vector<uint8_t> v6Types)
        : Napi::AsyncWorker(env), state_(std::move(state)), lapNum_(lapNum),
          rowTypeMask_(rowTypeMask), driverIndex_(driverIndex), v6Types_(std::move(v6Types)),
          deferred_(Napi::Promise::Deferred::New(env)) {}

    void Execute() override {
        if (state_->busy.load()) return;
        tnrp::TnrdReader::HistoryRequest request;
        {
            // Only the lap lookup takes the reader's lock; the read below does not.
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (!state_->reader.lapHistoryRequest(lapNum_, rowTypeMask_, driverIndex_, v6Types_,
                                                  request, start_, end_)) return;
        }
        std::lock_guard<std::mutex> historyLock(state_->historyMutex);
        if (!state_->history) return;
        std::string error;
        if (!state_->history->read(request, bytes_, &error, {})) {
            std::fprintf(stderr, "[analysis] second-file lap %d column read failed: %s\n", lapNum_, error.c_str());
            bytes_.clear();
            return;
        }
        ok_ = true;
    }

    void OnOK() override {
        Napi::Env env = Env();
        if (!ok_) {
            deferred_.Resolve(env.Null());
            return;
        }
        Napi::Object result = Napi::Object::New(env);
        result.Set("lapNum", lapNum_);
        result.Set("startSessionTime", start_);
        result.Set("endSessionTime", end_);
        result.Set("rowTypeMask", rowTypeMask_);
        result.Set("history", Napi::Buffer<uint8_t>::Copy(env, bytes_.data(), bytes_.size()));
        deferred_.Resolve(result);
    }
    void OnError(const Napi::Error& e) override { deferred_.Reject(e.Value()); }
    Napi::Promise GetPromise() { return deferred_.Promise(); }

private:
    std::shared_ptr<AnalysisReaderState> state_;
    int lapNum_;
    uint32_t rowTypeMask_;
    int driverIndex_;
    std::vector<uint8_t> v6Types_;
    float start_ = 0.0f;
    float end_ = 0.0f;
    std::vector<uint8_t> bytes_;
    bool ok_ = false;
    Napi::Promise::Deferred deferred_;
};

class AnalysisDeltaWorker : public Napi::AsyncWorker {
public:
    AnalysisDeltaWorker(Napi::Env env, std::shared_ptr<tnrp::Engine> engine,
                        std::shared_ptr<AnalysisReaderState> secondary,
                        int currentLap, bool currentSecondary, int currentDriver,
                        int comparisonLap, bool comparisonSecondary, int comparisonDriver,
                        bool sectorDelta)
        : Napi::AsyncWorker(env), engine_(std::move(engine)),
          secondary_(std::move(secondary)), currentLap_(currentLap),
          currentSecondary_(currentSecondary), currentDriver_(currentDriver),
          comparisonLap_(comparisonLap), comparisonSecondary_(comparisonSecondary),
          comparisonDriver_(comparisonDriver), sectorDelta_(sectorDelta),
          deferred_(Napi::Promise::Deferred::New(env)) {}

    void Execute() override {
        tnrp::AnalysisLapProgress current;
        tnrp::AnalysisLapProgress comparison;
        bool haveCurrent = false;
        bool haveComparison = false;

        if (currentSecondary_ && comparisonSecondary_) {
            std::lock_guard<std::mutex> lock(secondary_->mutex);
            haveCurrent = secondary_->reader.getAnalysisLapProgress(
                currentLap_, current, currentDriver_);
            haveComparison = secondary_->reader.getAnalysisLapProgress(
                comparisonLap_, comparison, comparisonDriver_);
        } else {
            if (currentSecondary_) {
                std::lock_guard<std::mutex> lock(secondary_->mutex);
                haveCurrent = secondary_->reader.getAnalysisLapProgress(
                    currentLap_, current, currentDriver_);
            } else if (engine_) {
                haveCurrent = engine_->playerGetAnalysisLapProgress(
                    currentLap_, current, currentDriver_);
            }
            if (comparisonSecondary_) {
                std::lock_guard<std::mutex> lock(secondary_->mutex);
                haveComparison = secondary_->reader.getAnalysisLapProgress(
                    comparisonLap_, comparison, comparisonDriver_);
            } else if (engine_) {
                haveComparison = engine_->playerGetAnalysisLapProgress(
                    comparisonLap_, comparison, comparisonDriver_);
            }
        }

        if (!haveCurrent || !haveComparison) return;
        json_ = tnrp::writeJson(tnrp::calculateLapDelta(current, comparison, sectorDelta_));
    }

    void OnOK() override { deferred_.Resolve(Napi::String::New(Env(), json_)); }
    void OnError(const Napi::Error& error) override { deferred_.Reject(error.Value()); }
    Napi::Promise GetPromise() { return deferred_.Promise(); }

private:
    std::shared_ptr<tnrp::Engine> engine_;
    std::shared_ptr<AnalysisReaderState> secondary_;
    int currentLap_;
    bool currentSecondary_;
    int currentDriver_;
    int comparisonLap_;
    bool comparisonSecondary_;
    int comparisonDriver_;
    bool sectorDelta_;
    std::string json_;
    Napi::Promise::Deferred deferred_;
};

// Colours the Analysis map (tnrp/AnalysisMap.h) for one or two laps, each from
// the playback recording or the secondary Analysis recording.
class AnalysisMapWorker : public Napi::AsyncWorker {
public:
    AnalysisMapWorker(Napi::Env env, std::shared_ptr<tnrp::Engine> engine,
                      std::shared_ptr<AnalysisReaderState> secondary, std::string mode,
                      int currentLap, bool currentSecondary, int currentDriver,
                      int comparisonLap, bool comparisonSecondary, int comparisonDriver)
        : Napi::AsyncWorker(env), engine_(std::move(engine)),
          secondary_(std::move(secondary)), mode_(std::move(mode)),
          currentLap_(currentLap), currentSecondary_(currentSecondary),
          currentDriver_(currentDriver), comparisonLap_(comparisonLap),
          comparisonSecondary_(comparisonSecondary), comparisonDriver_(comparisonDriver),
          deferred_(Napi::Promise::Deferred::New(env)) {}

    void Execute() override {
        const auto access = [this](bool secondary) -> tnrp::RecordingAccess {
            if (secondary) {
                auto state = secondary_;
                return [state](const std::function<void(const tnrp::TnrdReader&)>& read) {
                    if (state->busy.load()) return false;
                    std::lock_guard<std::mutex> lock(state->mutex);
                    if (!state->reader.isLoaded()) return false;
                    read(state->reader);
                    return true;
                };
            }
            auto engine = engine_;
            return [engine](const std::function<void(const tnrp::TnrdReader&)>& read) {
                return engine && engine->playerReadRecording(read);
            };
        };
        const tnrp::AnalysisMapLap current{access(currentSecondary_), currentLap_, currentDriver_};
        const tnrp::AnalysisMapLap comparison{access(comparisonSecondary_), comparisonLap_, comparisonDriver_};
        json_ = tnrp::writeJson(tnrp::buildAnalysisMap(
            mode_, current, comparisonLap_ >= 0 ? &comparison : nullptr));
    }

    void OnOK() override { deferred_.Resolve(Napi::String::New(Env(), json_)); }
    void OnError(const Napi::Error& error) override { deferred_.Reject(error.Value()); }
    Napi::Promise GetPromise() { return deferred_.Promise(); }

private:
    std::shared_ptr<tnrp::Engine> engine_;
    std::shared_ptr<AnalysisReaderState> secondary_;
    std::string mode_;
    int currentLap_;
    bool currentSecondary_;
    int currentDriver_;
    int comparisonLap_;
    bool comparisonSecondary_;
    int comparisonDriver_;
    std::string json_;
    Napi::Promise::Deferred deferred_;
};

// The answer to a track map change the renderer asked for.
struct TrackMapResult {
    bool ok{};
    std::string id;
    std::string error;
};

// [{address, port}] -> the engine's forwarding destinations; malformed entries are skipped.
static std::vector<tnrp::UdpForwardTarget> readForwardTargets(const Napi::Value& value) {
    std::vector<tnrp::UdpForwardTarget> result;
    if (!value.IsArray()) return result;
    Napi::Array targets = value.As<Napi::Array>();
    const uint32_t count = std::min<uint32_t>(targets.Length(), tnrp::kMaxUdpForwardTargets);
    result.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        Napi::Value item = targets.Get(i);
        if (!item.IsObject()) continue;
        Napi::Object targetObj = item.As<Napi::Object>();
        if (!targetObj.Has("address") || !targetObj.Get("address").IsString() ||
            !targetObj.Has("port") || !targetObj.Get("port").IsNumber()) continue;
        const uint32_t targetPort = targetObj.Get("port").As<Napi::Number>().Uint32Value();
        if (targetPort == 0 || targetPort > 65535) continue;
        result.push_back({
            targetObj.Get("address").As<Napi::String>().Utf8Value(),
            static_cast<uint16_t>(targetPort)
        });
    }
    return result;
}

// Rebinds the engine's UDP listener off the JS thread: the restart waits for
// the receive thread, which may itself be waiting on the engine lock. Resolves
// with the bind error, or "" once the listener is bound.
class UdpRestartWorker : public Napi::AsyncWorker {
public:
    UdpRestartWorker(Napi::Env env, std::shared_ptr<tnrp::Engine> engine, uint16_t port,
                     std::string bindAddress, std::vector<tnrp::UdpForwardTarget> forwardTargets)
        : Napi::AsyncWorker(env), engine_(std::move(engine)), port_(port),
          bindAddress_(std::move(bindAddress)), forwardTargets_(std::move(forwardTargets)),
          deferred_(Napi::Promise::Deferred::New(env)) {}
    void Execute() override {
        if (engine_->restartUdp(port_, bindAddress_, std::move(forwardTargets_))) return;
        error_ = engine_->udpLastError();
        if (error_.empty()) error_ = "Failed to start the UDP listener.";
    }
    void OnOK() override { deferred_.Resolve(Napi::String::New(Env(), error_)); }
    void OnError(const Napi::Error& error) override { deferred_.Reject(error.Value()); }
    Napi::Promise GetPromise() { return deferred_.Promise(); }

private:
    std::shared_ptr<tnrp::Engine> engine_;
    uint16_t port_;
    std::string bindAddress_;
    std::vector<tnrp::UdpForwardTarget> forwardTargets_;
    std::string error_;
    Napi::Promise::Deferred deferred_;
};

// Runs a track map call that reads a recording or the disk off the JS thread
// and resolves with the JSON it returns.
class TrackMapWorker : public Napi::AsyncWorker {
public:
    TrackMapWorker(Napi::Env env, std::function<std::string()> call)
        : Napi::AsyncWorker(env), call_(std::move(call)), deferred_(Napi::Promise::Deferred::New(env)) {}
    void Execute() override { json_ = call_(); }
    void OnOK() override { deferred_.Resolve(Napi::String::New(Env(), json_)); }
    void OnError(const Napi::Error& error) override { deferred_.Reject(error.Value()); }
    Napi::Promise GetPromise() { return deferred_.Promise(); }

private:
    std::function<std::string()> call_;
    std::string json_;
    Napi::Promise::Deferred deferred_;
};

class TNRPAddon : public Napi::ObjectWrap<TNRPAddon>, public tnrp::Sink {
public:
    static Napi::Object Init(Napi::Env env, Napi::Object exports) {
        TRACE("Init: before DefineClass");
        Napi::Function func = DefineClass(env, "Engine", {
            InstanceMethod("startUdp", &TNRPAddon::StartUdp),
            InstanceMethod("restartUdp", &TNRPAddon::RestartUdp),
            InstanceMethod("udpLastError", &TNRPAddon::UdpLastError),
            InstanceMethod("setDiagnosticsEnabled", &TNRPAddon::SetDiagnosticsEnabled),
            InstanceMethod("setNativeExceptionReporting", &TNRPAddon::SetNativeExceptionReporting),
            InstanceMethod("liveDiagnostics", &TNRPAddon::LiveDiagnostics),
            InstanceMethod("datagramsReceived", &TNRPAddon::DatagramsReceived),
            InstanceMethod("setOverride", &TNRPAddon::SetOverride),
            InstanceMethod("setTeamColorOverrides", &TNRPAddon::SetTeamColorOverrides),
            InstanceMethod("teamColorCatalog", &TNRPAddon::TeamColorCatalog),
            InstanceMethod("setStrategyMinimumStops", &TNRPAddon::SetStrategyMinimumStops),
            InstanceMethod("setLogging", &TNRPAddon::SetLogging),
            InstanceMethod("setLoggingZstd", &TNRPAddon::SetLoggingZstd),
            InstanceMethod("setLoggingGzip", &TNRPAddon::SetLoggingGzip),
            InstanceMethod("setRecordingScopes", &TNRPAddon::SetRecordingScopes),
            InstanceMethod("flushRecording", &TNRPAddon::FlushRecording),
            InstanceMethod("setDataRequirements", &TNRPAddon::SetDataRequirements),
            InstanceMethod("playerLoad", &TNRPAddon::PlayerLoad),
            InstanceMethod("playerPlay", &TNRPAddon::PlayerPlay),
            InstanceMethod("playerPause", &TNRPAddon::PlayerPause),
            InstanceMethod("playerSeek", &TNRPAddon::PlayerSeek),
            InstanceMethod("playerSetSpeed", &TNRPAddon::PlayerSetSpeed),
            InstanceMethod("playerSetDriver", &TNRPAddon::PlayerSetDriver),
            InstanceMethod("playerSetFocusDriver", &TNRPAddon::PlayerSetFocusDriver),
            InstanceMethod("playerGetLapData", &TNRPAddon::PlayerGetLapData),
            InstanceMethod("liveGetFastestLap", &TNRPAddon::LiveGetFastestLap),
            InstanceMethod("liveGetLapData", &TNRPAddon::LiveGetLapData),
            InstanceMethod("setLapHistoryCar", &TNRPAddon::SetLapHistoryCar),
            InstanceMethod("playerGetAllLapsData", &TNRPAddon::PlayerGetAllLapsData),
            InstanceMethod("playerGetWindowData", &TNRPAddon::PlayerGetWindowData),
            InstanceMethod("playerClose", &TNRPAddon::PlayerClose),
            InstanceMethod("setHostVisible", &TNRPAddon::SetHostVisible),
            InstanceMethod("analysisLoadFile", &TNRPAddon::AnalysisLoadFile),
            InstanceMethod("analysisGetLapData", &TNRPAddon::AnalysisGetLapData),
            InstanceMethod("analysisLapHistory", &TNRPAddon::AnalysisLapHistory),
            InstanceMethod("analysisStream", &TNRPAddon::AnalysisStream),
            InstanceMethod("analysisSetStreaming", &TNRPAddon::AnalysisSetStreaming),
            InstanceMethod("analysisSetLapHistoryCar", &TNRPAddon::AnalysisSetLapHistoryCar),
            InstanceMethod("analysisSetFocusDriver", &TNRPAddon::AnalysisSetFocusDriver),
            InstanceMethod("mergedLapTimeline", &TNRPAddon::MergedLapTimeline),
            InstanceMethod("playerSetMergedTimeline", &TNRPAddon::PlayerSetMergedTimeline),
            InstanceMethod("analysisCompareLaps", &TNRPAddon::AnalysisCompareLaps),
            InstanceMethod("analysisMap", &TNRPAddon::AnalysisMap),
            InstanceMethod("analysisCloseFile", &TNRPAddon::AnalysisCloseFile),
            InstanceMethod("playerExportParquet", &TNRPAddon::PlayerExportParquet),
            InstanceMethod("pairStart", &TNRPAddon::PairStart),
            InstanceMethod("pairStop", &TNRPAddon::PairStop),
            InstanceMethod("pairOpenWindow", &TNRPAddon::PairOpenWindow),
            InstanceMethod("pairCloseWindow", &TNRPAddon::PairCloseWindow),
            InstanceMethod("pairRespond", &TNRPAddon::PairRespond),
            InstanceMethod("pairRemoveDevice", &TNRPAddon::PairRemoveDevice),
            InstanceMethod("pairGetState", &TNRPAddon::PairGetState),
            InstanceMethod("telemetryRetention", &TNRPAddon::TelemetryRetention),
            InstanceMethod("trackMapsAddBuiltins", &TNRPAddon::TrackMapsAddBuiltins),
            InstanceMethod("trackMapsSetDirectory", &TNRPAddon::TrackMapsSetDirectory),
            InstanceMethod("trackMapsSetOptions", &TNRPAddon::TrackMapsSetOptions),
            InstanceMethod("trackMapsCatalog", &TNRPAddon::TrackMapsCatalog),
            InstanceMethod("trackMapsGet", &TNRPAddon::TrackMapsGet),
            InstanceMethod("trackMapsState", &TNRPAddon::TrackMapsState),
            InstanceMethod("trackMapsPin", &TNRPAddon::TrackMapsPin),
            InstanceMethod("trackMapsRename", &TNRPAddon::TrackMapsRename),
            InstanceMethod("trackMapsDelete", &TNRPAddon::TrackMapsDelete),
            InstanceMethod("trackMapsEdit", &TNRPAddon::TrackMapsEdit),
            InstanceMethod("trackMapsImport", &TNRPAddon::TrackMapsImport),
            InstanceMethod("trackMapsExport", &TNRPAddon::TrackMapsExport),
            InstanceMethod("trackMapsGenerate", &TNRPAddon::TrackMapsGenerate),
            InstanceMethod("trackMapsResolveRecording", &TNRPAddon::TrackMapsResolveRecording),
            InstanceMethod("destroy", &TNRPAddon::Destroy)
        });
        TRACE("Init: after DefineClass");

        Napi::FunctionReference* constructor = new Napi::FunctionReference();
        *constructor = Napi::Persistent(func);
        TRACE("Init: after Persistent");
        env.SetInstanceData(constructor);
        TRACE("Init: after SetInstanceData");

        exports.Set("Engine", func);
        TRACE("Init: after exports.Set");
        return exports;
    }

    TNRPAddon(const Napi::CallbackInfo& info) : Napi::ObjectWrap<TNRPAddon>(info) {
        TRACE("TNRPAddon ctor: start");
        Napi::Env env = info.Env();

        // Expected args: (configObj, callback)
        if (info.Length() < 2 || !info[0].IsObject() || !info[1].IsFunction()) {
            Napi::TypeError::New(env, "Expected (configObj, callback)").ThrowAsJavaScriptException();
            return;
        }

        Napi::Object configObj = info[0].As<Napi::Object>();
        tnrp::Config config;

        if (configObj.Has("format") && configObj.Get("format").IsString()) {
            std::string fmt = configObj.Get("format").As<Napi::String>().Utf8Value();
            config.protocol = tnrp::overrideFromString(fmt);
        }
        if (configObj.Has("port") && configObj.Get("port").IsNumber()) {
            const double port = configObj.Get("port").As<Napi::Number>().DoubleValue();
            if (!std::isfinite(port) || std::trunc(port) != port || port < 1 || port > 65535) {
                Napi::RangeError::New(env, "UDP port must be an integer from 1 to 65535")
                    .ThrowAsJavaScriptException();
                return;
            }
            config.port = static_cast<uint16_t>(port);
        }
        if (configObj.Has("bindAddress") && configObj.Get("bindAddress").IsString()) {
            config.bindAddress = configObj.Get("bindAddress").As<Napi::String>().Utf8Value();
        }
        if (configObj.Has("forwardTargets"))
            config.udpForwardTargets = readForwardTargets(configObj.Get("forwardTargets"));
        if (configObj.Has("binaryPlayback") && configObj.Get("binaryPlayback").IsBoolean()) {
            config.binaryPlayback = configObj.Get("binaryPlayback").As<Napi::Boolean>().Value();
        }
        if (configObj.Has("sparseV6Playback") && configObj.Get("sparseV6Playback").IsBoolean()) {
            config.sparseV6Playback = configObj.Get("sparseV6Playback").As<Napi::Boolean>().Value();
        }
        if (configObj.Has("columnarV6History") && configObj.Get("columnarV6History").IsBoolean()) {
            config.columnarV6History = configObj.Get("columnarV6History").As<Napi::Boolean>().Value();
        }
        if (configObj.Has("strategyMinimumStops") && configObj.Get("strategyMinimumStops").IsNumber()) {
            config.strategyMinimumStops = configObj.Get("strategyMinimumStops").As<Napi::Number>().Int32Value();
        }
        if (configObj.Has("teamColorOverrides")) {
            config.teamColorOverrides = readTeamColorOverrides(
                configObj.Get("teamColorOverrides"));
        }
        if (configObj.Has("pairEnabled") && configObj.Get("pairEnabled").IsBoolean()) {
            config.pairEnabled = configObj.Get("pairEnabled").As<Napi::Boolean>().Value();
        }
        if (configObj.Has("pairPort") && configObj.Get("pairPort").IsNumber()) {
            const uint32_t port = configObj.Get("pairPort").As<Napi::Number>().Uint32Value();
            if (port > 0 && port <= 65535) config.pairPort = static_cast<uint16_t>(port);
        }
        if (configObj.Has("pairName") && configObj.Get("pairName").IsString()) {
            config.pairName = configObj.Get("pairName").As<Napi::String>().Utf8Value();
        }
        if (configObj.Has("pairStateJson") && configObj.Get("pairStateJson").IsString()) {
            config.pairStateJson = configObj.Get("pairStateJson").As<Napi::String>().Utf8Value();
        }
        TRACE("TNRPAddon ctor: config parsed");

        Napi::Function cb = info[1].As<Napi::Function>();

        // Create ThreadSafeFunction for the JSON (cold + control) row batch.
        tsfn = Napi::ThreadSafeFunction::New(
            env,
            cb,
            "TNRP Callback",
            0,
            1,
            [this](Napi::Env) {
                // Finalizer
            }
        );
        tsfn.Unref(env); // Allow the Node event loop to exit even if tsfn is active
        TRACE("TNRPAddon ctor: tsfn created");

        // Optional second callback for the hot-row binary batch (Buffer).
        if (info.Length() >= 3 && info[2].IsFunction()) {
            Napi::Function binCb = info[2].As<Napi::Function>();
            tsfnBin = Napi::ThreadSafeFunction::New(
                env, binCb, "TNRP Binary Callback", 0, 1, [](Napi::Env) {});
            tsfnBin.Unref(env);
            hasBinCb_ = true;
        }

        // Optional third callback for the playback seek flush
        // (binary: Buffer, coldJson: string, currentLapStart: number,
        //  lapNum: number, allHistory: boolean).
        // Only fires when the engine runs with config.binaryPlayback.
        if (info.Length() >= 4 && info[3].IsFunction()) {
            Napi::Function seekCb = info[3].As<Napi::Function>();
            tsfnSeek = Napi::ThreadSafeFunction::New(
                env, seekCb, "TNRP SeekFlush Callback", 0, 1, [](Napi::Env) {});
            tsfnSeek.Unref(env);
            hasSeekCb_ = true;
        }

        // Optional fourth callback for public paired-mode UI state plus the
        // opaque private state document that the host persists unchanged.
        if (info.Length() >= 5 && info[4].IsFunction()) {
            Napi::Function pairCb = info[4].As<Napi::Function>();
            tsfnPair = Napi::ThreadSafeFunction::New(
                env, pairCb, "TNRP Pair State Callback", 0, 1,
                [](Napi::Env) {});
            tsfnPair.Unref(env);
            hasPairCb_ = true;
        }

        // Optional fifth callback for low-volume native paired-transport
        // lifecycle diagnostics. It is intentionally independent of the UI
        // state callback so diagnostics never become persisted pair state.
        if (info.Length() >= 6 && info[5].IsFunction()) {
            Napi::Function pairDiagnosticCb = info[5].As<Napi::Function>();
            tsfnPairDiagnostic = Napi::ThreadSafeFunction::New(
                env, pairDiagnosticCb, "TNRP Pair Diagnostic Callback", 0, 1,
                [](Napi::Env) {});
            tsfnPairDiagnostic.Unref(env);
            hasPairDiagnosticCb_ = true;
        }
        TRACE("TNRPAddon ctor: about to construct Engine");

        engine = std::make_shared<tnrp::Engine>(config, this);
        // Lap-synced playback: the engine's merged clock places the second
        // file, and its stream reads to there (SecondaryStream).
        engine->setMergedPlayheadCallback([stream = analysisReader_->stream](float secondTime) {
            stream->setTarget(secondTime);
        });
        TRACE("TNRPAddon ctor: Engine constructed, done");
    }

    ~TNRPAddon() {
        if (!destroyed_) {
            if (engine) {
                engine->pairStop(false);
                engine->stopUdp();
                // ObjectWrap finalization can run while Node is dismantling the
                // N-API environment. playerClose() emits playback_close through
                // onRow(), so calling it here may touch an already-closing TSFN.
                // Normal application shutdown invokes Destroy() while N-API is
                // still live; this fallback must not emit playback rows.
                engine.reset();
            }
            tsfn.Release();
            if (hasBinCb_) tsfnBin.Release();
            if (hasSeekCb_) tsfnSeek.Release();
            if (hasPairCb_) tsfnPair.Release();
            if (hasPairDiagnosticCb_) tsfnPairDiagnostic.Release();
        }
    }

    // Rows arrive on the engine's UDP/playback thread at up to several hundred per
    // second. Rather than paying a cross-thread call + JS callback + heap copy per
    // row, we accumulate rows into a newline-delimited buffer and only schedule a
    // flush when the buffer transitions from empty. The JS-thread callback then
    // drains the whole buffer in one call. This naturally coalesces under load
    // (≤1 in-flight TSFN entry) while staying ~1:1 when the main thread keeps up.
    void onRow(const std::string& json) override {
        enqueueRows(&json, 1);
    }

    void onRows(const std::vector<std::string>& rows) override {
        enqueueRows(rows.data(), rows.size());
    }

    // Hot-row binary batch. Same coalescing strategy as onRow(), but accumulates
    // raw bytes and hands them to JS as a single Buffer per flush.
    void onBinary(const uint8_t* data, size_t len) override {
        if (!hasBinCb_ || len == 0) return;
        auto fs = binFlush_;
        bool schedule = false;
        uint64_t generation = 0;
        {
            std::lock_guard<std::mutex> lk(fs->mutex);
            if (fs->discard) return;
            fs->pending.insert(fs->pending.end(), data, data + len);
            ++fs->writesEnqueued;
            fs->payloadBytesEnqueued += len;
            fs->peakPendingUsedBytes = std::max(fs->peakPendingUsedBytes, fs->pending.size());
            fs->peakPendingCapacityBytes = std::max(
                fs->peakPendingCapacityBytes, fs->pending.capacity());
            generation = fs->generation;
            if (!fs->scheduled) {
                fs->scheduled = true;
                fs->scheduledGeneration = generation;
                ++fs->scheduleAttempts;
                schedule = true;
            }
        }
        if (!schedule) return;

        auto status = tsfnBin.NonBlockingCall([fs, generation](Napi::Env env, Napi::Function cb) {
            {
                std::lock_guard<std::mutex> lk(fs->mutex);
                // playerClose() invalidates a queued playback batch before the
                // JSON playback_close row is allowed to reach the renderer.
                // A later live batch may already own this shared flush state,
                // so the obsolete callback must leave it untouched.
                if (fs->generation != generation ||
                    fs->scheduledGeneration != generation) return;
                fs->draining.swap(fs->pending);  // grab the batch; pending keeps storage
                fs->scheduled = false;
                fs->peakDrainingUsedBytes = std::max(
                    fs->peakDrainingUsedBytes, fs->draining.size());
                fs->peakDrainingCapacityBytes = std::max(
                    fs->peakDrainingCapacityBytes, fs->draining.capacity());
            }
            if (env != nullptr && cb != nullptr) {
                cb.Call({ Napi::Buffer<uint8_t>::Copy(env, fs->draining.data(), fs->draining.size()) });
            }
            std::lock_guard<std::mutex> lk(fs->mutex);
            ++fs->deliveredBatches;
            fs->deliveredPayloadBytes += fs->draining.size();
            fs->draining.clear();                // retain capacity for the next swap
        });

        if (status != napi_ok) {
            std::lock_guard<std::mutex> lk(fs->mutex);
            if (fs->scheduledGeneration == generation) fs->scheduled = false;
        }
    }

    // Playback seek flush (Config::binaryPlayback only). The reader hands us a
    // zero-copy view of its immutable packed store. Copy once into a normal V8
    // Buffer here: Electron IPC cannot serialize an external-memory Buffer.
    void onSeekFlush(std::shared_ptr<const std::vector<uint8_t>> binStore,
                     size_t binBegin, size_t binEnd, std::string&& coldJson,
                     float currentLapStart, int lapNum, bool allHistory,
                     uint64_t requestId, bool authoritativeSeek,
                     uint32_t rowTypeMask, float historyStart) override {
        postSeekFlush(std::move(binStore), binBegin, binEnd, std::move(coldJson),
                      currentLapStart, lapNum, allHistory, requestId,
                      authoritativeSeek, rowTypeMask, historyStart, std::nullopt);
    }

    // A host restore travels the seek-flush channel as an additive payload,
    // with its replace ranges in a trailing object argument.
    void onRestoreFlush(std::shared_ptr<const std::vector<uint8_t>> binStore,
                        size_t binBegin, size_t binEnd, std::string&& coldJson,
                        const tnrp::Sink::RestoreFlushInfo& info) override {
        postSeekFlush(std::move(binStore), binBegin, binEnd, std::move(coldJson),
                      info.currentLapStart, info.lapNum, false, 0, false,
                      info.rowTypeMask, info.chartFrom, info);
    }

    void postSeekFlush(std::shared_ptr<const std::vector<uint8_t>> binStore,
                       size_t binBegin, size_t binEnd, std::string&& coldJson,
                       float currentLapStart, int lapNum, bool allHistory,
                       uint64_t requestId, bool authoritativeSeek,
                       uint32_t rowTypeMask, float historyStart,
                       std::optional<tnrp::Sink::RestoreFlushInfo> restore) {
        if (!hasSeekCb_) return;
        struct SeekData {
            std::shared_ptr<const std::vector<uint8_t>> binStore;
            size_t binBegin;
            size_t binEnd;
            std::string cold;
            std::shared_ptr<std::atomic<size_t>> retainedCounter;
            size_t retainedBytes;
            float lapStart;
            int lapNum;
            bool allHistory;
            uint64_t requestId;
            bool authoritativeSeek;
            uint32_t rowTypeMask;
            float historyStart;
            std::optional<tnrp::Sink::RestoreFlushInfo> restore;
            bool reportExceptions;
            std::shared_ptr<FlushState> json;
            Napi::ThreadSafeFunction jsonTsfn;
        };
        const size_t binaryBytes = binStore && binEnd > binBegin ? binEnd - binBegin : 0;
        const size_t retainedBytes = binaryBytes + coldJson.capacity();
        seekFlushBytes_->fetch_add(retainedBytes, std::memory_order_relaxed);
        seekFlushCalls_->fetch_add(1, std::memory_order_relaxed);
        seekFlushPayloadBytes_->fetch_add(retainedBytes, std::memory_order_relaxed);
        updateAtomicMaximum(*seekFlushPeakBytes_, seekFlushBytes_->load(std::memory_order_relaxed));
        auto* d = new SeekData{ std::move(binStore), binBegin, binEnd, std::move(coldJson),
                                seekFlushBytes_, retainedBytes,
                                currentLapStart, lapNum, allHistory, requestId,
                                authoritativeSeek, rowTypeMask, historyStart, restore,
                                nativeExceptionReporting_->load(std::memory_order_relaxed),
                                flush_, tsfn };
        {
            std::lock_guard<std::mutex> lk(flush_->mutex);
            ++flush_->seekHolds;
        }
        auto status = tsfnSeek.NonBlockingCall(
            d, [](Napi::Env env, Napi::Function cb, SeekData* d) {
                std::exception_ptr unreportedException;
                if (env != nullptr && cb != nullptr) {
                    const size_t len = d->binStore && d->binEnd > d->binBegin
                        ? d->binEnd - d->binBegin : 0;
                    const char* stage = "creating the binary Buffer";
                    try {
                        auto buffer = len == 0
                            ? Napi::Buffer<uint8_t>::New(env, 0)
                            : Napi::Buffer<uint8_t>::Copy(
                                env, d->binStore->data() + d->binBegin, len);
                        stage = "creating the cold JSON string";
                        auto cold = Napi::String::New(env, d->cold);
                        stage = "creating the restore ranges";
                        Napi::Value restore = env.Undefined();
                        if (d->restore) {
                            auto object = Napi::Object::New(env);
                            object.Set("chartFrom", Napi::Number::New(env, d->restore->chartFrom));
                            object.Set("includesEvents", Napi::Boolean::New(env, d->restore->includesEvents));
                            object.Set("eventsFrom", Napi::Number::New(env, d->restore->eventsFrom));
                            object.Set("through", Napi::Number::New(env, d->restore->through));
                            object.Set("sessionChanged", Napi::Boolean::New(env, d->restore->sessionChanged));
                            restore = object;
                        }
                        stage = "calling the JavaScript seek callback";
                        cb.Call({ buffer,
                                  cold,
                                  Napi::Number::New(env, d->lapStart),
                                  Napi::Number::New(env, d->lapNum),
                                  Napi::Boolean::New(env, d->allHistory),
                                  Napi::Number::New(env, static_cast<double>(d->requestId)),
                                  Napi::Boolean::New(env, d->authoritativeSeek),
                                  Napi::Number::New(env, d->rowTypeMask),
                                  Napi::Number::New(env, d->historyStart),
                                  env.Undefined(),
                                  restore });
                    } catch (const Napi::Error& error) {
                        if (!d->reportExceptions) {
                            unreportedException = std::current_exception();
                        } else {
                            if (env.IsExceptionPending())
                                (void)env.GetAndClearPendingException();
                            const std::string message =
                                std::string("Seek flush failed while ") + stage +
                                " (requestId=" + std::to_string(d->requestId) +
                                ", binaryBytes=" + std::to_string(len) +
                                ", coldJsonBytes=" + std::to_string(d->cold.size()) +
                                ", rowTypeMask=" + std::to_string(d->rowTypeMask) +
                                "): " + error.Message();
                            try {
                                cb.Call({ env.Null(), env.Null(),
                                          Napi::Number::New(env, d->lapStart),
                                          Napi::Number::New(env, d->lapNum),
                                          Napi::Boolean::New(env, d->allHistory),
                                          Napi::Number::New(env, static_cast<double>(d->requestId)),
                                          Napi::Boolean::New(env, d->authoritativeSeek),
                                          Napi::Number::New(env, d->rowTypeMask),
                                          Napi::Number::New(env, d->historyStart),
                                          Napi::String::New(env, message) });
                            } catch (...) {
                                std::fprintf(stderr, "[native-callback] %s\n", message.c_str());
                                std::fflush(stderr);
                            }
                        }
                    } catch (...) {
                        if (!d->reportExceptions) unreportedException = std::current_exception();
                        else {
                            const std::string message =
                                std::string("Seek flush failed while ") + stage +
                                " (requestId=" + std::to_string(d->requestId) +
                                ", binaryBytes=" + std::to_string(len) +
                                ", coldJsonBytes=" + std::to_string(d->cold.size()) +
                                ", rowTypeMask=" + std::to_string(d->rowTypeMask) +
                                "): unknown native exception";
                            try {
                                cb.Call({ env.Null(), env.Null(),
                                          Napi::Number::New(env, d->lapStart),
                                          Napi::Number::New(env, d->lapNum),
                                          Napi::Boolean::New(env, d->allHistory),
                                          Napi::Number::New(env, static_cast<double>(d->requestId)),
                                          Napi::Boolean::New(env, d->authoritativeSeek),
                                          Napi::Number::New(env, d->rowTypeMask),
                                          Napi::Number::New(env, d->historyStart),
                                          Napi::String::New(env, message) });
                            } catch (...) {
                                std::fprintf(stderr, "[native-callback] %s\n", message.c_str());
                                std::fflush(stderr);
                            }
                        }
                    }
                }
                d->retainedCounter->fetch_sub(d->retainedBytes, std::memory_order_relaxed);
                // The flush has reached JavaScript: rows describing its cursor
                // may follow it now.
                releaseSeekHold(d->json, d->jsonTsfn);
                delete d;
                if (unreportedException) std::rethrow_exception(unreportedException);
            });
        if (status != napi_ok) {
            d->retainedCounter->fetch_sub(d->retainedBytes, std::memory_order_relaxed);
            releaseSeekHold(d->json, d->jsonTsfn);
            delete d;
        }
    }

    void onPairState(const std::string& publicJson,
                     const std::string& persistedJson) override {
        if (!hasPairCb_) return;
        struct PairStateData {
            std::string publicJson;
            std::string persistedJson;
        };
        auto* data = new PairStateData{publicJson, persistedJson};
        const auto status = tsfnPair.NonBlockingCall(
            data, [](Napi::Env env, Napi::Function callback,
                     PairStateData* state) {
                if (env != nullptr && callback != nullptr) {
                    callback.Call({Napi::String::New(env, state->publicJson),
                                   Napi::String::New(env, state->persistedJson)});
                }
                delete state;
            });
        if (status != napi_ok) delete data;
    }

    void onPairDiagnostic(const std::string& message) override {
        if (!hasPairDiagnosticCb_) return;
        auto* data = new std::string(message);
        const auto status = tsfnPairDiagnostic.NonBlockingCall(
            data, [](Napi::Env env, Napi::Function callback,
                     std::string* diagnostic) {
                if (env != nullptr && callback != nullptr)
                    callback.Call({Napi::String::New(env, *diagnostic)});
                delete diagnostic;
            });
        if (status != napi_ok) delete data;
    }

private:
    // Shared so queued flush callbacks remain valid even if the wrapper is torn down.
    struct FlushState {
        std::mutex  mutex;
        std::string pending;    // newline-delimited JSON awaiting delivery
        std::string draining;   // batch currently being handed to JS (reused storage)
        bool        scheduled = false;
        // Rows emitted after a seek flush was handed to its own TSFN describe
        // the new cursor, but the JSON TSFN can run first, while the bridge is
        // still discarding old-cursor rows as it waits for that flush. They are
        // held until the flush callback has run, then delivered.
        std::string held;
        int         seekHolds = 0;
        uint64_t    rowsEnqueued{};
        uint64_t    payloadBytesEnqueued{};
        uint64_t    scheduleAttempts{};
        uint64_t    deliveredBatches{};
        uint64_t    deliveredPayloadBytes{};
        size_t      peakPendingUsedBytes{};
        size_t      peakPendingCapacityBytes{};
        size_t      peakDrainingUsedBytes{};
        size_t      peakDrainingCapacityBytes{};
    };

    void enqueueRows(const std::string* rows, size_t count) {
        if (!rows || count == 0) return;
        auto fs = flush_;  // keep state alive independent of this object's lifetime
        bool schedule = false;
        {
            std::lock_guard<std::mutex> lk(fs->mutex);
            std::string& target = fs->seekHolds > 0 ? fs->held : fs->pending;
            for (size_t index = 0; index < count; ++index) {
                target += rows[index];
                target += '\n';
                ++fs->rowsEnqueued;
                fs->payloadBytesEnqueued += rows[index].size() + 1;
            }
            if (fs->seekHolds > 0) return;
            fs->peakPendingUsedBytes = std::max(fs->peakPendingUsedBytes, fs->pending.size());
            fs->peakPendingCapacityBytes = std::max(
                fs->peakPendingCapacityBytes, fs->pending.capacity());
            if (!fs->scheduled) {
                fs->scheduled = true;
                ++fs->scheduleAttempts;
                schedule = true;
            }
        }
        if (schedule) scheduleJsonFlush(fs, tsfn);
    }

    // Releases one seek hold; the last release delivers the held rows.
    static void releaseSeekHold(const std::shared_ptr<FlushState>& fs,
                                Napi::ThreadSafeFunction jsonTsfn) {
        bool schedule = false;
        {
            std::lock_guard<std::mutex> lk(fs->mutex);
            if (fs->seekHolds > 0) --fs->seekHolds;
            if (fs->seekHolds > 0 || fs->held.empty()) return;
            fs->pending += fs->held;
            fs->held.clear();
            if (!fs->scheduled) {
                fs->scheduled = true;
                ++fs->scheduleAttempts;
                schedule = true;
            }
        }
        if (schedule) scheduleJsonFlush(fs, jsonTsfn);
    }

    static void scheduleJsonFlush(const std::shared_ptr<FlushState>& fs,
                                  Napi::ThreadSafeFunction& jsonTsfn) {
        const auto status = jsonTsfn.NonBlockingCall([fs](Napi::Env env, Napi::Function cb) {
            {
                std::lock_guard<std::mutex> lk(fs->mutex);
                fs->draining.swap(fs->pending);  // grab the batch; pending keeps reusable storage
                fs->scheduled = false;
                fs->peakDrainingUsedBytes = std::max(
                    fs->peakDrainingUsedBytes, fs->draining.size());
                fs->peakDrainingCapacityBytes = std::max(
                    fs->peakDrainingCapacityBytes, fs->draining.capacity());
            }
            if (env != nullptr && cb != nullptr) {
                cb.Call({ Napi::String::New(env, fs->draining) });
            }
            std::lock_guard<std::mutex> lk(fs->mutex);
            ++fs->deliveredBatches;
            fs->deliveredPayloadBytes += fs->draining.size();
            fs->draining.clear();                // retain capacity for the next swap
        });

        if (status != napi_ok) {
            // Couldn't schedule; clear the flag so a later row retries the flush.
            std::lock_guard<std::mutex> lk(fs->mutex);
            fs->scheduled = false;
        }
    }

    // Shared so queued binary flush callbacks remain valid past teardown.
    struct BinFlushState {
        std::mutex           mutex;
        std::vector<uint8_t> pending;    // bytes awaiting delivery
        std::vector<uint8_t> draining;   // batch currently handed to JS (reused storage)
        bool                 scheduled = false;
        bool                 discard = false;
        uint64_t             generation = 0;
        uint64_t             scheduledGeneration = 0;
        uint64_t             writesEnqueued{};
        uint64_t             payloadBytesEnqueued{};
        uint64_t             scheduleAttempts{};
        uint64_t             deliveredBatches{};
        uint64_t             deliveredPayloadBytes{};
        size_t               peakPendingUsedBytes{};
        size_t               peakPendingCapacityBytes{};
        size_t               peakDrainingUsedBytes{};
        size_t               peakDrainingCapacityBytes{};
    };

    std::shared_ptr<tnrp::Engine> engine;
    Napi::ThreadSafeFunction tsfn;
    Napi::ThreadSafeFunction tsfnBin;
    Napi::ThreadSafeFunction tsfnSeek;
    Napi::ThreadSafeFunction tsfnPair;
    Napi::ThreadSafeFunction tsfnPairDiagnostic;
    bool destroyed_ = false;
    bool hasBinCb_  = false;
    bool hasSeekCb_ = false;
    bool hasPairCb_ = false;
    bool hasPairDiagnosticCb_ = false;
    std::shared_ptr<FlushState>    flush_    = std::make_shared<FlushState>();
    std::shared_ptr<BinFlushState> binFlush_ = std::make_shared<BinFlushState>();
    std::shared_ptr<std::atomic<size_t>> seekFlushBytes_ =
        std::make_shared<std::atomic<size_t>>(0);
    std::shared_ptr<std::atomic<size_t>> seekFlushPeakBytes_ =
        std::make_shared<std::atomic<size_t>>(0);
    std::shared_ptr<std::atomic<uint64_t>> seekFlushCalls_ =
        std::make_shared<std::atomic<uint64_t>>(0);
    std::shared_ptr<std::atomic<uint64_t>> seekFlushPayloadBytes_ =
        std::make_shared<std::atomic<uint64_t>>(0);
    std::shared_ptr<std::atomic<bool>> nativeExceptionReporting_ =
        std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<bool>> loadBusy_ = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<AnalysisReaderState> analysisReader_ = std::make_shared<AnalysisReaderState>();
    std::shared_ptr<OrderedEngineCalls> orderedCalls_ = std::make_shared<OrderedEngineCalls>();

    Napi::Value StartUdp(const Napi::CallbackInfo& info) {
        TRACE("StartUdp: calling engine->startUdp()");
        bool ok = engine->startUdp();
        TRACE("StartUdp: returned");
        return Napi::Boolean::New(info.Env(), ok);
    }

    // (port, bindAddress, forwardTargets) -> Promise<string>: "" or the bind error.
    Napi::Value RestartUdp(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        const double port = info.Length() >= 1 && info[0].IsNumber()
            ? info[0].As<Napi::Number>().DoubleValue() : 0.0;
        if (!engine || !std::isfinite(port) || std::trunc(port) != port || port < 1 || port > 65535) {
            auto deferred = Napi::Promise::Deferred::New(env);
            deferred.Resolve(Napi::String::New(env, engine
                ? "UDP port must be an integer from 1 to 65535"
                : "The telemetry engine is not available."));
            return deferred.Promise();
        }
        auto* worker = new UdpRestartWorker(env, engine, static_cast<uint16_t>(port),
            info.Length() >= 2 && info[1].IsString()
                ? info[1].As<Napi::String>().Utf8Value() : std::string("0.0.0.0"),
            info.Length() >= 3 ? readForwardTargets(info[2]) : std::vector<tnrp::UdpForwardTarget>{});
        Napi::Promise promise = worker->GetPromise();
        worker->Queue();
        return promise;
    }

    Napi::Value PairStart(const Napi::CallbackInfo& info) {
        std::string error;
        if (engine && engine->pairStart(&error)) return info.Env().Null();
        return Napi::String::New(info.Env(), error.empty()
            ? "The paired-display server is unavailable." : error);
    }

    Napi::Value PairStop(const Napi::CallbackInfo& info) {
        const bool persistDisabled = info.Length() == 0 || !info[0].IsBoolean() ||
            info[0].As<Napi::Boolean>().Value();
        if (engine) engine->pairStop(persistDisabled);
        return PairGetState(info);
    }

    Napi::Value PairOpenWindow(const Napi::CallbackInfo& info) {
        if (engine) engine->pairOpenWindow();
        return PairGetState(info);
    }

    Napi::Value PairCloseWindow(const Napi::CallbackInfo& info) {
        if (engine) engine->pairCloseWindow();
        return PairGetState(info);
    }

    Napi::Value PairRespond(const Napi::CallbackInfo& info) {
        if (engine && info.Length() >= 1 && info[0].IsBoolean())
            engine->pairRespond(info[0].As<Napi::Boolean>().Value());
        return PairGetState(info);
    }

    Napi::Value PairRemoveDevice(const Napi::CallbackInfo& info) {
        if (engine && info.Length() >= 1 && info[0].IsString())
            engine->pairRemoveDevice(info[0].As<Napi::String>().Utf8Value());
        return PairGetState(info);
    }

    Napi::Value PairGetState(const Napi::CallbackInfo& info) {
        return Napi::String::New(info.Env(), engine
            ? engine->pairStateJson() : "{}");
    }

    Napi::Value UdpLastError(const Napi::CallbackInfo& info) {
        return Napi::String::New(info.Env(), engine->udpLastError());
    }

    void SetDiagnosticsEnabled(const Napi::CallbackInfo& info) {
        if (engine && info.Length() >= 1)
            engine->setDiagnosticsEnabled(info[0].ToBoolean().Value());
    }

    void SetNativeExceptionReporting(const Napi::CallbackInfo& info) {
        if (info.Length() >= 1)
            nativeExceptionReporting_->store(
                info[0].ToBoolean().Value(), std::memory_order_relaxed);
    }

    Napi::Value DatagramsReceived(const Napi::CallbackInfo& info) {
        return Napi::Number::New(info.Env(),
            engine ? static_cast<double>(engine->datagramsReceived()) : 0.0);
    }

    Napi::Value LiveDiagnostics(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        Napi::Object out = Napi::Object::New(env);
        if (!engine) return out;
        const auto snapshot = engine->liveDiagnostics();
        out.Set("udpRunning", snapshot.udpRunning);
        out.Set("inPlayback", snapshot.inPlayback);
        out.Set("recording", snapshot.recording);
        out.Set("datagrams", Napi::Number::New(env, static_cast<double>(snapshot.datagrams)));
        out.Set("bytes", Napi::Number::New(env, static_cast<double>(snapshot.bytes)));
        out.Set("tooShort", Napi::Number::New(env, static_cast<double>(snapshot.tooShort)));
        out.Set("unsupportedFormat", Napi::Number::New(env, static_cast<double>(snapshot.unsupportedFormat)));
        out.Set("parserDropped", Napi::Number::New(env, static_cast<double>(snapshot.parserDropped)));
        out.Set("accepted", Napi::Number::New(env, static_cast<double>(snapshot.accepted)));
        out.Set("rowsProduced", Napi::Number::New(env, static_cast<double>(snapshot.rowsProduced)));
        out.Set("binaryBytesProduced", Napi::Number::New(env, static_cast<double>(snapshot.binaryBytesProduced)));
        out.Set("noOutput", Napi::Number::New(env, static_cast<double>(snapshot.noOutput)));
        Napi::Object packetIds = Napi::Object::New(env);
        for (size_t i = 0; i < snapshot.packetIds.size(); ++i) {
            if (snapshot.packetIds[i] == 0) continue;
            packetIds.Set(i == 17 ? "other" : std::to_string(i),
                Napi::Number::New(env, static_cast<double>(snapshot.packetIds[i])));
        }
        out.Set("packetIds", packetIds);
        Napi::Object formats = Napi::Object::New(env);
        formats.Set("2024", Napi::Number::New(env, static_cast<double>(snapshot.format2024)));
        formats.Set("2025", Napi::Number::New(env, static_cast<double>(snapshot.format2025)));
        formats.Set("2026", Napi::Number::New(env, static_cast<double>(snapshot.format2026)));
        out.Set("formats", formats);
        out.Set("lastIncomingFormat", snapshot.lastIncomingFormat);
        out.Set("lastPacketId", snapshot.lastPacketId);
        out.Set("lastDatagramLength", snapshot.lastDatagramLength);
        out.Set("lastSessionTime", snapshot.lastSessionTime);
        out.Set("consumerRowMask", snapshot.consumerRowMask);
        out.Set("consumerHistoryMask", snapshot.consumerHistoryMask);
        out.Set("consumerWindowSeconds", snapshot.consumerWindowSeconds);
        return out;
    }

    Napi::Value SetOverride(const Napi::CallbackInfo& info) {
        if (info.Length() >= 1 && info[0].IsString()) {
            std::string ovr = info[0].As<Napi::String>().Utf8Value();
            if (ovr == "auto") engine->setOverride(tnrp::Override::Auto);
            else if (ovr == "f1_24") engine->setOverride(tnrp::Override::F1_24);
            else if (ovr == "f1_25") engine->setOverride(tnrp::Override::F1_25);
            else if (ovr == "f1_26") engine->setOverride(tnrp::Override::F1_26);
        }
        return info.Env().Undefined();
    }

    Napi::Value TelemetryRetention(const Napi::CallbackInfo& info) {
        size_t jsonPendingBytes = 0;
        size_t jsonDrainingBytes = 0;
        size_t jsonPendingUsed = 0;
        size_t jsonDrainingUsed = 0;
        uint64_t jsonRowsEnqueued = 0;
        uint64_t jsonPayloadBytesEnqueued = 0;
        uint64_t jsonScheduleAttempts = 0;
        uint64_t jsonDeliveredBatches = 0;
        uint64_t jsonDeliveredPayloadBytes = 0;
        size_t jsonPeakPendingUsedBytes = 0;
        size_t jsonPeakPendingCapacityBytes = 0;
        size_t jsonPeakDrainingUsedBytes = 0;
        size_t jsonPeakDrainingCapacityBytes = 0;
        {
            std::lock_guard<std::mutex> lock(flush_->mutex);
            jsonPendingBytes = flush_->pending.capacity();
            jsonDrainingBytes = flush_->draining.capacity();
            jsonPendingUsed = flush_->pending.size();
            jsonDrainingUsed = flush_->draining.size();
            jsonRowsEnqueued = flush_->rowsEnqueued;
            jsonPayloadBytesEnqueued = flush_->payloadBytesEnqueued;
            jsonScheduleAttempts = flush_->scheduleAttempts;
            jsonDeliveredBatches = flush_->deliveredBatches;
            jsonDeliveredPayloadBytes = flush_->deliveredPayloadBytes;
            jsonPeakPendingUsedBytes = flush_->peakPendingUsedBytes;
            jsonPeakPendingCapacityBytes = flush_->peakPendingCapacityBytes;
            jsonPeakDrainingUsedBytes = flush_->peakDrainingUsedBytes;
            jsonPeakDrainingCapacityBytes = flush_->peakDrainingCapacityBytes;
        }

        size_t binaryPendingBytes = 0;
        size_t binaryDrainingBytes = 0;
        size_t binaryPendingUsed = 0;
        size_t binaryDrainingUsed = 0;
        uint64_t binaryWritesEnqueued = 0;
        uint64_t binaryPayloadBytesEnqueued = 0;
        uint64_t binaryScheduleAttempts = 0;
        uint64_t binaryDeliveredBatches = 0;
        uint64_t binaryDeliveredPayloadBytes = 0;
        size_t binaryPeakPendingUsedBytes = 0;
        size_t binaryPeakPendingCapacityBytes = 0;
        size_t binaryPeakDrainingUsedBytes = 0;
        size_t binaryPeakDrainingCapacityBytes = 0;
        {
            std::lock_guard<std::mutex> lock(binFlush_->mutex);
            binaryPendingBytes = binFlush_->pending.capacity();
            binaryDrainingBytes = binFlush_->draining.capacity();
            binaryPendingUsed = binFlush_->pending.size();
            binaryDrainingUsed = binFlush_->draining.size();
            binaryWritesEnqueued = binFlush_->writesEnqueued;
            binaryPayloadBytesEnqueued = binFlush_->payloadBytesEnqueued;
            binaryScheduleAttempts = binFlush_->scheduleAttempts;
            binaryDeliveredBatches = binFlush_->deliveredBatches;
            binaryDeliveredPayloadBytes = binFlush_->deliveredPayloadBytes;
            binaryPeakPendingUsedBytes = binFlush_->peakPendingUsedBytes;
            binaryPeakPendingCapacityBytes = binFlush_->peakPendingCapacityBytes;
            binaryPeakDrainingUsedBytes = binFlush_->peakDrainingUsedBytes;
            binaryPeakDrainingCapacityBytes = binFlush_->peakDrainingCapacityBytes;
        }

        const size_t seekBytes = seekFlushBytes_->load(std::memory_order_relaxed);
        const auto history = engine
            ? engine->liveHistoryMemoryStats()
            : tnrp::Engine::LiveHistoryMemoryStats{};
        const auto writer = engine
            ? engine->writerMemoryStats()
            : tnrp::TnrdWriter::MemoryStats{};
        const auto strategy = engine
            ? engine->strategyMemoryStats()
            : tnrp::Engine::StrategyMemoryStats{};
        const auto runtime = engine
            ? engine->runtimeMemoryStats()
            : tnrp::Engine::RuntimeMemoryStats{};
        const size_t retainedBytes = jsonPendingBytes + jsonDrainingBytes +
            binaryPendingBytes + binaryDrainingBytes + seekBytes + writer.retainedBytes +
            strategy.retainedBytes + runtime.retainedBytes;
        Napi::Object result = Napi::Object::New(info.Env());
        result.Set("retained_bytes", Napi::Number::New(info.Env(),
            static_cast<double>(retainedBytes)));
        result.Set("byte_basis", Napi::String::New(info.Env(),
            "reserved native flush/seek payload plus estimated engine-cache, writer, and Strategy retained allocation capacity; allocator overhead and transient allocations are reported separately, not added to retained_bytes"));

        Napi::Object json = Napi::Object::New(info.Env());
        json.Set("pending_bytes", Napi::Number::New(info.Env(),
            static_cast<double>(jsonPendingBytes)));
        json.Set("pending_used_bytes", Napi::Number::New(info.Env(),
            static_cast<double>(jsonPendingUsed)));
        json.Set("draining_bytes", Napi::Number::New(info.Env(),
            static_cast<double>(jsonDrainingBytes)));
        json.Set("draining_used_bytes", Napi::Number::New(info.Env(),
            static_cast<double>(jsonDrainingUsed)));
        Napi::Object jsonActivity = Napi::Object::New(info.Env());
        jsonActivity.Set("rows_enqueued", Napi::Number::New(info.Env(), static_cast<double>(jsonRowsEnqueued)));
        jsonActivity.Set("payload_bytes_enqueued", Napi::Number::New(info.Env(), static_cast<double>(jsonPayloadBytesEnqueued)));
        jsonActivity.Set("schedule_attempts", Napi::Number::New(info.Env(), static_cast<double>(jsonScheduleAttempts)));
        jsonActivity.Set("delivered_batches", Napi::Number::New(info.Env(), static_cast<double>(jsonDeliveredBatches)));
        jsonActivity.Set("delivered_payload_bytes", Napi::Number::New(info.Env(), static_cast<double>(jsonDeliveredPayloadBytes)));
        jsonActivity.Set("peak_pending_used_bytes", Napi::Number::New(info.Env(), static_cast<double>(jsonPeakPendingUsedBytes)));
        jsonActivity.Set("peak_pending_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(jsonPeakPendingCapacityBytes)));
        jsonActivity.Set("peak_draining_used_bytes", Napi::Number::New(info.Env(), static_cast<double>(jsonPeakDrainingUsedBytes)));
        jsonActivity.Set("peak_draining_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(jsonPeakDrainingCapacityBytes)));
        json.Set("activity", jsonActivity);
        result.Set("json_flush", json);

        Napi::Object binary = Napi::Object::New(info.Env());
        binary.Set("pending_bytes", Napi::Number::New(info.Env(),
            static_cast<double>(binaryPendingBytes)));
        binary.Set("pending_used_bytes", Napi::Number::New(info.Env(),
            static_cast<double>(binaryPendingUsed)));
        binary.Set("draining_bytes", Napi::Number::New(info.Env(),
            static_cast<double>(binaryDrainingBytes)));
        binary.Set("draining_used_bytes", Napi::Number::New(info.Env(),
            static_cast<double>(binaryDrainingUsed)));
        Napi::Object binaryActivity = Napi::Object::New(info.Env());
        binaryActivity.Set("writes_enqueued", Napi::Number::New(info.Env(), static_cast<double>(binaryWritesEnqueued)));
        binaryActivity.Set("payload_bytes_enqueued", Napi::Number::New(info.Env(), static_cast<double>(binaryPayloadBytesEnqueued)));
        binaryActivity.Set("schedule_attempts", Napi::Number::New(info.Env(), static_cast<double>(binaryScheduleAttempts)));
        binaryActivity.Set("delivered_batches", Napi::Number::New(info.Env(), static_cast<double>(binaryDeliveredBatches)));
        binaryActivity.Set("delivered_payload_bytes", Napi::Number::New(info.Env(), static_cast<double>(binaryDeliveredPayloadBytes)));
        binaryActivity.Set("peak_pending_used_bytes", Napi::Number::New(info.Env(), static_cast<double>(binaryPeakPendingUsedBytes)));
        binaryActivity.Set("peak_pending_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(binaryPeakPendingCapacityBytes)));
        binaryActivity.Set("peak_draining_used_bytes", Napi::Number::New(info.Env(), static_cast<double>(binaryPeakDrainingUsedBytes)));
        binaryActivity.Set("peak_draining_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(binaryPeakDrainingCapacityBytes)));
        binary.Set("activity", binaryActivity);
        result.Set("binary_flush", binary);
        result.Set("seek_flush_in_flight_bytes", Napi::Number::New(info.Env(),
            static_cast<double>(seekBytes)));
        Napi::Object seekActivity = Napi::Object::New(info.Env());
        seekActivity.Set("calls", Napi::Number::New(info.Env(), static_cast<double>(seekFlushCalls_->load(std::memory_order_relaxed))));
        seekActivity.Set("payload_bytes_created", Napi::Number::New(info.Env(), static_cast<double>(seekFlushPayloadBytes_->load(std::memory_order_relaxed))));
        seekActivity.Set("peak_in_flight_bytes", Napi::Number::New(info.Env(), static_cast<double>(seekFlushPeakBytes_->load(std::memory_order_relaxed))));
        result.Set("seek_flush_activity", seekActivity);

        Napi::Object runtimeStats = Napi::Object::New(info.Env());
        runtimeStats.Set("retained_bytes", Napi::Number::New(info.Env(), static_cast<double>(runtime.retainedBytes)));
        runtimeStats.Set("estimate_basis", Napi::String::New(info.Env(),
            "duplicate/latest-row/playback-path string capacities; transient parser-result and filtered-binary allocation activity is reported separately"));
        Napi::Object engineCaches = Napi::Object::New(info.Env());
        engineCaches.Set("duplicate_used_bytes", Napi::Number::New(info.Env(), static_cast<double>(runtime.duplicateCacheUsedBytes)));
        engineCaches.Set("duplicate_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(runtime.duplicateCacheCapacityBytes)));
        engineCaches.Set("latest_row_used_bytes", Napi::Number::New(info.Env(), static_cast<double>(runtime.latestRowCacheUsedBytes)));
        engineCaches.Set("latest_row_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(runtime.latestRowCacheCapacityBytes)));
        engineCaches.Set("playback_path_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(runtime.playbackPathCapacityBytes)));
        runtimeStats.Set("caches", engineCaches);
        Napi::Object parserActivity = Napi::Object::New(info.Env());
        parserActivity.Set("datagrams_processed", Napi::Number::New(info.Env(), static_cast<double>(runtime.datagramsProcessed)));
        parserActivity.Set("datagram_bytes_processed", Napi::Number::New(info.Env(), static_cast<double>(runtime.datagramBytesProcessed)));
        parserActivity.Set("rows_produced", Napi::Number::New(info.Env(), static_cast<double>(runtime.parserRowsProduced)));
        parserActivity.Set("control_rows_produced", Napi::Number::New(info.Env(), static_cast<double>(runtime.parserControlRowsProduced)));
        parserActivity.Set("hot_json_rows_produced", Napi::Number::New(info.Env(), static_cast<double>(runtime.parserHotJsonRowsProduced)));
        parserActivity.Set("json_bytes_produced", Napi::Number::New(info.Env(), static_cast<double>(runtime.parserJsonBytesProduced)));
        parserActivity.Set("binary_bytes_produced", Napi::Number::New(info.Env(), static_cast<double>(runtime.parserBinaryBytesProduced)));
        parserActivity.Set("result_capacity_bytes_allocated", Napi::Number::New(info.Env(), static_cast<double>(runtime.parserResultCapacityBytesAllocated)));
        parserActivity.Set("last_result_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(runtime.lastParserResultCapacityBytes)));
        parserActivity.Set("peak_result_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(runtime.peakParserResultCapacityBytes)));
        runtimeStats.Set("parser_activity", parserActivity);
        Napi::Object filteredBinaryActivity = Napi::Object::New(info.Env());
        filteredBinaryActivity.Set("batches", Napi::Number::New(info.Env(), static_cast<double>(runtime.filteredBinaryBatches)));
        filteredBinaryActivity.Set("bytes_produced", Napi::Number::New(info.Env(), static_cast<double>(runtime.filteredBinaryBytesProduced)));
        filteredBinaryActivity.Set("capacity_bytes_allocated", Napi::Number::New(info.Env(), static_cast<double>(runtime.filteredBinaryCapacityBytesAllocated)));
        filteredBinaryActivity.Set("last_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(runtime.lastFilteredBinaryCapacityBytes)));
        filteredBinaryActivity.Set("peak_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(runtime.peakFilteredBinaryCapacityBytes)));
        runtimeStats.Set("filtered_binary_activity", filteredBinaryActivity);
        result.Set("engine_runtime", runtimeStats);

        Napi::Object strategyStats = Napi::Object::New(info.Env());
        strategyStats.Set("subscribed", Napi::Boolean::New(info.Env(), strategy.subscribed));
        strategyStats.Set("retained_bytes", Napi::Number::New(info.Env(), static_cast<double>(strategy.retainedBytes)));
        strategyStats.Set("estimate_basis", Napi::String::New(info.Env(),
            "Strategy processor, rollback checkpoints/journal, queued/active work payload capacities, and cached snapshots; shared JSON may overlap live-history and work attribution"));
        strategyStats.Set("cache_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(strategy.cacheCapacityBytes)));
        Napi::Object strategyQueue = Napi::Object::New(info.Env());
        strategyQueue.Set("work_items", Napi::Number::New(info.Env(), static_cast<double>(strategy.queuedWorkItems)));
        strategyQueue.Set("rows", Napi::Number::New(info.Env(), static_cast<double>(strategy.queuedRows)));
        strategyQueue.Set("json_bytes", Napi::Number::New(info.Env(), static_cast<double>(strategy.queuedJsonBytes)));
        strategyQueue.Set("retained_bytes", Napi::Number::New(info.Env(), static_cast<double>(strategy.queuedRetainedBytes)));
        strategyQueue.Set("oldest_work_age_ms", Napi::Number::New(info.Env(), static_cast<double>(strategy.oldestQueuedWorkAgeMs)));
        strategyQueue.Set("peak_rows", Napi::Number::New(info.Env(), static_cast<double>(strategy.peakQueuedRows)));
        strategyQueue.Set("peak_retained_bytes", Napi::Number::New(info.Env(), static_cast<double>(strategy.peakQueuedRetainedBytes)));
        strategyStats.Set("work_queue", strategyQueue);
        Napi::Object strategyActive = Napi::Object::New(info.Env());
        strategyActive.Set("work_items", Napi::Number::New(info.Env(), static_cast<double>(strategy.activeWorkItems)));
        strategyActive.Set("rows", Napi::Number::New(info.Env(), static_cast<double>(strategy.activeRows)));
        strategyActive.Set("json_bytes", Napi::Number::New(info.Env(), static_cast<double>(strategy.activeJsonBytes)));
        strategyActive.Set("retained_bytes", Napi::Number::New(info.Env(), static_cast<double>(strategy.activeRetainedBytes)));
        strategyStats.Set("active_work", strategyActive);
        Napi::Object strategyRollback = Napi::Object::New(info.Env());
        strategyRollback.Set("retained_bytes", Napi::Number::New(info.Env(), static_cast<double>(strategy.rollback.retainedBytes)));
        strategyRollback.Set("checkpoints", Napi::Number::New(info.Env(), static_cast<double>(strategy.rollback.checkpoints)));
        strategyRollback.Set("journal_entries", Napi::Number::New(info.Env(), static_cast<double>(strategy.rollback.rows)));
        strategyRollback.Set("rollbacks", Napi::Number::New(info.Env(), static_cast<double>(strategy.rollback.rollbacks)));
        strategyRollback.Set("replayed_rows", Napi::Number::New(info.Env(), static_cast<double>(strategy.rollback.replayedRows)));
        strategyRollback.Set("fallbacks", Napi::Number::New(info.Env(), static_cast<double>(strategy.rollback.fallbacks)));
        strategyStats.Set("rollback", strategyRollback);
        Napi::Object strategyActivity = Napi::Object::New(info.Env());
        strategyActivity.Set("input_rows_enqueued", Napi::Number::New(info.Env(), static_cast<double>(strategy.inputRowsEnqueued)));
        strategyActivity.Set("input_json_bytes_enqueued", Napi::Number::New(info.Env(), static_cast<double>(strategy.inputJsonBytesEnqueued)));
        strategyActivity.Set("rows_processed", Napi::Number::New(info.Env(), static_cast<double>(strategy.rowsProcessed)));
        strategyActivity.Set("json_bytes_processed", Napi::Number::New(info.Env(), static_cast<double>(strategy.jsonBytesProcessed)));
        strategyActivity.Set("snapshots_generated", Napi::Number::New(info.Env(), static_cast<double>(strategy.snapshotsGenerated)));
        strategyActivity.Set("snapshots_emitted", Napi::Number::New(info.Env(), static_cast<double>(strategy.snapshotsEmitted)));
        strategyActivity.Set("snapshot_json_bytes_generated", Napi::Number::New(info.Env(), static_cast<double>(strategy.snapshotJsonBytesGenerated)));
        strategyActivity.Set("last_snapshot_json_bytes", Napi::Number::New(info.Env(), static_cast<double>(strategy.lastSnapshotJsonBytes)));
        strategyActivity.Set("last_snapshot_json_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(strategy.lastSnapshotJsonCapacityBytes)));
        strategyActivity.Set("peak_snapshot_json_bytes", Napi::Number::New(info.Env(), static_cast<double>(strategy.peakSnapshotJsonBytes)));
        strategyStats.Set("activity", strategyActivity);
        Napi::Object strategyProcessor = Napi::Object::New(info.Env());
        strategyProcessor.Set("retained_bytes", Napi::Number::New(info.Env(), static_cast<double>(strategy.processor.retainedBytes)));
        strategyProcessor.Set("cached_input_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(strategy.processor.cachedInputCapacityBytes)));
        strategyProcessor.Set("container_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(strategy.processor.containerCapacityBytes)));
        strategyProcessor.Set("string_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(strategy.processor.stringCapacityBytes)));
        strategyProcessor.Set("lap_time_entries", Napi::Number::New(info.Env(), static_cast<double>(strategy.processor.lapTimeEntries)));
        strategyProcessor.Set("rival_entries", Napi::Number::New(info.Env(), static_cast<double>(strategy.processor.rivalEntries)));
        strategyProcessor.Set("rival_recent_lap_entries", Napi::Number::New(info.Env(), static_cast<double>(strategy.processor.rivalRecentLapEntries)));
        strategyProcessor.Set("wear_history_entries", Napi::Number::New(info.Env(), static_cast<double>(strategy.processor.wearHistoryEntries)));
        strategyProcessor.Set("frozen_neutral_car_entries", Napi::Number::New(info.Env(), static_cast<double>(strategy.processor.frozenNeutralCarEntries)));
        strategyProcessor.Set("decision_history_entries", Napi::Number::New(info.Env(), static_cast<double>(strategy.processor.decisionHistoryEntries)));
        strategyProcessor.Set("conservative_past_entries", Napi::Number::New(info.Env(), static_cast<double>(strategy.processor.conservativePastEntries)));
        strategyProcessor.Set("aggressive_past_entries", Napi::Number::New(info.Env(), static_cast<double>(strategy.processor.aggressivePastEntries)));
        strategyProcessor.Set("required_lap_entries", Napi::Number::New(info.Env(), static_cast<double>(strategy.processor.requiredLapEntries)));
        strategyProcessor.Set("display_lap_entries", Napi::Number::New(info.Env(), static_cast<double>(strategy.processor.displayLapEntries)));
        strategyProcessor.Set("display_history_bytes", Napi::Number::New(info.Env(), static_cast<double>(strategy.processor.displayHistoryBytes)));
        strategyStats.Set("processor", strategyProcessor);
        result.Set("strategy", strategyStats);

        Napi::Object liveHistory = Napi::Object::New(info.Env());
        liveHistory.Set("retained_bytes", Napi::Number::New(info.Env(), static_cast<double>(history.retainedBytes)));
        liveHistory.Set("estimate_basis", Napi::String::New(info.Env(),
            "the live V6 store is the recording writer's V6 writer kept in memory; its builders, pending laps, committed chunk frames and shared records are counted once, under writer.retained_bytes, so retained_bytes here is 0; read images are transient"));
        liveHistory.Set("session_retained", Napi::Boolean::New(info.Env(), history.sessionRetained));
        liveHistory.Set("file_attached", Napi::Boolean::New(info.Env(), history.fileAttached));
        Napi::Object historyBuilders = Napi::Object::New(info.Env());
        historyBuilders.Set("count", Napi::Number::New(info.Env(), static_cast<double>(history.builderCount)));
        historyBuilders.Set("bytes", Napi::Number::New(info.Env(), static_cast<double>(history.builderBytes)));
        historyBuilders.Set("capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(history.builderCapacityBytes)));
        historyBuilders.Set("pending_laps", Napi::Number::New(info.Env(), static_cast<double>(history.pendingLapCount)));
        historyBuilders.Set("pending_lap_bytes", Napi::Number::New(info.Env(), static_cast<double>(history.pendingLapBytes)));
        liveHistory.Set("builders", historyBuilders);
        Napi::Object historyCommitted = Napi::Object::New(info.Env());
        historyCommitted.Set("laps", Napi::Number::New(info.Env(), static_cast<double>(history.committedLapCount)));
        historyCommitted.Set("chunks", Napi::Number::New(info.Env(), static_cast<double>(history.chunkCount)));
        historyCommitted.Set("chunk_bytes", Napi::Number::New(info.Env(), static_cast<double>(history.chunkBytes)));
        historyCommitted.Set("shared_records", Napi::Number::New(info.Env(), static_cast<double>(history.sharedRecords)));
        historyCommitted.Set("shared_bytes", Napi::Number::New(info.Env(), static_cast<double>(history.sharedBytes)));
        historyCommitted.Set("uncommitted_laps", Napi::Number::New(info.Env(), static_cast<double>(history.uncommittedLaps)));
        liveHistory.Set("committed", historyCommitted);
        Napi::Object historyCompression = Napi::Object::New(info.Env());
        historyCompression.Set("chunk_writes", Napi::Number::New(info.Env(), static_cast<double>(history.chunkWrites)));
        historyCompression.Set("plain_bytes_processed", Napi::Number::New(info.Env(), static_cast<double>(history.chunkPlainBytesProcessed)));
        historyCompression.Set("compressed_bytes", Napi::Number::New(info.Env(), static_cast<double>(history.chunkCompressedBytes)));
        historyCompression.Set("scratch_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(history.compressionScratchCapacityBytes)));
        historyCompression.Set("context_bytes", Napi::Number::New(info.Env(), static_cast<double>(history.compressionContextBytes)));
        liveHistory.Set("compression_activity", historyCompression);
        Napi::Object historyWork = Napi::Object::New(info.Env());
        historyWork.Set("queued_jobs", Napi::Number::New(info.Env(), static_cast<double>(history.queuedJobs)));
        historyWork.Set("queued_rows", Napi::Number::New(info.Env(), static_cast<double>(history.queuedRows)));
        historyWork.Set("queued_row_bytes", Napi::Number::New(info.Env(), static_cast<double>(history.queuedRowBytes)));
        historyWork.Set("queued_reads", Napi::Number::New(info.Env(), static_cast<double>(history.queuedReads)));
        historyWork.Set("images_received", Napi::Number::New(info.Env(), static_cast<double>(history.imagesReceived)));
        historyWork.Set("reads_completed", Napi::Number::New(info.Env(), static_cast<double>(history.readsCompleted)));
        historyWork.Set("last_image_encoded_bytes", Napi::Number::New(info.Env(), static_cast<double>(history.lastImageEncodedBytes)));
        historyWork.Set("peak_image_encoded_bytes", Napi::Number::New(info.Env(), static_cast<double>(history.peakImageEncodedBytes)));
        liveHistory.Set("work", historyWork);
        result.Set("live_history", liveHistory);

        Napi::Object writerStats = Napi::Object::New(info.Env());
        writerStats.Set("stream_active", Napi::Boolean::New(info.Env(), writer.streamActive));
        writerStats.Set("retained_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.retainedBytes)));
        writerStats.Set("estimate_basis", Napi::String::New(info.Env(),
            "queued event object/payload allocation plus rolling, dedupe, V6 builder, reusable compression scratch, chunk-index, branch, event, and lap-status capacities, and, while the live session is retained in memory, its committed chunk frames and shared records; transient activity is reported separately; allocator and map/deque node overhead excluded"));
        writerStats.Set("queued_events", Napi::Number::New(info.Env(), static_cast<double>(writer.queuedEvents)));
        writerStats.Set("queued_retained_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.queuedRetainedBytes)));
        writerStats.Set("queued_record_events", Napi::Number::New(info.Env(), static_cast<double>(writer.queuedRecordEvents)));
        writerStats.Set("queued_note_packet_events", Napi::Number::New(info.Env(), static_cast<double>(writer.queuedNotePacketEvents)));
        writerStats.Set("queued_control_events", Napi::Number::New(info.Env(), static_cast<double>(writer.queuedControlEvents)));
        writerStats.Set("queued_json_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.queuedJsonBytes)));
        writerStats.Set("queued_packet_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.queuedPacketBytes)));
        writerStats.Set("oldest_queued_event_age_ms", Napi::Number::New(info.Env(), static_cast<double>(writer.oldestQueuedEventAgeMs)));

        Napi::Object rolling = Napi::Object::New(info.Env());
        rolling.Set("entries", Napi::Number::New(info.Env(), static_cast<double>(writer.rollingEntries)));
        rolling.Set("payload_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.rollingPayloadBytes)));
        rolling.Set("payload_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.rollingPayloadCapacityBytes)));
        rolling.Set("container_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.rollingContainerCapacityBytes)));
        Napi::Object rollingActivity = Napi::Object::New(info.Env());
        rollingActivity.Set("batches", Napi::Number::New(info.Env(), static_cast<double>(writer.rollingFlushBatches)));
        rollingActivity.Set("entries_processed", Napi::Number::New(info.Env(), static_cast<double>(writer.rollingFlushEntriesProcessed)));
        rollingActivity.Set("payload_bytes_processed", Napi::Number::New(info.Env(), static_cast<double>(writer.rollingFlushPayloadBytesProcessed)));
        rollingActivity.Set("last_entries", Napi::Number::New(info.Env(), static_cast<double>(writer.lastRollingFlushEntries)));
        rollingActivity.Set("last_payload_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.lastRollingFlushPayloadBytes)));
        rollingActivity.Set("last_copy_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.lastRollingFlushCopyCapacityBytes)));
        rollingActivity.Set("peak_entries", Napi::Number::New(info.Env(), static_cast<double>(writer.peakRollingFlushEntries)));
        rollingActivity.Set("peak_payload_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.peakRollingFlushPayloadBytes)));
        rollingActivity.Set("peak_copy_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.peakRollingFlushCopyCapacityBytes)));
        rolling.Set("flush_copy_activity", rollingActivity);
        writerStats.Set("rolling_buffer", rolling);

        Napi::Object appendActivity = Napi::Object::New(info.Env());
        appendActivity.Set("batches", Napi::Number::New(info.Env(), static_cast<double>(writer.v6AppendBatches)));
        appendActivity.Set("rows_processed", Napi::Number::New(info.Env(), static_cast<double>(writer.v6AppendRowsProcessed)));
        appendActivity.Set("payload_bytes_processed", Napi::Number::New(info.Env(), static_cast<double>(writer.v6AppendPayloadBytesProcessed)));
        appendActivity.Set("last_rows", Napi::Number::New(info.Env(), static_cast<double>(writer.lastV6AppendRows)));
        appendActivity.Set("last_payload_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.lastV6AppendPayloadBytes)));
        appendActivity.Set("last_source_row_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.lastV6SourceRowCapacityBytes)));
        appendActivity.Set("peak_rows", Napi::Number::New(info.Env(), static_cast<double>(writer.peakV6AppendRows)));
        appendActivity.Set("peak_payload_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.peakV6AppendPayloadBytes)));
        appendActivity.Set("peak_source_row_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.peakV6SourceRowCapacityBytes)));
        writerStats.Set("v6_append_activity", appendActivity);

        Napi::Object dedupe = Napi::Object::New(info.Env());
        dedupe.Set("entries", Napi::Number::New(info.Env(), static_cast<double>(writer.dedupeEntries)));
        dedupe.Set("payload_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.dedupePayloadBytes)));
        dedupe.Set("payload_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.dedupePayloadCapacityBytes)));
        writerStats.Set("dedupe_cache", dedupe);

        Napi::Object v6 = Napi::Object::New(info.Env());
        v6.Set("retained_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.v6RetainedBytes)));
        v6.Set("builders", Napi::Number::New(info.Env(), static_cast<double>(writer.v6BuilderCount)));
        v6.Set("builder_plain_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.v6BuilderPlainBytes)));
        v6.Set("builder_plain_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.v6BuilderPlainCapacityBytes)));
        v6.Set("builder_row_index_entries", Napi::Number::New(info.Env(), static_cast<double>(writer.v6BuilderRowIndexEntries)));
        v6.Set("builder_row_index_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.v6BuilderRowIndexCapacityBytes)));
        v6.Set("chunks", Napi::Number::New(info.Env(), static_cast<double>(writer.v6ChunkCount)));
        v6.Set("chunk_container_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.v6ChunkContainerCapacityBytes)));
        v6.Set("chunk_row_index_entries", Napi::Number::New(info.Env(), static_cast<double>(writer.v6ChunkRowIndexEntries)));
        v6.Set("chunk_row_index_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.v6ChunkRowIndexCapacityBytes)));
        v6.Set("branches", Napi::Number::New(info.Env(), static_cast<double>(writer.v6BranchCount)));
        v6.Set("branch_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.v6BranchCapacityBytes)));
        v6.Set("laps", Napi::Number::New(info.Env(), static_cast<double>(writer.v6LapCount)));
        v6.Set("status_laps", Napi::Number::New(info.Env(), static_cast<double>(writer.v6StatusLapCount)));
        v6.Set("events", Napi::Number::New(info.Env(), static_cast<double>(writer.v6EventCount)));
        v6.Set("event_payload_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.v6EventPayloadBytes)));
        v6.Set("event_payload_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.v6EventPayloadCapacityBytes)));
        v6.Set("event_container_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.v6EventContainerCapacityBytes)));
        v6.Set("lap_status_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.v6LapStatusCapacityBytes)));
        Napi::Object compressionActivity = Napi::Object::New(info.Env());
        compressionActivity.Set("chunk_writes", Napi::Number::New(info.Env(), static_cast<double>(writer.v6ChunkWrites)));
        compressionActivity.Set("plain_bytes_processed", Napi::Number::New(info.Env(), static_cast<double>(writer.v6ChunkPlainBytesProcessed)));
        compressionActivity.Set("compressed_bytes_written", Napi::Number::New(info.Env(), static_cast<double>(writer.v6ChunkCompressedBytesWritten)));
        compressionActivity.Set("buffer_bytes_allocated", Napi::Number::New(info.Env(), static_cast<double>(writer.v6CompressionBufferBytesAllocated)));
        compressionActivity.Set("retained_scratch_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.v6CompressionScratchCapacityBytes)));
        compressionActivity.Set("retained_context_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.v6CompressionContextBytes)));
        compressionActivity.Set("last_plain_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.v6LastChunkPlainBytes)));
        compressionActivity.Set("last_compressed_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.v6LastChunkCompressedBytes)));
        compressionActivity.Set("last_buffer_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.v6LastCompressionBufferCapacityBytes)));
        compressionActivity.Set("peak_buffer_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.v6PeakCompressionBufferCapacityBytes)));
        v6.Set("compression_activity", compressionActivity);
        Napi::Object checkpointActivity = Napi::Object::New(info.Env());
        checkpointActivity.Set("writes", Napi::Number::New(info.Env(), static_cast<double>(writer.v6CheckpointWrites)));
        checkpointActivity.Set("scratch_bytes_allocated", Napi::Number::New(info.Env(), static_cast<double>(writer.v6CheckpointScratchBytesAllocated)));
        checkpointActivity.Set("last_scratch_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.v6LastCheckpointScratchBytes)));
        checkpointActivity.Set("peak_scratch_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.v6PeakCheckpointScratchBytes)));
        checkpointActivity.Set("last_directory_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.v6LastCheckpointDirectoryBytes)));
        checkpointActivity.Set("peak_directory_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.v6PeakCheckpointDirectoryBytes)));
        checkpointActivity.Set("last_row_index_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.v6LastCheckpointRowIndexBytes)));
        checkpointActivity.Set("peak_row_index_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.v6PeakCheckpointRowIndexBytes)));
        v6.Set("checkpoint_activity", checkpointActivity);
        writerStats.Set("v6", v6);
        result.Set("writer", writerStats);
        return result;
    }

    Napi::Value SetTeamColorOverrides(const Napi::CallbackInfo& info) {
        if (engine && info.Length() >= 1) {
            auto target = engine;
            queueOrderedEngineCall(info.Env(), orderedCalls_,
                [target, overrides = readTeamColorOverrides(info[0])] {
                    target->setTeamColorOverrides(overrides);
                });
        }
        return info.Env().Undefined();
    }

    Napi::Value TeamColorCatalog(const Napi::CallbackInfo& info) {
        return Napi::String::New(info.Env(), engine
            ? engine->teamColorCatalogJson() : "{}");
    }

    Napi::Value SetStrategyMinimumStops(const Napi::CallbackInfo& info) {
        if (engine && info.Length() >= 1 && info[0].IsNumber()) {
            auto target = engine;
            const int stops = info[0].As<Napi::Number>().Int32Value();
            queueOrderedEngineCall(info.Env(), orderedCalls_,
                [target, stops] { target->setStrategyMinimumStops(stops); });
        }
        return info.Env().Undefined();
    }

    Napi::Value SetLogging(const Napi::CallbackInfo& info) {
        return SetLoggingZstd(info);
    }

    Napi::Value FlushRecording(const Napi::CallbackInfo& info) {
        if (engine) engine->flushRecording();
        return info.Env().Undefined();
    }

    Napi::Value SetLoggingZstd(const Napi::CallbackInfo& info) {
        if (info.Length() >= 2 && info[0].IsBoolean() && info[1].IsString()) {
            bool enabled = info[0].As<Napi::Boolean>().Value();
            std::string dir = info[1].As<Napi::String>().Utf8Value();
            engine->setLoggingZstd(enabled, dir);
        }
        return info.Env().Undefined();
    }

    // { practice, qualifying, race, timeTrial }: each "all_drivers",
    // "driver_only", "both" or "ask". A missing or unknown value keeps every
    // driver.
    Napi::Value SetRecordingScopes(const Napi::CallbackInfo& info) {
        if (engine && info.Length() >= 1 && info[0].IsObject()) {
            const Napi::Object object = info[0].As<Napi::Object>();
            const auto read = [&object](const char* key) {
                const Napi::Value value = object.Get(key);
                return tnrp::recordingScopeFromString(
                    value.IsString() ? value.As<Napi::String>().Utf8Value() : std::string());
            };
            tnrp::RecordingScopes scopes;
            scopes.practice   = read("practice");
            scopes.qualifying = read("qualifying");
            scopes.race       = read("race");
            scopes.timeTrial  = read("timeTrial");
            engine->setRecordingScopes(scopes);
        }
        return info.Env().Undefined();
    }

    // Deprecated compatibility surface. Normal app recording never calls it.
    Napi::Value SetLoggingGzip(const Napi::CallbackInfo& info) {
        if (info.Length() >= 2 && info[0].IsBoolean() && info[1].IsString()) {
            bool enabled = info[0].As<Napi::Boolean>().Value();
            std::string dir = info[1].As<Napi::String>().Utf8Value();
#if defined(__clang__) || defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#elif defined(_MSC_VER)
#  pragma warning(push)
#  pragma warning(disable: 4996)
#endif
            engine->setLoggingGzip(enabled, dir);
#if defined(__clang__) || defined(__GNUC__)
#  pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#  pragma warning(pop)
#endif
        }
        return info.Env().Undefined();
    }

    // Async: resolves Promise<{ok, error?}>. The load (decompress + index scan) runs
    // on the libuv threadpool so the Electron main thread stays responsive.
    Napi::Value PlayerLoad(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        auto resolveFailure = [&env](const char* error) {
            auto d = Napi::Promise::Deferred::New(env);
            Napi::Object result = Napi::Object::New(env);
            result.Set("ok", false);
            result.Set("error", error);
            d.Resolve(result);
            return d.Promise();
        };
        if (info.Length() < 1 || !info[0].IsString() || !engine)
            return resolveFailure("The playback engine is not available.");
        if (loadBusy_->exchange(true))
            return resolveFailure("Another recording is already being opened.");
        auto* worker = new PlayerLoadWorker(env, engine,
                                            info[0].As<Napi::String>().Utf8Value(), loadBusy_);
        Napi::Promise p = worker->GetPromise();
        worker->Queue();
        return p;
    }

    Napi::Value PlayerPlay(const Napi::CallbackInfo& info) {
        if (engine) {
            auto target = engine;
            queueOrderedEngineCall(info.Env(), orderedCalls_, [target] { target->playerPlay(); });
        }
        return info.Env().Undefined();
    }

    Napi::Value PlayerPause(const Napi::CallbackInfo& info) {
        if (engine) {
            auto target = engine;
            queueOrderedEngineCall(info.Env(), orderedCalls_, [target] { target->playerPause(); });
        }
        return info.Env().Undefined();
    }

    Napi::Value SetDataRequirements(const Napi::CallbackInfo& info) {
        if (engine && info.Length() >= 2 && info[0].IsNumber() &&
            info[1].IsNumber()) {
            const uint32_t streamMask = info[0].As<Napi::Number>().Uint32Value();
            const uint32_t historyMask = info[1].As<Napi::Number>().Uint32Value();
            const float windowSeconds = info.Length() >= 3 && info[2].IsNumber()
                ? std::max(-1.0f, info[2].As<Napi::Number>().FloatValue()) : 0.0f;
            const uint64_t requestId = info.Length() >= 4 && info[3].IsNumber()
                ? static_cast<uint64_t>(info[3].As<Napi::Number>().Int64Value()) : 0;
            std::vector<uint8_t> v6Types;
            if (info.Length() >= 5 && info[4].IsArray()) {
                const auto values = info[4].As<Napi::Array>();
                for (uint32_t i = 0; i < values.Length(); ++i)
                    if (values.Get(i).IsNumber()) v6Types.push_back(
                        static_cast<uint8_t>(values.Get(i).As<Napi::Number>().Uint32Value()));
            }
            std::vector<uint8_t> v6HistoryTypes;
            if (info.Length() >= 6 && info[5].IsArray()) {
                const auto values = info[5].As<Napi::Array>();
                for (uint32_t i = 0; i < values.Length(); ++i)
                    if (values.Get(i).IsNumber()) v6HistoryTypes.push_back(
                        static_cast<uint8_t>(values.Get(i).As<Napi::Number>().Uint32Value()));
            }
            engine->requestDataRequirements(requestId);
            (new DataRequirementsWorker(info.Env(), engine, streamMask,
                historyMask, windowSeconds, requestId, std::move(v6Types),
                std::move(v6HistoryTypes)))->Queue();
        }
        return info.Env().Undefined();
    }

    Napi::Value PlayerSeek(const Napi::CallbackInfo& info) {
        if (info.Length() >= 1 && info[0].IsNumber()) {
            const bool allHistory = info.Length() >= 2 && info[1].IsBoolean() &&
                info[1].As<Napi::Boolean>().Value();
            const uint64_t requestId = info.Length() >= 3 && info[2].IsNumber()
                ? static_cast<uint64_t>(info[2].As<Napi::Number>().Int64Value()) : 0;
            const uint32_t rowTypeMask = info.Length() >= 4 && info[3].IsNumber()
                ? info[3].As<Napi::Number>().Uint32Value() : 0xFFFFFFFFu;
            const float windowSeconds = info.Length() >= 5 && info[4].IsNumber()
                ? std::max(0.0f, info[4].As<Napi::Number>().FloatValue()) : 0.0f;
            // A seek on the lap-synced timeline (two files open): its merged time.
            const double mergedTime = info.Length() >= 6 && info[5].IsNumber()
                ? info[5].As<Napi::Number>().DoubleValue() : NAN;
            engine->playerRequestSeek(requestId);
            // Every indexed seek can decompress, rebuild sparse state and prime
            // the streaming frontier. Never charge that work to Electron's
            // main thread, including current-lap/zero-window seeks.
            (new PlayerSeekWorker(info.Env(), engine,
                info[0].As<Napi::Number>().FloatValue(), allHistory, requestId,
                rowTypeMask, windowSeconds, mergedTime))->Queue();
        }
        return info.Env().Undefined();
    }

    // (inputJson | null) — lap-synced playback with the second file
    // (Engine::playerSetMergedTimeline); the input is mergedLapTimeline's.
    Napi::Value PlayerSetMergedTimeline(const Napi::CallbackInfo& info) {
        if (engine) {
            auto target = engine;
            std::string input = info.Length() >= 1 && info[0].IsString()
                ? info[0].As<Napi::String>().Utf8Value() : std::string();
            queueOrderedEngineCall(info.Env(), orderedCalls_, [target, input] {
                target->playerSetMergedTimeline(input.empty()
                    ? std::nullopt : tnrp::mergedLapTimelineFromInput(input));
            });
        }
        return info.Env().Undefined();
    }

    Napi::Value PlayerSetSpeed(const Napi::CallbackInfo& info) {
        if (engine && info.Length() >= 1 && info[0].IsNumber()) {
            auto target = engine;
            const float mult = info[0].As<Napi::Number>().FloatValue();
            queueOrderedEngineCall(info.Env(), orderedCalls_, [target, mult] { target->playerSetSpeed(mult); });
        }
        return info.Env().Undefined();
    }

    Napi::Value PlayerSetDriver(const Napi::CallbackInfo& info) {
        if (!engine || info.Length() < 1 || !info[0].IsNumber()) {
            auto deferred = Napi::Promise::Deferred::New(info.Env());
            deferred.Resolve(info.Env().Undefined());
            return deferred.Promise();
        }
        const bool useRecordedRows = info.Length() >= 2 && info[1].IsBoolean() &&
            info[1].As<Napi::Boolean>().Value();
        auto* worker = new PlayerSetDriverWorker(info.Env(), engine,
            info[0].As<Napi::Number>().Int32Value(), useRecordedRows);
        auto promise = worker->GetPromise();
        worker->Queue();
        return promise;
    }

    // Wraps a JS callback(headerJson, buffer) answering one live lap request.
    // The thread-safe function is released when the engine drops the request,
    // whether or not it was answered (there may be no such lap).
    static tnrp::Engine::LiveLapCallback liveLapCallback(Napi::Env env, Napi::Function fn) {
        auto tsfn = std::shared_ptr<Napi::ThreadSafeFunction>(
            new Napi::ThreadSafeFunction(Napi::ThreadSafeFunction::New(env, fn, "liveLapData", 0, 1)),
            [](Napi::ThreadSafeFunction* f) { f->Release(); delete f; });
        tsfn->Unref(env);
        return [tsfn](std::string header, std::shared_ptr<std::vector<uint8_t>> columnar) {
            struct LapData { std::string header; std::shared_ptr<std::vector<uint8_t>> columnar; };
            auto* d = new LapData{ std::move(header), std::move(columnar) };
            const auto status = tsfn->NonBlockingCall(d, [](Napi::Env env, Napi::Function cb, LapData* d) {
                if (env != nullptr && cb != nullptr) {
                    try {
                        auto buffer = d->columnar && !d->columnar->empty()
                            ? Napi::Buffer<uint8_t>::Copy(env, d->columnar->data(), d->columnar->size())
                            : Napi::Buffer<uint8_t>::New(env, 0);
                        cb.Call({ Napi::String::New(env, d->header), buffer });
                    } catch (const std::exception& e) {
                        std::fprintf(stderr, "[addon] live lap callback failed: %s\n", e.what());
                        std::fflush(stderr);
                    }
                }
                delete d;
            });
            if (status != napi_ok) delete d;
        };
    }

    // liveGetFastestLap(requestId, callback): the player's fastest completed
    // lap from the live store, answered as callback(headerJson, v6h1Buffer).
    Napi::Value LiveGetFastestLap(const Napi::CallbackInfo& info) {
        if (engine && info.Length() >= 2 && info[0].IsNumber() && info[1].IsFunction())
            engine->liveGetFastestLap(static_cast<uint64_t>(info[0].As<Napi::Number>().Int64Value()),
                                      liveLapCallback(info.Env(), info[1].As<Napi::Function>()));
        return info.Env().Undefined();
    }

    // liveGetLapData(requestId, lapNum, callback): one of the player's laps
    // from the live store, answered as callback(headerJson, v6h1Buffer).
    Napi::Value LiveGetLapData(const Napi::CallbackInfo& info) {
        if (engine && info.Length() >= 3 && info[0].IsNumber() && info[1].IsNumber() && info[2].IsFunction())
            engine->liveGetLapData(static_cast<uint64_t>(info[0].As<Napi::Number>().Int64Value()),
                                   info[1].As<Napi::Number>().Int32Value(),
                                   liveLapCallback(info.Env(), info[2].As<Napi::Function>()));
        return info.Env().Undefined();
    }

    // The car whose lap-times view is open (-1 closes it). The engine pushes
    // driver_lap_history rows for it as they change.
    Napi::Value SetLapHistoryCar(const Napi::CallbackInfo& info) {
        if (engine && info.Length() >= 1 && info[0].IsNumber()) {
            const int carIdx = info[0].As<Napi::Number>().Int32Value();
            auto target = engine;
            (new EngineCallWorker(info.Env(), [target, carIdx] {
                target->setLapHistoryCar(carIdx);
            }))->Queue();
        }
        return info.Env().Undefined();
    }

    Napi::Value PlayerSetFocusDriver(const Napi::CallbackInfo& info) {
        if (engine && info.Length() >= 1 && info[0].IsNumber()) {
            const int driverIndex = info[0].As<Napi::Number>().Int32Value();
            auto target = engine;
            (new EngineCallWorker(info.Env(), [target, driverIndex] {
                target->playerSetFocusDriver(driverIndex);
            }))->Queue();
        }
        return info.Env().Undefined();
    }

    Napi::Value PlayerGetLapData(const Napi::CallbackInfo& info) {
        if (info.Length() >= 1 && info[0].IsNumber() && engine) {
            const uint32_t rowTypeMask = info.Length() >= 2 && info[1].IsNumber()
                ? info[1].As<Napi::Number>().Uint32Value() : 0xFFFFFFFFu;
            auto target = engine;
            const int lapNum = info[0].As<Napi::Number>().Int32Value();
            queueOrderedEngineCall(info.Env(), orderedCalls_,
                [target, lapNum, rowTypeMask] { target->playerGetLapData(lapNum, rowTypeMask); });
        }
        return info.Env().Undefined();
    }

    Napi::Value PlayerGetAllLapsData(const Napi::CallbackInfo& info) {
        if (engine) {
            const uint64_t requestId = info.Length() >= 1 && info[0].IsNumber()
                ? static_cast<uint64_t>(info[0].As<Napi::Number>().Int64Value()) : 0;
            const uint32_t rowTypeMask = info.Length() >= 2 && info[1].IsNumber()
                ? info[1].As<Napi::Number>().Uint32Value() : 0xFFFFFFFFu;
            (new PlayerHistoryWorker(info.Env(), engine, requestId, rowTypeMask))->Queue();
        }
        return info.Env().Undefined();
    }

    // Showing may read the recording, so both edges run off the JS thread;
    // the sequence lets the engine ignore an edge that arrives late.
    Napi::Value SetHostVisible(const Napi::CallbackInfo& info) {
        if (engine && info.Length() >= 2 && info[0].IsBoolean() && info[1].IsNumber()) {
            const bool visible = info[0].As<Napi::Boolean>().Value();
            const uint64_t sequence =
                static_cast<uint64_t>(info[1].As<Napi::Number>().Int64Value());
            auto target = engine;
            (new EngineCallWorker(info.Env(), [target, visible, sequence] {
                target->setHostVisible(visible, sequence);
            }))->Queue();
        }
        return info.Env().Undefined();
    }

    Napi::Value PlayerGetWindowData(const Napi::CallbackInfo& info) {
        if (engine && info.Length() >= 1 && info[0].IsNumber()) {
            const float windowSeconds = std::max(0.0f, info[0].As<Napi::Number>().FloatValue());
            const uint64_t requestId = info.Length() >= 2 && info[1].IsNumber()
                ? static_cast<uint64_t>(info[1].As<Napi::Number>().Int64Value()) : 0;
            const uint32_t rowTypeMask = info.Length() >= 3 && info[2].IsNumber()
                ? info[2].As<Napi::Number>().Uint32Value() : 0xFFFFFFFFu;
            (new PlayerHistoryWorker(info.Env(), engine, requestId, rowTypeMask,
                                     windowSeconds, true))->Queue();
        }
        return info.Env().Undefined();
    }

    Napi::Value AnalysisLoadFile(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        auto resolveFailure = [&env](const char* error) {
            auto deferred = Napi::Promise::Deferred::New(env);
            Napi::Object result = Napi::Object::New(env);
            result.Set("ok", false);
            result.Set("error", error);
            deferred.Resolve(result);
            return deferred.Promise();
        };
        if (info.Length() < 1 || !info[0].IsString())
            return resolveFailure("A recording path is required.");
        if (analysisReader_->busy.exchange(true))
            return resolveFailure("Another comparison recording is already being opened.");

        auto* worker = new AnalysisLoadWorker(
            env, analysisReader_, info[0].As<Napi::String>().Utf8Value());
        Napi::Promise promise = worker->GetPromise();
        worker->Queue();
        return promise;
    }

    // Promise<string>: the lap is read on a worker thread (AnalysisLapDataWorker).
    Napi::Value AnalysisGetLapData(const Napi::CallbackInfo& info) {
        if (info.Length() < 4 || !info[0].IsNumber() || !info[1].IsNumber() ||
            !info[2].IsBoolean() || !info[3].IsNumber() || analysisReader_->busy.load()) {
            auto deferred = Napi::Promise::Deferred::New(info.Env());
            deferred.Resolve(Napi::String::New(info.Env(), ""));
            return deferred.Promise();
        }
        auto* worker = new AnalysisLapDataWorker(
            info.Env(), engine, analysisReader_,
            info[0].As<Napi::Number>().Int32Value(), info[1].As<Napi::Number>().Uint32Value(),
            info[2].As<Napi::Boolean>().Value(), info[3].As<Napi::Number>().Int32Value());
        Napi::Promise promise = worker->GetPromise();
        worker->Queue();
        return promise;
    }

    // (lapNum, rowTypeMask, driverIndex, v6Types?: number[]) → Promise<object | null>:
    // one of the second file's laps as column blocks (AnalysisLapHistoryWorker),
    // holding only the V6 fields in v6Types (absent or empty: every field).
    Napi::Value AnalysisLapHistory(const Napi::CallbackInfo& info) {
        if (info.Length() < 3 || !info[0].IsNumber() || !info[1].IsNumber() || !info[2].IsNumber() ||
            analysisReader_->busy.load()) {
            auto deferred = Napi::Promise::Deferred::New(info.Env());
            deferred.Resolve(info.Env().Null());
            return deferred.Promise();
        }
        std::vector<uint8_t> v6Types;
        if (info.Length() >= 4 && info[3].IsArray()) {
            const auto values = info[3].As<Napi::Array>();
            for (uint32_t i = 0; i < values.Length(); ++i) {
                const Napi::Value value = values.Get(i);
                if (!value.IsNumber()) continue;
                const int type = value.As<Napi::Number>().Int32Value();
                if (type > 0 && type <= 255) v6Types.push_back(static_cast<uint8_t>(type));
            }
        }
        auto* worker = new AnalysisLapHistoryWorker(
            info.Env(), analysisReader_, info[0].As<Napi::Number>().Int32Value(),
            info[1].As<Napi::Number>().Uint32Value(), info[2].As<Napi::Number>().Int32Value(),
            std::move(v6Types));
        Napi::Promise promise = worker->GetPromise();
        worker->Queue();
        return promise;
    }

    // (callback(batch: string)) — file B's streamed rows (SecondaryStream).
    Napi::Value AnalysisStream(const Napi::CallbackInfo& info) {
        if (info.Length() >= 1 && info[0].IsFunction())
            analysisReader_->stream->setCallback(info.Env(), info[0].As<Napi::Function>());
        return info.Env().Undefined();
    }

    // (enabled, driverIndex, longMask, longSeconds) — whether a page shows
    // file B's streamed rows, the car followed in B (-1: its recorded player),
    // and the longer history B's windows need (SecondaryStream::configure).
    // The engine places B on the playhead; this only starts, stops and points
    // the stream.
    Napi::Value AnalysisSetStreaming(const Napi::CallbackInfo& info) {
        const bool enabled = info.Length() >= 1 && info[0].IsBoolean() && info[0].As<Napi::Boolean>().Value();
        const int driver = info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : -1;
        const uint32_t longMask = info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Uint32Value() : 0;
        const float longSeconds = info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().FloatValue() : 0.0f;
        analysisReader_->stream->configure(enabled, driver, longMask, longSeconds);
        return info.Env().Undefined();
    }

    // (driverIndex) — the car selected in file B's Standings tower (-1: none).
    Napi::Value AnalysisSetFocusDriver(const Napi::CallbackInfo& info) {
        const int driver = info.Length() >= 1 && info[0].IsNumber() ? info[0].As<Napi::Number>().Int32Value() : -1;
        analysisReader_->stream->setFocusDriver(driver);
        return info.Env().Undefined();
    }

    // (carIdx) — the car whose lap-times view is open on file B (-1: none).
    Napi::Value AnalysisSetLapHistoryCar(const Napi::CallbackInfo& info) {
        const int car = info.Length() >= 1 && info[0].IsNumber() ? info[0].As<Napi::Number>().Int32Value() : -1;
        analysisReader_->stream->setLapHistoryCar(car);
        return info.Env().Undefined();
    }

    // (inputJson) → string: two files' laps on one timeline (tnrp/MergedLapTimeline.h).
    // Pure arithmetic over the laps given, so it runs inline.
    Napi::Value MergedLapTimeline(const Napi::CallbackInfo& info) {
        if (info.Length() < 1 || !info[0].IsString()) return Napi::String::New(info.Env(), "");
        return Napi::String::New(info.Env(), tnrp::mergedLapTimelineJson(info[0].As<Napi::String>().Utf8Value()));
    }

    Napi::Value AnalysisCompareLaps(const Napi::CallbackInfo& info) {
        auto resolveEmpty = [&info]() {
            auto deferred = Napi::Promise::Deferred::New(info.Env());
            deferred.Resolve(Napi::String::New(info.Env(), ""));
            return deferred.Promise();
        };
        if (info.Length() < 7 || !info[0].IsNumber() || !info[1].IsBoolean() ||
            !info[2].IsNumber() || !info[3].IsNumber() || !info[4].IsBoolean() ||
            !info[5].IsNumber() || !info[6].IsBoolean() ||
            !engine || analysisReader_->busy.load()) return resolveEmpty();

        auto* worker = new AnalysisDeltaWorker(
            info.Env(), engine, analysisReader_,
            info[0].As<Napi::Number>().Int32Value(), info[1].As<Napi::Boolean>().Value(),
            info[2].As<Napi::Number>().Int32Value(), info[3].As<Napi::Number>().Int32Value(),
            info[4].As<Napi::Boolean>().Value(), info[5].As<Napi::Number>().Int32Value(),
            info[6].As<Napi::Boolean>().Value());
        Napi::Promise promise = worker->GetPromise();
        worker->Queue();
        return promise;
    }

    // (mode, currentLap, currentSecondary, currentDriver,
    //  comparisonLap (-1 for none), comparisonSecondary, comparisonDriver)
    Napi::Value AnalysisMap(const Napi::CallbackInfo& info) {
        auto resolveEmpty = [&info]() {
            auto deferred = Napi::Promise::Deferred::New(info.Env());
            deferred.Resolve(Napi::String::New(info.Env(), ""));
            return deferred.Promise();
        };
        if (info.Length() < 7 || !info[0].IsString() || !info[1].IsNumber() ||
            !info[2].IsBoolean() || !info[3].IsNumber() || !info[4].IsNumber() ||
            !info[5].IsBoolean() || !info[6].IsNumber() || !engine) return resolveEmpty();

        auto* worker = new AnalysisMapWorker(
            info.Env(), engine, analysisReader_, info[0].As<Napi::String>().Utf8Value(),
            info[1].As<Napi::Number>().Int32Value(), info[2].As<Napi::Boolean>().Value(),
            info[3].As<Napi::Number>().Int32Value(), info[4].As<Napi::Number>().Int32Value(),
            info[5].As<Napi::Boolean>().Value(), info[6].As<Napi::Number>().Int32Value());
        Napi::Promise promise = worker->GetPromise();
        worker->Queue();
        return promise;
    }

    Napi::Value AnalysisCloseFile(const Napi::CallbackInfo& info) {
        if (!analysisReader_->busy.load()) {
            std::lock_guard<std::mutex> lock(analysisReader_->mutex);
            {
                std::lock_guard<std::mutex> historyLock(analysisReader_->historyMutex);
                analysisReader_->history.reset();
            }
            analysisReader_->stream->close();
            analysisReader_->reader.close();
        }
        return info.Env().Undefined();
    }

    Napi::Value Destroy(const Napi::CallbackInfo& info) {
        if (destroyed_) return info.Env().Undefined();
        destroyed_ = true;
        if (engine) {
            engine->pairStop(false);
            // A pending worker's reference can keep the engine alive past this
            // call; its listener must not go on receiving into this sink.
            engine->stopUdp();
        }
        engine.reset();   // a pending PlayerLoadWorker holds its own ref
        tsfn.Release();
        if (hasBinCb_) tsfnBin.Release();
        if (hasSeekCb_) tsfnSeek.Release();
        if (hasPairCb_) tsfnPair.Release();
        if (hasPairDiagnosticCb_) tsfnPairDiagnostic.Release();
        return info.Env().Undefined();
    }

    Napi::Value PlayerClose(const Napi::CallbackInfo& info) {
        std::fprintf(stderr, "[close-trace] addon PlayerClose entry\n");
        std::fflush(stderr);
        // JSON control rows and binary hot rows use separate thread-safe
        // callbacks. Invalidate the playback binary callback before stopping
        // the engine so it cannot run after playback_close and repopulate the
        // renderer with the final speed/RPM/temperature batch. While the close
        // is in progress UDP is either rejected by the engine or discarded
        // here; new live rows use the next generation after this call returns.
        auto binaryFlush = binFlush_;
        {
            std::lock_guard<std::mutex> lock(binaryFlush->mutex);
            binaryFlush->discard = true;
            ++binaryFlush->generation;
            binaryFlush->pending.clear();
            binaryFlush->draining.clear();
            binaryFlush->scheduled = false;
            binaryFlush->scheduledGeneration = binaryFlush->generation;
        }
        if (engine) engine->playerClose();
        {
            std::lock_guard<std::mutex> lock(binaryFlush->mutex);
            binaryFlush->discard = false;
        }
        std::fprintf(stderr, "[close-trace] addon PlayerClose returned\n");
        std::fflush(stderr);
        return info.Env().Undefined();
    }

    // ── Track maps (tnrp/TrackMaps.h) ────────────────────────────────────────

    static std::string stringArg(const Napi::CallbackInfo& info, size_t index) {
        return info.Length() > index && info[index].IsString()
            ? info[index].As<Napi::String>().Utf8Value() : std::string{};
    }

    // Promise-returning track map call; resolves "" when there is no engine.
    Napi::Value queueTrackMapCall(const Napi::CallbackInfo& info, std::function<std::string()> call) {
        if (!engine) call = [] { return std::string{}; };
        auto* worker = new TrackMapWorker(info.Env(), std::move(call));
        Napi::Promise promise = worker->GetPromise();
        worker->Queue();
        return promise;
    }

    // (jsonStrings[]) -> number registered.
    Napi::Value TrackMapsAddBuiltins(const Napi::CallbackInfo& info) {
        uint32_t added = 0;
        if (engine && info.Length() >= 1 && info[0].IsArray()) {
            Napi::Array maps = info[0].As<Napi::Array>();
            for (uint32_t i = 0; i < maps.Length(); ++i) {
                Napi::Value value = maps.Get(i);
                if (value.IsString() && engine->trackMaps().addBuiltin(value.As<Napi::String>().Utf8Value()))
                    ++added;
            }
            if (added) engine->refreshTrackMap();
        }
        return Napi::Number::New(info.Env(), added);
    }

    Napi::Value TrackMapsSetDirectory(const Napi::CallbackInfo& info) {
        if (engine && info.Length() >= 1 && info[0].IsString()) {
            engine->trackMaps().setUserDirectory(info[0].As<Napi::String>().Utf8Value());
            engine->refreshTrackMap();
        }
        return info.Env().Undefined();
    }

    // (moddedMaps, generateMaps)
    Napi::Value TrackMapsSetOptions(const Napi::CallbackInfo& info) {
        if (engine && info.Length() >= 2 && info[0].IsBoolean() && info[1].IsBoolean())
            engine->setTrackMapOptions(info[0].As<Napi::Boolean>().Value(), info[1].As<Napi::Boolean>().Value());
        return info.Env().Undefined();
    }

    Napi::Value TrackMapsCatalog(const Napi::CallbackInfo& info) {
        return Napi::String::New(info.Env(), engine ? engine->trackMaps().catalogJson() : "{}");
    }

    // (id) -> the map file's JSON, or "".
    Napi::Value TrackMapsGet(const Napi::CallbackInfo& info) {
        return Napi::String::New(info.Env(), engine ? engine->trackMaps().mapJson(stringArg(info, 0)) : "");
    }

    // The last "track_map" row, or "".
    Napi::Value TrackMapsState(const Napi::CallbackInfo& info) {
        return Napi::String::New(info.Env(), engine ? engine->trackMapStateJson() : "");
    }

    // (trackKey, mapId); an empty mapId clears the pin.
    Napi::Value TrackMapsPin(const Napi::CallbackInfo& info) {
        if (engine && info.Length() >= 2 && info[0].IsString() && info[1].IsString()) {
            engine->trackMaps().pin(info[0].As<Napi::String>().Utf8Value(),
                                    info[1].As<Napi::String>().Utf8Value());
            engine->refreshTrackMap();
        }
        return info.Env().Undefined();
    }

    // The mutating calls below resolve with {"ok","id","error"} JSON.
    Napi::Value TrackMapsRename(const Napi::CallbackInfo& info) {
        auto target = engine;
        return queueTrackMapCall(info, [target, id = stringArg(info, 0), name = stringArg(info, 1)] {
            TrackMapResult result{.id = id};
            result.ok = target->trackMaps().rename(id, name, &result.error);
            if (result.ok) target->refreshTrackMap();
            return tnrp::writeJson(result);
        });
    }

    // (id, editJson): tnrp::TrackMapEdit fields. Resolves with the edited map's
    // ID, which is a new one when a built-in map was edited.
    Napi::Value TrackMapsEdit(const Napi::CallbackInfo& info) {
        auto target = engine;
        return queueTrackMapCall(info, [target, id = stringArg(info, 0), json = stringArg(info, 1)] {
            TrackMapResult result;
            tnrp::TrackMapEdit edit;
            if (glz::read<glz::opts{.error_on_unknown_keys = false}>(edit, json)) {
                result.error = "Invalid map details";
                return tnrp::writeJson(result);
            }
            result.id = target->trackMaps().edit(id, edit, &result.error);
            result.ok = !result.id.empty();
            if (result.ok) target->refreshTrackMap();
            return tnrp::writeJson(result);
        });
    }

    Napi::Value TrackMapsDelete(const Napi::CallbackInfo& info) {
        auto target = engine;
        return queueTrackMapCall(info, [target, id = stringArg(info, 0)] {
            TrackMapResult result{.id = id};
            result.ok = target->trackMaps().remove(id, &result.error);
            if (result.ok) target->refreshTrackMap();
            return tnrp::writeJson(result);
        });
    }

    // (sourcePath)
    Napi::Value TrackMapsImport(const Napi::CallbackInfo& info) {
        auto target = engine;
        return queueTrackMapCall(info, [target, path = stringArg(info, 0)] {
            TrackMapResult result;
            result.id = target->trackMaps().importFile(path, &result.error);
            result.ok = !result.id.empty();
            if (result.ok) target->refreshTrackMap();
            return tnrp::writeJson(result);
        });
    }

    // (id, destinationPath)
    Napi::Value TrackMapsExport(const Napi::CallbackInfo& info) {
        auto target = engine;
        return queueTrackMapCall(info, [target, id = stringArg(info, 0), path = stringArg(info, 1)] {
            TrackMapResult result{.id = id};
            result.ok = target->trackMaps().exportFile(id, path, &result.error);
            return tnrp::writeJson(result);
        });
    }

    // (recordingPath): builds and saves a map from a V6 recording.
    Napi::Value TrackMapsGenerate(const Napi::CallbackInfo& info) {
        auto target = engine;
        return queueTrackMapCall(info, [target, path = stringArg(info, 0)] {
            TrackMapResult result;
            result.id = target->generateTrackMap(path, &result.error);
            result.ok = !result.id.empty();
            return tnrp::writeJson(result);
        });
    }

    // (recordingPath) -> a "track_map" row with scope "recording", or "".
    Napi::Value TrackMapsResolveRecording(const Napi::CallbackInfo& info) {
        auto target = engine;
        return queueTrackMapCall(info, [target, path = stringArg(info, 0)] {
            return target->resolveTrackMapForRecording(path);
        });
    }

    // Exports an arbitrary .tnrd file to a new "<recording>_parquet" folder
    // inside destDir, resolving {ok, folder?, error?}. Deliberately independent
    // of `engine`'s playback state: it opens its own throwaway TnrdReader
    // (see tnrp::exportTnrdFileToParquet). Registered as an instance method
    // purely because that's where N-API methods live on this class.
    Napi::Value PlayerExportParquet(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        if (info.Length() < 3 || !info[0].IsString() || !info[1].IsString() || !info[2].IsFunction()) {
            Napi::TypeError::New(env, "Expected (srcPath, destDir, onProgress)").ThrowAsJavaScriptException();
            return env.Undefined();
        }
        auto* worker = new ExportParquetWorker(env, info[0].As<Napi::String>().Utf8Value(),
                                                  info[1].As<Napi::String>().Utf8Value(),
                                                  info[2].As<Napi::Function>());
        Napi::Promise p = worker->GetPromise();
        worker->Queue();
        return p;
    }
};

// Module-level: the i18n label catalog JSON for a given packet format
// (2024/2025/2026). Lets the TS playback path emit a protocol_status with the
// recorded clip's labels without going through an Engine instance.
Napi::Value LabelsJson(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    uint16_t format = 2025;
    if (info.Length() >= 1 && info[0].IsNumber())
        format = static_cast<uint16_t>(info[0].As<Napi::Number>().Uint32Value());
    return Napi::String::New(env, tnrp::labelsJson(format));
}

// Module-level: the declarative card-colour spec (format-independent JSON). Lets
// both the live and playback paths hand the renderer one shared colour model.
Napi::Value CardColorsJson(const Napi::CallbackInfo& info) {
    return Napi::String::New(info.Env(), tnrp::cardColorsJson());
}

// Module-level: the Analysis map layer colours (tnrp/AnalysisMapPaint.h) as
// {"dark": palette, "light": palette}.
Napi::Value AnalysisMapPaletteJson(const Napi::CallbackInfo& info) {
    return Napi::String::New(info.Env(), tnrp::analysisMapPaletteJson());
}

// Module-level: reclaim stale "tracknrace_*.tmp" decompression temps left by
// crashed runs (either app). Call once at startup, after the single-instance
// lock is held, so a second instance can't unlink an active session's temp.
Napi::Value SweepTempFiles(const Napi::CallbackInfo& info) {
    tnrp::TnrdReader::sweepStaleTempFiles();
    return info.Env().Undefined();
}

// Runs a string-returning library call on libuv's threadpool and resolves a
// Promise<string> with its result. Used for first-time-setup checks that can
// take a while (firewall rule enumeration) or block on the user (UAC prompt).
class StringTaskWorker : public Napi::AsyncWorker {
public:
    StringTaskWorker(Napi::Env env, std::function<std::string()> task)
        : Napi::AsyncWorker(env), task_(std::move(task)), deferred_(Napi::Promise::Deferred::New(env)) {}
    void Execute() override { result_ = task_(); }
    void OnOK() override { deferred_.Resolve(Napi::String::New(Env(), result_)); }
    void OnError(const Napi::Error& e) override { deferred_.Reject(e.Value()); }
    Napi::Promise GetPromise() { return deferred_.Promise(); }

private:
    std::function<std::string()> task_;
    std::string result_;
    Napi::Promise::Deferred deferred_;
};

// Module-level: IPv4 addresses of active adapters as JSON
// ([{name, kind, address}], see tnrp/NetworkSetup.h).
Napi::Value NetworkInterfacesJson(const Napi::CallbackInfo& info) {
    return Napi::String::New(info.Env(), tnrp::networkInterfacesJson());
}

// Module-level: firewallStatusJson(port, programPath) -> Promise<string JSON>.
Napi::Value FirewallStatusJson(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    const uint16_t port = info.Length() >= 1 && info[0].IsNumber()
        ? static_cast<uint16_t>(info[0].As<Napi::Number>().Uint32Value()) : 20777;
    const std::string program = info.Length() >= 2 && info[1].IsString() ? info[1].As<Napi::String>().Utf8Value() : "";
    auto* worker = new StringTaskWorker(env, [port, program] { return tnrp::firewallStatusJson(port, program); });
    Napi::Promise promise = worker->GetPromise();
    worker->Queue();
    return promise;
}

// Module-level: firewallAllowProgram(programPath) -> Promise<string>; "" on
// success, "cancelled" when the UAC prompt is declined, otherwise an error.
Napi::Value FirewallAllowProgram(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    const std::string program = info.Length() >= 1 && info[0].IsString() ? info[0].As<Napi::String>().Utf8Value() : "";
    auto* worker = new StringTaskWorker(env, [program] { return tnrp::firewallAllowProgram(program); });
    Napi::Promise promise = worker->GetPromise();
    worker->Queue();
    return promise;
}

// Self-update (tnrp/Updates.h). Module-level, no Engine needed.
static std::string StringArg(const Napi::CallbackInfo& info, size_t index) {
    return info.Length() > index && info[index].IsString() ? info[index].As<Napi::String>().Utf8Value() : std::string{};
}

Napi::Value UpdateServiceUrl(const Napi::CallbackInfo& info) {
    return Napi::String::New(info.Env(), tnrp::updateServiceUrl());
}

Napi::Value UpdateServiceOverridden(const Napi::CallbackInfo& info) {
    return Napi::Boolean::New(info.Env(), tnrp::updateServiceOverridden());
}

// updateQuietPeriodOver(nowMs, sinceMs) -> boolean
Napi::Value UpdateQuietPeriodOver(const Napi::CallbackInfo& info) {
    const auto millis = [&](size_t index) -> int64_t {
        return info.Length() > index && info[index].IsNumber() ? info[index].As<Napi::Number>().Int64Value() : 0;
    };
    return Napi::Boolean::New(info.Env(), tnrp::updateQuietPeriodOver(millis(0), millis(1)));
}

// updateCheckPath(product, os, arch, format, currentVersion) -> string
Napi::Value UpdateCheckPath(const Napi::CallbackInfo& info) {
    return Napi::String::New(info.Env(), tnrp::updateCheckPath(StringArg(info, 0), StringArg(info, 1),
        StringArg(info, 2), StringArg(info, 3), StringArg(info, 4)));
}

// updateInstallPath(product, os, arch, format, currentVersion, version) -> string
Napi::Value UpdateInstallPath(const Napi::CallbackInfo& info) {
    return Napi::String::New(info.Env(), tnrp::updateInstallPath(StringArg(info, 0), StringArg(info, 1),
        StringArg(info, 2), StringArg(info, 3), StringArg(info, 4), StringArg(info, 5)));
}

// updateEvaluateJson(response, product, os, arch, format, currentVersion) -> string JSON
Napi::Value UpdateEvaluateJson(const Napi::CallbackInfo& info) {
    return Napi::String::New(info.Env(), tnrp::evaluateUpdateJson(StringArg(info, 0), StringArg(info, 1),
        StringArg(info, 2), StringArg(info, 3), StringArg(info, 4), StringArg(info, 5)));
}

// updateCompareVersions(left, right) -> 1 | 0 | -1 | null
Napi::Value UpdateCompareVersions(const Napi::CallbackInfo& info) {
    const auto order = tnrp::compareVersions(StringArg(info, 0), StringArg(info, 1));
    return order ? Napi::Number::New(info.Env(), *order) : info.Env().Null();
}

// updateVerifyFile(path, sha512, size) -> Promise<string>: "" when it matches.
Napi::Value UpdateVerifyFile(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    const std::string path = StringArg(info, 0);
    const std::string sha512 = StringArg(info, 1);
    const uint64_t size = info.Length() > 2 && info[2].IsNumber()
        ? static_cast<uint64_t>(info[2].As<Napi::Number>().Int64Value()) : 0;
    auto* worker = new StringTaskWorker(env, [path, sha512, size] { return tnrp::verifyUpdateFile(path, sha512, size); });
    Napi::Promise promise = worker->GetPromise();
    worker->Queue();
    return promise;
}

Napi::Object InitAll(Napi::Env env, Napi::Object exports) {
    TRACE("InitAll: module loading");
    TNRPAddon::Init(env, exports);
    TRACE("InitAll: module loaded");
    exports.Set("labelsJson", Napi::Function::New(env, LabelsJson));
    exports.Set("cardColorsJson", Napi::Function::New(env, CardColorsJson));
    exports.Set("analysisMapPaletteJson", Napi::Function::New(env, AnalysisMapPaletteJson));
    exports.Set("sweepTempFiles", Napi::Function::New(env, SweepTempFiles));
    exports.Set("networkInterfacesJson", Napi::Function::New(env, NetworkInterfacesJson));
    exports.Set("firewallStatusJson", Napi::Function::New(env, FirewallStatusJson));
    exports.Set("firewallAllowProgram", Napi::Function::New(env, FirewallAllowProgram));
    exports.Set("updateServiceUrl", Napi::Function::New(env, UpdateServiceUrl));
    exports.Set("updateServiceOverridden", Napi::Function::New(env, UpdateServiceOverridden));
    exports.Set("updateQuietPeriodOver", Napi::Function::New(env, UpdateQuietPeriodOver));
    exports.Set("updateCheckPath", Napi::Function::New(env, UpdateCheckPath));
    exports.Set("updateInstallPath", Napi::Function::New(env, UpdateInstallPath));
    exports.Set("updateEvaluateJson", Napi::Function::New(env, UpdateEvaluateJson));
    exports.Set("updateCompareVersions", Napi::Function::New(env, UpdateCompareVersions));
    exports.Set("updateVerifyFile", Napi::Function::New(env, UpdateVerifyFile));
    return exports;
}

NODE_API_MODULE(protocol_parser, InitAll)
