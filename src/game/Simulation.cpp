#include "game/Simulation.h"

#include "game/Tuning.h"

#include <algorithm>
#include <cmath>

namespace ww {

TerrainKind terrainAt(const StageDef& stage, float scrollY) {
    if (stage.terrain.empty()) return TerrainKind::Ocean;
    TerrainKind kind = stage.terrain.front().kind;
    for (const auto& seg : stage.terrain) {
        if (scrollY >= seg.fromY) kind = seg.kind;
    }
    return kind;
}

// Shot collider sizes (shared with the debug overlay; declared in Simulation.h).

void shotSize(ShotKind kind, float& w, float& h) {
    switch (kind) {
    case ShotKind::PlayerBullet:  w = 4.0f; h = 8.0f;  break;
    case ShotKind::PlayerCannon:  w = 7.0f; h = 12.0f; break;
    case ShotKind::PlayerRocket:  w = 6.0f; h = 13.0f; break;
    case ShotKind::EnemyBullet:   w = 5.0f; h = 5.0f;  break;
    case ShotKind::EnemyShell:    w = 8.0f; h = 8.0f;  break;
    case ShotKind::EnemyFlak:     w = 7.0f; h = 7.0f;  break;
    }
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
namespace {

constexpr float kMargin = 40.0f;

Vec2 playerAimDir(Vec2 from, Vec2 to) {
    Vec2 d = to - from;
    if (lengthSq(d) < 1e-6f) return {0.0f, -1.0f};
    return normalize(d);
}


std::uint32_t rgba(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) {
    return (static_cast<std::uint32_t>(r) << 24) |
           (static_cast<std::uint32_t>(g) << 16) |
           (static_cast<std::uint32_t>(b) << 8) | a;
}

}  // namespace

Simulation::Simulation(const GameData& data, std::uint64_t seed)
    : data_(data), rng_(seed) {}

// ---------------------------------------------------------------------------
// Pool management
// ---------------------------------------------------------------------------
int Simulation::alloc() {
    poolDirty_ = true;
    if (!freeList_.empty()) {
        const int i = freeList_.back();
        freeList_.pop_back();
        Entity& e = pool_[static_cast<std::size_t>(i)];
        e = Entity{};
        e.alive = true;
        ++liveCount_;
        return i;
    }
    pool_.push_back(Entity{});
    Entity& e = pool_.back();
    e.alive = true;
    ++liveCount_;
    return static_cast<int>(pool_.size()) - 1;
}

void Simulation::kill(int i) {
    Entity& e = pool_[static_cast<std::size_t>(i)];
    if (!e.alive) return;
    e.alive = false;
    if (e.kind == EKind::Shot) --shotCount_;
    --liveCount_;
    freeList_.push_back(static_cast<std::uint16_t>(i));
}

void Simulation::compactIfNeeded() {
    // Keep the pool from growing unbounded after heavy fights.
    if (pool_.size() > 256 && freeList_.size() * 2 > pool_.size()) {
        std::vector<Entity> dense;
        dense.reserve(pool_.size());
        for (const auto& e : pool_)
            if (e.alive) dense.push_back(e);
        pool_ = std::move(dense);
        freeList_.clear();
    }
}

bool Simulation::popEvent(SimEvent& out) {
    if (eventRead_ >= events_.size()) {
        events_.clear();
        eventRead_ = 0;
        return false;
    }
    out = events_[eventRead_++];
    return true;
}

// ---------------------------------------------------------------------------
// Stage lifecycle
// ---------------------------------------------------------------------------
void Simulation::startStage(int stageIndex, DifficultyMods mods) {
    mods_ = mods;
    stageIndex_ = stageIndex;
    for (auto& e : pool_) e.alive = false;
    pool_.clear();
    freeList_.clear();
    liveCount_ = shotCount_ = 0;
    formations_.clear();
    bosses_.clear();
    particles_.clear();
    explosions_.clear();
    spawnQueue_.clear();
    events_.clear();
    eventRead_ = 0;
    score_ = 0;

    player_ = PlayerInternal{};
    if (stageIndex < static_cast<int>(data_.stages.size()))
        director_.load(data_.stages[static_cast<std::size_t>(stageIndex)]);
}

PlayerStateView Simulation::playerView() const {
    PlayerStateView view;
    view.pos = player_.pos;
    view.vel = player_.vel;
    view.lives = player_.lives;
    view.hp = player_.hp;
    view.maxHp = player_.maxHp;
    view.bombs = player_.bombs;
    view.weapon = player_.weapon;
    view.weaponLevel = player_.weaponLevel;
    view.alive = player_.alive;
    view.invulnerable = player_.invuln > 0.0f;
    view.specialTimer = player_.specialTimer;
    view.respawnTimer = player_.respawnTimer;
    view.bank = player_.bank;
    return view;
}

void Simulation::setPlayerIntent(Vec2 moveDir, bool fire, bool special) {
    moveDir_ = moveDir;
    fireHeld_ = fire;
    specialHeld_ = special;
}

// ---------------------------------------------------------------------------
// Main update
// ---------------------------------------------------------------------------
void Simulation::update() {
    const float dt = kSimDt;

    // Director -> spawn queue -> world.
    director_.update(dt, spawnQueue_);
    const float now = director_.time();
    for (auto it = spawnQueue_.begin(); it != spawnQueue_.end();) {
        if (it->time <= now) {
            if (it->bossRef >= 0) {
                spawnBoss(it->bossRef);
            } else {
                int formationId = -1;
                auto fit = std::find_if(formations_.begin(), formations_.end(),
                                        [&](const FormationInstance& f) {
                                            return f.id == it->formation;
                                        });
                if (it->formation >= 0 && fit == formations_.end()) {
                    // First member of a fresh formation: create the group.
                    FormationInstance f;
                    f.id = it->formation;
                    f.totalCount = 0;
                    // Count all queued members sharing this formation.
                    for (const auto& r : spawnQueue_)
                        if (r.formation == it->formation) ++f.totalCount;
                    f.offsets = formationOffsets(it->shape, f.totalCount);
                    f.anchor = it->pos;
                    const EnemyDef& def =
                        data_.enemies[static_cast<std::size_t>(it->enemyRef)];
                    f.lead = formationLeadState(def, it->shape, it->pos,
                                                it->speedMul * mods_.speedMul);
                    if (it->pathRef >= 0 &&
                        it->pathRef < static_cast<int>(data_.paths.size())) {
                        f.lead.pathRef = it->pathRef;
                        f.lead.pattern =
                            data_.paths[static_cast<std::size_t>(it->pathRef)]
                                                .points.size() == 4
                                ? MovePattern::Bezier
                                : MovePattern::Waypoints;
                    }
                    formations_.push_back(std::move(f));
                    formationId = it->formation;
                } else if (it->formation >= 0) {
                    formationId = it->formation;
                }
                spawnEnemy(it->enemyRef, it->pos, it->hpMul, it->rateMul,
                           it->speedMul, it->forceDrop, formationId,
                           formationId >= 0 ? -1 : it->pathRef);
                it = spawnQueue_.erase(it);
            }
        } else {
            ++it;
        }
    }

    updatePlayer(dt);
    updateFormations(dt);
    updateEnemies(dt);
    updateShots(dt);
    updatePowerups(dt);
    updateBosses(dt);
    updateEffects(dt);
    buildColliders();
    resolveCollisions();
    compactIfNeeded();
}

// ---------------------------------------------------------------------------
// Player
// ---------------------------------------------------------------------------
namespace {
int weaponClassMaxLevel(WeaponClass c) {
    return c == WeaponClass::Gun ? 3 : 2;
}
}  // namespace

static int playerWeaponRef(const GameData& data, WeaponClass w, int level) {
    static const char* names[] = {"gun", "spread", "cannon", "rocket"};
    level = clamp(level, 1, weaponClassMaxLevel(w));
    const std::string n = std::string(names[static_cast<int>(w)]) +
                          "_l" + std::to_string(level);
    int ref = data.findWeapon(n);
    if (ref < 0) ref = data.findWeapon("gun_l1");
    return ref;
}

void Simulation::updatePlayer(float dt) {
    PlayerInternal& p = player_;

    if (!p.alive) {
        p.respawnTimer -= dt;
        if (p.respawnTimer <= 0.0f && p.lives >= 0) {
            p.alive = true;
            p.pos = {tuning::kRespawnX, tuning::kRespawnY};
            p.hp = p.maxHp;
            p.invuln = tuning::kRespawnInvuln;
            p.weaponLevel = std::max(1, p.weaponLevel - 1);
            p.fireCooldown = tuning::kRespawnGrace;
            p.burstLeft = 0;
        }
        return;
    }

    p.invuln = std::max(0.0f, p.invuln - dt);
    p.specialTimer = std::max(0.0f, p.specialTimer - dt);
    p.fireCooldown = std::max(0.0f, p.fireCooldown - dt);

    // Movement: immediate arcade response, 8 directions.
    const float kSpeed = tuning::kPlayerSpeed;
    Vec2 dir = moveDir_;
    if (lengthSq(dir) > 1.0f) dir = normalize(dir);
    p.vel = dir * kSpeed;  // informational (debug overlay); movement is instant
    p.pos = p.pos + dir * (kSpeed * dt);
    p.pos.x = clamp(p.pos.x, tuning::kPlayerMarginX, kPlayW - tuning::kPlayerMarginX);
    p.pos.y = clamp(p.pos.y, tuning::kPlayerMarginTop,
                kPlayH - tuning::kPlayerMarginBottom);
    p.bank = lerp(p.bank, dir.x, smoothFactor(14.0f, dt));

    // Special / evasive loop.
    const bool specialPressed = (specialHeld_ || playerBombWanted_) && !prevSpecialHeld_;
    prevSpecialHeld_ = specialHeld_;
    playerBombWanted_ = false;
    if (specialPressed && p.bombs > 0 && p.specialTimer <= 0.0f) {
        --p.bombs;
        p.specialTimer = tuning::kSpecialDuration;
        p.invuln = std::max(p.invuln, tuning::kSpecialInvuln);
        events_.push_back({SimEvent::Type::BombUsed, "bomb", 0, p.pos});
        emitSound("bomb");
        // Clear enemy bullets on screen, converting them to score.
        for (std::size_t i = 0; i < pool_.size(); ++i) {
            Entity& e = pool_[i];
            if (!e.alive || e.kind != EKind::Shot || e.team != Team::Enemy) continue;
            if (e.pos.x < -8 || e.pos.x > kPlayW + 8 || e.pos.y < -8 ||
                e.pos.y > kPlayH + 8)
                continue;
            addScore(5);
            for (int k = 0; k < 3; ++k) {
                particles_.push_back({e.pos,
                                      {rng_.range(-40, 40), rng_.range(-40, 40)},
                                      0.25f, 0.25f, 2.0f,
                                      rgba(255, 220, 120, 255), 3.0f});
            }
            kill(static_cast<int>(i));
        }
    }

    // Primary fire.
    const int weaponRef = playerWeaponRef(data_, p.weapon, p.weaponLevel);
    if (weaponRef >= 0 && fireHeld_ && p.fireCooldown <= 0.0f) {
        const WeaponDef& w = data_.weapons[static_cast<std::size_t>(weaponRef)];
        p.fireCooldown = w.interval;
        if (w.burstCount > 1) {
            spawnShot(weaponRef, p.pos + w.muzzle,
                      fromAngle(degToRad(w.baseAngleDeg)), Team::Player);
            p.burstLeft = w.burstCount - 1;
            p.burstTimer = w.burstDelay;
        } else {
            spawnVolley(weaponRef, p.pos, Team::Player, false);
        }
        emitSound(w.sound.c_str());
    }
    if (p.burstLeft > 0) {
        p.burstTimer -= dt;
        if (p.burstTimer <= 0.0f && fireHeld_) {
            const WeaponDef& w = data_.weapons[static_cast<std::size_t>(weaponRef)];
            spawnShot(weaponRef, p.pos + w.muzzle,
                      fromAngle(degToRad(w.baseAngleDeg)), Team::Player);
            --p.burstLeft;
            p.burstTimer = w.burstDelay;
        } else if (!fireHeld_) {
            p.burstLeft = 0;
        }
    }
}

// ---------------------------------------------------------------------------
// Spawning
// ---------------------------------------------------------------------------
int Simulation::spawnEnemy(int defRef, Vec2 pos, float hpMul, float rateMul,
                           float, int forceDrop, int formationId, int pathRef) {
    if (defRef < 0 || defRef >= static_cast<int>(data_.enemies.size())) return -1;
    const EnemyDef& def = data_.enemies[static_cast<std::size_t>(defRef)];
    const int i = alloc();
    Entity& e = pool_[static_cast<std::size_t>(i)];
    e.kind = EKind::Enemy;
    e.team = Team::Enemy;
    e.enemyDef = defRef;
    e.weaponDef = def.weaponRef;
    e.pos = pos;
    e.maxHp = e.hp = std::max(1, static_cast<int>(std::lroundf(
                                    static_cast<float>(def.hp) * hpMul * mods_.hpMul)));
    e.aimAtPlayer = def.aimAtPlayer;
    e.groundUnit = def.ground;
    e.rateMul = rateMul * mods_.rateMul;
    e.forceDrop = forceDrop;
    e.move = makeMoveState(def, mods_.speedMul, def.pathRef);
    if (pathRef >= 0 && pathRef < static_cast<int>(data_.paths.size())) {
        e.move.pathRef = pathRef;
        e.move.pattern = data_.paths[static_cast<std::size_t>(pathRef)].points.size() == 4
            ? MovePattern::Bezier
            : MovePattern::Waypoints;
    }
    e.move.origin = pos;
    e.move.lastPos = pos;
    e.move.phase = rng_.nextFloat() * (2.0f * kPi);
    e.fireTimer = def.firstFireDelay + rng_.range(0.0f, 0.3f);

    if (formationId >= 0) {
        for (auto& f : formations_) {
            if (f.id == formationId) {
                e.formation = formationId;
                e.formationSlot = static_cast<int>(f.members.size());
                f.members.push_back(i);
                ++f.aliveCount;
                e.move.pattern = MovePattern::Formation;
                break;
            }
        }
    }
    return i;
}

void Simulation::spawnShot(int weaponRef, Vec2 pos, Vec2 dir, Team team) {
    const WeaponDef& w = data_.weapons[static_cast<std::size_t>(weaponRef)];
    const int i = alloc();
    Entity& e = pool_[static_cast<std::size_t>(i)];
    e.kind = EKind::Shot;
    e.team = team;
    e.weaponDef = weaponRef;
    e.pos = pos;
    e.vel = dir * w.speed;
    e.angle = angleOf(e.vel);
    e.rotates = w.rotates;
    e.lifeLeft = w.lifetime;
    e.penetration = w.penetration;
    e.hp = 1;
    ++shotCount_;
    if (w.kind == ShotKind::PlayerRocket) {
        particles_.push_back({pos, {0, 30}, 0.3f, 0.3f, 3.0f,
                               rgba(160, 160, 160, 140), 2.0f});
    }
}

void Simulation::spawnVolley(int weaponRef, Vec2 pos, Team team,
                             bool /*aimAtPlayerPos*/) {
    const WeaponDef& w = data_.weapons[static_cast<std::size_t>(weaponRef)];
    auto aimToward = [&](Vec2 from) {
        return team == Team::Player ? Vec2{0.0f, -1.0f}
                                    : playerAimDir(from, player_.pos);
    };

    switch (w.pattern) {
    case ShotPattern::Forward: {
        for (int s = 0; s < w.count; ++s) {
            const float off = (static_cast<float>(s) - static_cast<float>(w.count - 1) * 0.5f) * 12.0f;
            spawnShot(weaponRef, pos + w.muzzle + Vec2{off, 0.0f},
                      fromAngle(degToRad(w.baseAngleDeg)), team);
        }
        break;
    }
    case ShotPattern::Aimed: {
        Vec2 d = aimToward(pos);
        if (w.aimJitterDeg > 0.0f)
            d = fromAngle(angleOf(d) + degToRad(rng_.range(-w.aimJitterDeg,
                                                           w.aimJitterDeg)));
        spawnShot(weaponRef, pos + w.muzzle, d, team);
        break;
    }
    case ShotPattern::AimSpread: {
        const float base = angleOf(aimToward(pos));
        for (int s = 0; s < w.count; ++s) {
            const float t = w.count > 1
                ? (static_cast<float>(s) / static_cast<float>(w.count - 1)) - 0.5f
                : 0.0f;
            spawnShot(weaponRef, pos + w.muzzle,
                      fromAngle(base + t * degToRad(w.spreadDeg)), team);
        }
        break;
    }
    case ShotPattern::Spread: {
        const float base = degToRad(w.baseAngleDeg);
        for (int s = 0; s < w.count; ++s) {
            const float t = w.count > 1
                ? (static_cast<float>(s) / static_cast<float>(w.count - 1)) - 0.5f
                : 0.0f;
            spawnShot(weaponRef, pos + w.muzzle,
                      fromAngle(base + t * degToRad(w.spreadDeg)), team);
        }
        break;
    }
    case ShotPattern::Radial: {
        const float step = 2.0f * kPi / static_cast<float>(w.count);
        const float rot = rng_.nextFloat() * step;
        for (int s = 0; s < w.count; ++s)
            spawnShot(weaponRef, pos + w.muzzle,
                      fromAngle(rot + step * static_cast<float>(s)), team);
        break;
    }
    case ShotPattern::Burst: {
        // Single shot now; remaining shots handled by the shooter's burst
        // counters (set by callers that own an entity).
        Vec2 d = w.aimJitterDeg > 0.0f || w.pattern == ShotPattern::Burst
            ? aimToward(pos)
            : fromAngle(degToRad(w.baseAngleDeg));
        spawnShot(weaponRef, pos + w.muzzle, d, team);
        break;
    }
    }
}

void Simulation::spawnPowerup(int defRef, Vec2 pos) {
    if (defRef < 0 || defRef >= static_cast<int>(data_.powerups.size())) return;
    const int i = alloc();
    Entity& e = pool_[static_cast<std::size_t>(i)];
    e.kind = EKind::Powerup;
    e.team = Team::Player;
    e.powerupDef = defRef;
    e.pos = pos;
    e.vel = {0.0f, data_.powerups[static_cast<std::size_t>(defRef)].fallSpeed};
    e.lifeLeft = 12.0f;
}

// ---------------------------------------------------------------------------
// Formations
// ---------------------------------------------------------------------------
void Simulation::updateFormations(float dt) {
    for (auto& f : formations_) {
        if (f.done) continue;
        // Remove dead members.
        f.members.erase(std::remove_if(f.members.begin(), f.members.end(),
                                       [&](int idx) {
                                           return !pool_[static_cast<std::size_t>(idx)].alive;
                                       }),
                        f.members.end());
        f.aliveCount = static_cast<int>(f.members.size());

        if (f.members.empty()) {
            if (f.totalCount > 1 && !f.escaped) {
                addScore(300);
                events_.push_back({SimEvent::Type::FormationCleared,
                                   "formation_bonus", 300, f.anchor});
                if (f.anchor.y > 40.0f && f.anchor.y < kPlayH - 80.0f) {
                    const int drop = rng_.chance(0.5f)
                        ? data_.findPowerup("powerup_p")
                        : data_.findPowerup("powerup_bomb");
                    spawnPowerup(drop, f.anchor);
                }
            }
            f.done = true;
            continue;
        }

        const PathDef* leadPath = f.lead.pathRef >= 0 &&
                f.lead.pathRef < static_cast<int>(data_.paths.size())
            ? &data_.paths[static_cast<std::size_t>(f.lead.pathRef)]
            : nullptr;
        f.anchor = advanceMovement(f.lead, f.anchor, player_.pos, leadPath, 0.0f, dt);
        // A formation that leaves the playfield escapes without bonus.
        if (f.anchor.y > kPlayH + 80.0f || f.anchor.y < -220.0f ||
            f.anchor.x < -120.0f || f.anchor.x > kPlayW + 120.0f) {
            if (f.lead.t > 1.0f) f.escaped = true;
        }
        for (int idx : f.members) {
            Entity& e = pool_[static_cast<std::size_t>(idx)];
            const Vec2& off = f.offsets.empty()
                ? Vec2{0.0f, 0.0f}
                : f.offsets[static_cast<std::size_t>(
                      std::min(e.formationSlot, static_cast<int>(f.offsets.size()) - 1))];
            e.pos = f.anchor + off;
        }
    }
}

// ---------------------------------------------------------------------------
// Enemies
// ---------------------------------------------------------------------------
void Simulation::updateEnemies(float dt) {
    const float scroll = director_.stageDef() ? director_.stageDef()->scrollSpeed *
                                                    mods_.speedMul
                                              : 60.0f;
    for (std::size_t i = 0; i < pool_.size(); ++i) {
        Entity& e = pool_[i];
        if (!e.alive || e.kind != EKind::Enemy) continue;
        e.age += dt;

        const PathDef* path = e.move.pathRef >= 0
            ? &data_.paths[static_cast<std::size_t>(e.move.pathRef)]
            : nullptr;
        if (e.formation < 0) {
            e.pos = advanceMovement(e.move, e.pos, player_.pos, path, scroll, dt);
        } else {
            e.move.lastPos = e.pos;  // position owned by the formation controller
        }

        // Flash & smoke.
        e.flashTimer = std::max(0.0f, e.flashTimer - dt);
        const EnemyDef& def = data_.enemies[static_cast<std::size_t>(e.enemyDef)];
        if (def.smokeWhileDamaged > 0.0f &&
            static_cast<float>(e.hp) < static_cast<float>(def.hp) * def.smokeWhileDamaged &&
            rng_.chance(6.0f * dt)) {
            particles_.push_back({e.pos, {rng_.range(-8, 8), rng_.range(4, 20)},
                                  0.7f, 0.7f, rng_.range(2.0f, 4.0f),
                                  rgba(40, 40, 40, 160), 1.5f});
        }

        // Firing.
        if (e.weaponDef >= 0 && e.age > def.firstFireDelay &&
            e.pos.y > 4.0f && e.pos.y < kPlayH + 10.0f) {
            e.fireTimer -= dt;
            if (e.fireTimer <= 0.0f) {
                const WeaponDef& w = data_.weapons[static_cast<std::size_t>(e.weaponDef)];
                if (w.burstCount > 1 && e.aimAtPlayer) {
                    spawnShot(e.weaponDef, e.pos + w.muzzle,
                              playerAimDir(e.pos, player_.pos), Team::Enemy);
                    e.burstLeft = w.burstCount - 1;
                    e.burstTimer = w.burstDelay;
                } else {
                    spawnVolley(e.weaponDef, e.pos, Team::Enemy, e.aimAtPlayer);
                }
                e.fireTimer = def.fireInterval * e.rateMul;
                emitSound(w.sound.c_str());
            }
        }
        if (e.burstLeft > 0) {
            e.burstTimer -= dt;
            if (e.burstTimer <= 0.0f) {
                const WeaponDef& w = data_.weapons[static_cast<std::size_t>(e.weaponDef)];
                spawnShot(e.weaponDef, e.pos + w.muzzle,
                          playerAimDir(e.pos, player_.pos), Team::Enemy);
                --e.burstLeft;
                e.burstTimer = w.burstDelay;
            }
        }

        // Despawn when far off-screen.
        if (e.pos.y > kPlayH + kMargin * 2 || e.pos.y < -kMargin * 6 ||
            e.pos.x < -kMargin * 3 || e.pos.x > kPlayW + kMargin * 3) {
            if (e.move.t > 0.5f) kill(static_cast<int>(i));  // don't kill at spawn
        }
    }

    // Formation anchor cleanup.
    formations_.erase(std::remove_if(formations_.begin(), formations_.end(),
                                     [](const FormationInstance& f) {
                                         return f.done && f.members.empty();
                                     }),
                     formations_.end());
}

// ---------------------------------------------------------------------------
// Shots & power-ups
// ---------------------------------------------------------------------------
void Simulation::updateShots(float dt) {
    for (std::size_t i = 0; i < pool_.size(); ++i) {
        Entity& e = pool_[i];
        if (!e.alive || e.kind != EKind::Shot) continue;
        const WeaponDef& w = data_.weapons[static_cast<std::size_t>(e.weaponDef)];
        e.vel.y += w.gravity * dt;
        e.pos = e.pos + e.vel * dt;
        e.age += dt;
        e.lifeLeft -= dt;
        if (e.rotates) e.angle = angleOf(e.vel);
        if (e.lifeLeft <= 0.0f || e.pos.x < -16.0f || e.pos.x > kPlayW + 16.0f ||
            e.pos.y < -24.0f || e.pos.y > kPlayH + 24.0f) {
            kill(static_cast<int>(i));
        }
    }
}

void Simulation::updatePowerups(float dt) {
    for (std::size_t i = 0; i < pool_.size(); ++i) {
        Entity& e = pool_[i];
        if (!e.alive || e.kind != EKind::Powerup) continue;
        e.age += dt;
        e.lifeLeft -= dt;
        // Magnet toward the player when close.
        if (player_.alive) {
            const float d2 = distanceSq(e.pos, player_.pos);
            if (d2 < 90.0f * 90.0f) {
                e.vel = e.vel + normalize(player_.pos - e.pos) * (260.0f * dt);
                const float sp = length(e.vel);
                if (sp > 170.0f) e.vel = e.vel * (170.0f / sp);
            }
        }
        e.pos = e.pos + e.vel * dt;
        if (e.lifeLeft <= 0.0f || e.pos.y > kPlayH + 24.0f)
            kill(static_cast<int>(i));
    }
}

// ---------------------------------------------------------------------------
// Bosses
// ---------------------------------------------------------------------------
void Simulation::spawnBoss(int defRef) {
    if (defRef < 0 || defRef >= static_cast<int>(data_.bosses.size())) return;
    const BossDef& def = data_.bosses[static_cast<std::size_t>(defRef)];
    BossInstance b;
    b.defRef = defRef;
    b.pos = {192.0f, -90.0f};

    b.hull = alloc();
    {
        Entity& h = pool_[static_cast<std::size_t>(b.hull)];
        h.kind = EKind::BossHull;
        h.team = Team::Enemy;
        h.maxHp = h.hp = std::max(1, static_cast<int>(
                                         static_cast<float>(def.hullHp) * mods_.hpMul));
        h.pos = b.pos;
        h.sprite = def.hullSprite;
        h.bossInstance = static_cast<int>(bosses_.size());
        h.weaponDef = -1;
    }
    for (std::size_t c = 0; c < def.components.size(); ++c) {
        const BossComponentDef& cd = def.components[c];
        const int ci = alloc();
        Entity& e = pool_[static_cast<std::size_t>(ci)];
        e.kind = EKind::BossComponent;
        e.team = Team::Enemy;
        e.maxHp = e.hp = std::max(1, static_cast<int>(
                                         static_cast<float>(cd.hp) * mods_.hpMul));
        e.weaponDef = cd.weapon.empty() ? -1 : data_.findWeapon(cd.weapon);
        e.pos = b.pos + Vec2{static_cast<float>(cd.offsetX),
                             static_cast<float>(cd.offsetY)};
        e.sprite = cd.sprite;
        e.bossInstance = static_cast<int>(bosses_.size());
        e.componentIndex = static_cast<int>(c);
        e.rateMul = mods_.rateMul;
        e.fireTimer = cd.firstFireDelay;
        b.components.push_back(ci);
    }
    bosses_.push_back(std::move(b));
    events_.push_back({SimEvent::Type::BossSpawned, def.name, defRef, {}});
}

Simulation::BossView Simulation::bossView() const {
    BossView v;
    for (const auto& b : bosses_) {
        if (b.state >= 3) continue;
        const BossDef& def = data_.bosses[static_cast<std::size_t>(b.defRef)];
        int hp = 0, maxHp = def.hullHp;
        if (b.hull >= 0 && pool_[static_cast<std::size_t>(b.hull)].alive)
            hp += pool_[static_cast<std::size_t>(b.hull)].hp;
        for (std::size_t c = 0; c < b.components.size(); ++c) {
            maxHp += def.components[c].hp;
            const Entity& e = pool_[static_cast<std::size_t>(b.components[c])];
            if (e.alive) hp += e.hp;
        }
        v.active = true;
        v.name = def.name;
        v.hpFraction = maxHp > 0 ? static_cast<float>(hp) / static_cast<float>(maxHp) : 0.0f;
        break;  // one boss at a time
    }
    return v;
}

void Simulation::updateBosses(float dt) {
    for (auto& b : bosses_) {
        if (b.state >= 3) continue;
        const BossDef& def = data_.bosses[static_cast<std::size_t>(b.defRef)];
        b.stateTimer += dt;

        if (b.state == 0) {
            // Entry: climb down to entry line.
            b.pos.y += 45.0f * dt;
            if (b.pos.y >= def.entryY) {
                b.pos.y = def.entryY;
                b.state = 1;
                b.stateTimer = 0.0f;
            }
        } else if (b.state == 1) {
            // Hover sway.
            b.pos.x = 192.0f + def.hoverAmp *
                std::sin(2.0f * kPi * def.hoverFreq * b.stateTimer);
            b.pos.y = def.entryY + 8.0f * std::sin(2.0f * kPi * 0.07f * b.stateTimer);
        } else if (b.state == 2) {
            // Death chain.
            b.pos.y += 20.0f * dt;
            b.deathChain -= dt;
            if (b.deathChain <= 0.0f) {
                b.deathChain = 0.14f;
                explosions_.push_back({b.pos + Vec2{rng_.range(-70, 70),
                                                    rng_.range(-40, 40)},
                                       0.0f, rng_.chance(0.4f) ? 2 : 1, false});
                emitSound("explosion");
                // Blow remaining components as the ship breaks apart.
                for (int ci : b.components) {
                    Entity& e = pool_[static_cast<std::size_t>(ci)];
                    if (e.alive && rng_.chance(0.3f)) {
                        explosions_.push_back({e.pos, 0.0f, 1, false});
                        kill(ci);
                    }
                }
            }
            if (b.stateTimer > 2.6f) {
                b.state = 3;
                explosions_.push_back({b.pos, 0.0f, 2, false});
                explosions_.push_back({b.pos + Vec2{-40, -20}, 0.0f, 2, false});
                explosions_.push_back({b.pos + Vec2{40, 20}, 0.0f, 2, false});
                if (b.hull >= 0) kill(b.hull);
                spawnPowerup(data_.findPowerup(def.defeatDrop), b.pos);
                events_.push_back({SimEvent::Type::BossDefeated, "boss_defeated",
                                   def.hullScore, b.pos});
            }
            continue;  // components frozen during death sequence
        }

        // Move hull & components with the boss body.
        if (b.hull >= 0 && pool_[static_cast<std::size_t>(b.hull)].alive)
            pool_[static_cast<std::size_t>(b.hull)].pos = b.pos;

        // Phase from remaining total hp.
        int hp = 0, max = def.hullHp;
        for (std::size_t c = 0; c < b.components.size(); ++c) {
            max += def.components[c].hp;
            const Entity& e = pool_[static_cast<std::size_t>(b.components[c])];
            if (e.alive) hp += e.hp;
        }
        const float frac = max > 0 ? static_cast<float>(hp) / static_cast<float>(max) : 0.0f;
        int phase = 0;
        for (std::size_t p = 0; p < def.phases.size(); ++p) {
            if (frac <= def.phases[p].hpFraction) phase = static_cast<int>(p) + 1;
        }
        if (phase != b.phase) {
            b.phase = phase;
            emitSound("boss_phase");
        }
        const float phaseRate = phase > 0 && phase <= static_cast<int>(def.phases.size())
            ? def.phases[static_cast<std::size_t>(phase - 1)].rateMul
            : 1.0f;

        // Component behavior.
        for (std::size_t c = 0; c < b.components.size(); ++c) {
            Entity& e = pool_[static_cast<std::size_t>(b.components[c])];
            if (!e.alive) continue;
            e.pos = b.pos + Vec2{static_cast<float>(def.components[c].offsetX),
                                 static_cast<float>(def.components[c].offsetY)};
            e.flashTimer = std::max(0.0f, e.flashTimer - dt);
            if (b.state != 1) continue;
            if (e.weaponDef >= 0) {
                e.fireTimer -= dt;
                if (e.fireTimer <= 0.0f) {
                    const WeaponDef& w = data_.weapons[static_cast<std::size_t>(e.weaponDef)];
                    if (w.burstCount > 1) {
                        spawnShot(e.weaponDef, e.pos + w.muzzle,
                                  playerAimDir(e.pos, player_.pos), Team::Enemy);
                        e.burstLeft = w.burstCount - 1;
                        e.burstTimer = w.burstDelay;
                    } else {
                        spawnVolley(e.weaponDef, e.pos, Team::Enemy, true);
                    }
                    e.fireTimer = def.components[c].fireInterval *
                                  e.rateMul * phaseRate;
                    emitSound(w.sound.c_str());
                }
            }
            if (e.burstLeft > 0) {
                e.burstTimer -= dt;
                if (e.burstTimer <= 0.0f) {
                    const WeaponDef& w = data_.weapons[static_cast<std::size_t>(e.weaponDef)];
                    spawnShot(e.weaponDef, e.pos + w.muzzle,
                              playerAimDir(e.pos, player_.pos), Team::Enemy);
                    --e.burstLeft;
                    e.burstTimer = w.burstDelay;
                }
            }
        }

        // Extra phase weapon fired by the hull.
        if (b.state == 1 && phase > 0 &&
            phase <= static_cast<int>(def.phases.size())) {
            const BossPhaseDef& pd = def.phases[static_cast<std::size_t>(phase - 1)];
            if (!pd.extraWeapon.empty()) {
                b.extraTimer -= dt;
                if (b.extraTimer <= 0.0f) {
                    b.extraTimer = pd.extraInterval;
                    spawnVolley(data_.findWeapon(pd.extraWeapon), b.pos,
                                Team::Enemy, true);
                    emitSound("enemy_gun");
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Effects
// ---------------------------------------------------------------------------
void Simulation::updateEffects(float dt) {
    for (auto it = particles_.begin(); it != particles_.end();) {
        it->life -= dt;
        if (it->life <= 0.0f) {
            it = particles_.erase(it);
            continue;
        }
        it->vel = it->vel - it->vel * (it->drag * dt);
        it->pos = it->pos + it->vel * dt;
        ++it;
    }
    for (auto it = explosions_.begin(); it != explosions_.end();) {
        it->t += dt;
        const float dur = it->sizeClass == 0 ? 0.42f : (it->sizeClass == 1 ? 0.62f : 0.9f);
        if (it->t >= dur)
            it = explosions_.erase(it);
        else
            ++it;
    }
}

// ---------------------------------------------------------------------------
// Collision
// ---------------------------------------------------------------------------
void Simulation::buildColliders() {
    grid_.clear();
    colliderOwner_.clear();

    auto add = [&](int owner, std::uint32_t cat, Rect r) {
        grid_.insert(owner, cat, r);
        colliderOwner_.push_back(static_cast<std::uint16_t>(owner));
    };

    if (player_.alive && player_.specialTimer <= 0.0f) {
        // Tiny 10x10 hitbox at the cockpit.
        add(-1, kCatPlayer, rectFromCenter(player_.pos, 10.0f, 10.0f));
    }

    for (std::size_t i = 0; i < pool_.size(); ++i) {
        Entity& e = pool_[i];
        if (!e.alive) continue;
        switch (e.kind) {
        case EKind::Shot: {
            const WeaponDef& w = data_.weapons[static_cast<std::size_t>(e.weaponDef)];
            float sw = 5.0f, sh = 5.0f;
            shotSize(w.kind, sw, sh);
            add(static_cast<int>(i),
                e.team == Team::Player ? kCatPlayerProjectile : kCatEnemyProjectile,
                rectFromCenter(e.pos, sw, sh));
            break;
        }
        case EKind::Enemy: {
            const EnemyDef& def = data_.enemies[static_cast<std::size_t>(e.enemyDef)];
            add(static_cast<int>(i),
                def.ground ? kCatEnemyGround : kCatEnemyAir,
                {e.pos.x + static_cast<float>(def.hitX) - static_cast<float>(def.hitW) * 0.5f,
                 e.pos.y + static_cast<float>(def.hitY) - static_cast<float>(def.hitH) * 0.5f,
                 static_cast<float>(def.hitW), static_cast<float>(def.hitH)});
            break;
        }
        case EKind::Powerup:
            add(static_cast<int>(i), kCatPowerup, rectFromCenter(e.pos, 22.0f, 22.0f));
            break;
        case EKind::BossHull: {
            const BossDef& def = data_.bosses[static_cast<std::size_t>(
                bosses_[static_cast<std::size_t>(e.bossInstance)].defRef)];
            add(static_cast<int>(i), kCatBoss,
                rectFromCenter(e.pos, static_cast<float>(def.hullWidth) - 16.0f,
                               static_cast<float>(def.hullHeight) - 12.0f));
            break;
        }
        case EKind::BossComponent: {
            const BossDef& def = data_.bosses[static_cast<std::size_t>(
                bosses_[static_cast<std::size_t>(e.bossInstance)].defRef)];
            const BossComponentDef& cd = def.components[static_cast<std::size_t>(e.componentIndex)];
            add(static_cast<int>(i), kCatBoss,
                rectFromCenter(e.pos, static_cast<float>(cd.width) - 6.0f,
                               static_cast<float>(cd.height) - 6.0f));
            break;
        }
        }
    }
}

void Simulation::resolveCollisions() {
    constexpr std::uint32_t kAll =
        kCatPlayer | kCatPlayerProjectile | kCatEnemyAir | kCatEnemyGround |
        kCatEnemyProjectile | kCatPowerup | kCatBoss;

    struct Pending {
        int shot = -1;
        int target = -1;
        bool hitPlayer = false;
        bool takeShot = false;
        bool hullHit = false;  // boss hull armor: resolved after other hits
    };
    std::vector<Pending> pendings;

    grid_.queryPairs(kAll, kAll, [&](const Collider& a, const Collider& b) {
        const bool aPlayerShot = (a.category & kCatPlayerProjectile) != 0;
        const bool bPlayerShot = (b.category & kCatPlayerProjectile) != 0;
        const bool aEnemyTarget =
            (a.category & (kCatEnemyAir | kCatEnemyGround | kCatBoss)) != 0;
        const bool bEnemyTarget =
            (b.category & (kCatEnemyAir | kCatEnemyGround | kCatBoss)) != 0;
        const bool aEnemyShot = (a.category & kCatEnemyProjectile) != 0;
        const bool bEnemyShot = (b.category & kCatEnemyProjectile) != 0;
        const bool aPowerup = (a.category & kCatPowerup) != 0;
        const bool bPowerup = (b.category & kCatPowerup) != 0;

        // Player shot vs enemy target.
        if (aPlayerShot && bEnemyTarget)
            pendings.push_back({a.entityId, b.entityId, false, true});
        else if (bPlayerShot && aEnemyTarget)
            pendings.push_back({b.entityId, a.entityId, false, true});
        // Enemy shot vs player.
        else if ((aEnemyShot && (b.category & kCatPlayer)) ||
                 (bEnemyShot && (a.category & kCatPlayer))) {
            pendings.push_back({aEnemyShot ? a.entityId : b.entityId,
                                -1, true, true});
        }
        // Player vs enemy body.
        else if ((a.category & kCatPlayer) && bEnemyTarget)
            pendings.push_back({-1, b.entityId, true, false});
        else if ((b.category & kCatPlayer) && aEnemyTarget)
            pendings.push_back({-1, a.entityId, true, false});
        // Player vs power-up.
        else if ((a.category & kCatPlayer) && bPowerup)
            pendings.push_back({-1, b.entityId, false, false});
        else if ((b.category & kCatPlayer) && aPowerup)
            pendings.push_back({-1, a.entityId, false, false});
    });

    // Resolve pending hits. Order-independent thanks to alive checks; boss
    // hulls with living destructible components spark but do not consume
    // shots, letting them reach the component colliders inside the hull.
    for (const auto& p : pendings) {
        if (p.hitPlayer && p.target == -1) {
            // Enemy bullet hits the player.
            if (player_.invuln <= 0.0f && player_.alive) {
                if (p.shot >= 0 && pool_[static_cast<std::size_t>(p.shot)].alive)
                    kill(p.shot);
                damagePlayer();
            }
            continue;
        }
        if (p.hitPlayer && p.target >= 0) {
            // Ramming.
            if (player_.invuln <= 0.0f && player_.alive) {
                Entity& t = pool_[static_cast<std::size_t>(p.target)];
                if (t.alive) {
                    if (t.kind == EKind::Enemy &&
                        data_.enemies[static_cast<std::size_t>(t.enemyDef)].deathEffect <= 1 &&
                        !data_.enemies[static_cast<std::size_t>(t.enemyDef)].ground) {
                        killEnemy(p.target, true);
                    }
                    damagePlayer();
                }
            }
            continue;
        }
        if (p.target >= 0 && p.shot == -1) {
            // Power-up pickup.
            Entity& pu = pool_[static_cast<std::size_t>(p.target)];
            if (!pu.alive || pu.kind != EKind::Powerup || !player_.alive) continue;
            const PowerupDef& def = data_.powerups[static_cast<std::size_t>(pu.powerupDef)];
            switch (def.kind) {
            case PowerupKind::WeaponUp: {
                const int maxLv = weaponClassMaxLevel(player_.weapon);
                if (player_.weaponLevel < maxLv) ++player_.weaponLevel;
                else addScore(200);
                break;
            }
            case PowerupKind::Spread:
                if (player_.weapon != WeaponClass::Spread) player_.weapon = WeaponClass::Spread,
                    player_.weaponLevel = 1;
                else player_.weaponLevel = std::min(player_.weaponLevel + 1, 2);
                break;
            case PowerupKind::Cannon:
                if (player_.weapon != WeaponClass::Cannon) player_.weapon = WeaponClass::Cannon,
                    player_.weaponLevel = 1;
                else player_.weaponLevel = std::min(player_.weaponLevel + 1, 2);
                break;
            case PowerupKind::Rocket:
                if (player_.weapon != WeaponClass::Rocket) player_.weapon = WeaponClass::Rocket,
                    player_.weaponLevel = 1;
                else player_.weaponLevel = std::min(player_.weaponLevel + 1, 2);
                break;
            case PowerupKind::Bomb:
                player_.bombs = std::min(player_.bombs + 1, tuning::kBombCap);
                break;
            case PowerupKind::Life:
                ++player_.lives;
                break;
            case PowerupKind::Repair:
                player_.hp = std::min(player_.maxHp, player_.hp + 1);
                break;
            }
            addScore(50);
            events_.push_back({SimEvent::Type::PowerupTaken, def.name,
                               static_cast<int>(def.kind), pu.pos});
            emitSound("powerup");
            kill(p.target);
            continue;
        }

        // Player shot vs enemy target.
        if (p.shot < 0 || p.target < 0) continue;
        Entity& shot = pool_[static_cast<std::size_t>(p.shot)];
        Entity& target = pool_[static_cast<std::size_t>(p.target)];
        if (!shot.alive || !target.alive) continue;
        const int targetKind = static_cast<int>(target.kind);
        const int bossInst = target.bossInstance;
        const int compIndex = target.componentIndex;
        const bool consumeShot = shot.penetration <= 0;
        const int damage =
            data_.weapons[static_cast<std::size_t>(shot.weaponDef)].damage;

        // Boss hull is armored while destructible components remain.
        if (target.kind == EKind::BossHull) {
            const BossInstance& b = bosses_[static_cast<std::size_t>(target.bossInstance)];
            bool compsLeft = false;
            const BossDef& bdef = data_.bosses[static_cast<std::size_t>(b.defRef)];
            for (std::size_t c = 0; c < b.components.size(); ++c) {
                if (bdef.components[c].destructibleTarget &&
                    pool_[static_cast<std::size_t>(b.components[c])].alive) {
                    compsLeft = true;
                    break;
                }
            }
            if (compsLeft || b.state != 1) {
                // Armor: impact sparks; the shot passes through so it can
                // still hit the component colliders nested inside the hull.
                if (rng_.chance(0.35f))
                    particles_.push_back({shot.pos, {rng_.range(-30, 30), rng_.range(-60, -10)},
                                          0.2f, 0.2f, 2.0f, rgba(255, 240, 180, 255), 2.0f});
                continue;
            }
        }

        auto& tm = pool_[static_cast<std::size_t>(p.target)];
        tm.hp -= damage;
        tm.flashTimer = 0.08f;
        // Impact particles.
        particles_.push_back({shot.pos,
                              {rng_.range(-40, 40), rng_.range(-60, 0)},
                              0.18f, 0.18f, 2.5f, rgba(255, 255, 210, 255), 2.0f});
        if (tm.hp <= 0) {
            if (targetKind == static_cast<int>(EKind::Enemy)) {
                killEnemy(p.target, true);  // may grow pool: use indices below
            } else if (targetKind == static_cast<int>(EKind::BossComponent)) {
                const BossInstance& b = bosses_[static_cast<std::size_t>(bossInst)];
                const BossDef& bdef = data_.bosses[static_cast<std::size_t>(b.defRef)];
                const BossComponentDef& cd =
                    bdef.components[static_cast<std::size_t>(compIndex)];
                addScore(cd.score);
                explosions_.push_back({tm.pos, 0.0f, 1, false});
                events_.push_back({SimEvent::Type::BossComponentDestroyed,
                                   cd.name, cd.score, tm.pos});
                emitSound("explosion");
                kill(p.target);
            } else if (targetKind == static_cast<int>(EKind::BossHull)) {
                BossInstance& b = bosses_[static_cast<std::size_t>(bossInst)];
                const BossDef& bdef = data_.bosses[static_cast<std::size_t>(b.defRef)];
                b.state = 2;
                b.stateTimer = 0.0f;
                b.deathChain = 0.0f;
                addScore(bdef.hullScore);
                events_.push_back({SimEvent::Type::Sound, "explosion_large", 0, {}});
            }
        }
        if (consumeShot) kill(p.shot);
        else --pool_[static_cast<std::size_t>(p.shot)].penetration;
    }
}

// ---------------------------------------------------------------------------
// Damage & scoring
// ---------------------------------------------------------------------------
void Simulation::damagePlayer() {
    PlayerInternal& p = player_;
    if (!p.alive || p.invuln > 0.0f || godMode_) return;
    --p.hp;
    p.invuln = 2.0f;
    events_.push_back({SimEvent::Type::PlayerHit, "player_damage", p.hp, p.pos});
    emitSound("player_damage");
    explosions_.push_back({p.pos, 0.0f, 0, false});
    if (p.hp <= 0) {
        p.alive = false;
        --p.lives;
        p.respawnTimer = tuning::kRespawnDelay;
        explosions_.push_back({p.pos, 0.0f, 2, false});
        for (int k = 0; k < 24; ++k) {
            const Vec2 v = fromAngle(rng_.nextFloat() * 2.0f * kPi, rng_.range(30, 160));
            particles_.push_back({p.pos, v, rng_.range(0.4f, 0.9f), 0.9f,
                                  rng_.range(2.0f, 4.0f),
                                  rng_.chance(0.5f) ? rgba(255, 180, 60, 255)
                                                    : rgba(90, 90, 90, 200),
                                  1.2f});
        }
        emitSound("explosion_large");
        events_.push_back({SimEvent::Type::PlayerDied, "", p.lives, p.pos});
    }
}

void Simulation::killEnemy(int i, bool byPlayer) {
    Entity& e = pool_[static_cast<std::size_t>(i)];
    if (!e.alive || e.kind != EKind::Enemy) return;
    const EnemyDef& def = data_.enemies[static_cast<std::size_t>(e.enemyDef)];
    if (byPlayer) {
        addScore(def.score);
        events_.push_back({SimEvent::Type::EnemyKilled, def.name, def.score, e.pos});
    }
    const bool overWater = !def.ground;  // simple effect placement heuristic
    explosions_.push_back({e.pos, 0.0f, def.deathEffect, overWater && def.deathEffect > 0});
    emitSound(def.deathEffect >= 2 ? "explosion_large" : "explosion");

    // Debris/spark particles.
    const int puffs = 6 + def.deathEffect * 6;
    for (int k = 0; k < puffs; ++k) {
        const Vec2 v = fromAngle(rng_.nextFloat() * 2.0f * kPi,
                                  rng_.range(20.0f, 40.0f + static_cast<float>(def.deathEffect) * 50.0f));
        particles_.push_back({e.pos, v, rng_.range(0.3f, 0.8f), 0.8f,
                              rng_.range(1.5f, 3.5f),
                              rng_.chance(0.6f) ? rgba(255, 200, 80, 255)
                                                : rgba(120, 120, 120, 180),
                              1.5f});
    }

    // Drops.
    if (e.forceDrop >= 0) {
        spawnPowerup(e.forceDrop, e.pos);
    } else if (def.dropChance > 0.0f && rng_.chance(def.dropChance)) {
        if (def.dropKind == DropKind::WeaponSpecific) {
            spawnPowerup(data_.findPowerup(def.dropWeapon), e.pos);
        } else {
            // Weighted random drop table.
            static const int weights[] = {30, 12, 10, 12, 16, 10, 5};  // WeaponUp, Spread, Cannon, Rocket, Bomb, Repair, Life
            int total = 0;
            for (int wt : weights) total += wt;
            int roll = rng_.rangeInt(0, total - 1);
            int idx = 0;
            for (int k = 0; k < 7; ++k) {
                roll -= weights[k];
                if (roll < 0) { idx = k; break; }
            }
            static const char* dropNames[] = {"powerup_p", "powerup_spread",
                                              "powerup_cannon", "powerup_rocket",
                                              "powerup_bomb", "powerup_repair",
                                              "powerup_life"};
            spawnPowerup(data_.findPowerup(dropNames[idx]), e.pos);
        }
    }

    // Formation bookkeeping.
    if (e.formation >= 0) {
        for (auto& f : formations_)
            if (f.id == e.formation) { --f.aliveCount; break; }
    }
    kill(i);
}

}  // namespace ww
