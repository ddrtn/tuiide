#include "tuiide/console_widget.hpp"

#include <algorithm>

namespace tuiide {

void ConsoleInput::onKeyPress(finalcut::FKeyEvent* event) {
  if (event->key() == finalcut::FKey::Ctrl_c && control_enabled_ && control_handler_) {
    control_handler_('c'); event->accept(); return;
  }
  if (event->key() == finalcut::FKey::Ctrl_d && control_enabled_ && control_handler_) {
    control_handler_('d'); event->accept(); return;
  }
  FLineEdit::onKeyPress(event);
}

ConsoleWidget::ConsoleWidget(finalcut::FWidget* parent) : FWidget(parent) {
  display_.setFocusable();
  input_.addCallback("activate", [this] {
    if (!input_handler_) return;
    auto value = input_.getText().toString();
    input_.clear();
    input_handler_(std::move(value));
  });
}

void ConsoleWidget::append(std::string_view bytes) {
  buffer_.append(bytes);
  display_.setText(finalcut::FString(buffer_.text()));
  display_.scrollToEnd();
  display_.redraw();
}

void ConsoleWidget::clear() {
  buffer_.clear(); display_.clear();
}

void ConsoleWidget::setInputHandler(std::function<void(std::string)> handler) {
  input_handler_ = std::move(handler);
}

void ConsoleWidget::setControlHandler(std::function<void(char)> handler) {
  input_.setControlHandler(std::move(handler));
}

void ConsoleWidget::setControlEnabled(bool enabled) { input_.setControlEnabled(enabled); }

void ConsoleWidget::focusInput() {
  input_.setFocus(); finalcut::FWidget::setFocusWidget(&input_);
}

auto ConsoleWidget::columns() const -> unsigned {
  return static_cast<unsigned>(std::max<std::size_t>(20, getWidth() > 2 ? getWidth() - 2 : 20));
}

auto ConsoleWidget::rows() const -> unsigned {
  return static_cast<unsigned>(std::max<std::size_t>(2, getHeight() > 4 ? getHeight() - 4 : 2));
}

void ConsoleWidget::adjustSize() { FWidget::adjustSize(); layoutChildren(); }

void ConsoleWidget::layoutChildren() {
  const auto width = std::max<std::size_t>(20, getWidth());
  const auto height = std::max<std::size_t>(4, getHeight());
  display_.setGeometry({1, 1}, {width, height - 2});
  prompt_.setGeometry({1, static_cast<int>(height)}, {7, 1});
  input_.setGeometry({8, static_cast<int>(height)}, {width > 7 ? width - 7 : 1, 1});
}

}  // namespace tuiide
