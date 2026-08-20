#include "tuiide/ui_dialogs.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

namespace tuiide {

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

}  // namespace tuiide
