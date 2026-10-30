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
  progression, plus the BOMB special: destroys every enemy aircraft, ship and
  ground target on screen and erases all enemy fire (flagship bosses only
  lose 20% of their hull/components), with brief invincibility.
- Ten complete stages with distinct terrain, wave scripts, and flagship
  bosses (*Battleship Yamashiro-class* through *Sky Fortress Omega*).
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
libsdl3-dev`, or build SDL from source and point CMake at it. On macOS install
it with `brew install sdl3` — that is only needed to *build*, though: the
resulting `.app` carries its own copy of SDL3 (see below), so it runs on Macs
that have nothing installed. On macOS the build produces a
`pacific-squadron.app` bundle (display name "Pacific Squadron") rather than a
bare executable, and CMake's `MACOSX_BUNDLE` nests the binary inside it, so run
`open build/pacific-squadron.app` or
`./build/pacific-squadron.app/Contents/MacOS/pacific-squadron` — the latter
keeps the startup log in your terminal. Configure with
`-DPACIFIC_SQUADRON_MAC_BUNDLE=OFF` for a plain `build/pacific-squadron`.

### Self-contained macOS bundle

`cmake/BundleSDL.cmake` copies the SDL3 dylib into
`pacific-squadron.app/Contents/Frameworks`, rewrites the executable to load it
from `@rpath` (and drops the Homebrew rpath CMake leaves behind, so a missing
bundle library fails loudly instead of quietly resolving on the build machine),
ships SDL's zlib license — the vendored `licenses/LICENSE.SDL3.txt`, since the
`LICENSE.txt` beside a Homebrew SDL3 is Homebrew's own license, not SDL's — as
`Resources/LICENSE.SDL3.txt`, and re-signs everything ad-hoc — required on
arm64, since rewriting load commands invalidates the linker signature. The
`.app` is then relocatable: drag it to another Mac, or anywhere on disk, and it
launches. Opt out with
`-DPACIFIC_SQUADRON_BUNDLE_SDL=OFF` to link against the system copy instead
(a ~2.4 MB smaller bundle, but SDL3 must be installed).

Ad-hoc signing means the bundle runs fine on your own machine but Gatekeeper
will block it when downloaded from elsewhere — that needs a Developer ID
certificate and notarization, which is out of scope here. Locally, `open`
works; if quarantine is applied, right-click → Open.

### Linux distributions (zip/tarball and AppImage)

Linux mirrors the macOS embedding: with `PACIFIC_SQUADRON_BUNDLE_SDL=ON`
(the default), the install tree carries the SDL3 shared library under
`lib/pacific-squadron/` (copied under its SONAME by
`cmake/BundleSDLUnix.cmake`) and the binary's install rpath becomes
`$ORIGIN/../lib/pacific-squadron`, so an extracted package runs on machines
without SDL3. Two artifact families are cut from that same install tree:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
cpack --config build/CPackConfig.cmake -G "TGZ;ZIP"   # zip + tar.gz in build/
cmake --build build --target appimage  # single-file AppImage in build/
```

- **zip / tar.gz** (`pacific-squadron-<ver>-linux-<arch>.{zip,tar.gz}` via
  CPack) — extract, run `bin/pacific-squadron` directly, or
  `sh share/pacific-squadron/install.sh` to install into `~/.local` (pass a
  prefix to install elsewhere, `--uninstall` to remove). The archive root
  carries a generated `README.txt` and SDL3's `LICENSE.txt`.
- **AppImage** (`Pacific-Squadron-<ver>-<arch>.AppImage`) — one file,
  `chmod +x` and run. Building one needs
  [`appimagetool`](https://github.com/AppImage/appimagetool/releases)
  (a standalone static binary; put it on `PATH` or configure
  `-DAPPIMAGETOOL=/path/to/appimagetool`). Use that maintained repo, **not**
  the archived AppImageKit build: the maintained tool embeds the static
  musl [`type2-runtime`](https://github.com/AppImage/type2-runtime), so the
  produced AppImage needs no `libfuse2`/`libfuse3` on the user's system and
  runs from a plain double-click even on FUSE-less distros. Offline builds
  can pin a downloaded runtime with
  `-DAPPIMAGE_RUNTIME=/path/to/runtime-x86_64`.

Build release artifacts on the oldest distro you want to support: binaries
only run where glibc is at least as new as the build machine's — a container
build is the usual answer, and both artifact families should come from the
same one.

At startup the
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

### Version number

The release version is `major.minor.bugfix` and lives in exactly one place:
the `VERSION` field of `project(wwii_fighter ...)` at the top of
`CMakeLists.txt`.

```cmake
project(wwii_fighter
    VERSION 1.0.0        # <-- bump this line and nothing else
    ...)
```

Re-run `cmake -S . -B build` after editing it. That single value becomes:

- `VERSION x.y.z` on the splash screen, under the subtitle (from the generated
  `build/generated/version.h`);
- `CFBundleVersion` / `CFBundleShortVersionString` in the macOS bundle's
  `Info.plist`, so Finder's Get Info and the App Store tooling agree;
- the version in the debug startup log (`PACIFIC SQUADRON 1.0.0 build <hash>`).

CMake refuses to configure if the version is not three dot-separated numbers.

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
| `WWII_SMOKE_BOMB_AT` | comma-separated sim times (s) at which to press the BOMB, e.g. `8,9.5` |
| `WWII_SMOKE_SPLASH_SHOT` | path to write a PNG of the finished loading splash (skips the press-any-key gate, like all `WWII_SMOKE` runs) |
| `WWII_START_STAGE`   | begin at stage *n* (1-based). With no `WWII_SMOKE` this is a normal hands-on playthrough of that stage; with `WWII_SMOKE` the autopilot starts there too |
| `WWII_BOSS_NOW`      | **not smoke-only** — works in normal runs too: trigger the current stage's own boss ~1 s after the intro (optionally `WWII_BOSS_NOW=<delay s>`). Combine with `WWII_START_STAGE=n` to jump straight to stage *n*'s boss, e.g. `WWII_START_STAGE=5 WWII_BOSS_NOW ./pacific-squadron`. Unlike `WWII_SMOKE_BOSS` (a boss definition by index), the fight, health bar and escorts are exactly as that stage scripts them; applies to the stage you start on, later stages keep their scripted `bossTime` |

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
| Bomb (special)| Shift / Ctrl          | B (East) / Left shoulder       |
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
respawn behaviour, bomb-charge cap). Per-weapon fire intervals/damage/speed and
per-stage scroll speed are data-driven:
`assets/data/weapons/*.json` and `assets/data/stages/*.json`.

Current defaults: player speed 230 px/s (instant start/stop, normalised
diagonals), 10 px hitbox, gun interval 0.14–0.10 s, bullet speed
380–560 px/s, stage scroll 60 px/s, enemy bullets 80–170 px/s (always
slower than the player so dodging is intentional).

## Startup & loading

A splash screen (title stack, progress bar with phase labels) is
presented as soon as the window exists; every heavy step — gamepad and
audio device probes, procedural art baking, music synthesis — runs behind
it. Game audio (music and SFX) stays silent until the first real frame
has been presented, so nothing is ever heard from behind an unopened
window.

All art and audio are generated deterministically, then dumped to
`art_cache.bin` / `sfx_cache.bin` in the preferences directory on the
first run; later launches upload straight from the caches, which makes
the generated content cost nearly nothing at startup.

- `WWII_NOCACHE=1` ignores both caches and regenerates from source (the
  caches are then rewritten).
- The caches are version-stamped: when procedural art changes, bump
  `kArtCacheVersion` in `src/graphics/ProceduralArt.cpp`; when synth code
  or tracks change, bump `kSfxCacheVersion` in `src/audio/ProceduralSfx.cpp`.
  Damaged or truncated cache files fall back to a full regeneration on
  their own.
- Debug builds log `[startup]` phase timings to the console; if a machine
  loads slowly, the log names the phase. Release builds
  (`-DCMAKE_BUILD_TYPE=Release`) run the remaining generation several
  times faster.

## Developer tools

- **F3** toggles the debug overlay (FPS/frame time, entity/enemy/projectile
  /particle counts, player position & speed, scroll position, stage/state,
  weapon, HP/bomb charges/lives, and every live collider: cyan player hitbox,
  red enemy hitboxes, magenta boss hull, yellow boss components, projectile
  boxes). Also enable at launch with `WWII_DEBUG=1`.
- `WWII_KEYLOG=1` logs received key events and periodic FPS/frame counts to
  the console (input/perf triage).
- `WWII_SMOKE*` headless smoke hooks (table above).
- `WWII_NOCACHE=1` bypasses the generated art/audio disk caches (see
  Startup & loading).
- `SIGINT`/`SIGTERM`/`SIGHUP` stop the game cleanly, including while it waits on
  the splash gate, so `timeout 30 ./pacific-squadron` and CI kill paths work even
  with nobody at the keyboard.
- `WWII_DUMP_SPRITES=<dir>` (debug builds, art-regeneration runs) writes the
  generated player fighter frames, the propeller overlays, every enemy
  aircraft sprite and an `enemy_prop_anchors.txt` (engine anchors used by
  the spinning-prop overlays) to `<dir>` for offline inspection.

## Application icon & packaging

`assets/app-icon-base.png` (1254x1254 master artwork) is the single source for
every platform icon. `tools/make_icons.py` regenerates the whole pack:

```
python3 tools/make_icons.py            # or: cmake --build . --target icons
```

Outputs in `assets/icons/`:

| File | Used by |
|---|---|
| `pacific-squadron-{16,24,32,48,64,128,256,512,1024}x{N}.png` | Linux/hicolor, runtime window icon |
| `pacific-squadron.ico` (16/24/32/48/64/128/256) | Windows executable resource |
| `pacific-squadron.icns` (icp4…ic14, 16→1024) | macOS app bundle |

The master has no alpha channel (its rounded corners are painted black), so the
generator attaches an analytic rounded-square mask, un-premultiplies the
black-blended edge, and resamples in premultiplied space — no dark fringe on a
light desktop, at any size. The `.ico` and `.icns` containers are written by the
script itself, so regeneration works identically on Linux, macOS and Windows
(needs only `pip install pillow numpy`; the generated files are committed, so a
normal build never needs Python).

Wired in automatically:

- **Runtime** — `Renderer::setWindowIconFromDir()` loads the PNG pack and hands
  the window a multi-resolution surface (X11/Wayland taskbars, Windows titlebar
  and Alt+Tab). Skipped on the dummy driver and on macOS, where the dock uses
  the bundle icon.
- **Windows** — `packaging/pacific-squadron.rc.in` is configured into the build
  tree and compiled into the executable, so Explorer/taskbar show the icon.
- **macOS** — the `.app` is built by default (`PACIFIC_SQUADRON_MAC_BUNDLE=ON`)
  with the `.icns` in `Contents/Resources`, alongside the embedded SDL3 copy and
  its license.
- **Linux install** — `cmake --install` places the binary in `bin/`, the PNGs in
  `share/icons/hicolor/<size>/apps/pacific-squadron.png`, and
  `packaging/pacific-squadron.desktop` in `share/applications/`.

Manual steps still required per platform:

- **Linux, no install** — to use the icon without `cmake --install`, copy any PNG
  to `~/.local/share/icons/hicolor/256x256/apps/pacific-squadron.png` and run
  `gtk-update-icon-cache ~/.local/share/icons/hicolor`.
- **Windows** — rebuild after regenerating the `.ico` (the resource is compiled
  in at build time; there is no post-hoc patching).
- **macOS** — the dock ignores `SDL_SetWindowIcon`; ship a `.app` (either the
  opt-in CMake bundle above or your own tooling) for the `.icns` to take effect.
  To rebuild the `.icns` with Apple's own tooling instead, `iconutil` needs an
  `.iconset` folder: `iconutil -c icns pacific-squadron.iconset`.

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
