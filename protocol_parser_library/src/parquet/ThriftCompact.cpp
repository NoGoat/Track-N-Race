#include "parquet/ThriftCompact.h"

namespace tnrp::parquet::detail {

void CompactWriter::varint(uint64_t value) {
    // ULEB128: 7 bits at a time from the least significant end, high bit set
    // on every byte but the last.
    while (value >= 0x80) {
        out_.push_back(static_cast<char>((value & 0x7f) | 0x80));
        value >>= 7;
    }
    out_.push_back(static_cast<char>(value));
}

void CompactWriter::fieldHeader(int16_t id, Type type) {
    int16_t& last = lastId_.back();
    const int delta = id - last;
    if (delta > 0 && delta <= 15) {
        out_.push_back(static_cast<char>((delta << 4) | type));
    } else {
        // Long form: the type nibble alone, then the id as a zigzag varint.
        out_.push_back(static_cast<char>(type));
        varint(zigzag(id));
    }
    last = id;
}

void CompactWriter::beginRoot() { lastId_.push_back(0); }

void CompactWriter::fieldI16(int16_t id, int16_t value) { fieldHeader(id, I16); varint(zigzag(value)); }
void CompactWriter::fieldI32(int16_t id, int32_t value) { fieldHeader(id, I32); varint(zigzag(value)); }
void CompactWriter::fieldI64(int16_t id, int64_t value) { fieldHeader(id, I64); varint(zigzag(value)); }

void CompactWriter::fieldString(int16_t id, std::string_view value) {
    fieldHeader(id, Binary);
    varint(value.size());  // lengths are plain varints, not zigzag
    out_.append(value);
}

void CompactWriter::fieldBool(int16_t id, bool value) {
    // A boolean field has no payload: the value is the header's type nibble.
    fieldHeader(id, value ? BoolTrue : BoolFalse);
}

void CompactWriter::beginStructField(int16_t id) {
    fieldHeader(id, Struct);
    lastId_.push_back(0);
}

void CompactWriter::endStruct() {
    out_.push_back(0);  // stop field
    lastId_.pop_back();
}

void CompactWriter::beginListField(int16_t id, Type elementType, size_t size) {
    fieldHeader(id, List);
    if (size < 15) {
        out_.push_back(static_cast<char>((size << 4) | elementType));
    } else {
        out_.push_back(static_cast<char>(0xf0 | elementType));
        varint(size);
    }
}

void CompactWriter::listI32(int32_t value) { varint(zigzag(value)); }

void CompactWriter::listString(std::string_view value) {
    varint(value.size());
    out_.append(value);
}

void CompactWriter::beginListStruct() { lastId_.push_back(0); }

} // namespace tnrp::parquet::detail
