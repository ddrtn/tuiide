#pragma once

#include "tuiide/document.hpp"
#include "tuiide/process.hpp"

#include <filesystem>
#include <chrono>
#include <map>
#include <optional>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

namespace tuiide {

[[nodiscard]] auto lspLanguageId(const std::filesystem::path& path) -> std::string;
[[nodiscard]] auto lspFileUri(const std::filesystem::path& path) -> std::string;
[[nodiscard]] auto lspPathFromFileUri(std::string_view value) -> std::filesystem::path;
[[nodiscard]] auto lspResponseError(const nlohmann::json& message)
  -> std::optional<std::string>;

/** Диагностика clangd; диапазон использует строки и UTF-16 колонки протокола LSP. */
struct Diagnostic {
  std::filesystem::path path;
  Position position;
  int severity{};
  std::string message;
};

/** Семантический токен LSP. `column` и `length` измеряются в кодовых единицах UTF-16. */
struct SemanticToken {
  std::filesystem::path path;
  std::size_t line{};
  std::size_t column{};  // UTF-16 code units
  std::size_t length{};  // UTF-16 code units
  std::string type;
  std::uint32_t modifiers{};
};

auto decodeSemanticTokens(const std::filesystem::path& path, const std::vector<std::uint32_t>& data,
  const std::vector<std::string>& token_types) -> std::vector<SemanticToken>;

struct SourceLocation {
  std::filesystem::path path;
  Position position;
};

struct LspTextEdit {
  Position start;  // UTF-16 line/column
  Position end;
  std::string text;
};

struct WorkspaceFileEdit {
  std::filesystem::path path;
  std::vector<LspTextEdit> edits;
  std::optional<int> version;
};

enum class WorkspaceFileOperationKind { Create, Rename, Delete };

struct WorkspaceFileOperation {
  WorkspaceFileOperationKind kind{WorkspaceFileOperationKind::Create};
  std::filesystem::path path;
  std::filesystem::path new_path;
  bool overwrite{};
  bool ignore_if_exists{};
  bool recursive{};
  bool ignore_if_not_exists{};
};

/** Набор текстовых и файловых изменений, присланный сервером как одна транзакция. */
struct WorkspaceEdit {
  std::vector<WorkspaceFileEdit> files;
  std::vector<WorkspaceFileOperation> file_operations;
};

struct WorkspaceApplyRequest {
  nlohmann::json id;
  std::string label;
  WorkspaceEdit edit;
};

[[nodiscard]] auto parseWorkspaceEdit(const nlohmann::json& value) -> WorkspaceEdit;

struct LspCompletionItem {
  std::string label;
  std::string insertion;
  std::string detail;
  std::string documentation;
  int kind{};
  std::optional<LspTextEdit> edit;
  std::filesystem::path source_path;
  int source_version{};
};

struct LspSignature {
  std::string label;
  std::string documentation;
  std::vector<std::string> parameters;
  std::size_t active_parameter{};
  bool active{};
};

struct LspDocumentSymbol {
  std::string name;
  std::string detail;
  int kind{};
  Position position;  // UTF-16 line/column
  std::size_t depth{};
};

struct LspDocumentSymbols {
  std::filesystem::path path;
  int version{};
  std::vector<LspDocumentSymbol> symbols;
};

struct LspCodeAction {
  std::string title;
  std::string kind;
  WorkspaceEdit edit;
  std::filesystem::path source_path;
  int source_version{};
  bool automatic{};
};

struct LspNavigationItem {
  std::string name;
  std::string detail;
  std::string relation;
  int kind{};
  std::filesystem::path path;
  Position position;  // UTF-16 line/column
};

struct LspHierarchy {
  std::string root;
  std::vector<LspNavigationItem> items;
};

auto parseDocumentSymbols(const nlohmann::json& result,
  const std::filesystem::path& requested_path) -> std::vector<LspDocumentSymbol>;

enum class LspOperation {
  Server, Completion, SignatureHelp, Hover, Definition, References, Rename, DocumentSymbols,
  CodeActions, OrganizeIncludes, SwitchSourceHeader, WorkspaceSymbols, CallHierarchy, TypeHierarchy
};

struct LspFeedback {
  LspOperation operation{LspOperation::Server};
  bool error{};
  std::string message;
};

/**
 * Клиент JSON-RPC для clangd, работающий поверх AsyncProcess.
 * Отправляет изменения документов с revision, хранит ответы до опроса UI и
 * отделяет ошибки протокола от пустых, но успешных результатов. Фильтрация
 * устаревших ответов принадлежит LspUiController.
 */
class LspClient {
 public:
  auto start(const std::filesystem::path& root,
    const std::vector<std::string>& extra_arguments = {},
    const std::map<std::string, std::string>& environment = {},
    const std::filesystem::path& compilation_database_directory = {}) -> bool;
  void stop();
  /** Регистрирует документ в clangd; каждой отправке соответствует close(). */
  void open(const Document& document);
  /** Ставит изменение в очередь; отправка объединяется debounce-механизмом. */
  void change(const Document& document);
  void close(const Document& document);
  void setActiveDocument(const Document* document);
  void requestCompletion(const Document& document);
  void requestSignatureHelp(const Document& document);
  void requestHover(const Document& document);
  void requestDefinition(const Document& document);
  void requestReferences(const Document& document);
  void requestRename(const Document& document, std::string new_name);
  void requestDocumentSymbols(const Document& document);
  void requestCodeActions(const Document& document, Position start, Position end);
  void requestOrganizeIncludes(const Document& document);
  void requestSwitchSourceHeader(const Document& document);
  void requestWorkspaceSymbols(std::string query);
  void requestCallHierarchy(const Document& document);
  void requestTypeHierarchy(const Document& document);
  void poll();

  [[nodiscard]] auto running() const -> bool;
  [[nodiscard]] auto ready() const -> bool;
  auto takeCompletions() -> std::vector<LspCompletionItem>;
  auto takeSignatures() -> std::vector<LspSignature>;
  auto takeHover() -> std::string;
  auto takeDefinitions() -> std::vector<SourceLocation>;
  auto takeReferences() -> std::vector<SourceLocation>;
  auto takeRenameEdit() -> std::optional<WorkspaceEdit>;
  auto takeWorkspaceApplyRequests() -> std::vector<WorkspaceApplyRequest>;
  void respondWorkspaceApplyEdit(nlohmann::json id, bool applied, std::string failure_reason = {});
  auto takeDocumentSymbols() -> std::optional<LspDocumentSymbols>;
  auto takeCodeActions() -> std::vector<LspCodeAction>;
  auto takeSwitchedSourceHeader() -> std::optional<std::filesystem::path>;
  auto takeWorkspaceSymbols() -> std::vector<LspNavigationItem>;
  auto takeCallHierarchy() -> std::optional<LspHierarchy>;
  auto takeTypeHierarchy() -> std::optional<LspHierarchy>;
  auto takeFeedback() -> std::vector<LspFeedback>;
  void clearDiagnostics();
  [[nodiscard]] auto diagnostics() const -> const std::vector<Diagnostic>&;
  [[nodiscard]] auto semanticTokens() const -> const std::vector<SemanticToken>&;
  [[nodiscard]] auto diagnosticsRevision() const -> std::uint64_t;
  [[nodiscard]] auto semanticTokensRevision() const -> std::uint64_t;

 private:
  void send(const nlohmann::json& message);
  void notify(std::string method, nlohmann::json params);
  auto request(std::string method, nlohmann::json params) -> int;
  void cancelRequest(int id);
  void flushChange(const std::filesystem::path& path);
  void flushChanges();
  void trackDocumentRequest(int id, const Document& document);
  void handle(const nlohmann::json& message);
  static auto uri(const std::filesystem::path& path) -> std::string;
  static auto pathFromUri(std::string_view value) -> std::filesystem::path;
  static auto position(const Document& document) -> nlohmann::json;
  void queueSemanticTokens(const std::filesystem::path& path);

  AsyncProcess process_;
  std::filesystem::path root_;
  std::string receive_buffer_;
  int next_id_{1};
  int initialize_id_{};
  int completion_id_{};
  std::filesystem::path completion_path_;
  int completion_version_{};
  int signature_id_{};
  int hover_id_{};
  int definition_id_{};
  int references_id_{};
  int rename_id_{};
  int document_symbols_id_{};
  std::filesystem::path document_symbols_path_;
  int document_symbols_version_{};
  int code_actions_id_{};
  std::filesystem::path code_actions_path_;
  int code_actions_version_{};
  bool organize_includes_request_{};
  int switch_source_header_id_{};
  int workspace_symbols_id_{};
  int call_prepare_id_{};
  int call_incoming_id_{};
  int call_outgoing_id_{};
  int call_pending_{};
  int type_prepare_id_{};
  int type_supertypes_id_{};
  int type_subtypes_id_{};
  int type_pending_{};
  bool initialized_{};
  bool process_started_{};
  bool exit_reported_{};
  std::vector<LspCompletionItem> completions_;
  std::vector<LspSignature> signatures_;
  std::string hover_;
  std::vector<SourceLocation> definitions_;
  std::vector<SourceLocation> references_;
  std::optional<WorkspaceEdit> rename_edit_;
  std::vector<WorkspaceApplyRequest> workspace_apply_requests_;
  std::optional<LspDocumentSymbols> document_symbols_;
  std::vector<LspCodeAction> code_actions_;
  std::optional<std::filesystem::path> switched_source_header_;
  std::vector<LspNavigationItem> workspace_symbols_;
  LspHierarchy call_hierarchy_;
  LspHierarchy type_hierarchy_;
  bool call_hierarchy_ready_{};
  bool type_hierarchy_ready_{};
  std::vector<LspFeedback> feedback_;
  std::vector<Diagnostic> diagnostics_;
  std::unordered_map<std::filesystem::path, nlohmann::json> diagnostic_payloads_;
  std::vector<SemanticToken> semantic_tokens_;
  std::vector<std::string> semantic_token_types_;
  struct VersionedRequest {
    std::filesystem::path path;
    int version{};
  };
  std::unordered_map<int, VersionedRequest> semantic_requests_;
  std::unordered_map<int, VersionedRequest> document_requests_;
  std::unordered_set<int> pending_requests_;
  std::unordered_set<std::filesystem::path> semantic_dirty_;
  struct OpenDocumentState {
    std::string text;
    std::string language_id;
    int version{};
    bool announced{};
    bool change_pending{};
    std::chrono::steady_clock::time_point changed_at{};
  };
  std::unordered_map<std::filesystem::path, OpenDocumentState> open_documents_;
  std::filesystem::path active_document_path_;
  bool active_document_set_{};
  std::uint64_t diagnostics_revision_{};
  std::uint64_t semantic_tokens_revision_{};
};

}  // namespace tuiide
