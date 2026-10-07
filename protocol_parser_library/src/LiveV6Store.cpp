#include "LiveV6Store.h"

#include "tnrp/TnrdReader.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <future>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace tnrp::detail {
namespace {

// The legacy chart families the live renderer backfills.
constexpr uint32_t kChartFamilies =
    (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 11) | (1u << 12);

// The V6 types that feed the chart families in `mask`, as TnrdReader maps
// them for a recording.
std::vector<uint8_t> typesForFamilies(uint32_t mask) {
    std::vector<uint8_t> out;
    const auto add = [&](std::initializer_list<uint8_t> values) {
        out.insert(out.end(), values.begin(), values.end());
    };
    if (mask & (1u << 1)) add({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11});
    if (mask & (1u << 2)) add({7, 13, 15, 16, 17, 18, 19, 20});
    if (mask & (1u << 3)) add({12, 14});
    if (mask & (1u << 4)) add({24});
    if (mask & (1u << 11)) add({21});
    if (mask & (1u << 12)) add({22});
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// What Strategy reads (kStrategyDependencyMask): the player's status, damage
// and tyre state, and every car's lap timing and tyre state.
const std::vector<uint8_t> kStrategyTypes{7, 12, 13, 14, 15, 16, 17, 18, 19, 20, 24};

template <typename T>
void updateAtomicMaximum(std::atomic<T>& target, T value) {
    T current = target.load(std::memory_order_relaxed);
    while (current < value && !target.compare_exchange_weak(
        current, value, std::memory_order_relaxed, std::memory_order_relaxed)) {}
}

// The player's chart families over the request, seeded at its start as a
// playback seek reads them, and its race events.
LiveV6History readHistory(const std::shared_ptr<const V6MemoryImage>& image,
                          const LiveV6Store::HistoryRequest& request) {
    LiveV6History out;
    if (!image) return out;
    if (request.chartMask != 0) {
        TnrdV6Archive archive;
        HeaderRow header;
        std::string error;
        if (archive.openMemory(image, header, &error)) {
            if (const auto player = archive.playerDriverIndex()) {
                auto payload = std::make_shared<std::vector<uint8_t>>();
                if (archive.columnarHistory(*player, {}, request.chartMask, request.chartFrom,
                                            request.through, true, *payload, &error))
                    out.columnar = std::move(payload);
            }
        }
        if (!error.empty()) {
            std::fprintf(stderr, "[live-v6] history read failed: %s\n", error.c_str());
            std::fflush(stderr);
        }
    }
    if (std::isfinite(request.eventsFrom)) {
        for (const auto& record : image->shared) {
            if (record.phase != image->phase || record.sessionTime < request.eventsFrom ||
                record.sessionTime > request.through) continue;
            if (!out.events.empty()) out.events.push_back('\n');
            out.events += record.json;
        }
    }
    return out;
}

}  // namespace

struct LiveV6Store::Impl {
    RequestImage requestImage;
    // Bumped whenever the live timeline moves: a read requested before it
    // describes a timeline that no longer exists.
    std::atomic<uint64_t> generation{1};

    mutable std::mutex readMutex;
    std::condition_variable readCv;
    std::deque<std::function<void()>> reads;
    bool readStopping{};

    std::atomic<uint64_t> imagesReceived{0}, readsCompleted{0};
    std::atomic<size_t> lastImageBytes{0}, peakImageBytes{0};

    // Last, so every member above exists before the thread starts.
    std::thread reader;

    explicit Impl(RequestImage request)
        : requestImage(std::move(request)), reader([this] { runReads(); }) {}

    ~Impl() {
        {
            std::lock_guard lock(readMutex);
            readStopping = true;
            reads.clear();
        }
        readCv.notify_all();
        if (reader.joinable()) reader.join();
    }

    void post(std::function<void()> read) {
        {
            std::lock_guard lock(readMutex);
            if (readStopping) return;
            reads.push_back(std::move(read));
        }
        readCv.notify_one();
    }

    void noteImage(const std::shared_ptr<const V6MemoryImage>& image) {
        if (!image) return;
        imagesReceived.fetch_add(1, std::memory_order_relaxed);
        lastImageBytes.store(image->encodedBytes, std::memory_order_relaxed);
        updateAtomicMaximum(peakImageBytes, image->encodedBytes);
    }

    void runReads() {
        for (;;) {
            std::function<void()> read;
            {
                std::unique_lock lock(readMutex);
                readCv.wait(lock, [this] { return readStopping || !reads.empty(); });
                if (readStopping) return;
                read = std::move(reads.front());
                reads.pop_front();
            }
            read();
            readsCompleted.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Requests an image on the writer thread and runs `read` with it on the
    // read thread. The writer may call back after this store is gone; the
    // weak reference then drops the read.
    static void readImage(const std::shared_ptr<Impl>& self, V6ImageFilter filter,
                          std::function<void(Impl&, std::shared_ptr<const V6MemoryImage>)> read) {
        std::weak_ptr<Impl> weak = self;
        self->requestImage(filter,
            [weak, read = std::move(read)](std::shared_ptr<const V6MemoryImage> image) mutable {
                const auto impl = weak.lock();
                if (!impl) return;
                impl->noteImage(image);
                Impl* target = impl.get();
                impl->post([target, image = std::move(image), read = std::move(read)]() mutable {
                    read(*target, std::move(image));
                });
            });
    }
};

LiveV6Store::LiveV6Store(RequestImage requestImage)
    : impl_(std::make_shared<Impl>(std::move(requestImage))) {}
LiveV6Store::~LiveV6Store() = default;

void LiveV6Store::invalidateReads() {
    impl_->generation.fetch_add(1, std::memory_order_acq_rel);
}

void LiveV6Store::requestHistory(const HistoryRequest& request,
                                 std::function<void(LiveV6History)> done,
                                 std::function<void()> stale) {
    const uint64_t generation = impl_->generation.load(std::memory_order_acquire);
    V6ImageFilter filter;
    filter.playerOnly = true;
    filter.chunks = request.chartMask != 0;
    filter.types = typesForFamilies(request.chartMask);
    if (std::isfinite(request.eventsFrom)) {
        filter.shared = true;
        filter.sharedType = 6;
        filter.sharedFrom = request.eventsFrom;
    }
    Impl::readImage(impl_, std::move(filter),
        [request, generation, done = std::move(done), stale = std::move(stale)]
        (Impl& impl, std::shared_ptr<const V6MemoryImage> image) mutable {
            LiveV6History history = readHistory(image, request);
            if (impl.generation.load(std::memory_order_acquire) != generation) {
                if (stale) stale();
                return;
            }
            if (done) done(std::move(history));
        });
}

void LiveV6Store::requestLap(
    int lapNumber,
    std::function<void(int, int, float, float, std::shared_ptr<std::vector<uint8_t>>)> done) {
    const uint64_t generation = impl_->generation.load(std::memory_order_acquire);
    V6ImageFilter filter;
    filter.playerOnly = true;
    filter.types = typesForFamilies(kChartFamilies);
    Impl::readImage(impl_, std::move(filter),
        [lapNumber, generation, done = std::move(done)](Impl& impl, std::shared_ptr<const V6MemoryImage> image) mutable {
            if (!image) return;
            TnrdV6Archive archive;
            HeaderRow header;
            std::string error;
            if (!archive.openMemory(image, header, &error)) return;
            const auto player = archive.playerDriverIndex();
            if (!player) return;
            // Summaries come in start order, so the last match of a lap number
            // is its latest attempt (a garage restart can reuse one).
            std::optional<V6LapSummary> best;
            for (const auto& lap : archive.driverLapSummaries(*player)) {
                if (lap.phase != image->phase || lap.lapNumber == 0 ||
                    lap.endSessionTime <= lap.startSessionTime) continue;
                if (lapNumber > 0) {
                    if (lap.lapNumber == static_cast<uint32_t>(lapNumber)) best = lap;
                } else if (lap.isCompleted && lap.lapTimeMs != 0 &&
                           (!best || lap.lapTimeMs < best->lapTimeMs)) {
                    best = lap;
                }
            }
            if (!best) return;
            // A lap's samples stop short of its end, which is the next lap's
            // first sample.
            const float end = std::nextafter(best->endSessionTime,
                                             -std::numeric_limits<float>::infinity());
            auto payload = std::make_shared<std::vector<uint8_t>>();
            if (!archive.columnarHistory(*player, {}, kChartFamilies, best->startSessionTime, end,
                                         true, *payload, &error)) return;
            if (impl.generation.load(std::memory_order_acquire) != generation || !done) return;
            done(static_cast<int>(best->lapNumber), static_cast<int>(best->lapTimeMs),
                 best->startSessionTime, best->endSessionTime, std::move(payload));
        });
}

bool LiveV6Store::rebuildStrategy(float through, int minimumStops,
                                  const TeamColorOverrides& teamColors, StrategyProcessor& out) {
    V6ImageFilter filter;
    filter.types = kStrategyTypes;
    filter.shared = true;
    auto promise = std::make_shared<std::promise<std::shared_ptr<const V6MemoryImage>>>();
    auto future = promise->get_future();
    impl_->requestImage(filter, [promise](std::shared_ptr<const V6MemoryImage> image) {
        promise->set_value(std::move(image));
    });
    const auto image = future.get();
    if (!image) return false;
    impl_->noteImage(image);
    auto archive = std::make_unique<TnrdV6Archive>();
    HeaderRow header;
    std::string error;
    if (!archive->openMemory(image, header, &error) || !archive->playerDriverIndex()) return false;
    TnrdReader reader;
    if (!reader.loadV6ArchiveForStrategy(std::move(archive), header)) return false;
    reader.setStrategyMinimumStops(minimumStops);
    reader.setTeamColorOverrides(teamColors);
    (void)reader.strategySnapshotAt(through, &out);
    return true;
}

LiveV6Store::ReadStats LiveV6Store::readStats() const {
    ReadStats out;
    {
        std::lock_guard lock(impl_->readMutex);
        out.queuedReads = impl_->reads.size();
    }
    out.imagesReceived = impl_->imagesReceived.load(std::memory_order_relaxed);
    out.readsCompleted = impl_->readsCompleted.load(std::memory_order_relaxed);
    out.lastImageEncodedBytes = impl_->lastImageBytes.load(std::memory_order_relaxed);
    out.peakImageEncodedBytes = impl_->peakImageBytes.load(std::memory_order_relaxed);
    return out;
}

} // namespace tnrp::detail
