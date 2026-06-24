#include "f1_24.h"

// Telemetry parsing constants
static const int HEADER_SIZE = 29;

// Packet IDs
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

static nlohmann::json ParseStatusF124(const uint8_t* data, int base, const PacketHeader& hdr) {

    int o = base;
    o += 2;
    uint8_t fuelMix = data[o++];
    uint8_t brakeBias = data[o++];
    o += 1;
    float fuelKg = ReadFloat(data, o); o += 4;
    o += 4;
    float fuelLaps = ReadFloat(data, o); o += 4;
    o += 2;
    o += 2;
    o += 1;
    bool drsAllowed = data[o++] != 0;
    o += 2;
    uint8_t tyreCompound = data[o++];
    uint8_t visualCompound = data[o++];
    uint8_t tyreAgeLaps = data[o++];
    o += 1;
    float enginePowerICE = ReadFloat(data, o); o += 4;
    float enginePowerMGUK = ReadFloat(data, o); o += 4;
    float ersJ = ReadFloat(data, o); o += 4;
    uint8_t ersMode = data[o++];
    float ersHarvestedMGUK = ReadFloat(data, o); o += 4;
    float ersHarvestedMGUH = ReadFloat(data, o); o += 4;
    float ersDeployedJ = ReadFloat(data, o);

    double ersPct = Round1(ersJ / 40000.0);

    return {
        {"fuel_mix", fuelMix},
        {"front_brake_bias", brakeBias},
        {"fuel_kg", Round2(fuelKg)},
        {"fuel_laps", Round1(fuelLaps)},
        {"drs_allowed", drsAllowed},
        {"tyre_compound", tyreCompound},
        {"visual_compound", visualCompound},
        {"tyre_age_laps", tyreAgeLaps},
        {"ers_j", (int)std::round(ersJ)},
        {"ers_pct", ersPct},
        {"ers_mode", ersMode},
        {"ers_deployed_j", (int)std::round(ersDeployedJ)},
        {"engine_power_ice_kw", Round1(enginePowerICE / 1000.0)},
        {"engine_power_mguk_kw", Round1(enginePowerMGUK / 1000.0)},
        {"ers_harvested_mguk_j", (int)std::round(ersHarvestedMGUK)},
        {"ers_harvested_mguh_j", (int)std::round(ersHarvestedMGUH)}
    };
}

std::vector<nlohmann::json> F1_24::ParsePacket(const uint8_t* data, int length, const PacketHeader& hdr, const std::string& timestamp) {
    std::vector<nlohmann::json> rows;

    switch (hdr.packetId) {
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
            rows.push_back(row);
            break;
        }
        case PID_MOTION: {
            int motionSize = 60;
            int base = HEADER_SIZE + hdr.playerCarIndex * motionSize;
            if (length < base + motionSize) return {};

            int o = base + 36;
            float gLat = ReadFloat(data, o);
            float gLong = ReadFloat(data, o + 4);
            float gVert = ReadFloat(data, o + 8);

            nlohmann::json row = {
                {"type", "motion"}, {"ts", timestamp}, {"session_time", hdr.sessionTime},
                {"g_lat", Round3(gLat)}, {"g_long", Round3(gLong)}, {"g_vert", Round3(gVert)}
            };
            rows.push_back(row);

            // allMotion x,z positions
            if (length >= HEADER_SIZE + 22 * motionSize) {
                nlohmann::json cars = nlohmann::json::array();
                for (int i = 0; i < 22; ++i) {
                    int cBase = HEADER_SIZE + i * motionSize;
                    float x = ReadFloat(data, cBase);
                    float z = ReadFloat(data, cBase + 8);
                    cars.push_back({{"idx", i}, {"x", Round2(x)}, {"z", Round2(z)}});
                }
                nlohmann::json posRow = {
                    {"type", "positions"}, {"ts", timestamp},
                    {"player_idx", hdr.playerCarIndex}, {"cars", cars}
                };
                rows.push_back(posRow);
            }
            break;
        }
        case PID_LAP_DATA: {
            int lapSize = 57;
            int base = HEADER_SIZE + hdr.playerCarIndex * lapSize;
            if (length < base + lapSize) return {};

            // player lap
            int o = base;
            uint32_t lastLap = ReadUInt32(data, o); o += 4;
            uint32_t curLap = ReadUInt32(data, o); o += 4;
            uint16_t s1H = ReadUInt16(data, o); o += 2;
            uint8_t s1M = data[o++];
            uint16_t s2H = ReadUInt16(data, o); o += 2;
            uint8_t s2M = data[o++];
            o += 3; // skip
            o += 3; // skip
            o += 12; // skip
            uint8_t position = data[o++];
            uint8_t lapNum = data[o++];
            uint8_t pitStatus = data[o++];
            uint8_t numPits = data[o++];
            uint8_t sector = data[o++];
            bool invalid = data[o++] != 0;
            uint8_t penaltiesS = data[o];

            nlohmann::json row = {
                {"type", "lap"}, {"ts", timestamp}, {"session_time", hdr.sessionTime},
                {"last_lap_ms", lastLap}, {"current_lap_ms", curLap},
                {"s1_ms", s1M * 60000 + s1H}, {"s2_ms", s2M * 60000 + s2H},
                {"position", position}, {"lap_num", lapNum},
                {"pit_status", pitStatus}, {"num_pit_stops", numPits},
                {"sector", sector}, {"lap_invalid", invalid}, {"penalties_s", penaltiesS}
            };
            rows.push_back(row);

            // allCars timing
            if (length >= HEADER_SIZE + 22 * lapSize) {
                nlohmann::json cars = nlohmann::json::array();
                for (int i = 0; i < 22; ++i) {
                    int cBase = HEADER_SIZE + i * lapSize;
                    int co = cBase;
                    uint32_t clast = ReadUInt32(data, co); co += 4;
                    uint32_t ccur = ReadUInt32(data, co); co += 4;
                    uint16_t cs1H = ReadUInt16(data, co); co += 2;
                    uint8_t cs1M = data[co++];
                    uint16_t cs2H = ReadUInt16(data, co); co += 2;
                    uint8_t cs2M = data[co++];
                    co += 3;
                    uint16_t cgapH = ReadUInt16(data, co); co += 2;
                    uint8_t cgapM = data[co++];
                    co += 12;
                    uint8_t cpos = data[co++];
                    uint8_t clap = data[co++];
                    uint8_t cpit = data[co++];
                    uint8_t cnumPits = data[co++];
                    uint8_t csect = data[co++];
                    bool cinvalid = data[co++] != 0;
                    uint8_t cpen = data[co++];
                    co += 2; // skip
                    uint8_t cdt = data[co++];
                    uint8_t csg = data[co++];
                    co += 1;
                    uint8_t cdriverStat = data[co++];
                    uint8_t cresultStat = data[co];

                    cars.push_back({
                        {"idx", i}, {"position", cpos}, {"lap_num", clap},
                        {"current_lap_ms", ccur}, {"last_lap_ms", clast},
                        {"s1_ms", cs1M * 60000 + cs1H}, {"s2_ms", cs2M * 60000 + cs2H},
                        {"gap_ms", cgapM * 60000 + cgapH}, {"pit_status", cpit},
                        {"num_pit_stops", cnumPits},
                        {"lap_invalid", cinvalid}, {"penalties_s", cpen},
                        {"num_dt_pens", cdt}, {"num_sg_pens", csg}, {"sector", csect},
                        {"result_status", cresultStat}, {"driver_status", cdriverStat}
                    });
                }
                nlohmann::json timingRow = {
                    {"type", "timing"}, {"ts", timestamp}, {"session_time", hdr.sessionTime},
                    {"player_idx", hdr.playerCarIndex}, {"cars", cars}
                };
                rows.push_back(timingRow);
            }
            break;
        }
        case PID_CAR_TEL: {
            int telSize = 60;
            int base = HEADER_SIZE + hdr.playerCarIndex * telSize;
            if (length < base + telSize) return {};

            int o = base;
            uint16_t speed = ReadUInt16(data, o); o += 2;
            float throt = ReadFloat(data, o); o += 4;
            float steer = ReadFloat(data, o); o += 4;
            float brake = ReadFloat(data, o); o += 4;
            o += 1; // skip clutch
            int8_t gear = ReadInt8(data, o); o += 1;
            uint16_t rpm = ReadUInt16(data, o); o += 2;
            uint8_t drs = data[o++];
            o += 1; // skip revLightsPct
            o += 2; // skip revLightsBitField
            uint16_t btRL = ReadUInt16(data, o); o += 2;
            uint16_t btRR = ReadUInt16(data, o); o += 2;
            uint16_t btFL = ReadUInt16(data, o); o += 2;
            uint16_t btFR = ReadUInt16(data, o); o += 2;
            uint8_t stRL = data[o++]; uint8_t stRR = data[o++]; uint8_t stFL = data[o++]; uint8_t stFR = data[o++];
            uint8_t itRL = data[o++]; uint8_t itRR = data[o++]; uint8_t itFL = data[o++]; uint8_t itFR = data[o++];
            uint16_t engT = ReadUInt16(data, o);

            nlohmann::json row = {
                {"type", "telemetry"}, {"ts", timestamp}, {"session_time", hdr.sessionTime},
                {"speed_kph", speed}, {"rpm", rpm}, {"gear", gear},
                {"throttle", throt}, {"brake", brake}, {"steering", Round4(steer)}, {"drs", drs},
                {"tyre_temp_surface_rl", stRL}, {"tyre_temp_surface_rr", stRR},
                {"tyre_temp_surface_fl", stFL}, {"tyre_temp_surface_fr", stFR},
                {"tyre_temp_inner_rl", itRL}, {"tyre_temp_inner_rr", itRR},
                {"tyre_temp_inner_fl", itFL}, {"tyre_temp_inner_fr", itFR},
                {"brake_temp_rl", btRL}, {"brake_temp_rr", btRR},
                {"brake_temp_fl", btFL}, {"brake_temp_fr", btFR},
                {"engine_temp", engT}
            };
            rows.push_back(row);
            break;
        }
        case PID_CAR_STATUS: {
            int statusSize = 55;
            int base = HEADER_SIZE + hdr.playerCarIndex * statusSize;
            if (length < base + statusSize) return {};

            nlohmann::json row = ParseStatusF124(data, base, hdr);
            row["type"] = "status";
            row["ts"] = timestamp;
            row["session_time"] = hdr.sessionTime;
            rows.push_back(row);

            // allStatus
            if (length >= HEADER_SIZE + 22 * statusSize) {
                nlohmann::json cars = nlohmann::json::array();
                for (int i = 0; i < 22; ++i) {
                    nlohmann::json cStat = ParseStatusF124(data, HEADER_SIZE + i * statusSize, hdr);
                    cStat["idx"] = i;
                    cars.push_back(cStat);
                }
                nlohmann::json allRow = {
                    {"type", "all_status"}, {"ts", timestamp},
                    {"session_time", hdr.sessionTime}, {"cars", cars}
                };
                rows.push_back(allRow);
            }
            break;
        }
        case PID_CAR_DAMAGE: {
            int damageSize = 42;
            int base = HEADER_SIZE + hdr.playerCarIndex * damageSize;
            if (length < base + damageSize) return {};

            int o = base;
            float wRL = ReadFloat(data, o); o += 4;
            float wRR = ReadFloat(data, o); o += 4;
            float wFL = ReadFloat(data, o); o += 4;
            float wFR = ReadFloat(data, o); o += 4;
            uint8_t tdRL = data[o++]; uint8_t tdRR = data[o++]; uint8_t tdFL = data[o++]; uint8_t tdFR = data[o++];
            uint8_t brRL = data[o++]; uint8_t brRR = data[o++]; uint8_t brFL = data[o++]; uint8_t brFR = data[o++];
            uint8_t wingFL = data[o++]; uint8_t wingFR = data[o++]; uint8_t wingRear = data[o++];
            uint8_t floorDmg = data[o++]; uint8_t carpetDmg = data[o++]; uint8_t diffuserDmg = data[o++];
            // CarDamageData order here is ...sidepod, drsFault, ersFault, gearBox, engine.
            uint8_t drsFault = data[o++]; uint8_t ersFault = data[o++];
            uint8_t gearboxDmg = data[o++]; uint8_t engineDmg = data[o++];

            nlohmann::json row = {
                {"type", "damage"}, {"ts", timestamp}, {"session_time", hdr.sessionTime},
                {"tyre_wear_rl", Round1(wRL)}, {"tyre_wear_rr", Round1(wRR)},
                {"tyre_wear_fl", Round1(wFL)}, {"tyre_wear_fr", Round1(wFR)},
                {"tyre_dmg_rl", tdRL}, {"tyre_dmg_rr", tdRR}, {"tyre_dmg_fl", tdFL}, {"tyre_dmg_fr", tdFR},
                {"brake_dmg_rl", brRL}, {"brake_dmg_rr", brRR}, {"brake_dmg_fl", brFL}, {"brake_dmg_fr", brFR},
                {"blisters_rl", 0}, {"blisters_rr", 0}, {"blisters_fl", 0}, {"blisters_fr", 0},
                {"wing_fl", wingFL}, {"wing_fr", wingFR}, {"wing_rear", wingRear},
                {"floor_damage", floorDmg}, {"sidepod_damage", carpetDmg},
                {"diffuser_damage", diffuserDmg}, {"gearbox_damage", gearboxDmg}, {"engine_damage", engineDmg},
                {"drs_fault", drsFault}, {"ers_fault", ersFault}
            };
            rows.push_back(row);
            break;
        }
        case PID_PARTICIPANTS: {
            int partSize = 60;
            if (length < HEADER_SIZE + 1 + 22 * partSize) return {};
            nlohmann::json drivers = nlohmann::json::array();
            for (int i = 0; i < 22; ++i) {
                int o = HEADER_SIZE + 1 + i * partSize;
                bool ai = data[o] != 0; o += 1;
                o += 2;
                uint8_t teamId = data[o]; o += 1;
                o += 1;
                uint8_t raceNum = data[o]; o += 1;
                o += 1;
                int nameStart = o;
                std::string name = ReadString(data, nameStart, 48);
                if (name.empty()) continue;
                std::string liveryColor = "#8e8e8e";
                auto it = F1_24_TEAM_COLORS.find(teamId);
                if (it != F1_24_TEAM_COLORS.end()) {
                    liveryColor = it->second;
                }
                drivers.push_back({
                    {"idx", i}, {"name", name}, {"team_id", teamId},
                    {"race_number", raceNum}, {"ai", ai}, {"livery_color", liveryColor}
                });
            }
            nlohmann::json row = {{"type", "participants"}, {"drivers", drivers}};
            rows.push_back(row);
            break;
        }
        case PID_EVENT: {
            if (length < HEADER_SIZE + 4) return {};
            std::string code(reinterpret_cast<const char*>(data + HEADER_SIZE), 4);
            nlohmann::json base = {
                {"type", "race_event"},
                {"ts", timestamp},
                {"session_time", hdr.sessionTime},
                {"code", code}
            };
            int o = HEADER_SIZE + 4;
            if (code == "FTLP") {
                if (length < o + 5) return {};
                uint8_t vehicleIdx = data[o];
                float lapTimeS = Round3(ReadFloat(data, o + 1));
                nlohmann::json fastest = {
                    {"type", "fastest_lap"},
                    {"ts", timestamp},
                    {"car_idx", vehicleIdx},
                    {"lap_time_s", lapTimeS}
                };
                nlohmann::json ev = base;
                ev["car_idx"] = vehicleIdx;
                ev["lap_time_s"] = lapTimeS;
                rows.push_back(fastest);
                rows.push_back(ev);
            } else if (code == "DRSE" || code == "DRSD" || code == "RDFL" || code == "CHQF" ||
                       code == "LGOT" || code == "SSTA" || code == "SEND") {
                rows.push_back(base);
            } else if (code == "SCAR") {
                if (length < o + 2) return {};
                uint8_t scType = data[o];
                uint8_t evType = data[o + 1];
                if (scType == 0) return {};
                nlohmann::json ev = base;
                ev["safety_car_type"] = scType;
                ev["event_type"] = evType;
                rows.push_back(ev);
            } else if (code == "RTMT" || code == "RCWN") {
                if (length < o + 1) return {};
                nlohmann::json ev = base;
                ev["car_idx"] = data[o];
                rows.push_back(ev);
            } else if (code == "PENA") {
                if (length < o + 7) return {};
                uint8_t penType = data[o];
                uint8_t infringementType = data[o + 1];
                uint8_t vehicleIdx = data[o + 2];
                uint8_t timeS = data[o + 4];
                nlohmann::json ev = base;
                ev["car_idx"] = vehicleIdx;
                ev["penalty_type"] = penType;
                ev["infringement_type"] = infringementType;
                ev["penalty_time_s"] = timeS;
                rows.push_back(ev);
            } else if (code == "DTSV" || code == "SGSV") {
                if (length < o + 1) return {};
                nlohmann::json ev = base;
                ev["car_idx"] = data[o];
                rows.push_back(ev);
            }
            break;
        }
        case PID_SESSION_HISTORY: {
            if (length < HEADER_SIZE + 7) return {};
            uint8_t carIdx = data[HEADER_SIZE];
            uint8_t bestLapNum = data[HEADER_SIZE + 3];
            if (bestLapNum == 0) return {};

            int lapOff = HEADER_SIZE + 7 + (bestLapNum - 1) * 14;
            if (length < lapOff + 14) return {};

            uint8_t lapValidBitFlags = data[lapOff + 13];
            if ((lapValidBitFlags & 0x01) == 0) return {};

            uint32_t bestLapTimeMs = ReadUInt32(data, lapOff);
            nlohmann::json row = {
                {"type", "session_history_fastest"},
                {"ts", timestamp},
                {"car_idx", carIdx},
                {"best_lap_time_ms", bestLapTimeMs}
            };
            rows.push_back(row);
            break;
        }
        case PID_TYRE_SETS: {
            if (length < 231) return {};
            uint8_t carIdx = data[HEADER_SIZE];
            if (carIdx != hdr.playerCarIndex) return {};

            nlohmann::json sets = nlohmann::json::array();
            for (int i = 0; i < 20; ++i) {
                int o = HEADER_SIZE + 1 + i * 10;
                sets.push_back({
                    {"idx", i},
                    {"actual_compound", data[o]},
                    {"visual_compound", data[o + 1]},
                    {"wear", data[o + 2]},
                    {"available", data[o + 3] == 1},
                    {"recommended_session", data[o + 4]},
                    {"life_span", data[o + 5]},
                    {"usable_life", data[o + 6]},
                    {"lap_delta_ms", ReadInt16(data, o + 7)},
                    {"fitted", data[o + 9] == 1}
                });
            }
            uint8_t fittedIdx = data[230];
            nlohmann::json row = {
                {"type", "tyre_sets"}, {"ts", timestamp}, {"session_time", hdr.sessionTime},
                {"sets", sets}, {"fitted_idx", fittedIdx}
            };
            rows.push_back(row);
            break;
        }
        case PID_MOTION_EX: {
            if (length < 225) return {};
            float frontAero = ReadFloat(data, 217);
            float rearAero = ReadFloat(data, 221);

            nlohmann::json row = {
                {"type", "motion_ex"}, {"ts", timestamp}, {"session_time", hdr.sessionTime},
                {"front_aero_height_mm", Round2(frontAero * 1000.0)},
                {"rear_aero_height_mm", Round2(rearAero * 1000.0)}
            };
            rows.push_back(row);
            break;
        }
    }

    return rows;
}
