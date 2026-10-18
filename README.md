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
  progression, plus the LOOP special: an evasive aerobatic barrel that
  grants brief invincibility and clears all enemy fire on screen.
- Four complete stages with distinct terrain, wave scripts, and flagship bosses
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
./build/pacific-squadron          # runs from the build dir; assets are copied there
```

On Linux you may need the SDL3 development package, e.g. `apt install
libsdl3-dev`, or build SDL from source and point CMake at it. At startup the
game disables SDL's HIDAPI-libusb fallback (the hidraw backend covers normal
desktop gamepads); on hosts with unresponsive USB devices the libusb scan can
otherwise freeze launch for many seconds.

The build produces:

| Target          | What it is                                            |
| --------------- | ----------------------------------------------------- |
| `gamecore`      | SDL-free simulation/game logic static library         |
| `pacific-squadron`  | the game (SDL presentation + audio + input)           |
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
WWII_SMOKE=25 SDL_VIDEO_DRIVER=dummy SDL_AUDIODRIVER=dummy ./build/pacific-squadron

# spawn the stage boss after 2 s, grab a screenshot at t=25 s
WWII_SMOKE=30 WWII_SMOKE_BOSS=1 \
WWII_SMOKE_SHOT=/tmp/boss.png WWII_SMOKE_SHOT_AT=25 \
SDL_VIDEO_DRIVER=dummy ./build/pacific-squadron
```

| Variable             | Effect                                            |
| -------------------- | ------------------------------------------------- |
| `WWII_SMOKE`         | seconds to run, autopilot on, then exit 0         |
| `WWII_SMOKE_BOSS`    | spawn the first boss definition after 2 s         |
| `WWII_SMOKE_GOD`     | autopilot flies invincible (full-campaign runs)   |
| `WWII_SMOKE_NOAUTO`  | disable autopilot (just run the title/timer)      |
| `WWII_SMOKE_SHOT`    | path to write a PNG screenshot                    |
| `WWII_SMOKE_SHOT_AT` | time (s) at which to capture it (default 10)      |
| `WWII_START_STAGE`   | begin at stage *n* (1-based). With no `WWII_SMOKE` this is a normal hands-on playthrough of that stage; with `WWII_SMOKE` the autopilot starts there too |

Note: run smoke durations that cross a stage's scripted `bossTime` to
exercise the natural boss-spawn path in stage scripts (stage 1: 86 s,
stage 2: 94 s, stage 3: 102 s) — e.g. `WWII_SMOKE=95` covers stage 1's
boss arrival, or `WWII_SMOKE=115 WWII_START_STAGE=3` jumps straight to
stage 3 and runs through its boss fight.

## Controls (defaults, rebindable in Options)

| Action        | Keyboard              | Gamepad                        |
| ------------- | --------------------- | ------------------------------ |
| Move          | WASD / Arrows         | Left stick                     |
| Fire          | Space                 | A (South) / Right shoulder     |
| Loop (special)| Shift / Ctrl          | B (East) / Left shoulder       |
| Pause         | Esc / P               | Start                          |
| Confirm / Back| Enter / Space, Esc    | A / B, Select                  |
| Debug overlay | F3                    | —                              |

Bindings, audio volumes, difficulty, and display mode persist to the
platform preference directory (`SDL_GetPrefPath`).

## Window & scaling

The window is resizable; the 384×512 logical canvas is scaled
nearest-neighbour and centred. When the window is close to an integer
multiple (1x/2x/3x/4x) it snaps to exactly that for uniform pixels; at
awkward sizes it fills the window with fractional nearest-neighbour
scaling. Fullscreen (Options) uses the desktop resolution.

## Tuning

Player-feel constants live in one file: **`src/game/Tuning.h`**
(movement speed, hitbox size, invulnerability windows, special duration,
respawn behaviour, loop-charge cap). Per-weapon fire intervals/damage/speed and
per-stage scroll speed are data-driven:
`assets/data/weapons/*.json` and `assets/data/stages/*.json`.

Current defaults: player speed 230 px/s (instant start/stop, normalised
diagonals), 10 px hitbox, gun interval 0.14–0.10 s, bullet speed
380–560 px/s, stage scroll 60 px/s, enemy bullets 80–170 px/s (always
slower than the player so dodging is intentional).

## Developer tools

- **F3** toggles the debug overlay (FPS/frame time, entity/enemy/projectile
  /particle counts, player position & speed, scroll position, stage/state,
  weapon, HP/loop charges/lives, and every live collider: cyan player hitbox,
  red enemy hitboxes, magenta boss hull, yellow boss components, projectile
  boxes). Also enable at launch with `WWII_DEBUG=1`.
- `WWII_KEYLOG=1` logs received key events and periodic FPS/frame counts to
  the console (input/perf triage).
- `WWII_SMOKE*` headless smoke hooks (table above).

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

Developed and validated on Linux. Automated verification uses the dummy
video/audio driver, the unit-test suite, the smoke-test hooks above, and
programmatic pixel inspection of captured screenshots. Desktop runtime was
verified on KDE/Wayland via Xwayland: window creation, resize (integer snap +
letterbox), fullscreen at native resolution, WM close, and steady ~60 fps at
~3 % CPU. Keyboard/gamepad feel still needs a human at the machine.
