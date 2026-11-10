#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tnrp::parquet::detail {

// Write-only Thrift compact protocol encoder, enough for Parquet's page
// headers and file footer (apache/thrift doc/specs/thrift-compact-protocol.md).
//
// A struct is written as a run of fields, each opened with a header carrying
// its type and the delta from the previous field id, and closed with a stop
// byte. Nested structs restart the field-id delta; beginStruct()/endStruct()
// keep the enclosing struct's last id on a stack. Fields must be written in
// ascending id order within a struct.
class CompactWriter {
public:
    // Compact protocol type ids (field headers and list element types).
    enum Type : uint8_t {
        BoolTrue = 1, BoolFalse = 2, I8 = 3, I16 = 4, I32 = 5, I64 = 6,
        Double = 7, Binary = 8, List = 9, Set = 10, Map = 11, Struct = 12,
    };

    void fieldI16(int16_t id, int16_t value);
    void fieldI32(int16_t id, int32_t value);
    void fieldI64(int16_t id, int64_t value);
    void fieldString(int16_t id, std::string_view value);
    void fieldBool(int16_t id, bool value);

    // A struct-typed field (or a union member): opens the field, then the
    // nested struct's fields follow until endStruct().
    void beginStructField(int16_t id);
    void endStruct();

    // A list-typed field. Elements follow: listI32()/listString() values, or
    // for a struct list, beginListStruct()/endStruct() pairs.
    void beginListField(int16_t id, Type elementType, size_t size);
    void listI32(int32_t value);
    void listString(std::string_view value);
    void beginListStruct();

    // Top-level struct (a page header or the footer): fields follow, then
    // endStruct() writes its stop byte.
    void beginRoot();

    const std::string& bytes() const { return out_; }
    void clear() { out_.clear(); lastId_.clear(); }

private:
    void fieldHeader(int16_t id, Type type);
    void varint(uint64_t value);
    static uint64_t zigzag(int64_t value) {
        return (static_cast<uint64_t>(value) << 1) ^ static_cast<uint64_t>(value >> 63);
    }

    std::string out_;
    std::vector<int16_t> lastId_;  // last field id written, per open struct
};

} // namespace tnrp::parquet::detail
