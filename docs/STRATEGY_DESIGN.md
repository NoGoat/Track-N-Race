# Strategy Engine Design and Roadmap

Status: Proposed  
Last updated: 2026-08-20  
Scope: Shared `libtnrp` strategy engine, Electron strategy page, Qt strategy page,
live telemetry, recorded-session playback, and seeking

## 1. Purpose

This document defines the intended direction for Track N Race's race strategy
system. It is deliberately more ambitious than a list of isolated fixes. The
goal is to turn the current strategy reducer into a deterministic race-decision
engine that can answer three questions reliably:

1. What is happening in the player's race right now?
2. What are the credible options from this point?
3. Which option best serves the selected objective, and why is it actionable?

The current implementation already has the correct architectural home: strategy
is calculated once in the shared C++ engine, consumed by both applications, and
reconstructed from normalized telemetry during playback and seeking. That
foundation should remain. The next work should improve the model inside that
boundary rather than moving arithmetic into either UI.

The design also reflects several product decisions already made:

- strategy is recalculated as the race develops, not generated once at the start;
- rivals are dynamic and must stop influencing the plan when they stop being a
  credible threat;
- safety-car and virtual-safety-car deployment must trigger an immediate pit
  decision;
- a temporary ten-second pit-loss reduction is used for SC/VSC projections;
- opaque model prose, full position tables, and decision-history cards do not
  belong in the main live sidebar;
- lap targets should show the real required pace, not an artificial
  "Adjusted Required" value;
- tyre messages should report observable facts rather than invent setup or
  driving advice;
- seeking must reconstruct the complete strategy state up to the target time;
- calls such as cover, undercut, and overcut must be real, actionable race
  decisions rather than labels attached to loosely related telemetry states.

## 2. Product definition

The strategy page is a race engineer, not a generic telemetry dashboard. Its
primary job is to reduce a large telemetry stream into a small number of useful
decisions. It should not expose engine internals simply because the data exists.

At any moment, the page should prioritize information in this order:

1. an exceptional immediate decision, such as `BOX NOW` under SC/VSC;
2. an actionable green-flag race call, such as cover or extend;
3. the next defensive and attacking pit windows;
4. the active race battle that makes those plans relevant;
5. tyre condition and the limiting tyre;
6. a weather crossover only when a crossover is actually forecast;
7. detailed stint and lap-target information for users who want to inspect the
   plan.

Anything that does not change an action, explain a plan, or quantify a real
risk should not occupy permanent live-screen space.

## 3. Goals

### 3.1 Correctness goals

- Recompute the plan at every completed lap and every material race event.
- Never issue an action that is no longer possible.
- Never describe two cars on matched fresh-tyre pit cycles as an overcut or
  undercut opportunity.
- Use only active, strategically relevant cars as rivals.
- Distinguish a car entering the pits, completing a stop, leaving on an out-lap,
  and settling onto its new stint.
- Respect physical tyre-set availability and dry-compound legality.
- Treat SC, VSC, rain transitions, red flags, retirements, and flashbacks as
  explicit state transitions.
- Produce the same strategy at the same session time in live processing,
  ordinary playback, and direct seeking.

### 3.2 Usefulness goals

- Show when to act, not merely what phenomenon has been detected.
- Show the cost of each credible option in time and projected position.
- Keep defensive and attacking plans meaningfully different.
- Prefer a stable recommendation over one that oscillates every packet.
- Change rivals when the current rival is no longer relevant, but avoid rapid
  rival thrashing between near-equal cars.
- Express uncertainty as data quality and decision margin, not generic
  confidence decoration.
- Make terse live UI copy sufficient without requiring explanatory paragraphs.

### 3.3 Engineering goals

- Keep strategy arithmetic exclusively in `protocol_parser_library`.
- Keep the processor deterministic and copyable so seek checkpoints remain
  practical.
- Avoid adding strategy rows to `.tnrd`; strategy remains derived from recorded
  normalized telemetry.
- Keep heavy scenario evaluation off the per-frame hot path.
- Give Electron and Qt equivalent semantic output even if their widgets differ.
- Make every recommendation inspectable through structured diagnostic evidence
  without showing that evidence in the default live UI.

## 4. Non-goals

The first complete version is not intended to be:

- a machine-learning race simulator;
- an exact reproduction of an F1 team's proprietary tyre model;
- a setup adviser;
- a driving coach;
- a guarantee of a particular finishing position;
- a replacement for race-control rules or stewarding logic;
- a high-frequency optimizer that rebuilds the entire race on every UDP packet;
- a user-tunable spreadsheet with dozens of unexplained coefficients.

The model should be transparent, deterministic, conservative about uncertain
inputs, and incrementally improvable.

## 5. Current architecture

### 5.1 Existing data flow

```text
F1 UDP packets
    |
    v
per-year parsers (F1 24 / 25 / 26)
    |
    v
normalized cold rows
    |
    +--> recording (.tnrd stores normalized inputs, not strategy)
    |
    v
StrategyProcessor::ingest(...)
    |
    v
StrategyProcessor::snapshot()
    |
    +--> Electron JSON strategy row
    +--> Qt typed StrategySnapshotRow
    +--> TnrdReader seek reconstruction
```

`StrategyProcessor` currently consumes lap, session, player status, car damage,
timing, participants, tyre sets, all-car status, and race-event rows. The
strategy dependency bitmask allows V4 playback seeking to decode only those
cold row families instead of inflating telemetry or motion chunks.

The processor is intentionally stateful. It retains completed lap times,
per-rival observations, tyre-wear history, used compounds, neutralisation state,
past stint targets, and rival-selection hysteresis. A snapshot is a complete UI
model and is not recorded.

### 5.2 Existing strengths

The current system already provides valuable foundations:

- shared logic for Electron and Qt;
- race-only activation and waiting states;
- track-specific base pit losses;
- physical tyre-set use and basic life limits;
- dry-compound legality checks;
- Monaco-specific extra-stop handling;
- conservative and aggressive plan variants;
- robust recent-lap pace filtering;
- tyre-cliff projection from recent completed-lap wear deltas;
- rival scoring, switching hysteresis, and race-window filtering;
- immediate SC/VSC evaluation with the temporary ten-second discount;
- weather forecast crossover estimation;
- replay and seek reconstruction with reusable checkpoints;
- concise race-battle and tyre-condition UI sections.

### 5.3 Current limitations

Most remaining problems come from the fact that state interpretation, strategy
generation, and UI call generation are still interleaved inside one large
`snapshot()` operation.

Examples of the resulting weaknesses include:

- a recent rival stop can influence a plan without first proving that the
  player's stop remains unmatched;
- pit-cycle concepts are inferred from scattered booleans and lap comparisons
  rather than an explicit state machine;
- conservative and aggressive plans are constructed with different first-stop
  heuristics, not scored from a common set of projected scenarios;
- pit loss is a static catalog value and is not calibrated from the current
  session;
- race position after a stop is estimated from leader gaps without a reusable
  rejoin/traffic model;
- pace normalization uses a fixed fuel correction rather than learning from the
  race;
- tyre degradation and raw tyre wear are sometimes treated as interchangeable;
- warm-up, out-lap, traffic, and compound pace effects are only partially
  separated;
- confidence is a broad snapshot number rather than uncertainty attached to
  individual estimates and decisions;
- reason strings are often implementation facts rather than user-facing action
  evidence;
- public snapshot fields still include data no longer intended for the main UI,
  such as full positions, model explanation, and decision history;
- strategy calls have no explicit lifecycle, expiry, or replacement identity;
- decision stability is achieved through isolated thresholds rather than a
  consistent change policy.

## 6. Design principles

### 6.1 Facts before interpretations

Raw normalized telemetry should first become a coherent race state. A strategy
rule should never directly consume a loosely related packet condition when a
stronger state can be derived.

For example:

```text
Bad:
    rival last_pit_lap is recent -> issue overcut

Required:
    rival began a stop
    + player's matching stop has not begun or completed
    + player remains on a viable older stint
    + rival is strategically ahead
    + extending has a projected benefit
    -> issue an actionable overcut/extend call
```

### 6.2 Scenarios before labels

"Defensive," "attacking," "cover," and "overcut" are outcomes of comparing
scenarios. They must not be the starting point for generating a plan.

### 6.3 Hard validity before optimization

Each candidate is first filtered by hard constraints:

- race and car state are valid;
- the action can still be taken;
- tyre sets physically exist;
- the plan can finish the race;
- compound rules are satisfied;
- the player is not already in the relevant transition;
- the candidate does not rely on a retired, lapped, or irrelevant rival.

Only valid candidates are scored.

### 6.4 Stable decisions, responsive emergencies

Normal plans should use hysteresis and minimum improvement thresholds. Material
events such as SC/VSC deployment, a rival entering the pits, a tyre crossing a
critical threshold, or a weather crossover should bypass ordinary throttling
and trigger immediate evaluation.

### 6.5 Determinism is a feature

Given the same ordered normalized rows and the same model version, the engine
must produce the same snapshot. No wall-clock time, random number, UI state, or
thread scheduling may affect a result.

### 6.6 The UI is not a debug console

The engine may retain rich evidence, but the main UI should show decisions and
facts. Diagnostics should be opt-in and structured.

## 7. Target architecture

The processor should be decomposed into explicit logical stages. These may be
separate classes or well-defined internal modules; the important requirement is
that their contracts remain clear.

```text
Normalized rows
    |
    v
RaceStateReducer
    |
    +--> SessionState
    +--> PlayerState
    +--> CarRaceState[]
    +--> TrackState
    +--> DataQuality
    |
    v
Estimators
    |
    +--> PaceModel
    +--> TyreModel
    +--> PitLossModel
    +--> RejoinModel
    +--> WeatherModel
    |
    v
RivalSelector + PitCycleInterpreter
    |
    v
ScenarioGenerator
    |
    v
RaceProjector
    |
    v
ScenarioScorer
    |
    v
DecisionStateMachine
    |
    v
StrategySnapshotRow
```

### 7.1 `RaceStateReducer`

Responsibilities:

- merge row families into one coherent session-time view;
- retain last-seen timestamps per source;
- derive completed-lap boundaries;
- detect stint changes and pit transitions;
- handle session restart, flashback, and retirement events;
- distinguish unknown values from real zero values;
- expose state without making strategy decisions.

The reducer should be inexpensive enough to run for every relevant cold row.

### 7.2 Estimators

Estimators convert race facts into quantities needed by scenario projection.
Each estimate should carry a value, confidence/quality, sample count, and the
session time or lap at which it was updated.

Suggested generic representation:

```cpp
template <typename T>
struct Estimate {
    T value{};
    double uncertainty{};
    int sample_count{};
    float updated_at{};
    std::string quality; // unavailable | weak | usable | strong
};
```

This does not have to be serialized directly. It is primarily an internal
contract that prevents a weak fallback estimate from looking identical to a
well-supported estimate.

### 7.3 `ScenarioGenerator`

Responsibilities:

- enumerate plausible stop laps near meaningful boundaries;
- enumerate usable physical tyre sets and legal compound sequences;
- include stay-out, cover, undercut, overcut/extend, and neutralisation options
  only when their preconditions are satisfied;
- cap the search space deterministically;
- generate both baseline and rival-responsive candidates from the same model.

### 7.4 `RaceProjector`

Responsibilities:

- project lap time, tyre age, tyre wear, pit loss, and track position for each
  candidate;
- model out-lap and tyre warm-up separately from settled pace;
- apply expected traffic costs after rejoin;
- project only as far as useful, while still proving that the plan can complete
  the race legally;
- expose component costs so scoring can be audited.

### 7.5 `ScenarioScorer`

Responsibilities:

- reject invalid candidates;
- calculate objective-specific scores;
- retain the best defensive, attacking, and neutral baseline candidates;
- include uncertainty penalties;
- require a meaningful improvement before replacing the currently published
  plan.

### 7.6 `DecisionStateMachine`

Responsibilities:

- convert scenario differences into named live calls;
- enforce actionability and expiry;
- avoid repeating an unchanged call every lap;
- replace or cancel calls when their prerequisites disappear;
- preserve enough internal history to assess behaviour without rendering a
  decision-history card.

## 8. Explicit race-state model

### 8.1 Per-car state

Each strategically relevant car should have a derived `CarRaceState` containing
at least:

- car index, driver identity, team identity, active/result status;
- race position, current lap, lap distance if available, and leader gap;
- signed gap to player and gap trend;
- current pit status, pit-stop count, and driver status;
- current actual and visual compound;
- tyre age and current stint start lap;
- recent valid green-flag laps;
- estimated settled pace and uncertainty;
- current pit-cycle state;
- lap on which the latest pit transition began and completed;
- whether the car is on the player's lap;
- whether the car is a valid offensive or defensive rival;
- last observation times for timing and all-car status.

### 8.2 Pit-cycle state machine

Pit behaviour should be represented as an enum rather than repeated ad hoc
conditions:

```text
Unknown
  -> StableStint
  -> InLap
  -> PitEntry
  -> InPitLane
  -> StopCompleted
  -> PitExit
  -> OutLap
  -> Settling
  -> StableStint
```

Transitions should use the strongest available evidence:

- `pit_status` for entry and in-pit state;
- `num_pit_stops` increments for a completed stop;
- compound or tyre-age reset for a fitted-set change;
- `driver_status` for in-lap and out-lap context;
- lap-number progression to age transitions and expire temporary states.

Conflicting or missing evidence should produce `Unknown` or a lower-quality
transition, not a confident strategic call.

### 8.3 Matched and unmatched stops

For any player-rival pair, derive a pit-cycle relationship:

- `same_cycle`: equal completed-stop count and neither car is in a new stop;
- `rival_stopping_first`: rival is actively pitting while player is stable;
- `rival_one_stop_ahead`: rival has completed one more stop;
- `player_stopping_first`: player is actively pitting while rival is stable;
- `player_one_stop_ahead`: player has completed one more stop;
- `cycle_ambiguous`: packet ordering or missing status prevents a reliable
  conclusion;
- `different_strategy`: stop-count difference persists beyond the normal
  response window and represents a genuinely offset strategy.

This relationship is the main precondition for cover, undercut, and overcut
logic. Tyre age alone must never determine pit-cycle relationship.

### 8.4 Data quality

The engine should maintain quality flags for:

- timing freshness;
- player-status freshness;
- all-car-status freshness;
- tyre-set availability;
- weather forecast availability and accuracy;
- pace sample quality;
- wear sample quality;
- gap validity;
- playback reconstruction completeness.

A missing source should disable only decisions that require it. For example,
missing all-car tyre status can still permit a tyre-life baseline, but should
suppress an undercut claim based on rival tyre age.

## 9. Rival model

### 9.1 Separate offensive and defensive roles

The engine should continue selecting at most one primary rival ahead and one
behind for the main display, but selection should be based on projected
strategic interaction rather than position alone.

Offensive relevance considers:

- reachable gap within a pit window;
- relative settled pace;
- tyre-life difference;
- pit-cycle relationship;
- expected rejoin overlap;
- remaining laps;
- whether passing is realistically possible at the track.

Defensive relevance considers:

- gap behind;
- the rival's undercut potential;
- relative pace and closing rate;
- pit-cycle relationship;
- expected rejoin overlap;
- remaining laps.

### 9.2 Eligibility gates

A rival is ineligible when any of the following applies:

- inactive, retired, disqualified, or invalid result state;
- invalid position or gap;
- more than one lap phase away unless explicit unlapping logic is added later;
- outside the maximum relevant time window;
- already strategically resolved because the projected interaction cannot occur
  before the race ends;
- data is stale enough that tyre or pit-cycle comparison is unsafe.

### 9.3 Threat scoring

Threat score should become an internal selection aid rather than a public UI
number. Suggested normalized components:

- gap proximity;
- relative pace;
- closing trend;
- tyre offset;
- pit-cycle overlap;
- projected rejoin proximity;
- track pass difficulty;
- remaining opportunity laps;
- estimate quality penalty.

The public snapshot should report the facts that caused selection, not the
arbitrary combined score.

### 9.4 Rival switching

Keep hysteresis, but define it in strategic terms:

- retain the current rival while still eligible unless another candidate has a
  materially better projected interaction;
- switch immediately if the rival retires, becomes lapped, falls outside the
  race window, or can no longer affect the plan;
- avoid switching because of a transient pit-lane position swap;
- reevaluate immediately after a pit transition;
- log the reason code for a switch in diagnostics.

### 9.5 Multi-car groups

The main UI should remain focused, but the projector should understand a nearby
cluster rather than simulate only one rival. A pit rejoin can be ruined by a
third car even if the named target is clear. The scenario should therefore
include every active car within the projected rejoin window, while exposing
only the primary offensive and defensive rivals in the default UI.

## 10. Pace model

### 10.1 Accepted samples

Pace samples should exclude:

- lap 1 standing-start effects;
- in-laps and out-laps;
- pit-lane laps;
- SC/VSC and formation laps;
- invalid laps;
- laps with implausible time outliers;
- laps immediately following a compound transition until warm-up is accounted
  for;
- flashback-invalidated future history.

### 10.2 Robust base pace

Continue using recency weighting and median-based outlier rejection, but split
the estimate into components:

```text
predicted lap time
    = settled car/driver pace
    + fuel effect
    + compound delta
    + tyre-age degradation
    + warm-up effect
    + traffic effect
    + track-state/weather effect
```

The model need not perfectly identify every component immediately. The
important improvement is to stop baking unrelated effects into one number that
is then reused everywhere.

### 10.3 Fuel correction

Replace the fixed per-lap correction with a bounded session estimate:

- learn a broad fuel trend from clean player laps;
- regularize it toward a conservative track-independent default;
- require enough samples before trusting it;
- reset or lower quality after weather or major track-state changes;
- never allow fuel normalization to turn an obviously slower lap into a
  dominant pace sample.

### 10.4 Compound and tyre-age effect

Use available physical-set lap deltas as priors, then update from observed
stints where possible. Separate:

- new-set pace offset;
- warm-up cost on the out-lap and first full lap;
- roughly linear degradation region;
- accelerating degradation near the projected cliff.

### 10.5 Traffic effect

Track whether a lap was likely constrained by a car ahead. A first version can
use gap and closing behaviour instead of requiring detailed corner telemetry.
Traffic-affected laps may remain useful for race projection but should not define
clean-air car pace.

### 10.6 Rival pace uncertainty

Rival data may be restricted or sparse. Rival pace output should be unavailable
rather than zero when no reliable samples exist. Scenario scoring should widen
uncertainty and avoid aggressive calls when the decision depends on that pace.

## 11. Tyre model

### 11.1 Wear versus degradation

The engine must use precise language and separate concepts:

- tyre wear is the current percentage reported for each corner;
- wear rate is percentage points per lap;
- pace degradation is lap-time loss per lap;
- tyre life is the projected number of laps before a limit;
- the cliff is a modelled region of sharply increasing risk or pace loss.

One quantity should not silently stand in for another.

### 11.2 Per-corner wear

Retain recent completed-lap wear deltas per corner. The limiting tyre should be
the corner expected to reach the configured threshold first, not necessarily
the tyre with the highest current wear.

Live UI messages should remain factual:

- `Right tyres wearing faster`;
- `Front-left is the limiting tyre`;
- `Rear-left wear far above average`.

No inferred setup changes or driving instructions should be emitted.

### 11.3 Wear-rate filtering

Wear samples should:

- be tied to a completed lap and compound identity;
- reset on a tyre-set change or flashback;
- exclude neutralised laps where appropriate;
- reject negative or physically implausible deltas;
- weight recent laps more strongly;
- expose sample count and spread;
- fall back conservatively when fewer than two valid deltas exist.

### 11.4 Cliff projection

Replace a single deterministic cliff lap with an internal interval:

- earliest plausible cliff;
- expected cliff;
- latest plausible cliff.

The UI may still show one lap, but plan scoring should penalize candidates that
depend on surviving beyond the pessimistic bound. A critical warning should be
based on the limiting tyre and uncertainty, not average wear alone.

### 11.5 Physical tyre sets

Candidate plans must preserve set identity, not just compound identity. The
engine should track:

- fitted set;
- available unused sets;
- remaining usable life;
- set lap delta;
- whether the set was returned or unavailable;
- compound legality across already completed and projected stints.

If physical set data is unavailable, the engine may generate a compound-level
fallback plan, but it must mark legality and set availability as uncertain.

### 11.6 Wet tyres

Wet and intermediate strategy should use different assumptions from dry tyres:

- no dry-compound-change rule;
- weather-dependent usable life and pace;
- crossover and standing-water risk;
- warm-up and track-drying effects;
- readiness of the next suitable tyre category rather than only nominal wear.

## 12. Pit-loss and rejoin model

### 12.1 Base pit loss

Keep the track catalog as the cold-start prior. Split it explicitly into:

- pit entry/in-lap loss;
- stationary/slow-lane loss if observable;
- pit exit/out-lap loss;
- total green-flag loss.

### 12.2 Session calibration

When clean examples exist, calibrate the prior using actual stops in the loaded
session:

- compare a car's pre-stop and post-stop race-time trajectory;
- reject queued, penalized, damaged, SC/VSC, and severely traffic-affected stops;
- update conservatively with a capped correction;
- retain the catalog when sample quality is weak.

The model should never immediately replace a known track prior with one noisy AI
stop.

### 12.3 SC/VSC loss

The current product rule remains:

```text
effective pit loss = max(0, normal pit loss - 10 seconds)
```

This is an explicit temporary approximation. It should live in the pit-loss
model, not be duplicated across call logic. Later calibration may replace it
with track- and neutralisation-specific factors, but only after evidence exists.

### 12.4 Queue and double-stack cost

Estimate:

- generic congestion from the number of nearby cars pitting;
- teammate double-stack risk from team identity;
- whether the teammate is ahead and likely to receive service first;
- uncertainty when pit intentions are inferred rather than observed.

### 12.5 Rejoin projection

For every candidate stop, project:

- expected race-time loss;
- cars passed while the player pits;
- expected rejoin position;
- nearest car ahead and behind after rejoin;
- gap to those cars;
- whether the player rejoins into traffic;
- whether a rival's out-lap overlaps the player.

Use lap phase and total distance where reliable. Leader gaps alone are a
fallback and must be invalidated for lapped or zero-gap anomalies.

### 12.6 Traffic penalty

Apply a bounded expected cost when a candidate rejoins behind a slower group.
This should consider track passing difficulty and expected duration in traffic.
The first implementation may use a track catalog with broad categories:

- easy to pass;
- normal;
- difficult;
- extreme, such as Monaco.

## 13. Scenario generation

### 13.1 Candidate stop laps

Do not evaluate every remaining lap with every tyre permutation. Generate a
bounded set around meaningful points:

- current lap and next lap;
- planned window boundaries;
- expected and pessimistic tyre-cliff laps;
- rival pit lap and one/two laps after it;
- earliest lap on which a legal set can reach the finish;
- latest lap that preserves a compound change;
- weather crossover boundaries;
- SC/VSC deployment lap;
- laps where a projected rejoin clears or enters a traffic group.

Deduplicate and cap candidates deterministically.

### 13.2 Tyre sequences

For each stop-lap candidate:

- choose from actual available sets;
- reject duplicate physical-set use;
- verify usable life to the next stop or finish;
- enforce dry-compound rules;
- allow wet-category transitions independently;
- include one extra-stop alternative when it is plausibly competitive;
- preserve the current fitted set as a real option only when it remains usable.

### 13.3 Scenario categories

Generate neutral categories first:

- stay out to tyre limit;
- stop for best race time;
- stop to clear traffic;
- cover rival behind;
- undercut rival ahead;
- extend/overcut rival ahead;
- opportunistic SC/VSC stop;
- weather crossover stop;
- recovery plan after an unexpected stop.

Defensive and attacking plans are selected from these candidates by different
objectives; they are not generated by unrelated algorithms.

### 13.4 Projection horizon

Project to the race finish for legality and total race time. For close tactical
decisions, retain higher detail for the next few laps and lower detail farther
out. This keeps evaluation bounded without ignoring downstream stop costs.

## 14. Scenario scoring

### 14.1 Hard constraints

A scenario is invalid if it:

- cannot complete the race on available tyre life;
- violates known compound rules;
- requires a set that is not available;
- contains a stop before the action can physically occur;
- assumes an already matched rival stop is unmatched;
- relies on invalid track-position data;
- attempts a green-flag call during an incompatible race-control state;
- projects beyond the race finish;
- uses a retired or irrelevant rival as its tactical objective.

### 14.2 Score components

For valid scenarios, retain component costs in milliseconds or normalized
position risk:

- projected race time;
- pit loss;
- tyre degradation and cliff risk;
- warm-up cost;
- traffic cost;
- queue/double-stack cost;
- weather mismatch cost;
- defensive position-loss risk;
- offensive pass probability or expected gain;
- uncertainty penalty;
- legality fallback penalty when set-level data is unavailable.

### 14.3 Defensive objective

The defensive plan should prioritize:

1. legal race completion;
2. retaining position against the primary rival behind;
3. avoiding severe cliff risk;
4. minimizing expected race time among similarly safe options.

It should not simply pit one lap after a rival because a pit event occurred.

### 14.4 Attacking objective

The attacking plan should prioritize:

1. legal race completion;
2. creating a credible position-gain opportunity against the primary rival
   ahead;
3. minimizing projected race time;
4. accepting bounded tyre or traffic risk when the expected gain justifies it.

"Attacking" must not automatically mean one extra stop. If the best attacking
route is a longer current stint to create an offset, the model should choose it.

### 14.5 Baseline objective

Retain an internal neutral best-race-time plan. Even if the UI continues to show
only defensive and attacking columns, the baseline is necessary to measure how
much each tactical plan costs.

### 14.6 Decision margin

Every selected plan or call should have a margin over its nearest alternative.
A change should normally require:

- a minimum expected time improvement;
- a projected position improvement;
- removal of a hard risk;
- or a material event that invalidates the current plan.

This is more meaningful than publishing a generic confidence percentage.

## 15. Pit-call semantics

### 15.1 General call contract

Every call must define:

- action;
- target car, if any;
- issued lap and session time;
- earliest and latest action lap;
- preconditions;
- expiry condition;
- projected benefit;
- alternative action and cost;
- evidence quality;
- stable reason code.

Suggested actions:

- `box_now`;
- `cover`;
- `undercut`;
- `extend`;
- `overcut` as an extend strategy specifically against a rival who stopped;
- `stay_out`;
- `prepare_weather_tyre`;
- `no_call`.

### 15.2 Cover

Issue `cover` only when:

- the relevant rival is behind;
- the rival has started or completed an unmatched stop;
- the player remains on a viable older stint and can still pit;
- the projected undercut risk exceeds the cost of responding;
- the player's rejoin and tyre-set path remain valid.

Expire when:

- the player enters the pits;
- the rival stop is matched;
- the response window closes;
- the rival ceases to be a threat;
- SC/VSC or weather supersedes the call.

### 15.3 Undercut

Issue `undercut` only when:

- the relevant rival is ahead and has not already made the stop being attacked;
- the player has a usable fresh set;
- the player is not already on a fresh/out-lap state;
- fresh-tyre gain plus traffic benefit can recover the pit gap within the
  response window;
- the projected rejoin is usable;
- the rival is expected to remain out long enough for the crossover.

An undercut is not a label for a stop the player already completed.

### 15.4 Overcut

Issue `overcut` only when:

- the relevant rival is ahead before the cycle or is the car whose position the
  player is attempting to gain;
- the rival has started or completed an unmatched stop;
- the player has not matched that stop;
- the player's current tyres remain viable for the extension;
- staying out creates a projected race-time or position advantage after the
  player's later stop;
- the call has a defined latest safe lap.

If both cars have completed the same stop and both are on fresh tyres, an
overcut is impossible and must not be shown.

### 15.5 Extend against an undercut attempt

When a rival behind pits first, the player may either cover or extend. This
choice should come from comparing both scenarios. Do not call the extension an
overcut merely because the rival is behind or on a different tyre age. The call
should explain the actual decision: `EXTEND` with the safe lap limit and the
projected reason it beats covering.

### 15.6 Call lifecycle

Internal lifecycle:

```text
Candidate -> Issued -> Active -> Followed | Expired | Superseded | Invalidated
```

The live snapshot should normally expose only the active call. Lifecycle data
may be retained for diagnostics and future evaluation, but should not recreate
the removed decision-history sidebar.

## 16. SC/VSC strategy

### 16.1 Trigger

Recompute immediately on the transition into SC or VSC. Do not wait for a lap
boundary. Freeze a clean position/gap basis at deployment so subsequent pit-lane
ordering does not corrupt the comparison.

### 16.2 Options

At minimum compare:

- box now;
- stay out and follow the existing plan;
- stay out and box on a later neutralised lap if still plausible;
- box now with queue/double-stack cost;
- stay out because no legal or useful set is available.

### 16.3 Projection

Use:

- normal pit loss minus ten seconds;
- deployment-time gaps;
- projected rejoin position;
- tyre gain over remaining laps;
- queue and teammate cost;
- compound legality;
- current and alternative tyre life;
- laps remaining;
- field compression uncertainty.

### 16.4 Stability

Once issued, avoid switching `BOX NOW` to `STAY OUT` because of pit-lane gaps
created by the decision itself. Recompute only on material new evidence such as
queue formation, the player passing pit entry, a tyre-set availability change,
or a new race-control state.

### 16.5 Exit

Clear the neutralisation call when:

- SC/VSC ends;
- the player completes the recommended stop;
- pit entry has passed and the current-lap action is impossible;
- a red flag supersedes the decision.

## 17. Weather strategy

### 17.1 Forecast and observed state

Separate:

- current track/weather state;
- forecast samples and forecast accuracy;
- tyre-category suitability;
- predicted crossover lap;
- uncertainty window around crossover;
- observed evidence that the transition has started.

### 17.2 Candidate crossover

Convert forecast minutes to laps using a pace appropriate to the expected track
state. A simple current dry-lap conversion is acceptable as a fallback but
should carry wider uncertainty.

### 17.3 Hysteresis

Weather calls should require a sustained or sufficiently strong signal. Avoid
switching between slicks and intermediates on every forecast sample. Use
separate enter and exit thresholds where possible.

### 17.4 UI rule

Do not show a permanent `Stay Dry` card when no transition is forecast. Show a
weather section only when the user has a decision to prepare for, or when the
current tyre category is becoming unsuitable.

### 17.5 Interaction with SC/VSC

Generate combined scenarios. A neutralised stop for a weather transition may
dominate both ordinary weather timing and ordinary SC timing; it should produce
one coherent call, not competing cards.

## 18. Special cases

### 18.1 Monaco and other difficult-passing tracks

Replace one-off strategy forcing with track characteristics:

- passing difficulty;
- traffic sensitivity;
- undercut strength;
- pit-loss prior;
- safety-car likelihood only if a defensible data source is later added.

Monaco may still need explicit product rules, but they should be represented as
track-model inputs rather than scattered conditionals.

### 18.2 Short races and sprints

Candidate generation must adapt when there is insufficient race distance for a
normal compound sequence. Do not force extra stops merely to make attacking and
defensive plans visually different.

### 18.3 Red flags

Treat red flag as a separate race-control state. Clear active green-flag and
SC/VSC calls, retain pre-red-flag evidence for diagnostics, and rebuild the
forward plan once valid restart and tyre information arrives.

### 18.4 Formation lap and race start

Suppress pace and pit calls until the race is active and enough stable state is
available. Lap 1 remains excluded from settled pace.

### 18.5 Flashbacks

All state after the rewind point must be discarded: pit transitions, wear
samples, pace samples, rival switches, active calls, and scenario decisions.
Playback and live flashback handling should converge on the same reducer state.

### 18.6 Mid-session startup

When the app begins receiving data mid-race:

- initialize current state immediately;
- mark unknown history explicitly;
- avoid inferring a recent pit solely from low tyre age;
- allow a baseline plan;
- delay tactical calls until pit-cycle and pace evidence are sufficient.

### 18.7 Restricted rival telemetry

Fall back from tyre-sensitive tactics when all-car status is unavailable or
stale. Gap- and pit-count-based defence may remain possible, but the UI should
not claim a rival tyre advantage it cannot observe.

## 19. Snapshot and API design

### 19.1 Public snapshot principles

The snapshot should be a renderer contract, not a dump of internal state. It
should contain:

- current race/tyre summary;
- defensive and attacking plans;
- one active immediate call;
- optional neutralisation decision;
- optional weather decision;
- primary race-battle facts;
- tyre-condition facts;
- data-quality flags needed to render unavailable states.

### 19.2 Fields to reconsider

After the replacement model is established:

- remove `explanation` from the public live snapshot;
- remove full `positions` from the strategy snapshot;
- remove public `decision_history` unless a separate analysis surface needs it;
- remove public `threat_score` and keep it internal;
- replace generic snapshot `confidence` with per-decision quality/margin;
- replace free-form reason prose with stable reason codes plus concise factual
  fields;
- retain detailed internal evidence behind an optional diagnostic interface.

These fields are already hidden from the main Electron and Qt sidebars, so
schema cleanup can be staged without changing the intended UI.

### 19.3 Proposed plan summary

Each plan should expose a concise summary independently of its stint rows:

```cpp
struct StrategyPlanSummary {
    int next_stop_lap{};
    int latest_safe_stop_lap{};
    int projected_finish_position{};
    double projected_race_time_delta_ms{};
    double risk_ms{};
    std::string objective;
    std::string primary_reason;
};
```

Exact names can change, but the UI should not have to derive the next stop by
searching stint end laps.

### 19.4 Proposed active call

```cpp
struct StrategyActiveCall {
    uint64_t id{};
    std::string action;
    int issued_lap{};
    int earliest_lap{};
    int latest_lap{};
    int target_idx{-1};
    std::string target_name;
    double projected_gain_ms{};
    int projected_position_delta{};
    std::string reason;
    std::string quality;
};
```

The identifier should be deterministic from session state, not randomly
generated.

### 19.5 Model version

Include an internal or diagnostic `strategy_model_version`. Because strategy is
derived rather than recorded, an application update may intentionally produce a
better strategy for an old `.tnrd`. The version makes comparisons and bug
reports understandable without changing the recording format.

## 20. UI design direction

### 20.1 Default live view

Keep the current three-column strategy composition, but make the sidebar
strictly action-first and flat:

- exceptional decision;
- race call;
- next pit windows;
- weather only when relevant;
- race battle;
- tyre condition.

Avoid nested cards, decorative elements that do not encode state, generic model
copy, and full field tables.

### 20.2 Plan columns

The plan columns should answer:

- how many stops remain;
- what tyre sequence is planned;
- when each stop occurs;
- what lap time is required;
- how actual laps compare with the real target;
- whether the plan is legal and physically possible.

Potential follow-up improvement: collapse completed stints by default and keep
the current/future stint visible. This would reduce scrolling late in a race
without deleting useful history.

### 20.3 Lap-target table

Keep:

- Lap;
- Required;
- Actual;
- Delta Lap;
- Delta Stint;
- Delta Total.

Do not reintroduce Adjusted Required. If space becomes constrained, move one of
the cumulative deltas into an optional expanded view rather than inventing a
new target.

### 20.4 Call presentation

A call should render the action prominently and show only facts needed to act:

```text
COVER ANTONELLI                 BOX L18
Rejoin P6, 0.7s ahead           +1.4s vs extend
```

The exact layout may differ, but it should not show internal reason identifiers
with underscores as user copy.

### 20.5 Empty and unavailable states

- No active call: omit the section.
- No weather crossover: omit the weather section.
- No valid rival: show tyre-life baseline in the plan header, not a fake rival.
- Weak rival tyre data: omit tyre age or mark it unavailable.
- No legal tyre path: show a concise blocking state and preserve the best
  fallback plan for inspection.

### 20.6 Electron and Qt parity

Both apps must render the same semantic hierarchy and actions. Visual details
may follow their native systems, but a user should not receive a different pit
call because of frontend choice. No strategy arithmetic should be added to
React or Qt widgets.

## 21. Playback and seeking

### 21.1 Deterministic reconstruction

Preserve the current strategy replay design:

- strategy remains unrecorded;
- the reader replays only the strategy dependency families;
- rows are consumed in chronological order;
- completed-lap checkpoints accelerate later seeks;
- the rebuilt processor is atomically committed with the winning seek request.

### 21.2 New-state compatibility

Any new internal state must be:

- copyable with `StrategyProcessor` checkpoints;
- entirely derivable from recorded normalized rows;
- reset correctly on session restart and flashback;
- independent of UI observation order;
- unaffected by playback speed.

### 21.3 Live/seek parity invariant

For any recorded time `t`:

```text
snapshot produced by sequential playback to t
    == snapshot produced by strategySnapshotAt(t)
```

Equality may ignore ephemeral delivery timestamps but must include plan, active
call, rivals, tyre model, and race-control decision.

### 21.4 Checkpoint policy

Completed-lap checkpoints remain the default. If event-heavy state makes
reconstruction expensive, optional checkpoints may be added at major state
transitions, but they must remain bounded and invalidated when the loaded file
changes.

## 22. Performance model

### 22.1 Ingest path

Per-row ingest should remain approximately O(number of cars) for timing and
all-car status, and O(1) for player-only rows. It should update state and mark
which estimators or decisions are dirty.

### 22.2 Evaluation triggers

Full scenario evaluation occurs on:

- completed lap;
- rival pit transition;
- player pit transition;
- rival change;
- SC/VSC deployment or end;
- meaningful weather crossover change;
- tyre-set availability or compound change;
- tyre cliff entering a critical horizon;
- retirement of a relevant rival;
- explicit snapshot request after seek reconstruction.

Ordinary repeated status packets should not rebuild a full race search if no
material state changed.

### 22.3 Bounded search

Set deterministic caps for:

- candidate stop laps;
- tyre-sequence depth;
- nearby cars included in detailed traffic projection;
- pace samples per car;
- wear samples;
- retained diagnostic decisions;
- strategy checkpoints.

### 22.4 Snapshot cost

After refactoring, `snapshot()` should mostly serialize already evaluated state.
It should not be the only place where every estimator and scenario is rebuilt.
This separation will make emit frequency safer and simplify testing.

## 23. Diagnostics and observability

### 23.1 Structured decision evidence

For development builds or an explicit diagnostic export, retain:

- evaluation trigger;
- input freshness;
- selected rivals and rejection reasons;
- generated candidate count;
- invalid candidate reasons;
- score components for top candidates;
- previous versus new plan margin;
- active-call transition and expiry reason;
- model version.

### 23.2 No default debug prose

Diagnostics should not return as a permanent `Strategy Model` card. They are for
bug reports, replay analysis, and development.

### 23.3 Strategy trace export

A future diagnostic action could export a compact JSON trace from a `.tnrd`
without changing the recording. This would allow strategy bugs to be reproduced
with the exact model version and decision evidence.

## 24. Validation strategy

No validation work is part of writing this document, but implementation should
be verified at several levels.

### 24.1 Reducer transition tests

Cover:

- pit entry, stop-count increment, pit exit, out-lap, and settled state;
- player and rival stopping together;
- rival stopping first and player responding;
- compound change without a reliable pit-status transition;
- mid-session startup;
- retirement and rival switch;
- SC/VSC start and end;
- flashback reset.

### 24.2 Call invariant tests

Examples:

- no overcut when both cars completed the same stop;
- no undercut when the player is already on fresh tyres from the relevant stop;
- no cover when the rival stop is already matched;
- no green-flag call during SC/VSC;
- no call against an ineligible or lapped rival;
- every active call has a non-expired action window;
- every call points to a valid scenario and alternative.

### 24.3 Scenario tests

Create small deterministic race states for:

- one-stop versus two-stop crossover;
- defensive cover versus extend;
- attacking undercut versus overcut;
- traffic-sensitive rejoin;
- insufficient tyre-set life;
- dry-compound legality;
- wet crossover;
- Monaco passing difficulty;
- SC/VSC queue and double-stack.

### 24.4 Property tests

Useful invariants:

- stint ranges never overlap or leave an unexplained race-distance gap;
- a legal plan finishes the race;
- one physical tyre set is never used twice;
- stop laps are monotonically increasing;
- projected tyre age never decreases without a stop;
- a completed matched stop cannot remain an active unmatched-stop call;
- projected position stays within the active-car count;
- confidence/quality cannot improve when required input freshness worsens.

### 24.5 Replay golden cases

Maintain representative `.tnrd` scenarios and expected decision transitions,
not pixel screenshots. Include dry races, wet races, safety cars, multi-stop
races, late app startup, flashbacks, and races with lapped cars.

### 24.6 Seek parity

For selected times throughout each fixture, compare sequential processing with
direct seek reconstruction and a repeated seek in non-monotonic order.

### 24.7 Frontend contract checks

Both frontends should be checked against snapshots containing:

- no rival;
- one or two rivals;
- no active call;
- each call type;
- SC/VSC decision;
- weather crossover;
- illegal plan;
- missing tyre data;
- long driver names;
- light and dark themes.

## 25. Implementation phases

### Phase 0: Preserve current behaviour while defining invariants

Purpose: stop known invalid calls before deeper refactoring.

Work:

- centralize player and rival actionability checks;
- formalize matched versus unmatched stop rules;
- remove tyre-age-only pit-cycle calls;
- ensure recent-stop logic affects plans only while the stop is unmatched;
- list all current reason codes and their expiry conditions;
- document current track pit-loss catalog coverage.

Acceptance criteria:

- both-fresh matched-stop cases never show undercut/overcut/cover;
- no call targets an ineligible rival;
- a call disappears immediately when its action is no longer possible;
- Electron and Qt receive the same snapshot.

### Phase 1: Race-state and pit-cycle reducer

Purpose: separate telemetry interpretation from decision logic.

Work:

- add explicit `CarRaceState` and `PitCycleState`;
- track transition evidence and quality;
- derive pairwise pit-cycle relationship;
- move rival experience into the new state model;
- expose dirty/event triggers;
- preserve copyability and seek reconstruction.

Acceptance criteria:

- all pit transition sequences have deterministic state;
- mid-session startup remains safe;
- flashback removes future transitions;
- no strategy decision reads raw pit booleans directly when a derived state
  exists.

### Phase 2: Estimator separation

Purpose: produce reusable pace, tyre, pit-loss, and rejoin estimates.

Work:

- extract pace sampling and uncertainty;
- extract per-corner tyre model;
- centralize pit-loss calculation and SC/VSC discount;
- create rejoin projection with validity gates;
- attach input quality to estimates.

Acceptance criteria:

- every scenario cost comes from a named estimator;
- unavailable rival pace is not represented as a real zero;
- tyre wear, wear rate, and pace degradation are separate quantities;
- rejoin projections reject invalid gap data.

### Phase 3: Common scenario engine

Purpose: generate defensive, attacking, and baseline plans from one candidate
set.

Work:

- bounded stop-lap generation;
- physical tyre-sequence generation;
- finish-to-race projection;
- hard validity filter;
- objective-specific scoring;
- plan stability margin.

Acceptance criteria:

- defensive and attacking plans differ only when objectives justify it;
- attacking does not automatically add a stop;
- every plan is legal or explicitly marked as a fallback;
- plan changes have a traceable scenario margin.

### Phase 4: Decision state machine

Purpose: turn scenario comparisons into stable actionable calls.

Work:

- implement call contract and lifecycle;
- compare cover versus extend;
- compare undercut versus overcut;
- define action windows and expiry;
- prevent repeated duplicate calls;
- retain bounded diagnostic history.

Acceptance criteria:

- every call has an alternative and projected margin;
- expired calls are never serialized as active;
- call transitions remain identical in live and seek playback;
- no call is inferred from tyre age alone.

### Phase 5: Neutralisation and weather integration

Purpose: make exceptional conditions use the same scenario framework.

Work:

- represent race-control state explicitly;
- generate combined neutralisation scenarios;
- stabilize deployment-gap basis;
- integrate queue and double-stack estimates;
- generate weather-category scenarios with hysteresis;
- resolve combined SC/weather opportunities as one decision.

Acceptance criteria:

- SC/VSC triggers evaluation immediately;
- temporary ten-second discount is applied exactly once;
- pit-lane ordering after deployment does not flip the original comparison;
- no permanent weather card appears without a crossover decision.

### Phase 6: Public snapshot cleanup and UI refinement

Purpose: align the data contract with the action-first product.

Work:

- add plan summaries and active-call action window;
- remove obsolete public model/debug fields;
- keep diagnostics behind an explicit path;
- make reason presentation concise and factual;
- mirror semantic changes in Electron and Qt;
- consider collapsing completed stints.

Acceptance criteria:

- UIs perform no strategy arithmetic;
- no internal score or model explanation occupies default live space;
- all visible fields directly support an action or race understanding;
- both frontends handle every snapshot state consistently.

### Phase 7: Calibration and regression corpus

Purpose: tune behaviour using real races without sacrificing determinism.

Work:

- collect representative recorded scenarios;
- compare projected and actual pit losses;
- tune bounded coefficients and thresholds;
- create decision-transition golden data;
- document known uncertainty and unsupported cases.

Acceptance criteria:

- changes can be evaluated against the same race corpus;
- model-version changes are visible in diagnostics;
- calibration never depends on external network data at runtime;
- old recordings remain loadable and derive strategy with the current model.

## 26. Recommended immediate sequence

The next implementation work should proceed in this order:

1. Introduce explicit pit-cycle and matched-stop state.
2. Move all cover/undercut/overcut preconditions onto that state.
3. Extract reusable pace, tyre, pit-loss, and rejoin estimators.
4. Add a neutral baseline scenario and common candidate generator.
5. Score defensive and attacking plans from that common candidate set.
6. Add call lifecycle, action windows, and expiry.
7. Fold SC/VSC and weather into the scenario system.
8. Clean the public snapshot after the replacement fields are stable.
9. Refine the two UIs around the final semantic contract.
10. Build a replay regression corpus and tune only against reproducible cases.

The first major milestone should not be a new card or another isolated rule. It
should be the point at which pit-cycle state is explicit enough that an invalid
call is structurally impossible.

## 27. Decisions already made

- Strategy remains in shared C++.
- Strategy remains derived rather than recorded.
- Both frontends consume the same semantic snapshot.
- Plans are recalculated throughout the race.
- Rival selection is dynamic.
- SC/VSC causes immediate evaluation.
- The temporary SC/VSC pit-loss discount is ten seconds.
- Adjusted Required remains removed.
- Default tyre messages remain factual and terse.
- Full position, model-explanation, and decision-history cards remain absent
  from the live sidebar.
- Seeking reconstructs strategy from the beginning/checkpoint to the selected
  time and atomically commits the rebuilt processor.

## 28. Open design decisions

These should be resolved during implementation with replay evidence:

- exact candidate-stop-lap cap;
- initial per-track passing-difficulty catalog values;
- minimum time or position margin needed to change a plan;
- how many settled laps are required before rival pace becomes usable;
- whether tyre-cliff uncertainty is exposed directly in the UI;
- whether the neutral baseline plan should eventually be user-visible;
- whether completed stints collapse automatically or through a preference;
- the first safe method for session pit-loss calibration;
- how restricted rival telemetry should be signalled in the race-battle row;
- whether diagnostic strategy traces belong in the existing export flow or a
  separate developer action.

## 29. Definition of done

The strategy redesign is complete when all of the following are true:

- race state and pit-cycle state are explicit;
- every plan is generated and scored as a scenario;
- every call is actionable, time-bounded, and linked to a projected alternative;
- defensive and attacking plans use one coherent model;
- rival selection reflects projected strategic interaction;
- tyre, pace, pit loss, traffic, weather, and neutralisation estimates expose
  their quality;
- no UI performs hidden strategy calculations;
- live, playback, and seek results are deterministic and equivalent;
- invalid gaps, stale fields, lapped cars, matched stops, and fresh-tyre states
  cannot produce fake tactical calls;
- the main UI remains concise even though diagnostic evidence is rich;
- behaviour changes can be evaluated on a reproducible recorded-race corpus;
- the system can explain every recommendation in structured facts without
  falling back to vague model prose.

