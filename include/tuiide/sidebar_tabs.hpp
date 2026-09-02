#pragma once

#include <final/final.h>

#include <cstddef>
#include <string>
#include <vector>

namespace tuiide {

/**
 * Переключаемая боковая панель. Прокручивает tab bar, если все названия не
 * помещаются, и не позволяет скрыть последнюю видимую страницу.
 */
class SidebarTabs final : public finalcut::FWidget {
 public:
  explicit SidebarTabs(finalcut::FWidget* parent = nullptr);

  void addTab(std::string title, finalcut::FWidget& page);
  void setCurrentIndex(std::size_t index, bool focus_page = false);
  auto setTabVisible(std::size_t index, bool visible, bool focus_page = false) -> bool;
  [[nodiscard]] auto isTabVisible(std::size_t index) const -> bool;
  [[nodiscard]] auto visibleCount() const -> std::size_t;
  [[nodiscard]] auto currentIndex() const -> std::size_t;
  [[nodiscard]] auto currentTitle() const -> std::string;
  void selectRelative(int direction, bool focus_page = false);
  void layoutPages();

 protected:
  void draw() override;
  void onKeyPress(finalcut::FKeyEvent* event) override;
  void onMouseDown(finalcut::FMouseEvent* event) override;

 private:
  struct Tab {
    std::string title;
    finalcut::FWidget* page{};
    int x{};
    int width{};
    bool visible{true};
  };

  void scrollRelative(int direction);
  std::vector<Tab> tabs_;
  std::size_t current_{};
  std::size_t first_visible_{};
  bool left_overflow_{};
  bool right_overflow_{};
};

}  // namespace tuiide
