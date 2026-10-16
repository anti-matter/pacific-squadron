cmake_minimum_required(VERSION 3.20)

project(wwii_fighter
    VERSION 0.1.0
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
add_executable(wwii-fighter
    src/main.cpp
    src/app/Application.cpp
    src/graphics/Renderer.cpp
    src/graphics/BitmapFont.cpp
    src/graphics/ProceduralArt.cpp
    src/audio/Audio.cpp
    src/audio/ProceduralSfx.cpp
    src/input/Input.cpp
    src/ui/Hud.cpp)
target_link_libraries(wwii-fighter PRIVATE
    gamecore third_party SDL3::SDL3 nlohmann_json)
target_compile_features(wwii-fighter PRIVATE cxx_std_20)
target_set_warnings_runtime(wwii-fighter)

# Copy assets next to the binary so the game runs from the build dir.
add_custom_command(TARGET wwii-fighter POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/assets
        $<TARGET_FILE_DIR:wwii-fighter>/assets)

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
    tests/test_boss.cpp)
target_link_libraries(wwii_tests PRIVATE gamecore)
target_set_warnings(wwii_tests)
add_test(NAME unit_tests COMMAND wwii_tests)
