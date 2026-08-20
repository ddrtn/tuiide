#pragma once

#include "tuiide/gdb_client.hpp"
#include "tuiide/ui_dialogs.hpp"

#include <memory>
#include <string>

namespace tuiide {

class BreakpointSettingsDialog final : public CenteredDialog {
 public:
  BreakpointSettingsDialog(const DebugBreakpoint& breakpoint,
    finalcut::FWidget* parent = nullptr);
  ~BreakpointSettingsDialog() override;

  [[nodiscard]] auto apply(DebugBreakpoint& breakpoint,
    std::string& error) const -> bool;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace tuiide
