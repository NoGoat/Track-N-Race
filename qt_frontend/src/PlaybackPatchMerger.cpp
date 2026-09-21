#include "PlaybackPatchMerger.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QMap>

#include <cmath>
#include <initializer_list>
#include <limits>
#include <string_view>
#include <utility>

namespace {

constexpr double missingDouble() {
    return std::numeric_limits<double>::quiet_NaN();
}

template <typename Fn>
void forEachPatchField(int type, Fn&& fn) {
    const auto fields = [&](std::initializer_list<const char*> values) {
        for (const char* value : values) fn(QLatin1String(value));
    };
    switch (type) {
        case 1:  fields({"speed_kph"}); break;
        case 2:  fields({"rpm", "rev_lights_pct", "rev_lights_bit_value"}); break;
        case 3:  fields({"gear"}); break;
        case 4:  fields({"throttle"}); break;
        case 5:  fields({"brake"}); break;
        case 6:  fields({"steering"}); break;
        case 7:  fields({"drs", "slm", "drs_allowed"}); break;
        case 8:  fields({"tyre_temp_surface_fl", "tyre_temp_surface_fr",
                         "tyre_temp_surface_rl", "tyre_temp_surface_rr"}); break;
        case 9:  fields({"tyre_temp_inner_fl", "tyre_temp_inner_fr",
                         "tyre_temp_inner_rl", "tyre_temp_inner_rr"}); break;
        case 10: fields({"brake_temp_fl", "brake_temp_fr",
                         "brake_temp_rl", "brake_temp_rr"}); break;
        case 11: fields({"engine_temp"}); break;
        case 12: fields({"tyre_wear_fl", "tyre_wear_fr",
                         "tyre_wear_rl", "tyre_wear_rr"}); break;
        case 13: fields({"tyre_compound", "visual_compound", "tyre_age_laps",
                         "sets", "fitted_idx"}); break;
        case 14: fields({"tyre_dmg_fl", "tyre_dmg_fr", "tyre_dmg_rl", "tyre_dmg_rr",
                         "brake_dmg_fl", "brake_dmg_fr", "brake_dmg_rl", "brake_dmg_rr",
                         "wing_fl", "wing_fr", "wing_rear", "floor_damage",
                         "diffuser_damage", "sidepod_damage", "gearbox_damage",
                         "engine_damage", "drs_fault", "ers_fault", "blisters_fl",
                         "blisters_fr", "blisters_rl", "blisters_rr"}); break;
        case 15: fields({"fuel_kg", "fuel_laps", "fuel_mix"}); break;
        case 16: fields({"ers_j", "ers_pct", "ers_mode"}); break;
        case 17: fields({"ers_harvested_mguk_j", "ers_harvested_mguh_j"}); break;
        case 18: fields({"ers_deployed_j"}); break;
        case 19: fields({"engine_power_ice_kw", "engine_power_mguk_kw"}); break;
        case 20: fields({"front_brake_bias"}); break;
        case 21: fields({"g_lat", "g_long", "g_vert"}); break;
        case 22: fields({"front_aero_height_mm", "rear_aero_height_mm"}); break;
        case 23: fields({"x", "z"}); break;
        case 24: fields({"lap_distance_m", "position", "lap_num", "current_lap_ms",
                         "last_lap_ms", "s1_ms", "s2_ms", "gap_ms", "pit_status",
                         "num_pit_stops", "lap_invalid", "penalties_s", "num_dt_pens",
                         "num_sg_pens", "sector", "result_status", "driver_status"}); break;
        default: break;
    }
}

void overlay(QJsonObject& target, const QJsonObject& patch) {
    for (auto it = patch.constBegin(); it != patch.constEnd(); ++it)
        target.insert(it.key(), it.value());
}

void withdrawFields(QJsonObject& object, int v6Type) {
    forEachPatchField(v6Type, [&](QLatin1String field) { object.remove(field); });
}

QHash<int, QJsonObject> carsByIndex(const QJsonObject& object) {
    QHash<int, QJsonObject> result;
    for (const QJsonValue& value : object.value(QStringLiteral("cars")).toArray()) {
        const QJsonObject car = value.toObject();
        if (car.contains(QStringLiteral("idx")))
            result.insert(car.value(QStringLiteral("idx")).toInt(), car);
    }
    return result;
}

QJsonObject mergePatch(const QJsonObject& previous, const QJsonObject& patch,
                       int v6Type) {
    QJsonObject merged = previous;
    overlay(merged, patch);

    if (patch.contains(QStringLiteral("cars"))) {
        QHash<int, QJsonObject> cars = carsByIndex(previous);
        for (const QJsonValue& value : patch.value(QStringLiteral("cars")).toArray()) {
            const QJsonObject carPatch = value.toObject();
            if (!carPatch.contains(QStringLiteral("idx"))) continue;
            const int index = carPatch.value(QStringLiteral("idx")).toInt();
            QJsonObject car = cars.value(index);
            overlay(car, carPatch);
            if (carPatch.value(QStringLiteral("available")).isBool() &&
                !carPatch.value(QStringLiteral("available")).toBool())
                withdrawFields(car, v6Type);
            car.remove(QStringLiteral("available"));
            cars.insert(index, std::move(car));
        }
        QMap<int, QJsonObject> ordered;
        for (auto it = cars.constBegin(); it != cars.constEnd(); ++it)
            ordered.insert(it.key(), it.value());
        QJsonArray array;
        for (auto it = ordered.constBegin(); it != ordered.constEnd(); ++it)
            array.append(it.value());
        merged.insert(QStringLiteral("cars"), array);
    } else if (patch.value(QStringLiteral("available")).isBool() &&
               !patch.value(QStringLiteral("available")).toBool()) {
        withdrawFields(merged, v6Type);
    }
    merged.remove(QStringLiteral("available"));
    return merged;
}

bool has(const QJsonObject& object, const char* field) {
    return object.contains(QLatin1String(field));
}

QHash<int, QJsonObject> decodedCars(const QJsonObject& object) {
    return carsByIndex(object);
}

void markMissing(tnrp::AnyRow& row, const QJsonObject& object) {
    const double nan = missingDouble();
    if (auto* value = std::get_if<TelemetryRow>(&row)) {
        if (!has(object, "speed_kph")) value->speed_kph = kPlaybackMissingInt;
        if (!has(object, "rpm")) value->rpm = kPlaybackMissingInt;
        if (!has(object, "gear")) value->gear = kPlaybackMissingInt;
        if (!has(object, "throttle")) value->throttle = static_cast<float>(nan);
        if (!has(object, "brake")) value->brake = static_cast<float>(nan);
        if (!has(object, "steering")) value->steering = nan;
        if (!has(object, "drs")) value->drs = kPlaybackMissingInt;
        if (!has(object, "slm")) value->slm = kPlaybackMissingInt;
        if (!has(object, "tyre_temp_surface_fl")) value->tyre_temp_surface_fl = kPlaybackMissingInt;
        if (!has(object, "tyre_temp_surface_fr")) value->tyre_temp_surface_fr = kPlaybackMissingInt;
        if (!has(object, "tyre_temp_surface_rl")) value->tyre_temp_surface_rl = kPlaybackMissingInt;
        if (!has(object, "tyre_temp_surface_rr")) value->tyre_temp_surface_rr = kPlaybackMissingInt;
        if (!has(object, "tyre_temp_inner_fl")) value->tyre_temp_inner_fl = kPlaybackMissingInt;
        if (!has(object, "tyre_temp_inner_fr")) value->tyre_temp_inner_fr = kPlaybackMissingInt;
        if (!has(object, "tyre_temp_inner_rl")) value->tyre_temp_inner_rl = kPlaybackMissingInt;
        if (!has(object, "tyre_temp_inner_rr")) value->tyre_temp_inner_rr = kPlaybackMissingInt;
        if (!has(object, "brake_temp_fl")) value->brake_temp_fl = kPlaybackMissingInt;
        if (!has(object, "brake_temp_fr")) value->brake_temp_fr = kPlaybackMissingInt;
        if (!has(object, "brake_temp_rl")) value->brake_temp_rl = kPlaybackMissingInt;
        if (!has(object, "brake_temp_rr")) value->brake_temp_rr = kPlaybackMissingInt;
        if (!has(object, "engine_temp")) value->engine_temp = kPlaybackMissingInt;
    } else if (auto* value = std::get_if<StatusRow>(&row)) {
        if (!has(object, "fuel_mix")) value->fuel_mix = kPlaybackMissingInt;
        if (!has(object, "front_brake_bias")) value->front_brake_bias = kPlaybackMissingInt;
        if (!has(object, "fuel_kg")) value->fuel_kg = nan;
        if (!has(object, "fuel_laps")) value->fuel_laps = nan;
        if (!has(object, "drs_allowed")) value->drs_allowed = false;
        if (!has(object, "tyre_compound")) value->tyre_compound = kPlaybackMissingInt;
        if (!has(object, "visual_compound")) value->visual_compound = kPlaybackMissingInt;
        if (!has(object, "tyre_age_laps")) value->tyre_age_laps = kPlaybackMissingInt;
        if (!has(object, "ers_j")) value->ers_j = kPlaybackMissingInt;
        if (!has(object, "ers_pct")) value->ers_pct = nan;
        if (!has(object, "ers_mode")) value->ers_mode = kPlaybackMissingInt;
        if (!has(object, "ers_deployed_j")) value->ers_deployed_j = kPlaybackMissingInt;
        if (!has(object, "engine_power_ice_kw")) value->engine_power_ice_kw = nan;
        if (!has(object, "engine_power_mguk_kw")) value->engine_power_mguk_kw = nan;
        if (!has(object, "ers_harvested_mguk_j")) value->ers_harvested_mguk_j = kPlaybackMissingInt;
        if (!has(object, "ers_harvested_mguh_j")) value->ers_harvested_mguh_j = kPlaybackMissingInt;
    } else if (auto* value = std::get_if<DamageRow>(&row)) {
        if (!has(object, "tyre_wear_fl")) value->tyre_wear_fl = nan;
        if (!has(object, "tyre_wear_fr")) value->tyre_wear_fr = nan;
        if (!has(object, "tyre_wear_rl")) value->tyre_wear_rl = nan;
        if (!has(object, "tyre_wear_rr")) value->tyre_wear_rr = nan;
#define TNR_MISSING_DAMAGE(field) if (!has(object, #field)) value->field = kPlaybackMissingInt
        TNR_MISSING_DAMAGE(tyre_dmg_fl); TNR_MISSING_DAMAGE(tyre_dmg_fr);
        TNR_MISSING_DAMAGE(tyre_dmg_rl); TNR_MISSING_DAMAGE(tyre_dmg_rr);
        TNR_MISSING_DAMAGE(brake_dmg_fl); TNR_MISSING_DAMAGE(brake_dmg_fr);
        TNR_MISSING_DAMAGE(brake_dmg_rl); TNR_MISSING_DAMAGE(brake_dmg_rr);
        TNR_MISSING_DAMAGE(blisters_fl); TNR_MISSING_DAMAGE(blisters_fr);
        TNR_MISSING_DAMAGE(blisters_rl); TNR_MISSING_DAMAGE(blisters_rr);
        TNR_MISSING_DAMAGE(wing_fl); TNR_MISSING_DAMAGE(wing_fr); TNR_MISSING_DAMAGE(wing_rear);
        TNR_MISSING_DAMAGE(floor_damage); TNR_MISSING_DAMAGE(sidepod_damage);
        TNR_MISSING_DAMAGE(diffuser_damage); TNR_MISSING_DAMAGE(gearbox_damage);
        TNR_MISSING_DAMAGE(engine_damage); TNR_MISSING_DAMAGE(drs_fault);
        TNR_MISSING_DAMAGE(ers_fault);
#undef TNR_MISSING_DAMAGE
    } else if (auto* value = std::get_if<MotionRow>(&row)) {
        if (!has(object, "g_lat")) value->g_lat = nan;
        if (!has(object, "g_long")) value->g_long = nan;
        if (!has(object, "g_vert")) value->g_vert = nan;
    } else if (auto* value = std::get_if<MotionExRow>(&row)) {
        if (!has(object, "front_aero_height_mm")) value->front_aero_height_mm = nan;
        if (!has(object, "rear_aero_height_mm")) value->rear_aero_height_mm = nan;
    } else if (auto* value = std::get_if<LapRow>(&row)) {
#define TNR_MISSING_LAP(field) if (!has(object, #field)) value->field = kPlaybackMissingInt
        TNR_MISSING_LAP(last_lap_ms); TNR_MISSING_LAP(current_lap_ms);
        if (!has(object, "lap_distance_m")) value->lap_distance_m = static_cast<float>(nan);
        TNR_MISSING_LAP(s1_ms); TNR_MISSING_LAP(s2_ms); TNR_MISSING_LAP(position);
        TNR_MISSING_LAP(lap_num); TNR_MISSING_LAP(pit_status); TNR_MISSING_LAP(num_pit_stops);
        TNR_MISSING_LAP(sector); TNR_MISSING_LAP(penalties_s); TNR_MISSING_LAP(driver_status);
#undef TNR_MISSING_LAP
        if (!has(object, "lap_invalid")) value->lap_invalid = false;
    } else if (auto* value = std::get_if<TimingRow>(&row)) {
        const auto cars = decodedCars(object);
        for (TimingCar& car : value->cars) {
            const QJsonObject source = cars.value(car.idx);
#define TNR_MISSING_TIMING(field) if (!has(source, #field)) car.field = kPlaybackMissingInt
            TNR_MISSING_TIMING(position); TNR_MISSING_TIMING(lap_num);
            TNR_MISSING_TIMING(current_lap_ms); TNR_MISSING_TIMING(last_lap_ms);
            TNR_MISSING_TIMING(s1_ms); TNR_MISSING_TIMING(s2_ms); TNR_MISSING_TIMING(gap_ms);
            TNR_MISSING_TIMING(pit_status); TNR_MISSING_TIMING(num_pit_stops);
            TNR_MISSING_TIMING(penalties_s); TNR_MISSING_TIMING(num_dt_pens);
            TNR_MISSING_TIMING(num_sg_pens); TNR_MISSING_TIMING(sector);
            TNR_MISSING_TIMING(result_status); TNR_MISSING_TIMING(driver_status);
#undef TNR_MISSING_TIMING
            if (!has(source, "lap_invalid")) car.lap_invalid = false;
            if (!has(source, "lap_distance_m")) car.lap_distance_m.reset();
        }
    } else if (auto* value = std::get_if<AllStatusRow>(&row)) {
        const auto cars = decodedCars(object);
        for (AllStatusCar& car : value->cars) {
            const QJsonObject source = cars.value(car.idx);
#define TNR_MISSING_STATUS(field) if (!has(source, #field)) car.field = kPlaybackMissingInt
            TNR_MISSING_STATUS(fuel_mix); TNR_MISSING_STATUS(front_brake_bias);
            if (!has(source, "fuel_kg")) car.fuel_kg = nan;
            if (!has(source, "fuel_laps")) car.fuel_laps = nan;
            if (!has(source, "drs_allowed")) car.drs_allowed = false;
            TNR_MISSING_STATUS(tyre_compound); TNR_MISSING_STATUS(visual_compound);
            TNR_MISSING_STATUS(tyre_age_laps); TNR_MISSING_STATUS(ers_j);
            if (!has(source, "ers_pct")) car.ers_pct = nan;
            TNR_MISSING_STATUS(ers_mode); TNR_MISSING_STATUS(ers_deployed_j);
            if (!has(source, "engine_power_ice_kw")) car.engine_power_ice_kw = nan;
            if (!has(source, "engine_power_mguk_kw")) car.engine_power_mguk_kw = nan;
            TNR_MISSING_STATUS(ers_harvested_mguk_j);
            TNR_MISSING_STATUS(ers_harvested_mguh_j);
#undef TNR_MISSING_STATUS
        }
    } else if (auto* value = std::get_if<PositionsRow>(&row)) {
        const auto cars = decodedCars(object);
        for (PositionCar& car : value->cars) {
            const QJsonObject source = cars.value(car.idx);
            if (!has(source, "x")) car.x = nan;
            if (!has(source, "z")) car.z = nan;
        }
    } else if (auto* value = std::get_if<tnrp::TyreSetsRow>(&row)) {
        if (!has(object, "sets")) value->sets.clear();
        if (!has(object, "fitted_idx")) value->fitted_idx = kPlaybackMissingInt;
    }
}

} // namespace

std::optional<PlaybackDecodedRow> PlaybackPatchMerger::decode(const QByteArray& json) {
    // Older recordings keep the existing typed fast path; only V6 projection
    // rows pay for a dynamic JSON merge.
    if (!json.contains("\"_v6_type\"")) {
        auto parsed = tnrp::parseRow(std::string_view(
            json.constData(), static_cast<size_t>(json.size())));
        if (!parsed) return std::nullopt;
        return PlaybackDecodedRow{std::move(*parsed), json, QJsonObject{}, false};
    }

    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(json, &error);
    const QJsonObject patch = document.isObject() ? document.object() : QJsonObject{};
    const QJsonValue typeValue = patch.value(QStringLiteral("_v6_type"));
    const double rawType = typeValue.toDouble(-1.0);
    const bool sparse = error.error == QJsonParseError::NoError && document.isObject() &&
        typeValue.isDouble() && std::isfinite(rawType) && std::floor(rawType) == rawType;

    QByteArray normalized = json;
    QJsonObject object;
    if (sparse) {
        const QString rowType = patch.value(QStringLiteral("type")).toString();
        if (rowType.isEmpty()) return std::nullopt;
        object = mergePatch(states_.value(rowType), patch, static_cast<int>(rawType));
        states_.insert(rowType, object);
        normalized = QJsonDocument(object).toJson(QJsonDocument::Compact);
    }

    auto parsed = tnrp::parseRow(std::string_view(
        normalized.constData(), static_cast<size_t>(normalized.size())));
    if (!parsed) return std::nullopt;
    if (sparse) markMissing(*parsed, object);
    return PlaybackDecodedRow{std::move(*parsed), std::move(normalized),
                              std::move(object), sparse};
}

bool playbackFieldAvailable(const QJsonObject* object, const char* field) {
    return !object || object->contains(QLatin1String(field));
}

bool playbackCarFieldAvailable(const QJsonObject* object, int carIndex,
                               const char* field) {
    if (!object) return true;
    const auto cars = carsByIndex(*object);
    const auto it = cars.constFind(carIndex);
    return it != cars.constEnd() && it->contains(QLatin1String(field));
}
