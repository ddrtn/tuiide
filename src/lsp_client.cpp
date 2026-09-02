#include "tuiide/lsp_client.hpp"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <sstream>

namespace tuiide {
using json = nlohmann::json;

auto lspLanguageId(const std::filesystem::path& path) -> std::string {
  return path.extension() == ".c" ? "c" : "cpp";
}

auto lspFileUri(const std::filesystem::path& path) -> std::string {
  static constexpr char hex[] = "0123456789ABCDEF";
  const auto value = std::filesystem::absolute(path).generic_string();
  std::string result = "file://";
  for (const unsigned char c : value) {
    const bool ascii_alphanumeric = (c >= 'a' && c <= 'z')
      || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    const bool unreserved = ascii_alphanumeric
      || c == '-' || c == '.' || c == '_' || c == '~' || c == '/';
    if (unreserved) result.push_back(static_cast<char>(c));
    else {
      result.push_back('%');
      result.push_back(hex[c >> 4U]);
      result.push_back(hex[c & 0x0fU]);
    }
  }
  return result;
}

auto lspPathFromFileUri(std::string_view value) -> std::filesystem::path {
  constexpr std::string_view prefix = "file://";
  if (!value.starts_with(prefix)) return {};
  value.remove_prefix(prefix.size());
  constexpr std::string_view localhost = "localhost";
  if (value.starts_with(localhost)) value.remove_prefix(localhost.size());
  if (value.empty() || value.front() != '/') return {};
  std::string decoded;
  decoded.reserve(value.size());
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (value[i] == '%') {
      const auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
      };
      if (i + 2 >= value.size()) return {};
      const int high = hex(value[i + 1]);
      const int low = hex(value[i + 2]);
      if (high < 0 || low < 0 || (high == 0 && low == 0)) return {};
      decoded.push_back(static_cast<char>(high * 16 + low));
      i += 2;
      continue;
    }
    if (value[i] == '?' || value[i] == '#') return {};
    decoded.push_back(value[i]);
  }
  return std::filesystem::path(decoded).lexically_normal();
}

auto lspResponseError(const json& message) -> std::optional<std::string> {
  if (!message.is_object() || !message.contains("error")) return std::nullopt;
  const auto& error = message["error"];
  if (!error.is_object()) return "Malformed JSON-RPC error response";
  if (error.contains("message") && error["message"].is_string()) {
    auto text = error["message"].get<std::string>();
    if (!text.empty()) return text;
  }
  if (error.contains("code") && error["code"].is_number_integer())
    return "JSON-RPC error " + error["code"].dump();
  return "Unknown JSON-RPC error";
}

auto parseWorkspaceEdit(const json& value) -> WorkspaceEdit {
  WorkspaceEdit workspace;
  const auto addEdits = [&workspace](const std::filesystem::path& path, const json& edits,
      std::optional<int> version = std::nullopt) {
    if (path.empty() || !edits.is_array()) return;
    WorkspaceFileEdit file{path, {}, version};
    for (const auto& edit : edits) {
      if (!edit.is_object() || !edit.contains("range")) continue;
      const auto& range = edit["range"];
      if (!range.contains("start") || !range.contains("end")) continue;
      const auto& start = range["start"];
      const auto& end = range["end"];
      file.edits.push_back({{start.value("line", 0U), start.value("character", 0U)},
        {end.value("line", 0U), end.value("character", 0U)}, edit.value("newText", std::string{})});
    }
    if (!file.edits.empty()) workspace.files.push_back(std::move(file));
  };
  if (!value.is_object()) return workspace;
  if (value.contains("changes") && value["changes"].is_object())
    for (auto iterator = value["changes"].begin(); iterator != value["changes"].end(); ++iterator)
      addEdits(lspPathFromFileUri(iterator.key()), iterator.value());
  if (value.contains("documentChanges") && value["documentChanges"].is_array()) {
    for (const auto& change : value["documentChanges"]) {
      if (!change.is_object()) continue;
      if (change.contains("textDocument") && change.contains("edits")) {
        const auto& text_document = change["textDocument"];
        std::optional<int> version;
        if (text_document.contains("version") && text_document["version"].is_number_integer())
          version = text_document["version"].get<int>();
        addEdits(lspPathFromFileUri(text_document.value("uri", std::string{})), change["edits"], version);
        continue;
      }
      const auto kind = change.value("kind", std::string{});
      const auto options = change.value("options", json::object());
      if (kind == "create") {
        workspace.file_operations.push_back({WorkspaceFileOperationKind::Create,
          lspPathFromFileUri(change.value("uri", std::string{})), {},
          options.value("overwrite", false), options.value("ignoreIfExists", false), false, false});
      } else if (kind == "rename") {
        workspace.file_operations.push_back({WorkspaceFileOperationKind::Rename,
          lspPathFromFileUri(change.value("oldUri", std::string{})),
          lspPathFromFileUri(change.value("newUri", std::string{})),
          options.value("overwrite", false), options.value("ignoreIfExists", false), false, false});
      } else if (kind == "delete") {
        workspace.file_operations.push_back({WorkspaceFileOperationKind::Delete,
          lspPathFromFileUri(change.value("uri", std::string{})), {}, false, false,
          options.value("recursive", false), options.value("ignoreIfNotExists", false)});
      }
    }
  }
  return workspace;
}

namespace {
auto parseNavigationItem(const json& item, std::string relation = {}) -> std::optional<LspNavigationItem> {
  if (!item.is_object()) return std::nullopt;
  auto location = item.value("location", json{});
  const auto uri = location.is_object() ? location.value("uri", std::string{})
    : item.value("uri", std::string{});
  auto range = location.is_object() ? location.value("range", json{})
    : item.value("selectionRange", item.value("range", json{}));
  if (uri.empty() || !range.is_object() || !range.contains("start")) return std::nullopt;
  const auto& start = range["start"];
  return LspNavigationItem{item.value("name", std::string{}),
    item.value("detail", item.value("containerName", std::string{})), std::move(relation),
    item.value("kind", 0), lspPathFromFileUri(uri),
    {start.value("line", 0U), start.value("character", 0U)}};
}
}  // namespace

auto decodeSemanticTokens(const std::filesystem::path& path, const std::vector<std::uint32_t>& data,
  const std::vector<std::string>& token_types) -> std::vector<SemanticToken> {
  std::vector<SemanticToken> result;
  std::size_t line{};
  std::size_t column{};
  for (std::size_t i = 0; i + 4 < data.size(); i += 5) {
    line += data[i];
    column = data[i] == 0 ? column + data[i + 1] : data[i + 1];
    const auto type_index = static_cast<std::size_t>(data[i + 3]);
    if (type_index >= token_types.size()) continue;
    result.push_back({path, line, column, data[i + 2], token_types[type_index], data[i + 4]});
  }
  return result;
}

auto parseDocumentSymbols(const json& result, const std::filesystem::path& requested_path)
    -> std::vector<LspDocumentSymbol> {
  std::vector<LspDocumentSymbol> symbols;
  if (!result.is_array()) return symbols;
  const auto append = [&](const auto& self, const json& item, std::size_t depth) -> void {
    if (!item.is_object()) return;
    const auto name = item.value("name", std::string{});
    auto range = item.value("selectionRange", json{});
    if (!range.is_object()) range = item.value("range", json{});
    if (name.empty() || !range.contains("start")) return;
    const auto& start = range["start"];
    symbols.push_back({name, item.value("detail", item.value("containerName", std::string{})),
      item.value("kind", 0), {start.value("line", 0U), start.value("character", 0U)}, depth});
    if (item.contains("children") && item["children"].is_array())
      for (const auto& child : item["children"]) self(self, child, depth + 1);
  };
  for (const auto& item : result) {
    if (item.contains("location")) {
      const auto& location = item["location"];
      const auto path = lspPathFromFileUri(location.value("uri", std::string{}));
      if (!path.empty() && normalizePath(path) != normalizePath(requested_path)) continue;
      auto normalized = item;
      normalized["range"] = location.value("range", json{});
      append(append, normalized, 0);
    } else append(append, item, 0);
  }
  return symbols;
}

auto LspClient::start(const std::filesystem::path& root,
    const std::vector<std::string>& extra_arguments,
    const std::map<std::string, std::string>& environment,
    const std::filesystem::path& compilation_database_directory) -> bool {
  root_ = std::filesystem::absolute(root);
  std::vector<std::string> arguments{"clangd", "--background-index", "--clang-tidy", "--completion-style=detailed"};
  arguments.insert(arguments.end(), extra_arguments.begin(), extra_arguments.end());
  if (!compilation_database_directory.empty())
    arguments.push_back("--compile-commands-dir=" + normalizePath(compilation_database_directory).string());
  if (!process_.start(arguments, false, root_, environment)) return false;
  process_started_ = true; exit_reported_ = false;
  initialize_id_ = request("initialize", {{"processId", nullptr}, {"rootUri", uri(root_)},
    {"capabilities", {{"workspace", {{"applyEdit", true}, {"workspaceEdit", {
      {"documentChanges", true}, {"resourceOperations", json::array({"create", "rename", "delete"})},
      {"failureHandling", "transactional"}}}}}, {"textDocument", {{"completion", {{"completionItem", {{"snippetSupport", false}}}}},
      {"signatureHelp", json::object()}, {"hover", json::object()},
      {"codeAction", {{"codeActionLiteralSupport", {{"codeActionKind", {{"valueSet", json::array({
        "quickfix", "refactor", "source.organizeImports"})}}}}}}},
      {"callHierarchy", json::object()}, {"typeHierarchy", json::object()},
      {"documentSymbol", {{"hierarchicalDocumentSymbolSupport", true}}}, {"semanticTokens", {{"requests", {{"full", true}}},
        {"tokenTypes", json::array()}, {"tokenModifiers", json::array()}, {"formats", json::array({"relative"})}}}}}}}});
  return true;
}

void LspClient::stop() {
  process_started_ = false;
  if (process_.running()) {
    // clangd ожидает didClose для каждого didOpen. Открытые документы способны
    // удерживать фоновую индексацию и writer pipe, пока приложение завершает работу.
    for (const auto& [path, state] : open_documents_)
      if (state.announced) notify("textDocument/didClose", {{"textDocument", {{"uri", uri(path)}}}});
    open_documents_.clear();
    semantic_dirty_.clear();
    semantic_requests_.clear();
    request("shutdown", nullptr);
    notify("exit", nullptr);
  }
  process_.stop();
  initialized_ = false; initialize_id_ = 0; exit_reported_ = false;
  receive_buffer_.clear(); completions_.clear(); signatures_.clear(); hover_.clear(); definitions_.clear(); references_.clear();
  rename_edit_.reset(); workspace_apply_requests_.clear(); document_symbols_.reset(); code_actions_.clear(); switched_source_header_.reset();
  workspace_symbols_.clear(); call_hierarchy_ = {}; type_hierarchy_ = {};
  call_hierarchy_ready_ = false; type_hierarchy_ready_ = false; call_pending_ = 0; type_pending_ = 0;
  feedback_.clear();
  diagnostics_.clear(); diagnostic_payloads_.clear(); semantic_tokens_.clear(); semantic_token_types_.clear(); semantic_requests_.clear(); semantic_dirty_.clear();
  open_documents_.clear(); document_requests_.clear(); pending_requests_.clear(); active_document_path_.clear();
  active_document_set_ = false;
  ++diagnostics_revision_; ++semantic_tokens_revision_;
}

void LspClient::open(const Document& document) {
  auto& state = open_documents_[document.path()];
  state = {document.text(), lspLanguageId(document.path()), document.version(), false, false, {}};
  if (initialized_) {
    notify("textDocument/didOpen", {{"textDocument", {{"uri", uri(document.path())}, {"languageId", state.language_id},
      {"version", state.version}, {"text", state.text}}}});
    state.announced = true;
  }
  semantic_dirty_.insert(document.path());
  queueSemanticTokens(document.path());
}

void LspClient::change(const Document& document) {
  auto& state = open_documents_[document.path()];
  state.text = document.text(); state.language_id = lspLanguageId(document.path()); state.version = document.version();
  std::vector<int> stale_requests;
  for (const auto& [id, request_state] : document_requests_)
    if (request_state.path == document.path() && request_state.version != document.version())
      stale_requests.push_back(id);
  for (const auto id : stale_requests) cancelRequest(id);
  if (initialized_ && !state.announced) {
    notify("textDocument/didOpen", {{"textDocument", {{"uri", uri(document.path())}, {"languageId", state.language_id},
      {"version", state.version}, {"text", state.text}}}});
    state.announced = true;
  } else if (initialized_) {
    state.change_pending = true;
    state.changed_at = std::chrono::steady_clock::now();
  }
  semantic_dirty_.insert(document.path());
}

void LspClient::close(const Document& document) {
  std::vector<int> document_requests;
  for (const auto& [id, request_state] : document_requests_)
    if (request_state.path == document.path()) document_requests.push_back(id);
  for (const auto id : document_requests) cancelRequest(id);
  const auto open = open_documents_.find(document.path());
  if (open != open_documents_.end() && open->second.announced)
    notify("textDocument/didClose", {{"textDocument", {{"uri", uri(document.path())}}}});
  const auto diagnostics_size = diagnostics_.size();
  diagnostics_.erase(std::remove_if(diagnostics_.begin(), diagnostics_.end(), [&document](const Diagnostic& diagnostic) {
    return diagnostic.path == document.path();
  }), diagnostics_.end());
  if (diagnostics_.size() != diagnostics_size) ++diagnostics_revision_;
  open_documents_.erase(document.path());
  diagnostic_payloads_.erase(document.path());
  semantic_dirty_.erase(document.path());
  const auto semantic_size = semantic_tokens_.size();
  semantic_tokens_.erase(std::remove_if(semantic_tokens_.begin(), semantic_tokens_.end(), [&document](const auto& token) {
    return token.path == document.path();
  }), semantic_tokens_.end());
  if (semantic_tokens_.size() != semantic_size) ++semantic_tokens_revision_;
}

void LspClient::setActiveDocument(const Document* document) {
  active_document_set_ = document != nullptr;
  active_document_path_ = document ? document->path() : std::filesystem::path{};
  std::vector<int> inactive_requests;
  for (const auto& [id, request_state] : document_requests_)
    if (active_document_set_ && request_state.path != active_document_path_)
      inactive_requests.push_back(id);
  for (const auto id : inactive_requests) cancelRequest(id);
}

void LspClient::requestCompletion(const Document& document) {
  flushChange(document.path()); cancelRequest(completion_id_);
  completions_.clear();
  completion_path_ = document.path(); completion_version_ = document.version();
  completion_id_ = request("textDocument/completion", {{"textDocument", {{"uri", uri(document.path())}}}, {"position", position(document)}});
  trackDocumentRequest(completion_id_, document);
}

void LspClient::requestSignatureHelp(const Document& document) {
  flushChange(document.path()); cancelRequest(signature_id_);
  signatures_.clear();
  signature_id_ = request("textDocument/signatureHelp", {{"textDocument", {{"uri", uri(document.path())}}},
    {"position", position(document)}});
  trackDocumentRequest(signature_id_, document);
}

void LspClient::requestHover(const Document& document) {
  flushChange(document.path()); cancelRequest(hover_id_);
  hover_.clear();
  hover_id_ = request("textDocument/hover", {{"textDocument", {{"uri", uri(document.path())}}}, {"position", position(document)}});
  trackDocumentRequest(hover_id_, document);
}

void LspClient::requestDefinition(const Document& document) {
  flushChange(document.path()); cancelRequest(definition_id_);
  definitions_.clear();
  definition_id_ = request("textDocument/definition", {{"textDocument", {{"uri", uri(document.path())}}}, {"position", position(document)}});
  trackDocumentRequest(definition_id_, document);
}

void LspClient::requestReferences(const Document& document) {
  flushChange(document.path()); cancelRequest(references_id_);
  references_.clear();
  references_id_ = request("textDocument/references", {{"textDocument", {{"uri", uri(document.path())}}},
    {"position", position(document)}, {"context", {{"includeDeclaration", true}}}});
  trackDocumentRequest(references_id_, document);
}

void LspClient::requestRename(const Document& document, std::string new_name) {
  flushChange(document.path()); cancelRequest(rename_id_);
  rename_edit_.reset();
  rename_id_ = request("textDocument/rename", {{"textDocument", {{"uri", uri(document.path())}}},
    {"position", position(document)}, {"newName", std::move(new_name)}});
  trackDocumentRequest(rename_id_, document);
}

void LspClient::requestDocumentSymbols(const Document& document) {
  flushChange(document.path()); cancelRequest(document_symbols_id_);
  document_symbols_.reset();
  document_symbols_path_ = document.path(); document_symbols_version_ = document.version();
  document_symbols_id_ = request("textDocument/documentSymbol",
    {{"textDocument", {{"uri", uri(document.path())}}}});
  trackDocumentRequest(document_symbols_id_, document);
}

void LspClient::requestCodeActions(const Document& document, Position start, Position end) {
  flushChange(document.path()); cancelRequest(code_actions_id_);
  code_actions_.clear(); code_actions_path_ = document.path(); code_actions_version_ = document.version();
  organize_includes_request_ = false;
  start.column = document.utf16Column(start.line, start.column);
  end.column = document.utf16Column(end.line, end.column);
  const auto diagnostics = diagnostic_payloads_.contains(document.path())
    ? diagnostic_payloads_.at(document.path()) : json::array();
  code_actions_id_ = request("textDocument/codeAction", {{"textDocument", {{"uri", uri(document.path())}}},
    {"range", {{"start", {{"line", start.line}, {"character", start.column}}},
      {"end", {{"line", end.line}, {"character", end.column}}}}},
    {"context", {{"diagnostics", diagnostics}}}});
  trackDocumentRequest(code_actions_id_, document);
}

void LspClient::requestOrganizeIncludes(const Document& document) {
  flushChange(document.path()); cancelRequest(code_actions_id_);
  code_actions_.clear(); code_actions_path_ = document.path(); code_actions_version_ = document.version();
  organize_includes_request_ = true;
  code_actions_id_ = request("textDocument/codeAction", {{"textDocument", {{"uri", uri(document.path())}}},
    {"range", {{"start", {{"line", 0}, {"character", 0}}}, {"end", {{"line", 0}, {"character", 0}}}}},
    {"context", {{"diagnostics", json::array()}, {"only", json::array({"source.organizeImports"})}}}});
  trackDocumentRequest(code_actions_id_, document);
}

void LspClient::requestSwitchSourceHeader(const Document& document) {
  flushChange(document.path()); cancelRequest(switch_source_header_id_);
  switched_source_header_.reset();
  switch_source_header_id_ = request("textDocument/switchSourceHeader",
    {{"uri", uri(document.path())}});
  trackDocumentRequest(switch_source_header_id_, document);
}

void LspClient::requestWorkspaceSymbols(std::string query) {
  cancelRequest(workspace_symbols_id_);
  workspace_symbols_.clear();
  workspace_symbols_id_ = request("workspace/symbol", {{"query", std::move(query)}});
}

void LspClient::requestCallHierarchy(const Document& document) {
  flushChange(document.path()); cancelRequest(call_prepare_id_); cancelRequest(call_incoming_id_); cancelRequest(call_outgoing_id_);
  call_hierarchy_ = {}; call_hierarchy_ready_ = false; call_pending_ = 0;
  call_prepare_id_ = request("textDocument/prepareCallHierarchy",
    {{"textDocument", {{"uri", uri(document.path())}}}, {"position", position(document)}});
  trackDocumentRequest(call_prepare_id_, document);
}

void LspClient::requestTypeHierarchy(const Document& document) {
  flushChange(document.path()); cancelRequest(type_prepare_id_); cancelRequest(type_supertypes_id_); cancelRequest(type_subtypes_id_);
  type_hierarchy_ = {}; type_hierarchy_ready_ = false; type_pending_ = 0;
  type_prepare_id_ = request("textDocument/prepareTypeHierarchy",
    {{"textDocument", {{"uri", uri(document.path())}}}, {"position", position(document)}});
  trackDocumentRequest(type_prepare_id_, document);
}

void LspClient::poll() {
  flushChanges();
  for (auto& chunk : process_.drain()) receive_buffer_ += chunk;
  for (;;) {
    const auto header_end = receive_buffer_.find("\r\n\r\n");
    if (header_end == std::string::npos) break;
    const auto length_at = receive_buffer_.find("Content-Length:");
    if (length_at == std::string::npos || length_at > header_end) { receive_buffer_.erase(0, header_end + 4); continue; }
    const auto number_at = length_at + 15;
    const auto length = static_cast<std::size_t>(std::stoul(receive_buffer_.substr(number_at, header_end - number_at)));
    if (receive_buffer_.size() < header_end + 4 + length) break;
    const auto payload = receive_buffer_.substr(header_end + 4, length);
    receive_buffer_.erase(0, header_end + 4 + length);
    try { handle(json::parse(payload)); } catch (const json::exception&) {}
  }
  if (process_started_ && !process_.running() && process_.exitCode().has_value() && !exit_reported_) {
    exit_reported_ = true; initialized_ = false;
    feedback_.push_back({LspOperation::Server, true,
      "clangd exited with code " + std::to_string(*process_.exitCode())});
  }
}

auto LspClient::running() const -> bool { return process_.running(); }
auto LspClient::ready() const -> bool { return initialized_; }
auto LspClient::takeCompletions() -> std::vector<LspCompletionItem> { std::vector<LspCompletionItem> result; result.swap(completions_); return result; }
auto LspClient::takeSignatures() -> std::vector<LspSignature> { std::vector<LspSignature> result; result.swap(signatures_); return result; }
auto LspClient::takeHover() -> std::string { std::string result; result.swap(hover_); return result; }
auto LspClient::takeDefinitions() -> std::vector<SourceLocation> { std::vector<SourceLocation> result; result.swap(definitions_); return result; }
auto LspClient::takeReferences() -> std::vector<SourceLocation> { std::vector<SourceLocation> result; result.swap(references_); return result; }
auto LspClient::takeRenameEdit() -> std::optional<WorkspaceEdit> { auto result = std::move(rename_edit_); rename_edit_.reset(); return result; }
auto LspClient::takeWorkspaceApplyRequests() -> std::vector<WorkspaceApplyRequest> {
  std::vector<WorkspaceApplyRequest> result; result.swap(workspace_apply_requests_); return result;
}
void LspClient::respondWorkspaceApplyEdit(json id, bool applied, std::string failure_reason) {
  json result{{"applied", applied}};
  if (!applied && !failure_reason.empty()) result["failureReason"] = std::move(failure_reason);
  send({{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}});
}
auto LspClient::takeDocumentSymbols() -> std::optional<LspDocumentSymbols> {
  auto result = std::move(document_symbols_); document_symbols_.reset(); return result;
}
auto LspClient::takeCodeActions() -> std::vector<LspCodeAction> {
  std::vector<LspCodeAction> result; result.swap(code_actions_); return result;
}
auto LspClient::takeSwitchedSourceHeader() -> std::optional<std::filesystem::path> {
  auto result = std::move(switched_source_header_); switched_source_header_.reset(); return result;
}
auto LspClient::takeWorkspaceSymbols() -> std::vector<LspNavigationItem> {
  std::vector<LspNavigationItem> result; result.swap(workspace_symbols_); return result;
}
auto LspClient::takeCallHierarchy() -> std::optional<LspHierarchy> {
  if (!call_hierarchy_ready_) return std::nullopt;
  call_hierarchy_ready_ = false; return std::move(call_hierarchy_);
}
auto LspClient::takeTypeHierarchy() -> std::optional<LspHierarchy> {
  if (!type_hierarchy_ready_) return std::nullopt;
  type_hierarchy_ready_ = false; return std::move(type_hierarchy_);
}
auto LspClient::takeFeedback() -> std::vector<LspFeedback> { std::vector<LspFeedback> result; result.swap(feedback_); return result; }
auto LspClient::diagnostics() const -> const std::vector<Diagnostic>& { return diagnostics_; }
void LspClient::clearDiagnostics() { diagnostics_.clear(); diagnostic_payloads_.clear(); ++diagnostics_revision_; }
auto LspClient::semanticTokens() const -> const std::vector<SemanticToken>& { return semantic_tokens_; }
auto LspClient::diagnosticsRevision() const -> std::uint64_t { return diagnostics_revision_; }
auto LspClient::semanticTokensRevision() const -> std::uint64_t { return semantic_tokens_revision_; }

void LspClient::send(const json& message) {
  const auto payload = message.dump();
  process_.write("Content-Length: " + std::to_string(payload.size()) + "\r\n\r\n" + payload);
}
void LspClient::notify(std::string method, json params) { send({{"jsonrpc", "2.0"}, {"method", std::move(method)}, {"params", std::move(params)}}); }
auto LspClient::request(std::string method, json params) -> int {
  const int id = next_id_++;
  send({{"jsonrpc", "2.0"}, {"id", id}, {"method", std::move(method)}, {"params", std::move(params)}});
  pending_requests_.insert(id);
  return id;
}

void LspClient::cancelRequest(int id) {
  if (id == 0 || pending_requests_.erase(id) == 0) return;
  document_requests_.erase(id);
  notify("$/cancelRequest", {{"id", id}});
}

void LspClient::flushChange(const std::filesystem::path& path) {
  const auto open = open_documents_.find(path);
  if (!initialized_ || open == open_documents_.end() || !open->second.announced
      || !open->second.change_pending) return;
  auto& state = open->second;
  notify("textDocument/didChange", {{"textDocument", {{"uri", uri(path)}, {"version", state.version}}},
    {"contentChanges", json::array({{{"text", state.text}}})}});
  state.change_pending = false;
  queueSemanticTokens(path);
}

void LspClient::flushChanges() {
  constexpr auto delay = std::chrono::milliseconds{150};
  const auto now = std::chrono::steady_clock::now();
  std::vector<std::filesystem::path> ready;
  for (const auto& [path, state] : open_documents_)
    if (state.change_pending && now - state.changed_at >= delay) ready.push_back(path);
  for (const auto& path : ready) flushChange(path);
}

void LspClient::trackDocumentRequest(int id, const Document& document) {
  document_requests_[id] = {document.path(), document.version()};
}

void LspClient::queueSemanticTokens(const std::filesystem::path& path) {
  if (!initialized_ || !open_documents_.contains(path) || !open_documents_.at(path).announced
      || !semantic_dirty_.contains(path) || semantic_token_types_.empty()) return;
  if (open_documents_.at(path).change_pending) return;
  if (std::any_of(semantic_requests_.begin(), semantic_requests_.end(), [&path](const auto& request) {
        return request.second.path == path;
      })) return;
  semantic_dirty_.erase(path);
  const auto id = request("textDocument/semanticTokens/full", {{"textDocument", {{"uri", uri(path)}}}});
  semantic_requests_[id] = {path, open_documents_.at(path).version};
}

void LspClient::handle(const json& message) {
  if (message.value("method", std::string{}) == "workspace/applyEdit"
      && message.contains("id")
      && (message["id"].is_number_integer() || message["id"].is_string())) {
    const auto params = message.value("params", json::object());
    auto edit = parseWorkspaceEdit(params.value("edit", json{}));
    workspace_apply_requests_.push_back({message["id"],
      params.value("label", std::string("Language server edit")), std::move(edit)});
    return;
  }
  if (message.contains("id") && message["id"].is_number_integer()) {
    const int id = message["id"].get<int>();
    if (pending_requests_.erase(id) == 0) return;
    std::optional<VersionedRequest> response_context;
    if (const auto context = document_requests_.find(id); context != document_requests_.end()) {
      response_context = context->second;
      document_requests_.erase(context);
    }
    const auto operation = [this, id]() -> std::optional<LspOperation> {
      if (id == completion_id_) return LspOperation::Completion;
      if (id == signature_id_) return LspOperation::SignatureHelp;
      if (id == hover_id_) return LspOperation::Hover;
      if (id == definition_id_) return LspOperation::Definition;
      if (id == references_id_) return LspOperation::References;
      if (id == rename_id_) return LspOperation::Rename;
      if (id == document_symbols_id_) return LspOperation::DocumentSymbols;
      if (id == code_actions_id_) return organize_includes_request_
        ? LspOperation::OrganizeIncludes : LspOperation::CodeActions;
      if (id == switch_source_header_id_) return LspOperation::SwitchSourceHeader;
      if (id == workspace_symbols_id_) return LspOperation::WorkspaceSymbols;
      if (id == call_prepare_id_ || id == call_incoming_id_ || id == call_outgoing_id_)
        return LspOperation::CallHierarchy;
      if (id == type_prepare_id_ || id == type_supertypes_id_ || id == type_subtypes_id_)
        return LspOperation::TypeHierarchy;
      return std::nullopt;
    };
    const auto response_error = lspResponseError(message);
    if (id == initialize_id_) {
      initialize_id_ = 0;
      if (response_error) {
        feedback_.push_back({LspOperation::Server, true,
          "clangd initialization failed: " + *response_error});
        process_started_ = false; process_.stop();
        return;
      }
      if (!message.contains("result") || !message["result"].is_object()) {
        feedback_.push_back({LspOperation::Server, true, "clangd returned an invalid initialize response"});
        process_started_ = false; process_.stop();
        return;
      }
      if (message["result"].contains("capabilities")) {
        const auto& provider = message["result"]["capabilities"].value("semanticTokensProvider", json::object());
        if (provider.is_object() && provider.contains("legend"))
          semantic_token_types_ = provider["legend"].value("tokenTypes", std::vector<std::string>{});
      }
      initialized_ = true;
      notify("initialized", json::object());
      for (auto& [path, state] : open_documents_) {
        notify("textDocument/didOpen", {{"textDocument", {{"uri", uri(path)}, {"languageId", state.language_id},
          {"version", state.version}, {"text", state.text}}}});
        state.announced = true;
      }
      const std::vector<std::filesystem::path> dirty(semantic_dirty_.begin(), semantic_dirty_.end());
      for (const auto& path : dirty) queueSemanticTokens(path);
      return;
    }
    if (response_context) {
      const auto open = open_documents_.find(response_context->path);
      if (open == open_documents_.end() || open->second.version != response_context->version
          || (active_document_set_ && active_document_path_ != response_context->path)) return;
    }
    if (response_error) {
      if (const auto requested_operation = operation())
        feedback_.push_back({*requested_operation, true, *response_error});
      else if (const auto semantic = semantic_requests_.find(id); semantic != semantic_requests_.end()) {
        semantic_dirty_.insert(semantic->second.path);
        semantic_requests_.erase(semantic);
      }
      if ((id == call_incoming_id_ || id == call_outgoing_id_) && call_pending_ > 0 && --call_pending_ == 0)
        call_hierarchy_ready_ = true;
      if ((id == type_supertypes_id_ || id == type_subtypes_id_) && type_pending_ > 0 && --type_pending_ == 0)
        type_hierarchy_ready_ = true;
      return;
    }
    if (const auto semantic = semantic_requests_.find(id); semantic != semantic_requests_.end()) {
      const auto request_state = semantic->second;
      const auto path = request_state.path;
      semantic_requests_.erase(semantic);
      if (open_documents_.contains(path) && open_documents_.at(path).version == request_state.version
          && message.contains("result") && message["result"].is_object()) {
        const auto data = message["result"].value("data", std::vector<std::uint32_t>{});
        auto decoded = decodeSemanticTokens(path, data, semantic_token_types_);
        semantic_tokens_.erase(std::remove_if(semantic_tokens_.begin(), semantic_tokens_.end(), [&path](const auto& token) {
          return token.path == path;
        }), semantic_tokens_.end());
        semantic_tokens_.insert(semantic_tokens_.end(), std::make_move_iterator(decoded.begin()), std::make_move_iterator(decoded.end()));
        ++semantic_tokens_revision_;
      } else semantic_dirty_.insert(path);
      queueSemanticTokens(path);
    } else if (id == completion_id_) {
      const auto result = message.value("result", json{});
      const auto items = result.is_array() ? result
        : (result.is_object() ? result.value("items", json::array()) : json::array());
      for (const auto& item : items) {
        if (completions_.size() >= 100) break;
        if (!item.is_object()) continue;
        LspCompletionItem completion;
        completion.label = item.value("label", std::string{});
        completion.insertion = item.value("insertText", completion.label);
        completion.detail = item.value("detail", std::string{});
        completion.kind = item.value("kind", 0);
        if (item.contains("textEdit") && item["textEdit"].is_object()) {
          completion.insertion = item["textEdit"].value("newText", completion.insertion);
          const auto& text_edit = item["textEdit"];
          const auto range = text_edit.contains("range") ? text_edit["range"] : text_edit.value("replace", json{});
          if (range.is_object() && range.contains("start") && range.contains("end")) {
            const auto& start = range["start"];
            const auto& end = range["end"];
            completion.edit = LspTextEdit{{start.value("line", 0U), start.value("character", 0U)},
              {end.value("line", 0U), end.value("character", 0U)}, completion.insertion};
          }
        }
        if (item.contains("documentation")) {
          const auto& documentation = item["documentation"];
          completion.documentation = documentation.is_string() ? documentation.get<std::string>()
            : documentation.value("value", std::string{});
        }
        completion.source_path = completion_path_; completion.source_version = completion_version_;
        if (!completion.label.empty()) completions_.push_back(std::move(completion));
      }
      if (completions_.empty()) feedback_.push_back({LspOperation::Completion, false, "no completion items"});
    } else if (id == signature_id_) {
      const auto result = message.value("result", json{});
      const auto items = result.is_object() ? result.value("signatures", json::array()) : json::array();
      const auto active_signature = result.is_object() ? result.value("activeSignature", 0U) : 0U;
      const auto active_parameter = result.is_object() ? result.value("activeParameter", 0U) : 0U;
      for (std::size_t index = 0; index < items.size(); ++index) {
        const auto& item = items[index];
        if (!item.is_object()) continue;
        LspSignature signature;
        signature.label = item.value("label", std::string{});
        if (item.contains("documentation")) {
          const auto& documentation = item["documentation"];
          signature.documentation = documentation.is_string() ? documentation.get<std::string>()
            : documentation.value("value", std::string{});
        }
        signature.active = index == active_signature;
        signature.active_parameter = signature.active ? active_parameter : item.value("activeParameter", 0U);
        for (const auto& parameter : item.value("parameters", json::array())) {
          if (!parameter.is_object() || !parameter.contains("label")) continue;
          const auto& label = parameter["label"];
          if (label.is_string()) signature.parameters.push_back(label.get<std::string>());
          else if (label.is_array() && label.size() == 2 && label[0].is_number_unsigned() && label[1].is_number_unsigned()) {
            const auto begin = label[0].get<std::size_t>();
            const auto end = label[1].get<std::size_t>();
            signature.parameters.push_back(begin <= end && end <= signature.label.size()
              ? signature.label.substr(begin, end - begin) : std::string{});
          }
        }
        if (!signature.label.empty()) signatures_.push_back(std::move(signature));
      }
      if (signatures_.empty()) feedback_.push_back({LspOperation::SignatureHelp, false, "no signature information"});
    } else if (id == hover_id_) {
      const auto result = message.value("result", json{});
      const auto contents = result.is_object() ? result.value("contents", json{}) : json{};
      if (contents.is_string()) hover_ = contents.get<std::string>();
      else if (contents.is_object()) hover_ = contents.value("value", std::string{});
      else if (contents.is_array()) for (const auto& part : contents) hover_ += part.is_string() ? part.get<std::string>() : part.value("value", std::string{});
      if (hover_.empty()) feedback_.push_back({LspOperation::Hover, false, "no symbol information"});
    } else if (id == definition_id_ || id == references_id_) {
      auto& destination = id == definition_id_ ? definitions_ : references_;
      destination.clear();
      const auto result = message.value("result", json{});
      auto items = result.is_array() ? result : (result.is_object() ? json::array({result}) : json::array());
      for (const auto& item : items) {
        const auto location_uri = item.value("uri", item.value("targetUri", std::string{}));
        const auto range = item.contains("range") ? item["range"] : item.value("targetSelectionRange", json{});
        if (location_uri.empty() || !range.contains("start")) continue;
        const auto& start = range["start"];
        destination.push_back({pathFromUri(location_uri), {start.value("line", 0U), start.value("character", 0U)}});
      }
      if (destination.empty()) feedback_.push_back({id == definition_id_ ? LspOperation::Definition : LspOperation::References,
        false, id == definition_id_ ? "definition not found" : "no references found"});
    } else if (id == document_symbols_id_) {
      const auto result = message.value("result", json::array());
      document_symbols_ = LspDocumentSymbols{document_symbols_path_, document_symbols_version_,
        parseDocumentSymbols(result, document_symbols_path_)};
    } else if (id == workspace_symbols_id_) {
      for (const auto& item : message.value("result", json::array())) {
        if (workspace_symbols_.size() >= 200) break;
        if (auto symbol = parseNavigationItem(item)) workspace_symbols_.push_back(std::move(*symbol));
      }
      if (workspace_symbols_.empty())
        feedback_.push_back({LspOperation::WorkspaceSymbols, false, "no matching workspace symbols"});
    } else if (id == call_prepare_id_) {
      const auto result = message.value("result", json::array());
      if (!result.is_array() || result.empty() || !result.front().is_object()) {
        feedback_.push_back({LspOperation::CallHierarchy, false, "call hierarchy is unavailable at the cursor"});
      } else {
        const auto item = result.front();
        call_hierarchy_.root = item.value("name", std::string("symbol")); call_pending_ = 2;
        call_incoming_id_ = request("callHierarchy/incomingCalls", {{"item", item}});
        call_outgoing_id_ = request("callHierarchy/outgoingCalls", {{"item", item}});
        if (response_context) {
          document_requests_[call_incoming_id_] = *response_context;
          document_requests_[call_outgoing_id_] = *response_context;
        }
      }
    } else if (id == call_incoming_id_ || id == call_outgoing_id_) {
      const bool incoming = id == call_incoming_id_;
      for (const auto& call : message.value("result", json::array())) {
        const auto item = call.value(incoming ? "from" : "to", json{});
        if (auto entry = parseNavigationItem(item, incoming ? "incoming" : "outgoing"))
          call_hierarchy_.items.push_back(std::move(*entry));
      }
      if (call_pending_ > 0 && --call_pending_ == 0) {
        call_hierarchy_ready_ = true;
        if (call_hierarchy_.items.empty())
          feedback_.push_back({LspOperation::CallHierarchy, false, "no incoming or outgoing calls"});
      }
    } else if (id == type_prepare_id_) {
      const auto result = message.value("result", json::array());
      if (!result.is_array() || result.empty() || !result.front().is_object()) {
        feedback_.push_back({LspOperation::TypeHierarchy, false, "type hierarchy is unavailable at the cursor"});
      } else {
        const auto item = result.front();
        type_hierarchy_.root = item.value("name", std::string("type")); type_pending_ = 2;
        type_supertypes_id_ = request("typeHierarchy/supertypes", {{"item", item}});
        type_subtypes_id_ = request("typeHierarchy/subtypes", {{"item", item}});
        if (response_context) {
          document_requests_[type_supertypes_id_] = *response_context;
          document_requests_[type_subtypes_id_] = *response_context;
        }
      }
    } else if (id == type_supertypes_id_ || id == type_subtypes_id_) {
      const bool supertype = id == type_supertypes_id_;
      for (const auto& item : message.value("result", json::array()))
        if (auto entry = parseNavigationItem(item, supertype ? "supertype" : "subtype"))
          type_hierarchy_.items.push_back(std::move(*entry));
      if (type_pending_ > 0 && --type_pending_ == 0) {
        type_hierarchy_ready_ = true;
        if (type_hierarchy_.items.empty())
          feedback_.push_back({LspOperation::TypeHierarchy, false, "no supertypes or subtypes"});
      }
    } else if (id == code_actions_id_) {
      const bool automatic = organize_includes_request_;
      for (const auto& item : message.value("result", json::array())) {
        if (!item.is_object()) continue;
        auto edit = parseWorkspaceEdit(item.value("edit", json{}));
        if (edit.files.empty() && edit.file_operations.empty()) continue;
        code_actions_.push_back({item.value("title", std::string("clangd action")),
          item.value("kind", std::string{}), std::move(edit), code_actions_path_, code_actions_version_, automatic});
      }
      if (code_actions_.empty()) feedback_.push_back({automatic ? LspOperation::OrganizeIncludes
        : LspOperation::CodeActions, false, automatic ? "no include changes available" : "no applicable code actions"});
    } else if (id == switch_source_header_id_) {
      const auto response = message.value("result", json{});
      const auto result = response.is_string() ? response.get<std::string>() : std::string{};
      if (result.empty()) feedback_.push_back({LspOperation::SwitchSourceHeader, false, "counterpart not found"});
      else switched_source_header_ = pathFromUri(result);
    } else if (id == rename_id_) {
      auto workspace = parseWorkspaceEdit(message.value("result", json{}));
      if (workspace.files.empty() && workspace.file_operations.empty())
        feedback_.push_back({LspOperation::Rename, false, "rename produced no edits"});
      else rename_edit_ = std::move(workspace);
    }
    return;
  }
  if (message.value("method", std::string{}) == "textDocument/publishDiagnostics") {
    const auto diagnostic_path = pathFromUri(message["params"].value("uri", std::string{}));
    if (message["params"].contains("version") && message["params"]["version"].is_number_integer()
        && open_documents_.contains(diagnostic_path)
        && open_documents_.at(diagnostic_path).version != message["params"]["version"].get<int>()) return;
    diagnostic_payloads_[diagnostic_path] = message["params"].value("diagnostics", json::array());
    diagnostics_.erase(std::remove_if(diagnostics_.begin(), diagnostics_.end(), [&diagnostic_path](const Diagnostic& diagnostic) {
      return diagnostic.path == diagnostic_path;
    }), diagnostics_.end());
    for (const auto& item : message["params"].value("diagnostics", json::array())) {
      const auto& start = item["range"]["start"];
      diagnostics_.push_back({diagnostic_path, {start.value("line", 0U), start.value("character", 0U)}, item.value("severity", 0), item.value("message", std::string{})});
    }
    ++diagnostics_revision_;
  }
}

auto LspClient::uri(const std::filesystem::path& path) -> std::string { return lspFileUri(path); }
auto LspClient::pathFromUri(std::string_view value) -> std::filesystem::path { return lspPathFromFileUri(value); }
auto LspClient::position(const Document& document) -> json {
  const auto cursor = document.cursor();
  return {{"line", cursor.line}, {"character", document.utf16Column(cursor.line, cursor.column)}};
}

}  // namespace tuiide
