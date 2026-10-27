# Embed SDL3 inside a macOS .app so the bundle runs on Macs without Homebrew.
#
#   cmake -DAPP_BUNDLE=<path>/pacific-squadron.app \
#         -DSDL_LIB=<path>/libSDL3.0.dylib \
#         [-DSDL_LICENSE=<path>/LICENSE.txt] \
#         -P cmake/BundleSDL.cmake
#
# Homebrew's libSDL3 records its own absolute install path as its install name,
# and the executable links against that path. Left alone, the .app only runs on
# the machine (and Homebrew prefix) it was built on. So:
#
#   1. copy the library into Contents/Frameworks/,
#   2. rewrite its install name to @rpath/<name>,
#   3. rewrite the executable's reference to @rpath/<name> and make sure the
#      binary carries an @executable_path/../Frameworks rpath,
#   4. ship the zlib license among the bundle resources (it must accompany
#      redistributed copies of the library),
#   5. re-sign ad-hoc -- install_name_tool invalidates the linker signature, and
#      arm64 macOS refuses to load unsigned or impostor binaries.
#
# Handles both layouts: a bare libSDL3*.dylib (Homebrew) and SDL3.framework
# (the official libs.org dmg). Idempotent, so POST_BUILD can rerun it freely.
if(NOT DEFINED APP_BUNDLE OR NOT DEFINED SDL_LIB)
    message(FATAL_ERROR "BundleSDL.cmake requires -DAPP_BUNDLE and -DSDL_LIB")
endif()

set(EXE "${APP_BUNDLE}/Contents/MacOS/pacific-squadron")
if(NOT EXISTS "${EXE}")
    message(FATAL_ERROR "BundleSDL: no executable at ${EXE}")
endif()
if(NOT EXISTS "${SDL_LIB}")
    message(FATAL_ERROR "BundleSDL: no SDL library at ${SDL_LIB}")
endif()

find_program(INSTALL_NAME_TOOL NAMES install_name_tool)
find_program(OTOOL NAMES otool)
find_program(CODESIGN NAMES codesign)
if(NOT INSTALL_NAME_TOOL OR NOT OTOOL)
    message(FATAL_ERROR
        "BundleSDL: install_name_tool/otool not found (install the Xcode "
        "command line tools: xcode-select --install)")
endif()

# Homebrew hands libSDL3 out through a chain of symlinks (lib/ ->
# Cellar/<version>/lib). file(COPY) preserves symlinks, which would drop a
# dangling link into the bundle, so resolve to the real file first.
get_filename_component(SDL_LIB "${SDL_LIB}" REALPATH)

set(FW_DIR "${APP_BUNDLE}/Contents/Frameworks")
file(MAKE_DIRECTORY "${FW_DIR}")

if(SDL_LIB MATCHES "\\.framework/Versions/[^/]+/[^/]+$")
    # Framework: copy the whole bundle and address the binary inside it.
    # .../SDL3.framework/Versions/<v>/<binary> -> three levels up is the .framework
    get_filename_component(FW_VER_DIR "${SDL_LIB}" DIRECTORY)   # .../Versions/<v>
    get_filename_component(FW_SRC "${FW_VER_DIR}" DIRECTORY)    # .../Versions
    get_filename_component(FW_SRC "${FW_SRC}" DIRECTORY)        # .../SDL3.framework
    get_filename_component(FW_NAME "${FW_SRC}" NAME)            # SDL3.framework
    get_filename_component(FW_VER "${FW_VER_DIR}" NAME)         # A
    # The Mach-O inside is named after the framework without the extension:
    # SDL3.framework/Versions/A/SDL3.
    string(REGEX REPLACE "\\.framework$" "" FW_BASE "${FW_NAME}")
    file(REMOVE_RECURSE "${FW_DIR}/${FW_NAME}")
    file(COPY "${FW_SRC}" DESTINATION "${FW_DIR}")
    set(TARGET_LIB "${FW_DIR}/${FW_NAME}/Versions/${FW_VER}/${FW_BASE}")
    set(INSTALL_REF "@rpath/${FW_NAME}/Versions/${FW_VER}/${FW_BASE}")
else()
    get_filename_component(SDL_NAME "${SDL_LIB}" NAME)     # libSDL3.0.dylib
    file(REMOVE "${FW_DIR}/${SDL_NAME}")
    file(COPY "${SDL_LIB}" DESTINATION "${FW_DIR}")
    set(TARGET_LIB "${FW_DIR}/${SDL_NAME}")
    set(INSTALL_REF "@rpath/${SDL_NAME}")
endif()
if(NOT EXISTS "${TARGET_LIB}")
    message(FATAL_ERROR "BundleSDL: library copy missing at ${TARGET_LIB}")
endif()
# file(COPY) preserves the source's read-only mode; install_name_tool needs write.
file(CHMOD "${TARGET_LIB}" PERMISSIONS
     OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)

# --- 1. give the copy a relocatable install name -----------------------------
execute_process(COMMAND ${INSTALL_NAME_TOOL} -id "${INSTALL_REF}" "${TARGET_LIB}"
                RESULT_VARIABLE rc ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "BundleSDL: failed to set install name: ${err}")
endif()

# --- 2. point the executable at the bundled copy -----------------------------
# Rewrite every SDL load command that is not already @rpath-relative. Matching on
# the file name rather than the exact recorded path keeps this correct when the
# binary was linked against a different prefix, and makes a rerun a no-op.
execute_process(COMMAND ${OTOOL} -L "${EXE}" OUTPUT_VARIABLE otool_out
                ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
string(REPLACE "\n" ";" otool_lines "${otool_out}")
foreach(line ${otool_lines})
    string(STRIP "${line}" line)
    # Lines look like: /opt/homebrew/lib/libSDL3.0.dylib (compat version ...)
    if(line MATCHES "^([^ ]*libSDL3[^ ]*\\.dylib|[^ ]*SDL3\\.framework/[^ ]*) ")
        set(old_ref "${CMAKE_MATCH_1}")
        if(NOT old_ref MATCHES "^@rpath/")
            execute_process(COMMAND ${INSTALL_NAME_TOOL}
                                -change "${old_ref}" "${INSTALL_REF}" "${EXE}"
                            RESULT_VARIABLE rc ERROR_VARIABLE err)
            if(NOT rc EQUAL 0)
                message(FATAL_ERROR "BundleSDL: failed to rewrite ${old_ref}: ${err}")
            endif()
        endif()
    endif()
endforeach()

# CMake also records the dependency's own directory as an rpath (e.g.
# /opt/homebrew/lib). A bundle carrying that silently resolves SDL from the
# build machine instead of itself, which hides packaging mistakes and breaks on
# any other Mac. Drop absolute rpaths that point at a package prefix -- and do
# it *before* adding ours, since the load-command region has a fixed size and
# freeing an entry is what makes room for the new one.
execute_process(COMMAND ${OTOOL} -l "${EXE}" OUTPUT_VARIABLE load_cmds ERROR_QUIET)
string(REGEX MATCHALL "path [^ ]+ \\(offset [0-9]+\\)" rpath_lines "${load_cmds}")
foreach(rp ${rpath_lines})
    if(rp MATCHES "path (/opt/homebrew[^ ]*|/usr/local[^ ]*|/opt/local[^ ]*) ")
        set(bad "${CMAKE_MATCH_1}")
        execute_process(COMMAND ${INSTALL_NAME_TOOL}
                            -delete_rpath "${bad}" "${EXE}"
                        RESULT_VARIABLE rc ERROR_QUIET)
        if(NOT rc EQUAL 0)
            message(WARNING "BundleSDL: could not drop rpath ${bad}")
        endif()
    endif()
endforeach()

# The rpath is what turns @rpath into a real location; without it nothing loads.
if(NOT load_cmds MATCHES "@executable_path/\\.\\./Frameworks")
    execute_process(COMMAND ${INSTALL_NAME_TOOL}
                        -add_rpath "@executable_path/../Frameworks" "${EXE}"
                    RESULT_VARIABLE rc ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR
            "BundleSDL: failed to add the @executable_path/../Frameworks rpath."
            "\nThere was no room left in the binary's load commands:"
            "\n  ${err}"
            "\nReconfigure with the extra linker padding install_name_tool needs:"
            "\n  cmake -DCMAKE_EXE_LINKER_FLAGS=-Wl,-headerpad_max_install_names ...")
    endif()
endif()

# --- 3. ship the license -----------------------------------------------------
# The zlib license has to travel with redistributed copies. Use the repo's
# vendored text, NOT the LICENSE.txt that ships beside a Homebrew SDL3 -- that
# one is Homebrew's own BSD-2 license for the formula, not SDL's.
set(SDL_LICENSE_FOUND "")
foreach(cand "${SDL_LICENSE}" "${CMAKE_CURRENT_LIST_DIR}/../licenses/LICENSE.SDL3.txt")
    if(cand AND EXISTS "${cand}")
        set(SDL_LICENSE_FOUND "${cand}")
        break()
    endif()
endforeach()
if(SDL_LICENSE_FOUND)
    configure_file("${SDL_LICENSE_FOUND}"
                   "${APP_BUNDLE}/Contents/Resources/LICENSE.SDL3.txt" COPYONLY)
else()
    message(WARNING "BundleSDL: SDL3 license not found; pass -DSDL_LICENSE= "
                    "so the zlib license ships with the bundled copy")
endif()

# --- 4. re-sign ad-hoc, inside out -------------------------------------------
if(CODESIGN)
    if(SDL_LIB MATCHES "\\.framework/")
        execute_process(COMMAND ${CODESIGN} --force --sign -
                            "${FW_DIR}/${FW_NAME}"
                        RESULT_VARIABLE rc ERROR_VARIABLE err)
    else()
        execute_process(COMMAND ${CODESIGN} --force --sign - "${TARGET_LIB}"
                        RESULT_VARIABLE rc ERROR_VARIABLE err)
    endif()
    if(NOT rc EQUAL 0)
        message(WARNING "BundleSDL: could not sign the SDL library: ${err}")
    endif()
    execute_process(COMMAND ${CODESIGN} --force --sign - "${APP_BUNDLE}"
                    RESULT_VARIABLE rc ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(WARNING "BundleSDL: could not sign the bundle: ${err}")
    endif()
else()
    message(WARNING "BundleSDL: codesign not found; the bundle may not launch")
endif()
