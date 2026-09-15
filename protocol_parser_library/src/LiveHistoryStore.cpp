#include "LiveHistoryStore.h"

#include "tnrp/BinaryRows.h"
#include "tnrp/Strategy.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <string_view>
#include <thread>
#include <utility>

#include <zstd.h>

namespace tnrp::detail {
namespace {

constexpr uint32_t kHistoricalMask =
    (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 11) |
    (1u << 12);

bool isPacked(uint8_t type) { return type == 1 || type == 11 || type == 12; }

template <typename T>
void updateAtomicMaximum(std::atomic<T>& target, T value) {
    T current = target.load(std::memory_order_relaxed);
    while (current < value && !target.compare_exchange_weak(
        current, value, std::memory_order_relaxed, std::memory_order_relaxed)) {}
}

float scanTime(std::string_view json) {
    constexpr std::string_view key = "\"session_time\":";
    const size_t at = json.find(key);
    if (at == std::string_view::npos) return -1.0f;
    return std::strtof(json.data() + at + key.size(), nullptr);
}

float packedTime(const uint8_t* record, size_t length) {
    if (length < 5) return -1.0f;
    float value{};
    std::memcpy(&value, record + 1, sizeof(value));
    return value;
}

struct Family {
    std::vector<uint8_t> packed;
    std::vector<LiveHistoryJsonRow> json;
    size_t jsonPayloadBytes{};
    size_t jsonPayloadCapacityBytes{};
    // JSON is compressed as newline-delimited text. Preserve the ingest
    // sequence separately so a Strategy rebuild retains deterministic ordering
    // when multiple families share a session timestamp.
    std::vector<uint64_t> sequences;
    std::vector<uint8_t> compressed;
    size_t plainSize{};
    bool compressionQueued{};
};

void refreshJsonUsage(Family& family) {
    family.jsonPayloadBytes = 0;
    family.jsonPayloadCapacityBytes = 0;
    for (const auto& row : family.json) {
        if (!row.json) continue;
        family.jsonPayloadBytes += row.json->size();
        family.jsonPayloadCapacityBytes += row.json->capacity() + 1;
    }
}

struct LapSegment {
    int lapNum{};
    float start{};
    float end{};
    int lapTimeMs{};
    std::array<Family, 16> families;
    mutable std::mutex mutex;
};

std::vector<uint8_t> familyPlain(const Family& family, uint8_t type) {
    if (isPacked(type)) return family.packed;
    size_t size = 0;
    for (const auto& row : family.json)
        if (row.json) size += row.json->size() + 1;
    std::vector<uint8_t> plain;
    plain.reserve(size);
    for (const auto& row : family.json) {
        if (!row.json) continue;
        plain.insert(plain.end(), row.json->begin(), row.json->end());
        plain.push_back('\n');
    }
    return plain;
}

bool decompress(const Family& family, std::vector<uint8_t>& plain) {
    if (family.compressed.empty()) return false;
    plain.resize(family.plainSize);
    const size_t result = ZSTD_decompress(plain.data(), plain.size(),
                                          family.compressed.data(),
                                          family.compressed.size());
    return !ZSTD_isError(result) && result == plain.size();
}

void appendJsonLines(const uint8_t* data, size_t length, float from, float through,
                     std::string& output,
                     std::vector<LiveHistoryJsonRow>* rows = nullptr,
                     const std::vector<uint64_t>* sequences = nullptr) {
    size_t start = 0;
    size_t ordinal = 0;
    while (start < length) {
        const auto* newline = static_cast<const uint8_t*>(
            std::memchr(data + start, '\n', length - start));
        const size_t end = newline ? static_cast<size_t>(newline - data) : length;
        if (end > start) {
            const std::string_view line(
                reinterpret_cast<const char*>(data + start), end - start);
            const float time = scanTime(line);
            if (time >= from && time <= through) {
                if (rows) {
                    const uint64_t sequence = sequences && ordinal < sequences->size()
                        ? (*sequences)[ordinal] : 0;
                    rows->push_back({time, sequence,
                        std::make_shared<const std::string>(line)});
                } else {
                    output.append(line);
                    output.push_back('\n');
                }
            }
        }
        if (!newline) break;
        start = end + 1;
        ++ordinal;
    }
}

} // namespace

struct LiveHistoryStore::Impl {
    enum class JobKind { Compress, Decompress, Range };
    struct Job {
        JobKind kind{};
        std::shared_ptr<LapSegment> lap;
        std::vector<std::shared_ptr<LapSegment>> laps;
        uint32_t mask{};
        float from{};
        float through{};
        BackfillCallback callback;
        uint64_t generation{};
    };

    mutable std::mutex stateMutex;
    std::map<int, std::shared_ptr<LapSegment>> laps;
    int current{};
    int previous{};
    int previousPrevious{};
    int fastest{};
    int fastestMs{};
    uint64_t generation{1};

    std::mutex workMutex;
    std::condition_variable workCv;
    std::deque<Job> jobs;
    bool stopping{};
    std::thread worker;
    std::atomic<int> activeJobKind{0};
    std::atomic<uint64_t> compressionJobs{0};
    std::atomic<uint64_t> compressedFamilies{0};
    std::atomic<uint64_t> compressionPlainBytesProcessed{0};
    std::atomic<uint64_t> compressionPlainBufferBytesAllocated{0};
    std::atomic<uint64_t> compressionBufferBytesAllocated{0};
    std::atomic<uint64_t> compressedOutputBytesAllocated{0};
    std::atomic<size_t> lastCompressionPlainBytes{0};
    std::atomic<size_t> lastCompressionBufferBytes{0};
    std::atomic<size_t> lastCompressionScratchBytes{0};
    std::atomic<size_t> peakCompressionPlainBytes{0};
    std::atomic<size_t> peakCompressionBufferBytes{0};
    std::atomic<size_t> peakCompressionScratchBytes{0};
    std::atomic<uint64_t> decompressionJobs{0};
    std::atomic<uint64_t> decompressionBufferBytesAllocated{0};
    std::atomic<size_t> lastDecompressionBufferBytes{0};
    std::atomic<size_t> peakDecompressionBufferBytes{0};
    std::atomic<uint64_t> rangeJobs{0};

    Impl() : worker([this] { run(); }) {}

    ~Impl() {
        {
            std::lock_guard<std::mutex> lock(workMutex);
            stopping = true;
            jobs.clear();
        }
        workCv.notify_all();
        if (worker.joinable()) worker.join();
    }

    void enqueue(Job job) {
        {
            std::lock_guard<std::mutex> lock(workMutex);
            if (stopping) return;
            jobs.push_back(std::move(job));
        }
        workCv.notify_one();
    }

    bool pinned(int lapNum) const {
        return lapNum == current || lapNum == previous ||
               lapNum == previousPrevious || lapNum == fastest;
    }

    void queueEligible() {
        for (const auto& [lapNum, lap] : laps) {
            if (pinned(lapNum)) continue;
            bool needsWork = false;
            {
                std::lock_guard<std::mutex> lapLock(lap->mutex);
                for (size_t familyIndex = 1; familyIndex < lap->families.size(); ++familyIndex) {
                    auto& family = lap->families[familyIndex];
                    if ((!family.packed.empty() || !family.json.empty()) &&
                        family.compressed.empty() && !family.compressionQueued) {
                        family.compressionQueued = true;
                        needsWork = true;
                    }
                }
            }
            if (needsWork) enqueue({JobKind::Compress, lap});
        }
    }

    void compressLap(const std::shared_ptr<LapSegment>& lap) {
        compressionJobs.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(lap->mutex);
        for (size_t familyIndex = 1; familyIndex < lap->families.size(); ++familyIndex) {
            const auto type = static_cast<uint8_t>(familyIndex);
            auto& family = lap->families[familyIndex];
            if (!family.compressionQueued || !family.compressed.empty()) continue;
            auto plain = familyPlain(family, type);
            family.compressionQueued = false;
            if (plain.empty()) continue;
            std::vector<uint8_t> compressed(ZSTD_compressBound(plain.size()));
            compressionPlainBytesProcessed.fetch_add(plain.size(), std::memory_order_relaxed);
            compressionPlainBufferBytesAllocated.fetch_add(
                plain.capacity(), std::memory_order_relaxed);
            compressionBufferBytesAllocated.fetch_add(compressed.capacity(), std::memory_order_relaxed);
            lastCompressionPlainBytes.store(plain.size(), std::memory_order_relaxed);
            lastCompressionBufferBytes.store(compressed.capacity(), std::memory_order_relaxed);
            const size_t scratchBytes = plain.capacity() + compressed.capacity();
            lastCompressionScratchBytes.store(scratchBytes, std::memory_order_relaxed);
            updateAtomicMaximum(peakCompressionPlainBytes, plain.size());
            updateAtomicMaximum(peakCompressionBufferBytes, compressed.capacity());
            updateAtomicMaximum(peakCompressionScratchBytes, scratchBytes);
            const size_t size = ZSTD_compress(compressed.data(), compressed.size(),
                                              plain.data(), plain.size(), 3);
            if (ZSTD_isError(size)) continue;
            // resize() would only reduce the logical size: moving that vector
            // into the lap would retain its compressBound-sized allocation.
            // Copy the much smaller result into an exact-sized allocation so
            // completing a lap actually releases its uncompressed footprint.
            std::vector<uint8_t> stored(size);
            const size_t fullScratchBytes = scratchBytes + stored.capacity();
            lastCompressionScratchBytes.store(fullScratchBytes, std::memory_order_relaxed);
            updateAtomicMaximum(peakCompressionScratchBytes, fullScratchBytes);
            compressedFamilies.fetch_add(1, std::memory_order_relaxed);
            compressedOutputBytesAllocated.fetch_add(stored.capacity(), std::memory_order_relaxed);
            std::memcpy(stored.data(), compressed.data(), size);
            if (!isPacked(type)) {
                family.sequences.reserve(family.json.size());
                for (const auto& row : family.json)
                    family.sequences.push_back(row.sequence);
            }
            family.plainSize = plain.size();
            family.compressed = std::move(stored);
            family.packed.clear();
            family.packed.shrink_to_fit();
            family.json.clear();
            family.json.shrink_to_fit();
            family.jsonPayloadBytes = 0;
            family.jsonPayloadCapacityBytes = 0;
        }
    }

    static void cancelCompression(const std::shared_ptr<LapSegment>& lap) {
        std::lock_guard<std::mutex> lock(lap->mutex);
        for (auto& family : lap->families) family.compressionQueued = false;
    }

    void decompressLap(const std::shared_ptr<LapSegment>& lap) {
        decompressionJobs.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(lap->mutex);
        for (size_t familyIndex = 1; familyIndex < lap->families.size(); ++familyIndex) {
            const auto type = static_cast<uint8_t>(familyIndex);
            auto& family = lap->families[familyIndex];
            family.compressionQueued = false;
            if (family.compressed.empty()) continue;
            std::vector<uint8_t> plain;
            decompressionBufferBytesAllocated.fetch_add(family.plainSize, std::memory_order_relaxed);
            lastDecompressionBufferBytes.store(family.plainSize, std::memory_order_relaxed);
            updateAtomicMaximum(peakDecompressionBufferBytes, family.plainSize);
            if (!decompress(family, plain)) continue;
            if (isPacked(type)) {
                family.packed = std::move(plain);
            } else {
                std::string ignored;
                appendJsonLines(plain.data(), plain.size(),
                                -std::numeric_limits<float>::infinity(),
                                std::numeric_limits<float>::infinity(), ignored,
                                &family.json, &family.sequences);
                refreshJsonUsage(family);
            }
            family.compressed.clear();
            family.compressed.shrink_to_fit();
            family.sequences.clear();
            family.sequences.shrink_to_fit();
            family.plainSize = 0;
        }
    }

    static void gatherFamily(const std::shared_ptr<LapSegment>& lap, uint8_t type,
                             float from, float through,
                             LiveHistoryBackfill& output) {
        std::lock_guard<std::mutex> lock(lap->mutex);
        const auto& family = lap->families[type];
        std::vector<uint8_t> decompressed;
        if (!family.compressed.empty() && !decompress(family, decompressed)) return;

        if (isPacked(type)) {
            const auto* data = family.compressed.empty()
                ? family.packed.data() : decompressed.data();
            const size_t length = family.compressed.empty()
                ? family.packed.size() : decompressed.size();
            (void)bin::forEachPackedRecord(data, length,
                [&](uint8_t, const uint8_t* record, size_t recordLength) {
                    const float time = packedTime(record, recordLength);
                    if (time >= from && time <= through)
                        output.binary->insert(output.binary->end(), record,
                                              record + recordLength);
                });
            return;
        }

        if (family.compressed.empty()) {
            for (const auto& row : family.json) {
                if (!row.json || row.sessionTime < from || row.sessionTime > through)
                    continue;
                output.json += *row.json;
                output.json.push_back('\n');
            }
        } else {
            appendJsonLines(decompressed.data(), decompressed.size(), from, through,
                            output.json);
        }
    }

    static LiveHistoryBackfill gather(
        const std::vector<std::shared_ptr<LapSegment>>& segments,
        uint32_t mask, float from, float through) {
        LiveHistoryBackfill output;
        output.binary = std::make_shared<std::vector<uint8_t>>();
        for (uint8_t type = 1; type < 16; ++type) {
            if (!(mask & (1u << type))) continue;
            for (const auto& lap : segments) {
                bool intersects = false;
                {
                    std::lock_guard<std::mutex> lock(lap->mutex);
                    intersects = lap->end >= from && lap->start <= through;
                }
                if (intersects)
                    gatherFamily(lap, type, from, through, output);
            }
        }
        if (!output.json.empty()) output.json.pop_back();
        if (output.binary->empty()) output.binary.reset();
        return output;
    }

    void run() {
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(workMutex);
                workCv.wait(lock, [&] { return stopping || !jobs.empty(); });
                if (stopping && jobs.empty()) return;
                job = std::move(jobs.front());
                jobs.pop_front();
            }
            if (job.kind == JobKind::Compress) {
                activeJobKind.store(1, std::memory_order_relaxed);
                bool eligible = false;
                {
                    std::lock_guard<std::mutex> lock(stateMutex);
                    const auto found = laps.find(job.lap->lapNum);
                    eligible = found != laps.end() && found->second == job.lap &&
                               !pinned(job.lap->lapNum);
                }
                if (eligible) compressLap(job.lap);
                else cancelCompression(job.lap);
                activeJobKind.store(0, std::memory_order_relaxed);
                continue;
            }
            if (job.kind == JobKind::Decompress) {
                activeJobKind.store(2, std::memory_order_relaxed);
                decompressLap(job.lap);
                activeJobKind.store(0, std::memory_order_relaxed);
                continue;
            }
            activeJobKind.store(3, std::memory_order_relaxed);
            rangeJobs.fetch_add(1, std::memory_order_relaxed);
            auto result = gather(job.laps, job.mask, job.from, job.through);
            bool currentGeneration = false;
            {
                std::lock_guard<std::mutex> lock(stateMutex);
                currentGeneration = job.generation == generation;
            }
            if (currentGeneration && job.callback)
                job.callback(std::move(result));
            activeJobKind.store(0, std::memory_order_relaxed);
        }
    }
};

LiveHistoryStore::LiveHistoryStore() : impl_(std::make_unique<Impl>()) {}
LiveHistoryStore::~LiveHistoryStore() = default;

void LiveHistoryStore::reset() {
    std::lock_guard<std::mutex> lock(impl_->stateMutex);
    impl_->laps.clear();
    impl_->current = impl_->previous = impl_->previousPrevious = 0;
    impl_->fastest = impl_->fastestMs = 0;
    ++impl_->generation;
}

void LiveHistoryStore::setLap(int lapNum, float startSessionTime,
                              int completedLapTimeMs) {
    if (lapNum <= 0) return;
    std::lock_guard<std::mutex> lock(impl_->stateMutex);
    if (impl_->current == lapNum) return;
    if (impl_->current != 0 && lapNum < impl_->current) return;

    if (impl_->current != 0) {
        auto completed = impl_->laps.find(impl_->current);
        if (completed != impl_->laps.end()) {
            std::lock_guard<std::mutex> lapLock(completed->second->mutex);
            completed->second->end = startSessionTime;
            completed->second->lapTimeMs = completedLapTimeMs;
        }
        impl_->previousPrevious = impl_->previous;
        impl_->previous = impl_->current;
        if (completedLapTimeMs > 0 &&
            (impl_->fastestMs == 0 || completedLapTimeMs < impl_->fastestMs)) {
            impl_->fastest = impl_->current;
            impl_->fastestMs = completedLapTimeMs;
        }
    }
    impl_->current = lapNum;
    auto& current = impl_->laps[lapNum];
    if (!current) {
        current = std::make_shared<LapSegment>();
        current->lapNum = lapNum;
        current->start = startSessionTime;
    }
    {
        std::lock_guard<std::mutex> lapLock(current->mutex);
        current->end = std::max(current->end, startSessionTime);
    }
    impl_->queueEligible();
}

void LiveHistoryStore::appendPacked(uint8_t type, float sessionTime,
                                    const uint8_t* data, size_t length) {
    if (!isPacked(type) || !data || length == 0) return;
    std::shared_ptr<LapSegment> lap;
    {
        std::lock_guard<std::mutex> lock(impl_->stateMutex);
        auto& slot = impl_->laps[impl_->current];
        if (!slot) {
            slot = std::make_shared<LapSegment>();
            slot->lapNum = impl_->current;
            slot->start = sessionTime;
        }
        lap = slot;
    }
    std::lock_guard<std::mutex> lock(lap->mutex);
    lap->end = std::max(lap->end, sessionTime);
    auto& family = lap->families[type];
    family.packed.insert(family.packed.end(), data, data + length);
}

void LiveHistoryStore::appendJson(uint8_t type, LiveHistoryJsonRow row) {
    if (type >= 16 || !row.json) return;
    std::shared_ptr<LapSegment> lap;
    {
        std::lock_guard<std::mutex> lock(impl_->stateMutex);
        auto& slot = impl_->laps[impl_->current];
        if (!slot) {
            slot = std::make_shared<LapSegment>();
            slot->lapNum = impl_->current;
            slot->start = row.sessionTime;
        }
        lap = slot;
    }
    std::lock_guard<std::mutex> lock(lap->mutex);
    lap->end = std::max(lap->end, row.sessionTime);
    auto& family = lap->families[type];
    family.jsonPayloadBytes += row.json->size();
    family.jsonPayloadCapacityBytes += row.json->capacity() + 1;
    family.json.push_back(std::move(row));
}

void LiveHistoryStore::rewind(float sessionTime) {
    std::lock_guard<std::mutex> lock(impl_->stateMutex);
    auto target = impl_->laps.end();
    for (auto it = impl_->laps.begin(); it != impl_->laps.end(); ++it)
        if (it->second->start <= sessionTime) target = it;
    if (target == impl_->laps.end()) return;

    const int targetLap = target->first;
    for (auto it = impl_->laps.upper_bound(targetLap); it != impl_->laps.end();)
        it = impl_->laps.erase(it);

    {
        std::lock_guard<std::mutex> lapLock(target->second->mutex);
        target->second->end = sessionTime;
        target->second->lapTimeMs = 0;
        for (uint8_t type = 1; type < 16; ++type) {
            auto& family = target->second->families[type];
            if (isPacked(type)) {
                std::vector<uint8_t> kept;
                (void)bin::forEachPackedRecord(
                    family.packed.data(), family.packed.size(),
                    [&](uint8_t, const uint8_t* record, size_t length) {
                        if (packedTime(record, length) <= sessionTime)
                            kept.insert(kept.end(), record, record + length);
                    });
                family.packed = std::move(kept);
            } else {
                family.json.erase(std::remove_if(family.json.begin(), family.json.end(),
                    [&](const auto& row) { return row.sessionTime > sessionTime; }),
                    family.json.end());
                refreshJsonUsage(family);
            }
        }
    }

    impl_->current = targetLap;
    auto previous = impl_->laps.lower_bound(targetLap);
    if (previous != impl_->laps.begin()) {
        --previous;
        impl_->previous = previous->first;
    } else {
        impl_->previous = 0;
    }
    impl_->previousPrevious = 0;
    if (impl_->fastest >= targetLap ||
        impl_->laps.find(impl_->fastest) == impl_->laps.end()) {
        impl_->fastest = 0;
        impl_->fastestMs = 0;
        for (const auto& [lapNum, lap] : impl_->laps) {
            if (lapNum == targetLap || lap->lapTimeMs <= 0) continue;
            if (impl_->fastestMs == 0 || lap->lapTimeMs < impl_->fastestMs) {
                impl_->fastest = lapNum;
                impl_->fastestMs = lap->lapTimeMs;
            }
        }
        const auto fastest = impl_->laps.find(impl_->fastest);
        if (fastest != impl_->laps.end())
            impl_->enqueue({Impl::JobKind::Decompress, fastest->second});
    }
    ++impl_->generation;
    impl_->queueEligible();
}

int LiveHistoryStore::currentLap() const {
    std::lock_guard<std::mutex> lock(impl_->stateMutex);
    return impl_->current;
}

float LiveHistoryStore::currentLapStart() const {
    std::lock_guard<std::mutex> lock(impl_->stateMutex);
    const auto it = impl_->laps.find(impl_->current);
    return it == impl_->laps.end() ? 0.0f : it->second->start;
}

LiveHistoryMemoryStats LiveHistoryStore::memoryStats() const {
    LiveHistoryMemoryStats stats;
    std::vector<std::shared_ptr<LapSegment>> laps;
    {
        std::lock_guard<std::mutex> lock(impl_->stateMutex);
        stats.lapCount = impl_->laps.size();
        for (const auto& [lapNum, lap] : impl_->laps) {
            if (impl_->pinned(lapNum)) ++stats.pinnedLapCount;
            laps.push_back(lap);
        }
    }
    for (const auto& lap : laps) {
        // Memory logging runs on Electron's main thread. Never wait for a
        // history compression/decompression job that currently owns this lap.
        std::unique_lock<std::mutex> lapLock(lap->mutex, std::try_to_lock);
        if (!lapLock.owns_lock()) {
            ++stats.busyLapCount;
            continue;
        }
        bool compressed = false;
        for (const auto& family : lap->families) {
            stats.packedBytes += family.packed.size();
            stats.packedCapacityBytes += family.packed.capacity();
            stats.jsonRows += family.json.size();
            stats.jsonContainerCapacityBytes +=
                family.json.capacity() * sizeof(LiveHistoryJsonRow);
            stats.jsonPayloadBytes += family.jsonPayloadBytes;
            stats.jsonPayloadCapacityBytes += family.jsonPayloadCapacityBytes;
            stats.sequenceEntries += family.sequences.size();
            stats.sequenceCapacityBytes +=
                family.sequences.capacity() * sizeof(uint64_t);
            stats.compressedPlainBytes += family.plainSize;
            stats.compressedBytes += family.compressed.size();
            stats.compressedCapacityBytes += family.compressed.capacity();
            compressed = compressed || !family.compressed.empty();
        }
        if (compressed) ++stats.compressedLapCount;
    }
    {
        std::lock_guard<std::mutex> lock(impl_->workMutex);
        stats.queuedJobs = impl_->jobs.size();
    }
    stats.activeJobKind = impl_->activeJobKind.load(std::memory_order_relaxed);
    stats.compressionJobs = impl_->compressionJobs.load(std::memory_order_relaxed);
    stats.compressedFamilies = impl_->compressedFamilies.load(std::memory_order_relaxed);
    stats.compressionPlainBytesProcessed =
        impl_->compressionPlainBytesProcessed.load(std::memory_order_relaxed);
    stats.compressionPlainBufferBytesAllocated =
        impl_->compressionPlainBufferBytesAllocated.load(std::memory_order_relaxed);
    stats.compressionBufferBytesAllocated =
        impl_->compressionBufferBytesAllocated.load(std::memory_order_relaxed);
    stats.compressedOutputBytesAllocated =
        impl_->compressedOutputBytesAllocated.load(std::memory_order_relaxed);
    stats.lastCompressionPlainBytes =
        impl_->lastCompressionPlainBytes.load(std::memory_order_relaxed);
    stats.lastCompressionBufferBytes =
        impl_->lastCompressionBufferBytes.load(std::memory_order_relaxed);
    stats.lastCompressionScratchBytes =
        impl_->lastCompressionScratchBytes.load(std::memory_order_relaxed);
    stats.peakCompressionPlainBytes =
        impl_->peakCompressionPlainBytes.load(std::memory_order_relaxed);
    stats.peakCompressionBufferBytes =
        impl_->peakCompressionBufferBytes.load(std::memory_order_relaxed);
    stats.peakCompressionScratchBytes =
        impl_->peakCompressionScratchBytes.load(std::memory_order_relaxed);
    stats.decompressionJobs = impl_->decompressionJobs.load(std::memory_order_relaxed);
    stats.decompressionBufferBytesAllocated =
        impl_->decompressionBufferBytesAllocated.load(std::memory_order_relaxed);
    stats.lastDecompressionBufferBytes =
        impl_->lastDecompressionBufferBytes.load(std::memory_order_relaxed);
    stats.peakDecompressionBufferBytes =
        impl_->peakDecompressionBufferBytes.load(std::memory_order_relaxed);
    stats.rangeJobs = impl_->rangeJobs.load(std::memory_order_relaxed);
    stats.retainedBytes = stats.packedCapacityBytes +
        stats.jsonPayloadCapacityBytes + stats.jsonContainerCapacityBytes +
        stats.sequenceCapacityBytes + stats.compressedCapacityBytes;
    return stats;
}

std::string LiveHistoryStore::latestJson(uint8_t type,
                                         float throughSessionTime) const {
    if (type >= 16) return {};
    std::lock_guard<std::mutex> lock(impl_->stateMutex);
    for (auto it = impl_->laps.rbegin(); it != impl_->laps.rend(); ++it) {
        std::lock_guard<std::mutex> lapLock(it->second->mutex);
        const auto& family = it->second->families[type];
        for (auto row = family.json.rbegin(); row != family.json.rend(); ++row)
            if (row->sessionTime <= throughSessionTime && row->json)
                return *row->json;
        if (!family.compressed.empty()) {
            std::vector<uint8_t> plain;
            if (!decompress(family, plain)) continue;
            std::string latest;
            appendJsonLines(plain.data(), plain.size(),
                            -std::numeric_limits<float>::infinity(),
                            throughSessionTime, latest);
            if (!latest.empty()) {
                if (latest.back() == '\n') latest.pop_back();
                const size_t newline = latest.rfind('\n');
                return newline == std::string::npos ? latest
                    : latest.substr(newline + 1);
            }
        }
    }
    return {};
}

void LiveHistoryStore::forEachStrategyRow(float throughSessionTime,
    const std::function<void(const LiveHistoryJsonRow&)>& visitor,
    const std::function<void(size_t, size_t, size_t)>& memory) const {
    std::vector<std::shared_ptr<LapSegment>> laps;
    {
        std::lock_guard<std::mutex> lock(impl_->stateMutex);
        for (const auto& [_, lap] : impl_->laps) laps.push_back(lap);
    }
    for (const auto& lap : laps) {
        std::vector<LiveHistoryJsonRow> rows;
        std::string ignored;
        size_t jsonBytes = 0, jsonCapacity = 0;
        const auto report = [&](size_t scratch = 0) {
            if (memory) memory(rows.size(), jsonBytes,
                rows.capacity() * sizeof(LiveHistoryJsonRow) + jsonCapacity + scratch);
        };
        {
            std::lock_guard<std::mutex> lock(lap->mutex);
            for (uint8_t type = 2; type <= 10; ++type) {
                if (!(kStrategyDependencyMask & (1u << type))) continue;
                const auto& family = lap->families[type];
                const size_t first = rows.size();
                std::vector<uint8_t> plain;
                if (family.compressed.empty()) {
                    for (const auto& row : family.json)
                        if (row.sessionTime <= throughSessionTime) rows.push_back(row);
                } else {
                    impl_->decompressionJobs.fetch_add(1, std::memory_order_relaxed);
                    impl_->decompressionBufferBytesAllocated.fetch_add(family.plainSize, std::memory_order_relaxed);
                    impl_->lastDecompressionBufferBytes.store(family.plainSize, std::memory_order_relaxed);
                    updateAtomicMaximum(impl_->peakDecompressionBufferBytes, family.plainSize);
                    report(family.plainSize);
                    if (decompress(family, plain))
                        appendJsonLines(plain.data(), plain.size(),
                                        -std::numeric_limits<float>::infinity(),
                                        throughSessionTime, ignored, &rows,
                                        &family.sequences);
                }
                for (size_t i = first; i < rows.size(); ++i) {
                    if (!rows[i].json) continue;
                    jsonBytes += rows[i].json->size();
                    jsonCapacity += rows[i].json->capacity() + 1;
                }
                report(plain.capacity());
            }
        }
        // Sequences are unique, so sort needs no separate stability buffer.
        std::sort(rows.begin(), rows.end(), [](const auto& left, const auto& right) {
            return left.sessionTime < right.sessionTime ||
                (left.sessionTime == right.sessionTime && left.sequence < right.sequence);
        });
        report();
        for (const auto& row : rows) visitor(row);
    }
    if (memory) memory(0, 0, 0);
}

void LiveHistoryStore::requestRange(uint32_t familyMask, float fromSessionTime,
                                    float throughSessionTime,
                                    BackfillCallback callback) {
    Impl::Job job;
    job.kind = Impl::JobKind::Range;
    job.mask = familyMask & kHistoricalMask;
    job.from = fromSessionTime;
    job.through = throughSessionTime;
    job.callback = std::move(callback);
    {
        std::lock_guard<std::mutex> lock(impl_->stateMutex);
        job.generation = impl_->generation;
        for (const auto& [_, lap] : impl_->laps) job.laps.push_back(lap);
    }
    impl_->enqueue(std::move(job));
}

void LiveHistoryStore::requestFastestLap(
    std::function<void(int, int, float, float, LiveHistoryBackfill)> callback) {
    Impl::Job job;
    job.kind = Impl::JobKind::Range;
    job.mask = kHistoricalMask;
    {
        std::lock_guard<std::mutex> lock(impl_->stateMutex);
        std::shared_ptr<LapSegment> best;
        for (const auto& [num, lap] : impl_->laps) {
            if (num >= impl_->current || lap->lapTimeMs <= 0) continue;
            if (!best || lap->lapTimeMs < best->lapTimeMs) best = lap;
        }
        if (!best) return;
        job.generation = impl_->generation;
        job.laps.push_back(best);
        job.from = best->start;
        job.through = best->end;
        job.callback = [callback = std::move(callback), num = best->lapNum,
                        ms = best->lapTimeMs, start = best->start, end = best->end]
                       (LiveHistoryBackfill data) mutable {
            callback(num, ms, start, end, std::move(data));
        };
    }
    impl_->enqueue(std::move(job));
}

} // namespace tnrp::detail
