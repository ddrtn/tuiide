#pragma once

#include "tuiide/gdb_client.hpp"
#include "tuiide/ui_dialogs.hpp"

#include <memory>
#include <filesystem>
#include <string>

namespace tuiide {

/** Диалог condition, ignore count и log message одного breakpoint/logpoint. */
class BreakpointSettingsDialog final : public CenteredDialog {
 public:
  BreakpointSettingsDialog(const DebugBreakpoint& breakpoint,
    finalcut::FWidget* parent = nullptr, std::string language = "en");
  ~BreakpointSettingsDialog() override;

  [[nodiscard]] auto apply(DebugBreakpoint& breakpoint,
    std::string& error) const -> bool;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/** Выбирает executable и соответствующий core dump для read-only сессии. */
class CoreDumpDialog final : public CenteredDialog {
 public:
  explicit CoreDumpDialog(std::filesystem::path initial_directory,
    finalcut::FWidget* parent = nullptr, std::string language = "en");
  ~CoreDumpDialog() override;

  [[nodiscard]] auto paths(std::filesystem::path& executable,
    std::filesystem::path& core_file, std::string& error) const -> bool;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace tuiide
