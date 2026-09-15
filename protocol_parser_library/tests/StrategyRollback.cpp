#include "StrategyRollback.h"

#include <cassert>
#include <iostream>

using tnrp::detail::StrategyRollback;
using namespace tnrp;

namespace {

template <class Row>
void feed(StrategyRollback& rollback, tnrp::StrategyProcessor& reference,
          float time, const Row& row) {
    const auto json = std::make_shared<const std::string>(tnrp::writeJson(row));
    rollback.ingest(time, json);
    reference.ingestJson(*json);
}

void sameSnapshot(StrategyRollback& rollback, tnrp::StrategyProcessor& reference) {
    const auto actual = rollback.snapshotJson();
    const auto expected = reference.snapshotJson();
    assert(actual == expected);
}

struct Race {
    StrategyRollback rollback;
    tnrp::StrategyProcessor reference;
    SessionRow session;
    LapRow lap;
    StatusRow status;
    TimingRow timing;

    Race() {
        session.session_type = 15; session.track_id = 0; session.total_laps = 20;
        feed(rollback, reference, 180, session);
        lap.lap_num = 3; lap.session_time = 180; lap.last_lap_ms = 90000;
        feed(rollback, reference, 180, lap);
        status.session_time = 180; status.tyre_compound = 18;
        status.visual_compound = 16; status.tyre_age_laps = 3;
        feed(rollback, reference, 180, status);
        DamageRow damage;
        damage.session_time = 180;
        damage.tyre_wear_fl = 18; damage.tyre_wear_fr = 12;
        damage.tyre_wear_rl = 10; damage.tyre_wear_rr = 11;
        feed(rollback, reference, 180, damage);
        TyreSetsRow sets; sets.session_time = 180;
        sets.sets.push_back({0,18,16,18,true,0,0,12,0,true});
        sets.sets.push_back({1,19,17,0,true,0,20,18,-700,false});
        feed(rollback, reference, 180, sets);
        ParticipantsRow participants;
        participants.drivers.push_back({0,"Player",0,1,false,"#ff0000"});
        participants.drivers.push_back({1,"Rival",0,2,false,"#00ff00"});
        feed(rollback, reference, 180, participants);
        timing.session_time = 180; timing.player_idx = 0;
        TimingCar player; player.idx = 0; player.position = 2;
        player.lap_num = 3; player.last_lap_ms = 90000;
        player.result_status = 2; player.driver_status = 4;
        timing.cars.push_back(player);
        auto rival = player; rival.idx = 1; rival.position = 1;
        rival.last_lap_ms = 89500;
        timing.cars.push_back(rival);
        feed(rollback, reference, 180, timing);
        sameSnapshot(rollback, reference);
    }

    void advance(float time, int number, int lapMs = 90000) {
        lap.session_time = time; lap.lap_num = number; lap.last_lap_ms = lapMs;
        feed(rollback, reference, time, lap);
        timing.session_time = time;
        for (auto& car : timing.cars) {
            car.lap_num = number; car.last_lap_ms = lapMs;
        }
        feed(rollback, reference, time, timing);
        sameSnapshot(rollback, reference);
    }
};

void sameLap() {
    Race race;
    race.advance(181.0f, 3);
    race.advance(181.5f, 3);
    auto target = race.reference;
    // Both changes are sticky in the reducer even though the lap is unchanged.
    RaceEventRow retirement; retirement.code = "RTMT";
    retirement.session_time = 181.8f; retirement.car_idx = 1;
    feed(race.rollback, race.reference, 181.8f, retirement);
    race.status.tyre_compound = 19; race.status.visual_compound = 17;
    race.status.tyre_age_laps = 0; race.status.session_time = 182;
    feed(race.rollback, race.reference, 182, race.status);
    race.session.safety_car_status = 1;
    feed(race.rollback, race.reference, 182, race.session);
    race.advance(183, 3);

    const bool restored = race.rollback.rollback(181.5f);
    assert(restored);
    sameSnapshot(race.rollback, target);
    assert(race.rollback.memoryStats().replayedRows > 0);
    assert(race.rollback.memoryStats().fallbacks == 0);
}

void lapBoundaryAndRepeatedBranch() {
    Race race;
    race.advance(189, 3);
    auto lapThree = race.reference;
    race.advance(190, 4, 91000);
    race.advance(191, 4, 91000);
    bool restored = race.rollback.rollback(189);
    assert(restored);
    race.reference = lapThree;
    sameSnapshot(race.rollback, race.reference);
    // Re-completing the lap must replace its time and rival observations.
    race.advance(190, 4, 88000);
    auto newLapFour = race.reference;
    race.advance(192, 5, 87000);
    restored = race.rollback.rollback(190);
    assert(restored);
    race.reference = newLapFour;
    sameSnapshot(race.rollback, race.reference);
    restored = race.rollback.rollback(189);
    assert(restored);
    race.reference = lapThree;
    sameSnapshot(race.rollback, race.reference);
    race.advance(190, 4, 86000);
    assert(race.rollback.memoryStats().fallbacks == 0);
}

void boundedHistory() {
    Race race;
    for (int second = 181; second <= 600; ++second) {
        race.lap.session_time = static_cast<float>(second);
        feed(race.rollback, race.reference, static_cast<float>(second), race.lap);
    }
    const auto memory = race.rollback.memoryStats();
    assert(memory.retainedBytes <= StrategyRollback::MAX_BYTES);
    assert(memory.checkpoints <= 33);
    assert(memory.rows <= 33);
    auto before = race.rollback.processor();
    const bool restored = race.rollback.rollback(200);
    assert(!restored); // The caller must use streaming history for this target.
    sameSnapshot(race.rollback, before); // A miss must not partly reset state.
    assert(race.rollback.memoryStats().fallbacks == 1);

    // Memory remains bounded even if telemetry keeps arriving while time stops.
    race.rollback.reset();
    const auto padded = std::make_shared<const std::string>(
        "{\"type\":\"race_event\",\"code\":\"TEST\",\"ts\":\"" +
        std::string(1024 * 1024, 'x') + "\",\"session_time\":1}");
    for (int i = 0; i < 80; ++i) race.rollback.ingest(1, padded);
    assert(race.rollback.memoryStats().retainedBytes <= StrategyRollback::MAX_BYTES);
    assert(race.rollback.memoryStats().rows < 33);
}

void configurationBoundary() {
    Race race;
    race.advance(181, 3);
    race.rollback.processor().setMinimumStops(2);
    race.reference.setMinimumStops(2);
    race.rollback.configurationChanged();
    race.advance(181.5f, 3);
    auto target = race.reference;
    race.advance(183, 4);
    const bool restored = race.rollback.rollback(181.5f);
    assert(restored);
    sameSnapshot(race.rollback, target);
}

} // namespace

int main() {
    sameLap();
    lapBoundaryAndRepeatedBranch();
    boundedHistory();
    configurationBoundary();
    std::cout << "strategy rollback ok\n";
}
