#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "tnrp/TrackMaps.h"

namespace tnrp::detail {

class TnrdV6Archive;

// Chooses the map for the live session or the loaded recording and, when
// asked, generates one for a track no saved map fits. The engine feeds it the
// rows it already parses; matching and generation run on a thread of its own
// and report "track_map" rows (TrackMapResolution) plus a
// "track_maps_changed" row when it saved a map.
class TrackMapTracker {
public:
    using Emit = std::function<void(const std::string& row)>;
    // The live session as an archive of positions, lap timing and aero, or
    // null when there is none.
    using OpenLive = std::function<std::unique_ptr<TnrdV6Archive>()>;

    TrackMapTracker(std::shared_ptr<TrackMapLibrary> library, Emit emit, OpenLive openLive);
    ~TrackMapTracker();
    TrackMapTracker(const TrackMapTracker&) = delete;
    TrackMapTracker& operator=(const TrackMapTracker&) = delete;

    // Off: no rows, no sampling. Turning it on resolves the current session.
    void setOptions(bool enabled, bool generate);

    // Live rows (the engine's receive thread). Cheap when nothing is needed.
    void noteLiveSession(uint64_t sessionUid, int trackId, double trackLengthM);
    void noteLiveTiming(std::string_view timingJson);
    void noteLivePositions(std::string_view positionsJson, float sessionTime);

    // A recording was loaded for playback, or playback ended.
    void notePlayback(const std::string& path, int trackId, double trackLengthM);
    void noteLive();

    // Resolve the current session again: a pin or the library changed.
    void refresh();
    // The last "track_map" row sent, or empty.
    std::string currentJson() const;

private:
    enum class Mode { Live, Playback };
    void run();
    void resolveLocked(std::unique_lock<std::mutex>& lock);
    void generateLocked(std::unique_lock<std::mutex>& lock);
    void publishLocked(std::unique_lock<std::mutex>& lock, TrackMapResolution resolution);
    bool wantsGeneration() const;

    std::shared_ptr<TrackMapLibrary> library_;
    Emit emit_;
    OpenLive openLive_;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::thread thread_;
    bool stop_ = false;

    bool enabled_ = false;
    bool generate_ = false;
    Mode mode_ = Mode::Live;
    // Bumped whenever the session being tracked changes, so work started for
    // an older one is dropped.
    uint64_t generation_ = 0;
    bool resolveRequested_ = false;
    bool generateRequested_ = false;
    bool generatedForSession_ = false;
    bool generationFailed_ = false;

    // Live session.
    bool liveKnown_ = false;
    uint64_t liveUid_ = 0;
    int liveTrackId_ = -1;
    double liveLengthM_ = 0.0;
    std::vector<WorldPoint> liveSamples_;
    size_t liveSampleNext_ = 0;
    bool liveChecked_ = false;
    float lastSampleTime_ = -1.0f;
    std::vector<int> livePitStatus_;
    std::vector<std::optional<WorldPoint>> lastCarPositions_;
    long long liveLapTotal_ = -1;
    long long lapTotalAtAttempt_ = -1;
    std::chrono::steady_clock::time_point lastAttempt_{};

    // Playback.
    std::string playbackPath_;
    int playbackTrackId_ = -1;
    double playbackLengthM_ = 0.0;
    std::optional<std::vector<WorldPoint>> playbackSamples_;

    std::optional<TrackMapResolution> current_;
    std::string currentJson_;
};

} // namespace tnrp::detail
