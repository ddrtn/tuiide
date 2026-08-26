#pragma once

#include <cstddef>

namespace tuiide {

struct CommandContext {
  bool has_project{};
  bool has_document{};
  bool has_saved_document{};
  bool document_modified{};
  bool source_document{};
  bool cmake_document{};
  bool has_selection{};
  bool has_modified_documents{};
  bool has_closed_document{};
  bool can_undo{};
  bool can_redo{};
  bool lsp_ready{};
  bool build_running{};
  bool run_running{};
  bool gdb_running{};
  bool gdb_active{};
  bool gdb_stopped{};
  std::size_t document_count{};
};

struct CommandAvailability {
  bool close_project{};
  bool project_file{};
  bool save{};
  bool save_all{};
  bool save_as{};
  bool close_document{};
  bool close_all{};
  bool close_others{};
  bool reopen_closed{};
  bool undo{};
  bool redo{};
  bool cut{};
  bool copy{};
  bool paste{};
  bool select_all{};
  bool find{};
  bool go_to_line{};
  bool definition{};
  bool references{};
  bool problems{};
  bool build{};
  bool cancel_build{};
  bool run{};
  bool stop_run{};
  bool cmake_configuration{};
  bool debug_start{};
  bool debug_pause{};
  bool debug_stop{};
  bool debug_restart{};
  bool breakpoint{};
  bool debug_step{};
  bool watch{};
  bool registers{};
  bool completion{};
  bool signature_help{};
  bool hover{};
  bool rename{};
  bool code_actions{};
  bool workspace_symbols{};
  bool hierarchy{};
  bool format_document{};
  bool format_selection{};
  bool switch_document{};
  bool project_panel{};
  bool debug_panel{};

  auto operator==(const CommandAvailability&) const -> bool = default;
};

[[nodiscard]] auto commandAvailability(const CommandContext& context)
  -> CommandAvailability;

}  // namespace tuiide
