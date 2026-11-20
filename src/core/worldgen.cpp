// Port of Beta 1.7.3 ChunkProviderGenerate, MapGenCaves, WorldChunkManager and
// the WorldGen* decorators used by populate().
#include "worldgen.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "game.h"
#include "mathhelper.h"

namespace {

struct BiomeInfo {
    const char* name;
    uint8_t top, filler;
};

const BiomeInfo BIOMES[BIOME_COUNT] = {
    {"Rainforest", GRASS, DIRT}, {"Swampland", GRASS, DIRT}, {"Seasonal Forest", GRASS, DIRT},
    {"Forest", GRASS, DIRT},     {"Savanna", GRASS, DIRT},   {"Shrubland", GRASS, DIRT},
    {"Taiga", GRASS, DIRT},      {"Desert", SAND, SAND},     {"Plains", GRASS, DIRT},
    {"Ice Desert", SAND, SAND},  {"Tundra", GRASS, DIRT},
};

uint8_t g_biomeLookup[4096];

uint8_t getBiome(float t, float h) {
    h *= t;
    if (t < 0.1f) return BIOME_TUNDRA;
    if (h < 0.2f) {
        if (t < 0.5f) return BIOME_TUNDRA;
        return t < 0.95f ? BIOME_SAVANNA : BIOME_DESERT;
    }
    if (h > 0.5f && t < 0.7f) return BIOME_SWAMPLAND;
    if (t < 0.5f) return BIOME_TAIGA;
    if (t < 0.97f) return h < 0.35f ? BIOME_SHRUBLAND : BIOME_FOREST;
    if (h < 0.45f) return BIOME_PLAINS;
    return h < 0.9f ? BIOME_SEASONAL_FOREST : BIOME_RAINFOREST;
}

void initBiomeLookup() {
    static bool done = false;
    if (done) return;
    done = true;
    for (int i = 0; i < 64; ++i)
        for (int j = 0; j < 64; ++j) g_biomeLookup[i + j * 64] = getBiome((float)i / 63.0f, (float)j / 63.0f);
}

inline uint8_t biomeFromLookup(real t, real h) { return g_biomeLookup[(int)(t * 63) + (int)(h * 63) * 64]; }

inline int iabs(int v) { return v < 0 ? -v : v; }

inline bool opaqueGen(uint8_t id) { return id != LEAVES && (g_blocks[id].flags & BF_OPAQUE); }

inline bool seesSky(const World& w, int x, int y, int z) { return y >= w.heightAt(x, z); }

inline int64_t oddLong(JRandom& r) { return r.nextLong() / 2 * 2 + 1; }

// ---------------------------------------------------------------- decorators

bool genTree(World& w, JRandom& r, int x, int y, int z, int height, int meta) {
    bool ok = true;
    if (y < 1 || y + height + 1 > 128) return false;
    for (int yy = y; yy <= y + 1 + height; ++yy) {
        int rad = 1;
        if (yy == y) rad = 0;
        if (yy >= y + 1 + height - 2) rad = 2;
        for (int xx = x - rad; xx <= x + rad && ok; ++xx)
            for (int zz = z - rad; zz <= z + rad && ok; ++zz) {
                if (yy >= 0 && yy < 128) {
                    uint8_t id = w.get(xx, yy, zz);
                    if (id != AIR && id != LEAVES) ok = false;
                } else {
                    ok = false;
                }
            }
    }
    if (!ok) return false;
    uint8_t below = w.get(x, y - 1, z);
    if (!((below == GRASS || below == DIRT) && y < 128 - height - 1)) return false;
    w.setRaw(x, y - 1, z, DIRT);
    for (int yy = y - 3 + height; yy <= y + height; ++yy) {
        int dy = yy - (y + height);
        int rr = 1 - dy / 2;
        for (int xx = x - rr; xx <= x + rr; ++xx) {
            int dx = xx - x;
            for (int zz = z - rr; zz <= z + rr; ++zz) {
                int dz = zz - z;
                if ((iabs(dx) != rr || iabs(dz) != rr || (r.nextInt(2) != 0 && dy != 0)) && !opaqueGen(w.get(xx, yy, zz)))
                    w.setRaw(xx, yy, zz, LEAVES, meta);
            }
        }
    }
    for (int i = 0; i < height; ++i) {
        uint8_t id = w.get(x, y + i, z);
        if (id == AIR || id == LEAVES) w.setRaw(x, y + i, z, LOG, meta);
    }
    return true;
}

bool genOak(World& w, JRandom& r, int x, int y, int z) { return genTree(w, r, x, y, z, r.nextInt(3) + 4, 0); }
bool genBirch(World& w, JRandom& r, int x, int y, int z) { return genTree(w, r, x, y, z, r.nextInt(3) + 5, 2); }

bool spaceCheck(const World& w, int x, int y, int z, int height, int trunk, int rad) {
    for (int yy = y; yy <= y + 1 + height; ++yy) {
        int rr = (yy - y < trunk) ? 0 : rad;
        for (int xx = x - rr; xx <= x + rr; ++xx)
            for (int zz = z - rr; zz <= z + rr; ++zz) {
                if (yy < 0 || yy >= 128) return false;
                uint8_t id = w.get(xx, yy, zz);
                if (id != AIR && id != LEAVES) return false;
            }
    }
    return true;
}

bool genTaiga1(World& w, JRandom& r, int x, int y, int z) {
    int h = r.nextInt(5) + 7;
    int trunk = h - r.nextInt(2) - 3;
    int crown = h - trunk;
    int maxR = 1 + r.nextInt(crown + 1);
    if (y < 1 || y + h + 1 > 128) return false;
    if (!spaceCheck(w, x, y, z, h, trunk, maxR)) return false;
    uint8_t below = w.get(x, y - 1, z);
    if (!((below == GRASS || below == DIRT) && y < 128 - h - 1)) return false;
    w.setRaw(x, y - 1, z, DIRT);
    int rad = 0;
    for (int yy = y + h; yy >= y + trunk; --yy) {
        for (int xx = x - rad; xx <= x + rad; ++xx) {
            int dx = xx - x;
            for (int zz = z - rad; zz <= z + rad; ++zz) {
                int dz = zz - z;
                if ((iabs(dx) != rad || iabs(dz) != rad || rad <= 0) && !opaqueGen(w.get(xx, yy, zz)))
                    w.setRaw(xx, yy, zz, LEAVES, 1);
            }
        }
        if (rad >= 1 && yy == y + trunk + 1)
            --rad;
        else if (rad < maxR)
            ++rad;
    }
    for (int i = 0; i < h - 1; ++i) {
        uint8_t id = w.get(x, y + i, z);
        if (id == AIR || id == LEAVES) w.setRaw(x, y + i, z, LOG, 1);
    }
    return true;
}

bool genTaiga2(World& w, JRandom& r, int x, int y, int z) {
    int h = r.nextInt(4) + 6;
    int trunk = 1 + r.nextInt(2);
    int crown = h - trunk;
    int maxR = 2 + r.nextInt(2);
    if (y < 1 || y + h + 1 > 128) return false;
    if (!spaceCheck(w, x, y, z, h, trunk, maxR)) return false;
    uint8_t below = w.get(x, y - 1, z);
    if (!((below == GRASS || below == DIRT) && y < 128 - h - 1)) return false;
    w.setRaw(x, y - 1, z, DIRT);
    int rad = r.nextInt(2);
    int limit = 1;
    int start = 0;
    for (int i = 0; i <= crown; ++i) {
        int yy = y + h - i;
        for (int xx = x - rad; xx <= x + rad; ++xx) {
            int dx = xx - x;
            for (int zz = z - rad; zz <= z + rad; ++zz) {
                int dz = zz - z;
                if ((iabs(dx) != rad || iabs(dz) != rad || rad <= 0) && !opaqueGen(w.get(xx, yy, zz)))
                    w.setRaw(xx, yy, zz, LEAVES, 1);
            }
        }
        if (rad >= limit) {
            rad = start;
            start = 1;
            ++limit;
            if (limit > maxR) limit = maxR;
        } else {
            ++rad;
        }
    }
    int cut = r.nextInt(3);
    for (int i = 0; i < h - cut; ++i) {
        uint8_t id = w.get(x, y + i, z);
        if (id == AIR || id == LEAVES) w.setRaw(x, y + i, z, LOG, 1);
    }
    return true;
}

void genMinable(World& w, JRandom& r, uint8_t id, int count, int x, int y, int z) {
    float f = r.nextFloat() * mh::PI;
    float fc = (float)count;
    real a = (real)((float)(x + 8) + mh::sin(f) * fc / 8.0f);
    real b = (real)((float)(x + 8) - mh::sin(f) * fc / 8.0f);
    real c = (real)((float)(z + 8) + mh::cos(f) * fc / 8.0f);
    real d = (real)((float)(z + 8) - mh::cos(f) * fc / 8.0f);
    real e = (real)(y + r.nextInt(3) + 2);
    real g = (real)(y + r.nextInt(3) + 2);
    for (int i = 0; i <= count; ++i) {
        real px = a + (b - a) * (real)i / (real)count;
        real py = e + (g - e) * (real)i / (real)count;
        real pz = c + (d - c) * (real)i / (real)count;
        real s = (real)(r.nextDouble() * (double)count / 16.0);
        real sn = (real)(mh::sin((float)i * mh::PI / fc) + 1.0f);
        real rw = sn * s + 1;
        real rh = sn * s + 1;
        int x0 = mh::floorr(px - rw / 2), y0 = mh::floorr(py - rh / 2), z0 = mh::floorr(pz - rw / 2);
        int x1 = mh::floorr(px + rw / 2), y1 = mh::floorr(py + rh / 2), z1 = mh::floorr(pz + rw / 2);
        for (int xx = x0; xx <= x1; ++xx) {
            real dx = ((real)xx + (real)0.5 - px) / (rw / 2);
            if (dx * dx >= 1) continue;
            for (int yy = y0; yy <= y1; ++yy) {
                real dy = ((real)yy + (real)0.5 - py) / (rh / 2);
                if (dx * dx + dy * dy >= 1) continue;
                for (int zz = z0; zz <= z1; ++zz) {
                    real dz = ((real)zz + (real)0.5 - pz) / (rw / 2);
                    if (dx * dx + dy * dy + dz * dz < 1 && w.get(xx, yy, zz) == STONE) w.setRaw(xx, yy, zz, id);
                }
            }
        }
    }
}

void genClay(World& w, JRandom& r, int count, int x, int y, int z) {
    if (!blockIsWater(w.get(x, y, z))) return;
    float f = r.nextFloat() * mh::PI;
    float fc = (float)count;
    real a = (real)((float)(x + 8) + mh::sin(f) * fc / 8.0f);
    real b = (real)((float)(x + 8) - mh::sin(f) * fc / 8.0f);
    real c = (real)((float)(z + 8) + mh::cos(f) * fc / 8.0f);
    real d = (real)((float)(z + 8) - mh::cos(f) * fc / 8.0f);
    real e = (real)(y + r.nextInt(3) + 2);
    real g = (real)(y + r.nextInt(3) + 2);
    for (int i = 0; i <= count; ++i) {
        real px = a + (b - a) * (real)i / (real)count;
        real py = e + (g - e) * (real)i / (real)count;
        real pz = c + (d - c) * (real)i / (real)count;
        real s = (real)(r.nextDouble() * (double)count / 16.0);
        real sn = (real)(mh::sin((float)i * mh::PI / fc) + 1.0f);
        real rw = sn * s + 1;
        real rh = sn * s + 1;
        int x0 = mh::floorr(px - rw / 2), x1 = mh::floorr(px + rw / 2);
        int y0 = mh::floorr(py - rh / 2), y1 = mh::floorr(py + rh / 2);
        int z0 = mh::floorr(pz - rw / 2), z1 = mh::floorr(pz + rw / 2);
        for (int xx = x0; xx <= x1; ++xx)
            for (int yy = y0; yy <= y1; ++yy)
                for (int zz = z0; zz <= z1; ++zz) {
                    real dx = ((real)xx + (real)0.5 - px) / (rw / 2);
                    real dy = ((real)yy + (real)0.5 - py) / (rh / 2);
                    real dz = ((real)zz + (real)0.5 - pz) / (rw / 2);
                    if (dx * dx + dy * dy + dz * dz < 1 && w.get(xx, yy, zz) == SAND) w.setRaw(xx, yy, zz, CLAY);
                }
    }
}

bool genLake(World& w, JRandom& r, uint8_t liquid, int x, int y, int z) {
    x -= 8;
    z -= 8;
    while (y > 0 && w.get(x, y, z) == AIR) --y;
    y -= 4;
    static bool mask[2048];
    std::memset(mask, 0, sizeof(mask));
    int n = r.nextInt(4) + 4;
    for (int i = 0; i < n; ++i) {
        real a = (real)(r.nextDouble() * 6.0 + 3.0);
        real b = (real)(r.nextDouble() * 4.0 + 2.0);
        real c = (real)(r.nextDouble() * 6.0 + 3.0);
        real px = (real)(r.nextDouble() * (16.0 - a - 2.0) + 1.0 + a / 2.0);
        real py = (real)(r.nextDouble() * (8.0 - b - 4.0) + 2.0 + b / 2.0);
        real pz = (real)(r.nextDouble() * (16.0 - c - 2.0) + 1.0 + c / 2.0);
        for (int xx = 1; xx < 15; ++xx)
            for (int zz = 1; zz < 15; ++zz)
                for (int yy = 1; yy < 7; ++yy) {
                    real dx = ((real)xx - px) / (a / 2);
                    real dy = ((real)yy - py) / (b / 2);
                    real dz = ((real)zz - pz) / (c / 2);
                    if (dx * dx + dy * dy + dz * dz < 1) mask[(xx * 16 + zz) * 8 + yy] = true;
                }
    }
    auto at = [&](int xx, int zz, int yy) { return mask[(xx * 16 + zz) * 8 + yy]; };
    auto border = [&](int xx, int zz, int yy) {
        return !at(xx, zz, yy) && ((xx < 15 && at(xx + 1, zz, yy)) || (xx > 0 && at(xx - 1, zz, yy)) ||
                                   (zz < 15 && at(xx, zz + 1, yy)) || (zz > 0 && at(xx, zz - 1, yy)) ||
                                   (yy < 7 && at(xx, zz, yy + 1)) || (yy > 0 && at(xx, zz, yy - 1)));
    };
    for (int xx = 0; xx < 16; ++xx)
        for (int zz = 0; zz < 16; ++zz)
            for (int yy = 0; yy < 8; ++yy) {
                if (!border(xx, zz, yy)) continue;
                uint8_t id = w.get(x + xx, y + yy, z + zz);
                if (yy >= 4 && blockIsLiquid(id)) return false;
                if (yy < 4 && !blockMatSolid(id) && id != liquid) return false;
            }
    for (int xx = 0; xx < 16; ++xx)
        for (int zz = 0; zz < 16; ++zz)
            for (int yy = 0; yy < 8; ++yy)
                if (at(xx, zz, yy)) w.setRaw(x + xx, y + yy, z + zz, yy >= 4 ? AIR : liquid);
    for (int xx = 0; xx < 16; ++xx)
        for (int zz = 0; zz < 16; ++zz)
            for (int yy = 4; yy < 8; ++yy)
                if (at(xx, zz, yy) && w.get(x + xx, y + yy - 1, z + zz) == DIRT && seesSky(w, x + xx, y + yy, z + zz))
                    w.setRaw(x + xx, y + yy - 1, z + zz, GRASS);
    if (blockIsLava(liquid)) {
        for (int xx = 0; xx < 16; ++xx)
            for (int zz = 0; zz < 16; ++zz)
                for (int yy = 0; yy < 8; ++yy) {
                    if (!border(xx, zz, yy)) continue;
                    if ((yy < 4 || r.nextInt(2) != 0) && blockMatSolid(w.get(x + xx, y + yy, z + zz)))
                        w.setRaw(x + xx, y + yy, z + zz, STONE);
                }
    }
    return true;
}

bool plantCanStay(const World& w, uint8_t id, int x, int y, int z) {
    uint8_t below = w.get(x, y - 1, z);
    switch (id) {
        case MUSHROOM_BROWN:
        case MUSHROOM_RED:
            return y >= 0 && y < 128 && !seesSky(w, x, y, z) && opaqueGen(below);
        case DEAD_BUSH:
            return seesSky(w, x, y, z) && below == SAND;
        default:
            return seesSky(w, x, y, z) && (below == GRASS || below == DIRT || below == 60);
    }
}

inline int spread(JRandom& r, int bound) {
    int a = r.nextInt(bound);
    int b = r.nextInt(bound);
    return a - b;
}

// WorldGenLiquids: a source in a stone wall with exactly one open side. Beta flows it out
// immediately; here the first update is queued and the game runs the flow.
void genSpring(World& w, uint8_t id, int x, int y, int z) {
    if (w.get(x, y + 1, z) != STONE || w.get(x, y - 1, z) != STONE) return;
    if (w.get(x, y, z) != AIR && w.get(x, y, z) != STONE) return;
    int stone = (w.get(x - 1, y, z) == STONE) + (w.get(x + 1, y, z) == STONE) + (w.get(x, y, z - 1) == STONE) +
                (w.get(x, y, z + 1) == STONE);
    int air = (w.get(x - 1, y, z) == AIR) + (w.get(x + 1, y, z) == AIR) + (w.get(x, y, z - 1) == AIR) +
              (w.get(x, y, z + 1) == AIR);
    if (stone == 3 && air == 1) {
        w.setRaw(x, y, z, id);
        w.scheduleTick(x, y, z, id, 0);
    }
}

// WorldGenDungeons: a cobblestone room with up to two loot chests around a spawner.
bool genDungeon(World& w, JRandom& r, int x, int y, int z) {
    const int h = 3;
    int rx = r.nextInt(2) + 2, rz = r.nextInt(2) + 2;
    int openings = 0;
    for (int xx = x - rx - 1; xx <= x + rx + 1; ++xx)
        for (int yy = y - 1; yy <= y + h + 1; ++yy)
            for (int zz = z - rz - 1; zz <= z + rz + 1; ++zz) {
                bool solid = blockMatSolid(w.get(xx, yy, zz));
                if ((yy == y - 1 || yy == y + h + 1) && !solid) return false;
                bool wall = xx == x - rx - 1 || xx == x + rx + 1 || zz == z - rz - 1 || zz == z + rz + 1;
                if (wall && yy == y && w.get(xx, yy, zz) == AIR && w.get(xx, yy + 1, zz) == AIR) ++openings;
            }
    if (openings < 1 || openings > 5) return false;
    for (int xx = x - rx - 1; xx <= x + rx + 1; ++xx)
        for (int yy = y + h; yy >= y - 1; --yy)
            for (int zz = z - rz - 1; zz <= z + rz + 1; ++zz) {
                bool inside = xx != x - rx - 1 && yy != y - 1 && zz != z - rz - 1 && xx != x + rx + 1 &&
                              yy != y + h + 1 && zz != z + rz + 1;
                if (inside) {
                    w.setRaw(xx, yy, zz, AIR);
                } else if (yy >= 0 && !blockMatSolid(w.get(xx, yy - 1, zz))) {
                    w.setRaw(xx, yy, zz, AIR);
                } else if (blockMatSolid(w.get(xx, yy, zz))) {
                    w.setRaw(xx, yy, zz, yy == y - 1 && r.nextInt(4) != 0 ? MOSSY_COBBLESTONE : COBBLESTONE);
                }
            }
    auto loot = [&]() -> ItemStack {
        int k = r.nextInt(11);
        switch (k) {
            case 0: return ItemStack(I_SADDLE);
            case 1: return ItemStack(I_IRON_INGOT, r.nextInt(4) + 1);
            case 2: return ItemStack(I_BREAD);
            case 3: return ItemStack(I_WHEAT, r.nextInt(4) + 1);
            case 4: return ItemStack(I_GUNPOWDER, r.nextInt(4) + 1);
            case 5: return ItemStack(I_STRING, r.nextInt(4) + 1);
            case 6: return ItemStack(I_BUCKET);
            case 7: return r.nextInt(100) == 0 ? ItemStack(I_GOLDEN_APPLE) : ItemStack();
            case 8: return r.nextInt(2) == 0 ? ItemStack(I_REDSTONE, r.nextInt(4) + 1) : ItemStack();
            case 9: return r.nextInt(10) == 0 ? ItemStack(I_RECORD_13 + r.nextInt(2)) : ItemStack();
            default: return ItemStack(I_DYE, 1, 3);
        }
    };
    for (int c = 0; c < 2; ++c)
        for (int attempt = 0; attempt < 3; ++attempt) {
            int cx = x + r.nextInt(rx * 2 + 1) - rx;
            int cz = z + r.nextInt(rz * 2 + 1) - rz;
            if (w.get(cx, y, cz) != AIR) continue;
            int walls = blockMatSolid(w.get(cx - 1, y, cz)) + blockMatSolid(w.get(cx + 1, y, cz)) +
                        blockMatSolid(w.get(cx, y, cz - 1)) + blockMatSolid(w.get(cx, y, cz + 1));
            if (walls != 1) continue;
            w.setRaw(cx, y, cz, CHEST);
            TileEntity* t = w.createTile(cx, y, cz, TILE_CHEST);
            for (int i = 0; i < 8; ++i) {
                ItemStack s = loot();
                if (!s.empty()) {
                    int slot = r.nextInt(27);
                    if (t) t->items[slot] = s;
                }
            }
            break;
        }
    w.setRaw(x, y, z, MOB_SPAWNER);
    static const uint8_t MOBS[4] = {ENT_SKELETON, ENT_ZOMBIE, ENT_ZOMBIE, ENT_SPIDER};
    uint8_t type = MOBS[r.nextInt(4)];
    if (TileEntity* t = w.createTile(x, y, z, TILE_SPAWNER)) {
        t->burnTime = 20;
        t->burnTotal = type;
    }
    return true;
}

void genFlowers(World& w, JRandom& r, uint8_t id, int x, int y, int z) {
    for (int i = 0; i < 64; ++i) {
        int xx = x + spread(r, 8);
        int yy = y + spread(r, 4);
        int zz = z + spread(r, 8);
        if (w.get(xx, yy, zz) == AIR && plantCanStay(w, id, xx, yy, zz)) w.setRaw(xx, yy, zz, id);
    }
}

void genTallGrass(World& w, JRandom& r, int meta, int x, int y, int z) {
    uint8_t id;
    while (((id = w.get(x, y, z)) == AIR || id == LEAVES) && y > 0) --y;
    for (int i = 0; i < 128; ++i) {
        int xx = x + spread(r, 8);
        int yy = y + spread(r, 4);
        int zz = z + spread(r, 8);
        if (w.get(xx, yy, zz) == AIR && plantCanStay(w, TALL_GRASS, xx, yy, zz)) w.setRaw(xx, yy, zz, TALL_GRASS, meta);
    }
}

void genDeadBush(World& w, JRandom& r, int x, int y, int z) {
    uint8_t id;
    while (((id = w.get(x, y, z)) == AIR || id == LEAVES) && y > 0) --y;
    for (int i = 0; i < 4; ++i) {
        int xx = x + spread(r, 8);
        int yy = y + spread(r, 4);
        int zz = z + spread(r, 8);
        if (w.get(xx, yy, zz) == AIR && plantCanStay(w, DEAD_BUSH, xx, yy, zz)) w.setRaw(xx, yy, zz, DEAD_BUSH);
    }
}

bool waterAround(const World& w, int x, int y, int z) {
    return blockIsWater(w.get(x - 1, y, z)) || blockIsWater(w.get(x + 1, y, z)) || blockIsWater(w.get(x, y, z - 1)) ||
           blockIsWater(w.get(x, y, z + 1));
}

bool reedCanStay(const World& w, int x, int y, int z) {
    uint8_t below = w.get(x, y - 1, z);
    if (below == REED) return true;
    if (below != GRASS && below != DIRT && below != SAND) return false;
    return waterAround(w, x, y - 1, z);
}

void genReeds(World& w, JRandom& r, int x, int y, int z) {
    for (int i = 0; i < 20; ++i) {
        int xx = x + spread(r, 4);
        int zz = z + spread(r, 4);
        if (w.get(xx, y, zz) == AIR && waterAround(w, xx, y - 1, zz)) {
            int h = 2 + r.nextInt(r.nextInt(3) + 1);
            for (int k = 0; k < h; ++k)
                if (reedCanStay(w, xx, y + k, zz)) w.setRaw(xx, y + k, zz, REED);
        }
    }
}

bool cactusCanStay(const World& w, int x, int y, int z) {
    if (blockMatSolid(w.get(x - 1, y, z)) || blockMatSolid(w.get(x + 1, y, z)) || blockMatSolid(w.get(x, y, z - 1)) ||
        blockMatSolid(w.get(x, y, z + 1)))
        return false;
    uint8_t below = w.get(x, y - 1, z);
    return below == CACTUS || below == SAND;
}

void genCactus(World& w, JRandom& r, int x, int y, int z) {
    for (int i = 0; i < 10; ++i) {
        int xx = x + spread(r, 8);
        int yy = y + spread(r, 4);
        int zz = z + spread(r, 8);
        if (w.get(xx, yy, zz) == AIR) {
            int h = 1 + r.nextInt(r.nextInt(3) + 1);
            for (int k = 0; k < h; ++k)
                if (cactusCanStay(w, xx, yy + k, zz)) w.setRaw(xx, yy + k, zz, CACTUS);
        }
    }
}

void genPumpkins(World& w, JRandom& r, int x, int y, int z) {
    for (int i = 0; i < 64; ++i) {
        int xx = x + spread(r, 8);
        int yy = y + spread(r, 4);
        int zz = z + spread(r, 8);
        if (w.get(xx, yy, zz) == AIR && w.get(xx, yy - 1, zz) == GRASS) w.setRaw(xx, yy, zz, PUMPKIN, r.nextInt(4));
    }
}

// ---------------------------------------------------------------- caves

// MapGenCaves; with `hell` set, MapGenCavesHell (flatter, wider tunnels through netherrack).
class Caves {
public:
    explicit Caves(bool hell = false) : hell_(hell) {}

    void generate(int64_t seed, int cx, int cz, uint8_t* buf) {
        rand_.setSeed(seed);
        int64_t a = oddLong(rand_);
        int64_t b = oddLong(rand_);
        for (int x = cx - RANGE; x <= cx + RANGE; ++x)
            for (int z = cz - RANGE; z <= cz + RANGE; ++z) {
                rand_.setSeed(jadd(jmul(x, a), jmul(z, b)) ^ seed);
                recursive(x, z, cx, cz, buf);
            }
    }

private:
    static constexpr int RANGE = 8;
    JRandom rand_;
    bool hell_;

    void recursive(int x, int z, int cx, int cz, uint8_t* buf) {
        int n = rand_.nextInt(rand_.nextInt(rand_.nextInt(hell_ ? 10 : 40) + 1) + 1);
        if (rand_.nextInt(hell_ ? 5 : 15) != 0) n = 0;
        for (int i = 0; i < n; ++i) {
            real px = (real)(x * 16 + rand_.nextInt(16));
            real py = (real)(hell_ ? rand_.nextInt(128) : rand_.nextInt(rand_.nextInt(120) + 8));
            real pz = (real)(z * 16 + rand_.nextInt(16));
            if (hell_) {
                int tunnels = 1;
                if (rand_.nextInt(4) == 0) {
                    float radius = 1.0f + rand_.nextFloat() * 6.0f;
                    node(cx, cz, buf, px, py, pz, radius, 0.0f, 0.0f, -1, -1, (real)0.5);
                    tunnels += rand_.nextInt(4);
                }
                for (int j = 0; j < tunnels; ++j) {
                    float yaw = rand_.nextFloat() * mh::PI * 2.0f;
                    float pitch = (rand_.nextFloat() - 0.5f) * 2.0f / 8.0f;
                    float r1 = rand_.nextFloat() * 2.0f;
                    float r2 = rand_.nextFloat();
                    node(cx, cz, buf, px, py, pz, (r1 + r2) * 2.0f, yaw, pitch, 0, 0, (real)0.5);
                }
                continue;
            }
            int tunnels = 1;
            if (rand_.nextInt(4) == 0) {
                float radius = 1.0f + rand_.nextFloat() * 6.0f;
                node(cx, cz, buf, px, py, pz, radius, 0.0f, 0.0f, -1, -1, (real)0.5);
                tunnels += rand_.nextInt(4);
            }
            for (int j = 0; j < tunnels; ++j) {
                float yaw = rand_.nextFloat() * mh::PI * 2.0f;
                float pitch = (rand_.nextFloat() - 0.5f) * 2.0f / 8.0f;
                float r1 = rand_.nextFloat() * 2.0f;
                float r2 = rand_.nextFloat();
                node(cx, cz, buf, px, py, pz, r1 + r2, yaw, pitch, 0, 0, (real)1.0);
            }
        }
    }

    void node(int cx, int cz, uint8_t* buf, real x, real y, real z, float radius, float yaw, float pitch, int step,
              int maxSteps, real yScale) {
        real ccx = (real)(cx * 16 + 8);
        real ccz = (real)(cz * 16 + 8);
        float dYaw = 0.0f, dPitch = 0.0f;
        JRandom r(rand_.nextLong());
        if (maxSteps <= 0) {
            int m = RANGE * 16 - 16;
            maxSteps = m - r.nextInt(m / 4);
        }
        bool room = false;
        if (step == -1) {
            step = maxSteps / 2;
            room = true;
        }
        int split = r.nextInt(maxSteps / 2) + maxSteps / 4;
        bool steep = r.nextInt(6) == 0;
        for (; step < maxSteps; ++step) {
            real rad = (real)1.5 + (real)(mh::sin((float)step * mh::PI / (float)maxSteps) * radius * 1.0f);
            real radY = rad * yScale;
            float cp = mh::cos(pitch);
            float sp = mh::sin(pitch);
            x += (real)(mh::cos(yaw) * cp);
            y += (real)sp;
            z += (real)(mh::sin(yaw) * cp);
            if (steep)
                pitch *= 0.92f;
            else
                pitch *= 0.7f;
            pitch += dPitch * 0.1f;
            yaw += dYaw * 0.1f;
            dPitch *= 0.9f;
            dYaw *= 0.75f;
            {
                float a = r.nextFloat();
                float b = r.nextFloat();
                float c = r.nextFloat();
                dPitch += (a - b) * c * 2.0f;
            }
            {
                float a = r.nextFloat();
                float b = r.nextFloat();
                float c = r.nextFloat();
                dYaw += (a - b) * c * 4.0f;
            }
            if (!room && step == split && radius > 1.0f) {
                float r1 = r.nextFloat() * 0.5f + 0.5f;
                node(cx, cz, buf, x, y, z, r1, yaw - 1.5707964f, pitch / 3.0f, step, maxSteps, (real)1.0);
                float r2 = r.nextFloat() * 0.5f + 0.5f;
                node(cx, cz, buf, x, y, z, r2, yaw + 1.5707964f, pitch / 3.0f, step, maxSteps, (real)1.0);
                return;
            }
            if (!room && r.nextInt(4) == 0) continue;

            real ddx = x - ccx, ddz = z - ccz;
            real left = (real)(maxSteps - step);
            real lim = (real)(radius + 2.0f + 16.0f);
            if (ddx * ddx + ddz * ddz - left * left > lim * lim) return;
            if (!(x >= ccx - 16 - rad * 2 && z >= ccz - 16 - rad * 2 && x <= ccx + 16 + rad * 2 && z <= ccz + 16 + rad * 2))
                continue;

            int x0 = mh::floorr(x - rad) - cx * 16 - 1;
            int x1 = mh::floorr(x + rad) - cx * 16 + 1;
            int y0 = mh::floorr(y - radY) - 1;
            int y1 = mh::floorr(y + radY) + 1;
            int z0 = mh::floorr(z - rad) - cz * 16 - 1;
            int z1 = mh::floorr(z + rad) - cz * 16 + 1;
            if (x0 < 0) x0 = 0;
            if (x1 > 16) x1 = 16;
            if (y0 < 1) y0 = 1;
            if (y1 > 120) y1 = 120;
            if (z0 < 0) z0 = 0;
            if (z1 > 16) z1 = 16;

            bool water = false;
            for (int xx = x0; !water && xx < x1; ++xx)
                for (int zz = z0; !water && zz < z1; ++zz)
                    for (int yy = y1 + 1; !water && yy >= y0 - 1; --yy) {
                        int i = (xx * 16 + zz) * 128 + yy;
                        if (yy >= 0 && yy < 128) {
                            if (hell_ ? (buf[i] == LAVA_FLOWING || buf[i] == LAVA)
                                      : (buf[i] == WATER_FLOWING || buf[i] == WATER))
                                water = true;
                            if (yy != y0 - 1 && xx != x0 && xx != x1 - 1 && zz != z0 && zz != z1 - 1) yy = y0;
                        }
                    }
            if (water) continue;

            for (int xx = x0; xx < x1; ++xx) {
                real fx = ((real)(xx + cx * 16) + (real)0.5 - x) / rad;
                for (int zz = z0; zz < z1; ++zz) {
                    real fz = ((real)(zz + cz * 16) + (real)0.5 - z) / rad;
                    int i = (xx * 16 + zz) * 128 + y1;
                    bool grass = false;
                    if (fx * fx + fz * fz < 1) {
                        for (int yy = y1 - 1; yy >= y0; --yy) {
                            real fy = ((real)yy + (real)0.5 - y) / radY;
                            if (fy > (real)-0.7 && fx * fx + fy * fy + fz * fz < 1) {
                                uint8_t id = buf[i];
                                if (hell_) {
                                    if (id == NETHERRACK || id == DIRT || id == GRASS) buf[i] = AIR;
                                    --i;
                                    continue;
                                }
                                if (id == GRASS) grass = true;
                                if (id == STONE || id == DIRT || id == GRASS) {
                                    if (yy < 10) {
                                        buf[i] = LAVA_FLOWING;
                                    } else {
                                        buf[i] = AIR;
                                        if (grass && buf[i - 1] == DIRT) buf[i - 1] = GRASS;
                                    }
                                }
                            }
                            --i;
                        }
                    }
                }
            }
            if (room) break;
        }
    }
};

// ---------------------------------------------------------------- chunk provider

class ChunkGenerator {
public:
    explicit ChunkGenerator(int64_t seed) : seed_(seed), rand_(seed) {
        k_.init(rand_, 16);
        l_.init(rand_, 16);
        m_.init(rand_, 8);
        n_.init(rand_, 4);
        o_.init(rand_, 4);
        a_.init(rand_, 10);
        b_.init(rand_, 16);
        mob_.init(rand_, 8);
    }

    void provide(int cx, int cz, uint8_t* buf, BiomeSource& bs) {
        rand_.setSeed(jadd(jmul(cx, 341873128712LL), jmul(cz, 132897987541LL)));
        std::memset(buf, 0, 32768);
        bs.load(cx * 16, cz * 16, 16, 16);
        generateTerrain(cx, cz, buf, bs);
        replaceBlocks(cx, cz, buf, bs.biomes);
        caves_.generate(seed_, cx, cz, buf);
    }

    void populate(World& w, BiomeSource& bs, int cx, int cz);

private:
    int64_t seed_;
    JRandom rand_;
    PerlinOctaves k_, l_, m_, n_, o_, a_, b_, mob_;
    real density_[5 * 17 * 5];
    real nd_[5 * 17 * 5], ne_[5 * 17 * 5], nf_[5 * 17 * 5], ng_[25], nh_[25];
    real sand_[256], gravel_[256], stone_[256];
    real temps_[256];
    Caves caves_;

    void density(int x, int y, int z, const BiomeSource& bs);
    void generateTerrain(int cx, int cz, uint8_t* buf, const BiomeSource& bs);
    void replaceBlocks(int cx, int cz, uint8_t* buf, const uint8_t* biomes);
    bool growTree(World& w, int biome, int x, int y, int z);
};

void ChunkGenerator::density(int x, int y, int z, const BiomeSource& bs) {
    const int NX = 5, NY = 17, NZ = 5;
    const real S = (real)684.412;
    a_.generate2(ng_, x, z, NX, NZ, (real)1.121, (real)1.121);
    b_.generate2(nh_, x, z, NX, NZ, (real)200.0, (real)200.0);
    m_.generate(nd_, (real)x, (real)y, (real)z, NX, NY, NZ, S / 80, S / 160, S / 80);
    k_.generate(ne_, (real)x, (real)y, (real)z, NX, NY, NZ, S, S, S);
    l_.generate(nf_, (real)x, (real)y, (real)z, NX, NY, NZ, S, S, S);
    int i3 = 0, i2 = 0;
    const int step = 16 / NX;
    for (int i = 0; i < NX; ++i) {
        int bx = i * step + step / 2;
        for (int j = 0; j < NZ; ++j) {
            int bz = j * step + step / 2;
            real t = bs.temperature[bx * 16 + bz];
            real hm = bs.humidity[bx * 16 + bz] * t;
            real f = 1 - hm;
            f *= f;
            f *= f;
            f = 1 - f;
            real scale = (ng_[i2] + 256) / 512;
            scale *= f;
            if (scale > 1) scale = 1;
            real depth = nh_[i2] / 8000;
            if (depth < 0) depth = -depth * (real)0.3;
            depth = depth * 3 - 2;
            if (depth < 0) {
                depth /= 2;
                if (depth < -1) depth = -1;
                depth /= (real)1.4;
                depth /= 2;
                scale = 0;
            } else {
                if (depth > 1) depth = 1;
                depth /= 8;
            }
            if (scale < 0) scale = 0;
            scale += (real)0.5;
            depth = depth * (real)NY / 16;
            real center = (real)NY / 2 + depth * 4;
            ++i2;
            for (int k = 0; k < NY; ++k) {
                real off = ((real)k - center) * 12 / scale;
                if (off < 0) off *= 4;
                real lo = ne_[i3] / 512;
                real hi = nf_[i3] / 512;
                real sel = (nd_[i3] / 10 + 1) / 2;
                real v;
                if (sel < 0)
                    v = lo;
                else if (sel > 1)
                    v = hi;
                else
                    v = lo + (hi - lo) * sel;
                v -= off;
                if (k > NY - 4) {
                    real top = (real)((float)(k - (NY - 4)) / 3.0f);
                    v = v * (1 - top) + (real)-10.0 * top;
                }
                density_[i3] = v;
                ++i3;
            }
        }
    }
}

void ChunkGenerator::generateTerrain(int cx, int cz, uint8_t* buf, const BiomeSource& bs) {
    const int CELL = 4, SEA = 64, NY = 17, NZ = CELL + 1;
    density(cx * CELL, 0, cz * CELL, bs);
    const real* D = density_;
    for (int i = 0; i < CELL; ++i) {
        for (int j = 0; j < CELL; ++j) {
            for (int k = 0; k < 16; ++k) {
                const real dy = (real)0.125;
                real a = D[((i + 0) * NZ + j + 0) * NY + k];
                real b = D[((i + 0) * NZ + j + 1) * NY + k];
                real c = D[((i + 1) * NZ + j + 0) * NY + k];
                real d = D[((i + 1) * NZ + j + 1) * NY + k];
                real da = (D[((i + 0) * NZ + j + 0) * NY + k + 1] - a) * dy;
                real db = (D[((i + 0) * NZ + j + 1) * NY + k + 1] - b) * dy;
                real dc = (D[((i + 1) * NZ + j + 0) * NY + k + 1] - c) * dy;
                real dd = (D[((i + 1) * NZ + j + 1) * NY + k + 1] - d) * dy;
                for (int y8 = 0; y8 < 8; ++y8) {
                    const real q = (real)0.25;
                    real v0 = a, v1 = b;
                    real s0 = (c - a) * q, s1 = (d - b) * q;
                    for (int x4 = 0; x4 < 4; ++x4) {
                        int idx = ((x4 + i * 4) << 11) | ((j * 4) << 7) | (k * 8 + y8);
                        real v = v0;
                        real sv = (v1 - v0) * q;
                        for (int z4 = 0; z4 < 4; ++z4) {
                            real t = bs.temperature[(i * 4 + x4) * 16 + j * 4 + z4];
                            int y = k * 8 + y8;
                            uint8_t id = AIR;
                            if (y < SEA) id = (t < (real)0.5 && y >= SEA - 1) ? ICE : WATER;
                            if (v > 0) id = STONE;
                            buf[idx] = id;
                            idx += 128;
                            v += sv;
                        }
                        v0 += s0;
                        v1 += s1;
                    }
                    a += da;
                    b += db;
                    c += dc;
                    d += dd;
                }
            }
        }
    }
}

void ChunkGenerator::replaceBlocks(int cx, int cz, uint8_t* buf, const uint8_t* biomes) {
    const int SEA = 64;
    const real s = (real)0.03125;
    n_.generate(sand_, (real)(cx * 16), (real)(cz * 16), 0, 16, 16, 1, s, s, 1);
    n_.generate(gravel_, (real)(cx * 16), (real)109.0134, (real)(cz * 16), 16, 1, 16, s, 1, s);
    o_.generate(stone_, (real)(cx * 16), (real)(cz * 16), 0, 16, 16, 1, s * 2, s * 2, s * 2);
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            const BiomeInfo& biome = BIOMES[biomes[z + x * 16]];
            bool sandy = sand_[z + x * 16] + (real)(rand_.nextDouble() * 0.2) > 0;
            bool gravelly = gravel_[z + x * 16] + (real)(rand_.nextDouble() * 0.2) > 3;
            int depth = (int)(stone_[z + x * 16] / 3 + 3 + (real)(rand_.nextDouble() * 0.25));
            int run = -1;
            uint8_t top = biome.top, fill = biome.filler;
            for (int y = 127; y >= 0; --y) {
                int idx = (x * 16 + z) * 128 + y;
                if (y <= 0 + rand_.nextInt(5)) {
                    buf[idx] = BEDROCK;
                    continue;
                }
                uint8_t b = buf[idx];
                if (b == AIR) {
                    run = -1;
                } else if (b == STONE) {
                    if (run == -1) {
                        if (depth <= 0) {
                            top = AIR;
                            fill = STONE;
                        } else if (y >= SEA - 4 && y <= SEA + 1) {
                            top = biome.top;
                            fill = biome.filler;
                            if (gravelly) top = AIR;
                            if (gravelly) fill = GRAVEL;
                            if (sandy) top = SAND;
                            if (sandy) fill = SAND;
                        }
                        if (y < SEA && top == AIR) top = WATER;
                        run = depth;
                        buf[idx] = y >= SEA - 1 ? top : fill;
                    } else if (run > 0) {
                        --run;
                        buf[idx] = fill;
                        if (run == 0 && fill == SAND) {
                            run = rand_.nextInt(4);
                            fill = SANDSTONE;
                        }
                    }
                }
            }
        }
    }
}

bool ChunkGenerator::growTree(World& w, int biome, int x, int y, int z) {
    JRandom& r = rand_;
    switch (biome) {
        case BIOME_FOREST:
            if (r.nextInt(5) == 0) return genBirch(w, r, x, y, z);
            r.nextInt(3);
            return genOak(w, r, x, y, z);
        case BIOME_RAINFOREST:
            r.nextInt(3);
            return genOak(w, r, x, y, z);
        case BIOME_TAIGA:
            if (r.nextInt(3) == 0) return genTaiga1(w, r, x, y, z);
            return genTaiga2(w, r, x, y, z);
        default:
            r.nextInt(10);
            return genOak(w, r, x, y, z);
    }
}

void ChunkGenerator::populate(World& w, BiomeSource& bs, int cx, int cz) {
    JRandom& r = rand_;
    int x0 = cx * 16, z0 = cz * 16;
    int biome = bs.biomeAt(x0 + 16, z0 + 16);
    r.setSeed(seed_);
    int64_t a = oddLong(r);
    int64_t b = oddLong(r);
    r.setSeed(jadd(jmul(cx, a), jmul(cz, b)) ^ seed_);

    auto px = [&]() { return x0 + r.nextInt(16) + 8; };
    auto pz = [&]() { return z0 + r.nextInt(16) + 8; };

    if (r.nextInt(4) == 0) {
        int x = px();
        int y = r.nextInt(128);
        int z = pz();
        genLake(w, r, WATER, x, y, z);
    }
    if (r.nextInt(8) == 0) {
        int x = px();
        int y = r.nextInt(r.nextInt(120) + 8);
        int z = pz();
        if (y < 64 || r.nextInt(10) == 0) genLake(w, r, LAVA, x, y, z);
    }
    for (int i = 0; i < 8; ++i) {
        int x = px();
        int y = r.nextInt(128);
        int z = pz();
        genDungeon(w, r, x, y, z);
    }
    for (int i = 0; i < 10; ++i) {
        int x = x0 + r.nextInt(16);
        int y = r.nextInt(128);
        int z = z0 + r.nextInt(16);
        genClay(w, r, 32, x, y, z);
    }
    struct Ore {
        uint8_t id;
        int size, tries, range;
    };
    static const Ore ORES[] = {{DIRT, 32, 20, 128},   {GRAVEL, 32, 10, 128},    {COAL_ORE, 16, 20, 128},
                               {IRON_ORE, 8, 20, 64}, {GOLD_ORE, 8, 2, 32},     {REDSTONE_ORE, 7, 8, 16},
                               {DIAMOND_ORE, 7, 1, 16}};
    for (const Ore& ore : ORES) {
        for (int i = 0; i < ore.tries; ++i) {
            int x = x0 + r.nextInt(16);
            int y = r.nextInt(ore.range);
            int z = z0 + r.nextInt(16);
            genMinable(w, r, ore.id, ore.size, x, y, z);
        }
    }
    {
        int x = x0 + r.nextInt(16);
        int y1 = r.nextInt(16);
        int y = y1 + r.nextInt(16);
        int z = z0 + r.nextInt(16);
        genMinable(w, r, LAPIS_ORE, 6, x, y, z);
    }

    int treeBase = (int)((mob_.noise2((real)x0 * (real)0.5, (real)z0 * (real)0.5) / 8 +
                          (real)(r.nextDouble() * 4.0) + 4) /
                         3);
    int trees = 0;
    if (r.nextInt(10) == 0) ++trees;
    switch (biome) {
        case BIOME_FOREST: trees += treeBase + 5; break;
        case BIOME_RAINFOREST: trees += treeBase + 5; break;
        case BIOME_SEASONAL_FOREST: trees += treeBase + 2; break;
        case BIOME_TAIGA: trees += treeBase + 5; break;
        case BIOME_DESERT:
        case BIOME_TUNDRA:
        case BIOME_PLAINS: trees -= 20; break;
        default: break;
    }
    for (int i = 0; i < trees; ++i) {
        int x = px();
        int z = pz();
        growTree(w, biome, x, w.heightAt(x, z), z);
    }

    int yellow = biome == BIOME_FOREST ? 2 : biome == BIOME_SEASONAL_FOREST ? 4 : biome == BIOME_TAIGA ? 2
                                                                            : biome == BIOME_PLAINS ? 3 : 0;
    for (int i = 0; i < yellow; ++i) {
        int x = px();
        int y = r.nextInt(128);
        int z = pz();
        genFlowers(w, r, FLOWER_YELLOW, x, y, z);
    }
    int grass = biome == BIOME_FOREST ? 2 : biome == BIOME_RAINFOREST ? 10 : biome == BIOME_SEASONAL_FOREST ? 2
                                          : biome == BIOME_TAIGA ? 1 : biome == BIOME_PLAINS ? 10 : 0;
    for (int i = 0; i < grass; ++i) {
        int meta = 1;
        if (biome == BIOME_RAINFOREST && r.nextInt(3) != 0) meta = 2;
        int x = px();
        int y = r.nextInt(128);
        int z = pz();
        genTallGrass(w, r, meta, x, y, z);
    }
    int bushes = biome == BIOME_DESERT ? 2 : 0;
    for (int i = 0; i < bushes; ++i) {
        int x = px();
        int y = r.nextInt(128);
        int z = pz();
        genDeadBush(w, r, x, y, z);
    }
    if (r.nextInt(2) == 0) {
        int x = px();
        int y = r.nextInt(128);
        int z = pz();
        genFlowers(w, r, FLOWER_RED, x, y, z);
    }
    if (r.nextInt(4) == 0) {
        int x = px();
        int y = r.nextInt(128);
        int z = pz();
        genFlowers(w, r, MUSHROOM_BROWN, x, y, z);
    }
    if (r.nextInt(8) == 0) {
        int x = px();
        int y = r.nextInt(128);
        int z = pz();
        genFlowers(w, r, MUSHROOM_RED, x, y, z);
    }
    for (int i = 0; i < 10; ++i) {
        int x = px();
        int y = r.nextInt(128);
        int z = pz();
        genReeds(w, r, x, y, z);
    }
    if (r.nextInt(32) == 0) {
        int x = px();
        int y = r.nextInt(128);
        int z = pz();
        genPumpkins(w, r, x, y, z);
    }
    int cacti = biome == BIOME_DESERT ? 10 : 0;
    for (int i = 0; i < cacti; ++i) {
        int x = px();
        int y = r.nextInt(128);
        int z = pz();
        genCactus(w, r, x, y, z);
    }
    for (int i = 0; i < 50; ++i) {
        int x = px();
        int y = r.nextInt(r.nextInt(120) + 8);
        int z = pz();
        genSpring(w, WATER_FLOWING, x, y, z);
    }
    for (int i = 0; i < 20; ++i) {
        int x = px();
        int y = r.nextInt(r.nextInt(r.nextInt(112) + 8) + 8);
        int z = pz();
        genSpring(w, LAVA_FLOWING, x, y, z);
    }

    bs.temperatures(temps_, x0 + 8, z0 + 8, 16, 16);
    for (int x = x0 + 8; x < x0 + 24; ++x) {
        for (int z = z0 + 8; z < z0 + 24; ++z) {
            int tx = x - (x0 + 8), tz = z - (z0 + 8);
            int y = w.topSolidAt(x, z);
            real t = temps_[tx * 16 + tz] - (real)(y - 64) / 64 * (real)0.3;
            if (t < (real)0.5 && y > 0 && y < 128 && w.get(x, y, z) == AIR) {
                uint8_t below = w.get(x, y - 1, z);
                if (blockMatSolid(below) && below != ICE) w.setRaw(x, y, z, SNOW_LAYER);
            }
        }
    }
}

// ---------------------------------------------------------------- Nether (ChunkProviderHell)

class HellGenerator {
public:
    explicit HellGenerator(int64_t seed) : seed_(seed), rand_(seed), caves_(true) {
        i_.init(rand_, 16);
        j_.init(rand_, 16);
        k_.init(rand_, 8);
        l_.init(rand_, 4);
        m_.init(rand_, 4);
        a_.init(rand_, 10);  // scale and depth noise: computed in Beta but never used
        b_.init(rand_, 16);
    }

    void provide(int cx, int cz, uint8_t* buf) {
        rand_.setSeed(jadd(jmul(cx, 341873128712LL), jmul(cz, 132897987541LL)));
        std::memset(buf, 0, 32768);
        generateTerrain(cx, cz, buf);
        replaceBlocks(cx, cz, buf);
        caves_.generate(seed_, cx, cz, buf);
    }

    void populate(World& w, int cx, int cz);

private:
    static constexpr int NX = 5, NY = 17, NZ = 5;
    int64_t seed_;
    JRandom rand_;
    PerlinOctaves i_, j_, k_, l_, m_, a_, b_;
    real density_[NX * NY * NZ];
    real nc_[NX * NY * NZ], nd_[NX * NY * NZ], ne_[NX * NY * NZ];
    real soul_[256], gravel_[256], depth_[256];
    Caves caves_;

    void density(int x, int z);
    void generateTerrain(int cx, int cz, uint8_t* buf);
    void replaceBlocks(int cx, int cz, uint8_t* buf);
};

void HellGenerator::density(int x, int z) {
    const real S = (real)684.412, SY = (real)2053.236;
    k_.generate(nc_, (real)x, 0, (real)z, NX, NY, NZ, S / 80, SY / 60, S / 80);
    i_.generate(nd_, (real)x, 0, (real)z, NX, NY, NZ, S, SY, S);
    j_.generate(ne_, (real)x, 0, (real)z, NX, NY, NZ, S, SY, S);
    real bias[NY];
    for (int y = 0; y < NY; ++y) {
        bias[y] = (real)(std::cos((double)y * 3.141592653589793 * 6.0 / NY) * 2.0);
        int yy = y > NY / 2 ? NY - 1 - y : y;
        if (yy < 4) {
            real d = (real)(4 - yy);
            bias[y] -= d * d * d * 10;
        }
    }
    int idx = 0;
    for (int i = 0; i < NX * NZ; ++i)
        for (int k = 0; k < NY; ++k, ++idx) {
            real lo = nd_[idx] / 512, hi = ne_[idx] / 512, sel = (nc_[idx] / 10 + 1) / 2;
            real v = sel < 0 ? lo : sel > 1 ? hi : lo + (hi - lo) * sel;
            v -= bias[k];
            if (k > NY - 4) {
                real t = (real)((float)(k - (NY - 4)) / 3.0f);
                v = v * (1 - t) + (real)-10.0 * t;
            }
            density_[idx] = v;
        }
}

void HellGenerator::generateTerrain(int cx, int cz, uint8_t* buf) {
    const int CELL = 4, LAVA_LEVEL = 32;
    density(cx * CELL, cz * CELL);
    const real* D = density_;
    for (int i = 0; i < CELL; ++i)
        for (int j = 0; j < CELL; ++j)
            for (int k = 0; k < 16; ++k) {
                const real dy = (real)0.125;
                real a = D[((i + 0) * NZ + j + 0) * NY + k];
                real b = D[((i + 0) * NZ + j + 1) * NY + k];
                real c = D[((i + 1) * NZ + j + 0) * NY + k];
                real d = D[((i + 1) * NZ + j + 1) * NY + k];
                real da = (D[((i + 0) * NZ + j + 0) * NY + k + 1] - a) * dy;
                real db = (D[((i + 0) * NZ + j + 1) * NY + k + 1] - b) * dy;
                real dc = (D[((i + 1) * NZ + j + 0) * NY + k + 1] - c) * dy;
                real dd = (D[((i + 1) * NZ + j + 1) * NY + k + 1] - d) * dy;
                for (int y8 = 0; y8 < 8; ++y8) {
                    const real q = (real)0.25;
                    real v0 = a, v1 = b, s0 = (c - a) * q, s1 = (d - b) * q;
                    int y = k * 8 + y8;
                    for (int x4 = 0; x4 < 4; ++x4) {
                        int idx = ((x4 + i * 4) << 11) | ((j * 4) << 7) | y;
                        real v = v0, sv = (v1 - v0) * q;
                        for (int z4 = 0; z4 < 4; ++z4) {
                            uint8_t id = y < LAVA_LEVEL ? LAVA : AIR;
                            if (v > 0) id = NETHERRACK;
                            buf[idx] = id;
                            idx += 128;
                            v += sv;
                        }
                        v0 += s0;
                        v1 += s1;
                    }
                    a += da;
                    b += db;
                    c += dc;
                    d += dd;
                }
            }
}

void HellGenerator::replaceBlocks(int cx, int cz, uint8_t* buf) {
    const int SEA = 64;
    const real s = (real)0.03125;
    l_.generate(soul_, (real)(cx * 16), (real)(cz * 16), 0, 16, 16, 1, s, s, 1);
    l_.generate(gravel_, (real)(cx * 16), (real)109.0134, (real)(cz * 16), 16, 1, 16, s, 1, s);
    m_.generate(depth_, (real)(cx * 16), (real)(cz * 16), 0, 16, 16, 1, s * 2, s * 2, s * 2);
    for (int a = 0; a < 16; ++a)
        for (int b = 0; b < 16; ++b) {
            bool soul = soul_[a + b * 16] + (real)(rand_.nextDouble() * 0.2) > 0;
            bool gravel = gravel_[a + b * 16] + (real)(rand_.nextDouble() * 0.2) > 0;
            int depth = (int)(depth_[a + b * 16] / 3 + 3 + (real)(rand_.nextDouble() * 0.25));
            int run = -1;
            uint8_t top = NETHERRACK, fill = NETHERRACK;
            for (int y = 127; y >= 0; --y) {
                int idx = (b * 16 + a) * 128 + y;
                if (y >= 127 - rand_.nextInt(5) || y <= rand_.nextInt(5)) {
                    buf[idx] = BEDROCK;
                    continue;
                }
                uint8_t id = buf[idx];
                if (id == AIR) {
                    run = -1;
                } else if (id == NETHERRACK) {
                    if (run == -1) {
                        if (depth <= 0) {
                            top = AIR;
                            fill = NETHERRACK;
                        } else if (y >= SEA - 4 && y <= SEA + 1) {
                            top = fill = NETHERRACK;
                            if (gravel) top = GRAVEL;
                            if (soul) top = fill = SOUL_SAND;
                        }
                        if (y < SEA && top == AIR) top = LAVA;
                        run = depth;
                        buf[idx] = y >= SEA - 1 ? top : fill;
                    } else if (run > 0) {
                        --run;
                        buf[idx] = fill;
                    }
                }
            }
        }
}

// WorldGenHellLava: a flowing source in a netherrack wall with exactly one open side.
void genHellLava(World& w, int x, int y, int z) {
    if (w.get(x, y + 1, z) != NETHERRACK) return;
    if (w.get(x, y, z) != AIR && w.get(x, y, z) != NETHERRACK) return;
    static const int N[5][3] = {{-1, 0, 0}, {1, 0, 0}, {0, 0, -1}, {0, 0, 1}, {0, -1, 0}};
    int rack = 0, air = 0;
    for (const auto& n : N) {
        uint8_t id = w.get(x + n[0], y + n[1], z + n[2]);
        rack += id == NETHERRACK;
        air += id == AIR;
    }
    if (rack == 4 && air == 1) {
        w.setRaw(x, y, z, LAVA_FLOWING);
        w.scheduleTick(x, y, z, LAVA_FLOWING, 0);
    }
}

// WorldGenFire
void genHellFire(World& w, JRandom& r, int x, int y, int z) {
    for (int i = 0; i < 64; ++i) {
        int xx = x + spread(r, 8), yy = y + spread(r, 4), zz = z + spread(r, 8);
        if (w.get(xx, yy, zz) == AIR && w.get(xx, yy - 1, zz) == NETHERRACK) w.setRaw(xx, yy, zz, FIRE);
    }
}

// WorldGenGlowStone1/2: a cluster hanging from the netherrack ceiling.
void genGlowstone(World& w, JRandom& r, int x, int y, int z) {
    if (w.get(x, y, z) != AIR || w.get(x, y + 1, z) != NETHERRACK) return;
    w.setRaw(x, y, z, GLOWSTONE);
    for (int i = 0; i < 1500; ++i) {
        int xx = x + spread(r, 8), yy = y - r.nextInt(12), zz = z + spread(r, 8);
        if (w.get(xx, yy, zz) != AIR) continue;
        int n = 0;
        for (int f = 0; f < 6; ++f) {
            static const int D[6][3] = {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};
            n += w.get(xx + D[f][0], yy + D[f][1], zz + D[f][2]) == GLOWSTONE;
        }
        if (n == 1) w.setRaw(xx, yy, zz, GLOWSTONE);
    }
}

void HellGenerator::populate(World& w, int cx, int cz) {
    JRandom& r = rand_;
    int x0 = cx * 16, z0 = cz * 16;
    // Beta keeps using the generator's random from the last provided chunk; seed it per chunk instead.
    r.setSeed(seed_);
    int64_t a = oddLong(r);
    int64_t b = oddLong(r);
    r.setSeed(jadd(jmul(cx, a), jmul(cz, b)) ^ seed_);
    auto px = [&]() { return x0 + r.nextInt(16) + 8; };
    auto pz = [&]() { return z0 + r.nextInt(16) + 8; };
    for (int i = 0; i < 8; ++i) {
        int x = px(), y = r.nextInt(120) + 4, z = pz();
        genHellLava(w, x, y, z);
    }
    int n = r.nextInt(r.nextInt(10) + 1) + 1;
    for (int i = 0; i < n; ++i) {
        int x = px(), y = r.nextInt(120) + 4, z = pz();
        genHellFire(w, r, x, y, z);
    }
    n = r.nextInt(r.nextInt(10) + 1);
    for (int i = 0; i < n; ++i) {
        int x = px(), y = r.nextInt(120) + 4, z = pz();
        genGlowstone(w, r, x, y, z);
    }
    for (int i = 0; i < 10; ++i) {
        int x = px(), y = r.nextInt(128), z = pz();
        genGlowstone(w, r, x, y, z);
    }
    if (r.nextInt(1) == 0) {
        int x = px(), y = r.nextInt(128), z = pz();
        genFlowers(w, r, MUSHROOM_BROWN, x, y, z);
    }
    if (r.nextInt(1) == 0) {
        int x = px(), y = r.nextInt(128), z = pz();
        genFlowers(w, r, MUSHROOM_RED, x, y, z);
    }
}

}  // namespace

const char* biomeName(int biome) { return biome >= 0 && biome < BIOME_COUNT ? BIOMES[biome].name : "?"; }

int biomeFromClimate(float temperature, float humidity) {
    initBiomeLookup();
    if (temperature < 0) temperature = 0;
    if (temperature > 1) temperature = 1;
    if (humidity < 0) humidity = 0;
    if (humidity > 1) humidity = 1;
    return biomeFromLookup(temperature, humidity);
}

bool growSapling(World& w, JRandom& r, int x, int y, int z, int type) {
    w.setRaw(x, y, z, AIR);
    bool ok = type == 1 ? genTaiga2(w, r, x, y, z) : type == 2 ? genBirch(w, r, x, y, z) : genOak(w, r, x, y, z);
    if (!ok) w.setRaw(x, y, z, SAPLING, type);
    return ok;
}

BiomeSource::BiomeSource(int64_t seed) {
    initBiomeLookup();
    JRandom r1(jmul(seed, 9871LL));
    tNoise_.init(r1, 4);
    JRandom r2(jmul(seed, 39811LL));
    hNoise_.init(r2, 4);
    JRandom r3(jmul(seed, 543321LL));
    oNoise_.init(r3, 2);
}

void BiomeSource::loadInto(real* t, real* hm, real* o, uint8_t* b, int x, int z, int w, int h) const {
    tNoise_.generate(t, (real)x, (real)z, w, w, (real)0.025, (real)0.025, (real)0.25);
    hNoise_.generate(hm, (real)x, (real)z, w, w, (real)0.05, (real)0.05, (real)(1.0 / 3.0));
    oNoise_.generate(o, (real)x, (real)z, w, w, (real)0.25, (real)0.25, (real)0.5882352941176471);
    int idx = 0;
    for (int i = 0; i < w; ++i) {
        for (int j = 0; j < h; ++j) {
            real v = o[idx] * (real)1.1 + (real)0.5;
            real tt = (t[idx] * (real)0.15 + (real)0.7) * (real)0.99 + v * (real)0.01;
            real hh = (hm[idx] * (real)0.15 + (real)0.5) * (real)0.998 + v * (real)0.002;
            tt = 1 - (1 - tt) * (1 - tt);
            if (tt < 0) tt = 0;
            if (hh < 0) hh = 0;
            if (tt > 1) tt = 1;
            if (hh > 1) hh = 1;
            t[idx] = tt;
            hm[idx] = hh;
            b[idx++] = biomeFromLookup(tt, hh);
        }
    }
}

void BiomeSource::load(int x, int z, int w, int h) {
    real other[256];
    loadInto(temperature, humidity, other, biomes, x, z, w, h);
}

int BiomeSource::biomeAt(int x, int z) const {
    real t[1], hm[1], o[1];
    uint8_t b[1];
    loadInto(t, hm, o, b, x, z, 1, 1);
    return b[0];
}

void BiomeSource::temperatures(real* out, int x, int z, int w, int h) const {
    real other[256];
    tNoise_.generate(out, (real)x, (real)z, w, h, (real)0.025, (real)0.025, (real)0.25);
    oNoise_.generate(other, (real)x, (real)z, w, h, (real)0.25, (real)0.25, (real)0.5882352941176471);
    for (int i = 0; i < w * h; ++i) {
        real v = other[i] * (real)1.1 + (real)0.5;
        real tt = (out[i] * (real)0.15 + (real)0.7) * (real)0.99 + v * (real)0.01;
        tt = 1 - (1 - tt) * (1 - tt);
        if (tt < 0) tt = 0;
        if (tt > 1) tt = 1;
        out[i] = tt;
    }
}

struct WorldGenerator::Impl {
    Impl(int64_t seed, bool hellWorld) : bs(seed) {
        if (hellWorld) hell = new HellGenerator(seed);
        else gen = new ChunkGenerator(seed);
    }
    ~Impl() {
        delete gen;
        delete hell;
    }
    BiomeSource bs;
    ChunkGenerator* gen = nullptr;
    HellGenerator* hell = nullptr;
    uint8_t buf[32768];
};

WorldGenerator::WorldGenerator(int64_t seed, bool hell) : impl_(new Impl(seed, hell)) {}
WorldGenerator::~WorldGenerator() { delete impl_; }

bool WorldGenerator::provide(Chunk& c) {
    Impl& m = *impl_;
    if (m.hell) {
        m.hell->provide(c.cx, c.cz, m.buf);
        // WorldChunkManagerHell: hot and dry everywhere
        std::memset(c.temperature, 255, sizeof(c.temperature));
        std::memset(c.humidity, 0, sizeof(c.humidity));
    } else {
        m.gen->provide(c.cx, c.cz, m.buf, m.bs);
        for (int i = 0; i < 256; ++i) {
            c.temperature[i] = (uint8_t)(m.bs.temperature[i] * 255);
            c.humidity[i] = (uint8_t)(m.bs.humidity[i] * 255);
        }
    }
    for (int sy = 0; sy < World::SECTIONS_Y; ++sy) {
        bool any = false;
        for (int x = 0; x < 16 && !any; ++x)
            for (int z = 0; z < 16 && !any; ++z) {
                const uint8_t* col = m.buf + ((x << 11) | (z << 7)) + sy * 16;
                for (int y = 0; y < 16; ++y)
                    if (col[y]) {
                        any = true;
                        break;
                    }
            }
        if (!any) continue;
        Section* s = (Section*)std::malloc(sizeof(Section));
        if (!s) return false;
        std::memset(s->meta, 0, sizeof(s->meta));
        std::memset(s->light, 0, sizeof(s->light));
        for (int x = 0; x < 16; ++x)
            for (int z = 0; z < 16; ++z)
                std::memcpy(s->blocks + sectionIndex(x, 0, z), m.buf + ((x << 11) | (z << 7)) + sy * 16, 16);
        c.sec[sy] = s;
    }
    return true;
}

void WorldGenerator::populate(World& w, int cx, int cz) {
    if (impl_->hell) impl_->hell->populate(w, cx, cz);
    else impl_->gen->populate(w, impl_->bs, cx, cz);
}

int WorldGenerator::biomeAt(int x, int z) const { return impl_->hell ? BIOME_DESERT : impl_->bs.biomeAt(x, z); }
