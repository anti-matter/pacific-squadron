# Pacific Squadron

An original WWII 2D vertical-scrolling arcade shooter (1942-style), built with
modern **C++20**, **SDL3**, and **CMake**. Every sprite, sound effect, and
music track is generated procedurally at load time — the game ships with zero
binary art assets and is fully playable out of the box. Drop PNG/WAV files
into `assets/` to override the procedural content.

## Features

- **384×512 logical resolution** with integer scaling and nearest-neighbor
  filtering; windowed or fullscreen.
- **Data-driven content** — weapons, enemies, formations, flight paths,
  power-ups, bosses, animations, and stage scripts are plain JSON in
  `assets/data/`.
- **Modular multi-component bosses**: armored hull plus destructible
  turrets/gun emplacements, hp-based attack phases, death chains, and victory
  power-up drops.
- **Four player weapon classes** (gun / spread / cannon / rocket) with level
  progression and smart bombs.
- Two full stages with distinct terrain, wave scripts, and flagship bosses
  (*Battleship Yamashiro-class*, *Sky Fortress GX-7*).
- Configurable keyboard **and** gamepad controls (persisted to disk), three
  difficulty levels, high-score table with initials, pause, options, and
  settings screens.
- **Deterministic fixed-step simulation** (60 Hz) fully decoupled from SDL,
  covered by unit tests and drivable headless for automated smoke tests.

## Building

Requirements: CMake ≥ 3.16, a C++20 compiler (GCC or Clang; MSVC should work),
and [SDL3](https://github.com/libsdl-org/SDL) (installed or via
`CMAKE_PREFIX_PATH`). All other dependencies (miniaudio, stb, nlohmann/json)
are vendored in `third_party/`.

```sh
cmake -S . -B build
cmake --build build -j
./build/wwii-fighter          # runs from the build dir; assets are copied there
```

On Linux you may need the SDL3 development package, e.g. `apt install
libsdl3-dev`, or build SDL from source and point CMake at it.

The build produces:

| Target          | What it is                                            |
| --------------- | ----------------------------------------------------- |
| `gamecore`      | SDL-free simulation/game logic static library         |
| `wwii-fighter`  | the game (SDL presentation + audio + input)           |
| `wwii_tests`    | unit tests (also registered with CTest)               |

Optional: `-DWWII_ENABLE_SANITIZERS=address,undefined` enables sanitizers.
`gamecore` and all first-party code compile warning-clean with GCC and Clang
at high warning levels; `third_party` is compiled with warnings suppressed.

## Testing

```sh
ctest --test-dir build --output-on-failure
# or directly:
./build/wwii_tests
```

103 checks cover movement, weapons/firing cadence, formations, flight paths,
collision broadphase + resolution, scoring, level/stage JSON parsing and
linking, power-ups, boss lifecycle (turrets → armor → hull → death chain →
power-up drop), and difficulty modifiers.

### Headless smoke test

The binary supports an env-var driven autopilot for automated validation
(no display or gamepad needed):

```sh
# 25 s of gameplay on the dummy video/audio driver
WWII_SMOKE=25 SDL_VIDEO_DRIVER=dummy SDL_AUDIODRIVER=dummy ./build/wwii-fighter

# spawn the stage boss after 2 s, grab a screenshot at t=25 s
WWII_SMOKE=30 WWII_SMOKE_BOSS=1 \
WWII_SMOKE_SHOT=/tmp/boss.png WWII_SMOKE_SHOT_AT=25 \
SDL_VIDEO_DRIVER=dummy ./build/wwii-fighter
```

| Variable             | Effect                                            |
| -------------------- | ------------------------------------------------- |
| `WWII_SMOKE`         | seconds to run, autopilot on, then exit 0         |
| `WWII_SMOKE_BOSS`    | spawn the first boss definition after 2 s         |
| `WWII_SMOKE_NOAUTO`  | disable autopilot (just run the title/timer)      |
| `WWII_SMOKE_SHOT`    | path to write a PNG screenshot                    |
| `WWII_SMOKE_SHOT_AT` | time (s) at which to capture it (default 10)      |

## Controls (defaults, rebindable in Options)

| Action        | Keyboard              | Gamepad                        |
| ------------- | --------------------- | ------------------------------ |
| Move          | WASD / Arrows         | Left stick                     |
| Fire          | Space                 | A (South) / Right shoulder     |
| Smart bomb    | Shift / Ctrl          | B (East) / Left shoulder       |
| Pause         | Esc / P               | Start                          |
| Confirm / Back| Enter / Space, Esc    | A / B, Select                  |

Bindings, audio volumes, difficulty, and display mode persist to the
platform preference directory (`SDL_GetPrefPath`).

## Architecture

```
src/
  core/       math, RNG, AABB spatial grid (no SDL, no game knowledge)
  game/       deterministic simulation: entities, weapons, formations,
              movement patterns, level scripting, bosses, scoring (no SDL)
  graphics/   SDL3 renderer wrapper, procedural sprite art, bitmap font
  audio/      miniaudio wrapper + procedural SFX/chiptune synthesis
  input/      keyboard/gamepad bindings, persistence
  ui/         HUD and screen widgets
  app/        game state machine glue + main loop
assets/data/  JSON content (weapons, enemies, paths, powerups, bosses,
              animations, stages)
third_party/  vendored single-header libraries
tests/        lightweight unit-test framework + suites
```

Design notes:

- **Simulation is SDL-free and deterministic.** `Simulation` runs a fixed
  60 Hz step over a pooled entity array (one struct + data-driven defs, no
  per-enemy class hierarchy). Bullets are entities; movement patterns
  (dive, sweep, loop, formation, …) are enums dispatched in
  `advanceMovement()`. Events (explosions, pickups, boss phases) surface
  through a queue consumed by the presentation layer, which maps them to
  sounds and score popups.
- **Two-phase collision.** A uniform-grid broadphase emits candidate pairs;
  resolution collects pending hits and applies them with alive-checks. An
  armored boss hull sparks but does **not** consume bullets while
  destructible components live — bullets fly on to hit the turrets nested
  inside the hull's outline. The hull itself only takes damage after all
  destructible components are destroyed.
- **Procedural-first assets.** `ProceduralArt.cpp` draws every plane, ship,
  turret, bullet, explosion frame, and terrain tile into 32-bit RGBA buffers;
  `ProceduralSfx.cpp` synthesizes all sounds and the chiptune soundtrack.
  PNG overrides in `assets/sprites` / `assets/tiles` and WAV overrides for
  sounds are picked up automatically if present.
- **Portability.** Fixed-width integers, `std::filesystem` paths, no
  compiler-specific extensions, platform code confined to SDL calls.

## Status (spec phases)

| Phase | Scope                                   | Status |
| ----- | --------------------------------------- | ------ |
| 1     | Foundation: build, window, loop, input  | ✅     |
| 2     | Playable prototype: player, shots, one enemy, collision | ✅ |
| 3     | Core systems: data-driven content, power-ups, scoring, lives, HUD | ✅ |
| 4     | World & content: terrain, stages, formations, path library | ✅ |
| 5     | Boss system: components, phases, death chain | ✅  |
| 6     | Polish: menus, options, difficulty, high scores, audio, persistence | ✅ |

## Validation notes

Developed and validated on Linux. Automated verification in headless CI-style
environments uses the dummy video/audio driver, the unit-test suite, and the
smoke-test hooks above, plus programmatic pixel checks of captured
screenshots. Interactive play (GPU renderer, real gamepad) has not been
verified in this environment.
