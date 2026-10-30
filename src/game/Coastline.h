#pragma once

#include <algorithm>
#include <cstddef>

#include "game/Simulation.h"  // terrainAt, StageDef, TerrainKind

namespace ww {

// ---------------------------------------------------------------------------
// Shared coastline surface contract between renderer and simulation.
//
// EnvironmentBaker draws the sea/land edge as a curved waterline around each
// shore band's outer boundary, wobbling up to kMaxWobble px past the band
// edge. The spawn gates must therefore judge ground units and ships against
// this margin - gating on the raw segment kind alone lets tanks wade in
// visible water at the band's sea-facing edge.
// ---------------------------------------------------------------------------
namespace coast {

// Wobble amplitudes of Environment.cpp's coastEdgeY (major + minor octave).
constexpr float kWobbleMajor = 22.0f;
constexpr float kWobbleMinor = 9.0f;
constexpr float kMaxWobble = (kWobbleMajor + kWobbleMinor) * 0.5f;

// A non-ship ground unit must be at least this far inside the land side of a
// shore band's water edge: worst-case waterline wobble + the foam fringe +
// formation-member offsets around the spawn point.
constexpr float kGroundMargin = 48.0f;
static_assert(kGroundMargin > kMaxWobble + 16.0f,
              "ground margin must clear the waterline wobble and foam");

// A ship's bow must stay this far seaward of a shore band's sea-facing edge:
// worst-case waterline wobble + the foam fringe + a little clearance, so a
// hull never reads as grounded on the rendered beach.
constexpr float kShipMargin = kMaxWobble + 8.0f;
static_assert(kShipMargin > kMaxWobble + 4.0f,
              "ship margin must clear the waterline wobble and foam");

}  // namespace coast

// True when the world row is a visually grounded surface for a land unit:
// any non-sea terrain, with shore bands only past their sea-facing margin
// (the curved rendered waterline can reach kMaxWobble px past the band edge).
inline bool groundSurfaceAt(const StageDef& stage, float worldY) {
    const TerrainKind kind = terrainAt(stage, worldY);
    if (kind == TerrainKind::Ocean) return false;
    if (kind != TerrainKind::Shore) return true;
    const auto& segs = stage.terrain;
    std::size_t idx = 0;
    for (std::size_t s = 0; s < segs.size(); ++s)
        if (worldY >= segs[s].fromY) idx = s;
    const bool seaLow =
        idx > 0 && segs[idx - 1].kind == TerrainKind::Ocean;
    const bool seaHigh =
        idx + 1 < segs.size() && segs[idx + 1].kind == TerrainKind::Ocean;
    if (seaLow && worldY < segs[idx].fromY + coast::kGroundMargin) return false;
    const float highEdge =
        idx + 1 < segs.size() ? segs[idx + 1].fromY : segs[idx].toY;
    if (seaHigh && worldY > highEdge - coast::kGroundMargin) return false;
    return true;
}

// True when the world row is visually open water for a surface vessel:
// Ocean, and clear of the curved rendered waterline fringe that the baker
// wobbles past any shore band's sea-facing edge. Ships use this to refuse
// self-motion that would beach them (the DriftDown pattern carries a vessel
// landward relative to the terrain at its own speed).
inline bool seaSurfaceAt(const StageDef& stage, float worldY) {
    if (terrainAt(stage, worldY) != TerrainKind::Ocean) return false;
    const auto& segs = stage.terrain;
    for (std::size_t s = 0; s < segs.size(); ++s) {
        if (segs[s].kind != TerrainKind::Shore) continue;
        // Rows inside the band itself never reach here (they are not
        // Ocean), so each sea-facing edge only endangers the fringe of
        // ocean rows just OUTSIDE it. The side bounds matter: testing the
        // margin without them rejects the entire ocean on one side of the
        // band (every row is trivially "< edge + margin"), the sea-hold
        // then never latches anywhere and ships drift landward unheld.
        // Ocean below the band: the fringe under the band's lower edge,
        // where the waterline bulges seaward.
        if (s > 0 && segs[s - 1].kind == TerrainKind::Ocean &&
            worldY > segs[s].fromY - coast::kShipMargin &&
            worldY < segs[s].fromY)
            return false;
        // Ocean above the band: the fringe past the band's top edge, where
        // the waterline bulges seaward into the ocean.
        if (s + 1 < segs.size() && segs[s + 1].kind == TerrainKind::Ocean &&
            worldY >= segs[s + 1].fromY &&
            worldY < segs[s + 1].fromY + coast::kShipMargin)
            return false;
    }
    return true;
}

}  // namespace ww
