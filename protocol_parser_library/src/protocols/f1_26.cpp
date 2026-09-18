#include "f1_26.h"
#include "tnrp/rows.h"
#include "tnrp/control_rows.h"
#include "tnrp/BinaryRows.h"
#include <algorithm>
#include <array>
#include <cstdio>

using namespace tnrp;

// All-driver field layouts: supplied f1-25-2026-season-8.md,
// CarMotionData p3, LapData p6, ParticipantData pp9-10, telemetry p11, damage pp13-14; privacy follows Restricted data (Your Telemetry).

static const int HEADER_SIZE = 29;

// 2026 Season Pack: grid grows from 22 to 24 cars.
static const int MAX_CARS = 24;

static const int PID_MOTION           = 0;
static const int PID_SESSION          = 1;
static const int PID_LAP_DATA         = 2;
static const int PID_EVENT            = 3;
static const int PID_PARTICIPANTS     = 4;
static const int PID_CAR_TEL          = 6;
static const int PID_CAR_STATUS       = 7;
static const int PID_CAR_DAMAGE       = 10;
static const int PID_SESSION_HISTORY  = 11;
static const int PID_TYRE_SETS        = 12;
static const int PID_MOTION_EX        = 13;
static const int PID_CAR_TEL2         = 16;

const char* F1_26::RecordingFilenamePrefix() { return "f1_26"; }

// 2026 moves the player's wing state out of Car Telemetry's m_drs (which stays 0
// under the new regs) and into Car Telemetry 2's m_activeAeroMode (0 = Corner /
// high-downforce, 1 = Straight / low-drag = "wing open"). The engine parses on a
// single thread, so each car's last-seen active-aero mode is cached here and
// surfaced on the telemetry row's dedicated `slm` field (separate from `drs`).
static std::array<uint8_t, 24> s_activeAero{};

// Car Status grew by one float (m_ersHarvestedLimitPerLap, inserted between
// m_ersHarvestedThisLapMGUH and m_ersDeployedThisLap) → 55 → 59 bytes per car.
static AllStatusCar ParseStatusF126(const uint8_t* data, int base) {
    AllStatusCar c;
    int o = base;
    o += 2;
    c.fuel_mix              = data[o++];
    c.front_brake_bias      = data[o++];
    o += 1;
    float fuelKg            = ReadFloat(data, o); o += 4;
    o += 4;
    float fuelLaps          = ReadFloat(data, o); o += 4;
    o += 2; o += 2; o += 1;
    c.drs_allowed           = data[o++] != 0;
    o += 2;
    c.tyre_compound         = data[o++];
    c.visual_compound       = data[o++];
    c.tyre_age_laps         = data[o++];
    o += 1;
    float enginePowerICE    = ReadFloat(data, o); o += 4;
    float enginePowerMGUK   = ReadFloat(data, o); o += 4;
    float ersJ              = ReadFloat(data, o); o += 4;
    c.ers_mode              = data[o++];
    float ersHarvestedMGUK  = ReadFloat(data, o); o += 4;
    float ersHarvestedMGUH  = ReadFloat(data, o); o += 4;
    o += 4;  // m_ersHarvestedLimitPerLap (new in 2026, not surfaced yet)
    float ersDeployedJ      = ReadFloat(data, o);

    c.fuel_kg               = Round2(fuelKg);
    c.fuel_laps             = Round1(fuelLaps);
    c.ers_j                 = (int)std::round(ersJ);
    c.ers_pct               = Round1(ersJ / 40000.0);
    c.ers_deployed_j        = (int)std::round(ersDeployedJ);
    c.engine_power_ice_kw   = Round1(enginePowerICE  / 1000.0);
    c.engine_power_mguk_kw  = Round1(enginePowerMGUK / 1000.0);
    c.ers_harvested_mguk_j  = (int)std::round(ersHarvestedMGUK);
    c.ers_harvested_mguh_j  = (int)std::round(ersHarvestedMGUH);
    return c;
}

std::vector<std::string> F1_26::ParsePacket(const uint8_t* data, int length, const PacketHeader& hdr, const std::string& timestamp, HotOut& hot, const TeamColorOverrides& teamColorOverrides) {
    std::vector<std::string> rows;
    std::string buf;

    switch (hdr.packetId) {

        // ── SESSION (complex, glaze) ───────────────────────────────────────
        // 2026 appends active-aero/DRS zones, start reaction time and extra
        // assist flags AFTER m_sector3LapDistanceStart, so every offset read
        // here (≤ 707) is identical to 2025.
        case PID_SESSION: {
            if (length < 708) return {};
            SessionRow sr;
            sr.ts                         = timestamp;
            sr.weather                    = data[29];
            sr.track_temp                 = ReadInt8(data, 30);
            sr.air_temp                   = ReadInt8(data, 31);
            sr.total_laps                 = data[32];
            sr.track_length_m             = ReadUInt16(data, 33);
            sr.session_type               = data[35];
            sr.track_id                   = ReadInt8(data, 36);
            sr.formula                    = data[37];
            sr.session_time_left          = ReadUInt16(data, 38);
            sr.session_duration           = ReadUInt16(data, 40);
            sr.pit_speed_limit            = data[42];
            sr.num_marshal_zones          = data[47];

            for (int i = 0; i < (int)data[47] && i < 21; ++i) {
                int o = 48 + i * 5;
                sr.marshal_zones.push_back({ ReadFloat(data, o), ReadInt8(data, o + 4) });
            }

            sr.safety_car_status = data[153];
            for (int i = 0; i < (int)data[155] && i < 64; ++i) {
                int o = 156 + i * 8;
                sr.weather_forecast_samples.push_back({
                    ReadUInt8(data, o + 1), ReadUInt8(data, o + 2), ReadUInt8(data, o + 7)
                });
            }

            sr.forecast_accuracy          = data[668];
            sr.ai_difficulty              = data[669];
            sr.pit_stop_window_ideal_lap  = data[682];
            sr.pit_stop_window_latest_lap = data[683];
            sr.pit_stop_rejoin_position   = data[684];
            sr.time_of_day                = ReadUInt32(data, 696);
            sr.num_safety_car_periods     = data[705];
            sr.num_virtual_sc_periods     = data[706];
            sr.num_red_flag_periods       = data[707];
            // Active-aero (SLM) track status: 0 = Full, 1 = Partial. Sits after
            // the config flags, m_weekendStructure[12] and the two sectorN lap
            // distances (bytes 733..752), at byte 753.
            if (length >= 754) sr.active_aero_track_status = data[753];

            buf.clear();
            (void)glz::write_json(sr, buf);
            rows.push_back(std::move(buf));
            break;
        }

        // ── MOTION (60 Hz, glaze) ──────────────────────────────────────────
        // 2026: g-forces quantised to int16 (÷1000) → per-car struct 60 → 54.
        case PID_MOTION: {
            int motionSize = 54;
            int base = HEADER_SIZE + hdr.playerCarIndex * motionSize;
            if (length < base + motionSize) return {};

            int o = base + 36;
            MotionRow mr;
            mr.ts           = timestamp;
            mr.session_time = hdr.sessionTime;
            mr.player_idx   = hdr.playerCarIndex;
            mr.g_lat        = Round3(ReadInt16(data, o)     / 1000.0);
            mr.g_long       = Round3(ReadInt16(data, o + 2) / 1000.0);
            mr.g_vert       = Round3(ReadInt16(data, o + 4) / 1000.0);

            bin::encodeMotion(hot.binary, mr);
            if (hot.wantHotJson) {
                buf.clear(); (void)glz::write_json(mr, buf); hot.hotJson.push_back(std::move(buf));
            }

            if (hot.wants(13) && length >= HEADER_SIZE + MAX_CARS * motionSize) {
                PositionsRow pr;
                pr.ts         = timestamp;
                pr.player_idx = hdr.playerCarIndex;
                pr.cars.resize(MAX_CARS);
                for (int i = 0; i < MAX_CARS; ++i) {
                    int cBase = HEADER_SIZE + i * motionSize;
                    pr.cars[i].idx = i;
                    pr.cars[i].x   = Round2(ReadFloat(data, cBase));
                    pr.cars[i].z   = Round2(ReadFloat(data, cBase + 8));
                    pr.cars[i].g_lat = ReadInt16(data, cBase + 36) / 1000.0;
                    pr.cars[i].g_long = ReadInt16(data, cBase + 38) / 1000.0;
                    pr.cars[i].g_vert = ReadInt16(data, cBase + 40) / 1000.0;
                }
                bin::encodePositions(hot.binary, pr);
                if (hot.wantHotJson) {
                    buf.clear(); (void)glz::write_json(pr, buf); hot.hotJson.push_back(std::move(buf));
                }
            }
            break;
        }

        // ── LAP_DATA (2 Hz, glaze) ─────────────────────────────────────────
        case PID_LAP_DATA: {
            int lapSize = 57;
            int base = HEADER_SIZE + hdr.playerCarIndex * lapSize;
            if (length < base + lapSize) return {};

            int o = base;
            uint32_t lastLap = ReadUInt32(data, o); o += 4;
            uint32_t curLap  = ReadUInt32(data, o); o += 4;
            uint16_t s1H = ReadUInt16(data, o); o += 2;
            uint8_t  s1M = data[o++];
            uint16_t s2H = ReadUInt16(data, o); o += 2;
            uint8_t  s2M = data[o++];
            o += 3; o += 3;
            float lapDistance = ReadFloat(data, o); o += 4;
            o += 8;
            uint8_t position   = data[o++];
            uint8_t lapNum     = data[o++];
            uint8_t pitStatus  = data[o++];
            uint8_t numPits    = data[o++];
            uint8_t sector     = data[o++];
            bool    invalid    = data[o++] != 0;
            uint8_t penaltiesS = data[o];
            uint8_t driverStatus = data[o + 6];

            LapRow lr;
            lr.ts             = timestamp;
            lr.session_time   = hdr.sessionTime;
            lr.player_idx     = hdr.playerCarIndex;
            lr.last_lap_ms    = (int)lastLap;
            lr.current_lap_ms = (int)curLap;
            lr.lap_distance_m = lapDistance;
            lr.s1_ms          = s1M * 60000 + s1H;
            lr.s2_ms          = s2M * 60000 + s2H;
            lr.position       = position;
            lr.lap_num        = lapNum;
            lr.pit_status     = pitStatus;
            lr.num_pit_stops  = numPits;
            lr.sector         = sector;
            lr.lap_invalid    = invalid;
            lr.penalties_s    = penaltiesS;
            lr.driver_status  = driverStatus;

            buf.clear();
            (void)glz::write_json(lr, buf);
            rows.push_back(std::move(buf));

            if (hot.wants(7) && length >= HEADER_SIZE + MAX_CARS * lapSize) {
                TimingRow tr;
                tr.ts           = timestamp;
                tr.session_time = hdr.sessionTime;
                tr.player_idx   = hdr.playerCarIndex;
                tr.cars.resize(MAX_CARS);
                for (int i = 0; i < MAX_CARS; ++i) {
                    int cBase = HEADER_SIZE + i * lapSize;
                    int co = cBase;
                    uint32_t clast = ReadUInt32(data, co); co += 4;
                    uint32_t ccur  = ReadUInt32(data, co); co += 4;
                    uint16_t cs1H  = ReadUInt16(data, co); co += 2;
                    uint8_t  cs1M  = data[co++];
                    uint16_t cs2H  = ReadUInt16(data, co); co += 2;
                    uint8_t  cs2M  = data[co++];
                    co += 3;
                    uint16_t cgapH = ReadUInt16(data, co); co += 2;
                    uint8_t  cgapM = data[co++];
                    co += 12;
                    uint8_t cpos        = data[co++];
                    uint8_t clap        = data[co++];
                    uint8_t cpit        = data[co++];
                    uint8_t cnumPits    = data[co++];
                    uint8_t csect       = data[co++];
                    bool    cinvalid    = data[co++] != 0;
                    uint8_t cpen        = data[co++];
                    co += 2;
                    uint8_t cdt         = data[co++];
                    uint8_t csg         = data[co++];
                    co += 1;
                    uint8_t cdriverStat = data[co++];
                    uint8_t cresultStat = data[co];

                    TimingCar& tc    = tr.cars[i];
                    tc.idx           = i;
                    tc.position      = cpos;
                    tc.lap_num       = clap;
                    tc.current_lap_ms = (int)ccur;
                    tc.last_lap_ms   = (int)clast;
                    tc.s1_ms         = cs1M * 60000 + cs1H;
                    tc.s2_ms         = cs2M * 60000 + cs2H;
                    tc.gap_ms        = cgapM * 60000 + cgapH;
                    tc.pit_status    = cpit;
                    tc.num_pit_stops = cnumPits;
                    tc.lap_invalid   = cinvalid;
                    tc.penalties_s   = cpen;
                    tc.num_dt_pens   = cdt;
                    tc.num_sg_pens   = csg;
                    tc.sector        = csect;
                    tc.result_status = cresultStat;
                    tc.driver_status = cdriverStat;
                    if (const auto distance = FiniteFloat(data, cBase + 20))
                        tc.lap_distance_m = static_cast<float>(*distance);
                }
                buf.clear();
                (void)glz::write_json(tr, buf);
                rows.push_back(std::move(buf));
            }
            break;
        }

        // ── CAR_TEL (60 Hz, glaze) ─────────────────────────────────────────
        // 2026: m_engineTemperature uint16 → uint8 → per-car struct 60 → 59.
        case PID_CAR_TEL: {
            int telSize = 59;
            int base = HEADER_SIZE + hdr.playerCarIndex * telSize;
            if (length < base + telSize) return {};

            int o = base;
            TelemetryRow t;
            t.ts            = timestamp;
            t.session_time  = hdr.sessionTime;
            t.player_idx    = hdr.playerCarIndex;
            t.speed_kph     = ReadUInt16(data, o); o += 2;
            t.throttle      = ReadFloat(data, o);  o += 4;
            t.steering      = Round4(ReadFloat(data, o)); o += 4;
            t.brake         = ReadFloat(data, o);  o += 4;
            o += 1;
            t.gear          = ReadInt8(data, o);   o += 1;
            t.rpm           = ReadUInt16(data, o); o += 2;
            t.drs           = data[o++];   // raw DRS (0 under 2026 regs)
            t.rev_lights_pct       = data[o++];
            t.rev_lights_bit_value = ReadUInt16(data, o); o += 2;
            // 2026 wing-open state lives in its own field, sourced from active aero
            // (Car Telemetry 2, PID 16). Kept strictly separate from drs.
            t.slm           = s_activeAero[hdr.playerCarIndex] ? 1 : 0;
            t.brake_temp_rl = ReadUInt16(data, o); o += 2;
            t.brake_temp_rr = ReadUInt16(data, o); o += 2;
            t.brake_temp_fl = ReadUInt16(data, o); o += 2;
            t.brake_temp_fr = ReadUInt16(data, o); o += 2;
            t.tyre_temp_surface_rl = data[o++]; t.tyre_temp_surface_rr = data[o++];
            t.tyre_temp_surface_fl = data[o++]; t.tyre_temp_surface_fr = data[o++];
            t.tyre_temp_inner_rl   = data[o++]; t.tyre_temp_inner_rr   = data[o++];
            t.tyre_temp_inner_fl   = data[o++]; t.tyre_temp_inner_fr   = data[o++];
            t.engine_temp   = ReadUInt8(data, o);

            // CarTelemetryData: wheel order RL/RR/FL/FR. Only JSON recording
            // needs the all-driver array; the live binary player record is unchanged.
            if (hot.wantHotJson && length >= HEADER_SIZE + MAX_CARS * telSize + 3) {
                t.cars.emplace();
                for (int i = 0; i < MAX_CARS; ++i) {
                    if (!hot.hasCar(i, hdr.playerCarIndex)) continue;
                    const int cBase = HEADER_SIZE + i * telSize;
                    TelemetryCar car;
                    car.idx = i;
                    car.speed_kph = ReadUInt16(data, cBase);
                    if (const auto throttle = FiniteFloat(data, cBase + 2))
                        car.throttle = static_cast<float>(*throttle);
                    if (const auto brake = FiniteFloat(data, cBase + 10))
                        car.brake = static_cast<float>(*brake);
                    if (const auto steering = FiniteFloat(data, cBase + 6))
                        car.steering = Round4(*steering);
                    car.gear = ReadInt8(data, cBase + 15);
                    car.rpm = ReadUInt16(data, cBase + 16);
                    car.drs = ReadUInt8(data, cBase + 18);
                    car.slm = s_activeAero[i] ? 1 : 0;
                    car.rev_lights_pct = ReadUInt8(data, cBase + 19);
                    car.rev_lights_bit_value = ReadUInt16(data, cBase + 20);
                    car.brake_temp_rl = ReadUInt16(data, cBase + 22);
                    car.brake_temp_rr = ReadUInt16(data, cBase + 24);
                    car.brake_temp_fl = ReadUInt16(data, cBase + 26);
                    car.brake_temp_fr = ReadUInt16(data, cBase + 28);
                    car.tyre_temp_surface_rl = ReadUInt8(data, cBase + 30);
                    car.tyre_temp_surface_rr = ReadUInt8(data, cBase + 31);
                    car.tyre_temp_surface_fl = ReadUInt8(data, cBase + 32);
                    car.tyre_temp_surface_fr = ReadUInt8(data, cBase + 33);
                    car.tyre_temp_inner_rl = ReadUInt8(data, cBase + 34);
                    car.tyre_temp_inner_rr = ReadUInt8(data, cBase + 35);
                    car.tyre_temp_inner_fl = ReadUInt8(data, cBase + 36);
                    car.tyre_temp_inner_fr = ReadUInt8(data, cBase + 37);
                    car.engine_temp = ReadUInt8(data, cBase + 38);
                    t.cars->push_back(std::move(car));
                }
            }

            bin::encodeTelemetry(hot.binary, t);
            if (hot.wantHotJson) {
                buf.clear(); (void)glz::write_json(t, buf); hot.hotJson.push_back(std::move(buf));
            }
            break;
        }

        // ── CAR_STATUS (2 Hz, glaze) ───────────────────────────────────────
        // The parser caps live publication at 2 Hz but preserves every sample
        // for recording at the game's configured menu rate.
        case PID_CAR_STATUS: {
            int statusSize = 59;
            int base = HEADER_SIZE + hdr.playerCarIndex * statusSize;
            if (length < base + statusSize) return {};

            AllStatusCar sc = ParseStatusF126(data, base);

            StatusRow sr;
            sr.ts                   = timestamp;
            sr.session_time         = hdr.sessionTime;
            sr.player_idx           = hdr.playerCarIndex;
            sr.fuel_mix             = sc.fuel_mix;
            sr.front_brake_bias     = sc.front_brake_bias;
            sr.fuel_kg              = sc.fuel_kg;
            sr.fuel_laps            = sc.fuel_laps;
            sr.drs_allowed          = sc.drs_allowed;
            sr.tyre_compound        = sc.tyre_compound;
            sr.visual_compound      = sc.visual_compound;
            sr.tyre_age_laps        = sc.tyre_age_laps;
            sr.ers_j                = sc.ers_j;
            sr.ers_pct              = sc.ers_pct;
            sr.ers_mode             = sc.ers_mode;
            sr.ers_deployed_j       = sc.ers_deployed_j;
            sr.engine_power_ice_kw  = sc.engine_power_ice_kw;
            sr.engine_power_mguk_kw = sc.engine_power_mguk_kw;
            sr.ers_harvested_mguk_j = sc.ers_harvested_mguk_j;
            sr.ers_harvested_mguh_j = sc.ers_harvested_mguh_j;

            buf.clear();
            (void)glz::write_json(sr, buf);
            rows.push_back(std::move(buf));

            if (hot.wants(9) && length >= HEADER_SIZE + MAX_CARS * statusSize) {
                AllStatusRow ar;
                ar.ts           = timestamp;
                ar.session_time = hdr.sessionTime;
                ar.cars.resize(MAX_CARS);
                for (int i = 0; i < MAX_CARS; ++i) {
                    ar.cars[i] = ParseStatusF126(data, HEADER_SIZE + i * statusSize);
                    ar.cars[i].idx = i;
                }
                buf.clear();
                (void)glz::write_json(ar, buf);
                rows.push_back(std::move(buf));
            }
            break;
        }

        // ── CAR_DAMAGE (10 Hz, glaze) ──────────────────────────────────────
        // Damage struct unchanged at 46 bytes; only the car-array count grows.
        case PID_CAR_DAMAGE: {
            int damageSize = 46;
            int base = HEADER_SIZE + hdr.playerCarIndex * damageSize;
            if (length < base + damageSize) return {};

            int o = base;
            DamageRow dr;
            dr.ts             = timestamp;
            dr.session_time   = hdr.sessionTime;
            dr.player_idx     = hdr.playerCarIndex;
            dr.tyre_wear_rl   = Round1(ReadFloat(data, o)); o += 4;
            dr.tyre_wear_rr   = Round1(ReadFloat(data, o)); o += 4;
            dr.tyre_wear_fl   = Round1(ReadFloat(data, o)); o += 4;
            dr.tyre_wear_fr   = Round1(ReadFloat(data, o)); o += 4;
            dr.tyre_dmg_rl    = data[o++]; dr.tyre_dmg_rr = data[o++];
            dr.tyre_dmg_fl    = data[o++]; dr.tyre_dmg_fr = data[o++];
            dr.brake_dmg_rl   = data[o++]; dr.brake_dmg_rr = data[o++];
            dr.brake_dmg_fl   = data[o++]; dr.brake_dmg_fr = data[o++];
            dr.blisters_rl    = data[o++]; dr.blisters_rr = data[o++];
            dr.blisters_fl    = data[o++]; dr.blisters_fr = data[o++];
            dr.wing_fl        = data[o++]; dr.wing_fr   = data[o++]; dr.wing_rear = data[o++];
            dr.floor_damage   = data[o++]; dr.diffuser_damage = data[o++]; dr.sidepod_damage = data[o++];
            dr.drs_fault      = data[o++]; dr.ers_fault = data[o++];
            dr.gearbox_damage = data[o++]; dr.engine_damage = data[o++];

            // Restricted data (Your Telemetry): local car is exempt. Unknown
            // opponent access leaves wear unavailable instead of inventing zero.
            if (length >= HEADER_SIZE + MAX_CARS * damageSize) {
                dr.cars.emplace();
                for (int i = 0; i < MAX_CARS; ++i) {
                    if (!hot.hasCar(i, hdr.playerCarIndex)) continue;
                    TyreWearCar car;
                    car.idx = i;
                    // Blisters and ERS fault are not in the restricted-data list.
                    const int publicBase = HEADER_SIZE + i * damageSize;
                    car.blisters_rl = data[publicBase + 24]; car.blisters_rr = data[publicBase + 25];
                    car.blisters_fl = data[publicBase + 26]; car.blisters_fr = data[publicBase + 27];
                    car.ers_fault = data[publicBase + 35];
                    if (hot.hasPrivateTelemetry(i, hdr.playerCarIndex)) {
                        const int cBase = HEADER_SIZE + i * damageSize;
                        car.tyre_wear_rl = FiniteFloat(data, cBase);
                        car.tyre_wear_rr = FiniteFloat(data, cBase + 4);
                        car.tyre_wear_fl = FiniteFloat(data, cBase + 8);
                        car.tyre_wear_fr = FiniteFloat(data, cBase + 12);
                        car.tyre_dmg_rl = data[cBase + 16]; car.tyre_dmg_rr = data[cBase + 17];
                        car.tyre_dmg_fl = data[cBase + 18]; car.tyre_dmg_fr = data[cBase + 19];
                        car.brake_dmg_rl = data[cBase + 20]; car.brake_dmg_rr = data[cBase + 21];
                        car.brake_dmg_fl = data[cBase + 22]; car.brake_dmg_fr = data[cBase + 23];
                        car.wing_fl = data[cBase + 28]; car.wing_fr = data[cBase + 29]; car.wing_rear = data[cBase + 30];
                        car.floor_damage = data[cBase + 31]; car.diffuser_damage = data[cBase + 32]; car.sidepod_damage = data[cBase + 33];
                        car.drs_fault = data[cBase + 34];
                        car.gearbox_damage = data[cBase + 36]; car.engine_damage = data[cBase + 37];
                    }
                    dr.cars->push_back(std::move(car));
                }
            }

            buf.clear();
            (void)glz::write_json(dr, buf);
            rows.push_back(std::move(buf));
            break;
        }

        // ── PARTICIPANTS (0.2 Hz, glaze) ───────────────────────────────────
        // 2026: driverId/networkId/teamId widened uint8 → uint16, so per-car
        // struct 57 → 60 and the head offsets shift; the tail (name-relative)
        // is identical to 2025.
        case PID_PARTICIPANTS: {
            int partSize = 60;
            if (length < HEADER_SIZE + 1 + MAX_CARS * partSize) return {};
            ParticipantsRow pr;
            pr.num_active_cars = data[HEADER_SIZE];
            pr.player_idx = hdr.playerCarIndex;
            for (int i = 0; i < std::min(pr.num_active_cars, MAX_CARS); ++i) {
                int o = HEADER_SIZE + 1 + i * partSize;
                bool ai = data[o] != 0;
                uint16_t teamId  = ReadUInt16(data, o + 5);
                uint8_t  raceNum = data[o + 8];
                int nameStart = o + 10;
                std::string name = ReadString(data, nameStart, 32);
                const auto accessValue = data[nameStart + 32];
                const std::optional<int> access = accessValue <= 1
                    ? std::optional<int>(accessValue) : std::nullopt;
                if (hot.knownCars) (*hot.knownCars)[i] = !name.empty();
                if (hot.telemetryAccess) (*hot.telemetryAccess)[i] = name.empty() ? std::nullopt : access;
                if (name.empty()) continue;
                uint8_t numColors = data[nameStart + 37];
                char hexColor[16];
                if (numColors > 0) {
                    uint8_t r = data[nameStart + 38];
                    uint8_t g = data[nameStart + 39];
                    uint8_t b = data[nameStart + 40];
                    snprintf(hexColor, sizeof(hexColor), "#%02x%02x%02x", r, g, b);
                } else {
                    snprintf(hexColor, sizeof(hexColor), "#8e8e8e");
                }
                pr.drivers.push_back({ i, std::move(name), teamId, raceNum, ai,
                    resolveTeamColor(2026, teamId, hexColor, teamColorOverrides), hexColor });
                pr.drivers.back().your_telemetry = access;
            }
            if (hot.wants(8)) {
                buf.clear();
                (void)glz::write_json(pr, buf);
                rows.push_back(std::move(buf));
            }
            break;
        }

        // ── EVENT (rare, glaze) ────────────────────────────────────────────
        case PID_EVENT: {
            if (length < HEADER_SIZE + 4) return {};
            std::string code(reinterpret_cast<const char*>(data + HEADER_SIZE), 4);
            RaceEventRow ev;
            ev.ts           = timestamp;
            ev.session_time = hdr.sessionTime;
            ev.code         = code;
            int o = HEADER_SIZE + 4;
            if (code == "FTLP") {
                if (length < o + 5) return {};
                uint8_t vehicleIdx = data[o];
                float lapTimeS = Round3(ReadFloat(data, o + 1));
                FastestLapRow fl;
                fl.ts = timestamp; fl.car_idx = vehicleIdx; fl.lap_time_s = lapTimeS;
                buf.clear(); (void)glz::write_json(fl, buf); rows.push_back(std::move(buf));
                ev.car_idx = vehicleIdx; ev.lap_time_s = lapTimeS;
                buf.clear(); (void)glz::write_json(ev, buf); rows.push_back(std::move(buf));
            } else if (code == "DRSE" || code == "DRSD" || code == "RDFL" || code == "CHQF" ||
                       code == "LGOT" || code == "SSTA" || code == "SEND") {
                buf.clear(); (void)glz::write_json(ev, buf); rows.push_back(std::move(buf));
            } else if (code == "SCAR") {
                if (length < o + 2) return {};
                uint8_t scType = data[o], evType = data[o + 1];
                if (scType == 0) return {};
                ev.safety_car_type = scType; ev.event_type = evType;
                buf.clear(); (void)glz::write_json(ev, buf); rows.push_back(std::move(buf));
            } else if (code == "RTMT" || code == "RCWN") {
                if (length < o + 1) return {};
                ev.car_idx = data[o];
                buf.clear(); (void)glz::write_json(ev, buf); rows.push_back(std::move(buf));
            } else if (code == "PENA") {
                if (length < o + 7) return {};
                ev.car_idx = data[o + 2]; ev.penalty_type = data[o];
                ev.infringement_type = data[o + 1]; ev.penalty_time_s = data[o + 4];
                buf.clear(); (void)glz::write_json(ev, buf); rows.push_back(std::move(buf));
            } else if (code == "DTSV" || code == "SGSV") {
                if (length < o + 1) return {};
                ev.car_idx = data[o];
                buf.clear(); (void)glz::write_json(ev, buf); rows.push_back(std::move(buf));
            }
            break;
        }

        // ── SESSION_HISTORY (rare, glaze) ──────────────────────────────────
        case PID_SESSION_HISTORY: {
            if (length < HEADER_SIZE + 7) return {};
            uint8_t carIdx     = data[HEADER_SIZE];
            uint8_t bestLapNum = data[HEADER_SIZE + 3];
            SessionHistoryFastestRow sh;
            sh.ts = timestamp; sh.car_idx = carIdx;
            const int numLaps = std::min<int>(data[HEADER_SIZE + 1], 100);
            const int numStints = std::min<int>(data[HEADER_SIZE + 2], 8);
            for (int lap = 1; lap <= numLaps; ++lap) {
                const int o = HEADER_SIZE + 7 + (lap - 1) * 14;
                if (length < o + 14) break;
                const int lapMs = (int)ReadUInt32(data, o);
                const uint8_t valid = data[o + 13];
                sh.laps.push_back({lap, lapMs,
                    (int)ReadUInt16(data, o + 4) + (int)data[o + 6] * 60000,
                    (int)ReadUInt16(data, o + 7) + (int)data[o + 9] * 60000,
                    (int)ReadUInt16(data, o + 10) + (int)data[o + 12] * 60000,
                    (valid & 1) != 0, (valid & 2) != 0, (valid & 4) != 0, (valid & 8) != 0});
                if (lapMs > 0) { sh.latest_lap_num = lap; sh.latest_lap_time_ms = lapMs; }
            }
            if (bestLapNum > 0 && bestLapNum <= sh.laps.size())
                sh.best_lap_time_ms = sh.laps[bestLapNum - 1].lap_time_ms;
            const int stintBase = HEADER_SIZE + 7 + 100 * 14;
            for (int i = 0; i < numStints && length >= stintBase + (i + 1) * 3; ++i)
                sh.tyre_stints.push_back({data[stintBase + i * 3], data[stintBase + i * 3 + 1], data[stintBase + i * 3 + 2]});
            buf.clear();
            (void)glz::write_json(sh, buf);
            rows.push_back(std::move(buf));
            break;
        }

        // ── TYRE_SETS (rare, glaze) ────────────────────────────────────────
        case PID_TYRE_SETS: {
            if (length < 231) return {};
            uint8_t carIdx = data[HEADER_SIZE];
            TyreSetsRow tsr;
            tsr.ts = timestamp; tsr.session_time = hdr.sessionTime; tsr.car_idx = carIdx;
            for (int i = 0; i < 20; ++i) {
                int o = HEADER_SIZE + 1 + i * 10;
                tsr.sets.push_back({
                    i, data[o], data[o+1], data[o+2], data[o+3] == 1,
                    data[o+4], data[o+5], data[o+6], ReadInt16(data, o+7), data[o+9] == 1
                });
            }
            tsr.fitted_idx = data[230];
            buf.clear();
            (void)glz::write_json(tsr, buf);
            rows.push_back(std::move(buf));
            break;
        }

        // ── MOTION_EX (60 Hz, glaze) ───────────────────────────────────────
        // Unchanged in 2026 (player-only, still 273 bytes).
        case PID_MOTION_EX: {
            if (length < 225) return {};
            MotionExRow me;
            me.ts                   = timestamp;
            me.session_time         = hdr.sessionTime;
            me.player_idx           = hdr.playerCarIndex;
            me.front_aero_height_mm = Round2(ReadFloat(data, 217) * 1000.0);
            me.rear_aero_height_mm  = Round2(ReadFloat(data, 221) * 1000.0);
            bin::encodeMotionEx(hot.binary, me);
            if (hot.wantHotJson) {
                buf.clear(); (void)glz::write_json(me, buf); hot.hotJson.push_back(std::move(buf));
            }
            break;
        }

        // ── CAR_TEL2 (60 Hz, state only) ───────────────────────────────────
        // New in 2026. Per car 10 bytes; byte 0 is m_activeAeroMode. Cache every
        // car's wing state for the V6 telemetry array. Emits no
        // row of its own. Every packet reaches us at the game's configured rate.
        case PID_CAR_TEL2: {
            int tel2Size = 10;
            if (length < HEADER_SIZE + MAX_CARS * tel2Size) return {};
            for (int i = 0; i < MAX_CARS; ++i) {
                const uint8_t activeAeroMode = data[HEADER_SIZE + i * tel2Size];
                s_activeAero[i] = activeAeroMode == 1 ? 1 : 0;
            }
            break;
        }
    }

    return rows;
}
