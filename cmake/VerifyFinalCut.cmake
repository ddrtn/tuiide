foreach(required SOURCE_DIR EXPECTED_COMMIT EXPECTED_VERSION EXPECTED_UPSTREAM GIT_EXECUTABLE)
  if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
    message(FATAL_ERROR "VerifyFinalCut.cmake requires -D${required}=...")
  endif()
endforeach()

if(NOT EXISTS "${SOURCE_DIR}/.git")
  message(FATAL_ERROR "${SOURCE_DIR} is not a Git checkout; initialize the Final Cut submodule")
endif()

function(run_git output_variable)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" ${ARGN}
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "Final Cut Git check failed: ${error}")
  endif()
  set(${output_variable} "${output}" PARENT_SCOPE)
endfunction()

run_git(head rev-parse HEAD)
if(NOT head STREQUAL EXPECTED_COMMIT)
  message(FATAL_ERROR
    "Final Cut commit is ${head}, expected ${EXPECTED_COMMIT}. Review the update and change cmake/FinalCutVendor.cmake.")
endif()

run_git(status status --porcelain=v1 --untracked-files=all)
if(NOT status STREQUAL "")
  message(FATAL_ERROR "Vendored Final Cut contains local changes:\n${status}")
endif()

run_git(upstream remote get-url origin)
if(NOT upstream STREQUAL EXPECTED_UPSTREAM)
  message(FATAL_ERROR "Final Cut origin is ${upstream}, expected ${EXPECTED_UPSTREAM}")
endif()

file(READ "${SOURCE_DIR}/configure.ac" configure_text)
if(NOT configure_text MATCHES "AC_INIT\\(\\[finalcut\\], \\[${EXPECTED_VERSION}\\]\\)")
  message(FATAL_ERROR "Final Cut configure.ac does not declare version ${EXPECTED_VERSION}")
endif()

message(STATUS "Final Cut ${EXPECTED_VERSION} at ${EXPECTED_COMMIT}: clean upstream checkout")
