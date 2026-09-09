cmake_minimum_required(VERSION 3.20)
project(tuiide VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(cmake/FinalCutVendor.cmake)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux"
    OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
  message(FATAL_ERROR "TUI IDE currently supports Linux/amd64 only")
endif()

set(CMAKE_INSTALL_DEFAULT_DIRECTORY_PERMISSIONS
  OWNER_READ OWNER_WRITE OWNER_EXECUTE
  GROUP_READ GROUP_EXECUTE
  WORLD_READ WORLD_EXECUTE)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(TUIIDE_ENABLE_SANITIZERS "Build TUI IDE targets with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(TUIIDE_ENABLE_CLANG_TIDY "Run clang-tidy while compiling TUI IDE targets" OFF)
option(TUIIDE_ENABLE_CPPCHECK "Run cppcheck while compiling TUI IDE targets" OFF)
option(TUIIDE_ENABLE_LONG_TESTS "Build and register long-running large-file tests" OFF)

add_library(tuiide_sanitizers INTERFACE)
if(TUIIDE_ENABLE_SANITIZERS)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(tuiide_sanitizers INTERFACE
      -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(tuiide_sanitizers INTERFACE -fsanitize=address,undefined)
  else()
    message(FATAL_ERROR "TUIIDE_ENABLE_SANITIZERS requires GCC or Clang")
  endif()
endif()

if(TUIIDE_ENABLE_CLANG_TIDY)
  find_program(TUIIDE_CLANG_TIDY_EXECUTABLE NAMES clang-tidy REQUIRED)
  set(TUIIDE_CLANG_TIDY_COMMAND
    "${TUIIDE_CLANG_TIDY_EXECUTABLE};--extra-arg=-Wno-unknown-warning-option")
endif()
if(TUIIDE_ENABLE_CPPCHECK)
  find_program(TUIIDE_CPPCHECK_EXECUTABLE NAMES cppcheck REQUIRED)
  set(TUIIDE_CPPCHECK_COMMAND
    "${TUIIDE_CPPCHECK_EXECUTABLE};--enable=warning,performance,portability;--inline-suppr;--suppress=missingIncludeSystem")
endif()

function(tuiide_apply_quality target)
  target_link_libraries(${target} PRIVATE tuiide_sanitizers)
  if(TUIIDE_ENABLE_CLANG_TIDY)
    set_property(TARGET ${target} PROPERTY CXX_CLANG_TIDY "${TUIIDE_CLANG_TIDY_COMMAND}")
  endif()
  if(TUIIDE_ENABLE_CPPCHECK)
    set_property(TARGET ${target} PROPERTY CXX_CPPCHECK "${TUIIDE_CPPCHECK_COMMAND}")
  endif()
endfunction()

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
target_compile_definitions(finalcut_vendor PRIVATE COMPILE_FINAL_CUT
  F_VERSION="${TUIIDE_FINALCUT_VERSION}")
target_compile_options(finalcut_vendor PRIVATE -O0)
target_precompile_headers(finalcut_vendor PRIVATE <final/final.h>)
target_link_libraries(finalcut_vendor PUBLIC Threads::Threads ${GPM_LIBRARY} ${TERMINFO_LIBRARY} rt)
set_target_properties(finalcut_vendor PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(tuiide_core
  src/cli.cpp
  src/build_command.cpp
  src/build_diagnostic.cpp
  src/build_output_collector.cpp
  src/build_progress.cpp
  src/build_session.cpp
  src/build_workflow.cpp
  src/command_state.cpp
  src/cmake_model.cpp
  src/cmake_presets.cpp
  src/cmake_session.cpp
  src/cmake_source_edit.cpp
  src/clipboard.cpp
  src/clang_format.cpp
  src/compilation_database.cpp
  src/debug_session.cpp
  src/debug_ui_controller.cpp
  src/document.cpp
  src/document_session.cpp
  src/document_labels.cpp
  src/event_log.cpp
  src/syntax.cpp
  src/tab_bar_layout.cpp
  src/text_display.cpp
  src/text_search.cpp
  src/terminal_buffer.cpp
  src/tool_discovery.cpp
  src/user_settings.cpp
  src/process.cpp
  src/pseudo_terminal.cpp
  src/project_creation.cpp
  src/project_history.cpp
  src/project_import.cpp
  src/project_settings.cpp
  src/project_session.cpp
  src/project_template.cpp
  src/project_tree.cpp
  src/recovery.cpp
  src/run_session.cpp
  src/workspace_edit.cpp
  src/workspace_file_transaction.cpp
  src/lsp_client.cpp
  src/lsp_ui_controller.cpp
  src/gdb_client.cpp
  src/gdb_mi.cpp
  src/launch_configuration.cpp
)
target_include_directories(tuiide_core PUBLIC include)
target_link_libraries(tuiide_core PUBLIC Threads::Threads nlohmann_json::nlohmann_json ${UTIL_LIBRARY})
target_compile_options(tuiide_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
tuiide_apply_quality(tuiide_core)

add_executable(tuiide src/main.cpp src/ide_window.cpp src/code_editor.cpp src/console_widget.cpp src/sidebar_tabs.cpp
  src/debug_dialogs.cpp src/project_dialogs.cpp src/run_dialogs.cpp src/search_dialogs.cpp src/ui_dialogs.cpp)
target_link_libraries(tuiide PRIVATE tuiide_core finalcut_vendor)
target_compile_options(tuiide PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
tuiide_apply_quality(tuiide)

include(CTest)
if(BUILD_TESTING)
  add_executable(tuiide_tests tests/core_tests.cpp)
  target_link_libraries(tuiide_tests PRIVATE tuiide_core)
  tuiide_apply_quality(tuiide_tests)
  add_test(NAME tuiide_core_tests COMMAND tuiide_tests)

  add_test(NAME tuiide_install_layout
    COMMAND ${CMAKE_COMMAND}
      -DBUILD_DIR=${CMAKE_BINARY_DIR}
      -DSTAGE_DIR=${CMAKE_BINARY_DIR}/install-test
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/install_layout_test.cmake)
  set_tests_properties(tuiide_install_layout PROPERTIES LABELS packaging)

  find_package(Git QUIET)
  if(Git_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/finalcut/.git")
    add_test(NAME tuiide_finalcut_vendor_clean
      COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/third_party/finalcut
        -DEXPECTED_COMMIT=${TUIIDE_FINALCUT_COMMIT}
        -DEXPECTED_VERSION=${TUIIDE_FINALCUT_VERSION}
        -DEXPECTED_UPSTREAM=${TUIIDE_FINALCUT_UPSTREAM}
        -DEXPECTED_UPSTREAM_REF=${TUIIDE_FINALCUT_UPSTREAM_REF}
        -DGIT_EXECUTABLE=${GIT_EXECUTABLE}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyFinalCut.cmake)
    set_tests_properties(tuiide_finalcut_vendor_clean PROPERTIES LABELS vendor)
    add_test(NAME tuiide_finalcut_vendor_provenance_tests
      COMMAND ${CMAKE_COMMAND}
        -DTEST_DIR=${CMAKE_BINARY_DIR}/finalcut-provenance-test
        -DVERIFY_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyFinalCut.cmake
        -DGIT_EXECUTABLE=${GIT_EXECUTABLE}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/finalcut_vendor_test.cmake)
    set_tests_properties(tuiide_finalcut_vendor_provenance_tests PROPERTIES LABELS vendor)
  endif()

  if(TUIIDE_ENABLE_LONG_TESTS)
    add_executable(tuiide_large_file_tests tests/large_file_tests.cpp)
    target_link_libraries(tuiide_large_file_tests PRIVATE tuiide_core)
    target_compile_options(tuiide_large_file_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
    tuiide_apply_quality(tuiide_large_file_tests)
    add_test(NAME tuiide_large_file_tests COMMAND tuiide_large_file_tests)
    set_tests_properties(tuiide_large_file_tests PROPERTIES TIMEOUT 180 LABELS long)
  endif()

  find_program(CLANGD_EXECUTABLE clangd)
  find_program(GDB_EXECUTABLE gdb)
  if(CLANGD_EXECUTABLE AND GDB_EXECUTABLE AND UTIL_LIBRARY)
    add_executable(tuiide_fake_lsp_server tests/fake_lsp_server.cpp)
    target_link_libraries(tuiide_fake_lsp_server PRIVATE nlohmann_json::nlohmann_json)
    target_compile_options(tuiide_fake_lsp_server PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
    tuiide_apply_quality(tuiide_fake_lsp_server)
    add_executable(tuiide_integration_tests tests/integration_tests.cpp)
    target_link_libraries(tuiide_integration_tests PRIVATE tuiide_core ${UTIL_LIBRARY})
    target_compile_options(tuiide_integration_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
    tuiide_apply_quality(tuiide_integration_tests)
    add_test(NAME tuiide_integration_tests COMMAND tuiide_integration_tests
      $<TARGET_FILE:tuiide> $<TARGET_FILE:tuiide_fake_lsp_server>)
    set_tests_properties(tuiide_integration_tests PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE LABELS integration)
    add_test(NAME tuiide_project_dialog_tests COMMAND tuiide_integration_tests
      $<TARGET_FILE:tuiide> $<TARGET_FILE:tuiide_fake_lsp_server>)
    set_tests_properties(tuiide_project_dialog_tests PROPERTIES
      ENVIRONMENT TUIIDE_PROJECT_DIALOGS_ONLY=1 TIMEOUT 120 RUN_SERIAL TRUE LABELS integration)
  add_test(NAME tuiide_window_help_tests COMMAND tuiide_integration_tests
      $<TARGET_FILE:tuiide> $<TARGET_FILE:tuiide_fake_lsp_server>)
    set_tests_properties(tuiide_window_help_tests PROPERTIES
      ENVIRONMENT TUIIDE_WINDOW_HELP_ONLY=1 TIMEOUT 90 RUN_SERIAL TRUE LABELS integration)
    add_test(NAME tuiide_panel_redraw_tests COMMAND tuiide_integration_tests
      $<TARGET_FILE:tuiide> $<TARGET_FILE:tuiide_fake_lsp_server>)
    set_tests_properties(tuiide_panel_redraw_tests PROPERTIES
      ENVIRONMENT "TUIIDE_WINDOW_HELP_ONLY=1;TUIIDE_PANEL_REDRAW_ONLY=1"
      TIMEOUT 90 RUN_SERIAL TRUE LABELS integration)
    add_test(NAME tuiide_menu_mnemonic_tests COMMAND tuiide_integration_tests
      $<TARGET_FILE:tuiide> $<TARGET_FILE:tuiide_fake_lsp_server>)
    set_tests_properties(tuiide_menu_mnemonic_tests PROPERTIES
      ENVIRONMENT "TUIIDE_WINDOW_HELP_ONLY=1;TUIIDE_MENU_MNEMONICS_ONLY=1"
      TIMEOUT 60 RUN_SERIAL TRUE LABELS integration)
    add_test(NAME tuiide_recent_files_tests COMMAND tuiide_integration_tests
      $<TARGET_FILE:tuiide> $<TARGET_FILE:tuiide_fake_lsp_server>)
    set_tests_properties(tuiide_recent_files_tests PROPERTIES
      ENVIRONMENT "TUIIDE_WINDOW_HELP_ONLY=1;TUIIDE_RECENT_FILES_ONLY=1"
      TIMEOUT 60 RUN_SERIAL TRUE LABELS integration)
    add_test(NAME tuiide_launch_configuration_tests COMMAND tuiide_integration_tests
      $<TARGET_FILE:tuiide> $<TARGET_FILE:tuiide_fake_lsp_server>)
    set_tests_properties(tuiide_launch_configuration_tests PROPERTIES
      ENVIRONMENT "TUIIDE_WINDOW_HELP_ONLY=1;TUIIDE_LAUNCH_CONFIGS_ONLY=1"
      TIMEOUT 90 RUN_SERIAL TRUE LABELS integration)
    add_test(NAME tuiide_tools_dialog_tests COMMAND tuiide_integration_tests
      $<TARGET_FILE:tuiide> $<TARGET_FILE:tuiide_fake_lsp_server>)
    set_tests_properties(tuiide_tools_dialog_tests PROPERTIES
      ENVIRONMENT TUIIDE_TOOLS_DIALOGS_ONLY=1 TIMEOUT 120 RUN_SERIAL TRUE LABELS integration)
    add_test(NAME tuiide_shortcut_dialog_tests COMMAND tuiide_integration_tests
      $<TARGET_FILE:tuiide> $<TARGET_FILE:tuiide_fake_lsp_server>)
    set_tests_properties(tuiide_shortcut_dialog_tests PROPERTIES
      ENVIRONMENT TUIIDE_SHORTCUTS_ONLY=1 TIMEOUT 90 RUN_SERIAL TRUE LABELS integration)
    add_test(NAME tuiide_search_dialog_tests COMMAND tuiide_integration_tests
      $<TARGET_FILE:tuiide> $<TARGET_FILE:tuiide_fake_lsp_server>)
    set_tests_properties(tuiide_search_dialog_tests PROPERTIES
      ENVIRONMENT TUIIDE_SEARCH_DIALOGS_ONLY=1 TIMEOUT 150 RUN_SERIAL TRUE LABELS integration)
    add_test(NAME tuiide_lsp_dialog_tests COMMAND tuiide_integration_tests
      $<TARGET_FILE:tuiide> $<TARGET_FILE:tuiide_fake_lsp_server>)
    set_tests_properties(tuiide_lsp_dialog_tests PROPERTIES
      ENVIRONMENT TUIIDE_LSP_DIALOGS_ONLY=1 TIMEOUT 150 RUN_SERIAL TRUE LABELS integration)
    add_test(NAME tuiide_lsp_protocol_tests COMMAND tuiide_integration_tests
      $<TARGET_FILE:tuiide> $<TARGET_FILE:tuiide_fake_lsp_server>)
    set_tests_properties(tuiide_lsp_protocol_tests PROPERTIES
      ENVIRONMENT TUIIDE_LSP_MATRIX_ONLY=1 TIMEOUT 60 RUN_SERIAL TRUE
      LABELS integration)
    add_test(NAME tuiide_debug_dialog_tests COMMAND tuiide_integration_tests
      $<TARGET_FILE:tuiide> $<TARGET_FILE:tuiide_fake_lsp_server>)
    set_tests_properties(tuiide_debug_dialog_tests PROPERTIES
      ENVIRONMENT TUIIDE_DEBUG_DIALOGS_ONLY=1 TIMEOUT 180 RUN_SERIAL TRUE LABELS integration)
    add_test(NAME tuiide_preset_dialog_tests COMMAND tuiide_integration_tests
      $<TARGET_FILE:tuiide> $<TARGET_FILE:tuiide_fake_lsp_server>)
    set_tests_properties(tuiide_preset_dialog_tests PROPERTIES
      ENVIRONMENT TUIIDE_PRESET_DIALOGS_ONLY=1 TIMEOUT 60 RUN_SERIAL TRUE LABELS integration)
    add_test(NAME tuiide_class_template_dialog_tests COMMAND tuiide_integration_tests
      $<TARGET_FILE:tuiide> $<TARGET_FILE:tuiide_fake_lsp_server>)
    set_tests_properties(tuiide_class_template_dialog_tests PROPERTIES
      ENVIRONMENT TUIIDE_CLASS_TEMPLATE_ONLY=1 TIMEOUT 60 RUN_SERIAL TRUE LABELS integration)
  else()
    message(STATUS "Skipping integration test target: clangd, gdb, or libutil not found")
  endif()
endif()

find_package(Git QUIET)
if(Git_FOUND)
  add_custom_target(check-finalcut-vendor
    COMMAND ${CMAKE_COMMAND}
      -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/third_party/finalcut
      -DEXPECTED_COMMIT=${TUIIDE_FINALCUT_COMMIT}
      -DEXPECTED_VERSION=${TUIIDE_FINALCUT_VERSION}
      -DEXPECTED_UPSTREAM=${TUIIDE_FINALCUT_UPSTREAM}
      -DEXPECTED_UPSTREAM_REF=${TUIIDE_FINALCUT_UPSTREAM_REF}
      -DGIT_EXECUTABLE=${GIT_EXECUTABLE}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyFinalCut.cmake
    COMMENT "Checking that vendored Final Cut is an unmodified pinned checkout")
endif()

install(TARGETS tuiide RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES docs/tuiide.1 DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
install(FILES README.md README.en.md DESTINATION ${CMAKE_INSTALL_DOCDIR})
install(FILES third_party/finalcut/LICENSE
  DESTINATION ${CMAKE_INSTALL_DOCDIR} RENAME LICENSE.finalcut)
install(FILES completions/tuiide.bash
  DESTINATION ${CMAKE_INSTALL_DATADIR}/bash-completion/completions RENAME tuiide)

set(CPACK_PACKAGE_NAME "tuiide")
set(CPACK_PACKAGE_VENDOR "TUI IDE contributors")
set(CPACK_PACKAGE_CONTACT "TUI IDE contributors")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Terminal IDE for C and C++")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_FILE_NAME "tuiide-${PROJECT_VERSION}-linux-amd64")
set(CPACK_SYSTEM_NAME "linux-amd64")
set(CPACK_GENERATOR "TGZ;DEB")
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "amd64")
set(CPACK_DEBIAN_PACKAGE_SECTION "devel")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS
  "cmake, clangd, gdb, clang-format, wl-clipboard | xclip | xsel")
set(CPACK_ARCHIVE_COMPONENT_INSTALL OFF)
include(CPack)
