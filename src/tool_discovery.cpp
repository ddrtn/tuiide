#include "tuiide/tool_discovery.hpp"

#include <cstdlib>
#include <unistd.h>

namespace tuiide {

auto findExecutable(std::string_view name, std::string_view path_environment)
    -> std::optional<std::filesystem::path> {
  if (name.empty() || name.find('/') != std::string_view::npos) return std::nullopt;
  std::size_t begin{};
  while (begin <= path_environment.size()) {
    const auto end = path_environment.find(':', begin);
    const auto component = path_environment.substr(begin,
      end == std::string_view::npos ? path_environment.size() - begin : end - begin);
    const auto directory = component.empty() ? std::filesystem::path{"."}
                                             : std::filesystem::path{component};
    const auto candidate = directory / std::string(name);
    std::error_code error;
    if (std::filesystem::is_regular_file(candidate, error)
        && ::access(candidate.c_str(), X_OK) == 0) {
      return std::filesystem::absolute(candidate, error).lexically_normal();
    }
    if (end == std::string_view::npos) break;
    begin = end + 1;
  }
  return std::nullopt;
}

auto discoverExternalTools() -> ExternalTools {
  const std::string path = std::getenv("PATH") ? std::getenv("PATH") : "";
  ExternalTools tools;
  tools.cmake = findExecutable("cmake", path);
  tools.clangd = findExecutable("clangd", path);
  tools.gdb = findExecutable("gdb", path);
  tools.lldb_dap = findExecutable("lldb-dap", path);
  if (!tools.lldb_dap) tools.lldb_dap = findExecutable("lldb-vscode", path);
  tools.clang_tidy = findExecutable("clang-tidy", path);
  tools.cppcheck = findExecutable("cppcheck", path);
  tools.include_what_you_use = findExecutable("iwyu_tool.py", path);
  if (!tools.include_what_you_use)
    tools.include_what_you_use = findExecutable("iwyu_tool", path);
  tools.gcovr = findExecutable("gcovr", path);
  tools.valgrind = findExecutable("valgrind", path);
  tools.perf = findExecutable("perf", path);
  for (const auto* name : {"wl-copy", "wl-paste", "xclip", "xsel"})
    tools.clipboard.push_back({name, findExecutable(name, path), "native system clipboard"});
  return tools;
}

auto externalToolMessages(const ExternalTools& tools, bool include_available)
    -> std::vector<std::string> {
  std::vector<std::string> messages;
  const auto add = [&](std::string_view name, const auto& executable, std::string_view missing) {
    if (executable && include_available)
      messages.push_back("Diagnostic: " + std::string(name) + " found at " + executable->string());
    else if (!executable) messages.push_back(std::string(missing));
  };
  add("CMake", tools.cmake,
    "CMake unavailable: install cmake or add it to PATH; configure and build are disabled.");
  add("clangd", tools.clangd,
    "clangd unavailable: install clangd or add it to PATH; C/C++ completion and code navigation are disabled.");
  add("GDB", tools.gdb,
    "GDB unavailable: install gdb or add it to PATH; debugging is disabled.");
  add("lldb-dap", tools.lldb_dap,
    "lldb-dap unavailable: the optional LLDB debug backend is disabled.");
  add("clang-tidy", tools.clang_tidy,
    "clang-tidy unavailable: install it or add it to PATH; clang-tidy analysis is disabled.");
  add("cppcheck", tools.cppcheck,
    "cppcheck unavailable: install it or add it to PATH; cppcheck analysis is disabled.");
  add("include-what-you-use", tools.include_what_you_use,
    "include-what-you-use unavailable: install iwyu_tool.py; IWYU analysis is disabled.");
  add("gcovr", tools.gcovr,
    "gcovr unavailable: coverage reports are disabled.");
  add("Valgrind", tools.valgrind,
    "Valgrind unavailable: memory profiling is disabled.");
  add("perf", tools.perf,
    "perf unavailable: CPU profiling is disabled.");

  bool wayland_copy{};
  bool wayland_paste{};
  bool x11{};
  for (const auto& helper : tools.clipboard) {
    if (helper.name == "wl-copy") wayland_copy = helper.executable.has_value();
    else if (helper.name == "wl-paste") wayland_paste = helper.executable.has_value();
    else if (helper.executable) x11 = true;
    if (include_available && helper.executable)
      messages.push_back("Diagnostic: clipboard helper " + helper.name + " found at " + helper.executable->string());
  }
  if (!(wayland_copy && wayland_paste) && !x11) {
    messages.push_back("Native clipboard helpers unavailable: install wl-clipboard, xclip, or xsel; the internal and OSC 52 fallbacks remain available.");
  } else if (include_available) {
    messages.push_back("Diagnostic: native clipboard integration is available.");
  }
  return messages;
}

}  // namespace tuiide
