#include "game/GameData.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "core/MathUtil.h"

namespace ww {
namespace {

using nlohmann::json;

std::string g_error;

void fail(const std::string& msg) { g_error = msg; }

float getF(const json& j, const char* key, float def) {
    if (j.contains(key) && j[key].is_number()) return j[key].get<float>();
    return def;
}
int getI(const json& j, const char* key, int def) {
    if (j.contains(key) && j[key].is_number_integer()) return j[key].get<int>();
    return def;
}
bool getB(const json& j, const char* key, bool def) {
    if (j.contains(key) && j[key].is_boolean()) return j[key].get<bool>();
    return def;
}
std::string getS(const json& j, const char* key, const std::string& def) {
    if (j.contains(key) && j[key].is_string()) return j[key].get<std::string>();
    return def;
}

MovePattern patternFromName(const std::string& s, MovePattern def) {
    if (s == "straight") return MovePattern::Straight;
    if (s == "sine_down") return MovePattern::SineDown;
    if (s == "sine_side") return MovePattern::SineSide;
    if (s == "circular") return MovePattern::Circular;
    if (s == "bezier") return MovePattern::Bezier;
    if (s == "waypoints") return MovePattern::Waypoints;
    if (s == "dive") return MovePattern::DiveAtPlayer;
    if (s == "formation") return MovePattern::Formation;
    if (s == "static") return MovePattern::Static;
    if (s == "drift_down") return MovePattern::DriftDown;
    return def;
}

ShotPattern shotPatternFromName(const std::string& s) {
    if (s == "aimed") return ShotPattern::Aimed;
    if (s == "spread") return ShotPattern::Spread;
    if (s == "radial") return ShotPattern::Radial;
    if (s == "burst") return ShotPattern::Burst;
    if (s == "aim_spread") return ShotPattern::AimSpread;
    return ShotPattern::Forward;
}

ShotKind shotKindFromName(const std::string& s) {
    if (s == "player_cannon") return ShotKind::PlayerCannon;
    if (s == "player_rocket") return ShotKind::PlayerRocket;
    if (s == "enemy_shell") return ShotKind::EnemyShell;
    if (s == "enemy_flak") return ShotKind::EnemyFlak;
    if (s == "enemy_bullet") return ShotKind::EnemyBullet;
    return ShotKind::PlayerBullet;
}

PowerupKind powerupKindFromName(const std::string& s) {
    if (s == "spread") return PowerupKind::Spread;
    if (s == "cannon") return PowerupKind::Cannon;
    if (s == "rocket") return PowerupKind::Rocket;
    if (s == "bomb") return PowerupKind::Bomb;
    if (s == "life") return PowerupKind::Life;
    if (s == "repair") return PowerupKind::Repair;
    return PowerupKind::WeaponUp;
}

TerrainKind terrainFromName(const std::string& s) {
    if (s == "island") return TerrainKind::Island;
    if (s == "shore") return TerrainKind::Shore;
    if (s == "jungle") return TerrainKind::Jungle;
    if (s == "road") return TerrainKind::Road;
    if (s == "airfield") return TerrainKind::Airfield;
    if (s == "fortress") return TerrainKind::Fortress;
    return TerrainKind::Ocean;
}

// Expects the JSON text to be an array of objects.
bool parseArray(const std::string& text, json& out, const char* what) {
    json j;
    try {
        j = json::parse(text);
    } catch (const json::exception& e) {
        fail(std::string(what) + ": JSON parse error: " + e.what());
        return false;
    }
    if (!j.is_array()) {
        fail(std::string(what) + ": expected a top-level array");
        return false;
    }
    out = std::move(j);
    return true;
}

std::string readFile(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

// ---------------------------------------------------------------------------

FormationShape formationShapeFromName(const std::string& name) {
    if (name == "v_formation") return FormationShape::VFormation;
    if (name == "h_line") return FormationShape::HLine;
    if (name == "v_line") return FormationShape::VLine;
    if (name == "arc") return FormationShape::Arc;
    if (name == "staggered") return FormationShape::Staggered;
    if (name == "wave_left") return FormationShape::SideWaveLeft;
    if (name == "wave_right") return FormationShape::SideWaveRight;
    if (name == "wedge") return FormationShape::Wedge;
    return FormationShape::Single;
}

int GameData::findWeapon(std::string_view name) const {
    if (name.empty()) return -1;
    for (std::size_t i = 0; i < weapons.size(); ++i)
        if (weapons[i].name == name) return static_cast<int>(i);
    return -1;
}
int GameData::findEnemy(std::string_view name) const {
    if (name.empty()) return -1;
    for (std::size_t i = 0; i < enemies.size(); ++i)
        if (enemies[i].name == name) return static_cast<int>(i);
    return -1;
}
int GameData::findBoss(std::string_view name) const {
    if (name.empty()) return -1;
    for (std::size_t i = 0; i < bosses.size(); ++i)
        if (bosses[i].name == name) return static_cast<int>(i);
    return -1;
}
int GameData::findPath(std::string_view name) const {
    if (name.empty()) return -1;
    for (std::size_t i = 0; i < paths.size(); ++i)
        if (paths[i].name == name) return static_cast<int>(i);
    return -1;
}
int GameData::findPowerup(std::string_view name) const {
    if (name.empty()) return -1;
    for (std::size_t i = 0; i < powerups.size(); ++i)
        if (powerups[i].name == name) return static_cast<int>(i);
    return -1;
}
int GameData::findAnim(std::string_view name) const {
    if (name.empty()) return -1;
    for (std::size_t i = 0; i < anims.size(); ++i)
        if (anims[i].name == name) return static_cast<int>(i);
    return -1;
}

// ---------------------------------------------------------------------------

bool parseAnimationsJson(const std::string& text, std::vector<AnimDef>& out, std::string& error) {
    json arr;
    if (!parseArray(text, arr, "animations")) { error = g_error; return false; }
    for (const auto& j : arr) {
        if (!j.is_object() || !j.contains("name") || !j["name"].is_string()) {
            error = "animations: entry missing 'name'";
            return false;
        }
        AnimDef a;
        a.name = j["name"].get<std::string>();
        a.frameW = getI(j, "frameWidth", 32);
        a.frameH = getI(j, "frameHeight", 32);
        a.frames = std::max(1, getI(j, "frames", 1));
        a.fps = std::max(0.1f, getF(j, "fps", 12.0f));
        a.loop = getB(j, "loop", false);
        if (j.contains("pivot") && j["pivot"].is_array() && j["pivot"].size() >= 2) {
            a.pivot = {j["pivot"][0].get<float>(), j["pivot"][1].get<float>()};
        }
        out.push_back(std::move(a));
    }
    return true;
}

bool parseWeaponsJson(const std::string& text, std::vector<WeaponDef>& out, std::string& error) {
    json arr;
    if (!parseArray(text, arr, "weapons")) { error = g_error; return false; }
    for (const auto& j : arr) {
        if (!j.is_object() || !j.contains("name") || !j["name"].is_string()) {
            error = "weapons: entry missing 'name'";
            return false;
        }
        WeaponDef w;
        w.name = j["name"].get<std::string>();
        w.pattern = shotPatternFromName(getS(j, "pattern", "forward"));
        w.kind = shotKindFromName(getS(j, "kind", "player_bullet"));
        w.sprite = getS(j, "sprite", w.kind == ShotKind::PlayerBullet ||
                                         w.kind == ShotKind::PlayerCannon ||
                                         w.kind == ShotKind::PlayerRocket
                                 ? "bullet_player" : "bullet_enemy");
        w.sound = getS(j, "sound", "player_gun");
        w.damage = std::max(1, getI(j, "damage", 1));
        w.interval = std::max(0.02f, getF(j, "fireInterval", 0.15f));
        w.speed = std::max(10.0f, getF(j, "speed", 400.0f));
        w.count = std::max(1, getI(j, "count", 1));
        w.spreadDeg = getF(j, "spreadDeg", 0.0f);
        w.lifetime = std::max(0.05f, getF(j, "lifetime", 2.0f));
        w.penetration = std::max(0, getI(j, "penetration", 0));
        w.burstCount = std::max(1, getI(j, "burstCount", 1));
        w.burstDelay = std::max(0.01f, getF(j, "burstDelay", 0.06f));
        w.baseAngleDeg = getF(j, "baseAngleDeg", -90.0f);
        w.aimJitterDeg = getF(j, "aimJitterDeg", 0.0f);
        if (j.contains("muzzle") && j["muzzle"].is_array() && j["muzzle"].size() >= 2) {
            w.muzzle = {j["muzzle"][0].get<float>(), j["muzzle"][1].get<float>()};
        }
        w.gravity = getF(j, "gravity", 0.0f);
        w.rotates = getB(j, "rotates", false);
        out.push_back(std::move(w));
    }
    return true;
}

bool parseEnemiesJson(const std::string& text, std::vector<EnemyDef>& out, std::string& error) {
    json arr;
    if (!parseArray(text, arr, "enemies")) { error = g_error; return false; }
    for (const auto& j : arr) {
        if (!j.is_object() || !j.contains("name") || !j["name"].is_string()) {
            error = "enemies: entry missing 'name'";
            return false;
        }
        EnemyDef e;
        e.name = j["name"].get<std::string>();
        e.sprite = getS(j, "sprite", e.sprite);
        e.width = getI(j, "width", e.width);
        e.height = getI(j, "height", e.height);
        if (j.contains("hitbox") && j["hitbox"].is_array() && j["hitbox"].size() >= 4) {
            e.hitX = j["hitbox"][0].get<int>();
            e.hitY = j["hitbox"][1].get<int>();
            e.hitW = j["hitbox"][2].get<int>();
            e.hitH = j["hitbox"][3].get<int>();
        }
        e.hp = std::max(1, getI(j, "hp", 1));
        e.score = getI(j, "score", 100);
        e.speed = getF(j, "speed", 80.0f);
        e.pattern = patternFromName(getS(j, "path", "sine_down"), e.pattern);
        e.amp = getF(j, "amp", e.amp);
        e.freq = getF(j, "freq", e.freq);
        e.angleDeg = getF(j, "angleDeg", e.angleDeg);
        e.diveDelay = getF(j, "diveDelay", e.diveDelay);
        e.path = getS(j, "pathRef", "");
        e.weapon = getS(j, "weapon", "");
        e.fireInterval = std::max(0.1f, getF(j, "fireInterval", e.fireInterval));
        e.firstFireDelay = std::max(0.0f, getF(j, "firstFireDelay", e.firstFireDelay));
        e.burstOverride = getI(j, "burstOverride", -1);
        e.aimAtPlayer = getB(j, "aimAtPlayer", e.aimAtPlayer);
        e.ground = getB(j, "ground", false);
        e.ship = getB(j, "ship", false);
        e.dropChance = clamp(getF(j, "dropChance", 0.0f), 0.0f, 1.0f);
        const std::string drop = getS(j, "drop", "none");
        if (drop == "random") e.dropKind = DropKind::Random;
        else if (!drop.empty() && drop != "none") {
            e.dropKind = DropKind::WeaponSpecific;
            e.dropWeapon = drop;
        }
        e.deathEffect = clamp(getI(j, "deathEffect", 1), 0, 2);
        e.deathSound = getS(j, "deathSound", "");
        if (e.deathSound.empty())
            e.deathSound = e.deathEffect >= 2 ? "explosion_large" : "explosion_small";
        e.smokeWhileDamaged = getF(j, "smokeWhileDamaged", e.smokeWhileDamaged);
        out.push_back(std::move(e));
    }
    return true;
}

bool parsePowerupsJson(const std::string& text, std::vector<PowerupDef>& out, std::string& error) {
    json arr;
    if (!parseArray(text, arr, "powerups")) { error = g_error; return false; }
    for (const auto& j : arr) {
        if (!j.is_object() || !j.contains("name") || !j["name"].is_string()) {
            error = "powerups: entry missing 'name'";
            return false;
        }
        PowerupDef p;
        p.name = j["name"].get<std::string>();
        p.kind = powerupKindFromName(getS(j, "effect", "weapon_up"));
        p.sprite = getS(j, "sprite", "powerup_p");
        p.fallSpeed = getF(j, "fallSpeed", 60.0f);
        out.push_back(std::move(p));
    }
    return true;
}

bool parseBossesJson(const std::string& text, std::vector<BossDef>& out, std::string& error) {
    json arr;
    if (!parseArray(text, arr, "bosses")) { error = g_error; return false; }
    for (const auto& j : arr) {
        if (!j.is_object() || !j.contains("name") || !j["name"].is_string()) {
            error = "bosses: entry missing 'name'";
            return false;
        }
        BossDef b;
        b.name = j["name"].get<std::string>();
        b.hullSprite = getS(j, "hullSprite", b.hullSprite);
        b.hullWidth = getI(j, "hullWidth", b.hullWidth);
        b.hullHeight = getI(j, "hullHeight", b.hullHeight);
        b.hullHp = std::max(1, getI(j, "hullHp", b.hullHp));
        b.hullScore = getI(j, "hullScore", b.hullScore);
        b.entryY = getF(j, "entryY", b.entryY);
        b.hoverAmp = getF(j, "hoverAmp", b.hoverAmp);
        b.hoverFreq = getF(j, "hoverFreq", b.hoverFreq);
        b.deathEffect = clamp(getI(j, "deathEffect", 2), 0, 2);
        b.defeatDrop = getS(j, "defeatDrop", b.defeatDrop);
        b.sortieEnemy = getS(j, "sortieEnemy", b.sortieEnemy);
        b.sortieInterval = std::max(0.0f, getF(j, "sortieInterval", b.sortieInterval));
        b.sailSpeed = std::max(0.0f, getF(j, "sailSpeed", b.sailSpeed));
        b.sortieCount = clamp(getI(j, "sortieCount", 2), 1, 6);
        b.arenaOcean = getB(j, "arenaOcean", b.arenaOcean);
        if (j.contains("components") && j["components"].is_array()) {
            for (const auto& c : j["components"]) {
                if (!c.is_object() || !c.contains("name") || !c["name"].is_string()) {
                    error = "bosses: component missing 'name'";
                    return false;
                }
                BossComponentDef cd;
                cd.name = c["name"].get<std::string>();
                cd.sprite = getS(c, "sprite", "boss_turret");
                cd.offsetX = getI(c, "offsetX", 0);
                cd.offsetY = getI(c, "offsetY", 0);
                cd.width = getI(c, "width", 32);
                cd.height = getI(c, "height", 32);
                cd.hp = std::max(1, getI(c, "hp", 20));
                cd.score = getI(c, "score", 500);
                cd.weapon = getS(c, "weapon", "");
                cd.fireInterval = std::max(0.1f, getF(c, "fireInterval", 1.5f));
                cd.firstFireDelay = getF(c, "firstFireDelay", 1.0f);
                cd.destructibleTarget = getB(c, "destructibleTarget", true);
                cd.aimAtPlayer = getB(c, "aimAtPlayer", false);
                b.components.push_back(std::move(cd));
            }
        }
        if (j.contains("phases") && j["phases"].is_array()) {
            for (const auto& p : j["phases"]) {
                if (!p.is_object()) continue;
                BossPhaseDef pd;
                pd.hpFraction = clamp(getF(p, "hpFraction", 0.5f), 0.0f, 1.0f);
                pd.rateMul = std::max(0.1f, getF(p, "rateMul", 1.0f));
                pd.extraWeapon = getS(p, "extraWeapon", "");
                pd.extraInterval = std::max(0.1f, getF(p, "extraInterval", 2.0f));
                b.phases.push_back(pd);
            }
        }
        out.push_back(std::move(b));
    }
    return true;
}

bool parseStageJson(const std::string& text, StageDef& out, std::string& error) {
    json j;
    try {
        j = json::parse(text);
    } catch (const json::exception& e) {
        error = std::string("stage: JSON parse error: ") + e.what();
        return false;
    }
    if (!j.is_object()) { error = "stage: expected an object"; return false; }
    out.name = getS(j, "name", "unnamed stage");
    out.duration = std::max(5.0f, getF(j, "duration", 90.0f));
    out.scrollSpeed = std::max(5.0f, getF(j, "scrollSpeed", 60.0f));
    out.boss = getS(j, "boss", "");
    out.bossTime = getF(j, "bossTime", out.duration - 10.0f);

    if (j.contains("paths") && j["paths"].is_array()) {
        // Stage-local named paths are appended later by loadAll into
        // GameData::paths; here we stash them into events only via names.
    }
    if (j.contains("terrain") && j["terrain"].is_array()) {
        for (const auto& t : j["terrain"]) {
            if (!t.is_object()) continue;
            TerrainSegment seg;
            seg.fromY = getF(t, "fromY", 0.0f);
            seg.toY = getF(t, "toY", 1e9f);
            seg.kind = terrainFromName(getS(t, "kind", "ocean"));
            out.terrain.push_back(seg);
        }
    }
    if (j.contains("events") && j["events"].is_array()) {
        for (const auto& e : j["events"]) {
            if (!e.is_object()) continue;
            SpawnEvent ev;
            ev.trigger = std::max(0.0f, getF(e, "trigger", 0.0f));
            ev.enemy = getS(e, "enemy", "");
            ev.formation = getS(e, "formation", "single");
            ev.count = std::max(1, getI(e, "count", 1));
            ev.path = getS(e, "movement", "");
            ev.x = getF(e, "x", 192.0f);
            ev.y = getF(e, "y", -30.0f);
            ev.delayBetween = std::max(0.0f, getF(e, "delayBetween", 0.15f));
            ev.spacing = std::max(4.0f, getF(e, "spacing", 46.0f));
            ev.hpMul = std::max(0.1f, getF(e, "hpMul", 1.0f));
            ev.rateMul = std::max(0.05f, getF(e, "rateMul", 1.0f));
            ev.speedMul = std::max(0.1f, getF(e, "speedMul", 1.0f));
            ev.boss = getS(e, "boss", "");
            out.events.push_back(std::move(ev));
        }
        std::stable_sort(out.events.begin(), out.events.end(),
                         [](const SpawnEvent& a, const SpawnEvent& b) {
                             return a.trigger < b.trigger;
                         });
    }
    return true;
}

// ---------------------------------------------------------------------------

bool GameData::linkReferences(std::string& error) {
    for (auto& e : enemies) {
        if (!e.weapon.empty()) {
            e.weaponRef = findWeapon(e.weapon);
            if (e.weaponRef < 0) {
                error = "enemy '" + e.name + "' references unknown weapon '" + e.weapon + "'";
                return false;
            }
        }
        if (!e.path.empty()) {
            e.pathRef = findPath(e.path);
            if (e.pathRef < 0) {
                error = "enemy '" + e.name + "' references unknown path '" + e.path + "'";
                return false;
            }
        }
        if (e.dropKind == DropKind::WeaponSpecific) {
            if (findPowerup(e.dropWeapon) < 0) {
                error = "enemy '" + e.name + "' references unknown drop '" + e.dropWeapon + "'";
                return false;
            }
        }
    }
    for (auto& stage : stages) {
        if (!stage.boss.empty()) {
            stage.bossRef = findBoss(stage.boss);
            if (stage.bossRef < 0) {
                error = "stage '" + stage.name + "' references unknown boss '" + stage.boss + "'";
                return false;
            }
        }
        for (auto& ev : stage.events) {
            if (!ev.boss.empty()) {
                ev.bossRef = findBoss(ev.boss);
                if (ev.bossRef < 0) {
                    error = "stage '" + stage.name + "' event references unknown boss '" + ev.boss + "'";
                    return false;
                }
            } else {
                ev.enemyRef = findEnemy(ev.enemy);
                if (ev.enemyRef < 0) {
                    error = "stage '" + stage.name + "' event references unknown enemy '" + ev.enemy + "'";
                    return false;
                }
            }
            if (!ev.path.empty()) {
                ev.pathRef = findPath(ev.path);
                if (ev.pathRef < 0) {
                    error = "stage '" + stage.name + "' event references unknown path '" + ev.path + "'";
                    return false;
                }
            }
            if (ev.forceDrop >= 0 && ev.forceDrop >= static_cast<int>(powerups.size())) {
                error = "stage '" + stage.name + "' event has invalid forceDrop index";
                return false;
            }
        }
    }
    for (auto& b : bosses) {
        if (!b.sortieEnemy.empty() && findEnemy(b.sortieEnemy) < 0) {
            error = "boss '" + b.name + "' sorties unknown enemy '" + b.sortieEnemy + "'";
            return false;
        }
        for (auto& c : b.components) {
            if (!c.weapon.empty() && findWeapon(c.weapon) < 0) {
                error = "boss '" + b.name + "' component '" + c.name + "' references unknown weapon '" + c.weapon + "'";
                return false;
            }
        }
        for (auto& p : b.phases) {
            if (!p.extraWeapon.empty() && findWeapon(p.extraWeapon) < 0) {
                error = "boss '" + b.name + "' phase references unknown weapon '" + p.extraWeapon + "'";
                return false;
            }
        }
    }
    return true;
}

bool GameData::loadAll(const std::string& assetDir, std::string& error) {
    namespace fs = std::filesystem;
    const fs::path base(assetDir);

    auto loadCategory = [&](const char* sub, const char* file, auto&& parseFn, auto& vec) -> bool {
        // Support a single file (data/<file>.json) or a directory of files.
        std::vector<fs::path> files;
        const fs::path single = base / "data" / sub / (std::string(file) + ".json");
        if (fs::exists(single)) files.push_back(single);
        const fs::path dir = base / "data" / sub;
        if (fs::is_directory(dir)) {
            for (const auto& entry : fs::directory_iterator(dir))
                if (entry.is_regular_file() && entry.path().extension() == ".json")
                    files.push_back(entry.path());
        }
        std::sort(files.begin(), files.end());
        for (const auto& f : files) {
            const std::string text = readFile(f);
            if (text.empty()) {
                error = "empty or missing data file: " + f.string();
                return false;
            }
            if (!parseFn(text, vec, error)) return false;
        }
        return true;
    };

    if (!loadCategory("animations", "animations", parseAnimationsJson, anims)) return false;
    if (!loadCategory("weapons", "weapons", parseWeaponsJson, weapons)) return false;
    if (!loadCategory("powerups", "powerups", parsePowerupsJson, powerups)) return false;
    if (!loadCategory("enemies", "enemies", parseEnemiesJson, enemies)) return false;
    if (!loadCategory("bosses", "bosses", parseBossesJson, bosses)) return false;

    // Paths: data/paths/*.json  [{name, points: [[x,y],...]}, ...]
    {
        std::vector<PathDef>& pathsVec = this->paths;
        if (!loadCategory("paths", "paths",
                          [](const std::string& text, std::vector<PathDef>& out, std::string& err) {
                              json arr;
                              if (!parseArray(text, arr, "paths")) { err = g_error; return false; }
                              for (const auto& j : arr) {
                                  if (!j.is_object() || !j.contains("name")) continue;
                                  PathDef p;
                                  p.name = j["name"].get<std::string>();
                                  if (j.contains("points") && j["points"].is_array()) {
                                      for (const auto& pt : j["points"]) {
                                          if (pt.is_array() && pt.size() >= 2)
                                              p.points.push_back({pt[0].get<float>(), pt[1].get<float>()});
                                      }
                                  }
                                  if (p.points.size() >= 2) out.push_back(std::move(p));
                              }
                              return true;
                          },
                          pathsVec))
            return false;
    }

    // Stages: data/stages/*.json, sorted by filename for ordering.
    {
        const fs::path stageDir = base / "data" / "stages";
        std::vector<fs::path> files;
        if (fs::is_directory(stageDir)) {
            for (const auto& entry : fs::directory_iterator(stageDir))
                if (entry.is_regular_file() && entry.path().extension() == ".json")
                    files.push_back(entry.path());
        }
        std::sort(files.begin(), files.end());
        for (const auto& f : files) {
            StageDef stage;
            const std::string text = readFile(f);
            if (text.empty()) {
                error = "empty stage file: " + f.string();
                return false;
            }
            if (!parseStageJson(text, stage, error)) return false;
            stages.push_back(std::move(stage));
        }
    }

    return linkReferences(error);
}

}  // namespace ww
