#pragma once

#include "tuiide/text_search.hpp"
#include "tuiide/ui_dialogs.hpp"

#include <memory>
#include <string>

namespace tuiide {

/** Действие, выбранное пользователем в едином Find/Replace диалоге. */
enum class SearchAction { None, Next, Previous, Replace, ReplaceAll, FindAll };

/** Полный запрос поиска, включая область и режимы регулярного выражения. */
struct SearchRequest {
  SearchAction action{SearchAction::None};
  std::string query;
  std::string replacement;
  SearchOptions options;
  bool project{};
};

/** Модальный интерфейс поиска/замены, не выполняющий файловые операции сам. */
class SearchDialog final : public CenteredDialog {
 public:
  SearchDialog(const SearchRequest& initial, bool has_project,
    finalcut::FWidget* parent = nullptr, std::string language = "en");
  ~SearchDialog() override;
  [[nodiscard]] auto request() const -> SearchRequest;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace tuiide
