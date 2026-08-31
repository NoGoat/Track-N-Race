#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <glaze/glaze.hpp>

#include "tnrp/rows.h"
#include "tnrp/control_rows.h"

namespace tnrp {

// Strategy is a derived cold-row family. It is deliberately not recorded: the
// processor rebuilds it from the normalized rows in live and playback paths.
inline constexpr uint8_t kStrategyRowType = 15;
inline constexpr uint32_t kStrategyRowBit = 1u << kStrategyRowType;
inline constexpr uint32_t kStrategyDependencyMask =
    (1u << 2) | (1u << 3) | (1u << 4) | (1u << 5) | (1u << 6) |
    (1u << 7) | (1u << 8) | (1u << 9) | (1u << 10);

struct PitLoss {
    double inlap_ms{11250.0};
    double outlap_ms{13750.0};
    double total_ms{25000.0};
};

PitLoss pitLossForTrack(int trackId);

struct StrategyLapTarget {
    int lap_num{};
    double required_ms{};
    double actual_ms{};
    double delta_lap_ms{};
    double delta_stint_ms{};
    double delta_total_ms{};
    bool has_actual{};
};

struct StrategyStint {
    std::string compound_name;
    int actual_compound{};
    int visual_compound{};
    int stint_number{};
    int start_lap{};
    int end_lap{};
    int expected_laps{};
    int actual_laps{};
    bool is_last{};
    std::vector<StrategyLapTarget> rows;
};

struct StrategyPlan {
    int stops{};
    std::string mode; // "defensive" | "attacking"
    int target_idx{-1};
    std::string target_name;
    std::string reason;
    double confidence{};
    bool legal{true};
    std::string legality_reason;
    bool requires_compound_change{};
    std::vector<StrategyStint> stints;
};

struct StrategyCall {
    std::string kind; // "cover" | "undercut" | "overcut"
    int target_idx{-1};
    std::string target_name;
    double gap_ms{};
    std::optional<int> crossover_laps;
    std::string reason;
    int detected_lap{};
};

struct StrategyRival {
    int idx{-1};
    std::string name;
    std::string direction; // "ahead" | "behind"
    int position{};
    int result_status{};
    double gap_ms{};
    bool retired{};
    double threat_score{};
    double pace_ms{};
    double pace_delta_ms{};
    double closing_ms_per_lap{};
    int tyre_age_laps{};
    int actual_compound{};
    int visual_compound{};
    int last_pit_lap{-1};
    std::string pit_reaction;
};

struct StrategyPosition {
    int idx{-1};
    std::string name;
    std::string livery_color;
    std::string role; // "ahead" | "player" | "behind"
    int position{};
    int pit_status{};
    int num_pit_stops{};
    double gap_ms{};
    int gap_trend{}; // -1 unknown, 0 opening, 1 closing
    bool immediate{};
};

struct StrategyWearWarning {
    std::string text;
    std::string severity; // danger | warning | caution
    int priority{};
};

struct StrategyFactor {
    std::string code;
    std::string text;
    double impact_ms{};
};

struct StrategyNeutralisation {
    std::string kind; // "safety_car" | "virtual_safety_car"
    std::string recommendation; // "box" | "stay_out"
    std::string reason; // stable machine-readable reason
    double normal_pit_loss_ms{};
    double effective_pit_loss_ms{};
    double queue_loss_ms{};
    double recoverable_time_ms{};
    double net_time_ms{};
    double box_now_cost_ms{};
    double box_later_cost_ms{};
    double box_now_advantage_ms{};
    int box_later_lap{};
    int current_position{};
    int projected_box_position{};
    int projected_stay_position{};
    int projected_later_box_position{};
    int positions_lost{};
    int rivals_boxing{};
    int decision_lap{};
    double confidence{};
    double data_age_s{};
    std::string position_basis; // deployment gaps
    std::optional<int> laps_to_recover;
    std::vector<StrategyFactor> factors;
};

struct StrategyWeatherDecision {
    std::string recommendation; // stay_dry | stay_wet | prepare_intermediates | prepare_wets | prepare_slicks
    std::string target_compound;
    int forecast_weather{};
    int rain_percentage{};
    int crossover_lap{};
    int minutes_until_change{};
    double confidence{};
    std::string reason;
};

struct StrategyDecisionRecord {
    std::string event; // lap_plan | neutralisation
    int lap_num{};
    float session_time{};
    std::string recommendation;
    std::string reason;
    int target_idx{-1};
    std::string target_name;
    int start_position{};
    int projected_position{};
    int start_num_pit_stops{};
    std::optional<int> actual_position;
    std::optional<bool> followed;
    std::optional<bool> successful;
};

struct StrategySnapshotRow {
    std::string type{"strategy"};
    float session_time{};
    std::string state{"waiting"}; // non_race | waiting | ready
    int lap_num{};
    int total_laps{};
    int current_actual_compound{};
    int current_visual_compound{};
    int current_tyre_age_laps{};
    std::string current_compound_name;
    double average_wear{};
    double wear_per_lap{};
    std::string limiting_corner;
    double limiting_wear{};
    double limiting_wear_per_lap{};
    int cliff_lap{};
    int laps_until_cliff{};
    bool is_monaco{};
    double confidence{};
    double data_age_s{};
    std::vector<StrategyFactor> explanation;
    StrategyPlan conservative;
    StrategyPlan aggressive;
    std::optional<StrategyCall> call;
    std::optional<StrategyNeutralisation> neutralisation;
    std::optional<StrategyWeatherDecision> weather_strategy;
    std::vector<StrategyDecisionRecord> decision_history;
    std::vector<StrategyRival> rivals;
    std::vector<StrategyPosition> positions;
    double wear_fl{};
    double wear_fr{};
    double wear_rl{};
    double wear_rr{};
    std::vector<StrategyWearWarning> wear_warnings;
};

// Stateful reducer over normalized cold rows. snapshot() is deterministic and
// contains the complete renderer model; no strategy arithmetic belongs in a UI.
class StrategyProcessor {
public:
    explicit StrategyProcessor(uint16_t format = 2025);
    void setFormat(uint16_t format);
    void setMinimumStops(int stops);
    void reset();
    void ingest(const LapRow& row);
    void ingest(const SessionRow& row);
    void ingest(const StatusRow& row);
    void ingest(const DamageRow& row);
    void ingest(const TimingRow& row);
    void ingest(const ParticipantsRow& row);
    void ingest(const TyreSetsRow& row);
    void ingest(const AllStatusRow& row);
    void ingest(const RaceEventRow& row);
    void ingestJson(std::string_view json);
    StrategySnapshotRow snapshot();
    std::string snapshotJson();

private:
    struct RivalLapSample {
        int lap{};
        int lap_ms{};
        double relative_gap_ms{};
        int position{};
        int tyre_age_laps{-1};
    };

    struct RivalExperience {
        int observed_lap{-1};
        int tyre_age_laps{-1};
        int tyre_compound{};
        int visual_compound{};
        int num_pit_stops{};
        int pit_status{};
        int last_pit_lap{-1};
        std::vector<RivalLapSample> recent_laps;
    };

    struct WearSample {
        int lap{};
        int actual_compound{};
        double fl{}, fr{}, rl{}, rr{};
    };

    struct NeutralCarState {
        int idx{-1};
        int position{};
        int lap_num{};
        double gap_ms{};
        int pit_status{};
        int num_pit_stops{};
        int result_status{};
    };

    void refreshRivals();
    bool isRivalThreatCandidate(int idx, bool ahead) const;
    double rivalPaceMs(int idx) const;
    double playerPaceMs() const;
    double rivalThreatScore(int idx, bool ahead) const;

    uint16_t format_{2025};
    int minimumStops_{};
    std::optional<LapRow> lap_;
    std::optional<SessionRow> session_;
    std::optional<StatusRow> status_;
    std::optional<DamageRow> damage_;
    std::optional<TimingRow> timing_;
    std::optional<ParticipantsRow> participants_;
    std::optional<TyreSetsRow> tyreSets_;
    std::optional<AllStatusRow> allStatus_;
    std::map<int, int> lapTimes_;
    int currentStintStart_{0};
    int rivalAhead_{-1};
    int rivalBehind_{-1};
    std::map<int, RivalExperience> rivalExperience_;
    std::set<int> retiredCars_;
    std::set<int> usedDryVisualCompounds_;
    std::vector<WearSample> wearHistory_;
    int lastWearLap_{-1};
    int neutralisationStartLap_{};
    float neutralisationStartTime_{};
    std::vector<NeutralCarState> frozenNeutralCars_;
    std::string neutralisationRecommendation_;
    std::vector<StrategyDecisionRecord> decisionHistory_;
    double previousAheadGap_{};
    double previousBehindGap_{};
    bool haveAheadGap_{false};
    bool haveBehindGap_{false};
    int aheadTrend_{-1};
    int behindTrend_{-1};
    struct PastStintState {
        int start_lap{};
        double required_base_ms{};
        std::string compound_name;
        int actual_compound{};
        int visual_compound{};
        bool post_pit{};
        int expected_laps{};
    };
    std::vector<PastStintState> conservativePast_;
    std::vector<PastStintState> aggressivePast_;
    std::map<int, double> conservativeRequired_;
    std::map<int, double> aggressiveRequired_;
};

} // namespace tnrp

template <> struct glz::meta<tnrp::StrategyLapTarget> {
    using T = tnrp::StrategyLapTarget;
    static constexpr auto value = glz::object("lap_num",&T::lap_num,"required_ms",&T::required_ms,"actual_ms",&T::actual_ms,"delta_lap_ms",&T::delta_lap_ms,"delta_stint_ms",&T::delta_stint_ms,"delta_total_ms",&T::delta_total_ms,"has_actual",&T::has_actual);
};
template <> struct glz::meta<tnrp::StrategyStint> {
    using T = tnrp::StrategyStint;
    static constexpr auto value = glz::object("compound_name",&T::compound_name,"actual_compound",&T::actual_compound,"visual_compound",&T::visual_compound,"stint_number",&T::stint_number,"start_lap",&T::start_lap,"end_lap",&T::end_lap,"expected_laps",&T::expected_laps,"actual_laps",&T::actual_laps,"is_last",&T::is_last,"rows",&T::rows);
};
template <> struct glz::meta<tnrp::StrategyPlan> { using T=tnrp::StrategyPlan; static constexpr auto value=glz::object("stops",&T::stops,"mode",&T::mode,"target_idx",&T::target_idx,"target_name",&T::target_name,"reason",&T::reason,"confidence",&T::confidence,"legal",&T::legal,"legality_reason",&T::legality_reason,"requires_compound_change",&T::requires_compound_change,"stints",&T::stints); };
template <> struct glz::meta<tnrp::StrategyCall> { using T=tnrp::StrategyCall; static constexpr auto value=glz::object("kind",&T::kind,"target_idx",&T::target_idx,"target_name",&T::target_name,"gap_ms",&T::gap_ms,"crossover_laps",&T::crossover_laps,"reason",&T::reason,"detected_lap",&T::detected_lap); };
template <> struct glz::meta<tnrp::StrategyRival> { using T=tnrp::StrategyRival; static constexpr auto value=glz::object("idx",&T::idx,"name",&T::name,"direction",&T::direction,"position",&T::position,"result_status",&T::result_status,"gap_ms",&T::gap_ms,"retired",&T::retired,"threat_score",&T::threat_score,"pace_ms",&T::pace_ms,"pace_delta_ms",&T::pace_delta_ms,"closing_ms_per_lap",&T::closing_ms_per_lap,"tyre_age_laps",&T::tyre_age_laps,"actual_compound",&T::actual_compound,"visual_compound",&T::visual_compound,"last_pit_lap",&T::last_pit_lap,"pit_reaction",&T::pit_reaction); };
template <> struct glz::meta<tnrp::StrategyPosition> { using T=tnrp::StrategyPosition; static constexpr auto value=glz::object("idx",&T::idx,"name",&T::name,"livery_color",&T::livery_color,"role",&T::role,"position",&T::position,"pit_status",&T::pit_status,"num_pit_stops",&T::num_pit_stops,"gap_ms",&T::gap_ms,"gap_trend",&T::gap_trend,"immediate",&T::immediate); };
template <> struct glz::meta<tnrp::StrategyWearWarning> { using T=tnrp::StrategyWearWarning; static constexpr auto value=glz::object("text",&T::text,"severity",&T::severity,"priority",&T::priority); };
template <> struct glz::meta<tnrp::StrategyFactor> { using T=tnrp::StrategyFactor; static constexpr auto value=glz::object("code",&T::code,"text",&T::text,"impact_ms",&T::impact_ms); };
template <> struct glz::meta<tnrp::StrategyNeutralisation> {
    using T=tnrp::StrategyNeutralisation;
    static constexpr auto value=glz::object("kind",&T::kind,"recommendation",&T::recommendation,"reason",&T::reason,"normal_pit_loss_ms",&T::normal_pit_loss_ms,"effective_pit_loss_ms",&T::effective_pit_loss_ms,"queue_loss_ms",&T::queue_loss_ms,"recoverable_time_ms",&T::recoverable_time_ms,"net_time_ms",&T::net_time_ms,"box_now_cost_ms",&T::box_now_cost_ms,"box_later_cost_ms",&T::box_later_cost_ms,"box_now_advantage_ms",&T::box_now_advantage_ms,"box_later_lap",&T::box_later_lap,"current_position",&T::current_position,"projected_box_position",&T::projected_box_position,"projected_stay_position",&T::projected_stay_position,"projected_later_box_position",&T::projected_later_box_position,"positions_lost",&T::positions_lost,"rivals_boxing",&T::rivals_boxing,"decision_lap",&T::decision_lap,"confidence",&T::confidence,"data_age_s",&T::data_age_s,"position_basis",&T::position_basis,"laps_to_recover",&T::laps_to_recover,"factors",&T::factors);
};
template <> struct glz::meta<tnrp::StrategyWeatherDecision> { using T=tnrp::StrategyWeatherDecision; static constexpr auto value=glz::object("recommendation",&T::recommendation,"target_compound",&T::target_compound,"forecast_weather",&T::forecast_weather,"rain_percentage",&T::rain_percentage,"crossover_lap",&T::crossover_lap,"minutes_until_change",&T::minutes_until_change,"confidence",&T::confidence,"reason",&T::reason); };
template <> struct glz::meta<tnrp::StrategyDecisionRecord> { using T=tnrp::StrategyDecisionRecord; static constexpr auto value=glz::object("event",&T::event,"lap_num",&T::lap_num,"session_time",&T::session_time,"recommendation",&T::recommendation,"reason",&T::reason,"target_idx",&T::target_idx,"target_name",&T::target_name,"start_position",&T::start_position,"projected_position",&T::projected_position,"start_num_pit_stops",&T::start_num_pit_stops,"actual_position",&T::actual_position,"followed",&T::followed,"successful",&T::successful); };
template <> struct glz::meta<tnrp::StrategySnapshotRow> {
    using T=tnrp::StrategySnapshotRow;
    static constexpr auto value=glz::object("type",&T::type,"session_time",&T::session_time,"state",&T::state,"lap_num",&T::lap_num,"total_laps",&T::total_laps,"current_actual_compound",&T::current_actual_compound,"current_visual_compound",&T::current_visual_compound,"current_tyre_age_laps",&T::current_tyre_age_laps,"current_compound_name",&T::current_compound_name,"average_wear",&T::average_wear,"wear_per_lap",&T::wear_per_lap,"limiting_corner",&T::limiting_corner,"limiting_wear",&T::limiting_wear,"limiting_wear_per_lap",&T::limiting_wear_per_lap,"cliff_lap",&T::cliff_lap,"laps_until_cliff",&T::laps_until_cliff,"is_monaco",&T::is_monaco,"confidence",&T::confidence,"data_age_s",&T::data_age_s,"explanation",&T::explanation,"conservative",&T::conservative,"aggressive",&T::aggressive,"call",&T::call,"neutralisation",&T::neutralisation,"weather_strategy",&T::weather_strategy,"decision_history",&T::decision_history,"rivals",&T::rivals,"positions",&T::positions,"wear_fl",&T::wear_fl,"wear_fr",&T::wear_fr,"wear_rl",&T::wear_rl,"wear_rr",&T::wear_rr,"wear_warnings",&T::wear_warnings);
};
