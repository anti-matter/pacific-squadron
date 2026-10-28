#pragma once

#include <cstdint>
#include <string>

#include "graphics/ProceduralArt.h"

namespace ww {

// ---------------------------------------------------------------------------
// Environment art — high-quality procedural fallbacks for the landscape
// system (see Environment.h). These are the composition primitives the
// terrain baker stamps into its chunk textures: trees, vegetation, civilian
// and military buildings, ponds, rocks and props. Every generator returns a
// fully baked PixelBuf with an alpha channel and a baked bottom-right cast
// shadow (sun from the top-left).
//
// External PNG artwork is preferred for final quality: any file named
// assets/sprites/<name>.png overrides the generated pixels at startup.
// ---------------------------------------------------------------------------

// Register every environment sprite with `add(name, pixels)`. Called from
// generateAllArt so the sprites join the art cache and the
// WWII_DUMP_SPRITES PNG dump.
void registerEnvironmentSprites(
    const std::function<void(const std::string&, const PixelBuf&)>& add);

// All env_* sprite names (shared by the PNG override loader in
// ProceduralArt.cpp and the chunk baker in Environment.cpp).
inline constexpr const char* const kEnvSpriteNames[] = {
    "env_tree_broadleaf_0", "env_tree_broadleaf_1", "env_tree_broadleaf_2",
    "env_tree_small_0", "env_tree_small_1", "env_tree_palm_0", "env_tree_palm_1",
    "env_tree_conifer_0", "env_tree_conifer_1", "env_bush_0", "env_bush_1",
    "env_hedgerow_0", "env_hedgerow_1", "env_farmhouse_0", "env_farmhouse_1",
    "env_house_0", "env_house_1", "env_house_2", "env_barn_0", "env_barn_1",
    "env_shed_0", "env_shed_1", "env_warehouse_0", "env_warehouse_1",
    "env_barracks_0", "env_barracks_1", "env_fuel_tank_0", "env_fuel_tank_1",
    "env_haystack", "env_well", "env_rock_0", "env_rock_1", "env_pond",
};
inline constexpr std::size_t kEnvSpriteCount =
    sizeof(kEnvSpriteNames) / sizeof(kEnvSpriteNames[0]);

// Names exported by registerEnvironmentSprites are all env_* (see
// kEnvSpriteNames above); the baker looks them up by name.

}  // namespace ww
