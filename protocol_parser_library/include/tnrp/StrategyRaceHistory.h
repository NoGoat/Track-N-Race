#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <set>
#include <utility>

namespace tnrp {

// Display-only completed laps. No telemetry, wear samples or rival state live
// here. Immutable prefixes are shared by live rollback and playback checkpoints;
// completing a lap allocates one record, not another copy of the race.
class StrategyRaceHistory {
public:
    // The game's lap numbers and race length are uint8 values.
    static constexpr int MAX_LAPS = 255;
    struct Lap {
        int lap_num{};
        int actual_ms{};
        std::array<double, 2> required_base_ms{}; // defend, attack; zero = unknown
    };

    int latestLap() const { return tail_ ? tail_->lap.lap_num : 0; }
    size_t size() const { return tail_ ? tail_->count : 0; }
    void reset() { tail_.reset(); }

    void complete(Lap lap) {
        if (lap.lap_num <= 0 || lap.lap_num > MAX_LAPS || lap.lap_num < latestLap()) return;
        auto previous = tail_;
        if (tail_ && lap.lap_num == latestLap()) {
            // Lap and timing rows can report the same finish. Preserve its
            // original target, and only replace a corrected actual time.
            if (lap.actual_ms <= 0 || lap.actual_ms == tail_->lap.actual_ms) return;
            lap.required_base_ms = tail_->lap.required_base_ms;
            previous = tail_->previous;
        }
        const size_t count = previous ? previous->count + 1 : 1;
        tail_ = std::make_shared<const Node>(Node{lap, std::move(previous), count});
    }

    const Lap* find(int lap) const {
        for (auto node = tail_.get(); node && node->lap.lap_num >= lap; node = node->previous.get())
            if (node->lap.lap_num == lap) return &node->lap;
        return nullptr;
    }

    std::array<const Lap*, MAX_LAPS + 1> laps() const {
        std::array<const Lap*, MAX_LAPS + 1> result{};
        for (auto node = tail_.get(); node; node = node->previous.get())
            result[node->lap.lap_num] = &node->lap;
        return result;
    }

    size_t retainedBytes() const { return size() * allocationBytes(); }
    size_t retainedBytes(std::set<const void*>& seen) const {
        size_t bytes = 0;
        for (auto node = tail_.get(); node && seen.insert(node).second; node = node->previous.get())
            bytes += allocationBytes();
        return bytes;
    }

private:
    struct Node {
        Lap lap;
        std::shared_ptr<const Node> previous;
        size_t count;
    };
    // Include an estimate of the make_shared control block.
    static constexpr size_t allocationBytes() { return sizeof(Node) + 2 * sizeof(void*); }
    std::shared_ptr<const Node> tail_;
};

} // namespace tnrp
