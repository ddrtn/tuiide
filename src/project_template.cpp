#include "tuiide/project_template.hpp"

#include "tuiide/cmake_source_edit.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <initializer_list>
#include <system_error>

namespace tuiide {
namespace {

auto normalized(const std::filesystem::path& path) -> std::filesystem::path {
  std::error_code error;
  auto value = std::filesystem::weakly_canonical(path, error);
  return error ? path.lexically_normal() : value;
}

auto validRelativePath(const std::filesystem::path& path) -> bool {
  if (path.empty() || path.is_absolute()) return false;
  return std::none_of(path.begin(), path.end(), [](const auto& component) { return component == ".."; });
}

auto withExtension(std::filesystem::path path, std::string_view extension) -> std::filesystem::path {
  if (path.extension() != extension) path.replace_extension(extension);
  return path;
}

auto includeGuard(const std::filesystem::path& path) -> std::string {
  auto guard = path.generic_string();
  std::transform(guard.begin(), guard.end(), guard.begin(), [](unsigned char value) {
    return std::isalnum(value) ? static_cast<char>(std::toupper(value)) : '_';
  });
  return "TUIIDE_" + guard + "_";
}

auto className(std::string value) -> std::string {
  std::string result;
  bool capitalize = true;
  for (const unsigned char value_char : value) {
    if (!std::isalnum(value_char)) { capitalize = true; continue; }
    auto character = static_cast<char>(value_char);
    if (result.empty() && std::isdigit(value_char)) result.push_back('_');
    if (capitalize) character = static_cast<char>(std::toupper(value_char));
    result.push_back(character);
    capitalize = false;
  }
  return result.empty() ? "NewClass" : result;
}

auto validIdentifier(std::string_view value) -> bool {
  if (value.empty() || (!std::isalpha(static_cast<unsigned char>(value.front())) && value.front() != '_')) return false;
  return std::all_of(value.begin() + 1, value.end(), [](unsigned char character) {
    return std::isalnum(character) || character == '_';
  });
}

auto validNamespace(std::string_view value) -> bool {
  if (value.empty()) return true;
  std::size_t position{};
  while (position < value.size()) {
    const auto separator = value.find("::", position);
    const auto part = value.substr(position, separator == std::string_view::npos ? value.size() - position : separator - position);
    if (!validIdentifier(part)) return false;
    if (separator == std::string_view::npos) return true;
    position = separator + 2;
  }
  return false;
}

auto safeBaseClass(std::string_view value) -> bool {
  return value.find_first_of("\r\n{};") == std::string_view::npos;
}

auto safeInclude(std::string_view value) -> bool {
  return value.find_first_of("\r\n") == std::string_view::npos;
}

auto validFileName(std::string_view value, std::initializer_list<std::string_view> extensions) -> bool {
  const std::filesystem::path path(value);
  if (value.empty() || path != path.filename() || path == "." || path == ".."
      || value.find_first_of("\r\n") != std::string_view::npos) return false;
  const auto extension = path.extension().string();
  return std::find(extensions.begin(), extensions.end(), extension) != extensions.end();
}

auto accessText(InheritanceAccess access) -> std::string_view {
  switch (access) {
    case InheritanceAccess::Public: return "public";
    case InheritanceAccess::Protected: return "protected";
    case InheritanceAccess::Private: return "private";
  }
  return "public";
}

struct GeneratedFile {
  std::filesystem::path relative;
  std::string content;
};

auto generate(ProjectTemplate type, std::filesystem::path path) -> std::vector<GeneratedFile> {
  switch (type) {
    case ProjectTemplate::CHeader: {
      path = withExtension(std::move(path), ".h");
      const auto guard = includeGuard(path);
      return {{path, "#ifndef " + guard + "\n#define " + guard + "\n\n#endif  // " + guard + "\n"}};
    }
    case ProjectTemplate::CppHeader:
      path = withExtension(std::move(path), ".hpp");
      return {{path, "#pragma once\n"}};
    case ProjectTemplate::CSource:
      path = withExtension(std::move(path), ".c");
      return {{path, "/* Implementation file. */\n"}};
    case ProjectTemplate::CppSource:
      path = withExtension(std::move(path), ".cpp");
      return {{path, "// Implementation file.\n"}};
    case ProjectTemplate::CppClass: {
      path.replace_extension();
      auto header = path; header += ".hpp";
      auto source = path; source += ".cpp";
      const auto name = className(path.filename().string());
      return {
        {header, "#pragma once\n\nclass " + name + " {\n public:\n  " + name + "() = default;\n};\n"},
        {source, "#include \"" + header.filename().string() + "\"\n"}
      };
    }
  }
  return {};
}

auto generateClass(const CppClassOptions& options) -> std::vector<GeneratedFile> {
  std::string declaration = "class " + options.class_name;
  if (options.final_class) declaration += " final";
  if (!options.base_class.empty()) declaration += " : " + std::string(accessText(options.inheritance)) + " " + options.base_class;
  declaration += " {\n public:\n";
  if (options.generate_constructor) declaration += "  " + options.class_name + "();\n";
  if (options.generate_destructor) {
    declaration += "  ";
    if (options.virtual_destructor) declaration += "virtual ";
    declaration += "~" + options.class_name + "();\n";
  }
  if (options.generate_copy_operations) {
    declaration += "  " + options.class_name + "(const " + options.class_name + "&) = default;\n";
    declaration += "  auto operator=(const " + options.class_name + "&) -> " + options.class_name + "& = default;\n";
  }
  if (options.generate_move_operations) {
    declaration += "  " + options.class_name + "(" + options.class_name + "&&) noexcept = default;\n";
    declaration += "  auto operator=(" + options.class_name + "&&) noexcept -> " + options.class_name + "& = default;\n";
  }
  declaration += "};\n";

  std::string header = "#pragma once\n";
  if (!options.base_header.empty()) {
    header += "\n#include ";
    if (options.base_header.front() == '<' || options.base_header.front() == '"') header += options.base_header;
    else header += "\"" + options.base_header + "\"";
    header += "\n";
  }
  header += "\n";
  if (!options.namespace_name.empty()) header += "namespace " + options.namespace_name + " {\n\n";
  header += declaration;
  if (!options.namespace_name.empty()) header += "\n}  // namespace " + options.namespace_name + "\n";

  auto include_path = options.header_path.lexically_relative(options.source_path.parent_path());
  if (include_path.empty()) include_path = options.header_path.filename();
  std::string source = "#include \"" + include_path.generic_string() + "\"\n";
  if (options.generate_constructor || options.generate_destructor) source += "\n";
  if (!options.namespace_name.empty() && (options.generate_constructor || options.generate_destructor))
    source += "namespace " + options.namespace_name + " {\n\n";
  if (options.generate_constructor) source += options.class_name + "::" + options.class_name + "() = default;\n";
  if (options.generate_destructor) source += options.class_name + "::~" + options.class_name + "() = default;\n";
  if (!options.namespace_name.empty() && (options.generate_constructor || options.generate_destructor))
    source += "\n}  // namespace " + options.namespace_name + "\n";
  return {{options.header_path, std::move(header)}, {options.source_path, std::move(source)}};
}

auto createGeneratedFiles(const std::filesystem::path& project_root,
                          const std::vector<GeneratedFile>& generated,
                          std::string_view preferred_target,
                          ProjectTemplateResult& result,
                          std::string& error) -> bool {
  result = {};
  error.clear();
  const auto root = normalized(project_root);
  std::vector<std::filesystem::path> absolute_paths;
  absolute_paths.reserve(generated.size());
  for (const auto& file : generated) {
    if (!validRelativePath(file.relative)) {
      error = "Enter paths relative to the project without '..'.";
      return false;
    }
    const auto absolute = (root / file.relative).lexically_normal();
    if (std::filesystem::exists(absolute)) {
      error = "File already exists: " + absolute.string();
      return false;
    }
    absolute_paths.push_back(absolute);
  }

  for (std::size_t index = 0; index < generated.size(); ++index) {
    std::error_code directory_error;
    std::filesystem::create_directories(absolute_paths[index].parent_path(), directory_error);
    if (directory_error) { error = "Cannot create directory: " + directory_error.message(); break; }
    std::ofstream output(absolute_paths[index], std::ios::binary);
    if (!output || !output.write(generated[index].content.data(),
        static_cast<std::streamsize>(generated[index].content.size()))) {
      error = "Cannot create file: " + absolute_paths[index].string();
      output.close();
      std::filesystem::remove(absolute_paths[index], directory_error);
      break;
    }
    result.created_files.push_back(absolute_paths[index]);
  }
  if (!error.empty()) {
    std::error_code cleanup_error;
    for (const auto& path : result.created_files) std::filesystem::remove(path, cleanup_error);
    result = {};
    return false;
  }

  CMakeSourceAddition addition;
  if (!addCMakeSourceReferences(root, absolute_paths, preferred_target, addition, error)) {
    std::error_code cleanup_error;
    for (const auto& path : result.created_files) std::filesystem::remove(path, cleanup_error);
    result = {};
    return false;
  }
  result.changed_cmake_file = addition.changed_file;
  result.cmake_references_added = addition.references_added;
  return true;
}

}  // namespace

auto createProjectTemplate(const std::filesystem::path& project_root,
                           ProjectTemplate type,
                           const std::filesystem::path& relative_path,
                           std::string_view preferred_target,
                           ProjectTemplateResult& result,
                           std::string& error) -> bool {
  result = {};
  error.clear();
  if (!validRelativePath(relative_path)) {
    error = "Enter a non-empty path relative to the project without '..'.";
    return false;
  }
  const auto generated = generate(type, relative_path);
  return createGeneratedFiles(project_root, generated, preferred_target, result, error);
}

auto createCppClassTemplate(const std::filesystem::path& project_root,
                            const CppClassOptions& options,
                            std::string_view preferred_target,
                            ProjectTemplateResult& result,
                            std::string& error) -> bool {
  if (!validateCppClassSettings(options, error)) return false;
  if (!validRelativePath(options.header_path) || !validRelativePath(options.source_path)) {
    error = "Header and source paths must stay inside the project.";
    return false;
  }
  if (options.header_path == options.source_path) { error = "Header and source paths must be different."; return false; }
  return createGeneratedFiles(project_root, generateClass(options), preferred_target, result, error);
}

auto validateCppClassSettings(const CppClassOptions& options, std::string& error) -> bool {
  error.clear();
  if (!validIdentifier(options.class_name)) { error = "Class name must be a valid C++ identifier."; return false; }
  if (!validNamespace(options.namespace_name)) { error = "Namespace must contain valid identifiers separated by '::'."; return false; }
  if (!safeBaseClass(options.base_class)) { error = "Base class contains unsupported characters."; return false; }
  if (!safeInclude(options.base_header)) { error = "Base header contains unsupported characters."; return false; }
  if (!options.header_file_name.empty()
      && !validFileName(options.header_file_name, {".h", ".hpp"})) {
    error = "Header file name must be one .h or .hpp file without a directory."; return false;
  }
  if (!options.source_file_name.empty()
      && !validFileName(options.source_file_name, {".cpp"})) {
    error = "Implementation file name must be one .cpp file without a directory."; return false;
  }
  return true;
}

}  // namespace tuiide
