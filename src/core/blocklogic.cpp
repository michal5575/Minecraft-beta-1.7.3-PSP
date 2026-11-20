// Block update machinery from Beta 1.7.3: World.setBlockWithNotify / notifyBlocksOfNeighborChange /
// scheduleBlockUpdate, BlockFlowing, BlockStationary, BlockFluid.checkForHarden and BlockFire.
#include "fluid.h"
#include "game.h"

namespace {

const int SIDES[6][3] = {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};

inline bool isFlowing(uint8_t id) { return id == WATER_FLOWING || id == LAVA_FLOWING; }

// BlockFlowing.blockBlocksFlow
inline bool blocksFlow(uint8_t id) {
    if (id == WOOD_DOOR || id == IRON_DOOR || id == LADDER || id == REED) return true;
    return id != AIR && blockMatSolid(id);
}

ScheduledTick g_due[1000];

}  // namespace

void Game::setBlockNotify(int x, int y, int z, uint8_t id, int meta) {
    if ((unsigned)y >= (unsigned)World::SY) return;
    uint8_t old = world.get(x, y, z);
    int oldMeta = world.getMeta(x, y, z);
    world.setBlock(x, y, z, id, meta);
    if (old != id) {
        onBlockRemoved(x, y, z, old, oldMeta);
        onBlockAdded(x, y, z, id);
    }
    notifyNeighbors(x, y, z, id);
}

void Game::notifyNeighbors(int x, int y, int z, uint8_t changed) {
    if (editingBlocks_) return;
    for (const auto& d : SIDES) onNeighborChange(x + d[0], y + d[1], z + d[2], changed);
}

void Game::onBlockRemoved(int x, int y, int z, uint8_t old, int oldMeta) {
    if (old == LOG || old == LEAVES) markLeavesForDecay(x, y, z);
    if (old == JUKEBOX || blockIsSign(old) || old == DISPENSER || old == NOTE_BLOCK) {
        if (TileEntity* t = world.tileAt(x, y, z)) {
            if (old == JUKEBOX && !t->items[0].empty()) {
                dropItem(t->items[0], x + 0.5f, y + 1.0f, z + 0.5f, false);
                jukeboxStop(x, y, z);
            }
            if (old == DISPENSER)
                for (const ItemStack& s : t->items) dropItem(s, x + 0.5f, y + 0.5f, z + 0.5f, false);
            if (openTile == t) openTile = nullptr;
            world.removeTile(x, y, z);
        }
    }
    redstoneRemoved(x, y, z, old, oldMeta);
    uint8_t now = world.get(x, y, z);
    if (old != AIR && !blockIsSolid(now)) popUnsupported(x, y, z);
}

void Game::onBlockAdded(int x, int y, int z, uint8_t id) {
    switch (id) {
        case WATER_FLOWING:
        case LAVA_FLOWING:
            liquidHarden(x, y, z);
            if (world.get(x, y, z) == id) world.scheduleTick(x, y, z, id, tickRate(id));
            break;
        case WATER:
        case LAVA: liquidHarden(x, y, z); break;
        case FIRE:
            if (world.get(x, y - 1, z) == OBSIDIAN && tryCreatePortal(x, y, z)) break;
            if (!fireCanStay(x, y, z))
                setBlockNotify(x, y, z, AIR);
            else
                world.scheduleTick(x, y, z, FIRE, tickRate(FIRE));
            break;
        case SAND:
        case GRAVEL: world.scheduleTick(x, y, z, id, 3); break;
        default: redstoneAdded(x, y, z, id); break;
    }
}

void Game::onNeighborChange(int x, int y, int z, uint8_t changed) {
    if ((unsigned)y >= (unsigned)World::SY || !world.isLoaded(x, z)) return;
    uint8_t id = world.get(x, y, z);
    switch (id) {
        case WATER:
        case LAVA:
            // BlockStationary.setNotStationary
            liquidHarden(x, y, z);
            if (world.get(x, y, z) == id) {
                world.setRaw(x, y, z, (uint8_t)(id - 1), world.getMeta(x, y, z));
                world.scheduleTick(x, y, z, (uint8_t)(id - 1), tickRate((uint8_t)(id - 1)));
            }
            break;
        case WATER_FLOWING:
        case LAVA_FLOWING: liquidHarden(x, y, z); break;
        case FIRE:
            if (!fireCanStay(x, y, z)) setBlockNotify(x, y, z, AIR);
            break;
        case SAND:
        case GRAVEL: world.scheduleTick(x, y, z, id, 3); break;
        case PORTAL: portalNeighborChange(x, y, z); break;
        case SIGN_POST:
        case WALL_SIGN:
        case TRAPDOOR:
        case CAKE:
            if (!blockCanStay(x, y, z, id, world.getMeta(x, y, z))) {
                if (mode == MODE_SURVIVAL) dropBlockItems(x, y, z, id, world.getMeta(x, y, z));
                setBlockNotify(x, y, z, AIR);
            }
            break;
        default: redstoneNeighborChange(x, y, z, id, changed); break;
    }
}

// BlockSign / BlockTrapDoor / BlockCake.onNeighborBlockChange support checks.
bool Game::blockCanStay(int x, int y, int z, uint8_t id, int meta) const {
    switch (id) {
        case SIGN_POST:
        case CAKE: return blockMatSolid(world.get(x, y - 1, z));
        case WALL_SIGN: {
            static const int BEHIND[6][2] = {{0, 0}, {0, 0}, {0, 1}, {0, -1}, {1, 0}, {-1, 0}};
            int m = meta >= 2 && meta <= 5 ? meta : 2;
            return blockMatSolid(world.get(x + BEHIND[m][0], y, z + BEHIND[m][1]));
        }
        case TRAPDOOR: {
            static const int BEHIND[4][2] = {{0, 1}, {0, -1}, {1, 0}, {-1, 0}};
            return blockIsNormalCube(world.get(x + BEHIND[meta & 3][0], y, z + BEHIND[meta & 3][1]));
        }
        default: return true;
    }
}

int Game::tickRate(uint8_t id) const {
    switch (id) {
        case WATER_FLOWING:
        case WATER: return 5;
        case LAVA_FLOWING:
        case LAVA: return 30;
        case FIRE: return 40;
        case REDSTONE_TORCH_OFF:
        case REDSTONE_TORCH_ON: return 2;
        case DISPENSER: return 4;
        case STONE_BUTTON:
        case STONE_PLATE:
        case WOOD_PLATE:
        case DETECTOR_RAIL: return 20;
        default: return 10;
    }
}

void Game::tickScheduled() {
    int n = world.takeDueTicks(g_due, 1000);
    for (int i = 0; i < n; ++i) {
        const ScheduledTick& t = g_due[i];
        if (!world.areaLoaded(t.x, t.z, 8)) continue;
        const Chunk* c = world.chunk(t.x >> 4, t.z >> 4);
        if (!c) continue;
        if (!(c->flags & CHUNK_LIT)) {
            world.scheduleTick(t.x, t.y, t.z, t.id, 10);
            continue;
        }
        if (world.get(t.x, t.y, t.z) == t.id) scheduledTick(t.x, t.y, t.z, t.id);
    }
}

void Game::scheduledTick(int x, int y, int z, uint8_t id) {
    switch (id) {
        case WATER_FLOWING:
        case LAVA_FLOWING: liquidTick(x, y, z, id); break;
        case FIRE: fireTick(x, y, z); break;
        case SAND:
        case GRAVEL: {
            uint8_t b = world.get(x, y - 1, z);
            if (y > 0 && (b == AIR || b == FIRE || blockIsLiquid(b))) {
                settleFalling(x, y - 1, z);
                world.relight(x, z);
                notifyNeighbors(x, y, z, AIR);
            }
            break;
        }
        default: redstoneTick(x, y, z, id); break;
    }
}

// ------------------------------------------------------------------ liquids

bool Game::liquidCanDisplace(int x, int y, int z, bool water) const {
    uint8_t id = world.get(x, y, z);
    if (fluidSame(id, water) || blockIsLava(id)) return false;
    return !blocksFlow(id);
}

void Game::liquidHarden(int x, int y, int z) {
    uint8_t id = world.get(x, y, z);
    if (!blockIsLava(id)) return;
    bool water = blockIsWater(world.get(x, y, z - 1)) || blockIsWater(world.get(x, y, z + 1)) ||
                 blockIsWater(world.get(x - 1, y, z)) || blockIsWater(world.get(x + 1, y, z)) ||
                 blockIsWater(world.get(x, y + 1, z));
    if (!water) return;
    int m = world.getMeta(x, y, z);
    if (m > 4) return;
    setBlockNotify(x, y, z, m == 0 ? OBSIDIAN : COBBLESTONE);
    playSound("random.fizz", x + 0.5f, y + 0.5f, z + 0.5f, 0.5f,
              2.6f + (soundRand.nextFloat() - soundRand.nextFloat()) * 0.8f);
    for (int i = 0; i < 8; ++i)
        particles.spawn(world, PART_SMOKE, x + particles.rnd(), y + 1.2f, z + particles.rnd(), 0, 0, 0, 2.5f);
}

void Game::liquidFlowInto(int x, int y, int z, uint8_t id, int meta) {
    bool water = id == WATER_FLOWING;
    if (!liquidCanDisplace(x, y, z, water)) return;
    uint8_t old = world.get(x, y, z);
    if (old != AIR && water && mode == MODE_SURVIVAL) dropBlockItems(x, y, z, old, world.getMeta(x, y, z));
    setBlockNotify(x, y, z, id, meta);
}

int Game::liquidFlowCost(int x, int y, int z, int depth, int from, bool water) const {
    int best = 1000;
    static const int OPP[4] = {1, 0, 3, 2};
    static const int D[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    for (int i = 0; i < 4; ++i) {
        if (i == OPP[from]) continue;
        int nx = x + D[i][0], nz = z + D[i][1];
        uint8_t nid = world.get(nx, y, nz);
        if (blocksFlow(nid) || (fluidSame(nid, water) && world.getMeta(nx, y, nz) == 0)) continue;
        if (!blocksFlow(world.get(nx, y - 1, nz))) return depth;
        if (depth < 4) {
            int c = liquidFlowCost(nx, y, nz, depth + 1, i, water);
            if (c < best) best = c;
        }
    }
    return best;
}

void Game::liquidOptimalFlow(int x, int y, int z, bool water, bool out[4]) const {
    static const int D[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    int cost[4];
    for (int i = 0; i < 4; ++i) {
        cost[i] = 1000;
        int nx = x + D[i][0], nz = z + D[i][1];
        uint8_t nid = world.get(nx, y, nz);
        if (blocksFlow(nid) || (fluidSame(nid, water) && world.getMeta(nx, y, nz) == 0)) continue;
        cost[i] = blocksFlow(world.get(nx, y - 1, nz)) ? liquidFlowCost(nx, y, nz, 1, i, water) : 0;
    }
    int m = cost[0];
    for (int i = 1; i < 4; ++i)
        if (cost[i] < m) m = cost[i];
    for (int i = 0; i < 4; ++i) out[i] = cost[i] == m;
}

// BlockFlowing.updateTick
void Game::liquidTick(int x, int y, int z, uint8_t id) {
    bool water = id == WATER_FLOWING;
    auto decayAt = [&](int ax, int ay, int az) {
        return fluidSame(world.get(ax, ay, az), water) ? world.getMeta(ax, ay, az) : -1;
    };
    int decay = decayAt(x, y, z);
    int step = water || dimension < 0 ? 1 : 2;
    bool settle = true;
    if (decay > 0) {
        int sources = 0;
        auto smallest = [&](int ax, int az, int cur) {
            int d = decayAt(ax, y, az);
            if (d < 0) return cur;
            if (d == 0) ++sources;
            if (d >= 8) d = 0;
            return cur >= 0 && d >= cur ? cur : d;
        };
        int m = -100;
        m = smallest(x - 1, z, m);
        m = smallest(x + 1, z, m);
        m = smallest(x, z - 1, m);
        m = smallest(x, z + 1, m);
        int nd = m + step;
        if (nd >= 8 || m < 0) nd = -1;
        int above = decayAt(x, y + 1, z);
        if (above >= 0) nd = above >= 8 ? above : above + 8;
        if (sources >= 2 && water) {
            uint8_t below = world.get(x, y - 1, z);
            if (blockMatSolid(below))
                nd = 0;
            else if (fluidSame(below, true) && world.getMeta(x, y - 1, z) == 0)
                nd = 0;
        }
        if (!water && decay < 8 && nd < 8 && nd > decay && rand.nextInt(4) != 0) {
            nd = decay;
            settle = false;
        }
        if (nd != decay) {
            decay = nd;
            if (nd < 0) {
                setBlockNotify(x, y, z, AIR);
            } else {
                world.setMeta(x, y, z, nd);
                world.scheduleTick(x, y, z, id, tickRate(id));
                notifyNeighbors(x, y, z, id);
            }
        } else if (settle) {
            world.setRaw(x, y, z, (uint8_t)(id + 1), world.getMeta(x, y, z));
        }
    } else {
        world.setRaw(x, y, z, (uint8_t)(id + 1), world.getMeta(x, y, z));
    }

    if (liquidCanDisplace(x, y - 1, z, water)) {
        liquidFlowInto(x, y - 1, z, id, decay >= 8 ? decay : decay + 8);
    } else if (decay >= 0 && (decay == 0 || blocksFlow(world.get(x, y - 1, z)))) {
        bool dirs[4];
        liquidOptimalFlow(x, y, z, water, dirs);
        int nd = decay + step;
        if (decay >= 8) nd = 1;
        if (nd >= 8) return;
        if (dirs[0]) liquidFlowInto(x - 1, y, z, id, nd);
        if (dirs[1]) liquidFlowInto(x + 1, y, z, id, nd);
        if (dirs[2]) liquidFlowInto(x, y, z - 1, id, nd);
        if (dirs[3]) liquidFlowInto(x, y, z + 1, id, nd);
    }
}

// BlockStationary.updateTick for lava (random ticks): sets fire to air next to burnable blocks.
void Game::lavaIgnite(int x, int y, int z) {
    int n = rand.nextInt(3);
    for (int i = 0; i < n; ++i) {
        x += rand.nextInt(3) - 1;
        ++y;
        z += rand.nextInt(3) - 1;
        if ((unsigned)y >= (unsigned)World::SY || !world.isLoaded(x, z)) return;
        uint8_t id = world.get(x, y, z);
        if (id == AIR) {
            for (const auto& d : SIDES)
                if (blockMatBurns(world.get(x + d[0], y + d[1], z + d[2]))) {
                    setBlockNotify(x, y, z, FIRE);
                    return;
                }
        } else if (blockMatSolid(id)) {
            return;
        }
    }
}

// ------------------------------------------------------------------ fire

bool Game::fireNeighborBurns(int x, int y, int z) const {
    for (const auto& d : SIDES)
        if (blockFireSpread(world.get(x + d[0], y + d[1], z + d[2])) > 0) return true;
    return false;
}

bool Game::fireCanStay(int x, int y, int z) const {
    return blockIsNormalCube(world.get(x, y - 1, z)) || fireNeighborBurns(x, y, z);
}

int Game::fireEncouragement(int x, int y, int z) const {
    if (world.get(x, y, z) != AIR) return 0;
    int best = 0;
    for (const auto& d : SIDES) {
        int v = blockFireSpread(world.get(x + d[0], y + d[1], z + d[2]));
        if (v > best) best = v;
    }
    return best;
}

void Game::fireTryCatch(int x, int y, int z, int chance, int age) {
    if ((unsigned)y >= (unsigned)World::SY || !world.isLoaded(x, z)) return;
    uint8_t id = world.get(x, y, z);
    if (rand.nextInt(chance) >= blockFlammability(id)) return;
    bool tnt = id == TNT;
    if (rand.nextInt(age + 10) < 5 && !rainingAt(x, y, z)) {
        int a = age + rand.nextInt(5) / 4;
        setBlockNotify(x, y, z, FIRE, a > 15 ? 15 : a);
    } else {
        setBlockNotify(x, y, z, AIR);
    }
    if (tnt && spawn(ENT_TNT, x + 0.5f, (float)y, z + 0.5f)) playSound("random.fuse", x + 0.5f, (float)y, z + 0.5f, 1.0f, 1.0f);
}

// BlockFire.updateTick
void Game::fireTick(int x, int y, int z) {
    bool infinite = world.get(x, y - 1, z) == NETHERRACK;
    if (!fireCanStay(x, y, z)) {
        setBlockNotify(x, y, z, AIR);
        return;
    }
    if (!infinite && isRaining() &&
        (rainingAt(x, y, z) || rainingAt(x - 1, y, z) || rainingAt(x + 1, y, z) || rainingAt(x, y, z - 1) ||
         rainingAt(x, y, z + 1))) {
        setBlockNotify(x, y, z, AIR);
        return;
    }
    int age = world.getMeta(x, y, z);
    if (age < 15) world.setMetaRaw(x, y, z, age + rand.nextInt(3) / 2);
    world.scheduleTick(x, y, z, FIRE, tickRate(FIRE));
    if (!infinite && !fireNeighborBurns(x, y, z)) {
        if (!blockIsNormalCube(world.get(x, y - 1, z)) || age > 3) setBlockNotify(x, y, z, AIR);
        return;
    }
    if (!infinite && blockFireSpread(world.get(x, y - 1, z)) <= 0 && age == 15 && rand.nextInt(4) == 0) {
        setBlockNotify(x, y, z, AIR);
        return;
    }
    fireTryCatch(x + 1, y, z, 300, age);
    fireTryCatch(x - 1, y, z, 300, age);
    fireTryCatch(x, y - 1, z, 250, age);
    fireTryCatch(x, y + 1, z, 250, age);
    fireTryCatch(x, y, z - 1, 300, age);
    fireTryCatch(x, y, z + 1, 300, age);
    for (int xx = x - 1; xx <= x + 1; ++xx)
        for (int zz = z - 1; zz <= z + 1; ++zz)
            for (int yy = y - 1; yy <= y + 4; ++yy) {
                if (xx == x && yy == y && zz == z) continue;
                if ((unsigned)yy >= (unsigned)World::SY || !world.isLoaded(xx, zz)) continue;
                int chance = 100;
                if (yy > y + 1) chance += (yy - (y + 1)) * 100;
                int enc = fireEncouragement(xx, yy, zz);
                if (enc <= 0) continue;
                int k = (enc + 40) / (age + 30);
                if (k > 0 && rand.nextInt(chance) <= k &&
                    !(isRaining() && rainingAt(xx, yy, zz)) && !rainingAt(xx - 1, yy, zz) &&
                    !rainingAt(xx + 1, yy, zz) && !rainingAt(xx, yy, zz - 1) && !rainingAt(xx, yy, zz + 1)) {
                    int a = age + rand.nextInt(5) / 4;
                    setBlockNotify(xx, yy, zz, FIRE, a > 15 ? 15 : a);
                }
            }
}
