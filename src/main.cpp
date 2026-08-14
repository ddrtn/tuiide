#include "tuiide/ide_window.hpp"

#include <final/final.h>

#include <filesystem>
#include <iostream>

int main(int argc, char* argv[]) {
  try {
    finalcut::FApplication application(argc, argv);
    const auto root = argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path{};
    tuiide::IdeWindow window(root, &application);
    const auto desktop_height = window.getDesktopHeight();
    window.setGeometry({1, 1}, {window.getDesktopWidth(), desktop_height > 1 ? desktop_height - 1 : desktop_height});
    finalcut::FWidget::setMainWidget(&window);
    window.show();
    return application.exec();
  } catch (const std::exception& error) {
    std::cerr << "tuiide: " << error.what() << '\n';
    return 1;
  }
}
