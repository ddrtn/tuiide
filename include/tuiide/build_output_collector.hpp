#pragma once

#include "tuiide/build_diagnostic.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tuiide {

struct BuildOutputUpdate {
  std::optional<unsigned> progress;
  std::size_t diagnostics_added{};
};

class BuildOutputCollector {
 public:
  [[nodiscard]] auto append(std::string_view chunk,
    const std::filesystem::path& project_root) -> BuildOutputUpdate;
  [[nodiscard]] auto finish(const std::filesystem::path& project_root) -> BuildOutputUpdate;
  void reset();
  void clearDiagnostics() { diagnostics_.clear(); }

  [[nodiscard]] auto diagnostics() const noexcept -> const std::vector<BuildDiagnostic>& {
    return diagnostics_;
  }
  [[nodiscard]] auto hasPartialLine() const noexcept -> bool { return !partial_.empty(); }

 private:
  void consumeLine(std::string_view line, const std::filesystem::path& project_root,
    BuildOutputUpdate& update);

  std::string partial_;
  std::vector<BuildDiagnostic> diagnostics_;
};

}  // namespace tuiide
