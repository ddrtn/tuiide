#include "tuiide/code_editor.hpp"

#include "tuiide/syntax.hpp"
#include "tuiide/text_display.hpp"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>

namespace tuiide {
namespace {
auto namedColor(std::string name) -> std::optional<finalcut::FColor> {
  name.erase(std::remove_if(name.begin(), name.end(), [](unsigned char character) {
    return std::isspace(character) != 0;
  }), name.end());
  std::transform(name.begin(), name.end(), name.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  static const std::map<std::string, finalcut::FColor> colors{
    {"black", finalcut::FColor::Black}, {"blue", finalcut::FColor::Blue},
    {"green", finalcut::FColor::Green}, {"cyan", finalcut::FColor::Cyan},
    {"red", finalcut::FColor::Red}, {"magenta", finalcut::FColor::Magenta},
    {"brown", finalcut::FColor::Brown}, {"lightgray", finalcut::FColor::LightGray},
    {"darkgray", finalcut::FColor::DarkGray}, {"lightblue", finalcut::FColor::LightBlue},
    {"lightgreen", finalcut::FColor::LightGreen}, {"lightcyan", finalcut::FColor::LightCyan},
    {"lightred", finalcut::FColor::LightRed}, {"lightmagenta", finalcut::FColor::LightMagenta},
    {"yellow", finalcut::FColor::Yellow}, {"white", finalcut::FColor::White},
  };
  const auto found = colors.find(name);
  return found == colors.end() ? std::nullopt : std::optional<finalcut::FColor>{found->second};
}

auto terminalColor(finalcut::FColor color, int maximum) -> finalcut::FColor {
  const auto value = static_cast<unsigned>(color);
  if (maximum > 16 || value < 16) {
    if (maximum >= 16 || value < 8) return color;
  }
  return static_cast<finalcut::FColor>(value % 8);
}
auto semanticKind(std::string_view type) -> TokenKind {
  if (type == "namespace") return TokenKind::Namespace;
  if (type == "type" || type == "class" || type == "struct" || type == "enum" || type == "typeParameter" || type == "concept") return TokenKind::Type;
  if (type == "function" || type == "method") return TokenKind::Function;
  if (type == "parameter") return TokenKind::Parameter;
  if (type == "property" || type == "field") return TokenKind::Property;
  if (type == "macro") return TokenKind::Macro;
  if (type == "enumMember") return TokenKind::EnumMember;
  return TokenKind::Variable;
}
}

CodeEditor::CodeEditor(finalcut::FWidget* parent) : FWidget(parent) {
  setFocusable();
  setVisibleCursor();
  rebuildColors();
}

void CodeEditor::setDocument(Document* document) {
  document_ = document; selection_anchor_.reset(); top_line_ = 0; left_column_ = 0;
  syntax_cache_.clear(); cmake_syntax_cache_.clear();
  rebuildDiagnosticIndex(); rebuildSemanticIndex(); redraw();
}
void CodeEditor::setDiagnostics(const std::vector<Diagnostic>* diagnostics) { diagnostics_ = diagnostics; rebuildDiagnosticIndex(); redraw(); }
void CodeEditor::setSemanticTokens(const std::vector<SemanticToken>* tokens) { semantic_tokens_ = tokens; rebuildSemanticIndex(); redraw(); }
void CodeEditor::setChangedHandler(std::function<void()> handler) { changed_handler_ = std::move(handler); }
void CodeEditor::setFeedbackHandler(std::function<void(std::string, bool)> handler) {
  feedback_handler_ = std::move(handler);
}
void CodeEditor::setCommandHandler(std::function<bool(finalcut::FKey)> handler) { command_handler_ = std::move(handler); }
void CodeEditor::setBreakpointProvider(std::function<bool(std::size_t)> provider) { breakpoint_provider_ = std::move(provider); }
void CodeEditor::setIndentation(unsigned width, bool use_spaces) {
  tab_width_ = std::clamp<std::size_t>(width, 1, 16);
  use_spaces_ = use_spaces;
  if (document_) ensureVisible();
  redraw();
}
void CodeEditor::setTheme(std::string theme, const std::map<std::string, std::string>& overrides) {
  theme_ = std::move(theme); color_overrides_ = overrides; rebuildColors(); redraw();
}
void CodeEditor::invalidateSyntax() { syntax_cache_.clear(); cmake_syntax_cache_.clear(); redraw(); }

void CodeEditor::reveal(Position position) {
  if (!document_) return;
  document_->setCursor(position);
  selection_anchor_.reset();
  ensureVisible();
  redraw();
}

void CodeEditor::selectRange(Position start, Position end) {
  if (!document_) return;
  selection_anchor_ = start;
  document_->setCursor(end);
  ensureVisible();
  redraw();
}

auto CodeEditor::hasSelection() const -> bool { return selectionRange().has_value(); }
auto CodeEditor::selectedRange() const -> std::optional<std::pair<Position, Position>> { return selectionRange(); }

void CodeEditor::draw() {
  using finalcut::FColor;
  const int width = static_cast<int>(getWidth());
  const int height = static_cast<int>(getHeight());
  FWidget::setColor(foreground_, background_);
  if (!document_) {
    for (int row = 1; row <= height; ++row) { print(finalcut::FPoint{1, row}); *this << std::string(static_cast<std::size_t>(width), ' '); }
    print(finalcut::FPoint{3, 2}); *this << "Open a C/C++ file (Ctrl+O)";
    return;
  }

  const bool cmake = isCMakePath(document_->path());
  if (cmake) (void)cmake_syntax_cache_.update(document_->lines());
  else (void)syntax_cache_.update(document_->lines());
  for (int row = 0; row < height; ++row) {
    const auto line_number = top_line_ + static_cast<std::size_t>(row);
    print(finalcut::FPoint{1, row + 1});
    const auto diagnostic = diagnostic_lines_.find(line_number);
    const bool has_diagnostic = diagnostic != diagnostic_lines_.end();
    const bool has_breakpoint = breakpoint_provider_ && breakpoint_provider_(line_number);
    auto gutter_color = gutter_;
    if (has_diagnostic) gutter_color = diagnostic->second <= 1 ? diagnostic_error_
      : (diagnostic->second == 2 ? diagnostic_warning_ : diagnostic_note_);
    if (has_breakpoint) gutter_color = breakpoint_;
    FWidget::setColor(gutter_color, background_);
    std::ostringstream gutter;
    if (line_number < document_->lines().size()) gutter << std::setw(gutter_width - 2) << line_number + 1 << (has_breakpoint ? " ●" : " │");
    else gutter << std::string(gutter_width - 1, ' ') << "│";
    *this << gutter.str();
    FWidget::setColor(foreground_, background_);
    *this << std::string(static_cast<std::size_t>(std::max(0, width - gutter_width)), ' ');
    if (line_number >= document_->lines().size()) continue;

    const auto& line = document_->line(line_number);
    const auto& tokens = cmake ? cmake_syntax_cache_.tokens(line_number) : syntax_cache_.tokens(line_number);
    std::vector<TokenKind> styles(line.size(), TokenKind::Plain);
    for (const auto& token : tokens) {
      const auto end = std::min(line.size(), token.begin + token.length);
      if (token.begin < end) std::fill(styles.begin() + static_cast<std::ptrdiff_t>(token.begin), styles.begin() + static_cast<std::ptrdiff_t>(end), token.kind);
    }
    if (const auto semantic = semantic_lines_.find(line_number); semantic != semantic_lines_.end()) {
      for (const auto& token : semantic->second) {
        const auto begin = document_->byteColumn(line_number, token.column);
        const auto end = document_->byteColumn(line_number, token.column + token.length);
        if (begin < end && begin < styles.size())
          std::fill(styles.begin() + static_cast<std::ptrdiff_t>(begin), styles.begin() + static_cast<std::ptrdiff_t>(std::min(end, styles.size())), semanticKind(token.type));
      }
    }
    const auto visible_width = static_cast<std::size_t>(std::max(0, width - gutter_width));
    const auto visible_end = left_column_ + visible_width;
    print(finalcut::FPoint{gutter_width + 1, row + 1});
    const auto selection = selectionRange();
    const bool selected_line = selection && line_number >= selection->first.line && line_number <= selection->second.line;
    const auto selected_begin = selected_line && line_number == selection->first.line
      ? selection->first.column : 0;
    const auto selected_end = selected_line && line_number == selection->second.line
      ? selection->second.column : line.size();
    bool visible_base{};
    for (const auto& unit : displayUnits(line, tab_width_)) {
      const auto unit_width = unit.column_end - unit.column_begin;
      if (unit_width == 0) {
        if (visible_base && unit.column_begin >= left_column_ && unit.column_begin < visible_end)
          *this << line.substr(unit.byte_begin, unit.byte_end - unit.byte_begin);
        continue;
      }
      if (unit.column_end <= left_column_) { visible_base = false; continue; }
      if (unit.column_begin >= visible_end) break;
      const bool selected = selected_line && unit.byte_begin >= selected_begin && unit.byte_begin < selected_end;
      if (selected) FWidget::setColor(selection_foreground_, selection_background_);
      else FWidget::setColor(token_colors_[static_cast<std::size_t>(styles[unit.byte_begin])], background_);
      const auto clipped_begin = std::max(unit.column_begin, left_column_);
      const auto clipped_end = std::min(unit.column_end, visible_end);
      const auto clipped_width = clipped_end - clipped_begin;
      const bool complete = clipped_begin == unit.column_begin && clipped_end == unit.column_end;
      if (unit.tab || !complete) {
        *this << std::string(clipped_width, ' ');
        visible_base = false;
      } else {
        *this << line.substr(unit.byte_begin, unit.byte_end - unit.byte_begin);
        visible_base = true;
      }
    }
  }

  const auto cursor = document_->cursor();
  const auto cursor_column = displayColumn(document_->line(cursor.line), cursor.column, tab_width_);
  const auto relative_column = cursor_column >= left_column_ ? cursor_column - left_column_ : 0;
  const int cursor_x = gutter_width + 1 + static_cast<int>(relative_column);
  const int cursor_y = 1 + static_cast<int>(cursor.line - top_line_);
  setCursorPos({std::clamp(cursor_x, gutter_width + 1, width), std::clamp(cursor_y, 1, height)});
}

void CodeEditor::onKeyPress(finalcut::FKeyEvent* event) {
  if (!document_) return;
  const auto key = event->key();
  const auto changed_line = document_->cursor().line;
  if (command_handler_ && command_handler_(key)) { event->accept(); return; }
  bool edited = false;
  switch (key) {
    case finalcut::FKey::Ctrl_a: selection_anchor_ = Position{}; document_->setCursor({document_->lines().size() - 1, document_->lines().back().size()}); break;
    case finalcut::FKey::Ctrl_c:
      if (const auto range = selectionRange()) {
        clipboard_.copy(document_->extractRange(range->first, range->second));
        if (feedback_handler_) feedback_handler_("Copied selection to clipboard\n", false);
      }
      break;
    case finalcut::FKey::Ctrl_x:
      if (const auto range = selectionRange()) {
        clipboard_.copy(document_->extractRange(range->first, range->second));
        edited = replaceSelection("");
        if (feedback_handler_) feedback_handler_("Cut selection to clipboard\n", false);
      }
      break;
    case finalcut::FKey::Ctrl_v: {
      const auto clipboard = clipboard_.paste();
      if (!clipboard.empty()) { if (!replaceSelection(clipboard)) document_->insert(clipboard); edited = true; }
      else if (feedback_handler_) feedback_handler_("Paste unavailable: clipboard is empty\n", true);
      break;
    }
    case finalcut::FKey::Shift_left: startSelection(); document_->moveLeft(); break;
    case finalcut::FKey::Shift_right: startSelection(); document_->moveRight(); break;
    case finalcut::FKey::Shift_up: startSelection(); moveVertical(false); break;
    case finalcut::FKey::Shift_down: startSelection(); moveVertical(true); break;
    case finalcut::FKey::Shift_home: startSelection(); document_->moveHome(); break;
    case finalcut::FKey::Shift_end: startSelection(); document_->moveEnd(); break;
    case finalcut::FKey::Left: clearSelection(); document_->moveLeft(); break;
    case finalcut::FKey::Right: clearSelection(); document_->moveRight(); break;
    case finalcut::FKey::Up: clearSelection(); moveVertical(false); break;
    case finalcut::FKey::Down: clearSelection(); moveVertical(true); break;
    case finalcut::FKey::Home: clearSelection(); document_->moveHome(); break;
    case finalcut::FKey::End: clearSelection(); document_->moveEnd(); break;
    case finalcut::FKey::Page_up:
      clearSelection();
      for (std::size_t i = 0; i < getHeight(); ++i) moveVertical(false);
      break;
    case finalcut::FKey::Page_down:
      clearSelection();
      for (std::size_t i = 0; i < getHeight(); ++i) moveVertical(true);
      break;
    case finalcut::FKey::Return:
      if (!replaceSelection("\n")) document_->smartNewline(indentationUnit());
      edited = true;
      break;
    case finalcut::FKey::Backspace:
    case finalcut::FKey::Erase: if (!replaceSelection("")) document_->backspace(); edited = true; break;
    case finalcut::FKey::Del_char: if (!replaceSelection("")) document_->deleteForward(); edited = true; break;
    case finalcut::FKey::Tab: {
      std::string indentation;
      if (use_spaces_) {
        const auto cursor = document_->cursor();
        const auto column = displayColumn(document_->line(cursor.line), cursor.column, tab_width_);
        indentation.assign(tab_width_ - column % tab_width_, ' ');
      } else indentation = "\t";
      if (!replaceSelection(indentation)) document_->insert(indentation);
      edited = true;
      break;
    }
    case finalcut::FKey::Ctrl_z: clearSelection(); edited = document_->undo(); break;
    case finalcut::FKey::Ctrl_y: clearSelection(); edited = document_->redo(); break;
    default: {
      const auto value = static_cast<char32_t>(key);
      if (value >= 0x20 && value <= 0x10ffff && value < 0x01000000) {
        const auto cursor = document_->cursor();
        const auto& line = document_->line(cursor.line);
        const auto next = cursor.column < line.size() ? line[cursor.column] : '\0';
        const auto previous = cursor.column > 0 ? line[cursor.column - 1] : '\0';
        const char character = value <= 0x7f ? static_cast<char>(value) : '\0';
        const bool closing = character == ')' || character == ']' || character == '}'
          || character == '\'' || character == '"';
        if (!hasSelection() && closing && next == character) {
          document_->moveRight();
        } else if (!hasSelection() && (character == '(' || character == '[' || character == '{')) {
          const char close = character == '(' ? ')' : (character == '[' ? ']' : '}');
          document_->insertPair(character, close); edited = true;
        } else if (!hasSelection() && (character == '\'' || character == '"') && previous != '\\'
            && (next == '\0' || std::isspace(static_cast<unsigned char>(next)) != 0
              || std::string_view(")]},;:").find(next) != std::string_view::npos)) {
          document_->insertPair(character, character); edited = true;
        } else {
          const auto text = encode(value); if (!replaceSelection(text)) document_->insert(text); edited = true;
        }
      }
      else { event->ignore(); return; }
    }
  }
  if (edited) {
    const auto invalidated_line = std::min(changed_line, document_->cursor().line);
    if (isCMakePath(document_->path())) cmake_syntax_cache_.invalidateFrom(invalidated_line);
    else syntax_cache_.invalidateFrom(invalidated_line);
  }
  ensureVisible();
  if (edited) changed(); else redraw();
  event->accept();
}

void CodeEditor::onMouseDown(finalcut::FMouseEvent* event) {
  if (!document_ || event->getButton() != finalcut::MouseButton::Left) return;
  const int row = event->getY() - 1;
  if (row >= 0) {
    const auto line = std::min(top_line_ + static_cast<std::size_t>(row), document_->lines().size() - 1);
    const auto screen_column = left_column_
      + static_cast<std::size_t>(std::max(0, event->getX() - gutter_width - 1));
    const auto column = byteColumnAtDisplay(document_->line(line), screen_column, tab_width_);
    document_->setCursor({line, column}); selection_anchor_ = document_->cursor(); mouse_selecting_ = true; ensureVisible(); redraw(); setFocus();
  }
}

void CodeEditor::onMouseUp(finalcut::FMouseEvent*) { mouse_selecting_ = false; if (selectionRange() == std::nullopt) selection_anchor_.reset(); }

void CodeEditor::onMouseMove(finalcut::FMouseEvent* event) {
  if (!document_ || !mouse_selecting_ || (event->getButton() & finalcut::MouseButton::Left) == finalcut::MouseButton::None) return;
  const auto line = std::min(top_line_ + static_cast<std::size_t>(std::max(0, event->getY() - 1)), document_->lines().size() - 1);
  const auto screen_column = left_column_
    + static_cast<std::size_t>(std::max(0, event->getX() - gutter_width - 1));
  const auto column = byteColumnAtDisplay(document_->line(line), screen_column, tab_width_);
  document_->setCursor({line, column}); ensureVisible(); redraw();
}

void CodeEditor::onWheel(finalcut::FWheelEvent* event) {
  if (!document_) return;
  if (event->getWheel() == finalcut::MouseWheel::Up) top_line_ = top_line_ > 3 ? top_line_ - 3 : 0;
  else top_line_ = std::min(top_line_ + 3, document_->lines().size() - 1);
  redraw();
}

void CodeEditor::ensureVisible() {
  const auto cursor = document_->cursor();
  const auto height = std::max<std::size_t>(1, getHeight());
  if (cursor.line < top_line_) top_line_ = cursor.line;
  else if (cursor.line >= top_line_ + height) top_line_ = cursor.line - height + 1;
  const auto width = getWidth() > gutter_width ? getWidth() - gutter_width : 1;
  const auto cursor_column = displayColumn(document_->line(cursor.line), cursor.column, tab_width_);
  if (cursor_column < left_column_) left_column_ = cursor_column;
  else if (cursor_column >= left_column_ + width) left_column_ = cursor_column - width + 1;
}
void CodeEditor::changed() { if (changed_handler_) changed_handler_(); redraw(); }

auto CodeEditor::selectionRange() const -> std::optional<std::pair<Position, Position>> {
  if (!document_ || !selection_anchor_ || *selection_anchor_ == document_->cursor()) return std::nullopt;
  auto start = *selection_anchor_;
  auto end = document_->cursor();
  if (end.line < start.line || (end.line == start.line && end.column < start.column)) std::swap(start, end);
  return std::pair{start, end};
}
auto CodeEditor::replaceSelection(std::string_view text) -> bool {
  const auto range = selectionRange();
  if (!range) return false;
  document_->replaceRange(range->first, range->second, text);
  selection_anchor_.reset();
  return true;
}
void CodeEditor::startSelection() { if (!selection_anchor_) selection_anchor_ = document_->cursor(); }
void CodeEditor::clearSelection() { selection_anchor_.reset(); }

auto CodeEditor::selectedLines() const -> std::pair<std::size_t, std::size_t> {
  if (const auto range = selectionRange()) {
    auto last = range->second.line;
    if (range->second.column == 0 && last > range->first.line) --last;
    return {range->first.line, last};
  }
  const auto line = document_ ? document_->cursor().line : 0;
  return {line, line};
}

auto CodeEditor::indentationUnit() const -> std::string {
  return use_spaces_ ? std::string(tab_width_, ' ') : std::string("\t");
}

void CodeEditor::toggleComment() {
  if (!document_) return;
  const auto [first, last] = selectedLines();
  document_->toggleLineComment(first, last); clearSelection(); changed(); ensureVisible();
}

void CodeEditor::duplicateLine() {
  if (!document_) return;
  const auto [first, last] = selectedLines();
  document_->duplicateLines(first, last); clearSelection(); changed(); ensureVisible();
}

void CodeEditor::moveLine(bool down) {
  if (!document_) return;
  const auto before = document_->version();
  const auto [first, last] = selectedLines();
  document_->moveLines(first, last, down); clearSelection();
  if (document_->version() != before) changed();
  else {
    redraw();
    if (feedback_handler_) feedback_handler_(down
      ? "Move line down unavailable: already at the end of the document\n"
      : "Move line up unavailable: already at the beginning of the document\n", true);
  }
  ensureVisible();
}

void CodeEditor::deleteLine() {
  if (!document_) return;
  const auto [first, last] = selectedLines();
  document_->deleteLines(first, last); clearSelection(); changed(); ensureVisible();
}

void CodeEditor::applyFormattedText(std::string text) {
  if (!document_ || text == document_->text()) return;
  const auto selection = selectionRange();
  document_->replaceTextPreservingCursor(text);
  if (selection) {
    const auto clamp = [this](Position position) {
      position.line = std::min(position.line, document_->lines().size() - 1);
      position.column = std::min(position.column, document_->line(position.line).size());
      return position;
    };
    selection_anchor_ = clamp(selection->first);
    document_->setCursor(clamp(selection->second));
  }
  syntax_cache_.clear(); cmake_syntax_cache_.clear();
  ensureVisible(); changed();
}

void CodeEditor::moveVertical(bool down) {
  if (!document_) return;
  const auto cursor = document_->cursor();
  if ((!down && cursor.line == 0) || (down && cursor.line + 1 >= document_->lines().size())) return;
  const auto screen_column = displayColumn(document_->line(cursor.line), cursor.column, tab_width_);
  const auto line = down ? cursor.line + 1 : cursor.line - 1;
  document_->setCursor({line, byteColumnAtDisplay(document_->line(line), screen_column, tab_width_)});
}

void CodeEditor::rebuildDiagnosticIndex() {
  diagnostic_lines_.clear();
  if (!document_ || !diagnostics_) return;
  for (const auto& diagnostic : *diagnostics_) {
    if (diagnostic.path != document_->path()) continue;
    const auto severity = diagnostic.severity == 0 ? 3 : diagnostic.severity;
    const auto [found, inserted] = diagnostic_lines_.try_emplace(diagnostic.position.line, severity);
    if (!inserted) found->second = std::min(found->second, severity);
  }
}

void CodeEditor::rebuildColors() {
  using finalcut::FColor;
  const auto maximum = finalcut::FVTerm::getFOutput() ? finalcut::FVTerm::getFOutput()->getMaxColor() : 16;
  const bool palette256 = maximum > 16;
  const bool light = theme_ == "Light";
  const bool contrast = theme_ == "High contrast";
  foreground_ = light ? FColor::Black : FColor::LightGray;
  background_ = light ? FColor::White : FColor::Black;
  gutter_ = light ? FColor::DarkGray : FColor::DarkGray;
  selection_foreground_ = light ? FColor::White : FColor::White;
  selection_background_ = light ? FColor::Blue : FColor::Blue;
  diagnostic_error_ = FColor::LightRed; diagnostic_warning_ = FColor::Yellow;
  diagnostic_note_ = FColor::LightCyan; breakpoint_ = FColor::LightRed;
  token_colors_ = {foreground_, FColor::LightMagenta, FColor::LightCyan, FColor::LightGreen,
    FColor::Yellow, light ? FColor::Green : FColor::DarkGray, FColor::LightBlue,
    FColor::LightBlue, FColor::LightGreen, foreground_, FColor::LightCyan,
    FColor::Cyan, FColor::LightRed, FColor::Yellow};
  if (palette256 && !light && !contrast) {
    foreground_ = static_cast<FColor>(252); background_ = static_cast<FColor>(16);
    gutter_ = static_cast<FColor>(244); diagnostic_error_ = static_cast<FColor>(203);
    diagnostic_warning_ = static_cast<FColor>(221); diagnostic_note_ = static_cast<FColor>(81);
    token_colors_ = {foreground_, static_cast<FColor>(213), static_cast<FColor>(81),
      static_cast<FColor>(114), static_cast<FColor>(221), static_cast<FColor>(244),
      static_cast<FColor>(75), static_cast<FColor>(75), static_cast<FColor>(151),
      foreground_, static_cast<FColor>(81), static_cast<FColor>(80),
      static_cast<FColor>(203), static_cast<FColor>(221)};
  }
  if (contrast) {
    foreground_ = FColor::White; background_ = FColor::Black; gutter_ = FColor::LightGray;
    token_colors_ = {foreground_, FColor::Yellow, FColor::LightCyan, FColor::LightGreen,
      FColor::White, FColor::LightGray, FColor::LightCyan, FColor::LightCyan,
      FColor::LightGreen, FColor::White, FColor::LightCyan, FColor::Cyan,
      FColor::LightRed, FColor::Yellow};
  }
  const std::map<std::string, finalcut::FColor*> roles{
    {"foreground", &foreground_}, {"background", &background_}, {"gutter", &gutter_},
    {"breakpoint", &breakpoint_}, {"diagnosticError", &diagnostic_error_},
    {"diagnosticWarning", &diagnostic_warning_}, {"diagnosticNote", &diagnostic_note_},
    {"selectionForeground", &selection_foreground_}, {"selectionBackground", &selection_background_},
  };
  for (const auto& [role, value] : color_overrides_) {
    const auto color = namedColor(value);
    if (!color) continue;
    if (const auto target = roles.find(role); target != roles.end()) *target->second = *color;
    static const std::map<std::string, TokenKind> tokens{
      {"plain", TokenKind::Plain}, {"keyword", TokenKind::Keyword}, {"type", TokenKind::Type},
      {"string", TokenKind::String}, {"number", TokenKind::Number}, {"comment", TokenKind::Comment},
      {"preprocessor", TokenKind::Preprocessor}, {"namespace", TokenKind::Namespace},
      {"function", TokenKind::Function}, {"variable", TokenKind::Variable},
      {"parameter", TokenKind::Parameter}, {"property", TokenKind::Property},
      {"macro", TokenKind::Macro}, {"enumMember", TokenKind::EnumMember},
    };
    if (const auto token = tokens.find(role); token != tokens.end())
      token_colors_[static_cast<std::size_t>(token->second)] = *color;
  }
  foreground_ = terminalColor(foreground_, maximum); background_ = terminalColor(background_, maximum);
  gutter_ = terminalColor(gutter_, maximum); breakpoint_ = terminalColor(breakpoint_, maximum);
  diagnostic_error_ = terminalColor(diagnostic_error_, maximum);
  diagnostic_warning_ = terminalColor(diagnostic_warning_, maximum);
  diagnostic_note_ = terminalColor(diagnostic_note_, maximum);
  selection_foreground_ = terminalColor(selection_foreground_, maximum);
  selection_background_ = terminalColor(selection_background_, maximum);
  for (auto& color : token_colors_) color = terminalColor(color, maximum);
  setForegroundColor(foreground_); setBackgroundColor(background_);
}

void CodeEditor::rebuildSemanticIndex() {
  semantic_lines_.clear();
  if (!document_ || !semantic_tokens_) return;
  for (const auto& token : *semantic_tokens_)
    if (token.path == document_->path()) semantic_lines_[token.line].push_back({token.column, token.length, token.type});
}

auto CodeEditor::encode(char32_t c) -> std::string {
  std::string out;
  if (c <= 0x7f) out.push_back(static_cast<char>(c));
  else if (c <= 0x7ff) { out.push_back(static_cast<char>(0xc0 | (c >> 6))); out.push_back(static_cast<char>(0x80 | (c & 0x3f))); }
  else if (c <= 0xffff) { out.push_back(static_cast<char>(0xe0 | (c >> 12))); out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3f))); out.push_back(static_cast<char>(0x80 | (c & 0x3f))); }
  else { out.push_back(static_cast<char>(0xf0 | (c >> 18))); out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3f))); out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3f))); out.push_back(static_cast<char>(0x80 | (c & 0x3f))); }
  return out;
}

}  // namespace tuiide
