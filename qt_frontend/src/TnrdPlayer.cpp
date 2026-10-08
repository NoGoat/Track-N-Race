#include "TnrdPlayer.h"
#include "PlaybackPatchMerger.h"

#include <QMetaObject>
#include <QtEndian>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <iterator>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <string_view>
#include <type_traits>
#include <vector>

#include <tnrp/AnyRow.h>
#include <tnrp/BinaryRows.h>
#include <tnrp/Engine.h>

namespace {

constexpr qsizetype kMaxRows = 750000;

std::string_view typeOf(std::string_view json) {
    constexpr std::string_view key = "\"type\":\"";
    const size_t begin = json.find(key);
    if (begin == std::string_view::npos) return {};
    const size_t value = begin + key.size();
    const size_t end = json.find('"', value);
    return end == std::string_view::npos ? std::string_view{} : json.substr(value, end - value);
}

float historyNumber(int value) {
    return value == kPlaybackMissingInt
        ? std::numeric_limits<float>::quiet_NaN()
        : static_cast<float>(value);
}

template <typename T>
void replaceSameTimestamp(QVector<T>& rows, float time, bool coalesce) {
    if (coalesce && !rows.isEmpty() && rows.last().t == time) rows.removeLast();
}

void appendHistoryRow(PlaybackHistoryBatch& batch, const tnrp::AnyRow& row,
                      bool coalesce = false) {
    auto& data = batch.data;
    if (const auto* t = std::get_if<TelemetryRow>(&row)) {
        replaceSameTimestamp(data.telBuf, t->session_time, coalesce);
        replaceSameTimestamp(data.tyreBuf, t->session_time, coalesce);
        data.onTelemetry(t->session_time, historyNumber(t->speed_kph),
                         historyNumber(t->rpm), historyNumber(t->gear),
                         t->throttle, t->brake,
                         static_cast<float>(t->steering));
        data.onTyre(t->session_time,
            historyNumber(t->tyre_temp_surface_fl), historyNumber(t->tyre_temp_surface_fr),
            historyNumber(t->tyre_temp_surface_rl), historyNumber(t->tyre_temp_surface_rr),
            historyNumber(t->tyre_temp_inner_fl), historyNumber(t->tyre_temp_inner_fr),
            historyNumber(t->tyre_temp_inner_rl), historyNumber(t->tyre_temp_inner_rr),
            historyNumber(t->brake_temp_fl), historyNumber(t->brake_temp_fr),
            historyNumber(t->brake_temp_rl), historyNumber(t->brake_temp_rr),
            0.0f, 0.0f, 0.0f, 0.0f);
    } else if (const auto* s = std::get_if<StatusRow>(&row)) {
        replaceSameTimestamp(data.stsBuf, s->session_time, coalesce);
        data.onStatus(s->session_time, static_cast<float>(s->ers_pct),
                      static_cast<float>(s->fuel_kg),
                      static_cast<float>(s->engine_power_ice_kw),
                      static_cast<float>(s->engine_power_mguk_kw),
                      historyNumber(s->ers_harvested_mguk_j),
                      historyNumber(s->ers_harvested_mguh_j),
                      s->tyre_compound, s->visual_compound, s->tyre_age_laps,
                      historyNumber(s->ers_deployed_j));
    } else if (const auto* d = std::get_if<DamageRow>(&row)) {
        replaceSameTimestamp(data.damageBuf, d->session_time, coalesce);
        data.onDamage(d->session_time, static_cast<float>(d->tyre_wear_fl),
                      static_cast<float>(d->tyre_wear_fr),
                      static_cast<float>(d->tyre_wear_rl),
                      static_cast<float>(d->tyre_wear_rr));
    } else if (const auto* l = std::get_if<LapRow>(&row)) {
        replaceSameTimestamp(batch.progress, l->session_time, coalesce);
        batch.progress.push_back({l->session_time, l->current_lap_ms,
                                  static_cast<float>(l->lap_distance_m), l->sector});
        data.latestTime = std::max(data.latestTime, l->session_time);
    } else if (const auto* m = std::get_if<MotionRow>(&row)) {
        replaceSameTimestamp(data.motionBuf, m->session_time, coalesce);
        data.onMotion(m->session_time, static_cast<float>(m->g_lat),
                      static_cast<float>(m->g_long));
    } else if (const auto* m = std::get_if<MotionExRow>(&row)) {
        replaceSameTimestamp(data.motionExBuf, m->session_time, coalesce);
        data.onMotionEx(m->session_time,
                        static_cast<float>(m->front_aero_height_mm),
                        static_cast<float>(m->rear_aero_height_mm));
    }
}

// Object counterpart of appendHistoryRow for already-parsed JSON (indexed lap
// reads). A missing field reads as the typed path would: NaN / missing-int for
// a V6 patch state, the row struct's zero default for a complete legacy row.
void appendHistoryObject(PlaybackHistoryBatch& batch, const QJsonObject& row, bool sparse) {
    auto& data = batch.data;
    const QString type = row.value(QStringLiteral("type")).toString();
    const float t = static_cast<float>(row.value(QStringLiteral("session_time")).toDouble());
    const float absent = sparse ? std::numeric_limits<float>::quiet_NaN() : 0.0f;
    const auto num = [&row, absent](const char* field) {
        const QJsonValue value = row.value(QLatin1String(field));
        return value.isDouble() ? static_cast<float>(value.toDouble()) : absent;
    };
    const auto integer = [&row, sparse](const char* field) {
        const QJsonValue value = row.value(QLatin1String(field));
        return value.isDouble() ? value.toInt() : sparse ? kPlaybackMissingInt : 0;
    };
    if (type == QLatin1String("telemetry")) {
        replaceSameTimestamp(data.telBuf, t, sparse);
        replaceSameTimestamp(data.tyreBuf, t, sparse);
        data.onTelemetry(t, num("speed_kph"), num("rpm"), num("gear"),
                         num("throttle"), num("brake"), num("steering"));
        data.onTyre(t,
            num("tyre_temp_surface_fl"), num("tyre_temp_surface_fr"),
            num("tyre_temp_surface_rl"), num("tyre_temp_surface_rr"),
            num("tyre_temp_inner_fl"), num("tyre_temp_inner_fr"),
            num("tyre_temp_inner_rl"), num("tyre_temp_inner_rr"),
            num("brake_temp_fl"), num("brake_temp_fr"),
            num("brake_temp_rl"), num("brake_temp_rr"),
            0.0f, 0.0f, 0.0f, 0.0f);
    } else if (type == QLatin1String("status")) {
        replaceSameTimestamp(data.stsBuf, t, sparse);
        data.onStatus(t, num("ers_pct"), num("fuel_kg"), num("engine_power_ice_kw"),
                      num("engine_power_mguk_kw"), num("ers_harvested_mguk_j"),
                      num("ers_harvested_mguh_j"), integer("tyre_compound"),
                      integer("visual_compound"), integer("tyre_age_laps"),
                      num("ers_deployed_j"));
    } else if (type == QLatin1String("damage")) {
        replaceSameTimestamp(data.damageBuf, t, sparse);
        data.onDamage(t, num("tyre_wear_fl"), num("tyre_wear_fr"),
                      num("tyre_wear_rl"), num("tyre_wear_rr"));
    } else if (type == QLatin1String("lap")) {
        replaceSameTimestamp(batch.progress, t, sparse);
        batch.progress.push_back({t, integer("current_lap_ms"), num("lap_distance_m"),
                                  integer("sector")});
        data.latestTime = std::max(data.latestTime, t);
    } else if (type == QLatin1String("motion")) {
        replaceSameTimestamp(data.motionBuf, t, sparse);
        data.onMotion(t, num("g_lat"), num("g_long"));
    } else if (type == QLatin1String("motion_ex")) {
        replaceSameTimestamp(data.motionExBuf, t, sparse);
        data.onMotionEx(t, num("front_aero_height_mm"), num("rear_aero_height_mm"));
    }
}

template <typename T>
void sortAndCap(QVector<T>& rows) {
    std::stable_sort(rows.begin(), rows.end(),
                     [](const T& a, const T& b) { return a.t < b.t; });
    if (rows.size() > kMaxRows) rows.remove(0, rows.size() - kMaxRows);
}

template <typename T>
void copyTimedRange(const QVector<T>& source, float start, float end, QVector<T>& target) {
    const auto first = std::lower_bound(source.begin(), source.end(), start,
        [](const T& row, float value) { return row.t < value; });
    const auto last = std::upper_bound(first, source.end(), end,
        [](float value, const T& row) { return value < row.t; });
    target.reserve(std::distance(first, last));
    for (auto it = first; it != last; ++it) target.push_back(*it);
}

// Every lap gets its progress; only the lap under the cursor gets sample
// families, which is all SessionModel installs (the session buffers hold the
// same samples). Copying every lap here only to discard it doubled the peak.
void buildLapDetails(PlaybackHistoryBatch& batch,
                     const QVector<PlaybackLapRange>& ranges) {
    int cursorLap = batch.lapNum;
    if (cursorLap <= 0)
        for (const PlaybackLapRange& range : ranges)
            if (range.start <= batch.data.latestTime && batch.data.latestTime <= range.end) cursorLap = range.lapNum;
    batch.lapDetails.reserve(ranges.size());
    for (const PlaybackLapRange& range : ranges) {
        LapBlock lap;
        lap.lapNum = range.lapNum;
        lap.startSessionTime = range.start;
        lap.endSessionTime = range.end;
        if (range.lapNum == cursorLap) {
            copyTimedRange(batch.data.telBuf, range.start, range.end, lap.tel);
            copyTimedRange(batch.data.tyreBuf, range.start, range.end, lap.tyre);
            copyTimedRange(batch.data.stsBuf, range.start, range.end, lap.sts);
            copyTimedRange(batch.data.damageBuf, range.start, range.end, lap.damage);
            copyTimedRange(batch.data.motionBuf, range.start, range.end, lap.motion);
            copyTimedRange(batch.data.motionExBuf, range.start, range.end, lap.motionEx);
        }
        copyTimedRange(batch.progress, range.start, range.end, lap.progress);
        if (!lap.tel.isEmpty() || !lap.tyre.isEmpty() || !lap.sts.isEmpty() ||
            !lap.damage.isEmpty() || !lap.motion.isEmpty() ||
            !lap.motionEx.isEmpty() || !lap.progress.isEmpty())
            batch.lapDetails.push_back(std::move(lap));
    }
}

// ── TNRD V6 columnar history (Config::columnarV6History) ─────────────────────
// Qt port of Electron's decodeV6History (renderer/src/lib/columnStore.ts). The
// payload is the library's 'V6H1' block format (TnrdV6Archive::columnarHistory):
//   u32 magic, u32 v4Mask, u32 blockCount; per block: u8 v6Type, u8 reserved,
//   u16 fieldCount, u32 rowCount, f32[rowCount] times, then per field
//   u8 nameLength, name, u8 kind (0 f32, 1 f64, 2 i32, 3 bool), u8 dense,
//   presence bitmap when sparse, one value per present row.
// Each row family's blocks are merged by time exactly as the JSON path merged
// the equivalent V6 patches, but without any JSON.
constexpr uint32_t kV6HistoryMagic = 0x31483656u;   // 'V6H1'

enum class V6Family { None, Telemetry, Status, Damage, Lap, Motion, MotionEx };

V6Family v6FieldFamily(int type, std::string_view field) {
    if (type == 7) return field == "drs_allowed" ? V6Family::Status : V6Family::Telemetry;
    if (type == 13) return field == "sets" || field == "fitted_idx" ? V6Family::None : V6Family::Status;
    if (type >= 1 && type <= 11) return V6Family::Telemetry;
    if (type == 12 || type == 14) return V6Family::Damage;
    if (type >= 15 && type <= 20) return V6Family::Status;
    if (type == 21) return V6Family::Motion;
    if (type == 22) return V6Family::MotionEx;
    if (type == 24) return V6Family::Lap;
    return V6Family::None;
}

uint32_t v6FamilyBit(V6Family family) {
    switch (family) {
        case V6Family::Telemetry: return 1u << 1;
        case V6Family::Status:    return 1u << 2;
        case V6Family::Damage:    return 1u << 3;
        case V6Family::Lap:       return 1u << 4;
        case V6Family::Motion:    return 1u << 11;
        case V6Family::MotionEx:  return 1u << 12;
        default:                  return 0;
    }
}

// A field's values stay packed in the payload: a merge reads them in row
// order through a V6FieldCursor, so no block is ever expanded to doubles.
struct V6HistoryField {
    std::string name;
    uint8_t kind = 0;                  // 0 f32, 1 f64, 2 i32, 3 bool
    uint8_t width = 4;
    const uint8_t* present = nullptr;  // presence bitmap; null when dense
    const uint8_t* data = nullptr;     // one value per present row
};
struct V6HistoryBlock {
    int type = 0;
    std::vector<float> time;
    std::vector<V6HistoryField> fields;
    std::optional<V6HistoryField> available;   // the block's availability column, if any
};

// Reads one field's values for rows 0, 1, 2, ... in turn. NaN = absent.
struct V6FieldCursor {
    const V6HistoryField* field = nullptr;
    size_t offset = 0;
    double next(size_t row) {
        const V6HistoryField& f = *field;
        if (f.present && !((f.present[row >> 3] >> (row & 7)) & 1))
            return std::numeric_limits<double>::quiet_NaN();
        const uint8_t* at = f.data + offset;
        offset += f.width;
        switch (f.kind) {
            case 0: { const quint32 v = qFromLittleEndian<quint32>(at);
                      float x; std::memcpy(&x, &v, sizeof x); return x; }
            case 1: { const quint64 v = qFromLittleEndian<quint64>(at);
                      double x; std::memcpy(&x, &v, sizeof x); return x; }
            case 2: return qFromLittleEndian<qint32>(at);
            default: return *at;
        }
    }
};

bool isV6HistoryPayload(const uint8_t* data, size_t length) {
    return data && length >= 12 && qFromLittleEndian<quint32>(data) == kV6HistoryMagic;
}

bool parseV6History(const uint8_t* data, size_t length, uint32_t& mask,
                    std::vector<V6HistoryBlock>& blocks) {
    size_t at = 0;
    const auto need = [&](size_t n) { return at + n <= length; };
    if (!need(12) || qFromLittleEndian<quint32>(data) != kV6HistoryMagic) return false;
    mask = qFromLittleEndian<quint32>(data + 4);
    const quint32 blockCount = qFromLittleEndian<quint32>(data + 8);
    at = 12;
    blocks.reserve(blockCount);
    for (quint32 b = 0; b < blockCount; ++b) {
        if (!need(8)) return false;
        V6HistoryBlock block;
        block.type = data[at];
        const quint16 fieldCount = qFromLittleEndian<quint16>(data + at + 2);
        const quint32 rows = qFromLittleEndian<quint32>(data + at + 4);
        at += 8;
        if (!need(size_t(rows) * 4)) return false;
        block.time.resize(rows);
        for (quint32 r = 0; r < rows; ++r, at += 4) {
            const quint32 bits = qFromLittleEndian<quint32>(data + at);
            std::memcpy(&block.time[r], &bits, sizeof(float));
        }
        for (quint16 f = 0; f < fieldCount; ++f) {
            if (!need(1)) return false;
            const size_t nameLength = data[at++];
            if (!need(nameLength + 2)) return false;
            std::string name(reinterpret_cast<const char*>(data + at), nameLength);
            at += nameLength;
            const uint8_t kind = data[at++];
            const bool dense = data[at++] != 0;
            const uint8_t* present = nullptr;
            if (!dense) {
                const size_t bitmapBytes = (size_t(rows) + 7) / 8;
                if (!need(bitmapBytes)) return false;
                present = data + at;
                at += bitmapBytes;
            }
            const uint8_t width = kind == 1 ? 8 : kind == 3 ? 1 : 4;
            size_t presentRows = rows;
            if (present) {
                presentRows = 0;
                for (quint32 r = 0; r < rows; ++r) presentRows += (present[r >> 3] >> (r & 7)) & 1;
            }
            if (!need(presentRows * width)) return false;
            V6HistoryField field{std::move(name), kind, width, present, data + at};
            at += presentRows * width;
            if (field.name == "available") block.available = std::move(field);
            else block.fields.push_back(std::move(field));
        }
        blocks.push_back(std::move(block));
    }
    return true;
}

// One family's merge: the union of its blocks' times with every field carried
// forward (Electron's decodeV6HistoryInner). Rows go to the caller as they are
// completed, so neither the blocks nor the family are ever held as tables of
// doubles: a full-race seek used to hold both.
class V6FamilyMerge {
public:
    // False when no block carries this family.
    bool prepare(const std::vector<V6HistoryBlock>& blocks, V6Family family) {
        const auto slotOf = [this](const std::string& name, bool create) -> int {
            for (size_t i = 0; i < names_.size(); ++i) if (names_[i] == name) return int(i);
            if (!create) return -1;
            names_.push_back(name);
            return int(names_.size()) - 1;
        };
        for (const V6HistoryBlock& block : blocks) {
            const std::vector<const char*> patchFields = playbackPatchFields(block.type);
            bool any = false;
            for (const V6HistoryField& field : block.fields)
                if (v6FieldFamily(block.type, field.name) == family) { any = true; break; }
            const bool availabilityOnly = !any && block.available && !patchFields.empty() &&
                v6FieldFamily(block.type, patchFields.front()) == family;
            if (!any && !availabilityOnly) continue;
            Track track;
            track.block = &block;
            for (const V6HistoryField& field : block.fields) {
                track.fieldSlots.push_back(v6FieldFamily(block.type, field.name) == family
                    ? slotOf(field.name, true) : -1);
                track.cursors.push_back({&field, 0});
            }
            if (block.available) track.available = V6FieldCursor{&*block.available, 0};
            tracks_.push_back(std::move(track));
        }
        if (tracks_.empty()) return false;
        // Withdrawal targets are resolved once every field slot exists.
        for (Track& track : tracks_)
            for (const char* name : playbackPatchFields(track.block->type))
                if (v6FieldFamily(track.block->type, name) == family)
                    if (const int slot = slotOf(name, false); slot >= 0) track.dropSlots.push_back(slot);
        for (const Track& track : tracks_) {
            std::vector<float> merged;
            merged.reserve(times_.size() + track.block->time.size());
            std::merge(times_.begin(), times_.end(), track.block->time.begin(), track.block->time.end(),
                       std::back_inserter(merged));
            merged.erase(std::unique(merged.begin(), merged.end()), merged.end());
            times_.swap(merged);
        }
        return true;
    }

    size_t rows() const { return times_.size(); }
    int column(std::string_view name) const {
        for (size_t i = 0; i < names_.size(); ++i) if (names_[i] == name) return int(i);
        return -1;
    }

    // row(time, state): state[column] is the value carried at that time, NaN
    // when absent. Call once.
    template <typename RowFn>
    void forEachRow(RowFn&& row) {
        constexpr double nan = std::numeric_limits<double>::quiet_NaN();
        std::vector<double> state(names_.size(), nan);
        for (const float t : times_) {
            for (Track& track : tracks_) {
                const V6HistoryBlock& block = *track.block;
                size_t& p = track.next;
                while (p < block.time.size() && block.time[p] <= t) {
                    if (track.available && track.available->next(p) == 0.0)
                        for (const int slot : track.dropSlots) state[size_t(slot)] = nan;
                    for (size_t f = 0; f < track.cursors.size(); ++f) {
                        const double value = track.cursors[f].next(p);
                        const int slot = track.fieldSlots[f];
                        if (slot >= 0 && value == value) state[size_t(slot)] = value;
                    }
                    ++p;
                }
            }
            row(t, static_cast<const std::vector<double>&>(state));
        }
    }

private:
    struct Track {
        const V6HistoryBlock* block = nullptr;
        std::vector<int> fieldSlots, dropSlots;
        std::vector<V6FieldCursor> cursors;
        std::optional<V6FieldCursor> available;
        size_t next = 0;
    };
    std::vector<Track> tracks_;
    std::vector<std::string> names_;
    std::vector<float> times_;
};

int missingInt(double value) {
    return std::isfinite(value) ? static_cast<int>(value) : kPlaybackMissingInt;
}

// Fills the batch's buffers from a V6H1 payload. Returns false when the bytes
// are not a valid payload (the caller then leaves the batch empty).
bool decodeColumnarHistory(PlaybackHistoryBatch& batch, const uint8_t* data, size_t length) {
    uint32_t mask = 0;
    std::vector<V6HistoryBlock> blocks;
    if (!parseV6History(data, length, mask, blocks)) return false;
    auto& out = batch.data;
    const auto f = [](double value) { return static_cast<float>(value); };
    const auto at = [](const std::vector<double>& state, int column) {
        return column < 0 ? std::numeric_limits<double>::quiet_NaN() : state[size_t(column)];
    };
    for (V6Family family : {V6Family::Telemetry, V6Family::Status, V6Family::Damage,
                            V6Family::Lap, V6Family::Motion, V6Family::MotionEx}) {
        if (!(mask & v6FamilyBit(family))) continue;
        V6FamilyMerge merge;
        if (!merge.prepare(blocks, family)) continue;
        const qsizetype rows = qsizetype(merge.rows());
        switch (family) {
            case V6Family::Telemetry: {
                const int speed = merge.column("speed_kph"), rpm = merge.column("rpm"),
                          gear = merge.column("gear"), throttle = merge.column("throttle"),
                          brake = merge.column("brake"), steering = merge.column("steering");
                const int temps[12] = {
                    merge.column("tyre_temp_surface_fl"), merge.column("tyre_temp_surface_fr"),
                    merge.column("tyre_temp_surface_rl"), merge.column("tyre_temp_surface_rr"),
                    merge.column("tyre_temp_inner_fl"), merge.column("tyre_temp_inner_fr"),
                    merge.column("tyre_temp_inner_rl"), merge.column("tyre_temp_inner_rr"),
                    merge.column("brake_temp_fl"), merge.column("brake_temp_fr"),
                    merge.column("brake_temp_rl"), merge.column("brake_temp_rr") };
                out.telBuf.reserve(out.telBuf.size() + rows);
                out.tyreBuf.reserve(out.tyreBuf.size() + rows);
                merge.forEachRow([&](float t, const std::vector<double>& s) {
                    out.onTelemetry(t, f(at(s, speed)), f(at(s, rpm)), f(at(s, gear)),
                                    f(at(s, throttle)), f(at(s, brake)), f(at(s, steering)));
                    out.onTyre(t, f(at(s, temps[0])), f(at(s, temps[1])),
                               f(at(s, temps[2])), f(at(s, temps[3])),
                               f(at(s, temps[4])), f(at(s, temps[5])),
                               f(at(s, temps[6])), f(at(s, temps[7])),
                               f(at(s, temps[8])), f(at(s, temps[9])),
                               f(at(s, temps[10])), f(at(s, temps[11])),
                               0.0f, 0.0f, 0.0f, 0.0f);
                });
                break;
            }
            case V6Family::Status: {
                const int ers = merge.column("ers_pct"), fuel = merge.column("fuel_kg"),
                          ice = merge.column("engine_power_ice_kw"), mguk = merge.column("engine_power_mguk_kw"),
                          harvestK = merge.column("ers_harvested_mguk_j"), harvestH = merge.column("ers_harvested_mguh_j"),
                          compound = merge.column("tyre_compound"), visual = merge.column("visual_compound"),
                          age = merge.column("tyre_age_laps"), deployed = merge.column("ers_deployed_j");
                out.stsBuf.reserve(out.stsBuf.size() + rows);
                merge.forEachRow([&](float t, const std::vector<double>& s) {
                    out.onStatus(t, f(at(s, ers)), f(at(s, fuel)),
                                 f(at(s, ice)), f(at(s, mguk)),
                                 f(at(s, harvestK)), f(at(s, harvestH)),
                                 missingInt(at(s, compound)), missingInt(at(s, visual)),
                                 missingInt(at(s, age)), f(at(s, deployed)));
                });
                break;
            }
            case V6Family::Damage: {
                const int fl = merge.column("tyre_wear_fl"), fr = merge.column("tyre_wear_fr"),
                          rl = merge.column("tyre_wear_rl"), rr = merge.column("tyre_wear_rr");
                out.damageBuf.reserve(out.damageBuf.size() + rows);
                merge.forEachRow([&](float t, const std::vector<double>& s) {
                    out.onDamage(t, f(at(s, fl)), f(at(s, fr)), f(at(s, rl)), f(at(s, rr)));
                });
                break;
            }
            case V6Family::Lap: {
                const int current = merge.column("current_lap_ms"), distance = merge.column("lap_distance_m"),
                          sector = merge.column("sector");
                batch.progress.reserve(batch.progress.size() + rows);
                merge.forEachRow([&](float t, const std::vector<double>& s) {
                    batch.progress.push_back({t, missingInt(at(s, current)),
                                              f(at(s, distance)), missingInt(at(s, sector))});
                    out.latestTime = std::max(out.latestTime, t);
                });
                break;
            }
            case V6Family::Motion: {
                const int lat = merge.column("g_lat"), lon = merge.column("g_long");
                out.motionBuf.reserve(out.motionBuf.size() + rows);
                merge.forEachRow([&](float t, const std::vector<double>& s) {
                    out.onMotion(t, f(at(s, lat)), f(at(s, lon)));
                });
                break;
            }
            case V6Family::MotionEx: {
                const int front = merge.column("front_aero_height_mm"), rear = merge.column("rear_aero_height_mm");
                out.motionExBuf.reserve(out.motionExBuf.size() + rows);
                merge.forEachRow([&](float t, const std::vector<double>& s) {
                    out.onMotionEx(t, f(at(s, front)), f(at(s, rear)));
                });
                break;
            }
            default: break;
        }
    }
    return true;
}

} // namespace

TnrdPlayer::TnrdPlayer(tnrp::Engine* engine, QObject* parent)
    : QObject(parent), engine_(engine) {
    for (int lane = 0; lane < LaneCount; ++lane)
        lanes_[lane].thread = std::thread(&TnrdPlayer::workerLoop, this, lane);
}

TnrdPlayer::Lane TnrdPlayer::laneOf(WorkKind kind) {
    switch (kind) {
        case WorkKind::LapData:
        case WorkKind::AnalysisLapData:
            return ReadLane;
        case WorkKind::SeekDecode:
        case WorkKind::RequirementsDecode:
        case WorkKind::LapDataDecode:
            return DecodeLane;
        default:
            return CommandLane;
    }
}

TnrdPlayer::~TnrdPlayer() { shutdown(); }

void TnrdPlayer::setEngine(tnrp::Engine* engine) {
    quiesce();
    resumeAfterSeek_ = false;
    latestSeekRequest_.fetch_add(1, std::memory_order_acq_rel);
    latestRequirementsRequest_.fetch_add(1, std::memory_order_acq_rel);
    engine_.store(engine, std::memory_order_release);
    requirementsApplied_ = false;
    catalogReady_ = false;
    if (!engine && loaded_.exchange(false, std::memory_order_acq_rel)) {
        loading_ = false;
        playing_ = false;
        emit closed();
    }
}

void TnrdPlayer::quiesce() {
    std::unique_lock lock(workMutex_);
    for (LaneState& lane : lanes_) lane.work.clear();
    idleCv_.wait(lock, [this] {
        return std::none_of(std::begin(lanes_), std::end(lanes_),
                            [](const LaneState& lane) { return lane.active; });
    });
}

void TnrdPlayer::shutdown() {
    {
        std::lock_guard lock(workMutex_);
        if (stopping_) return;
        stopping_ = true;
        for (LaneState& lane : lanes_) lane.work.clear();
    }
    workCv_.notify_all();
    for (LaneState& lane : lanes_)
        if (lane.thread.joinable()) lane.thread.join();
    engine_.store(nullptr, std::memory_order_release);
}

void TnrdPlayer::post(WorkKind kind, std::function<void()> work, bool replacePending) {
    {
        std::lock_guard lock(workMutex_);
        if (stopping_) return;
        // Supersession spans lanes: a new seek also drops a pending decode of
        // an older seek, and load/close empty every lane.
        if (replacePending) {
            for (LaneState& lane : lanes_) {
                if (kind == WorkKind::Close || kind == WorkKind::Load) {
                    lane.work.clear();
                } else if (kind == WorkKind::Driver) {
                    std::erase_if(lane.work, [](const WorkItem& item) {
                        return item.kind == WorkKind::Driver ||
                               item.kind == WorkKind::Seek ||
                               item.kind == WorkKind::SeekDecode;
                    });
                } else {
                    std::erase_if(lane.work, [kind](const WorkItem& item) {
                        return item.kind == kind ||
                            (kind == WorkKind::Seek && item.kind == WorkKind::SeekDecode);
                    });
                }
            }
        }
        lanes_[laneOf(kind)].work.push_back({kind, std::move(work)});
    }
    workCv_.notify_all();
}

void TnrdPlayer::workerLoop(int laneIndex) {
    LaneState& lane = lanes_[laneIndex];
    for (;;) {
        WorkItem item;
        {
            std::unique_lock lock(workMutex_);
            workCv_.wait(lock, [this, &lane] { return stopping_ || !lane.work.empty(); });
            if (stopping_) break;
            item = std::move(lane.work.front());
            lane.work.pop_front();
            lane.active = true;
        }
        item.run();
        {
            std::lock_guard lock(workMutex_);
            lane.active = false;
        }
        idleCv_.notify_all();
    }
    {
        std::lock_guard lock(workMutex_);
        lane.active = false;
    }
    idleCv_.notify_all();
}

void TnrdPlayer::load(const QString& path) {
    if (loading_) return;
    resumeAfterSeek_ = false;
    loading_ = true;
    emit loadingStarted();
    const std::string source = path.toStdString();
    post(WorkKind::Load, [this, source] {
        tnrp::Engine* engine = engine_.load(std::memory_order_acquire);
        std::string error;
        const bool ok = engine && engine->playerLoad(source, &error);
        if (!ok) {
            const QString reason = engine
                ? QString::fromStdString(error)
                : QStringLiteral("The telemetry engine is not available.");
            QMetaObject::invokeMethod(this, [this, reason] {
                loading_ = false;
                emit loadFailed(reason.isEmpty()
                    ? QStringLiteral("The file could not be read.") : reason);
            }, Qt::QueuedConnection);
        }
    }, true);
}

void TnrdPlayer::play() {
    auto* engine = engine_.load(std::memory_order_acquire);
    if (!engine || !loaded_) return;
    if (totalTime_ > startTime_ && currentTime_ >= totalTime_) {
        // The engine can rewind its cursor alone, but Qt must also replace the
        // installed history and panel snapshot before replay starts again.
        resumeAfterSeek_ = true;
        seek(0.0f);
        return;
    }
    engine->playerPlay();
}

void TnrdPlayer::pause() {
    resumeAfterSeek_ = false;
    if (auto* engine = engine_.load(std::memory_order_acquire); engine && loaded_)
        engine->playerPause();
}

void TnrdPlayer::close() {
    resumeAfterSeek_ = false;
    latestSeekRequest_.fetch_add(1, std::memory_order_acq_rel);
    latestRequirementsRequest_.fetch_add(1, std::memory_order_acq_rel);
    playing_ = false;
    post(WorkKind::Close, [this] {
        if (auto* engine = engine_.load(std::memory_order_acquire)) engine->playerClose();
    }, true);
}

void TnrdPlayer::seek(float pct) {
    tnrp::Engine* engine = engine_.load(std::memory_order_acquire);
    if (!engine || !loaded_) return;
    // Like Electron, a seek does not pause playback: the engine keeps its
    // clock, and MainWindow's seek barrier drops old-cursor rows and holds the
    // new cursor's rows until this seek's history has been installed. Nor does
    // it re-apply the data requirements: Engine::setDataRequirements re-primes
    // the playback cursor at the *current* (pre-seek) time, which is pure waste
    // immediately before playerSeek primes it at the target.
    const uint64_t requestId = latestSeekRequest_.fetch_add(1, std::memory_order_acq_rel) + 1;
    engine->playerRequestSeek(requestId);
    emit seekStarted(requestId);
    emit seeked();
    const uint32_t mask = historyMask_;
    const float window = historyWindowSeconds_ < 0.0f ? 0.0f : historyWindowSeconds_;
    const bool allHistory = historyWindowSeconds_ < 0.0f;
    post(WorkKind::Seek, [this, pct, requestId, mask, window, allHistory] {
        if (auto* current = engine_.load(std::memory_order_acquire))
            current->playerSeek(pct, allHistory, requestId, mask, window);
    }, true);
}

void TnrdPlayer::seekToTime(float absoluteTime) {
    const float duration = std::max(0.0f, totalTime_ - startTime_);
    seek(duration > 0.0f ? (absoluteTime - startTime_) / duration : 0.0f);
}

void TnrdPlayer::setSpeed(float mult) {
    if (auto* engine = engine_.load(std::memory_order_acquire); engine && loaded_)
        engine->playerSetSpeed(mult);
}

void TnrdPlayer::selectDriver(int driverIndex, bool useRecordedRows) {
    tnrp::Engine* engine = engine_.load(std::memory_order_acquire);
    if (!engine || !loaded_ || driverIndex < 0) return;

    // Electron changes the engine projection first and then seeks back to the
    // current percentage. Keep that ordering in one worker item so rapid
    // selections cannot interleave a driver change with another cursor rebuild.
    // As with seek(), playback is not paused around the rebuild.
    const float duration = std::max(0.0f, totalTime_ - startTime_);
    const float progress = duration > 0.0f
        ? std::clamp((currentTime_ - startTime_) / duration, 0.0f, 1.0f)
        : 0.0f;
    const uint64_t requestId =
        latestSeekRequest_.fetch_add(1, std::memory_order_acq_rel) + 1;
    engine->playerRequestSeek(requestId);
    emit seekStarted(requestId);
    emit seeked();

    // Electron: playerSetDriver, then the re-seek — no requirements re-apply.
    const uint32_t mask = historyMask_;
    const float window = historyWindowSeconds_ < 0.0f ? 0.0f : historyWindowSeconds_;
    const bool allHistory = historyWindowSeconds_ < 0.0f;
    post(WorkKind::Driver,
         [this, driverIndex, useRecordedRows, progress, requestId, mask, window,
          allHistory] {
        if (auto* current = engine_.load(std::memory_order_acquire)) {
            current->playerSetDriver(driverIndex, useRecordedRows);
            current->playerSeek(progress, allHistory, requestId, mask, window);
        }
    }, true);
}

void TnrdPlayer::setFocusDriver(int driverIndex) {
    tnrp::Engine* engine = engine_.load(std::memory_order_acquire);
    if (!engine || !loaded_) return;
    // Queued behind any seek or driver change, which re-prime the same lanes;
    // only the newest selection matters.
    post(WorkKind::Focus, [this, driverIndex] {
        if (auto* current = engine_.load(std::memory_order_acquire))
            current->playerSetFocusDriver(driverIndex);
    }, true);
}

void TnrdPlayer::setDataRequirements(uint32_t streamMask, uint32_t historyMask,
                                     float windowSeconds,
                                     std::vector<uint8_t> v6Types,
                                     std::vector<uint8_t> v6HistoryTypes) {
    if (requirementsApplied_ && streamMask_ == streamMask && historyMask_ == historyMask &&
        historyWindowSeconds_ == windowSeconds && v6Types_ == v6Types &&
        v6HistoryTypes_ == v6HistoryTypes) return;
    streamMask_ = streamMask;
    historyMask_ = historyMask;
    historyWindowSeconds_ = windowSeconds;
    v6Types_ = std::move(v6Types);
    v6HistoryTypes_ = std::move(v6HistoryTypes);
    requirementsApplied_ = true;
    tnrp::Engine* engine = engine_.load(std::memory_order_acquire);
    if (!engine) return;
    const uint64_t requestId =
        latestRequirementsRequest_.fetch_add(1, std::memory_order_acq_rel) + 1;
    engine->requestDataRequirements(requestId);
    post(WorkKind::Requirements,
         [this, streamMask, historyMask, windowSeconds, requestId,
          v6Types = v6Types_, v6HistoryTypes = v6HistoryTypes_] {
        tnrp::Engine* current = engine_.load(std::memory_order_acquire);
        if (!current) return;
        current->setDataRequirements(streamMask, historyMask, windowSeconds, requestId,
                                     v6Types, v6HistoryTypes);
        if (!loaded_ || historyMask == 0 ||
            !catalogReady_.load(std::memory_order_acquire) ||
            requestId != latestRequirementsRequest_.load(std::memory_order_acquire)) return;
        if (windowSeconds < 0.0f)
            current->playerGetAllLapsData(requestId, historyMask);
        else
            current->playerGetWindowData(windowSeconds, requestId, historyMask);
    }, true);
}

void TnrdPlayer::requestLapData(int lapNum, uint32_t rowTypeMask) {
    if (lapNum <= 0 || !loaded_) return;
    post(WorkKind::LapData, [this, lapNum, rowTypeMask] {
        if (auto* current = engine_.load(std::memory_order_acquire))
            current->playerGetLapData(lapNum, rowTypeMask);
    }, false);
}

void TnrdPlayer::requestAnalysisLapData(uint64_t generation, int driverIndex,
                                        int lapNum, uint32_t rowTypeMask) {
    if (lapNum <= 0 || !loaded_) return;
    post(WorkKind::AnalysisLapData,
         [this, generation, driverIndex, lapNum, rowTypeMask] {
        auto* current = engine_.load(std::memory_order_acquire);
        if (!current) return;
        const std::string encoded = current->playerGetAnalysisLapData(
            lapNum, rowTypeMask, driverIndex);
        auto batch = encoded.empty() ? std::shared_ptr<PlaybackHistoryBatch>{}
            : decodeLapData(QByteArray(encoded.data(),
                                      static_cast<qsizetype>(encoded.size())));
        QMetaObject::invokeMethod(this,
            [this, generation, driverIndex, lapNum, rowTypeMask,
             batch = std::move(batch)] {
                if (loaded_)
                    emit analysisLapDecoded(generation, driverIndex, lapNum,
                                            rowTypeMask, batch);
            }, Qt::QueuedConnection);
    }, false);
}

bool TnrdPlayer::handleControlRow(const QByteArray& json) {
    const std::string_view source(json.constData(), static_cast<size_t>(json.size()));
    const std::string_view type = typeOf(source);
    if (type == "playback_loaded") {
        tnrp::PlaybackLoadedRow row;
        if (glz::read_json(row, source)) return true;
        if (row.ok && row.header) {
            loading_ = false;
            loaded_ = true;
            catalogReady_ = false;
            playing_ = false;
            requirementsApplied_ = false;
            emit loaded(*row.header);
        }
        return true;
    }
    if (type == "playback_lap_blocks") {
        tnrp::PlaybackLapBlocksRow row;
        if (!glz::read_json(row, source)) {
            lapRanges_.clear();
            lapRanges_.reserve(static_cast<qsizetype>(row.blocks.size()));
            for (const auto& block : row.blocks)
                lapRanges_.push_back({block.lapNum, block.startSessionTime,
                                      block.endSessionTime});
            catalogReady_.store(true, std::memory_order_release);
            emit lapBlocksReady(row);
            requirementsApplied_ = false;
            setDataRequirements(streamMask_, historyMask_, historyWindowSeconds_,
                                v6Types_, v6HistoryTypes_);
        }
        return true;
    }
    if (type == "playback_state") {
        tnrp::PlaybackStateRow row;
        if (glz::read_json(row, source)) return true;
        startTime_ = row.start_time;
        totalTime_ = row.start_time + row.total_time;
        currentTime_ = row.start_time + row.current_time;
        speed_ = row.speed;
        playing_ = row.playing;
        emit stateChanged(playing_, row.current_time, row.total_time, speed_);
        return true;
    }
    if (type == "playback_finished") {
        playing_ = false;
        emit finished();
        return true;
    }
    if (type == "playback_close") {
        loaded_ = false;
        catalogReady_ = false;
        loading_ = false;
        playing_ = false;
        startTime_ = totalTime_ = currentTime_ = 0.0f;
        lapRanges_.clear();
        emit closed();
        return true;
    }
    if (type == "playback_lap_data") {
        const QByteArray copy = json;
        post(WorkKind::LapDataDecode, [this, copy] {
            auto decoded = decodeLapData(copy);
            if (!decoded) return;
            QMetaObject::invokeMethod(this, [this, decoded = std::move(decoded)] {
                if (loaded_) emit historyDecoded(decoded);
            }, Qt::QueuedConnection);
        }, false);
        return true;
    }
    if (type == "driver_restriction") {
        tnrp::DriverRestrictionRow row;
        if (!glz::read_json(row, source))
            emit driverRestrictionChanged(row.driverIndex, row.restricted, row.known);
        return true;
    }
    if (type == "playback_seek_flush") return true;
    return false;
}

void TnrdPlayer::handleSeekFlush(const std::shared_ptr<EngineSeekFlush>& flush) {
    if (!flush) return;
    if (flush->authoritativeSeek) {
        if (flush->requestId != latestSeekRequest_.load(std::memory_order_acquire)) return;
    } else if (flush->requestId != 0 &&
               flush->requestId != latestRequirementsRequest_.load(std::memory_order_acquire)) {
        return;
    }
    const WorkKind decodeKind = flush->authoritativeSeek
        ? WorkKind::SeekDecode : WorkKind::RequirementsDecode;
    const QVector<PlaybackLapRange> lapRanges = lapRanges_;
    post(decodeKind, [this, flush, lapRanges] {
        auto decoded = decodeHistory(flush, lapRanges);
        if (!decoded) return;
        QMetaObject::invokeMethod(this, [this, decoded = std::move(decoded)] {
            if (decoded->authoritativeSeek) {
                if (decoded->requestId != latestSeekRequest_.load(std::memory_order_acquire)) return;
            } else if (decoded->requestId != 0 &&
                       decoded->requestId != latestRequirementsRequest_.load(std::memory_order_acquire)) {
                return;
            }
            emit historyDecoded(decoded);
        }, Qt::QueuedConnection);
    }, true);
}

void TnrdPlayer::completeSeek(uint64_t requestId) {
    if (requestId != latestSeekRequest_.load(std::memory_order_acquire)) return;
    if (resumeAfterSeek_) {
        resumeAfterSeek_ = false;
        // The seek worker has positioned the engine; its state row may still
        // be queued on the GUI thread. Do not recheck the old EOF timestamp.
        if (auto* engine = engine_.load(std::memory_order_acquire); engine && loaded_)
            engine->playerPlay();
    }
}

std::shared_ptr<PlaybackHistoryBatch>
TnrdPlayer::decodeHistory(const std::shared_ptr<EngineSeekFlush>& flush,
                          const QVector<PlaybackLapRange>& lapRanges) {
    auto result = std::make_shared<PlaybackHistoryBatch>();
    result->data.trimBuffers = false;
    result->currentLapStart = flush->currentLapStart;
    result->lapNum = flush->lapNum;
    result->additive = flush->allHistory || !flush->authoritativeSeek;
    result->authoritativeSeek = flush->authoritativeSeek;
    result->requestId = flush->requestId;
    result->rowTypeMask = flush->rowTypeMask;
    result->historyStart = flush->historyStart;

    const uint8_t* binary = nullptr;
    size_t binaryLength = 0;
    if (flush->binaryStore && flush->binaryBegin <= flush->binaryEnd &&
        flush->binaryEnd <= flush->binaryStore->size()) {
        binary = flush->binaryStore->data() + flush->binaryBegin;
        binaryLength = flush->binaryEnd - flush->binaryBegin;
    }
    // TNRD V6 with columnar history: typed column blocks straight from the
    // recording, or in live mode from the engine's V6 store, no JSON. The cold
    // JSON beside them holds only live race events, which history never shows.
    if (isV6HistoryPayload(binary, binaryLength)) {
        decodeColumnarHistory(*result, binary, binaryLength);
        sortAndCap(result->data.telBuf);
        sortAndCap(result->data.stsBuf);
        sortAndCap(result->data.motionBuf);
        sortAndCap(result->data.motionExBuf);
        sortAndCap(result->data.tyreBuf);
        sortAndCap(result->data.damageBuf);
        sortAndCap(result->progress);
        buildLapDetails(*result, lapRanges);
        return result;
    }
    if (binary) {
        tnrp::bin::decodeBatch(binary, binaryLength, [&result](auto&& row) {
            appendHistoryRow(*result, tnrp::AnyRow(std::move(row)));
        });
    }

    qsizetype offset = 0;
    PlaybackPatchMerger patchMerger;
    while (offset < flush->coldJson.size()) {
        qsizetype end = flush->coldJson.indexOf('\n', offset);
        if (end < 0) end = flush->coldJson.size();
        if (end > offset) {
            const QByteArray line = flush->coldJson.mid(offset, end - offset);
            if (auto decoded = patchMerger.decode(line))
                appendHistoryRow(*result, decoded->row, decoded->sparse);
        }
        offset = end + 1;
    }

    sortAndCap(result->data.telBuf);
    sortAndCap(result->data.stsBuf);
    sortAndCap(result->data.motionBuf);
    sortAndCap(result->data.motionExBuf);
    sortAndCap(result->data.tyreBuf);
    sortAndCap(result->data.damageBuf);
    sortAndCap(result->progress);
    buildLapDetails(*result, lapRanges);
    return result;
}

std::shared_ptr<PlaybackHistoryBatch> TnrdPlayer::decodeLapData(const QByteArray& json) {
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return {};
    const QJsonObject object = document.object();
    auto result = std::make_shared<PlaybackHistoryBatch>();
    result->data.trimBuffers = false;
    result->lapNum = object.value("lapNum").toInt();
    result->currentLapStart = static_cast<float>(object.value("startSessionTime").toDouble());
    result->historyStart = result->currentLapStart;
    result->rowTypeMask = static_cast<uint32_t>(object.value("rowTypeMask").toDouble());
    result->authoritativeSeek = false;
    result->additive = true;
    result->isolatedLapRequest = true;

    // Rows are merged and read as JSON objects directly. Re-serialising every
    // row just to parse it twice more (patch merge, then the typed parser) made
    // an indexed lap read dominate seeks and lap changes.
    PlaybackPatchMerger patchMerger;
    auto appendArray = [&result, &object, &patchMerger](const char* name) {
        const QJsonArray rows = object.value(QString::fromLatin1(name)).toArray();
        for (const QJsonValue& value : rows) {
            if (!value.isObject()) continue;
            bool sparse = false;
            const QJsonObject merged = patchMerger.mergeObject(value.toObject(), &sparse);
            appendHistoryObject(*result, merged, sparse);
        }
    };
    appendArray("telemetry");
    appendArray("statusHistory");
    appendArray("motionHistory");
    appendArray("motionExHistory");
    appendArray("damageHistory");

    for (const QJsonValue& value : object.value("lapProgress").toArray()) {
        const QJsonObject row = value.toObject();
        result->progress.push_back({
            static_cast<float>(row.value("session_time").toDouble()),
            row.value("current_lap_ms").toInt(),
            static_cast<float>(row.value("lap_distance_m").toDouble()),
            row.value("sector").toInt()
        });
    }
    QVector<LapPositionSample> positions;
    for (const QJsonValue& value : object.value("playerPositions").toArray()) {
        const QJsonObject row = value.toObject();
        positions.push_back({
            static_cast<float>(row.value("session_time").toDouble()),
            row.value("x").toDouble(), row.value("z").toDouble()
        });
    }
    sortAndCap(result->data.telBuf);
    sortAndCap(result->data.stsBuf);
    sortAndCap(result->data.motionBuf);
    sortAndCap(result->data.motionExBuf);
    sortAndCap(result->data.tyreBuf);
    sortAndCap(result->data.damageBuf);
    sortAndCap(result->progress);
    sortAndCap(positions);
    LapBlock detail;
    detail.lapNum = result->lapNum;
    detail.startSessionTime = result->currentLapStart;
    detail.endSessionTime = static_cast<float>(object.value("endSessionTime").toDouble());
    detail.tel = result->data.telBuf;
    detail.sts = result->data.stsBuf;
    detail.tyre = result->data.tyreBuf;
    detail.damage = result->data.damageBuf;
    detail.motion = result->data.motionBuf;
    detail.motionEx = result->data.motionExBuf;
    detail.progress = result->progress;
    detail.positions = std::move(positions);
    result->lapDetails.push_back(std::move(detail));
    return result;
}
