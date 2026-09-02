#include "tuiide/cmake_presets.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <regex>
#include <unordered_map>
#include <unordered_set>

namespace tuiide {
namespace {
using Json = nlohmann::json;
struct RawPreset {
  Json value;
  std::filesystem::path source_file;
};
struct RawPresets {
  std::unordered_map<std::string, RawPreset> configure;
  std::unordered_map<std::string, RawPreset> build;
};

auto readJson(const std::filesystem::path& path) -> Json {
  std::ifstream input(path);
  if (!input) return {};
  return Json::parse(input, nullptr, false);
}

struct JsonOverride {
  std::filesystem::path path;
  const Json* value{};
};

auto collectFile(const std::filesystem::path& path, RawPresets& presets,
  std::unordered_set<std::filesystem::path>& visited, std::string& error,
  const std::optional<JsonOverride>& override = std::nullopt) -> bool {
  const auto absolute = std::filesystem::absolute(path).lexically_normal();
  if (!std::filesystem::exists(path) && (!override || override->path != absolute)) return true;
  if (!visited.insert(absolute).second) return true;
  const auto json = override && override->path == absolute ? *override->value : readJson(absolute);
  if (!json.is_object()) { error = "invalid CMake presets file: " + absolute.string(); return false; }
  auto includes = json.value("include", Json::array());
  if (includes.is_string()) includes = Json::array({includes});
  if (!includes.is_array()) { error = "invalid preset include list: " + absolute.string(); return false; }
  for (const auto& include : includes) {
    if (!include.is_string() || !collectFile(absolute.parent_path() / include.get<std::string>(),
        presets, visited, error, override)) return false;
  }
  const auto collect = [&](const char* key, auto& destination, const char* kind) {
    for (const auto& preset : json.value(key, Json::array())) {
      if (!preset.is_object() || !preset.contains("name") || !preset["name"].is_string()) continue;
      const auto name = preset["name"].get<std::string>();
      if (destination.contains(name)) { error = "duplicate CMake " + std::string(kind) + " preset: " + name; return false; }
      destination.emplace(name, RawPreset{preset, absolute});
    }
    return true;
  };
  return collect("configurePresets", presets.configure, "configure")
    && collect("buildPresets", presets.build, "build");
}

auto resolvePreset(const std::string& name, const std::unordered_map<std::string, RawPreset>& presets,
  std::unordered_map<std::string, Json>& resolved, std::unordered_set<std::string>& resolving, std::string& error) -> Json {
  if (const auto found = resolved.find(name); found != resolved.end()) return found->second;
  const auto raw = presets.find(name);
  if (raw == presets.end()) { error = "unknown inherited CMake preset: " + name; return {}; }
  if (!resolving.insert(name).second) { error = "cyclic CMake preset inheritance at: " + name; return {}; }
  Json result = Json::object();
  auto parents = raw->second.value.value("inherits", Json::array());
  if (parents.is_string()) parents = Json::array({parents});
  if (!parents.is_array()) { error = "invalid inherits list for preset: " + name; return {}; }
  for (const auto& parent : parents) {
    if (!parent.is_string()) continue;
    const auto inherited = resolvePreset(parent.get<std::string>(), presets, resolved, resolving, error);
    if (!error.empty()) return {};
    for (auto iterator = inherited.begin(); iterator != inherited.end(); ++iterator) {
      const auto& key = iterator.key();
      if (key == "name" || key == "hidden" || key == "displayName" || key == "description"
          || key == "inherits") continue;
      if (!result.contains(key)) result[key] = iterator.value();
    }
  }
  for (auto iterator = raw->second.value.begin(); iterator != raw->second.value.end(); ++iterator)
    result[iterator.key()] = iterator.value();
  resolving.erase(name);
  resolved[name] = result;
  return result;
}

void replaceAll(std::string& text, std::string_view marker, std::string_view value) {
  std::size_t position{};
  while ((position = text.find(marker, position)) != std::string::npos) {
    text.replace(position, marker.size(), value);
    position += value.size();
  }
}

auto expandString(std::string value, const std::filesystem::path& source, const std::string& preset_name,
  const std::string& generator) -> std::string {
  replaceAll(value, "${sourceDir}", source.string());
  replaceAll(value, "${sourceParentDir}", source.parent_path().string());
  replaceAll(value, "${sourceDirName}", source.filename().string());
  replaceAll(value, "${presetName}", preset_name);
  replaceAll(value, "${generator}", generator);
  replaceAll(value, "${hostSystemName}", "Linux");
  for (const auto& prefix : {std::string("$env{"), std::string("$penv{")}) {
    std::size_t position{};
    while ((position = value.find(prefix, position)) != std::string::npos) {
      const auto end = value.find('}', position + prefix.size());
      if (end == std::string::npos) break;
      const auto name = value.substr(position + prefix.size(), end - position - prefix.size());
      const char* environment = std::getenv(name.c_str());
      value.replace(position, end - position + 1, environment ? environment : "");
    }
  }
  return value;
}

auto expand(std::string value, const std::filesystem::path& source, const std::string& preset_name,
  const std::string& generator) -> std::filesystem::path {
  auto result = std::filesystem::path(expandString(std::move(value), source, preset_name, generator));
  if (result.is_relative()) result = source / result;
  return std::filesystem::absolute(result).lexically_normal();
}

auto conditionMatches(const Json& condition, const std::filesystem::path& source, const std::string& preset_name,
  const std::string& generator) -> bool {
  if (condition.is_null()) return true;
  if (condition.is_boolean()) return condition.get<bool>();
  if (!condition.is_object()) return false;
  const auto type = condition.value("type", std::string{});
  const auto expanded = [&](const char* field) {
    return expandString(condition.value(field, std::string{}), source, preset_name, generator);
  };
  if (type == "const") return condition.value("value", false);
  if (type == "equals" || type == "notEquals") {
    const auto equal = expanded("lhs") == expanded("rhs");
    return type == "equals" ? equal : !equal;
  }
  if (type == "inList" || type == "notInList") {
    const auto needle = expanded("string");
    bool found{};
    for (const auto& item : condition.value("list", Json::array())) {
      if (item.is_string() && expandString(item.get<std::string>(), source, preset_name, generator) == needle) {
        found = true;
        break;
      }
    }
    return type == "inList" ? found : !found;
  }
  if (type == "matches" || type == "notMatches") {
    bool matches{};
    try { matches = std::regex_search(expanded("string"), std::regex(expanded("regex"))); }
    catch (const std::regex_error&) { return false; }
    return type == "matches" ? matches : !matches;
  }
  if (type == "anyOf" || type == "allOf") {
    const auto conditions = condition.value("conditions", Json::array());
    if (!conditions.is_array()) return false;
    if (type == "anyOf") return std::any_of(conditions.begin(), conditions.end(), [&](const auto& item) {
      return conditionMatches(item, source, preset_name, generator);
    });
    return std::all_of(conditions.begin(), conditions.end(), [&](const auto& item) {
      return conditionMatches(item, source, preset_name, generator);
    });
  }
  if (type == "not" && condition.contains("condition"))
    return !conditionMatches(condition["condition"], source, preset_name, generator);
  return false;
}

auto collectPresets(const std::filesystem::path& source, RawPresets& raw, std::string& error,
    const std::optional<JsonOverride>& override = std::nullopt) -> bool {
  std::unordered_set<std::filesystem::path> visited;
  return collectFile(source / "CMakePresets.json", raw, visited, error, override)
    && collectFile(source / "CMakeUserPresets.json", raw, visited, error, override);
}

auto values(const Json& value) -> std::vector<std::string> {
  auto items = value;
  if (items.is_string()) items = Json::array({items});
  std::vector<std::string> result;
  if (!items.is_array()) return result;
  for (const auto& item : items)
    if (item.is_string()) result.push_back(item.get<std::string>());
  return result;
}

void setOptionalString(Json& value, const char* key, const std::string& text) {
  if (text.empty()) value.erase(key);
  else value[key] = text;
}

void setStringList(Json& value, const char* key, const std::vector<std::string>& items) {
  if (items.empty()) value.erase(key);
  else if (items.size() == 1) value[key] = items.front();
  else value[key] = items;
}

auto validatePresets(const std::filesystem::path& source, const Json& user,
    std::string& error) -> bool {
  RawPresets raw;
  const auto user_path = std::filesystem::absolute(source / "CMakeUserPresets.json").lexically_normal();
  if (!collectPresets(source, raw, error, JsonOverride{user_path, &user})) return false;
  std::unordered_map<std::string, Json> configure_resolved;
  for (const auto& [name, value] : raw.configure) {
    (void)value;
    std::unordered_set<std::string> resolving;
    (void)resolvePreset(name, raw.configure, configure_resolved, resolving, error);
    if (!error.empty()) return false;
  }
  std::unordered_map<std::string, Json> build_resolved;
  for (const auto& [name, value] : raw.build) {
    (void)value;
    std::unordered_set<std::string> resolving;
    const auto resolved = resolvePreset(name, raw.build, build_resolved, resolving, error);
    if (!error.empty()) return false;
    const auto configure = resolved.value("configurePreset", std::string{});
    if (!configure.empty() && !raw.configure.contains(configure)) {
      error = "build preset " + name + " references unknown configure preset: " + configure;
      return false;
    }
  }
  return true;
}

auto writeUserPresets(const std::filesystem::path& source, const Json& value,
    std::string& error) -> bool {
  if (!validatePresets(source, value, error)) return false;
  const auto destination = source / "CMakeUserPresets.json";
  auto temporary = destination;
  temporary += ".tuiide.tmp";
  {
    std::ofstream output(temporary, std::ios::trunc);
    if (!output) { error = "cannot write temporary presets file: " + temporary.string(); return false; }
    output << value.dump(2) << '\n';
    if (!output) { error = "cannot finish writing presets file: " + temporary.string(); return false; }
  }
  std::error_code filesystem_error;
  std::filesystem::rename(temporary, destination, filesystem_error);
  if (filesystem_error) {
    std::filesystem::remove(temporary, filesystem_error);
    error = "cannot atomically install " + destination.string();
    return false;
  }
  error.clear();
  return true;
}

auto userDocument(const std::filesystem::path& source, std::string& error) -> Json {
  const auto path = source / "CMakeUserPresets.json";
  if (!std::filesystem::exists(path)) {
    int version = 3;
    const auto project = readJson(source / "CMakePresets.json");
    if (project.is_object() && project.contains("version") && project["version"].is_number_integer())
      version = std::max(2, project["version"].get<int>());
    return Json{{"version", version}};
  }
  auto value = readJson(path);
  if (!value.is_object()) { error = "invalid CMake presets file: " + path.string(); return {}; }
  if (!value.contains("version")) value["version"] = 3;
  return value;
}

auto presetArrayKey(CMakePresetKind kind) -> const char* {
  return kind == CMakePresetKind::Configure ? "configurePresets" : "buildPresets";
}
}

auto loadCMakeConfigurePresets(const std::filesystem::path& source_directory, std::string& error)
  -> std::vector<CMakeConfigurePreset> {
  error.clear();
  const auto source = std::filesystem::absolute(source_directory).lexically_normal();
  RawPresets raw;
  if (!collectPresets(source, raw, error)) return {};
  std::unordered_map<std::string, Json> resolved;
  std::vector<CMakeConfigurePreset> result;
  for (const auto& [name, preset] : raw.configure) {
    (void)preset;
    std::unordered_set<std::string> resolving;
    const auto value = resolvePreset(name, raw.configure, resolved, resolving, error);
    if (!error.empty()) return {};
    if (value.value("hidden", false)) continue;
    const auto generator = value.value("generator", std::string{});
    if (value.contains("condition") && !conditionMatches(value["condition"], source, name, generator)) continue;
    const auto binary = value.value("binaryDir", std::string{});
    const auto user_file = std::filesystem::absolute(source / "CMakeUserPresets.json").lexically_normal();
    result.push_back({name, value.value("displayName", name), generator,
      binary.empty() ? std::filesystem::path{} : expand(binary, source, name, generator),
      preset.source_file, preset.source_file == user_file});
  }
  std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) { return left.name < right.name; });
  return result;
}

auto loadCMakeBuildPresets(const std::filesystem::path& source_directory, std::string& error)
  -> std::vector<CMakeBuildPreset> {
  error.clear();
  const auto source = std::filesystem::absolute(source_directory).lexically_normal();
  RawPresets raw;
  if (!collectPresets(source, raw, error)) return {};
  std::unordered_map<std::string, Json> resolved;
  std::vector<CMakeBuildPreset> result;
  for (const auto& [name, preset] : raw.build) {
    (void)preset;
    std::unordered_set<std::string> resolving;
    const auto value = resolvePreset(name, raw.build, resolved, resolving, error);
    if (!error.empty()) return {};
    if (value.value("hidden", false)) continue;
    if (value.contains("condition") && !conditionMatches(value["condition"], source, name, {})) continue;
    auto targets_value = value.value("targets", Json::array());
    if (targets_value.is_string()) targets_value = Json::array({targets_value});
    std::vector<std::string> targets;
    if (targets_value.is_array()) {
      for (const auto& target : targets_value)
        if (target.is_string()) targets.push_back(expandString(target.get<std::string>(), source, name, {}));
    }
    const auto user_file = std::filesystem::absolute(source / "CMakeUserPresets.json").lexically_normal();
    result.push_back({name, value.value("displayName", name), value.value("configurePreset", std::string{}),
      value.value("configuration", std::string{}), std::move(targets), value.value("cleanFirst", false),
      value.value("verbose", false), preset.source_file, preset.source_file == user_file});
  }
  std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) { return left.name < right.name; });
  return result;
}

auto loadCMakePresetForEdit(const std::filesystem::path& source_directory,
    CMakePresetKind kind, const std::string& name, CMakePresetEdit& preset,
    std::string& error) -> bool {
  error.clear();
  const auto source = std::filesystem::absolute(source_directory).lexically_normal();
  RawPresets raw;
  if (!collectPresets(source, raw, error)) return false;
  const auto& items = kind == CMakePresetKind::Configure ? raw.configure : raw.build;
  const auto found = items.find(name);
  if (found == items.end()) { error = "unknown CMake preset: " + name; return false; }
  const auto& value = found->second.value;
  preset = {};
  preset.kind = kind;
  preset.name = name;
  preset.display_name = value.value("displayName", std::string{});
  preset.inherits = values(value.value("inherits", Json::array()));
  preset.generator = value.value("generator", std::string{});
  preset.binary_directory = value.value("binaryDir", std::string{});
  preset.configure_preset = value.value("configurePreset", std::string{});
  preset.configuration = value.value("configuration", std::string{});
  preset.targets = values(value.value("targets", Json::array()));
  preset.clean_first = value.value("cleanFirst", false);
  preset.verbose = value.value("verbose", false);
  preset.hidden = value.value("hidden", false);
  preset.source_file = found->second.source_file;
  const auto user_file = std::filesystem::absolute(source / "CMakeUserPresets.json").lexically_normal();
  preset.user_editable = preset.source_file == user_file;
  return true;
}

auto loadCMakePresetsForEdit(const std::filesystem::path& source_directory,
    CMakePresetKind kind, std::string& error) -> std::vector<CMakePresetEdit> {
  error.clear();
  const auto source = std::filesystem::absolute(source_directory).lexically_normal();
  RawPresets raw;
  if (!collectPresets(source, raw, error)) return {};
  const auto& items = kind == CMakePresetKind::Configure ? raw.configure : raw.build;
  std::vector<CMakePresetEdit> result;
  result.reserve(items.size());
  for (const auto& [name, value] : items) {
    (void)value;
    CMakePresetEdit preset;
    if (!loadCMakePresetForEdit(source, kind, name, preset, error)) return {};
    result.push_back(std::move(preset));
  }
  std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
    return left.name < right.name;
  });
  return result;
}

auto saveCMakeUserPreset(const std::filesystem::path& source_directory,
    const std::string& original_name, const CMakePresetEdit& preset,
    std::string& error) -> bool {
  error.clear();
  const auto source = std::filesystem::absolute(source_directory).lexically_normal();
  if (preset.name.empty()) { error = "preset name is required"; return false; }
  if (preset.name.find_first_of("/\\") != std::string::npos) {
    error = "preset name must not contain a path separator"; return false;
  }
  auto user = userDocument(source, error);
  if (!error.empty()) return false;
  const auto key = presetArrayKey(preset.kind);
  if (!user.contains(key)) user[key] = Json::array();
  if (!user[key].is_array()) { error = std::string("invalid ") + key + " list"; return false; }

  RawPresets raw;
  if (!collectPresets(source, raw, error)) return false;
  auto& all = preset.kind == CMakePresetKind::Configure ? raw.configure : raw.build;
  if (const auto duplicate = all.find(preset.name);
      duplicate != all.end() && preset.name != original_name) {
    error = "CMake preset already exists: " + preset.name;
    return false;
  }

  Json value = Json::object();
  auto position = user[key].end();
  if (!original_name.empty()) {
    const auto existing = all.find(original_name);
    const auto user_file = std::filesystem::absolute(source / "CMakeUserPresets.json").lexically_normal();
    if (existing == all.end()) { error = "unknown CMake preset: " + original_name; return false; }
    if (existing->second.source_file != user_file) {
      error = "project and included presets are read-only; clone the preset first";
      return false;
    }
    position = std::find_if(user[key].begin(), user[key].end(), [&](const auto& item) {
      return item.is_object() && item.value("name", std::string{}) == original_name;
    });
    if (position == user[key].end()) { error = "user preset is missing from its source file"; return false; }
    value = *position;
  }
  value["name"] = preset.name;
  setOptionalString(value, "displayName", preset.display_name);
  setStringList(value, "inherits", preset.inherits);
  if (preset.kind == CMakePresetKind::Configure) {
    setOptionalString(value, "generator", preset.generator);
    setOptionalString(value, "binaryDir", preset.binary_directory);
  } else {
    setOptionalString(value, "configurePreset", preset.configure_preset);
    setOptionalString(value, "configuration", preset.configuration);
    setStringList(value, "targets", preset.targets);
    if (preset.clean_first) value["cleanFirst"] = true; else value.erase("cleanFirst");
    if (preset.verbose) value["verbose"] = true; else value.erase("verbose");
  }
  if (position == user[key].end()) user[key].push_back(std::move(value));
  else *position = std::move(value);
  return writeUserPresets(source, user, error);
}

auto cloneCMakePresetToUser(const std::filesystem::path& source_directory,
    CMakePresetKind kind, const std::string& source_name, const std::string& new_name,
    std::string& error) -> bool {
  error.clear();
  const auto source = std::filesystem::absolute(source_directory).lexically_normal();
  if (new_name.empty()) { error = "preset name is required"; return false; }
  RawPresets raw;
  if (!collectPresets(source, raw, error)) return false;
  const auto& all = kind == CMakePresetKind::Configure ? raw.configure : raw.build;
  const auto found = all.find(source_name);
  if (found == all.end()) { error = "unknown CMake preset: " + source_name; return false; }
  if (all.contains(new_name)) { error = "CMake preset already exists: " + new_name; return false; }
  auto user = userDocument(source, error);
  if (!error.empty()) return false;
  const auto key = presetArrayKey(kind);
  if (!user.contains(key)) user[key] = Json::array();
  if (!user[key].is_array()) { error = std::string("invalid ") + key + " list"; return false; }
  auto clone = found->second.value;
  clone["name"] = new_name;
  user[key].push_back(std::move(clone));
  return writeUserPresets(source, user, error);
}

auto deleteCMakeUserPreset(const std::filesystem::path& source_directory,
    CMakePresetKind kind, const std::string& name, std::string& error) -> bool {
  error.clear();
  const auto source = std::filesystem::absolute(source_directory).lexically_normal();
  CMakePresetEdit preset;
  if (!loadCMakePresetForEdit(source, kind, name, preset, error)) return false;
  if (!preset.user_editable) {
    error = "project and included presets are read-only";
    return false;
  }
  auto user = userDocument(source, error);
  if (!error.empty()) return false;
  const auto key = presetArrayKey(kind);
  if (!user.contains(key) || !user[key].is_array()) { error = "user preset list is invalid"; return false; }
  const auto position = std::find_if(user[key].begin(), user[key].end(), [&](const auto& item) {
    return item.is_object() && item.value("name", std::string{}) == name;
  });
  if (position == user[key].end()) { error = "user preset is missing from its source file"; return false; }
  user[key].erase(position);
  return writeUserPresets(source, user, error);
}

}  // namespace tuiide
