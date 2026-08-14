#pragma once

#include <optional>
#include <string_view>

namespace tuiide {

[[nodiscard]] auto parseBuildProgress(std::string_view line) -> std::optional<unsigned>;

}  // namespace tuiide
