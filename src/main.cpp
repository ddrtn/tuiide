#include "tuiide/cli.hpp"
#include "tuiide/ide_window.hpp"

#include <final/final.h>

#include <filesystem>
#include <iostream>
#include <string_view>
#include <vector>

int main(int argc, char* argv[]) {
  try {
    std::vector<std::string_view> arguments;
    for (int index = 1; index < argc; ++index) arguments.emplace_back(argv[index]);
    const auto parsed = tuiide::parseCommandLine(arguments);
    const std::string_view program = argc > 0 ? argv[0] : "tuiide";
    if (!parsed.error.empty()) {
      std::cerr << "tuiide: " << parsed.error << "\nTry '" << program
                << " --help' for more information.\n";
      return 2;
    }
    if (parsed.options.show_help) {
      std::cout << tuiide::commandLineHelp(program);
      return 0;
    }
    if (parsed.options.show_version) {
      std::cout << "tuiide " << tuiide::version << '\n';
      return 0;
    }
    int application_argc = 1;
    char* application_argv[]{argv[0], nullptr};
    finalcut::FApplication application(application_argc, application_argv);
    tuiide::IdeWindow window(parsed.options.project, parsed.options.log_file,
      parsed.options.diagnostic, &application);
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
