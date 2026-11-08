#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Minimal Apache Parquet writer for flat tables (no nesting, no repetition).
// It knows nothing about TNRD: callers describe the columns, then append rows.
//
// What it writes (apache/parquet-format):
//  - "PAR1", row groups, the Thrift-compact FileMetaData footer, its 4-byte
//    little-endian length, "PAR1".
//  - Each column chunk is a run of version 1 data pages: optional columns
//    carry RLE definition levels (4-byte length prefix); numeric pages are
//    written PLAIN or, when it compresses smaller, BYTE_STREAM_SPLIT (FLOAT,
//    DOUBLE) / DELTA_BINARY_PACKED (INT64); other values are PLAIN,
//    and each page body is zstd compressed.
//  - No dictionary pages, statistics, page indexes or encryption.
//
// Memory is bounded by one row group: rows are buffered per column and
// flushed every Options::rowGroupRows rows or Options::rowGroupBytes bytes.
namespace tnrp::parquet {

enum class PhysicalType : uint8_t { Boolean, Int32, Int64, Float, Double, ByteArray };

// Logical annotation for a ByteArray column.
enum class TextTag : uint8_t { None, String, Json };

struct Column {
    std::string name;
    PhysicalType type{PhysicalType::Double};
    TextTag text{TextTag::None};
    bool optional{true};  // false: every row must set it
};

struct KeyValue {
    std::string key;
    std::string value;
};

struct Options {
    size_t rowGroupRows{65536};
    size_t rowGroupBytes{64u << 20};
    size_t pageBytes{1u << 20};  // uncompressed value bytes per data page
    int zstdLevel{3};
};

class TableWriter {
public:
    TableWriter();
    ~TableWriter();  // aborts if still open
    TableWriter(const TableWriter&) = delete;
    TableWriter& operator=(const TableWriter&) = delete;

    // Creates (truncates) `path` (UTF-8) and writes the leading magic.
    bool open(const std::string& path, std::vector<Column> columns,
              std::vector<KeyValue> metadata, std::string* errorOut, Options options = {});

    // Set a value of the current row. Each column may be set at most once per
    // row, with the setter matching its physical type; a column left unset is
    // null (optional) or an error at endRow() (required).
    void setBool(size_t column, bool value);
    void setInt32(size_t column, int32_t value);
    void setInt64(size_t column, int64_t value);
    void setFloat(size_t column, float value);
    void setDouble(size_t column, double value);
    void setBytes(size_t column, std::string_view value);

    // Commits the current row; may flush a row group to disk.
    bool endRow(std::string* errorOut);

    // Flushes the last row group, writes the footer and closes the file.
    bool close(std::string* errorOut);
    // Closes and deletes the partial file.
    void abort();

    bool isOpen() const;
    uint64_t rowCount() const;
    size_t columnCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tnrp::parquet
