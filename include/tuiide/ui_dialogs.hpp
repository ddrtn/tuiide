#pragma once

#include <final/final.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace tuiide {

/**
 * Базовый диалог Final Cut: центрирует себя в главном окне и пересчитывает
 * расположение дочерних виджетов после изменения размера терминала.
 */
class CenteredDialog : public finalcut::FDialog {
 protected:
  using finalcut::FDialog::FDialog;

  void setDialogSize(finalcut::FSize size);
  void setResponsiveLayout(std::function<void()> layout);
  void adjustSize() override;

 private:
  void centerDialog();

  finalcut::FSize preferred_size_{};
  std::function<void()> responsive_layout_;
};

class EnterListBox final : public finalcut::FListBox {
 public:
  explicit EnterListBox(finalcut::FWidget* parent = nullptr) : FListBox(parent) {}
  void setEnterHandler(std::function<void()> handler) { enter_handler_ = std::move(handler); }

 protected:
  void onKeyPress(finalcut::FKeyEvent* event) override;

 private:
  std::function<void()> enter_handler_;
};

class PromptDialog final : public CenteredDialog {
 public:
  PromptDialog(const std::string& title, const std::string& label,
    finalcut::FWidget* parent = nullptr, std::string language = "en");
  [[nodiscard]] auto value() const -> std::string;

 private:
  finalcut::FLabel label_;
  finalcut::FLineEdit input_;
  finalcut::FButton ok_;
  finalcut::FButton cancel_;
};

class SelectionDialog final : public CenteredDialog {
 public:
  SelectionDialog(const std::string& title, const std::vector<std::string>& items,
    finalcut::FWidget* parent = nullptr, std::string language = "en");
  [[nodiscard]] auto selected() const -> std::size_t;

 private:
  EnterListBox list_;
  finalcut::FButton ok_;
  finalcut::FButton cancel_;
};

/** Поисковая палитра команд с сопоставлением подстроки без учёта регистра. */
class CommandPaletteDialog final : public CenteredDialog {
 public:
  CommandPaletteDialog(std::vector<std::string> items,
    finalcut::FWidget* parent = nullptr, std::string language = "en");
  [[nodiscard]] auto selected() const -> std::size_t;

 private:
  void refresh();
  void accept();

  std::vector<std::string> items_;
  std::vector<std::size_t> visible_;
  std::size_t selected_{};
  finalcut::FLabel filter_label_;
  finalcut::FLineEdit filter_;
  EnterListBox list_;
  finalcut::FButton run_;
  finalcut::FButton cancel_;
};

class TextDialog final : public CenteredDialog {
 public:
  TextDialog(std::string title, std::string text,
    finalcut::FWidget* parent = nullptr, std::string language = "en");

 private:
  finalcut::FTextView text_;
  finalcut::FButton close_;
};

class ConfirmTextDialog final : public CenteredDialog {
 public:
  ConfirmTextDialog(std::string title, std::string text,
    finalcut::FWidget* parent = nullptr, std::string language = "en");

 private:
  finalcut::FTextView text_;
  finalcut::FButton apply_;
  finalcut::FButton cancel_;
};

struct ShortcutEditorCommand {
  std::string id;
  std::string title;
  std::string default_shortcut;
};

/**
 * Поле захвата shortcut: следующее нажатие передаётся обработчику, а не вставляется
 * в текст. Это позволяет назначать F-клавиши и модификаторы из терминала.
 */
class ShortcutCaptureEdit final : public finalcut::FLineEdit {
 public:
  explicit ShortcutCaptureEdit(finalcut::FWidget* parent = nullptr) : FLineEdit(parent) {}
  void setCaptureHandler(std::function<bool(finalcut::FKey)> handler) { capture_handler_ = std::move(handler); }
  void setAcceptHandler(std::function<void()> handler) { accept_handler_ = std::move(handler); }
 protected:
  void onKeyPress(finalcut::FKeyEvent* event) override;
 private:
  std::function<bool(finalcut::FKey)> capture_handler_;
  std::function<void()> accept_handler_;
};

/** Редактор команд, defaults и пользовательских shortcut с проверкой конфликтов. */
class ShortcutEditorDialog final : public CenteredDialog {
 public:
  ShortcutEditorDialog(std::vector<ShortcutEditorCommand> commands,
    std::map<std::string, std::string> overrides, finalcut::FWidget* parent = nullptr,
    std::string language = "en");
  [[nodiscard]] auto overrides() const -> const std::map<std::string, std::string>&;

 private:
  void refreshList();
  void selectCurrent();
  void updateCurrentValue();
  void capture();
  auto captureKey(finalcut::FKey key) -> bool;
  void clearSelected();
  void resetSelected();
  void resetAll();
  void validate();
  auto selectedCommand() -> const ShortcutEditorCommand*;

  std::vector<ShortcutEditorCommand> commands_;
  std::vector<std::size_t> visible_;
  std::map<std::string, std::string> overrides_;
  std::string language_;
  std::size_t selected_{};
  bool updating_{};
  bool capturing_{};
  finalcut::FLabel filter_label_;
  finalcut::FLineEdit filter_;
  EnterListBox list_;
  finalcut::FLabel default_label_;
  finalcut::FLabel current_label_;
  ShortcutCaptureEdit current_;
  finalcut::FLabel validation_;
  finalcut::FButton capture_;
  finalcut::FButton clear_;
  finalcut::FButton reset_selected_;
  finalcut::FButton reset_all_;
  finalcut::FButton apply_;
  finalcut::FButton cancel_;
};

/** Выбирает тему с preview; Cancel откатывает preview, не меняя настройки. */
class ThemeEditorDialog final : public CenteredDialog {
 public:
  ThemeEditorDialog(std::string selected, std::map<std::string, std::string> custom_themes,
    std::function<void(const std::string&)> preview_handler,
    finalcut::FWidget* parent = nullptr, std::string language = "en");
  [[nodiscard]] auto selectedTheme() const -> std::string;
  [[nodiscard]] auto customThemes() const -> const std::map<std::string, std::string>&;

 private:
  void refresh();
  void preview();
  void createCopy();
  void reset();
  void apply();
  [[nodiscard]] auto currentTheme() const -> std::string;
  [[nodiscard]] auto currentBase() const -> std::string;

  std::string selected_;
  std::map<std::string, std::string> custom_themes_;
  std::vector<std::string> names_;
  std::function<void(const std::string&)> preview_handler_;
  std::string language_;
  EnterListBox list_;
  finalcut::FLabel kind_;
  finalcut::FTextView preview_;
  finalcut::FButton copy_;
  finalcut::FButton reset_;
  finalcut::FButton apply_;
  finalcut::FButton cancel_;
};

/** Редактирует override цветов ролей и применяет preview до подтверждения. */
class ColorEditorDialog final : public CenteredDialog {
 public:
  ColorEditorDialog(std::string theme, std::map<std::string, std::string> overrides,
    std::function<void(const std::map<std::string, std::string>&)> preview_handler,
    finalcut::FWidget* parent = nullptr, std::string language = "en");
  [[nodiscard]] auto overrides() const -> const std::map<std::string, std::string>&;

 private:
  void refreshRoles();
  void refreshPalette();
  void selectColor();
  void resetRole();
  void resetAll();
  void apply();
  [[nodiscard]] auto currentRole() const -> std::string;
  [[nodiscard]] auto effectiveColor(const std::string& role) const -> std::string;

  std::string theme_;
  std::map<std::string, std::string> overrides_;
  std::vector<std::string> roles_;
  std::vector<std::string> colors_;
  std::function<void(const std::map<std::string, std::string>&)> preview_handler_;
  std::string language_;
  bool updating_{};
  int terminal_colors_{16};
  finalcut::FLabel terminal_info_;
  EnterListBox roles_list_;
  finalcut::FLabel palette_label_;
  EnterListBox palette_list_;
  finalcut::FTextView preview_;
  finalcut::FButton reset_role_;
  finalcut::FButton reset_all_;
  finalcut::FButton apply_;
  finalcut::FButton cancel_;
};

}  // namespace tuiide
