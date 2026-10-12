#include "pch.h"
#include "MetadataWriter.h"
#include "MetadataNaming.h"
#include "Metadata.h"
#include "MetadataHeader.h"

#if __has_include(<vm/unicode.h>)
#include <vm/unicode.h>
#else
#include <platform/unicode.h>
#endif

#include <cstring>
#include <fstream>
#include <filesystem>
#include <limits>
#include <iomanip>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>

namespace {

uint32_t fnv1a(std::string_view value) {
    uint32_t hash = 2166136261u;
    for (unsigned char ch : value) {
        hash ^= ch;
        hash *= 16777619u;
    }
    return hash;
}

uint32_t constant_hash(std::string_view cls, std::string_view field,
                       std::string_view value) {
    uint32_t hash = 2166136261u;
    for (std::string_view part : {cls, field, value}) {
        for (unsigned char ch : part) {
            hash ^= ch;
            hash *= 16777619u;
        }
        hash ^= 0;
        hash *= 16777619u;
    }
    return hash;
}

std::string utf8_string(const dart::Object& object) {
    const auto& string = dart::String::Cast(object);
    const intptr_t length = dart::Utf8::Length(string);
    if (length < 0 || static_cast<uint64_t>(length) > std::numeric_limits<uint32_t>::max() - 2u) {
        throw std::runtime_error("metadata string exceeds 32-bit length");
    }
    std::string value(static_cast<size_t>(length), '\0');
    if (length != 0) {
        string.ToUTF8(reinterpret_cast<uint8_t*>(value.data()), length);
    }
    return value;
}

class StringPool {
public:
    StringPool() { data.push_back('\0'); offsets.emplace("", 0); }

    uint32_t Add(const std::string& value) {
        const auto found = offsets.find(value);
        if (found != offsets.end()) return found->second;
        // Leave room for the string terminator and the literal's final zero.
        const auto max_size = std::numeric_limits<uint32_t>::max();
        if (value.size() >= max_size - data.size() - 1) {
            throw std::runtime_error("metadata string pool exceeds 32-bit offsets");
        }
        const uint32_t offset = static_cast<uint32_t>(data.size());
        data.insert(data.end(), value.begin(), value.end());
        data.push_back('\0');
        offsets.emplace(value, offset);
        return offset;
    }

    std::vector<char> data;
    std::map<std::string, uint32_t> offsets;
};

struct OutSymbol { uint32_t hash, rva, name; };
struct OutLookup { uint32_t hash, index; };
struct OutClass { uint32_t cid, name; };
struct OutConstant { uint32_t cls, field, value, heap, value_length; };

std::string normalized_library(const DartLibrary& library) {
    std::string name = library.url;
    if (name.starts_with("package:")) name.erase(0, 8);
    else if (name.starts_with("file:")) name.erase(0, 5);
    if (name.ends_with(".dart")) name.erase(name.size() - 5);
    std::replace(name.begin(), name.end(), '/', '$');
    return name;
}

std::string normalized_function(const DartFunction& function, bool qualified) {
    return blutter::metadata::function_name(
        qualified ? normalized_library(function.Class().Library()) : std::string{},
        function.Class().Name(), function.Name(), qualified);
}

std::string field_name(DartClass* owner, uint32_t offset) {
    if (owner != nullptr) {
        if (DartField* field = owner->FindField(offset); field != nullptr && !field->Name().empty()) {
            return field->Name();
        }
        // The VM's internal _Enum class does not always retain Dart Field
        // objects in AOT metadata, but its stable public fields are useful for
        // indexing canonical enum instances.
        if (owner->Name() == "_Enum") {
            if (offset == 8) return "index";
            if (offset == 16) return "name";
        }
    }
    return std::format("field_{:x}", offset);
}

DartClass* owner_of(DartClass* cls, uint32_t offset) {
    if (cls == nullptr) return nullptr;
    DartClass* owner = cls;
    while (owner->Parent() != nullptr && offset < static_cast<uint32_t>(owner->Parent()->Size())) {
        owner = owner->Parent();
    }
    return owner;
}

void emit_byte_pool(std::ostream& out, const std::vector<char>& values) {
    out << "static constexpr char kStrings[] =\n";
    for (size_t i = 0; i < values.size(); ++i) {
        if (i % 32 == 0) out << "  \"";
        out << '\\' << std::oct << std::setw(3) << std::setfill('0')
            << static_cast<unsigned>(static_cast<unsigned char>(values[i])) << std::dec;
        if ((i + 1) % 32 == 0 || i + 1 == values.size()) out << "\"\n";
    }
    out << ";\n";
}

}  // namespace

void MetadataWriter::Create(const char* filename) {
    if (dart::kCompressedWordSize != sizeof(uint32_t)) {
        throw std::runtime_error("metadata requires 4-byte compressed Dart pointers");
    }
    StringPool strings;
    std::vector<OutClass> classes;
    std::vector<OutConstant> constants;
    std::vector<OutSymbol> symbols;
    std::vector<OutLookup> classes_by_name;
    std::vector<OutLookup> constants_by_key;

    for (DartClass* cls : app.classes) {
        if (cls != nullptr) classes.push_back({cls->Id(), strings.Add(cls->Name())});
    }

    const auto& pool = app.GetObjectPool();
    auto& object = dart::Object::Handle();
    auto& value_object = dart::Object::Handle();
    std::set<std::tuple<uint32_t, std::string, std::string>> seen_constants;
    for (intptr_t i = 0; i < pool.Length(); ++i) {
        if (pool.TypeAt(i) != dart::ObjectPool::EntryType::kTaggedObject) continue;
        object = pool.ObjectAt(i);
        const intptr_t cid = object.GetClassId();
        if (cid < dart::kNumPredefinedCids || !object.IsInstance()) continue;
        DartClass* cls = app.GetClass(cid);
        if (cls == nullptr || cls->Size() < static_cast<intptr_t>(dart::Instance::NextFieldOffset())) continue;
        const uintptr_t address = dart::UntaggedObject::ToAddr(object.ptr());
        for (uint32_t offset = dart::Instance::NextFieldOffset();
             offset + dart::kCompressedWordSize <= static_cast<uint32_t>(cls->Size());
             offset += dart::kCompressedWordSize) {
            DartClass* owner = owner_of(cls, offset);
            if (owner == nullptr || offset == static_cast<uint32_t>(cls->TypeArgumentsOffset()) ||
                offset == static_cast<uint32_t>(owner->TypeArgumentOffset()) ||
                owner->UnboxedFieldsBitmap().Get(offset / dart::kCompressedWordSize)) continue;
            const auto* pointer = reinterpret_cast<const dart::CompressedObjectPtr*>(address + offset);
            if (!pointer->IsHeapObject()) continue;
            value_object = pointer->Decompress(app.heap_base());
            if (!value_object.IsString()) continue;
            const std::string value = utf8_string(value_object);
            const std::string field = field_name(owner, offset);
            const uint32_t heap_offset = static_cast<uint32_t>(static_cast<intptr_t>(object.ptr()));
            if (!seen_constants.emplace(heap_offset, field, value).second) continue;
            const uint32_t field_string_offset = strings.Add(field);
            constants.push_back({strings.Add(cls->Name()), field_string_offset,
                                 strings.Add(value), heap_offset, static_cast<uint32_t>(value.size())});
        }
    }

    // Qualified names can still collide, e.g. anonymous closures in one class.
    // Deduplicate aliases only when both the name and address are identical.
    std::set<std::pair<std::string, uint32_t>> named_symbols;
    for (const auto& [address, function] : app.functions) {
        if (function == nullptr || function->Address() > 0xffffffffu) continue;
        named_symbols.emplace(normalized_function(*function, false),
                              static_cast<uint32_t>(function->Address()));
        named_symbols.emplace(normalized_function(*function, true),
                              static_cast<uint32_t>(function->Address()));
    }
    for (const auto& [address, stub] : app.stubs) {
        if (stub == nullptr || stub->Address() > 0xffffffffu) continue;
        named_symbols.emplace(stub->FullName(), static_cast<uint32_t>(stub->Address()));
    }
    for (const auto& [name, rva] : named_symbols) {
        symbols.push_back({fnv1a(name), rva, strings.Add(name)});
    }
    std::sort(symbols.begin(), symbols.end(), [&strings](const OutSymbol& left,
                                                        const OutSymbol& right) {
        if (left.hash != right.hash) return left.hash < right.hash;
        const int name_order = std::strcmp(strings.data.data() + left.name,
                                          strings.data.data() + right.name);
        if (name_order != 0) return name_order < 0;
        return left.rva < right.rva;
    });

    std::sort(classes.begin(), classes.end(), [](const OutClass& left, const OutClass& right) {
        return left.cid < right.cid;
    });
    for (size_t i = 0; i < classes.size(); ++i) {
        classes_by_name.push_back(
            {fnv1a(strings.data.data() + classes[i].name), static_cast<uint32_t>(i)});
    }
    std::sort(classes_by_name.begin(), classes_by_name.end(),
              [](const OutLookup& left, const OutLookup& right) {
                  return std::tie(left.hash, left.index) < std::tie(right.hash, right.index);
              });

    for (size_t i = 0; i < constants.size(); ++i) {
        const OutConstant& item = constants[i];
        constants_by_key.push_back({constant_hash(strings.data.data() + item.cls,
                                                   strings.data.data() + item.field,
                                                   std::string_view(strings.data.data() + item.value,
                                                                    item.value_length)),
                                    static_cast<uint32_t>(i)});
    }
    std::sort(constants_by_key.begin(), constants_by_key.end(),
              [](const OutLookup& left, const OutLookup& right) {
                  return std::tie(left.hash, left.index) < std::tie(right.hash, right.index);
              });

    std::ofstream out(filename, std::ios::trunc);
    if (!out) throw std::runtime_error(std::string("cannot write ") + filename);
    out.exceptions(std::ios::badbit | std::ios::failbit);
    const auto header_path = std::filesystem::path(filename).parent_path() / "blutter_meta.h";
    std::ofstream header(header_path, std::ios::trunc);
    if (!header) throw std::runtime_error("cannot write " + header_path.string());
    header.exceptions(std::ios::badbit | std::ios::failbit);
    header << kMetadataHeader;
    header.close();
    out << "// Generated by blutter MetadataWriter. Do not edit.\n"
           "#include \"blutter_meta.h\"\n\n"
           "namespace blutter::meta {\n";
    emit_byte_pool(out, strings.data);

    out << "static constexpr Symbol kSymbols[] = {\n";
    if (symbols.empty()) out << "  {},\n";
    for (const auto& item : symbols)
        out << std::format("  {{{}u, {:#x}u, {}u}},\n", item.hash, item.rva, item.name);
    out << "};\nstatic constexpr LookupIndex kClassesByName[] = {\n";
    if (classes_by_name.empty()) out << "  {},\n";
    for (const auto& item : classes_by_name)
        out << std::format("  {{{}u, {}u}},\n", item.hash, item.index);
    out << "};\nstatic constexpr ClassInfo kClasses[] = {\n";
    if (classes.empty()) out << "  {},\n";
    for (const auto& item : classes)
        out << std::format("  {{{}u, {}u}},\n", item.cid, item.name);
    out << "};\nstatic constexpr Constant kConstants[] = {\n";
    if (constants.empty()) out << "  {},\n";
    for (const auto& item : constants) {
        out << std::format("  {{{}u, {}u, {}u, {:#x}u, {}u}},\n",
                           item.cls, item.field, item.value, item.heap, item.value_length);
    }
    out << "};\nstatic constexpr LookupIndex kConstantsByKey[] = {\n";
    if (constants_by_key.empty()) out << "  {},\n";
    for (const auto& item : constants_by_key)
        out << std::format("  {{{}u, {}u}},\n", item.hash, item.index);
    out << "};\n\nconst Bundle& blutter_bundle() {\n"
           "  static constexpr Bundle bundle = {\n"
        << "    " << blutter::meta::kAbiVersion << "u, // metadata ABI version\n"
        << "    " << symbols.size() << "u, kSymbols,\n"
        << "    " << classes.size() << "u, kClasses, kClassesByName,\n"
        << "    " << constants.size() << "u, kConstants, kConstantsByKey,\n"
        << "    kStrings, sizeof(kStrings),\n"
        << "    {" << dart::kCompressedWordSize << "u, "
        << kUntaggedObjectClassIdTagPos << "u, "
        << ((1u << dart::UntaggedObject::kClassIdTagSize) - 1u) << "u, "
        << AOT_Instance_InstanceSize << "u, "
        << AOT_String_length_offset << "u, "
        << AOT_OneByteString_data_offset << "u, "
        << AOT_TwoByteString_data_offset << "u, "
        << dart::kSmiCid << "u, " << dart::kMintCid << "u, "
        << dart::kDoubleCid << "u, " << dart::kBoolCid << "u, "
        << dart::kOneByteStringCid << "u, " << dart::kTwoByteStringCid << "u}\n"
           "  };\n  return bundle;\n}\n\n}  // namespace blutter::meta\n";
    out.close();
}
