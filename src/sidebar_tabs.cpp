#include "tuiide/sidebar_tabs.hpp"
#include "tuiide/tab_bar_layout.hpp"

#include <algorithm>
#include <utility>

namespace tuiide {

SidebarTabs::SidebarTabs(finalcut::FWidget* parent) : FWidget(parent) {
  setFocusable();
  setForegroundColor(finalcut::FColor::Black);
  setBackgroundColor(finalcut::FColor::LightGray);
}

void SidebarTabs::addTab(std::string title, finalcut::FWidget& page) {
  tabs_.push_back({std::move(title), &page});
  page.setVisible(tabs_.size() == 1);
}

void SidebarTabs::setTabTitle(std::size_t index, std::string title) {
  if (index >= tabs_.size()) return;
  tabs_[index].title = std::move(title);
  if (isShown()) redrawCurrentPage();
}

void SidebarTabs::setCurrentIndex(std::size_t index, bool focus_page) {
  if (index >= tabs_.size() || !tabs_[index].visible) return;
  const auto previous = current_;
  current_ = index;
  for (std::size_t i = 0; i < tabs_.size(); ++i) {
    if (i == current_) continue;
    if (i == previous) tabs_[i].page->hide();
    tabs_[i].page->unsetVisible();
  }
  tabs_[current_].page->setVisible();
  if (isShown()) tabs_[current_].page->show();
  redrawCurrentPage();
  if (focus_page) {
    tabs_[current_].page->setFocus();
    finalcut::FWidget::setFocusWidget(tabs_[current_].page);
  }
}

auto SidebarTabs::currentIndex() const -> std::size_t { return current_; }

auto SidebarTabs::currentTitle() const -> std::string {
  return current_ < tabs_.size() ? tabs_[current_].title : std::string{};
}

auto SidebarTabs::setTabVisible(std::size_t index, bool visible, bool focus_page) -> bool {
  if (index >= tabs_.size() || tabs_[index].visible == visible) return index < tabs_.size();
  if (!visible && visibleCount() == 1) return false;
  tabs_[index].visible = visible;
  if (!visible) {
    tabs_[index].page->hide(); tabs_[index].page->unsetVisible();
    if (current_ == index) {
      auto next = index;
      for (std::size_t offset = 1; offset < tabs_.size(); ++offset) {
        const auto candidate = (index + offset) % tabs_.size();
        if (tabs_[candidate].visible) { next = candidate; break; }
      }
      setCurrentIndex(next, focus_page);
    }
  } else {
    setCurrentIndex(index, focus_page);
  }
  redraw(); return true;
}

auto SidebarTabs::isTabVisible(std::size_t index) const -> bool {
  return index < tabs_.size() && tabs_[index].visible;
}

auto SidebarTabs::visibleCount() const -> std::size_t {
  return static_cast<std::size_t>(std::count_if(tabs_.begin(), tabs_.end(), [](const auto& tab) { return tab.visible; }));
}

void SidebarTabs::layoutPages() {
  const auto width = getWidth();
  const auto height = getHeight();
  for (auto& tab : tabs_)
    if (tab.visible) tab.page->setGeometry({1, 2}, {width, height > 1 ? height - 1 : 1});
}

void SidebarTabs::redrawCurrentPage() {
  redraw();
  if (current_ < tabs_.size() && tabs_[current_].visible
      && tabs_[current_].page->isShown())
    tabs_[current_].page->redraw();
}

void SidebarTabs::draw() {
  const auto available = static_cast<int>(getWidth());
  print(finalcut::FPoint{1, 1});
  FWidget::setColor(finalcut::FColor::Black, finalcut::FColor::LightGray);
  *this << std::string(static_cast<std::size_t>(std::max(0, available)), ' ');

  std::vector<std::string> titles;
  std::vector<bool> visibility;
  for (const auto& tab : tabs_) { titles.push_back(tab.title); visibility.push_back(tab.visible); }
  const auto layout = layoutTabBar(titles, visibility, current_, first_visible_, available);
  first_visible_ = layout.first_index; left_overflow_ = layout.left_overflow; right_overflow_ = layout.right_overflow;
  for (auto& tab : tabs_) { tab.x = 0; tab.width = 0; }
  if (left_overflow_) { print(finalcut::FPoint{1, 1}); *this << "<"; }
  if (right_overflow_) { print(finalcut::FPoint{available, 1}); *this << ">"; }
  for (const auto& item : layout.items) {
    tabs_[item.index].x = item.x; tabs_[item.index].width = item.width;
    if (item.index == current_)
      FWidget::setColor(finalcut::FColor::White, finalcut::FColor::Blue);
    else
      FWidget::setColor(finalcut::FColor::Black, finalcut::FColor::LightGray);
    print(finalcut::FPoint{item.x, 1}); *this << item.label;
  }
}

void SidebarTabs::onKeyPress(finalcut::FKeyEvent* event) {
  if (event->key() == finalcut::FKey::Left) {
    selectRelative(-1);
    event->accept();
    return;
  }
  if (event->key() == finalcut::FKey::Right) {
    selectRelative(1);
    event->accept();
    return;
  }
  if (event->key() == finalcut::FKey::Return || event->key() == finalcut::FKey::Down) {
    if (!tabs_.empty()) setCurrentIndex(current_, true);
    event->accept();
    return;
  }
  FWidget::onKeyPress(event);
}

void SidebarTabs::onMouseDown(finalcut::FMouseEvent* event) {
  if (event->getButton() != finalcut::MouseButton::Left || event->getY() != 1) return;
  if (event->getX() == 1 && left_overflow_) { scrollRelative(-1); return; }
  if (event->getX() == static_cast<int>(getWidth()) && right_overflow_) { scrollRelative(1); return; }
  for (std::size_t i = 0; i < tabs_.size(); ++i) {
    if (event->getX() >= tabs_[i].x && event->getX() < tabs_[i].x + tabs_[i].width) {
      setCurrentIndex(i, true);
      return;
    }
  }
}

void SidebarTabs::selectRelative(int direction, bool focus_page) {
  if (tabs_.empty() || visibleCount() == 0) return;
  auto next = current_;
  for (std::size_t attempt{}; attempt < tabs_.size(); ++attempt) {
    next = static_cast<std::size_t>((static_cast<int>(next) + direction + static_cast<int>(tabs_.size()))
      % static_cast<int>(tabs_.size()));
    if (tabs_[next].visible) { setCurrentIndex(next, focus_page); return; }
  }
}

void SidebarTabs::scrollRelative(int direction) {
  selectRelative(direction);
}

}  // namespace tuiide
