#include "LiveHistoryStore.h"

#include "tnrp/BinaryRows.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

tnrp::detail::LiveHistoryBackfill request(
    tnrp::detail::LiveHistoryStore& store, uint32_t mask,
    float from, float through) {
    std::mutex mutex;
    std::condition_variable ready;
    bool done = false;
    tnrp::detail::LiveHistoryBackfill result;
    store.requestRange(mask, from, through,
        [&](tnrp::detail::LiveHistoryBackfill value) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                result = std::move(value);
                done = true;
            }
            ready.notify_one();
        });
    std::unique_lock<std::mutex> lock(mutex);
    assert(ready.wait_for(lock, std::chrono::seconds(5), [&] { return done; }));
    return result;
}

void appendLap(tnrp::detail::LiveHistoryStore& store, int lap, float time,
               uint64_t sequence) {
    TelemetryRow telemetry;
    telemetry.session_time = time;
    std::vector<uint8_t> packed;
    tnrp::bin::encodeTelemetry(packed, telemetry);
    store.appendPacked(1, time, packed.data(), packed.size());

    const auto json = std::make_shared<const std::string>(
        "{\"type\":\"status\",\"session_time\":" +
        std::to_string(time) + ",\"lap\":" + std::to_string(lap) + "}");
    store.appendJson(2, {time, sequence, json});
}

void expectFastest(tnrp::detail::LiveHistoryStore& store, int expectedLap,
                   int expectedMs) {
    std::mutex mutex;
    std::condition_variable ready;
    bool done = false;
    store.requestFastestLap([&](int lap, int ms, float start, float end,
                               tnrp::detail::LiveHistoryBackfill data) {
        assert(lap == expectedLap);
        assert(ms == expectedMs);
        assert(end > start);
        assert(data.binary && !data.binary->empty());
        assert(data.json.find("\"lap\":" + std::to_string(expectedLap)) != std::string::npos);
        assert(data.json.find("\"type\":\"race_event\"") == std::string::npos);
        size_t count = 0;
        assert(tnrp::bin::forEachPackedRecord(data.binary->data(), data.binary->size(),
            [&](uint8_t, const uint8_t*, size_t) { ++count; }));
        assert(count == 1); // Only the chosen lap, not surrounding history.
        {
            std::lock_guard<std::mutex> lock(mutex);
            done = true;
        }
        ready.notify_one();
    });
    std::unique_lock<std::mutex> lock(mutex);
    assert(ready.wait_for(lock, std::chrono::seconds(5), [&] { return done; }));
}

} // namespace

int main() {
    tnrp::detail::LiveHistoryStore store;
    store.setLap(1, 0.0f);
    appendLap(store, 1, 1.0f, 1);
    store.appendJson(6, {2.0f, 2, std::make_shared<const std::string>(
        R"({"type":"race_event","session_time":2,"code":"RTMT","car_idx":7})")});
    store.setLap(2, 10.0f, 9000); // lap 1 is fastest
    appendLap(store, 2, 11.0f, 3);
    store.setLap(3, 20.0f, 10000);
    appendLap(store, 3, 21.0f, 4);
    store.setLap(4, 30.0f, 11000);
    appendLap(store, 4, 31.0f, 5);
    store.setLap(5, 40.0f, 12000); // lap 2 becomes compression-eligible
    appendLap(store, 5, 41.0f, 6);

    // Family-selective reads must not materialize unrelated families. Queue
    // ordering also ensures this runs after the eligible old lap is compressed.
    const auto statuses = request(store, 1u << 2, 0.0f, 50.0f);
    assert(!statuses.binary);
    assert(statuses.json.find("\"lap\":1") != std::string::npos);
    assert(statuses.json.find("\"lap\":5") != std::string::npos);
    const auto events = request(store, 1u << 6, 0.0f, 50.0f);
    assert(!events.binary);
    assert(events.json.find("\"code\":\"RTMT\"") != std::string::npos);

    // Historical families must retain only their compressed payload, not the
    // much larger ZSTD_compressBound workspace capacity.
    const auto memory = store.memoryStats();
    assert(memory.retainedBytes > 0);
    assert(memory.lapCount == 5);
    assert(memory.compressedLapCount == 1);
    assert(memory.pinnedLapCount == 4);
    assert(memory.compressedPlainBytes > memory.compressedBytes);
    assert(memory.compressedBytes > 0);
    assert(memory.compressedCapacityBytes == memory.compressedBytes);

    const auto telemetry = request(store, 1u << 1, 0.0f, 50.0f);
    assert(telemetry.json.empty());
    assert(telemetry.binary);
    size_t telemetryRows = 0;
    assert(tnrp::bin::forEachPackedRecord(
        telemetry.binary->data(), telemetry.binary->size(),
        [&](uint8_t type, const uint8_t*, size_t) {
            assert(type == 1);
            ++telemetryRows;
        }));
    assert(telemetryRows == 5);

    size_t strategyRows = 0, peakStrategyRows = 0;
    store.forEachStrategyRow(50.0f, [&](const auto& row) {
        assert(row.sequence == ++strategyRows);
    }, [&](size_t rows, size_t, size_t) {
        peakStrategyRows = std::max(peakStrategyRows, rows);
    });
    assert(strategyRows == 6);
    assert(peakStrategyRows == 2); // Never expand more than one lap together.
    strategyRows = 0;
    store.forEachStrategyRow(25.0f, [&](const auto& row) {
        ++strategyRows;
        assert(row.sessionTime <= 25.0f);
        assert(row.sequence == strategyRows);
    });
    assert(strategyRows == 4); // Includes compressed lap 2, excludes the future.

    // A one-boundary rewind promotes lap 4 to Current and lap 3 to Previous.
    // Previous-previous is deliberately left empty; no N-3 decode is needed.
    store.rewind(35.0f);
    assert(store.currentLap() == 4);
    assert(store.currentLapStart() == 30.0f);
    assert(store.latestJson(2, 35.0f).find("\"lap\":4") != std::string::npos);
    const auto rewound = request(store, (1u << 1) | (1u << 2), 0.0f, 35.0f);
    assert(rewound.json.find("\"lap\":5") == std::string::npos);
    size_t rewoundTelemetryRows = 0;
    assert(rewound.binary);
    assert(tnrp::bin::forEachPackedRecord(
        rewound.binary->data(), rewound.binary->size(),
        [&](uint8_t, const uint8_t*, size_t) { ++rewoundTelemetryRows; }));
    assert(rewoundTelemetryRows == 4);
    expectFastest(store, 1, 9000);

    // A newer fastest makes the old best eligible for compression. Rewinding
    // into that newer lap must recover the older completed best, not the
    // invalidated time or the current lap's partial data.
    store.reset();
    store.setLap(1, 0.0f);
    appendLap(store, 1, 1.0f, 1);
    store.setLap(2, 10.0f, 9000);
    appendLap(store, 2, 11.0f, 2);
    store.setLap(3, 20.0f, 10000);
    appendLap(store, 3, 21.0f, 3);
    store.setLap(4, 30.0f, 11000);
    appendLap(store, 4, 31.0f, 4);
    store.setLap(5, 40.0f, 8000);
    appendLap(store, 5, 41.0f, 5);
    request(store, 1u << 2, 0.0f, 50.0f); // Drain compression work.
    assert(store.memoryStats().compressedLapCount > 0);
    expectFastest(store, 4, 8000);
    store.rewind(35.0f);
    expectFastest(store, 1, 9000);
}
