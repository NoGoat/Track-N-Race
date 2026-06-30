#pragma once

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <deque>
#include <optional>
#include <vector>

// Live hot-row smoother — a C++ port of the Electron renderer's HotRowSmoother
// (src/main/binaryForwardFill.ts). UDP jitter and packet loss (e.g. a flaky Wi-Fi
// link) leave gaps in the 60 Hz telemetry stream, so the live charts — whose x-axis
// IS the telemetry session_time — stutter or freeze. On a tick with no fresh
// telemetry we re-emit the last telemetry/motion/motion_ex row with session_time
// nudged forward by exactly one measured frame, capped at one frame past the last
// REAL sample so a real packet always lands monotonically after a fill (no spurious
// flashback/rewind). Display-only: recording happens upstream in the engine and is
// unaffected by these synthetic rows.
//
// The frame period is measured from the real telemetry session_time deltas (median),
// so fills line up with the actual send rate (20/40/60 Hz or an fps-capped value).
class HotRowSmoother {
public:
    void reset() {
        lastTel_.reset(); lastMot_.reset(); lastMotEx_.reset();
        lastRealST_ = 0.0f; lastEmitST_ = 0.0f;
        periodS_ = kDefaultPeriodS; prevTelST_ = -1.0f; deltas_.clear();
        realSinceTick_ = false;
    }

    // Measured frame period in whole ms (>=1), for driving the fill timer's cadence.
    int periodMs() const { return std::max(1, (int)std::lround(periodS_ * 1000.0f)); }

    // ── Feed real hot rows as they arrive live ───────────────────────────────
    void onTelemetry(const nlohmann::json& row, float st) {
        // A large backward jump is a flashback / new session — drop the fill state
        // so we don't synthesise rows across the discontinuity.
        if (lastTel_ && st < lastRealST_ - 0.2f) {
            lastEmitST_ = st; prevTelST_ = -1.0f; deltas_.clear();
        }
        observe(st);
        lastTel_    = row;
        lastRealST_ = st;
        if (lastRealST_ > lastEmitST_) lastEmitST_ = lastRealST_;
        realSinceTick_ = true;
    }
    void onMotion(const nlohmann::json& row)   { lastMot_   = row; realSinceTick_ = true; }
    void onMotionEx(const nlohmann::json& row) { lastMotEx_ = row; realSinceTick_ = true; }

    // ── Timer tick: forward-fill rows (0..3) when no real telemetry arrived ──
    std::vector<nlohmann::json> tick() {
        // Real data already flowed through the live path this interval — no fill.
        if (realSinceTick_) { realSinceTick_ = false; return {}; }
        if (!lastTel_) return {};

        // Hold the last sample, advance by one frame, capped at one frame past the
        // last real sample so real data stays monotonic.
        const float target = std::min(lastEmitST_ + periodS_, lastRealST_ + periodS_);
        if (target <= lastEmitST_ + 1e-6f) return {};
        lastEmitST_ = target;

        std::vector<nlohmann::json> out;
        out.push_back(withTime(*lastTel_, target));
        if (lastMot_)   out.push_back(withTime(*lastMot_, target));
        if (lastMotEx_) out.push_back(withTime(*lastMotEx_, target));
        return out;
    }

private:
    static constexpr float kDefaultPeriodS = 1.0f / 60.0f;  // bootstrap until measured
    static constexpr int   kWindow     = 24;     // deltas kept for the median estimate
    static constexpr int   kMinSamples = 8;      // before trusting the measured period
    static constexpr float kMinDeltaS  = 0.005f; // 200 Hz ceiling — reject sub-frame noise
    static constexpr float kMaxDeltaS  = 0.2f;   // 5 Hz floor — reject pauses/gaps as frames
    static constexpr float kChangeEpsS = 5e-5f;  // ignore rounding jitter, track real changes

    static nlohmann::json withTime(nlohmann::json row, float t) {
        row["session_time"] = t;
        return row;
    }

    void observe(float st) {
        if (prevTelST_ >= 0.0f) {
            const float d = st - prevTelST_;
            if (d >= kMinDeltaS && d <= kMaxDeltaS) {
                deltas_.push_back(d);
                if ((int)deltas_.size() > kWindow) deltas_.pop_front();
            }
        }
        prevTelST_ = st;
        if ((int)deltas_.size() >= kMinSamples) {
            std::vector<float> s(deltas_.begin(), deltas_.end());
            std::sort(s.begin(), s.end());
            const size_t n = s.size();
            const float m = (n % 2) ? s[n / 2] : (s[n / 2 - 1] + s[n / 2]) / 2.0f;
            const float candidate = std::round(m * 1e5f) / 1e5f;   // 0.01 ms granularity
            if (std::fabs(candidate - periodS_) > kChangeEpsS) periodS_ = candidate;
        }
    }

    std::optional<nlohmann::json> lastTel_, lastMot_, lastMotEx_;
    float lastRealST_ = 0.0f, lastEmitST_ = 0.0f;
    float periodS_    = kDefaultPeriodS;
    float prevTelST_  = -1.0f;
    std::deque<float> deltas_;
    bool  realSinceTick_ = false;
};
