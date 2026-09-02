#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace tuiide {

/** Роль фрагмента исходного текста для лексической и семантической подсветки. */
enum class TokenKind { Plain, Keyword, Type, String, Number, Comment, Preprocessor,
  Namespace, Function, Variable, Parameter, Property, Macro, EnumMember };
struct Token { std::size_t begin{}; std::size_t length{}; TokenKind kind{}; };

auto highlightCpp(std::string_view line, bool& in_block_comment) -> std::vector<Token>;
auto isCMakePath(const std::filesystem::path& path) -> bool;
auto highlightCMake(std::string_view line, int& bracket_equals,
                    bool& bracket_comment) -> std::vector<Token>;
auto completeCMake(const std::vector<std::string>& lines, std::size_t line,
                   std::size_t column) -> std::vector<std::string>;

/**
 * Построчный кэш лексической C/C++-подсветки. После правки пересчитывает только
 * изменённый участок и суффикс, на который повлияло состояние block comment.
 */
class CppSyntaxCache {
 public:
  void clear();
  void invalidateFrom(std::size_t line);
  auto update(const std::vector<std::string>& lines) -> std::size_t;
  [[nodiscard]] auto tokens(std::size_t line) const -> const std::vector<Token>&;

 private:
  struct Entry {
    std::size_t fingerprint{};
    std::size_t length{};
    bool starts_in_comment{};
    bool ends_in_comment{};
    std::vector<Token> tokens;
  };
  static auto fingerprint(std::string_view line) -> std::size_t;

  std::vector<Entry> entries_;
  std::size_t dirty_line_{};
  bool dirty_{true};
};

/** Аналогичный кэш для CMake с состоянием bracket comments и bracket strings. */
class CMakeSyntaxCache {
 public:
  void clear();
  void invalidateFrom(std::size_t line);
  auto update(const std::vector<std::string>& lines) -> std::size_t;
  [[nodiscard]] auto tokens(std::size_t line) const -> const std::vector<Token>&;

 private:
  std::vector<std::vector<Token>> entries_;
  bool dirty_{true};
};

}  // namespace tuiide
