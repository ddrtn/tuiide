#include "tuiide/document.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace tuiide {
namespace {
auto writeAtomically(const std::filesystem::path& destination, std::string_view data,
    std::string& error) -> bool {
  static std::atomic<unsigned long> sequence{};
  const auto directory = destination.has_parent_path() ? destination.parent_path() : std::filesystem::path{"."};
  std::filesystem::perms original_permissions{};
  std::error_code permission_error;
  const auto status = std::filesystem::status(destination, permission_error);
  const bool preserve_permissions = !permission_error && std::filesystem::exists(status);
  if (preserve_permissions) original_permissions = status.permissions();

  std::filesystem::path temporary;
  int descriptor{-1};
  for (unsigned attempt = 0; attempt < 100 && descriptor < 0; ++attempt) {
    temporary = directory / ("." + destination.filename().string() + ".tuiide-"
      + std::to_string(::getpid()) + "-" + std::to_string(sequence.fetch_add(1)));
    descriptor = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
    if (descriptor < 0 && errno != EEXIST) break;
  }
  if (descriptor < 0) {
    error = "Cannot create temporary file for " + destination.string() + ": " + std::strerror(errno);
    return false;
  }
  const auto fail = [&](std::string message) {
    const auto saved_errno = errno;
    (void)::close(descriptor); (void)::unlink(temporary.c_str());
    error = std::move(message) + ": " + std::strerror(saved_errno);
    return false;
  };
  if (preserve_permissions
      && ::fchmod(descriptor, static_cast<mode_t>(original_permissions) & 07777U) != 0)
    return fail("Cannot preserve permissions for " + destination.string());
  std::size_t offset{};
  while (offset < data.size()) {
    const auto count = ::write(descriptor, data.data() + offset, data.size() - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return fail("Write failed for " + destination.string());
    offset += static_cast<std::size_t>(count);
  }
  if (::fsync(descriptor) != 0) return fail("Cannot flush " + destination.string());
  if (::close(descriptor) != 0) {
    const auto saved_errno = errno; (void)::unlink(temporary.c_str());
    error = "Cannot close " + destination.string() + ": " + std::strerror(saved_errno); return false;
  }
  descriptor = -1;
  if (::rename(temporary.c_str(), destination.c_str()) != 0) {
    const auto saved_errno = errno; (void)::unlink(temporary.c_str());
    error = "Cannot install " + destination.string() + ": " + std::strerror(saved_errno); return false;
  }
  error.clear();
  return true;
}
}  // namespace

auto normalizePath(const std::filesystem::path& path) -> std::filesystem::path {
  if (path.empty()) return {};
  std::error_code error;
  auto absolute = std::filesystem::absolute(path, error);
  if (error) absolute = path;
  auto canonical = std::filesystem::weakly_canonical(absolute, error);
  return (error ? absolute : canonical).lexically_normal();
}

Document::Document() = default;

auto Document::load(const std::filesystem::path& path, std::string& error) -> bool {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    error = "Cannot open " + path.string();
    return false;
  }
  const std::string data{std::istreambuf_iterator<char>(input), {}};
  path_ = normalizePath(path);
  setText(data);
  disk_existed_ = diskFingerprint(path_, disk_fingerprint_, error);
  if (!disk_existed_) return false;
  error.clear();
  return true;
}

auto Document::save(std::string& error) -> bool {
  if (path_.empty()) {
    error = "No file name selected";
    return false;
  }
  return saveAs(path_, error);
}

auto Document::saveAs(const std::filesystem::path& path, std::string& error) -> bool {
  const auto destination = normalizePath(path);
  auto data = text();
  if (line_ending_ == LineEnding::CrLf) {
    std::string converted;
    converted.reserve(data.size() + lines_.size());
    for (const auto character : data) {
      if (character == '\n') converted += "\r\n";
      else converted.push_back(character);
    }
    data = std::move(converted);
  }
  if (utf8_bom_) data.insert(0, "\xef\xbb\xbf");
  if (!writeAtomically(destination, data, error)) return false;
  path_ = destination;
  disk_existed_ = diskFingerprint(path_, disk_fingerprint_, error);
  if (!disk_existed_) return false;
  saved_state_ = current_state_;
  breakGroup();
  return true;
}

void Document::relocate(const std::filesystem::path& path) { path_ = normalizePath(path); ++version_; }

void Document::setText(std::string text_value) {
  utf8_bom_ = text_value.starts_with("\xef\xbb\xbf");
  if (utf8_bom_) text_value.erase(0, 3);
  line_ending_ = LineEnding::Lf;
  if (const auto newline = text_value.find('\n'); newline != std::string::npos
      && newline > 0 && text_value[newline - 1] == '\r') line_ending_ = LineEnding::CrLf;
  if (text_value.find("\r\n") != std::string::npos) {
    std::string normalized;
    normalized.reserve(text_value.size());
    for (std::size_t index = 0; index < text_value.size(); ++index) {
      if (text_value[index] == '\r' && index + 1 < text_value.size() && text_value[index + 1] == '\n') continue;
      normalized.push_back(text_value[index]);
    }
    text_value = std::move(normalized);
  }
  lines_.clear();
  std::istringstream stream(text_value);
  std::string value;
  while (std::getline(stream, value)) {
    if (!value.empty() && value.back() == '\r') value.pop_back();
    lines_.push_back(std::move(value));
  }
  if (lines_.empty() || (!text_value.empty() && text_value.back() == '\n')) lines_.emplace_back();
  cursor_ = {};
  current_state_ = saved_state_ = next_state_ = 0;
  undo_.clear(); redo_.clear(); group_open_ = false;
  ++version_;
}

void Document::restoreText(std::string text_value) {
  setText(std::move(text_value));
  current_state_ = ++next_state_;
}

auto Document::text() const -> std::string {
  std::string result;
  for (std::size_t i = 0; i < lines_.size(); ++i) {
    result += lines_[i];
    if (i + 1 < lines_.size()) result.push_back('\n');
  }
  return result;
}

auto Document::lines() const -> const std::vector<std::string>& { return lines_; }
auto Document::line(std::size_t index) const -> const std::string& { return lines_.at(index); }
auto Document::path() const -> const std::filesystem::path& { return path_; }
auto Document::modified() const -> bool { return current_state_ != saved_state_; }
auto Document::cursor() const -> Position { return cursor_; }
auto Document::version() const -> int { return version_; }
auto Document::canUndo() const -> bool { return !undo_.empty(); }
auto Document::canRedo() const -> bool { return !redo_.empty(); }
auto Document::undoStorageBytes() const -> std::size_t {
  std::size_t bytes{};
  for (const auto& entry : undo_)
    for (const auto& atom : entry.atoms) bytes += atom.removed.size() + atom.inserted.size();
  return bytes;
}
auto Document::hasUtf8Bom() const -> bool { return utf8_bom_; }
auto Document::lineEnding() const -> LineEnding { return line_ending_; }
auto Document::hasFinalNewline() const -> bool {
  return lines_.size() > 1 && lines_.back().empty();
}

auto Document::diskFingerprint(const std::filesystem::path& path, std::string& fingerprint,
    std::string& error) -> bool {
  std::ifstream input(path, std::ios::binary);
  if (!input) { error = "Cannot read " + path.string(); return false; }
  std::uint64_t hash = 1469598103934665603ULL;
  std::uint64_t size{};
  char buffer[8192];
  while (input) {
    input.read(buffer, sizeof(buffer));
    const auto count = input.gcount();
    for (std::streamsize index = 0; index < count; ++index) {
      hash ^= static_cast<unsigned char>(buffer[index]); hash *= 1099511628211ULL;
    }
    size += static_cast<std::uint64_t>(count);
  }
  if (!input.eof()) { error = "Cannot read " + path.string(); return false; }
  fingerprint = std::to_string(size) + ":" + std::to_string(hash);
  error.clear(); return true;
}

auto Document::diskChange(std::string& error) const -> DiskChange {
  error.clear();
  if (path_.empty()) return DiskChange::Unchanged;
  std::error_code status_error;
  const bool exists = std::filesystem::exists(path_, status_error);
  if (status_error) { error = status_error.message(); return DiskChange::Unreadable; }
  if (!exists) return disk_existed_ ? DiskChange::Deleted : DiskChange::Unchanged;
  std::string fingerprint;
  if (!diskFingerprint(path_, fingerprint, error)) return DiskChange::Unreadable;
  return !disk_existed_ || fingerprint != disk_fingerprint_ ? DiskChange::Modified : DiskChange::Unchanged;
}

void Document::acknowledgeDiskState() {
  if (path_.empty()) return;
  std::string error;
  disk_existed_ = diskFingerprint(path_, disk_fingerprint_, error);
  if (!disk_existed_) disk_fingerprint_.clear();
}

auto Document::utf16Column(std::size_t line_index, std::size_t byte_column) const -> std::size_t {
  const auto& value = line(std::min(line_index, lines_.size() - 1));
  const auto limit = std::min(byte_column, value.size());
  std::size_t bytes{};
  std::size_t units{};
  while (bytes < limit) {
    const auto first = static_cast<unsigned char>(value[bytes]);
    std::size_t length = 1;
    char32_t codepoint = first;
    if ((first & 0xe0U) == 0xc0U && bytes + 1 < value.size()) { length = 2; codepoint = first & 0x1fU; }
    else if ((first & 0xf0U) == 0xe0U && bytes + 2 < value.size()) { length = 3; codepoint = first & 0x0fU; }
    else if ((first & 0xf8U) == 0xf0U && bytes + 3 < value.size()) { length = 4; codepoint = first & 0x07U; }
    for (std::size_t i = 1; i < length; ++i) codepoint = (codepoint << 6U) | (static_cast<unsigned char>(value[bytes + i]) & 0x3fU);
    if (bytes + length > limit) break;
    units += codepoint > 0xffff ? 2 : 1;
    bytes += length;
  }
  return units;
}

auto Document::byteColumn(std::size_t line_index, std::size_t utf16_column) const -> std::size_t {
  const auto& value = line(std::min(line_index, lines_.size() - 1));
  std::size_t bytes{};
  std::size_t units{};
  while (bytes < value.size() && units < utf16_column) {
    const auto first = static_cast<unsigned char>(value[bytes]);
    std::size_t length = 1;
    char32_t codepoint = first;
    if ((first & 0xe0U) == 0xc0U && bytes + 1 < value.size()) { length = 2; codepoint = first & 0x1fU; }
    else if ((first & 0xf0U) == 0xe0U && bytes + 2 < value.size()) { length = 3; codepoint = first & 0x0fU; }
    else if ((first & 0xf8U) == 0xf0U && bytes + 3 < value.size()) { length = 4; codepoint = first & 0x07U; }
    for (std::size_t i = 1; i < length; ++i) codepoint = (codepoint << 6U) | (static_cast<unsigned char>(value[bytes + i]) & 0x3fU);
    const auto next_units = units + (codepoint > 0xffff ? 2U : 1U);
    if (next_units > utf16_column) break;
    units = next_units;
    bytes += length;
  }
  return bytes;
}

auto Document::extractRange(Position start, Position end) const -> std::string {
  if (end.line < start.line || (end.line == start.line && end.column < start.column)) std::swap(start, end);
  start.line = std::min(start.line, lines_.size() - 1);
  end.line = std::min(end.line, lines_.size() - 1);
  start.column = std::min(start.column, lines_[start.line].size());
  end.column = std::min(end.column, lines_[end.line].size());
  if (start.line == end.line) return lines_[start.line].substr(start.column, end.column - start.column);
  std::string result = lines_[start.line].substr(start.column) + '\n';
  for (std::size_t line_index = start.line + 1; line_index < end.line; ++line_index) result += lines_[line_index] + '\n';
  result += lines_[end.line].substr(0, end.column);
  return result;
}

void Document::setCursor(Position position) {
  breakGroup();
  cursor_ = position;
  clampCursor();
}

auto Document::positionAfter(Position start, std::string_view text_value) -> Position {
  for (const auto character : text_value) {
    if (character == '\n') { ++start.line; start.column = 0; }
    else ++start.column;
  }
  return start;
}

void Document::replaceRaw(Position start, Position end, std::string_view replacement) {
  const auto prefix = lines_[start.line].substr(0, start.column);
  const auto suffix = lines_[end.line].substr(end.column);
  std::vector<std::string> inserted;
  std::istringstream stream{std::string(replacement)};
  std::string value;
  while (std::getline(stream, value)) inserted.push_back(std::move(value));
  if (inserted.empty()) inserted.emplace_back();
  if (!replacement.empty() && replacement.back() == '\n') inserted.emplace_back();
  inserted.front() = prefix + inserted.front();
  inserted.back() += suffix;
  auto begin = lines_.begin() + static_cast<std::ptrdiff_t>(start.line);
  begin = lines_.erase(begin, lines_.begin() + static_cast<std::ptrdiff_t>(end.line + 1));
  lines_.insert(begin, std::make_move_iterator(inserted.begin()), std::make_move_iterator(inserted.end()));
}

void Document::commit(HistoryEntry entry) {
  entry.state_before = current_state_;
  entry.state_after = ++next_state_;
  current_state_ = entry.state_after;
  redo_.clear();
  undo_.push_back(std::move(entry));
  if (undo_.size() > 200) undo_.erase(undo_.begin());
  group_open_ = undo_.back().group != EditGroup::None;
  changed();
}

void Document::breakGroup() { group_open_ = false; }

void Document::setCursorAfterEdit(Position position) {
  cursor_ = position;
  clampCursor();
  if (!undo_.empty()) undo_.back().cursor_after = cursor_;
}

void Document::replaceSingle(Position start, Position end, std::string_view replacement,
    EditGroup group) {
  if (end.line < start.line || (end.line == start.line && end.column < start.column)) std::swap(start, end);
  start.line = std::min(start.line, lines_.size() - 1);
  end.line = std::min(end.line, lines_.size() - 1);
  start.column = std::min(start.column, lines_[start.line].size());
  end.column = std::min(end.column, lines_[end.line].size());
  const auto removed = extractRange(start, end);
  const auto before = cursor_;
  const auto after = positionAfter(start, replacement);

  if (group_open_ && !undo_.empty() && undo_.back().group == group
      && undo_.back().atoms.size() == 1) {
    auto& entry = undo_.back();
    auto& atom = entry.atoms.front();
    bool merged{};
    if (group == EditGroup::Insert && removed.empty() && start == entry.cursor_after) {
      atom.inserted += replacement; merged = true;
    } else if (group == EditGroup::Backspace && replacement.empty()
        && end == atom.start && before == entry.cursor_after) {
      atom.start = start; atom.removed = removed + atom.removed; merged = true;
    } else if (group == EditGroup::DeleteForward && replacement.empty()
        && start == atom.start && before == entry.cursor_after) {
      atom.removed += removed; merged = true;
    }
    if (merged) {
      replaceRaw(start, end, replacement);
      cursor_ = after;
      entry.cursor_after = after;
      entry.state_after = ++next_state_;
      current_state_ = entry.state_after;
      changed();
      return;
    }
  }

  replaceRaw(start, end, replacement);
  cursor_ = after;
  HistoryEntry entry;
  entry.atoms.push_back({start, removed, std::string(replacement)});
  entry.cursor_before = before; entry.cursor_after = after; entry.group = group;
  commit(std::move(entry));
}

void Document::changed() {
  ++version_;
}

void Document::insert(std::string_view utf8) {
  if (utf8.empty()) return;
  replaceSingle(cursor_, cursor_, utf8,
    utf8.find('\n') == std::string_view::npos ? EditGroup::Insert : EditGroup::None);
}

void Document::replaceIdentifierBeforeCursor(std::string_view replacement) {
  auto& current = lines_[cursor_.line];
  auto begin = cursor_.column;
  while (begin > 0) {
    const auto c = static_cast<unsigned char>(current[begin - 1]);
    const bool identifier = (c >= static_cast<unsigned char>('0') && c <= static_cast<unsigned char>('9'))
      || (c >= static_cast<unsigned char>('A') && c <= static_cast<unsigned char>('Z'))
      || (c >= static_cast<unsigned char>('a') && c <= static_cast<unsigned char>('z'))
      || c == static_cast<unsigned char>('_');
    if (!identifier) break;
    --begin;
  }
  replaceSingle({cursor_.line, begin}, cursor_, replacement);
}

void Document::applyReplacements(std::vector<TextReplacement> replacements) {
  if (replacements.empty()) return;
  std::sort(replacements.begin(), replacements.end(), [](const auto& left, const auto& right) {
    return left.start.line > right.start.line || (left.start.line == right.start.line && left.start.column > right.start.column);
  });
  breakGroup();
  HistoryEntry entry;
  entry.cursor_before = cursor_;
  for (const auto& replacement : replacements) {
    if (replacement.start.line >= lines_.size() || replacement.end.line >= lines_.size()) continue;
    auto start = replacement.start;
    auto end = replacement.end;
    start.column = std::min(start.column, lines_[start.line].size());
    end.column = std::min(end.column, lines_[end.line].size());
    if (end.line < start.line || (end.line == start.line && end.column < start.column)) std::swap(start, end);
    entry.atoms.push_back({start, extractRange(start, end), replacement.text});
    replaceRaw(start, end, replacement.text);
  }
  if (entry.atoms.empty()) return;
  clampCursor();
  entry.cursor_after = cursor_;
  commit(std::move(entry));
}

void Document::replaceRange(Position start, Position end, std::string_view replacement) {
  replaceSingle(start, end, replacement);
}

void Document::replaceTextPreservingCursor(std::string_view replacement) {
  const auto old_cursor = cursor_;
  replaceSingle({0, 0}, {lines_.size() - 1, lines_.back().size()}, replacement);
  setCursorAfterEdit(old_cursor);
}

void Document::newline() {
  const auto& current = lines_[cursor_.line];
  const auto indentation = current.substr(0, current.find_first_not_of(" \t"));
  replaceSingle(cursor_, cursor_, "\n" + indentation);
}

void Document::smartNewline(std::string_view indentation_unit) {
  const auto current = lines_[cursor_.line];
  const auto indentation_end = current.find_first_not_of(" \t");
  const auto indentation = current.substr(0,
    indentation_end == std::string::npos ? current.size() : indentation_end);
  const auto before = current.substr(0, cursor_.column);
  const auto last = before.find_last_not_of(" \t");
  const auto suffix = current.substr(cursor_.column);
  const auto first = suffix.find_first_not_of(" \t");
  const char opening = last == std::string::npos ? '\0' : before[last];
  const char closing = first == std::string::npos ? '\0' : suffix[first];
  const bool opens_block = opening == '{' || opening == '[' || opening == '(';
  const bool closes_block = (opening == '{' && closing == '}')
    || (opening == '[' && closing == ']') || (opening == '(' && closing == ')');
  const auto inner = indentation + (opens_block ? std::string(indentation_unit) : std::string{});
  if (closes_block) {
    const auto start = cursor_;
    replaceSingle(start, {start.line, start.column + first}, "\n" + inner + "\n" + indentation);
    setCursorAfterEdit({start.line + 1, inner.size()});
  } else {
    replaceSingle(cursor_, cursor_, "\n" + inner);
  }
}

void Document::insertPair(char opening, char closing) {
  const auto start = cursor_;
  std::string pair{opening, closing};
  replaceSingle(start, start, pair);
  setCursorAfterEdit({start.line, start.column + 1});
}

void Document::toggleLineComment(std::size_t first_line, std::size_t last_line) {
  first_line = std::min(first_line, lines_.size() - 1);
  last_line = std::min(std::max(first_line, last_line), lines_.size() - 1);
  bool uncomment = true;
  for (auto index = first_line; index <= last_line; ++index) {
    const auto first = lines_[index].find_first_not_of(" \t");
    if (first != std::string::npos && lines_[index].compare(first, 2, "//") != 0) uncomment = false;
  }
  const auto old_cursor = cursor_;
  auto cursor_delta = std::ptrdiff_t{};
  std::vector<TextReplacement> replacements;
  for (auto index = first_line; index <= last_line; ++index) {
    const auto first = lines_[index].find_first_not_of(" \t");
    if (first == std::string::npos) continue;
    if (uncomment) {
      auto length = std::size_t{2};
      if (first + length < lines_[index].size() && lines_[index][first + length] == ' ') ++length;
      replacements.push_back({{index, first}, {index, first + length}, ""});
      if (index == old_cursor.line && old_cursor.column > first)
        cursor_delta = -static_cast<std::ptrdiff_t>(std::min(length, old_cursor.column - first));
    } else {
      replacements.push_back({{index, first}, {index, first}, "// "});
      if (index == old_cursor.line && old_cursor.column >= first) cursor_delta = 3;
    }
  }
  if (replacements.empty()) return;
  applyReplacements(std::move(replacements));
  setCursorAfterEdit({old_cursor.line,
    static_cast<std::size_t>(static_cast<std::ptrdiff_t>(old_cursor.column) + cursor_delta)});
}

void Document::duplicateLines(std::size_t first_line, std::size_t last_line) {
  first_line = std::min(first_line, lines_.size() - 1);
  last_line = std::min(std::max(first_line, last_line), lines_.size() - 1);
  const auto old_cursor = cursor_;
  const auto copy = extractRange({first_line, 0}, {last_line, lines_[last_line].size()});
  replaceSingle({last_line, lines_[last_line].size()}, {last_line, lines_[last_line].size()}, "\n" + copy);
  setCursorAfterEdit({old_cursor.line + last_line - first_line + 1, old_cursor.column});
}

void Document::moveLines(std::size_t first_line, std::size_t last_line, bool down) {
  first_line = std::min(first_line, lines_.size() - 1);
  last_line = std::min(std::max(first_line, last_line), lines_.size() - 1);
  const auto old_cursor = cursor_;
  const auto selected = extractRange({first_line, 0}, {last_line, lines_[last_line].size()});
  if (down) {
    if (last_line + 1 >= lines_.size()) return;
    const auto following = lines_[last_line + 1];
    replaceSingle({first_line, 0}, {last_line + 1, following.size()}, following + "\n" + selected);
    setCursorAfterEdit({old_cursor.line + 1, old_cursor.column});
  } else {
    if (first_line == 0) return;
    const auto preceding = lines_[first_line - 1];
    replaceSingle({first_line - 1, 0}, {last_line, lines_[last_line].size()}, selected + "\n" + preceding);
    setCursorAfterEdit({old_cursor.line - 1, old_cursor.column});
  }
}

void Document::deleteLines(std::size_t first_line, std::size_t last_line) {
  first_line = std::min(first_line, lines_.size() - 1);
  last_line = std::min(std::max(first_line, last_line), lines_.size() - 1);
  const auto old_cursor = cursor_;
  if (last_line + 1 < lines_.size()) {
    replaceSingle({first_line, 0}, {last_line + 1, 0}, "");
    setCursorAfterEdit({first_line, old_cursor.column});
  } else if (first_line > 0) {
    replaceSingle({first_line - 1, lines_[first_line - 1].size()},
      {last_line, lines_[last_line].size()}, "");
    setCursorAfterEdit({first_line - 1, old_cursor.column});
  } else {
    replaceSingle({0, 0}, {0, lines_[0].size()}, "");
    setCursorAfterEdit({0, 0});
  }
}

void Document::backspace() {
  if (cursor_ == Position{}) return;
  if (cursor_.column > 0) {
    const auto previous = previousCodepoint(lines_[cursor_.line], cursor_.column);
    replaceSingle({cursor_.line, previous}, cursor_, "", EditGroup::Backspace);
  } else {
    replaceSingle({cursor_.line - 1, lines_[cursor_.line - 1].size()}, cursor_, "");
  }
}

void Document::deleteForward() {
  auto& current = lines_[cursor_.line];
  if (cursor_.column == current.size() && cursor_.line + 1 == lines_.size()) return;
  if (cursor_.column < current.size()) {
    const auto next = nextCodepoint(current, cursor_.column);
    replaceSingle(cursor_, {cursor_.line, next}, "", EditGroup::DeleteForward);
  } else {
    replaceSingle(cursor_, {cursor_.line + 1, 0}, "");
  }
}

void Document::moveLeft() {
  breakGroup();
  if (cursor_.column > 0) cursor_.column = previousCodepoint(line(cursor_.line), cursor_.column);
  else if (cursor_.line > 0) { --cursor_.line; cursor_.column = line(cursor_.line).size(); }
}
void Document::moveRight() {
  breakGroup();
  if (cursor_.column < line(cursor_.line).size()) cursor_.column = nextCodepoint(line(cursor_.line), cursor_.column);
  else if (cursor_.line + 1 < lines_.size()) { ++cursor_.line; cursor_.column = 0; }
}
void Document::moveUp() { breakGroup(); if (cursor_.line > 0) { --cursor_.line; clampCursor(); } }
void Document::moveDown() { breakGroup(); if (cursor_.line + 1 < lines_.size()) { ++cursor_.line; clampCursor(); } }
void Document::moveHome() { breakGroup(); cursor_.column = 0; }
void Document::moveEnd() { breakGroup(); cursor_.column = line(cursor_.line).size(); }

auto Document::undo() -> bool {
  if (undo_.empty()) return false;
  breakGroup();
  auto entry = std::move(undo_.back()); undo_.pop_back();
  for (auto atom = entry.atoms.rbegin(); atom != entry.atoms.rend(); ++atom)
    replaceRaw(atom->start, positionAfter(atom->start, atom->inserted), atom->removed);
  cursor_ = entry.cursor_before;
  current_state_ = entry.state_before;
  redo_.push_back(std::move(entry));
  changed();
  return true;
}

auto Document::redo() -> bool {
  if (redo_.empty()) return false;
  breakGroup();
  auto entry = std::move(redo_.back()); redo_.pop_back();
  for (const auto& atom : entry.atoms)
    replaceRaw(atom.start, positionAfter(atom.start, atom.removed), atom.inserted);
  cursor_ = entry.cursor_after;
  current_state_ = entry.state_after;
  undo_.push_back(std::move(entry));
  changed();
  return true;
}

void Document::clampCursor() {
  cursor_.line = std::min(cursor_.line, lines_.size() - 1);
  cursor_.column = std::min(cursor_.column, lines_[cursor_.line].size());
  while (cursor_.column > 0 && cursor_.column < lines_[cursor_.line].size()
         && (static_cast<unsigned char>(lines_[cursor_.line][cursor_.column]) & 0xc0U) == 0x80U) --cursor_.column;
}

auto Document::previousCodepoint(std::string_view value, std::size_t offset) -> std::size_t {
  if (offset == 0) return 0;
  --offset;
  while (offset > 0 && (static_cast<unsigned char>(value[offset]) & 0xc0U) == 0x80U) --offset;
  return offset;
}

auto Document::nextCodepoint(std::string_view value, std::size_t offset) -> std::size_t {
  if (offset >= value.size()) return value.size();
  ++offset;
  while (offset < value.size() && (static_cast<unsigned char>(value[offset]) & 0xc0U) == 0x80U) ++offset;
  return offset;
}

}  // namespace tuiide
