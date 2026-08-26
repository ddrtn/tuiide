#include "tuiide/gdb_client.hpp"
#include "tuiide/gdb_mi.hpp"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <iterator>
#include <sstream>

namespace tuiide {
namespace {
auto unsignedField(const MiValue& value, std::string_view name) -> std::size_t {
  try { return static_cast<std::size_t>(std::stoull(value.string(name))); } catch (...) { return 0; }
}
auto intField(const MiValue& value, std::string_view name) -> int {
  try { return std::stoi(value.string(name)); } catch (...) { return 0; }
}
auto quoteMiArgument(std::string_view value) -> std::string {
  std::string result = "\"";
  for (const char character : value) {
    if (character == '\\' || character == '"') result.push_back('\\');
    result.push_back(character);
  }
  return result + '"';
}
auto disassemblyText(const MiRecord& record) -> std::string {
  const auto* instructions = record.result("asm_insns");
  if (!instructions) return {};
  std::ostringstream text;
  for (const auto& instruction : instructions->values) {
    text << instruction.string("address") << "  ";
    const auto function = instruction.string("func-name");
    if (!function.empty()) text << function << '+' << instruction.string("offset") << "  ";
    text << instruction.string("inst") << '\n';
  }
  return text.str();
}
auto memoryText(const MiRecord& record) -> std::string {
  const auto* blocks = record.result("memory");
  if (!blocks) return {};
  std::ostringstream text;
  for (const auto& block : blocks->values) {
    const auto contents = block.string("contents");
    std::uint64_t address{};
    try { address = std::stoull(block.string("begin"), nullptr, 0); } catch (...) {}
    for (std::size_t offset{}; offset + 1 < contents.size(); offset += 32) {
      text << "0x" << std::hex << std::setw(16) << std::setfill('0') << address + offset / 2 << "  ";
      std::string ascii;
      for (std::size_t byte{}; byte < 16; ++byte) {
        const auto position = offset + byte * 2;
        if (position + 1 >= contents.size()) { text << "   "; ascii.push_back(' '); continue; }
        const auto pair = contents.substr(position, 2);
        text << pair << ' ';
        unsigned value{};
        try { value = static_cast<unsigned>(std::stoul(pair, nullptr, 16)); } catch (...) {}
        ascii.push_back(value >= 32 && value < 127 ? static_cast<char>(value) : '.');
      }
      text << " |" << ascii << "|\n";
    }
  }
  return text.str();
}
}

auto gdbEvaluateCommand(std::string_view expression) -> std::string {
  return "-data-evaluate-expression " + quoteMiArgument(expression);
}

auto gdbDisassembleCommand(std::string_view address, std::size_t bytes) -> std::string {
  return "-data-disassemble -s " + quoteMiArgument(address)
    + " -e " + quoteMiArgument("(" + std::string(address) + ") + " + std::to_string(bytes))
    + " -- 0";
}

auto gdbReadMemoryCommand(std::string_view address, std::size_t bytes) -> std::string {
  return "-data-read-memory-bytes " + quoteMiArgument(address) + " " + std::to_string(bytes);
}

auto GdbClient::start(const std::filesystem::path& executable,
    const std::filesystem::path& working_directory,
    const std::map<std::string, std::string>& environment,
    const std::vector<std::string>& program_arguments,
    const std::filesystem::path& stdin_file,
    const std::filesystem::path& inferior_tty) -> bool {
  std::vector<std::string> arguments{"gdb", "--quiet", "--interpreter=mi3", "--args", executable.string()};
  arguments.insert(arguments.end(), program_arguments.begin(), program_arguments.end());
  if (!process_.start(arguments, true, working_directory, environment)) return false;
  inferior_active_ = false; stopped_ = false; inferior_exited_ = false; run_requested_ = false;
  selected_frame_ = 0; ++variable_generation_; pending_variables_.clear(); pending_children_.clear();
  pending_expressions_.clear(); results_.clear();
  stdin_file_ = stdin_file;
  command("-gdb-set pagination off");
  command("-gdb-set print pretty on");
  if (!inferior_tty.empty()) command("-inferior-tty-set " + quote(inferior_tty));
  command("-enable-pretty-printing");
  breakpoint_numbers_.clear();
  pending_breakpoints_.clear(); pending_breakpoint_commands_.clear();
  register_names_.clear(); registers_.clear();
  for (const auto& [key, breakpoint] : desired_breakpoints_) { (void)breakpoint; insertBreakpoint(key); }
  return true;
}
void GdbClient::stop() {
  if (process_.running()) command("-gdb-exit");
  process_.stop(); inferior_active_ = false; stopped_ = false; inferior_exited_ = false; run_requested_ = false;
  breakpoint_numbers_.clear(); pending_breakpoints_.clear(); pending_breakpoint_commands_.clear(); pending_watches_.clear();
  pending_variables_.clear(); pending_children_.clear(); ++variable_generation_; selected_frame_ = 0;
  pending_expressions_.clear(); results_.clear();
  frames_.clear(); variables_.clear(); threads_.clear(); registers_.clear(); register_names_.clear();
  for (auto& [key, breakpoint] : desired_breakpoints_) {
    (void)key; breakpoint.verified = false; breakpoint.error.clear();
  }
  stdin_file_.clear();
  markWatchesUnavailable("debugger not stopped");
}
void GdbClient::clearSessionState() {
  desired_breakpoints_.clear(); watches_.clear(); registers_enabled_ = false;
  output_.clear(); partial_.clear(); results_.clear(); pending_expressions_.clear();
}
void GdbClient::run() {
  inferior_active_ = true; stopped_ = false; inferior_exited_ = false; selected_frame_ = 0;
  frames_.clear(); variables_.clear(); ++variable_generation_; pending_variables_.clear(); pending_children_.clear();
  threads_.clear(); registers_.clear(); pending_watches_.clear(); markWatchesUnavailable("program running");
  run_requested_ = true;
  startRequestedRun();
}
void GdbClient::interrupt() { command("-exec-interrupt"); }
void GdbClient::continueExecution() { inferior_active_ = true; stopped_ = false; selected_frame_ = 0; frames_.clear(); variables_.clear(); ++variable_generation_; pending_variables_.clear(); pending_children_.clear(); threads_.clear(); registers_.clear(); pending_watches_.clear(); markWatchesUnavailable("program running"); command("-exec-continue"); }
void GdbClient::next() { inferior_active_ = true; stopped_ = false; selected_frame_ = 0; frames_.clear(); variables_.clear(); ++variable_generation_; pending_variables_.clear(); pending_children_.clear(); threads_.clear(); registers_.clear(); pending_watches_.clear(); markWatchesUnavailable("program running"); command("-exec-next"); }
void GdbClient::step() { inferior_active_ = true; stopped_ = false; selected_frame_ = 0; frames_.clear(); variables_.clear(); ++variable_generation_; pending_variables_.clear(); pending_children_.clear(); threads_.clear(); registers_.clear(); pending_watches_.clear(); markWatchesUnavailable("program running"); command("-exec-step"); }
void GdbClient::finish() { inferior_active_ = true; stopped_ = false; selected_frame_ = 0; frames_.clear(); variables_.clear(); ++variable_generation_; pending_variables_.clear(); pending_children_.clear(); threads_.clear(); registers_.clear(); pending_watches_.clear(); markWatchesUnavailable("program running"); command("-exec-finish"); }
void GdbClient::selectThread(const std::string& id) {
  if (!stopped_ || id.empty()) return;
  frames_.clear(); variables_.clear(); selected_frame_ = 0; ++variable_generation_;
  command("-thread-select " + id);
  refreshState();
}
auto GdbClient::selectFrame(int level) -> bool {
  if (!stopped_ || level < 0) return false;
  selected_frame_ = level;
  command("-stack-select-frame " + std::to_string(level));
  refreshVariables();
  refreshWatches();
  refreshRegisters();
  return true;
}
auto GdbClient::toggleVariable(std::size_t index) -> bool {
  if (!stopped_ || index >= variables_.size()) return false;
  auto& variable = variables_[index];
  if (!variable.expandable || variable.object.empty()) return false;
  if (variable.expanded) {
    variable.expanded = false;
    const auto depth = variable.depth;
    auto end = index + 1;
    while (end < variables_.size() && variables_[end].depth > depth) ++end;
    variables_.erase(variables_.begin() + static_cast<std::ptrdiff_t>(index + 1),
      variables_.begin() + static_cast<std::ptrdiff_t>(end));
  } else {
    variable.expanded = true;
    pending_children_[command("-var-list-children --all-values " + variable.object)] =
      {variable_generation_, variable.object};
  }
  return true;
}
auto GdbClient::evaluate(std::string expression) -> bool {
  if (!stopped_ || expression.empty()) return false;
  const auto token = command(gdbEvaluateCommand(expression));
  pending_expressions_[token] =
    {DebugResultKind::Evaluation, std::move(expression)};
  return true;
}
auto GdbClient::assign(std::string expression, std::string value) -> bool {
  if (!stopped_ || expression.empty() || value.empty()) return false;
  auto assignment = "(" + expression + ") = (" + value + ")";
  const auto token = command(gdbEvaluateCommand(assignment));
  pending_expressions_[token] =
    {DebugResultKind::Assignment, std::move(expression)};
  return true;
}
auto GdbClient::disassemble(std::string address, std::size_t bytes) -> bool {
  if (!stopped_ || address.empty() || bytes == 0 || bytes > 4096) return false;
  const auto token = command(gdbDisassembleCommand(address, bytes));
  pending_expressions_[token] =
    {DebugResultKind::Disassembly, std::move(address)};
  return true;
}
auto GdbClient::readMemory(std::string address, std::size_t bytes) -> bool {
  if (!stopped_ || address.empty() || bytes == 0 || bytes > 4096) return false;
  const auto token = command(gdbReadMemoryCommand(address, bytes));
  pending_expressions_[token] = {DebugResultKind::Memory, std::move(address)};
  return true;
}
auto GdbClient::addWatch(std::string expression) -> bool {
  if (expression.empty() || std::any_of(watches_.begin(), watches_.end(), [&](const auto& watch) { return watch.expression == expression; })) return false;
  watches_.push_back({std::move(expression), {}, stopped_ ? "pending" : "debugger not stopped"});
  if (stopped_) refreshWatches();
  return true;
}
auto GdbClient::removeWatch(std::size_t index) -> bool {
  if (index >= watches_.size()) return false;
  const auto expression = watches_[index].expression;
  watches_.erase(watches_.begin() + static_cast<std::ptrdiff_t>(index));
  for (auto iterator = pending_watches_.begin(); iterator != pending_watches_.end();) {
    if (iterator->second == expression) iterator = pending_watches_.erase(iterator);
    else ++iterator;
  }
  return true;
}
void GdbClient::setRegistersEnabled(bool enabled) {
  if (registers_enabled_ == enabled) return;
  registers_enabled_ = enabled;
  registers_.clear();
  if (enabled && stopped_) refreshRegisters();
}
auto GdbClient::toggleBreakpoint(const std::filesystem::path& file, std::size_t line) -> bool {
  const auto key = breakpointKey(file, line);
  if (desired_breakpoints_.contains(key)) { removeBreakpoint(file, line); return false; }
  return addBreakpoint(file, line);
}
auto GdbClient::addBreakpoint(const std::filesystem::path& file, std::size_t line) -> bool {
  if (line == 0) return false;
  const auto key = breakpointKey(file, line);
  DebugBreakpoint breakpoint;
  breakpoint.file = std::filesystem::absolute(file).lexically_normal(); breakpoint.line = line;
  if (!desired_breakpoints_.emplace(key, std::move(breakpoint)).second) return false;
  if (process_.running()) insertBreakpoint(key);
  return true;
}
auto GdbClient::updateBreakpoint(const DebugBreakpoint& breakpoint) -> bool {
  if (breakpoint.file.empty() || breakpoint.line == 0) return false;
  const auto key = breakpointKey(breakpoint.file, breakpoint.line);
  const auto found = desired_breakpoints_.find(key);
  if (found == desired_breakpoints_.end()) return false;
  auto updated = breakpoint;
  updated.file = std::filesystem::absolute(updated.file).lexically_normal();
  updated.verified = false; updated.error.clear();
  found->second = std::move(updated);
  if (const auto number = breakpoint_numbers_.find(key); number != breakpoint_numbers_.end()) {
    command("-break-delete " + number->second); breakpoint_numbers_.erase(number);
  }
  if (process_.running()) insertBreakpoint(key);
  return true;
}
auto GdbClient::removeBreakpoint(const std::filesystem::path& file, std::size_t line) -> bool {
  const auto key = breakpointKey(file, line);
  if (desired_breakpoints_.erase(key) == 0) return false;
  if (const auto iterator = breakpoint_numbers_.find(key); iterator != breakpoint_numbers_.end()) {
    command("-break-delete " + iterator->second); breakpoint_numbers_.erase(iterator);
  }
  return true;
}
void GdbClient::clearBreakpoints() {
  if (process_.running() && !breakpoint_numbers_.empty()) command("-break-delete");
  desired_breakpoints_.clear(); breakpoint_numbers_.clear(); pending_breakpoints_.clear();
}
auto GdbClient::hasBreakpoint(const std::filesystem::path& file, std::size_t line) const -> bool { return desired_breakpoints_.contains(breakpointKey(file, line)); }
auto GdbClient::breakpoints() const -> std::vector<DebugBreakpoint> {
  std::vector<DebugBreakpoint> result;
  result.reserve(desired_breakpoints_.size());
  for (const auto& [key, breakpoint] : desired_breakpoints_) { (void)key; result.push_back(breakpoint); }
  std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
    return left.file == right.file ? left.line < right.line : left.file.string() < right.file.string();
  });
  return result;
}
void GdbClient::refreshState() { command("-thread-info"); command("-stack-list-frames"); refreshVariables(); refreshWatches(); refreshRegisters(); }
auto GdbClient::running() const -> bool { return process_.running(); }
auto GdbClient::active() const -> bool { return inferior_active_; }
auto GdbClient::stopped() const -> bool { return stopped_; }
auto GdbClient::exited() const -> bool { return inferior_exited_; }
auto GdbClient::frames() const -> const std::vector<DebugFrame>& { return frames_; }
auto GdbClient::variables() const -> const std::vector<DebugVariable>& { return variables_; }
auto GdbClient::threads() const -> const std::vector<DebugThread>& { return threads_; }
auto GdbClient::watches() const -> const std::vector<DebugWatch>& { return watches_; }
auto GdbClient::registers() const -> const std::vector<DebugRegister>& { return registers_; }
auto GdbClient::registersEnabled() const -> bool { return registers_enabled_; }
auto GdbClient::selectedFrame() const -> int { return selected_frame_; }
auto GdbClient::takeResults() -> std::vector<DebugResult> {
  std::vector<DebugResult> result; result.swap(results_); return result;
}

void GdbClient::poll() {
  for (auto& chunk : process_.drain()) partial_ += chunk;
  std::size_t newline{};
  while ((newline = partial_.find('\n')) != std::string::npos) {
    auto line = partial_.substr(0, newline);
    partial_.erase(0, newline + 1);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line == "(gdb) " || line.empty()) continue;
    const auto mi = parseMiRecord(line);
    if (!mi.valid()) {
      output_.push_back("GDB/MI parse error: " + mi.error + ": " + line);
      continue;
    }
    if (mi.prefix == '~' || mi.prefix == '@' || mi.prefix == '&') {
      if (!mi.stream.empty()) output_.push_back(mi.stream);
      continue;
    }
    if (mi.prefix == '*' && mi.klass == "stopped") {
      const auto reason = mi.string("reason");
      if (reason.starts_with("exited")) {
        inferior_active_ = false; stopped_ = false; inferior_exited_ = true;
        selected_frame_ = 0; frames_.clear(); variables_.clear(); ++variable_generation_;
        pending_variables_.clear(); pending_children_.clear(); threads_.clear(); registers_.clear(); pending_watches_.clear();
        markWatchesUnavailable("program exited");
      } else {
        inferior_active_ = true; stopped_ = true; inferior_exited_ = false; selected_frame_ = 0; refreshState();
      }
    }
    else if (mi.prefix == '*' && mi.klass == "running") {
      inferior_active_ = true; stopped_ = false; inferior_exited_ = false;
    }
    if (mi.token) {
      const auto token = *mi.token;
        if (const auto pending = pending_expressions_.find(token); pending != pending_expressions_.end()) {
          DebugResult result;
          result.kind = pending->second.kind; result.expression = pending->second.expression;
          if (mi.prefix == '^' && mi.klass == "done") {
            if (result.kind == DebugResultKind::Disassembly) result.value = disassemblyText(mi);
            else if (result.kind == DebugResultKind::Memory) result.value = memoryText(mi);
            else result.value = mi.string("value");
            if (result.value.empty()) result.error = "GDB returned no data";
          }
          else {
            result.error = mi.string("msg");
            if (result.error.empty()) result.error = "GDB evaluation failed";
          }
          results_.push_back(std::move(result));
          if (pending->second.kind == DebugResultKind::Assignment && mi.klass == "done") {
            refreshVariables(); refreshWatches();
          }
          pending_expressions_.erase(pending);
        }
        if (const auto pending = pending_breakpoints_.find(token); pending != pending_breakpoints_.end()) {
          if (mi.prefix == '^' && mi.klass == "done") {
            if (const auto* breakpoint = mi.result("bkpt")) {
              const auto number = breakpoint->string("number");
              if (!number.empty()) {
                if (auto desired = desired_breakpoints_.find(pending->second); desired != desired_breakpoints_.end()) {
                  breakpoint_numbers_[pending->second] = number;
                  desired->second.verified = breakpoint->string("addr") != "<PENDING>";
                  desired->second.error = desired->second.verified ? std::string{} : "pending: source location is unresolved";
                  if (!desired->second.log_message.empty()) {
                    auto message = desired->second.log_message;
                    for (std::size_t index{}; (index = message.find('\\', index)) != std::string::npos; index += 2)
                      message.insert(index, "\\");
                    pending_breakpoint_commands_[command("-break-commands " + number + " " + quote("silent") + " "
                      + quote(std::filesystem::path("echo " + message + "\\n")) + " " + quote("continue"))] = pending->second;
                  }
                }
                else command("-break-delete " + number);
              }
            }
          } else if (auto desired = desired_breakpoints_.find(pending->second); desired != desired_breakpoints_.end()) {
            desired->second.verified = false;
            desired->second.error = mi.string("msg");
            if (desired->second.error.empty()) desired->second.error = "GDB rejected breakpoint";
          }
          pending_breakpoints_.erase(pending);
          startRequestedRun();
        }
        if (const auto commands = pending_breakpoint_commands_.find(token);
            commands != pending_breakpoint_commands_.end()) {
          if (mi.klass != "done") {
            if (auto desired = desired_breakpoints_.find(commands->second); desired != desired_breakpoints_.end()) {
              desired->second.verified = false; desired->second.error = mi.string("msg");
              if (desired->second.error.empty()) desired->second.error = "GDB rejected logpoint commands";
            }
          }
          pending_breakpoint_commands_.erase(commands); startRequestedRun();
        }
        if (const auto pending = pending_watches_.find(token); pending != pending_watches_.end()) {
          const auto watch = std::find_if(watches_.begin(), watches_.end(), [&](const auto& item) { return item.expression == pending->second; });
          if (watch != watches_.end()) {
            if (mi.klass == "done") { watch->value = mi.string("value"); watch->error.clear(); }
            else { watch->value.clear(); watch->error = mi.string("msg"); if (watch->error.empty()) watch->error = "evaluation failed"; }
          }
          pending_watches_.erase(pending);
        }
        if (const auto pending = pending_variables_.find(token); pending != pending_variables_.end()) {
          if (pending->second.generation == variable_generation_ && mi.klass == "done") {
            const auto variable = std::find_if(variables_.begin(), variables_.end(), [&](const auto& item) {
              return item.depth == 0 && item.expression == pending->second.expression && item.object.empty();
            });
            if (variable != variables_.end()) {
              variable->object = mi.string("name");
              variable->type = mi.string("type");
              const auto value = mi.string("value");
              if (!value.empty()) variable->value = value;
              variable->expandable = unsignedField(mi.results, "numchild") > 0;
            }
          }
          pending_variables_.erase(pending);
        }
        if (const auto pending = pending_children_.find(token); pending != pending_children_.end()) {
          if (pending->second.generation == variable_generation_ && mi.klass == "done") {
            const auto parent = std::find_if(variables_.begin(), variables_.end(), [&](const auto& item) {
              return item.object == pending->second.object;
            });
            if (parent != variables_.end() && parent->expanded) {
              const auto parent_index = static_cast<std::size_t>(std::distance(variables_.begin(), parent));
              const auto child_depth = parent->depth + 1;
              const auto parent_expression = parent->expression;
              std::vector<DebugVariable> children;
              const auto* list = mi.result("children");
              if (list) for (std::size_t i{}; i < list->values.size(); ++i) {
                if (i < list->names.size() && list->names[i] != "child") continue;
                const auto& record = list->values[i];
                DebugVariable child;
                child.name = record.string("exp");
                if (child.name.empty()) child.name = record.string("name");
                child.value = record.string("value"); child.type = record.string("type");
                child.object = record.string("name");
                child.depth = child_depth;
                child.expression = child.name.starts_with('[')
                  ? parent_expression + child.name : parent_expression + "." + child.name;
                child.expandable = unsignedField(record, "numchild") > 0;
                children.push_back(std::move(child));
              }
              auto descendants_end = parent_index + 1;
              while (descendants_end < variables_.size() && variables_[descendants_end].depth > parent->depth)
                ++descendants_end;
              variables_.erase(variables_.begin() + static_cast<std::ptrdiff_t>(parent_index + 1),
                variables_.begin() + static_cast<std::ptrdiff_t>(descendants_end));
              variables_.insert(variables_.begin() + static_cast<std::ptrdiff_t>(parent_index + 1),
                std::make_move_iterator(children.begin()), std::make_move_iterator(children.end()));
            }
          }
          pending_children_.erase(pending);
        }
    }
    if (mi.prefix == '^' && mi.klass == "error") {
      auto message = mi.string("msg");
      output_.push_back("GDB error: " + (message.empty() ? std::string("command failed") : std::move(message)));
    }
    if (const auto* names = mi.result("register-names")) {
      register_names_.clear();
      for (const auto& value : names->values)
        if (value.kind == MiValueKind::String) register_names_.push_back(value.text);
      refreshRegisters();
    } else if (const auto* values = mi.result("register-values")) {
      registers_.clear();
      for (const auto& value : values->values) {
        const auto number = unsignedField(value, "number");
        if (number < register_names_.size() && !register_names_[number].empty())
          registers_.push_back({register_names_[number], value.string("value")});
      }
    } else if (const auto* values = mi.result("threads")) {
      threads_.clear();
      const auto current_id = mi.string("current-thread-id");
      for (const auto& value : values->values) {
        DebugThread thread;
        thread.id = value.string("id"); thread.name = value.string("name");
        if (thread.name.empty()) thread.name = value.string("target-id");
        thread.state = value.string("state");
        thread.current = thread.id == current_id;
        if (!thread.id.empty()) threads_.push_back(std::move(thread));
      }
    } else if (const auto* values = mi.result("stack")) {
      frames_.clear();
      for (std::size_t i{}; i < values->values.size(); ++i) {
        if (i < values->names.size() && values->names[i] != "frame") continue;
        const auto& value = values->values[i];
        DebugFrame frame;
        frame.level = intField(value, "level"); frame.function = value.string("func");
        frame.file = value.string("fullname"); if (frame.file.empty()) frame.file = value.string("file");
        frame.line = unsignedField(value, "line");
        frames_.push_back(std::move(frame));
      }
    } else if (const auto* values = mi.result("variables")) {
      variables_.clear();
      for (const auto& value : values->values) {
        const auto name = value.string("name");
        if (!name.empty()) {
          DebugVariable variable;
          variable.name = name; variable.expression = name; variable.value = value.string("value");
          variable.type = value.string("type");
          variables_.push_back(std::move(variable));
          pending_variables_[command("-var-create - * " + quote(std::filesystem::path(name)))] =
            {variable_generation_, name};
        }
      }
    }
  }
}
auto GdbClient::takeOutput() -> std::vector<std::string> { std::vector<std::string> result; result.swap(output_); return result; }
auto GdbClient::command(std::string value) -> int {
  const int token = next_token_++;
  process_.write(std::to_string(token) + value + "\n");
  return token;
}
void GdbClient::insertBreakpoint(const std::string& key) {
  const auto found = desired_breakpoints_.find(key);
  if (found == desired_breakpoints_.end()) return;
  auto arguments = std::string("-break-insert");
  if (!found->second.enabled) arguments += " -d";
  if (!found->second.condition.empty()) arguments += " -c " + quote(found->second.condition);
  if (found->second.hit_count != 0) arguments += " -i " + std::to_string(found->second.hit_count);
  arguments += " " + quote(std::filesystem::path(key));
  found->second.verified = false; found->second.error = "pending GDB confirmation";
  const int token = command(std::move(arguments));
  pending_breakpoints_[token] = key;
}
void GdbClient::startRequestedRun() {
  if (!run_requested_ || !pending_breakpoints_.empty() || !pending_breakpoint_commands_.empty()) return;
  run_requested_ = false;
  if (stdin_file_.empty()) command("-exec-run");
  else {
    const auto console_command = std::string("run < ") + quote(stdin_file_);
    command("-interpreter-exec console " + quote(std::filesystem::path(console_command)));
  }
}
void GdbClient::refreshWatches() {
  if (!stopped_) return;
  for (auto& watch : watches_) {
    watch.value.clear(); watch.error = "pending";
    pending_watches_[command("-data-evaluate-expression " + quote(std::filesystem::path(watch.expression)))] = watch.expression;
  }
}
void GdbClient::refreshRegisters() {
  if (!registers_enabled_ || !stopped_) return;
  if (register_names_.empty()) { command("-data-list-register-names"); return; }
  static const std::unordered_set<std::string> general{
    "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp",
    "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15", "rip", "eflags", "cs", "ss"
  };
  std::string request = "-data-list-register-values x";
  for (std::size_t i = 0; i < register_names_.size(); ++i) {
    if (general.contains(register_names_[i])) request += " " + std::to_string(i);
  }
  command(std::move(request));
}
void GdbClient::refreshVariables() {
  for (const auto& variable : variables_)
    if (variable.depth == 0 && !variable.object.empty()) command("-var-delete " + variable.object);
  variables_.clear();
  ++variable_generation_;
  command("-stack-list-variables --all-values");
}
void GdbClient::markWatchesUnavailable(std::string message) {
  for (auto& watch : watches_) { watch.value.clear(); watch.error = message; }
}
auto GdbClient::breakpointKey(const std::filesystem::path& file, std::size_t line) -> std::string {
  return std::filesystem::absolute(file).lexically_normal().string() + ":" + std::to_string(line);
}
auto GdbClient::quote(const std::filesystem::path& value) -> std::string {
  return quoteMiArgument(value.string());
}

}  // namespace tuiide
