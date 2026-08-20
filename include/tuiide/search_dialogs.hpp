#pragma once

#include "tuiide/text_search.hpp"
#include "tuiide/ui_dialogs.hpp"

#include <memory>
#include <string>

namespace tuiide {

enum class SearchAction { None, Next, Previous, Replace, ReplaceAll, FindAll };

struct SearchRequest {
  SearchAction action{SearchAction::None};
  std::string query;
  std::string replacement;
  SearchOptions options;
  bool project{};
};

class SearchDialog final : public CenteredDialog {
 public:
  SearchDialog(const SearchRequest& initial, bool has_project,
    finalcut::FWidget* parent = nullptr);
  ~SearchDialog() override;
  [[nodiscard]] auto request() const -> SearchRequest;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace tuiide
