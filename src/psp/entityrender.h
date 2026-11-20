#pragma once
#include "../core/game.h"
#include "renderer.h"

// Mobs (Beta box models), dropped items, arrows and primed TNT. Runs inside the renderer's 3D pass.
void drawEntities(Game& g, const Camera& cam, float partial);

// First-person held item or bare arm (ItemRenderer). Equip animation state advances once per game tick.
struct HandState {
    float equip = 0, prevEquip = 0;
    ItemStack item;
    int slot = -1;
    int resets = 0;
    void tick(const Game& g);
};
void drawHand(Game& g, const Camera& cam, float partial, const HandState& h);

// Particles, falling rain and snow, and lightning bolts (effectrender.cpp). Runs after translucent terrain.
void drawEffects(const Game& g, const Camera& cam, float partial, bool fancy);
