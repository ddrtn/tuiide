#pragma once

#include <filesystem>
#include <cstdint>
#include <string>
#include <vector>

namespace tuiide {

[[nodiscard]] auto normalizePath(const std::filesystem::path& path) -> std::filesystem::path;

struct Position {
  std::size_t line{};
  std::size_t column{};  // UTF-8 byte offset within the line
  auto operator==(const Position&) const -> bool = default;
};

struct TextReplacement {
  Position start;
  Position end;
  std::string text;
};

enum class LineEnding { Lf, CrLf };
enum class DiskChange { Unchanged, Modified, Deleted, Unreadable };

class Document {
 public:
  Document();

  auto load(const std::filesystem::path& path, std::string& error) -> bool;
  auto save(std::string& error) -> bool;
  auto saveAs(const std::filesystem::path& path, std::string& error) -> bool;
  void relocate(const std::filesystem::path& path);
  void setText(std::string text);
  void restoreText(std::string text);

  [[nodiscard]] auto text() const -> std::string;
  [[nodiscard]] auto lines() const -> const std::vector<std::string>&;
  [[nodiscard]] auto line(std::size_t index) const -> const std::string&;
  [[nodiscard]] auto path() const -> const std::filesystem::path&;
  [[nodiscard]] auto modified() const -> bool;
  [[nodiscard]] auto cursor() const -> Position;
  [[nodiscard]] auto version() const -> int;
  [[nodiscard]] auto canUndo() const -> bool;
  [[nodiscard]] auto canRedo() const -> bool;
  [[nodiscard]] auto undoStorageBytes() const -> std::size_t;
  [[nodiscard]] auto hasUtf8Bom() const -> bool;
  [[nodiscard]] auto lineEnding() const -> LineEnding;
  [[nodiscard]] auto hasFinalNewline() const -> bool;
  [[nodiscard]] auto diskChange(std::string& error) const -> DiskChange;
  void acknowledgeDiskState();
  [[nodiscard]] auto utf16Column(std::size_t line, std::size_t byte_column) const -> std::size_t;
  [[nodiscard]] auto byteColumn(std::size_t line, std::size_t utf16_column) const -> std::size_t;
  [[nodiscard]] auto extractRange(Position start, Position end) const -> std::string;

  void setCursor(Position position);
  void insert(std::string_view utf8);
  void replaceIdentifierBeforeCursor(std::string_view replacement);
  void applyReplacements(std::vector<TextReplacement> replacements);
  void replaceRange(Position start, Position end, std::string_view replacement);
  void replaceTextPreservingCursor(std::string_view replacement);
  void newline();
  void smartNewline(std::string_view indentation_unit);
  void insertPair(char opening, char closing);
  void toggleLineComment(std::size_t first_line, std::size_t last_line);
  void duplicateLines(std::size_t first_line, std::size_t last_line);
  void moveLines(std::size_t first_line, std::size_t last_line, bool down);
  void deleteLines(std::size_t first_line, std::size_t last_line);
  void backspace();
  void deleteForward();
  void moveLeft();
  void moveRight();
  void moveUp();
  void moveDown();
  void moveHome();
  void moveEnd();
  auto undo() -> bool;
  auto redo() -> bool;

 private:
  struct EditAtom {
    Position start;
    std::string removed;
    std::string inserted;
  };
  enum class EditGroup { None, Insert, Backspace, DeleteForward };
  struct HistoryEntry {
    std::vector<EditAtom> atoms;
    Position cursor_before;
    Position cursor_after;
    std::uint64_t state_before{};
    std::uint64_t state_after{};
    EditGroup group{EditGroup::None};
  };

  void replaceSingle(Position start, Position end, std::string_view replacement,
    EditGroup group = EditGroup::None);
  void replaceRaw(Position start, Position end, std::string_view replacement);
  void commit(HistoryEntry entry);
  void breakGroup();
  void setCursorAfterEdit(Position position);
  void changed();
  void clampCursor();
  [[nodiscard]] static auto positionAfter(Position start, std::string_view text) -> Position;
  static auto previousCodepoint(std::string_view text, std::size_t offset) -> std::size_t;
  static auto nextCodepoint(std::string_view text, std::size_t offset) -> std::size_t;
  [[nodiscard]] static auto diskFingerprint(const std::filesystem::path& path,
    std::string& fingerprint, std::string& error) -> bool;

  std::filesystem::path path_;
  std::vector<std::string> lines_{1};
  Position cursor_{};
  int version_{1};
  std::uint64_t current_state_{};
  std::uint64_t saved_state_{};
  std::uint64_t next_state_{};
  bool group_open_{};
  std::vector<HistoryEntry> undo_;
  std::vector<HistoryEntry> redo_;
  bool utf8_bom_{};
  LineEnding line_ending_{LineEnding::Lf};
  std::string disk_fingerprint_;
  bool disk_existed_{};
};

}  // namespace tuiide
