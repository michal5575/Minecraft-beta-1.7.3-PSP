// Survival rules, entities and mobs, modeled on Beta 1.7.3 (EntityPlayer, EntityLiving, EntityCreature,
// EntityMob and subclasses, EntityItem, EntityArrow, Explosion, PlayerControllerSP, Block*.updateTick).
#include "game.h"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "chunkio.h"
#include "mathhelper.h"
#include "worldgen.h"

namespace {

constexpr float PI = 3.14159265f;
constexpr float DEG = PI / 180.0f;

inline int ifloor(float v) {
    int i = (int)v;
    return v < (float)i ? i - 1 : i;
}
inline float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
inline int iabs(int v) { return v < 0 ? -v : v; }

uint32_t g_fast = 0x9E3779B9u;
inline uint32_t fastRand() {
    g_fast ^= g_fast << 13;
    g_fast ^= g_fast >> 17;
    g_fast ^= g_fast << 5;
    return g_fast;
}

// EntityRenderer light table: brightness for light levels 0..15.
float lightBrightness(int l) {
    float f = 1.0f - l / 15.0f;
    return (1.0f - f) / (f * 3.0f + 1.0f);
}

float randGaussian(JRandom& r) {
    float s = 0;
    for (int i = 0; i < 4; ++i) s += r.nextFloat();
    return (s - 2.0f) * 1.732f;
}

struct MobInfo {
    float width, height;
    int16_t health;
    float speed;
};

const MobInfo MOBS[ENT_TYPE_COUNT] = {
    {0.6f, 1.8f, 0, 0},     {0.25f, 0.25f, 5, 0},  {0.5f, 0.5f, 1, 0},    {0.98f, 0.98f, 1, 0},
    {0.6f, 1.8f, 20, 0.5f}, {0.6f, 1.8f, 20, 0.7f}, {0.6f, 1.8f, 20, 0.7f}, {1.4f, 0.9f, 20, 0.8f},
    {0.9f, 0.9f, 10, 0.7f}, {0.9f, 1.3f, 10, 0.7f}, {0.9f, 1.3f, 10, 0.7f}, {0.3f, 0.4f, 4, 0.7f},
    {0.6f, 0.6f, 1, 0},     {0.95f, 0.95f, 10, 0},  {0.8f, 0.8f, 8, 1.1f},  {0.25f, 0.25f, 0, 0},
    {0.25f, 0.25f, 0, 0},   {0.25f, 0.25f, 0, 0},   {0.5f, 0.5f, 0, 0},     {0.98f, 0.7f, 0, 0},
    {1.5f, 0.6f, 0, 0},     {4.0f, 4.0f, 10, 0},    {0.6f, 1.8f, 20, 0.5f}, {1.0f, 1.0f, 0, 0},
};

float paintingOffset(int size) { return size == 32 || size == 64 ? 0.5f : 0.0f; }

// EntityPainting bounding box (func_412_b): thin along the facing axis.
AABB paintingBox(const Entity& e) {
    const PaintingArt& a = g_paintingArt[e.woolColor];
    float w = (float)a.w, h = (float)a.h, d = (float)a.w;
    if (e.fuse == 0 || e.fuse == 2) d = 0.5f;
    else w = 0.5f;
    w /= 32.0f, h /= 32.0f, d /= 32.0f;
    const float f = -0.00625f;
    return {e.x - w - f, e.y - h - f, e.z - d - f, e.x + w + f, e.y + h + f, e.z + d + f};
}

// Entity.canBeCollidedWith: what the crosshair and projectiles can hit.
bool collidable(const Entity& e) {
    return (entityIsMob(e.type) && e.health > 0) ||
           ((e.type == ENT_PAINTING || entityIsVehicle(e.type) || e.type == ENT_FIREBALL) && !e.dead);
}
AABB hitBox(const Entity& e) { return e.type == ENT_PAINTING ? paintingBox(e) : e.box(); }

void setSlimeSize(Entity& e, int size) {
    e.size = (uint8_t)size;
    e.width = e.height = 0.6f * size;
    e.health = (int16_t)(size * size);
    e.setPosition(e.x, e.y, e.z);
}

// Explosion resistance as Block.getExplosionResistance (resistance / 5).
float explosionResistance(uint8_t id) {
    switch (id) {
        case BEDROCK: return 1.8e6f;
        case OBSIDIAN: return 2000.0f;
        case WATER:
        case WATER_FLOWING:
        case LAVA:
        case LAVA_FLOWING: return 100.0f;
        case STONE:
        case COBBLESTONE:
        case BRICK:
        case MOSSY_COBBLESTONE:
        case COBBLE_STAIRS:
        case SLAB:
        case DOUBLE_SLAB:
        case GOLD_BLOCK:
        case IRON_BLOCK:
        case DIAMOND_BLOCK:
        case IRON_DOOR: return 6.0f;
        case PLANKS:
        case WOOD_STAIRS:
        case FENCE: return 3.0f;
        default: return g_blocks[id].hardness > 0 ? g_blocks[id].hardness : 0.0f;
    }
}

// Saved entity layout (chunk entity blob).
struct EntityRecord {
    uint8_t type, woolColor, flags, pad;
    float x, y, z, mx, my, mz, yaw;
    int16_t health, fuse;
    int32_t age, eggTimer;
    uint16_t itemId, itemDamage;
    uint8_t itemCount, pad2;
    int16_t pickupDelay;
};

// Records of this type continue the entity before them (older builds skip them as unknown types):
// kind 0 holds up to 8 minecart chest slots, kind 1 the furnace minecart push direction.
constexpr uint8_t EXTRA_RECORD = 0xFE;

int cartItemCount(const Entity& e) {
    int k = 0;
    for (const ItemStack& s : e.cartItems)
        if (!s.empty()) ++k;
    return k;
}

int extraRecordCount(const Entity& e) {
    if (e.type != ENT_MINECART) return 0;
    return (cartItemCount(e) + 7) / 8 + (e.pushX != 0 || e.pushZ != 0 ? 1 : 0);
}

int writeExtraRecords(const Entity& e, uint8_t* out) {
    if (e.type != ENT_MINECART) return 0;
    int n = 0;
    uint8_t rec[sizeof(EntityRecord)];
    int slot = 0;
    while (true) {
        std::memset(rec, 0, sizeof(rec));
        int k = 0;
        for (; slot < 27 && k < 8; ++slot) {
            const ItemStack& s = e.cartItems[slot];
            if (s.empty()) continue;
            uint8_t* p = rec + 4 + k * 6;
            std::memcpy(p, &s.id, 2);
            std::memcpy(p + 2, &s.damage, 2);
            p[4] = s.count;
            p[5] = (uint8_t)slot;
            ++k;
        }
        if (!k) break;
        rec[0] = EXTRA_RECORD, rec[1] = 0, rec[2] = (uint8_t)k;
        std::memcpy(out + n++ * sizeof(rec), rec, sizeof(rec));
    }
    if (e.pushX != 0 || e.pushZ != 0) {
        std::memset(rec, 0, sizeof(rec));
        rec[0] = EXTRA_RECORD, rec[1] = 1;
        std::memcpy(rec + 4, &e.pushX, 4);
        std::memcpy(rec + 8, &e.pushZ, 4);
        std::memcpy(out + n++ * sizeof(rec), rec, sizeof(rec));
    }
    return n;
}

void readExtraRecord(Entity& e, const uint8_t* rec) {
    if (e.type != ENT_MINECART) return;
    if (rec[1] == 0) {
        for (int k = 0; k < rec[2] && k < 8; ++k) {
            const uint8_t* p = rec + 4 + k * 6;
            uint16_t id, dmg;
            std::memcpy(&id, p, 2);
            std::memcpy(&dmg, p + 2, 2);
            if (p[5] < 27 && p[4] > 0 && itemExists(id)) e.cartItems[p[5]] = ItemStack(id, p[4], dmg);
        }
    } else if (rec[1] == 1) {
        std::memcpy(&e.pushX, rec + 4, 4);
        std::memcpy(&e.pushZ, rec + 8, 4);
    }
}

struct LevelData {
    char magic[8];
    int32_t version;
    int32_t mode;
    int64_t seed, time;
    int32_t spawnX, spawnY, spawnZ, spawnFound;
    float x, y, z, yaw, pitch;
    int32_t flying, health, air, fire, selected;
    uint16_t ids[40];
    uint16_t damages[40];
    uint8_t counts[40];
    int32_t bedX, bedY, bedZ, hasBed;  // added after version 1; older files end before bedX
    int32_t dimension;                 // added with the Nether
    int32_t portalCount;
    int32_t portals[32][4];
    int32_t rainTime, thunderTime, raining, thundering;  // added with the weather
    char name[32];
    int64_t lastPlayed;
};
const char LEVEL_MAGIC[8] = {'M', 'C', 'P', 'S', 'P', 'L', 'V', '1'};

void hsbToRgb(float h, float s, float b, float& r, float& g, float& bl) {
    h = (h - std::floor(h)) * 6.0f;
    int i = (int)h;
    float f = h - i, p = b * (1 - s), q = b * (1 - s * f), t = b * (1 - s * (1 - f));
    switch (i) {
        case 0: r = b, g = t, bl = p; break;
        case 1: r = q, g = b, bl = p; break;
        case 2: r = p, g = b, bl = t; break;
        case 3: r = p, g = q, bl = b; break;
        case 4: r = t, g = p, bl = b; break;
        default: r = b, g = p, bl = q; break;
    }
}

bool boxTouches(const World& w, const AABB& b, uint8_t id) {
    for (int x = ifloor(b.x0); x <= ifloor(b.x1); ++x)
        for (int y = ifloor(b.y0); y <= ifloor(b.y1); ++y)
            for (int z = ifloor(b.z0); z <= ifloor(b.z1); ++z)
                if (w.get(x, y, z) == id) return true;
    return false;
}

uint32_t packRGB(float r, float g, float b) {
    int R = (int)(clampf(r, 0, 1) * 255), G = (int)(clampf(g, 0, 1) * 255), B = (int)(clampf(b, 0, 1) * 255);
    return (uint32_t)((R << 16) | (G << 8) | B);
}

}  // namespace

const PaintingArt g_paintingArt[] = {
    {"Kebab", 16, 16, 0, 0},         {"Aztec", 16, 16, 16, 0},       {"Alban", 16, 16, 32, 0},
    {"Aztec2", 16, 16, 48, 0},       {"Bomb", 16, 16, 64, 0},        {"Plant", 16, 16, 80, 0},
    {"Wasteland", 16, 16, 96, 0},    {"Pool", 32, 16, 0, 32},        {"Courbet", 32, 16, 32, 32},
    {"Sea", 32, 16, 64, 32},         {"Sunset", 32, 16, 96, 32},     {"Creebet", 32, 16, 128, 32},
    {"Wanderer", 16, 32, 0, 64},     {"Graham", 16, 32, 16, 64},     {"Match", 32, 32, 0, 128},
    {"Bust", 32, 32, 32, 128},       {"Stage", 32, 32, 64, 128},     {"Void", 32, 32, 96, 128},
    {"SkullAndRoses", 32, 32, 128, 128}, {"Fighters", 64, 32, 0, 96}, {"Pointer", 64, 64, 0, 192},
    {"Pigscene", 64, 64, 64, 192},   {"BurningSkull", 64, 64, 128, 192}, {"Skeleton", 64, 48, 192, 64},
    {"DonkeyKong", 64, 48, 192, 112},
};
const int g_paintingArtCount = sizeof(g_paintingArt) / sizeof(g_paintingArt[0]);

// ------------------------------------------------------------------ inventory

bool Inventory::add(ItemStack& s) {
    if (s.empty()) return true;
    int max = itemMaxStack(s.id);
    for (int pass = 0; pass < 2 && s.count; ++pass)
        for (int i = 0; i < 36 && s.count; ++i) {
            ItemStack& d = main[i];
            if (pass == 0) {
                if (d.empty() || !d.sameItem(s) || max == 1 || d.count >= max) continue;
                int n = max - d.count;
                if (n > s.count) n = s.count;
                d.count = (uint8_t)(d.count + n);
                s.count = (uint8_t)(s.count - n);
            } else if (d.empty()) {
                d = s;
                if (d.count > max) {
                    d.count = (uint8_t)max;
                    s.count = (uint8_t)(s.count - max);
                } else {
                    s.count = 0;
                }
            }
        }
    if (s.count == 0) s.clear();
    return s.empty();
}

int Inventory::armorValue() const {
    int total = 0, damage = 0, max = 0;
    for (const ItemStack& a : armor) {
        if (a.empty()) continue;
        const ItemDef& d = itemDef(a.id);
        if (d.armorSlot < 0) continue;
        int m = d.maxDamage;
        int left = m - a.damage;
        damage += left;
        max += m;
        total += d.armorPoints;
    }
    if (max == 0) return 0;
    return (total - 1) * damage / max + 1;
}

void Inventory::damageArmor(int dmg) {
    for (ItemStack& a : armor) {
        if (a.empty()) continue;
        a.damage = (uint16_t)(a.damage + dmg);
        if (a.damage >= itemDef(a.id).maxDamage) a.clear();
    }
}

int Inventory::count(uint16_t id) const {
    int n = 0;
    for (const ItemStack& s : main)
        if (s.id == id) n += s.count;
    return n;
}

bool Inventory::consume(uint16_t id) {
    for (ItemStack& s : main)
        if (s.id == id && s.count) {
            if (--s.count == 0) s.clear();
            return true;
        }
    return false;
}

void Inventory::clear() {
    for (ItemStack& s : main) s.clear();
    for (ItemStack& s : armor) s.clear();
}

// ------------------------------------------------------------------ setup

Game::Game() : rand(1) {}

bool Game::init() {
    mh::init();
    initItems();
    initRecipes();
    world.onChunkLoaded = &Game::hookLoaded;
    world.onChunkUnload = &Game::hookUnload;
    world.onChunkPopulated = [](World&, Chunk* c, void* u) { ((Game*)u)->onChunkPopulated(c); };
    world.hookUser = this;
    return world.init();
}

void Game::hookLoaded(World&, Chunk* c, void* user) { ((Game*)user)->onChunkLoaded(c); }
void Game::hookUnload(World&, Chunk* c, void* user) {
    Game* g = (Game*)user;
    g->onChunkUnload(c);
    if (g->onChunkRelease) g->onChunkRelease(c, g->releaseUser);
}

bool Game::levelExists(const char* dir) {
    char path[96];
    std::snprintf(path, sizeof(path), "%s/level.dat", dir);
    return fileExists(path);
}

static bool readLevelData(const char* dir, LevelData& d, size_t& size) {
    char path[96];
    std::snprintf(path, sizeof(path), "%s/level.dat", dir);
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::memset(&d, 0, sizeof(d));
    size = std::fread(&d, 1, sizeof(d), f);
    std::fclose(f);
    return size >= offsetof(LevelData, bedX) && std::memcmp(d.magic, LEVEL_MAGIC, 8) == 0;
}

bool Game::readLevelInfo(const char* dir, LevelInfo& out) {
    LevelData d;
    size_t size;
    if (!readLevelData(dir, d, size)) return false;
    std::snprintf(out.name, sizeof(out.name), "%.31s", d.name[0] ? d.name : "World");
    out.seed = d.seed;
    out.lastPlayed = d.lastPlayed;
    out.mode = d.mode;
    return true;
}

bool Game::renameLevel(const char* dir, const char* name) {
    LevelData d;
    size_t size;
    if (!readLevelData(dir, d, size)) return false;
    std::memset(d.name, 0, sizeof(d.name));
    std::snprintf(d.name, sizeof(d.name), "%s", name);
    char path[96], tmp[96];
    std::snprintf(path, sizeof(path), "%s/level.dat", dir);
    std::snprintf(tmp, sizeof(tmp), "%s/level.tmp", dir);
    FILE* f = std::fopen(tmp, "wb");
    if (!f) return false;
    bool ok = std::fwrite(&d, sizeof(d), 1, f) == 1;
    ok = std::fclose(f) == 0 && ok;
    if (!ok) return false;
    std::remove(path);
    return std::rename(tmp, path) == 0;
}

void Game::deleteLevel(const char* dir) {
    char nether[96];
    std::snprintf(nether, sizeof(nether), "%s/DIM-1", dir);
    removeWorldFiles(nether);
    removeWorldFiles(dir);
}

static void resetEntities(Entity* ents, int n) {
    for (int i = 0; i < n; ++i) ents[i] = Entity();
}

void Game::dimensionDir(char* out, int size, int dim) const {
    if (dim < 0) std::snprintf(out, size, "%s/DIM-1", baseDir_);
    else std::snprintf(out, size, "%s", baseDir_);
}

bool Game::createWorld(const char* dir, int64_t seed, int m) {
    closeWorld();
    resetEntities(ents, MAX_ENTITIES);
    std::snprintf(baseDir_, sizeof(baseDir_), "%s", dir);
    char nether[64];
    dimensionDir(nether, sizeof(nether), -1);
    removeWorldFiles(nether);
    removeWorldFiles(dir);
    dimension = 0;
    knownPortalCount_ = 0;
    portalTime = prevPortalTime = 0;
    portalCooldown = 0;
    portalPending = false;
    clearWeather();
    rain = prevRain = thunder = prevThunder = 0;
    lightningFlash = boltCount = 0;
    particles.clear();
    if (!world.open(dir, seed)) return false;
    rand.setSeed(seed ^ 0x5DEECE66DLL);
    g_fast = (uint32_t)seed | 1u;
    mode = (uint8_t)m;
    inv.clear();
    inv.selected = 0;
    health = prevHealth = MAX_HEALTH;
    air = MAX_AIR;
    fire = hurtTime = hurtResist = lastDamage = 0;
    dead = false;
    spawnX = 0;
    spawnZ = 0;
    spawnY = 100;
    spawnFound = false;
    hasBed = sleeping = false;
    sleepTimer = 0;
    player = Player();
    player.setPosition(0.5f, 100.0f, 0.5f);
    world.skySubtracted = computeSkySubtracted();
    if (mode == MODE_CREATIVE) {
        const uint16_t HOTBAR[9] = {STONE, COBBLESTONE, DIRT, PLANKS, LOG, GLASS, BRICK, TORCH, SAND};
        for (int i = 0; i < 9; ++i) inv.main[i] = ItemStack(HOTBAR[i], 1);
    }
    return true;
}

bool Game::loadWorld(const char* dir) {
    closeWorld();
    LevelData d;
    size_t size;
    if (!readLevelData(dir, d, size)) return false;
    resetEntities(ents, MAX_ENTITIES);
    std::snprintf(baseDir_, sizeof(baseDir_), "%s", dir);
    dimension = d.dimension < 0 ? -1 : 0;
    knownPortalCount_ = d.portalCount < 0 ? 0 : d.portalCount > MAX_KNOWN_PORTALS ? MAX_KNOWN_PORTALS : d.portalCount;
    std::memcpy(knownPortals_, d.portals, sizeof(knownPortals_));
    // WorldInfo weather; World's constructor starts an ongoing storm at full strength.
    rainTime = d.rainTime, thunderTime = d.thunderTime;
    raining = d.raining != 0, thundering = d.thundering != 0;
    rain = prevRain = raining ? 1.0f : 0.0f;
    thunder = prevThunder = raining && thundering ? 1.0f : 0.0f;
    lightningFlash = boltCount = 0;
    particles.clear();
    std::snprintf(worldName, sizeof(worldName), "%.31s", d.name[0] ? d.name : "World");
    lastPlayed = d.lastPlayed;
    portalTime = prevPortalTime = 0;
    portalCooldown = 0;
    portalPending = false;
    char wdir[64];
    dimensionDir(wdir, sizeof(wdir), dimension);
    if (!world.open(wdir, d.seed, dimension)) return false;
    world.time = d.time;
    rand.setSeed(d.seed ^ d.time);
    g_fast = (uint32_t)d.time | 1u;
    mode = (uint8_t)d.mode;
    spawnX = d.spawnX;
    spawnY = d.spawnY;
    spawnZ = d.spawnZ;
    spawnFound = d.spawnFound != 0;
    hasBed = d.hasBed != 0;
    bedX = d.bedX;
    bedY = d.bedY;
    bedZ = d.bedZ;
    sleeping = false;
    sleepTimer = 0;
    player = Player();
    player.setPosition(d.x, d.y, d.z);
    player.yaw = d.yaw;
    player.pitch = d.pitch;
    player.flying = d.flying != 0 && mode == MODE_CREATIVE;
    health = prevHealth = d.health;
    air = d.air;
    fire = d.fire;
    hurtTime = hurtResist = lastDamage = 0;
    dead = health <= 0;
    inv.selected = d.selected % 9;
    for (int i = 0; i < 40; ++i) {
        ItemStack s(d.ids[i], d.counts[i], d.damages[i]);
        if (!itemExists(s.id)) s.clear();
        if (i < 36)
            inv.main[i] = s;
        else
            inv.armor[i - 36] = s;
    }
    world.skySubtracted = computeSkySubtracted();
    return true;
}

bool Game::saveLevel() {
    if (!world.isOpen()) return false;
    LevelData d;
    std::memset(&d, 0, sizeof(d));
    std::memcpy(d.magic, LEVEL_MAGIC, 8);
    d.version = 1;
    d.mode = mode;
    d.seed = world.seed;
    d.time = world.time;
    d.spawnX = spawnX;
    d.spawnY = spawnY;
    d.spawnZ = spawnZ;
    d.spawnFound = spawnFound ? 1 : 0;
    d.bedX = bedX;
    d.bedY = bedY;
    d.bedZ = bedZ;
    d.hasBed = hasBed ? 1 : 0;
    d.dimension = dimension;
    d.portalCount = knownPortalCount_;
    std::memcpy(d.portals, knownPortals_, sizeof(d.portals));
    d.rainTime = rainTime, d.thunderTime = thunderTime;
    d.raining = raining ? 1 : 0, d.thundering = thundering ? 1 : 0;
    std::snprintf(d.name, sizeof(d.name), "%s", worldName);
    d.lastPlayed = lastPlayed;
    d.x = player.x;
    d.y = player.y;
    d.z = player.z;
    d.yaw = player.yaw;
    d.pitch = player.pitch;
    d.flying = player.flying ? 1 : 0;
    d.health = dead ? MAX_HEALTH : health;
    d.air = air;
    d.fire = fire;
    d.selected = inv.selected;
    for (int i = 0; i < 40; ++i) {
        const ItemStack& s = i < 36 ? inv.main[i] : inv.armor[i - 36];
        d.ids[i] = s.id;
        d.counts[i] = s.count;
        d.damages[i] = s.damage;
    }
    if (dead) {
        d.x = spawnX + 0.5f;
        d.y = (float)spawnY;
        d.z = spawnZ + 0.5f;
        d.dimension = 0;
    }
    char path[96], tmp[96];
    std::snprintf(path, sizeof(path), "%s/level.dat", baseDir_);
    std::snprintf(tmp, sizeof(tmp), "%s/level.tmp", baseDir_);
    FILE* f = std::fopen(tmp, "wb");
    if (!f) return false;
    bool ok = std::fwrite(&d, sizeof(d), 1, f) == 1;
    ok = std::fclose(f) == 0 && ok;
    if (!ok) return false;
    std::remove(path);
    return std::rename(tmp, path) == 0;
}

void Game::closeWorld() {
    if (!world.isOpen()) return;
    saveLevel();
    world.close();
    resetEntities(ents, MAX_ENTITIES);
    particles.clear();
    boltCount = 0;
    openTile = nullptr;
    entityTarget = nullptr;
    fishHook = -1;
    if (record.record) {
        record.record = 0;
        ++record.serial;
    }
}

void Game::stream(int rd, double budget) {
    renderDistance = rd;
    world.update(player.x, player.z, rd + 1, budget);
}

bool Game::spawnReady(int radius) const {
    int n = (radius * 2 + 1) * (radius * 2 + 1);
    return world.readyCount(player.x, player.z, radius) >= n;
}

void Game::findSpawn() {
    for (int r = 0; r < 40; ++r)
        for (int dx = -r; dx <= r; ++dx)
            for (int dz = -r; dz <= r; ++dz) {
                if (iabs(dx) != r && iabs(dz) != r) continue;
                int x = spawnX + dx, z = spawnZ + dz;
                const Chunk* c = world.chunk(x >> 4, z >> 4);
                if (!c || !(c->flags & CHUNK_LIT)) continue;
                int y = world.topSolidAt(x, z);
                if (y <= 0) continue;
                uint8_t ground = world.get(x, y - 1, z);
                bool open = true;
                for (int yy = y; yy < World::SY && open; ++yy)
                    if (world.get(x, yy, z) != AIR) open = false;
                if (open && (ground == GRASS || ground == SAND)) {
                    spawnX = x;
                    spawnY = y;
                    spawnZ = z;
                    spawnFound = true;
                    return;
                }
            }
    spawnY = world.topSolidAt(spawnX, spawnZ);
    if (spawnY < 1) spawnY = 100;
    spawnFound = true;
}

void Game::placeAtSpawn() {
    if (hasBed && world.isLoaded(bedX, bedZ)) {
        int sx, sy, sz;
        if (world.get(bedX, bedY, bedZ) == BED && bedSpawnSpot(bedX, bedY, bedZ, sx, sy, sz)) {
            player.setPosition(sx + 0.5f, (float)sy, sz + 0.5f);
            return;
        }
        hasBed = false;
        message = "Your home bed was missing or obstructed";
    } else if (hasBed) {
        player.setPosition(bedX + 0.5f, bedY + 1.0f, bedZ + 0.5f);
        return;
    }
    if (!spawnFound) findSpawn();
    int y = world.topSolidAt(spawnX, spawnZ);
    player.setPosition(spawnX + 0.5f, (float)(y > 0 ? y : spawnY), spawnZ + 0.5f);
}

void Game::respawn() {
    if (Entity* v = ridingEntity()) v->rider = -1;
    riding = -1;
    dead = false;
    deathTime = 0;
    health = prevHealth = MAX_HEALTH;
    air = MAX_AIR;
    fire = 0;
    hurtTime = hurtResist = 0;
    player.mx = player.my = player.mz = 0;
    float yaw = player.yaw;
    placeAtSpawn();
    player.yaw = yaw;
    player.pitch = 0;
}

// ------------------------------------------------------------------ chunk hooks

void Game::onChunkLoaded(Chunk* c) {
    if (!c->entityData) return;
    uint32_t n = c->entityBytes / sizeof(EntityRecord);
    Entity* last = nullptr;
    for (uint32_t i = 0; i < n; ++i) {
        EntityRecord r;
        std::memcpy(&r, c->entityData + i * sizeof(EntityRecord), sizeof(r));
        if (r.type == EXTRA_RECORD) {
            if (last) readExtraRecord(*last, c->entityData + i * sizeof(EntityRecord));
            continue;
        }
        last = nullptr;
        if (r.type <= ENT_NONE || r.type >= ENT_TYPE_COUNT) continue;
        Entity* e = spawn(r.type, r.x, r.y, r.z);
        if (!e) break;
        last = e;
        e->mx = r.mx, e->my = r.my, e->mz = r.mz;
        e->yaw = e->prevYaw = r.yaw;
        e->health = r.health;
        e->fuse = r.fuse;
        e->age = r.age;
        e->eggTimer = r.eggTimer;
        e->woolColor = r.woolColor;
        e->sheared = (r.flags & 1) != 0;
        e->inGround = (r.flags & 2) != 0;
        e->fromPlayer = (r.flags & 4) != 0;
        e->tamed = (r.flags & 8) != 0;
        e->sitting = (r.flags & 16) != 0;
        e->angry = (r.flags & 32) != 0;
        e->saddled = (r.flags & 64) != 0;
        e->powered = (r.flags & 128) != 0;
        if (entityIsVehicle(e->type)) {
            e->cartType = r.woolColor <= 2 ? r.woolColor : 0;
            e->fuel = r.eggTimer;
            e->woolColor = 0;
            e->eggTimer = 0;
        }
        if (e->type == ENT_SLIME) {
            setSlimeSize(*e, r.woolColor >= 1 && r.woolColor <= 4 ? r.woolColor : 1);
            e->woolColor = 0;
        }
        e->health = r.health;
        e->item = ItemStack(r.itemId, r.itemCount, r.itemDamage);
        e->pickupDelay = r.pickupDelay;
        if (e->type == ENT_ITEM && (e->item.empty() || !itemExists(e->item.id))) e->type = ENT_NONE;
        if (e->type == ENT_PAINTING) {
            e->tileX = (int)r.mx, e->tileY = (int)r.my, e->tileZ = (int)r.mz;
            e->mx = e->my = e->mz = 0;
            if (e->woolColor >= g_paintingArtCount) e->woolColor = 0;
            setPaintingDir(*e, r.fuse & 3);
        }
        if (entityIsThrowable(e->type) || e->type == ENT_FISH) e->type = ENT_NONE;
    }
    std::free(c->entityData);
    c->entityData = nullptr;
    c->entityBytes = 0;
    c->flags |= CHUNK_NEEDS_SAVE;
}

void Game::onChunkUnload(Chunk* c) {
    if (openTile) {
        for (TileEntity* t = c->tiles; t; t = t->next)
            if (t == openTile) openTile = nullptr;
    }
    // A ridden vehicle and its rider unload together, wherever the rider stands.
    auto inChunk = [&](const Entity& e) {
        const Entity* at = &e;
        if (e.vehicle >= 0 && e.vehicle < MAX_ENTITIES && ents[e.vehicle].rider == &e - ents) at = &ents[e.vehicle];
        return (ifloor(at->x) >> 4) == c->cx && (ifloor(at->z) >> 4) == c->cz;
    };
    int count = 0;
    for (const Entity& e : ents)
        if (e.type != ENT_NONE && inChunk(e)) count += 1 + extraRecordCount(e);
    std::free(c->entityData);
    c->entityData = nullptr;
    c->entityBytes = 0;
    if (!count) return;
    c->entityData = (uint8_t*)std::malloc(count * sizeof(EntityRecord));
    int n = 0;
    for (Entity& e : ents) {
        if (e.type == ENT_NONE || !inChunk(e)) continue;
        if (entityTarget == &e) entityTarget = nullptr;
        int idx = (int)(&e - ents);
        if (idx == openCart) openCart = -1;
        if (idx == riding) {
            riding = -1;
            player.setPosition(e.x, e.box().y0 + e.height, e.z);
        }
        bool keep = e.type == ENT_ITEM || (entityIsMob(e.type) && e.health > 0) || (e.type == ENT_ARROW && e.inGround) ||
                    ((e.type == ENT_PAINTING || entityIsVehicle(e.type)) && !e.dead);
        if (&e - ents == fishHook) fishHook = -1;
        if (keep && c->entityData) {
            EntityRecord r;
            std::memset(&r, 0, sizeof(r));
            r.type = e.type;
            r.woolColor = e.type == ENT_SLIME ? e.size : entityIsVehicle(e.type) ? e.cartType : e.woolColor;
            r.flags = (uint8_t)((e.sheared ? 1 : 0) | (e.inGround ? 2 : 0) | (e.fromPlayer ? 4 : 0) |
                                (e.tamed ? 8 : 0) | (e.sitting ? 16 : 0) | (e.angry ? 32 : 0) | (e.saddled ? 64 : 0) |
                                (e.powered ? 128 : 0));
            r.x = e.x, r.y = e.y, r.z = e.z;
            r.mx = e.mx, r.my = e.my, r.mz = e.mz;
            if (e.type == ENT_PAINTING) r.mx = (float)e.tileX, r.my = (float)e.tileY, r.mz = (float)e.tileZ;
            r.yaw = e.yaw;
            r.health = e.health;
            r.fuse = e.fuse;
            r.age = e.age;
            r.eggTimer = entityIsVehicle(e.type) ? e.fuel : e.eggTimer;
            r.itemId = e.item.id;
            r.itemCount = e.item.count;
            r.itemDamage = e.item.damage;
            r.pickupDelay = e.pickupDelay;
            std::memcpy(c->entityData + n * sizeof(EntityRecord), &r, sizeof(r));
            ++n;
            n += writeExtraRecords(e, c->entityData + n * sizeof(EntityRecord));
        }
        e = Entity();
    }
    c->entityBytes = (uint32_t)(n * sizeof(EntityRecord));
    c->flags |= CHUNK_NEEDS_SAVE;
    if (!n) {
        std::free(c->entityData);
        c->entityData = nullptr;
    }
}

// SpawnerAnimals.performWorldGenSpawning: passive mobs appear with new terrain.
void Game::onChunkPopulated(Chunk* c) {
    int x0 = c->cx * 16 + 8, z0 = c->cz * 16 + 8;
    int biome = biomeAt(x0 + 8, z0 + 8);
    if (biome == BIOME_DESERT || biome == BIOME_ICE_DESERT) return;
    bool wolves = biome == BIOME_FOREST || biome == BIOME_TAIGA;
    while (rand.nextFloat() < 0.1f) {
        int w = rand.nextInt(wolves ? 42 : 40);
        uint8_t type = w < 12 ? ENT_SHEEP : w < 22 ? ENT_PIG : w < 32 ? ENT_CHICKEN : w < 40 ? ENT_COW : ENT_WOLF;
        int x = x0 + rand.nextInt(16), z = z0 + rand.nextInt(16);
        int group = 1 + rand.nextInt(4);
        for (int k = 0; k < group; ++k) {
            int y = world.topSolidAt(x, z);
            if (y > 0 && world.get(x, y - 1, z) == GRASS && !blockIsNormalCube(world.get(x, y, z)) &&
                !blockIsNormalCube(world.get(x, y + 1, z))) {
                Entity* e = spawn(type, x + 0.5f, (float)y, z + 0.5f);
                if (e) e->yaw = rand.nextFloat() * 360.0f;
            }
            x += rand.nextInt(5) - rand.nextInt(5);
            z += rand.nextInt(5) - rand.nextInt(5);
            if ((x >> 4) != c->cx && (x >> 4) != c->cx + 1) x = x0 + rand.nextInt(16);
            if ((z >> 4) != c->cz && (z >> 4) != c->cz + 1) z = z0 + rand.nextInt(16);
        }
    }
}

int Game::biomeAt(int x, int z) const {
    const Chunk* c = world.chunk(x >> 4, z >> 4);
    if (!c) return -1;
    int i = (x & 15) * 16 + (z & 15);
    return biomeFromClimate(c->temperature[i] / 255.0f, c->humidity[i] / 255.0f);
}

// Chunk.getRandomWithSeed(987234911L).nextInt(10) == 0, with Java's 32-bit products.
bool Game::slimeChunk(int cx, int cz) const {
    uint32_t x = (uint32_t)cx, z = (uint32_t)cz;
    uint64_t s = (uint64_t)world.seed + (uint64_t)(int64_t)(int32_t)(x * x * 4987142u) +
                 (uint64_t)(int64_t)(int32_t)(x * 5947611u) + (uint64_t)((int64_t)(int32_t)(z * z) * 4392871LL) +
                 (uint64_t)(int64_t)(int32_t)(z * 389711u);
    JRandom r((int64_t)(s ^ 987234911ULL));
    return r.nextInt(10) == 0;
}

// ------------------------------------------------------------------ entities

Entity* Game::spawn(uint8_t type, float x, float y, float z) {
    for (Entity& e : ents) {
        if (e.type != ENT_NONE) continue;
        e = Entity();
        e.type = type;
        e.width = MOBS[type].width;
        e.height = MOBS[type].height;
        e.health = MOBS[type].health;
        e.stepHeight = entityIsMob(type) ? 0.5f : 0.0f;
        e.makesSteps = entityIsMob(type) && type != ENT_GHAST;
        e.setPosition(x, y, z);
        if (type == ENT_SHEEP) {
            int r = rand.nextInt(100);
            e.woolColor = r < 5 ? 15 : r < 10 ? 7 : r < 15 ? 8 : r < 18 ? 12 : rand.nextInt(500) == 0 ? 6 : 0;
        }
        if (type == ENT_CHICKEN) e.eggTimer = rand.nextInt(6000) + 6000;
        if (type == ENT_TNT) e.fuse = 80;
        if (type == ENT_SLIME) {
            setSlimeSize(e, 1 << rand.nextInt(3));
            e.jumpDelay = (int16_t)(rand.nextInt(20) + 10);
        }
        if (type == ENT_SQUID) e.phaseSpeed = 1.0f / (rand.nextFloat() + 1.0f) * 0.2f;
        return &e;
    }
    return nullptr;
}

void Game::dropItem(const ItemStack& s, float x, float y, float z, bool fromPlayer) {
    if (s.empty()) return;
    Entity* e = spawn(ENT_ITEM, x, y, z);
    if (!e) return;
    e->item = s;
    e->spin = rand.nextFloat() * 360.0f;
    if (fromPlayer) {
        const float f = 0.3f;
        e->mx = -std::sin(player.yaw * DEG) * std::cos(player.pitch * DEG) * f;
        e->mz = std::cos(player.yaw * DEG) * std::cos(player.pitch * DEG) * f;
        e->my = -std::sin(player.pitch * DEG) * f + 0.1f;
        float a = rand.nextFloat() * PI * 2.0f, d = 0.02f * rand.nextFloat();
        e->mx += std::cos(a) * d;
        e->my += (rand.nextFloat() - rand.nextFloat()) * 0.1f;
        e->mz += std::sin(a) * d;
        e->pickupDelay = 40;
    } else {
        e->mx = rand.nextFloat() * 0.2f - 0.1f;
        e->my = 0.2f;
        e->mz = rand.nextFloat() * 0.2f - 0.1f;
        e->pickupDelay = 10;
    }
}

void Game::dropHeld(bool all) {
    ItemStack& h = inv.held();
    if (h.empty()) return;
    ItemStack s = h;
    if (!all) s.count = 1;
    if (mode == MODE_SURVIVAL || all) {
        h.count = (uint8_t)(h.count - s.count);
        if (!h.count) h.clear();
    }
    dropItem(s, player.x, player.y + Player::EYE - 0.3f, player.z, true);
}

// ------------------------------------------------------------------ sounds

void Game::playSound(const char* name, float x, float y, float z, float volume, float pitch) {
    if (!name || soundCount >= MAX_SOUNDS) return;
    sounds[soundCount++] = {name, x, y, z, volume, pitch, false};
}

void Game::playUiSound(const char* name, float volume, float pitch) {
    if (!name || soundCount >= MAX_SOUNDS) return;
    sounds[soundCount++] = {name, 0, 0, 0, volume, pitch, true};
}

namespace {

// EntityLiving.getLivingSound / getHurtSound / getDeathSound / getSoundVolume.
struct MobSounds {
    const char *living, *hurt, *death;
    float volume;
};

const MobSounds& mobSounds(uint8_t type) {
    static const MobSounds NONE{nullptr, nullptr, nullptr, 1.0f};
    static const MobSounds ZOMBIE{"mob.zombie", "mob.zombiehurt", "mob.zombiedeath", 1.0f};
    static const MobSounds SKELETON{"mob.skeleton", "mob.skeletonhurt", "mob.skeletonhurt", 1.0f};
    static const MobSounds CREEPER{nullptr, "mob.creeper", "mob.creeperdeath", 1.0f};
    static const MobSounds SPIDER{"mob.spider", "mob.spider", "mob.spiderdeath", 1.0f};
    static const MobSounds PIG{"mob.pig", "mob.pig", "mob.pigdeath", 1.0f};
    static const MobSounds COW{"mob.cow", "mob.cowhurt", "mob.cowhurt", 0.4f};
    static const MobSounds SHEEP{"mob.sheep", "mob.sheep", "mob.sheep", 1.0f};
    static const MobSounds CHICKEN{"mob.chicken", "mob.chickenhurt", "mob.chickenhurt", 1.0f};
    static const MobSounds SLIME{nullptr, "mob.slime", "mob.slime", 0.6f};
    static const MobSounds SQUID{nullptr, nullptr, nullptr, 0.4f};
    static const MobSounds WOLF{nullptr, "mob.wolf.hurt", "mob.wolf.death", 0.4f};
    static const MobSounds GHAST{"mob.ghast.moan", "mob.ghast.scream", "mob.ghast.death", 10.0f};
    static const MobSounds PIGZOMBIE{"mob.zombiepig.zpig", "mob.zombiepig.zpighurt", "mob.zombiepig.zpigdeath", 1.0f};
    switch (type) {
        case ENT_GHAST: return GHAST;
        case ENT_PIGZOMBIE: return PIGZOMBIE;
        case ENT_SLIME: return SLIME;
        case ENT_SQUID: return SQUID;
        case ENT_WOLF: return WOLF;
        case ENT_ZOMBIE: return ZOMBIE;
        case ENT_SKELETON: return SKELETON;
        case ENT_CREEPER: return CREEPER;
        case ENT_SPIDER: return SPIDER;
        case ENT_PIG: return PIG;
        case ENT_COW: return COW;
        case ENT_SHEEP: return SHEEP;
        case ENT_CHICKEN: return CHICKEN;
        default: return NONE;
    }
}

}  // namespace

float Game::mobPitch() { return (soundRand.nextFloat() - soundRand.nextFloat()) * 0.2f + 1.0f; }

// Entity.moveEntity footsteps and Entity.handleWaterMovement splashes queued by the physics.
void Game::bodySounds(Body& b) {
    if (b.stepPending) {
        b.stepPending = false;
        uint8_t id = b.stepBlock;
        if (world.get(b.stepX, b.stepY + 1, b.stepZ) == SNOW_LAYER) id = SNOW_LAYER;
        if (!blockIsLiquid(id)) {
            const StepSound& s = blockStepSound(id);
            playSoundAt(b, s.step, s.volume * 0.15f, s.pitch);
        }
    }
    if (b.splash > 0) {
        playSoundAt(b, "random.splash", b.splash, 1.0f + (soundRand.nextFloat() - soundRand.nextFloat()) * 0.4f);
        b.splash = 0;
        float y = (float)ifloor(b.y) + 1.0f;
        for (int pass = 0; pass < 2; ++pass)
            for (int i = 0; i < 1.0f + b.width * 20.0f; ++i) {
                float ox = (particles.rnd() * 2.0f - 1.0f) * b.width, oz = (particles.rnd() * 2.0f - 1.0f) * b.width;
                if (pass == 0)
                    particles.spawn(world, PART_BUBBLE, b.x + ox, y, b.z + oz, b.mx, b.my - particles.rnd() * 0.2f, b.mz);
                else
                    particles.spawn(world, PART_SPLASH, b.x + ox, y, b.z + oz, b.mx, b.my, b.mz);
            }
    }
}

void Game::blockSound(int x, int y, int z, uint8_t id, bool broken) {
    const StepSound& s = blockStepSound(id);
    playSound(broken ? s.broken : s.step, x + 0.5f, y + 0.5f, z + 0.5f, (s.volume + 1.0f) / 2.0f, s.pitch * 0.8f);
}

bool Game::hurtPlayer(int damage, float fromX, float fromZ, bool knock, Entity* attacker) {
    if (mode != MODE_SURVIVAL || dead || damage <= 0) return false;
    if (sleeping) wakeUp(false);
    if (attacker) alertWolves(*attacker, false);
    bool fresh = true;
    if (hurtResist > 10) {
        if (damage <= lastDamage) return false;
        int d = damage - lastDamage;
        lastDamage = damage;
        damage = d;
        fresh = false;
    } else {
        lastDamage = damage;
        prevHealth = health;
        hurtResist = 20;
        hurtTime = 10;
    }
    // EntityPlayer.damageEntity: armor absorbs part of the damage, keeping the remainder for the next hit.
    static int remainder = 0;
    int armorPts = inv.armorValue();
    int j = damage * (25 - armorPts) + remainder;
    inv.damageArmor(damage);
    damage = j / 25;
    remainder = j % 25;
    health -= damage;
    if (fresh) playSoundAt(player, "random.hurt", 1.0f, mobPitch());
    if (fresh) {
        float dx = fromX - player.x, dz = fromZ - player.z;
        attackedAtYaw = knock && (dx != 0 || dz != 0) ? std::atan2(dz, dx) * 57.29578f - player.yaw
                                                      : (float)soundRand.nextInt(2) * 180.0f;
    }
    if (fresh && knock) {
        float dx = fromX - player.x, dz = fromZ - player.z;
        float f = std::sqrt(dx * dx + dz * dz);
        if (f > 0.0001f) {
            player.mx = player.mx / 2 - dx / f * 0.4f;
            player.my = player.my / 2 + 0.4f;
            player.mz = player.mz / 2 - dz / f * 0.4f;
            if (player.my > 0.4f) player.my = 0.4f;
        }
    }
    if (health <= 0) {
        health = 0;
        dead = true;
        deathTime = 0;
        for (int i = 0; i < 40; ++i) {
            ItemStack& s = i < 36 ? inv.main[i] : inv.armor[i - 36];
            if (s.empty()) continue;
            Entity* e = spawn(ENT_ITEM, player.x, player.y + 1.2f, player.z);
            if (e) {
                e->item = s;
                float f = rand.nextFloat() * 0.5f, a = rand.nextFloat() * PI * 2.0f;
                e->mx = -std::sin(a) * f;
                e->mz = std::cos(a) * f;
                e->my = 0.2f;
                e->pickupDelay = 40;
            }
            s.clear();
        }
    }
    return true;
}

bool Game::hurtEntity(Entity& e, int damage, float fromX, float fromZ, bool byPlayer, Entity* attacker) {
    if (e.type == ENT_PAINTING) {
        // EntityPainting.attackEntityFrom
        if (e.dead) return false;
        e.dead = true;
        if (mode == MODE_SURVIVAL) dropItem(ItemStack(I_PAINTING), e.x, e.y, e.z, false);
        return true;
    }
    if (entityIsVehicle(e.type)) return hurtVehicle(e, damage);
    if (e.type == ENT_FIREBALL) {
        // EntityFireball.attackEntityFrom: sent off along the attacker's view
        if (!byPlayer || e.dead) return false;
        float dx, dy, dz;
        player.look(dx, dy, dz);
        e.mx = dx, e.my = dy, e.mz = dz;
        e.swimX = dx * 0.1f, e.swimY = dy * 0.1f, e.swimZ = dz * 0.1f;
        return true;
    }
    if (!entityIsMob(e.type) || e.health <= 0) return false;
    if (byPlayer && e.type == ENT_PIGZOMBIE) angerPigZombies(e);
    if (e.type == ENT_WOLF) {
        e.sitting = false;
        if (attacker) damage = (damage + 1) / 2;
    }
    e.limbAmount = 1.5f;
    bool knock = true;
    if (e.hurtResist > 10) {
        if (damage <= e.lastDamage) return false;
        e.health = (int16_t)(e.health - (damage - e.lastDamage));
        e.lastDamage = (int16_t)damage;
        knock = false;
    } else {
        e.lastDamage = (int16_t)damage;
        e.hurtResist = 20;
        e.health = (int16_t)(e.health - damage);
        e.hurtTime = 10;
    }
    if (knock) {
        float dx = fromX - e.x, dz = fromZ - e.z;
        float f = std::sqrt(dx * dx + dz * dz);
        if (f > 0.0001f) {
            e.mx = e.mx / 2 - dx / f * 0.4f;
            e.my = e.my / 2 + 0.4f;
            e.mz = e.mz / 2 - dz / f * 0.4f;
            if (e.my > 0.4f) e.my = 0.4f;
        }
    }
    if (byPlayer && entityIsHostile(e.type)) {
        e.targeting = true;
        e.target = -1;
    }
    int16_t from = attacker && attacker != &e ? (int16_t)(attacker - ents) : (int16_t)-1;
    if (from >= 0 && entityIsHostile(e.type) && e.type != ENT_SLIME) e.target = from;
    if (e.type == ENT_WOLF && (byPlayer || from >= 0)) {
        // EntityWolf.attackEntityFrom
        if (!e.tamed && !e.angry) {
            for (Entity& w : ents) {
                if (w.type != ENT_WOLF || w.tamed || w.health <= 0) continue;
                if (std::fabs(w.x - e.x) > 16.5f || std::fabs(w.y - e.y) > 4.5f || std::fabs(w.z - e.z) > 16.5f) continue;
                if (&w != &e && (w.targeting || targetOf(w))) continue;
                if (byPlayer) {
                    w.angry = true;
                    w.targeting = true;
                } else {
                    w.target = from;
                }
            }
        } else if (!(byPlayer && e.tamed)) {
            if (byPlayer) {
                e.targeting = true;
            } else {
                e.target = from;
                e.targeting = false;
            }
        }
    }
    if (knock) {
        const MobSounds& ms = mobSounds(e.type);
        playSoundAt(e, e.health <= 0 ? ms.death : ms.hurt, ms.volume, mobPitch());
    }
    if (e.health <= 0) killMob(e, attacker);
    return true;
}

void Game::killMob(Entity& e, Entity* killer) {
    e.splits = e.type == ENT_SLIME && e.health == 0 && e.size > 1;
    e.health = 0;
    e.deathTime = 1;
    int n = rand.nextInt(3);
    auto drop = [&](const ItemStack& s) { dropItem(s, e.x, e.y + 0.5f, e.z, false); };
    switch (e.type) {
        case ENT_ZOMBIE:
        case ENT_CHICKEN:
            for (int i = 0; i < n; ++i) drop(ItemStack(I_FEATHER));
            break;
        case ENT_SKELETON: {
            for (int i = 0; i < n; ++i) drop(ItemStack(I_ARROW));
            int b = rand.nextInt(3);
            for (int i = 0; i < b; ++i) drop(ItemStack(I_BONE));
            break;
        }
        case ENT_CREEPER:
            for (int i = 0; i < n; ++i) drop(ItemStack(I_GUNPOWDER));
            // EntityCreeper.onDeath
            if (killer && killer->type == ENT_SKELETON) drop(ItemStack(I_RECORD_13 + rand.nextInt(2)));
            break;
        case ENT_SPIDER:
            for (int i = 0; i < n; ++i) drop(ItemStack(I_STRING));
            break;
        case ENT_PIG:
            for (int i = 0; i < n; ++i) drop(ItemStack(e.fire > 0 ? I_PORK_COOKED : I_PORK_RAW));
            break;
        case ENT_COW:
            for (int i = 0; i < n; ++i) drop(ItemStack(I_LEATHER));
            break;
        case ENT_SHEEP:
            if (!e.sheared) drop(ItemStack(WOOL, 1, e.woolColor));
            break;
        case ENT_SLIME:
            if (e.size == 1)
                for (int i = 0; i < n; ++i) drop(ItemStack(I_SLIMEBALL));
            break;
        case ENT_SQUID:
            for (int i = 0; i < n + 1; ++i) drop(ItemStack(I_DYE, 1, 0));
            break;
        case ENT_GHAST:
            for (int i = 0; i < n; ++i) drop(ItemStack(I_GUNPOWDER));
            break;
        case ENT_PIGZOMBIE:
            for (int i = 0; i < n; ++i) drop(ItemStack(I_PORK_COOKED));
            break;
        default: break;
    }
}

// Entity.setEntityDead after the death animation; big slimes split into four smaller ones.
void Game::removeMob(Entity& e) {
    e.dead = true;
    if (e.type != ENT_SLIME || !e.splits) return;
    int size = e.size;
    float x = e.x, y = e.y, z = e.z;
    for (int i = 0; i < 4; ++i) {
        float ox = ((i % 2) - 0.5f) * size / 4.0f, oz = ((i / 2) - 0.5f) * size / 4.0f;
        Entity* s = spawn(ENT_SLIME, x + ox, y + 0.5f, z + oz);
        if (!s) break;
        setSlimeSize(*s, size / 2);
        s->yaw = s->prevYaw = rand.nextFloat() * 360.0f;
    }
}

void Game::shootArrow(Entity* shooter, float x, float y, float z, float dx, float dy, float dz, float speed,
                      float spread, bool fromPlayer) {
    Entity* a = spawn(ENT_ARROW, x, y, z);
    if (!a) return;
    a->target = shooter ? (int16_t)(shooter - ents) : (int16_t)-1;
    float len = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (len < 1e-4f) return;
    dx = dx / len + randGaussian(rand) * 0.0075f * spread;
    dy = dy / len + randGaussian(rand) * 0.0075f * spread;
    dz = dz / len + randGaussian(rand) * 0.0075f * spread;
    a->mx = dx * speed;
    a->my = dy * speed;
    a->mz = dz * speed;
    a->fromPlayer = fromPlayer;
    a->yaw = a->prevYaw = std::atan2(dx, dz) / DEG;
    a->pitch = std::atan2(dy, std::sqrt(dx * dx + dz * dz)) / DEG;
}

void Game::explode(float cx, float cy, float cz, float power, bool flaming) {
    // Explosion.doExplosionA
    static int list[4096][3];
    int count = 0;
    for (int i = 0; i < 16; ++i)
        for (int j = 0; j < 16; ++j)
            for (int k = 0; k < 16; ++k) {
                if (i != 0 && i != 15 && j != 0 && j != 15 && k != 0 && k != 15) continue;
                float dx = i / 15.0f * 2 - 1, dy = j / 15.0f * 2 - 1, dz = k / 15.0f * 2 - 1;
                float d = std::sqrt(dx * dx + dy * dy + dz * dz);
                dx /= d, dy /= d, dz /= d;
                float f = power * (0.7f + rand.nextFloat() * 0.6f);
                float x = cx, y = cy, z = cz;
                const float step = 0.3f;
                for (; f > 0; f -= step * 0.75f) {
                    int bx = ifloor(x), by = ifloor(y), bz = ifloor(z);
                    uint8_t id = world.get(bx, by, bz);
                    if (id) f -= (explosionResistance(id) + 0.3f) * step;
                    if (f > 0 && (id || flaming) && (unsigned)by < 128 && count < 4096) {
                        bool dup = false;
                        for (int n = count - 1; n >= 0 && n >= count - 8; --n)
                            if (list[n][0] == bx && list[n][1] == by && list[n][2] == bz) dup = true;
                        if (!dup) {
                            list[count][0] = bx, list[count][1] = by, list[count][2] = bz;
                            ++count;
                        }
                    }
                    x += dx * step, y += dy * step, z += dz * step;
                }
            }
    // Entity damage within 2 * power.
    float radius = power * 2.0f;
    auto impact = [&](float ex, float ey, float ez) {
        float dx = ex - cx, dy = ey - cy, dz = ez - cz;
        float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        return d / radius;
    };
    for (Entity& e : ents) {
        if (e.type == ENT_NONE) continue;
        float d = impact(e.x, e.y, e.z);
        if (d > 1.0f) continue;
        float imp = 1.0f - d;
        if (e.type == ENT_ITEM) {
            e.type = ENT_NONE;
            continue;
        }
        if (e.type == ENT_TNT) continue;
        int dmg = (int)((imp * imp + imp) / 2.0f * 8.0f * power + 1.0f);
        hurtEntity(e, dmg, cx, cz, false);
        float dx = e.x - cx, dy = e.y - cy, dz = e.z - cz, l = std::sqrt(dx * dx + dy * dy + dz * dz) + 1e-4f;
        e.mx += dx / l * imp, e.my += dy / l * imp, e.mz += dz / l * imp;
    }
    float pd = impact(player.x, player.y, player.z);
    if (pd <= 1.0f) {
        float imp = 1.0f - pd;
        hurtPlayer((int)((imp * imp + imp) / 2.0f * 8.0f * power + 1.0f), cx, cz, false);
        float dx = player.x - cx, dy = player.y - cy, dz = player.z - cz;
        float l = std::sqrt(dx * dx + dy * dy + dz * dz) + 1e-4f;
        player.mx += dx / l * imp, player.my += dy / l * imp, player.mz += dz / l * imp;
    }
    // doExplosionB: remove blocks, 30% drop chance.
    playSound("random.explode", cx, cy, cz, 4.0f,
              (1.0f + (soundRand.nextFloat() - soundRand.nextFloat()) * 0.2f) * 0.7f);
    // Beta spawns an explode and a smoke particle for every position the rays reached, air included; this
    // list only has blocks, so a few extra points around the center stand in for the air.
    auto puff = [&](float x, float y, float z) {
        float dx = x - cx, dy = y - cy, dz = z - cz, d = std::sqrt(dx * dx + dy * dy + dz * dz) + 1e-4f;
        dx /= d, dy /= d, dz /= d;
        float s = 0.5f / (d / power + 0.1f) * (particles.rnd() * particles.rnd() + 0.3f);
        dx *= s, dy *= s, dz *= s;
        particles.spawn(world, PART_EXPLODE, (x + cx) / 2.0f, (y + cy) / 2.0f, (z + cz) / 2.0f, dx, dy, dz);
        particles.spawn(world, PART_SMOKE, x, y, z, dx, dy, dz);
    };
    for (int n = 0; n < count; n += 1 + count / 96)
        puff(list[n][0] + particles.rnd(), list[n][1] + particles.rnd(), list[n][2] + particles.rnd());
    for (int n = 0; n < 24; ++n)
        puff(cx + (particles.rnd() * 2 - 1) * power, cy + (particles.rnd() * 2 - 1) * power,
             cz + (particles.rnd() * 2 - 1) * power);
    for (int n = 0; n < count; ++n) {
        int x = list[n][0], y = list[n][1], z = list[n][2];
        uint8_t id = world.get(x, y, z);
        if (!id || ((g_blocks[id].flags & BF_UNBREAKABLE) && id != PORTAL)) continue;
        if (id == TNT) {
            world.setRaw(x, y, z, AIR);
            world.markBlockDirty(x, y, z);
            Entity* t = spawn(ENT_TNT, x + 0.5f, (float)y, z + 0.5f);
            if (t) t->fuse = (int16_t)(rand.nextInt(20) + 10);
            continue;
        }
        int meta = world.getMeta(x, y, z);
        if (TileEntity* t = world.tileAt(x, y, z)) {
            for (const ItemStack& s : t->items) dropItem(s, x + 0.5f, y + 0.5f, z + 0.5f, false);
            if (openTile == t) openTile = nullptr;
            world.removeTile(x, y, z);
        }
        if (id == JUKEBOX) jukeboxStop(x, y, z);
        world.setRaw(x, y, z, AIR);
        world.markBlockDirty(x, y, z);
        if (mode == MODE_SURVIVAL && rand.nextFloat() <= 0.3f) dropBlockItems(x, y, z, id, meta);
        redstoneRemoved(x, y, z, id, meta);
    }
    world.relight(ifloor(cx), ifloor(cz));
    for (int n = 0; n < count; ++n) notifyNeighbors(list[n][0], list[n][1], list[n][2], AIR);
    if (flaming) {
        for (int n = count - 1; n >= 0; --n) {
            int x = list[n][0], y = list[n][1], z = list[n][2];
            if (world.get(x, y, z) == AIR && blockIsOpaque(world.get(x, y - 1, z)) && rand.nextInt(3) == 0)
                setBlockNotify(x, y, z, FIRE);
        }
    }
}

// ------------------------------------------------------------------ blocks

bool Game::supportsPlant(uint8_t id, uint8_t below, int x, int y, int z) const {
    switch (id) {
        case DEAD_BUSH: return below == SAND;
        case MUSHROOM_BROWN:
        case MUSHROOM_RED: return blockIsOpaque(below);
        case CROPS: return below == FARMLAND;
        case CACTUS: {
            if (below != SAND && below != CACTUS) return false;
            return !blockMatSolid(world.get(x - 1, y, z)) && !blockMatSolid(world.get(x + 1, y, z)) &&
                   !blockMatSolid(world.get(x, y, z - 1)) && !blockMatSolid(world.get(x, y, z + 1));
        }
        case REED: {
            if (below == REED) return true;
            if (below != GRASS && below != DIRT && below != SAND) return false;
            return blockIsWater(world.get(x - 1, y - 1, z)) || blockIsWater(world.get(x + 1, y - 1, z)) ||
                   blockIsWater(world.get(x, y - 1, z - 1)) || blockIsWater(world.get(x, y - 1, z + 1));
        }
        case SNOW_LAYER: return blockIsOpaque(below);
        case CAKE: return blockMatSolid(below);
        default: return below == GRASS || below == DIRT || below == FARMLAND;
    }
}

void Game::dropBlockItems(int x, int y, int z, uint8_t id, int meta) {
    auto drop = [&](uint16_t item, int count, int damage) {
        for (int i = 0; i < count; ++i) {
            float ox = rand.nextFloat() * 0.7f + 0.15f, oy = rand.nextFloat() * 0.7f + 0.15f,
                  oz = rand.nextFloat() * 0.7f + 0.15f;
            dropItem(ItemStack(item, 1, damage), x + ox, y + oy, z + oz, false);
        }
    };
    switch (id) {
        case STONE: drop(COBBLESTONE, 1, 0); break;
        case GRASS:
        case FARMLAND: drop(DIRT, 1, 0); break;
        case GRAVEL: drop(rand.nextInt(10) == 0 ? I_FLINT : GRAVEL, 1, 0); break;
        case LEAVES:
            if (rand.nextInt(20) == 0) drop(SAPLING, 1, meta & 3);
            break;
        case TALL_GRASS:
            if (rand.nextInt(8) == 0) drop(I_SEEDS, 1, 0);
            break;
        case DEAD_BUSH:
        case GLASS:
        case BOOKSHELF:
        case ICE: break;
        case SNOW_LAYER: drop(I_SNOWBALL, 1, 0); break;
        case SNOW_BLOCK: drop(I_SNOWBALL, 4, 0); break;
        case CLAY: drop(I_CLAY_BALL, 4, 0); break;
        case REDSTONE_ORE:
        case REDSTONE_ORE_LIT: drop(I_REDSTONE, 4 + rand.nextInt(2), 0); break;
        case LAPIS_ORE: drop(I_DYE, 4 + rand.nextInt(5), 4); break;
        case GLOWSTONE: drop(I_GLOWSTONE_DUST, 2 + rand.nextInt(3), 0); break;
        case COAL_ORE: drop(I_COAL, 1, 0); break;
        case DIAMOND_ORE: drop(I_DIAMOND, 1, 0); break;
        case CROPS:
            if (meta == 7) drop(I_WHEAT, 1, 0);
            for (int i = 0; i < 3; ++i)
                if (rand.nextInt(15) <= meta) drop(I_SEEDS, 1, 0);
            break;
        case REED: drop(I_REEDS, 1, 0); break;
        case DOUBLE_SLAB: drop(SLAB, 2, meta); break;
        case SLAB:
        case WOOL:
        case LOG: drop(id, 1, meta); break;
        case SAPLING: drop(id, 1, meta & 3); break;
        case WOOD_STAIRS: drop(PLANKS, 1, 0); break;
        case COBBLE_STAIRS: drop(COBBLESTONE, 1, 0); break;
        case FURNACE_LIT: drop(FURNACE, 1, 0); break;
        case WOOD_DOOR:
            if (!(meta & 8)) drop(I_WOOD_DOOR, 1, 0);
            break;
        case IRON_DOOR:
            if (!(meta & 8)) drop(I_IRON_DOOR, 1, 0);
            break;
        case BED:
            if (!(meta & 8)) drop(I_BED, 1, 0);
            break;
        case WEB: drop(I_STRING, 1, 0); break;
        case SIGN_POST:
        case WALL_SIGN: drop(I_SIGN, 1, 0); break;
        case REDSTONE_WIRE: drop(I_REDSTONE, 1, 0); break;
        case REDSTONE_TORCH_OFF:
        case REDSTONE_TORCH_ON: drop(REDSTONE_TORCH_ON, 1, 0); break;
        case REPEATER_OFF:
        case REPEATER_ON: drop(I_REPEATER, 1, 0); break;
        case PISTON_HEAD:
        case PISTON_MOVING:
        case FIRE:
        case CAKE:
        case MOB_SPAWNER: break;
        case WATER:
        case WATER_FLOWING:
        case LAVA:
        case LAVA_FLOWING:
        case BEDROCK:
        case PORTAL: break;
        default: drop(id, 1, 0); break;
    }
}

void Game::markLeavesForDecay(int x, int y, int z) {
    for (int dx = -4; dx <= 4; ++dx)
        for (int dy = -4; dy <= 4; ++dy)
            for (int dz = -4; dz <= 4; ++dz)
                if (world.get(x + dx, y + dy, z + dz) == LEAVES) {
                    int m = world.getMeta(x + dx, y + dy, z + dz);
                    if (!(m & 8)) world.setMetaRaw(x + dx, y + dy, z + dz, m | 8);
                }
}

// Pops blocks that depended on (x, y, z): plants and torches above, wall torches and ladders beside, doors.
void Game::popUnsupported(int x, int y, int z) {
    bool drops = mode == MODE_SURVIVAL;
    for (int yy = y + 1; yy < World::SY; ++yy) {
        uint8_t above = world.get(x, yy, z);
        int am = world.getMeta(x, yy, z);
        bool standingTorch = above == TORCH && (am == 5 || am == 0);
        bool door = blockIsDoor(above) && !(am & 8);
        if (!(g_blocks[above].flags & BF_NEEDS_FLOOR) && !standingTorch && !door) break;
        if (above == REED || above == CACTUS || (g_blocks[above].flags & BF_NEEDS_FLOOR)) {
            uint8_t below = world.get(x, yy - 1, z);
            if (yy - 1 != y && supportsPlant(above, below, x, yy, z)) break;
        }
        if (door) {
            world.setRaw(x, yy + 1, z, AIR);
            world.markBlockDirty(x, yy + 1, z);
        }
        world.setRaw(x, yy, z, AIR);
        world.markBlockDirty(x, yy, z);
        if (drops) dropBlockItems(x, yy, z, above, am);
        if (above != REED && above != CACTUS) break;
    }
    static const int SIDE[4][4] = {{1, 0, 0, 1}, {-1, 0, 0, 2}, {0, 0, 1, 3}, {0, 0, -1, 4}};
    static const int LADDER_META[4] = {5, 4, 3, 2};
    for (int i = 0; i < 4; ++i) {
        int tx = x + SIDE[i][0], tz = z + SIDE[i][2];
        uint8_t id = world.get(tx, y, tz);
        int m = world.getMeta(tx, y, tz);
        if ((id == TORCH && m == SIDE[i][3]) || (id == LADDER && m == LADDER_META[i])) {
            world.setRaw(tx, y, tz, AIR);
            world.markBlockDirty(tx, y, tz);
            if (drops) dropBlockItems(tx, y, tz, id, m);
        }
    }
}

void Game::settleFalling(int x, int y, int z) {
    int write = y;
    while (write > 0) {
        uint8_t b = world.get(x, write - 1, z);
        if (b != AIR && !blockIsLiquid(b) && !blockIsReplaceable(b)) break;
        --write;
    }
    for (int read = y + 1;; ++read) {
        uint8_t id = world.get(x, read, z);
        if (!(g_blocks[id].flags & BF_FALLS)) break;
        world.setRaw(x, read, z, AIR);
        world.markBlockDirty(x, read, z);
        world.setRaw(x, write, z, id);
        world.markBlockDirty(x, write, z);
        ++write;
    }
}

bool Game::breakBlock(int x, int y, int z, bool drops) {
    uint8_t id = world.get(x, y, z);
    if (id == AIR || (g_blocks[id].flags & BF_UNBREAKABLE) || blockIsLiquid(id)) return false;
    int meta = world.getMeta(x, y, z);
    blockSound(x, y, z, id, true);
    particles.blockDestroy(world, x, y, z, id, meta);
    if (TileEntity* t = world.tileAt(x, y, z)) {
        for (const ItemStack& s : t->items) {
            if (s.empty()) continue;
            dropItem(s, x + 0.5f, y + 0.5f, z + 0.5f, false);
        }
        if (openTile == t) openTile = nullptr;
        world.removeTile(x, y, z);
    }
    if (id == JUKEBOX) jukeboxStop(x, y, z);
    uint8_t replacement = AIR;
    if (id == ICE && mode == MODE_SURVIVAL) {
        uint8_t below = world.get(x, y - 1, z);
        if (blockMatSolid(below) || blockIsLiquid(below)) replacement = WATER_FLOWING;
    }
    world.setRaw(x, y, z, replacement);
    world.markBlockDirty(x, y, z);
    if (blockIsDoor(id)) {
        int oy = (meta & 8) ? y - 1 : y + 1;
        if (world.get(x, oy, z) == id) {
            world.setRaw(x, oy, z, AIR);
            world.markBlockDirty(x, oy, z);
        }
        if (meta & 8) meta = 0;
    }
    if (id == BED) {
        static const int HEAD[4][2] = {{0, 1}, {-1, 0}, {0, -1}, {1, 0}};
        int s = (meta & 8) ? -1 : 1;
        int ox = x + HEAD[meta & 3][0] * s, oz = z + HEAD[meta & 3][1] * s;
        if (world.get(ox, y, oz) == BED) {
            world.setRaw(ox, y, oz, AIR);
            world.markBlockDirty(ox, y, oz);
            world.relight(ox, oz);
            notifyNeighbors(ox, y, oz, BED);
        }
        if (sleeping) wakeUp(false);
        meta &= 7;
    }
    if (drops) dropBlockItems(x, y, z, id, meta);
    popUnsupported(x, y, z);
    if (id == LOG || id == LEAVES) markLeavesForDecay(x, y, z);
    if (g_blocks[world.get(x, y + 1, z)].flags & BF_FALLS) settleFalling(x, y, z);
    world.relight(x, z);
    if (replacement != AIR) onBlockAdded(x, y, z, replacement);
    redstoneRemoved(x, y, z, id, meta);
    notifyNeighbors(x, y, z, id);
    if (blockIsDoor(id)) notifyNeighbors(x, (meta & 8) ? y - 1 : y + 1, z, id);
    return true;
}

void Game::damageHeld(int amount) {
    if (mode != MODE_SURVIVAL) return;
    ItemStack& h = inv.held();
    if (h.empty() || !itemDef(h.id).maxDamage) return;
    h.damage = (uint16_t)(h.damage + amount);
    if (h.damage > itemDef(h.id).maxDamage) h.clear();
}

bool Game::placeHeld(const RayHit& hit) {
    ItemStack& item = inv.held();
    if (item.empty()) return false;
    static const int OFF[6][3] = {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};
    uint16_t placeId = item.id < 256 ? item.id : itemDef(item.id).places;
    if (!placeId || item.id == I_SEEDS) return false;
    uint8_t target = world.get(hit.x, hit.y, hit.z);
    auto consume = [&]() {
        if (mode == MODE_SURVIVAL && --item.count == 0) item.clear();
    };
    if (placeId == SLAB && target == SLAB && hit.face == 1 && world.getMeta(hit.x, hit.y, hit.z) == item.damage) {
        world.setBlock(hit.x, hit.y, hit.z, DOUBLE_SLAB, item.damage);
        notifyNeighbors(hit.x, hit.y, hit.z, DOUBLE_SLAB);
        blockSound(hit.x, hit.y, hit.z, SLAB, false);
        consume();
        return true;
    }
    int x, y, z, face = hit.face;
    if (target == TALL_GRASS || target == SNOW_LAYER || target == DEAD_BUSH) {
        x = hit.x, y = hit.y, z = hit.z;
        face = 1;
    } else {
        x = hit.x + OFF[face][0];
        y = hit.y + OFF[face][1];
        z = hit.z + OFF[face][2];
    }
    if ((unsigned)y >= (unsigned)World::SY || !world.isLoaded(x, z)) return false;
    uint8_t existing = world.get(x, y, z);
    if (!blockIsReplaceable(existing)) return false;

    int meta = item.id < 256 ? item.damage : 0;
    if (placeId == WOOL || placeId == LOG || placeId == SLAB || placeId == SAPLING || placeId == LEAVES ||
        placeId == TALL_GRASS)
        meta = item.damage & 15;
    else if (item.id < 256)
        meta = 0;
    uint8_t below = world.get(x, y - 1, z);
    int quarter = ifloor(player.yaw * 4.0f / 360.0f + 0.5f) & 3;
    auto cubeAt = [&](int ax, int ay, int az) { return blockIsNormalCube(world.get(ax, ay, az)); };
    switch (placeId) {
        case REDSTONE_WIRE:
        case REPEATER_OFF:
        case STONE_PLATE:
        case WOOD_PLATE:
        case RAIL:
        case POWERED_RAIL:
        case DETECTOR_RAIL:
            if (!blockIsNormalCube(below)) return false;
            if (placeId == REPEATER_OFF) meta = (quarter + 2) % 4;
            break;
        case LEVER: {
            // BlockLever.canPlaceBlockOnSide / onBlockPlaced
            if (face == 1 && cubeAt(x, y - 1, z)) meta = 5 + rand.nextInt(2);
            else if (face == 2 && cubeAt(x, y, z + 1)) meta = 4;
            else if (face == 3 && cubeAt(x, y, z - 1)) meta = 3;
            else if (face == 4 && cubeAt(x + 1, y, z)) meta = 2;
            else if (face == 5 && cubeAt(x - 1, y, z)) meta = 1;
            else return false;
            break;
        }
        case STONE_BUTTON: {
            if (face == 2 && cubeAt(x, y, z + 1)) meta = 4;
            else if (face == 3 && cubeAt(x, y, z - 1)) meta = 3;
            else if (face == 4 && cubeAt(x + 1, y, z)) meta = 2;
            else if (face == 5 && cubeAt(x - 1, y, z)) meta = 1;
            else return false;
            break;
        }
        case DISPENSER: {
            static const int FACING[4] = {2, 5, 3, 4};
            meta = FACING[quarter];
            break;
        }
        case PISTON:
        case STICKY_PISTON: {
            // BlockPistonBase.determineOrientation
            static const int FACING[4] = {2, 5, 3, 4};
            meta = FACING[quarter];
            if (std::fabs(player.x - (float)x) < 2.0f && std::fabs(player.z - (float)z) < 2.0f) {
                float eye = player.y + 1.82f;
                if (eye - y > 2.0f) meta = 1;
                else if (y - eye > 0.0f) meta = 0;
            }
            break;
        }
        case REDSTONE_TORCH_ON:
        case TORCH: {
            static const int FACE_META[6] = {0, 5, 4, 3, 2, 1};
            if (face == 0) return false;
            meta = FACE_META[face];
            if (!blockIsNormalCube(world.get(x - OFF[face][0], y - OFF[face][1], z - OFF[face][2]))) return false;
            break;
        }
        case LADDER: {
            if (face == 2 && blockIsNormalCube(world.get(x, y, z + 1))) meta = 2;
            else if (face == 3 && blockIsNormalCube(world.get(x, y, z - 1))) meta = 3;
            else if (face == 4 && blockIsNormalCube(world.get(x + 1, y, z))) meta = 4;
            else if (face == 5 && blockIsNormalCube(world.get(x - 1, y, z))) meta = 5;
            else return false;
            break;
        }
        case WOOD_STAIRS:
        case COBBLE_STAIRS: {
            static const int STAIR[4] = {2, 1, 3, 0};
            meta = STAIR[quarter];
            break;
        }
        case FURNACE:
        case CHEST: {
            static const int FACING[4] = {2, 5, 3, 4};
            meta = FACING[quarter];
            break;
        }
        case PUMPKIN:
        case JACK_O_LANTERN: meta = ifloor(player.yaw * 4.0f / 360.0f + 2.5f) & 3; break;
        case TRAPDOOR: {
            // BlockTrapDoor.canPlaceBlockOnSide / onBlockPlaced
            if (face < 2) return false;
            static const int FACE_META[6] = {0, 0, 0, 1, 2, 3};
            meta = FACE_META[face];
            if (!blockCanStay(x, y, z, TRAPDOOR, meta)) return false;
            break;
        }
        case WOOD_DOOR:
        case IRON_DOOR: {
            if (face != 1 || y >= World::SY - 1) return false;
            if (!blockIsNormalCube(below) || !blockIsReplaceable(world.get(x, y + 1, z))) return false;
            int rot = ifloor((player.yaw + 180.0f) * 4.0f / 360.0f - 0.5f) & 3;
            int dx = 0, dz = 0;
            if (rot == 0) dz = 1;
            if (rot == 1) dx = -1;
            if (rot == 2) dz = -1;
            if (rot == 3) dx = 1;
            int solidL = (blockIsNormalCube(world.get(x - dx, y, z - dz)) ? 1 : 0) +
                         (blockIsNormalCube(world.get(x - dx, y + 1, z - dz)) ? 1 : 0);
            int solidR = (blockIsNormalCube(world.get(x + dx, y, z + dz)) ? 1 : 0) +
                         (blockIsNormalCube(world.get(x + dx, y + 1, z + dz)) ? 1 : 0);
            bool doorL = blockIsDoor(world.get(x - dx, y, z - dz)) || blockIsDoor(world.get(x - dx, y + 1, z - dz));
            bool doorR = blockIsDoor(world.get(x + dx, y, z + dz)) || blockIsDoor(world.get(x + dx, y + 1, z + dz));
            bool flip = (doorL && !doorR) || solidR > solidL;
            if (flip) rot = ((rot - 1) & 3) + 4;
            AABB pb = player.box();
            AABB db = {(float)x, (float)y, (float)z, x + 1.0f, y + 2.0f, z + 1.0f};
            (void)pb;
            (void)db;
            world.setBlock(x, y, z, (uint8_t)placeId, rot);
            world.setBlock(x, y + 1, z, (uint8_t)placeId, rot + 8);
            notifyNeighbors(x, y, z, (uint8_t)placeId);
            notifyNeighbors(x, y + 1, z, (uint8_t)placeId);
            consume();
            return true;
        }
        case BED: {
            static const int HEAD[4][2] = {{0, 1}, {-1, 0}, {0, -1}, {1, 0}};
            if (face != 1) return false;
            int hx = x + HEAD[quarter][0], hz = z + HEAD[quarter][1];
            if (!world.isLoaded(hx, hz) || !blockIsReplaceable(world.get(hx, y, hz))) return false;
            if (!blockIsNormalCube(below) || !blockIsNormalCube(world.get(hx, y - 1, hz))) return false;
            const int cells[2][2] = {{x, z}, {hx, hz}};
            for (const auto& cell : cells) {
                AABB b = {(float)cell[0], (float)y, (float)cell[1], cell[0] + 1.0f, y + 0.5625f, cell[1] + 1.0f};
                if (b.intersects(player.box())) return false;
                for (const Entity& e : ents)
                    if (entityIsMob(e.type) && e.health > 0 && b.intersects(e.box())) return false;
            }
            world.setBlock(x, y, z, BED, quarter);
            world.setBlock(hx, y, hz, BED, quarter + 8);
            notifyNeighbors(x, y, z, BED);
            notifyNeighbors(hx, y, hz, BED);
            consume();
            return true;
        }
        default:
            if (g_blocks[placeId].flags & BF_NEEDS_FLOOR) {
                if (!supportsPlant((uint8_t)placeId, below, x, y, z)) return false;
            }
            break;
    }

    AABB box[2];
    int nb = blockCollisionBoxes(world, x, y, z, (uint8_t)placeId, box);
    if (g_blocks[placeId].render == RENDER_STAIRS || g_blocks[placeId].render == RENDER_LADDER) {
        nb = 1;
        box[0] = {0, 0, 0, 1, 1, 1};
    }
    for (int i = 0; i < nb; ++i) {
        AABB b = box[i].offset((float)x, (float)y, (float)z);
        if (b.intersects(player.box())) return false;
        for (const Entity& e : ents)
            if (entityIsMob(e.type) && e.health > 0 && b.intersects(e.box())) return false;
    }

    if ((g_blocks[placeId].flags & BF_FALLS) && (below == AIR || blockIsLiquid(below) || blockIsReplaceable(below))) {
        int land = y;
        while (land > 0) {
            uint8_t b = world.get(x, land - 1, z);
            if (b != AIR && !blockIsLiquid(b) && !blockIsReplaceable(b)) break;
            --land;
        }
        world.setRaw(x, land, z, (uint8_t)placeId, meta);
        world.markBlockDirty(x, land, z);
        world.relight(x, z);
        notifyNeighbors(x, y, z, AIR);
        notifyNeighbors(x, land, z, (uint8_t)placeId);
        blockSound(x, y, z, (uint8_t)placeId, false);
        consume();
        return true;
    }
    world.setBlock(x, y, z, (uint8_t)placeId, meta);
    if (placeId == FURNACE) world.createTile(x, y, z, TILE_FURNACE);
    if (placeId == CHEST) world.createTile(x, y, z, TILE_CHEST);
    if (placeId == DISPENSER) world.createTile(x, y, z, TILE_DISPENSER);
    if (placeId == NOTE_BLOCK) world.createTile(x, y, z, TILE_NOTE);
    onBlockAdded(x, y, z, (uint8_t)placeId);
    notifyNeighbors(x, y, z, (uint8_t)placeId);
    redstonePlaced(x, y, z, (uint8_t)placeId);
    blockSound(x, y, z, (uint8_t)placeId, false);
    consume();
    return true;
}

UseResult Game::useOnBlock(const RayHit& hit) {
    int x = hit.x, y = hit.y, z = hit.z;
    uint8_t id = world.get(x, y, z);
    switch (id) {
        case WORKBENCH: return UseResult::Workbench;
        case FURNACE:
        case FURNACE_LIT:
        case CHEST: {
            uint8_t type = id == CHEST ? TILE_CHEST : TILE_FURNACE;
            TileEntity* t = world.tileAt(x, y, z);
            if (!t || t->type != type) t = world.createTile(x, y, z, type);
            if (!t) return UseResult::None;
            openTile = t;
            return id == CHEST ? UseResult::Chest : UseResult::Furnace;
        }
        case DISPENSER: {
            TileEntity* t = world.tileAt(x, y, z);
            if (!t || t->type != TILE_DISPENSER) t = world.createTile(x, y, z, TILE_DISPENSER);
            if (!t) return UseResult::None;
            openTile = t;
            return UseResult::Dispenser;
        }
        case LEVER:
        case STONE_BUTTON:
        case REPEATER_OFF:
        case REPEATER_ON:
        case NOTE_BLOCK:
            redstoneUse(x, y, z, id);
            swingItem();
            return UseResult::None;
        case WOOD_DOOR: {
            int by = (world.getMeta(x, y, z) & 8) ? y - 1 : y;
            if (world.get(x, by, z) != WOOD_DOOR) return UseResult::None;
            int m = world.getMeta(x, by, z) ^ 4;
            world.setMeta(x, by, z, m);
            if (world.get(x, by + 1, z) == WOOD_DOOR) world.setMeta(x, by + 1, z, (m & 7) + 8);
            playSound((m & 4) ? "random.door_open" : "random.door_close", x + 0.5f, by + 0.5f, z + 0.5f, 1.0f,
                      soundRand.nextFloat() * 0.1f + 0.9f);
            swingItem();
            lastUse = UseResult::None;
            return UseResult::None;
        }
        case TRAPDOOR: {
            int m = world.getMeta(x, y, z) ^ 4;
            world.setMeta(x, y, z, m);
            playSound(soundRand.nextFloat() < 0.5f ? "random.door_open" : "random.door_close", x + 0.5f, y + 0.5f,
                      z + 0.5f, 1.0f, soundRand.nextFloat() * 0.1f + 0.9f);
            swingItem();
            return UseResult::None;
        }
        case CAKE:
            eatCake(x, y, z);
            return UseResult::None;
        case BED: {
            static const int HEAD[4][2] = {{0, 1}, {-1, 0}, {0, -1}, {1, 0}};
            int m = world.getMeta(x, y, z);
            if (!(m & 8)) {
                x += HEAD[m & 3][0];
                z += HEAD[m & 3][1];
                if (world.get(x, y, z) != BED) return UseResult::None;
                m = world.getMeta(x, y, z);
            }
            if (dimension < 0) {
                // WorldProvider.canRespawnHere is false in the Nether: the bed blows up
                float cx = x + 0.5f, cy = y + 0.5f, cz = z + 0.5f;
                setBlockNotify(x, y, z, AIR);
                int fx = x - HEAD[m & 3][0], fz = z - HEAD[m & 3][1];
                if (world.get(fx, y, fz) == BED) {
                    setBlockNotify(fx, y, fz, AIR);
                    cx = (cx + fx + 0.5f) / 2.0f;
                    cz = (cz + fz + 0.5f) / 2.0f;
                }
                explode(cx, cy, cz, 5.0f, true);
                return UseResult::None;
            }
            if (sleeping) return UseResult::None;
            if (isDaytime()) {
                message = "You can only sleep at night";
                return UseResult::None;
            }
            if (std::fabs(player.x - (x + 0.5f)) > 3.5f || std::fabs(player.y - y) > 2.5f ||
                std::fabs(player.z - (z + 0.5f)) > 3.5f)
                return UseResult::None;
            float ox = 0.5f, oz = 0.5f;
            switch (m & 3) {
                case 0: oz = 0.9f; break;
                case 1: ox = 0.1f; break;
                case 2: oz = 0.1f; break;
                default: ox = 0.9f; break;
            }
            sleeping = true;
            sleepTimer = 0;
            sleepX = x;
            sleepY = y;
            sleepZ = z;
            mining_ = false;
            player.setPosition(x + ox, y + 0.5625f, z + oz);
            player.mx = player.my = player.mz = 0;
            player.yaw = sleepYaw();
            player.pitch = 0;
            return UseResult::None;
        }
        default: break;
    }
    return UseResult::None;
}

// BlockBed.getNearestEmptyChunkCoordinates: a free standing spot around the head, then the foot.
bool Game::bedSpawnSpot(int x, int y, int z, int& ox, int& oy, int& oz) const {
    static const int HEAD[4][2] = {{0, 1}, {-1, 0}, {0, -1}, {1, 0}};
    int dir = world.getMeta(x, y, z) & 3;
    for (int half = 0; half <= 1; ++half) {
        int x0 = x - HEAD[dir][0] * half - 1, z0 = z - HEAD[dir][1] * half - 1;
        for (int xx = x0; xx <= x0 + 2; ++xx)
            for (int zz = z0; zz <= z0 + 2; ++zz)
                if (blockIsNormalCube(world.get(xx, y - 1, zz)) && world.get(xx, y, zz) == AIR &&
                    world.get(xx, y + 1, zz) == AIR) {
                    ox = xx;
                    oy = y;
                    oz = zz;
                    return true;
                }
    }
    return false;
}

float Game::sleepYaw() const {
    int dir = world.getMeta(sleepX, sleepY, sleepZ) & 3;
    return (float)(((dir + 2) & 3) * 90);
}

void Game::wakeUp(bool setSpawn) {
    if (!sleeping) return;
    sleeping = false;
    sleepTimer = setSpawn ? 100 : 0;
    int sx = sleepX, sy = sleepY + 1, sz = sleepZ;
    if (world.get(sleepX, sleepY, sleepZ) == BED) bedSpawnSpot(sleepX, sleepY, sleepZ, sx, sy, sz);
    player.setPosition(sx + 0.5f, sy + 0.1f, sz + 0.5f);
    player.prevX = player.x;
    player.prevY = player.y;
    player.prevZ = player.z;
    player.mx = player.my = player.mz = 0;
    if (setSpawn && world.get(sleepX, sleepY, sleepZ) == BED) {
        hasBed = true;
        bedX = sleepX;
        bedY = sleepY;
        bedZ = sleepZ;
    }
}

// EntityPlayer.onUpdate while sleeping and World.tick skipping the night.
void Game::tickSleep() {
    if (!sleeping) {
        if (sleepTimer > 0 && ++sleepTimer >= 110) sleepTimer = 0;
        return;
    }
    if (sleepTimer < 100) ++sleepTimer;
    if (world.get(sleepX, sleepY, sleepZ) != BED) {
        wakeUp(false);
    } else if (isDaytime()) {
        wakeUp(true);
    } else if (sleepTimer >= 100) {
        int64_t t = world.time + 24000;
        world.time = t - t % 24000;
        clearWeather();
        updateSky();
        wakeUp(true);
    }
}

bool Game::useItemInAir() {
    ItemStack& h = inv.held();
    if (h.empty()) return false;
    const ItemDef& d = itemDef(h.id);
    if (d.food) {
        if (mode == MODE_SURVIVAL) {
            health += d.food;
            if (health > MAX_HEALTH) health = MAX_HEALTH;
            if (h.id == I_MUSHROOM_STEW)
                h = ItemStack(I_BOWL);
            else if (--h.count == 0)
                h.clear();
        }
        return true;
    }
    if (h.id == I_BOW) {
        if (mode == MODE_SURVIVAL && !inv.consume(I_ARROW)) return false;
        float dx, dy, dz;
        player.look(dx, dy, dz);
        float ex = player.x - std::cos(player.yaw * DEG) * 0.16f, ey = player.y + Player::EYE - 0.1f,
              ez = player.z - std::sin(player.yaw * DEG) * 0.16f;
        shootArrow(nullptr, ex, ey, ez, dx, dy, dz, 1.5f, 1.0f, true);
        playSoundAt(player, "random.bow", 1.0f, 1.0f / (soundRand.nextFloat() * 0.4f + 0.8f));
        return true;
    }
    if (h.id == I_BOAT) return placeBoat();
    if (h.id == I_SNOWBALL || h.id == I_EGG) {
        // ItemSnowball / ItemEgg.onItemRightClick
        uint8_t type = h.id == I_SNOWBALL ? ENT_SNOWBALL : ENT_EGG;
        if (mode == MODE_SURVIVAL && --h.count == 0) h.clear();
        playSoundAt(player, "random.bow", 0.5f, 0.4f / (soundRand.nextFloat() * 0.4f + 0.8f));
        throwItem(type);
        return true;
    }
    if (h.id == I_FISHING_ROD) {
        // ItemFishingRod.onItemRightClick
        if (fishHook >= 0) {
            damageHeld(retrieveFish());
        } else {
            playSoundAt(player, "random.bow", 0.5f, 0.4f / (soundRand.nextFloat() * 0.4f + 0.8f));
            throwItem(ENT_FISH);
        }
        swingItem();
        return false;  // same stack: ItemRenderer keeps the hand up
    }
    if (h.id == I_BUCKET) {
        float dx, dy, dz;
        player.look(dx, dy, dz);
        float ex = player.x, ey = player.y + Player::EYE, ez = player.z;
        for (float t = 0; t < 5.0f; t += 0.1f) {
            int bx = ifloor(ex + dx * t), by = ifloor(ey + dy * t), bz = ifloor(ez + dz * t);
            uint8_t id = world.get(bx, by, bz);
            if (blockIsLiquid(id) && world.getMeta(bx, by, bz) == 0) {
                setBlockNotify(bx, by, bz, AIR);
                if (mode == MODE_SURVIVAL) h = ItemStack(blockIsWater(id) ? I_WATER_BUCKET : I_LAVA_BUCKET);
                return true;
            }
            if (id != AIR && !blockIsLiquid(id)) break;
        }
    }
    return false;
}

// ------------------------------------------------------------------ targeting and actions

void Game::updateTarget(float ex, float ey, float ez) {
    float dx, dy, dz;
    player.look(dx, dy, dz);
    float reach = mode == MODE_CREATIVE ? 5.0f : 4.0f;
    hasBlockTarget = raycast(world, ex, ey, ez, dx, dy, dz, reach, blockTarget);
    float best = hasBlockTarget ? blockTarget.t : reach;
    if (best > 3.0f) best = 3.0f;
    entityTarget = nullptr;
    for (Entity& e : ents) {
        if (!collidable(e)) continue;
        float t;
        int face;
        if (rayBox(ex, ey, ez, dx, dy, dz, hitBox(e).grow(0.1f), t, face) && t >= 0 && t < best) {
            best = t;
            entityTarget = &e;
        }
    }
    if (entityTarget) hasBlockTarget = false;
}

void Game::swingItem() {
    if (!swinging_ || swingInt_ >= 4 || swingInt_ < 0) {
        swingInt_ = -1;
        swinging_ = true;
    }
}

void Game::tickMining() {
    if (mineDelay_ > 0) --mineDelay_;
    if (attackPressed) swingItem();
    if (attackPressed && entityTarget) {
        if (entityIsMob(entityTarget->type)) alertWolves(*entityTarget, true);
        int dmg = attackDamage(inv.held());
        hurtEntity(*entityTarget, dmg, player.x, player.z, true);
        const ItemDef& d = itemDef(inv.held().id);
        if (d.tool == TOOL_SWORD) damageHeld(1);
        else if (d.tool != TOOL_NONE) damageHeld(2);
        swingItem();
    }
    if (attackPressed && !entityTarget && hasBlockTarget) {
        // World.onBlockHit: punching a block puts out fire on that face.
        static const int OFF[6][3] = {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};
        const RayHit& t = blockTarget;
        int fx = t.x + OFF[t.face][0], fy = t.y + OFF[t.face][1], fz = t.z + OFF[t.face][2];
        if (world.get(fx, fy, fz) == FIRE) {
            setBlockNotify(fx, fy, fz, AIR);
            playSound("random.fizz", fx + 0.5f, fy + 0.5f, fz + 0.5f, 0.5f,
                      2.6f + (soundRand.nextFloat() - soundRand.nextFloat()) * 0.8f);
            swingItem();
            attackPressed = false;
            mineDelay_ = 5;
            return;
        }
        // Block.onBlockClicked
        uint8_t id = world.get(t.x, t.y, t.z);
        if (id == TRAPDOOR) {
            int m = world.getMeta(t.x, t.y, t.z) ^ 4;
            world.setMeta(t.x, t.y, t.z, m);
            playSound(soundRand.nextFloat() < 0.5f ? "random.door_open" : "random.door_close", t.x + 0.5f,
                      t.y + 0.5f, t.z + 0.5f, 1.0f, soundRand.nextFloat() * 0.1f + 0.9f);
        } else if (id == CAKE) {
            eatCake(t.x, t.y, t.z);
        } else {
            redstoneClick(t.x, t.y, t.z, id);
        }
    }
    attackPressed = false;
    if (!attackHeld || !hasBlockTarget) {
        mining_ = false;
        mineProgress_ = 0;
        return;
    }
    const RayHit& h = blockTarget;
    if (!mining_ || h.x != mineX_ || h.y != mineY_ || h.z != mineZ_) {
        mining_ = true;
        mineX_ = h.x, mineY_ = h.y, mineZ_ = h.z;
        mineProgress_ = 0;
        mineSoundTicks_ = 0;
    }
    if (mineDelay_ > 0) return;
    uint8_t id = world.get(h.x, h.y, h.z);
    if (id == AIR) return;
    swingItem();
    if (mode == MODE_CREATIVE) {
        breakBlock(h.x, h.y, h.z, false);
        mineDelay_ = 5;
        mining_ = false;
        return;
    }
    float hard = g_blocks[id].hardness;
    float strength;
    const ItemStack& held = inv.held();
    if (hard < 0)
        strength = 0;
    else if (hard == 0)
        strength = 1.0f;
    else if (!canHarvest(held, id))
        strength = 1.0f / hard / 100.0f;
    else
        strength = toolStrength(held, id) / hard / 30.0f;
    if (player.inWater) strength /= 5.0f;
    if (!player.onGround) strength /= 5.0f;
    mineProgress_ += strength;
    if (mineSoundTicks_++ % 4 == 0) {
        const StepSound& s = blockStepSound(id);
        playSound(s.step, h.x + 0.5f, h.y + 0.5f, h.z + 0.5f, (s.volume + 1.0f) / 8.0f, s.pitch * 0.5f);
    }
    particles.blockHit(world, h.x, h.y, h.z, h.face);
    if (mineProgress_ >= 1.0f) {
        bool harvest = canHarvest(held, id);
        bool shearLeaves = id == LEAVES && held.id == I_SHEARS;
        int meta = world.getMeta(h.x, h.y, h.z);
        breakBlock(h.x, h.y, h.z, harvest && !shearLeaves);
        if (shearLeaves) dropItem(ItemStack(LEAVES, 1, meta & 3), h.x + 0.5f, h.y + 0.5f, h.z + 0.5f, false);
        const ItemDef& d = itemDef(held.id);
        if (d.tool == TOOL_SWORD) damageHeld(2);
        else if (d.tool != TOOL_NONE || held.id == I_SHEARS) damageHeld(1);
        mineProgress_ = 0;
        mineDelay_ = 5;
        mining_ = false;
    }
}

void Game::tickUse() {
    if (useDelay_ > 0) --useDelay_;
    if (!usePressed) return;
    usePressed = false;
    lastUse = UseResult::None;
    ItemStack& h = inv.held();
    if (entityTarget) {
        Entity& e = *entityTarget;
        if (useOnEntity(e)) return;
        if (e.type == ENT_WOLF && useOnWolf(e)) return;
        if (e.type == ENT_SHEEP && h.id == I_SHEARS && !e.sheared) {
            e.sheared = true;
            int n = 2 + rand.nextInt(3);
            for (int i = 0; i < n; ++i) dropItem(ItemStack(WOOL, 1, e.woolColor), e.x, e.y + 1.0f, e.z, false);
            damageHeld(1);
            swingItem();
            return;
        }
        if (e.type == ENT_COW && h.id == I_BUCKET) {
            if (mode == MODE_SURVIVAL) h = ItemStack(I_MILK_BUCKET);
            return;
        }
    }
    if (hasBlockTarget) {
        UseResult r = useOnBlock(blockTarget);
        if (r != UseResult::None) {
            lastUse = r;
            return;
        }
        uint8_t id = world.get(blockTarget.x, blockTarget.y, blockTarget.z);
        if (id == WOOD_DOOR || id == IRON_DOOR || id == BED || id == TRAPDOOR || id == CAKE || id == LEVER ||
            id == STONE_BUTTON || blockIsRepeater(id) || id == NOTE_BLOCK)
            return;
        const RayHit& b = blockTarget;
        const ItemDef& d = itemDef(h.id);
        if (id == JUKEBOX && world.getMeta(b.x, b.y, b.z) != 0) {
            // BlockJukeBox.ejectRecord
            TileEntity* t = world.tileAt(b.x, b.y, b.z);
            if (t && !t->items[0].empty()) {
                float ox = rand.nextFloat() * 0.7f + 0.15f, oy = rand.nextFloat() * 0.7f + 0.66f,
                      oz = rand.nextFloat() * 0.7f + 0.15f;
                dropItem(t->items[0], b.x + ox, b.y + oy, b.z + oz, false);
                t->items[0].clear();
            }
            jukeboxStop(b.x, b.y, b.z);
            world.setMeta(b.x, b.y, b.z, 0);
            return;
        }
        if (h.id == I_MINECART || h.id == I_CHEST_MINECART || h.id == I_FURNACE_MINECART) {
            if (placeCart(b)) swingItem();
            return;
        }
        if (!h.empty()) {
            if ((h.id == I_RECORD_13 || h.id == I_RECORD_CAT) && id == JUKEBOX) {
                // ItemRecord.onItemUse
                TileEntity* t = world.tileAt(b.x, b.y, b.z);
                if (!t) t = world.createTile(b.x, b.y, b.z, TILE_JUKEBOX);
                if (!t) return;
                t->items[0] = ItemStack(h.id);
                world.setMeta(b.x, b.y, b.z, 1);
                record.record = h.id;
                record.x = b.x, record.y = b.y, record.z = b.z;
                ++record.serial;
                message = h.id == I_RECORD_CAT ? "Now playing: C418 - cat" : "Now playing: C418 - 13";
                if (mode == MODE_SURVIVAL && --h.count == 0) h.clear();
                return;
            }
            if (h.id == I_SIGN) {
                // ItemSign.onItemUse
                static const int OFF[6][3] = {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};
                if (b.face == 0 || !blockMatSolid(id)) return;
                int x = b.x + OFF[b.face][0], y = b.y + OFF[b.face][1], z = b.z + OFF[b.face][2];
                if ((unsigned)y >= (unsigned)World::SY || !blockIsReplaceable(world.get(x, y, z))) return;
                if (b.face == 1)
                    setBlockNotify(x, y, z, SIGN_POST, ifloor((player.yaw + 180.0f) * 16.0f / 360.0f + 0.5f) & 15);
                else
                    setBlockNotify(x, y, z, WALL_SIGN, b.face);
                if (!blockIsSign(world.get(x, y, z))) return;
                if (mode == MODE_SURVIVAL && --h.count == 0) h.clear();
                TileEntity* t = world.createTile(x, y, z, TILE_SIGN);
                if (t) {
                    openTile = t;
                    lastUse = UseResult::Sign;
                }
                swingItem();
                return;
            }
            if (h.id == I_PAINTING) {
                if (placePainting(b)) swingItem();
                return;
            }
            if (d.tool == TOOL_HOE && b.face != 0 && (id == GRASS || id == DIRT) && world.get(b.x, b.y + 1, b.z) == AIR) {
                world.setBlock(b.x, b.y, b.z, FARMLAND);
                blockSound(b.x, b.y, b.z, FARMLAND, false);
                damageHeld(1);
                swingItem();
                return;
            }
            if (h.id == I_SEEDS) {
                if (id == FARMLAND && b.face == 1 && world.get(b.x, b.y + 1, b.z) == AIR) {
                    world.setBlock(b.x, b.y + 1, b.z, CROPS);
                    if (mode == MODE_SURVIVAL && --h.count == 0) h.clear();
                }
                return;
            }
            if (h.id == I_DYE && h.damage == 15) {
                if (id == CROPS) {
                    world.setMeta(b.x, b.y, b.z, 7);
                } else if (id == SAPLING) {
                    growSapling(world, rand, b.x, b.y, b.z, world.getMeta(b.x, b.y, b.z) & 3);
                    world.relight(b.x, b.z);
                    world.markBlockDirty(b.x, b.y, b.z);
                    for (int dy = 0; dy < 8; ++dy)
                        for (int dx = -3; dx <= 3; ++dx)
                            for (int dz = -3; dz <= 3; ++dz) world.markBlockDirty(b.x + dx, b.y + dy, b.z + dz);
                } else {
                    return;
                }
                if (mode == MODE_SURVIVAL && --h.count == 0) h.clear();
                return;
            }
            if (h.id == I_FLINT_AND_STEEL) {
                // ItemFlintAndSteel.onItemUse: fire on the clicked face.
                static const int OFF[6][3] = {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};
                int x = b.x + OFF[b.face][0], y = b.y + OFF[b.face][1], z = b.z + OFF[b.face][2];
                if ((unsigned)y < (unsigned)World::SY && world.get(x, y, z) == AIR) {
                    playSound("fire.ignite", x + 0.5f, y + 0.5f, z + 0.5f, 1.0f, soundRand.nextFloat() * 0.4f + 0.8f);
                    setBlockNotify(x, y, z, FIRE);
                }
                damageHeld(1);
                swingItem();
                return;
            }
            if (h.id == I_WATER_BUCKET || h.id == I_LAVA_BUCKET) {
                static const int OFF[6][3] = {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};
                int x = b.x + OFF[b.face][0], y = b.y + OFF[b.face][1], z = b.z + OFF[b.face][2];
                if (blockIsReplaceable(world.get(x, y, z)) && (unsigned)y < 128) {
                    if (dimension < 0 && h.id == I_WATER_BUCKET)
                        playSound("random.fizz", x + 0.5f, y + 0.5f, z + 0.5f, 0.5f,
                                  2.6f + (soundRand.nextFloat() - soundRand.nextFloat()) * 0.8f);
                    else
                        setBlockNotify(x, y, z, h.id == I_WATER_BUCKET ? WATER_FLOWING : LAVA_FLOWING);
                    if (mode == MODE_SURVIVAL) h = ItemStack(I_BUCKET);
                }
                return;
            }
            if (placeHeld(b)) {
                swingItem();
                return;
            }
        }
    }
    if (useItemInAir()) ++handResets;
}

// ------------------------------------------------------------------ ticking

// World.randomDisplayUpdates: ambient block sounds around the player.
void Game::displayTicks() {
    int px = ifloor(player.x), py = ifloor(player.y), pz = ifloor(player.z);
    for (int i = 0; i < 1000; ++i) {
        int x = px + soundRand.nextInt(16) - soundRand.nextInt(16);
        int y = py + soundRand.nextInt(16) - soundRand.nextInt(16);
        int z = pz + soundRand.nextInt(16) - soundRand.nextInt(16);
        uint8_t id = world.get(x, y, z);
        if (id == FIRE) {
            if (soundRand.nextInt(24) == 0)
                playSound("fire.fire", x + 0.5f, y + 0.5f, z + 0.5f, 1.0f + soundRand.nextFloat(),
                          soundRand.nextFloat() * 0.7f + 0.3f);
            fireSmoke(x, y, z);
        } else if (id == PORTAL) {
            if (soundRand.nextInt(100) == 0)
                playSound("portal.portal", x + 0.5f, y + 0.5f, z + 0.5f, 1.0f, soundRand.nextFloat() * 0.4f + 0.8f);
            portalParticles(x, y, z);
        } else if (id == TORCH || id == REDSTONE_TORCH_ON || id == FURNACE_LIT || id == REDSTONE_WIRE ||
                   id == REPEATER_ON || id == REDSTONE_ORE_LIT || id == LAVA || id == LAVA_FLOWING) {
            blockParticles(x, y, z, id);
        } else if (blockIsWater(id) && soundRand.nextInt(64) == 0) {
            int m = world.getMeta(x, y, z);
            if (m > 0 && m < 8)
                playSound("liquid.water", x + 0.5f, y + 0.5f, z + 0.5f, soundRand.nextFloat() * 0.25f + 0.75f,
                          soundRand.nextFloat() + 0.5f);
        }
    }
}

// BlockFire.randomDisplayTick: smoke licking the burnable neighbors, or rising from the floor.
void Game::fireSmoke(int x, int y, int z) {
    Particles& P = particles;
    auto smoke = [&](float px, float py, float pz) { P.spawn(world, PART_SMOKE, px, py, pz, 0, 0, 0, 2.5f); };
    if (!blockIsNormalCube(world.get(x, y - 1, z)) && blockFireSpread(world.get(x, y - 1, z)) <= 0) {
        auto burns = [&](int ax, int ay, int az) { return blockFireSpread(world.get(ax, ay, az)) > 0; };
        if (burns(x - 1, y, z))
            for (int i = 0; i < 2; ++i) smoke(x + P.rnd() * 0.1f, y + P.rnd(), z + P.rnd());
        if (burns(x + 1, y, z))
            for (int i = 0; i < 2; ++i) smoke(x + 1 - P.rnd() * 0.1f, y + P.rnd(), z + P.rnd());
        if (burns(x, y, z - 1))
            for (int i = 0; i < 2; ++i) smoke(x + P.rnd(), y + P.rnd(), z + P.rnd() * 0.1f);
        if (burns(x, y, z + 1))
            for (int i = 0; i < 2; ++i) smoke(x + P.rnd(), y + P.rnd(), z + 1 - P.rnd() * 0.1f);
        if (burns(x, y + 1, z))
            for (int i = 0; i < 2; ++i) smoke(x + P.rnd(), y + 1 - P.rnd() * 0.1f, z + P.rnd());
    } else {
        for (int i = 0; i < 3; ++i) smoke(x + P.rnd(), y + P.rnd() * 0.5f + 0.5f, z + P.rnd());
    }
}

// BlockPortal.randomDisplayTick
void Game::portalParticles(int x, int y, int z) {
    Particles& P = particles;
    for (int i = 0; i < 4; ++i) {
        float px = x + P.rnd(), py = y + P.rnd(), pz = z + P.rnd();
        float vx = (P.rnd() - 0.5f) * 0.5f, vy = (P.rnd() - 0.5f) * 0.5f, vz = (P.rnd() - 0.5f) * 0.5f;
        int k = (P.rnd() < 0.5f) * 2 - 1;
        if (world.get(x - 1, y, z) != PORTAL && world.get(x + 1, y, z) != PORTAL) {
            px = x + 0.5f + 0.25f * k;
            vx = P.rnd() * 2.0f * k;
        } else {
            pz = z + 0.5f + 0.25f * k;
            vz = P.rnd() * 2.0f * k;
        }
        P.spawn(world, PART_PORTAL, px, py, pz, vx, vy, vz);
    }
}

// randomDisplayTick of torches, furnaces, redstone parts and lava.
void Game::blockParticles(int x, int y, int z, uint8_t id) {
    Particles& P = particles;
    int meta = world.getMeta(x, y, z);
    switch (id) {
        case TORCH:
        case REDSTONE_TORCH_ON: {
            float px = x + 0.5f, py = y + 0.7f, pz = z + 0.5f;
            if (id == REDSTONE_TORCH_ON)
                px += (P.rnd() - 0.5f) * 0.2f, py += (P.rnd() - 0.5f) * 0.2f, pz += (P.rnd() - 0.5f) * 0.2f;
            const float up = 0.22f, side = 0.27f;
            if (meta == 1) px -= side, py += up;
            else if (meta == 2) px += side, py += up;
            else if (meta == 3) pz -= side, py += up;
            else if (meta == 4) pz += side, py += up;
            if (id == TORCH) {
                P.spawn(world, PART_SMOKE, px, py, pz, 0, 0, 0);
                P.spawn(world, PART_FLAME, px, py, pz, 0, 0, 0);
            } else {
                P.spawn(world, PART_REDDUST, px, py, pz, 0, 0, 0);
            }
            break;
        }
        case FURNACE_LIT: {
            float px = x + 0.5f, py = y + P.rnd() * 6.0f / 16.0f, pz = z + 0.5f;
            const float out = 0.52f;
            float along = P.rnd() * 0.6f - 0.3f;
            if (meta == 4) px -= out, pz += along;
            else if (meta == 5) px += out, pz += along;
            else if (meta == 2) pz -= out, px += along;
            else if (meta == 3) pz += out, px += along;
            else break;
            P.spawn(world, PART_SMOKE, px, py, pz, 0, 0, 0);
            P.spawn(world, PART_FLAME, px, py, pz, 0, 0, 0);
            break;
        }
        case REDSTONE_WIRE: {
            if (meta == 0) break;
            float f = meta / 15.0f;
            float r = f * 0.6f + 0.4f, g = f * f * 0.7f - 0.5f, b = f * f * 0.6f - 0.7f;
            if (g < 0) g = 0;
            if (b < 0) b = 0;
            P.spawn(world, PART_REDDUST, x + 0.5f + (P.rnd() - 0.5f) * 0.2f, y + 0.0625f,
                    z + 0.5f + (P.rnd() - 0.5f) * 0.2f, r, g, b);
            break;
        }
        case REPEATER_ON: {
            static const float DELAY_POS[4] = {-0.0625f, 0.0625f, 0.1875f, 0.3125f};
            float px = x + 0.5f + (P.rnd() - 0.5f) * 0.2f, py = y + 0.4f + (P.rnd() - 0.5f) * 0.2f;
            float pz = z + 0.5f + (P.rnd() - 0.5f) * 0.2f;
            float o = P.rnd() < 0.5f ? -0.3125f : DELAY_POS[(meta & 12) >> 2];
            switch (meta & 3) {
                case 0: pz -= o; break;
                case 1: px += o; break;
                case 2: pz += o; break;
                default: px -= o; break;
            }
            P.spawn(world, PART_REDDUST, px, py, pz, 0, 0, 0);
            break;
        }
        case REDSTONE_ORE_LIT: {
            const float f = 0.0625f;
            for (int i = 0; i < 6; ++i) {
                float px = x + P.rnd(), py = y + P.rnd(), pz = z + P.rnd();
                if (i == 0 && !blockIsOpaque(world.get(x, y + 1, z))) py = y + 1 + f;
                if (i == 1 && !blockIsOpaque(world.get(x, y - 1, z))) py = y - f;
                if (i == 2 && !blockIsOpaque(world.get(x, y, z + 1))) pz = z + 1 + f;
                if (i == 3 && !blockIsOpaque(world.get(x, y, z - 1))) pz = z - f;
                if (i == 4 && !blockIsOpaque(world.get(x + 1, y, z))) px = x + 1 + f;
                if (i == 5 && !blockIsOpaque(world.get(x - 1, y, z))) px = x - f;
                if (px < x || px > x + 1 || py < 0 || py > y + 1 || pz < z || pz > z + 1)
                    P.spawn(world, PART_REDDUST, px, py, pz, 0, 0, 0);
            }
            break;
        }
        default:  // lava pops out of open surfaces
            if (world.get(x, y + 1, z) == AIR && soundRand.nextInt(100) == 0)
                P.spawn(world, PART_LAVA, x + P.rnd(), y + 1.0f, z + P.rnd(), 0, 0, 0);
            break;
    }
}

// EntityLiving.spawnExplosionParticle and the death puff (onEntityDeath at deathTime 20).
void Game::deathPoof(const Entity& e) {
    Particles& P = particles;
    for (int i = 0; i < 20; ++i) {
        float vx = (P.rnd() + P.rnd() + P.rnd() - 1.5f) * 0.04f;
        float vy = (P.rnd() + P.rnd() + P.rnd() - 1.5f) * 0.04f;
        float vz = (P.rnd() + P.rnd() + P.rnd() - 1.5f) * 0.04f;
        P.spawn(world, PART_EXPLODE, e.x + P.rnd() * e.width * 2 - e.width, e.y + P.rnd() * e.height,
                e.z + P.rnd() * e.width * 2 - e.width, vx, vy, vz);
    }
}

// EntityWolf.showHeartsOrSmokeFX
void Game::heartsOrSmoke(const Entity& e, bool hearts) {
    Particles& P = particles;
    for (int i = 0; i < 7; ++i) {
        float vx = (P.rnd() + P.rnd() + P.rnd() - 1.5f) * 0.04f;
        float vy = (P.rnd() + P.rnd() + P.rnd() - 1.5f) * 0.04f;
        float vz = (P.rnd() + P.rnd() + P.rnd() - 1.5f) * 0.04f;
        P.spawn(world, hearts ? PART_HEART : PART_SMOKE, e.x + P.rnd() * e.width * 2 - e.width,
                e.y + 0.5f + P.rnd() * e.height, e.z + P.rnd() * e.width * 2 - e.width, vx, vy, vz);
    }
}

// World.updateBlocksAndPlayCaveSounds: once the countdown runs out, a random dark air block within
// 8 blocks of the player plays a cave sound.
void Game::caveSounds() {
    if (caveSoundTimer_ > 0) {
        --caveSoundTimer_;
        return;
    }
    int pcx = ifloor(player.x) >> 4, pcz = ifloor(player.z) >> 4;
    for (int cx = pcx - 1; cx <= pcx + 1; ++cx)
        for (int cz = pcz - 1; cz <= pcz + 1; ++cz) {
            int x = cx * 16 + soundRand.nextInt(16), y = soundRand.nextInt(World::SY), z = cz * 16 + soundRand.nextInt(16);
            if (!world.isLoaded(x, z) || world.get(x, y, z) != AIR) continue;
            if (world.getBrightness(x, y, z) > soundRand.nextInt(8) || world.getSkyLight(x, y, z) > 0) continue;
            float dx = x + 0.5f - player.x, dy = y + 0.5f - player.y, dz = z + 0.5f - player.z;
            float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 > 64.0f || d2 <= 4.0f) continue;
            playSound("ambient.cave.cave", x + 0.5f, y + 0.5f, z + 0.5f, 0.7f, 0.8f + soundRand.nextFloat() * 0.2f);
            caveSoundTimer_ = soundRand.nextInt(12000) + 6000;
            return;
        }
}

void Game::tick() {
    if (!world.isOpen()) return;
    ++world.time;
    tickWeather();
    updateSky();
    boatCount_ = 0;
    for (const Entity& e : ents)
        if (e.type == ENT_BOAT && !e.dead) ++boatCount_;
    g_entityBoxes = entityBoxesThunk;
    g_entityBoxesUser = this;
    prevSwing_ = swing_;
    if (swinging_) {
        if (++swingInt_ >= 8) {
            swingInt_ = 0;
            swinging_ = false;
        }
    } else {
        swingInt_ = 0;
    }
    swing_ = swingInt_ / 8.0f;
    if (dead) {
        ++deathTime;
        attackPressed = usePressed = false;
    } else if (sleeping) {
        attackPressed = usePressed = false;
        if (hurtTime > 0) --hurtTime;
        if (hurtResist > 0) --hurtResist;
    } else {
        tickPlayer();
        tickMining();
        tickUse();
    }
    tickSleep();
    tickDials();
    tickEntities();
    tickBolts();
    rollLightning();
    spawnMobs();
    tickScheduled();
    randomTicks();
    tickTiles();
    displayTicks();
    caveSounds();
    rainEffects();
    particles.viewX = player.x, particles.viewY = player.y + Player::EYE, particles.viewZ = player.z;
    particles.tick(world);
    // Autosave every minute, one chunk per tick so the save thread's copies stay small.
    if (++levelSaveTimer_ >= 1200) {
        levelSaveTimer_ = 0;
        if (!dead) saveLevel();
        autosaving_ = true;
    }
    if (autosaving_) autosaving_ = world.autosaveStep(1);
    g_entityBoxes = nullptr;
}

void Game::tickPlayer() {
    if (!world.isLoaded(ifloor(player.x), ifloor(player.z))) return;
    tickPortal();
    if (riding >= 0 && player.descend) mount(riding);
    if (riding >= 0) {
        // The vehicle places the player (tickRider); the input only steers boats (EntityBoat reads the
        // rider's motion after EntityLiving.moveEntityWithHeading in the air: moveFlying 0.02, drag 0.91).
        player.prevX = player.x, player.prevY = player.y, player.prevZ = player.z;
        player.prevBob = player.bob, player.prevBobPitch = player.bobPitch, player.prevWalked = player.walked;
        player.bob = player.bobPitch = 0;
        player.mx = player.my = player.mz = 0;
        player.moveFlying(player.strafe * 0.98f, player.forward * 0.98f, 0.02f);
        riderMX_ = player.mx * 0.91f;
        riderMZ_ = player.mz * 0.91f;
        player.mx = player.my = player.mz = 0;
        player.fallDistance = 0;
        player.updateLiquids(world);
    } else {
        player.tick(world);
        bodySounds(player);
    }
    if (player.touchedPortal) {
        // EntityPlayer.setInPortal
        player.touchedPortal = false;
        if (portalCooldown > 0) portalCooldown = 10;
        else inPortal_ = true;
    }
    touchPlates(player.box());
    if (hurtTime > 0) --hurtTime;
    if (hurtResist > 0) --hurtResist;
    if (mode == MODE_CREATIVE) {
        health = MAX_HEALTH;
        air = MAX_AIR;
        fire = 0;
        player.landedFall = 0;
        if (player.y < -64) placeAtSpawn();
    } else {
        if (player.landedFall > 0) {
            int d = (int)std::ceil(player.landedFall - 3.0f);
            player.landedFall = 0;
            if (d > 0 && hurtPlayer(d, player.x, player.z, false)) deathMessage = "fell from a high place";
        }
        float eye = player.y + Player::EYE;
        if (player.eyeInWater(world, eye)) {
            if (--air == -20) {
                air = 0;
                for (int i = 0; i < 8; ++i)
                    particles.spawn(world, PART_BUBBLE, player.x + particles.rnd() - particles.rnd(),
                                    player.y + particles.rnd() - particles.rnd(), player.z + particles.rnd() - particles.rnd(),
                                    player.mx, player.my, player.mz);
                if (hurtPlayer(2, player.x, player.z, false)) deathMessage = "drowned";
            }
            fire = 0;
        } else {
            air = MAX_AIR;
        }
        if (player.inLava) {
            if (hurtPlayer(4, player.x, player.z, false)) deathMessage = "tried to swim in lava";
            fire = 600;
        }
        bool wet = player.inWater || rainingAt(ifloor(player.x), ifloor(player.y), ifloor(player.z));
        if (boxTouches(world, player.box().grow(-0.001f), FIRE)) {
            if (hurtPlayer(1, player.x, player.z, false)) deathMessage = "went up in flames";
            if (!wet) fire = fire <= 0 ? 300 : fire + 1;
        }
        if (fire > 0) {
            if (wet) {
                if (!player.inWater)
                    playSoundAt(player, "random.fizz", 0.7f, 1.6f + (soundRand.nextFloat() - soundRand.nextFloat()) * 0.4f);
                fire = 0;
            } else {
                if (fire % 20 == 0 && hurtPlayer(1, player.x, player.z, false)) deathMessage = "burned to death";
                --fire;
            }
        }
        AABB pb = player.box().grow(0.001f);
        for (int x = ifloor(pb.x0); x <= ifloor(pb.x1); ++x)
            for (int y = ifloor(pb.y0); y <= ifloor(pb.y1); ++y)
                for (int z = ifloor(pb.z0); z <= ifloor(pb.z1); ++z)
                    if (world.get(x, y, z) == CACTUS && hurtPlayer(1, x + 0.5f, z + 0.5f, true))
                        deathMessage = "was pricked to death";
        if (player.headInBlock(world) && hurtPlayer(1, player.x, player.z, false))
            deathMessage = "suffocated in a wall";
        if (player.y < -64 && hurtPlayer(4, player.x, player.z, false)) deathMessage = "fell out of the world";
    }
    // EntityItem.onCollideWithPlayer
    AABB reach = player.box();
    reach.x0 -= 1, reach.x1 += 1, reach.z0 -= 1, reach.z1 += 1;
    for (Entity& e : ents) {
        auto pop = [&] {
            playSoundAt(e, "random.pop", 0.2f,
                        ((soundRand.nextFloat() - soundRand.nextFloat()) * 0.7f + 1.0f) * 2.0f);
        };
        if (e.type == ENT_ITEM && e.pickupDelay == 0 && reach.intersects(e.box())) {
            int before = e.item.count;
            inv.add(e.item);
            if (e.item.count != before) pop();
            if (e.item.empty()) e.type = ENT_NONE;
        } else if (e.type == ENT_ARROW && e.inGround && e.fromPlayer && reach.intersects(e.box())) {
            ItemStack a(I_ARROW);
            if (mode == MODE_CREATIVE || inv.add(a)) {
                pop();
                e.type = ENT_NONE;
            }
        } else if (e.type == ENT_SLIME && e.health > 0 && e.size > 1 && reach.intersects(e.box())) {
            // EntitySlime.onCollideWithPlayer (distance measured to the player's eye-level position)
            float eyeY = player.y + Player::EYE;
            float dx = e.x - player.x, dy = e.y - eyeY, dz = e.z - player.z;
            if (std::sqrt(dx * dx + dy * dy + dz * dz) < 0.6f * e.size && canSee(e, player.x, eyeY, player.z) &&
                hurtPlayer(e.size, e.x, e.z, true, &e)) {
                playSoundAt(e, "mob.slimeattack", 1.0f, (soundRand.nextFloat() - soundRand.nextFloat()) * 0.2f + 1.0f);
                deathMessage = "was slain by a slime";
            }
        }
    }
}

void Game::tickEntities() {
    // Ride links (World.updateEntityWithOptionalForce drops riders whose vehicle is gone).
    if (riding >= 0) {
        Entity& v = ents[riding];
        bool alive = !v.dead && (entityIsVehicle(v.type) || (v.type == ENT_PIG && v.health > 0));
        if (dead || !alive || v.rider != RIDER_PLAYER) {
            if (v.rider == RIDER_PLAYER) v.rider = -1;
            riding = -1;
        }
    }
    for (int i = 0; i < MAX_ENTITIES; ++i) {
        Entity& e = ents[i];
        if (e.rider == RIDER_PLAYER && riding != i) e.rider = -1;
        if (e.rider >= 0 && (ents[e.rider].vehicle != i || ents[e.rider].dead)) e.rider = -1;
        if (e.vehicle >= 0 && (ents[e.vehicle].rider != i || ents[e.vehicle].dead)) e.vehicle = -1;
    }
    auto finish = [&](Entity& e) {
        if (e.type != ENT_NONE && e.type != ENT_PAINTING && !e.dead) touchPlates(e.box());
        if (e.dead) {
            if (entityTarget == &e) entityTarget = nullptr;
            int16_t idx = (int16_t)(&e - ents);
            for (Entity& o : ents) {
                if (o.target == idx) o.target = -1;
                if (o.hooked == idx) o.hooked = -1;
            }
            if (fishHook == idx) fishHook = -1;
            if (openCart == idx) openCart = -1;
            if (riding == idx) riding = -1;
            if (e.vehicle >= 0) ents[e.vehicle].rider = -1;
            if (e.rider >= 0) ents[e.rider].vehicle = -1;
            e = Entity();
        }
    };
    for (Entity& e : ents) {
        if (e.type == ENT_NONE || e.vehicle >= 0) continue;
        if (!world.isLoaded(ifloor(e.x), ifloor(e.z))) continue;
        tickEntity(e);
        if (e.rider != -1 && !e.dead && e.type != ENT_NONE) {
            int r = e.rider;
            tickRider(e);
            if (r >= 0) finish(ents[r]);
        }
        finish(e);
    }
    vehicleCollisions();
    // Entity.applyEntityCollision between living things.
    for (int i = 0; i < MAX_ENTITIES; ++i) {
        Entity& a = ents[i];
        if (!entityIsMob(a.type) || a.health <= 0) continue;
        AABB ab = a.box();
        auto push = [&](float ox, float oz, float& amx, float& amz, float& bmx, float& bmz) {
            float dx = ox - a.x, dz = oz - a.z;
            float d = std::fabs(dx) > std::fabs(dz) ? std::fabs(dx) : std::fabs(dz);
            if (d < 0.01f) return;
            d = std::sqrt(d);
            dx /= d, dz /= d;
            float f = 1.0f / d;
            if (f > 1) f = 1;
            dx *= f * 0.05f, dz *= f * 0.05f;
            amx -= dx, amz -= dz, bmx += dx, bmz += dz;
        };
        if (!dead && riding != i && ab.intersects(player.box())) push(player.x, player.z, a.mx, a.mz, player.mx, player.mz);
        for (int j = i + 1; j < MAX_ENTITIES; ++j) {
            Entity& b = ents[j];
            if (!entityIsMob(b.type) || b.health <= 0 || !ab.intersects(b.box())) continue;
            push(b.x, b.z, a.mx, a.mz, b.mx, b.mz);
        }
    }
}

void Game::tickEntity(Entity& e) {
    e.prevX = e.x, e.prevY = e.y, e.prevZ = e.z;
    e.prevYaw = e.yaw;
    ++e.age;
    switch (e.type) {
        case ENT_ITEM: tickItem(e); break;
        case ENT_ARROW: tickArrow(e); break;
        case ENT_TNT: tickTnt(e); break;
        case ENT_SNOWBALL:
        case ENT_EGG: tickThrowable(e); break;
        case ENT_FISH: tickFish(e); break;
        case ENT_PAINTING: tickPainting(e); break;
        case ENT_MINECART: tickMinecart(e); break;
        case ENT_BOAT: tickBoat(e); break;
        case ENT_FIREBALL: tickFireball(e); break;
        default: tickMob(e); break;
    }
}

// ------------------------------------------------------------------ throwables, fishing, paintings

// EntitySnowball / EntityEgg / EntityFish constructors: thrown from the eye along the view.
void Game::throwItem(uint8_t type) {
    float yr = player.yaw * DEG, pr = player.pitch * DEG;
    float eye = type == ENT_FISH ? Player::EYE - 0.1f : Player::EYE + 0.12f - 0.1f;
    Entity* e = spawn(type, player.x - std::cos(yr) * 0.16f, player.y + eye, player.z - std::sin(yr) * 0.16f);
    if (!e) return;
    float dx = -std::sin(yr) * std::cos(pr), dz = std::cos(yr) * std::cos(pr), dy = -std::sin(pr);
    float l = std::sqrt(dx * dx + dy * dy + dz * dz);
    dx = (dx / l + randGaussian(rand) * 0.0075f) * 1.5f;
    dy = (dy / l + randGaussian(rand) * 0.0075f) * 1.5f;
    dz = (dz / l + randGaussian(rand) * 0.0075f) * 1.5f;
    e->mx = dx, e->my = dy, e->mz = dz;
    e->yaw = e->prevYaw = std::atan2(dx, dz) / DEG;
    e->pitch = std::atan2(dy, std::sqrt(dx * dx + dz * dz)) / DEG;
    e->fromPlayer = true;
    if (type == ENT_FISH) fishHook = (int)(e - ents);
}

namespace {
// World.isMaterialInBB for water.
bool waterInBox(const World& w, const AABB& b) {
    for (int x = ifloor(b.x0); x < ifloor(b.x1 + 1); ++x)
        for (int y = ifloor(b.y0); y < ifloor(b.y1 + 1); ++y)
            for (int z = ifloor(b.z0); z < ifloor(b.z1 + 1); ++z)
                if (blockIsWater(w.get(x, y, z))) return true;
    return false;
}
}  // namespace

void Game::tickThrowable(Entity& e) {
    ++e.airTicks;
    float speed = std::sqrt(e.mx * e.mx + e.my * e.my + e.mz * e.mz);
    if (speed < 1e-4f) speed = 1e-4f;
    float dx = e.mx / speed, dy = e.my / speed, dz = e.mz / speed;
    RayHit hit;
    bool blockHit = raycast(world, e.x, e.y, e.z, dx, dy, dz, speed, hit);
    float best = blockHit ? hit.t : speed;
    Entity* victim = nullptr;
    for (Entity& o : ents) {
        if (!collidable(o)) continue;
        float t;
        int face;
        if (rayBox(e.x, e.y, e.z, dx, dy, dz, hitBox(o).grow(0.3f), t, face) && t >= 0 && t < best) {
            best = t;
            victim = &o;
        }
    }
    bool hitPlayer = false;
    if (!dead && e.airTicks >= 5) {
        float t;
        int face;
        if (rayBox(e.x, e.y, e.z, dx, dy, dz, player.box().grow(0.3f), t, face) && t >= 0 && t < best) {
            victim = nullptr;
            hitPlayer = true;
        }
    }
    if (blockHit || victim || hitPlayer) {
        if (victim) hurtEntity(*victim, 0, e.x, e.z, e.fromPlayer);
        if (e.type == ENT_EGG && rand.nextInt(8) == 0) {
            int n = rand.nextInt(32) == 0 ? 4 : 1;
            for (int i = 0; i < n; ++i)
                if (Entity* c = spawn(ENT_CHICKEN, e.x, e.y, e.z)) c->yaw = c->prevYaw = e.yaw;
        }
        for (int i = 0; i < 8; ++i) particles.itemPiece(world, e.x, e.y, e.z, itemIcon(ItemStack{I_SNOWBALL, 1, 0}));
        e.dead = true;
        return;
    }
    e.x += e.mx, e.y += e.my, e.z += e.mz;
    e.yaw = std::atan2(e.mx, e.mz) / DEG;
    e.pitch = std::atan2(e.my, std::sqrt(e.mx * e.mx + e.mz * e.mz)) / DEG;
    float drag = blockIsWater(world.get(ifloor(e.x), ifloor(e.y), ifloor(e.z))) ? 0.8f : 0.99f;
    e.mx *= drag, e.my *= drag, e.mz *= drag;
    e.my -= 0.03f;
    if (e.y < -64) e.dead = true;
}

// EntityFish.onUpdate
void Game::tickFish(Entity& e) {
    int idx = (int)(&e - ents);
    float px = player.x - e.x, py = player.y + Player::EYE - e.y, pz = player.z - e.z;
    if (dead || inv.held().id != I_FISHING_ROD || fishHook != idx || px * px + py * py + pz * pz > 1024.0f) {
        e.dead = true;
        if (fishHook == idx) fishHook = -1;
        return;
    }
    if (e.hooked >= 0) {
        const Entity& o = ents[e.hooked];
        if (collidable(o)) {
            e.x = o.x;
            e.y = hitBox(o).y0 + o.height * 0.8f;
            e.z = o.z;
            return;
        }
        e.hooked = -1;
    }
    if (e.inGround) {
        if (++e.groundTicks >= 1200) {
            e.dead = true;
            fishHook = -1;
        }
        return;
    }
    ++e.airTicks;
    float speed = std::sqrt(e.mx * e.mx + e.my * e.my + e.mz * e.mz);
    if (speed > 1e-4f) {
        float dx = e.mx / speed, dy = e.my / speed, dz = e.mz / speed;
        RayHit hit;
        bool blockHit = raycast(world, e.x, e.y, e.z, dx, dy, dz, speed, hit);
        float best = blockHit ? hit.t : speed;
        Entity* victim = nullptr;
        for (Entity& o : ents) {
            if (!collidable(o)) continue;
            float t;
            int face;
            if (rayBox(e.x, e.y, e.z, dx, dy, dz, hitBox(o).grow(0.3f), t, face) && t >= 0 && t < best) {
                best = t;
                victim = &o;
            }
        }
        if (victim) {
            if (hurtEntity(*victim, 0, e.x, e.z, true)) e.hooked = (int16_t)(victim - ents);
            if (e.hooked >= 0) return;
        } else if (blockHit) {
            e.inGround = true;
            e.groundTicks = 0;
            return;
        }
    }
    e.move(world, e.mx, e.my, e.mz);
    float drag = (e.onGround || e.collidedH) ? 0.5f : 0.92f;
    AABB b = e.box();
    float frac = 0;
    for (int i = 0; i < 5; ++i) {
        AABB s = b;
        s.y0 = b.y0 + (b.y1 - b.y0) * i / 5.0f;
        s.y1 = b.y0 + (b.y1 - b.y0) * (i + 1) / 5.0f;
        if (waterInBox(world, s)) frac += 0.2f;
    }
    if (frac > 0) {
        if (e.biteTimer > 0) {
            --e.biteTimer;
        } else if (rand.nextInt(rainingAt(ifloor(e.x), ifloor(e.y) + 1, ifloor(e.z)) ? 300 : 500) == 0) {
            e.biteTimer = (int16_t)(rand.nextInt(30) + 10);
            e.my -= 0.2f;
            playSoundAt(e, "random.splash", 0.25f, 1.0f + (soundRand.nextFloat() - soundRand.nextFloat()) * 0.4f);
        }
    }
    if (e.biteTimer > 0) e.my -= rand.nextFloat() * rand.nextFloat() * rand.nextFloat() * 0.2f;
    e.my += 0.04f * (frac * 2.0f - 1.0f);
    if (frac > 0) {
        drag *= 0.9f;
        e.my *= 0.8f;
    }
    e.mx *= drag, e.my *= drag, e.mz *= drag;
}

// EntityFish.catchFish: returns the rod damage.
int Game::retrieveFish() {
    if (fishHook < 0) return 0;
    Entity& e = ents[fishHook];
    fishHook = -1;
    if (e.type != ENT_FISH) return 0;
    int dmg = 0;
    float dx = player.x - e.x, dy = player.y + Player::EYE - e.y, dz = player.z - e.z;
    float d = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (e.hooked >= 0 && collidable(ents[e.hooked])) {
        Entity& o = ents[e.hooked];
        o.mx += dx * 0.1f;
        o.my += dy * 0.1f + std::sqrt(d) * 0.08f;
        o.mz += dz * 0.1f;
        dmg = 3;
    } else if (e.biteTimer > 0) {
        if (Entity* f = spawn(ENT_ITEM, e.x, e.y, e.z)) {
            f->item = ItemStack(I_FISH_RAW);
            f->spin = rand.nextFloat() * 360.0f;
            f->mx = dx * 0.1f;
            f->my = dy * 0.1f + std::sqrt(d) * 0.08f;
            f->mz = dz * 0.1f;
        }
        dmg = 1;
    }
    if (e.inGround) dmg = 2;
    e.dead = true;
    return dmg;
}

void Game::setPaintingDir(Entity& e, int dir) {
    const PaintingArt& a = g_paintingArt[e.woolColor];
    e.fuse = (int16_t)dir;
    e.yaw = e.prevYaw = dir * 90.0f;
    float x = e.tileX + 0.5f, y = e.tileY + 0.5f, z = e.tileZ + 0.5f;
    const float off = 0.5625f;
    if (dir == 0) z -= off, x -= paintingOffset(a.w);
    if (dir == 1) x -= off, z += paintingOffset(a.w);
    if (dir == 2) z += off, x += paintingOffset(a.w);
    if (dir == 3) x += off, z -= paintingOffset(a.w);
    y += paintingOffset(a.h);
    e.x = e.prevX = x;
    e.y = e.prevY = y;
    e.z = e.prevZ = z;
}

// EntityPainting.onValidSurface
bool Game::paintingFits(const Entity& e) const {
    AABB b = paintingBox(e);
    if (e.collides(world, b)) return false;
    const PaintingArt& a = g_paintingArt[e.woolColor];
    bool alongX = e.fuse == 0 || e.fuse == 2;
    int bx = alongX ? ifloor(e.x - a.w / 32.0f) : e.tileX;
    int bz = alongX ? e.tileZ : ifloor(e.z - a.w / 32.0f);
    int by = ifloor(e.y - a.h / 32.0f);
    for (int i = 0; i < a.w / 16; ++i)
        for (int j = 0; j < a.h / 16; ++j) {
            uint8_t id = alongX ? world.get(bx + i, by + j, bz) : world.get(bx, by + j, bz + i);
            if (!blockMatSolid(id)) return false;
        }
    for (const Entity& o : ents)
        if (&o != &e && o.type == ENT_PAINTING && !o.dead && paintingBox(o).intersects(b)) return false;
    return true;
}

// ItemPainting.onItemUse
bool Game::placePainting(const RayHit& h) {
    if (h.face < 2) return false;
    static const int DIR[6] = {0, 0, 0, 2, 1, 3};
    int dir = DIR[h.face];
    Entity t;
    t.type = ENT_PAINTING;
    t.tileX = h.x, t.tileY = h.y, t.tileZ = h.z;
    int valid[32], n = 0;
    for (int a = 0; a < g_paintingArtCount; ++a) {
        t.woolColor = (uint8_t)a;
        setPaintingDir(t, dir);
        if (paintingFits(t)) valid[n++] = a;
    }
    if (!n) return false;
    t.woolColor = (uint8_t)valid[rand.nextInt(n)];
    setPaintingDir(t, dir);
    Entity* e = spawn(ENT_PAINTING, t.x, t.y, t.z);
    if (!e) return false;
    e->woolColor = t.woolColor;
    e->tileX = t.tileX, e->tileY = t.tileY, e->tileZ = t.tileZ;
    setPaintingDir(*e, dir);
    ItemStack& held = inv.held();
    if (mode == MODE_SURVIVAL && --held.count == 0) held.clear();
    return true;
}

void Game::tickPainting(Entity& e) {
    if (e.groundTicks++ == 100) {
        e.groundTicks = 0;
        if (!paintingFits(e)) {
            e.dead = true;
            if (mode == MODE_SURVIVAL) dropItem(ItemStack(I_PAINTING), e.x, e.y, e.z, false);
        }
    }
}

// BlockCake.eatCakeSlice
bool Game::eatCake(int x, int y, int z) {
    if (mode != MODE_SURVIVAL || health >= MAX_HEALTH) return false;
    health = health + 3 > MAX_HEALTH ? MAX_HEALTH : health + 3;
    int m = world.getMeta(x, y, z) + 1;
    if (m >= 6)
        setBlockNotify(x, y, z, AIR);
    else
        world.setMeta(x, y, z, m);
    return true;
}

void Game::jukeboxStop(int x, int y, int z) {
    if (!record.record || record.x != x || record.y != y || record.z != z) return;
    record.record = 0;
    ++record.serial;
}

// TextureCompassFX / TextureWatchFX needle damping.
void Game::tickDials() {
    auto approach = [](float target, float& angle, float& vel) {
        float d = target - angle;
        while (d < -PI) d += 2 * PI;
        while (d >= PI) d -= 2 * PI;
        d = clampf(d, -1.0f, 1.0f);
        vel += d * 0.1f;
        vel *= 0.8f;
        angle += vel;
        while (angle >= PI) angle -= 2 * PI;
        while (angle < -PI) angle += 2 * PI;
    };
    float dx = spawnX - player.x, dz = spawnZ - player.z;
    if (dimension < 0) {
        // TextureCompassFX / TextureWatchFX spin aimlessly outside the surface world
        approach(soundRand.nextFloat() * PI * 2.0f, compassAngle, compassVel);
        approach(-soundRand.nextFloat() * PI * 2.0f, clockAngle, clockVel);
        return;
    }
    approach((player.yaw - 90.0f) * DEG - std::atan2(dz, dx), compassAngle, compassVel);
    approach(-celestialAngle() * PI * 2.0f, clockAngle, clockVel);
}

void Game::tickItem(Entity& e) {
    e.my -= 0.04f;
    if (blockIsLava(world.get(ifloor(e.x), ifloor(e.y), ifloor(e.z)))) {
        e.my = 0.2f;
        e.mx = (rand.nextFloat() - rand.nextFloat()) * 0.2f;
        e.mz = (rand.nextFloat() - rand.nextFloat()) * 0.2f;
        if (e.age % 10 == 0) e.dead = true;
    }
    if (e.collides(world, e.box())) e.y += 0.1f;  // pushOutOfBlocks
    e.move(world, e.mx, e.my, e.mz);
    float f = e.onGround ? 0.6f * 0.98f : 0.98f;
    e.mx *= f;
    e.my *= 0.98f;
    e.mz *= f;
    if (e.onGround) e.my *= -0.5f;
    if (e.pickupDelay > 0) --e.pickupDelay;
    e.spin += 4.0f;
    if (e.age >= 6000 || e.y < -64) e.dead = true;
}

void Game::tickArrow(Entity& e) {
    if (e.inGround) {
        if (++e.groundTicks >= 1200) e.dead = true;
        if (world.get(ifloor(e.x), ifloor(e.y), ifloor(e.z)) == AIR) {
            e.inGround = false;
            e.mx *= rand.nextFloat() * 0.2f;
            e.my *= rand.nextFloat() * 0.2f;
            e.mz *= rand.nextFloat() * 0.2f;
        }
        return;
    }
    float speed = std::sqrt(e.mx * e.mx + e.my * e.my + e.mz * e.mz);
    if (speed < 1e-4f) speed = 1e-4f;
    float dx = e.mx / speed, dy = e.my / speed, dz = e.mz / speed;
    RayHit hit;
    bool blockHit = raycast(world, e.x, e.y, e.z, dx, dy, dz, speed, hit);
    float best = blockHit ? hit.t : speed;
    Entity* victim = nullptr;
    bool hitPlayer = false;
    for (Entity& o : ents) {
        if (!collidable(o)) continue;
        if (!e.fromPlayer && e.age < 5) continue;
        float t;
        int face;
        if (rayBox(e.x, e.y, e.z, dx, dy, dz, hitBox(o).grow(0.3f), t, face) && t >= 0 && t < best) {
            best = t;
            victim = &o;
        }
    }
    if (!e.fromPlayer && !dead) {
        float t;
        int face;
        if (rayBox(e.x, e.y, e.z, dx, dy, dz, player.box().grow(0.3f), t, face) && t >= 0 && t < best) {
            best = t;
            victim = nullptr;
            hitPlayer = true;
        }
    }
    auto drr = [&] { playSoundAt(e, "random.drr", 1.0f, 1.2f / (soundRand.nextFloat() * 0.2f + 0.9f)); };
    if (hitPlayer) {
        Entity* shooter = e.target >= 0 && ents[e.target].type == ENT_SKELETON ? &ents[e.target] : nullptr;
        if (hurtPlayer(4, e.x, e.z, true, shooter)) deathMessage = "was shot by a skeleton";
        drr();
        e.dead = true;
        return;
    }
    if (victim) {
        Entity* shooter = e.target >= 0 && entityIsMob(ents[e.target].type) ? &ents[e.target] : nullptr;
        hurtEntity(*victim, 4, e.x, e.z, e.fromPlayer, shooter);
        drr();
        e.dead = true;
        return;
    }
    if (blockHit) {
        drr();
        e.x += dx * (hit.t - 0.05f);
        e.y += dy * (hit.t - 0.05f);
        e.z += dz * (hit.t - 0.05f);
        e.inGround = true;
        e.groundTicks = 0;
        e.mx = e.my = e.mz = 0;
        return;
    }
    e.x += e.mx, e.y += e.my, e.z += e.mz;
    e.yaw = std::atan2(e.mx, e.mz) / DEG;
    e.pitch = std::atan2(e.my, std::sqrt(e.mx * e.mx + e.mz * e.mz)) / DEG;
    float drag = blockIsWater(world.get(ifloor(e.x), ifloor(e.y), ifloor(e.z))) ? 0.8f : 0.99f;
    e.mx *= drag, e.my *= drag, e.mz *= drag;
    e.my -= 0.03f;
    if (e.y < -64 || e.age > 1200) e.dead = true;
}

void Game::tickTnt(Entity& e) {
    particles.spawn(world, PART_SMOKE, e.x, e.y + 0.5f, e.z, 0, 0, 0);
    e.my -= 0.04f;
    e.move(world, e.mx, e.my, e.mz);
    e.mx *= 0.98f, e.my *= 0.98f, e.mz *= 0.98f;
    if (e.onGround) {
        e.mx *= 0.7f, e.mz *= 0.7f;
        e.my *= -0.5f;
    }
    if (--e.fuse <= 0) {
        bool wasDead = dead;
        e.dead = true;
        explode(e.x, e.y, e.z, 4.0f);
        if (!wasDead && dead) deathMessage = "blew up";
    }
}

void Game::tickMob(Entity& e) {
    if (e.hurtTime > 0) --e.hurtTime;
    if (e.hurtResist > 0) --e.hurtResist;
    if (e.attackTime > 0) --e.attackTime;
    if (e.health <= 0) {
        if (++e.deathTime >= 20) {
            deathPoof(e);
            removeMob(e);
        }
        if (e.type == ENT_SQUID) {
            e.move(world, e.mx, e.my, e.mz);
            e.mx *= 0.9f, e.my = (e.my - 0.08f) * 0.98f, e.mz *= 0.9f;
        } else {
            e.walkTick(world, 0, 0, false);
        }
        return;
    }
    e.updateLiquids(world);
    bool fireImmune = e.type == ENT_GHAST || e.type == ENT_PIGZOMBIE;
    if (e.inLava && !fireImmune) {
        hurtEntity(e, 4, e.x, e.z, false);
        e.fire = 600;
    }
    bool wet = e.inWater || rainingAt(ifloor(e.x), ifloor(e.y), ifloor(e.z));
    if (!fireImmune && boxTouches(world, e.box().grow(-0.001f), FIRE)) {
        hurtEntity(e, 1, e.x, e.z, false);
        if (!wet) e.fire = e.fire <= 0 ? 300 : e.fire + 1;
    }
    if (fireImmune) {
        e.fire = 0;
    } else if (e.fire > 0) {
        if (wet) {
            if (!e.inWater)
                playSoundAt(e, "random.fizz", 0.7f, 1.6f + (soundRand.nextFloat() - soundRand.nextFloat()) * 0.4f);
            e.fire = 0;
        } else {
            if (e.fire % 20 == 0) hurtEntity(e, 1, e.x, e.z, false);
            --e.fire;
        }
    }
    if (e.health <= 0) return;
    if (soundRand.nextInt(1000) < e.livingSoundTime++) {
        e.livingSoundTime = -80;
        const MobSounds& ms = mobSounds(e.type);
        const char* living = ms.living;
        if (e.type == ENT_WOLF) {
            if (e.angry)
                living = "mob.wolf.growl";
            else if (soundRand.nextInt(3) == 0)
                living = e.tamed && e.health < 10 ? "mob.wolf.whine" : "mob.wolf.panting";
            else
                living = "mob.wolf.bark";
        }
        playSoundAt(e, living, ms.volume, mobPitch());
    }
    if ((e.type == ENT_ZOMBIE || e.type == ENT_SKELETON) && isDaytime()) {
        float b = lightBrightness(world.getBrightness(ifloor(e.x), ifloor(e.y + 1.6f), ifloor(e.z)));
        if (b > 0.5f && ifloor(e.y) >= world.heightAt(ifloor(e.x), ifloor(e.z)) &&
            rand.nextFloat() * 30.0f < (b - 0.4f) * 2.0f)
            e.fire = 300;
    }
    if (e.landedFall > 0) {
        int d = (int)std::ceil(e.landedFall - 3.0f);
        e.landedFall = 0;
        if (d > 0 && e.type != ENT_CHICKEN && e.type != ENT_GHAST) hurtEntity(e, d, e.x, e.z, false);
        if (e.health <= 0) return;
    }
    // EntityPigZombie.onUpdate: the delayed angry grunt
    if (e.type == ENT_PIGZOMBIE && e.jumpDelay > 0 && --e.jumpDelay == 0)
        playSoundAt(e, "mob.zombiepig.zpigangry", 2.0f, mobPitch() * 1.8f);

    switch (e.type) {
        case ENT_GHAST: tickGhast(e); break;
        case ENT_SLIME: {
            // EntitySlime.onUpdate: squish when jumping and landing
            e.prevSquish = e.squish;
            bool wasOnGround = e.onGround;
            slimeAI(e);
            e.walkTick(world, e.moveForward, e.moveStrafe, e.jumping);
            if (e.onGround && !wasOnGround) {
                for (int i = 0; i < e.size * 8; ++i) {
                    float a = particles.rnd() * 3.1415927f * 2.0f, f = particles.rnd() * 0.5f + 0.5f;
                    particles.itemPiece(world, e.x + mh::sin(a) * e.size * 0.5f * f, e.y,
                                        e.z + mh::cos(a) * e.size * 0.5f * f, itemIcon(ItemStack{I_SLIMEBALL, 1, 0}));
                }
                if (e.size > 2)
                    playSoundAt(e, "mob.slime", 0.6f,
                                ((soundRand.nextFloat() - soundRand.nextFloat()) * 0.2f + 1.0f) / 0.8f);
                e.squish = -0.5f;
            }
            e.squish *= 0.6f;
            break;
        }
        case ENT_SQUID: squidTick(e); break;
        case ENT_WOLF:
            wolfAI(e);
            e.walkTick(world, e.moveForward, e.moveStrafe, e.jumping);
            wolfUpdate(e);
            break;
        default:
            mobAI(e);
            e.walkTick(world, e.moveForward, e.moveStrafe, e.jumping);
            break;
    }
    if (e.dead) return;
    bodySounds(e);
    if (e.type == ENT_SPIDER && e.collidedH) e.my = 0.2f;
    if (e.type == ENT_CHICKEN) {
        if (!e.onGround && e.my < 0) e.my *= 0.6f;
        if (--e.eggTimer <= 0) {
            playSoundAt(e, "mob.chickenplop", 1.0f, mobPitch());
            dropItem(ItemStack(I_EGG), e.x, e.y, e.z, false);
            e.eggTimer = rand.nextInt(6000) + 6000;
        }
    }
    float mdx = e.x - e.prevX, mdz = e.z - e.prevZ;
    float f = std::sqrt(mdx * mdx + mdz * mdz) * 4.0f;
    if (f > 1) f = 1;
    e.prevLimbAmount = e.limbAmount;
    e.limbAmount += (f - e.limbAmount) * 0.4f;
    e.limbSwing += e.limbAmount;

    // EntityLiving.despawnEntity
    if (entityIsHostile(e.type) || e.type == ENT_SQUID) {
        float dx = player.x - e.x, dy = player.y - e.y, dz = player.z - e.z;
        float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 > 128.0f * 128.0f) e.dead = true;
        if (e.age > 600 && rand.nextInt(800) == 0) {
            if (d2 < 32.0f * 32.0f)
                e.age = 0;
            else
                e.dead = true;
        }
    }
    if (e.y < -64) e.dead = true;
}

void Game::mobAI(Entity& e) {
    const float speed = e.type == ENT_PIGZOMBIE && e.targeting ? 0.95f : MOBS[e.type].speed;
    Entity* foe = entityIsHostile(e.type) ? targetOf(e) : nullptr;
    float px = player.x, py = player.y, pz = player.z;
    if (foe) px = foe->x, py = foe->y, pz = foe->z;
    float aimY = foe ? foe->y + foe->height * 0.85f : py + Player::EYE;
    AABB foeBox = foe ? foe->box() : player.box();
    float dx = px - e.x, dy = py - e.y, dz = pz - e.z;
    float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    e.moveForward = e.moveStrafe = 0;
    e.jumping = false;
    bool attacking = false;

    if (foe) {
        e.targeting = true;
    } else if (entityIsHostile(e.type)) {
        bool canTarget = !dead && mode == MODE_SURVIVAL && dist < 16.0f;
        // EntityPigZombie.findPlayerToAttack: only once angered, and then it does not give up
        if (e.type == ENT_PIGZOMBIE) canTarget = !dead && mode == MODE_SURVIVAL && e.angry && dist < 48.0f;
        if (e.type == ENT_SPIDER && !e.targeting) {
            float b = lightBrightness(world.getBrightness(ifloor(e.x), ifloor(e.y + 0.5f), ifloor(e.z)));
            canTarget = canTarget && b < 0.5f;
        }
        if (!canTarget) {
            e.targeting = false;
        } else {
            e.targeting = true;
        }
    }
    if (e.targeting) {
        float eyeY = e.y + e.height * 0.85f;
        bool see = canSee(e, px, aimY, pz);
        if (see) {
            switch (e.type) {
                case ENT_SKELETON:
                    if (dist < 10.0f) {
                        if (e.attackTime == 0) {
                            float ax = e.x, ay = eyeY - 0.1f, az = e.z;
                            float tdx = px - ax, tdz = pz - az;
                            float tdy = aimY - 0.2f - ay;
                            float hz = std::sqrt(tdx * tdx + tdz * tdz) * 0.2f;
                            shootArrow(&e, ax, ay, az, tdx, tdy + hz, tdz, 0.6f, 12.0f, false);
                            playSoundAt(e, "random.bow", 1.0f, 1.0f / (soundRand.nextFloat() * 0.4f + 0.8f));
                            e.attackTime = 30;
                        }
                        e.yaw = std::atan2(dz, dx) / DEG - 90.0f;
                        attacking = true;
                    }
                    break;
                case ENT_CREEPER: {
                    if ((e.fuse <= 0 && dist < 3.0f) || (e.fuse > 0 && dist < 7.0f)) {
                        e.prevFuse = e.fuse;
                        if (e.fuse == 0) playSoundAt(e, "random.fuse", 1.0f, 0.5f);
                        if (++e.fuse >= 30) {
                            bool wasDead = dead;
                            e.dead = true;
                            explode(e.x, e.y, e.z, e.powered ? 6.0f : 3.0f);
                            if (!wasDead && dead) deathMessage = "was blown up by a creeper";
                            return;
                        }
                        attacking = true;
                    } else {
                        e.prevFuse = e.fuse;
                        if (e.fuse > 0) --e.fuse;
                    }
                    break;
                }
                case ENT_SPIDER: {
                    float b = lightBrightness(world.getBrightness(ifloor(e.x), ifloor(e.y + 0.5f), ifloor(e.z)));
                    if (b > 0.5f && rand.nextInt(100) == 0) {
                        e.targeting = false;
                        e.target = -1;
                        break;
                    }
                    if (dist > 2.0f && dist < 6.0f && rand.nextInt(10) == 0) {
                        if (e.onGround) {
                            float hd = std::sqrt(dx * dx + dz * dz);
                            e.mx = dx / hd * 0.5f * 0.8f + e.mx * 0.2f;
                            e.mz = dz / hd * 0.5f * 0.8f + e.mz * 0.2f;
                            e.my = 0.4f;
                        }
                        break;
                    }
                }
                    [[fallthrough]];
                default:
                    if (e.attackTime <= 0 && dist < 2.0f && foeBox.y1 > e.box().y0 && foeBox.y0 < e.box().y1) {
                        e.attackTime = 20;
                        int strength = e.type == ENT_ZOMBIE || e.type == ENT_PIGZOMBIE ? 5 : 2;
                        if (foe)
                            hurtEntity(*foe, strength, e.x, e.z, false, &e);
                        else if (hurtPlayer(strength, e.x, e.z, true, &e))
                            deathMessage = e.type == ENT_ZOMBIE      ? "was slain by a zombie"
                                           : e.type == ENT_PIGZOMBIE ? "was slain by a zombie pigman"
                                                                     : "was slain by a spider";
                    }
                    break;
            }
        } else if (e.type == ENT_CREEPER && e.fuse > 0) {
            e.prevFuse = e.fuse;
            --e.fuse;
        }
        if (!attacking) {
            float target = std::atan2(dz, dx) / DEG - 90.0f;
            float diff = target - e.yaw;
            while (diff < -180) diff += 360;
            while (diff >= 180) diff -= 360;
            if (diff > 30) diff = 30;
            if (diff < -30) diff = -30;
            e.yaw += diff;
            e.moveForward = speed;
            if (py > e.y + 0.5f && std::sqrt(dx * dx + dz * dz) < 2.0f) e.jumping = true;
        }
    } else {
        if (e.type == ENT_CREEPER && e.fuse > 0) {
            e.prevFuse = e.fuse;
            --e.fuse;
        }
        // EntityCreature.updateWanderPath, without path finding.
        if (e.wanderTicks <= 0 && rand.nextInt(80) == 0) {
            float best = -99999.0f;
            for (int i = 0; i < 10; ++i) {
                int wx = ifloor(e.x + rand.nextInt(13) - 6), wy = ifloor(e.y + rand.nextInt(7) - 3),
                    wz = ifloor(e.z + rand.nextInt(13) - 6);
                float weight;
                if (entityIsAnimal(e.type))
                    weight = world.get(wx, wy - 1, wz) == GRASS ? 10.0f
                                                                 : lightBrightness(world.getBrightness(wx, wy, wz)) - 0.5f;
                else
                    weight = 0.5f - lightBrightness(world.getBrightness(wx, wy, wz));
                if (weight > best) {
                    best = weight;
                    e.wanderX = wx + 0.5f;
                    e.wanderZ = wz + 0.5f;
                }
            }
            e.wanderTicks = 100;
        }
        if (e.wanderTicks > 0) {
            --e.wanderTicks;
            float wdx = e.wanderX - e.x, wdz = e.wanderZ - e.z;
            if (wdx * wdx + wdz * wdz < 1.0f) {
                e.wanderTicks = 0;
            } else {
                float target = std::atan2(wdz, wdx) / DEG - 90.0f;
                float diff = target - e.yaw;
                while (diff < -180) diff += 360;
                while (diff >= 180) diff -= 360;
                if (diff > 30) diff = 30;
                if (diff < -30) diff = -30;
                e.yaw += diff;
                e.moveForward = speed;
            }
        } else if (rand.nextInt(20) == 0) {
            e.yaw += (rand.nextFloat() - 0.5f) * 60.0f;
        }
    }
    if (e.moveForward > 0 && e.collidedH) e.jumping = true;
    if ((e.inWater || e.inLava) && rand.nextFloat() < 0.8f) e.jumping = true;
}

Entity* Game::targetOf(const Entity& e) {
    if (e.target < 0 || e.target >= MAX_ENTITIES) return nullptr;
    Entity& t = ents[e.target];
    return entityIsMob(t.type) && t.health > 0 && &t != &e ? &t : nullptr;
}

// EntityLiving.faceEntity / updateRotation (yaw only).
void Game::faceTowards(Entity& e, float x, float z, float maxTurn) {
    float target = std::atan2(z - e.z, x - e.x) / DEG - 90.0f;
    float diff = target - e.yaw;
    while (diff < -180) diff += 360;
    while (diff >= 180) diff -= 360;
    diff = clampf(diff, -maxTurn, maxTurn);
    e.yaw += diff;
}

// EntityLiving.canEntityBeSeen, from the mob's eyes to a point.
bool Game::canSee(const Entity& e, float x, float y, float z) const {
    float eyeY = e.y + e.height * 0.85f;
    float dx = x - e.x, dy = y - eyeY, dz = z - e.z;
    float l = std::sqrt(dx * dx + dy * dy + dz * dz);
    RayHit h;
    return l < 0.01f || !raycast(world, e.x, eyeY, e.z, dx / l, dy / l, dz / l, l, h);
}

// EntityPlayer.alertWolves: tamed wolves near the player go after what the player fought.
void Game::alertWolves(Entity& victim, bool playerAttacked) {
    if (victim.type == ENT_CREEPER || (victim.type == ENT_WOLF && victim.tamed)) return;
    float eyeY = player.y + Player::EYE;
    for (Entity& w : ents) {
        if (w.type != ENT_WOLF || &w == &victim || !w.tamed || w.health <= 0) continue;
        if (w.targeting || targetOf(w) || (playerAttacked && w.sitting)) continue;
        if (w.x < player.x - 16.5f || w.x > player.x + 17.5f || w.z < player.z - 16.5f || w.z > player.z + 17.5f ||
            w.y + w.height < eyeY - 4 || w.y > eyeY + 5)
            continue;
        w.sitting = false;
        w.target = (int16_t)(&victim - ents);
    }
}

// EntitySlime.updateEntityActionState: hop towards the player.
void Game::slimeAI(Entity& e) {
    float dx = player.x - e.x, dy = player.y + Player::EYE - e.y, dz = player.z - e.z;
    bool near = !dead && mode == MODE_SURVIVAL && dx * dx + dy * dy + dz * dz < 16.0f * 16.0f;
    if (near) faceTowards(e, player.x, player.z, 10.0f);
    if (e.onGround && e.jumpDelay-- <= 0) {
        e.jumpDelay = (int16_t)(rand.nextInt(20) + 10);
        if (near) e.jumpDelay /= 3;
        e.jumping = true;
        if (e.size > 1)
            playSoundAt(e, "mob.slime", 0.6f, ((soundRand.nextFloat() - soundRand.nextFloat()) * 0.2f + 1.0f) * 0.8f);
        e.squish = 1.0f;
        e.moveStrafe = 1.0f - rand.nextFloat() * 2.0f;
        e.moveForward = (float)e.size;
    } else {
        e.jumping = false;
        if (e.onGround) e.moveStrafe = e.moveForward = 0;
    }
}

// EntitySquid.onLivingUpdate: swims in pulses while in water, flops around on land.
void Game::squidTick(Entity& e) {
    if (rand.nextInt(50) == 0 || !e.inWater || (e.swimX == 0 && e.swimY == 0 && e.swimZ == 0)) {
        float a = rand.nextFloat() * PI * 2.0f;
        e.swimX = std::cos(a) * 0.2f;
        e.swimY = -0.1f + rand.nextFloat() * 0.2f;
        e.swimZ = std::sin(a) * 0.2f;
    }
    e.move(world, e.mx, e.my, e.mz);
    e.prevBodyPitch = e.bodyPitch;
    e.prevBodySpin = e.bodySpin;
    e.prevTentacle = e.tentacle;
    e.phase += e.phaseSpeed;
    if (e.phase > PI * 2.0f) {
        e.phase -= PI * 2.0f;
        if (rand.nextInt(10) == 0) e.phaseSpeed = 1.0f / (rand.nextFloat() + 1.0f) * 0.2f;
    }
    // isInWater: the box shrunk by 0.6 at both ends, as handleMaterialAcceleration scans it.
    bool swimming = false;
    AABB b = e.box();
    int y0 = ifloor(b.y0 + 0.6f), y1 = ifloor(b.y1 - 0.6f + 1.0f);
    for (int x = ifloor(b.x0); x < ifloor(b.x1 + 1.0f) && !swimming; ++x)
        for (int y = y0; y < y1 && !swimming; ++y)
            for (int z = ifloor(b.z0); z < ifloor(b.z1 + 1.0f); ++z)
                if (blockIsWater(world.get(x, y, z))) {
                    swimming = true;
                    break;
                }
    if (swimming) {
        if (e.phase < PI) {
            float f = e.phase / PI;
            e.tentacle = std::sin(f * f * PI) * PI * 0.25f;
            if (f > 0.75f) {
                e.thrust = 1.0f;
                e.spinSpeed = 1.0f;
            } else {
                e.spinSpeed *= 0.8f;
            }
        } else {
            e.tentacle = 0;
            e.thrust *= 0.9f;
            e.spinSpeed *= 0.99f;
        }
        e.mx = e.swimX * e.thrust;
        e.my = e.swimY * e.thrust;
        e.mz = e.swimZ * e.thrust;
        float h = std::sqrt(e.mx * e.mx + e.mz * e.mz);
        e.yaw += (-std::atan2(e.mx, e.mz) / DEG - e.yaw) * 0.1f;
        e.bodySpin += PI * e.spinSpeed * 1.5f;
        e.bodyPitch += (-std::atan2(h, e.my) / DEG - e.bodyPitch) * 0.1f;
    } else {
        e.tentacle = std::fabs(std::sin(e.phase)) * PI * 0.25f;
        e.mx = e.mz = 0;
        e.my = (e.my - 0.08f) * 0.98f;
        e.bodyPitch += (-90.0f - e.bodyPitch) * 0.02f;
    }
}

float Game::wolfTailAngle(const Entity& e) {
    if (e.angry) return 1.5393804f;
    if (e.tamed) return (0.55f - (20 - e.health) * 0.02f) * PI;
    return 0.62831855f;
}

// EntityWolf / EntityCreature.updateEntityActionState without path finding.
void Game::wolfAI(Entity& e) {
    const float speed = MOBS[ENT_WOLF].speed;
    e.moveForward = e.moveStrafe = 0;
    e.jumping = false;
    bool blocked = e.sitting || e.shaking;
    float eyeY = player.y + Player::EYE;
    float pdx = player.x - e.x, pdy = eyeY - e.y, pdz = player.z - e.z;
    float playerDist = std::sqrt(pdx * pdx + pdy * pdy + pdz * pdz);
    bool playerTargetable = !dead && mode == MODE_SURVIVAL;

    Entity* foe = targetOf(e);
    if (foe) e.targeting = false;
    if (e.targeting && !playerTargetable) e.targeting = false;
    if (!foe && !e.targeting && e.angry && playerTargetable && playerDist < 16.0f) e.targeting = true;

    auto walkTo = [&](float tx, float ty, float tz) {
        float target = std::atan2(tz - e.z, tx - e.x) / DEG - 90.0f;
        float diff = target - e.yaw;
        while (diff < -180) diff += 360;
        while (diff >= 180) diff -= 360;
        e.yaw += clampf(diff, -30.0f, 30.0f);
        e.moveForward = speed;
        float hx = tx - e.x, hz = tz - e.z;
        if (ty > e.y + 0.5f && hx * hx + hz * hz < 4.0f) e.jumping = true;
    };

    if (foe || e.targeting) {
        float tx = foe ? foe->x : player.x, tz = foe ? foe->z : player.z;
        float ty = foe ? foe->y : eyeY;
        float aimY = foe ? foe->y + foe->height * 0.85f : eyeY;
        AABB tb = foe ? foe->box() : player.box();
        float dx = tx - e.x, dy = ty - e.y, dz = tz - e.z;
        float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (canSee(e, tx, aimY, tz)) {
            // EntityWolf.attackEntity: pounce from a distance, bite up close
            if (dist > 2.0f && dist < 6.0f && rand.nextInt(10) == 0) {
                if (e.onGround) {
                    float hd = std::sqrt(dx * dx + dz * dz);
                    e.mx = dx / hd * 0.5f * 0.8f + e.mx * 0.2f;
                    e.mz = dz / hd * 0.5f * 0.8f + e.mz * 0.2f;
                    e.my = 0.4f;
                }
            } else if (dist < 1.5f && tb.y1 > e.box().y0 && tb.y0 < e.box().y1) {
                e.attackTime = 20;
                int dmg = e.tamed ? 4 : 2;
                if (foe)
                    hurtEntity(*foe, dmg, e.x, e.z, false, &e);
                else if (hurtPlayer(dmg, e.x, e.z, true, &e))
                    deathMessage = "was slain by a wolf";
            }
        }
        if (!blocked) walkTo(tx, foe ? foe->y : player.y, tz);
    } else if (e.tamed && !blocked && playerDist > 5.0f) {
        // follow the owner, teleporting next to them when too far behind
        if (playerDist > 16.0f || (playerDist > 12.0f && e.collidedH)) {
            int bx = ifloor(player.x) - 2, bz = ifloor(player.z) - 2, by = ifloor(player.box().y0);
            for (int i = 0; i <= 4; ++i)
                for (int j = 0; j <= 4; ++j) {
                    if (i >= 1 && j >= 1 && i <= 3 && j <= 3) continue;
                    if (blockIsNormalCube(world.get(bx + i, by - 1, bz + j)) &&
                        !blockIsNormalCube(world.get(bx + i, by, bz + j)) &&
                        !blockIsNormalCube(world.get(bx + i, by + 1, bz + j))) {
                        e.setPosition(bx + i + 0.5f, (float)by, bz + j + 0.5f);
                        e.prevX = e.x, e.prevY = e.y, e.prevZ = e.z;
                        e.mx = e.my = e.mz = 0;
                        i = j = 5;
                    }
                }
        } else {
            walkTo(player.x, player.y, player.z);
        }
        e.wanderTicks = 0;
    } else if (!blocked) {
        if (!e.tamed && rand.nextInt(100) == 0) {
            int sheep[MAX_ENTITIES], n = 0;
            for (int i = 0; i < MAX_ENTITIES; ++i) {
                const Entity& s = ents[i];
                if (s.type == ENT_SHEEP && s.health > 0 && s.x > e.x - 16.5f && s.x < e.x + 17.5f &&
                    s.z > e.z - 16.5f && s.z < e.z + 17.5f && s.y + s.height > e.y - 4 && s.y < e.y + 5)
                    sheep[n++] = i;
            }
            if (n) e.target = (int16_t)sheep[rand.nextInt(n)];
        }
        if (e.wanderTicks <= 0 && rand.nextInt(80) == 0) {
            float best = -99999.0f;
            for (int i = 0; i < 10; ++i) {
                int wx = ifloor(e.x + rand.nextInt(13) - 6), wy = ifloor(e.y + rand.nextInt(7) - 3),
                    wz = ifloor(e.z + rand.nextInt(13) - 6);
                float weight = world.get(wx, wy - 1, wz) == GRASS
                                   ? 10.0f
                                   : lightBrightness(world.getBrightness(wx, wy, wz)) - 0.5f;
                if (weight > best) {
                    best = weight;
                    e.wanderX = wx + 0.5f;
                    e.wanderZ = wz + 0.5f;
                }
            }
            e.wanderTicks = 100;
        }
        if (e.wanderTicks > 0) {
            --e.wanderTicks;
            float wdx = e.wanderX - e.x, wdz = e.wanderZ - e.z;
            if (wdx * wdx + wdz * wdz < 1.0f)
                e.wanderTicks = 0;
            else
                walkTo(e.wanderX, e.y, e.wanderZ);
        }
    }
    // EntityLiving: glance at a nearby player now and then
    if (e.moveForward == 0 && !foe && !e.targeting) {
        if (e.lookTicks > 0) {
            --e.lookTicks;
            faceTowards(e, player.x, player.z, 10.0f);
            if (playerDist > 8.0f) e.lookTicks = 0;
        } else if (rand.nextFloat() < 0.02f && playerDist < 8.0f && !dead) {
            e.lookTicks = (int16_t)(10 + rand.nextInt(20));
        }
    } else {
        e.lookTicks = 0;
    }
    if (e.inWater) e.sitting = false;
    if (e.moveForward > 0 && e.collidedH) e.jumping = true;
    if ((e.inWater || e.inLava) && rand.nextFloat() < 0.8f) e.jumping = true;
}

// EntityWolf.onLivingUpdate / onUpdate: begging and shaking off water.
void Game::wolfUpdate(Entity& e) {
    bool moving = e.moveForward != 0;
    e.begging = false;
    if (e.lookTicks > 0 && !moving && !e.angry) {
        const ItemStack& h = inv.main[inv.selected];
        if (!e.tamed && h.id == I_BONE) e.begging = true;
        if (e.tamed && (h.id == I_PORK_RAW || h.id == I_PORK_COOKED)) e.begging = true;
    }
    if (e.wet && !e.shaking && !moving && e.onGround) {
        e.shaking = true;
        e.shake = e.prevShake = 0;
    }
    e.prevBeg = e.beg;
    e.beg += ((e.begging ? 1.0f : 0.0f) - e.beg) * 0.4f;
    if (e.begging) e.lookTicks = 10;
    if (e.inWater || rainingAt(ifloor(e.x), ifloor(e.y), ifloor(e.z))) {
        e.wet = true;
        e.shaking = false;
        e.shake = e.prevShake = 0;
    } else if (e.shaking) {
        if (e.shake == 0)
            playSoundAt(e, "mob.wolf.shake", 0.4f, (soundRand.nextFloat() - soundRand.nextFloat()) * 0.2f + 1.0f);
        e.prevShake = e.shake;
        e.shake += 0.05f;
        if (e.prevShake >= 2.0f) {
            e.wet = e.shaking = false;
            e.shake = e.prevShake = 0;
        }
        if (e.shake > 0.4f) {
            int n = (int)(mh::sin((e.shake - 0.4f) * 3.1415927f) * 7.0f);
            for (int i = 0; i < n; ++i)
                particles.spawn(world, PART_SPLASH, e.x + (particles.rnd() * 2 - 1) * e.width * 0.5f, e.y + 0.8f,
                                e.z + (particles.rnd() * 2 - 1) * e.width * 0.5f, e.mx, e.my, e.mz);
        }
    }
}

// EntityWolf.interact: tame with bones, heal with pork, sit or stand on command.
bool Game::useOnWolf(Entity& e) {
    ItemStack& h = inv.held();
    auto consume = [&] {
        if (mode == MODE_SURVIVAL && --h.count == 0) h.clear();
    };
    if (!e.tamed) {
        if (h.id != I_BONE || e.angry) return false;
        consume();
        if (rand.nextInt(3) == 0) {
            e.tamed = true;
            e.sitting = true;
            e.health = 20;
            e.target = -1;
            e.targeting = false;
            e.wanderTicks = 0;
            heartsOrSmoke(e, true);
        } else {
            heartsOrSmoke(e, false);
        }
        return true;
    }
    if ((h.id == I_PORK_RAW || h.id == I_PORK_COOKED) && e.health < 20) {
        consume();
        e.health = (int16_t)(e.health + 3 > 20 ? 20 : e.health + 3);
        return true;
    }
    e.sitting = !e.sitting;
    e.jumping = false;
    e.wanderTicks = 0;
    return true;
}

// EntityMob.isValidLightLevel
bool Game::monsterLightOk(int x, int y, int z) {
    if (world.getSkyLight(x, y, z) > rand.nextInt(32)) return false;
    int b = world.getBrightness(x, y, z);
    if (isThundering()) {
        uint8_t l = world.getLight(x, y, z);
        int s = (l >> 4) - 10, k = l & 15;
        b = s > k ? s : k;
    }
    return b <= rand.nextInt(8);
}

// TileEntityMobSpawner.updateEntity
void Game::tickSpawner(TileEntity& t) {
    t.prevSpin = t.spin;
    float dx = t.x + 0.5f - player.x, dy = t.y + 0.5f - (player.y + Player::EYE), dz = t.z + 0.5f - player.z;
    if (dx * dx + dy * dy + dz * dz >= 16.0f * 16.0f) return;
    {
        float px = t.x + particles.rnd(), py = t.y + particles.rnd(), pz = t.z + particles.rnd();
        particles.spawn(world, PART_SMOKE, px, py, pz, 0, 0, 0);
        particles.spawn(world, PART_FLAME, px, py, pz, 0, 0, 0);
    }
    t.spin += 1000.0f / (t.burnTime + 200.0f);
    while (t.spin > 360.0f) {
        t.spin -= 360.0f;
        t.prevSpin -= 360.0f;
    }
    auto resetDelay = [&] {
        t.burnTime = (int16_t)(200 + rand.nextInt(600));
        if (Chunk* c = world.chunk(t.x >> 4, t.z >> 4)) c->flags |= CHUNK_NEEDS_SAVE;
    };
    if (t.burnTime < 0) resetDelay();
    if (t.burnTime > 0) {
        --t.burnTime;
        return;
    }
    uint8_t type = (uint8_t)t.burnTotal;
    if (!entityIsMob(type) || type >= ENT_TYPE_COUNT) return;
    for (int i = 0; i < 4; ++i) {
        int nearby = 0;
        for (const Entity& o : ents)
            if (o.type == type && o.x + o.width / 2 > t.x - 8 && o.x - o.width / 2 < t.x + 9 &&
                o.z + o.width / 2 > t.z - 8 && o.z - o.width / 2 < t.z + 9 && o.y + o.height > t.y - 4 &&
                o.y < t.y + 5)
                ++nearby;
        if (nearby >= 6) {
            resetDelay();
            return;
        }
        float ex = t.x + (float)(rand.nextDouble() - rand.nextDouble()) * 4.0f;
        float ey = (float)(t.y + rand.nextInt(3) - 1);
        float ez = t.z + (float)(rand.nextDouble() - rand.nextDouble()) * 4.0f;
        // EntityMob.getCanSpawnHere: light, a clear box without liquid and nothing standing there
        Body probe;
        probe.width = MOBS[type].width;
        probe.height = MOBS[type].height;
        probe.setPosition(ex, ey, ez);
        AABB b = probe.box();
        int bx = ifloor(ex), by = ifloor(b.y0), bz = ifloor(ez);
        if (!monsterLightOk(bx, by, bz)) continue;
        if (lightBrightness(world.getBrightness(bx, by, bz)) > 0.5f) continue;
        if (probe.collides(world, b)) continue;
        bool liquid = false;
        for (int x = ifloor(b.x0); x <= ifloor(b.x1) && !liquid; ++x)
            for (int y = ifloor(b.y0); y <= ifloor(b.y1) && !liquid; ++y)
                for (int z = ifloor(b.z0); z <= ifloor(b.z1); ++z)
                    if (blockIsLiquid(world.get(x, y, z))) liquid = true;
        if (liquid) continue;
        bool blocked = !dead && b.intersects(player.box());
        for (const Entity& o : ents)
            if (!blocked && entityIsMob(o.type) && o.health > 0 && b.intersects(o.box())) blocked = true;
        if (blocked) continue;
        Entity* m = spawn(type, ex, ey, ez);
        if (!m) return;
        m->yaw = m->prevYaw = rand.nextFloat() * 360.0f;
        deathPoof(*m);
        resetDelay();
    }
}

bool Game::canSpawnAt(int x, int y, int z) const {
    if (y < 1 || y >= World::SY - 2) return false;
    if (!blockIsNormalCube(world.get(x, y - 1, z))) return false;
    uint8_t a = world.get(x, y, z), b = world.get(x, y + 1, z);
    return !blockIsSolid(a) && !blockIsLiquid(a) && !blockIsSolid(b) && !blockIsLiquid(b);
}

// SpawnerAnimals.performSpawning, simplified to a few attempts per tick around the player.
void Game::spawnMobs() {
    int hostile = 0, animals = 0, squids = 0;
    for (const Entity& e : ents) {
        if (entityIsHostile(e.type)) ++hostile;
        if (entityIsAnimal(e.type)) ++animals;
        if (e.type == ENT_SQUID) ++squids;
    }
    int range = (renderDistance + 1) * 16;
    if (range > 64) range = 64;
    auto randomSpot = [&](int& x, int& y, int& z) {
        x = ifloor(player.x) + rand.nextInt(range * 2) - range;
        z = ifloor(player.z) + rand.nextInt(range * 2) - range;
        y = rand.nextInt(128);
        float dx = x + 0.5f - player.x, dy = y - player.y, dz = z + 0.5f - player.z;
        return dx * dx + dy * dy + dz * dz >= 24.0f * 24.0f && world.isLoaded(x, z);
    };
    if (dimension < 0) {
        // BiomeGenHell: ghasts and zombie pigmen (EntityLiving.getCanSpawnHere, no light check); nothing else.
        if (hostile >= 10 || (world.time & 1) != 0) return;
        int x, y, z;
        if (!randomSpot(x, y, z) || !canSpawnAt(x, y, z)) return;
        uint8_t type = rand.nextInt(2) ? ENT_PIGZOMBIE : ENT_GHAST;
        int group = 1 + rand.nextInt(3);
        for (int i = 0; i < group && hostile < 10; ++i) {
            bool ok = canSpawnAt(x, y, z) && (type != ENT_GHAST || rand.nextInt(20) == 0);
            if (ok) {
                Body probe;
                probe.width = MOBS[type].width;
                probe.height = MOBS[type].height;
                probe.setPosition(x + 0.5f, (float)y, z + 0.5f);
                AABB b = probe.box();
                ok = !probe.collides(world, b) && !(!dead && b.intersects(player.box()));
                for (int bx = ifloor(b.x0); ok && bx <= ifloor(b.x1); ++bx)
                    for (int by = ifloor(b.y0); ok && by <= ifloor(b.y1); ++by)
                        for (int bz = ifloor(b.z0); ok && bz <= ifloor(b.z1); ++bz)
                            if (blockIsLiquid(world.get(bx, by, bz))) ok = false;
                for (const Entity& o : ents)
                    if (ok && entityIsMob(o.type) && o.health > 0 && b.intersects(o.box())) ok = false;
            }
            if (ok) {
                Entity* e = spawn(type, x + 0.5f, (float)y, z + 0.5f);
                if (!e) break;
                e->yaw = e->prevYaw = rand.nextFloat() * 360.0f;
                ++hostile;
            }
            x += rand.nextInt(6) - rand.nextInt(6);
            z += rand.nextInt(6) - rand.nextInt(6);
        }
        return;
    }
    if (hostile < 10 && (world.time & 1) == 0) {
        int x, y, z;
        if (randomSpot(x, y, z) && canSpawnAt(x, y, z)) {
            static const uint8_t TYPES[5] = {ENT_SPIDER, ENT_ZOMBIE, ENT_SKELETON, ENT_CREEPER, ENT_SLIME};
            uint8_t type = TYPES[rand.nextInt(5)];
            int group = 1 + rand.nextInt(3);
            for (int i = 0; i < group && hostile < 10; ++i) {
                bool ok = canSpawnAt(x, y, z);
                // EntitySlime.getCanSpawnHere ignores light: slime chunks below y 16
                if (ok && type == ENT_SLIME)
                    ok = rand.nextInt(10) == 0 && slimeChunk(x >> 4, z >> 4) && y < 16;
                else if (ok)
                    ok = monsterLightOk(x, y, z);
                if (ok) {
                    Entity* e = spawn(type, x + 0.5f, (float)y, z + 0.5f);
                    if (!e) break;
                    e->yaw = rand.nextFloat() * 360.0f;
                    ++hostile;
                }
                x += rand.nextInt(6) - rand.nextInt(6);
                z += rand.nextInt(6) - rand.nextInt(6);
            }
        }
    }
    // SpawnerAnimals for EnumCreatureType.waterCreature
    if (squids < 5 && (world.time & 3) == 1) {
        int x, y, z;
        if (randomSpot(x, y, z)) {
            int group = 1 + rand.nextInt(4);
            for (int i = 0; i < group && squids < 5; ++i) {
                if (blockIsWater(world.get(x, y, z)) && !blockIsNormalCube(world.get(x, y + 1, z)) &&
                    world.isLoaded(x, z)) {
                    Entity* e = spawn(ENT_SQUID, x + 0.5f, (float)y, z + 0.5f);
                    if (!e) break;
                    e->yaw = e->prevYaw = rand.nextFloat() * 360.0f;
                    ++squids;
                }
                x += rand.nextInt(6) - rand.nextInt(6);
                z += rand.nextInt(6) - rand.nextInt(6);
            }
        }
    }
    if (animals < 8 && world.time % 400 == 0) {
        for (int k = 0; k < 4; ++k) {
            int x, y, z;
            if (!randomSpot(x, y, z)) continue;
            y = world.topSolidAt(x, z);
            if (y <= 0 || world.get(x, y - 1, z) != GRASS || !canSpawnAt(x, y, z)) continue;
            uint8_t l = world.getLight(x, y, z);
            if ((l >> 4) <= 8 && (l & 15) <= 8) continue;
            int biome = biomeAt(x, z);
            int w = rand.nextInt(biome == BIOME_FOREST || biome == BIOME_TAIGA ? 42 : 40);
            uint8_t type = w < 12 ? ENT_SHEEP : w < 22 ? ENT_PIG : w < 32 ? ENT_CHICKEN : w < 40 ? ENT_COW : ENT_WOLF;
            Entity* e = spawn(type, x + 0.5f, (float)y, z + 0.5f);
            if (e) e->yaw = rand.nextFloat() * 360.0f;
            break;
        }
    }
}

void Game::randomTicks() {
    int pcx = ifloor(player.x) >> 4, pcz = ifloor(player.z) >> 4;
    const int R = 4;
    for (int dx = -R; dx <= R; ++dx)
        for (int dz = -R; dz <= R; ++dz) {
            Chunk* c = world.chunk(pcx + dx, pcz + dz);
            if (!c || !(c->flags & CHUNK_LIT)) continue;
            if (dimension == 0) weatherChunk(c);
            for (int k = 0; k < 80; ++k) {
                uint32_t r = fastRand();
                int lx = r & 15, lz = (r >> 4) & 15, y = (r >> 8) & 127;
                const Section* s = c->sec[y >> 4];
                if (!s) continue;
                uint8_t id = s->blocks[sectionIndex(lx, y & 15, lz)];
                switch (id) {
                    case GRASS:
                    case SAPLING:
                    case CROPS:
                    case FARMLAND:
                    case LEAVES:
                    case CACTUS:
                    case REED:
                    case SNOW_LAYER:
                    case ICE:
                    case WATER_FLOWING:
                    case LAVA_FLOWING:
                    case LAVA:
                    case FIRE:
                    case REDSTONE_TORCH_OFF:
                    case REDSTONE_TORCH_ON:
                    case STONE_BUTTON:
                    case STONE_PLATE:
                    case WOOD_PLATE: randomTick(c->cx * 16 + lx, y, c->cz * 16 + lz, id); break;
                    default: break;
                }
            }
        }
}

void Game::randomTick(int x, int y, int z, uint8_t id) {
    int meta = world.getMeta(x, y, z);
    switch (id) {
        case GRASS: {
            uint8_t above = world.get(x, y + 1, z);
            if (world.getBrightness(x, y + 1, z) < 4 && g_blocks[above].opacity > 2) {
                if (rand.nextInt(4) == 0) world.setBlock(x, y, z, DIRT);
            } else if (world.getBrightness(x, y + 1, z) >= 9) {
                int tx = x + rand.nextInt(3) - 1, ty = y + rand.nextInt(5) - 3, tz = z + rand.nextInt(3) - 1;
                if (world.get(tx, ty, tz) == DIRT && world.getBrightness(tx, ty + 1, tz) >= 4 &&
                    g_blocks[world.get(tx, ty + 1, tz)].opacity <= 2)
                    world.setBlock(tx, ty, tz, GRASS);
            }
            break;
        }
        case SAPLING:
            if (world.getBrightness(x, y + 1, z) >= 9 && rand.nextInt(30) == 0) {
                if (!(meta & 8)) {
                    world.setMetaRaw(x, y, z, meta | 8);
                } else if (growSapling(world, rand, x, y, z, meta & 3)) {
                    world.relight(x, z);
                    for (int dy = 0; dy < 10; ++dy)
                        for (int dx = -3; dx <= 3; ++dx)
                            for (int dz = -3; dz <= 3; ++dz) world.markBlockDirty(x + dx, y + dy, z + dz);
                }
            }
            break;
        case CROPS: {
            if (world.getBrightness(x, y + 1, z) < 9 || meta >= 7) break;
            // BlockCrops.getGrowthRate
            float f = 1.0f;
            for (int dx = -1; dx <= 1; ++dx)
                for (int dz = -1; dz <= 1; ++dz) {
                    float f1 = 0;
                    if (world.get(x + dx, y - 1, z + dz) == FARMLAND)
                        f1 = world.getMeta(x + dx, y - 1, z + dz) > 0 ? 3.0f : 1.0f;
                    if (dx || dz) f1 /= 4.0f;
                    f += f1;
                }
            bool row = world.get(x - 1, y, z) == CROPS || world.get(x + 1, y, z) == CROPS;
            bool col = world.get(x, y, z - 1) == CROPS || world.get(x, y, z + 1) == CROPS;
            if (row && col) f /= 2.0f;
            if (rand.nextInt((int)(100.0f / f)) == 0) world.setMeta(x, y, z, meta + 1);
            break;
        }
        case FARMLAND: {
            if (rand.nextInt(5) != 0) break;
            bool water = false;
            for (int dx = -4; dx <= 4 && !water; ++dx)
                for (int dy = 0; dy <= 1 && !water; ++dy)
                    for (int dz = -4; dz <= 4; ++dz)
                        if (blockIsWater(world.get(x + dx, y + dy, z + dz))) {
                            water = true;
                            break;
                        }
            if (water || rainingAt(x, y + 1, z)) {
                if (meta != 7) world.setMeta(x, y, z, 7);
            } else if (meta > 0) {
                world.setMeta(x, y, z, meta - 1);
            } else if (world.get(x, y + 1, z) != CROPS) {
                world.setBlock(x, y, z, DIRT);
            }
            break;
        }
        case LEAVES: {
            if (!(meta & 8)) break;
            // BlockLeaves: look for a log within 4 blocks through leaves.
            static int8_t dist[9][9][9];
            std::memset(dist, -1, sizeof(dist));
            static int queue[729][3];
            int head = 0, tail = 0;
            dist[4][4][4] = 0;
            queue[tail][0] = 0, queue[tail][1] = 0, queue[tail][2] = 0;
            ++tail;
            bool found = false;
            while (head < tail && !found) {
                int qx = queue[head][0], qy = queue[head][1], qz = queue[head][2];
                ++head;
                int d = dist[qx + 4][qy + 4][qz + 4];
                static const int N[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
                for (const auto& n : N) {
                    int nx = qx + n[0], ny = qy + n[1], nz = qz + n[2];
                    if (iabs(nx) > 4 || iabs(ny) > 4 || iabs(nz) > 4) continue;
                    if (dist[nx + 4][ny + 4][nz + 4] >= 0) continue;
                    uint8_t b = world.get(x + nx, y + ny, z + nz);
                    if (b == LOG) {
                        found = true;
                        break;
                    }
                    if (b == LEAVES && d < 3) {
                        dist[nx + 4][ny + 4][nz + 4] = (int8_t)(d + 1);
                        queue[tail][0] = nx, queue[tail][1] = ny, queue[tail][2] = nz;
                        ++tail;
                    }
                }
            }
            if (found) {
                world.setMetaRaw(x, y, z, meta & 7);
            } else {
                dropBlockItems(x, y, z, LEAVES, meta);
                setBlockNotify(x, y, z, AIR);
            }
            break;
        }
        case CACTUS:
        case REED:
            if (world.get(x, y + 1, z) == AIR) {
                int h = 1;
                while (world.get(x, y - h, z) == id) ++h;
                if (h < 3) {
                    if (meta == 15) {
                        world.setBlock(x, y + 1, z, id);
                        world.setMetaRaw(x, y, z, 0);
                    } else {
                        world.setMetaRaw(x, y, z, meta + 1);
                    }
                }
            }
            break;
        case SNOW_LAYER:
        case ICE:
            if ((world.getLight(x, y, z) & 15) > 11 || (id == ICE && (world.getLight(x, y + 1, z) & 15) > 11)) {
                setBlockNotify(x, y, z, id == ICE ? WATER : AIR);
            }
            break;
        case WATER_FLOWING:
        case LAVA_FLOWING: liquidTick(x, y, z, id); break;
        case LAVA: lavaIgnite(x, y, z); break;
        case FIRE: fireTick(x, y, z); break;
        default: redstoneTick(x, y, z, id); break;
    }
}

void Game::tickTiles() {
    int pcx = ifloor(player.x) >> 4, pcz = ifloor(player.z) >> 4;
    for (int dx = -5; dx <= 5; ++dx)
        for (int dz = -5; dz <= 5; ++dz) {
            Chunk* c = world.chunk(pcx + dx, pcz + dz);
            if (!c) continue;
            for (TileEntity* t = c->tiles; t; t = t->next)
                if (t->type == TILE_FURNACE)
                    tickFurnace(*t);
                else if (t->type == TILE_SPAWNER)
                    tickSpawner(*t);
        }
    tickPistons();
}

// TileEntityFurnace.updateEntity
void Game::tickFurnace(TileEntity& t) {
    ItemStack& in = t.items[0];
    ItemStack& fuel = t.items[1];
    ItemStack& out = t.items[2];
    auto canSmelt = [&]() {
        if (in.empty()) return false;
        ItemStack r = smeltResult(in);
        if (r.empty()) return false;
        if (out.empty()) return true;
        if (!out.sameItem(r)) return false;
        return out.count + r.count <= itemMaxStack(out.id);
    };
    bool wasBurning = t.burnTime > 0;
    if (t.burnTime > 0) --t.burnTime;
    if (t.burnTime == 0 && canSmelt()) {
        t.burnTotal = t.burnTime = (int16_t)(fuelValue(fuel) > 32767 ? 32767 : fuelValue(fuel));
        if (t.burnTime > 0 && !fuel.empty()) {
            if (--fuel.count == 0) fuel.clear();
        }
    }
    if (t.burnTime > 0 && canSmelt()) {
        if (++t.cookTime == 200) {
            t.cookTime = 0;
            ItemStack r = smeltResult(in);
            if (out.empty())
                out = r;
            else
                out.count = (uint8_t)(out.count + r.count);
            if (--in.count == 0) in.clear();
        }
    } else {
        t.cookTime = 0;
    }
    if (wasBurning != (t.burnTime > 0)) {
        uint8_t id = world.get(t.x, t.y, t.z);
        if (id == FURNACE || id == FURNACE_LIT) {
            int meta = world.getMeta(t.x, t.y, t.z);
            world.setBlock(t.x, t.y, t.z, t.burnTime > 0 ? FURNACE_LIT : FURNACE, meta);
        }
    }
    if (Chunk* c = world.chunk(t.x >> 4, t.z >> 4)) c->flags |= CHUNK_NEEDS_SAVE;
}

// ------------------------------------------------------------------ sky

float Game::celestialAngle() const {
    if (dimension < 0) return 0.5f;  // WorldProviderHell: always midnight
    int i = (int)(world.time % 24000);
    float f1 = i / 24000.0f - 0.25f;
    if (f1 < 0) f1 += 1;
    if (f1 > 1) f1 -= 1;
    float f2 = f1;
    f1 = 1.0f - (std::cos(f1 * PI) + 1.0f) / 2.0f;
    return f2 + (f1 - f2) / 3.0f;
}

int Game::computeSkySubtracted() const {
    float f = 1.0f - (std::cos(celestialAngle() * PI * 2.0f) * 2.0f + 0.5f);
    f = 1.0f - clampf(f, 0, 1);
    f *= 1.0f - rainStrength(1.0f) * 5.0f / 16.0f;
    f *= 1.0f - thunderStrength(1.0f) * 5.0f / 16.0f;
    return (int)((1.0f - f) * 11.0f);
}

// Greys a color toward its luminance (scaled by `dark`) as rain or thunder sets in.
static void weatherGrey(float& r, float& g, float& b, float strength, float dark, float keep) {
    if (strength <= 0) return;
    float grey = (r * 0.3f + g * 0.59f + b * 0.11f) * dark;
    float k = 1.0f - strength * keep;
    r = r * k + grey * (1.0f - k);
    g = g * k + grey * (1.0f - k);
    b = b * k + grey * (1.0f - k);
}

uint32_t Game::skyColor(float partial) const {
    float f2 = clampf(std::cos(celestialAngle() * PI * 2.0f) * 2.0f + 0.5f, 0, 1);
    float temp = world.biomeTemperature(ifloor(player.x), ifloor(player.z)) / 255.0f;
    float t = clampf(temp / 3.0f, -1, 1);
    float r, g, b;
    hsbToRgb(0.62222224f - t * 0.05f, 0.5f + t * 0.1f, 1.0f, r, g, b);
    r *= f2, g *= f2, b *= f2;
    weatherGrey(r, g, b, rainStrength(partial), 0.6f, 0.75f);
    weatherGrey(r, g, b, thunderStrength(partial), 0.2f, 0.75f);
    if (lightningFlash > 0) {
        float f = clampf(lightningFlash - partial, 0, 1) * 0.45f;
        r = r * (1.0f - f) + 0.8f * f;
        g = g * (1.0f - f) + 0.8f * f;
        b = b * (1.0f - f) + 1.0f * f;
    }
    return packRGB(r, g, b);
}

uint32_t Game::fogColor(float partial) const {
    if (dimension < 0) return packRGB(0.2f * 0.84f, 0.03f * 0.84f, 0.03f * 0.84f);  // red haze, mixed with the black sky
    float f2 = clampf(std::cos(celestialAngle() * PI * 2.0f) * 2.0f + 0.5f, 0, 1);
    float r = 0.7529412f * (f2 * 0.94f + 0.06f), g = 0.84705883f * (f2 * 0.94f + 0.06f), b = f2 * 0.91f + 0.09f;
    uint32_t sky = skyColor(partial);
    float sr = ((sky >> 16) & 255) / 255.0f, sg = ((sky >> 8) & 255) / 255.0f, sb = (sky & 255) / 255.0f;
    const float mix = 0.16f;
    r += (sr - r) * mix, g += (sg - g) * mix, b += (sb - b) * mix;
    // EntityRenderer.updateFogColor
    float rs = rainStrength(partial), ts = thunderStrength(partial);
    r *= 1.0f - rs * 0.5f, g *= 1.0f - rs * 0.5f, b *= 1.0f - rs * 0.4f;
    r *= 1.0f - ts * 0.5f, g *= 1.0f - ts * 0.5f, b *= 1.0f - ts * 0.5f;
    return packRGB(r, g, b);
}

// World.drawClouds color.
uint32_t Game::cloudColor(float partial) const {
    float f = clampf(std::cos(celestialAngle() * PI * 2.0f) * 2.0f + 0.5f, 0, 1);
    float r = 1, g = 1, b = 1;
    weatherGrey(r, g, b, rainStrength(partial), 0.6f, 0.95f);
    r *= f * 0.9f + 0.1f, g *= f * 0.9f + 0.1f, b *= f * 0.85f + 0.15f;
    weatherGrey(r, g, b, thunderStrength(partial), 0.2f, 0.95f);
    return packRGB(r, g, b);
}

float Game::starBrightness() const {
    if (dimension < 0) return 0;
    float f = 1.0f - (std::cos(celestialAngle() * PI * 2.0f) * 2.0f + 0.75f);
    f = clampf(f, 0, 1);
    return f * f * 0.5f;
}

void Game::updateSky() {
    int v = computeSkySubtracted();
    if (v != world.skySubtracted) {
        world.skySubtracted = v;
        world.markAllDirty();
        ++skyChanges;
    }
}
