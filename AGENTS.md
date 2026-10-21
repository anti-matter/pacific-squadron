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

Headless screenshot + autopilot harness (run **from the repo root**, use
`./build/pacific-squadron` — an installed copy in `~/.local/bin` may be
stale):

```sh
WWII_START_STAGE=5 WWII_BOSS_NOW=3 WWII_SMOKE=30 WWII_SMOKE_GOD=1 \
WWII_SMOKE_NOAUTO=1 WWII_SMOKE_SHOT=/tmp/opencode/x.png WWII_SMOKE_SHOT_AT=20 \
SDL_VIDEODRIVER=dummy ./build/pacific-squadron
```

- `WWII_SMOKE_NOAUTO=1` = idle player (keeps targets on screen).
- `WWII_SMOKE_SHOT_AT` is **real elapsed time**, not stage time (~2–3 s
  offset; the splash gate + intro eat a few seconds).
- `WWII_BOSS_NOW` skips to the current stage's boss. Note it spawns the boss
  *early*, i.e. before the stage's arena terrain has scrolled in — to verify
  an arena's real look, run the full scripted timing instead
  (e.g. `WWII_SMOKE=134 WWII_SMOKE_SHOT_AT=128` for a `bossTime` of 120).

## Layout

- `src/game/` — simulation, data loading (`GameData.*`), stage script
  (`LevelScript.*`), tuning constants (`Tuning.h`).
- `src/graphics/ProceduralArt.cpp` — **all** sprite generation.
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

## Data conventions

- **Stages are auto-discovered** from `assets/data/stages/*.json`; the game
  reaches Victory after the last one. `WWII_START_STAGE` is 1-based.
- **New bosses go at the END of `bosses.json`** — tests reference indices.
- **Kill-difficulty budget**: each boss's total HP (hull + components) is
  ~1.31–1.37× the previous boss's total, and `test_data.cpp` pins the exact
  number plus the band. Chain so far: destroyer 194 → carrier 252 →
  fortress 338 → cruiser 448 → airfield 596.
- `test_data.cpp` pins per-stage boss identity/composition/HP/phases. Any
  data change must update the pins deliberately, never by accident.
- Boss JSON: `arenaOcean: true` = boss carries its own ground and the world
  renders open sea during the fight (fortress/airfield style set pieces).
  Top-level `boss` + `bossTime` select the stage boss; event-level `"boss"`
  is broken, top-level stage `hpMul` is ignored.
- Component `"weapon": ""` = destructible structure that never fires
  (hangars). Weapons resolved by name: unknown name ⇒ never fires.
- Terrain segments are keyed by **world Y** (`worldY ≈ t × scrollSpeed`);
  the arena terrain must cover the scroll depth at `bossTime` (use
  `toY: 99999` for the arena tail). Boss x is 192: airfield runway lanes are
  tile columns 5–6 = screen x 160..224; align hull-sprite runways to that.
- Scroll freezes while a boss is engaged (`setScrollFrozen`). Ground units
  are held/dropped when they'd spawn on the wrong surface
  (`groundSpawnMismatch`), and all terrain-borne spawns are dropped during
  an `arenaOcean` fight (`seaArenaActive`). Post-`bossTime` events DO fire
  during the boss window — use them for in-fight bomber/elite waves.

## Help screen

`renderHelp()` / `renderHelpCatalog()` in `Application.cpp`; pages defined
by `kHelpPages` (sprite catalog rows: icon fit-box + one-line description),
page count by `Game::kHelpPages`. **Add new bosses/sprites to the catalog
pages when you create them.** The stroke font is ASCII-only (no `&`, no
em-dashes); at scale 1 budget ≈7 px/char and test screenshots for overflow.

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

Levels 1–7 shipped. Remaining per the design sheet: **8 FLOATING DRYDOCK**
(mobile drydock, crane guns, side sections with separate HP, launches a
ship, final phase undocks and *moves* — moving a scroll-frozen boss needs a
small director/`bossView` change), **9 SUPER BATTLESHIP** (Yamato: triple
turrets, predictive rotating turrets, multiple damage phases), **10 SKY
FORTRESS** (multi-stage flying fortress, bomb bays open with extra guns,
final core-exposed phase; boss index 1 already holds an older Sky Fortress —
the finale supersedes or reuses it).

## Gotchas

- `Hi::bake()` treats sub-pixel alpha as **coverage** (any nonzero fill
  counts solid, near-solid snaps to 255) — never encode translucency in
  `Hi` fills; generate soft/translucent sprites straight into `PixelBuf`.
- Git: commit to `main` and push only after the user approves that change.
- The stage clock resets per level; anything time-based in presentation must
  handle that (the glint drift uses the wall clock for exactly this reason).
- `fillRect`/`hline`/`vline` are integer; `softDisc`/`fillEllipse` float.
- Keep both GCC and Clang warning-free (warnings are treated as work).
