#include "tuiide/search_dialogs.hpp"
#include "tuiide/ui_localization.hpp"

namespace tuiide {

struct SearchDialog::Impl {
  Impl(SearchDialog* dialog, const SearchRequest& initial, bool has_project,
      const std::string& language)
      : owner(dialog),
        find_label(finalcut::FString(localizedUiText(language, "Find:")), owner), find(owner),
        replace_label(finalcut::FString(localizedUiText(language, "Replace:")), owner), replace(owner),
        case_sensitive(finalcut::FString(localizedUiText(language, "Case sensitive")), owner),
        whole_word(finalcut::FString(localizedUiText(language, "Whole word")), owner),
        regular_expression(finalcut::FString(localizedUiText(language, "Regular expression")), owner),
        entire_project(finalcut::FString(localizedUiText(language, "Entire project")), owner),
        next(finalcut::FString(localizedUiText(language, "&Next")), owner),
        previous(finalcut::FString(localizedUiText(language, "&Previous")), owner),
        replace_one(finalcut::FString(localizedUiText(language, "&Replace")), owner),
        replace_all(finalcut::FString(localizedUiText(language, "Replace &all")), owner),
        find_all(finalcut::FString(localizedUiText(language, "&Find all")), owner),
        cancel(finalcut::FString(localizedUiText(language, "&Cancel")), owner) {
    constexpr std::size_t width = 58;
    find_label.setGeometry({3, 2}, {11, 1}); find.setGeometry({14, 2}, {width - 17, 1});
    replace_label.setGeometry({3, 4}, {11, 1}); replace.setGeometry({14, 4}, {width - 17, 1});
    case_sensitive.setGeometry({3, 6}, {20, 1}); whole_word.setGeometry({27, 6}, {18, 1});
    regular_expression.setGeometry({3, 8}, {22, 1}); entire_project.setGeometry({27, 8}, {22, 1});
    find.setText(finalcut::FString(initial.query));
    replace.setText(finalcut::FString(initial.replacement));
    if (initial.options.case_sensitive) case_sensitive.setChecked();
    if (initial.options.whole_word) whole_word.setChecked();
    if (initial.options.regular_expression) regular_expression.setChecked();
    if (initial.project && has_project) entire_project.setChecked();
    entire_project.setEnable(has_project);
    next.setGeometry({3, 11}, {10, 1}); previous.setGeometry({16, 11}, {11, 1});
    replace_one.setGeometry({30, 11}, {11, 1}); replace_all.setGeometry({3, 13}, {12, 1});
    find_all.setGeometry({18, 13}, {10, 1}); cancel.setGeometry({31, 13}, {10, 1});
    bind(next, SearchAction::Next);
    bind(previous, SearchAction::Previous);
    bind(replace_one, SearchAction::Replace);
    bind(replace_all, SearchAction::ReplaceAll);
    bind(find_all, SearchAction::FindAll);
    cancel.addCallback("clicked", [this] {
      owner->done(finalcut::FDialog::ResultCode::Reject);
    });
    find.addCallback("activate", [this] {
      action = SearchAction::Next;
      owner->done(finalcut::FDialog::ResultCode::Accept);
    });
    find.setFocus();
  }

  void bind(finalcut::FButton& button, SearchAction selected_action) {
    button.addCallback("clicked", [this, selected_action] {
      action = selected_action;
      owner->done(finalcut::FDialog::ResultCode::Accept);
    });
  }

  auto request() const -> SearchRequest {
    return {action, find.getText().toString(), replace.getText().toString(),
      {.case_sensitive = case_sensitive.isChecked(), .whole_word = whole_word.isChecked(),
       .regular_expression = regular_expression.isChecked()}, entire_project.isChecked()};
  }

  SearchDialog* owner;
  finalcut::FLabel find_label; finalcut::FLineEdit find;
  finalcut::FLabel replace_label; finalcut::FLineEdit replace;
  finalcut::FCheckBox case_sensitive; finalcut::FCheckBox whole_word;
  finalcut::FCheckBox regular_expression; finalcut::FCheckBox entire_project;
  finalcut::FButton next; finalcut::FButton previous; finalcut::FButton replace_one;
  finalcut::FButton replace_all; finalcut::FButton find_all; finalcut::FButton cancel;
  SearchAction action{SearchAction::None};
};

SearchDialog::SearchDialog(const SearchRequest& initial, bool has_project,
    finalcut::FWidget* parent, std::string language)
    : CenteredDialog(finalcut::FString(localizedUiText(language, "Find and replace")), parent) {
  setDialogSize({58, 16});
  setModal();
  impl_ = std::make_unique<Impl>(this, initial, has_project, language);
}

SearchDialog::~SearchDialog() = default;

auto SearchDialog::request() const -> SearchRequest {
  return impl_->request();
}

}  // namespace tuiide
