#include "tuiide/document_session.hpp"

#include <algorithm>

namespace tuiide {

auto DocumentSession::createUntitled() -> OpenDocumentResult {
  documents_.push_back(std::make_unique<Document>());
  active_index_ = documents_.size() - 1;
  active_document_ = documents_.back().get();
  return {active_document_, active_index_, true};
}

auto DocumentSession::open(const std::filesystem::path& path, std::string& error)
    -> std::optional<OpenDocumentResult> {
  const auto normalized = normalizePath(path);
  for (std::size_t index = 0; index < documents_.size(); ++index) {
    if (documents_[index]->path() != normalized) continue;
    forgetClosed(normalized);
    active_index_ = index; active_document_ = documents_[index].get(); error.clear();
    return OpenDocumentResult{active_document_, active_index_, false};
  }
  auto document = std::make_unique<Document>();
  if (!document->load(normalized, error)) return std::nullopt;
  forgetClosed(normalized);
  documents_.push_back(std::move(document));
  active_index_ = documents_.size() - 1; active_document_ = documents_.back().get();
  return OpenDocumentResult{active_document_, active_index_, true};
}

auto DocumentSession::activate(std::size_t index) -> Document* {
  if (index >= documents_.size()) return nullptr;
  active_index_ = index; active_document_ = documents_[index].get();
  return active_document_;
}

void DocumentSession::closeActive(bool remember) {
  if (!active_document_ || active_index_ >= documents_.size()) return;
  if (remember && !active_document_->path().empty()) {
    const auto path = active_document_->path();
    forgetClosed(path);
    closed_documents_.push_back({path, active_document_->cursor()});
    if (closed_documents_.size() > 20) closed_documents_.erase(closed_documents_.begin());
  }
  documents_.erase(documents_.begin() + static_cast<std::ptrdiff_t>(active_index_));
  if (documents_.empty()) {
    active_document_ = nullptr; active_index_ = 0;
  } else {
    active_index_ = std::min(active_index_, documents_.size() - 1);
    active_document_ = documents_[active_index_].get();
  }
}

auto DocumentSession::takeLastClosed() -> std::optional<ClosedDocument> {
  if (closed_documents_.empty()) return std::nullopt;
  auto result = closed_documents_.back(); closed_documents_.pop_back(); return result;
}

void DocumentSession::clear() {
  documents_.clear(); closed_documents_.clear(); active_document_ = nullptr; active_index_ = 0;
}

auto DocumentSession::documents() -> std::vector<std::unique_ptr<Document>>& { return documents_; }
auto DocumentSession::documents() const -> const std::vector<std::unique_ptr<Document>>& { return documents_; }
auto DocumentSession::closedDocuments() -> std::vector<ClosedDocument>& { return closed_documents_; }
auto DocumentSession::activeDocument() -> Document*& { return active_document_; }
auto DocumentSession::activeIndex() -> std::size_t& { return active_index_; }

void DocumentSession::forgetClosed(const std::filesystem::path& path) {
  std::erase_if(closed_documents_, [&path](const auto& closed) { return closed.path == path; });
}

}  // namespace tuiide
