# Install-time script: copy the SDL3 shared library into the install tree as
#   <prefix>/lib/pacific-squadron/<SONAME>
# so the game finds it through its "$ORIGIN/../lib/pacific-squadron" rpath in
# any self-contained layout (system install, extracted zip, AppImage).
#
# Invoked from install(CODE)+install(SCRIPT) in CMakeLists.txt; SDL_LIB is the
# $<TARGET_FILE:SDL3::SDL3> path. The imported target usually points at the
# linker's dev symlink (libSDL3.so), but the loader resolves the DT_NEEDED
# entry by SONAME (libSDL3.so.0), so we must copy the real file under exactly
# that name.
#
# DESTDIR (cmake --install and CPack staging) needs no handling: during an
# install run CMake auto-prefixes it onto file(INSTALL) destinations.

if(NOT DEFINED SDL_LIB OR SDL_LIB STREQUAL "")
    message(FATAL_ERROR "BundleSDLUnix.cmake: SDL_LIB not set")
endif()

get_filename_component(ps_real "${SDL_LIB}" REALPATH)
get_filename_component(ps_base "${ps_real}" NAME)
if(NOT ps_real MATCHES "\\.so(\\.[0-9]+)*$")
    message(FATAL_ERROR "BundleSDLUnix.cmake: ${ps_real} is not a shared library")
endif()

# Prefer the loader's own answer for the name; fall back to the
# lib<name>.so.<major> convention (strip trailing version beyond the soname).
set(ps_soname "")
find_program(ps_objdump NAMES objdump)
if(ps_objdump)
    execute_process(COMMAND "${ps_objdump}" -p "${ps_real}"
                    OUTPUT_VARIABLE ps_dump OUTPUT_QUIET ERROR_QUIET)
    string(REGEX MATCH "SONAME[ \t]+([^ \r\n]+)" ps_match "${ps_dump}")
    if(ps_match)
        set(ps_soname "${CMAKE_MATCH_1}")
    endif()
endif()
if(NOT ps_soname)
    if(ps_base MATCHES "^(.+\\.so\\.[0-9]+)\\.[0-9].*")
        set(ps_soname "${CMAKE_MATCH_1}")
    else()
        set(ps_soname "${ps_base}")
    endif()
endif()

set(ps_dest "${CMAKE_INSTALL_PREFIX}/lib/pacific-squadron")
file(INSTALL "${ps_real}" DESTINATION "${ps_dest}" RENAME "${ps_soname}"
     USE_SOURCE_PERMISSIONS)
message(STATUS "Bundled SDL3: ${ps_real} -> ${ps_dest}/${ps_soname}")
