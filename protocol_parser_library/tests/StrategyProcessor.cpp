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
    // life_span is the laps left: one lap into a 23-lap life.
    sets.sets.push_back({5, 19, 16, 1, true, 0, 22, 23, 0, true});
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
// Lap rows one lap apart, so each completed lap is a clean pace sample.
void driveLaps(StrategyProcessor& p, LapRow& lap, StatusRow& status, int laps, int lapMs) {
    for (int i = 0; i < laps; ++i) {
        ++lap.lap_num; lap.session_time += lapMs / 1000.0f; lap.last_lap_ms = lapMs;
        ++status.tyre_age_laps; status.session_time = lap.session_time;
        p.ingest(status); p.ingest(lap);
    }
}

double currentTarget(const StrategySnapshotRow& s) {
    for (const auto& stint : s.conservative.stints)
        for (const auto& row : stint.rows)
            if (row.lap_num == s.lap_num) return row.required_ms;
    return 0;
}

int nextStop(const StrategyPlan& plan, int lap) {
    for (const auto& stint : plan.stints)
        if (stint.end_lap >= lap) return stint.is_last ? 0 : stint.end_lap;
    return 0;
}

// Lap 20 of 50 at Melbourne, the player P5 in a ten-car train three seconds
// apart, so a stop under the Safety Car costs places.
struct SafetyCarRace {
    SessionRow session; LapRow lap; StatusRow status; DamageRow damage; TyreSetsRow sets; TimingRow timing;
    SafetyCarRace(int stops, int tyreAge, int wear, int lifeLeft) {
        session.session_type = 15; session.track_id = 0; session.total_laps = 50;
        lap.lap_num = 20; lap.session_time = 1800; lap.last_lap_ms = 90000; lap.num_pit_stops = stops; lap.player_idx = 0;
        status.session_time = 1800; status.tyre_compound = 20; status.visual_compound = 18;
        status.tyre_age_laps = tyreAge; status.player_idx = 0;
        damage.session_time = 1800;
        damage.tyre_wear_fl = damage.tyre_wear_fr = damage.tyre_wear_rl = damage.tyre_wear_rr = wear;
        sets.session_time = 1800;
        sets.sets.push_back({0, 20, 18, wear, true, 0, lifeLeft, 40, 0, true});
        sets.sets.push_back({1, 19, 17, 0, true, 0, 30, 30, -300, false});
        timing.session_time = 1800; timing.player_idx = 0;
        for (int i = 0; i < 10; ++i) {
            TimingCar car; car.idx = i; car.position = i == 0 ? 5 : (i < 5 ? i : i + 1);
            car.lap_num = 20; car.num_pit_stops = stops; car.result_status = 2; car.driver_status = 4;
            car.gap_ms = (car.position - 1) * 3000; car.last_lap_ms = 90000; timing.cars.push_back(car);
        }
    }
    void feed(StrategyProcessor& p) const {
        p.ingest(session); p.ingest(lap); p.ingest(status); p.ingest(damage); p.ingest(timing); p.ingest(sets);
    }
};

void safetyCar() {
    // The required stop is done and two-lap-old hards reach the flag: a stop
    // would only lose places, so the call is to stay out and no stop moves.
    {
        SafetyCarRace race(1, 2, 4, 38);
        StrategyProcessor p(2025); p.setMinimumStops(1);
        StatusRow medium = race.status; medium.tyre_compound = 19; medium.visual_compound = 17; medium.tyre_age_laps = 10;
        // The stop for hards was two laps before the Safety Car.
        LapRow earlier = race.lap; earlier.lap_num = 18; earlier.session_time = 1620;
        p.ingest(race.session); p.ingest(earlier); p.ingest(medium);
        StatusRow hard = race.status; hard.tyre_age_laps = 0; p.ingest(hard);
        race.feed(p);
        SessionRow sc = race.session; sc.safety_car_status = 1; p.ingest(sc);
        const auto s = p.snapshot();
        assert(s.neutralisation && s.neutralisation->recommendation == "stay_out");
        assert(s.neutralisation->reason == "protect_track_position");
        assert(nextStop(s.conservative, s.lap_num) != s.lap_num + 1);
        assert(nextStop(s.aggressive, s.lap_num) != s.lap_num + 1);
    }
    // A stop is still owed: taking it under the Safety Car is the cheap one.
    // Once it is made the call clears and no further stop is planned for it.
    {
        SafetyCarRace race(0, 10, 20, 28);
        StrategyProcessor p(2025); p.setMinimumStops(1); race.feed(p);
        SessionRow sc = race.session; sc.safety_car_status = 1; p.ingest(sc);
        auto s = p.snapshot();
        assert(s.neutralisation && s.neutralisation->recommendation == "box");
        assert(nextStop(s.conservative, s.lap_num) == s.lap_num + 1);

        TimingRow pitted = race.timing; pitted.session_time = 1830; pitted.cars[0].num_pit_stops = 1;
        StatusRow fresh = race.status; fresh.session_time = 1830;
        fresh.tyre_compound = 19; fresh.visual_compound = 17; fresh.tyre_age_laps = 0;
        DamageRow newTyres = race.damage; newTyres.session_time = 1830;
        newTyres.tyre_wear_fl = newTyres.tyre_wear_fr = newTyres.tyre_wear_rl = newTyres.tyre_wear_rr = 0;
        TyreSetsRow after; after.session_time = 1830;
        after.sets.push_back({0, 20, 18, 20, true, 0, 28, 40, 900, false});
        after.sets.push_back({1, 19, 17, 0, true, 0, 30, 30, 0, true});
        p.ingest(pitted); p.ingest(fresh); p.ingest(newTyres); p.ingest(after); p.ingest(sc);
        s = p.snapshot();
        assert(s.neutralisation && s.neutralisation->recommendation == "stay_out");
        assert(s.neutralisation->reason == "stop_completed");
        assert(nextStop(s.conservative, s.lap_num) != s.lap_num + 1);
        assert(nextStop(s.aggressive, s.lap_num) != s.lap_num + 1);
    }
}

// The player P2 and one car behind, at Melbourne. `gapBehindMs` is the gap
// to that car when it pits.
struct CoverRace {
    SessionRow session; LapRow lap; StatusRow status; DamageRow damage; TyreSetsRow sets; TimingRow timing;
    CoverRace(int lapNum, int tyreAge, int gapBehindMs) {
        session.session_type = 15; session.track_id = 0; session.total_laps = 50;
        lap.lap_num = lapNum; lap.session_time = lapNum * 90.0f; lap.last_lap_ms = 90000; lap.player_idx = 0;
        status.session_time = lap.session_time; status.tyre_compound = 20; status.visual_compound = 18;
        status.tyre_age_laps = tyreAge; status.player_idx = 0;
        damage.session_time = lap.session_time;
        damage.tyre_wear_fl = damage.tyre_wear_fr = damage.tyre_wear_rl = damage.tyre_wear_rr = tyreAge * 1.5;
        sets.session_time = lap.session_time;
        sets.sets.push_back({0, 20, 18, 0, true, 0, 40 - tyreAge, 40, 0, true});
        sets.sets.push_back({1, 19, 17, 0, true, 0, 30, 30, -300, false});
        timing.session_time = lap.session_time; timing.player_idx = 0;
        const int gaps[] = {5000, 0, 5000 + gapBehindMs};
        const int positions[] = {2, 1, 3};
        for (int i = 0; i < 3; ++i) {
            TimingCar car; car.idx = i; car.position = positions[i]; car.lap_num = lapNum;
            car.result_status = 2; car.driver_status = 4; car.gap_ms = gaps[i]; car.last_lap_ms = 90000;
            timing.cars.push_back(car);
        }
    }
    void feed(StrategyProcessor& p) const {
        p.ingest(session); p.ingest(lap); p.ingest(status); p.ingest(damage); p.ingest(timing); p.ingest(sets);
    }
};

void coverCalls() {
    // Fifteen laps into a stint, the car two seconds behind pits: cover it.
    {
        CoverRace race(15, 15, 2000);
        StrategyProcessor p(2025); race.feed(p);
        TimingRow pits = race.timing; pits.session_time += 5; pits.cars[2].pit_status = 1; p.ingest(pits);
        const auto s = p.snapshot();
        assert(s.call && s.call->kind == "cover" && s.call->target_idx == 2);
        assert(nextStop(s.conservative, s.lap_num) == s.lap_num + 1);
    }
    // The same stop from fifteen seconds back cannot undercut: no call, and
    // the Defend stop stays where it was.
    {
        CoverRace race(15, 15, 15000);
        StrategyProcessor p(2025); race.feed(p);
        const int planned = nextStop(p.snapshot().conservative, 15);
        TimingRow pits = race.timing; pits.session_time += 5; pits.cars[2].pit_status = 1; p.ingest(pits);
        const auto s = p.snapshot();
        assert(!s.call);
        assert(nextStop(s.conservative, s.lap_num) == planned);
    }
    // The player stops on lap 24 and the car behind answers on lap 25: the
    // same pit cycle, so on two-lap-old tyres there is nothing to cover.
    {
        CoverRace race(24, 20, 2000);
        StrategyProcessor p(2025); race.feed(p);
        TimingRow t = race.timing;
        t.session_time += 60; t.cars[0].pit_status = 1; p.ingest(t);
        t.session_time += 25; t.cars[0].pit_status = 0; t.cars[0].num_pit_stops = 1;
        t.cars[0].position = 3; t.cars[2].position = 2; t.cars[0].gap_ms = 27000; p.ingest(t);
        StatusRow fresh = race.status; fresh.session_time = t.session_time;
        fresh.tyre_compound = 19; fresh.visual_compound = 17; fresh.tyre_age_laps = 0; p.ingest(fresh);
        LapRow lap = race.lap; lap.num_pit_stops = 1;
        // Lap 25: the other car pits from ahead and rejoins behind.
        lap.lap_num = 25; lap.session_time = 25 * 90.0f; p.ingest(lap);
        for (auto& car : t.cars) car.lap_num = 25;
        t.session_time = lap.session_time + 60; t.cars[2].pit_status = 1; p.ingest(t);
        t.session_time += 25; t.cars[2].pit_status = 0; t.cars[2].num_pit_stops = 1;
        t.cars[0].position = 2; t.cars[2].position = 3; t.cars[2].gap_ms = 48000; p.ingest(t);
        // Lap 26, on two-lap-old tyres.
        lap.lap_num = 26; lap.session_time = 26 * 90.0f; p.ingest(lap);
        for (auto& car : t.cars) car.lap_num = 26;
        t.session_time = lap.session_time; p.ingest(t);
        fresh.tyre_age_laps = 2; fresh.session_time = lap.session_time; p.ingest(fresh);
        const auto s = p.snapshot();
        assert(!s.call);
        for (const auto& rival : s.rivals) assert(rival.pit_reaction.empty());
        assert(nextStop(s.conservative, s.lap_num) != s.lap_num + 1);
    }
}

// Rain forecast in the 60-minute sample: that sample keeps saying "60" for
// about a quarter of an hour, so the lap must hold rather than slide a lap
// later every lap, and may only come forward when the sample steps down.
void forecastHolds() {
    StrategyProcessor p(2025);
    SessionRow session; session.session_type = 15; session.track_id = 0; session.total_laps = 60;
    session.weather_forecast_samples = {{0, 0, 5}, {30, 0, 10}, {45, 1, 20}, {60, 3, 80}};
    // 88 s laps: 60 minutes is 40.9 laps, clear of a rounding boundary.
    LapRow lap; lap.lap_num = 3; lap.session_time = 270; lap.last_lap_ms = 88000;
    StatusRow status; status.session_time = 270; status.tyre_compound = 20; status.visual_compound = 18; status.tyre_age_laps = 3;
    DamageRow damage; damage.session_time = 270;
    p.ingest(lap); p.ingest(status); p.ingest(damage); p.ingest(session);
    auto s = p.snapshot();
    assert(s.weather_strategy && s.weather_strategy->recommendation == "prepare_intermediates");
    const int first = s.weather_strategy->crossover_lap;
    assert(first == 3 + 41);
    for (int i = 0; i < 8; ++i) {
        ++lap.lap_num; lap.session_time += 88; status.session_time = lap.session_time; ++status.tyre_age_laps;
        p.ingest(status); p.ingest(lap); p.ingest(session);
        s = p.snapshot();
        assert(s.weather_strategy && s.weather_strategy->crossover_lap == first);
    }
    // The sample steps down to 45 minutes a little early: the lap comes forward.
    session.weather_forecast_samples = {{0, 0, 5}, {30, 1, 20}, {45, 3, 80}, {60, 3, 90}};
    p.ingest(session);
    s = p.snapshot();
    assert(s.weather_strategy && s.weather_strategy->crossover_lap < first);
    const int stepped = s.weather_strategy->crossover_lap;
    // A forecast revised later is followed.
    session.weather_forecast_samples = {{0, 0, 5}, {30, 0, 10}, {45, 1, 20}, {60, 3, 80}};
    p.ingest(session);
    assert(p.snapshot().weather_strategy->crossover_lap > stepped);
}

void paceModel() {
    SessionRow session; session.session_type = 15; session.track_id = 0; session.total_laps = 50;
    DamageRow damage; damage.session_time = 180;
    TyreSetsRow sets; sets.session_time = 180;
    sets.sets.push_back({0, 20, 18, 0, true, 0, 38, 40, 0, true});
    sets.sets.push_back({1, 19, 17, 0, true, 0, 30, 30, -1500, false});
    // One slow lap is an outlier; two in a row are the conditions changing.
    {
        StrategyProcessor p(2025);
        LapRow lap; lap.lap_num = 2; lap.session_time = 180; lap.last_lap_ms = 90000;
        StatusRow status; status.session_time = 180; status.tyre_compound = 20; status.visual_compound = 18; status.tyre_age_laps = 2;
        p.ingest(session); p.ingest(lap); p.ingest(status); p.ingest(damage); p.ingest(sets);
        driveLaps(p, lap, status, 6, 90000);
        driveLaps(p, lap, status, 1, 95000);
        assert(currentTarget(p.snapshot()) < 91000);
        driveLaps(p, lap, status, 1, 96000);
        assert(currentTarget(p.snapshot()) > 94500);
        driveLaps(p, lap, status, 2, 90000);
        assert(currentTarget(p.snapshot()) < 91000);
    }
    // After a stop the first target is the old pace plus the game's delta for
    // the set that went on, not the pace of the tyres that came off.
    {
        StrategyProcessor p(2025);
        LapRow lap; lap.lap_num = 2; lap.session_time = 180; lap.last_lap_ms = 90000;
        StatusRow status; status.session_time = 180; status.tyre_compound = 20; status.visual_compound = 18; status.tyre_age_laps = 2;
        p.ingest(session); p.ingest(lap); p.ingest(status); p.ingest(damage); p.ingest(sets);
        driveLaps(p, lap, status, 6, 90000);
        const double before = currentTarget(p.snapshot());
        assert(before > 89500 && before < 90500);
        status.tyre_compound = 19; status.visual_compound = 17; status.tyre_age_laps = 0; p.ingest(status);
        const double after = currentTarget(p.snapshot());
        assert(after > before - 1700 && after < before - 1300);
    }
}

// The fitted set's own laps left hold the stint: a set that went on used has
// fewer than the compound's life less the laps driven. A worn-out spare with
// no laps left is not planned.
void tyreLife() {
    StrategyProcessor p(2025);
    SessionRow session; session.session_type = 15; session.track_id = 0; session.total_laps = 50;
    LapRow lap; lap.lap_num = 10; lap.session_time = 900; lap.last_lap_ms = 90000;
    StatusRow status; status.session_time = 900; status.tyre_compound = 20; status.visual_compound = 18; status.tyre_age_laps = 5;
    DamageRow damage; damage.session_time = 900;
    damage.tyre_wear_fl = damage.tyre_wear_fr = damage.tyre_wear_rl = damage.tyre_wear_rr = 5;
    TyreSetsRow sets; sets.session_time = 900;
    sets.sets.push_back({0, 20, 18, 30, true, 0, 15, 40, 0, true});
    sets.sets.push_back({1, 19, 17, 70, true, 0, 0, 30, -200, false});
    sets.sets.push_back({2, 19, 17, 0, true, 0, 30, 30, -300, false});
    p.ingest(session); p.ingest(lap); p.ingest(status); p.ingest(damage); p.ingest(sets);
    const auto s = p.snapshot();
    assert(s.cliff_lap == 10 + 15);
    for (const auto* plan : {&s.conservative, &s.aggressive}) {
        int mediums = 0;
        for (const auto& stint : plan->stints) if (stint.visual_compound == 17) ++mediums;
        assert(mediums <= 1);
    }
}
// Attack's stop for the fastest set to the flag. A soft that is 0.9 s a lap
// quicker than four-lap-old hards gives that back as it wears, so 33 laps of
// it do not repay a stop; 2.5 s a lap over the last 15 laps does.
void attackEconomicStop() {
    const auto attackStop = [](int lapNum, int softDeltaMs) {
        StrategyProcessor p(2025); p.setMinimumStops(1);
        SessionRow session; session.session_type = 15; session.track_id = 0; session.total_laps = 71;
        LapRow lap; lap.lap_num = lapNum; lap.session_time = lapNum * 75.0f; lap.last_lap_ms = 75000; lap.num_pit_stops = 1;
        StatusRow soft; soft.session_time = 100; soft.tyre_compound = 19; soft.visual_compound = 16; soft.tyre_age_laps = 20;
        StatusRow hard; hard.session_time = lap.session_time; hard.tyre_compound = 21; hard.visual_compound = 18; hard.tyre_age_laps = 4;
        DamageRow damage; damage.session_time = lap.session_time;
        damage.tyre_wear_fl = damage.tyre_wear_fr = damage.tyre_wear_rl = damage.tyre_wear_rr = 4;
        TyreSetsRow sets; sets.session_time = lap.session_time;
        sets.sets.push_back({0, 21, 18, 4, true, 0, 50, 53, 0, true});
        sets.sets.push_back({1, 19, 16, 0, true, 0, 35, 35, softDeltaMs, false});
        TimingRow timing; timing.session_time = lap.session_time; timing.player_idx = 0;
        TimingCar player; player.idx = 0; player.position = 5; player.lap_num = lapNum; player.num_pit_stops = 1;
        player.result_status = 2; player.driver_status = 4; timing.cars.push_back(player);
        LapRow earlier = lap; earlier.lap_num = lapNum - 4; earlier.session_time -= 300;
        p.ingest(session); p.ingest(earlier); p.ingest(soft); p.ingest(hard);
        p.ingest(lap); p.ingest(damage); p.ingest(timing); p.ingest(sets);
        const auto s = p.snapshot();
        return nextStop(s.aggressive, s.lap_num) - s.lap_num;
    };
    assert(attackStop(38, -900) != 1);
    assert(attackStop(56, -2500) == 1);
}
double currentAttackTarget(const StrategySnapshotRow& s) {
    for (const auto& stint : s.aggressive.stints)
        for (const auto& row : stint.rows)
            if (row.lap_num == s.lap_num) return row.required_ms;
    return 0;
}

// An undercut has the gap to make up at the fresh tyres' gain per lap: 1.5 s
// at 0.8 s a lap is two laps. Four seconds would take five, too long.
void undercutCall() {
    const auto call = [](int gapMs) {
        CoverRace race(15, 15, 6000);
        race.sets.sets[0].life_span = 25;
        race.sets.sets[1].lap_delta_ms = -800;
        race.timing.cars[0].gap_ms = gapMs;
        StrategyProcessor p(2025); race.feed(p);
        AllStatusRow all; all.session_time = race.lap.session_time;
        AllStatusCar rival; rival.idx = 1; rival.tyre_compound = 20; rival.visual_compound = 18; rival.tyre_age_laps = 15;
        all.cars.push_back(rival); p.ingest(all);
        return p.snapshot().call;
    };
    const auto near = call(1500);
    assert(near && near->kind == "undercut" && near->target_idx == 1);
    assert(near->crossover_laps && *near->crossover_laps == 2);
    assert(!call(4000));
}

// Three laps behind a Safety Car wear the tyres far less. The first green lap
// after them must not average that in: the rate stays the green-flag one.
void wearAfterSafetyCar() {
    StrategyProcessor p(2025);
    SessionRow session; session.session_type = 15; session.track_id = 0; session.total_laps = 50;
    LapRow lap; lap.lap_num = 4; lap.session_time = 360; lap.last_lap_ms = 90000;
    StatusRow status; status.session_time = 360; status.tyre_compound = 20; status.visual_compound = 18; status.tyre_age_laps = 4;
    DamageRow damage; damage.session_time = 360;
    double wear = 8;
    damage.tyre_wear_fl = damage.tyre_wear_fr = damage.tyre_wear_rl = damage.tyre_wear_rr = wear;
    p.ingest(session); p.ingest(lap); p.ingest(status); p.ingest(damage);
    const auto drive = [&](double gained) {
        wear += gained; ++lap.lap_num; lap.session_time += 90; ++status.tyre_age_laps;
        damage.tyre_wear_fl = damage.tyre_wear_fr = damage.tyre_wear_rl = damage.tyre_wear_rr = wear;
        damage.session_time = status.session_time = lap.session_time;
        p.ingest(damage); p.ingest(status); p.ingest(lap);
    };
    for (int i = 0; i < 4; ++i) drive(2.0);
    assert(p.snapshot().limiting_wear_per_lap > 1.9);
    SessionRow sc = session; sc.safety_car_status = 1; p.ingest(sc);
    for (int i = 0; i < 2; ++i) drive(0.3);
    // The Safety Car comes in during the third neutralised lap.
    p.ingest(session);
    drive(0.5);
    drive(2.0);
    const auto s = p.snapshot();
    assert(s.limiting_wear_per_lap > 1.8 && s.limiting_wear_per_lap < 2.2);
}

// The current stint's target: against a quicker car behind, its pace plus the
// gap spread over the laps left; never slower than the player's own pace; to
// catch the car ahead, its pace less that share of the gap.
void lapTargets() {
    {
        CoverRace race(15, 15, 4000);
        race.timing.cars[2].last_lap_ms = 89000;
        StrategyProcessor p(2025); race.feed(p);
        const double target = currentTarget(p.snapshot());
        assert(target > 89050 && target < 89400);
    }
    {
        CoverRace race(15, 15, 4000);
        race.timing.cars[2].last_lap_ms = 93000;
        StrategyProcessor p(2025); race.feed(p);
        assert(currentTarget(p.snapshot()) == 90000);
    }
    {
        CoverRace race(15, 15, 20000);
        race.timing.cars[1].last_lap_ms = 89500;
        race.timing.cars[2].last_lap_ms = 93000;
        StrategyProcessor p(2025); race.feed(p);
        const double target = currentAttackTarget(p.snapshot());
        assert(target > 89000 && target < 89450);
    }
    // A rival's raw last lap through the pits is not a pace.
    {
        CoverRace race(15, 15, 2000);
        StrategyProcessor p(2025); race.feed(p);
        TimingRow t = race.timing; t.session_time += 60; t.cars[2].pit_status = 1; p.ingest(t);
        for (auto& car : t.cars) car.lap_num = 16;
        t.session_time += 30; t.cars[2].pit_status = 0; t.cars[2].num_pit_stops = 1;
        t.cars[2].last_lap_ms = 118000; p.ingest(t);
        LapRow lap = race.lap; lap.lap_num = 16; lap.session_time = t.session_time; p.ingest(lap);
        const auto s = p.snapshot();
        for (const auto& rival : s.rivals) if (rival.idx == 2) assert(rival.pace_ms == 0);
        assert(currentTarget(s) <= 90000);
    }
}

// Three clean stops, each costing 24 s against Melbourne's catalogued 21.5 s,
// pull the pit loss part of the way there; a stop under a Safety Car does not count.
void pitLossCalibration() {
    StrategyProcessor p(2025);
    SessionRow session; session.session_type = 15; session.track_id = 0; session.total_laps = 50;
    LapRow lap; lap.lap_num = 2; lap.session_time = 180; lap.last_lap_ms = 90000;
    StatusRow status; status.session_time = 180; status.tyre_compound = 20; status.visual_compound = 18; status.tyre_age_laps = 2;
    DamageRow damage; damage.session_time = 180;
    TimingRow t; t.session_time = 180; t.player_idx = 0;
    for (int i = 0; i < 5; ++i) {
        TimingCar car; car.idx = i; car.position = i + 1; car.lap_num = 2; car.result_status = 2;
        car.driver_status = 4; car.gap_ms = i * 3000; car.last_lap_ms = 90000; t.cars.push_back(car);
    }
    p.ingest(session); p.ingest(lap); p.ingest(status); p.ingest(damage); p.ingest(t);
    assert(p.snapshot().pit_loss_ms == 21500 && p.snapshot().pit_loss_samples == 0);
    const auto newLap = [&](int lapNum) {
        for (auto& car : t.cars) { car.lap_num = lapNum; if (car.pit_status == 0 && car.last_lap_ms > 100000) car.last_lap_ms = 90000; }
        t.session_time += 90; p.ingest(t);
    };
    for (int n = 3; n <= 5; ++n) newLap(n);
    // Cars 1-3 stop on lap 5, cross the line in the pit lane and rejoin on lap 6.
    // Car 4 does the same on lap 9, under a Safety Car.
    const auto stop = [&](std::vector<int> cars, int lapNum) {
        for (int idx : cars) t.cars[idx].pit_status = 1;
        t.session_time += 60; p.ingest(t);
        for (auto& car : t.cars) car.lap_num = lapNum + 1;
        for (int idx : cars) t.cars[idx].last_lap_ms = 100000;
        t.session_time += 30; p.ingest(t);
        for (int idx : cars) { t.cars[idx].pit_status = 0; ++t.cars[idx].num_pit_stops; }
        t.session_time += 20; p.ingest(t);
        for (auto& car : t.cars) car.lap_num = lapNum + 2;
        for (int idx : cars) t.cars[idx].last_lap_ms = 104000;
        t.session_time += 70; p.ingest(t);
    };
    stop({1, 2, 3}, 5);
    newLap(8);
    auto s = p.snapshot();
    assert(s.pit_loss_samples == 3);
    assert(s.pit_loss_ms > 22500 && s.pit_loss_ms < 23000);
    SessionRow sc = session; sc.safety_car_status = 1; p.ingest(sc);
    newLap(9);
    stop({4}, 9);
    p.ingest(session);
    newLap(12);
    assert(p.snapshot().pit_loss_samples == 3);
}

// A red flag puts every car in the pit lane: that is not a field of rivals
// stopping, nothing is called, and a stop still to come is taken for free.
void redFlag() {
    CoverRace race(15, 15, 2000);
    race.sets.sets[0].life_span = 25;
    StrategyProcessor p(2025); race.feed(p);
    assert(!p.snapshot().red_flag);
    RaceEventRow flag; flag.code = "RDFL"; p.ingest(flag);
    TimingRow t = race.timing; t.session_time += 30;
    for (auto& car : t.cars) car.pit_status = 1;
    p.ingest(t);
    LapRow lap = race.lap; lap.session_time = t.session_time; lap.pit_status = 1; p.ingest(lap);
    auto s = p.snapshot();
    assert(s.red_flag && !s.call && !s.neutralisation);
    for (const auto& rival : s.rivals) assert(rival.pit_reaction.empty() && rival.last_pit_lap < 0);
    assert(nextStop(s.conservative, s.lap_num) == s.lap_num + 1);
    assert(nextStop(s.aggressive, s.lap_num) == s.lap_num + 1);
    // Back on track: the flag is over, and no rival counts as having stopped.
    for (auto& car : t.cars) car.pit_status = 0;
    t.session_time += 600; p.ingest(t);
    lap.session_time = t.session_time; lap.pit_status = 0; p.ingest(lap);
    s = p.snapshot();
    assert(!s.red_flag && !s.call);
    for (const auto& rival : s.rivals) assert(rival.pit_reaction.empty());
}

// One stop is owed, but no spare set reaches the flag from here while the
// fitted tyres last until one does: boxing under the Safety Car buys a second stop.
void safetyCarExtraStop() {
    SafetyCarRace race(0, 10, 20, 28);
    race.sets.sets[1].life_span = 20; race.sets.sets[1].usable_life = 20;
    StrategyProcessor p(2025); p.setMinimumStops(1); race.feed(p);
    SessionRow sc = race.session; sc.safety_car_status = 1; p.ingest(sc);
    const auto s = p.snapshot();
    assert(s.neutralisation && s.neutralisation->recommendation == "stay_out");
    assert(s.neutralisation->reason == "box_now_adds_a_stop");
}

// The game's laps-left figure does not fall by exactly one a lap. A planned
// stop holds through a one-lap wobble and moves for two.
void steadyStops() {
    StrategyProcessor p(2025);
    SessionRow session; session.session_type = 15; session.track_id = 0; session.total_laps = 60;
    LapRow lap; lap.lap_num = 9; lap.session_time = 810; lap.last_lap_ms = 90000;
    StatusRow status; status.session_time = 810; status.tyre_compound = 20; status.visual_compound = 18; status.tyre_age_laps = 9;
    DamageRow damage; damage.session_time = 810;
    damage.tyre_wear_fl = damage.tyre_wear_fr = damage.tyre_wear_rl = damage.tyre_wear_rr = 5;
    TyreSetsRow sets; sets.session_time = 810;
    sets.sets.push_back({0, 20, 18, 5, true, 0, 21, 40, 0, true});
    sets.sets.push_back({1, 19, 17, 0, true, 0, 30, 30, 300, false});
    p.ingest(session); p.ingest(lap); p.ingest(status); p.ingest(damage); p.ingest(sets);
    const auto advance = [&](int lifeLeft) {
        sets.sets[0].life_span = lifeLeft; ++lap.lap_num; lap.session_time += 90; ++status.tyre_age_laps;
        sets.session_time = status.session_time = lap.session_time;
        p.ingest(sets); p.ingest(status); p.ingest(lap);
        return nextStop(p.snapshot().conservative, lap.lap_num);
    };
    assert(advance(20) == 30);
    assert(advance(20) == 30);  // the estimate is lap 31: held
    assert(advance(20) == 32);  // two laps on: the stop moves
    assert(advance(19) == 32);  // and is steady again
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
    safetyCar();
    coverCalls();
    forecastHolds();
    paceModel();
    tyreLife();
    attackEconomicStop();
    undercutCall();
    wearAfterSafetyCar();
    lapTargets();
    pitLossCalibration();
    redFlag();
    safetyCarExtraStop();
    steadyStops();

    RaceEventRow restart; restart.code="SSTA"; monaco.ingest(restart);
    assert(monaco.snapshot().state=="non_race");
    std::cout << "strategy processor ok\n";
}
