#pragma once

#include <string>
#include <vector>
#include <glaze/glaze.hpp>

// Typed structs for all hot-path telemetry row types.
// glz::meta specialisations below map C++ member names to JSON keys.
// Serialise with: std::string buf; glz::write_json(row, buf);
//
// Complex / low-frequency packets (session, participants, race_event,
// session_history, tyre_sets) and the control / playback rows live in
// tnrp/control_rows.h — also glaze, just split out to keep this file focused
// on the 60 Hz hot path.

// ── telemetry (60 Hz) ──────────────────────────────────────────────────────

struct TelemetryRow {
    std::string type{"telemetry"};
    std::string ts;
    float       session_time{};
    int         speed_kph{};
    int         rpm{};
    int         gear{};
    float       throttle{};
    float       brake{};
    double      steering{};
    int         drs{};
    int         slm{};   // 2026 active-aero / "straight line mode" wing state (kept separate from drs)
    int         tyre_temp_surface_rl{};
    int         tyre_temp_surface_rr{};
    int         tyre_temp_surface_fl{};
    int         tyre_temp_surface_fr{};
    int         tyre_temp_inner_rl{};
    int         tyre_temp_inner_rr{};
    int         tyre_temp_inner_fl{};
    int         tyre_temp_inner_fr{};
    int         brake_temp_rl{};
    int         brake_temp_rr{};
    int         brake_temp_fl{};
    int         brake_temp_fr{};
    int         engine_temp{};
};

template <>
struct glz::meta<TelemetryRow> {
    using T = TelemetryRow;
    static constexpr auto value = glz::object(
        "type",                 &T::type,
        "ts",                   &T::ts,
        "session_time",         &T::session_time,
        "speed_kph",            &T::speed_kph,
        "rpm",                  &T::rpm,
        "gear",                 &T::gear,
        "throttle",             &T::throttle,
        "brake",                &T::brake,
        "steering",             &T::steering,
        "drs",                  &T::drs,
        "slm",                  &T::slm,
        "tyre_temp_surface_rl", &T::tyre_temp_surface_rl,
        "tyre_temp_surface_rr", &T::tyre_temp_surface_rr,
        "tyre_temp_surface_fl", &T::tyre_temp_surface_fl,
        "tyre_temp_surface_fr", &T::tyre_temp_surface_fr,
        "tyre_temp_inner_rl",   &T::tyre_temp_inner_rl,
        "tyre_temp_inner_rr",   &T::tyre_temp_inner_rr,
        "tyre_temp_inner_fl",   &T::tyre_temp_inner_fl,
        "tyre_temp_inner_fr",   &T::tyre_temp_inner_fr,
        "brake_temp_rl",        &T::brake_temp_rl,
        "brake_temp_rr",        &T::brake_temp_rr,
        "brake_temp_fl",        &T::brake_temp_fl,
        "brake_temp_fr",        &T::brake_temp_fr,
        "engine_temp",          &T::engine_temp
    );
};

// ── motion (60 Hz) ─────────────────────────────────────────────────────────

struct MotionRow {
    std::string type{"motion"};
    std::string ts;
    float       session_time{};
    double      g_lat{};
    double      g_long{};
    double      g_vert{};
};

template <>
struct glz::meta<MotionRow> {
    using T = MotionRow;
    static constexpr auto value = glz::object(
        "type",         &T::type,
        "ts",           &T::ts,
        "session_time", &T::session_time,
        "g_lat",        &T::g_lat,
        "g_long",       &T::g_long,
        "g_vert",       &T::g_vert
    );
};

// ── positions (60 Hz) ──────────────────────────────────────────────────────

struct PositionCar {
    int    idx{};
    double x{};
    double z{};
};

template <>
struct glz::meta<PositionCar> {
    using T = PositionCar;
    static constexpr auto value = glz::object(
        "idx", &T::idx,
        "x",   &T::x,
        "z",   &T::z
    );
};

struct PositionsRow {
    std::string              type{"positions"};
    std::string              ts;
    int                      player_idx{};
    std::vector<PositionCar> cars;
};

template <>
struct glz::meta<PositionsRow> {
    using T = PositionsRow;
    static constexpr auto value = glz::object(
        "type",       &T::type,
        "ts",         &T::ts,
        "player_idx", &T::player_idx,
        "cars",       &T::cars
    );
};

// ── motion_ex (60 Hz) ──────────────────────────────────────────────────────

struct MotionExRow {
    std::string type{"motion_ex"};
    std::string ts;
    float       session_time{};
    double      front_aero_height_mm{};
    double      rear_aero_height_mm{};
};

template <>
struct glz::meta<MotionExRow> {
    using T = MotionExRow;
    static constexpr auto value = glz::object(
        "type",                 &T::type,
        "ts",                   &T::ts,
        "session_time",         &T::session_time,
        "front_aero_height_mm", &T::front_aero_height_mm,
        "rear_aero_height_mm",  &T::rear_aero_height_mm
    );
};

// ── lap (2 Hz) ─────────────────────────────────────────────────────────────

struct LapRow {
    std::string type{"lap"};
    std::string ts;
    float       session_time{};
    int         last_lap_ms{};
    int         current_lap_ms{};
    int         s1_ms{};
    int         s2_ms{};
    int         position{};
    int         lap_num{};
    int         pit_status{};
    int         num_pit_stops{};
    int         sector{};
    bool        lap_invalid{};
    int         penalties_s{};
};

template <>
struct glz::meta<LapRow> {
    using T = LapRow;
    static constexpr auto value = glz::object(
        "type",           &T::type,
        "ts",             &T::ts,
        "session_time",   &T::session_time,
        "last_lap_ms",    &T::last_lap_ms,
        "current_lap_ms", &T::current_lap_ms,
        "s1_ms",          &T::s1_ms,
        "s2_ms",          &T::s2_ms,
        "position",       &T::position,
        "lap_num",        &T::lap_num,
        "pit_status",     &T::pit_status,
        "num_pit_stops",  &T::num_pit_stops,
        "sector",         &T::sector,
        "lap_invalid",    &T::lap_invalid,
        "penalties_s",    &T::penalties_s
    );
};

// ── timing (2 Hz) ──────────────────────────────────────────────────────────

struct TimingCar {
    int  idx{};
    int  position{};
    int  lap_num{};
    int  current_lap_ms{};
    int  last_lap_ms{};
    int  s1_ms{};
    int  s2_ms{};
    int  gap_ms{};
    int  pit_status{};
    int  num_pit_stops{};
    bool lap_invalid{};
    int  penalties_s{};
    int  num_dt_pens{};
    int  num_sg_pens{};
    int  sector{};
    int  result_status{};
    int  driver_status{};
};

template <>
struct glz::meta<TimingCar> {
    using T = TimingCar;
    static constexpr auto value = glz::object(
        "idx",            &T::idx,
        "position",       &T::position,
        "lap_num",        &T::lap_num,
        "current_lap_ms", &T::current_lap_ms,
        "last_lap_ms",    &T::last_lap_ms,
        "s1_ms",          &T::s1_ms,
        "s2_ms",          &T::s2_ms,
        "gap_ms",         &T::gap_ms,
        "pit_status",     &T::pit_status,
        "num_pit_stops",  &T::num_pit_stops,
        "lap_invalid",    &T::lap_invalid,
        "penalties_s",    &T::penalties_s,
        "num_dt_pens",    &T::num_dt_pens,
        "num_sg_pens",    &T::num_sg_pens,
        "sector",         &T::sector,
        "result_status",  &T::result_status,
        "driver_status",  &T::driver_status
    );
};

struct TimingRow {
    std::string            type{"timing"};
    std::string            ts;
    float                  session_time{};
    int                    player_idx{};
    std::vector<TimingCar> cars;
};

template <>
struct glz::meta<TimingRow> {
    using T = TimingRow;
    static constexpr auto value = glz::object(
        "type",         &T::type,
        "ts",           &T::ts,
        "session_time", &T::session_time,
        "player_idx",   &T::player_idx,
        "cars",         &T::cars
    );
};

// ── status / all_status (2 Hz) ─────────────────────────────────────────────

// Live sinks publish this family at ~2 Hz; recording preserves the player's
// status row at the game's configured menu rate.
struct StatusRow {
    std::string type{"status"};
    std::string ts;
    float       session_time{};
    int         fuel_mix{};
    int         front_brake_bias{};
    double      fuel_kg{};
    double      fuel_laps{};
    bool        drs_allowed{};
    int         tyre_compound{};
    int         visual_compound{};
    int         tyre_age_laps{};
    int         ers_j{};
    double      ers_pct{};
    int         ers_mode{};
    int         ers_deployed_j{};
    double      engine_power_ice_kw{};
    double      engine_power_mguk_kw{};
    int         ers_harvested_mguk_j{};
    int         ers_harvested_mguh_j{};
};

template <>
struct glz::meta<StatusRow> {
    using T = StatusRow;
    static constexpr auto value = glz::object(
        "type",                   &T::type,
        "ts",                     &T::ts,
        "session_time",           &T::session_time,
        "fuel_mix",               &T::fuel_mix,
        "front_brake_bias",       &T::front_brake_bias,
        "fuel_kg",                &T::fuel_kg,
        "fuel_laps",              &T::fuel_laps,
        "drs_allowed",            &T::drs_allowed,
        "tyre_compound",          &T::tyre_compound,
        "visual_compound",        &T::visual_compound,
        "tyre_age_laps",          &T::tyre_age_laps,
        "ers_j",                  &T::ers_j,
        "ers_pct",                &T::ers_pct,
        "ers_mode",               &T::ers_mode,
        "ers_deployed_j",         &T::ers_deployed_j,
        "engine_power_ice_kw",    &T::engine_power_ice_kw,
        "engine_power_mguk_kw",   &T::engine_power_mguk_kw,
        "ers_harvested_mguk_j",   &T::ers_harvested_mguk_j,
        "ers_harvested_mguh_j",   &T::ers_harvested_mguh_j
    );
};

struct AllStatusCar {
    int    idx{};
    int    fuel_mix{};
    int    front_brake_bias{};
    double fuel_kg{};
    double fuel_laps{};
    bool   drs_allowed{};
    int    tyre_compound{};
    int    visual_compound{};
    int    tyre_age_laps{};
    int    ers_j{};
    double ers_pct{};
    int    ers_mode{};
    int    ers_deployed_j{};
    double engine_power_ice_kw{};
    double engine_power_mguk_kw{};
    int    ers_harvested_mguk_j{};
    int    ers_harvested_mguh_j{};
};

template <>
struct glz::meta<AllStatusCar> {
    using T = AllStatusCar;
    static constexpr auto value = glz::object(
        "idx",                    &T::idx,
        "fuel_mix",               &T::fuel_mix,
        "front_brake_bias",       &T::front_brake_bias,
        "fuel_kg",                &T::fuel_kg,
        "fuel_laps",              &T::fuel_laps,
        "drs_allowed",            &T::drs_allowed,
        "tyre_compound",          &T::tyre_compound,
        "visual_compound",        &T::visual_compound,
        "tyre_age_laps",          &T::tyre_age_laps,
        "ers_j",                  &T::ers_j,
        "ers_pct",                &T::ers_pct,
        "ers_mode",               &T::ers_mode,
        "ers_deployed_j",         &T::ers_deployed_j,
        "engine_power_ice_kw",    &T::engine_power_ice_kw,
        "engine_power_mguk_kw",   &T::engine_power_mguk_kw,
        "ers_harvested_mguk_j",   &T::ers_harvested_mguk_j,
        "ers_harvested_mguh_j",   &T::ers_harvested_mguh_j
    );
};

struct AllStatusRow {
    std::string               type{"all_status"};
    std::string               ts;
    float                     session_time{};
    std::vector<AllStatusCar> cars;
};

template <>
struct glz::meta<AllStatusRow> {
    using T = AllStatusRow;
    static constexpr auto value = glz::object(
        "type",         &T::type,
        "ts",           &T::ts,
        "session_time", &T::session_time,
        "cars",         &T::cars
    );
};

// ── damage (2 Hz) ──────────────────────────────────────────────────────────

struct DamageRow {
    std::string type{"damage"};
    std::string ts;
    float       session_time{};
    double      tyre_wear_rl{};
    double      tyre_wear_rr{};
    double      tyre_wear_fl{};
    double      tyre_wear_fr{};
    int         tyre_dmg_rl{};
    int         tyre_dmg_rr{};
    int         tyre_dmg_fl{};
    int         tyre_dmg_fr{};
    int         brake_dmg_rl{};
    int         brake_dmg_rr{};
    int         brake_dmg_fl{};
    int         brake_dmg_fr{};
    int         blisters_rl{};
    int         blisters_rr{};
    int         blisters_fl{};
    int         blisters_fr{};
    int         wing_fl{};
    int         wing_fr{};
    int         wing_rear{};
    int         floor_damage{};
    int         sidepod_damage{};
    int         diffuser_damage{};
    int         gearbox_damage{};
    int         engine_damage{};
    int         drs_fault{};
    int         ers_fault{};
};

template <>
struct glz::meta<DamageRow> {
    using T = DamageRow;
    static constexpr auto value = glz::object(
        "type",             &T::type,
        "ts",               &T::ts,
        "session_time",     &T::session_time,
        "tyre_wear_rl",     &T::tyre_wear_rl,
        "tyre_wear_rr",     &T::tyre_wear_rr,
        "tyre_wear_fl",     &T::tyre_wear_fl,
        "tyre_wear_fr",     &T::tyre_wear_fr,
        "tyre_dmg_rl",      &T::tyre_dmg_rl,
        "tyre_dmg_rr",      &T::tyre_dmg_rr,
        "tyre_dmg_fl",      &T::tyre_dmg_fl,
        "tyre_dmg_fr",      &T::tyre_dmg_fr,
        "brake_dmg_rl",     &T::brake_dmg_rl,
        "brake_dmg_rr",     &T::brake_dmg_rr,
        "brake_dmg_fl",     &T::brake_dmg_fl,
        "brake_dmg_fr",     &T::brake_dmg_fr,
        "blisters_rl",      &T::blisters_rl,
        "blisters_rr",      &T::blisters_rr,
        "blisters_fl",      &T::blisters_fl,
        "blisters_fr",      &T::blisters_fr,
        "wing_fl",          &T::wing_fl,
        "wing_fr",          &T::wing_fr,
        "wing_rear",        &T::wing_rear,
        "floor_damage",     &T::floor_damage,
        "sidepod_damage",   &T::sidepod_damage,
        "diffuser_damage",  &T::diffuser_damage,
        "gearbox_damage",   &T::gearbox_damage,
        "engine_damage",    &T::engine_damage,
        "drs_fault",        &T::drs_fault,
        "ers_fault",        &T::ers_fault
    );
};
