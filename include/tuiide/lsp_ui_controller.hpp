#pragma once

#include "tuiide/lsp_client.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>

namespace tuiide {

struct LspDocumentIdentity {
  std::filesystem::path path;
  int version{-1};

  auto operator==(const LspDocumentIdentity&) const -> bool = default;
};

struct LspUiChanges {
  bool became_ready{};
  bool diagnostics_changed{};
  bool semantic_tokens_changed{};
};

struct LspEventBatch {
  std::vector<LspCompletionItem> completions;
  std::vector<LspSignature> signatures;
  std::string hover;
  std::vector<SourceLocation> definitions;
  std::vector<SourceLocation> references;
  std::optional<WorkspaceEdit> rename_edit;
  std::vector<WorkspaceApplyRequest> workspace_apply_requests;
  std::optional<LspDocumentSymbols> document_symbols;
  std::vector<LspCodeAction> code_actions;
  std::optional<std::filesystem::path> switched_source_header;
  std::vector<LspNavigationItem> workspace_symbols;
  std::optional<LspHierarchy> call_hierarchy;
  std::optional<LspHierarchy> type_hierarchy;
  std::vector<LspFeedback> feedback;
  std::size_t discarded_completions{};
  std::size_t discarded_code_actions{};
};

enum class OutlineDecision { None, Clear, Request };

class LspUiController {
 public:
  void reset() noexcept;
  [[nodiscard]] auto observe(bool ready, std::uint64_t diagnostics_revision,
    std::uint64_t semantic_tokens_revision) noexcept -> LspUiChanges;
  [[nodiscard]] auto collect(LspClient& client) const -> LspEventBatch;
  [[nodiscard]] static auto route(LspEventBatch events,
    const std::optional<LspDocumentIdentity>& active_document) -> LspEventBatch;
  [[nodiscard]] auto updateOutline(std::optional<LspDocumentIdentity> document,
    bool ready, unsigned tick, unsigned debounce_ticks = 5) -> OutlineDecision;
  [[nodiscard]] auto acceptOutline(const LspDocumentIdentity& response,
    const std::optional<LspDocumentIdentity>& active_document) -> bool;
  [[nodiscard]] auto renderedOutlineMatches(const LspDocumentIdentity& document) const -> bool;
  [[nodiscard]] static auto responseMatches(const LspDocumentIdentity& response,
    const std::optional<LspDocumentIdentity>& active_document) -> bool;

 private:
  bool ready_{};
  std::uint64_t diagnostics_revision_{};
  std::uint64_t semantic_tokens_revision_{};
  std::optional<LspDocumentIdentity> observed_outline_;
  std::optional<LspDocumentIdentity> requested_outline_;
  std::optional<LspDocumentIdentity> rendered_outline_;
  unsigned outline_stable_tick_{};
  bool outline_reset_pending_{true};
};

[[nodiscard]] auto lspOperationLabel(LspOperation operation) noexcept -> std::string_view;

}  // namespace tuiide
