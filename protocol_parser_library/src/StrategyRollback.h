#pragma once

#include "tnrp/Strategy.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <memory>
#include <string>
#include <utility>

namespace tnrp::detail {

// Owned exclusively by the Strategy worker. Checkpoints contain the reducer,
// not historical JSON. The journal holds only the recent inputs actually
// consumed by that reducer, including snapshot()'s decision-state mutations.
class StrategyRollback {
public:
    static constexpr float WINDOW_SECONDS = 30.0f;
    static constexpr size_t MAX_BYTES = 32u * 1024u * 1024u;
    struct Stats {
        size_t retainedBytes{}, checkpoints{}, rows{};
        uint64_t rollbacks{}, replayedRows{}, fallbacks{};
    };

    StrategyProcessor& processor() { return processor_; }
    const StrategyProcessor& processor() const { return processor_; }

    // Settings are not telemetry. Save their resulting state before any later
    // snapshot mutates decisions, so replay never applies those decisions with
    // the configuration from an older checkpoint.
    void configurationChanged() {
        if (!checkpoints_.empty()) checkpoint();
        trim();
    }

    void reset() {
        processor_.reset();
        checkpoints_.clear();
        journal_.clear();
        journalBytes_ = checkpointBytes_ = 0;
        time_ = -std::numeric_limits<float>::infinity();
        nextOrdinal_ = 0;
    }

    void ingest(float time, std::shared_ptr<const std::string> json) {
        if (!json) return;
        // An initial state is saved after the caller applies configuration.
        if (checkpoints_.empty()) checkpoint();
        else if (std::isfinite(time) && time > time_ &&
            time >= checkpoints_.back().time + 1.0f) checkpoint();
        if (std::isfinite(time)) time_ = std::max(time_, time);
        processor_.ingestJson(*json);
        Entry entry{time_, nextOrdinal_++, std::move(json)};
        journalBytes_ += entry.bytes();
        journal_.push_back(std::move(entry));
        trim();
    }

    std::string snapshotJson() {
        auto result = processor_.snapshotJson();
        // A null JSON pointer is a snapshot operation, not a telemetry row.
        journal_.push_back({time_, nextOrdinal_++, {}});
        journalBytes_ += sizeof(Entry);
        trim();
        return result;
    }

    // False means that the target predates the bounded journal. The caller can
    // reconstruct exceptionally deep rewinds using a streaming history reader.
    bool rollback(float target) {
        ++stats_.rollbacks;
        if (!std::isfinite(target)) {
            ++stats_.fallbacks;
            return false;
        }
        auto selected = checkpoints_.end();
        for (auto it = checkpoints_.begin(); it != checkpoints_.end(); ++it)
            if (it->time <= target) selected = it;
        if (selected == checkpoints_.end()) {
            ++stats_.fallbacks;
            return false;
        }
        processor_ = selected->processor;
        for (const auto& entry : journal_) {
            if (entry.ordinal < selected->ordinal || entry.time > target) continue;
            if (entry.json) {
                processor_.ingestJson(*entry.json);
                ++stats_.replayedRows;
            } else {
                (void)processor_.snapshot();
            }
        }
        while (!checkpoints_.empty() && checkpoints_.back().time > target) {
            checkpointBytes_ -= checkpoints_.back().bytes;
            checkpoints_.pop_back();
        }
        while (!journal_.empty() && journal_.back().time > target) {
            journalBytes_ -= journal_.back().bytes();
            journal_.pop_back();
        }
        time_ = target;
        return true;
    }

    Stats memoryStats() const {
        auto result = stats_;
        result.retainedBytes = journalBytes_ + checkpointBytes_;
        result.checkpoints = checkpoints_.size();
        result.rows = journal_.size();
        return result;
    }

private:
    struct Entry {
        float time;
        uint64_t ordinal;
        std::shared_ptr<const std::string> json;
        size_t bytes() const { return sizeof(Entry) + (json ? json->capacity() + 1 : 0); }
    };
    struct Checkpoint {
        float time;
        uint64_t ordinal;
        StrategyProcessor processor;
        size_t bytes;
    };
    void checkpoint() {
        // A malformed, oversized input must not turn the checkpoint cache into
        // unbounded storage. Such a state can still use the streaming fallback.
        if (sizeof(Checkpoint) + processor_.memoryStats().retainedBytes > MAX_BYTES)
            return;
        Checkpoint saved{time_, nextOrdinal_, processor_, 0};
        saved.bytes = sizeof(Checkpoint) + saved.processor.memoryStats().retainedBytes;
        checkpointBytes_ += saved.bytes;
        checkpoints_.push_back(std::move(saved));
    }
    void trim() {
        while (checkpoints_.size() > 1 &&
               (checkpoints_[1].time <= time_ - WINDOW_SECONDS ||
                journalBytes_ + checkpointBytes_ > MAX_BYTES)) {
            checkpointBytes_ -= checkpoints_.front().bytes;
            checkpoints_.pop_front();
            while (!journal_.empty() && journal_.front().ordinal < checkpoints_.front().ordinal) {
                journalBytes_ -= journal_.front().bytes();
                journal_.pop_front();
            }
        }
        // Also bound traffic at a stationary session clock (menus/pauses).
        if (journalBytes_ + checkpointBytes_ > MAX_BYTES) {
            checkpoints_.clear();
            journal_.clear();
            checkpointBytes_ = journalBytes_ = 0;
            checkpoint();
        }
    }
    StrategyProcessor processor_;
    std::deque<Checkpoint> checkpoints_;
    std::deque<Entry> journal_;
    float time_{-std::numeric_limits<float>::infinity()};
    uint64_t nextOrdinal_{};
    size_t journalBytes_{}, checkpointBytes_{};
    Stats stats_;
};

} // namespace tnrp::detail
