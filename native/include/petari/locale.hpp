#pragma once
#include <optional>

namespace PetariNative {
struct LanguageSelection {
    unsigned table;
    unsigned language;
};

// Table indices and language values correspond to Game/System/Language.cpp.
// Selecting a table does not establish compatibility with a regional asset set.
constexpr std::optional<LanguageSelection> selectDiscLanguage(char region, int requested) {
    switch (region) {
    case 'E':
        return LanguageSelection{1, requested == 3 || requested == 4 ? static_cast<unsigned>(requested) : 1};
    case 'P':
        return LanguageSelection{2, requested >= 1 && requested <= 6 ? static_cast<unsigned>(requested) : 1};
    case 'J':
        return LanguageSelection{0, 0};
    case 'K':
        return LanguageSelection{4, 9};
    default:
        return std::nullopt;
    }
}
}
