#include "tnrp/Labels.h"

#include <map>
#include <mutex>

#include <glaze/glaze.hpp>

namespace tnrp {

std::string LabelCatalog::get(std::string_view key) const {
    auto it = map_.find(std::string(key));
    return it == map_.end() ? std::string(key) : it->second;
}

// ── Base layer ──────────────────────────────────────────────────────────────
// Format-neutral defaults (match the strings the frontends shipped for 2025, so
// the 2025/2024 catalogs reproduce today's UI verbatim). Per-format overrides
// below patch only the keys whose meaning changes by game year.
static std::unordered_map<std::string, std::string> baseLayer() {
    return {
        // Tyre — actual compound (keyed on actual_compound)
        {"tyre.actual.7", "INT"}, {"tyre.actual.8", "WET"},
        {"tyre.actual.16", "C5"}, {"tyre.actual.17", "C4"}, {"tyre.actual.18", "C3"},
        {"tyre.actual.19", "C2"}, {"tyre.actual.20", "C1"}, {"tyre.actual.21", "C0"},
        {"tyre.actual.22", "C6"},
        // Tyre — visual compound (keyed on visual_compound)
        {"tyre.visual.7", "INT"}, {"tyre.visual.8", "WET"},
        {"tyre.visual.16", "Soft"}, {"tyre.visual.17", "Medium"}, {"tyre.visual.18", "Hard"},

        // ERS deploy mode (Car Status m_ersDeployMode). Index 3 is overridden
        // for 2026 (Overtake → Boost).
        {"ers.mode.0", "None"}, {"ers.mode.1", "Auto"},
        {"ers.mode.2", "Hotlap"}, {"ers.mode.3", "Overtake"},

        // Overtake/aero concept. 2026 surfaces the active-aero "straight line"
        // mode where 2025 has DRS; the label is overridden per format.
        {"drs.label", "DRS"},
        {"boost.label", "Overtake"},
        {"aero.mode.0", "Corner"}, {"aero.mode.1", "Straight"},

        // Format-aware data key for the overview "wing" card: 2025 reads the `drs`
        // telemetry field, 2026 reads the dedicated `slm` field (see overrides).
        {"card.wing.key", "drs"},

        // Fuel mix
        {"fuel.mix.0", "Lean"}, {"fuel.mix.1", "Standard"},
        {"fuel.mix.2", "Rich"}, {"fuel.mix.3", "Max"},

        // Traction control / ABS
        {"tc.0", "Off"}, {"tc.1", "Medium"}, {"tc.2", "Full"},
        {"abs.0", "Off"}, {"abs.1", "On"},

        // FIA flags (vehicle / marshal zone). -1 keyed as "invalid".
        {"flag.invalid", "—"}, {"flag.0", "None"}, {"flag.1", "Green"},
        {"flag.2", "Blue"}, {"flag.3", "Yellow"}, {"flag.4", "Red"},

        // Weather
        {"weather.0", "Clear"}, {"weather.1", "Light Cloud"}, {"weather.2", "Overcast"},
        {"weather.3", "Light Rain"}, {"weather.4", "Heavy Rain"}, {"weather.5", "Storm"},

        // Session types
        {"session.0", "Unknown"}, {"session.1", "Practice 1"}, {"session.2", "Practice 2"},
        {"session.3", "Practice 3"}, {"session.4", "Short Practice"},
        {"session.5", "Qualifying 1"}, {"session.6", "Qualifying 2"}, {"session.7", "Qualifying 3"},
        {"session.8", "Short Qualifying"}, {"session.9", "One-Shot Qualifying"},
        {"session.10", "Sprint Shootout 1"}, {"session.11", "Sprint Shootout 2"},
        {"session.12", "Sprint Shootout 3"}, {"session.13", "Short Sprint Shootout"},
        {"session.14", "One-Shot Sprint Shootout"}, {"session.15", "Race"},
        {"session.16", "Race 2"}, {"session.17", "Race 3"}, {"session.18", "Time Trial"},

        // Formula (Session m_formula). 13 = F1 26 (new in 2026 appendix).
        {"formula.0", "F1 Modern"}, {"formula.1", "F1 Classic"}, {"formula.2", "F2"},
        {"formula.3", "F1 Generic"}, {"formula.4", "Beta"}, {"formula.6", "Esports"},
        {"formula.8", "F1 World"}, {"formula.9", "F1 Elimination"}, {"formula.13", "F1 26"},

        // Ruleset
        {"rules.0", "Practice & Qualifying"}, {"rules.1", "Race"},
        {"rules.2", "Time Trial"}, {"rules.12", "Elimination"},

        // Surface types
        {"surface.0", "Tarmac"}, {"surface.1", "Rumble strip"}, {"surface.2", "Concrete"},
        {"surface.3", "Rock"}, {"surface.4", "Gravel"}, {"surface.5", "Mud"},
        {"surface.6", "Sand"}, {"surface.7", "Grass"}, {"surface.8", "Water"},
        {"surface.9", "Cobblestone"}, {"surface.10", "Metal"}, {"surface.11", "Ridged"},

        // Result status (lap data / final classification)
        {"result.0", "Invalid"}, {"result.1", "Inactive"}, {"result.2", "Active"},
        {"result.3", "Finished"}, {"result.4", "Did not finish"}, {"result.5", "Disqualified"},
        {"result.6", "Not classified"}, {"result.7", "Retired"},
        {"result.short.4", "DNF"}, {"result.short.5", "DSQ"}, {"result.short.7", "RET"},

        // Safety car (event + session)
        {"sc.status.0", "No Safety Car"}, {"sc.status.1", "Safety Car"},
        {"sc.status.2", "Virtual Safety Car"}, {"sc.status.3", "Formation Lap"},
        {"sc.type.0", "No Safety Car"}, {"sc.type.1", "Safety Car"},
        {"sc.type.2", "Virtual Safety Car"}, {"sc.type.3", "Formation Lap"},
        {"sc.event.0", "Deployed"}, {"sc.event.1", "Returning"},
        {"sc.event.2", "Returned"}, {"sc.event.3", "Resume Race"},

        // Event string codes → display names (+ short forms used by the recorder)
        {"event.SSTA", "Session Start"}, {"event.SEND", "Session End"},
        {"event.FTLP", "Fastest Lap"}, {"event.RTMT", "Retirement"},
        {"event.DRSE", "DRS Enabled"}, {"event.DRSD", "DRS Disabled"},
        {"event.TMPT", "Team Mate in Pits"}, {"event.CHQF", "Chequered Flag"},
        {"event.RCWN", "Race Winner"}, {"event.PENA", "Penalty"},
        {"event.SPTP", "Speed Trap"}, {"event.STLG", "Start Lights"},
        {"event.LGOT", "Lights Out"}, {"event.DTSV", "DT Served"},
        {"event.SGSV", "SG Served"}, {"event.FLBK", "Flashback"},
        {"event.BUTN", "Button"}, {"event.RDFL", "Red Flag"},
        {"event.OVTK", "Overtake"}, {"event.SCAR", "Safety Car"},
        {"event.COLL", "Collision"},

        // Penalty types
        {"penalty.0", "Drive Through"}, {"penalty.1", "Stop-Go"}, {"penalty.2", "Grid Penalty"},
        {"penalty.3", "Penalty Reminder"}, {"penalty.4", "Time Penalty"}, {"penalty.5", "Warning"},
        {"penalty.6", "Disqualified"}, {"penalty.7", "Removed From Formation Lap"},
        {"penalty.8", "Parked Too Long Timer"}, {"penalty.9", "Tyre Regulations"},
        {"penalty.10", "This Lap Invalidated"}, {"penalty.11", "This & Next Lap Invalidated"},
        {"penalty.12", "This Lap Invalidated (No Reason)"},
        {"penalty.13", "This & Next Lap Invalidated (No Reason)"},
        {"penalty.14", "This & Previous Lap Invalidated"},
        {"penalty.15", "This & Previous Lap Invalidated (No Reason)"},
        {"penalty.16", "Retired"}, {"penalty.17", "Black Flag Timer"},

        // Infringement types (canonical wording; reconciles renderer/recorder)
        {"infringe.0", "Blocking by slow driving"}, {"infringe.1", "Blocking by wrong way driving"},
        {"infringe.2", "Reversing off the start line"}, {"infringe.3", "Severe collision"},
        {"infringe.4", "Collision"},
        {"infringe.5", "Collision failed to hand back position (single)"},
        {"infringe.6", "Collision failed to hand back position (multiple)"},
        {"infringe.7", "Corner cutting gained time"}, {"infringe.8", "Corner cutting overtake (single)"},
        {"infringe.9", "Corner cutting overtake (multiple)"}, {"infringe.10", "Crossed pit exit lane"},
        {"infringe.11", "Ignoring blue flags"}, {"infringe.12", "Ignoring yellow flags"},
        {"infringe.13", "Ignoring drive through"}, {"infringe.14", "Too many drive throughs"},
        {"infringe.15", "Drive through reminder (serve within n laps)"},
        {"infringe.16", "Drive through reminder (serve this lap)"}, {"infringe.17", "Pit lane speeding"},
        {"infringe.18", "Parked for too long"}, {"infringe.19", "Ignoring tyre regulations"},
        {"infringe.20", "Too many penalties"}, {"infringe.21", "Multiple warnings"},
        {"infringe.22", "Approaching disqualification"}, {"infringe.23", "Tyre regulations (select single)"},
        {"infringe.24", "Tyre regulations (select multiple)"},
        {"infringe.25", "Lap invalidated (corner cutting)"}, {"infringe.26", "Lap invalidated (running wide)"},
        {"infringe.27", "Corner cutting ran wide, gained time (minor)"},
        {"infringe.28", "Corner cutting ran wide, gained time (significant)"},
        {"infringe.29", "Corner cutting ran wide, gained time (extreme)"},
        {"infringe.30", "Lap invalidated (wall riding)"}, {"infringe.31", "Lap invalidated (flashback used)"},
        {"infringe.32", "Lap invalidated (reset to track)"}, {"infringe.33", "Blocking the pit lane"},
        {"infringe.34", "Jump start"}, {"infringe.35", "Safety car to car collision"},
        {"infringe.36", "Safety car illegal overtake"}, {"infringe.37", "Safety car exceeding allowed pace"},
        {"infringe.38", "Virtual safety car exceeding allowed pace"},
        {"infringe.39", "Formation lap below allowed speed"}, {"infringe.40", "Formation lap parking"},
        {"infringe.41", "Retired (mechanical failure)"}, {"infringe.42", "Retired (terminally damaged)"},
        {"infringe.43", "Safety car falling too far back"}, {"infringe.44", "Black flag timer"},
        {"infringe.45", "Unserved stop go penalty"}, {"infringe.46", "Unserved drive through penalty"},
        {"infringe.47", "Engine component change"}, {"infringe.48", "Gearbox change"},
        {"infringe.49", "Parc Fermé change"}, {"infringe.50", "League grid penalty"},
        {"infringe.51", "Retry penalty"}, {"infringe.52", "Illegal time gain"},
        {"infringe.53", "Mandatory pitstop"}, {"infringe.54", "Attribute assigned"},

        // ── Static UI chrome (ui.*) — extended incrementally per component ──
        {"ui.chart.speedRpmErs", "Speed / RPM / ERS Chart"},
        {"ui.tyre.wear", "TYRE WEAR"}, {"ui.tyre.life", "TYRE LIFE"},
        {"ui.ers.store", "ERS STORE"}, {"ui.ers.pct", "ERS %"},
        {"ui.ers.harvestThisLap", "ERS HARVEST THIS LAP"}, {"ui.ers.storeHistory", "ERS STORE HISTORY"},
        {"ui.overview.speed", "Speed"}, {"ui.overview.rpm", "RPM"}, {"ui.overview.gear", "Gear"},
        {"ui.overview.throttle", "Throttle"}, {"ui.overview.brake", "Brake"}, {"ui.overview.drs", "DRS"},
        {"ui.overview.engine", "Engine"}, {"ui.overview.ers", "ERS"}, {"ui.overview.fuel", "Fuel"},
        {"ui.overview.pos", "Pos"}, {"ui.overview.tyre", "Tyre"},

        // Units
        {"unit.kph", "km/h"}, {"unit.mph", "mph"}, {"unit.c", "°C"}, {"unit.f", "°F"},
        {"unit.mj", "MJ"}, {"unit.kj", "kJ"}, {"unit.pct", "%"}, {"unit.deg", "°"},
        {"unit.s", "s"}, {"unit.ms", "ms"}, {"unit.kg", "kg"}, {"unit.psi", "PSI"},
    };
}

// ── Per-format override layers ───────────────────────────────────────────────
static std::unordered_map<std::string, std::string> overrideLayer(uint16_t format) {
    if (format >= 2026) {
        return {
            // Track display-name overrides. Both UIs read defaults from their
            // bundled map JSON and consult only these format-specific deltas.
            // `track_name` and `circuit_name` can be overridden independently.
            {"track.4.track_name", "Barcelona-Catalunya Grand Prix"},
            {"track.42.track_name", "Spanish Grand Prix"},
            // ERS deploy mode 3: "Overtake" → "Boost" (2026 spec, Car Status).
            {"ers.mode.3", "Boost"},
            // 2026 active-aero / overtake concept replaces the DRS framing.
            {"drs.label", "Straight Line Mode"},
            {"boost.label", "Boost"},
            {"event.DRSE", "Straight Line Mode Enabled"},
            {"event.DRSD", "Straight Line Mode Disabled"},
            {"ui.overview.drs", "SLM"},
            // Wing card reads the dedicated active-aero field under 2026.
            {"card.wing.key", "slm"},
        };
    }
    return {};
}

const LabelCatalog& labelsFor(uint16_t format) {
    static std::mutex mtx;
    static std::map<uint16_t, LabelCatalog> cache;
    std::lock_guard<std::mutex> lk(mtx);
    auto it = cache.find(format);
    if (it != cache.end()) return it->second;

    LabelCatalog cat;
    cat.map_ = baseLayer();
    for (auto& [k, v] : overrideLayer(format)) cat.map_[k] = v;
    return cache.emplace(format, std::move(cat)).first->second;
}

std::string labelsJson(uint16_t format) {
    // Serialise to a stable, sorted JSON object (std::map for determinism).
    std::map<std::string, std::string> sorted(labelsFor(format).all().begin(),
                                               labelsFor(format).all().end());
    std::string out;
    (void)glz::write_json(sorted, out);
    return out;
}

} // namespace tnrp
