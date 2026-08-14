#pragma once

#include "tuiide/document.hpp"
#include "tuiide/clipboard.hpp"
#include "tuiide/lsp_client.hpp"
#include "tuiide/syntax.hpp"
#include "tuiide/text_display.hpp"

#include <final/final.h>

#include <functional>
#include <array>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tuiide {

class CodeEditor final : public finalcut::FWidget {
 public:
  explicit CodeEditor(finalcut::FWidget* parent = nullptr);
  void setDocument(Document* document);
  void setDiagnostics(const std::vector<Diagnostic>* diagnostics);
  void setSemanticTokens(const std::vector<SemanticToken>* tokens);
  void setChangedHandler(std::function<void()> handler);
  void setCommandHandler(std::function<bool(finalcut::FKey)> handler);
  void setBreakpointProvider(std::function<bool(std::size_t)> provider);
  void setIndentation(unsigned width, bool use_spaces);
  void setTheme(std::string theme, const std::map<std::string, std::string>& overrides);
  void invalidateSyntax();
  void reveal(Position position);
  void selectRange(Position start, Position end);
  [[nodiscard]] auto hasSelection() const -> bool;
  [[nodiscard]] auto selectedRange() const -> std::optional<std::pair<Position, Position>>;
  void toggleComment();
  void duplicateLine();
  void moveLine(bool down);
  void deleteLine();
  void applyFormattedText(std::string text);

 protected:
  void draw() override;
  void onKeyPress(finalcut::FKeyEvent* event) override;
  void onMouseDown(finalcut::FMouseEvent* event) override;
  void onMouseUp(finalcut::FMouseEvent* event) override;
  void onMouseMove(finalcut::FMouseEvent* event) override;
  void onWheel(finalcut::FWheelEvent* event) override;

 private:
  void ensureVisible();
  void changed();
  [[nodiscard]] auto selectionRange() const -> std::optional<std::pair<Position, Position>>;
  auto replaceSelection(std::string_view text) -> bool;
  void startSelection();
  void clearSelection();
  [[nodiscard]] auto selectedLines() const -> std::pair<std::size_t, std::size_t>;
  [[nodiscard]] auto indentationUnit() const -> std::string;
  void moveVertical(bool down);
  void rebuildDiagnosticIndex();
  void rebuildSemanticIndex();
  void rebuildColors();
  static auto encode(char32_t codepoint) -> std::string;

  Document* document_{};
  const std::vector<Diagnostic>* diagnostics_{};
  const std::vector<SemanticToken>* semantic_tokens_{};
  struct SemanticStyle { std::size_t column{}; std::size_t length{}; std::string type; };
  std::unordered_map<std::size_t, int> diagnostic_lines_;
  std::unordered_map<std::size_t, std::vector<SemanticStyle>> semantic_lines_;
  CppSyntaxCache syntax_cache_;
  CMakeSyntaxCache cmake_syntax_cache_;
  std::function<void()> changed_handler_;
  std::function<bool(finalcut::FKey)> command_handler_;
  std::function<bool(std::size_t)> breakpoint_provider_;
  std::optional<Position> selection_anchor_;
  SystemClipboard clipboard_;
  bool mouse_selecting_{};
  std::size_t top_line_{};
  std::size_t left_column_{};
  static constexpr int gutter_width = 7;
  std::size_t tab_width_{defaultTabWidth};
  bool use_spaces_{true};
  std::string theme_{"Dark"};
  std::map<std::string, std::string> color_overrides_;
  std::array<finalcut::FColor, 14> token_colors_{};
  finalcut::FColor foreground_{finalcut::FColor::LightGray};
  finalcut::FColor background_{finalcut::FColor::Black};
  finalcut::FColor gutter_{finalcut::FColor::DarkGray};
  finalcut::FColor breakpoint_{finalcut::FColor::LightRed};
  finalcut::FColor diagnostic_error_{finalcut::FColor::LightRed};
  finalcut::FColor diagnostic_warning_{finalcut::FColor::Yellow};
  finalcut::FColor diagnostic_note_{finalcut::FColor::LightCyan};
  finalcut::FColor selection_foreground_{finalcut::FColor::White};
  finalcut::FColor selection_background_{finalcut::FColor::Blue};
};

}  // namespace tuiide
