#include "tuiide/run_dialogs.hpp"

#include "tuiide/document.hpp"
#include "tuiide/project_dialogs.hpp"
#include "tuiide/project_settings.hpp"

#include <utility>

namespace tuiide {

struct LaunchSettingsDialog::Impl {
  Impl(LaunchSettingsDialog* dialog, std::filesystem::path project_root,
      const LaunchConfiguration& configuration, const std::vector<CMakeTarget>& targets)
      : owner(dialog), root(std::move(project_root)), target_label("CMake target:", owner),
        target(owner), executable_label("Executable override:", owner), executable(owner),
        executable_browse("&Browse...", owner), working_label("Working directory:", owner),
        working(owner), working_browse("B&rowse...", owner), arguments_label("Arguments:", owner),
        arguments(owner), environment_label("Environment:", owner), environment(owner),
        stdin_label("Stdin file:", owner), stdin(owner), stdin_browse("Bro&wse...", owner),
        pre_build("Build before launch", owner),
        external_terminal("External terminal (Run only)", owner),
        terminal_label("Terminal executable:", owner), terminal(owner), terminal_help(owner),
        save("&Save", owner), cancel("&Cancel", owner) {
    target_label.setGeometry({2, 1}, {18, 1}); target.setGeometry({20, 1}, {35, 1});
    target.insert("Use currently selected target");
    for (const auto& item : targets) target.insert(finalcut::FString(item.name));
    target.setText(finalcut::FString(configuration.target.empty()
      ? "Use currently selected target" : configuration.target));
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
    terminal_help.setText("Example: x-terminal-emulator");
    terminal_help.setGeometry({20, 11}, {35, 1});
    save.setGeometry({32, 13}, {10, 1}); cancel.setGeometry({44, 13}, {11, 1});
    executable_browse.addCallback("clicked", [this] {
      const auto selected = finalcut::FFileDialog::fileOpenChooser(
        owner, finalcut::FString(root.string()), "*");
      if (!selected.isEmpty()) executable.setText(selected);
    });
    working_browse.addCallback("clicked", [this] {
      const auto current = pathValue(working);
      ProjectDirectoryDialog dialog("Select launch working directory",
        current.empty() ? root : current, owner);
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
    executable.setFocus();
  }

  auto readConfiguration(LaunchConfiguration& result, std::string& error) const -> bool {
    result.executable = pathValue(executable);
    result.working_directory = pathValue(working);
    result.stdin_file = pathValue(stdin);
    const auto selected_target = target.getText().toString();
    result.target = selected_target == "Use currently selected target"
      ? std::string{} : selected_target;
    result.pre_launch_build = pre_build.isChecked();
    result.external_terminal = external_terminal.isChecked();
    result.terminal = terminal.getText().trim().toString();
    if (!parseArgumentList(arguments.getText().trim().toString(), result.arguments, error))
      return false;
    if (!parseEnvironmentSettings(environment.getText().trim().toString(), result.environment, error))
      return false;
    if (result.external_terminal && result.terminal.empty()) {
      error = "External terminal command is required.";
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
    finalcut::FWidget* parent)
    : CenteredDialog("Launch configuration", parent) {
  setDialogSize({78, 24});
  setModal();
  impl_ = std::make_unique<Impl>(this, std::move(root), configuration, targets);
}

LaunchSettingsDialog::~LaunchSettingsDialog() = default;

auto LaunchSettingsDialog::configuration(LaunchConfiguration& result,
    std::string& error) const -> bool {
  return impl_->readConfiguration(result, error);
}

}  // namespace tuiide
