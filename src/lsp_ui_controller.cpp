#include "tuiide/lsp_ui_controller.hpp"

#include <algorithm>

namespace tuiide {

void LspUiController::reset() noexcept {
  ready_ = false;
  diagnostics_revision_ = 0;
  semantic_tokens_revision_ = 0;
  observed_outline_.reset();
  requested_outline_.reset();
  rendered_outline_.reset();
  outline_stable_tick_ = 0;
  outline_reset_pending_ = true;
}

auto LspUiController::observe(bool ready, std::uint64_t diagnostics_revision,
    std::uint64_t semantic_tokens_revision) noexcept -> LspUiChanges {
  const LspUiChanges result{
    .became_ready = ready && !ready_,
    .diagnostics_changed = diagnostics_revision != diagnostics_revision_,
    .semantic_tokens_changed = semantic_tokens_revision != semantic_tokens_revision_
  };
  ready_ = ready;
  diagnostics_revision_ = diagnostics_revision;
  semantic_tokens_revision_ = semantic_tokens_revision;
  return result;
}

auto LspUiController::collect(LspClient& client) const -> LspEventBatch {
  return {
    .completions = client.takeCompletions(),
    .signatures = client.takeSignatures(),
    .hover = client.takeHover(),
    .definitions = client.takeDefinitions(),
    .references = client.takeReferences(),
    .rename_edit = client.takeRenameEdit(),
    .workspace_apply_requests = client.takeWorkspaceApplyRequests(),
    .document_symbols = client.takeDocumentSymbols(),
    .code_actions = client.takeCodeActions(),
    .switched_source_header = client.takeSwitchedSourceHeader(),
    .workspace_symbols = client.takeWorkspaceSymbols(),
    .call_hierarchy = client.takeCallHierarchy(),
    .type_hierarchy = client.takeTypeHierarchy(),
    .feedback = client.takeFeedback()
  };
}

auto LspUiController::route(LspEventBatch events,
    const std::optional<LspDocumentIdentity>& active_document) -> LspEventBatch {
  // Completion и code action меняют текст, поэтому ответ другой версии опаснее
  // обычного navigation-ответа: удаляем его ещё до показа какого-либо диалога.
  const auto completion_size = events.completions.size();
  std::erase_if(events.completions, [&active_document](const auto& item) {
    return !responseMatches({item.source_path, item.source_version}, active_document);
  });
  events.discarded_completions = completion_size - events.completions.size();
  const auto action_size = events.code_actions.size();
  std::erase_if(events.code_actions, [&active_document](const auto& item) {
    return !responseMatches({item.source_path, item.source_version}, active_document);
  });
  events.discarded_code_actions = action_size - events.code_actions.size();
  return events;
}

auto LspUiController::updateOutline(std::optional<LspDocumentIdentity> document,
    bool ready, unsigned tick, unsigned debounce_ticks) -> OutlineDecision {
  if (outline_reset_pending_) {
    outline_reset_pending_ = false;
    observed_outline_ = document;
    outline_stable_tick_ = tick;
    return OutlineDecision::Clear;
  }
  if (!document) {
    const bool had_state = observed_outline_.has_value() || requested_outline_.has_value()
      || rendered_outline_.has_value();
    observed_outline_.reset();
    requested_outline_.reset();
    rendered_outline_.reset();
    return had_state ? OutlineDecision::Clear : OutlineDecision::None;
  }
  if (observed_outline_ != document) {
    observed_outline_ = document;
    outline_stable_tick_ = tick;
    const bool stale_render = rendered_outline_.has_value() && rendered_outline_ != document;
    if (stale_render) rendered_outline_.reset();
    return stale_render ? OutlineDecision::Clear : OutlineDecision::None;
  }
  if (!ready || tick - outline_stable_tick_ < debounce_ticks
      || requested_outline_ == document) return OutlineDecision::None;
  requested_outline_ = std::move(document);
  return OutlineDecision::Request;
}

auto LspUiController::acceptOutline(const LspDocumentIdentity& response,
    const std::optional<LspDocumentIdentity>& active_document) -> bool {
  if (responseMatches(response, active_document)) {
    rendered_outline_ = response;
    return true;
  }
  if (requested_outline_ == response) requested_outline_.reset();
  return false;
}

auto LspUiController::renderedOutlineMatches(const LspDocumentIdentity& document) const -> bool {
  return rendered_outline_ == document;
}

auto LspUiController::responseMatches(const LspDocumentIdentity& response,
    const std::optional<LspDocumentIdentity>& active_document) -> bool {
  return active_document && response == *active_document;
}

auto lspOperationLabel(LspOperation operation) noexcept -> std::string_view {
  switch (operation) {
    case LspOperation::Completion: return "Completion";
    case LspOperation::SignatureHelp: return "Signature help";
    case LspOperation::Hover: return "Symbol information";
    case LspOperation::Definition: return "Go to definition";
    case LspOperation::References: return "Find references";
    case LspOperation::Rename: return "Rename";
    case LspOperation::DocumentSymbols: return "Outline";
    case LspOperation::CodeActions: return "Code Actions";
    case LspOperation::OrganizeIncludes: return "Organize Includes";
    case LspOperation::SwitchSourceHeader: return "Switch Header/Source";
    case LspOperation::WorkspaceSymbols: return "Workspace Symbols";
    case LspOperation::CallHierarchy: return "Call Hierarchy";
    case LspOperation::TypeHierarchy: return "Type Hierarchy";
    case LspOperation::Server: return "clangd";
  }
  return "clangd";
}

}  // namespace tuiide
