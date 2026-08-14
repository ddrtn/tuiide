#include "tuiide/terminal_buffer.hpp"

#include <algorithm>
#include <charconv>

namespace tuiide {

TerminalBuffer::TerminalBuffer(std::size_t maximum_lines)
    : maximum_lines_(std::max<std::size_t>(1, maximum_lines)) {}

void TerminalBuffer::append(std::string_view bytes) {
  for (const unsigned char value : bytes) {
    const char character = static_cast<char>(value);
    if (state_ == State::Osc) {
      if (value == 0x07) state_ = State::Text;
      else if (value == 0x1b) state_ = State::OscEscape;
      continue;
    }
    if (state_ == State::OscEscape) {
      state_ = character == '\\' ? State::Text : State::Osc;
      continue;
    }
    if (state_ == State::Escape) {
      if (character == '[') { state_ = State::Csi; parameters_.clear(); }
      else if (character == ']') state_ = State::Osc;
      else state_ = State::Text;
      continue;
    }
    if (state_ == State::Csi) {
      if (value >= 0x40 && value <= 0x7e) { finishCsi(character); state_ = State::Text; }
      else if (parameters_.size() < 32) parameters_.push_back(character);
      continue;
    }
    if (value == 0x1b) { state_ = State::Escape; continue; }
    if (character == '\r') { cursor_ = 0; continue; }
    if (character == '\n') { finishLine(); continue; }
    if (character == '\b') {
      if (cursor_ > 0) {
        --cursor_;
        while (cursor_ > 0 && (static_cast<unsigned char>(line_[cursor_]) & 0xc0U) == 0x80U) --cursor_;
      }
      continue;
    }
    if (character == '\t') {
      const auto spaces = 8 - cursor_ % 8;
      for (std::size_t i = 0; i < spaces; ++i) {
        if (cursor_ < line_.size()) line_[cursor_] = ' ';
        else line_.push_back(' ');
        ++cursor_;
      }
      continue;
    }
    if (value < 0x20 || value == 0x7f) continue;
    if (cursor_ < line_.size()) line_[cursor_] = character;
    else line_.push_back(character);
    ++cursor_;
  }
}

void TerminalBuffer::clear() {
  lines_.clear(); line_.clear(); cursor_ = 0; state_ = State::Text; parameters_.clear();
}

auto TerminalBuffer::text() const -> std::string {
  std::string result;
  for (const auto& completed : lines_) { result += completed; result.push_back('\n'); }
  result += line_;
  return result;
}

void TerminalBuffer::finishCsi(char command) {
  unsigned amount{1};
  const auto first = parameters_.substr(0, parameters_.find(';'));
  if (!first.empty()) {
    const auto parsed = std::from_chars(first.data(), first.data() + first.size(), amount);
    if (parsed.ec != std::errc{}) amount = 1;
  }
  if (command == 'K') {
    if (parameters_ == "2") { line_.clear(); cursor_ = 0; }
    else line_.erase(std::min(cursor_, line_.size()));
  } else if (command == 'G') {
    cursor_ = std::min<std::size_t>(amount > 0 ? amount - 1 : 0, line_.size());
  } else if (command == 'C') {
    cursor_ = std::min<std::size_t>(cursor_ + amount, line_.size());
  } else if (command == 'D') {
    cursor_ = amount > cursor_ ? 0 : cursor_ - amount;
  } else if (command == 'J' && parameters_ == "2") {
    clear();
  }
}

void TerminalBuffer::finishLine() {
  lines_.push_back(std::move(line_));
  line_.clear(); cursor_ = 0;
  if (lines_.size() > maximum_lines_)
    lines_.erase(lines_.begin(), lines_.begin() + static_cast<std::ptrdiff_t>(lines_.size() - maximum_lines_));
}

}  // namespace tuiide
