#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace tuiide {

/** Обнаруженный и проверенный набор инструментов C/C++ для CMake-проекта. */
struct ToolchainKit {
  std::string name;
  std::filesystem::path c_compiler;
  std::filesystem::path cpp_compiler;
  std::filesystem::path debugger;
  std::string debugger_kind;
  std::string generator;
  std::filesystem::path make_program;
  std::filesystem::path sysroot;
  std::filesystem::path toolchain_file;
  std::string compiler_version;
  std::string debugger_version;
  std::string generator_version;
  std::string tool_inventory;
  bool valid{};
};

/** Возвращает первую непустую строку `--version`, очищенную от пробелов. */
[[nodiscard]] auto firstVersionLine(std::string_view output) -> std::string;
/** Находит native GCC/Clang kits и CMake toolchain-файлы проекта. */
[[nodiscard]] auto discoverToolchainKits(std::string_view path_environment,
  const std::filesystem::path& project_root) -> std::vector<ToolchainKit>;
/** Компактное описание для списка выбора в TUI. */
[[nodiscard]] auto describeToolchainKit(const ToolchainKit& kit) -> std::string;

}  // namespace tuiide
