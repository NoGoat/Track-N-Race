#include "tnrp/AnyRow.h"

namespace tnrp {

namespace {

// Same read tolerance the reader's partial scans use: buffer is a string_view
// (not null-terminated) and unknown keys are skipped, so structs only pull the
// fields they declare and missing keys keep their defaults.
constexpr glz::opts kRowRead{ .null_terminated = false, .error_on_unknown_keys = false };

// Value of the top-level "type" key, or empty when absent/malformed.
std::string_view typeOf(std::string_view s) {
    constexpr std::string_view KEY = "\"type\":\"";
    const size_t k = s.find(KEY);
    if (k == std::string_view::npos) return {};
    const size_t vs = k + KEY.size();
    const size_t ve = s.find('"', vs);
    if (ve == std::string_view::npos) return {};
    return s.substr(vs, ve - vs);
}

template <class T>
std::optional<AnyRow> readAs(std::string_view s) {
    T row{};
    if (glz::read<kRowRead>(row, s)) return std::nullopt;
    return AnyRow{ std::move(row) };
}

} // namespace

std::optional<AnyRow> parseRow(std::string_view jsonl) {
    const std::string_view t = typeOf(jsonl);
    if (t.empty()) return std::nullopt;

    if (t == "telemetry")               return readAs<TelemetryRow>(jsonl);
    if (t == "motion")                  return readAs<MotionRow>(jsonl);
    if (t == "motion_ex")               return readAs<MotionExRow>(jsonl);
    if (t == "positions")               return readAs<PositionsRow>(jsonl);
    if (t == "lap")                     return readAs<LapRow>(jsonl);
    if (t == "timing")                  return readAs<TimingRow>(jsonl);
    if (t == "status")                  return readAs<StatusRow>(jsonl);
    if (t == "all_status")              return readAs<AllStatusRow>(jsonl);
    if (t == "damage")                  return readAs<DamageRow>(jsonl);
    if (t == "session")                 return readAs<SessionRow>(jsonl);
    if (t == "participants")            return readAs<ParticipantsRow>(jsonl);
    if (t == "tyre_sets")               return readAs<TyreSetsRow>(jsonl);
    if (t == "race_event")              return readAs<RaceEventRow>(jsonl);
    if (t == "fastest_lap")             return readAs<FastestLapRow>(jsonl);
    if (t == "session_history_fastest") return readAs<SessionHistoryFastestRow>(jsonl);
    if (t == "protocol_status")         return readAs<ProtocolStatusRow>(jsonl);
    if (t == "protocol_warning")        return readAs<ProtocolWarningRow>(jsonl);
    return std::nullopt;
}

const std::string& rowType(const AnyRow& row) {
    return std::visit([](const auto& r) -> const std::string& { return r.type; }, row);
}

float rowSessionTime(const AnyRow& row) {
    return std::visit([](const auto& r) -> float {
        if constexpr (requires { r.session_time; }) return (float)r.session_time;
        else return -1.0f;
    }, row);
}

} // namespace tnrp
