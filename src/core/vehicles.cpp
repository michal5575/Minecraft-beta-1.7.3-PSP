// Rails and vehicles from Beta 1.7.3: BlockRail, RailLogic, BlockDetectorRail, EntityMinecart, EntityBoat,
// ItemMinecart, ItemBoat, ItemSaddle, EntityPig.interact and Entity.mountEntity / updateRidden / updateRiderPosition.
#include <cmath>
#include <initializer_list>

#include "game.h"

namespace {

constexpr float PI = 3.14159265f;
constexpr float CART_Y = 0.35f;  // EntityMinecart.yOffset: posY is the box bottom plus this
constexpr float BOAT_Y = 0.3f;

inline int ifloor(float v) {
    int i = (int)v;
    return v < (float)i ? i - 1 : i;
}

// EntityMinecart.field_855_j: the two ends of each rail shape.
const int RAIL_ENDS[10][2][3] = {{{0, 0, -1}, {0, 0, 1}},  {{-1, 0, 0}, {1, 0, 0}},  {{-1, -1, 0}, {1, 0, 0}},
                                 {{-1, 0, 0}, {1, -1, 0}}, {{0, 0, -1}, {0, -1, 1}}, {{0, -1, -1}, {0, 0, 1}},
                                 {{0, 0, 1}, {1, 0, 0}},   {{0, 0, 1}, {-1, 0, 0}},  {{0, 0, -1}, {-1, 0, 0}},
                                 {{0, 0, -1}, {1, 0, 0}}};

inline bool railAt(const World& w, int x, int y, int z) { return blockIsRail(w.get(x, y, z)); }

int railShape(uint8_t id, int meta) {
    if (id != RAIL) meta &= 7;
    return meta <= 9 ? meta : 0;
}

// RailLogic: a rail and the (up to three) neighbors its shape points at.
struct RailLogic {
    Game* g;
    int x, y, z;
    bool straightOnly;  // powered and detector rails have no curves
    int n = 0;
    int c[4][3];

    RailLogic() = default;
    RailLogic(Game* game, int ax, int ay, int az) : g(game), x(ax), y(ay), z(az) {
        int meta = g->world.getMeta(x, y, z);
        straightOnly = g->world.get(x, y, z) != RAIL;
        if (straightOnly) meta &= ~8;
        setShape(meta);
    }

    void add(int ax, int ay, int az) {
        if (n == 4) return;
        c[n][0] = ax, c[n][1] = ay, c[n][2] = az;
        ++n;
    }

    void setShape(int m) {
        n = 0;
        switch (m) {
            case 0: add(x, y, z - 1), add(x, y, z + 1); break;
            case 1: add(x - 1, y, z), add(x + 1, y, z); break;
            case 2: add(x - 1, y, z), add(x + 1, y + 1, z); break;
            case 3: add(x - 1, y + 1, z), add(x + 1, y, z); break;
            case 4: add(x, y + 1, z - 1), add(x, y, z + 1); break;
            case 5: add(x, y, z - 1), add(x, y + 1, z + 1); break;
            case 6: add(x + 1, y, z), add(x, y, z + 1); break;
            case 7: add(x - 1, y, z), add(x, y, z + 1); break;
            case 8: add(x - 1, y, z), add(x, y, z - 1); break;
            case 9: add(x + 1, y, z), add(x, y, z - 1); break;
            default: break;
        }
    }

    bool rail(int ax, int ay, int az) const { return railAt(g->world, ax, ay, az); }

    // func_795_a: the rail at a connection, also one block up or down.
    bool logicAt(int ax, int ay, int az, RailLogic& out) const {
        for (int dy : {0, 1, -1})
            if (rail(ax, ay + dy, az)) {
                out = RailLogic(g, ax, ay + dy, az);
                return true;
            }
        return false;
    }

    // func_793_b
    bool connectsTo(const RailLogic& o) const {
        for (int i = 0; i < n; ++i)
            if (c[i][0] == o.x && c[i][2] == o.z) return true;
        return false;
    }

    // func_794_b
    bool hasConnection(int ax, int az) const {
        for (int i = 0; i < n; ++i)
            if (c[i][0] == ax && c[i][2] == az) return true;
        return false;
    }

    // func_785_b: keeps the connections whose rails point back here.
    void refreshConnected() {
        for (int i = 0; i < n; ++i) {
            RailLogic o;
            if (logicAt(c[i][0], c[i][1], c[i][2], o) && o.connectsTo(*this)) {
                c[i][0] = o.x, c[i][1] = o.y, c[i][2] = o.z;
            } else {
                for (int j = i; j < n - 1; ++j) c[j][0] = c[j + 1][0], c[j][1] = c[j + 1][1], c[j][2] = c[j + 1][2];
                --n;
                --i;
            }
        }
    }

    // func_784_a / func_790_c
    bool railNear(int ax, int ay, int az) const { return rail(ax, ay, az) || rail(ax, ay + 1, az) || rail(ax, ay - 1, az); }
    int adjacentRails() const {
        return (railNear(x, y, z - 1) ? 1 : 0) + (railNear(x, y, z + 1) ? 1 : 0) + (railNear(x - 1, y, z) ? 1 : 0) +
               (railNear(x + 1, y, z) ? 1 : 0);
    }

    // func_787_c
    bool canConnectTo(const RailLogic& o) const { return connectsTo(o) || n != 2; }

    void store(int shape) {
        int meta = shape;
        if (straightOnly) meta = (g->world.getMeta(x, y, z) & 8) | shape;
        g->world.setMeta(x, y, z, meta);
        g->notifyNeighbors(x, y, z, g->world.get(x, y, z));
    }

    int slopeFor(int shape) const {
        if (shape == 0) {
            if (rail(x, y + 1, z - 1)) shape = 4;
            if (rail(x, y + 1, z + 1)) shape = 5;
        }
        if (shape == 1) {
            if (rail(x + 1, y + 1, z)) shape = 2;
            if (rail(x - 1, y + 1, z)) shape = 3;
        }
        return shape < 0 ? 0 : shape;
    }

    // func_788_d
    void connectTo(const RailLogic& o) {
        add(o.x, o.y, o.z);
        bool n_ = hasConnection(x, z - 1), s = hasConnection(x, z + 1), w = hasConnection(x - 1, z),
             e = hasConnection(x + 1, z);
        int shape = -1;
        if (n_ || s) shape = 0;
        if (w || e) shape = 1;
        if (!straightOnly) {
            if (s && e && !n_ && !w) shape = 6;
            if (s && w && !n_ && !e) shape = 7;
            if (n_ && w && !s && !e) shape = 8;
            if (n_ && e && !s && !w) shape = 9;
        }
        store(slopeFor(shape));
    }

    // func_786_c
    bool canConnectFrom(int ax, int ay, int az) const {
        RailLogic o;
        if (!logicAt(ax, ay, az, o)) return false;
        o.refreshConnected();
        return o.canConnectTo(*this);
    }

    // func_792_a
    void update(bool powered, bool force) {
        bool n_ = canConnectFrom(x, y, z - 1), s = canConnectFrom(x, y, z + 1), w = canConnectFrom(x - 1, y, z),
             e = canConnectFrom(x + 1, y, z);
        int shape = -1;
        if ((n_ || s) && !w && !e) shape = 0;
        if ((w || e) && !n_ && !s) shape = 1;
        if (!straightOnly) {
            if (s && e && !n_ && !w) shape = 6;
            if (s && w && !n_ && !e) shape = 7;
            if (n_ && w && !s && !e) shape = 8;
            if (n_ && e && !s && !w) shape = 9;
        }
        if (shape == -1) {
            if (n_ || s) shape = 0;
            if (w || e) shape = 1;
            if (!straightOnly) {
                if (powered) {
                    if (s && e) shape = 6;
                    if (w && s) shape = 7;
                    if (e && n_) shape = 9;
                    if (n_ && w) shape = 8;
                } else {
                    if (n_ && w) shape = 8;
                    if (e && n_) shape = 9;
                    if (w && s) shape = 7;
                    if (s && e) shape = 6;
                }
            }
        }
        shape = slopeFor(shape);
        setShape(shape);
        int meta = shape;
        if (straightOnly) meta = (g->world.getMeta(x, y, z) & 8) | shape;
        if (!force && g->world.getMeta(x, y, z) == meta) return;
        g->world.setMeta(x, y, z, meta);
        g->notifyNeighbors(x, y, z, g->world.get(x, y, z));
        for (int i = 0; i < n; ++i) {
            RailLogic o;
            if (!logicAt(c[i][0], c[i][1], c[i][2], o)) continue;
            o.refreshConnected();
            if (o.canConnectTo(*this)) o.connectTo(*this);
        }
    }
};

float wrapDegrees(float d) {
    while (d >= 180.0f) d -= 360.0f;
    while (d < -180.0f) d += 360.0f;
    return d;
}

// World.isMaterialInBB for water.
bool waterInBox(const World& w, const AABB& b) {
    for (int x = ifloor(b.x0); x < ifloor(b.x1 + 1.0f); ++x)
        for (int y = ifloor(b.y0); y < ifloor(b.y1 + 1.0f); ++y)
            for (int z = ifloor(b.z0); z < ifloor(b.z1 + 1.0f); ++z) {
                if (!blockIsWater(w.get(x, y, z))) continue;
                int m = w.getMeta(x, y, z);
                float top = (float)(y + 1);
                if (m < 8) top = (float)(y + 1) - m / 8.0f;
                if (top >= b.y0) return true;
            }
    return false;
}

// Feet height of a rider: vehicle posY + getMountedYOffset + rider getYOffset (the player's eyes sit 1.62 up).
float riderFeet(const Entity& v, bool player) {
    float base;
    switch (v.type) {
        case ENT_MINECART: base = v.y + CART_Y - 0.3f; break;
        case ENT_BOAT: base = v.y + BOAT_Y - 0.3f; break;
        default: base = v.y + v.height * 0.75f; break;
    }
    return player ? base + 1.12f - Player::EYE : base;
}

}  // namespace

// EntityMinecart.func_514_g: the point on the track below (x, y, z); y is in posY terms.
bool minecartRailPos(const World& w, float& x, float& y, float& z) {
    int bx = ifloor(x), by = ifloor(y), bz = ifloor(z);
    if (railAt(w, bx, by - 1, bz)) --by;
    uint8_t id = w.get(bx, by, bz);
    if (!blockIsRail(id)) return false;
    const int(*d)[3] = RAIL_ENDS[railShape(id, w.getMeta(bx, by, bz))];
    float x0 = bx + 0.5f + d[0][0] * 0.5f, y0 = by + 0.5f + d[0][1] * 0.5f, z0 = bz + 0.5f + d[0][2] * 0.5f;
    float x1 = bx + 0.5f + d[1][0] * 0.5f, y1 = by + 0.5f + d[1][1] * 0.5f, z1 = bz + 0.5f + d[1][2] * 0.5f;
    float dx = x1 - x0, dy = (y1 - y0) * 2.0f, dz = z1 - z0, t;
    if (dx == 0) {
        x = bx + 0.5f;
        t = z - bz;
    } else if (dz == 0) {
        z = bz + 0.5f;
        t = x - bx;
    } else {
        t = ((x - x0) * dx + (z - z0) * dz) * 2.0f;
    }
    x = x0 + dx * t;
    y = y0 + dy * t;
    z = z0 + dz * t;
    if (dy < 0) y += 1.0f;
    if (dy > 0) y += 0.5f;
    return true;
}

// EntityMinecart.func_515_a: railPos after moving `offset` along the track.
bool minecartRailPosAhead(const World& w, float x, float y, float z, float offset, float& ox, float& oy, float& oz) {
    int bx = ifloor(x), by = ifloor(y), bz = ifloor(z);
    if (railAt(w, bx, by - 1, bz)) --by;
    uint8_t id = w.get(bx, by, bz);
    if (!blockIsRail(id)) return false;
    int shape = railShape(id, w.getMeta(bx, by, bz));
    y = (float)by;
    if (shape >= 2 && shape <= 5) y = (float)(by + 1);
    const int(*d)[3] = RAIL_ENDS[shape];
    float dx = (float)(d[1][0] - d[0][0]), dz = (float)(d[1][2] - d[0][2]);
    float len = std::sqrt(dx * dx + dz * dz);
    x += dx / len * offset;
    z += dz / len * offset;
    if (d[0][1] != 0 && ifloor(x) - bx == d[0][0] && ifloor(z) - bz == d[0][2])
        y += d[0][1];
    else if (d[1][1] != 0 && ifloor(x) - bx == d[1][0] && ifloor(z) - bz == d[1][2])
        y += d[1][1];
    ox = x, oy = y, oz = z;
    return minecartRailPos(w, ox, oy, oz);
}

// ------------------------------------------------------------------ rails

void Game::railRefresh(int x, int y, int z, bool powered, bool force) {
    if (!blockIsRail(world.get(x, y, z))) return;
    RailLogic(this, x, y, z).update(powered, force);
}

// BlockRail.onNeighborBlockChange
void Game::railNeighborChange(int x, int y, int z, uint8_t id, uint8_t changed) {
    int meta = world.getMeta(x, y, z);
    int shape = id == RAIL ? meta : meta & 7;
    auto cube = [&](int ax, int ay, int az) { return blockIsNormalCube(world.get(ax, ay, az)); };
    bool pop = !cube(x, y - 1, z);
    if (shape == 2 && !cube(x + 1, y, z)) pop = true;
    if (shape == 3 && !cube(x - 1, y, z)) pop = true;
    if (shape == 4 && !cube(x, y, z - 1)) pop = true;
    if (shape == 5 && !cube(x, y, z + 1)) pop = true;
    if (pop) {
        if (mode == MODE_SURVIVAL) dropBlockItems(x, y, z, id, meta);
        setBlockNotify(x, y, z, AIR);
    } else if (id == POWERED_RAIL) {
        bool p = indirectlyPowered(x, y, z) || indirectlyPowered(x, y + 1, z);
        p = p || poweredRailSearch(x, y, z, meta, true, 0) || poweredRailSearch(x, y, z, meta, false, 0);
        bool update = false;
        if (p && !(meta & 8)) {
            world.setMeta(x, y, z, shape | 8);
            notifyNeighbors(x, y, z, id);
            update = true;
        } else if (!p && (meta & 8)) {
            world.setMeta(x, y, z, shape);
            notifyNeighbors(x, y, z, id);
            update = true;
        }
        if (update) {
            notifyNeighbors(x, y - 1, z, id);
            if (shape >= 2 && shape <= 5) notifyNeighbors(x, y + 1, z, id);
        }
    } else if (id == RAIL && changed != AIR && canProvidePower(changed) && RailLogic(this, x, y, z).adjacentRails() == 3) {
        railRefresh(x, y, z, indirectlyPowered(x, y, z), false);
    }
}

// BlockRail.func_27044_a: follows the line of powered rails up to 8 blocks looking for a powered one.
bool Game::poweredRailSearch(int x, int y, int z, int meta, bool forward, int depth) const {
    if (depth >= 8) return false;
    int axis = meta & 7;
    bool lower = true;
    switch (axis) {
        case 0: forward ? ++z : --z; break;
        case 1: forward ? --x : ++x; break;
        case 2:
            if (forward) {
                --x;
            } else {
                ++x, ++y;
                lower = false;
            }
            axis = 1;
            break;
        case 3:
            if (forward) {
                --x, ++y;
                lower = false;
            } else {
                ++x;
            }
            axis = 1;
            break;
        case 4:
            if (forward) {
                ++z;
            } else {
                --z, ++y;
                lower = false;
            }
            axis = 0;
            break;
        case 5:
            if (forward) {
                ++z, ++y;
                lower = false;
            } else {
                --z;
            }
            axis = 0;
            break;
        default: break;
    }
    if (poweredRailAt(x, y, z, forward, depth, axis)) return true;
    return lower && poweredRailAt(x, y - 1, z, forward, depth, axis);
}

// BlockRail.func_27043_a
bool Game::poweredRailAt(int x, int y, int z, bool forward, int depth, int axis) const {
    if (world.get(x, y, z) != POWERED_RAIL) return false;
    int meta = world.getMeta(x, y, z), shape = meta & 7;
    if (axis == 1 && (shape == 0 || shape == 4 || shape == 5)) return false;
    if (axis == 0 && (shape == 1 || shape == 2 || shape == 3)) return false;
    if (!(meta & 8)) return false;
    if (indirectlyPowered(x, y, z) || indirectlyPowered(x, y + 1, z)) return true;
    return poweredRailSearch(x, y, z, meta, forward, depth + 1);
}

// BlockDetectorRail.func_27046_f
void Game::detectorUpdate(int x, int y, int z, int meta) {
    bool was = (meta & 8) != 0, found = false;
    AABB area = {x + 0.125f, (float)y, z + 0.125f, x + 0.875f, y + 0.25f, z + 0.875f};
    for (const Entity& e : ents)
        if (e.type == ENT_MINECART && !e.dead && area.intersects(e.box())) found = true;
    if (found != was) {
        world.setMeta(x, y, z, found ? meta | 8 : meta & 7);
        notifyNeighbors(x, y, z, DETECTOR_RAIL);
        notifyNeighbors(x, y - 1, z, DETECTOR_RAIL);
    }
    if (found) world.scheduleTick(x, y, z, DETECTOR_RAIL, 20);
}

// ------------------------------------------------------------------ riding

void Game::mount(int index) {
    if (index < 0 || index >= MAX_ENTITIES) return;
    Entity& v = ents[index];
    riderYawDelta_ = 0;
    if (riding == index) {
        // Entity.mountEntity with the current vehicle: get off on top of it.
        v.rider = -1;
        riding = -1;
        player.setPosition(v.x, v.box().y0 + v.height, v.z);
        return;
    }
    if (Entity* old = ridingEntity()) old->rider = -1;
    if (v.rider >= 0) ents[v.rider].vehicle = -1;
    v.rider = RIDER_PLAYER;
    riding = index;
}

// Entity.updateRidden for whoever sits in v, after v has moved (World.updateEntityWithOptionalForce).
void Game::tickRider(Entity& v) {
    float ox = 0, oz = 0;
    if (v.type == ENT_BOAT) {
        // EntityBoat.updateRiderPosition
        ox = std::cos(v.yaw * PI / 180.0f) * 0.4f;
        oz = std::sin(v.yaw * PI / 180.0f) * 0.4f;
    }
    if (v.rider == RIDER_PLAYER) {
        player.x = v.x + ox;
        player.y = riderFeet(v, true);
        player.z = v.z + oz;
        player.mx = player.my = player.mz = 0;
        player.fallDistance = 0;
        riderYawDelta_ = wrapDegrees(riderYawDelta_ + (v.yaw - v.prevYaw));
        float d = riderYawDelta_ * 0.5f;
        if (d > 10.0f) d = 10.0f;
        if (d < -10.0f) d = -10.0f;
        riderYawDelta_ -= d;
        player.yaw += d;
        return;
    }
    if (v.rider < 0) return;
    Entity& r = ents[v.rider];
    r.mx = r.my = r.mz = 0;
    tickEntity(r);
    if (r.type == ENT_NONE || r.dead) return;
    r.x = v.x + ox;
    r.y = riderFeet(v, false);
    r.z = v.z + oz;
    r.fallDistance = 0;
    r.landedFall = 0;
}

int Game::entityBoxes(const Body* self, const AABB& area, AABB* out, int max) const {
    const Entity* se = nullptr;
    if ((const void*)self >= (const void*)ents && (const void*)self < (const void*)(ents + MAX_ENTITIES))
        se = static_cast<const Entity*>(self);
    bool vehicle = se && entityIsVehicle(se->type);
    if (!vehicle && boatCount_ == 0) return 0;
    int selfIdx = se ? (int)(se - ents) : -1;
    bool isPlayer = self == &player;
    int n = 0;
    for (int i = 0; i < MAX_ENTITIES && n < max; ++i) {
        const Entity& o = ents[i];
        if (&o == se || o.type == ENT_NONE || o.dead) continue;
        bool add = o.type == ENT_BOAT;
        if (vehicle && !add) add = entityIsVehicle(o.type) || (entityIsMob(o.type) && o.health > 0);
        if (!add) continue;
        if (se && (se->rider == i || o.rider == selfIdx)) continue;
        if (isPlayer && riding == i) continue;
        AABB b = o.box();
        if (b.intersects(area)) out[n++] = b;
    }
    if (vehicle && n < max && !dead && se->rider != RIDER_PLAYER && player.box().intersects(area)) out[n++] = player.box();
    return n;
}

// ------------------------------------------------------------------ minecarts

// EntityMinecart.onUpdate
void Game::tickMinecart(Entity& e) {
    if (e.rollTime > 0) --e.rollTime;
    if (e.rollDamage > 0) --e.rollDamage;
    float px = e.x, py = e.y + CART_Y, pz = e.z;
    e.my -= 0.04f;
    int bx = ifloor(px), by = ifloor(py), bz = ifloor(pz);
    if (railAt(world, bx, by - 1, bz)) --by;
    const float maxSpeed = 0.4f, slope = 0.0078125f;
    bool pushed = false;
    uint8_t id = world.get(bx, by, bz);
    bool ridden = e.rider != -1;
    if (blockIsRail(id)) {
        float vx = px, vy = py, vz = pz;
        bool before = minecartRailPos(world, vx, vy, vz);
        int meta = world.getMeta(bx, by, bz);
        py = (float)by;
        bool boost = false, brake = false;
        if (id == POWERED_RAIL) {
            boost = (meta & 8) != 0;
            brake = !boost;
        }
        int shape = railShape(id, meta);
        if (shape >= 2 && shape <= 5) py = (float)(by + 1);
        if (shape == 2) e.mx -= slope;
        if (shape == 3) e.mx += slope;
        if (shape == 4) e.mz += slope;
        if (shape == 5) e.mz -= slope;
        const int(*d)[3] = RAIL_ENDS[shape];
        float dx = (float)(d[1][0] - d[0][0]), dz = (float)(d[1][2] - d[0][2]);
        float len = std::sqrt(dx * dx + dz * dz);
        if (e.mx * dx + e.mz * dz < 0) dx = -dx, dz = -dz;
        float sp = std::sqrt(e.mx * e.mx + e.mz * e.mz);
        e.mx = sp * dx / len;
        e.mz = sp * dz / len;
        if (brake) {
            if (std::sqrt(e.mx * e.mx + e.mz * e.mz) < 0.03f) {
                e.mx = e.my = e.mz = 0;
            } else {
                e.mx *= 0.5f, e.my = 0, e.mz *= 0.5f;
            }
        }
        float x0 = bx + 0.5f + d[0][0] * 0.5f, z0 = bz + 0.5f + d[0][2] * 0.5f;
        float x1 = bx + 0.5f + d[1][0] * 0.5f, z1 = bz + 0.5f + d[1][2] * 0.5f;
        dx = x1 - x0, dz = z1 - z0;
        float t;
        if (dx == 0) {
            px = bx + 0.5f;
            t = pz - bz;
        } else if (dz == 0) {
            pz = bz + 0.5f;
            t = px - bx;
        } else {
            t = ((px - x0) * dx + (pz - z0) * dz) * 2.0f;
        }
        px = x0 + dx * t;
        pz = z0 + dz * t;
        e.x = px, e.y = py, e.z = pz;
        float mvx = e.mx, mvz = e.mz;
        if (ridden) mvx *= 0.75f, mvz *= 0.75f;
        if (mvx < -maxSpeed) mvx = -maxSpeed;
        if (mvx > maxSpeed) mvx = maxSpeed;
        if (mvz < -maxSpeed) mvz = -maxSpeed;
        if (mvz > maxSpeed) mvz = maxSpeed;
        e.move(world, mvx, 0, mvz);
        px = e.x, py = e.y + CART_Y, pz = e.z;
        if (d[0][1] != 0 && ifloor(px) - bx == d[0][0] && ifloor(pz) - bz == d[0][2])
            py += d[0][1];
        else if (d[1][1] != 0 && ifloor(px) - bx == d[1][0] && ifloor(pz) - bz == d[1][2])
            py += d[1][1];
        if (ridden) {
            e.mx *= 0.997f, e.my = 0, e.mz *= 0.997f;
        } else {
            if (e.cartType == 2) {
                float pl = std::sqrt(e.pushX * e.pushX + e.pushZ * e.pushZ);
                if (pl > 0.01f) {
                    pushed = true;
                    e.pushX /= pl, e.pushZ /= pl;
                    e.mx *= 0.8f, e.my = 0, e.mz *= 0.8f;
                    e.mx += e.pushX * 0.04f;
                    e.mz += e.pushZ * 0.04f;
                } else {
                    e.mx *= 0.9f, e.my = 0, e.mz *= 0.9f;
                }
            }
            e.mx *= 0.96f, e.my = 0, e.mz *= 0.96f;
        }
        float ax = px, ay = py, az = pz;
        if (minecartRailPos(world, ax, ay, az) && before) {
            float dv = (vy - ay) * 0.05f;
            sp = std::sqrt(e.mx * e.mx + e.mz * e.mz);
            if (sp > 0) {
                e.mx = e.mx / sp * (sp + dv);
                e.mz = e.mz / sp * (sp + dv);
            }
            py = ay;
        }
        int nbx = ifloor(px), nbz = ifloor(pz);
        if (nbx != bx || nbz != bz) {
            sp = std::sqrt(e.mx * e.mx + e.mz * e.mz);
            e.mx = sp * (nbx - bx);
            e.mz = sp * (nbz - bz);
        }
        if (e.cartType == 2) {
            float pl = std::sqrt(e.pushX * e.pushX + e.pushZ * e.pushZ);
            if (pl > 0.01f && e.mx * e.mx + e.mz * e.mz > 0.001f) {
                e.pushX /= pl, e.pushZ /= pl;
                if (e.pushX * e.mx + e.pushZ * e.mz < 0) {
                    e.pushX = e.pushZ = 0;
                } else {
                    e.pushX = e.mx;
                    e.pushZ = e.mz;
                }
            }
        }
        if (boost) {
            sp = std::sqrt(e.mx * e.mx + e.mz * e.mz);
            auto cube = [&](int ax2, int ay2, int az2) { return blockIsNormalCube(world.get(ax2, ay2, az2)); };
            if (sp > 0.01f) {
                e.mx += e.mx / sp * 0.06f;
                e.mz += e.mz / sp * 0.06f;
            } else if (shape == 1) {
                if (cube(bx - 1, by, bz))
                    e.mx = 0.02f;
                else if (cube(bx + 1, by, bz))
                    e.mx = -0.02f;
            } else if (shape == 0) {
                if (cube(bx, by, bz - 1))
                    e.mz = 0.02f;
                else if (cube(bx, by, bz + 1))
                    e.mz = -0.02f;
            }
        }
        e.x = px, e.y = py - CART_Y, e.z = pz;
    } else {
        if (e.mx < -maxSpeed) e.mx = -maxSpeed;
        if (e.mx > maxSpeed) e.mx = maxSpeed;
        if (e.mz < -maxSpeed) e.mz = -maxSpeed;
        if (e.mz > maxSpeed) e.mz = maxSpeed;
        if (e.onGround) e.mx *= 0.5f, e.my *= 0.5f, e.mz *= 0.5f;
        e.move(world, e.mx, e.my, e.mz);
        if (!e.onGround) e.mx *= 0.95f, e.my *= 0.95f, e.mz *= 0.95f;
    }
    e.pitch = 0;
    float ddx = e.prevX - e.x, ddz = e.prevZ - e.z;
    if (ddx * ddx + ddz * ddz > 0.001f) {
        e.yaw = std::atan2(ddz, ddx) * 180.0f / PI;
        if (e.cartFlipped) e.yaw += 180.0f;
    }
    float turn = wrapDegrees(e.yaw - e.prevYaw);
    if (turn < -170.0f || turn >= 170.0f) {
        e.yaw += 180.0f;
        e.cartFlipped = !e.cartFlipped;
    }
    AABB near = e.box();
    near.x0 -= 0.2f, near.x1 += 0.2f, near.z0 -= 0.2f, near.z1 += 0.2f;
    int self = (int)(&e - ents);
    for (int i = 0; i < MAX_ENTITIES; ++i) {
        Entity& o = ents[i];
        if (&o == &e || o.type != ENT_MINECART || o.dead || e.rider == i || !near.intersects(o.box())) continue;
        cartCollide(o, e, &e);
    }
    (void)self;
    if (pushed && rand.nextInt(4) == 0 && --e.fuel < 0) e.pushX = e.pushZ = 0;
    if (e.cartType == 2 && e.fuel > 0 && particles.rnd() < 0.25f) particles.spawn(world, PART_SMOKE, e.x, e.y + 0.8f, e.z, 0, 0, 0, 2.5f);
    if (e.y < -64) e.dead = true;
}

// EntityMinecart.applyEntityCollision: `other` bumps into the cart.
void Game::cartCollide(Entity& cart, Body& other, Entity* otherEnt) {
    int cartIdx = (int)(&cart - ents);
    if (otherEnt) {
        int oi = (int)(otherEnt - ents);
        if (cart.rider == oi) return;
        if (entityIsMob(otherEnt->type) && otherEnt->health > 0 && cart.cartType == 0 &&
            cart.mx * cart.mx + cart.mz * cart.mz > 0.01f && cart.rider == -1 && otherEnt->vehicle == -1 &&
            otherEnt->rider == -1) {
            cart.rider = (int16_t)oi;
            otherEnt->vehicle = (int16_t)cartIdx;
        }
    } else if (cart.rider == RIDER_PLAYER) {
        return;
    }
    float dx = other.x - cart.x, dz = other.z - cart.z;
    float d2 = dx * dx + dz * dz;
    if (d2 < 1.0e-4f) return;
    float d = std::sqrt(d2);
    dx /= d, dz /= d;
    float f = 1.0f / d;
    if (f > 1) f = 1;
    dx *= f * 0.1f * 0.5f;
    dz *= f * 0.1f * 0.5f;
    if (otherEnt && otherEnt->type == ENT_MINECART) {
        Entity& o = *otherEnt;
        float ox = o.x - cart.x, oz = o.z - cart.z;
        float k = ox * o.mz + oz * o.prevX;  // sic: Beta multiplies by the other cart's last x position
        if (k * k > 5.0f) return;
        float sx = o.mx + cart.mx, sz = o.mz + cart.mz;
        if (o.cartType == 2 && cart.cartType != 2) {
            cart.mx *= 0.2f, cart.mz *= 0.2f;
            cart.mx += o.mx - dx, cart.mz += o.mz - dz;
            o.mx *= 0.7f, o.mz *= 0.7f;
        } else if (o.cartType != 2 && cart.cartType == 2) {
            o.mx *= 0.2f, o.mz *= 0.2f;
            o.mx += cart.mx + dx, o.mz += cart.mz + dz;
            cart.mx *= 0.7f, cart.mz *= 0.7f;
        } else {
            sx /= 2, sz /= 2;
            cart.mx *= 0.2f, cart.mz *= 0.2f;
            cart.mx += sx - dx, cart.mz += sz - dz;
            o.mx *= 0.2f, o.mz *= 0.2f;
            o.mx += sx + dx, o.mz += sz + dz;
        }
    } else {
        cart.mx -= dx, cart.mz -= dz;
        other.mx += dx / 4, other.mz += dz / 4;
    }
}

// ------------------------------------------------------------------ boats

// EntityBoat.onUpdate
void Game::tickBoat(Entity& e) {
    e.updateLiquids(world);
    if (e.rollTime > 0) --e.rollTime;
    if (e.rollDamage > 0) --e.rollDamage;
    AABB b = e.box();
    float buoy = 0;
    for (int i = 0; i < 5; ++i) {
        float y0 = b.y0 + (b.y1 - b.y0) * i / 5.0f - 0.125f, y1 = b.y0 + (b.y1 - b.y0) * (i + 1) / 5.0f - 0.125f;
        if (waterInBox(world, {b.x0, y0, b.z0, b.x1, y1, b.z1})) buoy += 0.2f;
    }
    if (buoy < 1.0f) {
        e.my += 0.04f * (buoy * 2.0f - 1.0f);
    } else {
        if (e.my < 0) e.my /= 2.0f;
        e.my += 0.007f;
    }
    if (e.rider == RIDER_PLAYER) {
        e.mx += riderMX_ * 0.2f;
        e.mz += riderMZ_ * 0.2f;
    }
    const float maxSpeed = 0.4f;
    if (e.mx < -maxSpeed) e.mx = -maxSpeed;
    if (e.mx > maxSpeed) e.mx = maxSpeed;
    if (e.mz < -maxSpeed) e.mz = -maxSpeed;
    if (e.mz > maxSpeed) e.mz = maxSpeed;
    if (e.onGround) e.mx *= 0.5f, e.my *= 0.5f, e.mz *= 0.5f;
    e.move(world, e.mx, e.my, e.mz);
    float sp = std::sqrt(e.mx * e.mx + e.mz * e.mz);
    if (e.collidedH && sp > 0.15f) {
        if (e.rider == RIDER_PLAYER) mount((int)(&e - ents));
        e.dead = true;
        for (int i = 0; i < 3; ++i) dropItem(ItemStack(PLANKS), e.x, e.y + BOAT_Y, e.z, false);
        for (int i = 0; i < 2; ++i) dropItem(ItemStack(I_STICK), e.x, e.y + BOAT_Y, e.z, false);
        return;
    }
    e.mx *= 0.99f, e.my *= 0.95f, e.mz *= 0.99f;
    e.pitch = 0;
    float target = e.yaw;
    float ddx = e.prevX - e.x, ddz = e.prevZ - e.z;
    if (ddx * ddx + ddz * ddz > 0.001f) target = std::atan2(ddz, ddx) * 180.0f / PI;
    float turn = wrapDegrees(target - e.yaw);
    if (turn > 20.0f) turn = 20.0f;
    if (turn < -20.0f) turn = -20.0f;
    e.yaw += turn;
    AABB near = e.box();
    near.x0 -= 0.2f, near.x1 += 0.2f, near.z0 -= 0.2f, near.z1 += 0.2f;
    for (int i = 0; i < MAX_ENTITIES; ++i) {
        Entity& o = ents[i];
        if (&o == &e || o.type != ENT_BOAT || o.dead || e.rider == i || !near.intersects(o.box())) continue;
        // Entity.applyEntityCollision
        float dx = e.x - o.x, dz = e.z - o.z;
        float d = std::fabs(dx) > std::fabs(dz) ? std::fabs(dx) : std::fabs(dz);
        if (d < 0.01f) continue;
        d = std::sqrt(d);
        dx /= d, dz /= d;
        float f = 1.0f / d;
        if (f > 1) f = 1;
        dx *= f * 0.05f, dz *= f * 0.05f;
        o.mx -= dx, o.mz -= dz;
        e.mx += dx, e.mz += dz;
    }
    for (int i = 0; i < 4; ++i) {
        int x = ifloor(e.x + ((i % 2) - 0.5f) * 0.8f), y = ifloor(e.y + BOAT_Y), z = ifloor(e.z + ((i / 2) - 0.5f) * 0.8f);
        if (world.get(x, y, z) == SNOW_LAYER) setBlockNotify(x, y, z, AIR);
    }
    if (e.y < -64) e.dead = true;
}

// Living things walking into vehicles (EntityLiving.onLivingUpdate pushing whatever canBePushed).
void Game::vehicleCollisions() {
    for (int i = 0; i < MAX_ENTITIES; ++i) {
        Entity& v = ents[i];
        if (!entityIsVehicle(v.type) || v.dead) continue;
        AABB vb = v.box();
        auto touches = [&](const Body& b) {
            AABB a = b.box();
            a.x0 -= 0.2f, a.x1 += 0.2f, a.z0 -= 0.2f, a.z1 += 0.2f;
            return a.intersects(vb);
        };
        auto boatPush = [&](Body& other) {
            float dx = other.x - v.x, dz = other.z - v.z;
            float d = std::fabs(dx) > std::fabs(dz) ? std::fabs(dx) : std::fabs(dz);
            if (d < 0.01f) return;
            d = std::sqrt(d);
            dx /= d, dz /= d;
            float f = 1.0f / d;
            if (f > 1) f = 1;
            dx *= f * 0.05f, dz *= f * 0.05f;
            v.mx -= dx, v.mz -= dz;
            other.mx += dx, other.mz += dz;
        };
        if (!dead && riding != i && touches(player)) {
            if (v.type == ENT_MINECART)
                cartCollide(v, player, nullptr);
            else
                boatPush(player);
        }
        for (int j = 0; j < MAX_ENTITIES; ++j) {
            Entity& m = ents[j];
            if (!entityIsMob(m.type) || m.health <= 0 || v.rider == j || m.vehicle == i || !touches(m)) continue;
            if (v.type == ENT_MINECART)
                cartCollide(v, m, &m);
            else
                boatPush(m);
        }
    }
}

// EntityMinecart / EntityBoat.attackEntityFrom
bool Game::hurtVehicle(Entity& e, int damage) {
    if (e.dead) return false;
    e.rollDir = (int16_t)-e.rollDir;
    e.rollTime = 10;
    e.rollDamage = (int16_t)(e.rollDamage + damage * 10);
    bool creative = mode == MODE_CREATIVE;
    if (e.rollDamage <= 40 && !creative) return true;
    if (e.rider == RIDER_PLAYER) mount((int)(&e - ents));
    if (e.rider >= 0) ents[e.rider].vehicle = -1;
    e.rider = -1;
    e.dead = true;
    if (creative) return true;
    float y = e.y + (e.type == ENT_MINECART ? CART_Y : BOAT_Y);
    if (e.type == ENT_BOAT) {
        for (int i = 0; i < 3; ++i) dropItem(ItemStack(PLANKS), e.x, y, e.z, false);
        for (int i = 0; i < 2; ++i) dropItem(ItemStack(I_STICK), e.x, y, e.z, false);
        return true;
    }
    for (ItemStack& s : e.cartItems) {
        if (s.empty()) continue;
        float ox = rand.nextFloat() * 0.8f + 0.1f, oy = rand.nextFloat() * 0.8f + 0.1f, oz = rand.nextFloat() * 0.8f + 0.1f;
        while (s.count > 0) {
            int k = rand.nextInt(21) + 10;
            if (k > s.count) k = s.count;
            s.count = (uint8_t)(s.count - k);
            dropItem(ItemStack(s.id, k, s.damage), e.x + ox, y + oy, e.z + oz, false);
        }
        s.clear();
    }
    dropItem(ItemStack(I_MINECART), e.x, y, e.z, false);
    if (e.cartType == 1) dropItem(ItemStack(CHEST), e.x, y, e.z, false);
    if (e.cartType == 2) dropItem(ItemStack(FURNACE), e.x, y, e.z, false);
    return true;
}

// Entity.interact for minecarts, boats and pigs, then ItemSaddle.saddleEntity.
bool Game::useOnEntity(Entity& e) {
    int idx = (int)(&e - ents);
    ItemStack& h = inv.held();
    switch (e.type) {
        case ENT_MINECART:
            if (e.dead) return false;
            if (e.cartType == 1) {
                openCart = idx;
                lastUse = UseResult::CartChest;
            } else if (e.cartType == 2) {
                if (h.id == I_COAL) {
                    if (mode == MODE_SURVIVAL && --h.count == 0) h.clear();
                    e.fuel += 1200;
                }
                e.pushX = e.x - player.x;
                e.pushZ = e.z - player.z;
            } else {
                mount(idx);
            }
            return true;
        case ENT_BOAT:
            if (e.dead) return false;
            mount(idx);
            return true;
        case ENT_PIG:
            if (e.health <= 0) return false;
            if (e.saddled) {
                mount(idx);
                return true;
            }
            if (h.id == I_SADDLE) {
                e.saddled = true;
                if (--h.count == 0) h.clear();
                return true;
            }
            return false;
        default: return false;
    }
}

// ItemMinecart.onItemUse
bool Game::placeCart(const RayHit& hit) {
    ItemStack& h = inv.held();
    if (!blockIsRail(world.get(hit.x, hit.y, hit.z))) return false;
    Entity* e = spawn(ENT_MINECART, hit.x + 0.5f, hit.y + 0.5f, hit.z + 0.5f);
    if (!e) return false;
    e->cartType = h.id == I_CHEST_MINECART ? 1 : h.id == I_FURNACE_MINECART ? 2 : 0;
    if (mode == MODE_SURVIVAL && --h.count == 0) h.clear();
    return true;
}

// ItemBoat.onItemRightClick: the first block or still liquid within 5 blocks of the eye.
bool Game::placeBoat() {
    float dx, dy, dz;
    player.look(dx, dy, dz);
    float ex = player.x, ey = player.y + Player::EYE, ez = player.z;
    RayHit hit;
    bool solid = raycast(world, ex, ey, ez, dx, dy, dz, 5.0f, hit);
    float limit = solid ? hit.t : 5.0f;
    int bx = 0, by = 0, bz = 0;
    bool found = false;
    for (float t = 0; t < limit; t += 0.05f) {
        int x = ifloor(ex + dx * t), y = ifloor(ey + dy * t), z = ifloor(ez + dz * t);
        if (blockIsLiquid(world.get(x, y, z)) && world.getMeta(x, y, z) == 0) {
            bx = x, by = y, bz = z;
            found = true;
            break;
        }
    }
    if (!found && solid) bx = hit.x, by = hit.y, bz = hit.z, found = true;
    if (!found) return false;
    if (world.get(bx, by, bz) == SNOW_LAYER) --by;
    if (!spawn(ENT_BOAT, bx + 0.5f, by + 1.0f, bz + 0.5f)) return false;
    ItemStack& h = inv.held();
    if (mode == MODE_SURVIVAL && --h.count == 0) h.clear();
    return true;
}
