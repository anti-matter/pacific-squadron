cmake_minimum_required(VERSION 3.20)

# Release version: THE single source of truth for the whole project. Bump the
# three numbers here and nothing else -- cmake/version.h.in turns it into
# WW_VERSION_STRING for the splash screen, and the same value feeds the macOS
# bundle plist (CFBundleVersion / CFBundleShortVersionString). Format is
# major.minor.bugfix.
project(wwii_fighter
    VERSION 1.0.0
    DESCRIPTION "WWII-inspired 2D vertical-scrolling arcade shooter"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Warnings)
include(Sanitizers)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
find_package(SDL3 REQUIRED CONFIG)

add_library(third_party STATIC
    third_party/miniaudio/miniaudio_impl.c
    third_party/stb/stb_image_impl.c
    third_party/stb/stb_image_write_impl.c)
target_include_directories(third_party PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniaudio
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb)
target_compile_features(third_party PUBLIC cxx_std_20)
if(UNIX)
    target_link_libraries(third_party PUBLIC m pthread dl)
elseif(WIN32)
    target_link_libraries(third_party PUBLIC ole32)
endif()
target_warnings_off(third_party)

add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

# ---------------------------------------------------------------------------
# Core game logic (SDL-free, unit-testable)
# ---------------------------------------------------------------------------
add_library(gamecore STATIC
    src/core/Vec2.cpp
    src/core/Collision.cpp
    src/core/Rng.cpp
    src/game/GameData.cpp
    src/game/Game.cpp
    src/game/LevelScript.cpp
    src/game/Formation.cpp
    src/game/Movement.cpp
    src/game/Simulation.cpp)
target_include_directories(gamecore PUBLIC src)
target_link_libraries(gamecore PUBLIC nlohmann_json)
target_compile_features(gamecore PUBLIC cxx_std_20)
target_set_warnings(gamecore)

# ---------------------------------------------------------------------------
# Game executable
# ---------------------------------------------------------------------------
add_executable(pacific-squadron
    src/main.cpp
    src/app/Application.cpp
    src/graphics/Renderer.cpp
    src/graphics/BitmapFont.cpp
    src/graphics/ProceduralArt.cpp
    src/graphics/EnvironmentArt.cpp
    src/graphics/Environment.cpp
    src/audio/Audio.cpp
    src/audio/ProceduralSfx.cpp
    src/input/Input.cpp
    src/ui/Hud.cpp)
target_link_libraries(pacific-squadron PRIVATE
    gamecore third_party SDL3::SDL3 nlohmann_json)
target_compile_features(pacific-squadron PRIVATE cxx_std_20)
target_set_warnings_runtime(pacific-squadron)

# Build identity: git hash baked into the binary (debug startup log).
add_custom_target(buildinfo ALL
    COMMAND ${CMAKE_COMMAND}
        -DREPO_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DBUILD_INFO_HEADER=${CMAKE_CURRENT_BINARY_DIR}/generated/buildinfo.h
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenVersion.cmake
    BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/generated/buildinfo.h
    COMMENT "Generating buildinfo.h")
add_dependencies(pacific-squadron buildinfo)
target_include_directories(pacific-squadron PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)

# Release version header (splash screen "Version x.y.z"). Guarded so a
# two-component VERSION can't silently ship as e.g. "1.0".
if(NOT PROJECT_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR
        "project VERSION must be major.minor.bugfix (got '${PROJECT_VERSION}')")
endif()
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.h.in
               ${CMAKE_CURRENT_BINARY_DIR}/generated/version.h @ONLY)

# Declared before the asset copy below, which needs to know the bundle layout.
if(APPLE)
    option(PACIFIC_SQUADRON_MAC_BUNDLE "Package the executable as a macOS .app bundle" ON)
endif()

# Copy runtime assets so the game runs straight out of the build tree.
# cmake/CopyAssets.cmake copies only what the game loads (data, icons,
# optional sprite/tile overrides, splash.png) -- the source tree also holds
# multi-MB dev-only artwork that must not ship, and `cmake -E copy_directory`
# cannot exclude it. For a macOS .app the assets go into Contents/Resources
# (the idiomatic location, and what SDL_GetBasePath() reports); otherwise they
# land next to the executable.
if(APPLE AND PACIFIC_SQUADRON_MAC_BUNDLE)
    set(ASSET_DEST $<TARGET_BUNDLE_CONTENT_DIR:pacific-squadron>/Resources/assets)
else()
    set(ASSET_DEST $<TARGET_FILE_DIR:pacific-squadron>/assets)
endif()
add_custom_command(TARGET pacific-squadron POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        -DCOPY_SRC=${CMAKE_CURRENT_SOURCE_DIR}/assets
        -DCOPY_DEST=${ASSET_DEST}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CopyAssets.cmake
    COMMENT "Copying runtime assets")

# ---------------------------------------------------------------------------
# Application icon (assets/icons/ -- see tools/make_icons.py)
# ---------------------------------------------------------------------------
set(ICON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/assets/icons)
set(GAME_ICO ${ICON_DIR}/pacific-squadron.ico)
set(GAME_ICNS ${ICON_DIR}/pacific-squadron.icns)

# Regenerate the whole pack from the master artwork: cmake --build . --target icons
# (opt-in maintenance task -- the generated icons are committed, so a normal
# build never needs Python or Pillow.)
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    execute_process(COMMAND ${Python3_EXECUTABLE} -c "import numpy, PIL"
                    RESULT_VARIABLE _icons_py_ok OUTPUT_QUIET ERROR_QUIET)
    if(_icons_py_ok EQUAL 0)
        add_custom_target(icons
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/make_icons.py
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            COMMENT "Regenerating icon pack from assets/app-icon-base.png"
            VERBATIM)
    else()
        message(STATUS "icons target skipped: python3 lacks Pillow/numpy "
                       "(pip install pillow numpy to enable)")
    endif()
endif()

if(WIN32)
    # Bake the .ico into the executable so Explorer/taskbar/Alt+Tab use it.
    # The path is configured absolute because windres resolves relative paths
    # against the build directory, not the .rc file's own directory.
    set(PACIFIC_SQUADRON_ICO ${GAME_ICO})
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/packaging/pacific-squadron.rc.in
                   ${CMAKE_CURRENT_BINARY_DIR}/pacific-squadron.rc @ONLY)
    target_sources(pacific-squadron PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/pacific-squadron.rc)
endif()

if(APPLE AND PACIFIC_SQUADRON_MAC_BUNDLE)
    # The dock icon comes from the bundle, not from SDL_SetWindowIcon, so the
    # .icns has to be a bundle resource. Default ON on macOS: assets resolve
    # via SDL_GetBasePath() (Contents/Resources), which is where the POST_BUILD
    # copy puts them, so running the .app from the build tree -- or from
    # Finder -- works. Disable with -DPACIFIC_SQUADRON_MAC_BUNDLE=OFF for a
    # bare executable.
    set_source_files_properties(${GAME_ICNS} TARGET_DIRECTORY pacific-squadron
        PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    target_sources(pacific-squadron PRIVATE ${GAME_ICNS})
    set_target_properties(pacific-squadron PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_ICON_FILE pacific-squadron.icns
        MACOSX_BUNDLE_BUNDLE_NAME "Pacific Squadron"
        MACOSX_BUNDLE_GUI_IDENTIFIER com.pacificsquadron.game
        MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION})

    # Ship SDL3 inside the .app so it runs on Macs without Homebrew. The
    # Homebrew dylib's install name is its absolute build-machine path;
    # cmake/BundleSDL.cmake copies it into Contents/Frameworks, repoints the
    # executable at @rpath (adding an @executable_path/../Frameworks rpath and
    # dropping Homebrew's), ships the zlib license, and re-signs ad-hoc.
    # Disable with -DPACIFIC_SQUADRON_BUNDLE_SDL=OFF to keep linking against the
    # system/Homebrew copy (smaller bundle, needs SDL3 installed).
    option(PACIFIC_SQUADRON_BUNDLE_SDL
           "Embed the SDL3 dylib in the macOS .app bundle" ON)
    get_target_property(SDL3_TARGET_TYPE SDL3::SDL3 TYPE)
    if(PACIFIC_SQUADRON_BUNDLE_SDL AND SDL3_TARGET_TYPE STREQUAL "SHARED_LIBRARY")
        add_custom_command(TARGET pacific-squadron POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                -DAPP_BUNDLE=$<TARGET_BUNDLE_DIR:pacific-squadron>
                -DSDL_LIB=$<TARGET_FILE:SDL3::SDL3>
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/BundleSDL.cmake
            COMMENT "Bundling SDL3 into the .app")
    elseif(PACIFIC_SQUADRON_BUNDLE_SDL)
        message(STATUS "SDL bundling skipped: SDL3::SDL3 is not a shared "
                       "library (${SDL3_TARGET_TYPE})")
    endif()
endif()

# ---------------------------------------------------------------------------
# Install (Linux/Unix layout; icons registered with the hicolor theme)
# ---------------------------------------------------------------------------
if(APPLE AND PACIFIC_SQUADRON_MAC_BUNDLE)
    # A MACOSX_BUNDLE target needs a BUNDLE destination; `make install` drops
    # Pacific Squadron.app in the install prefix (e.g. /usr/local or /opt/homebrew).
    install(TARGETS pacific-squadron BUNDLE DESTINATION . RUNTIME DESTINATION bin)
else()
    install(TARGETS pacific-squadron RUNTIME DESTINATION bin)
endif()
if(UNIX AND NOT APPLE)
    # Runtime assets beside the installed binary's share dir so the .desktop
    # launcher works from any cwd (the app also probes <exe>/../share/...).
    # Dev reference sheets and the icon master are not needed at runtime.
    install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/assets/
            DESTINATION share/pacific-squadron/assets
            PATTERN "P-38.png" EXCLUDE
            PATTERN "zero.png" EXCLUDE
            PATTERN "app-icon-base.png" EXCLUDE
            PATTERN "Pacific Squadron*" EXCLUDE)
    foreach(size 16 24 32 48 64 128 256 512 1024)
        install(FILES ${ICON_DIR}/pacific-squadron-${size}x${size}.png
                DESTINATION share/icons/hicolor/${size}x${size}/apps
                RENAME pacific-squadron.png)
    endforeach()
    install(FILES ${GAME_ICO} DESTINATION share/pacific-squadron)
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/packaging/pacific-squadron.desktop
            DESTINATION share/applications)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()
add_executable(wwii_tests
    tests/test_main.cpp
    tests/test_movement.cpp
    tests/test_collision.cpp
    tests/test_weapons.cpp
    tests/test_formation.cpp
    tests/test_scoring.cpp
    tests/test_levelscript.cpp
    tests/test_boss.cpp
    tests/test_ground_scroll.cpp
    tests/test_coastline.cpp
    tests/test_data.cpp
    tests/test_input.cpp
    tests/test_sfx.cpp
    tests/test_menu.cpp)
target_link_libraries(wwii_tests PRIVATE gamecore SDL3::SDL3)
target_sources(wwii_tests PRIVATE src/audio/ProceduralSfx.cpp src/input/Input.cpp)
target_set_warnings(wwii_tests)
add_test(NAME unit_tests COMMAND wwii_tests)
