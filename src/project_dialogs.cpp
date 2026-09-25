#include "tuiide/project_dialogs.hpp"

#include "tuiide/document.hpp"
#include "tuiide/ui_localization.hpp"

#include <algorithm>
#include <utility>
#include <vector>

namespace tuiide {

ProjectPathDialog::ProjectPathDialog(std::string title, std::filesystem::path root,
    std::filesystem::path start, std::string suggested_name,
    finalcut::FWidget* parent, std::string language)
    : CenteredDialog(finalcut::FString(localizedUiText(language, title)), parent),
      root_(normalizePath(root)), current_(normalizePath(start)),
      language_(std::move(language)), path_label_(this),
      entries_(this),
      name_label_(finalcut::FString(localizedUiText(language_, "File name:")), this),
      name_(this),
      up_(finalcut::FString(localizedUiText(language_, "&Up")), this),
      new_directory_(finalcut::FString(localizedUiText(language_, "New &directory")), this),
      create_(finalcut::FString(localizedUiText(language_, "&Create")), this),
      cancel_(finalcut::FString(localizedUiText(language_, "&Cancel")), this) {
  std::error_code relative_error;
  const auto relative = std::filesystem::relative(current_, root_, relative_error);
  if (relative_error || (!relative.empty() && *relative.begin() == "..")) current_ = root_;
  setDialogSize({72, 20});
  setModal();
  path_label_.setGeometry({2, 1}, {53, 1});
  entries_.setGeometry({2, 2}, {53, 7});
  name_label_.setGeometry({2, 10}, {13, 1});
  name_.setGeometry({16, 10}, {39, 1});
  name_.setText(finalcut::FString(std::move(suggested_name)));
  up_.setGeometry({2, 13}, {9, 1});
  new_directory_.setGeometry({13, 13}, {17, 1});
  create_.setGeometry({32, 13}, {10, 1});
  cancel_.setGeometry({44, 13}, {11, 1});
  up_.addCallback("clicked", [this] { goUp(); });
  new_directory_.addCallback("clicked", [this] { createDirectory(); });
  create_.addCallback("clicked", [this] { acceptPath(); });
  cancel_.addCallback("clicked", [this] { done(ResultCode::Reject); });
  name_.addCallback("activate", [this] { acceptPath(); });
  entries_.addCallback("row-selected", [this] { activateEntry(); });
  entries_.addCallback("clicked", [this] { activateEntry(); });
  populate();
  name_.setFocus();
}

auto ProjectPathDialog::selectedPath() const -> std::filesystem::path {
  return selected_path_;
}

void ProjectPathDialog::populate() {
  entries_.clear();
  entry_paths_.clear();
  std::error_code iterator_error;
  for (std::filesystem::directory_iterator iterator(current_,
         std::filesystem::directory_options::skip_permission_denied, iterator_error), end;
       iterator != end; iterator.increment(iterator_error)) {
    if (iterator_error) { iterator_error.clear(); continue; }
    const auto status = iterator->symlink_status(iterator_error);
    if (iterator_error || std::filesystem::is_symlink(status)) {
      iterator_error.clear();
      continue;
    }
    if (std::filesystem::is_directory(status) || std::filesystem::is_regular_file(status))
      entry_paths_.push_back(iterator->path());
  }
  std::sort(entry_paths_.begin(), entry_paths_.end(), [](const auto& left, const auto& right) {
    std::error_code left_error, right_error;
    const bool left_dir = std::filesystem::is_directory(left, left_error);
    const bool right_dir = std::filesystem::is_directory(right, right_error);
    return left_dir != right_dir ? left_dir
      : left.filename().string() < right.filename().string();
  });
  for (const auto& path : entry_paths_) {
    std::error_code type_error;
    const auto directory = std::filesystem::is_directory(path, type_error);
    entries_.insert(finalcut::FString((directory ? "[DIR] " : "      ")
      + path.filename().string()));
  }
  std::error_code relative_error;
  const auto relative = std::filesystem::relative(current_, root_, relative_error);
  path_label_.setText(finalcut::FString(localizedUiText(language_, "Project / ")
    + (relative_error || relative == "." ? std::string{} : relative.generic_string())));
  path_label_.redraw();
  entries_.redraw();
}

void ProjectPathDialog::activateEntry() {
  const auto index = entries_.currentItem();
  if (index == 0 || index > entry_paths_.size()) return;
  std::error_code type_error;
  if (std::filesystem::is_directory(entry_paths_[index - 1], type_error)) {
    current_ = entry_paths_[index - 1];
    populate();
    return;
  }
  name_.setText(finalcut::FString(entry_paths_[index - 1].filename().string()));
  name_.redraw();
}

void ProjectPathDialog::goUp() {
  if (current_ == root_) return;
  current_ = current_.parent_path();
  populate();
}

void ProjectPathDialog::createDirectory() {
  PromptDialog prompt("New directory", "Directory name:", this, language_);
  if (prompt.exec() != ResultCode::Accept) return;
  const auto value = std::filesystem::path(prompt.value());
  if (value.empty() || value.has_parent_path() || value == "." || value == "..") {
    finalcut::FMessageBox::error(this,
      finalcut::FString(localizedUiText(language_, "Enter one valid directory name.")));
    return;
  }
  std::error_code create_error;
  if (!std::filesystem::create_directory(current_ / value, create_error)) {
    finalcut::FMessageBox::error(this, finalcut::FString(create_error
      ? localizedUiText(language_, "Cannot create directory: ") + create_error.message()
      : localizedUiText(language_, "Directory already exists.")));
    return;
  }
  current_ /= value;
  populate();
}

void ProjectPathDialog::acceptPath() {
  const auto value = std::filesystem::path(name_.getText().trim().toString());
  if (value.empty() || value.has_parent_path() || value == "." || value == "..") {
    finalcut::FMessageBox::error(this,
      finalcut::FString(localizedUiText(language_, "Enter one valid file name.")));
    return;
  }
  const auto candidate = current_ / value;
  if (std::filesystem::exists(candidate)) {
    finalcut::FMessageBox::error(this,
      finalcut::FString(localizedUiText(language_, "The selected file already exists.")));
    return;
  }
  selected_path_ = candidate;
  done(ResultCode::Accept);
}

ProjectDirectoryDialog::ProjectDirectoryDialog(std::string title,
    std::filesystem::path start, finalcut::FWidget* parent, std::string language)
    : CenteredDialog(finalcut::FString(localizedUiText(language, title)), parent),
      current_(normalizePath(start)), language_(std::move(language)),
      path_(this), entries_(this),
      up_(finalcut::FString(localizedUiText(language_, "&Up")), this),
      make_(finalcut::FString(localizedUiText(language_, "New &directory")), this),
      select_(finalcut::FString(localizedUiText(language_, "&Select")), this),
      cancel_(finalcut::FString(localizedUiText(language_, "&Cancel")), this) {
  setDialogSize({70, 18});
  setModal();
  path_.setGeometry({2, 1}, {53, 1}); entries_.setGeometry({2, 2}, {53, 9});
  up_.setGeometry({2, 13}, {9, 1}); make_.setGeometry({13, 13}, {17, 1});
  select_.setGeometry({32, 13}, {10, 1}); cancel_.setGeometry({44, 13}, {11, 1});
  up_.addCallback("clicked", [this] {
    if (current_ == current_.root_path()) return;
    current_ = current_.parent_path();
    populate();
  });
  make_.addCallback("clicked", [this] { createDirectory(); });
  select_.addCallback("clicked", [this] { done(ResultCode::Accept); });
  cancel_.addCallback("clicked", [this] { done(ResultCode::Reject); });
  entries_.addCallback("row-selected", [this] { enterDirectory(); });
  entries_.addCallback("clicked", [this] { enterDirectory(); });
  populate();
  entries_.setFocus();
}

auto ProjectDirectoryDialog::selectedPath() const -> std::filesystem::path {
  return current_;
}

void ProjectDirectoryDialog::populate() {
  entries_.clear();
  directories_.clear();
  std::error_code error;
  for (std::filesystem::directory_iterator iterator(current_,
         std::filesystem::directory_options::skip_permission_denied, error), end;
       iterator != end; iterator.increment(error)) {
    if (error) { error.clear(); continue; }
    const auto status = iterator->symlink_status(error);
    if (!error && std::filesystem::is_directory(status)
        && !std::filesystem::is_symlink(status))
      directories_.push_back(iterator->path());
    error.clear();
  }
  std::sort(directories_.begin(), directories_.end());
  for (const auto& directory : directories_)
    entries_.insert("[DIR] " + directory.filename().string());
  path_.setText(finalcut::FString(current_.string()));
  path_.redraw();
  entries_.redraw();
}

void ProjectDirectoryDialog::enterDirectory() {
  const auto index = entries_.currentItem();
  if (index == 0 || index > directories_.size()) return;
  current_ = directories_[index - 1];
  populate();
}

void ProjectDirectoryDialog::createDirectory() {
  PromptDialog prompt("New directory", "Directory name:", this, language_);
  if (prompt.exec() != ResultCode::Accept) return;
  const std::filesystem::path name(prompt.value());
  if (name.empty() || name.has_parent_path() || name == "." || name == "..") {
    finalcut::FMessageBox::error(this,
      finalcut::FString(localizedUiText(language_, "Enter one valid directory name.")));
    return;
  }
  std::error_code error;
  if (!std::filesystem::create_directory(current_ / name, error)) {
    finalcut::FMessageBox::error(this,
      finalcut::FString(error ? error.message()
        : localizedUiText(language_, "Directory already exists.")));
    return;
  }
  current_ /= name;
  populate();
}

struct ProjectSettingsDialog::Impl {
  Impl(ProjectSettingsDialog* dialog, std::filesystem::path project_root,
      const ProjectSettings& settings, std::string language)
      : owner(dialog), root(std::move(project_root)), original(settings),
        language(std::move(language)),
        source_label(finalcut::FString(localizedUiText(this->language, "Source directory:")), owner), source(owner),
        build_label(finalcut::FString(localizedUiText(this->language, "Build directory:")), owner), build(owner),
        browse(finalcut::FString(localizedUiText(this->language, "B&rowse...")), owner),
        generator_label(finalcut::FString(localizedUiText(this->language, "Generator:")), owner), generator(owner),
        toolchain_label(finalcut::FString(localizedUiText(this->language, "Toolchain file:")), owner), toolchain(owner),
        c_compiler_label(finalcut::FString(localizedUiText(this->language, "C compiler:")), owner), c_compiler(owner),
        cpp_compiler_label(finalcut::FString(localizedUiText(this->language, "C++ compiler:")), owner), cpp_compiler(owner),
        debugger_label(finalcut::FString(localizedUiText(this->language, "Debugger:")), owner), debugger(owner),
        adapter_label(finalcut::FString(localizedUiText(this->language, "DAP adapter:")), owner), adapter(owner),
        c_standard_label(finalcut::FString(localizedUiText(this->language, "C standard:")), owner), c_standard(owner),
        cpp_standard_label(finalcut::FString(localizedUiText(this->language, "C++ standard:")), owner), cpp_standard(owner),
        build_type_label(finalcut::FString(localizedUiText(this->language, "Build type:")), owner), build_type(owner),
        jobs_label(finalcut::FString(localizedUiText(this->language, "Jobs:")), owner),
        jobs(owner), tab_width_label(finalcut::FString(localizedUiText(this->language, "Tab width:")), owner), tab_width(owner),
        use_spaces(finalcut::FString(localizedUiText(this->language, "Insert spaces")), owner),
        environment_label(finalcut::FString(localizedUiText(this->language, "Environment:")), owner),
        environment(owner), clangd_label("clangd arguments:", owner), clangd(owner),
        environment_help(owner), save(finalcut::FString(localizedUiText(this->language, "&Save")), owner),
        cancel(finalcut::FString(localizedUiText(this->language, "&Cancel")), owner) {
    source.setText(finalcut::FString(root.string())); source.setReadOnly();
    build.setText(finalcut::FString(settings.build_directory.string()));
    generator.setText(finalcut::FString(settings.generator));
    jobs.setText(finalcut::FString(std::to_string(settings.build_jobs)));
    toolchain.setText(finalcut::FString(settings.toolchain.string()));
    c_compiler.setText(finalcut::FString(settings.c_compiler.string()));
    cpp_compiler.setText(finalcut::FString(settings.cpp_compiler.string()));
    setupCombo(debugger, {"GDB/MI", "LLDB/DAP"},
      settings.debugger_backend == "lldb-dap" ? "LLDB/DAP" : "GDB/MI");
    adapter.setText(finalcut::FString(settings.debugger_adapter.string()));
    setupCombo(c_standard, {"Inherit", "90", "99", "11", "17", "23"}, settings.c_standard);
    setupCombo(cpp_standard, {"Inherit", "98", "11", "14", "17", "20", "23", "26"}, settings.cpp_standard);
    setupCombo(build_type, {"Inherit", "Debug", "Release", "RelWithDebInfo", "MinSizeRel"}, settings.build_type);
    environment.setText(finalcut::FString(formatEnvironmentSettings(settings.environment)));
    clangd.setText(finalcut::FString(formatArgumentList(settings.clangd_arguments)));
    tab_width.setText(finalcut::FString(std::to_string(settings.tab_width)));
    if (settings.use_spaces) use_spaces.setChecked(); else use_spaces.unsetChecked();
    clangd_label.setText(finalcut::FString(localizedUiText(this->language, "clangd arguments:")));
    environment_help.setText(finalcut::FString(localizedUiText(this->language,
      "Use NAME=value; OTHER=value")));
    browse.addCallback("clicked", [this] {
      const auto current = std::filesystem::path(build.getText().trim().toString());
      ProjectDirectoryDialog dialog("Select build directory",
        current.empty() ? root.parent_path() : current, owner, this->language);
      if (dialog.exec() == finalcut::FDialog::ResultCode::Accept)
        build.setText(finalcut::FString(dialog.selectedPath().string()));
    });
    save.addCallback("clicked", [this] { owner->done(finalcut::FDialog::ResultCode::Accept); });
    cancel.addCallback("clicked", [this] { owner->done(finalcut::FDialog::ResultCode::Reject); });
    build.setFocus();
  }

  auto readSettings(ProjectSettings& result, std::string& error) const -> bool {
    result = original;
    result.version = 1;
    result.build_directory = pathValue(build);
    result.generator = generator.getText().trim().toString();
    result.toolchain = pathValue(toolchain);
    result.c_compiler = pathValue(c_compiler);
    result.cpp_compiler = pathValue(cpp_compiler);
    result.debugger_backend = debugger.getText().toString() == "LLDB/DAP" ? "lldb-dap" : "gdb-mi";
    result.debugger_adapter = pathValue(adapter);
    result.c_standard = comboValue(c_standard);
    result.cpp_standard = comboValue(cpp_standard);
    result.build_type = comboValue(build_type);
    try {
      const auto text = jobs.getText().trim().toString();
      std::size_t parsed{};
      const auto value = std::stoul(text, &parsed);
      if (parsed != text.size() || value == 0 || value > 1024) throw std::out_of_range("jobs");
      result.build_jobs = static_cast<unsigned>(value);
    } catch (const std::exception&) {
      error = localizedUiText(language, "Parallel jobs must be between 1 and 1024.");
      return false;
    }
    try {
      const auto text = tab_width.getText().trim().toString();
      std::size_t parsed{};
      const auto value = std::stoul(text, &parsed);
      if (parsed != text.size() || value == 0 || value > 16) throw std::out_of_range("tab width");
      result.tab_width = static_cast<unsigned>(value);
    } catch (const std::exception&) {
      error = localizedUiText(language, "Tab width must be between 1 and 16.");
      return false;
    }
    result.use_spaces = use_spaces.isChecked();
    if (!parseEnvironmentSettings(environment.getText().trim().toString(), result.environment, error)) {
      error = localizedUiText(language, error);
      return false;
    }
    if (!parseArgumentList(clangd.getText().trim().toString(), result.clangd_arguments, error)
        || !validateProjectSettings(root, result, error)) {
      error = localizedUiText(language, error);
      return false;
    }
    return true;
  }

  void layoutControls() {
    const bool compact = owner->getWidth() < 70 || owner->getHeight() < 20;
    if (compact) {
      source_label.setGeometry({2, 1}, {18, 1}); source.setGeometry({20, 1}, {35, 1});
      build_label.setGeometry({2, 2}, {18, 1}); build.setGeometry({20, 2}, {24, 1}); browse.setGeometry({46, 2}, {9, 1});
      generator_label.setGeometry({2, 3}, {18, 1}); generator.setGeometry({20, 3}, {18, 1});
      jobs_label.setGeometry({40, 3}, {6, 1}); jobs.setGeometry({47, 3}, {8, 1});
      toolchain_label.setGeometry({2, 4}, {18, 1}); toolchain.setGeometry({20, 4}, {35, 1});
      c_compiler_label.setGeometry({2, 5}, {18, 1}); c_compiler.setGeometry({20, 5}, {35, 1});
      cpp_compiler_label.setGeometry({2, 6}, {18, 1}); cpp_compiler.setGeometry({20, 6}, {35, 1});
      debugger_label.setGeometry({2, 7}, {18, 1}); debugger.setGeometry({20, 7}, {16, 1});
      adapter_label.setGeometry({2, 8}, {18, 1}); adapter.setGeometry({20, 8}, {35, 1});
      c_standard_label.setGeometry({2, 9}, {11, 1}); c_standard.setGeometry({13, 9}, {8, 1});
      cpp_standard_label.setGeometry({22, 9}, {13, 1}); cpp_standard.setGeometry({35, 9}, {8, 1});
      build_type_label.setGeometry({2, 10}, {11, 1}); build_type.setGeometry({13, 10}, {15, 1});
      environment_label.setGeometry({2, 11}, {18, 1}); environment.setGeometry({20, 11}, {35, 1});
      clangd_label.setGeometry({2, 12}, {18, 1}); clangd.setGeometry({20, 12}, {35, 1});
      tab_width_label.setGeometry({2, 13}, {12, 1}); tab_width.setGeometry({14, 13}, {6, 1});
      use_spaces.setGeometry({22, 13}, {18, 1}); environment_help.setGeometry({2, 14}, {32, 1});
      save.setGeometry({32, 15}, {10, 1}); cancel.setGeometry({44, 15}, {11, 1});
      return;
    }
    source_label.setGeometry({2, 1}, {19, 1}); source.setGeometry({22, 1}, {53, 1});
    build_label.setGeometry({2, 3}, {19, 1}); build.setGeometry({22, 3}, {41, 1}); browse.setGeometry({65, 3}, {10, 1});
    generator_label.setGeometry({2, 5}, {19, 1}); generator.setGeometry({22, 5}, {24, 1});
    jobs_label.setGeometry({49, 5}, {7, 1}); jobs.setGeometry({57, 5}, {8, 1});
    toolchain_label.setGeometry({2, 7}, {19, 1}); toolchain.setGeometry({22, 7}, {53, 1});
    c_compiler_label.setGeometry({2, 9}, {19, 1}); c_compiler.setGeometry({22, 9}, {22, 1});
    cpp_compiler_label.setGeometry({45, 9}, {15, 1}); cpp_compiler.setGeometry({60, 9}, {15, 1});
    debugger_label.setGeometry({2, 11}, {19, 1}); debugger.setGeometry({22, 11}, {18, 1});
    adapter_label.setGeometry({42, 11}, {13, 1}); adapter.setGeometry({55, 11}, {20, 1});
    c_standard_label.setGeometry({2, 13}, {12, 1}); c_standard.setGeometry({14, 13}, {12, 1});
    cpp_standard_label.setGeometry({28, 13}, {14, 1}); cpp_standard.setGeometry({42, 13}, {12, 1});
    build_type_label.setGeometry({56, 13}, {11, 1}); build_type.setGeometry({67, 13}, {9, 1});
    environment_label.setGeometry({2, 15}, {19, 1}); environment.setGeometry({22, 15}, {53, 1});
    clangd_label.setGeometry({2, 17}, {19, 1}); clangd.setGeometry({22, 17}, {53, 1});
    tab_width_label.setGeometry({2, 19}, {12, 1}); tab_width.setGeometry({14, 19}, {6, 1});
    use_spaces.setGeometry({23, 19}, {18, 1}); environment_help.setGeometry({43, 19}, {32, 1});
    save.setGeometry({52, 21}, {10, 1}); cancel.setGeometry({65, 21}, {11, 1});
  }

  auto pathValue(const finalcut::FLineEdit& field) const -> std::filesystem::path {
    const std::filesystem::path path(field.getText().trim().toString());
    return path.empty() ? path : normalizePath(path.is_absolute() ? path : root / path);
  }
  static void setupCombo(finalcut::FComboBox& combo, const std::vector<std::string>& values,
      const std::string& selected) {
    for (const auto& value : values) combo.insert(finalcut::FString(value));
    combo.setText(finalcut::FString(selected.empty() ? "Inherit" : selected));
    combo.unsetEditable();
  }
  static auto comboValue(const finalcut::FComboBox& combo) -> std::string {
    const auto value = combo.getText().toString();
    return value == "Inherit" ? std::string{} : value;
  }

  ProjectSettingsDialog* owner;
  std::filesystem::path root;
  ProjectSettings original;
  std::string language;
  finalcut::FLabel source_label; finalcut::FLineEdit source;
  finalcut::FLabel build_label; finalcut::FLineEdit build; finalcut::FButton browse;
  finalcut::FLabel generator_label; finalcut::FLineEdit generator;
  finalcut::FLabel toolchain_label; finalcut::FLineEdit toolchain;
  finalcut::FLabel c_compiler_label; finalcut::FLineEdit c_compiler;
  finalcut::FLabel cpp_compiler_label; finalcut::FLineEdit cpp_compiler;
  finalcut::FLabel debugger_label; finalcut::FComboBox debugger;
  finalcut::FLabel adapter_label; finalcut::FLineEdit adapter;
  finalcut::FLabel c_standard_label; finalcut::FComboBox c_standard;
  finalcut::FLabel cpp_standard_label; finalcut::FComboBox cpp_standard;
  finalcut::FLabel build_type_label; finalcut::FComboBox build_type;
  finalcut::FLabel jobs_label; finalcut::FLineEdit jobs;
  finalcut::FLabel tab_width_label; finalcut::FLineEdit tab_width;
  finalcut::FCheckBox use_spaces;
  finalcut::FLabel environment_label; finalcut::FLineEdit environment;
  finalcut::FLabel clangd_label; finalcut::FLineEdit clangd;
  finalcut::FLabel environment_help; finalcut::FButton save; finalcut::FButton cancel;
};

ProjectSettingsDialog::ProjectSettingsDialog(std::filesystem::path root,
    const ProjectSettings& settings, finalcut::FWidget* parent, std::string language)
    : CenteredDialog(finalcut::FString(localizedUiText(language, "Project Settings")), parent) {
  setDialogSize({78, 26});
  setModal();
  impl_ = std::make_unique<Impl>(this, std::move(root), settings, std::move(language));
  setResponsiveLayout([this] { impl_->layoutControls(); });
}

ProjectSettingsDialog::~ProjectSettingsDialog() = default;

auto ProjectSettingsDialog::settings(ProjectSettings& result, std::string& error) const -> bool {
  return impl_->readSettings(result, error);
}

struct ClassOptionsDialog::Impl {
  Impl(ClassOptionsDialog* dialog, std::string header_extension, std::string_view language)
      : owner(dialog),
        class_label(finalcut::FString(localizedUiText(language, "Class name:")), owner), class_name(owner),
        header_file_label(finalcut::FString(localizedUiText(language, "Header file:")), owner), header_file(owner),
        source_file_label(finalcut::FString(localizedUiText(language, "Source file:")), owner), source_file(owner),
        namespace_label(finalcut::FString(localizedUiText(language, "Namespace:")), owner), namespace_name(owner),
        base_label(finalcut::FString(localizedUiText(language, "Base class:")), owner), base_class(owner),
        base_header_label(finalcut::FString(localizedUiText(language, "Base header:")), owner), base_header(owner),
        access_label(finalcut::FString(localizedUiText(language, "Inheritance:")), owner), access(owner),
        constructor(finalcut::FString(localizedUiText(language, "Generate constructor")), owner),
        destructor(finalcut::FString(localizedUiText(language, "Generate destructor")), owner),
        virtual_destructor(finalcut::FString(localizedUiText(language, "Virtual destructor")), owner),
        final_class(finalcut::FString(localizedUiText(language, "Final class")), owner),
        copy(finalcut::FString(localizedUiText(language, "Copy operations")), owner),
        move(finalcut::FString(localizedUiText(language, "Move operations")), owner),
        next(finalcut::FString(localizedUiText(language, "&Proceed")), owner),
        cancel(finalcut::FString(localizedUiText(language, "&Cancel")), owner),
        header_extension_(header_extension == "h" ? "h" : "hpp") {
    class_label.setGeometry({2, 1}, {14, 1}); class_name.setGeometry({18, 1}, {50, 1});
    header_file_label.setGeometry({2, 3}, {14, 1}); header_file.setGeometry({18, 3}, {50, 1});
    source_file_label.setGeometry({2, 5}, {14, 1}); source_file.setGeometry({18, 5}, {50, 1});
    header_suggestion_ = "NewClass." + header_extension_;
    source_suggestion_ = "NewClass.cpp";
    header_file.setText(finalcut::FString(header_suggestion_));
    source_file.setText(finalcut::FString(source_suggestion_));
    namespace_label.setGeometry({2, 7}, {14, 1}); namespace_name.setGeometry({18, 7}, {50, 1});
    base_label.setGeometry({2, 9}, {14, 1}); base_class.setGeometry({18, 9}, {22, 1});
    base_header_label.setGeometry({42, 9}, {13, 1}); base_header.setGeometry({56, 9}, {12, 1});
    access_label.setGeometry({2, 11}, {14, 1}); access.setGeometry({18, 11}, {22, 1});
    access.insert("public"); access.insert("protected"); access.insert("private");
    access.setCurrentItem(1); access.unsetEditable();
    constructor.setGeometry({2, 13}, {28, 1}); destructor.setGeometry({34, 13}, {28, 1});
    virtual_destructor.setGeometry({2, 14}, {28, 1}); final_class.setGeometry({34, 14}, {24, 1});
    copy.setGeometry({2, 15}, {28, 1}); move.setGeometry({34, 15}, {28, 1});
    constructor.setChecked(); destructor.setChecked(); virtual_destructor.setChecked();
    next.setGeometry({45, 18}, {10, 1}); cancel.setGeometry({57, 18}, {12, 1});
    next.addCallback("clicked", [this] {
      owner->done(finalcut::FDialog::ResultCode::Accept);
    });
    cancel.addCallback("clicked", [this] {
      owner->done(finalcut::FDialog::ResultCode::Reject);
    });
    class_name.addCallback("activate", [this] {
      header_file.moveCursorToEnd();
      header_file.setFocus();
    });
    header_file.addCallback("activate", [this] {
      source_file.moveCursorToEnd();
      source_file.setFocus();
    });
    source_file.addCallback("activate", [this] { namespace_name.setFocus(); });
    class_name.addCallback("changed", [this] { updateSuggestedNames(); });
    class_name.setFocus();
  }

  void updateSuggestedNames() {
    auto base = class_name.getText().trim().toString();
    if (base.empty()) base = "NewClass";
    const auto next_header = base + "." + header_extension_;
    const auto next_source = base + ".cpp";
    if (header_file.getText().trim().toString() == header_suggestion_) {
      header_file.setText(finalcut::FString(next_header));
      header_file.redraw();
    }
    if (source_file.getText().trim().toString() == source_suggestion_) {
      source_file.setText(finalcut::FString(next_source));
      source_file.redraw();
    }
    header_suggestion_ = next_header;
    source_suggestion_ = next_source;
  }

  auto options() const -> CppClassOptions {
    CppClassOptions result;
    result.class_name = class_name.getText().trim().toString();
    result.header_file_name = header_file.getText().trim().toString();
    result.source_file_name = source_file.getText().trim().toString();
    result.namespace_name = namespace_name.getText().trim().toString();
    result.base_class = base_class.getText().trim().toString();
    result.base_header = base_header.getText().trim().toString();
    const auto selected_access = access.getText().toString();
    result.inheritance = selected_access == "protected" ? InheritanceAccess::Protected
      : (selected_access == "private" ? InheritanceAccess::Private : InheritanceAccess::Public);
    result.final_class = final_class.isChecked();
    result.generate_constructor = constructor.isChecked();
    result.generate_destructor = destructor.isChecked();
    result.virtual_destructor = virtual_destructor.isChecked() && result.generate_destructor;
    result.generate_copy_operations = copy.isChecked();
    result.generate_move_operations = move.isChecked();
    return result;
  }

  ClassOptionsDialog* owner;
  finalcut::FLabel class_label; finalcut::FLineEdit class_name;
  finalcut::FLabel header_file_label; finalcut::FLineEdit header_file;
  finalcut::FLabel source_file_label; finalcut::FLineEdit source_file;
  finalcut::FLabel namespace_label; finalcut::FLineEdit namespace_name;
  finalcut::FLabel base_label; finalcut::FLineEdit base_class;
  finalcut::FLabel base_header_label; finalcut::FLineEdit base_header;
  finalcut::FLabel access_label; finalcut::FComboBox access;
  finalcut::FCheckBox constructor; finalcut::FCheckBox destructor;
  finalcut::FCheckBox virtual_destructor; finalcut::FCheckBox final_class;
  finalcut::FCheckBox copy; finalcut::FCheckBox move;
  finalcut::FButton next; finalcut::FButton cancel;
  std::string header_extension_;
  std::string header_suggestion_;
  std::string source_suggestion_;
};

ClassOptionsDialog::ClassOptionsDialog(std::string header_extension,
    finalcut::FWidget* parent, std::string language)
    : CenteredDialog(finalcut::FString(localizedUiText(language, "C++ class options")), parent) {
  setDialogSize({78, 24});
  setModal();
  impl_ = std::make_unique<Impl>(this, std::move(header_extension), language);
}

ClassOptionsDialog::~ClassOptionsDialog() = default;

auto ClassOptionsDialog::options() const -> CppClassOptions {
  return impl_->options();
}

NewProjectDialog::NewProjectDialog(finalcut::FWidget* parent, std::string language)
    : CenteredDialog(finalcut::FString(localizedUiText(language, "New project settings")), parent),
      name_label_(finalcut::FString(localizedUiText(language, "Project name:")), this), name_(this),
      language_label_(finalcut::FString(localizedUiText(language, "Language:")), this), language_(this),
      target_label_(finalcut::FString(localizedUiText(language, "Target type:")), this), target_(this),
      standard_label_(finalcut::FString(localizedUiText(language, "Language standard:")), this), standard_(this),
      header_label_(finalcut::FString(localizedUiText(language, "C++ headers:")), this), header_(this),
      generator_label_(finalcut::FString(localizedUiText(language, "Generator:")), this), generator_(this),
      build_type_label_(finalcut::FString(localizedUiText(language, "Build type:")), this), build_type_(this),
      install_label_(finalcut::FString(localizedUiText(language, "Install layout:")), this), install_(this),
      warnings_(finalcut::FString(localizedUiText(language, "Enable compiler warnings")), this),
      readme_(finalcut::FString(localizedUiText(language, "Create README.md")), this),
      gitignore_(finalcut::FString(localizedUiText(language, "Create .gitignore")), this),
      testing_(finalcut::FString(localizedUiText(language, "Enable CTest")), this),
      next_(finalcut::FString(localizedUiText(language, "&Next")), this),
      cancel_(finalcut::FString(localizedUiText(language, "&Cancel")), this) {
  setDialogSize({74, 23});
  setModal();
  name_label_.setGeometry({2, 1}, {14, 1}); name_.setGeometry({17, 1}, {38, 1});
  language_label_.setGeometry({2, 3}, {10, 1}); language_.setGeometry({12, 3}, {13, 1});
  language_.insert("C++"); language_.insert("C"); language_.setCurrentItem(1); language_.unsetEditable();
  target_label_.setGeometry({27, 3}, {12, 1}); target_.setGeometry({39, 3}, {16, 1});
  target_.insert("Executable"); target_.insert("Static library"); target_.insert("Shared library");
  target_.setCurrentItem(1); target_.unsetEditable();
  standard_label_.setGeometry({2, 5}, {14, 1}); standard_.setGeometry({17, 5}, {8, 1});
  header_label_.setGeometry({27, 5}, {13, 1}); header_.setGeometry({41, 5}, {14, 1});
  header_.insert("hpp"); header_.insert("h"); header_.setCurrentItem(1); header_.unsetEditable();
  generator_label_.setGeometry({2, 7}, {12, 1}); generator_.setGeometry({14, 7}, {18, 1});
  generator_.insert("Default"); generator_.insert("Ninja"); generator_.insert("Unix Makefiles");
  generator_.setCurrentItem(1); generator_.unsetEditable();
  build_type_label_.setGeometry({33, 7}, {11, 1}); build_type_.setGeometry({44, 7}, {11, 1});
  build_type_.insert("Debug"); build_type_.insert("Release"); build_type_.insert("RelWithDebInfo");
  build_type_.insert("MinSizeRel"); build_type_.setCurrentItem(1); build_type_.unsetEditable();
  install_label_.setGeometry({2, 9}, {14, 1}); install_.setGeometry({17, 9}, {18, 1});
  install_.insert("None"); install_.insert("GNU standard"); install_.setCurrentItem(1); install_.unsetEditable();
  warnings_.setGeometry({2, 10}, {27, 1}); readme_.setGeometry({30, 10}, {24, 1});
  gitignore_.setGeometry({2, 11}, {27, 1}); testing_.setGeometry({30, 11}, {24, 1});
  warnings_.setChecked(); readme_.setChecked(); gitignore_.setChecked();
  next_.setGeometry({32, 13}, {10, 1}); cancel_.setGeometry({44, 13}, {11, 1});
  language_.addCallback("row-changed", [this] { updateLanguageFields(); });
  next_.addCallback("clicked", [this] { done(ResultCode::Accept); });
  cancel_.addCallback("clicked", [this] { done(ResultCode::Reject); });
  updateLanguageFields();
  name_.setFocus();
}

auto NewProjectDialog::options() const -> NewProjectOptions {
  NewProjectOptions result;
  result.name = name_.getText().trim().toString();
  result.language = language_.getText() == "C" ? ProjectLanguage::C : ProjectLanguage::Cpp;
  const auto target = target_.getText().toString();
  result.target_type = target == "Static library" ? ProjectTargetType::StaticLibrary
    : (target == "Shared library" ? ProjectTargetType::SharedLibrary : ProjectTargetType::Executable);
  result.language_standard = standard_.getText().toString();
  result.cpp_header_extension = header_.getText().toString();
  const auto generator = generator_.getText().toString();
  result.generator = generator == "Default" ? std::string{} : generator;
  result.build_type = build_type_.getText().toString();
  result.install_layout = install_.getText() == "GNU standard"
    ? ProjectInstallLayout::Gnu : ProjectInstallLayout::None;
  result.warnings = warnings_.isChecked();
  result.create_readme = readme_.isChecked();
  result.create_gitignore = gitignore_.isChecked();
  result.enable_testing = testing_.isChecked();
  return result;
}

void NewProjectDialog::updateLanguageFields() {
  const auto cpp = language_.getText() != "C";
  standard_.clear();
  for (const auto* value : cpp
      ? std::vector<const char*>{"98", "11", "14", "17", "20", "23", "26"}
      : std::vector<const char*>{"90", "99", "11", "17", "23"})
    standard_.insert(value);
  standard_.setCurrentItem(cpp ? 5 : 4);
  standard_.unsetEditable();
  header_.setEnable(cpp);
  header_label_.setEnable(cpp);
  standard_.redraw();
  header_.redraw();
  header_label_.redraw();
}

ImportProjectDialog::ImportProjectDialog(std::string suggested_name,
    finalcut::FWidget* parent, std::string language)
    : CenteredDialog(finalcut::FString(localizedUiText(language, "Import source directory")), parent),
      name_label_(finalcut::FString(localizedUiText(language, "Target name:")), this), name_(this),
      language_label_(finalcut::FString(localizedUiText(language, "Language:")), this), language_(this),
      target_label_(finalcut::FString(localizedUiText(language, "Target type:")), this), target_(this),
      standard_label_(finalcut::FString(localizedUiText(language, "Language standard:")), this), standard_(this),
      warnings_(finalcut::FString(localizedUiText(language, "Enable compiler warnings")), this),
      next_(finalcut::FString(localizedUiText(language, "&Preview")), this),
      cancel_(finalcut::FString(localizedUiText(language, "&Cancel")), this) {
  setDialogSize({70, 16});
  setModal();
  name_label_.setGeometry({2, 1}, {14, 1}); name_.setGeometry({17, 1}, {38, 1});
  name_.setText(finalcut::FString(std::move(suggested_name)));
  language_label_.setGeometry({2, 3}, {10, 1}); language_.setGeometry({12, 3}, {13, 1});
  language_.insert("C++"); language_.insert("C"); language_.setCurrentItem(1); language_.unsetEditable();
  target_label_.setGeometry({27, 3}, {12, 1}); target_.setGeometry({39, 3}, {16, 1});
  target_.insert("Executable"); target_.insert("Static library"); target_.insert("Shared library");
  target_.setCurrentItem(1); target_.unsetEditable();
  standard_label_.setGeometry({2, 5}, {18, 1}); standard_.setGeometry({21, 5}, {10, 1}); standard_.setText("20");
  warnings_.setGeometry({2, 7}, {28, 1}); warnings_.setChecked();
  next_.setGeometry({31, 11}, {11, 1}); cancel_.setGeometry({44, 11}, {11, 1});
  next_.addCallback("clicked", [this] { done(ResultCode::Accept); });
  cancel_.addCallback("clicked", [this] { done(ResultCode::Reject); });
  name_.setFocus();
}

auto ImportProjectDialog::options() const -> ProjectImportOptions {
  ProjectImportOptions result;
  result.target_name = name_.getText().trim().toString();
  result.language = language_.getText() == "C" ? ProjectLanguage::C : ProjectLanguage::Cpp;
  const auto target = target_.getText().toString();
  result.target_type = target == "Static library" ? ProjectTargetType::StaticLibrary
    : (target == "Shared library" ? ProjectTargetType::SharedLibrary : ProjectTargetType::Executable);
  result.language_standard = standard_.getText().trim().toString();
  if (result.language == ProjectLanguage::C && result.language_standard == "20")
    result.language_standard = "17";
  result.warnings = warnings_.isChecked();
  return result;
}

}  // namespace tuiide
