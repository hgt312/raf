# Provides
#   - GIT_FOUND - true if the command line client was found
#   - GIT_EXECUTABLE - path to git command line client
#   - GIT_VERSION_STRING - the version of git found (since CMake 2.8.8)
#   - MNM_GIT_VERSION
find_package(Git QUIET)
if (${GIT_FOUND})
  message(STATUS "Git found: ${GIT_EXECUTABLE}")
  execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse HEAD
                  WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
                  OUTPUT_VARIABLE MNM_GIT_VERSION
                  ERROR_QUIET
                  OUTPUT_STRIP_TRAILING_WHITESPACE)
else()
  message(WARNING "Git not found")
  set(MNM_GIT_VERSION "git-not-found")
endif()