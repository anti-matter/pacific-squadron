# Generates buildinfo.h containing the git describe of the working tree.
# Only rewrites the header when the git state actually changed, so normal
# rebuilds do not trigger relinks. Unlike __DATE__/__TIME__ (which reflect
# the compile time of one translation unit), the hash identifies exactly
# which commit a binary came from.

execute_process(
    COMMAND git describe --always --abbrev=7 --dirty
    WORKING_DIRECTORY "${REPO_DIR}"
    OUTPUT_VARIABLE GIT_DESC
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE GIT_RC)
if(NOT GIT_RC EQUAL 0)
    set(GIT_DESC "unknown")
endif()

set(OLD_DESC "")
if(EXISTS "${BUILD_INFO_HEADER}")
    file(READ "${BUILD_INFO_HEADER}" OLD_CONTENTS)
    string(REGEX MATCH "WW_BUILD_HASH \"([^\"]*)\"" _ "${OLD_CONTENTS}")
    set(OLD_DESC "${CMAKE_MATCH_1}")
endif()

if(OLD_DESC STREQUAL GIT_DESC)
    return()
endif()

string(TIMESTAMP TS "%Y-%m-%d %H:%M")
file(WRITE "${BUILD_INFO_HEADER}"
     "#pragma once\n"
     "#define WW_BUILD_HASH \"${GIT_DESC}\"\n"
     "#define WW_BUILD_TS \"${TS}\"\n")
