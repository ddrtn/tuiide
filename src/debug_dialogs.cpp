#include "tuiide/debug_dialogs.hpp"

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

}  // namespace tuiide
