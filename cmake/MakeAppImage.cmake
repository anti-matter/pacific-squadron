# Build an AppImage from the staged install tree.
#
# Configure-time wrapper: the `appimage` custom target invokes this with
#   cmake -DBUILD_DIR=<cmake build dir> -DSRC_DIR=<repo>
#        -DOUT=<output .AppImage path> [-DAPPIMAGETOOL=<path>]
#        -DARCH=<arch> -DVERSION=<ver> [-DAPPIMAGE_UPDATE_INFO=<str>]
#        -P cmake/MakeAppImage.cmake
#
# Steps: stage `cmake --install --prefix /usr --destdir <AppDir>` (which
# already carries the bundled SDL3 via the $ORIGIN rpath and the assets +
# .desktop + hicolor icons), add the AppRun launcher and the AppDir-root
# .desktop/icon that appimagetool expects, then fuse it into one file.
#
# appimagetool is a standalone static binary (not packaged by distros) from
# the maintained AppImage/appimagetool repo -- NOT the archived AppImageKit
# one, whose runtime dynamically loads libfuse.so.2 (dropped by most current
# distros, so the produced AppImage would not double-click). The maintained
# tool embeds the static musl runtime from AppImage/type2-runtime, which
# needs no libfuse on the target:
#   https://github.com/AppImage/appimagetool/releases
# Drop it on PATH or pass -DAPPIMAGETOOL=/path/to/appimagetool.
# Offline builds: -DAPPIMAGE_RUNTIME=/path/to/runtime-<arch> pins the runtime
# (else appimagetool downloads the latest type2-runtime itself).

foreach(req BUILD_DIR SRC_DIR OUT ARCH)
    if(NOT DEFINED ${req} OR "${${req}}" STREQUAL "")
        message(FATAL_ERROR "MakeAppImage.cmake: -D${req}=... is required")
    endif()
endforeach()

if(NOT APPIMAGETOOL)
    find_program(APPIMAGETOOL appimagetool)
endif()
if(NOT APPIMAGETOOL)
    message(FATAL_ERROR
        "appimagetool not found. Download the static binary from the\n"
        "maintained repo (NOT the archived AppImageKit one -- its runtime\n"
        "requires libfuse.so.2 and the produced AppImage will not\n"
        "double-click on modern distros):\n"
        "  https://github.com/AppImage/appimagetool/releases\n"
        "(chmod +x, put it on PATH) or configure with\n"
        "  -DAPPIMAGETOOL=/path/to/appimagetool\n"
        "then re-run: cmake --build ${BUILD_DIR} --target appimage")
endif()

set(APP_DIR "${BUILD_DIR}/appimage")

# 1. Fresh staged tree: AppDir/usr/{bin,share,lib}
file(REMOVE_RECURSE "${APP_DIR}")
execute_process(COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}"
                --prefix /usr
                ENVIRONMENT "DESTDIR=${APP_DIR}"
                RESULT_VARIABLE ps_rc)
if(NOT ps_rc EQUAL 0)
    message(FATAL_ERROR "staging install failed (${ps_rc}); build first")
endif()

# 2. AppRun launcher at the AppDir root (the AppImage runtime execs this).
#    Assets resolve through the binary's <exe>/../share probe; the LD_LIBRARY_PATH
#    export covers the bundled library dir for libSDL3's OWN dependencies
#    (DT_RUNPATH is not transitive; system libraries still win first because
#    the bundled dir only ever holds libSDL3 itself).
file(WRITE "${APP_DIR}/AppRun"
"#!/bin/sh
here=\"$(cd \"$(dirname \"$(readlink -f \"$0\")\")\" && pwd)\"
LD_LIBRARY_PATH=\"$here/usr/lib/pacific-squadron\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}\"
export LD_LIBRARY_PATH
exec \"$here/usr/bin/pacific-squadron\" \"$@\"
")
execute_process(COMMAND chmod +x "${APP_DIR}/AppRun" COMMAND_ERROR_IS_FATAL ANY)

# 3. appimagetool convention: <name>.desktop + matching icon at the root.
file(COPY "${SRC_DIR}/packaging/pacific-squadron.desktop"
     DESTINATION "${APP_DIR}")
file(COPY "${SRC_DIR}/assets/icons/pacific-squadron-256x256.png"
     DESTINATION "${APP_DIR}")
file(RENAME "${APP_DIR}/pacific-squadron-256x256.png"
            "${APP_DIR}/pacific-squadron.png")

# 4. Fuse. APPIMAGE_EXTRACT_AND_RUN lets appimagetool itself run on systems
#    without FUSE (CI containers); it does not affect the produced AppImage.
set(ps_runtime "")
if(APPIMAGE_RUNTIME)
    if(NOT EXISTS "${APPIMAGE_RUNTIME}")
        message(FATAL_ERROR "APPIMAGE_RUNTIME=${APPIMAGE_RUNTIME} does not exist")
    endif()
    set(ps_runtime "--runtime-file=${APPIMAGE_RUNTIME}")
endif()
# Update information (empty = embed nothing). Read back from the ELF by
# AppImageUpdate/Gearlever/zypper to check for new releases.
set(ps_update "")
if(APPIMAGE_UPDATE_INFO)
    set(ps_update "--updateinformation=${APPIMAGE_UPDATE_INFO}")
endif()
# Validate the staged AppStream metainfo ourselves, offline. appimagetool's
# built-in check does ONLINE URL reachability probes and treats even
# warnings as fatal -- an unpublished repo or a sandboxed CI would fail the
# pack for reasons unrelated to the package. --no-net keeps the structural
# validation (id/filename/license/urls-format) deterministic.
find_program(APPSTREAM_CLI appstreamcli)
file(GLOB ps_metainfo "${APP_DIR}/usr/share/metainfo/*.xml")
if(APPSTREAM_CLI AND ps_metainfo)
    execute_process(COMMAND "${APPSTREAM_CLI}" validate --no-net ${ps_metainfo}
                    RESULT_VARIABLE ps_as_rc)
    if(NOT ps_as_rc EQUAL 0)
        message(FATAL_ERROR "AppStream metainfo failed appstreamcli validation")
    endif()
endif()
get_filename_component(ps_out_dir "${OUT}" DIRECTORY)
file(MAKE_DIRECTORY "${ps_out_dir}")
# VERSION makes the tool stamp X-AppImage-Version into the root desktop
# file, which is where app-integration daemons read the package version.
# -n: skip appimagetool's own (online) AppStream check -- done above.
execute_process(COMMAND "${APPIMAGETOOL}" ${ps_runtime} ${ps_update} -n "${APP_DIR}" "${OUT}"
                ENVIRONMENT "ARCH=${ARCH}"
                ENVIRONMENT "VERSION=${VERSION}"
                ENVIRONMENT "APPIMAGE_EXTRACT_AND_RUN=1"
                RESULT_VARIABLE ps_rc)
if(NOT ps_rc EQUAL 0)
    message(FATAL_ERROR "appimagetool failed (${ps_rc})")
endif()
message(STATUS "AppImage written: ${OUT}")
