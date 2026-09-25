#pragma once

#include <string>
#include <string_view>

namespace tuiide {

/** Возвращает перевод подписи UI; неизвестные ключи остаются на английском. */
[[nodiscard]] auto localizedUiText(std::string_view language, std::string_view english) -> std::string;

/** Возвращает справку по клавишам на выбранном языке без изменения самих сочетаний. */
[[nodiscard]] auto keyboardHelpText(std::string_view language) -> std::string_view;

}  // namespace tuiide
