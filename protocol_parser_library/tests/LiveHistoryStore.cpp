#include "LiveHistoryStore.h"

#include "tnrp/BinaryRows.h"

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

} // namespace

int main() {
    tnrp::detail::LiveHistoryStore store;
    store.setLap(1, 0.0f);
    appendLap(store, 1, 1.0f, 1);
    store.setLap(2, 10.0f, 9000); // lap 1 is fastest
    appendLap(store, 2, 11.0f, 2);
    store.setLap(3, 20.0f, 10000);
    appendLap(store, 3, 21.0f, 3);
    store.setLap(4, 30.0f, 11000);
    appendLap(store, 4, 31.0f, 4);
    store.setLap(5, 40.0f, 12000); // lap 2 becomes compression-eligible
    appendLap(store, 5, 41.0f, 5);

    // Family-selective reads must not materialize unrelated families. Queue
    // ordering also ensures this runs after the eligible old lap is compressed.
    const auto statuses = request(store, 1u << 2, 0.0f, 50.0f);
    assert(!statuses.binary);
    assert(statuses.json.find("\"lap\":1") != std::string::npos);
    assert(statuses.json.find("\"lap\":5") != std::string::npos);

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

    const auto strategyRows = store.strategyRows(50.0f);
    assert(strategyRows.size() == 5);
    for (size_t i = 0; i < strategyRows.size(); ++i)
        assert(strategyRows[i].sequence == i + 1);

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
}
