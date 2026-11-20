#include "particles.h"

#include <cmath>

#include "blocks.h"
#include "items.h"
#include "mathhelper.h"
#include "mesher.h"
#include "player.h"

namespace {

inline float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

// BlockFluid.getPercentAir
inline float fluidAir(int meta) {
    if (meta >= 8) meta = 0;
    return (meta + 1) / 9.0f;
}

// AxisAlignedBB.calculateXOffset and friends.
float clipX(const AABB& o, const AABB& b, float d) {
    if (b.y1 <= o.y0 || b.y0 >= o.y1 || b.z1 <= o.z0 || b.z0 >= o.z1) return d;
    if (d > 0 && b.x1 <= o.x0) {
        float m = o.x0 - b.x1;
        if (m < d) d = m;
    }
    if (d < 0 && b.x0 >= o.x1) {
        float m = o.x1 - b.x0;
        if (m > d) d = m;
    }
    return d;
}
float clipY(const AABB& o, const AABB& b, float d) {
    if (b.x1 <= o.x0 || b.x0 >= o.x1 || b.z1 <= o.z0 || b.z0 >= o.z1) return d;
    if (d > 0 && b.y1 <= o.y0) {
        float m = o.y0 - b.y1;
        if (m < d) d = m;
    }
    if (d < 0 && b.y0 >= o.y1) {
        float m = o.y1 - b.y0;
        if (m > d) d = m;
    }
    return d;
}
float clipZ(const AABB& o, const AABB& b, float d) {
    if (b.x1 <= o.x0 || b.x0 >= o.x1 || b.y1 <= o.y0 || b.y0 >= o.y1) return d;
    if (d > 0 && b.z1 <= o.z0) {
        float m = o.z0 - b.z1;
        if (m < d) d = m;
    }
    if (d < 0 && b.z0 >= o.z1) {
        float m = o.z1 - b.z0;
        if (m > d) d = m;
    }
    return d;
}

}  // namespace

float Particles::rnd() {
    seed_ ^= seed_ << 13;
    seed_ ^= seed_ >> 17;
    seed_ ^= seed_ << 5;
    return (seed_ >> 8) * (1.0f / 16777216.0f);
}

Particle* Particles::alloc() {
    if (count < MAX) return &p[count++];
    // EffectRenderer drops the oldest particle once a layer is full; overwrite them round robin.
    victim_ = (victim_ + 1) % MAX;
    return &p[victim_];
}

// EntityFX constructor.
void Particles::base(Particle& q, float x, float y, float z, float vx, float vy, float vz) {
    q.x = q.px = x, q.y = q.py = y, q.z = q.pz = z;
    q.half = 0.1f;
    q.r = q.g = q.b = 1.0f;
    q.mx = vx + (rnd() * 2.0f - 1.0f) * 0.4f;
    q.my = vy + (rnd() * 2.0f - 1.0f) * 0.4f;
    q.mz = vz + (rnd() * 2.0f - 1.0f) * 0.4f;
    float speed = (rnd() + rnd() + 1.0f) * 0.15f;
    float len = std::sqrt(q.mx * q.mx + q.my * q.my + q.mz * q.mz);
    if (len < 1e-6f) len = 1e-6f;
    q.mx = q.mx / len * speed * 0.4f;
    q.my = q.my / len * speed * 0.4f + 0.1f;
    q.mz = q.mz / len * speed * 0.4f;
    q.u = rnd() * 3.0f;
    q.v = rnd() * 3.0f;
    q.scale = q.baseScale = (rnd() * 0.5f + 0.5f) * 2.0f;
    q.maxAge = (int16_t)(4.0f / (rnd() * 0.9f + 0.1f));
    q.age = 0;
    q.gravity = 0;
    q.tex = 0;
    q.layer = LAYER_PARTICLES;
    q.onGround = false;
    q.noClip = false;
}

Particle* Particles::spawn(const World& w, ParticleKind k, float x, float y, float z, float vx, float vy, float vz,
                           float extra) {
    float dx = viewX - x, dy = viewY - y, dz = viewZ - z;
    if (dx * dx + dy * dy + dz * dz > 16.0f * 16.0f) return nullptr;
    if (k != PART_DIGGING && k != PART_ITEM) {
        if (level >= 2) return nullptr;
        if (level == 1 && rnd() < 0.5f) return nullptr;
    }
    (void)w;
    Particle& q = *alloc();
    q.kind = k;
    switch (k) {
        case PART_SMOKE:
            base(q, x, y, z, 0, 0, 0);
            q.mx = q.mx * 0.1f + vx, q.my = q.my * 0.1f + vy, q.mz = q.mz * 0.1f + vz;
            q.r = q.g = q.b = rnd() * 0.3f;
            q.scale *= 0.75f * extra;
            q.maxAge = (int16_t)((int)(8.0f / (rnd() * 0.8f + 0.2f)) * extra);
            break;
        case PART_FLAME:
            base(q, x, y, z, vx, vy, vz);
            q.mx = q.mx * 0.01f + vx, q.my = q.my * 0.01f + vy, q.mz = q.mz * 0.01f + vz;
            q.maxAge = (int16_t)((int)(8.0f / (rnd() * 0.8f + 0.2f)) + 4);
            q.noClip = true;
            q.tex = 48;
            break;
        case PART_EXPLODE:
            base(q, x, y, z, vx, vy, vz);
            q.mx = vx + (rnd() * 2.0f - 1.0f) * 0.05f;
            q.my = vy + (rnd() * 2.0f - 1.0f) * 0.05f;
            q.mz = vz + (rnd() * 2.0f - 1.0f) * 0.05f;
            q.r = q.g = q.b = rnd() * 0.3f + 0.7f;
            q.scale = rnd() * rnd() * 6.0f + 1.0f;
            q.maxAge = (int16_t)((int)(16.0f / (rnd() * 0.8f + 0.2f)) + 2);
            break;
        case PART_BUBBLE:
            base(q, x, y, z, vx, vy, vz);
            q.tex = 32;
            q.half = 0.01f;
            q.scale *= rnd() * 0.6f + 0.2f;
            q.mx = vx * 0.2f + (rnd() * 2.0f - 1.0f) * 0.02f;
            q.my = vy * 0.2f + (rnd() * 2.0f - 1.0f) * 0.02f;
            q.mz = vz * 0.2f + (rnd() * 2.0f - 1.0f) * 0.02f;
            q.maxAge = (int16_t)(8.0f / (rnd() * 0.8f + 0.2f));
            break;
        case PART_RAIN:
        case PART_SPLASH:
            base(q, x, y, z, 0, 0, 0);
            q.mx *= 0.3f;
            q.my = rnd() * 0.2f + 0.1f;
            q.mz *= 0.3f;
            q.tex = (uint16_t)(19 + (int)(rnd() * 4.0f));
            q.half = 0.005f;
            q.gravity = 0.06f;
            q.maxAge = (int16_t)(8.0f / (rnd() * 0.8f + 0.2f));
            if (k == PART_SPLASH) {
                q.gravity = 0.04f;
                ++q.tex;
                if (vy == 0 && (vx != 0 || vz != 0)) q.mx = vx, q.my = vy + 0.1f, q.mz = vz;
            }
            break;
        case PART_REDDUST: {
            base(q, x, y, z, 0, 0, 0);
            q.mx *= 0.1f, q.my *= 0.1f, q.mz *= 0.1f;
            float r = vx == 0 ? 1.0f : vx;
            float f = rnd() * 0.4f + 0.6f;
            q.r = (rnd() * 0.2f + 0.8f) * r * f;
            q.g = (rnd() * 0.2f + 0.8f) * vy * f;
            q.b = (rnd() * 0.2f + 0.8f) * vz * f;
            q.scale *= 0.75f * extra;
            q.maxAge = (int16_t)((int)(8.0f / (rnd() * 0.8f + 0.2f)) * extra);
            break;
        }
        case PART_PORTAL: {
            base(q, x, y, z, vx, vy, vz);
            q.mx = vx, q.my = vy, q.mz = vz;
            q.ox = x, q.oy = y, q.oz = z;
            float f = rnd() * 0.6f + 0.4f;
            q.scale = rnd() * 0.2f + 0.5f;
            q.r = f * 0.9f, q.g = f * 0.3f, q.b = f;
            q.maxAge = (int16_t)((int)(rnd() * 10.0f) + 40);
            q.noClip = true;
            q.tex = (uint16_t)(int)(rnd() * 8.0f);
            break;
        }
        case PART_NOTE: {
            base(q, x, y, z, 0, 0, 0);
            q.mx *= 0.01f, q.my *= 0.01f, q.mz *= 0.01f;
            q.my += 0.2f;
            const float TAU = 3.1415927f * 2.0f;
            q.r = mh::sin((vx + 0.0f) * TAU) * 0.65f + 0.35f;
            q.g = mh::sin((vx + 0.33333334f) * TAU) * 0.65f + 0.35f;
            q.b = mh::sin((vx + 0.6666667f) * TAU) * 0.65f + 0.35f;
            q.scale *= 0.75f * 2.0f;
            q.maxAge = 6;
            q.tex = 64;
            break;
        }
        case PART_HEART:
            base(q, x, y, z, 0, 0, 0);
            q.mx *= 0.01f, q.my *= 0.01f, q.mz *= 0.01f;
            q.my += 0.1f;
            q.scale *= 0.75f * 2.0f;
            q.maxAge = 16;
            q.tex = 80;
            break;
        case PART_LAVA:
            base(q, x, y, z, 0, 0, 0);
            q.mx *= 0.8f, q.mz *= 0.8f;
            q.my = rnd() * 0.4f + 0.05f;
            q.scale *= rnd() * 2.0f + 0.2f;
            q.maxAge = (int16_t)(16.0f / (rnd() * 0.8f + 0.2f));
            q.tex = 49;
            break;
        case PART_DIGGING:
        case PART_ITEM:
            base(q, x, y, z, vx, vy, vz);
            q.gravity = 1.0f;
            q.scale /= 2.0f;
            break;
    }
    q.baseScale = q.scale;
    return &q;
}

void Particles::blockDestroy(const World& w, int x, int y, int z, uint8_t id, int meta) {
    if (id == AIR) return;
    uint32_t tint = id == GRASS ? 0xFFFFFF : blockTint(&w, id, meta, 0, x, z);
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 4; ++k) {
                float px = x + (i + 0.5f) / 4.0f, py = y + (j + 0.5f) / 4.0f, pz = z + (k + 0.5f) / 4.0f;
                Particle* q = spawn(w, PART_DIGGING, px, py, pz, px - x - 0.5f, py - y - 0.5f, pz - z - 0.5f);
                if (!q) return;
                q->layer = LAYER_TERRAIN;
                q->tex = (uint16_t)blockTexture(id, meta, 0);
                q->r = 0.6f * ((tint >> 16) & 255) / 255.0f;
                q->g = 0.6f * ((tint >> 8) & 255) / 255.0f;
                q->b = 0.6f * (tint & 255) / 255.0f;
            }
}

void Particles::blockHit(const World& w, int x, int y, int z, int side) {
    uint8_t id = w.get(x, y, z);
    if (id == AIR) return;
    AABB b;
    if (!blockSelectionBox(w, x, y, z, id, b)) b = {0, 0, 0, 1, 1, 1};
    const float e = 0.1f;
    float px = x + rnd() * (b.x1 - b.x0 - e * 2) + e + b.x0;
    float py = y + rnd() * (b.y1 - b.y0 - e * 2) + e + b.y0;
    float pz = z + rnd() * (b.z1 - b.z0 - e * 2) + e + b.z0;
    if (side == 0) py = y + b.y0 - e;
    if (side == 1) py = y + b.y1 + e;
    if (side == 2) pz = z + b.z0 - e;
    if (side == 3) pz = z + b.z1 + e;
    if (side == 4) px = x + b.x0 - e;
    if (side == 5) px = x + b.x1 + e;
    int meta = w.getMeta(x, y, z);
    Particle* q = spawn(w, PART_DIGGING, px, py, pz, 0, 0, 0);
    if (!q) return;
    uint32_t tint = id == GRASS ? 0xFFFFFF : blockTint(&w, id, meta, 0, x, z);
    q->layer = LAYER_TERRAIN;
    q->tex = (uint16_t)blockTexture(id, meta, 0);
    q->r = 0.6f * ((tint >> 16) & 255) / 255.0f;
    q->g = 0.6f * ((tint >> 8) & 255) / 255.0f;
    q->b = 0.6f * (tint & 255) / 255.0f;
    // EntityFX.multiplyVelocity(0.2) and multipleParticleScaleBy(0.6)
    q->mx *= 0.2f;
    q->my = (q->my - 0.1f) * 0.2f + 0.1f;
    q->mz *= 0.2f;
    q->scale = q->baseScale = q->scale * 0.6f;
    q->half *= 0.6f;
}

void Particles::itemPiece(const World& w, float x, float y, float z, uint16_t icon) {
    Particle* q = spawn(w, PART_ITEM, x, y, z, 0, 0, 0);
    if (!q) return;
    q->layer = LAYER_ITEMS;
    q->tex = icon;
}

// Entity.moveEntity for the particle's box against block collision boxes only.
void Particles::move(const World& w, Particle& q, float dx, float dy, float dz) {
    if (q.noClip) {
        q.x += dx, q.y += dy, q.z += dz;
        return;
    }
    float h = q.half;
    AABB box = {q.x - h, q.y - h, q.z - h, q.x + h, q.y + h, q.z + h};
    AABB area = box.expand(dx, dy, dz);
    AABB boxes[32];
    int n = 0;
    int x0 = mh::floorf_(area.x0), x1 = mh::floorf_(area.x1 + 1.0f);
    int y0 = mh::floorf_(area.y0 - 1.0f), y1 = mh::floorf_(area.y1 + 1.0f);
    int z0 = mh::floorf_(area.z0), z1 = mh::floorf_(area.z1 + 1.0f);
    if (x1 - x0 > 3) x1 = x0 + 3;
    if (y1 - y0 > 3) y1 = y0 + 3;
    if (z1 - z0 > 3) z1 = z0 + 3;
    for (int x = x0; x < x1; ++x)
        for (int z = z0; z < z1; ++z)
            for (int y = y0; y < y1; ++y) {
                uint8_t id = w.get(x, y, z);
                if (id == AIR || n > 30) continue;
                AABB local[2];
                int k = blockCollisionBoxes(w, x, y, z, id, local);
                for (int i = 0; i < k; ++i) {
                    AABB b = local[i].offset((float)x, (float)y, (float)z);
                    if (b.intersects(area)) boxes[n++] = b;
                }
            }
    float odx = dx, ody = dy, odz = dz;
    for (int i = 0; i < n; ++i) dy = clipY(boxes[i], box, dy);
    box = box.offset(0, dy, 0);
    for (int i = 0; i < n; ++i) dx = clipX(boxes[i], box, dx);
    box = box.offset(dx, 0, 0);
    for (int i = 0; i < n; ++i) dz = clipZ(boxes[i], box, dz);
    box = box.offset(0, 0, dz);
    q.x = (box.x0 + box.x1) * 0.5f;
    q.y = box.y0 + h;
    q.z = (box.z0 + box.z1) * 0.5f;
    q.onGround = ody != dy && ody < 0;
    if (odx != dx) q.mx = 0;
    if (ody != dy) q.my = 0;
    if (odz != dz) q.mz = 0;
}

// Returns false once the particle is dead.
bool Particles::tickOne(const World& w, Particle& q) {
    q.px = q.x, q.py = q.y, q.pz = q.z;
    auto ground = [&] {
        if (q.onGround) q.mx *= 0.7f, q.mz *= 0.7f;
    };
    switch (q.kind) {
        case PART_SMOKE:
        case PART_EXPLODE:
        case PART_REDDUST: {
            if (q.age++ >= q.maxAge) return false;
            q.tex = (uint16_t)(7 - q.age * 8 / q.maxAge);
            if (q.kind != PART_REDDUST) q.my += 0.004f;
            move(w, q, q.mx, q.my, q.mz);
            float drag = q.kind == PART_EXPLODE ? 0.9f : 0.96f;
            if (q.kind != PART_EXPLODE && q.y == q.py) q.mx *= 1.1f, q.mz *= 1.1f;
            q.mx *= drag, q.my *= drag, q.mz *= drag;
            ground();
            return true;
        }
        case PART_FLAME:
            if (q.age++ >= q.maxAge) return false;
            move(w, q, q.mx, q.my, q.mz);
            q.mx *= 0.96f, q.my *= 0.96f, q.mz *= 0.96f;
            ground();
            return true;
        case PART_BUBBLE: {
            q.my += 0.002f;
            move(w, q, q.mx, q.my, q.mz);
            q.mx *= 0.85f, q.my *= 0.85f, q.mz *= 0.85f;
            if (!blockIsWater(w.get(mh::floorf_(q.x), mh::floorf_(q.y), mh::floorf_(q.z)))) return false;
            if (q.maxAge-- <= 0) return false;
            return true;
        }
        case PART_RAIN:
        case PART_SPLASH: {
            q.my -= q.gravity;
            move(w, q, q.mx, q.my, q.mz);
            q.mx *= 0.98f, q.my *= 0.98f, q.mz *= 0.98f;
            if (q.maxAge-- <= 0) return false;
            if (q.onGround) {
                if (rnd() < 0.5f) return false;
                q.mx *= 0.7f, q.mz *= 0.7f;
            }
            int bx = mh::floorf_(q.x), by = mh::floorf_(q.y), bz = mh::floorf_(q.z);
            uint8_t id = w.get(bx, by, bz);
            if (blockIsLiquid(id) || blockMatSolid(id)) {
                float top = by + 1 - fluidAir(w.getMeta(bx, by, bz));
                if (q.y < top) return false;
            }
            return true;
        }
        case PART_PORTAL: {
            float f = (float)q.age / q.maxAge, f2 = f;
            f = 1.0f - (-f + f * f * 2.0f);
            q.x = q.ox + q.mx * f;
            q.y = q.oy + q.my * f + (1.0f - f2);
            q.z = q.oz + q.mz * f;
            if (q.age++ >= q.maxAge) return false;
            return true;
        }
        case PART_NOTE:
        case PART_HEART: {
            if (q.age++ >= q.maxAge) return false;
            move(w, q, q.mx, q.my, q.mz);
            if (q.y == q.py) q.mx *= 1.1f, q.mz *= 1.1f;
            float drag = q.kind == PART_NOTE ? 0.66f : 0.86f;
            q.mx *= drag, q.my *= drag, q.mz *= drag;
            ground();
            return true;
        }
        case PART_LAVA: {
            if (q.age++ >= q.maxAge) return false;
            float f = (float)q.age / q.maxAge;
            if (rnd() > f && count < MAX) spawn(w, PART_SMOKE, q.x, q.y, q.z, q.mx, q.my, q.mz);
            q.my -= 0.03f;
            move(w, q, q.mx, q.my, q.mz);
            q.mx *= 0.999f, q.my *= 0.999f, q.mz *= 0.999f;
            ground();
            return true;
        }
        case PART_DIGGING:
        case PART_ITEM:
            if (q.age++ >= q.maxAge) return false;
            q.my -= 0.04f * q.gravity;
            move(w, q, q.mx, q.my, q.mz);
            q.mx *= 0.98f, q.my *= 0.98f, q.mz *= 0.98f;
            ground();
            return true;
    }
    return false;
}

void Particles::tick(const World& w) {
    for (int i = 0; i < count;) {
        // Lava spawns smoke while it ticks, but only into free slots, so `p[i]` stays valid.
        if (tickOne(w, p[i])) {
            ++i;
        } else {
            p[i] = p[--count];
        }
    }
}

float Particles::renderScale(const Particle& q, float partial) const {
    float f = (q.age + partial) / q.maxAge;
    switch (q.kind) {
        case PART_SMOKE:
        case PART_REDDUST:
        case PART_NOTE:
        case PART_HEART: return q.baseScale * clamp01(f * 32.0f);
        case PART_FLAME: return q.baseScale * (1.0f - f * f * 0.5f);
        case PART_LAVA: return q.baseScale * (1.0f - f * f);
        case PART_PORTAL: {
            float g = 1.0f - f;
            return q.baseScale * (1.0f - g * g);
        }
        default: return q.scale;
    }
}

float Particles::brightness(const Particle& q, float b, float partial) const {
    switch (q.kind) {
        case PART_FLAME: {
            float f = clamp01((q.age + partial) / q.maxAge);
            return b * f + (1.0f - f);
        }
        case PART_LAVA: return 1.0f;
        case PART_PORTAL: {
            float f = (float)q.age / q.maxAge;
            f *= f;
            f *= f;
            return b * (1.0f - f) + f;
        }
        default: return b;
    }
}
