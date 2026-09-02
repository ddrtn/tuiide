#pragma once

#include "tuiide/terminal_buffer.hpp"

#include <final/final.h>

#include <functional>
#include <string>
#include <string_view>

namespace tuiide {

/** Строка ввода терминала, передающая Ctrl+C и Ctrl+D владельцу консоли. */
class ConsoleInput final : public finalcut::FLineEdit {
 public:
  explicit ConsoleInput(finalcut::FWidget* parent = nullptr) : FLineEdit(parent) {}
  void setControlHandler(std::function<void(char)> handler) { control_handler_ = std::move(handler); }
  void setControlEnabled(bool enabled) { control_enabled_ = enabled; }
 protected:
  void onKeyPress(finalcut::FKeyEvent* event) override;
 private:
  std::function<void(char)> control_handler_;
  bool control_enabled_{};
};

/** Виджет истории PTY-вывода и строки `stdin>` для Run/GDB inferior. */
class ConsoleWidget final : public finalcut::FWidget {
 public:
  explicit ConsoleWidget(finalcut::FWidget* parent = nullptr);
  void append(std::string_view bytes);
  void clear();
  void setInputHandler(std::function<void(std::string)> handler);
  void setControlHandler(std::function<void(char)> handler);
  void setControlEnabled(bool enabled);
  void focusInput();
  [[nodiscard]] auto columns() const -> unsigned;
  [[nodiscard]] auto rows() const -> unsigned;
  [[nodiscard]] auto text() const -> std::string { return buffer_.text(); }
 protected:
  void adjustSize() override;
 private:
  void layoutChildren();
  TerminalBuffer buffer_;
  finalcut::FTextView display_{this};
  finalcut::FLabel prompt_{"stdin>", this};
  ConsoleInput input_{this};
  std::function<void(std::string)> input_handler_;
};

}  // namespace tuiide
