#include "tnrp/Strategy.h"
#include "tnrp/AnyRow.h"

#include <cassert>
#include <iostream>

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

    StrategyProcessor incomplete(2025); incomplete.ingest(session);
    assert(incomplete.snapshot().state=="waiting");

    StrategyProcessor monaco(2025);
    SessionRow wetSession=session; wetSession.track_id=5; wetSession.weather=3; wetSession.total_laps=40;
    LapRow wetLap=lap; wetLap.lap_num=3;
    StatusRow wetStatus=status; wetStatus.tyre_compound=7; wetStatus.visual_compound=7;
    DamageRow uneven=damage; uneven.tyre_wear_fl=60; uneven.tyre_wear_fr=10; uneven.tyre_wear_rl=10; uneven.tyre_wear_rr=10;
    TyreSetsRow wetSets; wetSets.sets.push_back({0,7,7,10,true,0,25,22,0,true});
    wetSets.sets.push_back({1,8,8,0,true,0,30,28,-300,false});
    wetSets.sets.push_back({2,19,17,0,true,0,25,24,-800,false}); // dry: must be filtered
    monaco.ingest(wetSession); monaco.ingest(wetLap); monaco.ingest(wetStatus);
    monaco.ingest(uneven); monaco.ingest(wetSets); monaco.ingest(timing);
    auto m=monaco.snapshot();
    assert(m.state=="ready" && m.is_monaco);
    assert(m.conservative.stops>=2);
    for(size_t i=1;i<m.conservative.stints.size();++i)
        assert(m.conservative.stints[i].actual_compound==7 || m.conservative.stints[i].actual_compound==8);
    assert(!m.wear_warnings.empty() && m.wear_warnings.front().priority==0);

    RaceEventRow restart; restart.code="SSTA"; monaco.ingest(restart);
    assert(monaco.snapshot().state=="non_race");
    std::cout << "strategy processor ok\n";
}
