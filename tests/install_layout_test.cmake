foreach(required BUILD_DIR STAGE_DIR)
  if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
    message(FATAL_ERROR "install_layout_test.cmake requires -D${required}=...")
  endif()
endforeach()

file(REMOVE_RECURSE "${STAGE_DIR}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --prefix "${STAGE_DIR}"
  RESULT_VARIABLE install_result
  OUTPUT_VARIABLE install_output
  ERROR_VARIABLE install_error)
if(NOT install_result EQUAL 0)
  message(FATAL_ERROR "Installation failed:\n${install_output}\n${install_error}")
endif()

foreach(installed_file
    bin/tuiide
    share/man/man1/tuiide.1
    share/doc/tuiide/README.md
    share/doc/tuiide/LICENSE.finalcut
    share/bash-completion/completions/tuiide)
  if(NOT EXISTS "${STAGE_DIR}/${installed_file}")
    message(FATAL_ERROR "Installed file is missing: ${installed_file}")
  endif()
endforeach()

execute_process(
  COMMAND "${STAGE_DIR}/bin/tuiide" --version
  RESULT_VARIABLE version_result
  OUTPUT_VARIABLE version_output
  OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT version_result EQUAL 0 OR NOT version_output STREQUAL "tuiide 0.1.0")
  message(FATAL_ERROR "Installed executable check failed: ${version_output}")
endif()
