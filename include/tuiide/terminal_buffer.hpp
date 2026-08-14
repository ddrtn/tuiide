#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace tuiide {

class TerminalBuffer {
 public:
  explicit TerminalBuffer(std::size_t maximum_lines = 2000);
  void append(std::string_view bytes);
  void clear();
  [[nodiscard]] auto text() const -> std::string;

 private:
  enum class State { Text, Escape, Csi, Osc, OscEscape };
  void finishCsi(char command);
  void finishLine();

  std::size_t maximum_lines_;
  std::vector<std::string> lines_;
  std::string line_;
  std::size_t cursor_{};
  State state_{State::Text};
  std::string parameters_;
};

}  // namespace tuiide
