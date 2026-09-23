#include "tnrp/Strategy.h"

#include "tnrp/AnyRow.h"
#include "tnrp/Labels.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <set>

namespace tnrp {

PitLoss pitLossForTrack(int id) {
    // Canonical strategy catalog, migrated from the duplicated UI map assets.
    static const std::map<int, PitLoss> values = {
        {0,{8550,10450,21500}}, {2,{7750,9480,19730}}, {3,{8500,10390,21400}},
        {4,{8820,10780,22100}}, {5,{7380,9020,18900}}, {6,{8550,10450,21500}},
        {7,{9450,11550,23500}}, {9,{9000,11000,22500}}, {10,{7960,9740,20200}},
        {11,{8550,10450,21500}}, {12,{9420,11520,23440}}, {13,{7940,9710,20150}},
        {14,{9900,12100,24500}}, {15,{8370,10230,21100}}, {16,{8370,10230,21100}},
        {17,{9450,11550,23500}}, {19,{7200,8800,18500}}, {20,{7420,9080,19000}},
        {26,{9000,11000,22500}}, {27,{11250,13750,27500}}, {29,{10350,12650,25500}},
        {30,{9000,11000,22500}}, {31,{7650,9350,19500}}, {32,{10800,13200,26500}},
        {39,{9450,11550,23500}}, {40,{9450,11550,23500}}, {41,{9000,11000,22500}},
    };
    auto it = values.find(id);
    if (it != values.end()) return it->second;
    PitLoss fallback;
    fallback.estimated = true;
    return fallback;
}

namespace {

struct RawStint {
    std::string name;
    int actual{}, visual{}, lapCount{}, startLap{}, pitLap{-1}, setIdx{-1};
    bool last{};
};
struct RawPlan {
    int stops{};
    bool legal{true};
    std::string legalityReason;
    bool requiresCompoundChange{};
    std::vector<RawStint> stints;
};

std::string tyreName(uint16_t format, int actual) {
    if (actual <= 0) return "—";
    return labelsFor(format).get("tyre.actual." + std::to_string(actual));
}

std::string driverName(const std::optional<ParticipantsRow>& p, int idx) {
    if (p) for (const auto& d : p->drivers) if (d.idx == idx) {
        std::string n = d.name;
        const size_t at = n.find_last_of(" \t");
        if (at != std::string::npos) n = n.substr(at + 1);
        std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c){ return (char)std::toupper(c); });
        return n;
    }
    return "Car " + std::to_string(idx);
}

std::string liveryColor(const std::optional<ParticipantsRow>& p, int idx) {
    if (p) for (const auto& d : p->drivers) if (d.idx == idx) return d.livery_color;
    return {};
}

bool dryVisual(int compound) { return compound == 16 || compound == 17 || compound == 18; }
bool wetActual(int compound) { return compound == 7 || compound == 8; }

// Tyre categories in increasing wetness. Weather codes: 0-2 dry,
// 3 light rain, 4 heavy rain, 5 storm.
constexpr int kSlick = 0, kInter = 1, kFullWet = 2;
int tyreCategory(int actual) { return actual == 8 ? kFullWet : actual == 7 ? kInter : kSlick; }
int weatherCategory(int weather) { return weather >= 4 ? kFullWet : weather == 3 ? kInter : kSlick; }
const char* categoryCompound(int category) {
    return category == kFullWet ? "wet" : category == kInter ? "intermediate" : "slick";
}

int tyreLife(const TyreSet& set) {
    if (set.life_span > 0) return set.life_span;
    return std::max(0, set.usable_life);
}

RawPlan buildPlan(uint16_t format, int lap, int total, int firstPit,
                  int curActual, int curVisual, const std::vector<TyreSet>& pool) {
    RawPlan out;
    const int clamped = std::min(firstPit, total);
    out.stints.push_back({tyreName(format,curActual),curActual,curVisual,
                          std::max(0,clamped-lap),lap,clamped>=total?-1:clamped,-1,clamped>=total});
    if (clamped >= total) return out;
    int pit = clamped, remaining = total - pit;
    for (const auto& set : pool) {
        if (remaining <= 0) break;
        const int len = std::min(tyreLife(set), remaining);
        if (len <= 0) continue;
        const int next = pit + len;
        const bool last = next >= total;
        out.stints.push_back({tyreName(format,set.actual_compound),set.actual_compound,
                              set.visual_compound,len,pit,last?-1:next,set.idx,last});
        remaining -= len; pit = next;
        if (last) break;
    }
    if (remaining > 0 && !out.stints.empty()) {
        auto& last = out.stints.back();
        last.lapCount += remaining; last.pitLap = -1; last.last = true;
        out.legal = false;
        out.legalityReason = "insufficient_tyre_life";
    }
    out.stops = std::max(0, (int)out.stints.size() - 1);
    return out;
}

RawPlan forceExtra(uint16_t format, RawPlan in, const std::vector<TyreSet>& pool,
                   std::optional<int> requiredDifferentVisual = std::nullopt,
                   bool includeCurrentStint = false) {
    if (in.stints.empty()) return in;
    size_t longest = includeCurrentStint ? 0 : (in.stints.size() > 1 ? 1 : 0);
    for (size_t i=longest+1;i<in.stints.size();++i) if (in.stints[i].lapCount>in.stints[longest].lapCount) longest=i;
    RawStint target=in.stints[longest]; if(target.lapCount<4||pool.empty()) return in;
    const int end=target.last?target.startLap+target.lapCount:
        (target.pitLap>=0?target.pitLap:target.startLap+target.lapCount);
    std::set<int> used; for(const auto& s:in.stints) if (s.setIdx >= 0) used.insert(s.setIdx);
    const TyreSet* next=nullptr;
    for(const auto& s:pool) {
        if (used.count(s.idx) || tyreLife(s) <= 0) continue;
        if (requiredDifferentVisual && s.visual_compound == *requiredDifferentVisual) continue;
        next=&s; break;
    }
    if(!next && !requiredDifferentVisual)
        for(const auto& s:pool) if(!used.count(s.idx)&&tyreLife(s)>0){next=&s;break;}
    if(!next) {
        in.legal=false;
        in.legalityReason=requiredDifferentVisual?"no_second_dry_compound":"no_unused_physical_set";
        return in;
    }
    const int split=std::max(target.startLap+2,
        std::max(target.startLap+target.lapCount/2,end-tyreLife(*next)));
    if(split>end-2){in.legal=false;in.legalityReason="insufficient_extra_set_life";return in;}
    RawStint a=target; a.lapCount=split-target.startLap; a.pitLap=split; a.last=false;
    RawStint b{tyreName(format,next->actual_compound),next->actual_compound,next->visual_compound,
               end-split,split,target.pitLap,next->idx,target.last};
    in.stints.erase(in.stints.begin()+(ptrdiff_t)longest);
    in.stints.insert(in.stints.begin()+(ptrdiff_t)longest,b);
    in.stints.insert(in.stints.begin()+(ptrdiff_t)longest,a);
    ++in.stops; return in;
}

void enforceDryCompoundRule(uint16_t format, RawPlan& plan,
                            const std::vector<TyreSet>& pool,
                            const std::set<int>& alreadyUsed) {
    std::set<int> visuals = alreadyUsed;
    for (const auto& stint : plan.stints) if (dryVisual(stint.visual)) visuals.insert(stint.visual);
    if (visuals.size() >= 2) return;
    plan.requiresCompoundChange = true;
    int current = visuals.empty() ? 0 : *visuals.begin();
    plan = forceExtra(format, std::move(plan), pool, current);
    visuals = alreadyUsed;
    for (const auto& stint : plan.stints) if (dryVisual(stint.visual)) visuals.insert(stint.visual);
    if (visuals.size() < 2) {
        plan.legal = false;
        plan.legalityReason = "no_second_dry_compound";
    }
}

void enforceMinimumStops(uint16_t format, RawPlan& plan,
                         const std::vector<TyreSet>& pool, int minimumStops) {
    while (plan.stops < minimumStops) {
        const int previousStops = plan.stops;
        plan = forceExtra(format, std::move(plan), pool, std::nullopt, true);
        if (plan.stops == previousStops) break;
    }
}

const TimingCar* findCar(const TimingRow& t, int idx) {
    for (const auto& c:t.cars) if(c.idx==idx) return &c; return nullptr;
}
const TimingCar* atPosition(const TimingRow& t, int pos) {
    for(const auto& c:t.cars) if(c.result_status==2&&c.position==pos) return &c; return nullptr;
}

bool validRaceLapMs(int lapMs) {
    return lapMs > 0 && lapMs < 600000;
}

// Relative gaps are signed (+ = rival behind). Closing is only meaningful
// while the running order is unchanged; a pass flips the sign.
double closingMs(double previous, double latest) {
    if ((previous < 0.0) != (latest < 0.0)) return 0.0;
    return std::abs(previous) - std::abs(latest);
}

// Partial rows are applied onto existing state; absent keys keep their value.
constexpr glz::opts kPatch{.null_terminated = false, .error_on_unknown_keys = false};

// Pit status alone cannot tell a strategic stop from serving a penalty.
bool penaltyPending(const TimingCar& car) {
    return car.num_dt_pens > 0 || car.num_sg_pens > 0;
}

double medianLapMs(const std::vector<int>& laps) {
    if (laps.empty()) return 0.0;
    std::vector<int> sorted = laps;
    std::sort(sorted.begin(), sorted.end());
    const size_t middle = sorted.size() / 2;
    if (sorted.size() % 2 != 0) return sorted[middle];
    return (sorted[middle - 1] + sorted[middle]) / 2.0;
}

double robustWeightedPace(std::vector<std::pair<int,int>> laps, int currentLap) {
    if (laps.empty()) return 0.0;
    std::vector<int> values;
    values.reserve(laps.size());
    for (const auto& lap : laps) values.push_back(lap.second);
    const double median = medianLapMs(values);
    double weighted = 0.0, weights = 0.0;
    int accepted = 0;
    for (auto it = laps.rbegin(); it != laps.rend(); ++it) {
        if (!validRaceLapMs(it->second)) continue;
        // Very fast/slow laps are normally invalid laps, traffic or pit transitions.
        if (laps.size() >= 3 && (it->second < median * .94 || it->second > median * 1.04)) continue;
        const double weight = std::pow(.62, accepted++);
        // Normalize older, fuel-heavier laps to the current lap with a deliberately
        // modest correction. This is an estimate, not a car-performance model.
        const double normalized = it->second - std::max(0, currentLap - it->first) * 30.0;
        weighted += normalized * weight;
        weights += weight;
        if (accepted == 5) break;
    }
    return weights > 0.0 ? weighted / weights : median;
}

} // namespace

StrategyProcessor::StrategyProcessor(uint16_t format):format_(format){}
void StrategyProcessor::setFormat(uint16_t f){ if(f>=2024) format_=f; }
void StrategyProcessor::setTeamColorOverrides(TeamColorOverrides overrides){
    teamColorOverrides_=sanitizeTeamColorOverrides(overrides);
    if(participants_)for(auto&driver:participants_->drivers)
        applyTeamColorToDriver(driver,format_,teamColorOverrides_);
}
void StrategyProcessor::setMinimumStops(int stops){minimumStops_=std::clamp(stops,0,8);}
void StrategyProcessor::reset(){
    lap_.reset();session_.reset();status_.reset();damage_.reset();timing_.reset();participants_.reset();tyreSets_.reset();allStatus_.reset();
    lapTimes_.clear();raceHistory_.reset();completedStops_=0;
    currentStintStart_=0;rivalAhead_=rivalBehind_=-1;
    rivalExperience_.clear();retiredCars_.clear();usedDryVisualCompounds_.clear();
    wearHistory_.clear();lastWearLap_=-1;
    neutralisationStartLap_=0;neutralisationStartTime_=0;
    frozenNeutralCars_.clear();neutralisationRecommendation_.clear();
    decisionHistory_.clear();
    haveAheadGap_=haveBehindGap_=false;aheadTrend_=behindTrend_=-1;
    aheadGapIdx_=behindGapIdx_=-1;
    playerTaintedLap_=-1;paceStintStartLap_=0;
    stints_.clear();
}

void StrategyProcessor::rememberPaceLap(int lap, int milliseconds) {
    // Pace is per tyre stint, as for rivals. Earlier-stint laps remain only
    // as a fallback until the first clean lap on the current tyres exists.
    if (lap < paceStintStartLap_) return;
    lapTimes_[lap] = milliseconds;
    lapTimes_.erase(lapTimes_.begin(), lapTimes_.lower_bound(paceStintStartLap_));
    // The estimator uses five accepted samples; retain a small allowance for
    // its outlier filter without copying a full race into every checkpoint.
    while (lapTimes_.size() > 12) lapTimes_.erase(lapTimes_.begin());
}

void StrategyProcessor::completeLap(int nextLap, int milliseconds) {
    if (!session_ || session_->session_type < 15 || session_->session_type > 17) return;
    const int completed = nextLap - 1;
    if (completed <= 0 || completed > session_->total_laps ||
        completed > StrategyRaceHistory::MAX_LAPS) return;
    StrategyRaceHistory::Lap result{completed, std::max(0, milliseconds), {}};
    if (completed > raceHistory_.latestLap() && lap_ && lap_->lap_num == completed) {
        // Capture the last recommendation from the outgoing lap's inputs.
        // This runs for live input AND replay, regardless of page visibility.
        // The internal calculation omits full-race rows and serialization.
        const auto plan = makeSnapshot(false, true);
        const StrategyPlan* plans[] = {&plan.conservative, &plan.aggressive};
        for (size_t i = 0; i < 2; ++i)
            for (const auto& stint : plans[i]->stints)
                for (const auto& row : stint.rows)
                    if (row.lap_num == completed) result.required_base_ms[i] = row.required_ms;
    }
    raceHistory_.complete(result);
}

void StrategyProcessor::observeStint(bool changed) {
    if (!lap_ || !status_ || !session_ || lap_->lap_num <= 0 ||
        status_->tyre_compound <= 0 || session_->session_type < 15 || session_->session_type > 17) return;
    int currentLap = lap_->lap_num;
    if (timing_) if (const auto* player = findCar(*timing_, timing_->player_idx))
        currentLap = std::max(currentLap, player->lap_num);
    if (currentLap > StrategyRaceHistory::MAX_LAPS) return;
    if (stints_.empty()) {
        currentStintStart_ = std::max(1, currentLap - status_->tyre_age_laps);
        stints_.push_back({currentStintStart_, status_->tyre_compound, status_->visual_compound, {}});
    } else if (changed) {
        currentStintStart_ = currentLap;
        paceStintStartLap_ = currentLap;
        StintProgress stint{currentLap, status_->tyre_compound, status_->visual_compound, {}};
        if (currentLap > stints_.back().start_lap) stints_.push_back(stint);
        else stints_.back() = stint;
        // Stint changes describe tyre use. Only the race's reported stop
        // counter satisfies required pit stops; a tyre change alone need not.
    }
}
void StrategyProcessor::commitDecisions(){(void)makeSnapshot(false,true);}

void StrategyProcessor::ingest(const SessionRow& r){
    // The header UID identifies a session exactly (including restarts at the
    // same track and type). Older recordings fall back to track/type changes.
    const bool newSession=session_&&(r.session_uid&&session_->session_uid
        ?*r.session_uid!=*session_->session_uid
        :(session_->track_id!=r.track_id||session_->session_type!=r.session_type)&&lap_&&lap_->lap_num>1);
    if(newSession)reset();
    const int oldStatus=session_?session_->safety_car_status:0;
    if((r.safety_car_status==1||r.safety_car_status==2)&&r.safety_car_status!=oldStatus){
        neutralisationStartLap_=lap_?lap_->lap_num:0;
        neutralisationStartTime_=timing_?timing_->session_time:(status_?status_->session_time:0);
        neutralisationRecommendation_.clear();
        frozenNeutralCars_.clear();
        if(timing_)for(const auto&car:timing_->cars)frozenNeutralCars_.push_back({
            car.idx,car.position,car.lap_num,(double)car.gap_ms,car.pit_status,
            car.num_pit_stops,car.result_status});
    }else if(r.safety_car_status==0&&oldStatus!=0){
        neutralisationStartLap_=0;neutralisationStartTime_=0;
        frozenNeutralCars_.clear();neutralisationRecommendation_.clear();
    }
    session_=r;
    observeStint();
    // Session rows arrive at a fixed rate in live and replay alike, so SC/VSC
    // hysteresis advances on inputs rather than on how often a UI polls.
    if(r.safety_car_status==1||r.safety_car_status==2)commitDecisions();
}
void StrategyProcessor::ingest(const StatusRow&r){
    const bool changed=status_&&lap_&&(r.tyre_compound!=status_->tyre_compound||
        r.visual_compound!=status_->visual_compound||r.tyre_age_laps<status_->tyre_age_laps);
    if(changed){
        // Freeze the outgoing stint's expected interval before replacing tyres.
        commitDecisions();
        wearHistory_.clear();lastWearLap_=-1;
    }
    status_=r;
    if(dryVisual(r.visual_compound))usedDryVisualCompounds_.insert(r.visual_compound);
    observeStint(changed);
}
void StrategyProcessor::ingest(const DamageRow&r){damage_=r;}
void StrategyProcessor::ingest(const ParticipantsRow&r){
    participants_=r;
    for(auto&driver:participants_->drivers)
        applyTeamColorToDriver(driver,format_,teamColorOverrides_);
}
int StrategyProcessor::playerIndex() const{
    if(timing_&&timing_->player_idx>=0)return timing_->player_idx;
    if(lap_&&lap_->player_idx>=0)return lap_->player_idx;
    if(status_&&status_->player_idx>=0)return status_->player_idx;
    return -1;
}
void StrategyProcessor::ingest(const TyreSetsRow&r){
    // The game cycles this packet through every car; only the player's sets
    // describe our options. Rows without a car index predate that field.
    if(r.car_idx>=0&&r.car_idx!=playerIndex())return;
    tyreSets_=r;
}
void StrategyProcessor::ingest(const AllStatusRow&r){
    allStatus_=r;
    for(const auto& car:r.cars){
        auto& experience=rivalExperience_[car.idx];
        if(experience.tyre_age_laps>=0&&
           (car.tyre_compound!=experience.tyre_compound||
            car.visual_compound!=experience.visual_compound||
            car.tyre_age_laps+1<experience.tyre_age_laps)){
            experience.recent_laps.clear();
            // Penalties can be served at a tyre stop; the tyre change makes it strategic.
            if(experience.penalty_visit&&experience.observed_lap>=0){
                experience.last_pit_lap=experience.observed_lap;
                experience.penalty_visit=false;
            }
        }
        experience.tyre_age_laps=car.tyre_age_laps;
        experience.tyre_compound=car.tyre_compound;
        experience.visual_compound=car.visual_compound;
    }
}
void StrategyProcessor::ingest(const RaceEventRow&r){
    if(r.code=="SSTA"){reset();return;}
    if(r.code=="RTMT"&&r.car_idx){
        retiredCars_.insert(*r.car_idx);
        refreshRivals();
    }
}

void StrategyProcessor::refreshRivals(){
    if(!timing_)return;
    const TimingCar* player=findCar(*timing_,timing_->player_idx);
    if(!player||player->position<=0||player->result_status!=2){rivalAhead_=rivalBehind_=-1;return;}
    auto choose=[&](bool ahead,int current){
        int best=-1;double bestScore=12.0;
        for(const auto&car:timing_->cars){
            if(!isRivalThreatCandidate(car.idx,ahead))continue;
            const double score=rivalThreatScore(car.idx,ahead);
            if(score>bestScore){best=car.idx;bestScore=score;}
        }
        if(current>=0){
            if(isRivalThreatCandidate(current,ahead)){
                const double currentScore=rivalThreatScore(current,ahead);
                if(currentScore>=12.0&&(best<0||bestScore<currentScore+8.0))return current;
            }
        }
        return best;
    };
    rivalAhead_=choose(true,rivalAhead_);
    rivalBehind_=choose(false,rivalBehind_);
}

bool StrategyProcessor::isRivalThreatCandidate(int idx,bool ahead) const{
    if(!timing_)return false;
    const auto*player=findCar(*timing_,timing_->player_idx);
    const auto*car=findCar(*timing_,idx);
    if(!player||!car||car->idx==player->idx||player->result_status!=2||
       car->result_status!=2||player->position<=0||car->position<=0||
       retiredCars_.count(idx))return false;
    if(ahead!=(car->position<player->position))return false;

    // Strategy rivals are cars in the player's active race window, not every
    // active car that happened to stop recently. A zero leader delta is usable
    // for P1 only; accepting it for another position makes lapped/pit-cycle
    // cars look exactly alongside the player.
    constexpr int kMaxPositionDelta=3;
    if(std::abs(car->position-player->position)>kMaxPositionDelta)return false;
    if(std::abs(car->lap_num-player->lap_num)>1)return false;
    if((player->position!=1&&player->gap_ms<=0)||(car->position!=1&&car->gap_ms<=0))
        return false;

    const double gap=ahead?(double)player->gap_ms-car->gap_ms:
                             (double)car->gap_ms-player->gap_ms;
    const double pitWindow=session_?pitLossForTrack(session_->track_id).total_ms:25000.0;
    return gap>=0.0&&gap<=pitWindow*1.25;
}

double StrategyProcessor::rivalPaceMs(int idx) const{
    if(idx<0)return 0.0;
    auto it=rivalExperience_.find(idx);
    if(it!=rivalExperience_.end()){
        if(it->second.pit_status!=0||it->second.tyre_age_laps==0||it->second.tyre_age_laps==1)return 0.0;
        std::vector<std::pair<int,int>> laps;
        for(const auto&sample:it->second.recent_laps)laps.push_back({sample.lap,sample.lap_ms});
        const double experienced=robustWeightedPace(std::move(laps),lap_?lap_->lap_num:0);
        if(experienced>0)return experienced;
    }
    // Do not turn a neutralised or formation lap into the next green-flag target.
    if(session_&&session_->safety_car_status!=0)return 0.0;
    if(timing_){
        const TimingCar* car=findCar(*timing_,idx);
        if(car&&!car->lap_invalid&&car->pit_status==0&&validRaceLapMs(car->last_lap_ms))return car->last_lap_ms;
    }
    return 0.0;
}

double StrategyProcessor::playerPaceMs() const{
    std::vector<std::pair<int,int>> laps;
    for(const auto&lap:lapTimes_)if(validRaceLapMs(lap.second))laps.push_back(lap);
    const double pace=robustWeightedPace(std::move(laps),lap_?lap_->lap_num:0);
    if(pace>0)return pace;
    if(timing_){
        const auto*player=findCar(*timing_,timing_->player_idx);
        if(player&&!player->lap_invalid&&player->pit_status==0&&validRaceLapMs(player->last_lap_ms))return player->last_lap_ms;
    }
    return lap_&&validRaceLapMs(lap_->last_lap_ms)?lap_->last_lap_ms:0.0;
}

double StrategyProcessor::rivalThreatScore(int idx,bool ahead) const{
    if(!isRivalThreatCandidate(idx,ahead))return 0.0;
    const auto*player=findCar(*timing_,timing_->player_idx);
    const auto*car=findCar(*timing_,idx);
    const double gap=ahead?(double)player->gap_ms-car->gap_ms:
                             (double)car->gap_ms-player->gap_ms;
    const double pitWindow=session_?pitLossForTrack(session_->track_id).total_ms:25000.0;
    double score=45.0*std::max(0.0,1.0-gap/std::max(1000.0,pitWindow));
    const double playerPace=playerPaceMs(),rivalPace=rivalPaceMs(idx);
    if(playerPace>0&&rivalPace>0){
        const double opportunity=ahead?playerPace-rivalPace:rivalPace-playerPace;
        score+=std::clamp(-opportunity/55.0,0.0,22.0);
    }
    auto it=rivalExperience_.find(idx);
    if(it!=rivalExperience_.end()){
        const int playerAge=status_?status_->tyre_age_laps:0;
        const int ageEdge=ahead?it->second.tyre_age_laps-playerAge:playerAge-it->second.tyre_age_laps;
        score+=std::clamp(ageEdge*1.8,0.0,14.0);
        if(it->second.recent_laps.size()>=2){
            const auto&last=it->second.recent_laps.back();
            const auto&prev=it->second.recent_laps[it->second.recent_laps.size()-2];
            const double closing=closingMs(prev.relative_gap_ms,last.relative_gap_ms);
            score+=std::clamp(closing/80.0,0.0,15.0);
        }
        if(it->second.last_pit_lap>=0&&lap_&&it->second.last_pit_lap>=lap_->lap_num-1)score+=12.0;
    }
    if(car->pit_status!=0&&!(it!=rivalExperience_.end()&&it->second.penalty_visit))score+=8.0;
    return std::clamp(score,0.0,100.0);
}

void StrategyProcessor::ingest(const TimingRow&r){
    if (const auto* player = findCar(r, r.player_idx)) {
        completeLap(player->lap_num, player->last_lap_ms);
        completedStops_ = std::max(completedStops_, player->num_pit_stops);
    }
    timing_=r;
    const bool neutralised=session_&&(session_->safety_car_status==1||session_->safety_car_status==2);
    const TimingCar* player=findCar(r,r.player_idx);
    for(const auto& car:r.cars){
        auto& experience=rivalExperience_[car.idx];
        const bool justStopped=experience.observed_lap>=0&&car.num_pit_stops>experience.num_pit_stops;
        const bool enteredPits=experience.observed_lap>=0&&experience.pit_status==0&&car.pit_status!=0;
        // Unserved penalty counters are the only signal that a pit visit is a
        // drive-through/stop-go rather than a strategic tyre stop.
        if(enteredPits)experience.penalty_visit=penaltyPending(car);
        if(justStopped&&!experience.penalty_visit){experience.recent_laps.clear();}
        if((justStopped||enteredPits)&&!experience.penalty_visit)experience.last_pit_lap=car.lap_num;
        experience.num_pit_stops=car.num_pit_stops;
        experience.pit_status=car.pit_status;
        if(car.pit_status==0)experience.penalty_visit=false;
        if(car.lap_num>experience.observed_lap){
            // Lap 1 includes the standing start and is not representative race pace.
            const int completedLap=car.lap_num-1;
            const bool settledTyres=experience.tyre_age_laps<0||experience.tyre_age_laps>1;
            // lap_invalid in this row already belongs to the new lap; use the
            // flags latched while the completed lap was being driven.
            const bool cleanLap=experience.observed_lap==completedLap&&
                experience.tainted_lap!=completedLap;
            if(completedLap>=2&&!neutralised&&!justStopped&&settledTyres&&cleanLap&&
               car.result_status==2&&car.pit_status==0&&validRaceLapMs(car.last_lap_ms)){
                const double relative=player?(double)car.gap_ms-player->gap_ms:0.0;
                experience.recent_laps.push_back({completedLap,car.last_lap_ms,relative,
                    car.position,experience.tyre_age_laps});
                if(experience.recent_laps.size()>5)experience.recent_laps.erase(experience.recent_laps.begin());
                if(car.idx==r.player_idx&&car.driver_status!=2&&car.driver_status!=3)
                    rememberPaceLap(completedLap,car.last_lap_ms);
            }
            experience.observed_lap=car.lap_num;
        }
        if(car.lap_num>0&&(car.lap_invalid||car.pit_status!=0||neutralised))experience.tainted_lap=car.lap_num;
    }
    refreshRivals();
    const TimingCar* p=player;
    if(!p){haveAheadGap_=haveBehindGap_=false;return;}
    const TimingCar* a=atPosition(r,p->position-1); const TimingCar* b=atPosition(r,p->position+1);
    // A trend belongs to one car; after a pass it restarts as unknown. The
    // reference only moves when the trend is (re)decided, so gradual changes
    // accumulate instead of vanishing below the per-row threshold.
    auto trend=[](const TimingCar* car,double gap,double& reference,int& idx,bool& have,int& value){
        if(!car){have=false;idx=-1;value=-1;return;}
        if(!have||idx!=car->idx){reference=gap;idx=car->idx;have=true;value=-1;return;}
        if(std::abs(gap-reference)>30){value=gap<reference?1:0;reference=gap;}
    };
    trend(a,a?(double)p->gap_ms-a->gap_ms:0.0,previousAheadGap_,aheadGapIdx_,haveAheadGap_,aheadTrend_);
    trend(b,b?(double)b->gap_ms-p->gap_ms:0.0,previousBehindGap_,behindGapIdx_,haveBehindGap_,behindTrend_);
}
void StrategyProcessor::ingest(const LapRow&r){
    completeLap(r.lap_num,r.last_lap_ms);
    completedStops_=std::max(completedStops_,r.num_pit_stops);
    const bool neutralised=session_&&(session_->safety_car_status==1||session_->safety_car_status==2);
    const bool advanced=lap_&&r.lap_num>lap_->lap_num;
    // Judge the completed lap on the flags latched while it was driven; this
    // row's lap_invalid/pit flags already describe the new lap.
    const bool clean=advanced&&r.lap_num==lap_->lap_num+1&&playerTaintedLap_!=lap_->lap_num&&
        r.pit_status==0&&r.driver_status!=2&&r.driver_status!=3;
    const bool settled=!status_||status_->tyre_age_laps>1;
    // Lap 1 includes the standing start and is not representative race pace.
    if(r.lap_num>2&&validRaceLapMs(r.last_lap_ms)&&!neutralised&&clean&&settled)
        rememberPaceLap(r.lap_num-1,r.last_lap_ms);
    if(r.lap_num>1&&r.lap_num-1>lastWearLap_&&damage_&&status_&&!neutralised){
        wearHistory_.push_back({r.lap_num-1,status_->tyre_compound,damage_->tyre_wear_fl,
            damage_->tyre_wear_fr,damage_->tyre_wear_rl,damage_->tyre_wear_rr});
        if(wearHistory_.size()>6)wearHistory_.erase(wearHistory_.begin());
        lastWearLap_=r.lap_num-1;
    }
    lap_=r;observeStint();
    if(r.lap_num>0&&(r.lap_invalid||r.pit_status!=0||neutralised))playerTaintedLap_=r.lap_num;
    // Finalize the previous lap's decision with the position at the new lap.
    if(advanced)commitDecisions();
}

void StrategyProcessor::ingestJson(std::string_view json){
    // V6 recordings store a driver's status and damage as separate field
    // families, so playback delivers them as partial rows. Apply those onto the
    // current state: parsed on their own, every absent field reads as zero, and
    // a zero tyre compound holds Strategy in its waiting state.
    if(json.find("\"_v6_type\":")!=std::string_view::npos){
        if(json.find("\"type\":\"status\"")!=std::string_view::npos){
            StatusRow merged=status_.value_or(StatusRow{});
            if(!glz::read<kPatch>(merged,json))ingest(merged);
            return;
        }
        if(json.find("\"type\":\"damage\"")!=std::string_view::npos){
            DamageRow merged=damage_.value_or(DamageRow{});
            if(!glz::read<kPatch>(merged,json))ingest(merged);
            return;
        }
    }
    auto parsed=parseRow(json); if(!parsed)return;
    std::visit([this](const auto&r){
        using T=std::decay_t<decltype(r)>;
        if constexpr(std::is_same_v<T,LapRow>||std::is_same_v<T,SessionRow>||std::is_same_v<T,StatusRow>||
                     std::is_same_v<T,DamageRow>||std::is_same_v<T,TimingRow>||std::is_same_v<T,ParticipantsRow>||
                     std::is_same_v<T,TyreSetsRow>||std::is_same_v<T,AllStatusRow>||std::is_same_v<T,RaceEventRow>) ingest(r);
    },*parsed);
}

StrategySnapshotRow StrategyProcessor::snapshot(){return makeSnapshot(true,false);}

StrategySnapshotRow StrategyProcessor::makeSnapshot(bool includeHistory,bool commit){
    StrategySnapshotRow out;
    // Decision state is evaluated on copies; only a commit writes it back.
    std::string heldRecommendation=neutralisationRecommendation_;
    std::vector<StrategyDecisionRecord> decisions=decisionHistory_;
    std::array<int,2> currentExpected=stints_.empty()?std::array<int,2>{}:stints_.back().expected_laps;
    out.session_time=std::max({lap_?lap_->session_time:0.0f,status_?status_->session_time:0.0f,
        damage_?damage_->session_time:0.0f,timing_?timing_->session_time:0.0f,
        tyreSets_?tyreSets_->session_time:0.0f,allStatus_?allStatus_->session_time:0.0f});
    if(lap_)out.lap_num=lap_->lap_num;if(session_)out.total_laps=session_->total_laps;
    const bool race=session_&&(session_->session_type==15||session_->session_type==16||session_->session_type==17);
    if(!race){out.state="non_race";return out;}
    const bool neutralised=session_&&(session_->safety_car_status==1||session_->safety_car_status==2);
    // Ready as soon as the core rows exist. Early laps need no gate: the pace
    // filters already exclude the standing-start lap, and targets without a
    // pace estimate are reported as unknown rather than guessed.
    if(!lap_||!session_||!status_||!damage_||status_->tyre_compound<=0||session_->total_laps<=0||
       lap_->lap_num<=0){out.state="waiting";return out;}
    out.state="ready";out.current_actual_compound=status_->tyre_compound;out.current_visual_compound=status_->visual_compound;out.current_tyre_age_laps=status_->tyre_age_laps;
    out.current_compound_name=tyreName(format_,status_->tyre_compound);out.is_monaco=session_->track_id==5;
    std::vector<float> sourceTimes{lap_->session_time,status_->session_time,damage_->session_time};
    if(timing_)sourceTimes.push_back(timing_->session_time);
    if(allStatus_)sourceTimes.push_back(allStatus_->session_time);
    out.data_age_s=0;
    for(float source:sourceTimes)out.data_age_s=std::max(out.data_age_s,std::max(0.0,(double)out.session_time-source));
    out.confidence=.96;
    if(!timing_)out.confidence-=.18;
    if(!allStatus_)out.confidence-=.10;
    if(!tyreSets_)out.confidence-=.18;
    out.confidence-=std::min(.25,out.data_age_s*.04);
    out.confidence=std::clamp(out.confidence,.20,.98);
    out.wear_fl=damage_->tyre_wear_fl;out.wear_fr=damage_->tyre_wear_fr;out.wear_rl=damage_->tyre_wear_rl;out.wear_rr=damage_->tyre_wear_rr;
    const double wears[4]={out.wear_fl,out.wear_fr,out.wear_rl,out.wear_rr}; const char* names[4]={"FL","FR","RL","RR"};
    out.average_wear=(wears[0]+wears[1]+wears[2]+wears[3])/4.0;
    auto historicWearRate=[&](int corner){
        double weighted=0,weights=0;int recent=0;
        for(size_t i=wearHistory_.size();i>1;--i){
            const auto&now=wearHistory_[i-1];const auto&before=wearHistory_[i-2];
            if(now.actual_compound!=status_->tyre_compound||before.actual_compound!=status_->tyre_compound)continue;
            const int laps=now.lap-before.lap;if(laps<=0)continue;
            auto value=[&](const WearSample&s){switch(corner){case 0:return s.fl;case 1:return s.fr;case 2:return s.rl;default:return s.rr;}};
            const double rate=(value(now)-value(before))/laps;
            if(rate<0||rate>12)continue;
            const double weight=std::pow(.65,recent++);weighted+=rate*weight;weights+=weight;
            if(recent==4)break;
        }
        return weights>0?weighted/weights:-1.0;
    };
    double wearRates[4];
    for(int i=0;i<4;++i){
        wearRates[i]=historicWearRate(i);
        if(wearRates[i]<0)wearRates[i]=status_->tyre_age_laps>0?wears[i]/status_->tyre_age_laps:2.0;
    }
    out.wear_per_lap=(wearRates[0]+wearRates[1]+wearRates[2]+wearRates[3])/4.0;
    const double cliff=(status_->visual_compound==16||status_->visual_compound==17||status_->visual_compound==18)?80.0:70.0;
    int limit=0;double best=1e9;
    for(int i=0;i<4;++i){double left=(cliff-wears[i])/std::max(.01,wearRates[i]);if(left<best){best=left;limit=i;}}
    out.limiting_corner=names[limit];out.limiting_wear=wears[limit];out.limiting_wear_per_lap=std::max(.01,wearRates[limit]);
    out.laps_until_cliff=std::max(0,(int)std::floor((cliff-out.limiting_wear)/out.limiting_wear_per_lap));out.cliff_lap=out.lap_num+out.laps_until_cliff;
    const PitLoss pit=pitLossForTrack(session_->track_id);
    if(pit.estimated)out.confidence=std::clamp(out.confidence-.08,.20,.98);
    const double effectivePitLoss=neutralised?std::max(0.0,pit.total_ms-10000.0):pit.total_ms;

    // Condition changes follow pace, not the weather label: the game's per-set
    // lap delta (vs the fitted set) already reflects how wet the track is. Each
    // other tyre category is judged by its least-worn available set (a new set
    // whenever one exists), and a switch is worthwhile only when that gain,
    // held for the rest of the race, repays the stop.
    const int fittedCategory=tyreCategory(status_->tyre_compound);
    const int remainingRaceLaps=std::max(0,out.total_laps-out.lap_num);
    // Right after a stop the sets packet can still mark the old tyres as
    // fitted; its deltas are then relative to the wrong set, so ignore them.
    const bool setDeltasCurrent=tyreSets_&&std::any_of(tyreSets_->sets.begin(),tyreSets_->sets.end(),
        [&](const TyreSet&s){return s.fitted&&s.actual_compound==status_->tyre_compound;});
    auto representativeSet=[&](int category)->const TyreSet*{
        const TyreSet* chosen=nullptr;
        if(setDeltasCurrent)for(const auto&s:tyreSets_->sets){
            if(!s.available||s.fitted||tyreLife(s)<=0||tyreCategory(s.actual_compound)!=category)continue;
            if(!chosen||s.wear<chosen->wear||(s.wear==chosen->wear&&s.lap_delta_ms<chosen->lap_delta_ms))chosen=&s;
        }
        return chosen;
    };
    constexpr double kMinConditionGainMs=250.0;
    auto conditionSwitchWorthwhile=[&](const TyreSet& set){
        const double gain=-(double)set.lap_delta_ms;
        return gain>=kMinConditionGainMs&&remainingRaceLaps>0&&gain*remainingRaceLaps>=effectivePitLoss;
    };
    int switchCategory=-1;const TyreSet* switchSet=nullptr;
    for(int category:{kSlick,kInter,kFullWet}){
        if(category==fittedCategory)continue;
        const TyreSet* set=representativeSet(category);
        if(set&&conditionSwitchWorthwhile(*set)&&(!switchSet||set->lap_delta_ms<switchSet->lap_delta_ms)){
            switchCategory=category;switchSet=set;
        }
    }
    // Without tyre-set data only the weather label is available.
    const int planCategory=switchSet?switchCategory:fittedCategory;
    const bool wet=tyreSets_?planCategory!=kSlick:
        session_->weather>=3||wetActual(status_->tyre_compound);
    std::vector<TyreSet> base;
    if(tyreSets_)for(const auto&s:tyreSets_->sets)
        if(s.available&&!s.fitted&&tyreLife(s)>0&&tyreCategory(s.actual_compound)==planCategory)
            base.push_back(s);
    auto cons=base,agg=base;
    std::sort(cons.begin(),cons.end(),[](auto&a,auto&b){return tyreLife(a)>tyreLife(b);});
    std::sort(agg.begin(),agg.end(),[](auto&a,auto&b){return a.lap_delta_ms<b.lap_delta_ms;});
    const TyreSet* fitted=nullptr;
    if(tyreSets_)for(const auto&s:tyreSets_->sets)if(s.fitted){fitted=&s;break;}
    // The fitted set may be refitted later, but only with the life left after
    // the current stint has run on it.
    auto futureWith=[&](std::vector<TyreSet> pool,int currentStintLaps){
        if(fitted&&fitted->available&&tyreCategory(fitted->actual_compound)==planCategory){
            const int remaining=tyreLife(*fitted)-std::max(0,currentStintLaps);
            if(remaining>2){
                TyreSet later=*fitted;
                later.fitted=false;later.life_span=remaining;later.usable_life=remaining;
                pool.push_back(later);
            }
        }
        return pool;
    };

    const double fittedDelta=fitted?fitted->lap_delta_ms:0.0;
    const double conservativeFresh=!cons.empty()?std::max(0.0,fittedDelta-cons.front().lap_delta_ms):0.0;
    const double fresh=!agg.empty()?std::max(0.0,fittedDelta-agg.front().lap_delta_ms):0.0;
    const TimingCar* player=timing_?findCar(*timing_,timing_->player_idx):nullptr;
    auto experienceFor=[&](int idx)->const RivalExperience*{
        auto it=rivalExperience_.find(idx);
        return it==rivalExperience_.end()?nullptr:&it->second;
    };
    const auto*playerExperience=player?experienceFor(player->idx):nullptr;
    const bool playerRecentlyStopped=(playerExperience&&
        playerExperience->last_pit_lap>=out.lap_num-1)||
        (stints_.size()>1&&currentStintStart_>=out.lap_num-1);
    const bool pitCallActionable=player&&player->pit_status==0&&
        status_->tyre_age_laps>1&&!playerRecentlyStopped;
    auto rivalStartedUnmatchedStop=[&](int idx){
        if(!pitCallActionable||idx<0)return false;
        const auto*rival=findCar(*timing_,idx);
        const auto*experience=experienceFor(idx);
        if(!rival||!experience||experience->last_pit_lap<out.lap_num-1)return false;
        // Stop counts differ legitimately between strategies; what matters is
        // whether the player has stopped since this rival's strategic stop.
        const int playerLastStop=std::max(playerExperience?playerExperience->last_pit_lap:-1,
            stints_.size()>1?currentStintStart_:-1);
        return playerLastStop<experience->last_pit_lap;
    };
    const double playerLap=playerPaceMs();
    const double aheadPace=rivalPaceMs(rivalAhead_);
    const double behindPace=rivalPaceMs(rivalBehind_);
    const double defensivePace=behindPace>0?behindPace:aheadPace;
    double attackingPace=0.0;
    if(aheadPace>0&&behindPace>0)attackingPace=std::min(aheadPace,behindPace);
    else attackingPace=std::max(aheadPace,behindPace);

    // A pace-justified switch is called now. Otherwise the forecast gives a
    // heads-up for conditions that differ from both the fitted tyre and the
    // current weather (the current weather is already judged by pace). Offsets
    // are minutes from the packet; m_weather sets the category, while
    // m_rainPercentage is only the chance of rain and scales confidence.
    if(!session_->weather_forecast_samples.empty()||switchSet){
        const int nowCategory=weatherCategory(session_->weather);
        const int nowRain=session_->weather_forecast_samples.empty()?0:
            session_->weather_forecast_samples.front().rain_percentage;
        StrategyWeatherDecision weather;
        weather.recommendation=fittedCategory==kSlick?"stay_dry":"stay_wet";
        weather.target_compound=categoryCompound(fittedCategory);
        weather.reason="no_crossover_forecast";
        weather.forecast_weather=session_->weather;weather.rain_percentage=nowRain;
        auto describeSet=[&](const TyreSet* set){
            if(!set)return;
            weather.lap_delta_ms=set->lap_delta_ms;weather.set_wear=set->wear;
        };
        auto prepare=[&](int target){
            return target==kSlick?"prepare_slicks":target==kInter?"prepare_intermediates":"prepare_wets";
        };
        double likelihood=1.0;
        bool forecastCall=false;
        if(switchSet){
            weather.recommendation=prepare(switchCategory);
            weather.target_compound=categoryCompound(switchCategory);
            weather.reason="faster_tyre_available";
            weather.crossover_lap=out.lap_num;
            describeSet(switchSet);
        }else{
            for(const auto&sample:session_->weather_forecast_samples){
                if(sample.time_offset<=0||sample.time_offset>60)continue;
                const int target=weatherCategory(sample.weather);
                if(target==fittedCategory||target==nowCategory)continue;
                weather.forecast_weather=sample.weather;weather.rain_percentage=sample.rain_percentage;
                weather.minutes_until_change=sample.time_offset;
                const double lapDuration=playerLap>0?playerLap:90000.0;
                weather.crossover_lap=std::min(out.total_laps,
                    out.lap_num+(int)std::ceil(sample.time_offset*60000.0/lapDuration));
                weather.recommendation=prepare(target);
                weather.target_compound=categoryCompound(target);
                const bool wetter=target>fittedCategory;
                weather.reason=fittedCategory==kSlick?"rain_crossover_forecast":
                    target==kSlick?"dry_crossover_forecast":
                    wetter?"heavier_rain_forecast":"lighter_rain_forecast";
                const double chance=std::clamp(sample.rain_percentage/100.0,0.0,1.0);
                likelihood=std::max(.35,wetter?chance:1.0-chance);
                describeSet(representativeSet(target));
                forecastCall=true;
                break;
            }
            // The weather now suggests other tyres, but pace says stay.
            if(!forecastCall&&nowCategory!=fittedCategory){
                const TyreSet* alternative=representativeSet(nowCategory);
                weather.reason=!alternative?"no_set_for_conditions":
                    alternative->lap_delta_ms>=0?"current_tyre_still_faster":"switch_not_worth_pit_loss";
                describeSet(alternative);
            }
        }
        // Pace calls come from observed game deltas; forecasts lose confidence
        // with accuracy, horizon and rain chance.
        weather.confidence=switchSet?.90:(session_->forecast_accuracy==0 ? .86 : .62)*
            std::max(.45,1.0-weather.minutes_until_change/90.0)*likelihood;
        out.weather_strategy=weather;
    }

    if(neutralised&&player&&timing_){
        std::vector<NeutralCarState> basis=frozenNeutralCars_;
        if(basis.empty())for(const auto&car:timing_->cars)basis.push_back({car.idx,car.position,car.lap_num,
            (double)car.gap_ms,car.pit_status,car.num_pit_stops,car.result_status});
        const auto playerBasis=std::find_if(basis.begin(),basis.end(),[&](const auto&car){return car.idx==player->idx;});
        const double playerGap=playerBasis!=basis.end()?playerBasis->gap_ms:player->gap_ms;
        const int playerLapNum=playerBasis!=basis.end()?playerBasis->lap_num:player->lap_num;
        const int playerPosition=playerBasis!=basis.end()&&playerBasis->position>0?playerBasis->position:player->position;
        const bool playerGapValid=playerPosition==1||playerGap>0;
        const double lapWindow=playerLap>0?playerLap:90000.0;
        int rivalsBoxing=0;double queueLoss=0;
        auto teamFor=[&](int idx){if(participants_)for(const auto&driver:participants_->drivers)if(driver.idx==idx)return driver.team_id;return -1;};
        const int playerTeam=teamFor(player->idx);
        for(const auto&car:timing_->cars){
            if(car.idx==player->idx||car.result_status!=2||car.pit_status==0)continue;
            ++rivalsBoxing;queueLoss+=350.0;
            if(playerTeam>=0&&teamFor(car.idx)==playerTeam)queueLoss+=2500.0;
        }
        queueLoss=std::min(queueLoss,5000.0);
        auto projectedPosition=[&](double playerCost){
            int position=1;
            for(const auto&car:basis){
                if(car.idx==player->idx||car.result_status!=2||car.position<=0||retiredCars_.count(car.idx))continue;
                // Leader deltas order cars within one racing lap. Lap numbers
                // alone mislabel a car that has just crossed the line, so they
                // only rule out cars that are genuinely a lap or more apart;
                // those keep their race order whatever either car does.
                const double interval=car.gap_ms-playerGap;
                const bool gapValid=playerGapValid&&(car.position==1||car.gap_ms>0);
                const bool contested=gapValid&&std::abs(car.lap_num-playerLapNum)<=1&&
                    std::abs(interval)<lapWindow;
                if(!contested){if(car.position<playerPosition)++position;continue;}
                double rivalCost=0;
                const auto live=findCar(*timing_,car.idx);
                auto exp=rivalExperience_.find(car.idx);
                const bool boxing=(live&&live->pit_status!=0&&
                        !(exp!=rivalExperience_.end()&&exp->second.penalty_visit))||
                    (neutralisationStartLap_>0&&exp!=rivalExperience_.end()&&
                     exp->second.last_pit_lap>=neutralisationStartLap_);
                // A rival's stop under the same neutralisation costs what ours does.
                if(boxing)rivalCost=effectivePitLoss;
                if(car.gap_ms+rivalCost<playerGap+playerCost)++position;
            }
            return position;
        };
        const int remainingLaps=std::max(0,out.total_laps-out.lap_num);
        const int usefulFreshLaps=!agg.empty()?std::min(remainingLaps,tyreLife(agg.front())):0;
        const double recoverable=fresh*usefulFreshLaps;
        const int waitLaps=std::clamp(out.laps_until_cliff-1,1,3);
        const double degradationCost=waitLaps*std::max(0.0,out.limiting_wear_per_lap-1.0)*120.0;
        const double boxNowCost=effectivePitLoss+queueLoss-recoverable;
        const double boxLaterRecoverable=fresh*std::max(0,usefulFreshLaps-waitLaps);
        const double boxLaterCost=pit.total_ms+degradationCost-boxLaterRecoverable;
        const double advantage=boxLaterCost-boxNowCost;
        const int boxPosition=projectedPosition(effectivePitLoss+queueLoss);
        const int stayPosition=projectedPosition(0);
        const int laterPosition=projectedPosition(pit.total_ms);
        // Tyres at the cliff only force a stop if the race outlasts them.
        const bool stopDue=out.laps_until_cliff<=2&&remainingLaps>out.laps_until_cliff;
        const bool replacementReachesFinish=!agg.empty()&&tyreLife(agg.front())>=remainingLaps;
        const bool resolvesRequiredStop=remainingLaps>out.laps_until_cliff&&replacementReachesFinish;
        // Required stops = 0 disables the compound rule, as for the plans.
        const bool dryCompoundRequired=minimumStops_>0&&!wet&&usedDryVisualCompounds_.size()<2&&
            std::any_of(agg.begin(),agg.end(),[&](const auto&set){return set.visual_compound!=status_->visual_compound;});
        const bool freeStop=boxPosition<=player->position&&remainingLaps>=3;
        const bool canBox=!agg.empty()&&remainingLaps>0;
        bool proposedBox=canBox&&(stopDue||resolvesRequiredStop||dryCompoundRequired||freeStop||advantage>750.0);
        std::string reason;
        if(!canBox)reason="no_usable_set";
        else if(stopDue)reason="tyres_at_cliff";
        else if(resolvesRequiredStop)reason="tyres_cannot_finish";
        else if(dryCompoundRequired)reason="mandatory_compound_change";
        else if(freeStop)reason="free_stop";
        else if(advantage>750.0)reason="box_now_beats_box_later";
        else reason="protect_track_position";
        const std::string proposed=proposedBox?"box":"stay_out";
        if(heldRecommendation.empty())heldRecommendation=proposed;
        else if(proposed!=heldRecommendation){
            const bool strongSwitch=(proposed=="box"&&advantage>1500.0)||
                (proposed=="box"&&(stopDue||resolvesRequiredStop||dryCompoundRequired||freeStop))||
                (proposed=="stay_out"&&advantage<-1500.0)||!canBox;
            if(strongSwitch)heldRecommendation=proposed;
            else reason="decision_held_by_hysteresis";
        }
        std::optional<int> lapsToRecover;
        if(fresh>0)lapsToRecover=(int)std::ceil(effectivePitLoss/fresh);
        StrategyNeutralisation decision;
        decision.kind=session_->safety_car_status==1?"safety_car":"virtual_safety_car";
        decision.recommendation=heldRecommendation;decision.reason=reason;
        decision.normal_pit_loss_ms=pit.total_ms;decision.effective_pit_loss_ms=effectivePitLoss;
        decision.queue_loss_ms=queueLoss;decision.recoverable_time_ms=recoverable;
        decision.net_time_ms=recoverable-effectivePitLoss-queueLoss;
        decision.box_now_cost_ms=boxNowCost;decision.box_later_cost_ms=boxLaterCost;
        decision.box_now_advantage_ms=advantage;decision.box_later_lap=std::min(out.total_laps,out.lap_num+waitLaps);
        decision.current_position=player->position;
        decision.projected_box_position=boxPosition;decision.projected_stay_position=stayPosition;
        decision.projected_later_box_position=laterPosition;
        decision.positions_lost=std::max(0,boxPosition-player->position);
        decision.rivals_boxing=rivalsBoxing;
        decision.decision_lap=neutralisationStartLap_>0?neutralisationStartLap_:out.lap_num;
        decision.data_age_s=out.data_age_s;decision.position_basis="deployment_gaps";
        decision.confidence=std::clamp(out.confidence-(frozenNeutralCars_.empty() ? .12 : 0.0),.20,.98);
        decision.laps_to_recover=lapsToRecover;
        decision.factors.push_back({"pit_loss_discount","SC/VSC pit loss reduced by 10.0 seconds",-10000.0});
        decision.factors.push_back({"pit_queue","Estimated pit-lane and team stacking delay",queueLoss});
        decision.factors.push_back({"fresh_tyre_gain","Estimated remaining fresh-tyre gain",-recoverable});
        decision.factors.push_back({"delay_cost","Cost of waiting for the planned stop",boxLaterCost-boxNowCost});
        out.neutralisation=std::move(decision);
    }

    int first=(conservativeFresh>500&&conservativeFresh*out.laps_until_cliff>effectivePitLoss)
        ?std::max(out.lap_num+1,out.lap_num+(int)std::ceil(effectivePitLoss/conservativeFresh))
        :out.cliff_lap;
    if(rivalStartedUnmatchedStop(rivalBehind_))
        first=out.lap_num+1;
    if(out.neutralisation&&out.neutralisation->recommendation=="box")first=out.lap_num+1;
    if(switchSet)first=out.lap_num+1;
    const int completedStops=completedStops_;
    RawPlan cp=buildPlan(format_,out.lap_num,out.total_laps,first,status_->tyre_compound,status_->visual_compound,cons);
    const auto futureCons=futureWith(cons,cp.stints.front().lapCount);
    enforceMinimumStops(format_,cp,futureCons,std::max(0,minimumStops_-completedStops));
    const int aggLeft=std::max(0,(int)std::floor((cliff-out.limiting_wear)/(out.limiting_wear_per_lap*1.2)));
    const int aggCliff=out.lap_num+aggLeft;
    const int econ=!agg.empty()?std::max(out.lap_num+1,out.total_laps-tyreLife(agg.front())):aggCliff;
    int aggressiveFirst=std::min(aggCliff,econ);
    if(rivalStartedUnmatchedStop(rivalAhead_)&&out.laps_until_cliff>2)
        aggressiveFirst=std::max(aggressiveFirst,out.lap_num+2);
    if(out.neutralisation&&out.neutralisation->recommendation=="box")aggressiveFirst=out.lap_num+1;
    if(switchSet)aggressiveFirst=out.lap_num+1;
    RawPlan ap=buildPlan(format_,out.lap_num,out.total_laps,aggressiveFirst,status_->tyre_compound,status_->visual_compound,agg);
    const auto futureAgg=futureWith(agg,ap.stints.front().lapCount);
    if(ap.stops<=cp.stops)ap=forceExtra(format_,ap,futureAgg);
    if(minimumStops_>0)enforceMinimumStops(format_,ap,futureAgg,std::max(0,minimumStops_+1-completedStops));
    if(!wet&&minimumStops_>0){enforceDryCompoundRule(format_,cp,futureCons,usedDryVisualCompounds_);enforceDryCompoundRule(format_,ap,futureAgg,usedDryVisualCompounds_);}

    const auto completedLaps=includeHistory?raceHistory_.laps():
        std::array<const StrategyRaceHistory::Lap*,StrategyRaceHistory::MAX_LAPS+1>{};
    auto display=[&](const RawPlan& raw,bool aggressive){
        const size_t planIndex=aggressive?1:0;
        StrategyPlan plan;
        plan.stops=raw.stops;
        plan.mode=aggressive?"attacking":"defensive";
        plan.target_idx=aggressive?rivalAhead_:rivalBehind_;
        plan.target_name=plan.target_idx>=0?driverName(participants_,plan.target_idx):"No active rival";
        plan.reason=plan.target_idx>=0?(aggressive?"close_or_pass_scored_rival":"protect_from_scored_rival"):
            "manage_tyre_life_and_race_distance";
        plan.confidence=out.confidence;
        plan.legal=raw.legal;plan.legality_reason=raw.legalityReason;
        plan.requires_compound_change=raw.requiresCompoundChange;
        const int opening=currentStintStart_>0?currentStintStart_:std::max(1,out.lap_num-status_->tyre_age_laps);
        // The attacking plan must repay only the stops it adds over the
        // defensive plan, spread across the laps left to do it in.
        const int extraStops=std::max(0,ap.stops-cp.stops);
        const double offset=aggressive&&extraStops>0?
            extraStops*effectivePitLoss/std::max(1,out.total_laps-out.lap_num):0;
        const double rivalTarget=aggressive?attackingPace:defensivePace;
        const auto& availableSets=aggressive?futureAgg:futureCons;
        auto targetFor=[&](const RawStint&s,size_t i){
            if(!validRaceLapMs((int)playerLap))return 0.0;
            if(i==0)return std::max(60000.0,(rivalTarget>0?rivalTarget:playerLap)-offset);
            for(const auto&set:availableSets)if((s.setIdx>=0&&set.idx==s.setIdx)||
                                      (s.setIdx<0&&set.actual_compound==s.actual)){
                double estimate=playerLap+set.lap_delta_ms-fittedDelta-offset;
                if(rivalTarget>0)estimate=std::min(estimate,rivalTarget-offset);
                return std::max(60000.0,estimate);
            }
            return 0.0;
        };
        if(!stints_.empty()&&!raw.stints.empty()){
            int end=raw.stints[0].last?out.total_laps:raw.stints[0].pitLap;
            currentExpected[planIndex]=std::max(1,end-opening+1);
        }
        double raceCum=0;
        auto rows=[&](int start,int end,double req,bool post,bool pre){
            std::vector<StrategyLapTarget> values;
            double cum=0;
            const int first=includeHistory?start:std::max(start,out.lap_num);
            const int last=includeHistory?end:std::min(end,out.lap_num);
            for(int n=first;n>0&&n<=last&&n<=StrategyRaceHistory::MAX_LAPS;++n){
                double lapBase=req;
                const auto* completed=completedLaps[n];
                if(n<out.lap_num)lapBase=completed?completed->required_base_ms[planIndex]:0.0;
                StrategyLapTarget row;
                row.lap_num=n;
                row.required_ms=lapBase>0?lapBase+(includeHistory&&post&&n==start?pit.outlap_ms:0)+
                    (includeHistory&&pre&&n==end?pit.inlap_ms+pit.stationary_ms():0):0;
                if(n<out.lap_num&&completed&&completed->actual_ms>0){
                    row.has_actual=true;
                    row.actual_ms=completed->actual_ms;
                    if(row.required_ms>0){
                        row.delta_lap_ms=row.actual_ms-row.required_ms;
                        cum+=row.delta_lap_ms;
                        raceCum+=row.delta_lap_ms;
                    }
                    row.delta_stint_ms=cum;
                    row.delta_total_ms=raceCum;
                }
                values.push_back(row);
            }
            return values;
        };
        for(size_t k=0;includeHistory&&k+1<stints_.size();++k){
            const auto&ps=stints_[k];
            int end=stints_[k+1].start_lap-1;
            if(end<ps.start_lap)continue;
            StrategyStint stint;
            stint.compound_name=tyreName(format_,ps.actual_compound);stint.actual_compound=ps.actual_compound;
            stint.visual_compound=ps.visual_compound;stint.stint_number=(int)plan.stints.size()+1;
            stint.start_lap=ps.start_lap;stint.end_lap=end;
            stint.expected_laps=ps.expected_laps[planIndex]>0?ps.expected_laps[planIndex]:end-ps.start_lap+1;
            stint.actual_laps=end-ps.start_lap+1;
            stint.rows=rows(stint.start_lap,stint.end_lap,0,k>0,true);
            plan.stints.push_back(std::move(stint));
        }
        const bool pitted=completedStops_>0;
        for(size_t i=0;i<raw.stints.size();++i){
            const auto&rs=raw.stints[i];
            StrategyStint stint;
            stint.compound_name=rs.name;stint.actual_compound=rs.actual;stint.visual_compound=rs.visual;
            stint.stint_number=(int)plan.stints.size()+1;
            // Adjacent stints must partition the race; the pit lap belongs to
            // the outgoing stint and must not be displayed/count toward Δ twice.
            stint.start_lap=i==0?opening:rs.startLap+1;
            stint.end_lap=rs.last?out.total_laps:rs.pitLap;
            stint.expected_laps=i==0?std::max(1,stint.end_lap-stint.start_lap+1):rs.lapCount;
            stint.is_last=rs.last;
            const double target=targetFor(rs,i);
            stint.rows=rows(stint.start_lap,stint.end_lap,target,i==0?pitted:true,!rs.last);
            for(const auto&row:stint.rows)if(row.has_actual)++stint.actual_laps;
            plan.stints.push_back(std::move(stint));
        }
        return plan;
    };
    out.conservative=display(cp,false);
    out.aggressive=display(ap,true);
    out.explanation.push_back({"recent_degradation","Cliff uses recent completed-lap wear deltas when available",
        out.limiting_wear_per_lap});
    if(rivalBehind_>=0)out.explanation.push_back({"defensive_rival","Defensive plan targets the highest scored car behind",
        rivalThreatScore(rivalBehind_,false)});
    if(rivalAhead_>=0)out.explanation.push_back({"attacking_rival","Attacking plan targets the highest scored car ahead",
        rivalThreatScore(rivalAhead_,true)});
    if(!cp.legal||!ap.legal)out.explanation.push_back({"tyre_legality","Available physical tyre sets cannot form a fully legal plan",0});
    if(pit.estimated)out.explanation.push_back({"pit_loss_estimated","Track pit loss is not catalogued; using a generic estimate",pit.total_ms});
    if(out.data_age_s>2.0)out.explanation.push_back({"stale_data","One or more strategy inputs are stale",out.data_age_s*1000.0});

    // Warnings: same thresholds and priority ordering as the two former UIs.
    const double left=(wears[0]+wears[2])/2,right=(wears[1]+wears[3])/2,front=(wears[0]+wears[1])/2,rear=(wears[2]+wears[3])/2,maxw=*std::max_element(wears,wears+4),diag=(wears[0]+wears[3]-wears[1]-wears[2])/2,rearImb=wears[2]-wears[3];
    struct W{std::string t,s;int p;};std::vector<W>w;
    if(maxw>out.average_wear+22){int i=(int)(std::max_element(wears,wears+4)-wears);w.push_back({std::string(names[i])+" wear far above average","danger",0});}
    auto sorted=std::vector<double>(wears,wears+4);std::sort(sorted.begin(),sorted.end(),std::greater<double>());if(maxw-sorted[1]>10){int i=(int)(std::max_element(wears,wears+4)-wears);const char* titles[]={"Front-left is the limiting tyre","Front-right is the limiting tyre","Rear-left is the limiting tyre","Rear-right is the limiting tyre"};w.push_back({titles[i],"danger",1});}
    if(std::abs(diag)>8)w.push_back({diag>0?"Front-left and rear-right wearing faster":"Front-right and rear-left wearing faster","warning",2});
    if(rear>front&&std::abs(rearImb)>8)w.push_back({rearImb>0?"Rear-left wearing faster than rear-right":"Rear-right wearing faster than rear-left","warning",2});
    if(std::abs(front-rear)>8)w.push_back({front>rear?"Front tyres wearing faster":"Rear tyres wearing faster","caution",3});
    if(std::abs(right-left)>5)w.push_back({right>left?"Right tyres wearing faster":"Left tyres wearing faster","caution",3});
    std::stable_sort(w.begin(),w.end(),[](auto&a,auto&b){return a.p<b.p;});if(w.size()>2)w.resize(2);for(auto&x:w)out.wear_warnings.push_back({x.t,x.s,x.p});

    if(player&&timing_){
        std::vector<const TimingCar*> ahead,behind;for(const auto&c:timing_->cars)if(c.result_status==2&&c.position>0&&!retiredCars_.count(c.idx)){if(c.position<player->position)ahead.push_back(&c);else if(c.position>player->position)behind.push_back(&c);}std::sort(ahead.begin(),ahead.end(),[](auto*a,auto*b){return a->position>b->position;});std::sort(behind.begin(),behind.end(),[](auto*a,auto*b){return a->position<b->position;});
        const int ac=std::min(3,(int)ahead.size()),bc=std::min(3,(int)behind.size());ahead.resize(std::min((int)ahead.size(),ac+std::max(0,3-bc)));behind.resize(std::min((int)behind.size(),bc+std::max(0,3-ac)));std::reverse(ahead.begin(),ahead.end());
        auto addPos=[&](const TimingCar&c,std::string role,bool immediate){double gap=role=="ahead"?player->gap_ms-c.gap_ms:role=="behind"?c.gap_ms-player->gap_ms:0;int trend=immediate?(role=="ahead"?aheadTrend_:role=="behind"?behindTrend_:-1):-1;out.positions.push_back({c.idx,driverName(participants_,c.idx),liveryColor(participants_,c.idx),role,c.position,c.pit_status,c.num_pit_stops,gap,trend,immediate});};
        for(size_t i=0;i<ahead.size();++i)addPos(*ahead[i],"ahead",i+1==ahead.size());addPos(*player,"player",false);for(size_t i=0;i<behind.size();++i)addPos(*behind[i],"behind",i==0);
        auto addRival=[&](int idx,const char*dir){
            if(idx<0)return;const auto*c=findCar(*timing_,idx);
            if(!c||c->result_status!=2||retiredCars_.count(idx))return;
            const bool isAhead=std::string(dir)=="ahead";
            StrategyRival rival;
            rival.idx=idx;rival.name=driverName(participants_,idx);rival.direction=dir;
            rival.position=c->position;rival.result_status=c->result_status;
            rival.gap_ms=isAhead?player->gap_ms-c->gap_ms:c->gap_ms-player->gap_ms;
            rival.threat_score=rivalThreatScore(idx,isAhead);rival.pace_ms=rivalPaceMs(idx);
            rival.pace_delta_ms=rival.pace_ms>0&&playerLap>0?rival.pace_ms-playerLap:0;
            if(auto it=rivalExperience_.find(idx);it!=rivalExperience_.end()){
                const auto&experience=it->second;
                rival.tyre_age_laps=std::max(0,experience.tyre_age_laps);
                rival.actual_compound=experience.tyre_compound;rival.visual_compound=experience.visual_compound;
                rival.last_pit_lap=experience.last_pit_lap;
                if(experience.recent_laps.size()>=2){
                    const auto&last=experience.recent_laps.back();
                    const auto&previous=experience.recent_laps[experience.recent_laps.size()-2];
                    rival.closing_ms_per_lap=closingMs(previous.relative_gap_ms,last.relative_gap_ms);
                }
                if(rivalStartedUnmatchedStop(idx))
                    rival.pit_reaction=isAhead?"extend_for_overcut":"cover_stop";
            }
            out.rivals.push_back(std::move(rival));
        };addRival(rivalAhead_,"ahead");addRival(rivalBehind_,"behind");
        auto age=[&](int idx){auto it=rivalExperience_.find(idx);return it==rivalExperience_.end()?0:std::max(0,it->second.tyre_age_laps);};
        if(!neutralised){
            if(const auto*b=findCar(*timing_,rivalBehind_);b&&
               rivalStartedUnmatchedStop(b->idx)&&out.laps_until_cliff>1){
                const double gap=b->gap_ms-player->gap_ms;
                out.call=StrategyCall{"cover",b->idx,driverName(participants_,b->idx),gap,
                    1,"rival_behind_has_pitted",out.lap_num};
            }
            if(!out.call)if(const auto*a=findCar(*timing_,rivalAhead_);a&&
               rivalStartedUnmatchedStop(a->idx)&&out.laps_until_cliff>2){
                const double gap=player->gap_ms-a->gap_ms;
                out.call=StrategyCall{"overcut",a->idx,driverName(participants_,a->idx),gap,
                    1,"rival_ahead_has_pitted",out.lap_num};
            }
            if(!out.call&&pitCallActionable)if(const auto*a=findCar(*timing_,rivalAhead_)){
                double gap=player->gap_ms-a->gap_ms;
                int cross=fresh>0?(int)std::ceil(std::max(0.0,pit.total_ms-gap)/fresh):999;
                if(gap>0&&gap<5000&&fresh>200&&age(a->idx)>=status_->tyre_age_laps+3&&cross<=10)
                    out.call=StrategyCall{"undercut",a->idx,driverName(participants_,a->idx),gap,cross,
                        "fresh_tyre_crossover_before_rival",out.lap_num};
            }
        }
    }

    // Finalize past records once. Current-lap records remain live, then become
    // immutable when the next lap starts, which keeps playback/seek history stable.
    for(auto&record:decisions)if(!record.actual_position&&out.lap_num>record.lap_num&&player){
        if(record.event=="neutralisation"){
            const bool stopped=player->num_pit_stops>record.start_num_pit_stops;
            if(!stopped&&neutralised&&out.lap_num<=record.lap_num+1)continue;
            record.actual_position=player->position;
            record.followed=record.recommendation=="box"?stopped:!stopped;
            if(*record.followed)record.successful=player->position<=record.projected_position;
        }else{
            record.actual_position=player->position;
            const auto* actual=raceHistory_.find(record.lap_num);
            const double required=actual?actual->required_base_ms[record.recommendation=="attack"?1:0]:0;
            if(actual&&actual->actual_ms>0&&required>0){
                record.followed=actual->actual_ms<=required*1.01;
                record.successful=*record.followed||player->position<=record.start_position;
            }
        }
    }
    if(player){
        auto upsert=[&](StrategyDecisionRecord record){
            auto existing=std::find_if(decisions.begin(),decisions.end(),[&](const auto&value){
                return value.event==record.event&&value.lap_num==record.lap_num;});
            if(existing==decisions.end())decisions.push_back(std::move(record));
            else if(!existing->actual_position){
                // Allow the current recommendation to evolve, but keep the
                // original position/stop baseline used to evaluate it later.
                existing->recommendation=std::move(record.recommendation);
                existing->reason=std::move(record.reason);
                existing->target_idx=record.target_idx;
                existing->target_name=std::move(record.target_name);
                existing->projected_position=record.projected_position;
            }
        };
        StrategyDecisionRecord lapDecision;
        lapDecision.event="lap_plan";lapDecision.lap_num=out.lap_num;lapDecision.session_time=out.session_time;
        lapDecision.recommendation=out.call&&(out.call->kind=="undercut"||out.call->kind=="overcut")?"attack":"defend";
        lapDecision.reason=out.call?out.call->reason:"rival_adjusted_plan";
        lapDecision.target_idx=lapDecision.recommendation=="attack"?rivalAhead_:rivalBehind_;
        lapDecision.target_name=lapDecision.target_idx>=0?driverName(participants_,lapDecision.target_idx):"";
        lapDecision.start_position=player->position;lapDecision.projected_position=player->position;
        lapDecision.start_num_pit_stops=player->num_pit_stops;upsert(std::move(lapDecision));
        if(out.neutralisation){
            StrategyDecisionRecord neutral;
            neutral.event="neutralisation";neutral.lap_num=out.neutralisation->decision_lap;
            neutral.session_time=neutralisationStartTime_;neutral.recommendation=out.neutralisation->recommendation;
            neutral.reason=out.neutralisation->reason;neutral.start_position=out.neutralisation->current_position;
            neutral.projected_position=neutral.recommendation=="box"?out.neutralisation->projected_box_position:
                out.neutralisation->projected_stay_position;
            neutral.start_num_pit_stops=player->num_pit_stops;upsert(std::move(neutral));
        }
    }
    if(commit){
        neutralisationRecommendation_=heldRecommendation;
        decisionHistory_=decisions;
        if(!stints_.empty())stints_.back().expected_laps=currentExpected;
    }
    out.decision_history=std::move(decisions);
    return out;
}

std::string StrategyProcessor::snapshotJson(){return writeJson(snapshot());}

StrategyProcessor::MemoryStats StrategyProcessor::memoryStats() const {
    MemoryStats stats;
    const auto stringCapacity = [](const std::string& value) {
        return value.capacity() + 1;
    };
    const auto mapCapacity = [](const auto& values) {
        using Value = typename std::decay_t<decltype(values)>::value_type;
        // std::map does not expose node allocation size. Include the stored
        // value plus a conservative three-pointer tree-node estimate.
        return values.size() * (sizeof(Value) + 3 * sizeof(void*));
    };
    const auto setCapacity = [](const auto& values) {
        using Value = typename std::decay_t<decltype(values)>::value_type;
        return values.size() * (sizeof(Value) + 3 * sizeof(void*));
    };

    const auto addBaseStrings = [&](const auto& row) {
        stats.stringCapacityBytes += stringCapacity(row.type);
        if constexpr (requires { row.ts; })
            stats.stringCapacityBytes += stringCapacity(row.ts);
    };
    if (lap_) addBaseStrings(*lap_);
    if (status_) addBaseStrings(*status_);
    if (damage_) addBaseStrings(*damage_);
    if (timing_) {
        addBaseStrings(*timing_);
        stats.cachedInputCapacityBytes += timing_->cars.capacity() * sizeof(TimingCar);
    }
    if (session_) {
        addBaseStrings(*session_);
        stats.cachedInputCapacityBytes +=
            session_->marshal_zones.capacity() * sizeof(MarshalZone) +
            session_->weather_forecast_samples.capacity() * sizeof(WeatherSample);
    }
    if (participants_) {
        stats.stringCapacityBytes += stringCapacity(participants_->type);
        stats.cachedInputCapacityBytes += participants_->drivers.capacity() * sizeof(Driver);
        for (const auto& driver : participants_->drivers)
            stats.stringCapacityBytes += stringCapacity(driver.name) +
                stringCapacity(driver.livery_color);
    }
    if (tyreSets_) {
        addBaseStrings(*tyreSets_);
        stats.cachedInputCapacityBytes += tyreSets_->sets.capacity() * sizeof(TyreSet);
    }
    if (allStatus_) {
        addBaseStrings(*allStatus_);
        stats.cachedInputCapacityBytes += allStatus_->cars.capacity() * sizeof(AllStatusCar);
    }

    stats.lapTimeEntries = lapTimes_.size();
    stats.rivalEntries = rivalExperience_.size();
    stats.wearHistoryEntries = wearHistory_.size();
    stats.frozenNeutralCarEntries = frozenNeutralCars_.size();
    stats.decisionHistoryEntries = decisionHistory_.size();
    stats.conservativePastEntries = stints_.size();
    stats.aggressivePastEntries = stints_.size();
    stats.requiredLapEntries = raceHistory_.size() * 2;
    stats.displayLapEntries = raceHistory_.size();
    stats.displayHistoryBytes = raceHistory_.retainedBytes();

    stats.containerCapacityBytes += mapCapacity(lapTimes_) +
        mapCapacity(rivalExperience_) + setCapacity(retiredCars_) +
        setCapacity(usedDryVisualCompounds_) +
        wearHistory_.capacity() * sizeof(WearSample) +
        frozenNeutralCars_.capacity() * sizeof(NeutralCarState) +
        decisionHistory_.capacity() * sizeof(StrategyDecisionRecord) +
        stints_.capacity() * sizeof(StintProgress);
    for (const auto& [_, experience] : rivalExperience_) {
        stats.rivalRecentLapEntries += experience.recent_laps.size();
        stats.containerCapacityBytes +=
            experience.recent_laps.capacity() * sizeof(RivalLapSample);
    }
    for (const auto& record : decisionHistory_)
        stats.stringCapacityBytes += stringCapacity(record.event) +
            stringCapacity(record.recommendation) + stringCapacity(record.reason) +
            stringCapacity(record.target_name);
    stats.stringCapacityBytes += stringCapacity(neutralisationRecommendation_);
    for (const auto& [_, byTeam] : teamColorOverrides_) {
        stats.containerCapacityBytes += mapCapacity(byTeam);
        for (const auto& [teamId, color] : byTeam) {
            (void)teamId;
            stats.stringCapacityBytes += stringCapacity(color);
        }
    }
    stats.containerCapacityBytes += mapCapacity(teamColorOverrides_);

    stats.retainedBytes = stats.cachedInputCapacityBytes +
        stats.containerCapacityBytes + stats.stringCapacityBytes + stats.displayHistoryBytes;
    return stats;
}

} // namespace tnrp
