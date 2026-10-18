#pragma once

#include <string>
#include <vector>

#include "core/Vec2.h"

namespace ww {

// ---------------------------------------------------------------------------
// Animations
// ---------------------------------------------------------------------------
struct AnimDef {
    std::string name;
    int frameW = 32;
    int frameH = 32;
    int frames = 1;
    float fps = 12.0f;
    bool loop = false;
    Vec2 pivot{16.0f, 16.0f};
};

// ---------------------------------------------------------------------------
// Weapons / projectiles
// ---------------------------------------------------------------------------
enum class ShotPattern : std::uint8_t {
    Forward,   // straight up (or along def.angle)
    Aimed,     // toward the player at fire time
    Spread,    // `count` shots around a direction
    Radial,    // `count` shots evenly around a circle
    Burst,     // `burstCount` shots fired sequentially along a direction
    AimSpread, // aimed spread at the player
};

enum class ShotKind : std::uint8_t {
    PlayerBullet,
    PlayerCannon,
    PlayerRocket,
    EnemyBullet,
    EnemyShell,
    EnemyFlak,
};

struct WeaponDef {
    std::string name;
    ShotPattern pattern = ShotPattern::Forward;
    ShotKind kind = ShotKind::PlayerBullet;
    std::string sprite = "bullet_player";
    std::string sound = "player_gun";
    int damage = 1;
    float interval = 0.15f;      // seconds between volleys
    float speed = 400.0f;        // logical px/s
    int count = 1;               // shots per volley (spread/radial)
    float spreadDeg = 0.0f;      // total arc for spread patterns
    float lifetime = 2.0f;
    int penetration = 0;         // extra enemies a player shot passes through
    int burstCount = 1;          // for Burst pattern
    float burstDelay = 0.06f;
    float baseAngleDeg = -90.0f; // -90 = straight up
    float aimJitterDeg = 0.0f;   // random spread on aimed shots
    Vec2 muzzle{0.0f, 0.0f};     // spawn offset from shooter center
    float gravity = 0.0f;        // optional downward accel
    bool rotates = false;        // visual rotation to velocity
};

// ---------------------------------------------------------------------------
// Movement patterns (shared by enemies, formations and bosses)
// ---------------------------------------------------------------------------
enum class MovePattern : std::uint8_t {
    Static,      // holds position (turrets)
    Straight,    // constant velocity along angleDeg
    SineDown,    // descends while oscillating horizontally
    SineSide,    // crosses the screen horizontally, light vertical bob
    Circular,    // loops around a moving center
    Bezier,      // cubic Bezier through a named path
    Waypoints,   // polyline through a named path
    DiveAtPlayer,// holds, then dives at the player's last position
    Formation,   // follows a formation anchor (set by the controller)
    DriftDown,   // scrolls down with the world (ground units)
};

struct PathDef {
    std::string name;
    std::vector<Vec2> points;  // control points (bezier: 4, waypoints: N)
};

struct MoveState {
    MovePattern pattern = MovePattern::Straight;
    Vec2 origin{};
    float angleDeg = 90.0f;   // direction of travel (90 = down)
    float speed = 60.0f;
    float amp = 30.0f;        // sine amplitude / circle radius
    float freq = 1.0f;        // cycles per second
    float delay = 0.0f;       // e.g. dive delay after entry
    float duration = 1e9f;    // pattern active time
    float phase = 0.0f;       // per-instance phase offset
    int pathRef = -1;         // index into GameData::paths
    float t = 0.0f;           // elapsed time in pattern
    Vec2 lastPos{};           // integrated position
    Vec2 diveVel{};           // locked dive velocity (DiveAtPlayer)
    bool diveLocked = false;
};

// ---------------------------------------------------------------------------
// Enemies
// ---------------------------------------------------------------------------
enum class DropKind : std::uint8_t { None, Random, WeaponSpecific };

struct EnemyDef {
    std::string name;
    std::string sprite = "enemy_fighter_light";
    int width = 32;
    int height = 32;
    // Hitbox offset from center + size.
    int hitX = 0, hitY = 0, hitW = 12, hitH = 12;
    int hp = 1;
    int score = 100;
    float speed = 80.0f;
    MovePattern pattern = MovePattern::SineDown;
    float amp = 30.0f;
    float freq = 0.8f;
    float angleDeg = 90.0f;
    float diveDelay = 1.0f;
    std::string path = "";           // for Bezier/Waypoints
    std::string weapon = "";         // WeaponDef name ("" = does not shoot)
    float fireInterval = 1.5f;
    float firstFireDelay = 0.8f;
    int burstOverride = -1;
    bool aimAtPlayer = true;
    bool ground = false;             // turret/tank/bunker (scrolls with world)
    bool ship = false;               // surface vessel
    float dropChance = 0.0f;
    DropKind dropKind = DropKind::Random;
    std::string dropWeapon = "";     // for WeaponSpecific drops
    int deathEffect = 1;             // 0 small, 1 medium, 2 large
    std::string deathSound;          // explosion class name; empty = from deathEffect
    float smokeWhileDamaged = 0.5f;  // hp fraction below which it smokes
    int pathRef = -1;                // resolved at load
    int weaponRef = -1;              // resolved at load
};

// ---------------------------------------------------------------------------
// Power-ups
// ---------------------------------------------------------------------------
enum class PowerupKind : std::uint8_t {
    WeaponUp,   // upgrade current weapon
    Spread,     // switch to spread gun
    Cannon,     // switch to heavy cannon
    Rocket,     // switch to rockets
    Bomb,       // +1 special
    Life,       // +1 life
    Repair,     // +1 hp
};

struct PowerupDef {
    std::string name;
    PowerupKind kind = PowerupKind::WeaponUp;
    std::string sprite = "powerup_p";
    float fallSpeed = 60.0f;
};

// ---------------------------------------------------------------------------
// Bosses: hull + independently destructible components
// ---------------------------------------------------------------------------
struct BossComponentDef {
    std::string name;
    std::string sprite;
    int offsetX = 0;        // offset from boss anchor (center)
    int offsetY = 0;
    int width = 32;
    int height = 32;
    int hp = 20;
    int score = 500;
    std::string weapon = "";
    float fireInterval = 1.5f;
    float firstFireDelay = 1.0f;
    bool destructibleTarget = true;   // counts toward boss defeat
};

struct BossPhaseDef {
    float hpFraction = 0.5f;  // active while total hp <= fraction * max
    float rateMul = 1.0f;     // fire-rate multiplier (lower = faster)
    std::string extraWeapon = "";
    float extraInterval = 2.0f;
};

struct BossDef {
    std::string name;
    std::string hullSprite = "boss_hull";
    int hullWidth = 160;
    int hullHeight = 96;
    int hullHp = 60;
    int hullScore = 2000;
    float entryY = 110.0f;        // screen y where the boss settles
    float hoverAmp = 40.0f;       // horizontal sway amplitude
    float hoverFreq = 0.10f;
    std::vector<BossComponentDef> components;
    std::vector<BossPhaseDef> phases;
    int deathEffect = 2;
    std::string defeatDrop = "powerup_p";  // drop after defeat
    // Air group (carriers): periodically launch fighters from the deck.
    // sortieInterval 0 (or empty sortieEnemy) disables the mechanic.
    std::string sortieEnemy;
    float sortieInterval = 0.0f;
    int sortieCount = 2;
};

// ---------------------------------------------------------------------------
// Terrain / stages
// ---------------------------------------------------------------------------
enum class TerrainKind : std::uint8_t {
    Ocean, Island, Shore, Jungle, Road, Airfield, Fortress
};

struct TerrainSegment {
    float fromY = 0.0f;   // scroll distance range
    float toY = 1e9f;
    TerrainKind kind = TerrainKind::Ocean;
};

struct SpawnEvent {
    float trigger = 0.0f;         // seconds since stage start
    std::string enemy;            // EnemyDef name
    int enemyRef = -1;            // resolved index
    std::string formation = "single";
    int count = 1;
    std::string path = "";        // optional named path override
    int pathRef = -1;
    float x = 192.0f;             // anchor spawn x
    float y = -30.0f;             // anchor spawn y (usually off-screen)
    float delayBetween = 0.15f;   // stagger per member
    float spacing = 36.0f;        // formation member spacing
    float hpMul = 1.0f;
    float rateMul = 1.0f;         // fire interval multiplier
    float speedMul = 1.0f;
    int forceDrop = -1;           // PowerupDef index, -1 = use enemy def
    std::string boss = "";        // BossDef name (for boss events)
    int bossRef = -1;
    bool fired = false;
};

enum class FormationShape : std::uint8_t {
    Single, VLine, HLine, VFormation, Arc, Staggered, SideWaveLeft, SideWaveRight, Wedge
};

struct StageDef {
    std::string name;
    float duration = 90.0f;       // playable length before boss window
    float scrollSpeed = 60.0f;    // world px/s
    std::vector<TerrainSegment> terrain;
    std::vector<SpawnEvent> events;
    std::string boss = "";
    float bossTime = 80.0f;
    int bossRef = -1;
};

// ---------------------------------------------------------------------------
// Aggregate game data, loaded from JSON
// ---------------------------------------------------------------------------
struct GameData {
    std::vector<AnimDef> anims;
    std::vector<WeaponDef> weapons;
    std::vector<EnemyDef> enemies;
    std::vector<PowerupDef> powerups;
    std::vector<BossDef> bosses;
    std::vector<PathDef> paths;
    std::vector<StageDef> stages;

    int findWeapon(std::string_view name) const;
    int findEnemy(std::string_view name) const;
    int findBoss(std::string_view name) const;
    int findPath(std::string_view name) const;
    int findPowerup(std::string_view name) const;
    int findAnim(std::string_view name) const;

    // Loads every JSON file in `dir` for a category ("weapons", "enemies",
    // ...). Returns false with `error` set on parse problems.
    bool loadAll(const std::string& assetDir, std::string& error);
    // Resolves cross-references (weapon names, path names, boss refs...).
    bool linkReferences(std::string& error);
};

// Parsing helpers (exposed for unit tests; also usable directly).
bool parseAnimationsJson(const std::string& text, std::vector<AnimDef>& out, std::string& error);
bool parseWeaponsJson(const std::string& text, std::vector<WeaponDef>& out, std::string& error);
bool parseEnemiesJson(const std::string& text, std::vector<EnemyDef>& out, std::string& error);
bool parsePowerupsJson(const std::string& text, std::vector<PowerupDef>& out, std::string& error);
bool parseBossesJson(const std::string& text, std::vector<BossDef>& out, std::string& error);
bool parseStageJson(const std::string& text, StageDef& out, std::string& error);

FormationShape formationShapeFromName(const std::string& name);

}  // namespace ww
