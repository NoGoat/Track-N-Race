#include "f1_24.h"
#include "tnrp/rows.h"

static const int HEADER_SIZE = 29;

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

static const std::unordered_map<int, std::string> F1_24_TEAM_COLORS = {
    {0, "#27f4d2"}, {1, "#e80020"}, {2, "#3671c6"}, {3, "#64c4ff"},
    {4, "#229971"}, {5, "#0093cc"}, {6, "#6692ff"}, {7, "#e6002b"},
    {8, "#ff8000"}, {9, "#52e252"}, {41, "#8e8e8e"}, {104, "#8e8e8e"},
    {143, "#ecebeb"}, {144, "#ff4646"}, {145, "#005aff"}, {146, "#1b2c56"},
    {147, "#39ff14"}, {148, "#ff3c00"}, {149, "#ff7c00"}, {150, "#ff2828"},
    {151, "#0028ff"}, {152, "#ffb400"}, {153, "#ffff00"}
};

static AllStatusCar ParseStatusF124(const uint8_t* data, int base) {
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

std::vector<std::string> F1_24::ParsePacket(const uint8_t* data, int length, const PacketHeader& hdr, const std::string& timestamp) {
    std::vector<std::string> rows;
    std::string buf;

    switch (hdr.packetId) {

        // ── SESSION (complex, keep nlohmann) ──────────────────────────────
        case PID_SESSION: {
            if (length < 708) return {};
            uint8_t weather = data[29];
            int8_t trackTemp = ReadInt8(data, 30);
            int8_t airTemp = ReadInt8(data, 31);
            uint8_t totalLaps = data[32];
            uint16_t trackLengthM = ReadUInt16(data, 33);
            uint8_t sessionType = data[35];
            int8_t trackId = ReadInt8(data, 36);
            uint16_t sessionTimeLeft = ReadUInt16(data, 38);
            uint16_t sessionDuration = ReadUInt16(data, 40);
            uint8_t pitSpeedLimit = data[42];
            uint8_t numMarshalZones = data[47];

            nlohmann::json marshalZones = nlohmann::json::array();
            for (int i = 0; i < numMarshalZones && i < 21; ++i) {
                int o = 48 + i * 5;
                marshalZones.push_back({{"zone_start", ReadFloat(data, o)}, {"flag", ReadInt8(data, o + 4)}});
            }

            uint8_t safetyCarStatus = data[153];
            uint8_t numForecastSamples = data[155];

            nlohmann::json weatherForecast = nlohmann::json::array();
            for (int i = 0; i < numForecastSamples && i < 64; ++i) {
                int o = 156 + i * 8;
                weatherForecast.push_back({
                    {"time_offset", ReadUInt8(data, o + 1)},
                    {"weather", ReadUInt8(data, o + 2)},
                    {"rain_percentage", ReadUInt8(data, o + 7)}
                });
            }

            uint8_t forecastAccuracy = data[668];
            uint8_t aiDifficulty = data[669];
            uint8_t pitStopWindowIdealLap = data[682];
            uint8_t pitStopWindowLatestLap = data[683];
            uint8_t pitStopRejoinPosition = data[684];
            uint32_t timeOfDay = ReadUInt32(data, 696);
            uint8_t numSafetyCarPeriods = data[705];
            uint8_t numVirtualScPeriods = data[706];
            uint8_t numRedFlagPeriods = data[707];

            nlohmann::json row = {
                {"type", "session"}, {"ts", timestamp},
                {"weather", weather}, {"track_temp", trackTemp}, {"air_temp", airTemp},
                {"track_length_m", trackLengthM},
                {"track_id", trackId}, {"session_type", sessionType},
                {"total_laps", totalLaps}, {"session_time_left", sessionTimeLeft},
                {"session_duration", sessionDuration}, {"pit_speed_limit", pitSpeedLimit},
                {"pit_stop_window_ideal_lap", pitStopWindowIdealLap},
                {"pit_stop_window_latest_lap", pitStopWindowLatestLap},
                {"pit_stop_rejoin_position", pitStopRejoinPosition},
                {"num_marshal_zones", numMarshalZones}, {"marshal_zones", marshalZones},
                {"weather_forecast_samples", weatherForecast},
                {"safety_car_status", safetyCarStatus},
                {"forecast_accuracy", forecastAccuracy},
                {"ai_difficulty", aiDifficulty},
                {"time_of_day", timeOfDay},
                {"num_safety_car_periods", numSafetyCarPeriods},
                {"num_virtual_sc_periods", numVirtualScPeriods},
                {"num_red_flag_periods", numRedFlagPeriods}
            };
            rows.push_back(row.dump());
            break;
        }

        // ── MOTION (60 Hz, glaze) ──────────────────────────────────────────
        case PID_MOTION: {
            int motionSize = 60;
            int base = HEADER_SIZE + hdr.playerCarIndex * motionSize;
            if (length < base + motionSize) return {};

            int o = base + 36;
            MotionRow mr;
            mr.ts           = timestamp;
            mr.session_time = hdr.sessionTime;
            mr.g_lat        = Round3(ReadFloat(data, o));
            mr.g_long       = Round3(ReadFloat(data, o + 4));
            mr.g_vert       = Round3(ReadFloat(data, o + 8));

            buf.clear();
            (void)glz::write_json(mr, buf);
            rows.push_back(std::move(buf));

            if (length >= HEADER_SIZE + 22 * motionSize) {
                PositionsRow pr;
                pr.ts         = timestamp;
                pr.player_idx = hdr.playerCarIndex;
                pr.cars.resize(22);
                for (int i = 0; i < 22; ++i) {
                    int cBase = HEADER_SIZE + i * motionSize;
                    pr.cars[i].idx = i;
                    pr.cars[i].x   = Round2(ReadFloat(data, cBase));
                    pr.cars[i].z   = Round2(ReadFloat(data, cBase + 8));
                }
                buf.clear();
                (void)glz::write_json(pr, buf);
                rows.push_back(std::move(buf));
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

            if (length >= HEADER_SIZE + 22 * lapSize) {
                TimingRow tr;
                tr.ts           = timestamp;
                tr.session_time = hdr.sessionTime;
                tr.player_idx   = hdr.playerCarIndex;
                tr.cars.resize(22);
                for (int i = 0; i < 22; ++i) {
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

                    TimingCar& tc   = tr.cars[i];
                    tc.idx          = i;
                    tc.position     = cpos;
                    tc.lap_num      = clap;
                    tc.current_lap_ms = (int)ccur;
                    tc.last_lap_ms  = (int)clast;
                    tc.s1_ms        = cs1M * 60000 + cs1H;
                    tc.s2_ms        = cs2M * 60000 + cs2H;
                    tc.gap_ms       = cgapM * 60000 + cgapH;
                    tc.pit_status   = cpit;
                    tc.num_pit_stops = cnumPits;
                    tc.lap_invalid  = cinvalid;
                    tc.penalties_s  = cpen;
                    tc.num_dt_pens  = cdt;
                    tc.num_sg_pens  = csg;
                    tc.sector       = csect;
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
        case PID_CAR_TEL: {
            int telSize = 60;
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
            t.drs           = data[o++];
            o += 1; o += 2;
            t.brake_temp_rl = ReadUInt16(data, o); o += 2;
            t.brake_temp_rr = ReadUInt16(data, o); o += 2;
            t.brake_temp_fl = ReadUInt16(data, o); o += 2;
            t.brake_temp_fr = ReadUInt16(data, o); o += 2;
            t.tyre_temp_surface_rl = data[o++]; t.tyre_temp_surface_rr = data[o++];
            t.tyre_temp_surface_fl = data[o++]; t.tyre_temp_surface_fr = data[o++];
            t.tyre_temp_inner_rl   = data[o++]; t.tyre_temp_inner_rr   = data[o++];
            t.tyre_temp_inner_fl   = data[o++]; t.tyre_temp_inner_fr   = data[o++];
            t.engine_temp   = ReadUInt16(data, o);

            buf.clear();
            (void)glz::write_json(t, buf);
            rows.push_back(std::move(buf));
            break;
        }

        // ── CAR_STATUS (2 Hz, glaze) ───────────────────────────────────────
        case PID_CAR_STATUS: {
            int statusSize = 55;
            int base = HEADER_SIZE + hdr.playerCarIndex * statusSize;
            if (length < base + statusSize) return {};

            AllStatusCar sc = ParseStatusF124(data, base);

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

            if (length >= HEADER_SIZE + 22 * statusSize) {
                AllStatusRow ar;
                ar.ts           = timestamp;
                ar.session_time = hdr.sessionTime;
                ar.cars.resize(22);
                for (int i = 0; i < 22; ++i) {
                    ar.cars[i] = ParseStatusF124(data, HEADER_SIZE + i * statusSize);
                    ar.cars[i].idx = i;
                }
                buf.clear();
                (void)glz::write_json(ar, buf);
                rows.push_back(std::move(buf));
            }
            break;
        }

        // ── CAR_DAMAGE (2 Hz, glaze) ───────────────────────────────────────
        case PID_CAR_DAMAGE: {
            int damageSize = 42;
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
            // F1_24 has no blisters — stays zero-initialised
            dr.wing_fl        = data[o++]; dr.wing_fr   = data[o++]; dr.wing_rear = data[o++];
            dr.floor_damage   = data[o++]; dr.sidepod_damage = data[o++]; dr.diffuser_damage = data[o++];
            dr.drs_fault      = data[o++]; dr.ers_fault = data[o++];
            dr.gearbox_damage = data[o++]; dr.engine_damage = data[o++];

            buf.clear();
            (void)glz::write_json(dr, buf);
            rows.push_back(std::move(buf));
            break;
        }

        // ── PARTICIPANTS (0.2 Hz, keep nlohmann) ──────────────────────────
        case PID_PARTICIPANTS: {
            int partSize = 60;
            if (length < HEADER_SIZE + 1 + 22 * partSize) return {};
            nlohmann::json drivers = nlohmann::json::array();
            for (int i = 0; i < 22; ++i) {
                int o = HEADER_SIZE + 1 + i * partSize;
                bool ai = data[o] != 0; o += 1;
                o += 2;
                uint8_t teamId  = data[o]; o += 1;
                o += 1;
                uint8_t raceNum = data[o]; o += 1;
                o += 1;
                int nameStart = o;
                std::string name = ReadString(data, nameStart, 48);
                if (name.empty()) continue;
                std::string liveryColor = "#8e8e8e";
                auto it = F1_24_TEAM_COLORS.find(teamId);
                if (it != F1_24_TEAM_COLORS.end()) liveryColor = it->second;
                drivers.push_back({
                    {"idx", i}, {"name", name}, {"team_id", teamId},
                    {"race_number", raceNum}, {"ai", ai}, {"livery_color", liveryColor}
                });
            }
            rows.push_back(nlohmann::json{{"type", "participants"}, {"drivers", drivers}}.dump());
            break;
        }

        // ── EVENT (rare, keep nlohmann) ────────────────────────────────────
        case PID_EVENT: {
            if (length < HEADER_SIZE + 4) return {};
            std::string code(reinterpret_cast<const char*>(data + HEADER_SIZE), 4);
            nlohmann::json base = {
                {"type", "race_event"}, {"ts", timestamp},
                {"session_time", hdr.sessionTime}, {"code", code}
            };
            int o = HEADER_SIZE + 4;
            if (code == "FTLP") {
                if (length < o + 5) return {};
                uint8_t vehicleIdx = data[o];
                float lapTimeS = Round3(ReadFloat(data, o + 1));
                rows.push_back(nlohmann::json{{"type","fastest_lap"},{"ts",timestamp},{"car_idx",vehicleIdx},{"lap_time_s",lapTimeS}}.dump());
                nlohmann::json ev = base;
                ev["car_idx"] = vehicleIdx; ev["lap_time_s"] = lapTimeS;
                rows.push_back(ev.dump());
            } else if (code == "DRSE" || code == "DRSD" || code == "RDFL" || code == "CHQF" ||
                       code == "LGOT" || code == "SSTA" || code == "SEND") {
                rows.push_back(base.dump());
            } else if (code == "SCAR") {
                if (length < o + 2) return {};
                uint8_t scType = data[o], evType = data[o + 1];
                if (scType == 0) return {};
                nlohmann::json ev = base;
                ev["safety_car_type"] = scType; ev["event_type"] = evType;
                rows.push_back(ev.dump());
            } else if (code == "RTMT" || code == "RCWN") {
                if (length < o + 1) return {};
                nlohmann::json ev = base; ev["car_idx"] = data[o];
                rows.push_back(ev.dump());
            } else if (code == "PENA") {
                if (length < o + 7) return {};
                nlohmann::json ev = base;
                ev["car_idx"] = data[o + 2]; ev["penalty_type"] = data[o];
                ev["infringement_type"] = data[o + 1]; ev["penalty_time_s"] = data[o + 4];
                rows.push_back(ev.dump());
            } else if (code == "DTSV" || code == "SGSV") {
                if (length < o + 1) return {};
                nlohmann::json ev = base; ev["car_idx"] = data[o];
                rows.push_back(ev.dump());
            }
            break;
        }

        // ── SESSION_HISTORY (rare, keep nlohmann) ──────────────────────────
        case PID_SESSION_HISTORY: {
            if (length < HEADER_SIZE + 7) return {};
            uint8_t carIdx     = data[HEADER_SIZE];
            uint8_t bestLapNum = data[HEADER_SIZE + 3];
            if (bestLapNum == 0) return {};
            int lapOff = HEADER_SIZE + 7 + (bestLapNum - 1) * 14;
            if (length < lapOff + 14) return {};
            if ((data[lapOff + 13] & 0x01) == 0) return {};
            uint32_t bestLapTimeMs = ReadUInt32(data, lapOff);
            rows.push_back(nlohmann::json{
                {"type","session_history_fastest"},{"ts",timestamp},
                {"car_idx",carIdx},{"best_lap_time_ms",bestLapTimeMs}
            }.dump());
            break;
        }

        // ── TYRE_SETS (rare, keep nlohmann) ────────────────────────────────
        case PID_TYRE_SETS: {
            if (length < 231) return {};
            uint8_t carIdx = data[HEADER_SIZE];
            if (carIdx != hdr.playerCarIndex) return {};
            nlohmann::json sets = nlohmann::json::array();
            for (int i = 0; i < 20; ++i) {
                int o = HEADER_SIZE + 1 + i * 10;
                sets.push_back({
                    {"idx", i}, {"actual_compound", data[o]}, {"visual_compound", data[o+1]},
                    {"wear", data[o+2]}, {"available", data[o+3] == 1},
                    {"recommended_session", data[o+4]}, {"life_span", data[o+5]},
                    {"usable_life", data[o+6]}, {"lap_delta_ms", ReadInt16(data, o+7)},
                    {"fitted", data[o+9] == 1}
                });
            }
            rows.push_back(nlohmann::json{
                {"type","tyre_sets"},{"ts",timestamp},{"session_time",hdr.sessionTime},
                {"sets",sets},{"fitted_idx",data[230]}
            }.dump());
            break;
        }

        // ── MOTION_EX (60 Hz, glaze) ───────────────────────────────────────
        case PID_MOTION_EX: {
            if (length < 225) return {};
            MotionExRow me;
            me.ts                  = timestamp;
            me.session_time        = hdr.sessionTime;
            me.front_aero_height_mm = Round2(ReadFloat(data, 217) * 1000.0);
            me.rear_aero_height_mm  = Round2(ReadFloat(data, 221) * 1000.0);
            buf.clear();
            (void)glz::write_json(me, buf);
            rows.push_back(std::move(buf));
            break;
        }
    }

    return rows;
}
