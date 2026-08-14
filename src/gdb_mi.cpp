#include "tuiide/gdb_mi.hpp"

#include <charconv>
#include <cctype>

namespace tuiide {
namespace {
class Parser {
 public:
  explicit Parser(std::string_view input) : input_(input) {}

  auto record() -> MiRecord {
    MiRecord result;
    const auto token_begin = position_;
    while (position_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[position_]))) ++position_;
    if (position_ > token_begin) {
      int token{};
      if (std::from_chars(input_.data() + token_begin, input_.data() + position_, token).ec == std::errc{})
        result.token = token;
    }
    if (position_ >= input_.size()) { result.error = "missing record prefix"; return result; }
    result.prefix = input_[position_++];
    if (result.prefix == '~' || result.prefix == '@' || result.prefix == '&') {
      auto value = cstring();
      if (!error_.empty()) result.error = error_;
      else result.stream = std::move(value.text);
      return result;
    }
    if (result.prefix != '^' && result.prefix != '*' && result.prefix != '+' && result.prefix != '=') {
      result.error = "unknown record prefix"; return result;
    }
    result.klass = word();
    while (consume(',')) {
      auto name = word();
      if (name.empty() || !consume('=')) { fail("invalid result"); break; }
      result.results.names.push_back(std::move(name));
      result.results.values.push_back(value());
      if (!error_.empty()) break;
    }
    skipSpace();
    if (error_.empty() && position_ != input_.size()) fail("trailing input");
    result.error = error_;
    return result;
  }

 private:
  auto value() -> MiValue {
    if (position_ >= input_.size()) { fail("missing value"); return {}; }
    if (input_[position_] == '"') return cstring();
    if (input_[position_] == '{') return aggregate('}', MiValueKind::Tuple);
    if (input_[position_] == '[') return aggregate(']', MiValueKind::List);
    fail("invalid value"); return {};
  }
  auto aggregate(char close, MiValueKind kind) -> MiValue {
    MiValue result(kind); ++position_;
    if (consume(close)) return result;
    while (position_ < input_.size()) {
      const auto saved = position_;
      auto name = word();
      if (!name.empty() && consume('=')) {
        result.names.push_back(std::move(name)); result.values.push_back(value());
      } else {
        position_ = saved; result.names.emplace_back(); result.values.push_back(value());
      }
      if (!error_.empty()) return result;
      if (consume(close)) return result;
      if (!consume(',')) { fail("missing aggregate separator"); return result; }
    }
    fail(std::string("missing '") + close + "'"); return result;
  }
  auto cstring() -> MiValue {
    MiValue result;
    if (!consume('"')) { fail("missing string"); return result; }
    while (position_ < input_.size()) {
      char character = input_[position_++];
      if (character == '"') return result;
      if (character != '\\') { result.text.push_back(character); continue; }
      if (position_ >= input_.size()) { fail("unfinished escape"); return result; }
      character = input_[position_++];
      switch (character) {
        case 'a': result.text.push_back('\a'); break; case 'b': result.text.push_back('\b'); break;
        case 'f': result.text.push_back('\f'); break; case 'n': result.text.push_back('\n'); break;
        case 'r': result.text.push_back('\r'); break; case 't': result.text.push_back('\t'); break;
        case 'v': result.text.push_back('\v'); break; case '\\': result.text.push_back('\\'); break;
        case '"': result.text.push_back('"'); break;
        default:
          if (character >= '0' && character <= '7') {
            unsigned value = static_cast<unsigned>(character - '0');
            for (int count = 1; count < 3 && position_ < input_.size()
                && input_[position_] >= '0' && input_[position_] <= '7'; ++count)
              value = value * 8U + static_cast<unsigned>(input_[position_++] - '0');
            result.text.push_back(static_cast<char>(value & 0xffU));
          } else result.text.push_back(character);
      }
    }
    fail("unterminated string"); return result;
  }
  auto word() -> std::string {
    const auto begin = position_;
    while (position_ < input_.size()) {
      const char character = input_[position_];
      if (!std::isalnum(static_cast<unsigned char>(character)) && character != '-' && character != '_') break;
      ++position_;
    }
    return std::string(input_.substr(begin, position_ - begin));
  }
  auto consume(char expected) -> bool {
    skipSpace();
    if (position_ >= input_.size() || input_[position_] != expected) return false;
    ++position_; return true;
  }
  void skipSpace() { while (position_ < input_.size() && (input_[position_] == ' ' || input_[position_] == '\r')) ++position_; }
  void fail(std::string message) { if (error_.empty()) error_ = std::move(message) + " at byte " + std::to_string(position_); }

  std::string_view input_;
  std::size_t position_{};
  std::string error_;
};
}

auto MiValue::find(std::string_view name) const -> const MiValue* {
  for (std::size_t index{}; index < names.size() && index < values.size(); ++index)
    if (names[index] == name) return &values[index];
  return nullptr;
}
auto MiValue::string(std::string_view name) const -> std::string {
  const auto* value = find(name);
  return value && value->kind == MiValueKind::String ? value->text : std::string{};
}
auto parseMiRecord(std::string_view line) -> MiRecord { return Parser(line).record(); }

}  // namespace tuiide
