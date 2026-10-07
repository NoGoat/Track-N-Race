#pragma once

#include "tnrd/TNRD_V6.h"
#include "tnrp/Strategy.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace tnrp::detail {

// What a history read returns: the player's chart families as a V6H1 payload
// (TnrdV6Archive::columnarHistory, as playback produces), and race events as
// newline-joined JSON.
struct LiveV6History {
    std::shared_ptr<std::vector<uint8_t>> columnar;
    std::string events;
};

// The read side of the live session's V6 store. The store itself is the
// recorder's one V6 writer (TnrdWriter with setRetainSession): it takes every
// packet once, holds the session by driver, lap and type, commits laps on the
// write delay, and also writes them to a file while recording. This class asks
// that writer's thread for a memory image (committed chunks shared, laps still
// being built encoded for it), and decodes the image on a thread of its own
// with TnrdV6Archive, exactly as playback reads a file, so a long All Laps read
// never holds up ingestion.
class LiveV6Store {
public:
    using ImageCallback = std::function<void(std::shared_ptr<const V6MemoryImage>)>;
    // Builds an image on the writer thread, after everything queued before
    // the call, and calls back there; null when no session is held.
    using RequestImage = std::function<void(const V6ImageFilter&, ImageCallback)>;

    explicit LiveV6Store(RequestImage requestImage);
    ~LiveV6Store();
    LiveV6Store(const LiveV6Store&) = delete;
    LiveV6Store& operator=(const LiveV6Store&) = delete;

    // The live timeline moved (a flashback, a clock reset, a new session):
    // reads already requested report stale.
    void invalidateReads();

    struct HistoryRequest {
        uint32_t chartMask{};    // legacy chart families (1, 2, 3, 4, 11, 12)
        float chartFrom{};
        // Finite: race events from here on are included.
        float eventsFrom{std::numeric_limits<float>::infinity()};
        float through{};
    };
    // Callbacks run on the read thread. `stale` runs instead of `done` when
    // the timeline moved after the read was requested.
    void requestHistory(const HistoryRequest& request,
                        std::function<void(LiveV6History)> done,
                        std::function<void()> stale = {});
    // One of the player's laps, its chart families as V6H1: lap `lapNumber`
    // (its latest attempt), or with lapNumber 0 the fastest completed lap.
    // Committed laps are decompressed from their chunks; a lap still inside
    // the write delay is read from its builders. Nothing is called back when
    // there is no such lap, or the timeline moved meanwhile.
    void requestLap(int lapNumber,
                    std::function<void(int lapNum, int lapTimeMs, float start, float end,
                                       std::shared_ptr<std::vector<uint8_t>> columnar)> done);
    // The Strategy reducer at `through`, replayed from the whole session as a
    // V6 recording's playback rebuild does. Blocks until the writer thread has
    // built an image. False when no session is held.
    bool rebuildStrategy(float through, int minimumStops, const TeamColorOverrides& teamColors,
                         StrategyProcessor& out);

    struct ReadStats {
        size_t queuedReads{};
        uint64_t imagesReceived{}, readsCompleted{};
        size_t lastImageEncodedBytes{}, peakImageEncodedBytes{};
    };
    ReadStats readStats() const;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace tnrp::detail
