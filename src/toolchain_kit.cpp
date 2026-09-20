#include "tuiide/toolchain_kit.hpp"

#include "tuiide/process.hpp"
#include "tuiide/tool_discovery.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <set>
#include <thread>

namespace tuiide {
namespace {
using namespace std::chrono_literals;

auto probe(const std::filesystem::path& executable,
    std::vector<std::string> arguments) -> std::string {
  if (executable.empty()) return {};
  arguments.insert(arguments.begin(), executable.string());
  AsyncProcess process;
  if (!process.start(arguments)) return {};
  process.closeInput();
  std::string output;
  for (int attempt{}; attempt < 50 && process.running(); ++attempt) {
    for (auto& chunk : process.drain()) output += chunk;
    std::this_thread::sleep_for(10ms);
  }
  for (auto& chunk : process.drain()) output += chunk;
  if (process.running()) process.stop();
  return output;
}

auto executable(std::string_view name, std::string_view path)
    -> std::filesystem::path {
  const auto found = findExecutable(name, path);
  return found ? *found : std::filesystem::path{};
}

auto toolchainFiles(const std::filesystem::path& root)
    -> std::vector<std::filesystem::path> {
  std::vector<std::filesystem::path> result;
  std::error_code error;
  for (const auto& directory : {root, root / "cmake", root / "toolchains"}) {
    if (!std::filesystem::is_directory(directory, error)) { error.clear(); continue; }
    for (std::filesystem::directory_iterator it(directory,
           std::filesystem::directory_options::skip_permission_denied, error), end;
         it != end; it.increment(error)) {
      if (error) { error.clear(); continue; }
      const auto name = it->path().filename().string();
      if (it->is_regular_file(error) && it->path().extension() == ".cmake"
          && name.find("toolchain") != std::string::npos)
        result.push_back(it->path().lexically_normal());
      error.clear();
    }
  }
  std::ranges::sort(result);
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}
}  // namespace

auto firstVersionLine(std::string_view output) -> std::string {
  while (!output.empty() && (output.front() == '\n' || output.front() == '\r'
      || output.front() == ' ' || output.front() == '\t')) output.remove_prefix(1);
  const auto end = output.find_first_of("\r\n");
  auto line = std::string(output.substr(0, end));
  while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.pop_back();
  return line;
}

auto discoverToolchainKits(std::string_view path_environment,
    const std::filesystem::path& project_root) -> std::vector<ToolchainKit> {
  const auto ninja = executable("ninja", path_environment);
  const auto make = executable("make", path_environment);
  const auto gdb = executable("gdb", path_environment);
  const auto lldb = executable("lldb", path_environment);
  auto lldb_dap = executable("lldb-dap", path_environment);
  if (lldb_dap.empty()) lldb_dap = executable("lldb-vscode", path_environment);
  const auto generator = !ninja.empty() ? "Ninja" : (!make.empty() ? "Unix Makefiles" : "");
  const auto build_tool = !ninja.empty() ? ninja : make;
  const auto debugger = !gdb.empty() ? gdb : lldb_dap;
  const std::string debugger_kind = !gdb.empty() ? "GDB" : (!lldb_dap.empty() ? "LLDB" : "");
  const auto debugger_version = firstVersionLine(probe(
    debugger_kind == "LLDB" && !lldb.empty() ? lldb : debugger, {"--version"}));
  const auto generator_version = firstVersionLine(probe(build_tool, {"--version"}));
  std::string inventory;
  const auto inventoryItem = [&](std::string_view name, const std::filesystem::path& path) {
    if (path.empty()) return;
    if (!inventory.empty()) inventory += "; ";
    inventory += std::string(name) + ": " + firstVersionLine(probe(path, {"--version"}));
  };
  inventoryItem("GDB", gdb); inventoryItem("LLDB", lldb); inventoryItem("lldb-dap", lldb_dap);
  inventoryItem("Ninja", ninja); inventoryItem("Make", make);

  std::vector<ToolchainKit> result;
  const auto add = [&](std::string name, std::string_view c_name,
      std::string_view cpp_name) {
    ToolchainKit kit;
    kit.name = std::move(name);
    kit.c_compiler = executable(c_name, path_environment);
    kit.cpp_compiler = executable(cpp_name, path_environment);
    if (kit.c_compiler.empty() || kit.cpp_compiler.empty()) return;
    kit.debugger = debugger; kit.debugger_kind = debugger_kind;
    kit.generator = generator; kit.make_program = build_tool;
    kit.compiler_version = firstVersionLine(probe(kit.cpp_compiler, {"--version"}));
    kit.debugger_version = debugger_version; kit.generator_version = generator_version;
    kit.tool_inventory = inventory;
    kit.sysroot = firstVersionLine(probe(kit.cpp_compiler, {"--print-sysroot"}));
    if (kit.sysroot == "/") kit.sysroot.clear();
    kit.valid = !kit.compiler_version.empty() && !kit.generator.empty();
    result.push_back(std::move(kit));
  };
  add("GCC native", "gcc", "g++");
  add("Clang native", "clang", "clang++");

  for (const auto& file : toolchainFiles(project_root)) {
    ToolchainKit kit;
    kit.name = "CMake: " + file.filename().string();
    kit.toolchain_file = file; kit.generator = generator;
    kit.make_program = build_tool; kit.debugger = debugger;
    kit.debugger_kind = debugger_kind; kit.debugger_version = debugger_version;
    kit.generator_version = generator_version;
    kit.tool_inventory = inventory;
    kit.valid = !kit.generator.empty();
    result.push_back(std::move(kit));
  }
  return result;
}

auto describeToolchainKit(const ToolchainKit& kit) -> std::string {
  auto result = kit.name + " | " + (kit.compiler_version.empty()
    ? (kit.toolchain_file.empty() ? "compiler unavailable" : kit.toolchain_file.string())
    : kit.compiler_version);
  result += " | " + (kit.generator.empty() ? "generator unavailable" : kit.generator);
  if (!kit.debugger_kind.empty()) result += " | " + kit.debugger_kind;
  if (!kit.tool_inventory.empty()) result += " | " + kit.tool_inventory;
  if (!kit.valid) result += " [invalid]";
  return result;
}

}  // namespace tuiide
