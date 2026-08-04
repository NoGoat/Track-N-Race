#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <glaze/glaze.hpp>

#include "tnrp/CardColors.h"

// Typed structs for the lower-frequency / control / playback rows that used to be
// built with nlohmann::json. Serialise with glaze, mirroring tnrp/rows.h.
//
// Member names map 1:1 to JSON keys via glaze reflection (no glz::meta needed),
// except ProtocolStatusRow whose "override" key can't be a bare member name.
//
// Nullable fields are std::optional<>. glaze's default skip_null_members=true
// OMITS empty optionals — which is what race_event wants (per-code keys). Rows
// that must emit explicit `null` (protocol_status / protocol_warning /
// playback_loaded) are written with writeJsonNullable() below.

namespace tnrp {

// ── write helpers ──────────────────────────────────────────────────────────

template <class T>
inline void writeJson(const T& v, std::string& buf) {
    buf.clear();
    (void)glz::write_json(v, buf);
}

template <class T>
inline std::string writeJson(const T& v) {
    std::string buf;
    (void)glz::write_json(v, buf);
    return buf;
}

// Emits empty optionals as explicit JSON null instead of omitting the key.
template <class T>
inline std::string writeJsonNullable(const T& v) {
    std::string buf;
    (void)glz::write<glz::opts{.skip_null_members = false}>(v, buf);
    return buf;
}

// ── session (≤2 Hz) ────────────────────────────────────────────────────────

struct MarshalZone {
    double zone_start{};
    int    flag{};
};

struct WeatherSample {
    int time_offset{};
    int weather{};
    int rain_percentage{};
};

struct SessionRow {
    std::string type{"session"};
    std::string ts;
    int weather{};
    int track_temp{};
    int air_temp{};
    int track_length_m{};
    int track_id{};
    int session_type{};
    int total_laps{};
    int session_time_left{};
    int session_duration{};
    int pit_speed_limit{};
    int pit_stop_window_ideal_lap{};
    int pit_stop_window_latest_lap{};
    int pit_stop_rejoin_position{};
    int num_marshal_zones{};
    std::vector<MarshalZone>   marshal_zones;
    std::vector<WeatherSample> weather_forecast_samples;
    int safety_car_status{};
    int forecast_accuracy{};
    int ai_difficulty{};
    int64_t time_of_day{};
    int num_safety_car_periods{};
    int num_virtual_sc_periods{};
    int num_red_flag_periods{};
    // 2026 active-aero (SLM) track status: 0 = Full, 1 = Partial, -1 = n/a
    // (F1 24/25). Selects which SLM zone set the map draws (dry vs wet).
    int active_aero_track_status{-1};
};

// ── participants (rare) ────────────────────────────────────────────────────

struct Driver {
    int         idx{};
    std::string name;
    int         team_id{};
    int         race_number{};
    bool        ai{};
    std::string livery_color;
};

struct ParticipantsRow {
    std::string         type{"participants"};
    std::vector<Driver> drivers;
};

// ── tyre_sets (rare) ───────────────────────────────────────────────────────

struct TyreSet {
    int  idx{};
    int  actual_compound{};
    int  visual_compound{};
    int  wear{};
    bool available{};
    int  recommended_session{};
    int  life_span{};
    int  usable_life{};
    int  lap_delta_ms{};
    bool fitted{};
};

struct TyreSetsRow {
    std::string          type{"tyre_sets"};
    std::string          ts;
    float                session_time{};
    std::vector<TyreSet> sets;
    int                  fitted_idx{};
};

// ── session_history / fastest_lap (rare) ───────────────────────────────────

struct SessionHistoryFastestRow {
    std::string type{"session_history_fastest"};
    std::string ts;
    int         car_idx{};
    int64_t     best_lap_time_ms{};
    std::optional<int> latest_lap_num;
    std::optional<int> latest_lap_time_ms;
};

struct FastestLapRow {
    std::string type{"fastest_lap"};
    std::string ts;
    int         car_idx{};
    float       lap_time_s{};
};

// ── race_event (rare) — optionals omitted per code (default skip_null_members) ─

struct RaceEventRow {
    std::string         type{"race_event"};
    std::string         ts;
    float               session_time{};
    std::string         code;
    std::optional<int>   car_idx;
    std::optional<float> lap_time_s;
    std::optional<int>   safety_car_type;
    std::optional<int>   event_type;
    std::optional<int>   penalty_type;
    std::optional<int>   infringement_type;
    std::optional<int>   penalty_time_s;
};

// ── protocol_status / protocol_warning (control) — emit explicit null ───────

struct Capabilities {
    std::optional<int> gameYear;
    bool hasBlisters{};
    bool hasLiveryColors{};
    bool hasLapPositions{};
    bool hasMguh{};
};

struct ProtocolStatusRow {
    std::string        type{"protocol_status"};
    std::optional<int> detected_format;
    std::optional<int> active_format;
    std::string        override_;   // mapped to "override" below
    Capabilities       capabilities;
    // Library-owned i18n label catalog for the active format (see tnrp/Labels.h).
    // The renderer caches this and resolves every UI label through it, so labels
    // re-theme live when the format changes.
    std::map<std::string, std::string> labels;
    // Library-owned declarative card-colour spec (format-independent; see
    // tnrp/CardColors.h). Shipped here so the renderer shares one colour model.
    std::map<std::string, ColorSpec> cardColors;
    // Overtaking-aid mode for the active format: "drs" (F1 24/25) or "slm"
    // (F1 26). Selects the track-map overlay (see tnrp/AeroMode.h).
    std::string aero_mode;
};

struct ProtocolWarningRow {
    std::string        type{"protocol_warning"};
    std::optional<int> detected_format;
    std::optional<int> forced_format;
};

// Emitted by the asynchronous recording disk thread when a .tnrd stream cannot
// be opened, written, flushed, finalized, or repaired after a flashback. Hosts
// must surface this independently of recording intent: logging can remain
// enabled even though no usable output stream exists.
struct RecordingErrorRow {
    std::string type{"recording_error"};
    std::string operation;
    std::string message;
    std::string path;
};

// ── TNRD file header (writer emits, reader parses) ─────────────────────────

struct HeaderRow {
    std::string magic;
    std::optional<std::string> compression;  // V2/V3: "zstd"; omitted by V1
    int         protocol{};
    int         track_id{};
    std::string track_name;
    std::optional<int> track_length_m;         // V3; omitted by V1/V2
    int         session_type{};
    std::string session_name;
    int64_t     start_time{};
};

// ── playback control messages (Engine) ────────────────────────────────────

struct PlaybackLoadedRow {
    std::string              type{"playback_loaded"};
    bool                     ok{};
    std::optional<HeaderRow> header;   // null on failure → writeJsonNullable
};

struct PlaybackSeekFlushRow {
    std::string type{"playback_seek_flush"};
    float       currentLapStart{};
    int         lapNum{};
};

struct PlaybackStateRow {
    std::string type{"playback_state"};
    bool        playing{};
    float       current_time{};
    float       total_time{};
    float       speed{};
    // Absolute session_time of the recording's first row. current_time is
    // relative to it; consumers needing the absolute cursor add the two.
    float       start_time{};
};

struct TypeOnlyRow {
    std::string type;
};

// ── playback payloads built by TnrdReader (raw rows embedded verbatim) ─────

// Slim per-lap chart points for the renderer's Speed/RPM/ERS comparison view.
// Only populated when the reader runs with binary playback enabled (the
// Electron path); empty arrays otherwise — additive for JSON-only consumers.
struct SlimTelemetryPoint {
    std::string type{"telemetry"};
    float       session_time{};
    int         speed_kph{};
    int         rpm{};
};

struct SlimStatusPoint {
    std::string type{"status"};
    float       session_time{};
    double      ers_pct{};
    int         tyre_compound{};
    int         visual_compound{};
};

struct LapBlockMeta {
    int   lapNum{};
    float startSessionTime{};
    float endSessionTime{};
    std::vector<SlimTelemetryPoint> telemetry;
    std::vector<SlimStatusPoint>    statusHistory;
};

// V3-only player progress samples. current_lap_ms and lap_distance_m originate
// in the same menu-rate Lap Data packet, so interpolating between these points
// yields elapsed time at a common physical position around the circuit.
struct LapProgressPoint {
    float session_time{};
    int   current_lap_ms{};
    float lap_distance_m{};
};

struct LapMeta {
    int lapNum{};
    int lapTimeMs{};
};

struct PlaybackLapBlocksRow {
    std::string                type{"playback_lap_blocks"};
    std::vector<LapBlockMeta>  blocks;
    int                        fastestLapNum{};
    double                     initialFuelKg{-1.0};
    std::vector<glz::raw_json> events;
    std::vector<LapMeta>       laps;
    std::string                tnrdVersion;
    bool                       deltaAvailable{};
    int                        trackLengthM{};
};

struct PlaybackLapDataRow {
    std::string                type{"playback_lap_data"};
    int                        lapNum{};
    float                      startSessionTime{};
    float                      endSessionTime{};
    std::vector<glz::raw_json> telemetry;
    std::vector<glz::raw_json> statusHistory;
    std::vector<glz::raw_json> motionHistory;
    std::vector<glz::raw_json> motionExHistory;
    std::vector<glz::raw_json> damageHistory;
    std::vector<LapProgressPoint> lapProgress;
};

} // namespace tnrp

// ProtocolStatusRow needs an explicit mapping because the JSON key "override"
// cannot be a bare C++ member name.
template <>
struct glz::meta<tnrp::ProtocolStatusRow> {
    using T = tnrp::ProtocolStatusRow;
    static constexpr auto value = glz::object(
        "type",            &T::type,
        "detected_format", &T::detected_format,
        "active_format",   &T::active_format,
        "override",        &T::override_,
        "capabilities",    &T::capabilities,
        "labels",          &T::labels,
        "cardColors",      &T::cardColors,
        "aero_mode",       &T::aero_mode
    );
};
