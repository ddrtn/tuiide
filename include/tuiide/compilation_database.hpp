#pragma once

#include <filesystem>
#include <string>
#include <unordered_set>

namespace tuiide {

class CompilationDatabase {
 public:
  auto load(const std::filesystem::path& build_directory, std::string& error) -> bool;
  void clear();

  [[nodiscard]] auto available() const -> bool;
  [[nodiscard]] auto contains(const std::filesystem::path& source) const -> bool;
  [[nodiscard]] auto size() const -> std::size_t;
  [[nodiscard]] auto path() const -> const std::filesystem::path&;

 private:
  std::filesystem::path path_;
  std::unordered_set<std::filesystem::path> sources_;
  bool available_{};
};

}  // namespace tuiide
