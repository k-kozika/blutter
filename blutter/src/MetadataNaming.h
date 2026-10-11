#pragma once

#include <string>
#include <string_view>

namespace blutter::metadata {

inline std::string function_name(std::string_view library, std::string_view class_name,
                                 std::string_view name, bool qualified) {
    std::string result;
    if (qualified && !library.empty()) {
        result += library;
        result.push_back('$');
    }
    if (!class_name.empty()) {
        result += class_name;
        result += "::";
    }
    result += name;
    return result;
}

}  // namespace blutter::metadata
