// The Nether from Beta 1.7.3: BlockPortal, the portal timer of EntityPlayerSP, Minecraft.usePortal, Teleporter,
// EntityGhast (EntityFlying), EntityFireball and the anger of EntityPigZombie.
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "game.h"

namespace {

constexpr float PI = 3.14159265f;
constexpr float DEG = PI / 180.0f;

inline int ifloor(float v) {
    int i = (int)v;
    return v < (float)i ? i - 1 : i;
}

float gaussian(JRandom& r) {
    float s = 0;
    for (int i = 0; i < 4; ++i) s += r.nextFloat();
    return (s - 2.0f) * 1.732f;
}

}  // namespace

// ------------------------------------------------------------------ portal block

// BlockPortal.tryToCreatePortal: an obsidian frame 4 wide and 5 tall (corners optional) around air or fire.
bool Game::tryCreatePortal(int x, int y, int z) {
    int dx = 0, dz = 0;
    if (world.get(x - 1, y, z) == OBSIDIAN || world.get(x + 1, y, z) == OBSIDIAN) dx = 1;
    if (world.get(x, y, z - 1) == OBSIDIAN || world.get(x, y, z + 1) == OBSIDIAN) dz = 1;
    if (dx == dz) return false;
    if (world.get(x - dx, y, z - dz) == AIR) {
        x -= dx;
        z -= dz;
    }
    for (int i = -1; i <= 2; ++i)
        for (int j = -1; j <= 3; ++j) {
            bool edge = i == -1 || i == 2 || j == -1 || j == 3;
            if ((i == -1 || i == 2) && (j == -1 || j == 3)) continue;
            uint8_t id = world.get(x + dx * i, y + j, z + dz * i);
            if (edge) {
                if (id != OBSIDIAN) return false;
            } else if (id != AIR && id != FIRE) {
                return false;
            }
        }
    editingBlocks_ = true;
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 3; ++j) setBlockNotify(x + dx * i, y + j, z + dz * i, PORTAL);
    editingBlocks_ = false;
    rememberPortal(x, y, z);
    return true;
}

// BlockPortal.onNeighborBlockChange: the portal collapses unless its column and frame are intact.
void Game::portalNeighborChange(int x, int y, int z) {
    int dx = 0, dz = 1;
    if (world.get(x - 1, y, z) == PORTAL || world.get(x + 1, y, z) == PORTAL) dx = 1, dz = 0;
    int bottom = y;
    while (world.get(x, bottom - 1, z) == PORTAL) --bottom;
    if (world.get(x, bottom - 1, z) != OBSIDIAN) {
        setBlockNotify(x, y, z, AIR);
        return;
    }
    int h = 1;
    while (h < 4 && world.get(x, bottom + h, z) == PORTAL) ++h;
    if (h != 3 || world.get(x, bottom + h, z) != OBSIDIAN) {
        setBlockNotify(x, y, z, AIR);
        return;
    }
    bool alongX = world.get(x - 1, y, z) == PORTAL || world.get(x + 1, y, z) == PORTAL;
    bool alongZ = world.get(x, y, z - 1) == PORTAL || world.get(x, y, z + 1) == PORTAL;
    if (alongX && alongZ) {
        setBlockNotify(x, y, z, AIR);
        return;
    }
    uint8_t a = world.get(x + dx, y, z + dz), b = world.get(x - dx, y, z - dz);
    if (!((a == OBSIDIAN && b == PORTAL) || (b == OBSIDIAN && a == PORTAL))) setBlockNotify(x, y, z, AIR);
}

// ------------------------------------------------------------------ travel

// EntityPlayerSP.onLivingUpdate: four seconds in a portal sends the player through.
void Game::tickPortal() {
    prevPortalTime = portalTime;
    if (inPortal_) {
        if (riding >= 0) mount(riding);
        if (portalTime == 0.0f) playUiSound("portal.trigger", 1.0f, soundRand.nextFloat() * 0.4f + 0.8f);
        portalTime += 0.0125f;
        if (portalTime >= 1.0f) {
            portalTime = 1.0f;
            if (!portalPending) {
                portalCooldown = 10;
                playUiSound("portal.travel", 1.0f, soundRand.nextFloat() * 0.4f + 0.8f);
                portalPending = true;
            }
        }
        inPortal_ = false;
    } else {
        if (portalTime > 0.0f) portalTime -= 0.05f;
        if (portalTime < 0.0f) portalTime = 0.0f;
    }
    if (portalCooldown > 0) --portalCooldown;
}

bool Game::travelDimension() {
    portalPending = false;
    portalCooldown = 10;
    if (!world.isOpen()) return false;
    if (riding >= 0) mount(riding);
    int target = dimension < 0 ? 0 : -1;
    float scale = target < 0 ? 1.0f / 8.0f : 8.0f;
    float px = player.x * scale, py = player.y, pz = player.z * scale;
    float yaw = player.yaw, pitch = player.pitch;
    int64_t seed = world.seed, time = world.time;
    closeWorld();
    dimension = target;
    char dir[64];
    dimensionDir(dir, sizeof(dir), target);
    if (!world.open(dir, seed, target)) return false;
    world.time = time;
    player.setPosition(px, py, pz);
    player.yaw = yaw;
    player.pitch = pitch;
    player.mx = player.my = player.mz = 0;
    player.fallDistance = 0;
    inPortal_ = false;
    world.skySubtracted = computeSkySubtracted();
    saveLevel();
    return true;
}

bool Game::placeInPortal() {
    if (findPortal()) return true;
    int best = -1;
    float bestD = 0;
    for (int i = 0; i < knownPortalCount_; ++i) {
        const int32_t* p = knownPortals_[i];
        float dx = p[0] + 0.5f - player.x, dz = p[2] + 0.5f - player.z;
        if (p[3] != dimension || std::fabs(dx) > 128 || std::fabs(dz) > 128) continue;
        if (world.chunk(p[0] >> 4, p[2] >> 4)) {
            // Loaded but not found by findPortal: the portal is gone.
            std::memmove(knownPortals_[i], knownPortals_[i + 1], sizeof(knownPortals_[0]) * (knownPortalCount_ - i - 1));
            --knownPortalCount_;
            --i;
            continue;
        }
        float d = dx * dx + dz * dz;
        if (best < 0 || d < bestD) best = i, bestD = d;
    }
    if (best >= 0) {
        player.setPosition(knownPortals_[best][0] + 0.5f, (float)knownPortals_[best][1], knownPortals_[best][2] + 0.5f);
        player.mx = player.my = player.mz = 0;
        return false;
    }
    createPortal();
    findPortal();
    return true;
}

void Game::rememberPortal(int x, int y, int z) {
    for (int i = 0; i < knownPortalCount_; ++i) {
        int32_t* p = knownPortals_[i];
        if (p[3] == dimension && std::abs(p[0] - x) <= 4 && std::abs(p[1] - y) <= 5 && std::abs(p[2] - z) <= 4) {
            p[0] = x, p[1] = y, p[2] = z;
            return;
        }
    }
    if (knownPortalCount_ == MAX_KNOWN_PORTALS) {
        std::memmove(knownPortals_[0], knownPortals_[1], sizeof(knownPortals_[0]) * (MAX_KNOWN_PORTALS - 1));
        --knownPortalCount_;
    }
    int32_t* p = knownPortals_[knownPortalCount_++];
    p[0] = x, p[1] = y, p[2] = z, p[3] = dimension;
}

// Teleporter.findPortal: the nearest portal within 128 blocks (only loaded chunks are searched).
bool Game::findPortal() {
    const int R = 128;
    int px = ifloor(player.x), pz = ifloor(player.z);
    float best = -1.0f;
    int bx = 0, by = 0, bz = 0;
    for (int cx = (px - R) >> 4; cx <= (px + R) >> 4; ++cx)
        for (int cz = (pz - R) >> 4; cz <= (pz + R) >> 4; ++cz) {
            const Chunk* c = world.chunk(cx, cz);
            if (!c) continue;
            for (int sy = 0; sy < World::SECTIONS_Y; ++sy) {
                const Section* s = c->sec[sy];
                if (!s) continue;
                for (int i = 0; i < 4096; ++i) {
                    if (s->blocks[i] != PORTAL) continue;
                    int x = cx * 16 + (i >> 8), y = sy * 16 + (i & 15), z = cz * 16 + ((i >> 4) & 15);
                    if (x < px - R || x > px + R || z < pz - R || z > pz + R) continue;
                    if (world.get(x, y - 1, z) == PORTAL) continue;  // only the bottom of each column
                    float dx = x + 0.5f - player.x, dy = y + 0.5f - player.y, dz = z + 0.5f - player.z;
                    float d = dx * dx + dy * dy + dz * dz;
                    if (best < 0 || d < best) best = d, bx = x, by = y, bz = z;
                }
            }
        }
    if (best < 0) return false;
    rememberPortal(bx, by, bz);
    float x = bx + 0.5f, y = by + 0.5f, z = bz + 0.5f;
    if (world.get(bx - 1, by, bz) == PORTAL) x -= 0.5f;
    if (world.get(bx + 1, by, bz) == PORTAL) x += 0.5f;
    if (world.get(bx, by, bz - 1) == PORTAL) z -= 0.5f;
    if (world.get(bx, by, bz + 1) == PORTAL) z += 0.5f;
    player.setPosition(x, y, z);
    player.pitch = 0;
    player.mx = player.my = player.mz = 0;
    return true;
}

// Teleporter.createPortal: the nearest spot within 16 blocks with room for a frame on solid ground;
// otherwise a frame on a small obsidian platform at y 70..118.
void Game::createPortal() {
    const int R = 16;
    int px = ifloor(player.x), py = ifloor(player.y), pz = ifloor(player.z);
    int fx = px, fy = py, fz = pz, dir = 0;
    float best = -1.0f;
    int start = rand.nextInt(4);
    auto air = [&](int x, int y, int z) { return world.get(x, y, z) == AIR; };
    for (int pass = 0; pass < 2 && best < 0; ++pass) {
        int orientations = pass == 0 ? 4 : 2;
        for (int x = px - R; x <= px + R; ++x) {
            float dx = x + 0.5f - player.x;
            for (int z = pz - R; z <= pz + R; ++z) {
                float dz = z + 0.5f - player.z;
                for (int y = World::SY - 1; y >= 0; --y) {
                    if (!air(x, y, z)) continue;
                    while (y > 0 && air(x, y - 1, z)) --y;
                    for (int o = start; o < start + orientations; ++o) {
                        int ox = o % 2, oz = 1 - ox;
                        if (o % 4 >= 2) ox = -ox, oz = -oz;
                        bool ok = true;
                        if (pass == 0) {
                            for (int w = 0; w < 3 && ok; ++w)
                                for (int l = 0; l < 4 && ok; ++l)
                                    for (int h = -1; h < 4 && ok; ++h) {
                                        int bx = x + (l - 1) * ox + w * oz, by = y + h, bz = z + (l - 1) * oz - w * ox;
                                        ok = h < 0 ? blockMatSolid(world.get(bx, by, bz)) : air(bx, by, bz);
                                    }
                        } else {
                            for (int l = 0; l < 4 && ok; ++l)
                                for (int h = -1; h < 4 && ok; ++h) {
                                    int bx = x + (l - 1) * ox, by = y + h, bz = z + (l - 1) * oz;
                                    ok = h < 0 ? blockMatSolid(world.get(bx, by, bz)) : air(bx, by, bz);
                                }
                        }
                        // Beta abandons the remaining orientations of this spot once one does not fit.
                        if (!ok) break;
                        float dy = y + 0.5f - player.y;
                        float d = dx * dx + dy * dy + dz * dz;
                        if (best < 0 || d < best) best = d, fx = x, fy = y, fz = z, dir = o % (pass == 0 ? 4 : 2);
                    }
                }
            }
        }
    }
    int ox = dir % 2, oz = 1 - ox;
    if (dir % 4 >= 2) ox = -ox, oz = -oz;
    if (best < 0) {
        fy = fy < 70 ? 70 : fy > 118 ? 118 : fy;
        for (int w = -1; w <= 1; ++w)
            for (int l = 1; l < 3; ++l)
                for (int h = -1; h < 3; ++h)
                    setBlockNotify(fx + (l - 1) * ox + w * oz, fy + h, fz + (l - 1) * oz - w * ox, h < 0 ? OBSIDIAN : AIR);
    }
    editingBlocks_ = true;
    for (int l = 0; l < 4; ++l)
        for (int h = -1; h < 4; ++h) {
            bool frame = l == 0 || l == 3 || h == -1 || h == 3;
            setBlockNotify(fx + (l - 1) * ox, fy + h, fz + (l - 1) * oz, frame ? OBSIDIAN : PORTAL);
        }
    editingBlocks_ = false;
    for (int l = 0; l < 4; ++l)
        for (int h = -1; h < 4; ++h) {
            int x = fx + (l - 1) * ox, y = fy + h, z = fz + (l - 1) * oz;
            notifyNeighbors(x, y, z, world.get(x, y, z));
        }
}

// ------------------------------------------------------------------ ghasts and fireballs

// EntityGhast.isCourseTraversable: the body swept toward the waypoint hits nothing.
bool Game::ghastCourseClear(const Entity& e, float x, float y, float z, float dist) const {
    float sx = (x - e.x) / dist, sy = (y - e.y) / dist, sz = (z - e.z) / dist;
    AABB b = e.box();
    for (int i = 1; i < dist; ++i) {
        b = b.offset(sx, sy, sz);
        if (e.collides(world, b)) return false;
    }
    return true;
}

// EntityGhast.updateEntityActionState + EntityFlying.moveEntityWithHeading. The waypoint lives in
// swimX/Y/Z, the course change cooldown in jumpDelay, the retarget cooldown in lookTicks and the attack
// counter in fuse.
void Game::tickGhast(Entity& e) {
    e.prevFuse = e.fuse;
    float dx = e.swimX - e.x, dy = e.swimY - e.y, dz = e.swimZ - e.z;
    float d = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (d < 1.0f || d > 60.0f) {
        e.swimX = e.x + (rand.nextFloat() * 2.0f - 1.0f) * 16.0f;
        e.swimY = e.y + (rand.nextFloat() * 2.0f - 1.0f) * 16.0f;
        e.swimZ = e.z + (rand.nextFloat() * 2.0f - 1.0f) * 16.0f;
    }
    if (e.jumpDelay-- <= 0) {
        e.jumpDelay = (int16_t)(e.jumpDelay + rand.nextInt(5) + 2);
        if (d > 0.0f && ghastCourseClear(e, e.swimX, e.swimY, e.swimZ, d)) {
            e.mx += dx / d * 0.1f;
            e.my += dy / d * 0.1f;
            e.mz += dz / d * 0.1f;
        } else {
            e.swimX = e.x, e.swimY = e.y, e.swimZ = e.z;
        }
    }
    bool canTarget = !dead && mode == MODE_SURVIVAL;
    if (!canTarget) {
        e.targeting = false;
    } else if (!e.targeting || e.lookTicks-- <= 0) {
        float px = player.x - e.x, py = player.y - e.y, pz = player.z - e.z;
        e.targeting = px * px + py * py + pz * pz < 100.0f * 100.0f;
        if (e.targeting) e.lookTicks = 20;
    }
    float tx = player.x - e.x, tz = player.z - e.z;
    if (e.targeting && tx * tx + (player.y - e.y) * (player.y - e.y) + tz * tz < 64.0f * 64.0f) {
        float ty = player.y + Player::HEIGHT / 2.0f - (e.y + e.height / 2.0f);
        e.yaw = -std::atan2(tx, tz) / DEG;
        if (canSee(e, player.x, player.y + Player::EYE, player.z)) {
            if (e.fuse == 10) playSoundAt(e, "mob.ghast.charge", 10.0f, mobPitch());
            if (++e.fuse == 20) {
                playSoundAt(e, "mob.ghast.fireball", 10.0f, mobPitch());
                shootFireball(e, tx, ty, tz);
                e.fuse = -40;
            }
        } else if (e.fuse > 0) {
            --e.fuse;
        }
    } else {
        e.yaw = -std::atan2(e.mx, e.mz) / DEG;
        if (e.fuse > 0) --e.fuse;
    }
    if (e.inWater) {
        e.move(world, e.mx, e.my, e.mz);
        e.mx *= 0.8f, e.my *= 0.8f, e.mz *= 0.8f;
    } else if (e.inLava) {
        e.move(world, e.mx, e.my, e.mz);
        e.mx *= 0.5f, e.my *= 0.5f, e.mz *= 0.5f;
    } else {
        float f = e.onGround ? 0.546f : 0.91f;
        e.move(world, e.mx, e.my, e.mz);
        e.mx *= f, e.my *= f, e.mz *= f;
    }
    e.fallDistance = 0;
    e.landedFall = 0;
}

// EntityFireball(world, shooter, dx, dy, dz) spawned 4 blocks in front of the ghast. The acceleration is kept
// in swimX/Y/Z and the shooter in target.
void Game::shootFireball(Entity& shooter, float dx, float dy, float dz) {
    float lx = -std::sin(shooter.yaw * DEG), lz = std::cos(shooter.yaw * DEG);
    Entity* f = spawn(ENT_FIREBALL, shooter.x + lx * 4.0f, shooter.y + shooter.height / 2.0f + 0.5f,
                      shooter.z + lz * 4.0f);
    if (!f) return;
    dx += gaussian(rand) * 0.4f;
    dy += gaussian(rand) * 0.4f;
    dz += gaussian(rand) * 0.4f;
    float l = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (l < 1e-4f) l = 1e-4f;
    f->swimX = dx / l * 0.1f, f->swimY = dy / l * 0.1f, f->swimZ = dz / l * 0.1f;
    f->target = (int16_t)(&shooter - ents);
    f->yaw = f->prevYaw = shooter.yaw;
}

// EntityFireball.onUpdate: flies with constant acceleration and explodes (power 1, with fire) on whatever it
// touches first. The shooter is ignored for the first 25 ticks.
void Game::tickFireball(Entity& e) {
    ++e.airTicks;
    particles.spawn(world, PART_SMOKE, e.x, e.y + 0.5f, e.z, 0, 0, 0);
    if (e.age > 1200 || e.y < -64 || e.y > 256) {
        e.dead = true;
        return;
    }
    float speed = std::sqrt(e.mx * e.mx + e.my * e.my + e.mz * e.mz);
    if (speed > 1e-4f) {
        float dx = e.mx / speed, dy = e.my / speed, dz = e.mz / speed;
        RayHit hit;
        bool blockHit = raycast(world, e.x, e.y, e.z, dx, dy, dz, speed, hit);
        float best = blockHit ? hit.t : speed;
        bool struck = blockHit;
        for (Entity& o : ents) {
            if (&o == &e || o.type == ENT_PAINTING || !(entityIsMob(o.type) ? o.health > 0 : !o.dead)) continue;
            if (!entityIsMob(o.type) && !entityIsVehicle(o.type) && o.type != ENT_FIREBALL) continue;
            if (&o - ents == e.target && e.airTicks < 25) continue;
            float t;
            int face;
            if (rayBox(e.x, e.y, e.z, dx, dy, dz, o.box().grow(0.3f), t, face) && t >= 0 && t < best) {
                best = t;
                struck = true;
            }
        }
        if (!dead) {
            float t;
            int face;
            if (rayBox(e.x, e.y, e.z, dx, dy, dz, player.box().grow(0.3f), t, face) && t >= 0 && t < best)
                struck = true;
        }
        if (struck) {
            e.dead = true;
            bool wasDead = dead;
            explode(e.x, e.y, e.z, 1.0f, true);
            if (!wasDead && dead) deathMessage = "was fireballed by a ghast";
            return;
        }
    }
    e.x += e.mx, e.y += e.my, e.z += e.mz;
    float drag = blockIsWater(world.get(ifloor(e.x), ifloor(e.y), ifloor(e.z))) ? 0.8f : 0.95f;
    e.mx = (e.mx + e.swimX) * drag;
    e.my = (e.my + e.swimY) * drag;
    e.mz = (e.mz + e.swimZ) * drag;
}

// EntityPigZombie.attackEntityFrom: every zombie pigman within 32 blocks turns on the player for good.
void Game::angerPigZombies(const Entity& victim) {
    AABB area = victim.box().grow(32.0f);
    for (Entity& p : ents) {
        if (p.type != ENT_PIGZOMBIE || p.health <= 0 || !area.intersects(p.box())) continue;
        p.angry = true;
        p.targeting = true;
        p.target = -1;
        p.jumpDelay = (int16_t)rand.nextInt(40);
    }
}
