#include "player.h"

#include "fluid.h"

#include <cmath>

EntityBoxFn g_entityBoxes = nullptr;
void* g_entityBoxesUser = nullptr;

namespace {

constexpr float DEG = 3.14159265f / 180.0f;
constexpr int MAX_BOXES = 256;

inline int ifloor(float v) {
    int i = (int)v;
    return v < (float)i ? i - 1 : i;
}

int gatherBoxes(const World& w, const AABB& area, AABB* out, const Body* self) {
    int n = 0;
    if (g_entityBoxes) n = g_entityBoxes(g_entityBoxesUser, self, area, out, 16);
    int x0 = ifloor(area.x0), x1 = ifloor(area.x1 + 1.0f);
    int y0 = ifloor(area.y0) - 1, y1 = ifloor(area.y1 + 1.0f);
    int z0 = ifloor(area.z0), z1 = ifloor(area.z1 + 1.0f);
    for (int x = x0; x < x1; ++x)
        for (int z = z0; z < z1; ++z)
            for (int y = y0; y < y1; ++y) {
                AABB b[2];
                int k;
                if (y < 0 || !w.isLoaded(x, z)) {
                    b[0] = {0, 0, 0, 1, 1, 1};
                    k = 1;
                } else {
                    if (y >= World::SY) continue;
                    k = blockCollisionBoxes(w, x, y, z, w.get(x, y, z), b);
                }
                for (int i = 0; i < k && n < MAX_BOXES; ++i) out[n++] = b[i].offset((float)x, (float)y, (float)z);
            }
    return n;
}

float clipX(const AABB& b, const AABB& e, float d) {
    if (e.y1 > b.y0 && e.y0 < b.y1 && e.z1 > b.z0 && e.z0 < b.z1) {
        if (d > 0 && e.x1 <= b.x0) {
            float m = b.x0 - e.x1;
            if (m < d) d = m;
        }
        if (d < 0 && e.x0 >= b.x1) {
            float m = b.x1 - e.x0;
            if (m > d) d = m;
        }
    }
    return d;
}

float clipY(const AABB& b, const AABB& e, float d) {
    if (e.x1 > b.x0 && e.x0 < b.x1 && e.z1 > b.z0 && e.z0 < b.z1) {
        if (d > 0 && e.y1 <= b.y0) {
            float m = b.y0 - e.y1;
            if (m < d) d = m;
        }
        if (d < 0 && e.y0 >= b.y1) {
            float m = b.y1 - e.y0;
            if (m > d) d = m;
        }
    }
    return d;
}

float clipZ(const AABB& b, const AABB& e, float d) {
    if (e.x1 > b.x0 && e.x0 < b.x1 && e.y1 > b.y0 && e.y0 < b.y1) {
        if (d > 0 && e.z1 <= b.z0) {
            float m = b.z0 - e.z1;
            if (m < d) d = m;
        }
        if (d < 0 && e.z0 >= b.z1) {
            float m = b.z1 - e.z0;
            if (m > d) d = m;
        }
    }
    return d;
}

bool anyLiquid(const World& w, const AABB& b, bool water) {
    int x0 = ifloor(b.x0), x1 = ifloor(b.x1 + 1.0f);
    int y0 = ifloor(b.y0), y1 = ifloor(b.y1 + 1.0f);
    int z0 = ifloor(b.z0), z1 = ifloor(b.z1 + 1.0f);
    for (int x = x0; x < x1; ++x)
        for (int y = y0; y < y1; ++y)
            for (int z = z0; z < z1; ++z) {
                uint8_t id = w.get(x, y, z);
                if (water ? blockIsWater(id) : blockIsLava(id)) {
                    float top = (float)(y + 1) - 1.0f / 9.0f;
                    if (b.y0 < top) return true;
                }
            }
    return false;
}

bool doorBox(const World& w, int x, int y, int z, AABB& out) {
    int m = w.getMeta(x, y, z);
    if (m & 8) m = w.getMeta(x, y - 1, z);
    const float t = 0.1875f;
    switch (doorState(m & 7)) {
        case 0: out = {0, 0, 0, 1, 1, t}; break;
        case 1: out = {1 - t, 0, 0, 1, 1, 1}; break;
        case 2: out = {0, 0, 1 - t, 1, 1, 1}; break;
        default: out = {0, 0, 0, t, 1, 1}; break;
    }
    return true;
}

bool ladderBox(int meta, AABB& out) {
    const float t = 0.125f;
    switch (meta) {
        case 2: out = {0, 0, 1 - t, 1, 1, 1}; break;
        case 3: out = {0, 0, 0, 1, 1, t}; break;
        case 4: out = {1 - t, 0, 0, 1, 1, 1}; break;
        case 5: out = {0, 0, 0, t, 1, 1}; break;
        default: out = {0, 0, 0, 1, 1, 1}; break;
    }
    return true;
}

}  // namespace

bool rayBox(float ox, float oy, float oz, float dx, float dy, float dz, const AABB& b, float& tHit, int& face) {
    float tmin = -1e30f, tmax = 1e30f;
    int f = -1;
    const float o[3] = {ox, oy, oz}, d[3] = {dx, dy, dz};
    const float lo[3] = {b.x0, b.y0, b.z0}, hi[3] = {b.x1, b.y1, b.z1};
    static const int negFace[3] = {4, 0, 2}, posFace[3] = {5, 1, 3};
    for (int a = 0; a < 3; ++a) {
        if (std::fabs(d[a]) < 1e-9f) {
            if (o[a] < lo[a] || o[a] > hi[a]) return false;
            continue;
        }
        float t1 = (lo[a] - o[a]) / d[a], t2 = (hi[a] - o[a]) / d[a];
        int f1 = negFace[a], f2 = posFace[a];
        if (t1 > t2) {
            float t = t1;
            t1 = t2;
            t2 = t;
            int ft = f1;
            f1 = f2;
            f2 = ft;
        }
        if (t1 > tmin) {
            tmin = t1;
            f = f1;
        }
        if (t2 < tmax) tmax = t2;
        if (tmin > tmax) return false;
    }
    if (tmax < 0) return false;
    tHit = tmin;
    face = f;
    return true;
}

bool blockShapeBox(uint8_t id, int meta, AABB& out) {
    const BlockDef& d = g_blocks[id];
    if (id == AIR || (d.flags & BF_LIQUID)) return false;
    switch (d.render) {
        case RENDER_SLAB: out = {0, 0, 0, 1, 0.5f, 1}; return true;
        case RENDER_SNOW: out = {0, 0, 0, 1, 0.125f, 1}; return true;
        case RENDER_CACTUS: out = {1 / 16.0f, 0, 1 / 16.0f, 15 / 16.0f, 1, 15 / 16.0f}; return true;
        case RENDER_FARMLAND: out = {0, 0, 0, 1, 15 / 16.0f, 1}; return true;
        case RENDER_BED: out = {0, 0, 0, 1, 0.5625f, 1}; return true;
        case RENDER_FIRE: return false;
        case RENDER_CROPS: out = {0, 0, 0, 1, 0.25f, 1}; return true;
        case RENDER_LADDER: return ladderBox(meta, out);
        case RENDER_TORCH:
            switch (meta) {
                case 1: out = {0, 0.2f, 0.35f, 0.3f, 0.8f, 0.65f}; break;
                case 2: out = {0.7f, 0.2f, 0.35f, 1, 0.8f, 0.65f}; break;
                case 3: out = {0.35f, 0.2f, 0, 0.65f, 0.8f, 0.3f}; break;
                case 4: out = {0.35f, 0.2f, 0.7f, 0.65f, 0.8f, 1}; break;
                default: out = {0.4f, 0, 0.4f, 0.6f, 0.6f, 0.6f}; break;
            }
            return true;
        case RENDER_CAKE: out = {(1 + 2 * meta) / 16.0f, 0, 1 / 16.0f, 15 / 16.0f, 0.5f, 15 / 16.0f}; return true;
        case RENDER_TRAPDOOR: {
            const float t = 0.1875f;
            if (!(meta & 4)) out = {0, 0, 0, 1, t, 1};
            else if ((meta & 3) == 0) out = {0, 0, 1 - t, 1, 1, 1};
            else if ((meta & 3) == 1) out = {0, 0, 0, 1, 1, t};
            else if ((meta & 3) == 2) out = {1 - t, 0, 0, 1, 1, 1};
            else out = {0, 0, 0, t, 1, 1};
            return true;
        }
        case RENDER_SIGN:
            if (id == SIGN_POST) out = {0.25f, 0, 0.25f, 0.75f, 1, 0.75f};
            else if (meta == 3) out = {0, 0.28125f, 0, 1, 0.78125f, 0.125f};
            else if (meta == 4) out = {0.875f, 0.28125f, 0, 1, 0.78125f, 1};
            else if (meta == 5) out = {0, 0.28125f, 0, 0.125f, 0.78125f, 1};
            else out = {0, 0.28125f, 0.875f, 1, 0.78125f, 1};
            return true;
        case RENDER_CROSS:
            if (id == WEB) out = {0, 0, 0, 1, 1, 1};
            else if (id == REED) out = {0.125f, 0, 0.125f, 0.875f, 1, 0.875f};
            else if (id == TALL_GRASS) out = {0.1f, 0, 0.1f, 0.9f, 0.8f, 0.9f};
            else out = {0.3f, 0, 0.3f, 0.7f, 0.6f, 0.7f};
            return true;
        case RENDER_NONE: return false;
        case RENDER_WIRE: out = {0, 0, 0, 1, 1 / 16.0f, 1}; return true;
        case RENDER_REPEATER: out = {0, 0, 0, 1, 0.125f, 1}; return true;
        case RENDER_RAIL: out = {0, 0, 0, 1, meta >= 2 && meta <= 5 ? 0.625f : 0.125f, 1}; return true;
        case RENDER_LEVER:
            switch (meta & 7) {
                case 1: out = {0, 0.2f, 0.3125f, 0.375f, 0.8f, 0.6875f}; break;
                case 2: out = {0.625f, 0.2f, 0.3125f, 1, 0.8f, 0.6875f}; break;
                case 3: out = {0.3125f, 0.2f, 0, 0.6875f, 0.8f, 0.375f}; break;
                case 4: out = {0.3125f, 0.2f, 0.625f, 0.6875f, 0.8f, 1}; break;
                default: out = {0.25f, 0, 0.25f, 0.75f, 0.6f, 0.75f}; break;
            }
            return true;
        case RENDER_SHAPE:
            if (id == STONE_BUTTON) {
                const float d = (meta & 8) ? 0.0625f : 0.125f, h = 0.1875f, y0 = 0.375f, y1 = 0.625f;
                switch (meta & 7) {
                    case 1: out = {0, y0, 0.5f - h, d, y1, 0.5f + h}; break;
                    case 2: out = {1 - d, y0, 0.5f - h, 1, y1, 0.5f + h}; break;
                    case 3: out = {0.5f - h, y0, 0, 0.5f + h, y1, d}; break;
                    case 4: out = {0.5f - h, y0, 1 - d, 0.5f + h, y1, 1}; break;
                    default: out = {0.3125f, 0.375f, 0.375f, 0.6875f, 0.625f, 0.625f}; break;
                }
            } else {
                const float e = 1 / 16.0f;
                out = {e, 0, e, 1 - e, meta ? 1 / 32.0f : e, 1 - e};
            }
            return true;
        case RENDER_PISTON:
            out = {0, 0, 0, 1, 1, 1};
            if (meta & 8) {
                switch (meta & 7) {
                    case 0: out.y0 = 0.25f; break;
                    case 1: out.y1 = 0.75f; break;
                    case 2: out.z0 = 0.25f; break;
                    case 3: out.z1 = 0.75f; break;
                    case 4: out.x0 = 0.25f; break;
                    case 5: out.x1 = 0.75f; break;
                }
            }
            return true;
        case RENDER_PISTON_HEAD:
            out = {0, 0, 0, 1, 1, 1};
            switch (meta & 7) {
                case 0: out.y1 = 0.25f; break;
                case 1: out.y0 = 0.75f; break;
                case 2: out.z1 = 0.25f; break;
                case 3: out.z0 = 0.75f; break;
                case 4: out.x1 = 0.25f; break;
                case 5: out.x0 = 0.75f; break;
            }
            return true;
        default: out = {0, 0, 0, 1, 1, 1}; return true;
    }
}

// BlockPistonExtension.getCollidingBoundingBoxes: the plate and the arm.
static int pistonHeadBoxes(int meta, AABB* out) {
    static const AABB ARM[6] = {
        {0.375f, 0.25f, 0.375f, 0.625f, 1, 0.625f}, {0.375f, 0, 0.375f, 0.625f, 0.75f, 0.625f},
        {0.25f, 0.375f, 0.25f, 0.75f, 0.625f, 1},   {0.25f, 0.375f, 0, 0.75f, 0.625f, 0.75f},
        {0.375f, 0.25f, 0.25f, 0.625f, 0.75f, 1},   {0, 0.375f, 0.25f, 0.75f, 0.625f, 0.75f}};
    int dir = meta & 7;
    if (dir > 5) dir = 0;
    blockShapeBox(PISTON_HEAD, meta, out[0]);
    out[1] = ARM[dir];
    return 2;
}

int pistonMovingBoxes(const World& w, int x, int y, int z, AABB* out) {
    const TileEntity* t = w.tileAt(x, y, z);
    if (!t || t->type != TILE_PISTON) return 0;
    uint8_t id = (uint8_t)t->burnTotal;
    if (id == AIR || id == PISTON_MOVING || !blockIsSolid(id)) return 0;
    int meta = t->cookTime & 15, n;
    if (id == PISTON_HEAD) {
        n = pistonHeadBoxes(meta, out);
    } else if (g_blocks[id].render == RENDER_REPEATER) {
        n = blockShapeBox(id, meta, out[0]) ? 1 : 0;
    } else {
        out[0] = {0, 0, 0, 1, 1, 1};
        n = 1;
    }
    static const int DX[6] = {0, 0, 0, 0, -1, 1}, DY[6] = {-1, 1, 0, 0, 0, 0}, DZ[6] = {0, 0, -1, 1, 0, 0};
    int dir = pistonFacing(*t);
    float p = pistonLastSteps(*t) * 0.5f, f = pistonExtending(*t) ? p - 1 : 1 - p;
    for (int i = 0; i < n; ++i) out[i] = out[i].offset(DX[dir] * f, DY[dir] * f, DZ[dir] * f);
    return n;
}

bool blockSelectionBox(const World& w, int x, int y, int z, uint8_t id, AABB& out) {
    if (blockIsDoor(id)) return doorBox(w, x, y, z, out);
    if (id == PORTAL) {
        if (w.get(x - 1, y, z) != PORTAL && w.get(x + 1, y, z) != PORTAL) out = {0.375f, 0, 0, 0.625f, 1, 1};
        else out = {0, 0, 0.375f, 1, 1, 0.625f};
        return true;
    }
    return blockShapeBox(id, w.getMeta(x, y, z), out);
}

int blockCollisionBoxes(const World& w, int x, int y, int z, uint8_t id, AABB* out) {
    if (!blockIsSolid(id)) return 0;
    if (id == SOUL_SAND) {
        out[0] = {0, 0, 0, 1, 0.875f, 1};
        return 1;
    }
    switch (g_blocks[id].render) {
        case RENDER_SLAB: out[0] = {0, 0, 0, 1, 0.5f, 1}; return 1;
        case RENDER_BED: out[0] = {0, 0, 0, 1, 0.5625f, 1}; return 1;
        case RENDER_CACTUS: out[0] = {1 / 16.0f, 0, 1 / 16.0f, 15 / 16.0f, 15 / 16.0f, 15 / 16.0f}; return 1;
        case RENDER_FENCE: out[0] = {0, 0, 0, 1, 1.5f, 1}; return 1;
        case RENDER_TRAPDOOR: return blockShapeBox(id, w.getMeta(x, y, z), out[0]) ? 1 : 0;
        case RENDER_CAKE: {
            blockShapeBox(id, w.getMeta(x, y, z), out[0]);
            out[0].y1 = 0.4375f;
            return 1;
        }
        case RENDER_LADDER: return ladderBox(w.getMeta(x, y, z), out[0]) ? 1 : 0;
        case RENDER_DOOR: return doorBox(w, x, y, z, out[0]) ? 1 : 0;
        case RENDER_REPEATER: return blockShapeBox(id, w.getMeta(x, y, z), out[0]) ? 1 : 0;
        case RENDER_PISTON_HEAD: return pistonHeadBoxes(w.getMeta(x, y, z), out);
        case RENDER_NONE: return id == PISTON_MOVING ? pistonMovingBoxes(w, x, y, z, out) : 0;
        case RENDER_STAIRS: {
            out[0] = {0, 0, 0, 1, 0.5f, 1};
            switch (w.getMeta(x, y, z) & 3) {
                case 0: out[1] = {0.5f, 0.5f, 0, 1, 1, 1}; break;
                case 1: out[1] = {0, 0.5f, 0, 0.5f, 1, 1}; break;
                case 2: out[1] = {0, 0.5f, 0.5f, 1, 1, 1}; break;
                default: out[1] = {0, 0.5f, 0, 1, 1, 0.5f}; break;
            }
            return 2;
        }
        default: out[0] = {0, 0, 0, 1, 1, 1}; return 1;
    }
}

bool raycast(const World& w, float ox, float oy, float oz, float dx, float dy, float dz, float maxDist, RayHit& hit) {
    int x = ifloor(ox), y = ifloor(oy), z = ifloor(oz);
    int sx = dx > 0 ? 1 : -1, sy = dy > 0 ? 1 : -1, sz = dz > 0 ? 1 : -1;
    auto boundary = [](float o, float d, int c) -> float {
        if (std::fabs(d) < 1e-9f) return 1e30f;
        float edge = d > 0 ? (float)(c + 1) : (float)c;
        return (edge - o) / d;
    };
    float tx = boundary(ox, dx, x), ty = boundary(oy, dy, y), tz = boundary(oz, dz, z);
    float ddx = std::fabs(dx) < 1e-9f ? 1e30f : std::fabs(1.0f / dx);
    float ddy = std::fabs(dy) < 1e-9f ? 1e30f : std::fabs(1.0f / dy);
    float ddz = std::fabs(dz) < 1e-9f ? 1e30f : std::fabs(1.0f / dz);
    for (int i = 0; i < 64; ++i) {
        if ((unsigned)y < (unsigned)World::SY && w.isLoaded(x, z)) {
            uint8_t id = w.get(x, y, z);
            AABB b;
            if (blockSelectionBox(w, x, y, z, id, b)) {
                float t;
                int face;
                if (rayBox(ox, oy, oz, dx, dy, dz, b.offset((float)x, (float)y, (float)z), t, face) && t <= maxDist &&
                    face >= 0) {
                    hit = {x, y, z, face, t};
                    return true;
                }
            }
        }
        float t;
        if (tx < ty && tx < tz) {
            t = tx;
            x += sx;
            tx += ddx;
        } else if (ty < tz) {
            t = ty;
            y += sy;
            ty += ddy;
        } else {
            t = tz;
            z += sz;
            tz += ddz;
        }
        if (t > maxDist) break;
    }
    return false;
}

// ------------------------------------------------------------------ Body

void Body::setPosition(float px, float py, float pz) {
    x = prevX = px;
    y = prevY = py;
    z = prevZ = pz;
    mx = my = mz = 0;
    fallDistance = 0;
}

bool Body::collides(const World& w, const AABB& b) const {
    AABB boxes[MAX_BOXES];
    int n = gatherBoxes(w, b, boxes, this);
    for (int i = 0; i < n; ++i)
        if (boxes[i].intersects(b)) return true;
    return false;
}

void Body::moveFlying(float s, float f, float speed) {
    float d = std::sqrt(s * s + f * f);
    if (d < 0.01f) return;
    if (d < 1.0f) d = 1.0f;
    d = speed / d;
    s *= d;
    f *= d;
    float sn = std::sin(yaw * DEG), cs = std::cos(yaw * DEG);
    mx += s * cs - f * sn;
    mz += f * cs + s * sn;
}

void Body::move(const World& w, float dx, float dy, float dz) {
    if (inWeb) {
        inWeb = false;
        dx *= 0.25f, dy *= 0.05f, dz *= 0.25f;
        mx = my = mz = 0;
    }
    float odx = dx, ody = dy, odz = dz;
    AABB start = box();
    AABB b = start;
    static AABB boxes[MAX_BOXES];
    int n = gatherBoxes(w, b.expand(dx, dy, dz), boxes, this);
    for (int i = 0; i < n; ++i) dy = clipY(boxes[i], b, dy);
    b = b.offset(0, dy, 0);
    bool grounded = onGround || (ody != dy && ody < 0);
    for (int i = 0; i < n; ++i) dx = clipX(boxes[i], b, dx);
    b = b.offset(dx, 0, 0);
    for (int i = 0; i < n; ++i) dz = clipZ(boxes[i], b, dz);
    b = b.offset(0, 0, dz);

    if (stepHeight > 0 && grounded && (odx != dx || odz != dz)) {
        float sdx = odx, sdy = stepHeight, sdz = odz;
        AABB s = start;
        int m = gatherBoxes(w, s.expand(odx, stepHeight, odz), boxes, this);
        for (int i = 0; i < m; ++i) sdy = clipY(boxes[i], s, sdy);
        s = s.offset(0, sdy, 0);
        for (int i = 0; i < m; ++i) sdx = clipX(boxes[i], s, sdx);
        s = s.offset(sdx, 0, 0);
        for (int i = 0; i < m; ++i) sdz = clipZ(boxes[i], s, sdz);
        s = s.offset(0, 0, sdz);
        float down = -sdy;
        for (int i = 0; i < m; ++i) down = clipY(boxes[i], s, down);
        s = s.offset(0, down, 0);
        if (sdx * sdx + sdz * sdz > dx * dx + dz * dz) {
            dx = sdx;
            dz = sdz;
            dy = sdy + down;
            b = s;
        }
    }

    collidedH = odx != dx || odz != dz;
    collidedV = ody != dy;
    onGround = ody != dy && ody < 0;
    if (onGround) {
        if (fallDistance > 0) landedFall = fallDistance;
        fallDistance = 0;
    } else if (dy < 0) {
        fallDistance -= dy;
    }
    if (odx != dx) mx = 0;
    if (ody != dy) my = 0;
    if (odz != dz) mz = 0;
    float ox = x, oz = z;
    x = (b.x0 + b.x1) * 0.5f;
    y = b.y0;
    z = (b.z0 + b.z1) * 0.5f;
    // Entity.doBlockCollisions: onEntityCollidedWithBlock of webs, soul sand and portals
    for (int bx = ifloor(b.x0 + 0.001f); bx <= ifloor(b.x1 - 0.001f); ++bx)
        for (int by = ifloor(b.y0 + 0.001f); by <= ifloor(b.y1 - 0.001f); ++by)
            for (int bz = ifloor(b.z0 + 0.001f); bz <= ifloor(b.z1 - 0.001f); ++bz) {
                uint8_t id = w.get(bx, by, bz);
                if (id == WEB) inWeb = true;
                else if (id == SOUL_SAND) mx *= 0.4f, mz *= 0.4f;
                else if (id == PORTAL) touchedPortal = true;
            }
    if (makesSteps) {
        float wx = x - ox, wz = z - oz;
        walked += std::sqrt(wx * wx + wz * wz) * 0.6f;
        int bx = ifloor(x), by = ifloor(y - 0.2f), bz = ifloor(z);
        uint8_t id = w.get(bx, by, bz);
        if (walked > (float)nextStep && id != AIR) {
            nextStep = (int)walked + 1;
            stepPending = true;
            stepBlock = id;
            stepX = bx, stepY = by, stepZ = bz;
        }
    }
}

void Body::updateLiquids(const World& w) {
    AABB wb = box();
    wb.y0 += 0.4f;
    wb.y1 -= 0.4f;
    wb.x0 += 0.001f;
    wb.x1 -= 0.001f;
    wb.z0 += 0.001f;
    wb.z1 -= 0.001f;
    bool wasInWater = inWater;
    inWater = anyLiquid(w, wb, true);
    inLava = anyLiquid(w, wb, false);
    if (inWater && !wasInWater && makesSteps) {
        float f = std::sqrt(mx * mx * 0.2f + my * my + mz * mz * 0.2f) * 0.2f;
        splash = f > 1.0f ? 1.0f : f;
    }
    if (inWater) {
        fallDistance = 0;
        // World.handleMaterialAcceleration: flowing water pushes at 0.014 per tick.
        float sx = 0, sy = 0, sz = 0;
        for (int bx = ifloor(wb.x0); bx <= ifloor(wb.x1); ++bx)
            for (int by = ifloor(wb.y0); by <= ifloor(wb.y1); ++by)
                for (int bz = ifloor(wb.z0); bz <= ifloor(wb.z1); ++bz) {
                    if (!blockIsWater(w.get(bx, by, bz))) continue;
                    if (wb.y1 < by + 1 - fluidHeightPercent(w.getMeta(bx, by, bz))) continue;
                    float fx, fy, fz;
                    fluidFlow(w, bx, by, bz, true, fx, fy, fz);
                    sx += fx, sy += fy, sz += fz;
                }
        float l = std::sqrt(sx * sx + sy * sy + sz * sz);
        if (l > 1e-4f) {
            mx += sx / l * 0.014f;
            my += sy / l * 0.014f;
            mz += sz / l * 0.014f;
        }
    }
}

bool Body::onLadder(const World& w) const { return w.get(ifloor(x), ifloor(y), ifloor(z)) == LADDER; }

void Body::walkTick(const World& w, float forward, float strafe, bool jump, float speedScale) {
    if (jump) {
        if (inWater || inLava)
            my += 0.04f;
        else if (onGround)
            my = 0.42f;
    }
    float s = strafe * 0.98f, f = forward * 0.98f;
    if (inWater || inLava) {
        float oldY = y;
        moveFlying(s, f, 0.02f);
        move(w, mx, my, mz);
        float drag = inWater ? 0.8f : 0.5f;
        mx *= drag;
        my *= drag;
        mz *= drag;
        my -= 0.02f;
        if (collidedH && !collides(w, box().offset(mx, my + 0.6f - y + oldY, mz))) my = 0.3f;
        return;
    }
    float fr = 0.91f;
    if (onGround) {
        uint8_t below = w.get(ifloor(x), ifloor(y - 0.01f), ifloor(z));
        fr = (below == ICE ? 0.98f : 0.6f) * 0.91f;
    }
    float accel = onGround ? 0.1f * speedScale * (0.16277136f / (fr * fr * fr)) : 0.02f * speedScale;
    moveFlying(s, f, accel);
    bool ladder = onLadder(w);
    if (ladder) {
        const float lim = 0.15f;
        if (mx < -lim) mx = -lim;
        if (mx > lim) mx = lim;
        if (mz < -lim) mz = -lim;
        if (mz > lim) mz = lim;
        fallDistance = 0;
        if (my < -0.15f) my = -0.15f;
    }
    move(w, mx, my, mz);
    if (collidedH && ladder) my = 0.2f;
    my -= 0.08f;
    my *= 0.98f;
    mx *= fr;
    mz *= fr;
}

// ------------------------------------------------------------------ Player

void Player::look(float& dx, float& dy, float& dz) const {
    float cp = std::cos(pitch * DEG);
    dx = -std::sin(yaw * DEG) * cp;
    dy = -std::sin(pitch * DEG);
    dz = std::cos(yaw * DEG) * cp;
}

bool Player::eyeInWater(const World& w, float eyeY) const {
    int bx = ifloor(x), by = ifloor(eyeY), bz = ifloor(z);
    if (!blockIsWater(w.get(bx, by, bz))) return false;
    float top = blockIsWater(w.get(bx, by + 1, bz)) ? (float)(by + 1) : (float)(by + 1) - 1.0f / 9.0f;
    return eyeY < top;
}

bool Player::headInBlock(const World& w) const {
    for (int i = 0; i < 8; ++i) {
        float px = x + ((i & 1) - 0.5f) * width * 0.8f;
        float py = y + EYE + (((i >> 1) & 1) - 0.5f) * 0.1f;
        float pz = z + (((i >> 2) & 1) - 0.5f) * width * 0.8f;
        uint8_t id = w.get(ifloor(px), ifloor(py), ifloor(pz));
        if (blockIsNormalCube(id) && w.isLoaded(ifloor(px), ifloor(pz))) return true;
    }
    return false;
}

void Player::updateBob() {
    float f = std::sqrt(mx * mx + mz * mz);
    float f1 = std::atan(-my * 0.2f) * 15.0f;
    if (f > 0.1f) f = 0.1f;
    if (!onGround) f = 0;
    if (onGround) f1 = 0;
    bob += (f - bob) * 0.4f;
    bobPitch += (f1 - bobPitch) * 0.8f;
}

void Player::tick(const World& w) {
    prevX = x;
    prevY = y;
    prevZ = z;
    prevWalked = walked;
    prevBob = bob;
    prevBobPitch = bobPitch;
    updateLiquids(w);

    if (flying) {
        float s = strafe, f = forward;
        float d = std::sqrt(s * s + f * f);
        if (d > 1.0f) {
            s /= d;
            f /= d;
        }
        float sn = std::sin(yaw * DEG), cs = std::cos(yaw * DEG);
        float tx = (s * cs - f * sn) * 0.55f;
        float tz = (f * cs + s * sn) * 0.55f;
        float ty = (jump ? 0.4f : 0.0f) - (descend ? 0.4f : 0.0f);
        mx += (tx - mx) * 0.4f;
        mz += (tz - mz) * 0.4f;
        my += (ty - my) * 0.5f;
        move(w, mx, my, mz);
        fallDistance = 0;
        landedFall = 0;
        updateBob();
        return;
    }

    bool wantJump = jump || autoJumpQueued_;
    autoJumpQueued_ = false;
    walkTick(w, forward, strafe, wantJump);

    float s = strafe * 0.98f, f = forward * 0.98f;
    if (autoJump && collidedH && onGround && !inWater && (std::fabs(f) + std::fabs(s)) > 0.1f) {
        float sn = std::sin(yaw * DEG), cs = std::cos(yaw * DEG);
        float dirX = s * cs - f * sn, dirZ = f * cs + s * sn;
        float len = std::sqrt(dirX * dirX + dirZ * dirZ);
        if (len > 0.001f) {
            dirX = dirX / len * 0.3f;
            dirZ = dirZ / len * 0.3f;
            AABB ahead = box().offset(dirX, 0.0f, dirZ);
            AABB lifted = box().offset(dirX, 1.1f, dirZ);
            if (collides(w, ahead) && !collides(w, lifted) && !collides(w, box().offset(0, 1.1f, 0)))
                autoJumpQueued_ = true;
        }
    }
    updateBob();
}
