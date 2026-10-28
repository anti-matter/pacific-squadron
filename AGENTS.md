# AGENTS.md — Pacific Squadron

Guidance for AI agents (and new contributors) working on this repository.
Supersedes the original `2D vertical-scrolling.md` brief, which is deleted;
its still-valid principles are distilled here. See `README.md` for the
player-facing docs (controls, env-var table, tuning).

**Maintenance: update this file whenever a substantial and/or relevant
change lands** — new systems or conventions, content/level milestones,
changed workflows, new gotchas, or roadmap progress. Keep it current rather
than letting it drift; stale guidance is worse than none.

## What this is

An **original** WWII 2D vertical-scrolling arcade shooter (1942-style) in
modern **C++20** with **SDL3** and **CMake**. Every sprite, sound and music
track is generated procedurally and cached; gameplay content (weapons,
enemies, formations, paths, power-ups, bosses, stages) is plain JSON under
`assets/`. The game must remain **original art** — never copy names, sprites,
music or level layouts from copyrighted games; WWII *history* is fine, other
games' IP is not.

Key invariants:

- **Deterministic 60 Hz fixed-step simulation** (`src/game/Simulation.*`),
  fully decoupled from SDL. The `gamecore` static lib builds without SDL;
  only `src/app/`, `Renderer`, `Audio`, `Input` touch SDL. Never introduce
  wall-clock or RNG nondeterminism into the simulation (presentation-only
  code, like the ocean glint drift, may use `SDL_GetTicks`).
- **Data-driven**: new content is JSON + a procedural sprite, not code.
- **Portability**: Linux primary, Windows/macOS targets. No platform APIs
  outside SDL. RAII/value semantics; avoid deep inheritance.
- Logical resolution **384×512**, integer-scaled.

## Build & verify

```sh
cmake -S . -B build && cmake --build build -j8     # must stay warning-clean
ctest --test-dir build                              # must stay green
```

macOS: `brew install sdl3` first (only to *build* — the `.app` embeds its own
SDL3 copy, see Gotchas). The `PACIFIC_SQUADRON_MAC_BUNDLE` option is ON
by default, so the binary lands at
`./build/pacific-squadron.app/Contents/MacOS/pacific-squadron` (assets are copied
beside it) — the smoke-harness commands below need that path, or configure with
`-DPACIFIC_SQUADRON_MAC_BUNDLE=OFF` to keep the plain `./build/pacific-squadron`
path the docs assume.

**Version**: `major.minor.bugfix` lives only in `project(wwii_fighter VERSION
…)` at the top of `CMakeLists.txt`. Bump that one line, re-run `cmake -S . -B
build`; `cmake/version.h.in` generates `build/generated/version.h`
(`WW_VERSION_STRING`) for the splash line, and the same value feeds the macOS
plist. Configure fails if the version is not three numbers.

Headless screenshot + autopilot harness (run **from the repo root**, use
`./build/pacific-squadron` — an installed copy in `~/.local/bin` may be
stale):

```sh
WWII_START_STAGE=5 WWII_BOSS_NOW=3 WWII_SMOKE=30 WWII_SMOKE_GOD=1 \
WWII_SMOKE_NOAUTO=1 WWII_SMOKE_SHOT=/tmp/opencode/x.png WWII_SMOKE_SHOT_AT=20 \
SDL_VIDEODRIVER=dummy ./build/pacific-squadron
```

- `WWII_SMOKE_NOAUTO=1` = idle player (keeps targets on screen).
- `WWII_SMOKE_BOMB_AT=8,9.5` = press the BOMB special at those **sim** times
  (comma-separated) — headless verification of bomb effects.
- `WWII_SMOKE_SHOT_AT` is **real elapsed time**, not stage time (~2–3 s
  offset; the splash gate + intro eat a few seconds).
- `WWII_BOSS_NOW` skips to the current stage's boss. Note it spawns the boss
  *early*, i.e. before the stage's arena terrain has scrolled in — to verify
  an arena's real look, run the full scripted timing instead
  (e.g. `WWII_SMOKE=134 WWII_SMOKE_SHOT_AT=128` for a `bossTime` of 120).

## Layout

- `src/game/` — simulation, data loading (`GameData.*`), stage script
  (`LevelScript.*`), tuning constants (`Tuning.h`).
- `src/graphics/ProceduralArt.cpp` — entity/boss/HUD sprite generation.
- `src/graphics/EnvironmentArt.cpp` — scenery sprite generation (`env_*`:
  trees, buildings, rocks, pond) consumed by the terrain baker.
- `src/graphics/Environment.cpp` — `EnvironmentBaker`: bakes full-width
  64 px land "chunks" (farmland, roads, set pieces, trees) into pooled
  `env_chunk_N` textures; falls back to the legacy 32 px filler tiles.
- `src/graphics/SpriteKit.h` — shared `kit::` raster toolkit (`Hi`,
  `shade`, `mix24`, `mixPacked`, `outlineSilhouette`, `softBlendEllipse`).
- `src/app/Application.cpp` — rendering, menus/help, smoke harness.
- `assets/data/{weapons,enemies,bosses,stages,anims,...}/*.json`.
- `tests/test_data.cpp` — data pins (see below).

## Sprite system rules

- Sprites are generated in `ProceduralArt.cpp` and registered with `add()`
  in the registration block. **An unknown sprite name renders the entity
  invisible** — grep registrations before debugging "missing" enemies.
- **Bump `kArtCacheVersion` whenever generated art changes**, or every
  existing install keeps loading stale cached pixels.
- Boss hulls auto-swap variants at HP thresholds when registered:
  `_light` (<85%), `_dmg` (<70%), `_wreck` (<35%), plus `_flash`
  (`toFlashSilhouette`) for hit feedback. Register all for a new boss.
- `WWII_DUMP_SPRITES=/tmp/opencode/sprites` (Debug build) dumps every
  registered sprite as PNG — but only when the art cache is missing:
  `rm ~/.local/share/pacific-squadron/pacific-squadron/art_cache.bin` first.
- Shared look helpers: `softDisc` (AA'd discs), `outlineSilhouette` (crisp
  contour), `mix`/`shadeBy`, deterministic `hash`/`frand`.
- **`kit::mix24` takes raw `0xRRGGBB`; `col()`-packed `0xRRGGBBAA` values
  need `kit::mixPacked`** — passing packed operands into `mix24` double-
  shifts the channels (the famous purple-sprite bug).
- **Terrain chunks** (`Environment.cpp`): land rows in `Island`/`Jungle`
  segments render as baked 384×64 `env_chunk_N` textures instead of 32 px
  tiles. Everything in a bake is keyed to **absolute world coordinates**
  (candidate grids, hash keys, structure cells) so neighbouring padded
  canvases make identical decisions — any per-chunk-relative keying shows
  up as a visible band every 64 px. Verify seams with
  `WWII_DUMP_CHUNKS=/tmp/dir` (Debug; dumps canvas PPMs; compare chunk N
  canvas row 32 vs chunk N+1 row 95 — seam diff should be in the same
  range as adjacent interior rows, ~5–10).
- **Composed-land coastlines are baked, not tiled**: each span's bake extends
  through its adjacent shore band to the real waterline (`Coast`/`CoastEdge`
  in Environment.cpp). The land/water edge is a curved waterline
  (`coastEdgeY`, coherent noise keyed to world X — identical in every canvas),
  with a curved sand→grass coverage ramp in the LandMask (features only stamp
  at full coverage, so the tree line follows the curve), an alpha fade of a few
  px at the water edge, and a baked soft broken-cream foam fringe (never a
  solid white line). Where the neighbour beyond the shore band is land
  (fortress/airfield), the edge is a dry `joint` blend instead. In
  `Application::renderTerrain`, shore bands adjacent to composed land draw
  **plain sea filler** (the chunk paints beach/land/foam over them) and a
  filler tile is skipped only when its full 32 px range lies inside a span
  (`composedTileCovered`); centre-based skipping leaves clear-colour
  (`0x06101C`) stripes at every coastline.
- **`env_*` sprites are PNG-overridable** from `assets/sprites/<name>.png`
  like all other sprites; the baker reads them through the Renderer's env
  pixel store, so authored art and procedural fallbacks both bake.

## Data conventions

- **Stages are auto-discovered** from `assets/data/stages/*.json`, ordered by
  the first number in the filename (`stage2` before `stage10`); the game
  reaches Victory after the last one. `WWII_START_STAGE` is 1-based.
- **New bosses go at the END of `bosses.json`** — tests reference indices.
- **Kill-difficulty budget**: each boss's total HP (hull + components) is
  ~1.31–1.37× the previous boss's total, and `test_data.cpp` pins the exact
  number plus the band. Chain so far: destroyer 194 → carrier 252 →
  fortress 338 → cruiser 448 → airfield 596 → drydock 800 → battleship 1080
  → sky fortress 1450.
- `test_data.cpp` pins per-stage boss identity/composition/HP/phases. Any
  data change must update the pins deliberately, never by accident.
- Boss JSON: `arenaOcean: true` = boss carries its own ground and the world
  renders open sea during the fight (fortress/airfield style set pieces).
  `sailSpeed > 0` = mobile boss (drydock): anchored until its last
  destructible mount dies, then it maneuvers sideways at that speed,
  bouncing inside `hullWidth/2 + 6` margins (`BossView.sailing/sailDir`
  drive the wake). Ship sorties (`sortieEnemy` is a `ship: true` enemy)
  splash on launch.
  Top-level `boss` + `bossTime` select the stage boss; event-level `"boss"`
  is broken, top-level stage `hpMul` is ignored.
  `hullSpriteOpen` + `openAtFraction` = multi-stage hull (sky fortress):
  once remaining **mount** hp falls to that fraction of the boss total the
  hull entity's `sprite` swaps to the open variant for good — the renderer
  resolves `_dmg/_wreck/_flash` against the new base automatically. Register
  the full variant set for the open name. Note phase `hpFraction` and
  `openAtFraction` both key on mount hp / (hull + mounts), hull hp excluded.
  Component `"aimAtPlayer": true` = predictive turret: its `Entity.angle`
  tracks the player (with lead for shell flight time, deterministic) and the
  renderer rotates the sprite about its centre — barrel points **screen-down**
  at angle 0, so draw such turret art symmetric around that pose.
- Component `"weapon": ""` = destructible structure that never fires
  (hangars). Weapons resolved by name: unknown name ⇒ never fires.
- Terrain segments are keyed by **world Y** (`worldY ≈ t × scrollSpeed`);
  the arena terrain must cover the scroll depth at `bossTime` (use
  `toY: 99999` for the arena tail). Boss x is 192: airfield runway lanes are
  tile columns 5–6 = screen x 160..224; align hull-sprite runways to that.
- Stage JSON may carry `"landmarks": [{ "kind", "x", "y" }]` — intentional
  scenery set pieces (`farmstead` | `hamlet` | `depot`) baked by the
  environment baker at fixed world Y (same coords as terrain segments).
  They are stamped before the random set-piece scatter, which is suppressed
  within 88 px of any landmark; purely decorative, silently skipped when the
  spot is not on composed land.
- Scroll freezes while a boss is engaged (`setScrollFrozen`). Ground units
  are held/dropped when they'd spawn on the wrong surface
  (`groundSpawnMismatch`), and all terrain-borne spawns are dropped during
  an `arenaOcean` fight (`seaArenaActive`). Enemy **ships that overlap an
  engaged boss's hull box are rammed under** (killed, no score) unless they
  spawned inside it (a boss's own well-boats pass through) — escorts should
  spawn in side lanes clear of the hull box (see stage 9's edge columns).
  Post-`bossTime` events DO fire
  during the boss window — use them for in-fight bomber/elite waves.

## Help screen

`renderHelp()` / `renderHelpCatalog()` in `Application.cpp`; pages defined
by `kHelpPages` (sprite catalog rows: icon fit-box + one-line description),
page count by `Game::kHelpPages`. **Add new bosses/sprites to the catalog
pages when you create them.** Flagship catalogs hold at most 5 rows per page
(56×56 icons, 56 px rows); descriptions start at the page's `textX` — keep
them ≤ 36 chars at the boss pages' textX 88. The stroke font is ASCII-only
(no `&`, no em-dashes); at scale 1 budget ≈7 px/char and test screenshots
for overflow.

## Art direction

Top-down / slight 3/4 arcade perspective. Target is a **polished modern 2D
shooter** look — explicitly NOT chunky retro pixel art, and never fix visual
problems with blur or generic smoothing filters. Concretely:

- Smooth curves and clean silhouettes; soft anti-aliased edges (`softDisc`)
  instead of hard blocky grids; richer multi-tone shading and clear material
  separation (vegetation vs sand vs wet sand vs rock vs concrete vs metal vs
  wood vs water), with one consistent light direction (sun from the
  **top-left**; shadows cast to the bottom-right).
- Genuinely higher-fidelity shapes and detail — do not upscale or blur the
  low-res sprites, and do not preserve blockiness for nostalgia.
- Stylized, not photorealistic; **gameplay readability is absolute**:
  turrets, guns, bullets, weak points and navigable space must stay obvious.
  Boss threats are bright metal on bright pads; scenery stays low-contrast
  and must never grow over gun pads or emplacements (see `nearPad`,
  `inVegBand`).
- Natural forms for environments: organic (harmonically warped, but
  deterministic and baked) coastlines, irregular vegetation clusters with
  several species/sizes instead of uniform rings/checkerboards, layered
  shoreline transitions (deep water → reef shallows → foam → wet sand →
  beach → vegetation).
- Reduce visible repetition via multiple variants and deterministic hash
  masks (never simulation-affecting randomness).
- Everything bakes into the existing sprites — no per-frame decorative
  entities or transparent overlay stacks.

## Content roadmap

**All ten levels shipped.** Stage 10 THE SKY FORTRESS closes the design
sheet with SKY FORTRESS OMEGA: a multi-stage flying fortress whose bomb-bay
doors blow open at 30% mount hp (hull sprite swap to `boss_fortress2_open`,
bombs raining via `b_bombs`) and which grinds through four escalating
phases; five predictive turrets, four AA gondolas, a command spire and
two destructible (silent) bay doors. Continuous escort waves through the
boss window; Victory follows the last stage. Level 9 TRUK LAGOON (SUPER
BATTLESHIP, Yamato-class at anchor) is documented in `test_data.cpp` pins.
The boss-index-1 "SKY FORTRESS GX-7" remains as an early-stage set piece;
the finale is a distinct boss (index 9).

## Gotchas

- **Never verify text/splash visuals with `SDL_VIDEODRIVER=dummy`.** The
  native-resolution stroke font is baked at `scale × window-scale` pixels; the
  dummy driver settles on a ~1.08 canvas scale, where the ~1 px pen strokes
  merge and every glyph reads as a solid blob (pre-existing, not a regression).
  Use `SDL_VIDEODRIVER=cocoa` for macOS screenshots — at Retina 3x the glyphs
  are crisp. `WWII_SMOKE_SPLASH_SHOT=<path>` grabs the finished splash (prompt
  visible); the procedural fallback splash needs `splash.png` absent from the
  resolved `assets/` dir, so copy the `.app` to a temp dir and delete it there
  (a cwd `assets/` shadows the bundle's).
- **`Renderer::text`/`textWidth` take a `float` scale** — fractional sizes
  (the splash version line uses 0.8) rasterize crisply because the atlas is
  baked per physical pixel size; never "fix" small text by pre-scaling a
  bitmap.
- **macOS asset resolution**: inside a `.app`, `SDL_GetBasePath()` is
  `Contents/Resources/`, *not* `Contents/MacOS/`, and a Finder/`open` launch
  starts with cwd `/` — so `cwd/assets` never hits. `Application::init()`
  probes `Resources/assets` (where `cmake/CopyAssets.cmake` puts them) plus
  the legacy `MacOS/assets` and the installed `share/` layout. If you move the
  asset copy, update both. `CopyAssets.cmake` exists because
  `cmake -E copy_directory` cannot exclude the multi-MB dev-only artwork in
  `assets/` (icon master, reference sheets); it copies only `data/`, `icons/`,
  optional `sprites/`+`tiles/` overrides and `splash.png`.
- **Empty asset tree used to boot silently**: `GameData::loadAll` reports
  success when it finds zero files, which produced a playable-looking game
  with no enemies, no firing and no splash art. `Application::init()` now
  aborts (log + message box, skipped for `WWII_SMOKE`/dummy-video) if stages,
  enemies or weapons come back empty.
- **The `.app` is self-contained (SDL3 embedded)**:
  `cmake/BundleSDL.cmake` copies the SDL3 dylib into `Contents/Frameworks`,
  rewrites the install name and the executable's reference to `@rpath`, deletes
  the absolute Homebrew rpath CMake leaves behind (otherwise a broken bundle
  silently resolves SDL on the build machine and only fails on other Macs),
  copies SDL's zlib license to `Resources/LICENSE.SDL3.txt`, and re-signs
  inside-out ad-hoc — arm64 refuses to load a binary whose load commands were
  edited after signing. Runs on any POST_BUILD (idempotent) and handles both a
  bare `libSDL3*.dylib` and `SDL3.framework`. The license it ships comes from
  the vendored `licenses/LICENSE.SDL3.txt` — do *not* grab the `LICENSE.txt`
  next to a Homebrew SDL3, that is Homebrew's own BSD-2 formula license. Opt out
  with `-DPACIFIC_SQUADRON_BUNDLE_SDL=OFF`. Two traps:
  `$<TARGET_FILE:SDL3::SDL3>` is a *symlink* under Homebrew and `file(COPY)`
  preserves symlinks, so resolve with `REALPATH` first; and signing is ad-hoc,
  so Gatekeeper still blocks a downloaded copy (distributing would need a
  Developer ID and notarization). Verify self-containment with
  `DYLD_PRINT_LIBRARIES=1 <exe> 2>&1 | grep libSDL3` — it must print a path
  inside the bundle.

- **Boss-ship wakes animate at runtime**: the seagoing boss hulls
  (battleship/yamato/cruiser/destroyer/carrier) bake only foam hugging the
  plating; the bow-wave V and stern trail are drawn per-frame by
  `Application::renderShipWake()` (presentation-only, wall-clock time like
  the glint), using the small `wake_dot` sprite. Geometry (bow/stern offsets
  from the sprite centre, half-beam) is hard-coded per ship family there —
  update it if a hull canvas resizes.
- The **BOMB** special (Shift/Ctrl, `p.bombs` charges) wipes on-screen enemies
  and enemy fire and takes 20% of max HP off every living boss part (hull only
  once armor is stripped — same gate as bullets). During the AIRFIELD COMMAND
  fight each detonation also leaves a persistent crater decal (`bomb_crater`,
  presentation-only in `Application.cpp`, driven by the charge-count watcher).

- `Hi::bake()` treats sub-pixel alpha as **coverage** (any nonzero fill
  counts solid, near-solid snaps to 255) — never encode translucency in
  `Hi` fills; generate soft/translucent sprites straight into `PixelBuf`.
- Git: commit to `main` and push only after the user approves that change.
- The stage clock resets per level; anything time-based in presentation must
  handle that (the glint drift uses the wall clock for exactly this reason).
- `fillRect`/`hline`/`vline` are integer; `softDisc`/`fillEllipse` float.
- Keep both GCC and Clang warning-free (warnings are treated as work).
