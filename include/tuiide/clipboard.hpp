#pragma once

#include <string>
#include <string_view>

namespace tuiide {

auto base64Encode(std::string_view value) -> std::string;
auto osc52CopySequence(std::string_view value, bool tmux_passthrough = false) -> std::string;

class SystemClipboard {
 public:
  void copy(std::string text);
  auto paste() -> std::string;
  [[nodiscard]] auto internal() const -> const std::string&;

 private:
  std::string internal_;
};

}  // namespace tuiide
