#include "tuiide/debug_dialogs.hpp"

#include "tuiide/document.hpp"

#include <stdexcept>

namespace tuiide {

struct BreakpointSettingsDialog::Impl {
  Impl(BreakpointSettingsDialog* dialog, const DebugBreakpoint& breakpoint)
      : owner(dialog), location(finalcut::FString(breakpoint.file.string() + ":"
          + std::to_string(breakpoint.line)), owner), enabled("Enabled", owner),
        condition_label("Condition:", owner), condition(owner),
        hits_label("Ignore first hits:", owner), hits(owner),
        log_label("Log message:", owner), log(owner),
        help("A logpoint prints its message and continues automatically.", owner),
        save("&Save", owner), cancel("&Cancel", owner) {
    location.setGeometry({2, 1}, {53, 1});
    enabled.setGeometry({2, 3}, {15, 1});
    if (breakpoint.enabled) enabled.setChecked();
    condition_label.setGeometry({2, 5}, {18, 1});
    condition.setGeometry({21, 5}, {34, 1});
    condition.setText(finalcut::FString(breakpoint.condition));
    hits_label.setGeometry({2, 7}, {18, 1});
    hits.setGeometry({21, 7}, {10, 1});
    hits.setText(finalcut::FString(std::to_string(breakpoint.hit_count)));
    log_label.setGeometry({2, 9}, {18, 1});
    log.setGeometry({21, 9}, {34, 1});
    log.setText(finalcut::FString(breakpoint.log_message));
    help.setGeometry({2, 11}, {53, 1});
    save.setGeometry({32, 13}, {10, 1});
    cancel.setGeometry({44, 13}, {11, 1});
    save.addCallback("clicked", [this] {
      owner->done(finalcut::FDialog::ResultCode::Accept);
    });
    cancel.addCallback("clicked", [this] {
      owner->done(finalcut::FDialog::ResultCode::Reject);
    });
    condition.setFocus();
  }

  auto apply(DebugBreakpoint& breakpoint, std::string& error) const -> bool {
    breakpoint.enabled = enabled.isChecked();
    breakpoint.condition = condition.getText().trim().toString();
    breakpoint.log_message = log.getText().toString();
    const auto value = hits.getText().trim().toString();
    try {
      std::size_t parsed{};
      const auto count = std::stoul(value.empty() ? "0" : value, &parsed);
      if (parsed != (value.empty() ? 1U : value.size()) || count > 1000000000UL)
        throw std::out_of_range("hits");
      breakpoint.hit_count = static_cast<unsigned>(count);
    } catch (...) {
      error = "Ignore hit count must be an integer from 0 to 1000000000.";
      return false;
    }
    return true;
  }

  BreakpointSettingsDialog* owner;
  finalcut::FLabel location;
  finalcut::FCheckBox enabled;
  finalcut::FLabel condition_label;
  finalcut::FLineEdit condition;
  finalcut::FLabel hits_label;
  finalcut::FLineEdit hits;
  finalcut::FLabel log_label;
  finalcut::FLineEdit log;
  finalcut::FLabel help;
  finalcut::FButton save;
  finalcut::FButton cancel;
};

BreakpointSettingsDialog::BreakpointSettingsDialog(const DebugBreakpoint& breakpoint,
    finalcut::FWidget* parent)
    : CenteredDialog("Breakpoint properties", parent) {
  setDialogSize({70, 18});
  setModal();
  impl_ = std::make_unique<Impl>(this, breakpoint);
}

BreakpointSettingsDialog::~BreakpointSettingsDialog() = default;

auto BreakpointSettingsDialog::apply(DebugBreakpoint& breakpoint,
    std::string& error) const -> bool {
  return impl_->apply(breakpoint, error);
}

struct CoreDumpDialog::Impl {
  Impl(CoreDumpDialog* dialog, std::filesystem::path initial)
      : owner(dialog), initial_directory(std::move(initial)),
        executable_label("Executable:", owner), executable(owner),
        executable_browse("&Browse...", owner), core_label("Core dump:", owner),
        core(owner), core_browse("B&rowse...", owner),
        help("Core sessions are read-only: inspect stack, variables, memory and registers.", owner),
        open("&Open", owner), cancel("&Cancel", owner) {
    executable_label.setGeometry({2, 2}, {14, 1});
    executable.setGeometry({16, 2}, {39, 1});
    executable_browse.setGeometry({57, 2}, {10, 1});
    core_label.setGeometry({2, 4}, {14, 1});
    core.setGeometry({16, 4}, {39, 1});
    core_browse.setGeometry({57, 4}, {10, 1});
    help.setGeometry({2, 7}, {65, 1});
    open.setGeometry({43, 10}, {10, 1});
    cancel.setGeometry({55, 10}, {12, 1});
    executable_browse.addCallback("clicked", [this] { chooseFile(executable); });
    core_browse.addCallback("clicked", [this] { chooseFile(core); });
    open.addCallback("clicked", [this] { owner->done(finalcut::FDialog::ResultCode::Accept); });
    cancel.addCallback("clicked", [this] { owner->done(finalcut::FDialog::ResultCode::Reject); });
    executable.setFocus();
  }

  void chooseFile(finalcut::FLineEdit& field) {
    const auto current = pathValue(field);
    const auto directory = current.empty() ? initial_directory : current.parent_path();
    const auto selected = finalcut::FFileDialog::fileOpenChooser(
      owner, finalcut::FString(directory.string()), "*");
    if (!selected.isEmpty()) field.setText(selected);
  }

  auto paths(std::filesystem::path& executable_path,
      std::filesystem::path& core_path, std::string& error) const -> bool {
    executable_path = pathValue(executable);
    core_path = pathValue(core);
    if (executable_path.empty() || core_path.empty()) {
      error = "Select both an executable and a core dump.";
      return false;
    }
    std::error_code status_error;
    if (!std::filesystem::is_regular_file(executable_path, status_error)) {
      error = "Executable is not a readable regular file: " + executable_path.string();
      return false;
    }
    status_error.clear();
    if (!std::filesystem::is_regular_file(core_path, status_error)) {
      error = "Core dump is not a readable regular file: " + core_path.string();
      return false;
    }
    return true;
  }

  auto pathValue(const finalcut::FLineEdit& field) const -> std::filesystem::path {
    const std::filesystem::path value(field.getText().trim().toString());
    if (value.empty()) return {};
    return normalizePath(value.is_absolute() ? value : initial_directory / value);
  }

  CoreDumpDialog* owner;
  std::filesystem::path initial_directory;
  finalcut::FLabel executable_label; finalcut::FLineEdit executable;
  finalcut::FButton executable_browse;
  finalcut::FLabel core_label; finalcut::FLineEdit core; finalcut::FButton core_browse;
  finalcut::FLabel help; finalcut::FButton open; finalcut::FButton cancel;
};

CoreDumpDialog::CoreDumpDialog(std::filesystem::path initial_directory,
    finalcut::FWidget* parent)
    : CenteredDialog("Open core dump", parent) {
  setDialogSize({72, 15});
  setModal();
  impl_ = std::make_unique<Impl>(this, std::move(initial_directory));
}

CoreDumpDialog::~CoreDumpDialog() = default;

auto CoreDumpDialog::paths(std::filesystem::path& executable,
    std::filesystem::path& core_file, std::string& error) const -> bool {
  return impl_->paths(executable, core_file, error);
}

}  // namespace tuiide
