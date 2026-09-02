#pragma once

#include <optional>
#include <string_view>

namespace tuiide {

/** Извлекает процент из полной строки CMake; nullopt означает обычный вывод. */
[[nodiscard]] auto parseBuildProgress(std::string_view line) -> std::optional<unsigned>;

}  // namespace tuiide
