#pragma once

#include <cstdint>
#include <string_view>

namespace blutter::meta {

inline constexpr uint32_t kAbiVersion = 5;

// String fields are byte offsets into Bundle::strings. Addresses are relative
// to the libapp base; heap offsets include the Dart heap object tag.
// A name may have multiple symbols with different addresses, including fully
// qualified closure names. Symbols are sorted by (hash, name, rva); scan all
// matching names within a hash range to retrieve every address.
struct Symbol {
    uint32_t hash;
    uint32_t rva;
    uint32_t name;
};

struct LookupIndex {
    uint32_t hash;
    uint32_t index;
};

struct ClassInfo {
    uint32_t cid;
    uint32_t name;
};

struct Constant {
    uint32_t cls;
    uint32_t field;
    uint32_t value;
    uint32_t heap;
    // UTF-8 byte count, excluding the terminator. The value may contain NULs.
    uint32_t value_length;
};

struct Layout {
    uint32_t compressed_word_size;
    uint32_t class_id_tag_pos;
    uint32_t class_id_tag_mask;
    uint32_t instance_size;
    uint32_t string_length_offset;
    uint32_t one_byte_string_data_offset;
    uint32_t two_byte_string_data_offset;
    uint32_t smi_cid;
    uint32_t mint_cid;
    uint32_t double_cid;
    uint32_t bool_cid;
    uint32_t one_byte_string_cid;
    uint32_t two_byte_string_cid;
};

// Symbols and lookup indices are sorted by FNV-1a hash. Classes are sorted by
// cid. Hash collisions must be resolved by comparing strings. Constant keys
// hash the class, field and the entire value_length-byte value, each followed
// by a zero byte. Class and field names are NUL-terminated.
struct Bundle {
    uint32_t abi_version;
    uint32_t symbol_count;
    const Symbol* symbols;
    uint32_t class_count;
    const ClassInfo* classes;
    const LookupIndex* classes_by_name;
    uint32_t constant_count;
    const Constant* constants;
    const LookupIndex* constants_by_key;
    const char* strings;
    uint32_t strings_size;
    Layout layout;
};

const Bundle& blutter_bundle();

inline std::string_view constant_value(const Bundle& bundle, const Constant& constant) {
    return {bundle.strings + constant.value, constant.value_length};
}

}  // namespace blutter::meta
