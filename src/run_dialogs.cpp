#include "tuiide/run_dialogs.hpp"

#include "tuiide/document.hpp"
#include "tuiide/project_dialogs.hpp"
#include "tuiide/project_settings.hpp"
#include "tuiide/ui_localization.hpp"

#include <algorithm>
#include <sstream>
#include <utility>

namespace tuiide {

struct LaunchSettingsDialog::Impl {
  Impl(LaunchSettingsDialog* dialog, std::filesystem::path project_root,
      const LaunchConfiguration& configuration, const std::vector<CMakeTarget>& targets,
      std::string language_value)
      : owner(dialog), root(std::move(project_root)), language(std::move(language_value)),
        target_label(finalcut::FString(localizedUiText(language, "CMake target:")), owner),
        target(owner),
        executable_label(finalcut::FString(localizedUiText(language, "Executable override:")), owner),
        executable(owner),
        executable_browse(finalcut::FString(localizedUiText(language, "&Browse...")), owner),
        working_label(finalcut::FString(localizedUiText(language, "Working directory:")), owner),
        working(owner),
        working_browse(finalcut::FString(localizedUiText(language, "B&rowse...")), owner),
        arguments_label(finalcut::FString(localizedUiText(language, "Arguments:")), owner),
        arguments(owner),
        environment_label(finalcut::FString(localizedUiText(language, "Environment:")), owner),
        environment(owner),
        stdin_label(finalcut::FString(localizedUiText(language, "Stdin file:")), owner),
        stdin(owner),
        stdin_browse(finalcut::FString(localizedUiText(language, "Bro&wse...")), owner),
        pre_build(finalcut::FString(localizedUiText(language, "Build before launch")), owner),
        external_terminal(finalcut::FString(localizedUiText(language,
          "External terminal (Run only)")), owner),
        terminal_label(finalcut::FString(localizedUiText(language, "Terminal executable:")), owner),
        terminal(owner), terminal_help(owner),
        save(finalcut::FString(localizedUiText(language, "&Save")), owner),
        cancel(finalcut::FString(localizedUiText(language, "&Cancel")), owner) {
    target_label.setGeometry({2, 1}, {18, 1}); target.setGeometry({20, 1}, {35, 1});
    target.insert(finalcut::FString(localizedUiText(language, "Use currently selected target")));
    for (const auto& item : targets) target.insert(finalcut::FString(item.name));
    target.setText(finalcut::FString(configuration.target.empty()
      ? localizedUiText(language, "Use currently selected target") : configuration.target));
    target.unsetEditable();
    executable_label.setGeometry({2, 2}, {18, 1}); executable.setGeometry({20, 2}, {24, 1});
    executable.setText(finalcut::FString(configuration.executable.string()));
    executable_browse.setGeometry({46, 2}, {9, 1});
    working_label.setGeometry({2, 3}, {18, 1}); working.setGeometry({20, 3}, {24, 1});
    working.setText(finalcut::FString(configuration.working_directory.string()));
    working_browse.setGeometry({46, 3}, {9, 1});
    arguments_label.setGeometry({2, 5}, {18, 1}); arguments.setGeometry({20, 5}, {35, 1});
    arguments.setText(finalcut::FString(formatArgumentList(configuration.arguments)));
    environment_label.setGeometry({2, 6}, {18, 1}); environment.setGeometry({20, 6}, {35, 1});
    environment.setText(finalcut::FString(formatEnvironmentSettings(configuration.environment)));
    stdin_label.setGeometry({2, 7}, {18, 1}); stdin.setGeometry({20, 7}, {24, 1});
    stdin.setText(finalcut::FString(configuration.stdin_file.string()));
    stdin_browse.setGeometry({46, 7}, {9, 1});
    pre_build.setGeometry({2, 9}, {24, 1});
    external_terminal.setGeometry({27, 9}, {28, 1});
    if (configuration.pre_launch_build) pre_build.setChecked();
    if (configuration.external_terminal) external_terminal.setChecked();
    terminal_label.setGeometry({2, 10}, {18, 1}); terminal.setGeometry({20, 10}, {35, 1});
    terminal.setText(finalcut::FString(configuration.terminal));
    terminal_help.setText(finalcut::FString(localizedUiText(language,
      "Example: x-terminal-emulator")));
    terminal_help.setGeometry({20, 11}, {35, 1});
    save.setGeometry({32, 13}, {10, 1}); cancel.setGeometry({44, 13}, {11, 1});
    executable_browse.addCallback("clicked", [this] {
      const auto selected = finalcut::FFileDialog::fileOpenChooser(
        owner, finalcut::FString(root.string()), "*");
      if (!selected.isEmpty()) executable.setText(selected);
    });
    working_browse.addCallback("clicked", [this] {
      const auto current = pathValue(working);
      ProjectDirectoryDialog dialog(localizedUiText(language, "Select launch working directory"),
        current.empty() ? root : current, owner, language);
      if (dialog.exec() == finalcut::FDialog::ResultCode::Accept)
        working.setText(finalcut::FString(dialog.selectedPath().string()));
    });
    stdin_browse.addCallback("clicked", [this] {
      const auto selected = finalcut::FFileDialog::fileOpenChooser(
        owner, finalcut::FString(root.string()), "*");
      if (!selected.isEmpty()) stdin.setText(selected);
    });
    save.addCallback("clicked", [this] { owner->done(finalcut::FDialog::ResultCode::Accept); });
    cancel.addCallback("clicked", [this] { owner->done(finalcut::FDialog::ResultCode::Reject); });
    arguments.setFocus();
  }

  auto readConfiguration(LaunchConfiguration& result, std::string& error) const -> bool {
    result.executable = pathValue(executable);
    result.working_directory = pathValue(working);
    result.stdin_file = pathValue(stdin);
    const auto selected_target = target.getText().toString();
    result.target = selected_target == localizedUiText(language, "Use currently selected target")
      ? std::string{} : selected_target;
    result.pre_launch_build = pre_build.isChecked();
    result.external_terminal = external_terminal.isChecked();
    result.terminal = terminal.getText().trim().toString();
    if (!parseArgumentList(arguments.getText().trim().toString(), result.arguments, error))
      return false;
    if (!parseEnvironmentSettings(environment.getText().trim().toString(), result.environment, error))
      return false;
    if (result.external_terminal && result.terminal.empty()) {
      error = localizedUiText(language, "External terminal command is required.");
      return false;
    }
    return true;
  }

  auto pathValue(const finalcut::FLineEdit& field) const -> std::filesystem::path {
    const std::filesystem::path path(field.getText().trim().toString());
    return path.empty() ? path : normalizePath(path.is_absolute() ? path : root / path);
  }

  LaunchSettingsDialog* owner;
  std::filesystem::path root;
  std::string language;
  finalcut::FLabel target_label; finalcut::FComboBox target;
  finalcut::FLabel executable_label; finalcut::FLineEdit executable;
  finalcut::FButton executable_browse;
  finalcut::FLabel working_label; finalcut::FLineEdit working;
  finalcut::FButton working_browse;
  finalcut::FLabel arguments_label; finalcut::FLineEdit arguments;
  finalcut::FLabel environment_label; finalcut::FLineEdit environment;
  finalcut::FLabel stdin_label; finalcut::FLineEdit stdin; finalcut::FButton stdin_browse;
  finalcut::FCheckBox pre_build; finalcut::FCheckBox external_terminal;
  finalcut::FLabel terminal_label; finalcut::FLineEdit terminal;
  finalcut::FLabel terminal_help; finalcut::FButton save; finalcut::FButton cancel;
};

LaunchSettingsDialog::LaunchSettingsDialog(std::filesystem::path root,
    const LaunchConfiguration& configuration, const std::vector<CMakeTarget>& targets,
    finalcut::FWidget* parent, std::string language)
    : CenteredDialog(finalcut::FString(localizedUiText(language, "Launch configuration")), parent) {
  setDialogSize({78, 24});
  setModal();
  impl_ = std::make_unique<Impl>(this, std::move(root), configuration, targets,
    std::move(language));
}

LaunchSettingsDialog::~LaunchSettingsDialog() = default;

auto LaunchSettingsDialog::configuration(LaunchConfiguration& result,
    std::string& error) const -> bool {
  return impl_->readConfiguration(result, error);
}

LaunchConfigurationManagerDialog::LaunchConfigurationManagerDialog(
    std::filesystem::path root, std::vector<NamedLaunchConfiguration> configurations,
    std::string selected, const std::vector<CMakeTarget>& targets, finalcut::FWidget* parent,
    std::string language)
    : CenteredDialog(finalcut::FString(localizedUiText(language, "Run/Debug configurations")), parent),
      root_(std::move(root)), language_(std::move(language)),
      configurations_(std::move(configurations)), selected_(std::move(selected)), targets_(targets),
      list_(this),
      add_(finalcut::FString(localizedUiText(language_, "&Add...")), this),
      clone_(finalcut::FString(localizedUiText(language_, "&Clone...")), this),
      edit_(finalcut::FString(localizedUiText(language_, "&Edit...")), this),
      remove_(finalcut::FString(localizedUiText(language_, "&Delete")), this),
      select_(finalcut::FString(localizedUiText(language_, "&Select")), this),
      cancel_(finalcut::FString(localizedUiText(language_, "C&ancel")), this) {
  setDialogSize({82, 25});
  setModal();
  list_.setGeometry({2, 2}, {77, 15});
  add_.setGeometry({2, 19}, {11, 1});
  clone_.setGeometry({15, 19}, {11, 1});
  edit_.setGeometry({28, 19}, {11, 1});
  remove_.setGeometry({41, 19}, {11, 1});
  select_.setGeometry({56, 19}, {10, 1});
  cancel_.setGeometry({68, 19}, {11, 1});
  add_.addCallback("clicked", [this] { addConfiguration(); });
  clone_.addCallback("clicked", [this] { cloneConfiguration(); });
  edit_.addCallback("clicked", [this] { editConfiguration(); });
  remove_.addCallback("clicked", [this] { deleteConfiguration(); });
  select_.addCallback("clicked", [this] { selectConfiguration(); });
  cancel_.addCallback("clicked", [this] { done(ResultCode::Reject); });
  list_.addCallback("clicked", [this] { editConfiguration(); });
  refresh(selected_);
}

auto LaunchConfigurationManagerDialog::currentIndex() const -> std::optional<std::size_t> {
  const auto row = list_.currentItem();
  if (row == 0 || row > configurations_.size()) return std::nullopt;
  return row - 1;
}

auto LaunchConfigurationManagerDialog::requestUniqueName(std::string title)
    -> std::optional<std::string> {
  PromptDialog dialog(std::move(title), "Configuration name:", this, language_);
  if (dialog.exec() != ResultCode::Accept) return std::nullopt;
  auto name = dialog.value();
  const auto begin = name.find_first_not_of(" \t");
  const auto end = name.find_last_not_of(" \t");
  name = begin == std::string::npos ? std::string{} : name.substr(begin, end - begin + 1);
  if (name.empty()) {
    finalcut::FMessageBox::error(this, finalcut::FString(localizedUiText(language_,
      "Configuration name is required.")));
    return std::nullopt;
  }
  if (std::any_of(configurations_.begin(), configurations_.end(), [&name](const auto& item) {
      return item.name == name;
    })) {
    finalcut::FMessageBox::error(this, finalcut::FString(localizedUiText(language_,
      "A configuration with this name already exists.")));
    return std::nullopt;
  }
  return name;
}

void LaunchConfigurationManagerDialog::refresh(const std::string& preferred) {
  list_.clear();
  std::size_t selected_row{1};
  for (std::size_t index{}; index < configurations_.size(); ++index) {
    const auto& item = configurations_[index];
    auto detail = !item.configuration.executable.empty()
      ? item.configuration.executable.string()
      : !item.configuration.target.empty() ? item.configuration.target
        : localizedUiText(language_, "current CMake target");
    list_.insert(finalcut::FString{(item.name == selected_ ? "* " : "  ")
      + item.name + " — " + detail});
    if (item.name == (preferred.empty() ? selected_ : preferred)) selected_row = index + 1;
  }
  list_.setCurrentItem(selected_row);
  remove_.setEnable(configurations_.size() > 1);
  list_.redraw();
  activateWindow();
  raiseWindow();
  setWindowFocusWidget(&list_);
  list_.setFocus();
}

void LaunchConfigurationManagerDialog::addConfiguration() {
  const auto name = requestUniqueName("Add Run/Debug configuration");
  if (!name) return;
  LaunchConfiguration configuration;
  LaunchSettingsDialog dialog(root_, configuration, targets_, this, language_);
  if (dialog.exec() != ResultCode::Accept) return;
  std::string error;
  if (!dialog.configuration(configuration, error)) {
    finalcut::FMessageBox::error(this, finalcut::FString{error}); return;
  }
  configurations_.push_back({*name, std::move(configuration)});
  refresh(*name);
}

void LaunchConfigurationManagerDialog::cloneConfiguration() {
  const auto index = currentIndex();
  if (!index) return;
  const auto name = requestUniqueName("Clone Run/Debug configuration");
  if (!name) return;
  configurations_.push_back({*name, configurations_[*index].configuration});
  refresh(*name);
}

void LaunchConfigurationManagerDialog::editConfiguration() {
  const auto index = currentIndex();
  if (!index) return;
  LaunchSettingsDialog dialog(root_, configurations_[*index].configuration, targets_, this, language_);
  if (dialog.exec() != ResultCode::Accept) return;
  LaunchConfiguration configuration;
  std::string error;
  if (!dialog.configuration(configuration, error)) {
    finalcut::FMessageBox::error(this, finalcut::FString{error}); return;
  }
  configurations_[*index].configuration = std::move(configuration);
  refresh(configurations_[*index].name);
}

void LaunchConfigurationManagerDialog::deleteConfiguration() {
  const auto index = currentIndex();
  if (!index) return;
  if (configurations_.size() == 1) {
    finalcut::FMessageBox::info(this,
      finalcut::FString(localizedUiText(language_, "Run/Debug configurations")),
      finalcut::FString(localizedUiText(language_, "At least one configuration must remain.")));
    return;
  }
  const auto answer = finalcut::FMessageBox::info(this,
    finalcut::FString(localizedUiText(language_, "Delete configuration")),
    finalcut::FString{localizedUiText(language_, "Delete ") + configurations_[*index].name + "?"},
    finalcut::FMessageBox::ButtonType::Yes, finalcut::FMessageBox::ButtonType::No,
    finalcut::FMessageBox::ButtonType::Reject);
  if (answer != finalcut::FMessageBox::ButtonType::Yes) return;
  const auto deleted_active = configurations_[*index].name == selected_;
  configurations_.erase(configurations_.begin() + static_cast<std::ptrdiff_t>(*index));
  if (deleted_active) selected_ = configurations_.front().name;
  refresh(selected_);
}

void LaunchConfigurationManagerDialog::selectConfiguration() {
  const auto index = currentIndex();
  if (!index) return;
  selected_ = configurations_[*index].name;
  done(ResultCode::Accept);
}

namespace {
auto joinPresetValues(const std::vector<std::string>& values) -> std::string {
  std::string result;
  for (const auto& value : values) {
    if (!result.empty()) result += "; ";
    result += value;
  }
  return result;
}

auto splitPresetValues(std::string value) -> std::vector<std::string> {
  std::vector<std::string> result;
  std::stringstream stream(std::move(value));
  std::string item;
  while (std::getline(stream, item, ';')) {
    const auto begin = item.find_first_not_of(" \t");
    if (begin == std::string::npos) continue;
    const auto end = item.find_last_not_of(" \t");
    result.push_back(item.substr(begin, end - begin + 1));
  }
  return result;
}
}

struct CMakePresetEditDialog::Impl {
  Impl(CMakePresetEditDialog* dialog, CMakePresetEdit value, const std::string& language)
      : owner(dialog), value_(std::move(value)),
        name_label(finalcut::FString(localizedUiText(language, "Name:")), owner), name(owner),
        display_label(finalcut::FString(localizedUiText(language, "Display name:")), owner),
        display(owner),
        inherits_label(finalcut::FString(localizedUiText(language, "Inherits (; separated):")), owner),
        inherits(owner),
        generator_label(finalcut::FString(localizedUiText(language, "Generator:")), owner),
        generator(owner),
        binary_label(finalcut::FString(localizedUiText(language, "Binary directory:")), owner),
        binary(owner),
        configure_label(finalcut::FString(localizedUiText(language, "Configure preset:")), owner),
        configure(owner),
        configuration_label(finalcut::FString(localizedUiText(language, "Configuration:")), owner),
        configuration(owner),
        targets_label(finalcut::FString(localizedUiText(language, "Targets (; separated):")), owner),
        targets(owner),
        clean_first(finalcut::FString(localizedUiText(language, "Clean first")), owner),
        verbose(finalcut::FString(localizedUiText(language, "Verbose build")), owner),
        save(finalcut::FString(localizedUiText(language, "&Save")), owner),
        cancel(finalcut::FString(localizedUiText(language, "&Cancel")), owner) {
    name_label.setGeometry({2, 1}, {20, 1}); name.setGeometry({23, 1}, {48, 1});
    display_label.setGeometry({2, 3}, {20, 1}); display.setGeometry({23, 3}, {48, 1});
    inherits_label.setGeometry({2, 5}, {20, 1}); inherits.setGeometry({23, 5}, {48, 1});
    name.setText(finalcut::FString(value_.name));
    display.setText(finalcut::FString(value_.display_name));
    inherits.setText(finalcut::FString(joinPresetValues(value_.inherits)));
    if (value_.kind == CMakePresetKind::Configure) {
      generator_label.setGeometry({2, 7}, {20, 1}); generator.setGeometry({23, 7}, {48, 1});
      binary_label.setGeometry({2, 9}, {20, 1}); binary.setGeometry({23, 9}, {48, 1});
      generator.setText(finalcut::FString(value_.generator));
      binary.setText(finalcut::FString(value_.binary_directory));
      configure_label.hide(); configure.hide(); configuration_label.hide(); configuration.hide();
      targets_label.hide(); targets.hide(); clean_first.hide(); verbose.hide();
    } else {
      configure_label.setGeometry({2, 7}, {20, 1}); configure.setGeometry({23, 7}, {48, 1});
      configuration_label.setGeometry({2, 9}, {20, 1}); configuration.setGeometry({23, 9}, {48, 1});
      targets_label.setGeometry({2, 11}, {20, 1}); targets.setGeometry({23, 11}, {48, 1});
      clean_first.setGeometry({23, 13}, {18, 1}); verbose.setGeometry({44, 13}, {20, 1});
      configure.setText(finalcut::FString(value_.configure_preset));
      configuration.setText(finalcut::FString(value_.configuration));
      targets.setText(finalcut::FString(joinPresetValues(value_.targets)));
      if (value_.clean_first) clean_first.setChecked();
      if (value_.verbose) verbose.setChecked();
      generator_label.hide(); generator.hide(); binary_label.hide(); binary.hide();
    }
    save.setGeometry({47, 16}, {10, 1}); cancel.setGeometry({59, 16}, {12, 1});
    save.addCallback("clicked", [this] { owner->done(finalcut::FDialog::ResultCode::Accept); });
    cancel.addCallback("clicked", [this] { owner->done(finalcut::FDialog::ResultCode::Reject); });
    name.setFocus();
  }

  auto value() const -> CMakePresetEdit {
    auto result = value_;
    result.name = name.getText().trim().toString();
    result.display_name = display.getText().trim().toString();
    result.inherits = splitPresetValues(inherits.getText().toString());
    result.generator = generator.getText().trim().toString();
    result.binary_directory = binary.getText().trim().toString();
    result.configure_preset = configure.getText().trim().toString();
    result.configuration = configuration.getText().trim().toString();
    result.targets = splitPresetValues(targets.getText().toString());
    result.clean_first = clean_first.isChecked();
    result.verbose = verbose.isChecked();
    return result;
  }

  CMakePresetEditDialog* owner;
  CMakePresetEdit value_;
  finalcut::FLabel name_label; finalcut::FLineEdit name;
  finalcut::FLabel display_label; finalcut::FLineEdit display;
  finalcut::FLabel inherits_label; finalcut::FLineEdit inherits;
  finalcut::FLabel generator_label; finalcut::FLineEdit generator;
  finalcut::FLabel binary_label; finalcut::FLineEdit binary;
  finalcut::FLabel configure_label; finalcut::FLineEdit configure;
  finalcut::FLabel configuration_label; finalcut::FLineEdit configuration;
  finalcut::FLabel targets_label; finalcut::FLineEdit targets;
  finalcut::FCheckBox clean_first; finalcut::FCheckBox verbose;
  finalcut::FButton save; finalcut::FButton cancel;
};

CMakePresetEditDialog::CMakePresetEditDialog(CMakePresetEdit preset,
    finalcut::FWidget* parent, std::string language)
    : CenteredDialog(finalcut::FString(localizedUiText(language,
        preset.kind == CMakePresetKind::Configure ? "Configure preset" : "Build preset")), parent) {
  setDialogSize({78, 22});
  setModal();
  impl_ = std::make_unique<Impl>(this, std::move(preset), language);
}

CMakePresetEditDialog::~CMakePresetEditDialog() = default;

auto CMakePresetEditDialog::preset() const -> CMakePresetEdit { return impl_->value(); }

CMakePresetManagerDialog::CMakePresetManagerDialog(std::filesystem::path root,
    CMakePresetKind kind, std::string selected, finalcut::FWidget* parent,
    std::string language)
    : CenteredDialog(finalcut::FString(localizedUiText(language,
        kind == CMakePresetKind::Configure ? "CMake configure presets" : "CMake build presets")),
        parent),
      root_(std::move(root)), kind_(kind), language_(std::move(language)),
      initial_selection_(std::move(selected)),
      list_(this),
      add_(finalcut::FString(localizedUiText(language_, "&Add")), this),
      clone_(finalcut::FString(localizedUiText(language_, "C&lone")), this),
      edit_(finalcut::FString(localizedUiText(language_, "&Edit")), this),
      remove_(finalcut::FString(localizedUiText(language_, "&Delete")), this),
      reload_(finalcut::FString(localizedUiText(language_, "&Reload")), this),
      select_(finalcut::FString(localizedUiText(language_, "&Select")), this),
      cancel_(finalcut::FString(localizedUiText(language_, "&Cancel")), this) {
  setDialogSize({92, 24});
  setModal();
  list_.setGeometry({2, 1}, {87, 16});
  add_.setGeometry({2, 19}, {10, 1}); clone_.setGeometry({13, 19}, {10, 1});
  edit_.setGeometry({24, 19}, {10, 1}); remove_.setGeometry({35, 19}, {10, 1});
  reload_.setGeometry({46, 19}, {11, 1}); select_.setGeometry({65, 19}, {10, 1});
  cancel_.setGeometry({77, 19}, {12, 1});
  add_.addCallback("clicked", [this] { addPreset(); });
  clone_.addCallback("clicked", [this] { clonePreset(); });
  edit_.addCallback("clicked", [this] { editPreset(); });
  remove_.addCallback("clicked", [this] { deletePreset(); });
  reload_.addCallback("clicked", [this] { reload(); });
  select_.addCallback("clicked", [this] { selectPreset(); });
  cancel_.addCallback("clicked", [this] { done(ResultCode::Reject); });
  list_.setEnterHandler([this] { selectPreset(); });
  reload(initial_selection_);
  list_.setFocus();
}

auto CMakePresetManagerDialog::selectedPreset() const -> std::string { return selected_; }

auto CMakePresetManagerDialog::currentPreset() -> CMakePresetEdit* {
  const auto row = list_.currentItem();
  if (row <= 1 || row - 2 >= presets_.size()) return nullptr;
  return &presets_[row - 2];
}

void CMakePresetManagerDialog::reload(const std::string& preferred) {
  std::string error;
  auto loaded = loadCMakePresetsForEdit(root_, kind_, error);
  if (!error.empty()) {
    finalcut::FMessageBox::error(this, finalcut::FString(error));
    return;
  }
  presets_ = std::move(loaded);
  list_.clear();
  list_.insert(finalcut::FString(localizedUiText(language_,
    kind_ == CMakePresetKind::Configure ? "No configure preset" : "No build preset")));
  std::size_t selected_row = 1;
  for (std::size_t index = 0; index < presets_.size(); ++index) {
    const auto& preset = presets_[index];
    std::error_code relative_error;
    const auto relative_source = std::filesystem::relative(preset.source_file, root_, relative_error);
    auto origin = preset.user_editable
      ? "CMakeUserPresets.json — " + localizedUiText(language_, "editable")
      : (relative_error ? preset.source_file.string() : relative_source.generic_string());
    auto label = preset.name;
    if (!preset.display_name.empty()) label += " — " + preset.display_name;
    label += "  [" + origin + (preset.hidden ? localizedUiText(language_, ", hidden") : "") + "]";
    list_.insert(finalcut::FString(label));
    if (preset.name == (preferred.empty() ? initial_selection_ : preferred)) selected_row = index + 2;
  }
  list_.setCurrentItem(selected_row);
  list_.redraw();
  activateWindow();
  raiseWindow();
  setFocus();
  setWindowFocusWidget(&list_);
  list_.setFocus();
}

void CMakePresetManagerDialog::addPreset() {
  CMakePresetEdit value;
  value.kind = kind_;
  if (kind_ == CMakePresetKind::Configure) {
    value.generator = "Ninja";
    value.binary_directory = "${sourceDir}/build/${presetName}";
  }
  CMakePresetEditDialog dialog(std::move(value), this, language_);
  if (dialog.exec() != ResultCode::Accept) return;
  const auto preset = dialog.preset();
  std::string error;
  if (!saveCMakeUserPreset(root_, {}, preset, error)) {
    finalcut::FMessageBox::error(this, finalcut::FString(error)); return;
  }
  changed_ = true;
  reload(preset.name);
}

void CMakePresetManagerDialog::clonePreset() {
  auto* current = currentPreset();
  if (!current) {
    finalcut::FMessageBox::info(this,
      finalcut::FString(localizedUiText(language_, "CMake presets")),
      finalcut::FString(localizedUiText(language_, "Select a preset to clone.")));
    return;
  }
  PromptDialog prompt("Clone CMake preset", "New name:", this, language_);
  if (prompt.exec() != ResultCode::Accept || prompt.value().empty()) return;
  std::string error;
  if (!cloneCMakePresetToUser(root_, kind_, current->name, prompt.value(), error)) {
    finalcut::FMessageBox::error(this, finalcut::FString(error)); return;
  }
  changed_ = true;
  reload(prompt.value());
}

void CMakePresetManagerDialog::editPreset() {
  auto* current = currentPreset();
  if (!current) {
    finalcut::FMessageBox::info(this,
      finalcut::FString(localizedUiText(language_, "CMake presets")),
      finalcut::FString(localizedUiText(language_, "Select a preset to edit.")));
    return;
  }
  if (!current->user_editable) {
    finalcut::FMessageBox::info(this,
      finalcut::FString(localizedUiText(language_, "Read-only preset")),
      finalcut::FString(localizedUiText(language_,
        "Project and included presets are read-only. Clone this preset to edit it.")));
    return;
  }
  const auto original = current->name;
  CMakePresetEditDialog dialog(*current, this, language_);
  if (dialog.exec() != ResultCode::Accept) return;
  const auto preset = dialog.preset();
  std::string error;
  if (!saveCMakeUserPreset(root_, original, preset, error)) {
    finalcut::FMessageBox::error(this, finalcut::FString(error)); return;
  }
  changed_ = true;
  reload(preset.name);
}

void CMakePresetManagerDialog::deletePreset() {
  auto* current = currentPreset();
  if (!current) {
    finalcut::FMessageBox::info(this,
      finalcut::FString(localizedUiText(language_, "CMake presets")),
      finalcut::FString(localizedUiText(language_, "Select a preset to delete.")));
    return;
  }
  if (!current->user_editable) {
    finalcut::FMessageBox::info(this,
      finalcut::FString(localizedUiText(language_, "Read-only preset")),
      finalcut::FString(localizedUiText(language_,
        "Only presets from CMakeUserPresets.json can be deleted.")));
    return;
  }
  const auto name = current->name;
  const auto answer = finalcut::FMessageBox::info(this,
    finalcut::FString(localizedUiText(language_, "Delete CMake preset")),
    finalcut::FString(localizedUiText(language_, "Delete user preset '") + name + "'?"),
    finalcut::FMessageBox::ButtonType::Yes, finalcut::FMessageBox::ButtonType::No,
    finalcut::FMessageBox::ButtonType::Reject);
  if (answer != finalcut::FMessageBox::ButtonType::Yes) return;
  std::string error;
  if (!deleteCMakeUserPreset(root_, kind_, name, error)) {
    finalcut::FMessageBox::error(this, finalcut::FString(error)); return;
  }
  changed_ = true;
  reload();
}

void CMakePresetManagerDialog::selectPreset() {
  auto* current = currentPreset();
  if (current && current->hidden) {
    finalcut::FMessageBox::info(this,
      finalcut::FString(localizedUiText(language_, "Hidden preset")),
      finalcut::FString(localizedUiText(language_,
        "Hidden presets cannot be selected directly.")));
    return;
  }
  selected_ = current ? current->name : std::string{};
  done(ResultCode::Accept);
}

}  // namespace tuiide
