#include "tnrp/ParquetWriter.h"

#include "parquet/ThriftCompact.h"
#include "TnrdCodec.h"

#include <zstd.h>

#include <algorithm>
#include <iterator>
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <thread>

static_assert(std::endian::native == std::endian::little,
              "PLAIN values are copied as native bytes; Parquet requires little endian");

namespace tnrp::parquet {

namespace {

using detail::CompactWriter;

// parquet.thrift enum values.
constexpr int32_t kTypeBoolean = 0, kTypeInt32 = 1, kTypeInt64 = 2, kTypeFloat = 4,
                  kTypeDouble = 5, kTypeByteArray = 6;
constexpr int32_t kRequired = 0, kOptional = 1;
constexpr int32_t kConvertedUtf8 = 0, kConvertedJson = 19;
constexpr int16_t kLogicalString = 1, kLogicalJson = 12;
constexpr int32_t kEncodingPlain = 0, kEncodingRle = 3, kEncodingDeltaBinaryPacked = 5,
                  kEncodingByteStreamSplit = 9;
constexpr int32_t kCodecZstd = 6;
constexpr int32_t kPageData = 0;
constexpr int32_t kFormatVersion = 1;

// Rows per data page are counted in an i32; keep far below it.
constexpr size_t kMaxPageRows = 1u << 20;

int32_t thriftType(PhysicalType type) {
    switch (type) {
        case PhysicalType::Boolean:   return kTypeBoolean;
        case PhysicalType::Int32:     return kTypeInt32;
        case PhysicalType::Int64:     return kTypeInt64;
        case PhysicalType::Float:     return kTypeFloat;
        case PhysicalType::Double:    return kTypeDouble;
        case PhysicalType::ByteArray: return kTypeByteArray;
    }
    return kTypeByteArray;
}

// Bytes one value occupies in a column's row-group buffer. Booleans are kept
// one byte each and bit-packed when a page is built; ByteArray is variable
// (a 4-byte length, then the bytes, exactly as PLAIN writes it).
size_t fixedWidth(PhysicalType type) {
    switch (type) {
        case PhysicalType::Boolean: return 1;
        case PhysicalType::Int32:   return 4;
        case PhysicalType::Int64:   return 8;
        case PhysicalType::Float:   return 4;
        case PhysicalType::Double:  return 8;
        case PhysicalType::ByteArray: return 0;
    }
    return 0;
}

void putU32(std::string& out, uint32_t value) {
    char bytes[4];
    std::memcpy(bytes, &value, 4);
    out.append(bytes, 4);
}

void putVarint(std::string& out, uint64_t value) {
    while (value >= 0x80) {
        out.push_back(static_cast<char>((value & 0x7f) | 0x80));
        value >>= 7;
    }
    out.push_back(static_cast<char>(value));
}

// Definition levels (0 = null, 1 = present) in the RLE / bit-packing hybrid
// at bit width 1, as RLE runs only: each run is varint(length << 1) followed
// by the repeated value in ceil(1 / 8) = 1 byte.
std::string encodeLevels(const uint8_t* levels, size_t count) {
    std::string out;
    size_t i = 0;
    while (i < count) {
        const uint8_t value = levels[i];
        size_t run = 1;
        while (i + run < count && levels[i + run] == value) ++run;
        putVarint(out, static_cast<uint64_t>(run) << 1);
        out.push_back(static_cast<char>(value));
        i += run;
    }
    return out;
}

// Each page of a numeric column is compressed twice, PLAIN and one
// alternative, and the smaller is kept (every encoding is lossless, and pages
// of one column chunk may differ). Measured on recordings, neither wins
// everywhere:
//  - FLOAT / DOUBLE: BYTE_STREAM_SPLIT (byte k of every value, then byte
//    k+1, ...) wins on smooth signals such as session_time, but loses badly
//    on decimals stored as doubles (G-force, steering), whose low bytes are
//    noise.
//  - INT64: DELTA_BINARY_PACKED wins on counters and slow signals, loses on
//    columns whose PLAIN bytes repeat.
// Booleans, INT32 and byte arrays are PLAIN only.
constexpr int32_t kNoAlternative = -1;

int32_t alternativeEncoding(PhysicalType type) {
    switch (type) {
        case PhysicalType::Float: case PhysicalType::Double: return kEncodingByteStreamSplit;
        case PhysicalType::Int64: return kEncodingDeltaBinaryPacked;
        default: return kNoAlternative;
    }
}

// BYTE_STREAM_SPLIT: K streams of N bytes for N values of K bytes each.
void appendByteStreamSplit(std::string& out, const char* values, size_t count, size_t width) {
    const size_t start = out.size();
    out.resize(start + count * width);
    char* dst = out.data() + start;
    for (size_t i = 0; i < count; ++i)
        for (size_t k = 0; k < width; ++k) dst[k * count + i] = values[i * width + k];
}

// Bit-packs `count` values at `width` bits each, least significant bit first.
void appendBitPacked(std::string& out, const uint64_t* values, size_t count, unsigned width) {
    if (width == 0) return;
    const size_t start = out.size();
    out.resize(start + (count * width + 7) / 8, '\0');
    auto* dst = reinterpret_cast<uint8_t*>(out.data() + start);
    size_t bit = 0;
    for (size_t i = 0; i < count; ++i) {
        uint64_t value = values[i];
        unsigned remaining = width;
        while (remaining) {
            const unsigned offset = static_cast<unsigned>(bit % 8);
            const unsigned take = std::min(remaining, 8u - offset);
            dst[bit / 8] |= static_cast<uint8_t>((value & ((1ull << take) - 1)) << offset);
            value = take == 64 ? 0 : value >> take;
            remaining -= take;
            bit += take;
        }
    }
}

uint64_t zigzag64(int64_t value) {
    return (static_cast<uint64_t>(value) << 1) ^ static_cast<uint64_t>(value >> 63);
}

// DELTA_BINARY_PACKED for INT64: header <block size 128> <4 miniblocks>
// <value count> <first value>, then per block of 128 deltas: <min delta>,
// four miniblock bit widths, and each needed miniblock's 32 (delta - min)
// values bit-packed (the last one padded with zeros). Deltas wrap in two's
// complement, so the arithmetic is done on uint64.
constexpr size_t kDeltaBlock = 128, kDeltaMiniblocks = 4, kDeltaMiniblock = kDeltaBlock / kDeltaMiniblocks;

void appendDeltaInt64(std::string& out, const char* bytes, size_t count) {
    auto value = [&](size_t i) {
        int64_t v;
        std::memcpy(&v, bytes + i * 8, 8);
        return static_cast<uint64_t>(v);
    };
    putVarint(out, kDeltaBlock);
    putVarint(out, kDeltaMiniblocks);
    putVarint(out, count);
    putVarint(out, zigzag64(count ? static_cast<int64_t>(value(0)) : 0));
    if (count < 2) return;

    uint64_t deltas[kDeltaBlock];
    uint64_t packed[kDeltaMiniblock];
    for (size_t first = 1; first < count; first += kDeltaBlock) {
        const size_t n = std::min(kDeltaBlock, count - first);
        int64_t minDelta = INT64_MAX;
        for (size_t i = 0; i < n; ++i) {
            deltas[i] = value(first + i) - value(first + i - 1);
            minDelta = std::min(minDelta, static_cast<int64_t>(deltas[i]));
        }
        putVarint(out, zigzag64(minDelta));
        unsigned widths[kDeltaMiniblocks] = {};
        for (size_t m = 0; m < kDeltaMiniblocks; ++m) {
            uint64_t bits = 0;
            for (size_t i = m * kDeltaMiniblock; i < std::min(n, (m + 1) * kDeltaMiniblock); ++i)
                bits |= deltas[i] - static_cast<uint64_t>(minDelta);
            widths[m] = static_cast<unsigned>(std::bit_width(bits));
            out.push_back(static_cast<char>(widths[m]));  // unused miniblocks: 0
        }
        for (size_t m = 0; m < kDeltaMiniblocks && m * kDeltaMiniblock < n; ++m) {
            // Zero-fill, then copy the deltas this miniblock has. (A per-element
            // `i < n ? … : 0` here crashed MSVC's LTCG code generator, C1001.)
            const size_t begin = m * kDeltaMiniblock;
            const size_t end = std::min(n, begin + kDeltaMiniblock);
            std::fill(std::begin(packed), std::end(packed), uint64_t{0});
            for (size_t i = begin; i < end; ++i)
                packed[i - begin] = deltas[i] - static_cast<uint64_t>(minDelta);
            appendBitPacked(out, packed, kDeltaMiniblock, widths[m]);
        }
    }
}

// Deletes a partial output file; UTF-8 path, extended-length on Windows.
void removeFile(const std::string& path) {
    std::error_code ignored;
#ifdef _WIN32
    std::filesystem::remove(std::filesystem::path(::tnrp::detail::windowsExtendedPath(path)), ignored);
#else
    std::filesystem::remove(std::filesystem::path(path), ignored);
#endif
}

struct ChunkMeta {
    int64_t numValues{};
    int64_t uncompressed{};
    int64_t compressed{};
    int64_t dataPageOffset{};
    uint32_t encodings{};  // bit per encoding id used by this chunk's pages
};

struct RowGroupMeta {
    std::vector<ChunkMeta> chunks;
    int64_t numRows{};
    int64_t totalByteSize{};
    int64_t totalCompressed{};
    int64_t fileOffset{};
};

struct ColumnBuffer {
    Column spec;
    std::string values;           // PLAIN-encoded present values (bools: 1 byte each)
    std::vector<uint8_t> levels;  // per row, optional columns only
    bool setThisRow{};
};

} // namespace

struct TableWriter::Impl {
    std::FILE* file{};
    std::string path;
    Options options;
    std::vector<ColumnBuffer> columns;
    std::vector<KeyValue> metadata;
    std::vector<RowGroupMeta> rowGroups;
    uint64_t offset{};
    uint64_t totalRows{};
    size_t groupRows{};
    size_t groupBytes{};
    std::string misuse;  // first API misuse in the current row
    std::vector<ZSTD_CCtx*> contexts;  // one per compression worker, made at the first flush

    ~Impl() {
        for (ZSTD_CCtx* ctx : contexts) ZSTD_freeCCtx(ctx);
    }

    bool write(const void* data, size_t size, std::string* errorOut) {
        if (size && std::fwrite(data, 1, size, file) != size) {
            if (errorOut) *errorOut = "could not write " + path;
            return false;
        }
        offset += size;
        return true;
    }

    ColumnBuffer* begin(size_t column, PhysicalType type) {
        if (column >= columns.size()) {
            if (misuse.empty()) misuse = "column index out of range";
            return nullptr;
        }
        ColumnBuffer& buffer = columns[column];
        if (buffer.spec.type != type) {
            if (misuse.empty()) misuse = "wrong value type for column " + buffer.spec.name;
            return nullptr;
        }
        if (buffer.setThisRow) {
            if (misuse.empty()) misuse = "column " + buffer.spec.name + " set twice in one row";
            return nullptr;
        }
        buffer.setThisRow = true;
        return &buffer;
    }

    void append(size_t column, PhysicalType type, const void* bytes, size_t size) {
        if (ColumnBuffer* buffer = begin(column, type)) {
            buffer->values.append(static_cast<const char*>(bytes), size);
            groupBytes += size;
        }
    }

    // Builds one data page and appends its header and compressed body to `out`.
    // Touches only `column`, `meta` and `out`, so columns can run in parallel.
    bool writePage(ColumnBuffer& column, size_t firstRow, size_t endRow, size_t firstValue,
                   size_t endValue, size_t presentCount, ChunkMeta& meta, ZSTD_CCtx* ctx,
                   std::string& out, std::string* errorOut) const {
        std::string levelsPrefix;
        if (column.spec.optional) {
            // Data page v1: definition levels are prefixed by their byte length.
            const std::string levels = encodeLevels(column.levels.data() + firstRow, endRow - firstRow);
            putU32(levelsPrefix, static_cast<uint32_t>(levels.size()));
            levelsPrefix += levels;
        }

        // The page body for one value encoding, compressed into `compressed`.
        const char* values = column.values.data() + firstValue;
        const size_t width = fixedWidth(column.spec.type);
        auto build = [&](int32_t encoding, std::string& body, std::string& compressed) {
            body = levelsPrefix;
            if (column.spec.type == PhysicalType::Boolean) {
                // PLAIN booleans: bit-packed, least significant bit first.
                const size_t start = body.size();
                body.resize(start + (presentCount + 7) / 8, '\0');
                for (size_t i = 0; i < presentCount; ++i)
                    if (values[i]) body[start + i / 8] |= static_cast<char>(1u << (i % 8));
            } else if (encoding == kEncodingByteStreamSplit) {
                appendByteStreamSplit(body, values, presentCount, width);
            } else if (encoding == kEncodingDeltaBinaryPacked) {
                appendDeltaInt64(body, values, presentCount);
            } else {
                body.append(values, endValue - firstValue);
            }
            compressed.resize(ZSTD_compressBound(body.size()));
            const size_t size = ZSTD_compressCCtx(ctx, compressed.data(), compressed.size(),
                                                  body.data(), body.size(), options.zstdLevel);
            if (ZSTD_isError(size)) {
                if (errorOut) *errorOut = std::string("zstd: ") + ZSTD_getErrorName(size);
                return false;
            }
            compressed.resize(size);
            return true;
        };

        int32_t encoding = kEncodingPlain;
        std::string body, compressed;
        if (!build(encoding, body, compressed)) return false;
        const int32_t alternative = alternativeEncoding(column.spec.type);
        if (alternative != kNoAlternative && presentCount > 0) {
            std::string altBody, altCompressed;
            if (!build(alternative, altBody, altCompressed)) return false;
            if (altCompressed.size() < compressed.size()) {
                encoding = alternative;
                body.swap(altBody);
                compressed.swap(altCompressed);
            }
        }
        if (body.size() > INT32_MAX || compressed.size() > INT32_MAX) {
            if (errorOut) *errorOut = "Parquet page too large in column " + column.spec.name;
            return false;
        }

        CompactWriter header;
        header.beginRoot();
        header.fieldI32(1, kPageData);
        header.fieldI32(2, static_cast<int32_t>(body.size()));
        header.fieldI32(3, static_cast<int32_t>(compressed.size()));
        header.beginStructField(5);  // DataPageHeader
        header.fieldI32(1, static_cast<int32_t>(endRow - firstRow));  // includes nulls
        header.fieldI32(2, encoding);
        header.fieldI32(3, kEncodingRle);
        header.fieldI32(4, kEncodingRle);
        header.endStruct();
        header.endStruct();

        out += header.bytes();
        out += compressed;
        meta.uncompressed += static_cast<int64_t>(header.bytes().size() + body.size());
        meta.compressed += static_cast<int64_t>(header.bytes().size() + compressed.size());
        meta.encodings |= 1u << encoding;
        if (column.spec.optional) meta.encodings |= 1u << kEncodingRle;
        return true;
    }

    // Encodes a column chunk's pages into `out`; meta.dataPageOffset is set
    // when the buffer is written to the file.
    bool writeChunk(ColumnBuffer& column, size_t rows, ChunkMeta& meta, ZSTD_CCtx* ctx, std::string& out,
                    std::string* errorOut) const {
        meta.numValues = static_cast<int64_t>(rows);
        const size_t width = fixedWidth(column.spec.type);
        size_t row = 0, value = 0;
        do {
            const size_t firstRow = row, firstValue = value;
            size_t present = 0;
            while (row < rows && row - firstRow < kMaxPageRows &&
                   (row == firstRow || value - firstValue < options.pageBytes)) {
                if (!column.spec.optional || column.levels[row]) {
                    if (width) {
                        value += width;
                    } else {
                        uint32_t length;
                        std::memcpy(&length, column.values.data() + value, 4);
                        value += 4 + static_cast<size_t>(length);
                    }
                    ++present;
                }
                ++row;
            }
            if (!writePage(column, firstRow, row, firstValue, value, present, meta, ctx, out, errorOut))
                return false;
        } while (row < rows);
        return true;
    }

    bool flushRowGroup(std::string* errorOut) {
        if (groupRows == 0) return true;
        RowGroupMeta group;
        group.numRows = static_cast<int64_t>(groupRows);
        group.fileOffset = static_cast<int64_t>(offset);
        group.chunks.resize(columns.size());

        // Encode and compress the column chunks in parallel, one zstd context
        // per worker, then write them to the file in column order: the file is
        // byte-for-byte what a single thread would write.
        const size_t count = columns.size();
        const size_t workers = std::clamp<size_t>(std::thread::hardware_concurrency(), 1, count);
        while (contexts.size() < workers) {
            ZSTD_CCtx* ctx = ZSTD_createCCtx();
            if (!ctx) {
                if (errorOut) *errorOut = "could not create a zstd context";
                return false;
            }
            contexts.push_back(ctx);
        }
        std::vector<std::string> buffers(count), errors(count);
        std::vector<char> ok(count, 0);
        std::atomic<size_t> next{0};
        auto work = [&](size_t worker) {
            for (size_t i = next++; i < count; i = next++)
                ok[i] = writeChunk(columns[i], groupRows, group.chunks[i], contexts[worker], buffers[i], &errors[i]);
        };
        std::vector<std::thread> threads;
        threads.reserve(workers - 1);
        for (size_t w = 1; w < workers; ++w) threads.emplace_back(work, w);
        work(0);
        for (std::thread& thread : threads) thread.join();

        for (size_t i = 0; i < count; ++i) {
            if (!ok[i]) {
                if (errorOut) *errorOut = errors[i];
                return false;
            }
            group.chunks[i].dataPageOffset = static_cast<int64_t>(offset);
            if (!write(buffers[i].data(), buffers[i].size(), errorOut)) return false;
            group.totalByteSize += group.chunks[i].uncompressed;
            group.totalCompressed += group.chunks[i].compressed;
            columns[i].values.clear();
            columns[i].levels.clear();
        }
        rowGroups.push_back(std::move(group));
        groupRows = 0;
        groupBytes = 0;
        return true;
    }

    std::string footer() const {
        CompactWriter w;
        w.beginRoot();
        w.fieldI32(1, kFormatVersion);

        w.beginListField(2, CompactWriter::Struct, columns.size() + 1);
        w.beginListStruct();  // root group
        w.fieldString(4, "schema");
        w.fieldI32(5, static_cast<int32_t>(columns.size()));
        w.endStruct();
        for (const ColumnBuffer& column : columns) {
            w.beginListStruct();
            w.fieldI32(1, thriftType(column.spec.type));
            w.fieldI32(3, column.spec.optional ? kOptional : kRequired);
            w.fieldString(4, column.spec.name);
            if (column.spec.type == PhysicalType::ByteArray && column.spec.text != TextTag::None) {
                const bool json = column.spec.text == TextTag::Json;
                w.fieldI32(6, json ? kConvertedJson : kConvertedUtf8);
                w.beginStructField(10);  // LogicalType union
                w.beginStructField(json ? kLogicalJson : kLogicalString);
                w.endStruct();           // empty StringType / JsonType
                w.endStruct();
            }
            w.endStruct();
        }

        w.fieldI64(3, static_cast<int64_t>(totalRows));

        w.beginListField(4, CompactWriter::Struct, rowGroups.size());
        for (size_t g = 0; g < rowGroups.size(); ++g) {
            const RowGroupMeta& group = rowGroups[g];
            w.beginListStruct();
            w.beginListField(1, CompactWriter::Struct, group.chunks.size());
            for (size_t c = 0; c < group.chunks.size(); ++c) {
                const ChunkMeta& chunk = group.chunks[c];
                const Column& spec = columns[c].spec;
                w.beginListStruct();             // ColumnChunk
                w.fieldI64(2, 0);                // file_offset: deprecated, 0
                w.beginStructField(3);           // ColumnMetaData
                w.fieldI32(1, thriftType(spec.type));
                w.beginListField(2, CompactWriter::I32, static_cast<size_t>(std::popcount(chunk.encodings)));
                for (int32_t id = 0; id < 32; ++id)
                    if (chunk.encodings & (1u << id)) w.listI32(id);
                w.beginListField(3, CompactWriter::Binary, 1);
                w.listString(spec.name);
                w.fieldI32(4, kCodecZstd);
                w.fieldI64(5, chunk.numValues);
                w.fieldI64(6, chunk.uncompressed);
                w.fieldI64(7, chunk.compressed);
                w.fieldI64(9, chunk.dataPageOffset);
                w.endStruct();
                w.endStruct();
            }
            w.fieldI64(2, group.totalByteSize);
            w.fieldI64(3, group.numRows);
            w.fieldI64(5, group.fileOffset);
            w.fieldI64(6, group.totalCompressed);
            w.fieldI16(7, static_cast<int16_t>(std::min<size_t>(g, INT16_MAX)));
            w.endStruct();
        }

        if (!metadata.empty()) {
            w.beginListField(5, CompactWriter::Struct, metadata.size());
            for (const KeyValue& kv : metadata) {
                w.beginListStruct();
                w.fieldString(1, kv.key);
                w.fieldString(2, kv.value);
                w.endStruct();
            }
        }
        w.fieldString(6, "Track N Race");
        w.endStruct();
        return w.bytes();
    }

    void closeFile() {
        if (file) std::fclose(file);
        file = nullptr;
    }
};

TableWriter::TableWriter() = default;

TableWriter::~TableWriter() {
    if (isOpen()) abort();
}

bool TableWriter::open(const std::string& path, std::vector<Column> columns,
                       std::vector<KeyValue> metadata, std::string* errorOut, Options options) {
    if (isOpen()) abort();
    if (columns.empty()) {
        if (errorOut) *errorOut = "a Parquet table needs at least one column";
        return false;
    }
    auto impl = std::make_unique<Impl>();
    impl->path = path;
    impl->options = options;
    if (impl->options.rowGroupRows == 0) impl->options.rowGroupRows = 1;
    if (impl->options.pageBytes == 0) impl->options.pageBytes = 1;
    impl->metadata = std::move(metadata);
    impl->columns.reserve(columns.size());
    for (Column& column : columns) impl->columns.push_back(ColumnBuffer{std::move(column), {}, {}, false});
    impl->file = ::tnrp::detail::openTnrdFile(path, "wb");
    if (!impl->file) {
        if (errorOut) *errorOut = "could not create " + path;
        return false;
    }
    if (!impl->write("PAR1", 4, errorOut)) {
        impl->closeFile();
        removeFile(path);
        return false;
    }
    impl_ = std::move(impl);
    return true;
}

void TableWriter::setBool(size_t column, bool value) {
    const uint8_t byte = value ? 1 : 0;
    impl_->append(column, PhysicalType::Boolean, &byte, 1);
}
void TableWriter::setInt32(size_t column, int32_t value) { impl_->append(column, PhysicalType::Int32, &value, 4); }
void TableWriter::setInt64(size_t column, int64_t value) { impl_->append(column, PhysicalType::Int64, &value, 8); }
void TableWriter::setFloat(size_t column, float value) { impl_->append(column, PhysicalType::Float, &value, 4); }
void TableWriter::setDouble(size_t column, double value) { impl_->append(column, PhysicalType::Double, &value, 8); }

void TableWriter::setBytes(size_t column, std::string_view value) {
    if (value.size() > UINT32_MAX) {
        if (impl_->misuse.empty()) impl_->misuse = "value too long for a Parquet byte array";
        return;
    }
    if (ColumnBuffer* buffer = impl_->begin(column, PhysicalType::ByteArray)) {
        putU32(buffer->values, static_cast<uint32_t>(value.size()));
        buffer->values.append(value);
        impl_->groupBytes += 4 + value.size();
    }
}

bool TableWriter::endRow(std::string* errorOut) {
    Impl& impl = *impl_;
    if (impl.misuse.empty()) {
        for (const ColumnBuffer& column : impl.columns)
            if (!column.spec.optional && !column.setThisRow) {
                impl.misuse = "required column " + column.spec.name + " not set";
                break;
            }
    }
    if (!impl.misuse.empty()) {
        if (errorOut) *errorOut = impl.misuse;
        return false;
    }
    for (ColumnBuffer& column : impl.columns) {
        if (column.spec.optional) {
            column.levels.push_back(column.setThisRow ? 1 : 0);
            ++impl.groupBytes;
        }
        column.setThisRow = false;
    }
    ++impl.groupRows;
    ++impl.totalRows;
    if (impl.groupRows >= impl.options.rowGroupRows || impl.groupBytes >= impl.options.rowGroupBytes)
        return impl.flushRowGroup(errorOut);
    return true;
}

bool TableWriter::close(std::string* errorOut) {
    if (!isOpen()) {
        if (errorOut) *errorOut = "Parquet writer is not open";
        return false;
    }
    Impl& impl = *impl_;
    bool ok = impl.flushRowGroup(errorOut);
    if (ok) {
        const std::string meta = impl.footer();
        char length[4];
        const uint32_t size = static_cast<uint32_t>(meta.size());
        std::memcpy(length, &size, 4);
        ok = impl.write(meta.data(), meta.size(), errorOut) && impl.write(length, 4, errorOut) &&
             impl.write("PAR1", 4, errorOut);
    }
    if (ok && std::fflush(impl.file) != 0) {
        if (errorOut) *errorOut = "could not write " + impl.path;
        ok = false;
    }
    if (!ok) {
        abort();
        return false;
    }
    impl.closeFile();
    impl_.reset();
    return true;
}

void TableWriter::abort() {
    if (!impl_) return;
    const std::string path = impl_->path;
    impl_->closeFile();
    impl_.reset();
    removeFile(path);
}

bool TableWriter::isOpen() const { return impl_ && impl_->file; }
uint64_t TableWriter::rowCount() const { return impl_ ? impl_->totalRows : 0; }
size_t TableWriter::columnCount() const { return impl_ ? impl_->columns.size() : 0; }

} // namespace tnrp::parquet
