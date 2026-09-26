#include "PairLapData.h"

#include "tnrp/control_rows.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>

namespace tnrp {
namespace pair_lap_data {

// The playback_lap_data members a phone can ask for. glaze reflects this type,
// and MSVC cannot reflect one with internal linkage, so it sits outside the
// anonymous namespace below.
struct PairLapSource {
    int lapNum{};
    float startSessionTime{};
    float endSessionTime{};
    std::vector<glz::raw_json> telemetry;
    std::vector<glz::raw_json> statusHistory;
    std::vector<glz::raw_json> damageHistory;
    std::vector<glz::raw_json> motionHistory;
    std::vector<glz::raw_json> motionExHistory;
    std::vector<LapProgressPoint> lapProgress;
    std::vector<PlayerPositionPoint> playerPositions;
};

} // namespace pair_lap_data

namespace {

using pair_lap_data::PairLapSource;

constexpr glz::opts kPartialRead{.null_terminated = false, .error_on_unknown_keys = false};

// The playback_lap_data members a phone can ask for, keyed by the family name
// used in its channels. Row type ids match libtnrp's row families.
struct FamilySpec {
    std::string_view name;
    uint8_t rowType;
};
constexpr FamilySpec kFamilies[] = {
    {"telemetry", 1}, {"status", 2}, {"damage", 3}, {"motion", 11}, {"motion_ex", 12},
};
constexpr uint8_t kLapRowType = 4;
constexpr uint8_t kPositionsRowType = 13;

const std::vector<glz::raw_json>& rowsOf(const PairLapSource& source, std::string_view family) {
    if (family == "telemetry") return source.telemetry;
    if (family == "status") return source.statusHistory;
    if (family == "damage") return source.damageHistory;
    if (family == "motion") return source.motionHistory;
    return source.motionExHistory;
}

const FamilySpec* familyOf(std::string_view name) {
    for (const auto& family : kFamilies)
        if (family.name == name) return &family;
    return nullptr;
}

// "family.field" -> family name and field, or false for a malformed channel.
bool splitChannel(std::string_view channel, std::string_view& family, std::string_view& field) {
    const size_t dot = channel.find('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 >= channel.size()) return false;
    family = channel.substr(0, dot);
    field = channel.substr(dot + 1);
    // Field names are written back as JSON keys, so only plain row keys pass.
    for (const char c : field)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return familyOf(family) != nullptr;
}

void appendNumber(std::string& out, double value, const char* format) {
    if (!std::isfinite(value)) {
        out += "null";
        return;
    }
    char buffer[32];
    const int length = std::snprintf(buffer, sizeof(buffer), format, value);
    if (length > 0) out.append(buffer, static_cast<size_t>(length));
    else out += "null";
}

template <class Values, class Read>
void appendArray(std::string& out, std::string_view key, const Values& values,
                 const char* format, Read read) {
    out += '"';
    out += key;
    out += "\":[";
    bool first = true;
    for (const auto& value : values) {
        if (!first) out += ',';
        first = false;
        appendNumber(out, read(value), format);
    }
    out += ']';
}

// One family's samples at their distinct timestamps, with each requested field
// carried forward across V6 patches exactly as the desktop's column store does.
struct FamilyColumns {
    std::vector<double> times;
    std::vector<std::string> fields;
    std::vector<std::vector<double>> values;
};

double numberOf(const glz::generic& value) {
    if (value.is_number()) return value.get_number();
    if (value.is_boolean()) return value.get_boolean() ? 1.0 : 0.0;
    return std::numeric_limits<double>::quiet_NaN();
}

FamilyColumns collectFamily(const std::vector<glz::raw_json>& rows,
                            std::vector<std::string> fields, double origin) {
    FamilyColumns out;
    out.fields = std::move(fields);
    out.values.resize(out.fields.size());
    constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
    std::vector<double> state(out.fields.size(), kNaN);
    // The V6 type whose patch last wrote each field: an unavailable patch of
    // that type clears exactly the fields it would have carried.
    std::vector<int> owner(out.fields.size(), 0);
    glz::generic row;
    for (const auto& raw : rows) {
        if (glz::read_json(row, raw.str) || !row.is_object()) continue;
        const auto& object = row.get_object();
        const auto time = object.find("session_time");
        if (time == object.end() || !time->second.is_number()) continue;
        const double sessionTime = time->second.get_number();
        const auto v6 = object.find("_v6_type");
        const int v6Type = v6 != object.end() && v6->second.is_number()
            ? static_cast<int>(v6->second.get_number()) : 0;
        const auto available = object.find("available");
        const bool unavailable = available != object.end() &&
            available->second.is_boolean() && !available->second.get_boolean();

        for (size_t index = 0; index < out.fields.size(); ++index) {
            if (unavailable) {
                if (v6Type == 0 || owner[index] == v6Type) state[index] = kNaN;
                continue;
            }
            const auto field = object.find(out.fields[index]);
            if (field != object.end()) {
                state[index] = numberOf(field->second);
                if (v6Type != 0) owner[index] = v6Type;
            } else if (v6Type == 0) {
                // A complete row that omits a field does not carry it.
                state[index] = kNaN;
            }
        }

        // Several rows can share a timestamp; the last one describes it.
        const double relative = sessionTime - origin;
        if (!out.times.empty() && out.times.back() == relative) {
            for (size_t index = 0; index < state.size(); ++index) out.values[index].back() = state[index];
            continue;
        }
        if (!out.times.empty() && relative < out.times.back()) continue;
        out.times.push_back(relative);
        for (size_t index = 0; index < state.size(); ++index) out.values[index].push_back(state[index]);
    }
    return out;
}

} // namespace

uint32_t pairLapDataRowMask(const std::vector<std::string>& channels) {
    uint32_t mask = (1u << kLapRowType) | (1u << kPositionsRowType);
    for (const auto& channel : channels) {
        std::string_view family;
        std::string_view field;
        if (splitChannel(channel, family, field)) mask |= 1u << familyOf(family)->rowType;
    }
    return mask;
}

std::string pairLapDataJson(std::string_view playbackLapData,
                            const std::vector<std::string>& channels) {
    PairLapSource source;
    if (playbackLapData.empty() || glz::read<kPartialRead>(source, playbackLapData)) return {};
    const double origin = source.startSessionTime;

    std::map<std::string_view, std::vector<std::string>> fieldsByFamily;
    for (const auto& channel : channels) {
        std::string_view family;
        std::string_view field;
        if (!splitChannel(channel, family, field)) continue;
        auto& fields = fieldsByFamily[family];
        if (std::find(fields.begin(), fields.end(), field) == fields.end()) fields.emplace_back(field);
    }

    std::string out;
    out.reserve(256 * 1024);
    out += "{\"lapNum\":" + std::to_string(source.lapNum);
    out += ",\"startSessionTime\":";
    appendNumber(out, source.startSessionTime, "%.3f");
    out += ",\"endSessionTime\":";
    appendNumber(out, source.endSessionTime, "%.3f");

    out += ",\"progress\":{";
    appendArray(out, "t", source.lapProgress, "%.3f",
        [&](const LapProgressPoint& point) { return point.session_time - origin; });
    out += ',';
    appendArray(out, "distance", source.lapProgress, "%.2f",
        [](const LapProgressPoint& point) { return point.lap_distance_m; });
    out += ',';
    appendArray(out, "elapsedMs", source.lapProgress, "%.0f",
        [](const LapProgressPoint& point) { return point.current_lap_ms; });
    out += ',';
    appendArray(out, "sector", source.lapProgress, "%.0f",
        [](const LapProgressPoint& point) { return point.sector; });
    out += ',';
    appendArray(out, "s1Ms", source.lapProgress, "%.0f",
        [](const LapProgressPoint& point) { return point.s1_ms; });
    out += ',';
    appendArray(out, "s2Ms", source.lapProgress, "%.0f",
        [](const LapProgressPoint& point) { return point.s2_ms; });
    out += '}';

    out += ",\"positions\":{";
    appendArray(out, "t", source.playerPositions, "%.3f",
        [&](const PlayerPositionPoint& point) { return point.session_time - origin; });
    out += ',';
    appendArray(out, "x", source.playerPositions, "%.2f",
        [](const PlayerPositionPoint& point) { return point.x; });
    out += ',';
    appendArray(out, "z", source.playerPositions, "%.2f",
        [](const PlayerPositionPoint& point) { return point.z; });
    out += '}';

    out += ",\"families\":{";
    bool firstFamily = true;
    for (auto& [family, fields] : fieldsByFamily) {
        const FamilyColumns columns = collectFamily(rowsOf(source, family), std::move(fields), origin);
        if (!firstFamily) out += ',';
        firstFamily = false;
        out += '"';
        out += family;
        out += "\":{";
        appendArray(out, "t", columns.times, "%.3f", [](double value) { return value; });
        out += ",\"fields\":{";
        for (size_t index = 0; index < columns.fields.size(); ++index) {
            if (index > 0) out += ',';
            appendArray(out, columns.fields[index], columns.values[index], "%.6g",
                [](double value) { return value; });
        }
        out += "}}";
    }
    out += "}}";
    return out;
}

} // namespace tnrp
