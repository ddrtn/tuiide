#include "tuiide/ui_dialogs.hpp"
#include "tuiide/user_settings.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

namespace tuiide {
namespace {

auto normalizedShortcut(std::string value) -> std::string {
  value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char character) {
    return std::isspace(character) != 0;
  }), value.end());
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
    return static_cast<char>(std::toupper(character));
  });
  return value;
}

auto canonicalShortcut(std::string value) -> std::string {
  const auto normalized = normalizedShortcut(std::move(value));
  static const std::map<std::string, std::string> names{
    {"CTRL+A", "Ctrl+A"}, {"CTRL+B", "Ctrl+B"},
    {"CTRL+SHIFT+B", "Ctrl+Shift+B"}, {"CTRL+D", "Ctrl+D"},
    {"CTRL+E", "Ctrl+E"}, {"CTRL+F", "Ctrl+F"}, {"CTRL+G", "Ctrl+G"},
    {"CTRL+K", "Ctrl+K"}, {"CTRL+L", "Ctrl+L"}, {"CTRL+N", "Ctrl+N"},
    {"CTRL+O", "Ctrl+O"}, {"CTRL+P", "Ctrl+P"}, {"CTRL+Q", "Ctrl+Q"},
    {"CTRL+R", "Ctrl+R"}, {"CTRL+S", "Ctrl+S"}, {"CTRL+T", "Ctrl+T"},
    {"CTRL+U", "Ctrl+U"}, {"CTRL+W", "Ctrl+W"}, {"CTRL+Y", "Ctrl+Y"},
    {"CTRL+SPACE", "Ctrl+Space"}, {"CTRL+PAGEUP", "Ctrl+PageUp"},
    {"CTRL+PAGEDOWN", "Ctrl+PageDown"}, {"ALT+A", "Alt+A"},
    {"ALT+PAGEUP", "Alt+PageUp"}, {"ALT+PAGEDOWN", "Alt+PageDown"},
    {"ALT+SHIFT+PAGEUP", "Alt+Shift+PageUp"}, {"ALT+SHIFT+PAGEDOWN", "Alt+Shift+PageDown"},
    {"ALT+B", "Alt+B"}, {"ALT+D", "Alt+D"}, {"ALT+E", "Alt+E"}, {"ALT+F", "Alt+F"},
    {"ALT+H", "Alt+H"}, {"ALT+K", "Alt+K"}, {"ALT+L", "Alt+L"},
    {"ALT+SHIFT+L", "Alt+Shift+L"},
    {"ALT+F8", "Alt+F8"},
    {"ALT+P", "Alt+P"}, {"ALT+R", "Alt+R"}, {"ALT+S", "Alt+S"}, {"ALT+T", "Alt+T"},
    {"ALT+U", "Alt+U"}, {"ALT+W", "Alt+W"}, {"ALT+SHIFT+W", "Alt+Shift+W"},
    {"ALT+SHIFT+U", "Alt+Shift+U"},
    {"F1", "F1"}, {"F2", "F2"}, {"F3", "F3"}, {"F4", "F4"},
    {"F5", "F5"}, {"F6", "F6"}, {"F7", "F7"}, {"F8", "F8"},
    {"F9", "F9"}, {"F10", "F10"}, {"F11", "F11"}, {"F12", "F12"},
    {"CTRL+F8", "Ctrl+F8"},
  };
  const auto found = names.find(normalized);
  return found == names.end() ? std::string{} : found->second;
}

auto shortcutForKey(finalcut::FKey key) -> std::string {
  static const std::map<finalcut::FKey, std::string> names{
    {finalcut::FKey::Ctrl_a, "Ctrl+A"}, {finalcut::FKey::Ctrl_b, "Ctrl+B"},
    {finalcut::FKey::Ctrl_d, "Ctrl+D"}, {finalcut::FKey::Ctrl_e, "Ctrl+E"},
    {finalcut::FKey::Ctrl_f, "Ctrl+F"}, {finalcut::FKey::Ctrl_g, "Ctrl+G"},
    {finalcut::FKey::Ctrl_k, "Ctrl+K"}, {finalcut::FKey::Ctrl_l, "Ctrl+L"},
    {finalcut::FKey::Ctrl_n, "Ctrl+N"}, {finalcut::FKey::Ctrl_o, "Ctrl+O"},
    {finalcut::FKey::Ctrl_p, "Ctrl+P"}, {finalcut::FKey::Ctrl_q, "Ctrl+Q"},
    {finalcut::FKey::Ctrl_r, "Ctrl+R"}, {finalcut::FKey::Ctrl_s, "Ctrl+S"},
    {finalcut::FKey::Ctrl_t, "Ctrl+T"}, {finalcut::FKey::Ctrl_u, "Ctrl+U"},
    {finalcut::FKey::Ctrl_w, "Ctrl+W"}, {finalcut::FKey::Ctrl_y, "Ctrl+Y"},
    {finalcut::FKey::Ctrl_space, "Ctrl+Space"}, {finalcut::FKey::Ctrl_page_up, "Ctrl+PageUp"},
    {finalcut::FKey::Ctrl_page_down, "Ctrl+PageDown"}, {finalcut::FKey::Meta_a, "Alt+A"},
    {finalcut::FKey::Meta_page_up, "Alt+PageUp"}, {finalcut::FKey::Meta_page_down, "Alt+PageDown"},
    {finalcut::FKey::Shift_Meta_page_up, "Alt+Shift+PageUp"},
    {finalcut::FKey::Shift_Meta_page_down, "Alt+Shift+PageDown"},
    {finalcut::FKey::Meta_b, "Alt+B"}, {finalcut::FKey::Meta_d, "Alt+D"},
    {finalcut::FKey::Meta_e, "Alt+E"}, {finalcut::FKey::Meta_f, "Alt+F"},
    {finalcut::FKey::Meta_h, "Alt+H"},
    {finalcut::FKey::Meta_k, "Alt+K"}, {finalcut::FKey::Meta_l, "Alt+L"},
    {finalcut::FKey::Meta_L, "Alt+Shift+L"},
    {finalcut::FKey::Meta_f8, "Alt+F8"}, {finalcut::FKey::F56, "Alt+F8"},
    {finalcut::FKey::Meta_p, "Alt+P"},
    {finalcut::FKey::Meta_r, "Alt+R"}, {finalcut::FKey::Meta_s, "Alt+S"},
    {finalcut::FKey::Meta_t, "Alt+T"},
    {finalcut::FKey::Meta_u, "Alt+U"}, {finalcut::FKey::Meta_w, "Alt+W"},
    {finalcut::FKey::Meta_U, "Alt+Shift+U"},
    {finalcut::FKey::Meta_W, "Alt+Shift+W"}, {finalcut::FKey::F1, "F1"},
    {finalcut::FKey::F2, "F2"}, {finalcut::FKey::F3, "F3"}, {finalcut::FKey::F4, "F4"},
    {finalcut::FKey::F5, "F5"}, {finalcut::FKey::F6, "F6"}, {finalcut::FKey::F7, "F7"},
    {finalcut::FKey::F8, "F8"}, {finalcut::FKey::F9, "F9"}, {finalcut::FKey::F10, "F10"},
    {finalcut::FKey::F11, "F11"}, {finalcut::FKey::F12, "F12"},
    {finalcut::FKey::F32, "Ctrl+F8"},
  };
  const auto found = names.find(key);
  return found == names.end() ? std::string{} : found->second;
}

}  // namespace

void CenteredDialog::setDialogSize(finalcut::FSize size) {
  preferred_size_ = size;
  centerDialog();
}

void CenteredDialog::setResponsiveLayout(std::function<void()> layout) {
  responsive_layout_ = std::move(layout);
  responsive_layout_();
}

void CenteredDialog::adjustSize() {
  auto* focused_widget = getWindowFocusWidget();
  if (focused_widget == nullptr) focused_widget = getFocusWidget();
  centerDialog();
  finalcut::FDialog::adjustSize();
  if (responsive_layout_) responsive_layout_();
  if (!isModal()) return;
  activateWindow();
  raiseWindow();
  setFocus();
  if (focused_widget != nullptr) {
    setWindowFocusWidget(focused_widget);
    focused_widget->setFocus();
  } else {
    focusFirstChild();
  }
}

void CenteredDialog::centerDialog() {
  if (preferred_size_.getWidth() == 0 || preferred_size_.getHeight() == 0) return;
  const auto desktop_width = getDesktopWidth();
  const auto desktop_height = getDesktopHeight();
  const auto available_width = desktop_width > 2 ? desktop_width - 2 : desktop_width;
  const auto available_height = desktop_height > 2 ? desktop_height - 2 : desktop_height;
  const auto width = std::min(preferred_size_.getWidth(), available_width);
  const auto height = std::min(preferred_size_.getHeight(), available_height);
  const auto x = 1 + static_cast<int>((desktop_width - width) / 2);
  const auto y = 1 + static_cast<int>((desktop_height - height) / 2);
  setGeometry({x, y}, {width, height}, false);
}

void EnterListBox::onKeyPress(finalcut::FKeyEvent* event) {
  if (enter_handler_ && event->key() == finalcut::FKey::Return) {
    enter_handler_();
    event->accept();
    return;
  }
  FListBox::onKeyPress(event);
}

void ShortcutCaptureEdit::onKeyPress(finalcut::FKeyEvent* event) {
  if (capture_handler_ && capture_handler_(event->key())) {
    event->accept();
    return;
  }
  if (accept_handler_ && (event->key() == finalcut::FKey::Return
      || event->key() == finalcut::FKey::Ctrl_s)) {
    accept_handler_();
    event->accept();
    return;
  }
  FLineEdit::onKeyPress(event);
}

PromptDialog::PromptDialog(const std::string& title, const std::string& label,
    finalcut::FWidget* parent)
    : CenteredDialog(finalcut::FString(title), parent),
      label_(finalcut::FString(label), this), input_(this), ok_("&OK", this),
      cancel_("&Cancel", this) {
  setDialogSize({62, 9});
  setModal();
  label_.setGeometry({2, 2}, {18, 1});
  input_.setGeometry({20, 2}, {35, 1});
  ok_.setGeometry({27, 4}, {12, 1});
  cancel_.setGeometry({42, 4}, {14, 1});
  ok_.addCallback("clicked", [this] { done(ResultCode::Accept); });
  cancel_.addCallback("clicked", [this] { done(ResultCode::Reject); });
  input_.addCallback("activate", [this] { done(ResultCode::Accept); });
  input_.setFocus();
}

auto PromptDialog::value() const -> std::string {
  return input_.getText().toString();
}

SelectionDialog::SelectionDialog(const std::string& title,
    const std::vector<std::string>& items, finalcut::FWidget* parent)
    : CenteredDialog(finalcut::FString(title), parent), list_(this),
      ok_("&OK", this), cancel_("&Cancel", this) {
  constexpr std::size_t width = 58;
  constexpr std::size_t height = 16;
  setDialogSize({width, height});
  setModal();
  list_.setGeometry({2, 1}, {width - 3, height - 4});
  ok_.setGeometry({static_cast<int>(width - 29), static_cast<int>(height - 2)}, {10, 1});
  cancel_.setGeometry({static_cast<int>(width - 16), static_cast<int>(height - 2)}, {12, 1});
  for (const auto& item : items) list_.insert(finalcut::FString(item));
  list_.setEnterHandler([this] { done(ResultCode::Accept); });
  ok_.addCallback("clicked", [this] { done(ResultCode::Accept); });
  cancel_.addCallback("clicked", [this] { done(ResultCode::Reject); });
  list_.setFocus();
}

auto SelectionDialog::selected() const -> std::size_t {
  return list_.currentItem();
}

CommandPaletteDialog::CommandPaletteDialog(std::vector<std::string> items,
    finalcut::FWidget* parent)
    : CenteredDialog("Command palette", parent), items_(std::move(items)),
      filter_label_("Search:", this), filter_(this), list_(this), run_("&Run", this),
      cancel_("&Cancel", this) {
  setDialogSize({58, 16});
  setModal();
  filter_label_.setGeometry({2, 1}, {9, 1});
  filter_.setGeometry({11, 1}, {44, 1});
  list_.setGeometry({2, 3}, {53, 9});
  run_.setGeometry({32, 13}, {10, 1});
  cancel_.setGeometry({44, 13}, {11, 1});
  filter_.addCallback("changed", [this] { refresh(); });
  filter_.addCallback("activate", [this] { accept(); });
  list_.setEnterHandler([this] { accept(); });
  run_.addCallback("clicked", [this] { accept(); });
  cancel_.addCallback("clicked", [this] { done(ResultCode::Reject); });
  refresh();
  filter_.setFocus();
}

auto CommandPaletteDialog::selected() const -> std::size_t {
  return selected_;
}

void CommandPaletteDialog::refresh() {
  list_.clear();
  visible_.clear();
  auto query = filter_.getText().toString();
  std::transform(query.begin(), query.end(), query.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  for (std::size_t index = 0; index < items_.size(); ++index) {
    auto searchable = items_[index];
    std::transform(searchable.begin(), searchable.end(), searchable.begin(), [](unsigned char character) {
      return static_cast<char>(std::tolower(character));
    });
    if (!query.empty() && searchable.find(query) == std::string::npos) continue;
    visible_.push_back(index);
    list_.insert(finalcut::FString(items_[index]));
  }
  list_.redraw();
}

void CommandPaletteDialog::accept() {
  if (visible_.empty()) return;
  const auto row = list_.currentItem();
  selected_ = visible_[row == 0 ? 0 : std::min(row - 1, visible_.size() - 1)] + 1;
  done(ResultCode::Accept);
}

TextDialog::TextDialog(std::string title, std::string text, finalcut::FWidget* parent)
    : CenteredDialog(finalcut::FString(std::move(title)), parent), text_(this),
      close_("&Close", this) {
  constexpr std::size_t width = 58;
  constexpr std::size_t height = 16;
  setDialogSize({width, height});
  setModal();
  text_.setGeometry({2, 1}, {width - 3, height - 4});
  text_.setText(finalcut::FString(std::move(text)));
  close_.setGeometry({static_cast<int>(width - 15), static_cast<int>(height - 2)}, {12, 1});
  // В текстовом диалоге Enter закрывает справку, как в обычном MessageBox.
  close_.addAccelerator(finalcut::FKey::Return);
  close_.addCallback("clicked", [this] { done(ResultCode::Accept); });
  text_.setFocus();
}

ConfirmTextDialog::ConfirmTextDialog(std::string title, std::string text,
    finalcut::FWidget* parent)
    : CenteredDialog(finalcut::FString(std::move(title)), parent), text_(this),
      apply_("&Apply", this), cancel_("&Cancel", this) {
  constexpr std::size_t width = 58;
  constexpr std::size_t height = 16;
  setDialogSize({width, height});
  setModal();
  text_.setGeometry({2, 1}, {width - 3, height - 4});
  text_.setText(finalcut::FString(std::move(text)));
  apply_.setGeometry({static_cast<int>(width - 29), static_cast<int>(height - 2)}, {10, 1});
  cancel_.setGeometry({static_cast<int>(width - 16), static_cast<int>(height - 2)}, {12, 1});
  apply_.addCallback("clicked", [this] { done(ResultCode::Accept); });
  cancel_.addCallback("clicked", [this] { done(ResultCode::Reject); });
  text_.setFocus();
}

ShortcutEditorDialog::ShortcutEditorDialog(std::vector<ShortcutEditorCommand> commands,
    std::map<std::string, std::string> overrides, finalcut::FWidget* parent)
    : CenteredDialog("Configure shortcuts", parent), commands_(std::move(commands)),
      overrides_(std::move(overrides)), filter_label_("Search:", this), filter_(this), list_(this),
      default_label_("Default:", this), current_label_("Current:", this), current_(this),
      validation_(this), capture_("&Capture", this), clear_("C&lear", this),
      reset_selected_("Reset &selected", this), reset_all_("Reset &all", this),
      apply_("A&pply", this), cancel_("Ca&ncel", this) {
  setDialogSize({88, 25});
  setModal();
  filter_label_.setGeometry({2, 1}, {9, 1}); filter_.setGeometry({11, 1}, {73, 1});
  list_.setGeometry({2, 3}, {83, 11});
  default_label_.setGeometry({2, 15}, {83, 1}); current_label_.setGeometry({2, 17}, {9, 1});
  current_.setGeometry({11, 17}, {39, 1}); validation_.setGeometry({2, 19}, {83, 1});
  capture_.setGeometry({53, 17}, {11, 1}); clear_.setGeometry({65, 17}, {10, 1});
  reset_selected_.setGeometry({2, 21}, {17, 1}); reset_all_.setGeometry({20, 21}, {13, 1});
  apply_.setGeometry({62, 21}, {10, 1}); cancel_.setGeometry({73, 21}, {12, 1});
  apply_.addAccelerator(finalcut::FKey::Ctrl_s);
  filter_.addCallback("changed", [this] { refreshList(); });
  list_.addCallback("row-changed", [this] { selectCurrent(); });
  current_.addCallback("changed", [this] { updateCurrentValue(); });
  current_.setAcceptHandler([this] {
    validate();
    if (apply_.isEnabled()) done(ResultCode::Accept);
  });
  current_.setCaptureHandler([this](finalcut::FKey key) { return captureKey(key); });
  capture_.addCallback("clicked", [this] { capture(); });
  clear_.addCallback("clicked", [this] { clearSelected(); });
  reset_selected_.addCallback("clicked", [this] { resetSelected(); });
  reset_all_.addCallback("clicked", [this] { resetAll(); });
  apply_.addCallback("clicked", [this] { if (apply_.isEnabled()) done(ResultCode::Accept); });
  cancel_.addCallback("clicked", [this] { done(ResultCode::Reject); });
  refreshList();
  filter_.setFocus();
}

auto ShortcutEditorDialog::overrides() const -> const std::map<std::string, std::string>& {
  return overrides_;
}

void ShortcutEditorDialog::refreshList() {
  auto query = filter_.getText().toString();
  std::transform(query.begin(), query.end(), query.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  const auto previous = selected_;
  visible_.clear(); list_.clear();
  for (std::size_t index = 0; index < commands_.size(); ++index) {
    auto searchable = commands_[index].title + " " + commands_[index].id;
    std::transform(searchable.begin(), searchable.end(), searchable.begin(), [](unsigned char character) {
      return static_cast<char>(std::tolower(character));
    });
    if (!query.empty() && searchable.find(query) == std::string::npos) continue;
    visible_.push_back(index);
    const auto found = overrides_.find(commands_[index].id);
    const auto value = found == overrides_.end() ? commands_[index].default_shortcut : found->second;
    list_.insert(finalcut::FString(commands_[index].title + "  | Default: "
      + commands_[index].default_shortcut + " | Current: " + (value.empty() ? "None" : value)));
  }
  if (visible_.empty()) { selected_ = 0; validate(); return; }
  const auto found = std::find(visible_.begin(), visible_.end(), previous);
  const auto row = found == visible_.end() ? 1 : static_cast<std::size_t>(std::distance(visible_.begin(), found)) + 1;
  list_.setCurrentItem(row); selected_ = visible_[row - 1]; list_.redraw(); selectCurrent();
}

auto ShortcutEditorDialog::selectedCommand() -> const ShortcutEditorCommand* {
  if (visible_.empty() || selected_ >= commands_.size()) return nullptr;
  return &commands_[selected_];
}

void ShortcutEditorDialog::selectCurrent() {
  const auto row = list_.currentItem();
  if (row == 0 || row - 1 >= visible_.size()) return;
  selected_ = visible_[row - 1];
  const auto* command = selectedCommand();
  if (!command) return;
  const auto found = overrides_.find(command->id);
  const auto value = found == overrides_.end() ? command->default_shortcut : found->second;
  updating_ = true;
  current_.setText(finalcut::FString(value));
  updating_ = false;
  default_label_.setText(finalcut::FString("Default: " + command->default_shortcut));
  validation_.setText("Type a supported shortcut, or use Clear to disable this command.");
  validate();
}

void ShortcutEditorDialog::updateCurrentValue() {
  if (updating_) return;
  const auto* command = selectedCommand();
  if (!command) return;
  const auto restore_focus = current_.hasFocus();
  const auto canonical = canonicalShortcut(current_.getText().toString());
  const auto raw = normalizedShortcut(current_.getText().toString());
  overrides_[command->id] = raw.empty() ? std::string{} : (canonical.empty() ? current_.getText().toString() : canonical);
  refreshList();
  if (restore_focus) {
    setWindowFocusWidget(&current_);
    current_.setFocus();
  }
}

void ShortcutEditorDialog::capture() {
  capturing_ = true;
  validation_.setText("Press the desired key now (Escape cancels capture).");
  validation_.redraw();
  setWindowFocusWidget(&current_);
  current_.setFocus();
}

auto ShortcutEditorDialog::captureKey(finalcut::FKey key) -> bool {
  if (!capturing_) return false;
  if (key == finalcut::FKey::Escape) {
    capturing_ = false;
    validation_.setText("Capture cancelled.");
    validation_.redraw();
    return true;
  }
  const auto value = shortcutForKey(key);
  if (value.empty()) {
    validation_.setText("Unsupported key. Use Ctrl/Alt combinations, Ctrl+F8, or F1-F12.");
    validation_.redraw();
    return true;
  }
  capturing_ = false;
  updating_ = true; current_.setText(finalcut::FString(value)); updating_ = false;
  updateCurrentValue();
  if (apply_.isEnabled()) {
    setWindowFocusWidget(&apply_);
    apply_.setFocus();
  } else {
    setWindowFocusWidget(&current_);
    current_.setFocus();
  }
  return true;
}

void ShortcutEditorDialog::clearSelected() {
  const auto* command = selectedCommand();
  if (!command) return;
  overrides_[command->id] = "";
  updating_ = true; current_.clear(); updating_ = false;
  refreshList();
}

void ShortcutEditorDialog::resetSelected() {
  const auto* command = selectedCommand();
  if (!command) return;
  overrides_.erase(command->id); refreshList();
}

void ShortcutEditorDialog::resetAll() { overrides_.clear(); refreshList(); }

void ShortcutEditorDialog::validate() {
  std::map<std::string, std::string> owners;
  std::string error;
  for (const auto& command : commands_) {
    const auto found = overrides_.find(command.id);
    const auto value = found == overrides_.end() ? command.default_shortcut : found->second;
    if (value.empty()) continue;
    const auto canonical = canonicalShortcut(value);
    if (canonical.empty()) { error = "Unsupported shortcut: " + value; break; }
    if (const auto reason = reservedShortcutReason(canonical); !reason.empty()) { error = reason; break; }
    const auto [owner, inserted] = owners.emplace(canonical, command.title);
    if (!inserted) { error = "Conflict: " + canonical + " is assigned to " + owner->second + " and " + command.title + "."; break; }
  }
  if (error.empty()) validation_.setText("Ready. Changes are saved only after Apply.");
  else validation_.setText(finalcut::FString(error));
  apply_.setEnable(error.empty());
  validation_.redraw();
  apply_.redraw();
}

ThemeEditorDialog::ThemeEditorDialog(std::string selected,
    std::map<std::string, std::string> custom_themes,
    std::function<void(const std::string&)> preview_handler, finalcut::FWidget* parent)
    : CenteredDialog("Editor theme", parent), selected_(std::move(selected)),
      custom_themes_(std::move(custom_themes)), preview_handler_(std::move(preview_handler)),
      list_(this), kind_(this), preview_(this), copy_("Create &copy", this),
      reset_("&Reset", this), apply_("&Apply", this), cancel_("Ca&ncel", this) {
  setDialogSize({76, 22});
  setModal();
  list_.setGeometry({2, 1}, {28, 15});
  kind_.setGeometry({32, 1}, {41, 1});
  preview_.setGeometry({32, 3}, {41, 13});
  copy_.setGeometry({2, 18}, {15, 1}); reset_.setGeometry({18, 18}, {10, 1});
  apply_.setGeometry({51, 18}, {10, 1}); cancel_.setGeometry({62, 18}, {12, 1});
  list_.addCallback("row-changed", [this] { preview(); });
  list_.setEnterHandler([this] { apply(); });
  copy_.addCallback("clicked", [this] { createCopy(); });
  reset_.addCallback("clicked", [this] { reset(); });
  apply_.addCallback("clicked", [this] { apply(); });
  cancel_.addCallback("clicked", [this] { done(ResultCode::Reject); });
  refresh();
  list_.setFocus();
}

auto ThemeEditorDialog::selectedTheme() const -> std::string { return selected_; }

auto ThemeEditorDialog::customThemes() const -> const std::map<std::string, std::string>& {
  return custom_themes_;
}

auto ThemeEditorDialog::currentTheme() const -> std::string {
  const auto row = list_.currentItem();
  return row == 0 || row > names_.size() ? std::string{} : names_[row - 1];
}

auto ThemeEditorDialog::currentBase() const -> std::string {
  const auto name = currentTheme();
  const auto custom = custom_themes_.find(name);
  return custom == custom_themes_.end() ? name : custom->second;
}

void ThemeEditorDialog::refresh() {
  static const std::vector<std::string> builtins{"Dark", "Light", "High contrast"};
  names_ = builtins;
  for (const auto& [name, base] : custom_themes_) {
    (void)base;
    names_.push_back(name);
  }
  list_.clear();
  std::size_t selected_row = 1;
  for (std::size_t index = 0; index < names_.size(); ++index) {
    const auto custom = custom_themes_.find(names_[index]);
    const auto suffix = custom == custom_themes_.end() ? "  [built-in]"
      : "  [copy of " + custom->second + "]";
    list_.insert(finalcut::FString(names_[index] + suffix));
    if (names_[index] == selected_) selected_row = index + 1;
  }
  list_.setCurrentItem(selected_row);
  list_.redraw();
  preview();
}

void ThemeEditorDialog::preview() {
  const auto name = currentTheme();
  if (name.empty()) return;
  const auto base = currentBase();
  const auto custom = custom_themes_.contains(name);
  kind_.setText(finalcut::FString(custom ? "User theme — editable color overrides"
    : "Built-in theme — read-only"));
  kind_.redraw();
  preview_.setText(finalcut::FString("Preview: " + name + "\nBase palette: " + base
    + "\n\n#include <iostream>\n\nint main() {\n  // UTF-8 source\n  std::cout << \"Hello\";\n  return 0;\n}"));
  preview_.redraw();
  if (preview_handler_) preview_handler_(base);
}

void ThemeEditorDialog::createCopy() {
  const auto base = currentBase();
  if (base.empty()) return;
  PromptDialog prompt("Create theme copy", "Theme name:", this);
  if (prompt.exec() != ResultCode::Accept) return;
  auto name = prompt.value();
  name.erase(name.begin(), std::find_if(name.begin(), name.end(), [](unsigned char c) {
    return std::isspace(c) == 0;
  }));
  name.erase(std::find_if(name.rbegin(), name.rend(), [](unsigned char c) {
    return std::isspace(c) == 0;
  }).base(), name.end());
  if (name.empty() || name == "Dark" || name == "Light" || name == "High contrast"
      || custom_themes_.contains(name)) {
    finalcut::FMessageBox::error(this, "Theme name is empty, reserved, or already exists.");
    return;
  }
  custom_themes_[name] = base;
  selected_ = name;
  refresh();
}

void ThemeEditorDialog::reset() {
  selected_ = "Dark";
  refresh();
}

void ThemeEditorDialog::apply() {
  const auto name = currentTheme();
  if (name.empty()) return;
  selected_ = name;
  done(ResultCode::Accept);
}

ColorEditorDialog::ColorEditorDialog(std::string theme,
    std::map<std::string, std::string> overrides,
    std::function<void(const std::map<std::string, std::string>&)> preview_handler,
    finalcut::FWidget* parent)
    : CenteredDialog("Editor colors", parent), theme_(std::move(theme)),
      overrides_(std::move(overrides)),
      roles_{"foreground", "background", "gutter", "breakpoint", "diagnosticError",
        "diagnosticWarning", "diagnosticNote", "selectionForeground", "selectionBackground",
        "executionLineBackground",
        "plain", "keyword", "type", "string", "number", "comment", "preprocessor",
        "namespace", "function", "variable", "parameter", "property", "macro", "enumMember"},
      colors_{"Black", "Blue", "Green", "Cyan", "Red", "Magenta", "Brown", "LightGray",
        "DarkGray", "LightBlue", "LightGreen", "LightCyan", "LightRed", "LightMagenta",
        "Yellow", "White"}, preview_handler_(std::move(preview_handler)),
      terminal_info_(this), roles_list_(this), palette_label_("Palette:", this),
      palette_list_(this), preview_(this), reset_role_("Reset &role", this),
      reset_all_("Reset &all", this), apply_("&Apply", this), cancel_("Ca&ncel", this) {
  setDialogSize({88, 25});
  setModal();
  if (const auto output = finalcut::FVTerm::getFOutput(); output)
    terminal_colors_ = static_cast<int>(output->getMaxColor());
  terminal_info_.setGeometry({2, 1}, {82, 1});
  roles_list_.setGeometry({2, 3}, {52, 12});
  palette_label_.setGeometry({56, 3}, {20, 1});
  palette_list_.setGeometry({56, 4}, {29, 11});
  preview_.setGeometry({2, 16}, {83, 4});
  reset_role_.setGeometry({2, 21}, {14, 1}); reset_all_.setGeometry({17, 21}, {13, 1});
  apply_.setGeometry({62, 21}, {10, 1}); cancel_.setGeometry({73, 21}, {12, 1});
  apply_.addAccelerator(finalcut::FKey::Ctrl_s);
  roles_list_.addCallback("row-changed", [this] { refreshPalette(); });
  roles_list_.setEnterHandler([this] { palette_list_.setFocus(); });
  palette_list_.addCallback("row-changed", [this] { selectColor(); });
  palette_list_.setEnterHandler([this] { selectColor(); });
  reset_role_.addCallback("clicked", [this] { resetRole(); });
  reset_all_.addCallback("clicked", [this] { resetAll(); });
  apply_.addCallback("clicked", [this] { apply(); });
  cancel_.addCallback("clicked", [this] { done(ResultCode::Reject); });
  terminal_info_.setText(finalcut::FString("Terminal palette: "
    + std::to_string(terminal_colors_) + " colors | Theme: " + theme_));
  preview_.setText("Preview: int main() { // comment\n  const char* text = \"UTF-8\"; return 42; }");
  refreshRoles();
  roles_list_.setFocus();
}

auto ColorEditorDialog::overrides() const -> const std::map<std::string, std::string>& {
  return overrides_;
}

auto ColorEditorDialog::currentRole() const -> std::string {
  const auto row = roles_list_.currentItem();
  return row == 0 || row > roles_.size() ? std::string{} : roles_[row - 1];
}

auto ColorEditorDialog::effectiveColor(const std::string& role) const -> std::string {
  if (const auto value = overrides_.find(role); value != overrides_.end()) return value->second;
  const bool light = theme_ == "Light";
  const bool contrast = theme_ == "High contrast";
  if (!light && !contrast && terminal_colors_ > 16) {
    static const std::map<std::string, int> indexed{
      {"foreground", 252}, {"background", 16}, {"gutter", 244}, {"plain", 252},
      {"keyword", 213}, {"type", 81}, {"string", 114}, {"number", 221}, {"comment", 244},
      {"preprocessor", 75}, {"namespace", 75}, {"function", 151}, {"variable", 252},
      {"parameter", 81}, {"property", 80}, {"macro", 203}, {"enumMember", 221},
      {"executionLineBackground", 22}};
    if (const auto value = indexed.find(role); value != indexed.end())
      return "Index " + std::to_string(value->second);
  }
  static const std::map<std::string, std::string> common{
    {"breakpoint", "LightRed"}, {"diagnosticError", "LightRed"},
    {"diagnosticWarning", "Yellow"}, {"diagnosticNote", "LightCyan"},
    {"selectionForeground", "White"}, {"selectionBackground", "Blue"},
    {"keyword", "LightMagenta"}, {"type", "LightCyan"}, {"string", "LightGreen"},
    {"number", "Yellow"}, {"preprocessor", "LightBlue"}, {"namespace", "LightBlue"},
    {"function", "LightGreen"}, {"parameter", "LightCyan"}, {"property", "Cyan"},
    {"macro", "LightRed"}, {"enumMember", "Yellow"}};
  if (role == "background") return light ? "White" : "Black";
  if (role == "foreground" || role == "plain" || role == "variable")
    return light ? "Black" : (contrast ? "White" : "LightGray");
  if (role == "gutter") return contrast ? "LightGray" : "DarkGray";
  if (role == "executionLineBackground")
    return light ? "LightGray" : (contrast ? "Blue" : "Green");
  if (role == "comment") return light ? "Green" : (contrast ? "LightGray" : "DarkGray");
  if (contrast && (role == "keyword" || role == "number")) return "Yellow";
  if (const auto value = common.find(role); value != common.end()) return value->second;
  return "LightGray";
}

void ColorEditorDialog::refreshRoles() {
  const auto selected = currentRole();
  roles_list_.clear();
  std::size_t selected_row = 1;
  for (std::size_t index = 0; index < roles_.size(); ++index) {
    const auto custom = overrides_.find(roles_[index]);
    const auto override = custom == overrides_.end() ? "Theme default" : custom->second;
    roles_list_.insert(finalcut::FString(roles_[index] + " | Effective: "
      + effectiveColor(roles_[index]) + " | Override: " + override));
    if (roles_[index] == selected) selected_row = index + 1;
  }
  roles_list_.setCurrentItem(selected_row);
  roles_list_.redraw();
  refreshPalette();
  if (preview_handler_) preview_handler_(overrides_);
}

void ColorEditorDialog::refreshPalette() {
  const auto role = currentRole();
  if (role.empty()) return;
  updating_ = true;
  palette_list_.clear();
  palette_list_.insert(finalcut::FString("Theme default [" + effectiveColor(role) + "]"));
  std::size_t selected_row = 1;
  const auto configured = overrides_.find(role);
  for (std::size_t index = 0; index < colors_.size(); ++index) {
    palette_list_.insert(finalcut::FString(colors_[index]));
    if (configured != overrides_.end() && configured->second == colors_[index])
      selected_row = index + 2;
  }
  palette_list_.setCurrentItem(selected_row);
  palette_list_.redraw();
  updating_ = false;
}

void ColorEditorDialog::selectColor() {
  if (updating_) return;
  const auto role = currentRole();
  const auto row = palette_list_.currentItem();
  if (role.empty() || row == 0) return;
  if (row == 1) overrides_.erase(role);
  else if (row - 2 < colors_.size()) overrides_[role] = colors_[row - 2];
  const auto restore_focus = palette_list_.hasFocus();
  refreshRoles();
  if (restore_focus) palette_list_.setFocus();
}

void ColorEditorDialog::resetRole() {
  const auto role = currentRole();
  if (role.empty()) return;
  overrides_.erase(role);
  refreshRoles();
}

void ColorEditorDialog::resetAll() { overrides_.clear(); refreshRoles(); }

void ColorEditorDialog::apply() { done(ResultCode::Accept); }

}  // namespace tuiide
