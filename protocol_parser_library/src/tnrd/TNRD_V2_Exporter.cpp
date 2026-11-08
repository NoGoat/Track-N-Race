#include "TnrdExporter.h"

#include "tnrp/ParquetWriter.h"

#include <glaze/glaze.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

// TNRD_V2 Parquet export. A V2 recording is zstd JSONL: one JSON object per
// row, its kind in "type". Every row is exported, one table per distinct
// "type" value, in the order the types first appear:
//
//  - Columns are discovered from the rows (a first pass over the recording),
//    so a key is never dropped because no one listed it. A column's Parquet
//    type follows the JSON values it holds: only true/false -> BOOLEAN, only
//    integers -> INT64, numbers with any fraction -> DOUBLE, only strings ->
//    STRING, and anything else (objects, arrays, mixed kinds) -> the value's
//    JSON text, tagged JSON. Missing and null values are Parquet nulls.
//  - A packet that lists several cars, drivers or tyre sets ("cars",
//    "drivers" or "sets": an array of objects) becomes one Parquet row per
//    item, the packet's own fields repeated on each. An item key that clashes
//    with a packet key is prefixed with the array's name.
//  - row_index numbers the recording's rows within their table, so the rows
//    exploded from one packet share it; session_time is the time the V2 index
//    gives the row (its own session_time, or the latest one before it for
//    rows that carry none).
//  - A line that is not a JSON object goes to the Unparsed table verbatim.
//
// Both passes run on batches of rows split across threads; results are
// combined in row order, so the files match a single-threaded export.
namespace tnrp::detail {

namespace {

constexpr const char* kFormatName = "TNRD_V2";
constexpr size_t kBatchRows = 8192;     // rows per batch
constexpr size_t kMinSliceRows = 256;   // fewer rows per thread are not worth a thread
constexpr size_t kMaxWorkers = 16;
constexpr bool kTimeNullable = false;
constexpr double kDiscoverShare = 0.4;  // progress share of the first pass
constexpr std::string_view kItemArrays[] = {"cars", "drivers", "sets"};
constexpr glz::opts kSlice{.null_terminated = false};

// ── JSON scanning ───────────────────────────────────────────────────────────
// Only splits objects and arrays into raw member texts; values are decoded
// when written.

struct Member {
    std::string_view key;  // as written (row keys are plain identifiers)
    std::string_view raw;
};

void skipSpace(std::string_view s, size_t& i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
}

bool skipString(std::string_view s, size_t& i) {
    ++i;  // opening quote
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

// ── Value kinds ─────────────────────────────────────────────────────────────

enum Kind : uint8_t {
    KindNull = 0, KindBool = 1, KindInt = 2, KindFloat = 4, KindString = 8,
    KindNested = 16, KindOther = 32,  // Other: a number no Parquet type holds exactly
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

// ── Tables ──────────────────────────────────────────────────────────────────

struct ColumnInfo {
    std::string key;
    uint8_t kinds{};
    // Settled after discovery:
    parquet::PhysicalType type{parquet::PhysicalType::ByteArray};
    parquet::TextTag text{parquet::TextTag::String};
    bool rawJson{};
    size_t writerColumn{};
};

// Columns in first-seen order. Lookups take the caller's cache of the previous
// row's layout (rows of one type repeat their key order, so it answers without
// hashing); with one cache per thread, a finished set is read by many threads.
struct ColumnSet {
    std::vector<ColumnInfo> columns;
    std::unordered_map<std::string, size_t> byKey;

    static constexpr size_t npos = static_cast<size_t>(-1);
    using Cache = std::vector<size_t>;

    size_t find(size_t position, std::string_view key, Cache& cache) const {
        if (position < cache.size() && cache[position] < columns.size() && columns[cache[position]].key == key)
            return cache[position];
        const auto it = byKey.find(std::string(key));
        if (it == byKey.end()) return npos;
        remember(position, it->second, cache);
        return it->second;
    }

    size_t findOrAdd(size_t position, std::string_view key, Cache& cache) {
        size_t slot = find(position, key, cache);
        if (slot != npos) return slot;
        slot = columns.size();
        columns.push_back(ColumnInfo{std::string(key)});
        byKey.emplace(columns.back().key, slot);
        remember(position, slot, cache);
        return slot;
    }

    // Adds `other`'s columns, in its order, and their kinds.
    void merge(const ColumnSet& other) {
        for (const ColumnInfo& column : other.columns) {
            const auto it = byKey.find(column.key);
            size_t slot;
            if (it != byKey.end()) {
                slot = it->second;
            } else {
                slot = columns.size();
                columns.push_back(ColumnInfo{column.key});
                byKey.emplace(column.key, slot);
            }
            columns[slot].kinds |= column.kinds;
        }
    }

private:
    static void remember(size_t position, size_t slot, Cache& cache) {
        if (position >= cache.size()) cache.resize(position + 1, npos);
        cache[position] = slot;
    }
};

// An item array ("cars", ...) seen in a table, with the columns of its items.
struct ItemArray {
    std::string key;
    ColumnSet columns;
    bool usable{true};  // false once it held something other than an array of objects
    ColumnSet::Cache cache;
};

// What discovery learns about one table's rows.
struct Shape {
    uint64_t rows{};
    ColumnSet packet;
    std::vector<ItemArray> arrays;  // first-seen order; the first one is exploded if usable
    ColumnSet::Cache cache;

    ItemArray& array(std::string_view key) {
        for (ItemArray& a : arrays)
            if (a.key == key) return a;
        arrays.push_back(ItemArray{std::string(key)});
        return arrays.back();
    }

    void merge(const Shape& other) {
        rows += other.rows;
        packet.merge(other.packet);
        for (const ItemArray& a : other.arrays) {
            ItemArray& mine = array(a.key);
            mine.usable = mine.usable && a.usable;
            mine.columns.merge(a.columns);
        }
    }
};

// Table keys: the rows' "type" string ("" without one), or kUnparsed.
const std::string kUnparsed("\x01");

struct Table {
    std::string key;
    std::string fileName;
    Shape shape;
    bool exploding{};       // shape.arrays[0] becomes one row per item
    uint64_t rowIndex{};    // next row_index while writing
    std::unique_ptr<parquet::TableWriter> writer;

    bool unparsed() const { return key == kUnparsed; }
    const std::string& itemKey() const { return shape.arrays.front().key; }
    const ColumnSet& items() const { return shape.arrays.front().columns; }
};

void settle(ColumnInfo& column) {
    const uint8_t kinds = column.kinds & ~KindNull;
    const int categories = ((kinds & KindBool) ? 1 : 0) + ((kinds & (KindInt | KindFloat)) ? 1 : 0) +
                           ((kinds & KindString) ? 1 : 0);
    column.rawJson = false;
    column.text = parquet::TextTag::None;
    if (kinds == 0) {
        column.type = parquet::PhysicalType::ByteArray;  // only ever null
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

// One converted value, ready to set (24 bytes; text lives in the slice's arena).
struct Converted {
    uint32_t column{};
    parquet::PhysicalType type{};
    bool flag{};
    uint32_t textBegin{}, textSize{};
    union {
        int64_t integer;
        double real{};
    };
};

// Parses `raw` for `column`; false for a value the column's type cannot hold.
// A JSON null leaves `isNull` set and nothing to apply. Text is appended to
// `arena`; `scratch` is reused for decoding JSON strings.
bool convertValue(const ColumnInfo& column, std::string_view raw, Converted& out, bool& isNull,
                  std::string& arena, std::string& scratch) {
    isNull = raw == "null";
    if (isNull) return true;  // Parquet null
    out.column = static_cast<uint32_t>(column.writerColumn);
    out.type = column.rawJson ? parquet::PhysicalType::ByteArray : column.type;
    auto keep = [&](std::string_view text) {
        out.textBegin = static_cast<uint32_t>(arena.size());
        out.textSize = static_cast<uint32_t>(text.size());
        arena.append(text);
    };
    if (column.rawJson) {
        keep(raw);
        return true;
    }
    switch (column.type) {
        case parquet::PhysicalType::Boolean:
            out.flag = raw.front() == 't';
            return true;
        case parquet::PhysicalType::Int64:
            return readInt(raw, out.integer);
        case parquet::PhysicalType::Double:
            return readDouble(raw, out.real);
        default:
            scratch.clear();
            if (glz::read<kSlice>(scratch, raw)) return false;
            keep(scratch);
            return true;
    }
}

void applyValue(parquet::TableWriter& writer, const Converted& value, const std::string& arena) {
    switch (value.type) {
        case parquet::PhysicalType::Boolean: writer.setBool(value.column, value.flag); break;
        case parquet::PhysicalType::Int64:   writer.setInt64(value.column, value.integer); break;
        case parquet::PhysicalType::Double:  writer.setDouble(value.column, value.real); break;
        default:
            writer.setBytes(value.column, std::string_view(arena).substr(value.textBegin, value.textSize));
            break;
    }
}

// ── Parallel batches ────────────────────────────────────────────────────────
// Rows are copied into batches; each batch is split into contiguous slices,
// one per thread, and the slices' results are combined in row order, so the
// output is exactly what a single thread would produce.

struct Batch {
    std::string text;
    std::vector<std::pair<size_t, size_t>> spans;  // offset, length per row
    std::vector<float> times;

    void add(float time, std::string_view line) {
        spans.emplace_back(text.size(), line.size());
        text.append(line);
        times.push_back(time);
    }
    size_t size() const { return spans.size(); }
    std::string_view line(size_t i) const { return std::string_view(text).substr(spans[i].first, spans[i].second); }
    void clear() {
        text.clear();
        spans.clear();
        times.clear();
    }
};

size_t workerCount() {
    return std::clamp<size_t>(std::thread::hardware_concurrency(), 1, kMaxWorkers);
}

// Runs work(slice, begin, end) over `count` rows split into `slices` parts.
template <class Work>
void forSlices(size_t count, size_t slices, Work&& work) {
    std::vector<std::thread> threads;
    for (size_t s = 1; s < slices; ++s)
        threads.emplace_back([&, s] { work(s, count * s / slices, count * (s + 1) / slices); });
    work(0, 0, count / slices);
    for (std::thread& thread : threads) thread.join();
}

std::string typeOf(const std::vector<Member>& members) {
    for (const Member& member : members) {
        if (member.key != "type") continue;
        std::string type;
        if (member.raw.front() == '"' && !glz::read<kSlice>(type, member.raw)) return type;
        break;
    }
    return {};
}

// One thread's discovery over its slice: tables in first-seen order.
struct LocalDiscovery {
    std::vector<std::string> order;
    std::unordered_map<std::string, Shape> shapes;
    std::vector<Member> members, itemMembers;
    std::vector<std::string_view> elements;

    Shape& shape(const std::string& key) {
        auto [it, added] = shapes.try_emplace(key);
        if (added) order.push_back(key);
        return it->second;
    }

    void noteItems(ItemArray& array, std::string_view raw) {
        if (raw == "null") return;  // no items this time; written as one row
        if (raw.front() != '[' || !splitArray(raw, elements)) {
            array.usable = false;
            return;
        }
        for (std::string_view element : elements) {
            if (element.front() != '{' || !splitObject(element, itemMembers)) {
                array.usable = false;
                return;
            }
            for (size_t m = 0; m < itemMembers.size(); ++m)
                array.columns.columns[array.columns.findOrAdd(m, itemMembers[m].key, array.cache)].kinds |=
                    classify(itemMembers[m].raw);
        }
    }

    void note(std::string_view line) {
        if (!splitObject(line, members)) {
            shape(kUnparsed).rows++;
            return;
        }
        Shape& s = shape(typeOf(members));
        s.rows++;
        for (size_t m = 0; m < members.size(); ++m) {
            const Member& member = members[m];
            if (member.key == "type" || member.key == "session_time") continue;
            s.packet.columns[s.packet.findOrAdd(m, member.key, s.cache)].kinds |= classify(member.raw);
            if (!isItemArray(member.key)) continue;
            ItemArray& array = s.array(member.key);
            if (array.usable) noteItems(array, member.raw);
        }
    }

    void clear() {
        order.clear();
        shapes.clear();
    }
};

// One source row's output, converted by a worker and written in order.
struct RowPlan {
    uint32_t table{};
    float time{};
    bool hasTime{};
    bool unparsed{};
    uint32_t line{};          // batch row, for the Unparsed table
    uint32_t packetBegin{}, packetEnd{};
    uint32_t outputs{};       // rows written: one per item, at least one
    uint32_t itemsBegin{};    // first of `outputs` entries in itemRanges
};

// One thread's converted slice. Vectors keep their capacity between batches.
struct SlicePlan {
    std::vector<RowPlan> rows;
    std::vector<Converted> values;
    size_t used{};
    std::string arena, scratch;  // converted text, JSON string decoding
    std::vector<std::pair<uint32_t, uint32_t>> itemRanges;
    std::vector<ColumnSet::Cache> packetCaches, itemCaches;  // per table
    std::vector<Member> members, itemMembers;
    std::vector<std::string_view> elements;
    bool bad{};

    Converted& next() {
        if (used == values.size()) values.emplace_back();
        return values[used++];
    }
    void clear() {
        rows.clear();
        used = 0;
        arena.clear();
        itemRanges.clear();
        bad = false;
    }
};

class Exporter {
public:
    Exporter(const JsonlExportSource& source, const ParquetExportRequest& request)
        : source_(source), request_(request), workers_(workerCount()) {}

    bool run(std::string* errorOut) {
        report(0.0, "Reading rows");
        if (!discover(errorOut)) return false;
        if (!openWriters(errorOut)) return false;
        report(kDiscoverShare, "Writing tables");
        if (!writeRows(errorOut)) return fail(errorOut);
        for (Table& table : tables_)
            if (!table.writer->close(errorOut)) return fail(errorOut);
        report(1.0, "Done");
        return true;
    }

private:
    bool fail(std::string* errorOut) {
        if (errorOut && errorOut->empty() && cancelled_) *errorOut = "cancelled";
        for (Table& table : tables_)
            if (table.writer) table.writer->abort();
        return false;
    }

    void report(double fraction, const std::string& stage) {
        if (request_.progress) request_.progress(fraction, stage);
    }

    // Progress and cancellation, once per batch.
    bool tick(size_t rowsDone, double base, double share, const char* stage) {
        if (request_.cancelled && request_.cancelled()) {
            cancelled_ = true;
            return false;
        }
        const double total = source_.rowCount ? static_cast<double>(source_.rowCount) : 1.0;
        report(base + share * std::min(1.0, static_cast<double>(rowsDone) / total), stage);
        return true;
    }

    size_t slicesFor(size_t rows) const {
        return std::clamp<size_t>(rows / kMinSliceRows, 1, workers_);
    }

    // Visits every row in batches of kBatchRows; process(batch) false stops.
    template <class Process>
    bool forEachBatch(Process&& process, std::string* errorOut) {
        batch_.clear();
        bool stopped = false;
        const bool ok = source_.forEach([&](float time, std::string_view line) {
            batch_.add(time, line);
            if (batch_.size() < kBatchRows) return true;
            stopped = !process(batch_);
            batch_.clear();
            return !stopped;
        }, errorOut);
        if (!ok) return false;
        if (batch_.size() && !process(batch_)) return false;
        batch_.clear();
        return true;
    }

    Table& tableFor(const std::string& key) {
        const auto it = tableIndex_.find(key);
        if (it != tableIndex_.end()) return tables_[it->second];
        tableIndex_.emplace(key, tables_.size());
        tables_.emplace_back();
        tables_.back().key = key;
        return tables_.back();
    }

    bool discover(std::string* errorOut) {
        std::vector<LocalDiscovery> locals(workers_);
        size_t done = 0;
        const bool ok = forEachBatch([&](const Batch& batch) {
            const size_t slices = slicesFor(batch.size());
            forSlices(batch.size(), slices, [&](size_t s, size_t begin, size_t end) {
                LocalDiscovery& local = locals[s];
                local.clear();
                for (size_t i = begin; i < end; ++i) local.note(batch.line(i));
            });
            for (size_t s = 0; s < slices; ++s)
                for (const std::string& key : locals[s].order) tableFor(key).shape.merge(locals[s].shapes.at(key));
            done += batch.size();
            return tick(done, 0.0, kDiscoverShare, "Reading rows");
        }, errorOut);
        if (!ok && errorOut && errorOut->empty()) *errorOut = cancelled_ ? "cancelled" : "could not read the recording";
        return ok;
    }

    bool openWriters(std::string* errorOut) {
        std::unordered_map<std::string, int> usedNames;
        std::string header;
        writeJson(request_.header, header);
        for (Table& table : tables_) {
            std::string base = table.unparsed() ? "Unparsed" : tableFileName(table.key);
            if (const int seen = usedNames[base]++) base += "_" + std::to_string(seen + 1);
            table.fileName = base + ".parquet";

            std::vector<parquet::Column> columns;
            columns.push_back({"row_index", parquet::PhysicalType::Int64, parquet::TextTag::None, false});
            columns.push_back({"session_time", parquet::PhysicalType::Float, parquet::TextTag::None, kTimeNullable});
            if (table.unparsed()) {
                columns.push_back({"line", parquet::PhysicalType::ByteArray, parquet::TextTag::String, false});
            } else {
                table.exploding = !table.shape.arrays.empty() && table.shape.arrays.front().usable;
                std::unordered_map<std::string, bool> taken{{"row_index", true}, {"session_time", true}};
                for (ColumnInfo& column : table.shape.packet.columns) {
                    if (table.exploding && column.key == table.itemKey()) continue;
                    settle(column);
                    column.writerColumn = columns.size();
                    columns.push_back({column.key, column.type, column.text, true});
                    taken[column.key] = true;
                }
                if (table.exploding) {
                    for (ColumnInfo& column : table.shape.arrays.front().columns.columns) {
                        settle(column);
                        column.writerColumn = columns.size();
                        const std::string name = taken.count(column.key) ? table.itemKey() + "_" + column.key
                                                                         : column.key;
                        columns.push_back({name, column.type, column.text, true});
                    }
                }
            }

            std::vector<parquet::KeyValue> metadata{
                {"tnrd.format", kFormatName},
                {"tnrd.table", table.unparsed() ? std::string("(unparsed)") : table.key},
                {"tnrd.header", header},
            };
            parquet::Options options;
            options.rowGroupBytes = 8u << 20;  // every table is open at once
            table.writer = std::make_unique<parquet::TableWriter>();
            const std::string path = request_.directory + "/" + table.fileName;
            if (!table.writer->open(path, std::move(columns), std::move(metadata), errorOut, options))
                return fail(errorOut);
        }
        return true;
    }

    // Worker: converts one row into `plan`. False if the row no longer
    // matches what discovery saw.
    bool convertRow(const Batch& batch, size_t i, SlicePlan& plan) const {
        RowPlan row;
        row.line = static_cast<uint32_t>(i);
        if (!splitObject(batch.line(i), plan.members)) {
            const auto it = tableIndex_.find(kUnparsed);
            if (it == tableIndex_.end()) return false;
            row.table = static_cast<uint32_t>(it->second);
            row.unparsed = true;
            row.outputs = 1;
            row.time = batch.times[i];
            row.hasTime = true;
            plan.rows.push_back(row);
            return true;
        }
        const auto it = tableIndex_.find(typeOf(plan.members));
        if (it == tableIndex_.end()) return false;
        const Table& table = tables_[it->second];
        row.table = static_cast<uint32_t>(it->second);
        row.time = batch.times[i];
            row.hasTime = true;

        ColumnSet::Cache& packetCache = plan.packetCaches[it->second];
        std::string_view itemsRaw;
        row.packetBegin = static_cast<uint32_t>(plan.used);
        for (size_t m = 0; m < plan.members.size(); ++m) {
            const Member& member = plan.members[m];
            if (member.key == "type" || member.key == "session_time") continue;
            if (table.exploding && member.key == table.itemKey()) {
                itemsRaw = member.raw;
                continue;
            }
            const size_t slot = table.shape.packet.find(m, member.key, packetCache);
            if (slot == ColumnSet::npos) return false;
            Converted& value = plan.next();
            bool isNull;
            if (!convertValue(table.shape.packet.columns[slot], member.raw, value, isNull, plan.arena, plan.scratch))
                return false;
            if (isNull) --plan.used;
        }
        row.packetEnd = static_cast<uint32_t>(plan.used);

        plan.elements.clear();
        if (!itemsRaw.empty() && itemsRaw != "null" && !splitArray(itemsRaw, plan.elements)) return false;
        // A packet without items still gets one row, its item columns null.
        row.outputs = static_cast<uint32_t>(std::max<size_t>(1, plan.elements.size()));
        row.itemsBegin = static_cast<uint32_t>(plan.itemRanges.size());
        ColumnSet::Cache& itemCache = plan.itemCaches[it->second];
        for (size_t e = 0; e < row.outputs; ++e) {
            const uint32_t begin = static_cast<uint32_t>(plan.used);
            if (e < plan.elements.size()) {
                if (!splitObject(plan.elements[e], plan.itemMembers)) return false;
                for (size_t m = 0; m < plan.itemMembers.size(); ++m) {
                    const size_t slot = table.items().find(m, plan.itemMembers[m].key, itemCache);
                    if (slot == ColumnSet::npos) return false;
                    Converted& value = plan.next();
                    bool isNull;
                    if (!convertValue(table.items().columns[slot], plan.itemMembers[m].raw, value, isNull,
                                      plan.arena, plan.scratch))
                        return false;
                    if (isNull) --plan.used;
                }
            }
            plan.itemRanges.emplace_back(begin, static_cast<uint32_t>(plan.used));
        }
        plan.rows.push_back(row);
        return true;
    }

    // Main thread: writes one slice's rows in order.
    bool applySlice(const Batch& batch, const SlicePlan& plan, std::string* errorOut) {
        for (const RowPlan& row : plan.rows) {
            Table& table = tables_[row.table];
            parquet::TableWriter& writer = *table.writer;
            const int64_t rowIndex = static_cast<int64_t>(table.rowIndex++);
            if (row.unparsed) {
                writer.setInt64(0, rowIndex);
                if (row.hasTime) writer.setFloat(1, row.time);
                writer.setBytes(2, batch.line(row.line));
                if (!writer.endRow(errorOut)) return false;
                continue;
            }
            for (uint32_t o = 0; o < row.outputs; ++o) {
                writer.setInt64(0, rowIndex);
                if (row.hasTime) writer.setFloat(1, row.time);
                for (uint32_t p = row.packetBegin; p < row.packetEnd; ++p) applyValue(writer, plan.values[p], plan.arena);
                const auto [begin, end] = plan.itemRanges[row.itemsBegin + o];
                for (uint32_t v = begin; v < end; ++v) applyValue(writer, plan.values[v], plan.arena);
                if (!writer.endRow(errorOut)) return false;
            }
        }
        return true;
    }

    bool writeRows(std::string* errorOut) {
        std::vector<SlicePlan> plans(workers_);
        for (SlicePlan& plan : plans) {
            plan.packetCaches.resize(tables_.size());
            plan.itemCaches.resize(tables_.size());
        }
        size_t done = 0;
        bool bad = false, writeFailed = false;
        const bool ok = forEachBatch([&](const Batch& batch) {
            const size_t slices = slicesFor(batch.size());
            forSlices(batch.size(), slices, [&](size_t s, size_t begin, size_t end) {
                SlicePlan& plan = plans[s];
                plan.clear();
                for (size_t i = begin; i < end && !plan.bad; ++i) plan.bad = !convertRow(batch, i, plan);
            });
            for (size_t s = 0; s < slices; ++s) {
                if (plans[s].bad) return (bad = true, false);
                if (!applySlice(batch, plans[s], errorOut)) return (writeFailed = true, false);
            }
            done += batch.size();
            return tick(done, kDiscoverShare, 1.0 - kDiscoverShare, "Writing tables");
        }, errorOut);
        if (!ok && errorOut && errorOut->empty())
            *errorOut = cancelled_ ? "cancelled"
                      : bad       ? "a row changed between the export's two passes"
                                  : "could not read the recording";
        return ok && !writeFailed;
    }

    const JsonlExportSource& source_;
    const ParquetExportRequest& request_;
    const size_t workers_;
    std::vector<Table> tables_;  // first-seen order
    std::unordered_map<std::string, size_t> tableIndex_;
    Batch batch_;
    bool cancelled_{};
};

} // namespace

bool exportTnrdV2Parquet(const JsonlExportSource& rows, const ParquetExportRequest& request,
                         std::string* errorOut) {
    Exporter exporter(rows, request);
    return exporter.run(errorOut);
}

} // namespace tnrp::detail
