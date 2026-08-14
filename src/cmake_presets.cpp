#include "tuiide/cmake_presets.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>
#include <regex>
#include <unordered_map>
#include <unordered_set>

namespace tuiide {
namespace {
using Json = nlohmann::json;
struct RawPreset { Json value; };
struct RawPresets {
  std::unordered_map<std::string, RawPreset> configure;
  std::unordered_map<std::string, RawPreset> build;
};

auto readJson(const std::filesystem::path& path) -> Json {
  std::ifstream input(path);
  if (!input) return {};
  return Json::parse(input, nullptr, false);
}

auto collectFile(const std::filesystem::path& path, RawPresets& presets,
  std::unordered_set<std::filesystem::path>& visited, std::string& error) -> bool {
  if (!std::filesystem::exists(path)) return true;
  const auto absolute = std::filesystem::absolute(path).lexically_normal();
  if (!visited.insert(absolute).second) return true;
  const auto json = readJson(absolute);
  if (!json.is_object()) { error = "invalid CMake presets file: " + absolute.string(); return false; }
  auto includes = json.value("include", Json::array());
  if (includes.is_string()) includes = Json::array({includes});
  if (!includes.is_array()) { error = "invalid preset include list: " + absolute.string(); return false; }
  for (const auto& include : includes) {
    if (!include.is_string() || !collectFile(absolute.parent_path() / include.get<std::string>(), presets, visited, error)) return false;
  }
  const auto collect = [&](const char* key, auto& destination, const char* kind) {
    for (const auto& preset : json.value(key, Json::array())) {
      if (!preset.is_object() || !preset.contains("name") || !preset["name"].is_string()) continue;
      const auto name = preset["name"].get<std::string>();
      if (destination.contains(name)) { error = "duplicate CMake " + std::string(kind) + " preset: " + name; return false; }
      destination.emplace(name, RawPreset{preset});
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

auto collectPresets(const std::filesystem::path& source, RawPresets& raw, std::string& error) -> bool {
  std::unordered_set<std::filesystem::path> visited;
  return collectFile(source / "CMakePresets.json", raw, visited, error)
    && collectFile(source / "CMakeUserPresets.json", raw, visited, error);
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
    result.push_back({name, value.value("displayName", name), generator,
      binary.empty() ? std::filesystem::path{} : expand(binary, source, name, generator)});
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
    result.push_back({name, value.value("displayName", name), value.value("configurePreset", std::string{}),
      value.value("configuration", std::string{}), std::move(targets), value.value("cleanFirst", false),
      value.value("verbose", false)});
  }
  std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) { return left.name < right.name; });
  return result;
}

}  // namespace tuiide
