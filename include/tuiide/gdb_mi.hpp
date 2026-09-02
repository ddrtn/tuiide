#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tuiide {

/** Формы значений MI: строка, именованный tuple или список значений. */
enum class MiValueKind { String, Tuple, List };

struct MiValue {
  MiValue(MiValueKind value_kind = MiValueKind::String) : kind(value_kind) {}
  MiValueKind kind{MiValueKind::String};
  std::string text;
  std::vector<std::string> names;
  std::vector<MiValue> values;

  [[nodiscard]] auto find(std::string_view name) const -> const MiValue*;
  [[nodiscard]] auto string(std::string_view name) const -> std::string;
};

/** Одна синтаксически разобранная строка протокола GDB/MI. */
struct MiRecord {
  std::optional<int> token;
  char prefix{};
  std::string klass;
  MiValue results{MiValueKind::Tuple};
  std::string stream;
  std::string error;

  [[nodiscard]] auto valid() const -> bool { return error.empty() && prefix != 0; }
  [[nodiscard]] auto result(std::string_view name) const -> const MiValue* { return results.find(name); }
  [[nodiscard]] auto string(std::string_view name) const -> std::string { return results.string(name); }
};

/** Синтаксические ошибки записываются в поле `error`, а не выбрасываются наружу. */
auto parseMiRecord(std::string_view line) -> MiRecord;

}  // namespace tuiide
