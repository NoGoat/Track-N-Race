#include "TnrdExporter.h"

#include "TNRD_V6.h"
#include "tnrp/ParquetWriter.h"

#include <glaze/glaze.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// TNRD_V6 / TNRD_V6.1 Parquet export. A V6 recording stores per-driver,
// per-lap chunks of typed sample columns (one V6 data type per chunk), a list
// of shared JSON records (session, participants, events, ...), and an index
// of drivers and laps. Everything is exported:
//
//  - One table per V6 data type, named in PascalCase from the stored type name
//    (g_force -> GForce.parquet, tyre_wear -> TyreWear.parquet), its
//    chunks in storage order. Columns: session_time, driver_index, lap_id,
//    lap_number, phase ("race" / "formation"), then every sample column any
//    of that type's chunks holds, in first-seen order; a chunk without a
//    column leaves it null. Stored kinds map Int -> INT64, Float32 -> FLOAT,
//    Float64 / Decimal -> DOUBLE, Bool -> BOOLEAN, Json -> JSON text. A name
//    stored with different kinds in different chunks widens to DOUBLE when
//    that is exact (floats, and integers within 2^53), else to JSON text.
//  - One table per shared record "type" (Session.parquet, ...): row_index,
//    session_time, phase, then the record's keys, discovered from the records
//    the same way the JSONL generations' exporters do (an array of cars,
//    drivers or tyre sets becomes one row per item).
//  - Laps.parquet: the lap index (lap_id joins the data tables).
//  - Drivers.parquet: the driver index, restriction changes and tyre stints
//    as JSON text.
namespace tnrp::detail {

namespace {

constexpr size_t kCheckEvery = 4096;
constexpr std::string_view kItemArrays[] = {"cars", "drivers", "sets"};
constexpr glz::opts kSlice{.null_terminated = false};
constexpr double kExactIntLimit = 9007199254740992.0;  // 2^53

const char* phaseName(V6Phase phase) { return phase == V6Phase::Formation ? "formation" : "race"; }

const char* settingName(TelemetrySetting setting) {
    switch (setting) {
        case TelemetrySetting::Restricted: return "restricted";
        case TelemetrySetting::Public:     return "public";
        default:                           return "unknown";
    }
}

void appendReal(std::string& out, double value) {
    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    out.append(buffer, result.ptr);
}

// ── Progress, cancellation and file naming shared by every table ────────────

class ExportProgress {
public:
    ExportProgress(const ParquetExportRequest& request, uint64_t totalUnits)
        : request_(request), total_(totalUnits ? static_cast<double>(totalUnits) : 1.0) {}

    // Advances by `units`; false once cancelled.
    bool advance(uint64_t units, const std::string& stage) {
        done_ += units;
        sinceCheck_ += units;
        if (sinceCheck_ < kCheckEvery) return true;
        sinceCheck_ = 0;
        if (request_.cancelled && request_.cancelled()) {
            cancelled_ = true;
            return false;
        }
        if (request_.progress) request_.progress(std::min(1.0, static_cast<double>(done_) / total_), stage);
        return true;
    }
    void report(double fraction, const std::string& stage) {
        if (request_.progress) request_.progress(fraction, stage);
    }

    std::string pathFor(const std::string& tableName) {
        std::string base = tableName.empty() ? "Untyped" : tableName;
        if (const int seen = usedNames_[base]++) base += "_" + std::to_string(seen + 1);
        return request_.directory + "/" + base + ".parquet";
    }

    bool cancelled() const { return cancelled_; }
    const ParquetExportRequest& request() const { return request_; }

private:
    const ParquetExportRequest& request_;
    double total_;
    uint64_t done_{};
    uint64_t sinceCheck_{};
    bool cancelled_{};
    std::unordered_map<std::string, int> usedNames_;
};

std::vector<parquet::KeyValue> tableMetadata(const std::string& format, const std::string& table,
                                             const std::string& header) {
    return {{"tnrd.format", format}, {"tnrd.table", table}, {"tnrd.header", header}};
}

std::string tableFileName(std::string_view type);  // snake_case -> PascalCase, below

// ── Data-type tables (typed chunks) ─────────────────────────────────────────

constexpr uint8_t SeenInt = 1, SeenFloat32 = 2, SeenFloat64 = 4, SeenBool = 8, SeenJson = 16;

struct SampleColumn {
    std::string name;
    uint8_t seen{};
    bool intBeyondDouble{};  // an Int value a double cannot hold exactly
    parquet::PhysicalType type{};
    bool asJson{};
    size_t writerColumn{};
};

uint8_t seenBit(V6ChunkColumns::Kind kind) {
    switch (kind) {
        case V6ChunkColumns::Kind::Int:     return SeenInt;
        case V6ChunkColumns::Kind::Float32: return SeenFloat32;
        case V6ChunkColumns::Kind::Float64: return SeenFloat64;
        case V6ChunkColumns::Kind::Bool:    return SeenBool;
        case V6ChunkColumns::Kind::Json:    return SeenJson;
    }
    return SeenJson;
}

void settle(SampleColumn& column) {
    const uint8_t seen = column.seen;
    const bool reals = seen & (SeenFloat32 | SeenFloat64);
    column.asJson = false;
    if (seen == SeenInt) {
        column.type = parquet::PhysicalType::Int64;
    } else if (seen == SeenBool) {
        column.type = parquet::PhysicalType::Boolean;
    } else if (seen == SeenFloat32) {
        column.type = parquet::PhysicalType::Float;
    } else if (reals && !(seen & (SeenBool | SeenJson)) && !((seen & SeenInt) && column.intBeyondDouble)) {
        column.type = parquet::PhysicalType::Double;
    } else {
        column.type = parquet::PhysicalType::ByteArray;
        column.asJson = true;
    }
}

void renderJsonValue(std::string& out, const V6ChunkColumns::Column& column, size_t row) {
    out.clear();
    switch (column.kind) {
        case V6ChunkColumns::Kind::Int:  out = std::to_string(column.ints[row]); break;
        case V6ChunkColumns::Kind::Bool: out = column.ints[row] ? "true" : "false"; break;
        case V6ChunkColumns::Kind::Json: out = column.texts[row]; break;
        default:                         appendReal(out, column.reals[row]); break;
    }
}

class DataTypeTables {
public:
    DataTypeTables(TnrdV6Archive& archive, ExportProgress& session, std::string format, std::string header)
        : archive_(archive), session_(session), format_(std::move(format)), header_(std::move(header)) {
        for (const auto& driver : archive_.driverHeaders())
            for (const auto& lap : archive_.driverLapSummaries(driver.vehicleIndex))
                lapNumbers_[lap.lapId] = lap.lapNumber;
    }

    bool run(std::string* errorOut) {
        // Chunks per data type, each list in storage (sequence) order; types in
        // the order their first chunk was stored.
        const auto& chunks = archive_.v6Chunks();
        std::vector<size_t> order(chunks.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(),
                         [&](size_t a, size_t b) { return chunks[a].sequence < chunks[b].sequence; });
        std::vector<uint8_t> types;
        std::map<uint8_t, std::vector<size_t>> byType;
        for (size_t index : order) {
            const uint8_t type = chunks[index].typeId;
            if (byType[type].empty()) types.push_back(type);
            byType[type].push_back(index);
        }
        for (uint8_t type : types)
            if (!writeType(type, byType[type], errorOut)) return false;
        return true;
    }

private:
    // The stored type name ("g_force"), recorded as tnrd.table.
    std::string typeName(uint8_t type) const {
        const char* name = v6TypeName(static_cast<V6DataType>(type));
        return name && *name ? std::string(name) : "type_" + std::to_string(type);
    }

    bool load(size_t index, std::string* errorOut) {
        if (!archive_.loadChunkColumns(index, chunk_, errorOut)) {
            if (errorOut && errorOut->empty()) *errorOut = "could not read a V6 chunk";
            return false;
        }
        if (chunk_.time.size() != archive_.v6Chunks()[index].sampleCount) {
            if (errorOut) *errorOut = "a V6 chunk holds a different number of samples than its index says";
            return false;
        }
        return true;
    }

    bool writeType(uint8_t type, const std::vector<size_t>& indices, std::string* errorOut) {
        const std::string name = typeName(type);
        const std::string fileName = tableFileName(name);  // GForce.parquet
        const std::string stage = "Writing " + fileName;

        // Pass 1: the union of the type's sample columns.
        std::vector<SampleColumn> columns;
        std::unordered_map<std::string, size_t> byName;
        for (size_t index : indices) {
            if (!load(index, errorOut)) return false;
            for (const auto& source : chunk_.columns) {
                auto [it, added] = byName.emplace(source.name, columns.size());
                if (added) columns.push_back(SampleColumn{source.name});
                SampleColumn& column = columns[it->second];
                column.seen |= seenBit(source.kind);
                if (source.kind == V6ChunkColumns::Kind::Int && !column.intBeyondDouble)
                    for (size_t row = 0; row < chunk_.time.size(); ++row)
                        if (source.has(row) && std::fabs(static_cast<double>(source.ints[row])) > kExactIntLimit) {
                            column.intBeyondDouble = true;
                            break;
                        }
            }
            if (!session_.advance(chunk_.time.size(), stage)) return cancelled(errorOut);
        }

        std::vector<parquet::Column> schema{
            {"session_time", parquet::PhysicalType::Float, parquet::TextTag::None, false},
            {"driver_index", parquet::PhysicalType::Int32, parquet::TextTag::None, false},
            {"lap_id", parquet::PhysicalType::Int64, parquet::TextTag::None, false},
            {"lap_number", parquet::PhysicalType::Int64, parquet::TextTag::None, true},
            {"phase", parquet::PhysicalType::ByteArray, parquet::TextTag::String, false},
        };
        for (SampleColumn& column : columns) {
            settle(column);
            column.writerColumn = schema.size();
            const std::string columnName = byNameTaken(schema, column.name) ? "sample_" + column.name : column.name;
            schema.push_back({columnName, column.type,
                              column.asJson ? parquet::TextTag::Json : parquet::TextTag::None, true});
        }

        parquet::TableWriter writer;
        if (!writer.open(session_.pathFor(fileName), std::move(schema), tableMetadata(format_, name, header_),
                         errorOut))
            return false;

        // Pass 2: the rows.
        std::vector<size_t> slots;
        std::string json;
        for (size_t index : indices) {
            if (!load(index, errorOut)) return abandon(writer);
            const V6ChunkInfo& info = archive_.v6Chunks()[index];
            const auto lap = lapNumbers_.find(info.lapId);
            slots.clear();
            for (const auto& source : chunk_.columns) slots.push_back(byName.at(source.name));
            for (size_t row = 0; row < chunk_.time.size(); ++row) {
                writer.setFloat(0, chunk_.time[row]);
                writer.setInt32(1, info.driverIndex);
                writer.setInt64(2, info.lapId);
                if (lap != lapNumbers_.end()) writer.setInt64(3, lap->second);
                writer.setBytes(4, phaseName(info.phase));
                for (size_t c = 0; c < chunk_.columns.size(); ++c) {
                    const auto& source = chunk_.columns[c];
                    if (!source.has(row)) continue;
                    const SampleColumn& target = columns[slots[c]];
                    if (target.asJson) {
                        renderJsonValue(json, source, row);
                        writer.setBytes(target.writerColumn, json);
                        continue;
                    }
                    switch (target.type) {
                        case parquet::PhysicalType::Int64:
                            writer.setInt64(target.writerColumn, source.ints[row]);
                            break;
                        case parquet::PhysicalType::Boolean:
                            writer.setBool(target.writerColumn, source.ints[row] != 0);
                            break;
                        case parquet::PhysicalType::Float:
                            writer.setFloat(target.writerColumn, static_cast<float>(source.reals[row]));
                            break;
                        default:  // Double: reals, or integers within 2^53
                            writer.setDouble(target.writerColumn,
                                             source.kind == V6ChunkColumns::Kind::Int
                                                 ? static_cast<double>(source.ints[row])
                                                 : source.reals[row]);
                            break;
                    }
                }
                if (!writer.endRow(errorOut)) return abandon(writer);
            }
            if (!session_.advance(chunk_.time.size(), stage)) {
                cancelled(errorOut);
                return abandon(writer);
            }
        }
        return writer.close(errorOut);
    }

    static bool byNameTaken(const std::vector<parquet::Column>& schema, const std::string& name) {
        for (const auto& column : schema)
            if (column.name == name) return true;
        return false;
    }

    bool cancelled(std::string* errorOut) {
        if (errorOut) *errorOut = "cancelled";
        return false;
    }
    static bool abandon(parquet::TableWriter& writer) {
        writer.abort();
        return false;
    }

    TnrdV6Archive& archive_;
    ExportProgress& session_;
    std::string format_;
    std::string header_;
    std::unordered_map<uint32_t, uint32_t> lapNumbers_;
    V6ChunkColumns chunk_;
};

// ── Shared-record tables (JSON) ─────────────────────────────────────────────

struct Member {
    std::string_view key;
    std::string_view raw;
};

void skipSpace(std::string_view s, size_t& i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
}

bool skipString(std::string_view s, size_t& i) {
    ++i;
    while (i < s.size()) {
        const char c = s[i++];
        if (c == '\\') {
            if (i >= s.size()) return false;
            ++i;
        } else if (c == '"') {
            return true;
        }
    }
    return false;
}

bool skipValue(std::string_view s, size_t& i) {
    if (i >= s.size()) return false;
    const char first = s[i];
    if (first == '"') return skipString(s, i);
    if (first == '{' || first == '[') {
        int depth = 0;
        while (i < s.size()) {
            const char c = s[i];
            if (c == '"') {
                if (!skipString(s, i)) return false;
                continue;
            }
            if (c == '{' || c == '[') {
                ++depth;
            } else if (c == '}' || c == ']') {
                if (--depth == 0) {
                    ++i;
                    return true;
                }
            }
            ++i;
        }
        return false;
    }
    const size_t start = i;
    while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']' && s[i] != ' ' &&
           s[i] != '\t' && s[i] != '\r' && s[i] != '\n')
        ++i;
    return i > start;
}

bool splitObject(std::string_view s, std::vector<Member>& out) {
    out.clear();
    size_t i = 0;
    skipSpace(s, i);
    if (i >= s.size() || s[i] != '{') return false;
    ++i;
    skipSpace(s, i);
    if (i < s.size() && s[i] == '}') return true;
    while (i < s.size()) {
        skipSpace(s, i);
        if (i >= s.size() || s[i] != '"') return false;
        const size_t keyStart = i + 1;
        if (!skipString(s, i)) return false;
        const std::string_view key = s.substr(keyStart, i - 1 - keyStart);
        skipSpace(s, i);
        if (i >= s.size() || s[i] != ':') return false;
        ++i;
        skipSpace(s, i);
        const size_t valueStart = i;
        if (!skipValue(s, i)) return false;
        out.push_back({key, s.substr(valueStart, i - valueStart)});
        skipSpace(s, i);
        if (i < s.size() && s[i] == ',') {
            ++i;
            continue;
        }
        return i < s.size() && s[i] == '}';
    }
    return false;
}

bool splitArray(std::string_view s, std::vector<std::string_view>& out) {
    out.clear();
    size_t i = 0;
    skipSpace(s, i);
    if (i >= s.size() || s[i] != '[') return false;
    ++i;
    skipSpace(s, i);
    if (i < s.size() && s[i] == ']') return true;
    while (i < s.size()) {
        skipSpace(s, i);
        const size_t start = i;
        if (!skipValue(s, i)) return false;
        out.push_back(s.substr(start, i - start));
        skipSpace(s, i);
        if (i < s.size() && s[i] == ',') {
            ++i;
            continue;
        }
        return i < s.size() && s[i] == ']';
    }
    return false;
}

enum Kind : uint8_t {
    KindNull = 0, KindBool = 1, KindInt = 2, KindFloat = 4, KindString = 8,
    KindNested = 16, KindOther = 32,
};

bool readInt(std::string_view raw, int64_t& out) {
    const char* end = raw.data() + raw.size();
    const auto result = std::from_chars(raw.data(), end, out);
    return result.ec == std::errc() && result.ptr == end;
}

bool readDouble(std::string_view raw, double& out) {
    return !glz::read<kSlice>(out, raw) && std::isfinite(out);
}

uint8_t classify(std::string_view raw) {
    switch (raw.front()) {
        case '"': return KindString;
        case 'n': return KindNull;
        case 't': case 'f': return KindBool;
        case '{': case '[': return KindNested;
        default: break;
    }
    // Decided from the text; a full parse only where the value may not fit
    // exactly: integers of 19+ digits, decimals with 31+ digits or a 3+ digit
    // exponent. (The write pass parses every value and fails on a bad one.)
    bool decimal = false, exponent = false;
    size_t digits = 0, exponentDigits = 0;
    for (const char c : raw) {
        if (c >= '0' && c <= '9') {
            ++(exponent ? exponentDigits : digits);
        } else if (c == '.') {
            decimal = true;
        } else if (c == 'e' || c == 'E') {
            decimal = exponent = true;
        } else if (c != '-' && c != '+') {
            return KindOther;
        }
    }
    if (digits == 0) return KindOther;
    if (!decimal) {
        if (digits <= 18) return KindInt;
        int64_t value;
        return readInt(raw, value) ? KindInt : KindOther;
    }
    if (digits <= 30 && exponentDigits <= 2) return KindFloat;
    double value;
    return readDouble(raw, value) ? KindFloat : KindOther;
}

bool isItemArray(std::string_view key) {
    for (std::string_view name : kItemArrays)
        if (key == name) return true;
    return false;
}

struct JsonColumn {
    std::string key;
    uint8_t kinds{};
    parquet::PhysicalType type{parquet::PhysicalType::ByteArray};
    parquet::TextTag text{parquet::TextTag::String};
    bool rawJson{};
    size_t writerColumn{};
};

struct JsonColumnSet {
    std::vector<JsonColumn> columns;
    std::unordered_map<std::string, size_t> byKey;
    std::vector<size_t> lastOrder;
    bool frozen{};
    static constexpr size_t npos = static_cast<size_t>(-1);

    size_t find(size_t position, std::string_view key) {
        if (position < lastOrder.size() && columns[lastOrder[position]].key == key) return lastOrder[position];
        const auto it = byKey.find(std::string(key));
        size_t slot;
        if (it != byKey.end()) {
            slot = it->second;
        } else if (frozen) {
            return npos;
        } else {
            slot = columns.size();
            columns.push_back(JsonColumn{std::string(key)});
            byKey.emplace(columns.back().key, slot);
        }
        if (position >= lastOrder.size()) lastOrder.resize(position + 1, 0);
        lastOrder[position] = slot;
        return slot;
    }
};

void settleJson(JsonColumn& column) {
    const uint8_t kinds = column.kinds & ~KindNull;
    const int categories = ((kinds & KindBool) ? 1 : 0) + ((kinds & (KindInt | KindFloat)) ? 1 : 0) +
                           ((kinds & KindString) ? 1 : 0);
    column.rawJson = false;
    column.text = parquet::TextTag::None;
    if (kinds == 0) {
        column.type = parquet::PhysicalType::ByteArray;
        column.text = parquet::TextTag::String;
    } else if ((kinds & (KindNested | KindOther)) || categories > 1) {
        column.type = parquet::PhysicalType::ByteArray;
        column.text = parquet::TextTag::Json;
        column.rawJson = true;
    } else if (kinds == KindBool) {
        column.type = parquet::PhysicalType::Boolean;
    } else if (kinds == KindInt) {
        column.type = parquet::PhysicalType::Int64;
    } else if (kinds & (KindInt | KindFloat)) {
        column.type = parquet::PhysicalType::Double;
    } else {
        column.type = parquet::PhysicalType::ByteArray;
        column.text = parquet::TextTag::String;
    }
}

std::string tableFileName(std::string_view type) {
    std::string name;
    bool upper = true;
    for (const char c : type) {
        if (c == '_' || c == '-' || c == ' ') {
            upper = true;
        } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            name.push_back(upper && c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c);
            upper = false;
        }
    }
    return name.empty() ? "Untyped" : name;
}

bool writeJsonValue(parquet::TableWriter& writer, const JsonColumn& column, std::string_view raw) {
    if (raw == "null") return true;
    const size_t at = column.writerColumn;
    if (column.rawJson) {
        writer.setBytes(at, raw);
        return true;
    }
    switch (column.type) {
        case parquet::PhysicalType::Boolean:
            writer.setBool(at, raw.front() == 't');
            return true;
        case parquet::PhysicalType::Int64: {
            int64_t value;
            if (!readInt(raw, value)) return false;
            writer.setInt64(at, value);
            return true;
        }
        case parquet::PhysicalType::Double: {
            double value;
            if (!readDouble(raw, value)) return false;
            writer.setDouble(at, value);
            return true;
        }
        default: {
            std::string text;
            if (glz::read<kSlice>(text, raw)) return false;
            writer.setBytes(at, text);
            return true;
        }
    }
}

struct RecordTable {
    std::string type;
    bool unparsed{};
    JsonColumnSet packet;
    JsonColumnSet items;
    std::string itemKey;
    bool itemsUsable{true};
    uint64_t rows{};
    std::unique_ptr<parquet::TableWriter> writer;
};

// Column order: row_index, session_time, phase, then the record's own keys.
constexpr size_t kRowIndexColumn = 0, kTimeColumn = 1, kPhaseColumn = 2, kFirstRecordColumn = 3;

class SharedRecordTables {
public:
    SharedRecordTables(TnrdV6Archive& archive, ExportProgress& session, std::string format, std::string header)
        : records_(archive.sharedRecords()), session_(session), format_(std::move(format)),
          header_(std::move(header)) {}

    bool run(std::string* errorOut) {
        if (records_.empty()) return true;
        discover();
        if (!openWriters(errorOut)) return false;
        if (!writeRecords(errorOut)) return fail(errorOut);
        for (RecordTable& table : tables_)
            if (!table.writer->close(errorOut)) return fail(errorOut);
        return true;
    }

private:
    bool fail(std::string* errorOut) {
        if (errorOut && errorOut->empty())
            *errorOut = session_.cancelled() ? "cancelled" : "could not write the shared records";
        for (RecordTable& table : tables_)
            if (table.writer) table.writer->abort();
        return false;
    }

    RecordTable& tableFor(std::string_view type, bool unparsed) {
        const std::string key = unparsed ? std::string("\x01") : std::string(type);
        const auto it = tableIndex_.find(key);
        if (it != tableIndex_.end()) return tables_[it->second];
        tableIndex_.emplace(key, tables_.size());
        tables_.emplace_back();
        RecordTable& table = tables_.back();
        table.type = unparsed ? std::string() : std::string(type);
        table.unparsed = unparsed;
        return table;
    }

    RecordTable& tableForRecord(const std::vector<Member>& members) {
        for (const Member& member : members) {
            if (member.key != "type") continue;
            std::string type;
            if (member.raw.front() == '"' && !glz::read<kSlice>(type, member.raw)) return tableFor(type, false);
            break;
        }
        return tableFor({}, false);
    }

    void noteItems(RecordTable& table, std::string_view raw) {
        if (raw == "null") return;  // no items this time; written as one row
        if (raw.front() != '[' || !splitArray(raw, elements_)) {
            table.itemsUsable = false;
            return;
        }
        for (std::string_view element : elements_) {
            if (element.front() != '{' || !splitObject(element, itemMembers_)) {
                table.itemsUsable = false;
                return;
            }
            for (size_t m = 0; m < itemMembers_.size(); ++m)
                table.items.columns[table.items.find(m, itemMembers_[m].key)].kinds |=
                    classify(itemMembers_[m].raw);
        }
    }

    void discover() {
        for (const V6SharedRecord& record : records_) {
            if (!splitObject(record.json, members_)) {
                tableFor({}, true);
                continue;
            }
            RecordTable& table = tableForRecord(members_);
            for (size_t m = 0; m < members_.size(); ++m) {
                const Member& member = members_[m];
                if (member.key == "type" || member.key == "session_time") continue;
                table.packet.columns[table.packet.find(m, member.key)].kinds |= classify(member.raw);
                if (!isItemArray(member.key)) continue;
                if (table.itemKey.empty()) table.itemKey = std::string(member.key);
                if (member.key == table.itemKey && table.itemsUsable) noteItems(table, member.raw);
            }
        }
    }

    bool openWriters(std::string* errorOut) {
        for (RecordTable& table : tables_) {
            std::vector<parquet::Column> columns{
                {"row_index", parquet::PhysicalType::Int64, parquet::TextTag::None, false},
                {"session_time", parquet::PhysicalType::Float, parquet::TextTag::None, false},
                {"phase", parquet::PhysicalType::ByteArray, parquet::TextTag::String, false},
            };
            if (table.unparsed) {
                columns.push_back({"record", parquet::PhysicalType::ByteArray, parquet::TextTag::String, false});
            } else {
                const bool exploding = !table.itemKey.empty() && table.itemsUsable;
                std::unordered_map<std::string, bool> taken{
                    {"row_index", true}, {"session_time", true}, {"phase", true}};
                for (JsonColumn& column : table.packet.columns) {
                    if (exploding && column.key == table.itemKey) continue;
                    settleJson(column);
                    column.writerColumn = columns.size();
                    const std::string name = taken.count(column.key) ? "record_" + column.key : column.key;
                    columns.push_back({name, column.type, column.text, true});
                    taken[name] = true;
                }
                if (exploding) {
                    for (JsonColumn& column : table.items.columns) {
                        settleJson(column);
                        column.writerColumn = columns.size();
                        const std::string name = taken.count(column.key) ? table.itemKey + "_" + column.key
                                                                         : column.key;
                        columns.push_back({name, column.type, column.text, true});
                    }
                }
            }
            table.packet.frozen = true;
            table.items.frozen = true;
            const std::string tableName = table.unparsed ? "UnparsedRecords" : tableFileName(table.type);
            table.writer = std::make_unique<parquet::TableWriter>();
            if (!table.writer->open(session_.pathFor(tableName), std::move(columns),
                                    tableMetadata(format_, table.unparsed ? "(unparsed)" : table.type, header_),
                                    errorOut))
                return fail(errorOut);
        }
        return true;
    }

    bool writePacketFields(RecordTable& table, bool exploding) {
        for (size_t m = 0; m < members_.size(); ++m) {
            const Member& member = members_[m];
            if (member.key == "type" || member.key == "session_time") continue;
            if (exploding && member.key == table.itemKey) continue;
            const size_t slot = table.packet.find(m, member.key);
            if (slot == JsonColumnSet::npos || !writeJsonValue(*table.writer, table.packet.columns[slot], member.raw))
                return false;
        }
        return true;
    }

    bool writeRecords(std::string* errorOut) {
        for (const V6SharedRecord& record : records_) {
            if (!session_.advance(1, "Writing shared records")) return false;
            if (!splitObject(record.json, members_)) {
                RecordTable& table = tableFor({}, true);
                parquet::TableWriter& writer = *table.writer;
                writer.setInt64(kRowIndexColumn, static_cast<int64_t>(table.rows++));
                writer.setFloat(kTimeColumn, record.sessionTime);
                writer.setBytes(kPhaseColumn, phaseName(record.phase));
                writer.setBytes(kFirstRecordColumn, record.json);
                if (!writer.endRow(errorOut)) return false;
                continue;
            }
            RecordTable& table = tableForRecord(members_);
            parquet::TableWriter& writer = *table.writer;
            const int64_t rowIndex = static_cast<int64_t>(table.rows++);
            const bool exploding = !table.itemKey.empty() && table.itemsUsable;
            std::string_view itemsRaw;
            if (exploding)
                for (const Member& member : members_)
                    if (member.key == table.itemKey) itemsRaw = member.raw;
            if (!itemsRaw.empty() && itemsRaw != "null") {
                if (!splitArray(itemsRaw, elements_)) return false;
            } else {
                elements_.clear();
            }
            const size_t outputRows = std::max<size_t>(1, elements_.size());
            for (size_t e = 0; e < outputRows; ++e) {
                writer.setInt64(kRowIndexColumn, rowIndex);
                writer.setFloat(kTimeColumn, record.sessionTime);
                writer.setBytes(kPhaseColumn, phaseName(record.phase));
                if (!writePacketFields(table, exploding)) return false;
                if (e < elements_.size()) {
                    if (!splitObject(elements_[e], itemMembers_)) return false;
                    for (size_t m = 0; m < itemMembers_.size(); ++m) {
                        const size_t slot = table.items.find(m, itemMembers_[m].key);
                        if (slot == JsonColumnSet::npos ||
                            !writeJsonValue(writer, table.items.columns[slot], itemMembers_[m].raw))
                            return false;
                    }
                }
                if (!writer.endRow(errorOut)) return false;
            }
        }
        return true;
    }

    const std::vector<V6SharedRecord>& records_;
    ExportProgress& session_;
    std::string format_;
    std::string header_;
    std::vector<RecordTable> tables_;
    std::unordered_map<std::string, size_t> tableIndex_;
    std::vector<Member> members_;
    std::vector<Member> itemMembers_;
    std::vector<std::string_view> elements_;
};

// ── Index tables ────────────────────────────────────────────────────────────

bool writeLaps(TnrdV6Archive& archive, ExportProgress& session, const std::string& format, const std::string& header,
               std::string* errorOut) {
    std::vector<parquet::Column> columns{
        {"lap_id", parquet::PhysicalType::Int64, parquet::TextTag::None, false},
        {"driver_index", parquet::PhysicalType::Int32, parquet::TextTag::None, false},
        {"lap_number", parquet::PhysicalType::Int64, parquet::TextTag::None, false},
        {"phase", parquet::PhysicalType::ByteArray, parquet::TextTag::String, false},
        {"start_session_time", parquet::PhysicalType::Float, parquet::TextTag::None, false},
        {"end_session_time", parquet::PhysicalType::Float, parquet::TextTag::None, false},
        {"lap_time_ms", parquet::PhysicalType::Int64, parquet::TextTag::None, false},
        {"s1_ms", parquet::PhysicalType::Int64, parquet::TextTag::None, false},
        {"s2_ms", parquet::PhysicalType::Int64, parquet::TextTag::None, false},
        {"s3_ms", parquet::PhysicalType::Int64, parquet::TextTag::None, false},
        {"is_completed", parquet::PhysicalType::Boolean, parquet::TextTag::None, false},
        {"is_valid", parquet::PhysicalType::Boolean, parquet::TextTag::None, false},
        {"is_partial", parquet::PhysicalType::Boolean, parquet::TextTag::None, false},
    };
    parquet::TableWriter writer;
    if (!writer.open(session.pathFor("Laps"), std::move(columns), tableMetadata(format, "(laps)", header),
                     errorOut))
        return false;
    for (const auto& driver : archive.driverHeaders()) {
        for (const V6LapSummary& lap : archive.driverLapSummaries(driver.vehicleIndex)) {
            writer.setInt64(0, lap.lapId);
            writer.setInt32(1, lap.driverIndex);
            writer.setInt64(2, lap.lapNumber);
            writer.setBytes(3, phaseName(lap.phase));
            writer.setFloat(4, lap.startSessionTime);
            writer.setFloat(5, lap.endSessionTime);
            writer.setInt64(6, lap.lapTimeMs);
            writer.setInt64(7, lap.s1Ms);
            writer.setInt64(8, lap.s2Ms);
            writer.setInt64(9, lap.s3Ms);
            writer.setBool(10, lap.isCompleted);
            writer.setBool(11, lap.isValid);
            writer.setBool(12, lap.isPartial);
            if (!writer.endRow(errorOut)) {
                writer.abort();
                return false;
            }
        }
    }
    return writer.close(errorOut);
}

bool writeDrivers(TnrdV6Archive& archive, ExportProgress& session, const std::string& format, const std::string& header,
                  std::string* errorOut) {
    std::vector<parquet::Column> columns{
        {"driver_index", parquet::PhysicalType::Int32, parquet::TextTag::None, false},
        {"driver_name", parquet::PhysicalType::ByteArray, parquet::TextTag::String, false},
        {"team_id", parquet::PhysicalType::Int32, parquet::TextTag::None, false},
        {"race_number", parquet::PhysicalType::Int32, parquet::TextTag::None, false},
        {"is_player", parquet::PhysicalType::Boolean, parquet::TextTag::None, false},
        {"initial_telemetry_setting", parquet::PhysicalType::ByteArray, parquet::TextTag::String, false},
        {"available_type_mask", parquet::PhysicalType::Int64, parquet::TextTag::None, false},
        {"restriction_changes", parquet::PhysicalType::ByteArray, parquet::TextTag::Json, false},
        {"tyre_stints", parquet::PhysicalType::ByteArray, parquet::TextTag::Json, false},
    };
    parquet::TableWriter writer;
    if (!writer.open(session.pathFor("Drivers"), std::move(columns), tableMetadata(format, "(drivers)", header),
                     errorOut))
        return false;
    std::string json;
    for (const V6DriverHeader& driver : archive.driverHeaders()) {
        writer.setInt32(0, driver.vehicleIndex);
        writer.setBytes(1, driver.driverName);
        writer.setInt32(2, driver.teamId);
        writer.setInt32(3, driver.raceNumber);
        writer.setBool(4, driver.isPlayer);
        writer.setBytes(5, settingName(driver.initialTelemetrySetting));
        // The mask is a bit set; its 64 bits are kept as they are.
        writer.setInt64(6, static_cast<int64_t>(driver.availableTypeMask));

        json = "[";
        for (size_t i = 0; i < driver.restrictionChanges.size(); ++i) {
            const V6RestrictionChange& change = driver.restrictionChanges[i];
            if (i) json += ',';
            json += "{\"phase\":\"";
            json += phaseName(change.phase);
            json += "\",\"session_time\":";
            appendReal(json, change.sessionTime);
            json += ",\"setting\":\"";
            json += settingName(change.setting);
            json += "\"}";
        }
        json += ']';
        writer.setBytes(7, json);

        json = "[";
        for (size_t i = 0; i < driver.tyreStints.size(); ++i) {
            const V6TyreStintSummary& stint = driver.tyreStints[i];
            if (i) json += ',';
            json += "{\"end_lap\":" + std::to_string(stint.endLap) +
                    ",\"actual_compound\":" + std::to_string(stint.actualCompound) +
                    ",\"visual_compound\":" + std::to_string(stint.visualCompound) + "}";
        }
        json += ']';
        writer.setBytes(8, json);

        if (!writer.endRow(errorOut)) {
            writer.abort();
            return false;
        }
    }
    return writer.close(errorOut);
}

} // namespace

bool exportTnrdV6Parquet(TnrdV6Archive& archive, const ParquetExportRequest& request, std::string* errorOut) {
    const std::string format = archive.revision() ? "TNRD_V6.1" : "TNRD_V6";
    std::string header;
    writeJson(request.header, header);

    // Each chunk sample is visited twice (columns, then rows); records once.
    uint64_t units = archive.sharedRecords().size();
    for (const auto& chunk : archive.v6Chunks()) units += 2ull * chunk.sampleCount;
    ExportProgress session(request, units);
    session.report(0.0, "Reading chunks");

    DataTypeTables data(archive, session, format, header);
    if (!data.run(errorOut)) return false;
    SharedRecordTables shared(archive, session, format, header);
    if (!shared.run(errorOut)) return false;
    session.report(0.99, "Writing laps and drivers");
    if (!writeLaps(archive, session, format, header, errorOut)) return false;
    if (!writeDrivers(archive, session, format, header, errorOut)) return false;
    session.report(1.0, "Done");
    return true;
}

} // namespace tnrp::detail
