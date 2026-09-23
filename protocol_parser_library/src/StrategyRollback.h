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
// consumed by that reducer; snapshot() does not mutate it.
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
    // input commits decisions, so replay never applies those inputs with the
    // configuration from an older checkpoint.
    void configurationChanged() {
        if (!checkpoints_.empty()) checkpoint();
        trim();
    }

    void reset() {
        processor_.reset();
        checkpoints_.clear();
        journal_.clear();
        journalBytes_ = checkpointBytes_ = checkpointHistoryUpperBound_ = 0;
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

    std::string snapshotJson() { return processor_.snapshotJson(); }

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
            processor_.ingestJson(*entry.json);
            ++stats_.replayedRows;
        }
        while (!checkpoints_.empty() && checkpoints_.back().time > target) {
            checkpointBytes_ -= checkpoints_.back().bytes;
            checkpointHistoryUpperBound_ -= checkpoints_.back().processor.displayHistory().retainedBytes();
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
        // The active processor accounts for its own shared prefix. Only charge
        // additional checkpoint branches here, so engine totals do not count
        // the same completed lap once per checkpoint.
        result.retainedBytes = journalBytes_ + checkpointBytes_ +
            sharedHistoryBytes() - processor_.displayHistory().retainedBytes();
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
        const auto memory = processor_.memoryStats();
        if (sizeof(Checkpoint) + memory.retainedBytes > MAX_BYTES)
            return;
        Checkpoint saved{time_, nextOrdinal_, processor_, 0};
        const auto copiedMemory = saved.processor.memoryStats();
        saved.bytes = sizeof(Checkpoint) + copiedMemory.retainedBytes - copiedMemory.displayHistoryBytes;
        checkpointBytes_ += saved.bytes;
        checkpointHistoryUpperBound_ += copiedMemory.displayHistoryBytes;
        checkpoints_.push_back(std::move(saved));
    }
    void trim() {
        while (checkpoints_.size() > 1 &&
               (checkpoints_[1].time <= time_ - WINDOW_SECONDS ||
                retainedUpperBound() > MAX_BYTES)) {
            checkpointBytes_ -= checkpoints_.front().bytes;
            checkpointHistoryUpperBound_ -= checkpoints_.front().processor.displayHistory().retainedBytes();
            checkpoints_.pop_front();
            while (!journal_.empty() && journal_.front().ordinal < checkpoints_.front().ordinal) {
                journalBytes_ -= journal_.front().bytes();
                journal_.pop_front();
            }
        }
        // Also bound traffic at a stationary session clock (menus/pauses).
        if (retainedUpperBound() > MAX_BYTES) {
            checkpoints_.clear();
            journal_.clear();
            checkpointBytes_ = journalBytes_ = checkpointHistoryUpperBound_ = 0;
            checkpoint();
        }
    }
    size_t retainedUpperBound() const {
        // Allocation-free budget check on each input. The diagnostic below
        // deduplicates prefixes; this conservative bound avoids constructing a
        // set of every historical lap on the telemetry ingestion path.
        return journalBytes_ + checkpointBytes_ + checkpointHistoryUpperBound_ +
            processor_.displayHistory().retainedBytes();
    }
    size_t sharedHistoryBytes() const {
        std::set<const void*> seen;
        size_t bytes = processor_.displayHistory().retainedBytes(seen);
        for (const auto& saved : checkpoints_)
            bytes += saved.processor.displayHistory().retainedBytes(seen);
        return bytes;
    }
    StrategyProcessor processor_;
    std::deque<Checkpoint> checkpoints_;
    std::deque<Entry> journal_;
    float time_{-std::numeric_limits<float>::infinity()};
    uint64_t nextOrdinal_{};
    size_t journalBytes_{}, checkpointBytes_{}, checkpointHistoryUpperBound_{};
    Stats stats_;
};

} // namespace tnrp::detail
