#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {
using json = nlohmann::json;

auto readMessage(json& message) -> bool {
  std::string line;
  std::size_t content_length{};
  while (std::getline(std::cin, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) break;
    constexpr std::string_view prefix{"Content-Length:"};
    if (line.starts_with(prefix))
      content_length = static_cast<std::size_t>(std::stoul(line.substr(prefix.size())));
  }
  if (!std::cin || content_length == 0) return false;
  std::string payload(content_length, '\0');
  std::cin.read(payload.data(), static_cast<std::streamsize>(payload.size()));
  if (static_cast<std::size_t>(std::cin.gcount()) != payload.size()) return false;
  message = json::parse(payload, nullptr, false);
  return !message.is_discarded();
}

void send(json message) {
  const auto payload = message.dump();
  std::cout << "Content-Length: " << payload.size() << "\r\n\r\n" << payload << std::flush;
}

auto cursorLine(const json& request) -> std::size_t {
  const auto params = request.value("params", json::object());
  if (params.contains("position"))
    return params.value("position", json::object()).value("line", std::size_t{});
  return params.value("range", json::object()).value("start", json::object())
    .value("line", std::size_t{});
}

auto documentUri(const json& request) -> std::string {
  return request.value("params", json::object()).value("textDocument", json::object())
    .value("uri", std::string{});
}
}  // namespace

int main() {
  json request;
  std::string active_uri;
  while (readMessage(request)) {
    const auto method = request.value("method", std::string{});
    if (method == "exit") return 0;
    if (method == "textDocument/didOpen") {
      active_uri = request.value("params", json::object()).value("textDocument", json::object())
        .value("uri", std::string{});
      if (std::getenv("TUIIDE_FAKE_NULLABLE_DIAGNOSTICS") != nullptr) {
        send({{"jsonrpc", "2.0"},
          {"method", "textDocument/publishDiagnostics"},
          {"params", {{"uri", active_uri}, {"diagnostics", json::array({
            {{"range", {{"start", {{"line", 0}, {"character", 0}}},
              {"end", {{"line", 0}, {"character", 1}}}}},
              {"severity", nullptr}, {"message", nullptr}}
          })}}}});
      }
      continue;
    }
    if (!request.contains("id")) continue;
    const auto id = request["id"];
    if (method == "initialize") {
      send({{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"capabilities", json::object()}}}});
      continue;
    }
    if (method == "shutdown") {
      send({{"jsonrpc", "2.0"}, {"id", id}, {"result", nullptr}});
      continue;
    }
    if (method == "textDocument/semanticTokens/full") {
      send({{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"data", json::array()}}}});
      continue;
    }

    if (method == "workspace/symbol") {
      const auto query = request.value("params", json::object()).value("query", std::string{});
      if (query == "failure") {
        send({{"jsonrpc", "2.0"}, {"id", id},
          {"error", {{"code", -32001}, {"message", "forced operation failure"}}}});
      } else if (query == "matrix") {
        send({{"jsonrpc", "2.0"}, {"id", id}, {"result", json::array({
          {{"name", "matrixSymbol"}, {"kind", 12}, {"containerName", "fixture"},
            {"location", {{"uri", active_uri}, {"range", {
              {"start", {{"line", 0}, {"character", 0}}},
              {"end", {{"line", 0}, {"character", 6}}}}}}}}
        })}});
      } else {
        send({{"jsonrpc", "2.0"}, {"id", id}, {"result", json::array()}});
      }
      continue;
    }

    const auto line = cursorLine(request);
    if (line == 2) {
      send({{"jsonrpc", "2.0"}, {"id", id},
        {"error", {{"code", -32001}, {"message", "forced operation failure"}}}});
      continue;
    }
    const auto uri = documentUri(request);
    json result = nullptr;
    if (line == 0) {
      if (method == "textDocument/completion")
        result = json::array({{{"label", "matrixItem"}, {"insertText", nullptr},
          {"detail", nullptr}, {"documentation", nullptr}, {"kind", 3}}});
      else if (method == "textDocument/signatureHelp")
        result = {{"activeSignature", 0}, {"activeParameter", 0}, {"signatures", json::array({
          {{"label", "int matrix(int value)"}, {"documentation", "matrix signature"},
            {"parameters", json::array({{{"label", "int value"}}})}}
        })}};
      else if (method == "textDocument/hover")
        result = {{"contents", json::array({
          {{"kind", "markdown"}, {"value", nullptr}}, "matrix hover"})}};
      else if (method == "textDocument/definition" || method == "textDocument/references")
        result = json::array({{{"uri", uri}, {"range", {{"start", {{"line", 0}, {"character", 0}}},
          {"end", {{"line", 0}, {"character", 6}}}}}}});
      else if (method == "textDocument/rename")
        result = {{"changes", {{uri, json::array({{{"range", {{"start", {{"line", 0}, {"character", 0}}},
          {"end", {{"line", 0}, {"character", 6}}}}}, {"newText", "renamed"}}})}}}};
      else if (method == "textDocument/codeAction")
        result = json::array({{{"title", "Matrix quick fix"}, {"kind", "quickfix"},
          {"edit", {{"changes", {{uri, json::array({{{"range", {{"start", {{"line", 0}, {"character", 0}}},
            {"end", {{"line", 0}, {"character", 6}}}}}, {"newText", "fixed"}}})}}}}}}});
      else if (method == "textDocument/prepareCallHierarchy"
          || method == "textDocument/prepareTypeHierarchy")
        result = json::array({{{"name", method == "textDocument/prepareCallHierarchy"
            ? "matrixRoot" : "MatrixType"}, {"kind", 12}, {"uri", uri},
          {"range", {{"start", {{"line", 0}, {"character", 0}}},
            {"end", {{"line", 0}, {"character", 6}}}}},
          {"selectionRange", {{"start", {{"line", 0}, {"character", 0}}},
            {"end", {{"line", 0}, {"character", 6}}}}}}});
      else if (method == "callHierarchy/incomingCalls" || method == "callHierarchy/outgoingCalls") {
        const auto item_uri = request.value("params", json::object()).value("item", json::object())
          .value("uri", active_uri);
        const json item{{"name", method == "callHierarchy/incomingCalls" ? "matrixCaller" : "matrixCallee"},
          {"detail", "fixture call"}, {"kind", 12}, {"uri", item_uri},
          {"range", {{"start", {{"line", 0}, {"character", 0}}},
            {"end", {{"line", 0}, {"character", 6}}}}},
          {"selectionRange", {{"start", {{"line", 0}, {"character", 0}}},
            {"end", {{"line", 0}, {"character", 6}}}}}};
        result = json::array({{{method == "callHierarchy/incomingCalls" ? "from" : "to", item}}});
      } else if (method == "typeHierarchy/supertypes" || method == "typeHierarchy/subtypes") {
        const auto item_uri = request.value("params", json::object()).value("item", json::object())
          .value("uri", active_uri);
        result = json::array({{{"name", method == "typeHierarchy/supertypes"
            ? "MatrixBase" : "MatrixDerived"}, {"detail", "fixture type"}, {"kind", 5},
          {"uri", item_uri}, {"range", {{"start", {{"line", 0}, {"character", 0}}},
            {"end", {{"line", 0}, {"character", 6}}}}},
          {"selectionRange", {{"start", {{"line", 0}, {"character", 0}}},
            {"end", {{"line", 0}, {"character", 6}}}}}}});
      }
    }
    send({{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}});
  }
}
