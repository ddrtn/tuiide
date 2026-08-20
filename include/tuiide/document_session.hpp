#pragma once

#include "tuiide/document.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tuiide {

struct ClosedDocument {
  std::filesystem::path path;
  Position cursor;
};

struct OpenDocumentResult {
  Document* document{};
  std::size_t index{};
  bool newly_loaded{};
};

class DocumentSession {
 public:
  auto createUntitled() -> OpenDocumentResult;
  auto open(const std::filesystem::path& path, std::string& error) -> std::optional<OpenDocumentResult>;
  auto activate(std::size_t index) -> Document*;
  void closeActive(bool remember = true);
  auto takeLastClosed() -> std::optional<ClosedDocument>;
  void clear();

  [[nodiscard]] auto documents() -> std::vector<std::unique_ptr<Document>>&;
  [[nodiscard]] auto documents() const -> const std::vector<std::unique_ptr<Document>>&;
  [[nodiscard]] auto closedDocuments() -> std::vector<ClosedDocument>&;
  [[nodiscard]] auto activeDocument() -> Document*&;
  [[nodiscard]] auto activeIndex() -> std::size_t&;

 private:
  void forgetClosed(const std::filesystem::path& path);

  std::vector<std::unique_ptr<Document>> documents_;
  std::vector<ClosedDocument> closed_documents_;
  Document* active_document_{};
  std::size_t active_index_{};
};

}  // namespace tuiide
