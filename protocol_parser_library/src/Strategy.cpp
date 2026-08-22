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
    return it == values.end() ? PitLoss{} : it->second;
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
                   std::optional<int> requiredDifferentVisual = std::nullopt) {
    if (in.stints.empty()) return in;
    size_t longest = in.stints.size() > 1 ? 1 : 0;
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

const TimingCar* findCar(const TimingRow& t, int idx) {
    for (const auto& c:t.cars) if(c.idx==idx) return &c; return nullptr;
}
const TimingCar* atPosition(const TimingRow& t, int pos) {
    for(const auto& c:t.cars) if(c.result_status==2&&c.position==pos) return &c; return nullptr;
}

bool validRaceLapMs(int lapMs) {
    return lapMs > 0 && lapMs < 600000;
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
void StrategyProcessor::reset(){
    lap_.reset();session_.reset();status_.reset();damage_.reset();timing_.reset();participants_.reset();tyreSets_.reset();allStatus_.reset();
    lapTimes_.clear();currentStintStart_=0;rivalAhead_=rivalBehind_=-1;
    rivalExperience_.clear();retiredCars_.clear();usedDryVisualCompounds_.clear();
    wearHistory_.clear();lastWearLap_=-1;
    neutralisationStartLap_=0;neutralisationStartTime_=0;
    frozenNeutralCars_.clear();neutralisationRecommendation_.clear();
    decisionHistory_.clear();
    haveAheadGap_=haveBehindGap_=false;aheadTrend_=behindTrend_=-1;
    conservativePast_.clear();aggressivePast_.clear();conservativeRequired_.clear();aggressiveRequired_.clear();
}
void StrategyProcessor::ingest(const SessionRow& r){
    if(session_ && (session_->track_id!=r.track_id || session_->session_type!=r.session_type) && lap_ && lap_->lap_num>1) reset();
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
}
void StrategyProcessor::ingest(const StatusRow&r){
    if(status_&&lap_&&(r.tyre_compound!=status_->tyre_compound||r.visual_compound!=status_->visual_compound||r.tyre_age_laps+1<status_->tyre_age_laps)){
        currentStintStart_=std::max(1,lap_->lap_num);
        wearHistory_.clear();lastWearLap_=-1;
    }
    status_=r;
    if(dryVisual(r.visual_compound))usedDryVisualCompounds_.insert(r.visual_compound);
    if(currentStintStart_<=0&&lap_)currentStintStart_=std::max(1,lap_->lap_num-r.tyre_age_laps);
}
void StrategyProcessor::ingest(const DamageRow&r){damage_=r;}
void StrategyProcessor::ingest(const ParticipantsRow&r){participants_=r;}
void StrategyProcessor::ingest(const TyreSetsRow&r){tyreSets_=r;}
void StrategyProcessor::ingest(const AllStatusRow&r){
    allStatus_=r;
    for(const auto& car:r.cars){
        auto& experience=rivalExperience_[car.idx];
        if(experience.tyre_age_laps>=0&&
           (car.tyre_compound!=experience.tyre_compound||
            car.visual_compound!=experience.visual_compound||
            car.tyre_age_laps+1<experience.tyre_age_laps))
            experience.recent_laps.clear();
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
            const double closing=prev.relative_gap_ms-last.relative_gap_ms;
            score+=std::clamp(closing/80.0,0.0,15.0);
        }
        if(it->second.last_pit_lap>=0&&lap_&&it->second.last_pit_lap>=lap_->lap_num-1)score+=12.0;
    }
    if(car->pit_status!=0)score+=8.0;
    return std::clamp(score,0.0,100.0);
}

void StrategyProcessor::ingest(const TimingRow&r){
    timing_=r;
    const bool neutralised=session_&&(session_->safety_car_status==1||session_->safety_car_status==2);
    const TimingCar* player=findCar(r,r.player_idx);
    for(const auto& car:r.cars){
        auto& experience=rivalExperience_[car.idx];
        const bool justStopped=experience.observed_lap>=0&&car.num_pit_stops>experience.num_pit_stops;
        const bool enteredPits=experience.observed_lap>=0&&experience.pit_status==0&&car.pit_status!=0;
        if(justStopped){experience.recent_laps.clear();}
        if(justStopped||enteredPits)experience.last_pit_lap=car.lap_num;
        experience.num_pit_stops=car.num_pit_stops;
        experience.pit_status=car.pit_status;
        if(car.lap_num>experience.observed_lap){
            // Lap 1 includes the standing start and is not representative race pace.
            const int completedLap=car.lap_num-1;
            const bool settledTyres=experience.tyre_age_laps<0||experience.tyre_age_laps>1;
            if(completedLap>=2&&!neutralised&&!justStopped&&settledTyres&&
               car.result_status==2&&car.pit_status==0&&
               !car.lap_invalid&&validRaceLapMs(car.last_lap_ms)){
                const double relative=player?std::abs((double)car.gap_ms-player->gap_ms):0.0;
                experience.recent_laps.push_back({completedLap,car.last_lap_ms,relative,
                    car.position,experience.tyre_age_laps});
                if(experience.recent_laps.size()>5)experience.recent_laps.erase(experience.recent_laps.begin());
                if(car.idx==r.player_idx&&car.driver_status!=2&&car.driver_status!=3)
                    lapTimes_[completedLap]=car.last_lap_ms;
            }
            experience.observed_lap=car.lap_num;
        }
    }
    refreshRivals();
    const TimingCar* p=player;
    if(!p){haveAheadGap_=haveBehindGap_=false;return;}
    const TimingCar* a=atPosition(r,p->position-1); const TimingCar* b=atPosition(r,p->position+1);
    if(a){double g=p->gap_ms-a->gap_ms;if(haveAheadGap_&&std::abs(g-previousAheadGap_)>30)aheadTrend_=g<previousAheadGap_?1:0;previousAheadGap_=g;haveAheadGap_=true;}else{haveAheadGap_=false;aheadTrend_=-1;}
    if(b){double g=b->gap_ms-p->gap_ms;if(haveBehindGap_&&std::abs(g-previousBehindGap_)>30)behindTrend_=g<previousBehindGap_?1:0;previousBehindGap_=g;haveBehindGap_=true;}else{haveBehindGap_=false;behindTrend_=-1;}
}
void StrategyProcessor::ingest(const LapRow&r){
    const bool neutralised=session_&&(session_->safety_car_status==1||session_->safety_car_status==2);
    const TimingCar* player=timing_?findCar(*timing_,timing_->player_idx):nullptr;
    const bool clean=!timing_||(player&&player->pit_status==0&&!player->lap_invalid&&
        player->driver_status!=2&&player->driver_status!=3);
    const bool settled=!status_||status_->tyre_age_laps>1;
    if(r.lap_num>1&&validRaceLapMs(r.last_lap_ms)&&!neutralised&&clean&&settled)
        lapTimes_[r.lap_num-1]=r.last_lap_ms;
    if(r.lap_num>1&&r.lap_num-1>lastWearLap_&&damage_&&status_&&!neutralised){
        wearHistory_.push_back({r.lap_num-1,status_->tyre_compound,damage_->tyre_wear_fl,
            damage_->tyre_wear_fr,damage_->tyre_wear_rl,damage_->tyre_wear_rr});
        if(wearHistory_.size()>6)wearHistory_.erase(wearHistory_.begin());
        lastWearLap_=r.lap_num-1;
    }
    lap_=r;if(status_&&currentStintStart_<=0)currentStintStart_=std::max(1,r.lap_num-status_->tyre_age_laps);
}

void StrategyProcessor::ingestJson(std::string_view json){
    auto parsed=parseRow(json); if(!parsed)return;
    std::visit([this](const auto&r){
        using T=std::decay_t<decltype(r)>;
        if constexpr(std::is_same_v<T,LapRow>||std::is_same_v<T,SessionRow>||std::is_same_v<T,StatusRow>||
                     std::is_same_v<T,DamageRow>||std::is_same_v<T,TimingRow>||std::is_same_v<T,ParticipantsRow>||
                     std::is_same_v<T,TyreSetsRow>||std::is_same_v<T,AllStatusRow>||std::is_same_v<T,RaceEventRow>) ingest(r);
    },*parsed);
}

StrategySnapshotRow StrategyProcessor::snapshot(){
    StrategySnapshotRow out;
    out.session_time=std::max({lap_?lap_->session_time:0.0f,status_?status_->session_time:0.0f,
        damage_?damage_->session_time:0.0f,timing_?timing_->session_time:0.0f,
        tyreSets_?tyreSets_->session_time:0.0f,allStatus_?allStatus_->session_time:0.0f});
    if(lap_)out.lap_num=lap_->lap_num;if(session_)out.total_laps=session_->total_laps;
    const bool race=session_&&(session_->session_type==15||session_->session_type==16||session_->session_type==17);
    if(!race){out.state="non_race";return out;}
    const bool neutralised=session_&&(session_->safety_car_status==1||session_->safety_car_status==2);
    // Normally lap 1 is discarded as a standing-start outlier. During an SC/VSC
    // the position decision is useful immediately, so allow the model to become
    // ready as soon as the core tyre and timing rows exist.
    if(!lap_||!session_||!status_||!damage_||status_->tyre_compound<=0||session_->total_laps<=0||
       (lap_->lap_num<3&&!neutralised)){out.state="waiting";return out;}
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
    const bool wet=session_->weather>=3||wetActual(status_->tyre_compound);
    std::vector<TyreSet> base;
    if(tyreSets_)for(const auto&s:tyreSets_->sets)
        if(s.available&&!s.fitted&&tyreLife(s)>0&&
           (wet?wetActual(s.actual_compound):!wetActual(s.actual_compound)))base.push_back(s);
    auto cons=base,agg=base;
    std::sort(cons.begin(),cons.end(),[](auto&a,auto&b){return tyreLife(a)>tyreLife(b);});
    std::sort(agg.begin(),agg.end(),[](auto&a,auto&b){return a.lap_delta_ms<b.lap_delta_ms;});
    const TyreSet* fitted=nullptr;
    if(tyreSets_)for(const auto&s:tyreSets_->sets)if(s.fitted){fitted=&s;break;}
    auto futureCons=cons,futureAgg=agg;
    if(fitted&&fitted->available&&tyreLife(*fitted)>2&&
       (wet?wetActual(fitted->actual_compound):!wetActual(fitted->actual_compound))){
        futureCons.push_back(*fitted);futureAgg.push_back(*fitted);
    }

    const double fittedDelta=fitted?fitted->lap_delta_ms:0.0;
    const double conservativeFresh=!cons.empty()?std::max(0.0,fittedDelta-cons.front().lap_delta_ms):0.0;
    const double fresh=!agg.empty()?std::max(0.0,fittedDelta-agg.front().lap_delta_ms):0.0;
    const PitLoss pit=pitLossForTrack(session_->track_id);
    const double effectivePitLoss=neutralised?std::max(0.0,pit.total_ms-10000.0):pit.total_ms;
    const TimingCar* player=timing_?findCar(*timing_,timing_->player_idx):nullptr;
    auto experienceFor=[&](int idx)->const RivalExperience*{
        auto it=rivalExperience_.find(idx);
        return it==rivalExperience_.end()?nullptr:&it->second;
    };
    const auto*playerExperience=player?experienceFor(player->idx):nullptr;
    const bool playerRecentlyStopped=playerExperience&&
        playerExperience->last_pit_lap>=out.lap_num-1;
    const bool pitCallActionable=player&&player->pit_status==0&&
        status_->tyre_age_laps>1&&!playerRecentlyStopped;
    auto rivalStartedUnmatchedStop=[&](int idx){
        if(!pitCallActionable||idx<0)return false;
        const auto*rival=findCar(*timing_,idx);
        const auto*experience=experienceFor(idx);
        if(!rival||!experience||experience->last_pit_lap<out.lap_num-1)return false;
        return rival->pit_status!=0||rival->num_pit_stops>player->num_pit_stops;
    };
    const double playerLap=playerPaceMs();
    const double aheadPace=rivalPaceMs(rivalAhead_);
    const double behindPace=rivalPaceMs(rivalBehind_);
    const double defensivePace=behindPace>0?behindPace:aheadPace;
    double attackingPace=0.0;
    if(aheadPace>0&&behindPace>0)attackingPace=std::min(aheadPace,behindPace);
    else attackingPace=std::max(aheadPace,behindPace);

    // Forecast offsets are minutes from the packet. Translate the first dry/wet
    // crossover into a lap using the filtered race pace; forecast accuracy and
    // horizon deliberately reduce confidence.
    if(!session_->weather_forecast_samples.empty()){
        StrategyWeatherDecision weather;
        weather.recommendation=wet?"stay_wet":"stay_dry";
        weather.target_compound=wet?"wet_or_intermediate":"slick";
        weather.reason="no_crossover_forecast";
        for(const auto&sample:session_->weather_forecast_samples){
            if(sample.time_offset<=0||sample.time_offset>60)continue;
            const bool sampleWet=sample.weather>=3;
            if(sampleWet==wet)continue;
            weather.forecast_weather=sample.weather;weather.rain_percentage=sample.rain_percentage;
            weather.minutes_until_change=sample.time_offset;
            const double lapDuration=playerLap>0?playerLap:90000.0;
            weather.crossover_lap=std::min(out.total_laps,out.lap_num+(int)std::ceil(sample.time_offset*60000.0/lapDuration));
            if(sampleWet){
                const bool heavy=sample.weather>=4||sample.rain_percentage>=70;
                weather.recommendation=heavy?"prepare_wets":"prepare_intermediates";
                weather.target_compound=heavy?"wet":"intermediate";
                weather.reason="rain_crossover_forecast";
            }else{
                weather.recommendation="prepare_slicks";weather.target_compound="slick";
                weather.reason="dry_crossover_forecast";
            }
            break;
        }
        weather.confidence=(session_->forecast_accuracy==0 ? .86 : .62)*
            std::max(.45,1.0-weather.minutes_until_change/90.0);
        out.weather_strategy=weather;
    }

    if(neutralised&&player&&timing_){
        std::vector<NeutralCarState> basis=frozenNeutralCars_;
        if(basis.empty())for(const auto&car:timing_->cars)basis.push_back({car.idx,car.position,car.lap_num,
            (double)car.gap_ms,car.pit_status,car.num_pit_stops,car.result_status});
        const auto playerBasis=std::find_if(basis.begin(),basis.end(),[&](const auto&car){return car.idx==player->idx;});
        const double playerGap=playerBasis!=basis.end()?playerBasis->gap_ms:player->gap_ms;
        const int playerLapNum=playerBasis!=basis.end()?playerBasis->lap_num:player->lap_num;
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
                if(car.lap_num>playerLapNum){++position;continue;}
                if(car.lap_num<playerLapNum)continue;
                double rivalCost=0;
                const auto live=findCar(*timing_,car.idx);
                auto exp=rivalExperience_.find(car.idx);
                const bool boxing=(live&&live->pit_status!=0)||
                    (exp!=rivalExperience_.end()&&exp->second.last_pit_lap>=neutralisationStartLap_);
                if(boxing)rivalCost=effectivePitLoss*(session_->safety_car_status==1?.35:.60);
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
        const bool stopDue=out.laps_until_cliff<=2;
        const bool replacementReachesFinish=!agg.empty()&&tyreLife(agg.front())>=remainingLaps;
        const bool resolvesRequiredStop=remainingLaps>out.laps_until_cliff&&replacementReachesFinish;
        const bool dryCompoundRequired=!wet&&usedDryVisualCompounds_.size()<2&&
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
        if(neutralisationRecommendation_.empty())neutralisationRecommendation_=proposed;
        else if(proposed!=neutralisationRecommendation_){
            const bool strongSwitch=(proposed=="box"&&advantage>1500.0)||
                (proposed=="box"&&(stopDue||resolvesRequiredStop||dryCompoundRequired||freeStop))||
                (proposed=="stay_out"&&advantage<-1500.0)||!canBox;
            if(strongSwitch)neutralisationRecommendation_=proposed;
            else reason="decision_held_by_hysteresis";
        }
        std::optional<int> lapsToRecover;
        if(fresh>0)lapsToRecover=(int)std::ceil(effectivePitLoss/fresh);
        StrategyNeutralisation decision;
        decision.kind=session_->safety_car_status==1?"safety_car":"virtual_safety_car";
        decision.recommendation=neutralisationRecommendation_;decision.reason=reason;
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
    RawPlan cp=buildPlan(format_,out.lap_num,out.total_laps,first,status_->tyre_compound,status_->visual_compound,cons);
    if(out.is_monaco&&cp.stops<2)cp=forceExtra(format_,cp,futureCons);
    const int aggLeft=std::max(0,(int)std::floor((cliff-out.limiting_wear)/(out.limiting_wear_per_lap*1.2)));
    const int aggCliff=out.lap_num+aggLeft;
    const int econ=!agg.empty()?std::max(out.lap_num+1,out.total_laps-tyreLife(agg.front())):aggCliff;
    int aggressiveFirst=std::min(aggCliff,econ);
    if(rivalStartedUnmatchedStop(rivalAhead_)&&out.laps_until_cliff>2)
        aggressiveFirst=std::max(aggressiveFirst,out.lap_num+2);
    if(out.neutralisation&&out.neutralisation->recommendation=="box")aggressiveFirst=out.lap_num+1;
    RawPlan ap=buildPlan(format_,out.lap_num,out.total_laps,aggressiveFirst,status_->tyre_compound,status_->visual_compound,agg);
    if(ap.stops<=cp.stops)ap=forceExtra(format_,ap,futureAgg);
    if(out.is_monaco&&ap.stops<2)ap=forceExtra(format_,ap,futureAgg);
    if(!wet){enforceDryCompoundRule(format_,cp,futureCons,usedDryVisualCompounds_);enforceDryCompoundRule(format_,ap,futureAgg,usedDryVisualCompounds_);}

    auto display=[&](const RawPlan& raw,bool aggressive,std::vector<PastStintState>& past,
                     std::map<int,double>& requiredByLap){
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
        if(!validRaceLapMs((int)playerLap))return plan;
        const int opening=currentStintStart_>0?currentStintStart_:std::max(1,out.lap_num-status_->tyre_age_laps);
        const double offset=aggressive?effectivePitLoss/std::max(1,out.total_laps-out.lap_num):0;
        const double rivalTarget=aggressive?attackingPace:defensivePace;
        const auto& availableSets=aggressive?futureAgg:futureCons;
        auto targetFor=[&](const RawStint&s,size_t i){
            if(i==0)return std::max(60000.0,(rivalTarget>0?rivalTarget:playerLap)-offset);
            for(const auto&set:availableSets)if((s.setIdx>=0&&set.idx==s.setIdx)||
                                      (s.setIdx<0&&set.actual_compound==s.actual)){
                double estimate=playerLap+set.lap_delta_ms-fittedDelta-offset;
                if(rivalTarget>0)estimate=std::min(estimate,rivalTarget-offset);
                return std::max(60000.0,estimate);
            }
            return 0.0;
        };
        const double firstTarget=raw.stints.empty()?0:targetFor(raw.stints[0],0);
        if(firstTarget>0&&(past.empty()||opening>past.back().start_lap))
            past.push_back({opening,firstTarget,raw.stints[0].name,raw.stints[0].actual,
                            raw.stints[0].visual,!past.empty(),0});
        if(!past.empty()&&!raw.stints.empty()){
            int end=raw.stints[0].last?out.total_laps:raw.stints[0].pitLap;
            past.back().expected_laps=std::max(1,end-opening+1);
        }
        double raceCum=0;
        auto rows=[&](int start,int end,double req,bool post,bool pre){
            std::vector<StrategyLapTarget> values;
            double cum=0;
            for(int n=start;n>0&&n<=end;++n){
                double lapBase=req;
                if(n<out.lap_num){
                    lapBase=requiredByLap.emplace(n,req).first->second;
                }else if(n==out.lap_num){
                    requiredByLap[n]=req;
                    lapBase=req;
                }
                StrategyLapTarget row;
                row.lap_num=n;
                row.required_ms=lapBase+(post&&n==start?pit.outlap_ms:0)+(pre&&n==end?pit.inlap_ms:0);
                auto actual=lapTimes_.find(n);
                if(n<out.lap_num&&actual!=lapTimes_.end()&&actual->second>0){
                    row.has_actual=true;
                    row.actual_ms=actual->second;
                    row.delta_lap_ms=row.actual_ms-row.required_ms;
                    cum+=row.delta_lap_ms;
                    row.delta_stint_ms=cum;
                    raceCum+=row.delta_lap_ms;
                    row.delta_total_ms=raceCum;
                }
                values.push_back(row);
            }
            return values;
        };
        for(size_t k=0;k+1<past.size();++k){
            const auto&ps=past[k];
            int end=past[k+1].start_lap-1;
            if(end<ps.start_lap)continue;
            StrategyStint stint;
            stint.compound_name=ps.compound_name;stint.actual_compound=ps.actual_compound;
            stint.visual_compound=ps.visual_compound;stint.stint_number=(int)plan.stints.size()+1;
            stint.start_lap=ps.start_lap;stint.end_lap=end;
            stint.expected_laps=ps.expected_laps>0?ps.expected_laps:end-ps.start_lap+1;
            stint.actual_laps=end-ps.start_lap+1;
            stint.rows=rows(stint.start_lap,stint.end_lap,ps.required_base_ms,ps.post_pit,true);
            plan.stints.push_back(std::move(stint));
        }
        const bool pitted=past.size()>1;
        for(size_t i=0;i<raw.stints.size();++i){
            const auto&rs=raw.stints[i];
            StrategyStint stint;
            stint.compound_name=rs.name;stint.actual_compound=rs.actual;stint.visual_compound=rs.visual;
            stint.stint_number=(int)plan.stints.size()+1;stint.start_lap=i==0?opening:rs.startLap;
            stint.end_lap=rs.last?out.total_laps:rs.pitLap;
            stint.expected_laps=i==0?std::max(1,stint.end_lap-stint.start_lap+1):rs.lapCount;
            stint.is_last=rs.last;
            const double target=targetFor(rs,i);
            if(target>0){
                stint.rows=rows(stint.start_lap,stint.end_lap,target,i==0?pitted:true,!rs.last);
                for(const auto&row:stint.rows)if(row.has_actual)++stint.actual_laps;
            }
            plan.stints.push_back(std::move(stint));
        }
        return plan;
    };
    out.conservative=display(cp,false,conservativePast_,conservativeRequired_);
    out.aggressive=display(ap,true,aggressivePast_,aggressiveRequired_);
    out.explanation.push_back({"recent_degradation","Cliff uses recent completed-lap wear deltas when available",
        out.limiting_wear_per_lap});
    if(rivalBehind_>=0)out.explanation.push_back({"defensive_rival","Defensive plan targets the highest scored car behind",
        rivalThreatScore(rivalBehind_,false)});
    if(rivalAhead_>=0)out.explanation.push_back({"attacking_rival","Attacking plan targets the highest scored car ahead",
        rivalThreatScore(rivalAhead_,true)});
    if(!cp.legal||!ap.legal)out.explanation.push_back({"tyre_legality","Available physical tyre sets cannot form a fully legal plan",0});
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
                    rival.closing_ms_per_lap=previous.relative_gap_ms-last.relative_gap_ms;
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
    for(auto&record:decisionHistory_)if(!record.actual_position&&out.lap_num>record.lap_num&&player){
        if(record.event=="neutralisation"){
            const bool stopped=player->num_pit_stops>record.start_num_pit_stops;
            if(!stopped&&neutralised&&out.lap_num<=record.lap_num+1)continue;
            record.actual_position=player->position;
            record.followed=record.recommendation=="box"?stopped:!stopped;
            if(*record.followed)record.successful=player->position<=record.projected_position;
        }else{
            record.actual_position=player->position;
            auto actual=lapTimes_.find(record.lap_num);
            auto required=record.recommendation=="attack"?aggressiveRequired_.find(record.lap_num):conservativeRequired_.find(record.lap_num);
            const bool hasRequired=required!=(record.recommendation=="attack"?aggressiveRequired_.end():conservativeRequired_.end());
            if(actual!=lapTimes_.end()&&hasRequired){
                record.followed=actual->second<=required->second*1.01;
                record.successful=*record.followed||player->position<=record.start_position;
            }
        }
    }
    if(player){
        auto upsert=[&](StrategyDecisionRecord record){
            auto existing=std::find_if(decisionHistory_.begin(),decisionHistory_.end(),[&](const auto&value){
                return value.event==record.event&&value.lap_num==record.lap_num;});
            if(existing==decisionHistory_.end())decisionHistory_.push_back(std::move(record));
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
    out.decision_history=decisionHistory_;
    return out;
}

std::string StrategyProcessor::snapshotJson(){return writeJson(snapshot());}

} // namespace tnrp
