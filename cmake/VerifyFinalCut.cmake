foreach(required SOURCE_DIR EXPECTED_COMMIT EXPECTED_VERSION EXPECTED_UPSTREAM
    EXPECTED_UPSTREAM_REF GIT_EXECUTABLE)
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

execute_process(
  COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" show-ref --verify --quiet
    "${EXPECTED_UPSTREAM_REF}"
  RESULT_VARIABLE ref_result)
if(NOT ref_result EQUAL 0)
  message(FATAL_ERROR
    "Final Cut upstream ref ${EXPECTED_UPSTREAM_REF} is missing. Fetch the configured upstream branch before verification.")
endif()
run_git(upstream_head rev-parse "${EXPECTED_UPSTREAM_REF}")
execute_process(
  COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" merge-base --is-ancestor
    "${EXPECTED_COMMIT}" "${EXPECTED_UPSTREAM_REF}"
  RESULT_VARIABLE ancestry_result
  ERROR_VARIABLE ancestry_error)
if(ancestry_result EQUAL 1)
  message(FATAL_ERROR
    "Final Cut commit ${EXPECTED_COMMIT} is not reachable from fetched upstream ref ${EXPECTED_UPSTREAM_REF} at ${upstream_head}")
elseif(NOT ancestry_result EQUAL 0)
  message(FATAL_ERROR "Final Cut ancestry check failed: ${ancestry_error}")
endif()

file(READ "${SOURCE_DIR}/configure.ac" configure_text)
if(NOT configure_text MATCHES "AC_INIT\\(\\[finalcut\\], \\[${EXPECTED_VERSION}\\]\\)")
  message(FATAL_ERROR "Final Cut configure.ac does not declare version ${EXPECTED_VERSION}")
endif()

message(STATUS
  "Final Cut ${EXPECTED_VERSION} at ${EXPECTED_COMMIT}: clean checkout reachable from ${EXPECTED_UPSTREAM_REF}")
