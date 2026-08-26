#include "tuiide/cli.hpp"

namespace tuiide {
namespace {
auto takeValue(const std::vector<std::string_view>& arguments, std::size_t& index,
    std::string_view option, std::string& error) -> std::string_view {
  if (index + 1 >= arguments.size()) {
    error = "option " + std::string(option) + " requires a value";
    return {};
  }
  ++index;
  if (arguments[index].empty()) error = "option " + std::string(option) + " requires a non-empty value";
  return arguments[index];
}

auto setProject(CliParseResult& result, std::string_view value) -> bool {
  if (!result.options.project.empty()) {
    result.error = "project path was specified more than once";
    return false;
  }
  result.options.project = value;
  return true;
}
}  // namespace

auto parseCommandLine(const std::vector<std::string_view>& arguments) -> CliParseResult {
  CliParseResult result;
  bool positional_only{};
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    const auto argument = arguments[index];
    if (!positional_only && argument == "--") { positional_only = true; continue; }
    if (!positional_only && (argument == "--help" || argument == "-h")) {
      result.options.show_help = true;
    } else if (!positional_only && argument == "--version") {
      result.options.show_version = true;
    } else if (!positional_only && argument == "--diagnostic") {
      result.options.diagnostic = true;
    } else if (!positional_only && argument == "--project") {
      const auto value = takeValue(arguments, index, argument, result.error);
      if (!result.error.empty() || !setProject(result, value)) return result;
    } else if (!positional_only && argument.starts_with("--project=")) {
      const auto value = argument.substr(std::string_view{"--project="}.size());
      if (value.empty()) { result.error = "option --project requires a non-empty value"; return result; }
      if (!setProject(result, value)) return result;
    } else if (!positional_only && argument == "--log-file") {
      const auto value = takeValue(arguments, index, argument, result.error);
      if (!result.error.empty()) return result;
      result.options.log_file = value;
    } else if (!positional_only && argument.starts_with("--log-file=")) {
      const auto value = argument.substr(std::string_view{"--log-file="}.size());
      if (value.empty()) { result.error = "option --log-file requires a non-empty value"; return result; }
      result.options.log_file = value;
    } else if (!positional_only && argument.starts_with('-')) {
      result.error = "unknown option: " + std::string(argument);
      return result;
    } else if (!setProject(result, argument)) {
      return result;
    }
  }
  return result;
}

auto commandLineHelp(std::string_view program) -> std::string {
  return "Usage: " + std::string(program) + " [OPTIONS] [PROJECT]\n"
    "C/C++ terminal IDE for Linux/amd64.\n\n"
    "Options:\n"
    "  -h, --help           Show this help and exit\n"
    "      --version        Show version and exit\n"
    "      --project PATH   Open the CMake project at PATH\n"
    "      --log-file FILE  Append structured IDE events to FILE\n"
    "      --diagnostic     Report available and missing external tools\n"
    "      --               Treat the remaining argument as a project path\n";
}

}  // namespace tuiide
