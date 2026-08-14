cmake_minimum_required(VERSION 3.20)
project(tuiide VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)
find_package(nlohmann_json 3.11 REQUIRED)
find_library(UTIL_LIBRARY util REQUIRED)

# Upstream Final Cut uses Autotools. Build its checked-out sources as an
# internal static target so this project still has one reproducible CMake build.
file(GLOB_RECURSE FINALCUT_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/third_party/finalcut/final/*.cpp")
if(NOT FINALCUT_SOURCES)
  message(FATAL_ERROR "Final Cut sources are missing; initialize third_party/finalcut")
endif()
find_library(GPM_LIBRARY gpm REQUIRED)
find_library(TERMINFO_LIBRARY NAMES tinfo ncursesw ncurses REQUIRED)
add_library(finalcut_vendor STATIC ${FINALCUT_SOURCES})
target_include_directories(finalcut_vendor PUBLIC third_party/finalcut)
target_compile_definitions(finalcut_vendor PRIVATE COMPILE_FINAL_CUT F_VERSION="0.9.2")
target_compile_options(finalcut_vendor PRIVATE -O0)
target_precompile_headers(finalcut_vendor PRIVATE <final/final.h>)
target_link_libraries(finalcut_vendor PUBLIC Threads::Threads ${GPM_LIBRARY} ${TERMINFO_LIBRARY} rt)
set_target_properties(finalcut_vendor PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(tuiide_core
  src/build_diagnostic.cpp
  src/build_progress.cpp
  src/command_state.cpp
  src/cmake_model.cpp
  src/cmake_presets.cpp
  src/cmake_source_edit.cpp
  src/clipboard.cpp
  src/clang_format.cpp
  src/compilation_database.cpp
  src/debug_session.cpp
  src/document.cpp
  src/document_labels.cpp
  src/syntax.cpp
  src/tab_bar_layout.cpp
  src/text_display.cpp
  src/text_search.cpp
  src/terminal_buffer.cpp
  src/process.cpp
  src/pseudo_terminal.cpp
  src/project_creation.cpp
  src/project_history.cpp
  src/project_import.cpp
  src/project_settings.cpp
  src/project_template.cpp
  src/project_tree.cpp
  src/recovery.cpp
  src/workspace_edit.cpp
  src/workspace_file_transaction.cpp
  src/lsp_client.cpp
  src/gdb_client.cpp
  src/gdb_mi.cpp
  src/launch_configuration.cpp
)
target_include_directories(tuiide_core PUBLIC include)
target_link_libraries(tuiide_core PUBLIC Threads::Threads nlohmann_json::nlohmann_json ${UTIL_LIBRARY})
target_compile_options(tuiide_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion)

add_executable(tuiide src/main.cpp src/ide_window.cpp src/code_editor.cpp src/console_widget.cpp src/sidebar_tabs.cpp)
target_link_libraries(tuiide PRIVATE tuiide_core finalcut_vendor)
target_compile_options(tuiide PRIVATE -Wall -Wextra -Wpedantic -Wconversion)

include(CTest)
if(BUILD_TESTING)
  add_executable(tuiide_tests tests/core_tests.cpp)
  target_link_libraries(tuiide_tests PRIVATE tuiide_core)
  add_test(NAME tuiide_core_tests COMMAND tuiide_tests)

  find_program(CLANGD_EXECUTABLE clangd)
  find_program(GDB_EXECUTABLE gdb)
  if(CLANGD_EXECUTABLE AND GDB_EXECUTABLE AND UTIL_LIBRARY)
    add_executable(tuiide_integration_tests tests/integration_tests.cpp)
    target_link_libraries(tuiide_integration_tests PRIVATE tuiide_core ${UTIL_LIBRARY})
    target_compile_options(tuiide_integration_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
    add_test(NAME tuiide_integration_tests COMMAND tuiide_integration_tests $<TARGET_FILE:tuiide>)
    set_tests_properties(tuiide_integration_tests PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE LABELS integration)
  else()
    message(STATUS "Skipping integration test target: clangd, gdb, or libutil not found")
  endif()
endif()

install(TARGETS tuiide RUNTIME DESTINATION bin)
