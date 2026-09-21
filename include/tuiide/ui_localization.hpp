#pragma once

#include <string>
#include <string_view>

namespace tuiide {

/** Возвращает перевод подписи UI; неизвестные ключи остаются на английском. */
[[nodiscard]] auto localizedUiText(std::string_view language, std::string_view english) -> std::string;

}  // namespace tuiide
