#include "TrackMapTracker.h"

#include <cmath>

#include "TrackMapBuilder.h"
#include "tnrd/TNRD_V6.h"
#include "tnrp/DisplayNames.h"
#include "tnrp/control_rows.h"
#include "tnrp/rows.h"

namespace tnrp::detail {
namespace {

constexpr glz::opts kPartialRead{.null_terminated = false, .error_on_unknown_keys = false};
constexpr size_t kMaxLiveSamples = 600;
constexpr float kSampleIntervalS = 0.5f;
// A car that moved less than this since its last sample is parked (on the
// grid, in the garage) and says nothing about the layout.
constexpr double kMinMoveM = 5.0;
constexpr auto kAttemptSpacing = std::chrono::seconds(30);
constexpr size_t kMaxCars = 24;

} // namespace

TrackMapTracker::TrackMapTracker(std::shared_ptr<TrackMapLibrary> library, Emit emit, OpenLive openLive)
    : library_(std::move(library)), emit_(std::move(emit)), openLive_(std::move(openLive)) {
    thread_ = std::thread(&TrackMapTracker::run, this);
}

TrackMapTracker::~TrackMapTracker() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void TrackMapTracker::setOptions(bool enabled, bool generate) {
    {
        std::lock_guard lock(mutex_);
        const bool changed = enabled != enabled_ || generate != generate_;
        enabled_ = enabled;
        generate_ = generate;
        if (!enabled) {
            current_.reset();
            currentJson_.clear();
            return;
        }
        if (!changed) return;
        generationFailed_ = false;
        resolveRequested_ = true;
    }
    wake_.notify_all();
}

bool TrackMapTracker::wantsGeneration() const {
    return enabled_ && generate_ && !generatedForSession_ && !generationFailed_ && current_ &&
           current_->checked && (current_->map_id.empty() || current_->mismatch);
}

void TrackMapTracker::noteLiveSession(uint64_t sessionUid, int trackId, double trackLengthM) {
    {
        std::lock_guard lock(mutex_);
        if (liveKnown_ && sessionUid == liveUid_ && trackId == liveTrackId_ &&
            std::abs(trackLengthM - liveLengthM_) < 1.0) return;
        liveKnown_ = true;
        liveUid_ = sessionUid;
        liveTrackId_ = trackId;
        liveLengthM_ = trackLengthM;
        liveSamples_.clear();
        liveSampleNext_ = 0;
        liveChecked_ = false;
        lastSampleTime_ = -1.0f;
        livePitStatus_.clear();
        liveLapTotal_ = -1;
        lapTotalAtAttempt_ = -1;
        lastCarPositions_.assign(kMaxCars, std::nullopt);
        if (mode_ != Mode::Live) return;
        ++generation_;
        generatedForSession_ = false;
        generationFailed_ = false;
        if (!enabled_) return;
        resolveRequested_ = true;
    }
    wake_.notify_all();
}

void TrackMapTracker::noteLiveTiming(std::string_view timingJson) {
    {
        std::lock_guard lock(mutex_);
        if (!enabled_ || mode_ != Mode::Live || !liveKnown_) return;
        if (liveChecked_ && !wantsGeneration()) return;
    }
    TimingRow row;
    if (glz::read<kPartialRead>(row, timingJson)) return;
    std::vector<int> pit(kMaxCars, 0);
    long long laps = 0;
    for (const auto& car : row.cars) {
        if (car.idx < 0 || car.idx >= static_cast<int>(kMaxCars)) continue;
        pit[static_cast<size_t>(car.idx)] = car.pit_status;
        laps += std::max(0, car.lap_num);
    }
    {
        std::lock_guard lock(mutex_);
        if (mode_ != Mode::Live) return;
        livePitStatus_ = std::move(pit);
        liveLapTotal_ = laps;
    }
}

void TrackMapTracker::noteLivePositions(std::string_view positionsJson, float sessionTime) {
    std::vector<int> pit;
    {
        std::lock_guard lock(mutex_);
        if (!enabled_ || mode_ != Mode::Live || !liveKnown_ || liveChecked_) return;
        if (lastSampleTime_ >= 0.0f && sessionTime >= lastSampleTime_ &&
            sessionTime - lastSampleTime_ < kSampleIntervalS) return;
        lastSampleTime_ = sessionTime;
        pit = livePitStatus_;
    }
    PositionsRow row;
    if (glz::read<kPartialRead>(row, positionsJson)) return;
    bool enough = false;
    {
        std::lock_guard lock(mutex_);
        if (mode_ != Mode::Live || liveChecked_) return;
        if (lastCarPositions_.size() != kMaxCars) lastCarPositions_.assign(kMaxCars, std::nullopt);
        for (const auto& car : row.cars) {
            if (car.idx < 0 || car.idx >= static_cast<int>(kMaxCars)) continue;
            if (car.x == 0.0 && car.z == 0.0) continue;
            const auto index = static_cast<size_t>(car.idx);
            const WorldPoint point{car.x, car.z};
            auto& last = lastCarPositions_[index];
            const bool moved = last && std::hypot(point[0] - (*last)[0], point[1] - (*last)[1]) >= kMinMoveM;
            last = point;
            if (!moved || (index < pit.size() && pit[index] != 0)) continue;
            if (liveSamples_.size() < kMaxLiveSamples) {
                liveSamples_.push_back(point);
            } else {
                liveSamples_[liveSampleNext_] = point;
                liveSampleNext_ = (liveSampleNext_ + 1) % kMaxLiveSamples;
            }
        }
        enough = enoughFitSamples(liveSamples_);
        if (enough) resolveRequested_ = true;
    }
    if (enough) wake_.notify_all();
}

void TrackMapTracker::notePlayback(const std::string& path, int trackId, double trackLengthM) {
    {
        std::lock_guard lock(mutex_);
        mode_ = Mode::Playback;
        // Loading a recording drops the live session from the engine; the
        // next session packet starts it again.
        liveKnown_ = false;
        liveChecked_ = false;
        liveSamples_.clear();
        liveSampleNext_ = 0;
        playbackPath_ = path;
        playbackTrackId_ = trackId;
        playbackLengthM_ = trackLengthM;
        playbackSamples_.reset();
        ++generation_;
        generatedForSession_ = false;
        generationFailed_ = false;
        if (!enabled_) return;
        resolveRequested_ = true;
    }
    wake_.notify_all();
}

void TrackMapTracker::noteLive() {
    {
        std::lock_guard lock(mutex_);
        if (mode_ == Mode::Live) return;
        mode_ = Mode::Live;
        playbackPath_.clear();
        playbackSamples_.reset();
        ++generation_;
        generatedForSession_ = false;
        generationFailed_ = false;
        if (!enabled_) return;
        resolveRequested_ = true;
    }
    wake_.notify_all();
}

void TrackMapTracker::refresh() {
    {
        std::lock_guard lock(mutex_);
        if (!enabled_) return;
        resolveRequested_ = true;
    }
    wake_.notify_all();
}

std::string TrackMapTracker::currentJson() const {
    std::lock_guard lock(mutex_);
    return currentJson_;
}

void TrackMapTracker::run() {
    std::unique_lock lock(mutex_);
    while (true) {
        wake_.wait_for(lock, std::chrono::seconds(1),
                       [this] { return stop_ || resolveRequested_ || generateRequested_; });
        if (stop_) return;
        if (resolveRequested_) {
            resolveRequested_ = false;
            resolveLocked(lock);
            continue;
        }
        // Live: try again when a car has completed another lap since the last
        // attempt, at most every kAttemptSpacing.
        if (!generateRequested_ && mode_ == Mode::Live && wantsGeneration() && liveLapTotal_ > 0 &&
            liveLapTotal_ > lapTotalAtAttempt_ &&
            std::chrono::steady_clock::now() - lastAttempt_ >= kAttemptSpacing)
            generateRequested_ = true;
        if (generateRequested_) {
            generateRequested_ = false;
            generateLocked(lock);
        }
    }
}

void TrackMapTracker::resolveLocked(std::unique_lock<std::mutex>& lock) {
    if (!enabled_) return;
    const uint64_t generation = generation_;
    const Mode mode = mode_;
    int trackId = -1;
    double lengthM = 0.0;
    std::vector<WorldPoint> samples;
    std::string path;
    if (mode == Mode::Live) {
        if (!liveKnown_) return;
        trackId = liveTrackId_;
        lengthM = liveLengthM_;
        samples = liveSamples_;
    } else {
        trackId = playbackTrackId_;
        lengthM = playbackLengthM_;
        path = playbackPath_;
        if (playbackSamples_) samples = *playbackSamples_;
    }
    const bool needPlaybackSamples = mode == Mode::Playback && !playbackSamples_;

    lock.unlock();
    if (needPlaybackSamples) samples = recordingFitSamples(path);
    TrackMapResolution resolution = library_->resolve(trackId, lengthM, samples);
    resolution.scope = mode == Mode::Live ? "live" : "playback";
    lock.lock();

    if (generation != generation_ || !enabled_) return;
    if (needPlaybackSamples) playbackSamples_ = std::move(samples);
    if (mode == Mode::Live && resolution.checked) liveChecked_ = true;
    current_ = resolution;
    resolution.generating = wantsGeneration();
    publishLocked(lock, std::move(resolution));
    if (generation == generation_ && wantsGeneration() &&
        (mode == Mode::Playback || liveLapTotal_ > 0))
        generateRequested_ = true;
}

void TrackMapTracker::generateLocked(std::unique_lock<std::mutex>& lock) {
    if (!wantsGeneration()) return;
    const uint64_t generation = generation_;
    const Mode mode = mode_;
    TrackMapGenerateInfo info;
    std::string path;
    if (mode == Mode::Live) {
        lapTotalAtAttempt_ = liveLapTotal_;
        lastAttempt_ = std::chrono::steady_clock::now();
        info.trackId = liveTrackId_;
        info.trackLengthM = liveLengthM_;
        if (liveTrackId_ >= 0) info.trackName = std::string(circuitName(liveTrackId_));
        info.createdFrom = "Live session";
    } else {
        path = playbackPath_;
    }

    lock.unlock();
    TrackMapData map;
    std::string error;
    bool built = false;
    if (mode == Mode::Live) {
        if (auto archive = openLive_ ? openLive_() : nullptr)
            built = buildTrackMap(*archive, info, map, &error);
    } else {
        built = generateTrackMapFromRecording(path, map, &error);
    }
    std::string id;
    if (built) {
        lock.lock();
        const bool current = generation == generation_ && enabled_;
        lock.unlock();
        if (current) id = library_->saveGenerated(std::move(map), &error);
    }
    lock.lock();

    if (generation != generation_ || !enabled_) return;
    if (id.empty()) {
        // A live session tries again after the next lap; a recording will not
        // gain any.
        if (mode == Mode::Playback) {
            generationFailed_ = true;
            if (current_) {
                TrackMapResolution resolution = *current_;
                resolution.generating = false;
                publishLocked(lock, std::move(resolution));
            }
        }
        return;
    }
    generatedForSession_ = true;
    TrackMapResolution resolution = current_.value_or(TrackMapResolution{});
    resolution.scope = mode == Mode::Live ? "live" : "playback";
    resolution.map_id = id;
    resolution.reason = "generated";
    resolution.mismatch = false;
    resolution.generating = false;
    resolution.fit_m = -1.0;
    current_ = resolution;
    publishLocked(lock, std::move(resolution));
    lock.unlock();
    emit_(R"({"type":"track_maps_changed"})");
    lock.lock();
}

void TrackMapTracker::publishLocked(std::unique_lock<std::mutex>& lock, TrackMapResolution resolution) {
    std::string json = writeJson(resolution);
    currentJson_ = json;
    lock.unlock();
    emit_(json);
    lock.lock();
}

} // namespace tnrp::detail
