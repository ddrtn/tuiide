#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace tuiide {

/**
 * Лёгкий буфер терминального текста: удаляет ANSI SGR и обрабатывает CR как
 * перерисовку текущей строки, что делает progress-команды читаемыми в UI.
 */
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
