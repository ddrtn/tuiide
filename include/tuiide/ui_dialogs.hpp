#pragma once

#include <final/final.h>

#include <functional>
#include <string>
#include <vector>

namespace tuiide {

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
    finalcut::FWidget* parent = nullptr);
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
    finalcut::FWidget* parent = nullptr);
  [[nodiscard]] auto selected() const -> std::size_t;

 private:
  EnterListBox list_;
  finalcut::FButton ok_;
  finalcut::FButton cancel_;
};

class CommandPaletteDialog final : public CenteredDialog {
 public:
  CommandPaletteDialog(std::vector<std::string> items,
    finalcut::FWidget* parent = nullptr);
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
    finalcut::FWidget* parent = nullptr);

 private:
  finalcut::FTextView text_;
  finalcut::FButton close_;
};

class ConfirmTextDialog final : public CenteredDialog {
 public:
  ConfirmTextDialog(std::string title, std::string text,
    finalcut::FWidget* parent = nullptr);

 private:
  finalcut::FTextView text_;
  finalcut::FButton apply_;
  finalcut::FButton cancel_;
};

}  // namespace tuiide
