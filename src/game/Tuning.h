#pragma once

// ---------------------------------------------------------------------------
// Central gameplay tuning knobs.
//
// Everything that defines the *feel* of the game lives here so it can be
// adjusted in one place. Per-weapon fire intervals/damage and per-stage
// scroll speed are deliberately data-driven instead: see
//   assets/data/weapons/*.json    (interval, speed, damage, ...)
//   assets/data/stages/*.json     (scrollSpeed, wave scripts, ...)
// The values below are the hard-coded simulation defaults that are not part
// of any data file.
// ---------------------------------------------------------------------------

namespace ww {
namespace tuning {

// --- player movement -------------------------------------------------------
// World pixels per second. Instant start/stop (no inertia); diagonals are
// normalized so they are never faster than the cardinal directions.
inline constexpr float kPlayerSpeed = 230.0f;

// Playfield clamp for the player ship (px inset from each edge).
inline constexpr float kPlayerMarginX = 12.0f;
inline constexpr float kPlayerMarginTop = 60.0f;   // keeps the ship below HUD
inline constexpr float kPlayerMarginBottom = 18.0f;

// --- player combat ---------------------------------------------------------
// Player bullet-time hitbox (square, centered on the cockpit). Deliberately
// much smaller than the visible sprite.
inline constexpr float kPlayerHitbox = 10.0f;

inline constexpr int kPlayerStartLives = 2;
inline constexpr int kPlayerMaxHp = 3;
inline constexpr int kPlayerStartBombs = 2;
inline constexpr int kBombCap = 9;

// --- special / evasive action ----------------------------------------------
inline constexpr float kSpecialDuration = 0.9f;   // wingman loop time (s)
inline constexpr float kSpecialInvuln = 1.5f;     // i-frames from a bomb (s)

// --- respawn ---------------------------------------------------------------
inline constexpr float kStartInvuln = 2.5f;       // i-frames at stage start (s)
inline constexpr float kRespawnDelay = 1.8f;      // s before the ship returns
inline constexpr float kRespawnInvuln = 3.0f;     // i-frames on respawn (s)
inline constexpr float kRespawnGrace = 0.5f;      // fire cooldown after respawn
inline constexpr float kRespawnX = 192.0f;        // respawn position
inline constexpr float kRespawnY = 440.0f;

// --- scrolling -------------------------------------------------------------
// Default stage scroll speed (px/s) used when a stage JSON omits one; must
// match the loader default in GameData.cpp.
inline constexpr float kDefaultScrollSpeed = 60.0f;

}  // namespace tuning
}  // namespace ww
