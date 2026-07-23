#include "tnrp/CardColors.h"

namespace tnrp {

// Helper to keep the table terse.
static ColorRule R(const char* on, const char* op, double v, const char* color) {
    return ColorRule{ on, op, v, color };
}

const std::map<std::string, ColorSpec>& cardColors() {
    static const std::map<std::string, ColorSpec> specs = {
        // ── Overview stat cards ──────────────────────────────────────────────
        {"speed",    { "pos",     {} }},
        {"rpm",      { "neg",     {} }},
        {"throttle", { "pos",     {} }},
        {"gear",     { "neg",     { R("self","lte",2,"info"), R("self","lte",4,"mguk"),
                                     R("self","lte",6,"warnAlt") } }},
        {"brake",    { "neutral", { R("brake","gt",0.05,"neg") } }},
        // Wing card (drs in 2025 / slm in 2026): ON ⇒ green, OFF ⇒ gray.
        {"wing",     { "off",     { R("self","eq",1,"pos") } }},
        {"engine",   { "neutral", { R("self","gt",112,"neg") } }},
        // Overview ERS: red on overtake/boost mode, amber when low, else blue.
        {"ers",      { "info",    { R("ers_mode","eq",3,"neg"), R("ers_pct","lt",20,"warn") } }},
        // Overview fuel: by laps-remaining vs finish.
        {"fuel",     { "neg",     { R("fuel_laps","gt",1,"pos"), R("fuel_laps","gte",0,"warn") } }},
        {"pos",      { "neutral", {} }},
        // Tyre compound colour (keyed on visual_compound).
        {"tyre",     { "neutral", { R("visual_compound","eq",16,"compoundSoft"),
                                     R("visual_compound","eq",17,"compoundMedium"),
                                     R("visual_compound","eq",18,"compoundHard"),
                                     R("visual_compound","eq",7,"compoundInter"),
                                     R("visual_compound","eq",8,"compoundWet") } }},

        // ── Power stat cards ─────────────────────────────────────────────────
        {"power.total", { "pos", { R("self","gt",800,"neg"), R("self","gt",600,"mguk") } }},
        {"power.ice",   { "ice",     {} }},
        {"power.mguk",  { "mguk",    {} }},
        {"power.split", { "neutral", {} }},
        // Power ERS store/% ramp (distinct from the overview ERS rule above).
        {"power.ers",   { "neg", { R("self","gt",60,"ice"), R("self","gt",30,"mguk") } }},
        {"power.fuel",  { "fuel",    {} }},

        // ── Thermal / wear ramps (shared by thermal + tyre cards) ────────────
        {"temp.tyre",  { "neg", { R("self","lt",60,"info"), R("self","lt",80,"warn"),
                                   R("self","lte",110,"pos"), R("self","lte",130,"warnAlt") } }},
        {"temp.brake", { "neg", { R("self","lt",200,"pos"), R("self","lt",400,"warn"),
                                   R("self","lt",600,"warnAlt") } }},
        {"wear",       { "neg", { R("self","lt",20,"wear1"), R("self","lt",40,"wear2"),
                                   R("self","lt",60,"mguk"),  R("self","lt",80,"wear3") } }},

        // ── Session stat cards ───────────────────────────────────────────────
        {"session.pitSpeed",  { "info",  {} }},
        {"session.pitWindow", { "mguk",  {} }},
        {"session.rejoin",    { "wear1", {} }},
        {"session.trackTemp", { "neg", { R("self","lt",25,"info"), R("self","lt",35,"wear1"),
                                          R("self","lt",45,"mguk"), R("self","lt",55,"wear3") } }},
        {"session.airTemp",   { "wear3", { R("self","lt",18,"info"), R("self","lt",26,"wear1"),
                                            R("self","lt",34,"mguk") } }},
    };
    return specs;
}

std::string cardColorsJson() {
    std::string out;
    (void)glz::write_json(cardColors(), out);
    return out;
}

} // namespace tnrp
