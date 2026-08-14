#pragma once

#include "tuiide/document.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace tuiide {

struct RecoveryDocument {
  std::filesystem::path path;
  std::string text;
  Position cursor;
};

auto saveRecovery(const std::filesystem::path& file,
  const std::vector<RecoveryDocument>& documents, std::string& error) -> bool;
auto loadRecovery(const std::filesystem::path& file,
  std::vector<RecoveryDocument>& documents, std::string& error) -> bool;
void clearRecovery(const std::filesystem::path& file);

}  // namespace tuiide
