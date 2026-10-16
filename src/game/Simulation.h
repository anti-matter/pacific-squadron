#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/Collision.h"
#include "core/MathUtil.h"
#include "core/Rng.h"
#include "core/Vec2.h"
#include "game/GameData.h"
#include "game/Formation.h"
#include "game/LevelScript.h"
#include "game/Movement.h"

namespace ww {

constexpr float kPlayW = 384.0f;
constexpr float kPlayH = 512.0f;
constexpr float kSimDt = 1.0f / 60.0f;

enum class Team : std::uint8_t { Player, Enemy };

enum class EKind : std::uint8_t {
    Shot,            // projectile (player or enemy)
    Enemy,           // aircraft, ground unit, ship
    Powerup,
    BossHull,
    BossComponent,
};

struct Entity {
    bool alive = false;
    EKind kind = EKind::Shot;
    Team team = Team::Enemy;
    int enemyDef = -1;     // EnemyDef index (for Enemy)
    int weaponDef = -1;    // WeaponDef index (for Shot / shooters)
    int powerupDef = -1;   // PowerupDef index

    Vec2 pos{};
    Vec2 vel{};
    float angle = 0.0f;    // visual rotation (radians, 0 = +x)
    std::string sprite;    // renderer texture name (boss parts; others use defs)
    int hp = 1;
    int maxHp = 1;

    float age = 0.0f;
    float lifeLeft = 1e9f;
    int penetration = 0;
    float flashTimer = 0.0f;   // damage flash
    float fireTimer = 0.0f;
    int burstLeft = 0;
    float burstTimer = 0.0f;
    float rateMul = 1.0f;
    int forceDrop = -1;

    MoveState move;
    int formation = -1;
    int formationSlot = 0;
    bool aimAtPlayer = false;
    bool rotates = false;
    bool groundUnit = false;
    bool collidesWithPlayer = true;

    int bossInstance = -1;       // BossInstance index or -1
    int componentIndex = -1;

    std::uint16_t nextFree = 0;  // pool bookkeeping
};

struct Particle {
    Vec2 pos, vel;
    float life = 0.0f;
    float maxLife = 1.0f;
    float size = 2.0f;
    std::uint32_t color = 0xFFFFFFFFu;  // 0xRRGGBBAA
    float drag = 1.0f;
};

struct ExplosionFx {
    Vec2 pos;
    float t = 0.0f;
    int sizeClass = 1;   // 0 small, 1 medium, 2 large
    bool splash = false; // water splash variant
};

struct SimEvent {
    enum class Type : std::uint8_t {
        Sound, PlayerHit, PlayerDied, EnemyKilled, FormationCleared,
        PowerupTaken, BossSpawned, BossComponentDestroyed, BossDefeated,
        StageComplete, BombUsed,
    };
    Type type;
    std::string name;    // sound name for Sound events
    int value = 0;
    Vec2 pos{};
};

enum class WeaponClass : std::uint8_t { Gun, Spread, Cannon, Rocket };

struct DifficultyMods {
    float hpMul = 1.0f;
    float rateMul = 1.0f;    // enemy fire interval multiplier (higher = slower)
    float speedMul = 1.0f;
};

struct PlayerStateView {
    Vec2 pos;
    int lives = 0;
    int hp = 0;
    int maxHp = 0;
    int bombs = 0;
    WeaponClass weapon = WeaponClass::Gun;
    int weaponLevel = 1;
    bool alive = false;
    bool invulnerable = false;
    float specialTimer = 0.0f;
    float respawnTimer = 0.0f;
    float bank = 0.0f;
};

// The playing field. Owns all entities and advances the world by fixed
// timesteps. No SDL, no audio, no rendering: side effects are surfaced as
// SimEvents for the application layer.
class Simulation {
public:
    explicit Simulation(const GameData& data, std::uint64_t seed = 12345);

    void startStage(int stageIndex, DifficultyMods mods);

    void setPlayerIntent(Vec2 moveDir, bool fire, bool special);
    void update();

    // ---- queries for rendering / HUD ----
    struct EntityView {
        const Entity* e;
    };
    const std::vector<Entity>& entities() const { return pool_; }
    const std::vector<Particle>& particles() const { return particles_; }
    const std::vector<ExplosionFx>& explosions() const { return explosions_; }
    PlayerStateView player() const { return playerView(); }    const LevelDirector& director() const { return director_; }
    int score() const { return score_; }
    bool playerAlive() const { return player_.alive; }

    struct BossView {
        bool active = false;
        std::string name;
        float hpFraction = 0.0f;
    };
    BossView bossView() const;

    int entityCount() const { return liveCount_; }
    int projectileCount() const { return shotCount_; }
    Vec2 playerHitboxCenter() const { return player_.pos; }

    // Consumed by the application (audio) and Game (state) layers.
    bool popEvent(SimEvent& out);

    // Fire the player's special/bomb externally (e.g. debugging or input).
    void requestBomb() { playerBombWanted_ = true; }
    // Debug/testing: ignore player damage.
    void setGodMode(bool g) { godMode_ = g; }
    // Headless smoke-testing hook.
    void debugSpawnBoss(int defRef) { spawnBoss(defRef); }

    const GameData& data() const { return data_; }

private:
    struct FormationInstance {
        int id = -1;
        int aliveCount = 0;
        int totalCount = 0;
        Vec2 anchor{};
        MoveState lead;
        std::vector<Vec2> offsets;
        std::vector<int> members;
        bool done = false;
        bool escaped = false;
    };

    struct BossInstance {
        int defRef = -1;
        int hull = -1;
        std::vector<int> components;
        Vec2 pos{};
        int state = 0;              // 0 entering, 1 fighting, 2 dying, 3 dead
        float stateTimer = 0.0f;
        float extraTimer = 0.0f;
        float deathChain = 0.0f;
        int phase = 0;
    };

    struct PlayerInternal {
        Vec2 pos{192.0f, 430.0f};
        Vec2 vel{};
        int lives = 2;
        int hp = 3;
        int maxHp = 3;
        int bombs = 2;
        WeaponClass weapon = WeaponClass::Gun;
        int weaponLevel = 1;
        bool alive = true;
        float invuln = 2.5f;
        float respawnTimer = 0.0f;
        float fireCooldown = 0.0f;
        float specialTimer = 0.0f;
        float bank = 0.0f;
        int burstLeft = 0;
        float burstTimer = 0.0f;
    };

    // pool helpers
    int alloc();
    void kill(int i);
    void compactIfNeeded();

    // spawning
    int spawnEnemy(int defRef, Vec2 pos, float hpMul, float rateMul,
                   float speedMul, int forceDrop, int formationId,
                   int pathRef = -1);
    void spawnShot(int weaponRef, Vec2 pos, Vec2 aimDir, Team team);
    void spawnVolley(int weaponRef, Vec2 pos, Team team, bool aimAtPlayerPos);
    void spawnPowerup(int defRef, Vec2 pos);
    void spawnBoss(int defRef);

    // systems
    void updatePlayer(float dt);
    void updateEnemies(float dt);
    void updateFormations(float dt);
    void updateShots(float dt);
    void updatePowerups(float dt);
    void updateBosses(float dt);
    void updateEffects(float dt);
    void buildColliders();
    void resolveCollisions();
    void damagePlayer();
    void killEnemy(int i, bool byPlayer);
    void addScore(int pts) { score_ += pts; }
    void emitSound(const char* name) {
        events_.push_back({SimEvent::Type::Sound, name, 0, {}});
    }
    PlayerStateView playerView() const;

    const GameData& data_;
    Rng rng_;
    std::vector<Entity> pool_;
    std::vector<std::uint16_t> freeList_;
    int liveCount_ = 0;
    int shotCount_ = 0;
    bool poolDirty_ = false;

    LevelDirector director_;
    DifficultyMods mods_;
    int stageIndex_ = 0;

    PlayerInternal player_;
    Vec2 moveDir_{};
    bool fireHeld_ = false;
    bool specialHeld_ = false;
    bool prevSpecialHeld_ = false;
    bool playerBombWanted_ = false;
    std::vector<SpawnRequest> spawnQueue_;

    std::vector<FormationInstance> formations_;
    std::vector<BossInstance> bosses_;

    std::vector<Particle> particles_;
    std::vector<ExplosionFx> explosions_;

    SpatialGrid grid_{kPlayW, kPlayH, 64.0f};
    std::vector<std::uint16_t> colliderOwner_;  // collider idx -> entity idx

    int score_ = 0;
    bool godMode_ = false;
    std::vector<SimEvent> events_;
    std::size_t eventRead_ = 0;
};

// Terrain helper shared with the renderer.
TerrainKind terrainAt(const StageDef& stage, float scrollY);

}  // namespace ww
