#include "f1_26.h"
#include "tnrp/rows.h"
#include "tnrp/control_rows.h"
#include "tnrp/BinaryRows.h"
#include <cstdio>

using namespace tnrp;

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
// single thread, so the player's last-seen active-aero mode is cached here and
// surfaced on the telemetry row's dedicated `slm` field (separate from `drs`).
static uint8_t s_playerActiveAero = 0;

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

std::vector<std::string> F1_26::ParsePacket(const uint8_t* data, int length, const PacketHeader& hdr, const std::string& timestamp, HotOut& hot) {
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
            mr.g_lat        = Round3(ReadInt16(data, o)     / 1000.0);
            mr.g_long       = Round3(ReadInt16(data, o + 2) / 1000.0);
            mr.g_vert       = Round3(ReadInt16(data, o + 4) / 1000.0);

            bin::encodeMotion(hot.binary, mr);
            if (hot.wantHotJson) {
                buf.clear(); (void)glz::write_json(mr, buf); hot.hotJson.push_back(std::move(buf));
            }

            if (length >= HEADER_SIZE + MAX_CARS * motionSize) {
                PositionsRow pr;
                pr.ts         = timestamp;
                pr.player_idx = hdr.playerCarIndex;
                pr.cars.resize(MAX_CARS);
                for (int i = 0; i < MAX_CARS; ++i) {
                    int cBase = HEADER_SIZE + i * motionSize;
                    pr.cars[i].idx = i;
                    pr.cars[i].x   = Round2(ReadFloat(data, cBase));
                    pr.cars[i].z   = Round2(ReadFloat(data, cBase + 8));
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
            o += 3; o += 3; o += 12;
            uint8_t position   = data[o++];
            uint8_t lapNum     = data[o++];
            uint8_t pitStatus  = data[o++];
            uint8_t numPits    = data[o++];
            uint8_t sector     = data[o++];
            bool    invalid    = data[o++] != 0;
            uint8_t penaltiesS = data[o];

            LapRow lr;
            lr.ts             = timestamp;
            lr.session_time   = hdr.sessionTime;
            lr.last_lap_ms    = (int)lastLap;
            lr.current_lap_ms = (int)curLap;
            lr.s1_ms          = s1M * 60000 + s1H;
            lr.s2_ms          = s2M * 60000 + s2H;
            lr.position       = position;
            lr.lap_num        = lapNum;
            lr.pit_status     = pitStatus;
            lr.num_pit_stops  = numPits;
            lr.sector         = sector;
            lr.lap_invalid    = invalid;
            lr.penalties_s    = penaltiesS;

            buf.clear();
            (void)glz::write_json(lr, buf);
            rows.push_back(std::move(buf));

            if (length >= HEADER_SIZE + MAX_CARS * lapSize) {
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
            t.speed_kph     = ReadUInt16(data, o); o += 2;
            t.throttle      = ReadFloat(data, o);  o += 4;
            t.steering      = Round4(ReadFloat(data, o)); o += 4;
            t.brake         = ReadFloat(data, o);  o += 4;
            o += 1;
            t.gear          = ReadInt8(data, o);   o += 1;
            t.rpm           = ReadUInt16(data, o); o += 2;
            t.drs           = data[o++];   // raw DRS (0 under 2026 regs)
            // 2026 wing-open state lives in its own field, sourced from active aero
            // (Car Telemetry 2, PID 16). Kept strictly separate from drs.
            t.slm           = s_playerActiveAero ? 1 : 0;
            o += 1; o += 2;
            t.brake_temp_rl = ReadUInt16(data, o); o += 2;
            t.brake_temp_rr = ReadUInt16(data, o); o += 2;
            t.brake_temp_fl = ReadUInt16(data, o); o += 2;
            t.brake_temp_fr = ReadUInt16(data, o); o += 2;
            t.tyre_temp_surface_rl = data[o++]; t.tyre_temp_surface_rr = data[o++];
            t.tyre_temp_surface_fl = data[o++]; t.tyre_temp_surface_fr = data[o++];
            t.tyre_temp_inner_rl   = data[o++]; t.tyre_temp_inner_rr   = data[o++];
            t.tyre_temp_inner_fl   = data[o++]; t.tyre_temp_inner_fr   = data[o++];
            t.engine_temp   = ReadUInt8(data, o);

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

            if (length >= HEADER_SIZE + MAX_CARS * statusSize) {
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

        // ── CAR_DAMAGE (2 Hz, glaze) ───────────────────────────────────────
        // Damage struct unchanged at 46 bytes; only the car-array count grows.
        case PID_CAR_DAMAGE: {
            int damageSize = 46;
            int base = HEADER_SIZE + hdr.playerCarIndex * damageSize;
            if (length < base + damageSize) return {};

            int o = base;
            DamageRow dr;
            dr.ts             = timestamp;
            dr.session_time   = hdr.sessionTime;
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
            dr.floor_damage   = data[o++]; dr.sidepod_damage = data[o++]; dr.diffuser_damage = data[o++];
            dr.drs_fault      = data[o++]; dr.ers_fault = data[o++];
            dr.gearbox_damage = data[o++]; dr.engine_damage = data[o++];

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
            for (int i = 0; i < MAX_CARS; ++i) {
                int o = HEADER_SIZE + 1 + i * partSize;
                bool ai = data[o] != 0;
                uint16_t teamId  = ReadUInt16(data, o + 5);
                uint8_t  raceNum = data[o + 8];
                int nameStart = o + 10;
                std::string name = ReadString(data, nameStart, 32);
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
                pr.drivers.push_back({ i, std::move(name), teamId, raceNum, ai, hexColor });
            }
            buf.clear();
            (void)glz::write_json(pr, buf);
            rows.push_back(std::move(buf));
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
            if (bestLapNum == 0) return {};
            int lapOff = HEADER_SIZE + 7 + (bestLapNum - 1) * 14;
            if (length < lapOff + 14) return {};
            if ((data[lapOff + 13] & 0x01) == 0) return {};
            SessionHistoryFastestRow sh;
            sh.ts = timestamp; sh.car_idx = carIdx;
            sh.best_lap_time_ms = ReadUInt32(data, lapOff);
            buf.clear();
            (void)glz::write_json(sh, buf);
            rows.push_back(std::move(buf));
            break;
        }

        // ── TYRE_SETS (rare, glaze) ────────────────────────────────────────
        case PID_TYRE_SETS: {
            if (length < 231) return {};
            uint8_t carIdx = data[HEADER_SIZE];
            if (carIdx != hdr.playerCarIndex) return {};
            TyreSetsRow tsr;
            tsr.ts = timestamp; tsr.session_time = hdr.sessionTime;
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
            me.front_aero_height_mm = Round2(ReadFloat(data, 217) * 1000.0);
            me.rear_aero_height_mm  = Round2(ReadFloat(data, 221) * 1000.0);
            bin::encodeMotionEx(hot.binary, me);
            if (hot.wantHotJson) {
                buf.clear(); (void)glz::write_json(me, buf); hot.hotJson.push_back(std::move(buf));
            }
            break;
        }

        // ── CAR_TEL2 (60 Hz, state only) ───────────────────────────────────
        // New in 2026. Per car 10 bytes; byte 0 is m_activeAeroMode. We only need
        // the player's wing state, cached for the telemetry row above. Emits no
        // row of its own. PID 16 sits outside the rate-limit table, so every
        // packet reaches us each frame.
        case PID_CAR_TEL2: {
            int tel2Size = 10;
            int base = HEADER_SIZE + hdr.playerCarIndex * tel2Size;
            if (length < base + tel2Size) return {};
            uint8_t activeAeroMode = data[base];   // 0 = Corner, 1 = Straight
            s_playerActiveAero = (activeAeroMode == 1) ? 1 : 0;
            break;
        }
    }

    return rows;
}
