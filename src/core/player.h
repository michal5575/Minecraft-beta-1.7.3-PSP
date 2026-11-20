#pragma once
#include "world.h"

struct AABB {
    float x0, y0, z0, x1, y1, z1;

    AABB offset(float dx, float dy, float dz) const { return {x0 + dx, y0 + dy, z0 + dz, x1 + dx, y1 + dy, z1 + dz}; }
    AABB expand(float dx, float dy, float dz) const {
        AABB b = *this;
        if (dx < 0) b.x0 += dx; else b.x1 += dx;
        if (dy < 0) b.y0 += dy; else b.y1 += dy;
        if (dz < 0) b.z0 += dz; else b.z1 += dz;
        return b;
    }
    AABB grow(float g) const { return {x0 - g, y0 - g, z0 - g, x1 + g, y1 + g, z1 + g}; }
    bool intersects(const AABB& o) const {
        return o.x1 > x0 && o.x0 < x1 && o.y1 > y0 && o.y0 < y1 && o.z1 > z0 && o.z0 < z1;
    }
};

// Block-local bounds (0..1); false when the block has no box of that kind.
bool blockSelectionBox(const World& w, int x, int y, int z, uint8_t id, AABB& out);
// Up to 2 block-local collision boxes; returns the count.
int blockCollisionBoxes(const World& w, int x, int y, int z, uint8_t id, AABB* out);
// Shape used by the item icon and placement checks (no world context).
bool blockShapeBox(uint8_t id, int meta, AABB& out);
// Collision of a block being moved by a piston, at its position at the start of the tick.
int pistonMovingBoxes(const World& w, int x, int y, int z, AABB* out);

struct RayHit {
    int x, y, z;
    int face;  // Beta side numbering: 0 bottom, 1 top, 2 -z, 3 +z, 4 -x, 5 +x
    float t;
};

bool raycast(const World& w, float ox, float oy, float oz, float dx, float dy, float dz, float maxDist, RayHit& hit);
bool rayBox(float ox, float oy, float oz, float dx, float dy, float dz, const AABB& b, float& tHit, int& face);

class Body;
// Entity boxes that bodies collide with (World.getCollidingBoundingBoxes); set by the game while it ticks.
using EntityBoxFn = int (*)(void* user, const Body* self, const AABB& area, AABB* out, int max);
extern EntityBoxFn g_entityBoxes;
extern void* g_entityBoxesUser;

// Entity physics shared by the player, mobs and dropped items (Entity.moveEntity, EntityLiving.moveEntityWithHeading).
class Body {
public:
    float width = 0.6f, height = 1.8f;
    float stepHeight = 0.5f;
    float x = 0, y = 0, z = 0;
    float prevX = 0, prevY = 0, prevZ = 0;
    float mx = 0, my = 0, mz = 0;
    float yaw = 0, pitch = 0;
    bool onGround = false, inWater = false, inLava = false, collidedH = false, collidedV = false;
    bool inWeb = false;
    bool touchedPortal = false;  // set by move(), cleared by whoever consumes it
    float fallDistance = 0;
    float landedFall = 0;  // fall distance at the last landing; consumed by the game for damage
    // Footsteps and splashes for the game to turn into sounds (only when makesSteps is set).
    bool makesSteps = false;
    float walked = 0;
    int nextStep = 1;
    bool stepPending = false;
    uint8_t stepBlock = 0;
    int stepX = 0, stepY = 0, stepZ = 0;
    float splash = 0;

    void setPosition(float px, float py, float pz);
    AABB box() const { return {x - width / 2, y, z - width / 2, x + width / 2, y + height, z + width / 2}; }
    void move(const World& w, float dx, float dy, float dz);
    void moveFlying(float s, float f, float speed);
    bool collides(const World& w, const AABB& b) const;
    void updateLiquids(const World& w);
    bool onLadder(const World& w) const;
    // One tick of walking physics with the given input (-1..1) and jump flag.
    void walkTick(const World& w, float forward, float strafe, bool jump, float speedScale = 1.0f);
};

class Player : public Body {
public:
    static constexpr float WIDTH = 0.6f;
    static constexpr float HEIGHT = 1.8f;
    static constexpr float EYE = 1.62f;

    Player() { makesSteps = true; }

    bool flying = false, autoJump = true;

    // View bobbing (EntityPlayer.cameraYaw / cameraPitch, Entity.distanceWalkedModified).
    float bob = 0, prevBob = 0, bobPitch = 0, prevBobPitch = 0, prevWalked = 0;

    // Input for the next tick.
    float forward = 0, strafe = 0;
    bool jump = false, descend = false;

    void tick(const World& w);
    void look(float& dx, float& dy, float& dz) const;
    bool eyeInWater(const World& w, float eyeY) const;
    bool headInBlock(const World& w) const;  // suffocation

private:
    void updateBob();
    bool autoJumpQueued_ = false;
};
