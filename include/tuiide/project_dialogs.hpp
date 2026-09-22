#pragma once

#include "tuiide/project_creation.hpp"
#include "tuiide/project_import.hpp"
#include "tuiide/project_settings.hpp"
#include "tuiide/project_template.hpp"
#include "tuiide/ui_dialogs.hpp"

#include <memory>

namespace tuiide {

/** Выбирает или создаёт путь файла строго внутри корня открытого проекта. */
class ProjectPathDialog final : public CenteredDialog {
 public:
  ProjectPathDialog(std::string title, std::filesystem::path root,
    std::filesystem::path start, std::string suggested_name,
    finalcut::FWidget* parent = nullptr, std::string language = "en");
  [[nodiscard]] auto selectedPath() const -> std::filesystem::path;

 private:
  void populate();
  void activateEntry();
  void goUp();
  void createDirectory();
  void acceptPath();

  std::filesystem::path root_;
  std::filesystem::path current_;
  std::filesystem::path selected_path_;
  std::vector<std::filesystem::path> entry_paths_;
  std::string language_;
  finalcut::FLabel path_label_;
  finalcut::FListBox entries_;
  finalcut::FLabel name_label_;
  finalcut::FLineEdit name_;
  finalcut::FButton up_;
  finalcut::FButton new_directory_;
  finalcut::FButton create_;
  finalcut::FButton cancel_;
};

/** Навигатор по каталогам проекта с безопасным созданием подкаталогов. */
class ProjectDirectoryDialog final : public CenteredDialog {
 public:
  ProjectDirectoryDialog(std::string title, std::filesystem::path start,
    finalcut::FWidget* parent = nullptr, std::string language = "en");
  [[nodiscard]] auto selectedPath() const -> std::filesystem::path;

 private:
  void populate();
  void enterDirectory();
  void createDirectory();

  std::filesystem::path current_;
  std::vector<std::filesystem::path> directories_;
  std::string language_;
  finalcut::FLabel path_;
  finalcut::FListBox entries_;
  finalcut::FButton up_;
  finalcut::FButton make_;
  finalcut::FButton select_;
  finalcut::FButton cancel_;
};

/** UI-обёртка валидируемых ProjectSettings с адаптивной раскладкой. */
class ProjectSettingsDialog final : public CenteredDialog {
 public:
  ProjectSettingsDialog(std::filesystem::path root, const ProjectSettings& settings,
    finalcut::FWidget* parent = nullptr);
  ~ProjectSettingsDialog() override;
  [[nodiscard]] auto settings(ProjectSettings& result, std::string& error) const -> bool;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/** Wizard параметров C++ класса; имена header и source редактируются независимо. */
class ClassOptionsDialog final : public CenteredDialog {
 public:
  explicit ClassOptionsDialog(std::string header_extension = "hpp",
    finalcut::FWidget* parent = nullptr, std::string language = "en");
  ~ClassOptionsDialog() override;
  [[nodiscard]] auto options() const -> CppClassOptions;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/** Первый экран wizard создания CMake-проекта. */
class NewProjectDialog final : public CenteredDialog {
 public:
  explicit NewProjectDialog(finalcut::FWidget* parent = nullptr);
  [[nodiscard]] auto options() const -> NewProjectOptions;

 private:
  void updateLanguageFields();

  finalcut::FLabel name_label_;
  finalcut::FLineEdit name_;
  finalcut::FLabel language_label_;
  finalcut::FComboBox language_;
  finalcut::FLabel target_label_;
  finalcut::FComboBox target_;
  finalcut::FLabel standard_label_;
  finalcut::FComboBox standard_;
  finalcut::FLabel header_label_;
  finalcut::FComboBox header_;
  finalcut::FLabel generator_label_;
  finalcut::FComboBox generator_;
  finalcut::FLabel build_type_label_;
  finalcut::FComboBox build_type_;
  finalcut::FLabel install_label_;
  finalcut::FComboBox install_;
  finalcut::FCheckBox warnings_;
  finalcut::FCheckBox readme_;
  finalcut::FCheckBox gitignore_;
  finalcut::FCheckBox testing_;
  finalcut::FButton next_;
  finalcut::FButton cancel_;
};

/** Собирает параметры преобразования существующего дерева исходников в CMake-проект. */
class ImportProjectDialog final : public CenteredDialog {
 public:
  ImportProjectDialog(std::string suggested_name,
    finalcut::FWidget* parent = nullptr);
  [[nodiscard]] auto options() const -> ProjectImportOptions;

 private:
  finalcut::FLabel name_label_;
  finalcut::FLineEdit name_;
  finalcut::FLabel language_label_;
  finalcut::FComboBox language_;
  finalcut::FLabel target_label_;
  finalcut::FComboBox target_;
  finalcut::FLabel standard_label_;
  finalcut::FLineEdit standard_;
  finalcut::FCheckBox warnings_;
  finalcut::FButton next_;
  finalcut::FButton cancel_;
};

}  // namespace tuiide
