#pragma once
// BlockFluid helpers shared by the game logic, the mesher and entity physics.
#include "world.h"

inline bool fluidSame(uint8_t id, bool water) { return water ? blockIsWater(id) : blockIsLava(id); }

// BlockFluid.getFluidHeightPercent: fraction of the block that is empty above the liquid.
inline float fluidHeightPercent(int meta) {
    if (meta >= 8) meta = 0;
    return (meta + 1) / 9.0f;
}

// BlockFluid.getEffectiveFlowDecay: -1 when not this liquid, falling liquid counts as 0.
inline int fluidEffectiveDecay(const World& w, int x, int y, int z, bool water) {
    if (!fluidSame(w.get(x, y, z), water)) return -1;
    int m = w.getMeta(x, y, z);
    return m >= 8 ? 0 : m;
}

// RenderBlocks.getFluidHeight: surface height at the corner shared by (x-1..x, z-1..z).
float fluidCornerHeight(const World& w, int x, int y, int z, bool water);

// BlockFluid.getFlowVector, normalized (zero when still).
void fluidFlow(const World& w, int x, int y, int z, bool water, float& fx, float& fy, float& fz);
