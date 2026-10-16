Create a new 2D vertical-scrolling arcade shooter inspired by the gameplay style of classic WWII-era shooters such as 1942, 1943, and 1941.

This must be an original game. Do not copy Capcom code, sprites, music, level layouts, logos, or other copyrighted game assets.

## Primary technical goals

Implement the game in modern C++ with portability as a first-class design requirement.

Initial development platform:

- Linux, primarily CachyOS / Arch Linux

Future target platforms:

- Windows
- macOS

The game should require as little platform-specific code as possible.

Use:

- C++20 or newer, unless there is a compelling portability reason not to
- SDL3 for windowing, input, controller support, timing, and basic 2D rendering
- CMake for the build system
- PNG assets with alpha transparency
- a lightweight portable audio solution such as miniaudio, or an SDL3-compatible audio layer
- lightweight portable libraries where useful, such as stb_image and nlohmann/json

Avoid unnecessary heavyweight frameworks or platform-specific dependencies.

Do not use Linux-specific APIs for game functionality unless isolated behind a platform abstraction.

## Architecture

Keep game logic independent of SDL wherever practical.

SDL-specific functionality should be concentrated into infrastructure classes such as:

- Application
- Window
- Renderer
- Input
- Audio
- FileSystem / resource loading

Gameplay classes should not make direct SDL calls unnecessarily.

Structure the project approximately like:

src/
  app/
  core/
  graphics/
  audio/
  input/
  game/
    player/
    enemies/
    weapons/
    projectiles/
    levels/
    bosses/
    powerups/
  world/
  ui/
  data/

assets/
  sprites/
    player/
    enemies/
    bosses/
    ground/
    projectiles/
    powerups/
    effects/
    ui/
  tiles/
  backgrounds/
  audio/
  data/

cmake/

Use RAII, smart pointers where ownership requires them, and normal value semantics where possible.

Avoid unnecessary inheritance. Prefer composition and data-driven behavior.

## Game concept

Create a top-down vertically scrolling WWII-inspired arcade shooter.

The player controls a fighter aircraft and:

- moves in 8 directions
- continuously advances through a scrolling world
- shoots enemy aircraft
- attacks ground targets
- attacks ships
- dodges enemy fire
- collects weapon upgrades and power-ups
- encounters scripted formations
- fights large multi-part bosses

Gameplay should be fast, responsive, deterministic, and arcade-oriented.

Do not simulate realistic aircraft physics such as lift, drag, stalls, realistic turn radius, or ballistics.

Movement should respond immediately to controls.

## Controls

Initial keyboard controls:

- WASD or arrow keys: movement
- Space: primary fire
- Left Shift or Ctrl: special/evasive maneuver
- Escape: pause

Also support SDL game controllers from the beginning.

Input bindings should be configurable and should not be hard-coded directly into player logic.

## Logical resolution

Use an internal logical gameplay resolution around:

384 × 512

Render to arbitrary window sizes while preserving aspect ratio and integer scaling where practical.

Support:

- windowed mode
- fullscreen
- borderless fullscreen if SDL supports it portably

Pixel-art rendering must use nearest-neighbor scaling.

Do not couple gameplay coordinates to physical monitor resolution.

## Initial player

Create an original WWII-inspired player fighter.

Approximate sprite size:

48×48 pixels

Use a much smaller gameplay hitbox, approximately:

10×10 pixels

The player should support:

- normal flight
- banking left/right
- firing
- damage state
- destruction
- evasive maneuver / loop

## Weapons

Begin with:

- basic forward guns
- spread weapon
- heavy cannon
- rockets

Weapon definitions should be data-driven.

Example properties:

- damage
- fire interval
- projectile velocity
- projectile count
- spread angle
- lifetime
- penetration
- sprite
- sound

Do not hard-code weapon behavior directly into Player.

## Enemy system

Implement reusable enemy behavior components.

Initial enemies:

- light fighter
- formation fighter
- fast interceptor
- heavy fighter
- bomber
- AA turret
- tank
- bunker
- patrol boat
- destroyer

Enemies should primarily be defined using data plus reusable behavior.

Example concepts:

- health
- score value
- movement pattern
- weapon
- fire interval
- sprite
- hitbox
- power-up drop
- destruction effect

## Enemy movement

Support reusable movement patterns:

- straight
- diagonal
- sine wave
- circular
- spline / Bézier
- player-directed dive
- formation movement
- scripted waypoint path

Avoid creating a unique C++ class for every movement pattern or enemy variation.

## Enemy formations

Support groups such as:

- V formation
- horizontal line
- vertical line
- arc
- staggered formation
- waves entering from screen sides

Destroying an entire formation may award bonus points or power-ups.

## Projectiles

Create a reusable projectile system supporting:

- straight shots
- aimed shots
- spread shots
- bursts
- radial patterns
- rockets
- heavy shells

Most aimed enemy bullets should aim at the player's position when fired rather than continuously tracking the player.

Keep enemy projectile speed low enough for intentional dodging.

## Collision

Keep rendering bounds and collision bounds separate.

Support collision categories such as:

- player vs enemy projectile
- player vs enemy aircraft
- player projectile vs enemy
- player projectile vs ground target
- player vs power-up

Design collision handling so that more efficient broad-phase collision detection can be introduced later if needed.

Do not build collision logic into sprite rendering code.

## Scrolling world

Use a vertically scrolling tile-based world.

Recommended tile size:

32×32

Support terrain such as:

- ocean
- islands
- shoreline
- jungle
- roads
- airfields
- military bases
- fortresses

Terrain should use reusable tiles plus placed objects.

Levels should be much taller than the visible screen and scroll through world coordinates.

## Level scripting

Do not randomly spawn all enemies.

Stages should primarily use scripted encounters.

A stage definition should be able to specify:

- trigger time or world position
- enemy type
- spawn position
- formation
- movement pattern
- weapon
- difficulty modifiers
- power-up drops

Store stage definitions in external data files such as JSON.

Example conceptual event:

{
  "trigger": 12.0,
  "enemy": "fighter_light",
  "formation": "v_formation",
  "count": 6,
  "path": "sine_down"
}

## Bosses

Bosses should support independently destructible components.

Examples:

- giant bomber
- aircraft carrier
- battleship
- island fortress
- experimental aircraft

A battleship might contain:

- hull
- front turret
- rear turret
- AA guns
- bridge

Boss components should have independent:

- health
- weapons
- hitboxes
- destroyed states

Avoid creating bosses as one giant monolithic class.

## Effects

Implement reusable effects for:

- small explosions
- medium explosions
- large explosions
- smoke
- fire
- muzzle flashes
- projectile impacts
- water splashes
- boss destruction sequences

A lightweight particle system may be used for effects that do not require hand-drawn sprite animation.

## Asset strategy

All artwork must be original.

Target visual style:

- 16-bit-style arcade pixel art
- top-down / slight 3/4 perspective
- WWII-inspired
- readable silhouettes
- strong contrast between enemies, bullets, and backgrounds
- dramatic arcade explosions

Approximate scale:

- player: 48×48
- fighters: 24–40 px
- heavy aircraft: 48–64 px
- bombers: 64–96 px
- ground targets: 32–64 px
- power-ups: 24–32 px
- terrain: 32×32 tiles
- large ships and bosses: assembled from multiple sprites

Assets that can reasonably be generated procedurally should be, including:

- basic bullets
- shadows
- tracers
- simple particles
- some smoke
- simple UI geometry

Create placeholders where finished artwork does not yet exist.

The absence of final artwork must never block gameplay development.

## Animation data

Keep animation information outside gameplay code.

Example:

{
  "name": "enemy_fighter_light_destroy",
  "frameWidth": 32,
  "frameHeight": 32,
  "frames": 8,
  "fps": 12,
  "loop": false,
  "pivot": [16,16],
  "hitbox": [10,10,12,12]
}

Implement an animation system capable of loading definitions such as this.

## Audio

Create an abstraction for:

- music
- sound effects
- volume categories

Required initial sounds:

- player gun
- enemy gun
- explosion
- player damage
- power-up
- menu selection

Game code should not depend directly on the selected audio library.

## UI

Initial HUD should contain:

- score
- high score
- health or lives
- current weapon
- special/evasive maneuver count
- boss health when applicable

UI layout should operate in logical-resolution coordinates.

## Game states

Implement an explicit game-state system including:

- title
- menu
- stage intro
- playing
- paused
- boss
- stage complete
- player death
- game over
- high score

Do not scatter state-transition logic throughout unrelated classes.

## Portability requirements

Assume this code will eventually be compiled with:

Linux:
- GCC
- Clang

Windows:
- MSVC
- potentially Clang

macOS:
- Apple Clang

Therefore:

- avoid compiler-specific extensions
- avoid assuming path separator "/"
- avoid assuming case-sensitive filesystems
- avoid Linux filesystem locations
- avoid endianness assumptions
- use fixed-width integer types where binary formats require them
- do not depend on sizeof(int), sizeof(long), etc.
- do not assume x86 architecture
- avoid unaligned binary structure casts
- avoid implementation-defined behavior
- keep filesystem and resource location handling portable
- use std::filesystem where appropriate
- keep save/config data separate from packaged assets

CMake should detect the host platform rather than requiring platform-specific source edits.

The same source tree should build on all supported platforms.

## Build system

Provide a clean CMake setup.

Desired workflow on Linux:

cmake -S . -B build
cmake --build build
./build/<game-name>

Use target-based CMake practices.

Do not use global include_directories(), add_definitions(), or unnecessary global compiler flags.

Prefer:

target_link_libraries()
target_include_directories()
target_compile_features()
target_compile_options()

Enable useful warnings for the active compiler without making the build depend on GCC-only flags.

Optionally support sanitizers on compatible development builds.

## Testing

Where practical, keep systems sufficiently decoupled to unit-test:

- movement calculations
- weapon timing
- formation behavior
- collision logic
- scoring
- level event parsing
- data loading

The game should also support a debug mode containing useful development information such as:

- FPS
- entity count
- projectile count
- player hitbox
- enemy hitboxes
- spawn markers
- current world position

## Development phases

Do not attempt to implement the complete game immediately.

### Phase 1 — Foundation

Create:

- CMake project
- SDL3 application
- portable application loop
- renderer abstraction
- input abstraction
- resource manager
- logical-resolution rendering
- basic game-state system

Verify clean startup and shutdown.

### Phase 2 — Minimal playable prototype

Implement only:

- scrolling ocean
- player aircraft placeholder
- movement
- shooting
- one light fighter
- one AA turret
- enemy bullets
- collisions
- explosions
- scoring
- simple HUD

The result should already be a playable vertical shooter.

### Phase 3 — Core systems

Add:

- animation system
- data-driven enemies
- data-driven weapons
- formation controller
- movement/path system
- power-ups
- configurable controls
- audio

### Phase 4 — World/content

Add:

- island terrain
- shoreline
- tanks
- bunkers
- bombers
- multiple fighter types
- ships
- stage scripting

### Phase 5 — Boss system

Implement:

- modular boss components
- boss weapons
- boss phases
- boss health UI
- multi-stage destruction effects

### Phase 6 — Polish

Add:

- finished art
- music
- menus
- gamepad polish
- high scores
- settings
- difficulty levels
- additional stages
- packaging

## First task

Before writing substantial gameplay code:

1. Inspect the current project directory if one already exists.
2. Propose the directory and subsystem architecture.
3. Identify external dependencies and explain why each is necessary.
4. Create the initial CMake/SDL3 skeleton.
5. Confirm that it compiles and runs on Linux.
6. Create a basic scrolling window with logical-resolution rendering.
7. Only then begin the minimal playable prototype.

Keep dependencies minimal.

Favor understandable native C++ over excessive abstraction.

The eventual game should feel like a purpose-built C++ arcade game rather than an engine layered under another engine.
