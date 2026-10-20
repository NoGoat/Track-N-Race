#include "tnrp/Strategy.h"
#include "tnrp/AnyRow.h"

#include <cassert>
#include <iostream>

namespace {
using namespace tnrp;

bool dryVisual(int visual) { return visual == 16 || visual == 17 || visual == 18; }

// First lap of the first dry stint still to come in a plan, 0 if none.
int firstDryStart(const StrategyPlan& plan, int lap) {
    for (const auto& stint : plan.stints)
        if (stint.start_lap > lap && dryVisual(stint.visual_compound)) return stint.start_lap;
    return 0;
}

// No stop planned for three laps or fewer: it never repays the pit loss.
void assertNoShortStints(const StrategySnapshotRow& s) {
    for (const auto* plan : {&s.conservative, &s.aggressive})
        for (const auto& stint : plan->stints)
            if (stint.start_lap > s.lap_num) assert(stint.end_lap - stint.start_lap + 1 > 3);
}

// A wet race on intermediates at lap 3 of 30, with a new inter set and three
// slick sets that are slower on today's track.
struct WetRace {
    SessionRow session;
    LapRow lap;
    StatusRow status;
    DamageRow damage;
    TyreSetsRow sets;
    TimingRow timing;

    WetRace() {
        session.session_type = 15; session.track_id = 0; session.total_laps = 30; session.weather = 3;
        lap.lap_num = 3; lap.session_time = 270; lap.last_lap_ms = 90000;
        status.session_time = 270; status.tyre_compound = 7; status.visual_compound = 7; status.tyre_age_laps = 3;
        damage.session_time = 270;
        damage.tyre_wear_fl = damage.tyre_wear_fr = damage.tyre_wear_rl = damage.tyre_wear_rr = 6;
        sets.session_time = 270;
        sets.sets.push_back({0, 7, 7, 6, true, 0, 20, 18, 0, true});
        sets.sets.push_back({1, 7, 7, 0, true, 0, 25, 22, -200, false});
        sets.sets.push_back({2, 19, 16, 0, true, 0, 18, 16, 900, false});
        sets.sets.push_back({3, 20, 17, 0, true, 0, 25, 22, 1000, false});
        sets.sets.push_back({4, 21, 18, 0, true, 0, 35, 30, 1100, false});
        timing.session_time = 270; timing.player_idx = 0;
        TimingCar player; player.idx = 0; player.position = 5; player.lap_num = 3;
        player.result_status = 2; player.driver_status = 4; timing.cars.push_back(player);
    }

    void feed(StrategyProcessor& p) const {
        p.ingest(session); p.ingest(lap); p.ingest(status); p.ingest(damage);
        p.ingest(timing); p.ingest(sets);
    }
};

void weatherScheduling() {
    // A likely dry spell forecast in 10 minutes schedules the switch to slicks
    // in both plans, Attack a lap before Defend, instead of inters to the flag.
    {
        WetRace race;
        race.session.weather_forecast_samples = {{0,3,80},{5,3,60},{10,2,20},{15,2,10},{30,2,5}};
        StrategyProcessor p(2025); race.feed(p);
        const auto s = p.snapshot();
        assert(s.weather_strategy && s.weather_strategy->recommendation == "prepare_slicks");
        assert(s.weather_strategy->crossover_lap == 10);  // 10 min at the default 90 s lap
        assert(firstDryStart(s.conservative, s.lap_num) == 11);
        assert(firstDryStart(s.aggressive, s.lap_num) == 10);
        assert(s.conservative.legal && s.aggressive.legal);
        // A planned switch off wet tyres makes it a wet race: no compound rule.
        assert(!s.conservative.requires_compound_change);
        // The new tyres' targets come from today's pace, not today's +0.9 s
        // (a mid-stint lap: the first carries the out-lap, the last the in-lap).
        for (const auto& stint : s.conservative.stints)
            if (dryVisual(stint.visual_compound) && stint.rows.size() > 2)
                assert(stint.rows[1].required_ms == 0 || stint.rows[1].required_ms < 90500);
        assertNoShortStints(s);
    }
    // A forecast that thinks rain more likely than not is still on the card,
    // but does not move a stop in either plan.
    {
        WetRace race;
        race.session.weather_forecast_samples = {{0,3,80},{5,3,75},{10,2,70},{15,3,70},{30,3,70}};
        StrategyProcessor p(2025); race.feed(p);
        const auto s = p.snapshot();
        assert(s.weather_strategy && s.weather_strategy->recommendation == "prepare_slicks");
        assert(firstDryStart(s.conservative, s.lap_num) == 0);
        assert(firstDryStart(s.aggressive, s.lap_num) == 0);
    }
    // No forecast change, but the game's slick delta falls 550 ms in a lap:
    // the track is drying, and the switch is scheduled for when it crosses.
    {
        WetRace race;
        StrategyProcessor p(2025); race.feed(p);
        assert(firstDryStart(p.snapshot().conservative, 3) == 0);
        LapRow next = race.lap; next.lap_num = 4; next.session_time = 360; p.ingest(next);
        TyreSetsRow drier = race.sets; drier.session_time = 360;
        drier.sets[2].lap_delta_ms = 350; drier.sets[3].lap_delta_ms = 450; drier.sets[4].lap_delta_ms = 550;
        p.ingest(drier);
        const auto s = p.snapshot();
        assert(s.weather_strategy && s.weather_strategy->reason == "conditions_trend");
        assert(s.weather_strategy->crossover_lap == 6);
        assert(firstDryStart(s.conservative, s.lap_num) == 7);
        assertNoShortStints(s);
    }
    // Rivals who have just pitted onto slicks call the crossover for next lap;
    // rivals who merely stayed out on slicks would not.
    {
        WetRace race;
        for (int idx = 1; idx <= 6; ++idx) {
            TimingCar rival; rival.idx = idx; rival.position = idx < 5 ? idx : idx + 1; rival.lap_num = 3;
            rival.result_status = 2; rival.driver_status = 4; race.timing.cars.push_back(rival);
        }
        StrategyProcessor p(2025); race.feed(p);
        TimingRow pitted = race.timing; pitted.session_time = 300;
        AllStatusRow tyres; tyres.session_time = 300;
        for (auto& car : pitted.cars) {
            AllStatusCar status; status.idx = car.idx; status.tyre_compound = 7; status.visual_compound = 7; status.tyre_age_laps = 3;
            if (car.idx >= 1 && car.idx <= 3) {
                car.num_pit_stops = 1;
                status.tyre_compound = 19; status.visual_compound = 16; status.tyre_age_laps = 0;
            }
            tyres.cars.push_back(status);
        }
        p.ingest(pitted); p.ingest(tyres);
        const auto s = p.snapshot();
        assert(s.weather_strategy && s.weather_strategy->reason == "rivals_switched");
        assert(s.weather_strategy->crossover_lap == 4);
        assert(firstDryStart(s.aggressive, s.lap_num) == 5);
    }
    // After the required stop, fresh hards to the flag: Attack must not plan
    // a stop on the next lap just because an unused set could last the race.
    {
        StrategyProcessor p(2025);
        SessionRow session; session.session_type = 15; session.track_id = 0; session.total_laps = 30;
        LapRow lap; lap.lap_num = 16; lap.session_time = 1440; lap.last_lap_ms = 90000; lap.num_pit_stops = 1;
        StatusRow status; status.session_time = 1440; status.tyre_compound = 20; status.visual_compound = 18; status.tyre_age_laps = 1;
        DamageRow damage; damage.session_time = 1440;
        damage.tyre_wear_fl = damage.tyre_wear_fr = damage.tyre_wear_rl = damage.tyre_wear_rr = 2;
        TyreSetsRow sets; sets.session_time = 1440;
        sets.sets.push_back({0, 20, 18, 2, true, 0, 30, 28, 0, true});
        sets.sets.push_back({1, 19, 16, 0, true, 0, 18, 16, -500, false});
        TimingRow timing; timing.session_time = 1440; timing.player_idx = 0;
        TimingCar player; player.idx = 0; player.position = 5; player.lap_num = 16; player.num_pit_stops = 1;
        player.result_status = 2; player.driver_status = 4; timing.cars.push_back(player);
        p.ingest(session); p.ingest(lap); p.ingest(status); p.ingest(damage); p.ingest(timing); p.ingest(sets);
        const auto s = p.snapshot();
        for (const auto& stint : s.aggressive.stints)
            if (stint.start_lap <= s.lap_num && stint.end_lap >= s.lap_num)
                assert(stint.is_last || stint.end_lap > s.lap_num + 3);
        assertNoShortStints(s);
    }
}

// Stint sizing on slicks after an early switch off inters (the British GP
// sample at lap 7): new softs with a 23-lap life, mediums of 29, a hard of 36.
void stintSizing() {
    SessionRow session; session.session_type = 15; session.track_id = 0; session.total_laps = 52;
    LapRow lap; lap.lap_num = 7; lap.session_time = 630; lap.last_lap_ms = 90000; lap.num_pit_stops = 1;
    StatusRow inter; inter.session_time = 400; inter.tyre_compound = 7; inter.visual_compound = 7; inter.tyre_age_laps = 4;
    StatusRow soft; soft.session_time = 630; soft.tyre_compound = 19; soft.visual_compound = 16; soft.tyre_age_laps = 1;
    // One lap of light wear: extrapolated to the 80% cliff it lasts the race.
    DamageRow damage; damage.session_time = 630;
    damage.tyre_wear_fl = damage.tyre_wear_fr = damage.tyre_wear_rl = damage.tyre_wear_rr = 1;
    TyreSetsRow sets; sets.session_time = 630;
    sets.sets.push_back({5, 19, 16, 1, true, 0, 23, 23, 0, true});
    sets.sets.push_back({6, 19, 16, 0, true, 0, 23, 23, -15, false});
    sets.sets.push_back({9, 20, 17, 0, true, 0, 29, 29, 470, false});
    sets.sets.push_back({12, 21, 18, 0, true, 0, 36, 36, 880, false});
    TimingRow timing; timing.session_time = 630; timing.player_idx = 0;
    TimingCar player; player.idx = 0; player.position = 10; player.lap_num = 7; player.num_pit_stops = 1;
    player.result_status = 2; player.driver_status = 4; timing.cars.push_back(player);

    StrategyProcessor p(2025);
    p.setMinimumStops(1);
    p.ingest(session); p.ingest(lap); p.ingest(inter); p.ingest(soft); p.ingest(damage); p.ingest(timing); p.ingest(sets);
    const auto s = p.snapshot();
    // The fitted set's game life holds the current stint, not the wear cliff.
    assert(s.cliff_lap == 7 + 23 - 1);
    for (const auto* plan : {&s.conservative, &s.aggressive}) {
        for (const auto& stint : plan->stints)
            if (stint.start_lap <= 7 && stint.end_lap >= 7) assert(stint.end_lap <= s.cliff_lap);
        // Inters were used: no compound rule, and so no stop added for one.
        assert(!plan->requires_compound_change && plan->legal);
        assert(plan->stops == 1);  // stops still to come: one
    }
    // Attack stops earlier and finishes on the set that lasts: mediums for
    // the 27 laps left, not softs plus a stop for the last few laps.
    const auto& attack = s.aggressive.stints;
    assert(attack.back().visual_compound == 17 && attack.back().end_lap == 52);
    assert(attack[attack.size() - 2].end_lap < s.cliff_lap);
    assertNoShortStints(s);
}
}  // namespace

int main() {
    using namespace tnrp;
    StrategyProcessor p(2025);
    SessionRow session; session.session_type=15; session.track_id=0; session.total_laps=20;
    LapRow lap; lap.lap_num=3; lap.session_time=180; lap.last_lap_ms=90000;
    StatusRow status; status.session_time=180; status.tyre_compound=18; status.visual_compound=16; status.tyre_age_laps=3;
    DamageRow damage; damage.session_time=180; damage.tyre_wear_fl=18; damage.tyre_wear_fr=12; damage.tyre_wear_rl=10; damage.tyre_wear_rr=11;
    TyreSetsRow sets; sets.session_time=180;
    sets.sets.push_back({0,18,16,18,true,0,0,12,0,true});
    sets.sets.push_back({1,19,17,0,true,0,20,18,-700,false});
    TimingRow timing; timing.session_time=180; timing.player_idx=0;
    TimingCar player; player.idx=0; player.position=2; player.lap_num=3;
    player.last_lap_ms=90000; player.gap_ms=1000; player.num_pit_stops=1;
    player.result_status=2; player.driver_status=4; timing.cars.push_back(player);
    TimingCar ahead; ahead.idx=1; ahead.position=1; ahead.lap_num=3;
    ahead.last_lap_ms=89500; ahead.num_pit_stops=2;
    ahead.result_status=2; ahead.driver_status=4; timing.cars.push_back(ahead);
    TimingCar behind; behind.idx=2; behind.position=3; behind.lap_num=3;
    behind.last_lap_ms=90500; behind.gap_ms=2000; behind.num_pit_stops=3;
    behind.result_status=2; behind.driver_status=4; timing.cars.push_back(behind);
    ParticipantsRow participants; participants.drivers.push_back({0,"Player Driver",0,1,false,"#ff0000"}); participants.drivers.push_back({1,"Ahead Driver",0,2,false,"#00ff00"}); participants.drivers.push_back({2,"Behind Driver",0,3,false,"#0000ff"});
    AllStatusRow all; all.session_time=180; AllStatusCar rival; rival.idx=1; rival.tyre_age_laps=7; all.cars.push_back(rival);
    p.ingest(session);p.ingest(lap);p.ingest(status);p.ingest(damage);p.ingest(sets);p.ingest(timing);p.ingest(participants);p.ingest(all);
    auto s=p.snapshot();
    assert(s.state=="ready"); assert(s.limiting_corner=="FL");
    assert(!s.conservative.stints.empty()); assert(s.positions.size()==3);
    assert(s.positions[1].num_pit_stops==1);
    auto typed=parseRow(p.snapshotJson());
    assert(typed && std::holds_alternative<StrategySnapshotRow>(*typed));
    LapRow nextLap=lap; nextLap.lap_num=4; nextLap.last_lap_ms=89500; p.ingest(nextLap);
    auto unsettled=p.snapshot();
    assert(!unsettled.conservative.stints.empty() && unsettled.conservative.stints.front().start_lap==1);
    assert(pitLossForTrack(0).total_ms==21500); assert(pitLossForTrack(-1).total_ms==25000);

    StrategyProcessor configured(2025);
    DamageRow lowWear=damage;
    lowWear.tyre_wear_fl=lowWear.tyre_wear_fr=lowWear.tyre_wear_rl=lowWear.tyre_wear_rr=1;
    TyreSetsRow extraSets=sets;
    extraSets.sets.push_back({2,19,17,0,true,0,20,18,-600,false});
    extraSets.sets.push_back({3,20,18,0,true,0,20,18,-500,false});
    extraSets.sets.push_back({4,18,16,0,true,0,20,18,-400,false});
    configured.setMinimumStops(2);
    configured.ingest(session); configured.ingest(lap); configured.ingest(status);
    configured.ingest(lowWear); configured.ingest(extraSets); configured.ingest(timing);
    const auto configuredPlan=configured.snapshot();
    // The player has already completed one stop, so only the balance remains.
    assert(configuredPlan.conservative.stops==1);
    // The required stops bind Attack too; its own extra stop needs fresh tyres
    // to repay it, which ~0.7 s over three-lap-old tyres on a 17-lap stint does not.
    assert(configuredPlan.aggressive.stops==1);

    StrategyProcessor unrestricted(2025);
    unrestricted.setMinimumStops(0);
    unrestricted.ingest(session); unrestricted.ingest(lap); unrestricted.ingest(status);
    unrestricted.ingest(lowWear); unrestricted.ingest(extraSets); unrestricted.ingest(timing);
    const auto unrestrictedPlan=unrestricted.snapshot();
    assert(unrestrictedPlan.conservative.legal);
    assert(!unrestrictedPlan.conservative.requires_compound_change);

    StrategyProcessor incomplete(2025); incomplete.ingest(session);
    assert(incomplete.snapshot().state=="waiting");

    StrategyProcessor monaco(2025);
    SessionRow wetSession=session; wetSession.track_id=5; wetSession.weather=3; wetSession.total_laps=40;
    LapRow wetLap=lap; wetLap.lap_num=3;
    StatusRow wetStatus=status; wetStatus.tyre_compound=7; wetStatus.visual_compound=7;
    DamageRow uneven=damage; uneven.tyre_wear_fl=60; uneven.tyre_wear_fr=10; uneven.tyre_wear_rl=10; uneven.tyre_wear_rr=10;
    TyreSetsRow wetSets; wetSets.sets.push_back({0,7,7,10,true,0,25,22,0,true});
    wetSets.sets.push_back({1,8,8,0,true,0,30,28,-300,false});
    // Inters and full wets are separate tyre types, so the stop needs an inter.
    wetSets.sets.push_back({3,7,7,0,true,0,40,36,-100,false});
    wetSets.sets.push_back({2,19,17,0,true,0,25,24,800,false}); // dry and slower: must be filtered
    monaco.ingest(wetSession); monaco.ingest(wetLap); monaco.ingest(wetStatus);
    monaco.ingest(uneven); monaco.ingest(wetSets); monaco.ingest(timing);
    auto m=monaco.snapshot();
    assert(m.state=="ready" && m.is_monaco);
    assert(m.conservative.stops==1); // Monaco does not impose an extra stop.
    for(size_t i=1;i<m.conservative.stints.size();++i)
        assert(m.conservative.stints[i].actual_compound==7 || m.conservative.stints[i].actual_compound==8);
    assert(!m.wear_warnings.empty() && m.wear_warnings.front().priority==0);

    // A dry set 0.8 s a lap faster on today's track is called now, and the
    // plans run slicks from the next lap.
    StrategyProcessor drying(2025);
    TyreSetsRow dryingSets=wetSets;
    for(auto&set:dryingSets.sets)if(set.visual_compound==17)set.lap_delta_ms=-800;
    drying.ingest(wetSession); drying.ingest(wetLap); drying.ingest(wetStatus);
    drying.ingest(uneven); drying.ingest(dryingSets); drying.ingest(timing);
    const auto d=drying.snapshot();
    assert(d.weather_strategy && d.weather_strategy->reason=="faster_tyre_available");
    assert(d.conservative.stints.size()>=2 && d.conservative.stints[1].visual_compound==17);

    weatherScheduling();
    stintSizing();

    RaceEventRow restart; restart.code="SSTA"; monaco.ingest(restart);
    assert(monaco.snapshot().state=="non_race");
    std::cout << "strategy processor ok\n";
}
