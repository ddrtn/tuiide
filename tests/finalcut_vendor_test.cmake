foreach(required TEST_DIR VERIFY_SCRIPT GIT_EXECUTABLE)
  if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
    message(FATAL_ERROR "finalcut_vendor_test.cmake requires -D${required}=...")
  endif()
endforeach()

file(REMOVE_RECURSE "${TEST_DIR}")
file(MAKE_DIRECTORY "${TEST_DIR}")

function(run_git)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${TEST_DIR}" ${ARGN}
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "Fixture Git command failed: ${error}")
  endif()
  set(GIT_OUTPUT "${output}" PARENT_SCOPE)
endfunction()

run_git(init)
run_git(config user.name "TUI IDE test")
run_git(config user.email "tuiide-test@example.invalid")
run_git(remote add origin "https://github.com/gansm/finalcut.git")
file(WRITE "${TEST_DIR}/configure.ac" "AC_INIT([finalcut], [1.2.3])\n")
run_git(add configure.ac)
run_git(commit -m "upstream fixture")
run_git(rev-parse HEAD)
set(upstream_commit "${GIT_OUTPUT}")
run_git(update-ref refs/remotes/origin/main "${upstream_commit}")

file(APPEND "${TEST_DIR}/configure.ac" "# local-only commit\n")
run_git(add configure.ac)
run_git(commit -m "local fixture")
run_git(rev-parse HEAD)
set(local_commit "${GIT_OUTPUT}")

set(verify_command "${CMAKE_COMMAND}"
  "-DSOURCE_DIR=${TEST_DIR}"
  "-DEXPECTED_COMMIT=${local_commit}"
  "-DEXPECTED_VERSION=1.2.3"
  "-DEXPECTED_UPSTREAM=https://github.com/gansm/finalcut.git"
  "-DEXPECTED_UPSTREAM_REF=refs/remotes/origin/main"
  "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
  "-P" "${VERIFY_SCRIPT}")

run_git(update-ref -d refs/remotes/origin/main)
execute_process(
  COMMAND ${verify_command}
  RESULT_VARIABLE missing_ref_result
  OUTPUT_VARIABLE missing_ref_output
  ERROR_VARIABLE missing_ref_error)
if(missing_ref_result EQUAL 0
    OR NOT "${missing_ref_output}${missing_ref_error}" MATCHES "is missing")
  message(FATAL_ERROR "Verifier did not require a fetched upstream ref")
endif()
run_git(update-ref refs/remotes/origin/main "${upstream_commit}")

execute_process(
  COMMAND ${verify_command}
  RESULT_VARIABLE rejected_result
  OUTPUT_VARIABLE rejected_output
  ERROR_VARIABLE rejected_error)
if(rejected_result EQUAL 0)
  message(FATAL_ERROR "Verifier accepted a local-only Final Cut commit")
endif()
if(NOT "${rejected_output}${rejected_error}" MATCHES "not reachable")
  message(FATAL_ERROR
    "Verifier rejected the fixture for an unexpected reason:\n${rejected_output}${rejected_error}")
endif()

run_git(update-ref refs/remotes/origin/main "${local_commit}")
execute_process(
  COMMAND ${verify_command}
  RESULT_VARIABLE accepted_result
  OUTPUT_VARIABLE accepted_output
  ERROR_VARIABLE accepted_error)
if(NOT accepted_result EQUAL 0)
  message(FATAL_ERROR
    "Verifier rejected a commit reachable from the fetched upstream ref:\n${accepted_output}${accepted_error}")
endif()

file(REMOVE_RECURSE "${TEST_DIR}")
