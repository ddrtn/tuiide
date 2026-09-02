#pragma once

#include <string>
#include <string_view>

namespace tuiide {

auto base64Encode(std::string_view value) -> std::string;
auto osc52CopySequence(std::string_view value, bool tmux_passthrough = false) -> std::string;

/**
 * Буфер обмена с in-process fallback, Wayland/X11 helper и OSC 52 copy.
 * Внешние команды необязательны: отсутствие helper не делает copy/paste фатальным.
 */
class SystemClipboard {
 public:
  void copy(std::string text);
  auto paste() -> std::string;
  [[nodiscard]] auto internal() const -> const std::string&;

 private:
  std::string internal_;
};

}  // namespace tuiide
