#include "tuiide/lldb_dap_client.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;

void send(const nlohmann::json& message) {
  const auto body = message.dump();
  const auto framed = "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
  std::size_t offset{};
  while (offset < framed.size()) {
    const auto written = ::write(STDOUT_FILENO, framed.data() + offset, framed.size() - offset);
    if (written <= 0) std::exit(2);
    offset += static_cast<std::size_t>(written);
  }
}

void response(const nlohmann::json& request, nlohmann::json body = {}) {
  send({{"seq", request["seq"].get<int>() + 1000}, {"type", "response"},
    {"request_seq", request["seq"]}, {"success", true}, {"command", request["command"]},
    {"body", std::move(body)}});
}

auto runAdapter() -> int {
  std::string input;
  char buffer[2048];
  for (;;) {
    const auto count = ::read(STDIN_FILENO, buffer, sizeof(buffer));
    if (count <= 0) return 0;
    input.append(buffer, static_cast<std::size_t>(count));
    for (;;) {
      const auto headers = input.find("\r\n\r\n");
      if (headers == std::string::npos) break;
      const auto begin = input.find(':') + 1;
      const auto length = static_cast<std::size_t>(std::stoul(input.substr(begin, headers - begin)));
      const auto body_begin = headers + 4;
      if (input.size() - body_begin < length) break;
      const auto request = nlohmann::json::parse(input.substr(body_begin, length));
      input.erase(0, body_begin + length);
      const auto command = request.value("command", std::string{});
      if (command == "initialize") {
        response(request, {{"supportsConfigurationDoneRequest", true},
          {"supportsSetExpression", true}, {"supportsReadMemoryRequest", true},
          {"supportsDisassembleRequest", true}});
        send({{"seq", 2001}, {"type", "event"}, {"event", "initialized"},
          {"body", nlohmann::json::object()}});
      } else if (command == "configurationDone") {
        response(request);
        send({{"seq", 2002}, {"type", "event"}, {"event", "stopped"},
          {"body", {{"reason", "breakpoint"}, {"threadId", 7}}}});
      } else if (command == "threads") {
        response(request, {{"threads", {{{"id", 7}, {"name", "main"}}}}});
      } else if (command == "stackTrace") {
        response(request, {{"stackFrames", {{{"id", 70}, {"name", "main"}, {"line", 12},
          {"source", {{"path", "/tmp/main.cpp"}}}}}}});
      } else if (command == "scopes") {
        response(request, {{"scopes", {{{"name", "Locals"}, {"variablesReference", 10}},
          {{"name", "Registers"}, {"variablesReference", 11}}}}});
      } else if (command == "variables") {
        const auto reference = request["arguments"].value("variablesReference", 0);
        if (reference == 10)
          response(request, {{"variables", {{{"name", "answer"}, {"value", "42"},
            {"type", "int"}, {"evaluateName", "answer"}, {"variablesReference", 12}}}}});
        else if (reference == 11)
          response(request, {{"variables", {{{"name", "rax"}, {"value", "0x2a"},
            {"variablesReference", 0}}}}});
        else response(request, {{"variables", {{{"name", "nested"}, {"value", "1"},
          {"type", "int"}, {"evaluateName", "answer.nested"}, {"variablesReference", 0}}}}});
      } else if (command == "evaluate") {
        const auto expression = request["arguments"].value("expression", std::string{});
        response(request, {{"result", expression == "answer" ? "42" : "value"},
          {"variablesReference", 0}});
      } else if (command == "setExpression") {
        response(request, {{"value", request["arguments"].value("value", std::string{})},
          {"variablesReference", 0}});
      } else if (command == "readMemory") {
        response(request, {{"address", "0x1000"}, {"data", "KgAAAA=="}});
      } else if (command == "disassemble") {
        response(request, {{"instructions", {{{"address", "0x1000"},
          {"instruction", "ret"}}}}});
      } else if (command == "next") {
        response(request);
        send({{"seq", 2003}, {"type", "event"}, {"event", "stopped"},
          {"body", {{"reason", "step"}, {"threadId", 7}}}});
      } else if (command == "disconnect") {
        response(request);
        return 0;
      } else response(request);
    }
  }
}

template <typename Predicate>
auto waitFor(tuiide::LldbDapClient& client, Predicate predicate) -> bool {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (std::chrono::steady_clock::now() < deadline) {
    client.poll();
    if (predicate()) return true;
    std::this_thread::sleep_for(10ms);
  }
  return false;
}

void expect(bool condition, std::string_view message) {
  if (condition) return;
  std::cerr << "FAIL: " << message << '\n';
  std::exit(1);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1 && std::string_view(argv[1]) == "--adapter") return runAdapter();
  if (std::filesystem::path(argv[0]).filename().string().starts_with("tuiide-fake-lldb-dap-"))
    return runAdapter();
  const auto self = std::filesystem::canonical(argv[0]);
  // Обёртка передаёт обязательный режим fake adapter, сохраняя настоящий DAP transport.
  const auto wrapper = std::filesystem::temp_directory_path()
    / ("tuiide-fake-lldb-dap-" + std::to_string(::getpid()));
  std::filesystem::create_symlink(self, wrapper);
  // LldbDapClient запускает executable без дополнительных аргументов, поэтому
  // fake adapter определяет свою роль по имени символической ссылки в argv[0].

  tuiide::LldbDapClient client;
  client.addBreakpoint("/tmp/main.cpp", 12);
  client.addWatch("answer");
  client.setRegistersEnabled(true);
  expect(client.start(wrapper, self, "/tmp"), "fake LLDB DAP starts");
  expect(waitFor(client, [&] {
    return client.stopped() && !client.frames().empty() && !client.variables().empty()
      && !client.registers().empty() && !client.watches().empty()
      && client.watches()[0].error.empty();
  }), "DAP handshake exposes threads, stack, locals, registers, and watches");
  expect(client.frames()[0].line == 12 && client.variables()[0].value == "42"
      && client.registers()[0].name == "rax", "DAP state is converted to the shared model");
  expect(client.toggleVariable(0), "expandable DAP variable requests children");
  expect(waitFor(client, [&] { return client.variables().size() == 2; }),
    "DAP variable children are inserted into the model");
  expect(client.evaluate("answer") && client.assign("answer", "43")
      && client.readMemory("0x1000", 4) && client.disassemble("0x1000", 4),
    "DAP inspection and mutation requests are accepted while stopped");
  std::vector<tuiide::DebugResult> results;
  expect(waitFor(client, [&] {
    auto next = client.takeResults();
    results.insert(results.end(), next.begin(), next.end());
    return results.size() >= 4;
  }), "DAP evaluate, assignment, memory, and disassembly responses are delivered");
  client.next();
  expect(waitFor(client, [&] { return client.stopped(); }), "DAP next reaches the next stop event");
  expect(client.stop(), "DAP session stops cleanly");
  std::error_code error;
  std::filesystem::remove(wrapper, error);
  std::cout << "LLDB DAP tests passed\n";
  return 0;
}
