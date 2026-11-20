#pragma once
#include <cstdint>

#include "world.h"

// EffectRenderer and the EntityFX subclasses (World.spawnParticle names in the comments).
enum ParticleKind : uint8_t {
    PART_SMOKE,    // "smoke" and "largesmoke" (EntitySmokeFX)
    PART_FLAME,    // EntityFlameFX
    PART_EXPLODE,  // EntityExplodeFX
    PART_BUBBLE,   // EntityBubbleFX
    PART_SPLASH,   // EntitySplashFX
    PART_RAIN,     // EntityRainFX
    PART_REDDUST,  // EntityReddustFX
    PART_PORTAL,   // EntityPortalFX
    PART_NOTE,     // EntityNoteFX
    PART_HEART,    // EntityHeartFX
    PART_LAVA,     // EntityLavaFX
    PART_DIGGING,  // EntityDiggingFX (terrain.png)
    PART_ITEM,     // EntitySlimeFX: "snowballpoof" and "slime" (gui/items.png)
};

// Texture each particle samples: particles.png, terrain.png or gui/items.png.
enum ParticleLayer : uint8_t { LAYER_PARTICLES, LAYER_TERRAIN, LAYER_ITEMS };

struct Particle {
    float x, y, z, px, py, pz, mx, my, mz;
    float r, g, b;
    float scale, baseScale, gravity, half;  // half: half the collision box size
    float ox, oy, oz;                       // portal: spawn point
    float u, v;                             // digging and item pieces: offset inside the tile (0..3 quarters)
    int16_t age, maxAge;
    uint16_t tex;  // tile index (16 per row)
    uint8_t kind, layer;
    bool onGround, noClip;
};

class Particles {
public:
    static constexpr int MAX = 400;
    // 0 all, 1 decreased (half of the ambient particles), 2 minimal (only block pieces).
    int level = 0;
    // Where the camera is: particles further than 16 blocks are not spawned (RenderGlobal.spawnParticle).
    float viewX = 0, viewY = 0, viewZ = 0;
    Particle p[MAX];
    int count = 0;

    void clear() { count = 0; }
    void tick(const World& w);
    // World.spawnParticle; `extra` scales "largesmoke" (2.5) and reddust.
    Particle* spawn(const World& w, ParticleKind k, float x, float y, float z, float vx, float vy, float vz,
                    float extra = 1.0f);
    // EffectRenderer.addBlockDestroyEffects / addBlockHitEffects.
    void blockDestroy(const World& w, int x, int y, int z, uint8_t id, int meta);
    void blockHit(const World& w, int x, int y, int z, int side);
    // EntitySlimeFX with an item icon.
    void itemPiece(const World& w, float x, float y, float z, uint16_t icon);

    // Size and light factor at render time (EntityFX.renderParticle overrides and getEntityBrightness).
    float renderScale(const Particle& q, float partial) const;
    float brightness(const Particle& q, float worldBrightness, float partial) const;

    float rnd();  // Math.random

private:
    Particle* alloc();
    void base(Particle& q, float x, float y, float z, float vx, float vy, float vz);
    void move(const World& w, Particle& q, float dx, float dy, float dz);
    bool tickOne(const World& w, Particle& q);
    uint32_t seed_ = 0x2545F491u;
    int victim_ = 0;
};
