#include "tnrp/XlsxExport.h"

#include "tnrp/TnrdReader.h"
#include "tnrp/rows.h"
#include "tnrp/control_rows.h"

#include <xlsxwriter.h>
#include <glaze/glaze.hpp>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Raw-data XLSX export: walks a loaded TnrdReader's whole row index in file
// order and writes one worksheet per row type encountered (first-seen
// order), every field of every row as its own column — no aggregation, no
// lap-grouping. Multi-car packets (timing/all_status/positions/participants/
// tyre_sets) are exploded into one Excel row per car/driver/set.

namespace tnrp {

namespace {

// Local copy of TnrdReader.cpp's partial-read options (that one lives in an
// anonymous namespace there, so it isn't visible here).
constexpr glz::opts kPartialRead{ .null_terminated = false, .error_on_unknown_keys = false };

const char* sheetNameForType(uint8_t type) {
    switch (type) {
        case 1:  return "Telemetry";
        case 2:  return "Status";
        case 3:  return "Damage";
        case 4:  return "Lap";
        case 5:  return "Session";
        case 6:  return "RaceEvent";
        case 7:  return "Timing";
        case 8:  return "Participants";
        case 9:  return "AllStatus";
        case 10: return "TyreSets";
        case 11: return "Motion";
        case 12: return "MotionEx";
        case 13: return "Positions";
        default: return nullptr;
    }
}

// Hand-written column lists (glaze's compile-time reflection isn't
// runtime-iterable, and multi-car flattening needs custom handling anyway).
// Every sheet is prefixed with row_index/session_time, taken from the
// index entry rather than the row body so it's always populated.
const std::vector<const char*>& columnsForType(uint8_t type) {
    static const std::vector<const char*> telemetry = {
        "row_index", "session_time", "ts", "speed_kph", "rpm", "gear", "throttle", "brake",
        "steering", "drs", "slm",
        "tyre_temp_surface_rl", "tyre_temp_surface_rr", "tyre_temp_surface_fl", "tyre_temp_surface_fr",
        "tyre_temp_inner_rl", "tyre_temp_inner_rr", "tyre_temp_inner_fl", "tyre_temp_inner_fr",
        "brake_temp_rl", "brake_temp_rr", "brake_temp_fl", "brake_temp_fr", "engine_temp"
    };
    static const std::vector<const char*> status = {
        "row_index", "session_time", "ts", "fuel_mix", "front_brake_bias", "fuel_kg", "fuel_laps",
        "drs_allowed", "tyre_compound", "visual_compound", "tyre_age_laps", "ers_j", "ers_pct",
        "ers_mode", "ers_deployed_j", "engine_power_ice_kw", "engine_power_mguk_kw",
        "ers_harvested_mguk_j", "ers_harvested_mguh_j"
    };
    static const std::vector<const char*> damage = {
        "row_index", "session_time", "ts",
        "tyre_wear_rl", "tyre_wear_rr", "tyre_wear_fl", "tyre_wear_fr",
        "tyre_dmg_rl", "tyre_dmg_rr", "tyre_dmg_fl", "tyre_dmg_fr",
        "brake_dmg_rl", "brake_dmg_rr", "brake_dmg_fl", "brake_dmg_fr",
        "blisters_rl", "blisters_rr", "blisters_fl", "blisters_fr",
        "wing_fl", "wing_fr", "wing_rear", "floor_damage", "sidepod_damage",
        "diffuser_damage", "gearbox_damage", "engine_damage", "drs_fault", "ers_fault"
    };
    static const std::vector<const char*> lap = {
        "row_index", "session_time", "ts", "last_lap_ms", "current_lap_ms", "s1_ms", "s2_ms",
        "position", "lap_num", "pit_status", "num_pit_stops", "sector", "lap_invalid", "penalties_s"
    };
    static const std::vector<const char*> session = {
        "row_index", "session_time", "ts", "weather", "track_temp", "air_temp", "track_length_m",
        "track_id", "session_type", "total_laps", "session_time_left", "session_duration",
        "pit_speed_limit", "pit_stop_window_ideal_lap", "pit_stop_window_latest_lap",
        "pit_stop_rejoin_position", "num_marshal_zones", "marshal_zones_json",
        "weather_forecast_samples_json", "safety_car_status", "forecast_accuracy", "ai_difficulty",
        "time_of_day", "num_safety_car_periods", "num_virtual_sc_periods", "num_red_flag_periods",
        "active_aero_track_status"
    };
    static const std::vector<const char*> raceEvent = {
        "row_index", "session_time", "ts", "code", "car_idx", "lap_time_s", "safety_car_type",
        "event_type", "penalty_type", "infringement_type", "penalty_time_s"
    };
    static const std::vector<const char*> timing = {
        "row_index", "session_time", "ts", "player_idx", "car_idx", "position", "lap_num",
        "current_lap_ms", "last_lap_ms", "s1_ms", "s2_ms", "gap_ms", "pit_status", "num_pit_stops",
        "lap_invalid", "penalties_s", "num_dt_pens", "num_sg_pens", "sector", "result_status",
        "driver_status"
    };
    static const std::vector<const char*> participants = {
        "row_index", "session_time", "idx", "name", "team_id", "race_number", "ai", "livery_color"
    };
    static const std::vector<const char*> allStatus = {
        "row_index", "session_time", "ts", "car_idx", "fuel_mix", "front_brake_bias", "fuel_kg",
        "fuel_laps", "drs_allowed", "tyre_compound", "visual_compound", "tyre_age_laps", "ers_j",
        "ers_pct", "ers_mode", "ers_deployed_j", "engine_power_ice_kw", "engine_power_mguk_kw",
        "ers_harvested_mguk_j", "ers_harvested_mguh_j"
    };
    static const std::vector<const char*> tyreSets = {
        "row_index", "session_time", "ts", "fitted_idx", "set_idx", "actual_compound",
        "visual_compound", "wear", "available", "recommended_session", "life_span", "usable_life",
        "lap_delta_ms", "fitted"
    };
    static const std::vector<const char*> motion = {
        "row_index", "session_time", "ts", "g_lat", "g_long", "g_vert"
    };
    static const std::vector<const char*> motionEx = {
        "row_index", "session_time", "ts", "front_aero_height_mm", "rear_aero_height_mm"
    };
    static const std::vector<const char*> positions = {
        "row_index", "session_time", "ts", "player_idx", "car_idx", "x", "z"
    };
    static const std::vector<const char*> empty = {};

    switch (type) {
        case 1:  return telemetry;
        case 2:  return status;
        case 3:  return damage;
        case 4:  return lap;
        case 5:  return session;
        case 6:  return raceEvent;
        case 7:  return timing;
        case 8:  return participants;
        case 9:  return allStatus;
        case 10: return tyreSets;
        case 11: return motion;
        case 12: return motionEx;
        case 13: return positions;
        default: return empty;
    }
}

void writeHeaderRow(lxw_worksheet* ws, lxw_format* bold, const std::vector<const char*>& cols) {
    for (size_t i = 0; i < cols.size(); ++i)
        worksheet_write_string(ws, 0, static_cast<lxw_col_t>(i), cols[i], bold);
}

// Writes one (or more, for multi-car packets) data row(s) for a single JSONL
// line, advancing `row` past whatever it wrote.
void writeDataRow(lxw_worksheet* ws, lxw_row_t& row, float t, uint8_t type, const std::string& line) {
    std::string_view sv(line);

    switch (type) {
    case 1: { // telemetry
        TelemetryRow r{}; (void)glz::read<kPartialRead>(r, sv);
        lxw_col_t c = 0;
        worksheet_write_number(ws, row, c++, static_cast<double>(row - 1), nullptr);
        worksheet_write_number(ws, row, c++, t, nullptr);
        worksheet_write_string(ws, row, c++, r.ts.c_str(), nullptr);
        worksheet_write_number(ws, row, c++, r.speed_kph, nullptr);
        worksheet_write_number(ws, row, c++, r.rpm, nullptr);
        worksheet_write_number(ws, row, c++, r.gear, nullptr);
        worksheet_write_number(ws, row, c++, r.throttle, nullptr);
        worksheet_write_number(ws, row, c++, r.brake, nullptr);
        worksheet_write_number(ws, row, c++, r.steering, nullptr);
        worksheet_write_number(ws, row, c++, r.drs, nullptr);
        worksheet_write_number(ws, row, c++, r.slm, nullptr);
        worksheet_write_number(ws, row, c++, r.tyre_temp_surface_rl, nullptr);
        worksheet_write_number(ws, row, c++, r.tyre_temp_surface_rr, nullptr);
        worksheet_write_number(ws, row, c++, r.tyre_temp_surface_fl, nullptr);
        worksheet_write_number(ws, row, c++, r.tyre_temp_surface_fr, nullptr);
        worksheet_write_number(ws, row, c++, r.tyre_temp_inner_rl, nullptr);
        worksheet_write_number(ws, row, c++, r.tyre_temp_inner_rr, nullptr);
        worksheet_write_number(ws, row, c++, r.tyre_temp_inner_fl, nullptr);
        worksheet_write_number(ws, row, c++, r.tyre_temp_inner_fr, nullptr);
        worksheet_write_number(ws, row, c++, r.brake_temp_rl, nullptr);
        worksheet_write_number(ws, row, c++, r.brake_temp_rr, nullptr);
        worksheet_write_number(ws, row, c++, r.brake_temp_fl, nullptr);
        worksheet_write_number(ws, row, c++, r.brake_temp_fr, nullptr);
        worksheet_write_number(ws, row, c++, r.engine_temp, nullptr);
        ++row;
        break;
    }
    case 2: { // status
        StatusRow r{}; (void)glz::read<kPartialRead>(r, sv);
        lxw_col_t c = 0;
        worksheet_write_number(ws, row, c++, static_cast<double>(row - 1), nullptr);
        worksheet_write_number(ws, row, c++, t, nullptr);
        worksheet_write_string(ws, row, c++, r.ts.c_str(), nullptr);
        worksheet_write_number(ws, row, c++, r.fuel_mix, nullptr);
        worksheet_write_number(ws, row, c++, r.front_brake_bias, nullptr);
        worksheet_write_number(ws, row, c++, r.fuel_kg, nullptr);
        worksheet_write_number(ws, row, c++, r.fuel_laps, nullptr);
        worksheet_write_boolean(ws, row, c++, r.drs_allowed ? 1 : 0, nullptr);
        worksheet_write_number(ws, row, c++, r.tyre_compound, nullptr);
        worksheet_write_number(ws, row, c++, r.visual_compound, nullptr);
        worksheet_write_number(ws, row, c++, r.tyre_age_laps, nullptr);
        worksheet_write_number(ws, row, c++, r.ers_j, nullptr);
        worksheet_write_number(ws, row, c++, r.ers_pct, nullptr);
        worksheet_write_number(ws, row, c++, r.ers_mode, nullptr);
        worksheet_write_number(ws, row, c++, r.ers_deployed_j, nullptr);
        worksheet_write_number(ws, row, c++, r.engine_power_ice_kw, nullptr);
        worksheet_write_number(ws, row, c++, r.engine_power_mguk_kw, nullptr);
        worksheet_write_number(ws, row, c++, r.ers_harvested_mguk_j, nullptr);
        worksheet_write_number(ws, row, c++, r.ers_harvested_mguh_j, nullptr);
        ++row;
        break;
    }
    case 3: { // damage
        DamageRow r{}; (void)glz::read<kPartialRead>(r, sv);
        lxw_col_t c = 0;
        worksheet_write_number(ws, row, c++, static_cast<double>(row - 1), nullptr);
        worksheet_write_number(ws, row, c++, t, nullptr);
        worksheet_write_string(ws, row, c++, r.ts.c_str(), nullptr);
        worksheet_write_number(ws, row, c++, r.tyre_wear_rl, nullptr);
        worksheet_write_number(ws, row, c++, r.tyre_wear_rr, nullptr);
        worksheet_write_number(ws, row, c++, r.tyre_wear_fl, nullptr);
        worksheet_write_number(ws, row, c++, r.tyre_wear_fr, nullptr);
        worksheet_write_number(ws, row, c++, r.tyre_dmg_rl, nullptr);
        worksheet_write_number(ws, row, c++, r.tyre_dmg_rr, nullptr);
        worksheet_write_number(ws, row, c++, r.tyre_dmg_fl, nullptr);
        worksheet_write_number(ws, row, c++, r.tyre_dmg_fr, nullptr);
        worksheet_write_number(ws, row, c++, r.brake_dmg_rl, nullptr);
        worksheet_write_number(ws, row, c++, r.brake_dmg_rr, nullptr);
        worksheet_write_number(ws, row, c++, r.brake_dmg_fl, nullptr);
        worksheet_write_number(ws, row, c++, r.brake_dmg_fr, nullptr);
        worksheet_write_number(ws, row, c++, r.blisters_rl, nullptr);
        worksheet_write_number(ws, row, c++, r.blisters_rr, nullptr);
        worksheet_write_number(ws, row, c++, r.blisters_fl, nullptr);
        worksheet_write_number(ws, row, c++, r.blisters_fr, nullptr);
        worksheet_write_number(ws, row, c++, r.wing_fl, nullptr);
        worksheet_write_number(ws, row, c++, r.wing_fr, nullptr);
        worksheet_write_number(ws, row, c++, r.wing_rear, nullptr);
        worksheet_write_number(ws, row, c++, r.floor_damage, nullptr);
        worksheet_write_number(ws, row, c++, r.sidepod_damage, nullptr);
        worksheet_write_number(ws, row, c++, r.diffuser_damage, nullptr);
        worksheet_write_number(ws, row, c++, r.gearbox_damage, nullptr);
        worksheet_write_number(ws, row, c++, r.engine_damage, nullptr);
        worksheet_write_number(ws, row, c++, r.drs_fault, nullptr);
        worksheet_write_number(ws, row, c++, r.ers_fault, nullptr);
        ++row;
        break;
    }
    case 4: { // lap
        LapRow r{}; (void)glz::read<kPartialRead>(r, sv);
        lxw_col_t c = 0;
        worksheet_write_number(ws, row, c++, static_cast<double>(row - 1), nullptr);
        worksheet_write_number(ws, row, c++, t, nullptr);
        worksheet_write_string(ws, row, c++, r.ts.c_str(), nullptr);
        worksheet_write_number(ws, row, c++, r.last_lap_ms, nullptr);
        worksheet_write_number(ws, row, c++, r.current_lap_ms, nullptr);
        worksheet_write_number(ws, row, c++, r.s1_ms, nullptr);
        worksheet_write_number(ws, row, c++, r.s2_ms, nullptr);
        worksheet_write_number(ws, row, c++, r.position, nullptr);
        worksheet_write_number(ws, row, c++, r.lap_num, nullptr);
        worksheet_write_number(ws, row, c++, r.pit_status, nullptr);
        worksheet_write_number(ws, row, c++, r.num_pit_stops, nullptr);
        worksheet_write_number(ws, row, c++, r.sector, nullptr);
        worksheet_write_boolean(ws, row, c++, r.lap_invalid ? 1 : 0, nullptr);
        worksheet_write_number(ws, row, c++, r.penalties_s, nullptr);
        ++row;
        break;
    }
    case 5: { // session
        SessionRow r{}; (void)glz::read<kPartialRead>(r, sv);
        lxw_col_t c = 0;
        worksheet_write_number(ws, row, c++, static_cast<double>(row - 1), nullptr);
        worksheet_write_number(ws, row, c++, t, nullptr);
        worksheet_write_string(ws, row, c++, r.ts.c_str(), nullptr);
        worksheet_write_number(ws, row, c++, r.weather, nullptr);
        worksheet_write_number(ws, row, c++, r.track_temp, nullptr);
        worksheet_write_number(ws, row, c++, r.air_temp, nullptr);
        worksheet_write_number(ws, row, c++, r.track_length_m, nullptr);
        worksheet_write_number(ws, row, c++, r.track_id, nullptr);
        worksheet_write_number(ws, row, c++, r.session_type, nullptr);
        worksheet_write_number(ws, row, c++, r.total_laps, nullptr);
        worksheet_write_number(ws, row, c++, r.session_time_left, nullptr);
        worksheet_write_number(ws, row, c++, r.session_duration, nullptr);
        worksheet_write_number(ws, row, c++, r.pit_speed_limit, nullptr);
        worksheet_write_number(ws, row, c++, r.pit_stop_window_ideal_lap, nullptr);
        worksheet_write_number(ws, row, c++, r.pit_stop_window_latest_lap, nullptr);
        worksheet_write_number(ws, row, c++, r.pit_stop_rejoin_position, nullptr);
        worksheet_write_number(ws, row, c++, r.num_marshal_zones, nullptr);
        worksheet_write_string(ws, row, c++, writeJson(r.marshal_zones).c_str(), nullptr);
        worksheet_write_string(ws, row, c++, writeJson(r.weather_forecast_samples).c_str(), nullptr);
        worksheet_write_number(ws, row, c++, r.safety_car_status, nullptr);
        worksheet_write_number(ws, row, c++, r.forecast_accuracy, nullptr);
        worksheet_write_number(ws, row, c++, r.ai_difficulty, nullptr);
        worksheet_write_number(ws, row, c++, static_cast<double>(r.time_of_day), nullptr);
        worksheet_write_number(ws, row, c++, r.num_safety_car_periods, nullptr);
        worksheet_write_number(ws, row, c++, r.num_virtual_sc_periods, nullptr);
        worksheet_write_number(ws, row, c++, r.num_red_flag_periods, nullptr);
        worksheet_write_number(ws, row, c++, r.active_aero_track_status, nullptr);
        ++row;
        break;
    }
    case 6: { // race_event — optional<> fields: leave the cell blank when unset
        RaceEventRow r{}; (void)glz::read<kPartialRead>(r, sv);
        lxw_col_t c = 0;
        worksheet_write_number(ws, row, c++, static_cast<double>(row - 1), nullptr);
        worksheet_write_number(ws, row, c++, t, nullptr);
        worksheet_write_string(ws, row, c++, r.ts.c_str(), nullptr);
        worksheet_write_string(ws, row, c++, r.code.c_str(), nullptr);
        if (r.car_idx)           worksheet_write_number(ws, row, c, *r.car_idx, nullptr);           ++c;
        if (r.lap_time_s)        worksheet_write_number(ws, row, c, *r.lap_time_s, nullptr);        ++c;
        if (r.safety_car_type)   worksheet_write_number(ws, row, c, *r.safety_car_type, nullptr);   ++c;
        if (r.event_type)        worksheet_write_number(ws, row, c, *r.event_type, nullptr);        ++c;
        if (r.penalty_type)      worksheet_write_number(ws, row, c, *r.penalty_type, nullptr);      ++c;
        if (r.infringement_type) worksheet_write_number(ws, row, c, *r.infringement_type, nullptr); ++c;
        if (r.penalty_time_s)    worksheet_write_number(ws, row, c, *r.penalty_time_s, nullptr);    ++c;
        ++row;
        break;
    }
    case 7: { // timing — multi-car: one Excel row per car
        TimingRow r{}; (void)glz::read<kPartialRead>(r, sv);
        for (const TimingCar& car : r.cars) {
            lxw_col_t c = 0;
            worksheet_write_number(ws, row, c++, static_cast<double>(row - 1), nullptr);
            worksheet_write_number(ws, row, c++, t, nullptr);
            worksheet_write_string(ws, row, c++, r.ts.c_str(), nullptr);
            worksheet_write_number(ws, row, c++, r.player_idx, nullptr);
            worksheet_write_number(ws, row, c++, car.idx, nullptr);
            worksheet_write_number(ws, row, c++, car.position, nullptr);
            worksheet_write_number(ws, row, c++, car.lap_num, nullptr);
            worksheet_write_number(ws, row, c++, car.current_lap_ms, nullptr);
            worksheet_write_number(ws, row, c++, car.last_lap_ms, nullptr);
            worksheet_write_number(ws, row, c++, car.s1_ms, nullptr);
            worksheet_write_number(ws, row, c++, car.s2_ms, nullptr);
            worksheet_write_number(ws, row, c++, car.gap_ms, nullptr);
            worksheet_write_number(ws, row, c++, car.pit_status, nullptr);
            worksheet_write_number(ws, row, c++, car.num_pit_stops, nullptr);
            worksheet_write_boolean(ws, row, c++, car.lap_invalid ? 1 : 0, nullptr);
            worksheet_write_number(ws, row, c++, car.penalties_s, nullptr);
            worksheet_write_number(ws, row, c++, car.num_dt_pens, nullptr);
            worksheet_write_number(ws, row, c++, car.num_sg_pens, nullptr);
            worksheet_write_number(ws, row, c++, car.sector, nullptr);
            worksheet_write_number(ws, row, c++, car.result_status, nullptr);
            worksheet_write_number(ws, row, c++, car.driver_status, nullptr);
            ++row;
        }
        break;
    }
    case 8: { // participants — multi-driver: one Excel row per driver (no ts on this packet)
        ParticipantsRow r{}; (void)glz::read<kPartialRead>(r, sv);
        for (const Driver& d : r.drivers) {
            lxw_col_t c = 0;
            worksheet_write_number(ws, row, c++, static_cast<double>(row - 1), nullptr);
            worksheet_write_number(ws, row, c++, t, nullptr);
            worksheet_write_number(ws, row, c++, d.idx, nullptr);
            worksheet_write_string(ws, row, c++, d.name.c_str(), nullptr);
            worksheet_write_number(ws, row, c++, d.team_id, nullptr);
            worksheet_write_number(ws, row, c++, d.race_number, nullptr);
            worksheet_write_boolean(ws, row, c++, d.ai ? 1 : 0, nullptr);
            worksheet_write_string(ws, row, c++, d.livery_color.c_str(), nullptr);
            ++row;
        }
        break;
    }
    case 9: { // all_status — multi-car: one Excel row per car
        AllStatusRow r{}; (void)glz::read<kPartialRead>(r, sv);
        for (const AllStatusCar& car : r.cars) {
            lxw_col_t c = 0;
            worksheet_write_number(ws, row, c++, static_cast<double>(row - 1), nullptr);
            worksheet_write_number(ws, row, c++, t, nullptr);
            worksheet_write_string(ws, row, c++, r.ts.c_str(), nullptr);
            worksheet_write_number(ws, row, c++, car.idx, nullptr);
            worksheet_write_number(ws, row, c++, car.fuel_mix, nullptr);
            worksheet_write_number(ws, row, c++, car.front_brake_bias, nullptr);
            worksheet_write_number(ws, row, c++, car.fuel_kg, nullptr);
            worksheet_write_number(ws, row, c++, car.fuel_laps, nullptr);
            worksheet_write_boolean(ws, row, c++, car.drs_allowed ? 1 : 0, nullptr);
            worksheet_write_number(ws, row, c++, car.tyre_compound, nullptr);
            worksheet_write_number(ws, row, c++, car.visual_compound, nullptr);
            worksheet_write_number(ws, row, c++, car.tyre_age_laps, nullptr);
            worksheet_write_number(ws, row, c++, car.ers_j, nullptr);
            worksheet_write_number(ws, row, c++, car.ers_pct, nullptr);
            worksheet_write_number(ws, row, c++, car.ers_mode, nullptr);
            worksheet_write_number(ws, row, c++, car.ers_deployed_j, nullptr);
            worksheet_write_number(ws, row, c++, car.engine_power_ice_kw, nullptr);
            worksheet_write_number(ws, row, c++, car.engine_power_mguk_kw, nullptr);
            worksheet_write_number(ws, row, c++, car.ers_harvested_mguk_j, nullptr);
            worksheet_write_number(ws, row, c++, car.ers_harvested_mguh_j, nullptr);
            ++row;
        }
        break;
    }
    case 10: { // tyre_sets — multi-set: one Excel row per set
        TyreSetsRow r{}; (void)glz::read<kPartialRead>(r, sv);
        for (const TyreSet& set : r.sets) {
            lxw_col_t c = 0;
            worksheet_write_number(ws, row, c++, static_cast<double>(row - 1), nullptr);
            worksheet_write_number(ws, row, c++, t, nullptr);
            worksheet_write_string(ws, row, c++, r.ts.c_str(), nullptr);
            worksheet_write_number(ws, row, c++, r.fitted_idx, nullptr);
            worksheet_write_number(ws, row, c++, set.idx, nullptr);
            worksheet_write_number(ws, row, c++, set.actual_compound, nullptr);
            worksheet_write_number(ws, row, c++, set.visual_compound, nullptr);
            worksheet_write_number(ws, row, c++, set.wear, nullptr);
            worksheet_write_boolean(ws, row, c++, set.available ? 1 : 0, nullptr);
            worksheet_write_number(ws, row, c++, set.recommended_session, nullptr);
            worksheet_write_number(ws, row, c++, set.life_span, nullptr);
            worksheet_write_number(ws, row, c++, set.usable_life, nullptr);
            worksheet_write_number(ws, row, c++, set.lap_delta_ms, nullptr);
            worksheet_write_boolean(ws, row, c++, set.fitted ? 1 : 0, nullptr);
            ++row;
        }
        break;
    }
    case 11: { // motion
        MotionRow r{}; (void)glz::read<kPartialRead>(r, sv);
        lxw_col_t c = 0;
        worksheet_write_number(ws, row, c++, static_cast<double>(row - 1), nullptr);
        worksheet_write_number(ws, row, c++, t, nullptr);
        worksheet_write_string(ws, row, c++, r.ts.c_str(), nullptr);
        worksheet_write_number(ws, row, c++, r.g_lat, nullptr);
        worksheet_write_number(ws, row, c++, r.g_long, nullptr);
        worksheet_write_number(ws, row, c++, r.g_vert, nullptr);
        ++row;
        break;
    }
    case 12: { // motion_ex
        MotionExRow r{}; (void)glz::read<kPartialRead>(r, sv);
        lxw_col_t c = 0;
        worksheet_write_number(ws, row, c++, static_cast<double>(row - 1), nullptr);
        worksheet_write_number(ws, row, c++, t, nullptr);
        worksheet_write_string(ws, row, c++, r.ts.c_str(), nullptr);
        worksheet_write_number(ws, row, c++, r.front_aero_height_mm, nullptr);
        worksheet_write_number(ws, row, c++, r.rear_aero_height_mm, nullptr);
        ++row;
        break;
    }
    case 13: { // positions — multi-car: one Excel row per car
        PositionsRow r{}; (void)glz::read<kPartialRead>(r, sv);
        for (const PositionCar& car : r.cars) {
            lxw_col_t c = 0;
            worksheet_write_number(ws, row, c++, static_cast<double>(row - 1), nullptr);
            worksheet_write_number(ws, row, c++, t, nullptr);
            worksheet_write_string(ws, row, c++, r.ts.c_str(), nullptr);
            worksheet_write_number(ws, row, c++, r.player_idx, nullptr);
            worksheet_write_number(ws, row, c++, car.idx, nullptr);
            worksheet_write_number(ws, row, c++, car.x, nullptr);
            worksheet_write_number(ws, row, c++, car.z, nullptr);
            ++row;
        }
        break;
    }
    default:
        break;
    }
}

} // namespace

bool TnrdReader::exportXlsx(const HeaderRow& header, const std::string& outPath, std::string* errorOut,
                            const std::function<void(size_t, size_t, const std::string&)>& onProgress) {
    if (!tempFile_) {
        if (errorOut) *errorOut = "no .tnrd loaded";
        return false;
    }

    lxw_workbook* wb = workbook_new(outPath.c_str());
    if (!wb) {
        if (errorOut) *errorOut = "could not create workbook (invalid destination path?)";
        return false;
    }

    lxw_format* bold = workbook_add_format(wb);
    format_set_bold(bold);

    (void)header;  // no longer written to an Info sheet; kept for API stability

    const size_t total = index_.size();
    // Throttle progress callbacks to ~500 calls over the whole export rather
    // than once per row (row counts can run into the hundreds of thousands).
    const size_t reportEvery = total > 0 ? std::max<size_t>(1, total / 500) : 1;

    // The bar is split into two honest bands:
    //   0 .. kRowBandTop  — building each worksheet in memory (row-by-row)
    //   kRowBandTop .. 1  — reserved for workbook_close(), which serializes and
    //                       zip-compresses the actual .xlsx bytes to disk. That
    //                       call is opaque (no callbacks), so the bar holds at
    //                       kRowBandTop while it runs and only snaps to 100%
    //                       once the file is fully written.
    constexpr double kRowBandTop = 0.95; // sheet building fills 0..95%

    // Report an absolute [0,1] fraction of the bar plus a stage message through
    // the (done,total,stage) callback (consumers compute pct = 100*done/total).
    auto report = [&](double frac, const std::string& stage) {
        if (!onProgress) return;
        if (frac < 0.0) frac = 0.0;
        if (frac > 1.0) frac = 1.0;
        size_t doneUnits = total > 0 ? static_cast<size_t>(frac * static_cast<double>(total) + 0.5)
                                     : static_cast<size_t>(frac + 0.5);
        onProgress(doneUnits, total > 0 ? total : 1, stage);
    };

    report(0.0, "Preparing worksheets");

    // Bucket the flat, time-ordered index into per-type groups, preserving each
    // type's rows in file order and the first-seen order of the types (so sheet
    // tabs come out in the same order as before). Building one sheet fully
    // before the next lets us report an honest per-sheet stage message.
    std::vector<uint8_t> typeOrder;                       // first-seen order
    std::unordered_map<uint8_t, std::vector<const IndexEntry*>> byType;
    for (const IndexEntry& e : index_) {
        if (!sheetNameForType(e.type)) continue;          // skip unknown/untabled types
        auto it = byType.find(e.type);
        if (it == byType.end()) {
            typeOrder.push_back(e.type);
            it = byType.emplace(e.type, std::vector<const IndexEntry*>{}).first;
        }
        it->second.push_back(&e);
    }

    size_t done = 0;
    for (size_t s = 0; s < typeOrder.size(); ++s) {
        const uint8_t type = typeOrder[s];
        const char* sheetName = sheetNameForType(type);
        const auto& entries = byType[type];

        std::string stage = "Writing " + std::string(sheetName) + " sheet ("
                          + std::to_string(s + 1) + "/" + std::to_string(typeOrder.size()) + ")";
        report(kRowBandTop * static_cast<double>(done) / static_cast<double>(total > 0 ? total : 1), stage);

        lxw_worksheet* ws = workbook_add_worksheet(wb, sheetName);
        writeHeaderRow(ws, bold, columnsForType(type));
        lxw_row_t nextRow = 1;

        // The Participants packet is re-emitted throughout the session (once per
        // session_time change) but its driver roster is effectively static, so
        // writing every occurrence duplicates the same drivers. Emit it just
        // once (the first occurrence). Other row types are genuine time series.
        const bool firstOnly = (type == 8);

        bool wrote = false;
        for (const IndexEntry* e : entries) {
            if (!firstOnly || !wrote) {
                std::string line = readLine(e->offset);
                if (!line.empty()) {
                    writeDataRow(ws, nextRow, e->sessionTime, type, line);
                    wrote = true;
                }
            }

            ++done;
            if (total > 0 && (done % reportEvery == 0 || done == total))
                report(kRowBandTop * static_cast<double>(done) / static_cast<double>(total), stage);
        }
    }

    // All sheets built in memory; now flush to disk. Bar holds at kRowBandTop.
    report(kRowBandTop, "Writing file to disk");
    lxw_error err = workbook_close(wb);
    if (err != LXW_NO_ERROR) {
        if (errorOut) *errorOut = lxw_strerror(err);
        return false;
    }

    // File is now fully written to disk — only now is the bar truly complete.
    report(1.0, "Done");
    return true;
}

bool exportTnrdFileToXlsx(const std::string& srcTnrdPath, const std::string& destXlsxPath, std::string* errorOut,
                          const XlsxProgressFn& onProgress) {
    TnrdReader reader;
    HeaderRow header;
    if (!reader.load(srcTnrdPath, header)) {
        if (errorOut) *errorOut = "failed to open/decompress/parse .tnrd file";
        return false;
    }
    bool ok = reader.exportXlsx(header, destXlsxPath, errorOut, onProgress);
    reader.close();
    return ok;
}

} // namespace tnrp
