#include "tuiide/lldb_dap_client.hpp"

#include "tuiide/document.hpp"

#include <algorithm>
#include <charconv>
#include <utility>

namespace tuiide {
namespace {

auto text(const nlohmann::json& value, std::string_view key) -> std::string {
  const auto item = value.find(key);
  return item != value.end() && item->is_string() ? item->get<std::string>() : std::string{};
}

auto number(const nlohmann::json& value, std::string_view key, int fallback = 0) -> int {
  const auto item = value.find(key);
  return item != value.end() && item->is_number_integer() ? item->get<int>() : fallback;
}

auto flag(const nlohmann::json& value, std::string_view key, bool fallback = false) -> bool {
  const auto item = value.find(key);
  return item != value.end() && item->is_boolean() ? item->get<bool>() : fallback;
}

auto positiveId(std::string_view value) -> int {
  int result{};
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
  return parsed.ec == std::errc{} && result > 0 ? result : 0;
}

}  // namespace

LldbDapClient::~LldbDapClient() { (void)stop(); }

auto LldbDapClient::start(const std::filesystem::path& adapter,
    const std::filesystem::path& executable,
    const std::filesystem::path& working_directory,
    const std::map<std::string, std::string>& environment,
    const std::vector<std::string>& arguments,
    const std::filesystem::path& inferior_tty) -> bool {
  std::error_code error;
  if (process_.running() || adapter.empty() || executable.empty()
      || !std::filesystem::is_regular_file(adapter, error)
      || !std::filesystem::is_regular_file(executable, error)
      || !process_.start({adapter.string()}, false, working_directory)) return false;
  input_.clear(); output_.clear(); pending_.clear(); results_.clear();
  frames_.clear(); frame_ids_.clear(); variables_.clear(); threads_.clear(); registers_.clear();
  next_sequence_ = 1; initialized_ = false; launch_sent_ = false; configured_ = false;
  active_ = false; stopped_ = false; exited_ = false;
  selected_thread_ = 0; selected_frame_id_ = 0; selected_frame_ = 0;
  executable_ = executable; working_directory_ = working_directory;
  environment_ = environment; arguments_ = arguments; inferior_tty_ = inferior_tty;
  request("initialize", {{"clientID", "tuiide"}, {"clientName", "TUI IDE"},
    {"adapterID", "lldb"}, {"pathFormat", "path"}, {"linesStartAt1", true},
    {"columnsStartAt1", true}, {"supportsVariableType", true},
    {"supportsMemoryReferences", true}});
  return true;
}

auto LldbDapClient::stop() -> bool {
  if (process_.running()) {
    request("disconnect", {{"terminateDebuggee", active_}});
    poll();
  }
  process_.stop();
  active_ = false; stopped_ = false; exited_ = false;
  initialized_ = false; launch_sent_ = false; configured_ = false;
  pending_.clear(); input_.clear();
  return true;
}

void LldbDapClient::clearSessionState() {
  (void)stop();
  breakpoints_.clear(); watches_.clear(); registers_enabled_ = false;
  output_.clear(); results_.clear(); frames_.clear(); frame_ids_.clear();
  variables_.clear(); threads_.clear(); registers_.clear();
}

void LldbDapClient::run() {
  if (!process_.running()) return;
  if (stopped_) continueExecution();
}

void LldbDapClient::interrupt() {
  if (active_ && !stopped_) request("pause", {{"threadId", selected_thread_}});
}

void LldbDapClient::continueExecution() {
  if (!active_ || !stopped_) return;
  request("continue", {{"threadId", selected_thread_}});
  stopped_ = false; frames_.clear(); frame_ids_.clear(); variables_.clear(); registers_.clear();
  markWatchesUnavailable("program running");
}

void LldbDapClient::next() {
  if (!active_ || !stopped_) return;
  request("next", {{"threadId", selected_thread_}, {"granularity", "statement"}});
  stopped_ = false;
}

void LldbDapClient::step() {
  if (!active_ || !stopped_) return;
  request("stepIn", {{"threadId", selected_thread_}, {"granularity", "statement"}});
  stopped_ = false;
}

void LldbDapClient::finish() {
  if (!active_ || !stopped_) return;
  request("stepOut", {{"threadId", selected_thread_}, {"granularity", "statement"}});
  stopped_ = false;
}

void LldbDapClient::selectThread(const std::string& id) {
  const auto parsed = positiveId(id);
  if (!stopped_ || parsed == 0) return;
  selected_thread_ = parsed; selected_frame_ = 0; selected_frame_id_ = 0;
  requestFrames();
}

auto LldbDapClient::selectFrame(int level) -> bool {
  if (!stopped_ || level < 0 || static_cast<std::size_t>(level) >= frame_ids_.size()) return false;
  selected_frame_ = level; selected_frame_id_ = frame_ids_[static_cast<std::size_t>(level)];
  variables_.clear(); registers_.clear(); requestScopes(); refreshWatches();
  return true;
}

auto LldbDapClient::toggleVariable(std::size_t index) -> bool {
  if (!stopped_ || index >= variables_.size() || !variables_[index].expandable) return false;
  auto& variable = variables_[index];
  if (variable.expanded) {
    const auto depth = variable.depth;
    variable.expanded = false;
    auto end = index + 1;
    while (end < variables_.size() && variables_[end].depth > depth) ++end;
    variables_.erase(variables_.begin() + static_cast<std::ptrdiff_t>(index + 1),
      variables_.begin() + static_cast<std::ptrdiff_t>(end));
    return true;
  }
  const auto reference = positiveId(variable.object);
  if (reference == 0) return false;
  variable.expanded = true;
  request("variables", {{"variablesReference", reference}}, "children:" + std::to_string(index));
  return true;
}

auto LldbDapClient::evaluate(std::string expression) -> bool {
  if (!stopped_ || expression.empty()) return false;
  request("evaluate", {{"expression", expression}, {"frameId", selected_frame_id_},
    {"context", "repl"}}, "evaluate:" + expression);
  return true;
}

auto LldbDapClient::assign(std::string expression, std::string value) -> bool {
  if (!stopped_ || expression.empty() || value.empty()) return false;
  request("setExpression", {{"expression", expression}, {"value", value},
    {"frameId", selected_frame_id_}}, "assign:" + expression);
  return true;
}

auto LldbDapClient::disassemble(std::string address, std::size_t bytes) -> bool {
  if (!stopped_ || address.empty() || bytes == 0) return false;
  request("disassemble", {{"memoryReference", address}, {"instructionOffset", 0},
    {"instructionCount", std::max<std::size_t>(1, bytes / 4)}, {"resolveSymbols", true}},
    "disassemble:" + address);
  return true;
}

auto LldbDapClient::readMemory(std::string address, std::size_t bytes) -> bool {
  if (!stopped_ || address.empty() || bytes == 0) return false;
  request("readMemory", {{"memoryReference", address}, {"offset", 0}, {"count", bytes}},
    "memory:" + address);
  return true;
}

auto LldbDapClient::addWatch(std::string expression) -> bool {
  if (expression.empty() || std::any_of(watches_.begin(), watches_.end(),
      [&expression](const auto& item) { return item.expression == expression; })) return false;
  watches_.push_back({std::move(expression), {}, stopped_ ? "pending" : "debugger not stopped"});
  if (stopped_) refreshWatches();
  return true;
}

auto LldbDapClient::removeWatch(std::size_t index) -> bool {
  if (index >= watches_.size()) return false;
  watches_.erase(watches_.begin() + static_cast<std::ptrdiff_t>(index));
  return true;
}

void LldbDapClient::setRegistersEnabled(bool enabled) {
  registers_enabled_ = enabled;
  if (!enabled) registers_.clear();
  else if (stopped_) requestScopes();
}

auto LldbDapClient::toggleBreakpoint(const std::filesystem::path& file,
    std::size_t line) -> bool {
  return hasBreakpoint(file, line) ? removeBreakpoint(file, line) : addBreakpoint(file, line);
}

auto LldbDapClient::addBreakpoint(const std::filesystem::path& file,
    std::size_t line) -> bool {
  if (file.empty() || line == 0) return false;
  DebugBreakpoint breakpoint{.file = normalizePath(file), .line = line};
  if (!breakpoints_.emplace(breakpointKey(file, line), std::move(breakpoint)).second) return false;
  if (configured_) configureBreakpoints();
  return true;
}

auto LldbDapClient::updateBreakpoint(const DebugBreakpoint& breakpoint) -> bool {
  const auto key = breakpointKey(breakpoint.file, breakpoint.line);
  const auto found = breakpoints_.find(key);
  if (found == breakpoints_.end()) return false;
  found->second = breakpoint; found->second.file = normalizePath(breakpoint.file);
  if (configured_) configureBreakpoints();
  return true;
}

auto LldbDapClient::removeBreakpoint(const std::filesystem::path& file,
    std::size_t line) -> bool {
  if (breakpoints_.erase(breakpointKey(file, line)) == 0) return false;
  if (configured_) configureBreakpoints();
  return true;
}

void LldbDapClient::clearBreakpoints() {
  breakpoints_.clear();
  if (configured_) configureBreakpoints();
}

auto LldbDapClient::hasBreakpoint(const std::filesystem::path& file,
    std::size_t line) const -> bool {
  return breakpoints_.contains(breakpointKey(file, line));
}

auto LldbDapClient::breakpoints() const -> std::vector<DebugBreakpoint> {
  std::vector<DebugBreakpoint> result;
  result.reserve(breakpoints_.size());
  for (const auto& [key, breakpoint] : breakpoints_) { (void)key; result.push_back(breakpoint); }
  std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
    return left.file == right.file ? left.line < right.line : left.file < right.file;
  });
  return result;
}

void LldbDapClient::refreshState() {
  if (stopped_) request("threads");
}

void LldbDapClient::poll() {
  for (auto& chunk : process_.drain()) input_ += chunk;
  for (;;) {
    const auto header_end = input_.find("\r\n\r\n");
    if (header_end == std::string::npos) break;
    const auto marker = input_.find("Content-Length:");
    if (marker == std::string::npos || marker > header_end) {
      input_.erase(0, header_end + 4); continue;
    }
    const auto begin = marker + 15;
    std::size_t length{};
    const auto parsed = std::from_chars(input_.data() + begin, input_.data() + header_end, length);
    if (parsed.ec != std::errc{}) { input_.erase(0, header_end + 4); continue; }
    const auto body = header_end + 4;
    if (input_.size() - body < length) break;
    try { handleMessage(nlohmann::json::parse(input_.substr(body, length))); }
    catch (const nlohmann::json::exception& exception) {
      output_.push_back("LLDB DAP protocol error: " + std::string(exception.what()));
    }
    input_.erase(0, body + length);
  }
  if (!process_.running() && (active_ || launch_sent_)) {
    active_ = false; stopped_ = false; exited_ = true;
  }
}

auto LldbDapClient::running() const -> bool { return process_.running(); }
auto LldbDapClient::active() const -> bool { return active_; }
auto LldbDapClient::stopped() const -> bool { return stopped_; }
auto LldbDapClient::exited() const -> bool { return exited_; }
auto LldbDapClient::frames() const -> const std::vector<DebugFrame>& { return frames_; }
auto LldbDapClient::variables() const -> const std::vector<DebugVariable>& { return variables_; }
auto LldbDapClient::threads() const -> const std::vector<DebugThread>& { return threads_; }
auto LldbDapClient::watches() const -> const std::vector<DebugWatch>& { return watches_; }
auto LldbDapClient::registers() const -> const std::vector<DebugRegister>& { return registers_; }
auto LldbDapClient::registersEnabled() const -> bool { return registers_enabled_; }
auto LldbDapClient::selectedFrame() const -> int { return selected_frame_; }

auto LldbDapClient::takeResults() -> std::vector<DebugResult> {
  std::vector<DebugResult> result; result.swap(results_); return result;
}

auto LldbDapClient::takeOutput() -> std::vector<std::string> {
  std::vector<std::string> result; result.swap(output_); return result;
}

auto LldbDapClient::request(std::string command, nlohmann::json arguments) -> int {
  return request(std::move(command), std::move(arguments), {});
}

auto LldbDapClient::request(std::string command, nlohmann::json arguments,
    std::string context) -> int {
  const auto sequence = next_sequence_++;
  const nlohmann::json message{{"seq", sequence}, {"type", "request"},
    {"command", command}, {"arguments", std::move(arguments)}};
  const auto body = message.dump();
  if (!process_.write("Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body))
    return 0;
  pending_.emplace(sequence, PendingRequest{std::move(command), std::move(context)});
  return sequence;
}

void LldbDapClient::handleMessage(const nlohmann::json& message) {
  const auto type = text(message, "type");
  if (type == "response") handleResponse(message);
  else if (type == "event") handleEvent(message);
}

void LldbDapClient::handleResponse(const nlohmann::json& message) {
  const auto request_sequence = number(message, "request_seq");
  const auto pending = pending_.find(request_sequence);
  if (pending == pending_.end()) return;
  const auto command = pending->second.command;
  const auto context = pending->second.context;
  pending_.erase(pending);
  const auto success = flag(message, "success");
  const auto body_it = message.find("body");
  const auto body = body_it != message.end() && body_it->is_object()
    ? *body_it : nlohmann::json::object();
  if (!success) {
    const auto error = text(message, "message").empty() ? "request failed" : text(message, "message");
    output_.push_back("LLDB DAP " + command + " error: " + error);
    if (context.starts_with("evaluate:") || context.starts_with("assign:")
        || context.starts_with("memory:") || context.starts_with("disassemble:")) {
      auto kind = DebugResultKind::Evaluation;
      if (context.starts_with("assign:")) kind = DebugResultKind::Assignment;
      else if (context.starts_with("memory:")) kind = DebugResultKind::Memory;
      else if (context.starts_with("disassemble:")) kind = DebugResultKind::Disassembly;
      results_.push_back({kind, context.substr(context.find(':') + 1), {}, error});
    }
    return;
  }
  if (command == "initialize") { initialized_ = true; sendLaunch(); return; }
  if (command == "threads") {
    threads_.clear();
    const auto list = body.find("threads");
    if (list != body.end() && list->is_array()) for (const auto& item : *list) {
      const auto id = number(item, "id");
      threads_.push_back({std::to_string(id), text(item, "name"), stopped_ ? "stopped" : "running",
        id == selected_thread_});
      if (selected_thread_ == 0) selected_thread_ = id;
    }
    for (auto& thread : threads_) thread.current = positiveId(thread.id) == selected_thread_;
    requestFrames(); return;
  }
  if (command == "stackTrace") {
    frames_.clear(); frame_ids_.clear();
    const auto list = body.find("stackFrames");
    if (list != body.end() && list->is_array()) for (std::size_t index = 0; index < list->size(); ++index) {
      const auto& item = (*list)[index];
      std::filesystem::path file;
      const auto source = item.find("source");
      if (source != item.end() && source->is_object()) file = text(*source, "path");
      frames_.push_back({static_cast<int>(index), text(item, "name"), file,
        static_cast<std::size_t>(std::max(0, number(item, "line")))});
      frame_ids_.push_back(number(item, "id"));
    }
    selected_frame_ = 0;
    selected_frame_id_ = frame_ids_.empty() ? 0 : frame_ids_.front();
    requestScopes(); refreshWatches(); return;
  }
  if (command == "scopes") {
    const auto scopes = body.find("scopes");
    if (scopes != body.end() && scopes->is_array()) for (const auto& scope : *scopes) {
      const auto name = text(scope, "name");
      const auto reference = number(scope, "variablesReference");
      if (reference <= 0) continue;
      if (name == "Registers" || name == "Register") {
        if (registers_enabled_) request("variables", {{"variablesReference", reference}}, "registers");
      } else if (name == "Locals" || name == "Local") {
        request("variables", {{"variablesReference", reference}}, "locals");
      }
    }
    return;
  }
  if (command == "variables") {
    const auto list = body.find("variables");
    if (list == body.end() || !list->is_array()) return;
    if (context == "registers") {
      registers_.clear();
      for (const auto& item : *list) registers_.push_back({text(item, "name"), text(item, "value")});
    } else {
      std::vector<DebugVariable> parsed;
      for (const auto& item : *list) {
        const auto reference = number(item, "variablesReference");
        parsed.push_back({text(item, "name"), text(item, "value"), text(item, "type"),
          text(item, "evaluateName"), reference > 0 ? std::to_string(reference) : std::string{},
          0, reference > 0, false});
      }
      if (context == "locals") variables_ = std::move(parsed);
      else if (context.starts_with("children:")) {
        const auto parent = positiveId(context.substr(9));
        if (parent < variables_.size()) {
          const auto depth = variables_[parent].depth + 1;
          for (auto& item : parsed) item.depth = depth;
          variables_.insert(variables_.begin() + static_cast<std::ptrdiff_t>(parent + 1),
            std::make_move_iterator(parsed.begin()), std::make_move_iterator(parsed.end()));
        }
      }
    }
    return;
  }
  if (command == "evaluate") {
    const auto expression = context.substr(context.find(':') + 1);
    if (context.starts_with("watch:")) {
      const auto found = std::find_if(watches_.begin(), watches_.end(), [&expression](const auto& item) {
        return item.expression == expression;
      });
      if (found != watches_.end()) { found->value = text(body, "result"); found->error.clear(); }
    } else results_.push_back({DebugResultKind::Evaluation, expression, text(body, "result"), {}});
    return;
  }
  if (command == "setExpression") {
    const auto expression = context.substr(context.find(':') + 1);
    results_.push_back({DebugResultKind::Assignment, expression, text(body, "value"), {}});
    refreshState(); return;
  }
  if (command == "readMemory") {
    results_.push_back({DebugResultKind::Memory, context.substr(context.find(':') + 1),
      text(body, "data"), {}}); return;
  }
  if (command == "disassemble") {
    std::string value;
    const auto instructions = body.find("instructions");
    if (instructions != body.end() && instructions->is_array()) for (const auto& item : *instructions)
      value += text(item, "address") + "  " + text(item, "instruction") + "\n";
    results_.push_back({DebugResultKind::Disassembly,
      context.substr(context.find(':') + 1), std::move(value), {}});
  }
}

void LldbDapClient::handleEvent(const nlohmann::json& message) {
  const auto event = text(message, "event");
  const auto body_it = message.find("body");
  const auto body = body_it != message.end() && body_it->is_object()
    ? *body_it : nlohmann::json::object();
  if (event == "initialized") { configureBreakpoints(); configured_ = true; request("configurationDone"); }
  else if (event == "stopped") {
    active_ = true; stopped_ = true; exited_ = false;
    selected_thread_ = number(body, "threadId", selected_thread_);
    refreshState();
  } else if (event == "continued") {
    active_ = true; stopped_ = false;
  } else if (event == "terminated" || event == "exited") {
    active_ = false; stopped_ = false; exited_ = true;
  } else if (event == "output") {
    auto value = text(body, "output");
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) value.pop_back();
    if (!value.empty()) output_.push_back(std::move(value));
  }
}

void LldbDapClient::sendLaunch() {
  if (!initialized_ || launch_sent_) return;
  nlohmann::json environment = nlohmann::json::object();
  for (const auto& [name, value] : environment_) environment[name] = value;
  nlohmann::json launch{{"program", executable_.string()}, {"args", arguments_},
    {"cwd", working_directory_.string()}, {"env", std::move(environment)}, {"stopOnEntry", false}};
  if (!inferior_tty_.empty()) launch["stdio"] = nlohmann::json::array(
    {inferior_tty_.string(), inferior_tty_.string(), inferior_tty_.string()});
  launch_sent_ = request("launch", std::move(launch)) != 0;
}

void LldbDapClient::configureBreakpoints() {
  std::map<std::filesystem::path, std::vector<DebugBreakpoint*>> groups;
  for (auto& [key, breakpoint] : breakpoints_) { (void)key; groups[breakpoint.file].push_back(&breakpoint); }
  for (auto& [file, items] : groups) {
    auto values = nlohmann::json::array();
    for (auto* item : items) if (item->enabled) {
      nlohmann::json breakpoint{{"line", item->line}};
      if (!item->condition.empty()) breakpoint["condition"] = item->condition;
      if (item->hit_count != 0) breakpoint["hitCondition"] = std::to_string(item->hit_count);
      if (!item->log_message.empty()) breakpoint["logMessage"] = item->log_message;
      values.push_back(std::move(breakpoint));
      item->verified = false; item->error.clear();
    }
    request("setBreakpoints", {{"source", {{"path", file.string()}}},
      {"breakpoints", std::move(values)}, {"sourceModified", false}});
  }
}

void LldbDapClient::requestFrames() {
  if (selected_thread_ > 0)
    request("stackTrace", {{"threadId", selected_thread_}, {"startFrame", 0}, {"levels", 100}});
}

void LldbDapClient::requestScopes() {
  if (selected_frame_id_ > 0) request("scopes", {{"frameId", selected_frame_id_}});
}

void LldbDapClient::refreshWatches() {
  if (!stopped_ || selected_frame_id_ <= 0) return;
  for (auto& watch : watches_) {
    watch.value.clear(); watch.error = "pending";
    request("evaluate", {{"expression", watch.expression}, {"frameId", selected_frame_id_},
      {"context", "watch"}}, "watch:" + watch.expression);
  }
}

void LldbDapClient::markWatchesUnavailable(std::string message) {
  for (auto& watch : watches_) { watch.value.clear(); watch.error = message; }
}

auto LldbDapClient::breakpointKey(const std::filesystem::path& file,
    std::size_t line) -> std::string {
  return normalizePath(file).string() + ':' + std::to_string(line);
}

}  // namespace tuiide
