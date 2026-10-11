#pragma once

#include <cstdint>

namespace blutter::meta {

// String fields are byte offsets into Bundle::strings. Addresses are relative
// to the libapp base; heap offsets include the Dart heap object tag.
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
// hash the class, field and value, each followed by a zero byte.
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

}  // namespace blutter::meta
