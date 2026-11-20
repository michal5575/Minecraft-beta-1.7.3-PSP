// Redstone from Beta 1.7.3: World.isBlockGettingPowered / isBlockIndirectlyProvidingPowerTo /
// isBlockIndirectlyGettingPowered, BlockRedstoneWire, BlockRedstoneTorch, BlockRedstoneRepeater, BlockLever,
// BlockButton, BlockPressurePlate, BlockDoor.onPoweredBlockChange, BlockNote, BlockDispenser, BlockPistonBase,
// BlockPistonExtension, BlockPistonMoving and TileEntityPiston.
#include <cmath>

#include "game.h"

namespace {

const int DX[6] = {0, 0, 0, 0, -1, 1};
const int DY[6] = {-1, 1, 0, 0, 0, 0};
const int DZ[6] = {0, 0, -1, 1, 0, 0};

float gaussian(JRandom& r) {
    float s = 0;
    for (int i = 0; i < 4; ++i) s += r.nextFloat();
    return (s - 2.0f) * 1.732f;
}

// BlockRedstoneTorch.torchUpdates: recent turn-offs, for burnout.
struct TorchUpdate {
    int x, y, z;
    int64_t time;
};
const int TORCH_UPDATES_MAX = 64;
TorchUpdate g_torchUpdates[TORCH_UPDATES_MAX];
int g_torchUpdateCount = 0;

bool torchBurnedOut(int x, int y, int z, int64_t now, bool add) {
    if (add) {
        if (g_torchUpdateCount == TORCH_UPDATES_MAX) {
            for (int i = 1; i < g_torchUpdateCount; ++i) g_torchUpdates[i - 1] = g_torchUpdates[i];
            --g_torchUpdateCount;
        }
        g_torchUpdates[g_torchUpdateCount++] = {x, y, z, now};
    }
    int n = 0;
    for (int i = 0; i < g_torchUpdateCount; ++i) {
        const TorchUpdate& u = g_torchUpdates[i];
        if (u.x == x && u.y == y && u.z == z && ++n >= 8) return true;
    }
    return false;
}

// BlockRedstoneWire.blocksNeedingUpdate, used as a stack: a nested update works above its caller's entries.
const int WIRE_SET_MAX = 4096;
int g_wireSet[WIRE_SET_MAX][3];
int g_wireSetCount = 0;
int g_wireSetBase = 0;

void wireSetAdd(int x, int y, int z) {
    for (int i = g_wireSetBase; i < g_wireSetCount; ++i)
        if (g_wireSet[i][0] == x && g_wireSet[i][1] == y && g_wireSet[i][2] == z) return;
    if (g_wireSetCount == WIRE_SET_MAX) return;
    g_wireSet[g_wireSetCount][0] = x, g_wireSet[g_wireSetCount][1] = y, g_wireSet[g_wireSetCount][2] = z;
    ++g_wireSetCount;
}

// BlockContainer subclasses: pistons never move them.
bool hasTileEntity(uint8_t id) {
    switch (id) {
        case CHEST: case FURNACE: case FURNACE_LIT: case DISPENSER: case MOB_SPAWNER: case NOTE_BLOCK:
        case JUKEBOX: case SIGN_POST: case WALL_SIGN: case PISTON_MOVING:
            return true;
        default: return false;
    }
}

}  // namespace

// ------------------------------------------------------------------ power queries

bool Game::canProvidePower(uint8_t id) const {
    switch (id) {
        case REDSTONE_WIRE: return wirePower_;
        case REDSTONE_TORCH_OFF: case REDSTONE_TORCH_ON: case LEVER: case STONE_BUTTON: case STONE_PLATE:
        case WOOD_PLATE: case DETECTOR_RAIL:
            return true;
        default: return false;
    }
}

bool Game::powerWeak(int x, int y, int z, int side) const {
    uint8_t id = world.get(x, y, z);
    switch (id) {
        case REDSTONE_WIRE: return wireWeak(x, y, z, side);
        case REDSTONE_TORCH_ON: {
            int m = world.getMeta(x, y, z);
            return !((m == 5 && side == 1) || (m == 3 && side == 3) || (m == 4 && side == 2) || (m == 1 && side == 5) ||
                     (m == 2 && side == 4));
        }
        case LEVER:
        case STONE_BUTTON: return (world.getMeta(x, y, z) & 8) != 0;
        case STONE_PLATE:
        case WOOD_PLATE: return world.getMeta(x, y, z) > 0;
        case DETECTOR_RAIL: return (world.getMeta(x, y, z) & 8) != 0;
        case REPEATER_ON: {
            static const int OUT[4] = {3, 4, 2, 5};
            return side == OUT[world.getMeta(x, y, z) & 3];
        }
        default: return false;
    }
}

bool Game::powerStrong(int x, int y, int z, int side) const {
    uint8_t id = world.get(x, y, z);
    switch (id) {
        case REDSTONE_WIRE: return wirePower_ && wireWeak(x, y, z, side);
        case REDSTONE_TORCH_ON: return side == 0;
        case LEVER:
        case STONE_BUTTON: {
            int m = world.getMeta(x, y, z);
            if (!(m & 8)) return false;
            int d = m & 7;
            return ((d == 6 || d == 5) && side == 1) || (d == 4 && side == 2) || (d == 3 && side == 3) ||
                   (d == 2 && side == 4) || (d == 1 && side == 5);
        }
        case STONE_PLATE:
        case WOOD_PLATE: return world.getMeta(x, y, z) > 0 && side == 1;
        case DETECTOR_RAIL: return (world.getMeta(x, y, z) & 8) && side == 1;
        case REPEATER_ON: return powerWeak(x, y, z, side);
        default: return false;
    }
}

bool Game::blockPowered(int x, int y, int z) const {
    return powerStrong(x, y - 1, z, 0) || powerStrong(x, y + 1, z, 1) || powerStrong(x, y, z - 1, 2) ||
           powerStrong(x, y, z + 1, 3) || powerStrong(x - 1, y, z, 4) || powerStrong(x + 1, y, z, 5);
}

bool Game::powerTo(int x, int y, int z, int side) const {
    if (blockIsNormalCube(world.get(x, y, z))) return blockPowered(x, y, z);
    return powerWeak(x, y, z, side);
}

bool Game::indirectlyPowered(int x, int y, int z) const {
    return powerTo(x, y - 1, z, 0) || powerTo(x, y + 1, z, 1) || powerTo(x, y, z - 1, 2) || powerTo(x, y, z + 1, 3) ||
           powerTo(x - 1, y, z, 4) || powerTo(x + 1, y, z, 5);
}

void Game::notifyAround(int x, int y, int z, uint8_t id) {
    for (int s = 0; s < 6; ++s) notifyNeighbors(x + DX[s], y + DY[s], z + DZ[s], id);
}

// ------------------------------------------------------------------ wire

// BlockRedstoneWire.isPowerProviderOrWire
bool Game::wireConnects(int x, int y, int z, int dir) const {
    uint8_t id = world.get(x, y, z);
    if (id == REDSTONE_WIRE) return true;
    if (id == AIR) return false;
    if (canProvidePower(id)) return true;
    if (!blockIsRepeater(id)) return false;
    static const int REMAP[4] = {2, 3, 0, 1};
    return dir == REMAP[world.getMeta(x, y, z) & 3];
}

bool Game::wireWeak(int x, int y, int z, int side) const {
    if (!wirePower_ || world.getMeta(x, y, z) == 0) return false;
    if (side == 1) return true;
    auto cube = [&](int ax, int ay, int az) { return blockIsNormalCube(world.get(ax, ay, az)); };
    bool w = wireConnects(x - 1, y, z, 1) || (!cube(x - 1, y, z) && wireConnects(x - 1, y - 1, z, -1));
    bool e = wireConnects(x + 1, y, z, 3) || (!cube(x + 1, y, z) && wireConnects(x + 1, y - 1, z, -1));
    bool n = wireConnects(x, y, z - 1, 2) || (!cube(x, y, z - 1) && wireConnects(x, y - 1, z - 1, -1));
    bool s = wireConnects(x, y, z + 1, 0) || (!cube(x, y, z + 1) && wireConnects(x, y - 1, z + 1, -1));
    if (!cube(x, y + 1, z)) {
        if (cube(x - 1, y, z) && wireConnects(x - 1, y + 1, z, -1)) w = true;
        if (cube(x + 1, y, z) && wireConnects(x + 1, y + 1, z, -1)) e = true;
        if (cube(x, y, z - 1) && wireConnects(x, y + 1, z - 1, -1)) n = true;
        if (cube(x, y, z + 1) && wireConnects(x, y + 1, z + 1, -1)) s = true;
    }
    if (!n && !e && !w && !s && side >= 2 && side <= 5) return true;
    if (side == 2 && n && !w && !e) return true;
    if (side == 3 && s && !w && !e) return true;
    if (side == 4 && w && !n && !s) return true;
    return side == 5 && e && !n && !s;
}

// BlockRedstoneWire.updateAndPropagateCurrentStrength
void Game::wireUpdate(int x, int y, int z) {
    int savedBase = g_wireSetBase;
    int base = g_wireSetBase = g_wireSetCount;
    wireCalc(x, y, z, x, y, z);
    int end = g_wireSetCount;
    g_wireSetBase = end;
    for (int i = base; i < end; ++i)
        notifyNeighbors(g_wireSet[i][0], g_wireSet[i][1], g_wireSet[i][2], REDSTONE_WIRE);
    g_wireSetCount = base;
    g_wireSetBase = savedBase;
}

void Game::wireCalc(int x, int y, int z, int sx, int sy, int sz) {
    static const int N4[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    auto cube = [&](int ax, int ay, int az) { return blockIsNormalCube(world.get(ax, ay, az)); };
    auto strength = [&](int ax, int ay, int az, int cur) {
        if (world.get(ax, ay, az) != REDSTONE_WIRE) return cur;
        int m = world.getMeta(ax, ay, az);
        return m > cur ? m : cur;
    };
    int old = world.getMeta(x, y, z);
    wirePower_ = false;
    bool powered = indirectlyPowered(x, y, z);
    wirePower_ = true;
    int power = 0;
    if (powered) {
        power = 15;
    } else {
        for (const auto& d : N4) {
            int nx = x + d[0], nz = z + d[1];
            if (nx != sx || y != sy || nz != sz) power = strength(nx, y, nz, power);
            if (cube(nx, y, nz) && !cube(x, y + 1, z)) {
                if (nx != sx || y + 1 != sy || nz != sz) power = strength(nx, y + 1, nz, power);
            } else if (!cube(nx, y, nz) && (nx != sx || y - 1 != sy || nz != sz)) {
                power = strength(nx, y - 1, nz, power);
            }
        }
        power = power > 0 ? power - 1 : 0;
    }
    if (old == power) return;
    editingBlocks_ = true;
    world.setMeta(x, y, z, power);
    editingBlocks_ = false;
    int cur = power;
    for (const auto& d : N4) {
        int nx = x + d[0], nz = z + d[1];
        int ny = cube(nx, y, nz) ? y + 1 : y - 1;
        int q = strength(nx, y, nz, -1);
        cur = world.getMeta(x, y, z);
        if (cur > 0) --cur;
        if (q >= 0 && q != cur) wireCalc(nx, y, nz, x, y, z);
        q = strength(nx, ny, nz, -1);
        cur = world.getMeta(x, y, z);
        if (cur > 0) --cur;
        if (q >= 0 && q != cur) wireCalc(nx, ny, nz, x, y, z);
    }
    if (old == 0 || cur == 0) {
        wireSetAdd(x, y, z);
        for (int s = 0; s < 6; ++s) wireSetAdd(x + DX[s], y + DY[s], z + DZ[s]);
    }
}

// BlockRedstoneWire.notifyWireNeighborsOfNeighborChange
void Game::wireNotifyIfWire(int x, int y, int z) {
    if (world.get(x, y, z) != REDSTONE_WIRE) return;
    notifyNeighbors(x, y, z, REDSTONE_WIRE);
    for (int s = 0; s < 6; ++s) notifyNeighbors(x + DX[s], y + DY[s], z + DZ[s], REDSTONE_WIRE);
}

// ------------------------------------------------------------------ torches, repeaters, levers, buttons, plates

bool Game::redstoneTorchPowered(int x, int y, int z, int meta) const {
    return (meta == 5 && powerTo(x, y - 1, z, 0)) || (meta == 3 && powerTo(x, y, z - 1, 2)) ||
           (meta == 4 && powerTo(x, y, z + 1, 3)) || (meta == 1 && powerTo(x - 1, y, z, 4)) ||
           (meta == 2 && powerTo(x + 1, y, z, 5));
}

void Game::redstoneTorchTick(int x, int y, int z, uint8_t id) {
    int meta = world.getMeta(x, y, z);
    bool powered = redstoneTorchPowered(x, y, z, meta);
    int drop = 0;
    while (drop < g_torchUpdateCount && world.time - g_torchUpdates[drop].time > 100) ++drop;
    if (drop) {
        for (int i = drop; i < g_torchUpdateCount; ++i) g_torchUpdates[i - drop] = g_torchUpdates[i];
        g_torchUpdateCount -= drop;
    }
    if (id == REDSTONE_TORCH_ON) {
        if (powered) {
            setBlockNotify(x, y, z, REDSTONE_TORCH_OFF, meta);
            if (torchBurnedOut(x, y, z, world.time, true))
                playSound("random.fizz", x + 0.5f, y + 0.5f, z + 0.5f, 0.5f,
                          2.6f + (soundRand.nextFloat() - soundRand.nextFloat()) * 0.8f);
        }
    } else if (!powered && !torchBurnedOut(x, y, z, world.time, false)) {
        setBlockNotify(x, y, z, REDSTONE_TORCH_ON, meta);
    }
}

bool Game::repeaterPowered(int x, int y, int z, int meta) const {
    auto wireAt = [&](int ax, int az) { return world.get(ax, y, az) == REDSTONE_WIRE && world.getMeta(ax, y, az) > 0; };
    switch (meta & 3) {
        case 0: return powerTo(x, y, z + 1, 3) || wireAt(x, z + 1);
        case 1: return powerTo(x - 1, y, z, 4) || wireAt(x - 1, z);
        case 2: return powerTo(x, y, z - 1, 2) || wireAt(x, z - 1);
        default: return powerTo(x + 1, y, z, 5) || wireAt(x + 1, z);
    }
}

static int repeaterDelay(int meta) { return (((meta >> 2) & 3) + 1) * 2; }

void Game::notifyAttached(int x, int y, int z, uint8_t id, int meta) {
    notifyNeighbors(x, y, z, id);
    switch (meta & 7) {
        case 1: notifyNeighbors(x - 1, y, z, id); break;
        case 2: notifyNeighbors(x + 1, y, z, id); break;
        case 3: notifyNeighbors(x, y, z - 1, id); break;
        case 4: notifyNeighbors(x, y, z + 1, id); break;
        default: notifyNeighbors(x, y - 1, z, id); break;
    }
}

void Game::toggleLever(int x, int y, int z) {
    int meta = world.getMeta(x, y, z);
    int on = 8 - (meta & 8);
    world.setMeta(x, y, z, (meta & 7) + on);
    playSound("random.click", x + 0.5f, y + 0.5f, z + 0.5f, 0.3f, on ? 0.6f : 0.5f);
    notifyAttached(x, y, z, LEVER, meta);
}

void Game::pressButton(int x, int y, int z) {
    int meta = world.getMeta(x, y, z);
    if (meta & 8) return;
    world.setMeta(x, y, z, meta | 8);
    playSound("random.click", x + 0.5f, y + 0.5f, z + 0.5f, 0.3f, 0.6f);
    notifyAttached(x, y, z, STONE_BUTTON, meta);
    world.scheduleTick(x, y, z, STONE_BUTTON, 20);
}

void Game::plateUpdate(int x, int y, int z) {
    uint8_t id = world.get(x, y, z);
    bool was = world.getMeta(x, y, z) == 1;
    AABB area = {x + 0.125f, (float)y, z + 0.125f, x + 0.875f, y + 0.25f, z + 0.875f};
    bool found = !dead && area.intersects(player.box());
    for (int i = 0; i < MAX_ENTITIES && !found; ++i) {
        const Entity& e = ents[i];
        if (e.type == ENT_NONE || e.type == ENT_PAINTING) continue;
        if (id == STONE_PLATE && !entityIsMob(e.type)) continue;
        if (area.intersects(e.box())) found = true;
    }
    if (found != was) {
        world.setMeta(x, y, z, found ? 1 : 0);
        notifyNeighbors(x, y, z, id);
        notifyNeighbors(x, y - 1, z, id);
        playSound("random.click", x + 0.5f, y + 0.1f, z + 0.5f, 0.3f, found ? 0.6f : 0.5f);
    }
    if (found) world.scheduleTick(x, y, z, id, 20);
}

void Game::touchPlates(const AABB& box) {
    AABB b = box.grow(-0.001f);
    int x0 = (int)std::floor(b.x0), x1 = (int)std::floor(b.x1);
    int y0 = (int)std::floor(b.y0), y1 = (int)std::floor(b.y1);
    int z0 = (int)std::floor(b.z0), z1 = (int)std::floor(b.z1);
    for (int x = x0; x <= x1; ++x)
        for (int y = y0; y <= y1; ++y)
            for (int z = z0; z <= z1; ++z) {
                uint8_t id = world.get(x, y, z);
                if (blockIsPlate(id) && world.getMeta(x, y, z) != 1) plateUpdate(x, y, z);
                if (id == DETECTOR_RAIL && !(world.getMeta(x, y, z) & 8)) detectorUpdate(x, y, z, world.getMeta(x, y, z));
            }
}

// ------------------------------------------------------------------ doors, note blocks, dispensers

void Game::doorPowered(int x, int y, int z, bool powered) {
    uint8_t id = world.get(x, y, z);
    int meta = world.getMeta(x, y, z);
    if (meta & 8) {
        if (world.get(x, y - 1, z) == id) doorPowered(x, y - 1, z, powered);
        return;
    }
    if (((meta & 4) != 0) == powered) return;
    if (world.get(x, y + 1, z) == id) world.setMeta(x, y + 1, z, (meta ^ 4) + 8);
    world.setMeta(x, y, z, meta ^ 4);
    playSound(soundRand.nextFloat() < 0.5f ? "random.door_open" : "random.door_close", x + 0.5f, y + 0.5f, z + 0.5f,
              1.0f, soundRand.nextFloat() * 0.1f + 0.9f);
}

// TileEntityNote.triggerNote
void Game::notePlay(int x, int y, int z) {
    if (world.get(x, y + 1, z) != AIR) return;
    TileEntity* t = world.tileAt(x, y, z);
    if (!t) t = world.createTile(x, y, z, TILE_NOTE);
    if (!t) return;
    const char* name = "note.harp";
    switch (g_blocks[world.get(x, y - 1, z)].material) {
        case MAT_ROCK: name = "note.bd"; break;
        case MAT_SAND: name = "note.snare"; break;
        case MAT_GLASS: name = "note.hat"; break;
        case MAT_WOOD: name = "note.bassattack"; break;
        default: break;
    }
    playSound(name, x + 0.5f, y + 0.5f, z + 0.5f, 3.0f, std::pow(2.0f, (t->burnTime - 12) / 12.0f));
    particles.spawn(world, PART_NOTE, x + 0.5f, y + 1.2f, z + 0.5f, t->burnTime / 24.0f, 0, 0);
}

// BlockDispenser.dispenseItem
void Game::dispense(int x, int y, int z) {
    int meta = world.getMeta(x, y, z);
    int dx = 0, dz = 0;
    if (meta == 3) dz = 1;
    else if (meta == 2) dz = -1;
    else if (meta == 5) dx = 1;
    else dx = -1;
    TileEntity* t = world.tileAt(x, y, z);
    int slot = -1;
    if (t && t->type == TILE_DISPENSER) {
        for (int i = 0, n = 1; i < 9; ++i)
            if (!t->items[i].empty() && rand.nextInt(n++) == 0) slot = i;
    }
    float px = x + dx * 0.6f + 0.5f, py = y + 0.5f, pz = z + dz * 0.6f + 0.5f;
    if (slot < 0) {
        playSound("random.click", (float)x, (float)y, (float)z, 1.0f, 1.2f);
        return;
    }
    ItemStack s = t->items[slot];
    s.count = 1;
    if (--t->items[slot].count == 0) t->items[slot].clear();
    if (Chunk* c = world.chunk(x >> 4, z >> 4)) c->flags |= CHUNK_NEEDS_SAVE;
    if (s.id == I_ARROW) {
        shootArrow(nullptr, px, py, pz, (float)dx, 0.1f, (float)dz, 1.1f, 6.0f, true);
        playSound("random.bow", (float)x, (float)y, (float)z, 1.0f, 1.2f);
    } else if (s.id == I_EGG || s.id == I_SNOWBALL) {
        if (Entity* e = spawn(s.id == I_EGG ? ENT_EGG : ENT_SNOWBALL, px, py, pz)) {
            float vx = (float)dx, vy = 0.1f, vz = (float)dz;
            float l = std::sqrt(vx * vx + vy * vy + vz * vz);
            vx = (vx / l + gaussian(rand) * 0.0075f * 6.0f) * 1.1f;
            vy = (vy / l + gaussian(rand) * 0.0075f * 6.0f) * 1.1f;
            vz = (vz / l + gaussian(rand) * 0.0075f * 6.0f) * 1.1f;
            e->mx = vx, e->my = vy, e->mz = vz;
            e->yaw = e->prevYaw = std::atan2(vx, vz) * 57.29578f;
            e->pitch = std::atan2(vy, std::sqrt(vx * vx + vz * vz)) * 57.29578f;
        }
        playSound("random.bow", (float)x, (float)y, (float)z, 1.0f, 1.2f);
    } else {
        if (Entity* e = spawn(ENT_ITEM, px, py - 0.3f, pz)) {
            e->item = s;
            e->spin = rand.nextFloat() * 360.0f;
            float v = rand.nextFloat() * 0.1f + 0.2f;
            e->mx = dx * v + gaussian(rand) * 0.0075f * 6.0f;
            e->my = 0.2f + gaussian(rand) * 0.0075f * 6.0f;
            e->mz = dz * v + gaussian(rand) * 0.0075f * 6.0f;
        }
        playSound("random.click", (float)x, (float)y, (float)z, 1.0f, 1.0f);
    }
}

// ------------------------------------------------------------------ pistons

bool Game::pistonPowered(int x, int y, int z, int dir) const {
    return (dir != 0 && powerTo(x, y - 1, z, 0)) || (dir != 1 && powerTo(x, y + 1, z, 1)) ||
           (dir != 2 && powerTo(x, y, z - 1, 2)) || (dir != 3 && powerTo(x, y, z + 1, 3)) ||
           (dir != 5 && powerTo(x + 1, y, z, 5)) || (dir != 4 && powerTo(x - 1, y, z, 4)) || powerTo(x, y, z, 0) ||
           powerTo(x, y + 2, z, 1) || powerTo(x, y + 1, z - 1, 2) || powerTo(x, y + 1, z + 1, 3) ||
           powerTo(x - 1, y + 1, z, 4) || powerTo(x + 1, y + 1, z, 5);
}

bool Game::pistonCanPush(uint8_t id, int x, int y, int z, bool destroy) const {
    if (id == OBSIDIAN) return false;
    if (!blockIsPiston(id)) {
        if (g_blocks[id].hardness < 0) return false;
        int m = blockMobility(id);
        if (m == 2 || (!destroy && m == 1)) return false;
    } else if (world.getMeta(x, y, z) & 8) {
        return false;
    }
    return !hasTileEntity(id) && !world.tileAt(x, y, z);
}

bool Game::pistonCanExtend(int x, int y, int z, int dir) const {
    int px = x + DX[dir], py = y + DY[dir], pz = z + DZ[dir];
    for (int i = 0; i < 13; ++i) {
        if (py <= 0 || py >= World::SY - 1) return false;
        uint8_t id = world.get(px, py, pz);
        if (id == AIR) return true;
        if (!pistonCanPush(id, px, py, pz, true)) return false;
        if (blockMobility(id) == 1) return true;
        if (i == 12) return false;
        px += DX[dir], py += DY[dir], pz += DZ[dir];
    }
    return true;
}

TileEntity* Game::pistonTile(int x, int y, int z, uint8_t id, int meta, int dir, bool extending, bool source) {
    TileEntity* t = world.createTile(x, y, z, TILE_PISTON);
    if (!t) return nullptr;
    t->burnTotal = id;
    t->cookTime = (int16_t)meta;
    t->burnTime = (int16_t)(dir | (extending ? 8 : 0) | (source ? 16 : 0));
    return t;
}

bool Game::pistonTryExtend(int x, int y, int z, int dir) {
    int px = x + DX[dir], py = y + DY[dir], pz = z + DZ[dir];
    int i = 0;
    for (; i < 13; ++i) {
        if (py <= 0 || py >= World::SY - 1) return false;
        uint8_t id = world.get(px, py, pz);
        if (id == AIR) break;
        if (!pistonCanPush(id, px, py, pz, true)) return false;
        if (blockMobility(id) == 1) {
            if (mode == MODE_SURVIVAL) dropBlockItems(px, py, pz, id, world.getMeta(px, py, pz));
            setBlockNotify(px, py, pz, AIR);
            break;
        }
        if (i == 12) return false;
        px += DX[dir], py += DY[dir], pz += DZ[dir];
    }
    uint8_t self = world.get(x, y, z);
    int headMeta = dir | (self == STICKY_PISTON ? 8 : 0);
    while (px != x || py != y || pz != z) {
        int bx = px - DX[dir], by = py - DY[dir], bz = pz - DZ[dir];
        uint8_t id = world.get(bx, by, bz);
        int meta = world.getMeta(bx, by, bz);
        if (bx == x && by == y && bz == z) {
            world.setBlock(px, py, pz, PISTON_MOVING, headMeta);
            pistonTile(px, py, pz, PISTON_HEAD, headMeta, dir, true, false);
        } else {
            world.setBlock(px, py, pz, PISTON_MOVING, meta);
            pistonTile(px, py, pz, id, meta, dir, true, false);
        }
        px = bx, py = by, pz = bz;
    }
    return true;
}

// BlockPistonBase.updatePistonState
void Game::pistonUpdate(int x, int y, int z) {
    int meta = world.getMeta(x, y, z), dir = meta & 7;
    if (meta == 7 || dir > 5) return;
    bool powered = pistonPowered(x, y, z, dir);
    if (powered && !(meta & 8)) {
        if (pistonCanExtend(x, y, z, dir)) {
            world.setMeta(x, y, z, dir | 8);
            pistonEvent(x, y, z, 0, dir);
        }
    } else if (!powered && (meta & 8)) {
        world.setMeta(x, y, z, dir);
        pistonEvent(x, y, z, 1, dir);
    }
}

// BlockPistonBase.powerBlock (the block event)
void Game::pistonEvent(int x, int y, int z, int event, int dir) {
    uint8_t self = world.get(x, y, z);
    if (!blockIsPiston(self)) return;
    pistonBusy_ = true;
    if (event == 0) {
        if (pistonTryExtend(x, y, z, dir)) {
            world.setMeta(x, y, z, dir | 8);
            notifyNeighbors(x, y, z, self);
            playSound("tile.piston.out", x + 0.5f, y + 0.5f, z + 0.5f, 0.5f, rand.nextFloat() * 0.25f + 0.6f);
        }
    } else {
        int fx = x + DX[dir], fy = y + DY[dir], fz = z + DZ[dir];
        TileEntity* front = world.tileAt(fx, fy, fz);
        if (front && front->type == TILE_PISTON) pistonFinish(fx, fy, fz);
        world.setBlock(x, y, z, PISTON_MOVING, dir);
        pistonTile(x, y, z, self, dir, dir, false, true);
        float sx = x + 0.5f, sy = y + 0.5f, sz = z + 0.5f;
        if (self == STICKY_PISTON) {
            int x2 = fx + DX[dir], y2 = fy + DY[dir], z2 = fz + DZ[dir];
            uint8_t id2 = world.get(x2, y2, z2);
            int m2 = world.getMeta(x2, y2, z2);
            bool pulledMoving = false;
            if (id2 == PISTON_MOVING) {
                TileEntity* t2 = world.tileAt(x2, y2, z2);
                if (t2 && t2->type == TILE_PISTON && pistonFacing(*t2) == dir && pistonExtending(*t2)) {
                    id2 = (uint8_t)t2->burnTotal;
                    m2 = t2->cookTime;
                    pistonFinish(x2, y2, z2);
                    pulledMoving = true;
                }
            }
            if (!pulledMoving && id2 != AIR && pistonCanPush(id2, x2, y2, z2, false) &&
                (blockMobility(id2) == 0 || blockIsPiston(id2))) {
                pistonBusy_ = false;
                setBlockNotify(x2, y2, z2, AIR);
                pistonBusy_ = true;
                world.setBlock(fx, fy, fz, PISTON_MOVING, m2);
                pistonTile(fx, fy, fz, id2, m2, dir, false, false);
                sx += DX[dir], sy += DY[dir], sz += DZ[dir];
            } else if (!pulledMoving) {
                pistonBusy_ = false;
                setBlockNotify(fx, fy, fz, AIR);
                pistonBusy_ = true;
            }
        } else {
            pistonBusy_ = false;
            setBlockNotify(fx, fy, fz, AIR);
            pistonBusy_ = true;
        }
        playSound("tile.piston.in", sx, sy, sz, 0.5f, rand.nextFloat() * 0.15f + 0.6f);
    }
    pistonBusy_ = false;
}

// TileEntityPiston.clearPistonTileEntity
void Game::pistonFinish(int x, int y, int z) {
    TileEntity* t = world.tileAt(x, y, z);
    if (!t || t->type != TILE_PISTON) return;
    uint8_t id = (uint8_t)t->burnTotal;
    int meta = t->cookTime;
    world.removeTile(x, y, z);
    if (world.get(x, y, z) == PISTON_MOVING) setBlockNotify(x, y, z, id, meta);
}

// TileEntityPiston.pushEntities
void Game::pistonPush(const TileEntity& t, float progress, float amount) {
    uint8_t id = (uint8_t)t.burnTotal;
    if (id == AIR || id == PISTON_MOVING || !blockIsSolid(id)) return;
    int dir = pistonFacing(t);
    float f = pistonExtending(t) ? progress - 1 : 1 - progress;
    AABB b = {0, 0, 0, 1, 1, 1};
    if (id == PISTON_HEAD) blockShapeBox(id, t.cookTime, b);
    b = b.offset(t.x + DX[dir] * f, t.y + DY[dir] * f, t.z + DZ[dir] * f);
    float mx = DX[dir] * amount, my = DY[dir] * amount, mz = DZ[dir] * amount;
    if (!dead && b.intersects(player.box())) player.move(world, mx, my, mz);
    for (Entity& e : ents)
        if (e.type != ENT_NONE && e.type != ENT_PAINTING && b.intersects(e.box())) e.move(world, mx, my, mz);
}

void Game::tickPistons() {
    static int pos[256][3];
    int n = 0;
    for (int i = 0; i < World::RING; ++i)
        for (int j = 0; j < World::RING; ++j) {
            const Chunk* c = world.slot(i, j);
            if (!c) continue;
            for (const TileEntity* t = c->tiles; t && n < 256; t = t->next)
                if (t->type == TILE_PISTON) pos[n][0] = t->x, pos[n][1] = t->y, pos[n][2] = t->z, ++n;
        }
    for (int i = 0; i < n; ++i) {
        int x = pos[i][0], y = pos[i][1], z = pos[i][2];
        TileEntity* t = world.tileAt(x, y, z);
        if (!t || t->type != TILE_PISTON) continue;
        int steps = pistonSteps(*t);
        if (steps >= 2) {
            setPistonSteps(*t, 2, 2);
            pistonPush(*t, 1.0f, 0.25f);
            uint8_t id = (uint8_t)t->burnTotal;
            int meta = t->cookTime;
            world.removeTile(x, y, z);
            if (world.get(x, y, z) == PISTON_MOVING) setBlockNotify(x, y, z, id, meta);
        } else {
            setPistonSteps(*t, steps, steps + 1);
            if (pistonExtending(*t)) pistonPush(*t, (steps + 1) * 0.5f, 0.5f + 0.0625f);
        }
        if (Chunk* c = world.chunk(x >> 4, z >> 4)) c->flags |= CHUNK_NEEDS_SAVE;
    }
}

// ------------------------------------------------------------------ block hooks

bool Game::redstoneNeighborChange(int x, int y, int z, uint8_t id, uint8_t changed) {
    auto cube = [&](int ax, int ay, int az) { return blockIsNormalCube(world.get(ax, ay, az)); };
    auto pop = [&]() {
        if (mode == MODE_SURVIVAL) dropBlockItems(x, y, z, id, world.getMeta(x, y, z));
        setBlockNotify(x, y, z, AIR);
    };
    int meta = world.getMeta(x, y, z);
    switch (id) {
        case REDSTONE_WIRE:
            if (!cube(x, y - 1, z)) pop();
            else wireUpdate(x, y, z);
            return true;
        case REDSTONE_TORCH_OFF:
        case REDSTONE_TORCH_ON: {
            bool ok;
            switch (meta) {
                case 1: ok = cube(x - 1, y, z); break;
                case 2: ok = cube(x + 1, y, z); break;
                case 3: ok = cube(x, y, z - 1); break;
                case 4: ok = cube(x, y, z + 1); break;
                default: ok = cube(x, y - 1, z) || world.get(x, y - 1, z) == FENCE; break;
            }
            if (!ok) {
                pop();
                return true;
            }
            world.scheduleTick(x, y, z, id, 2);
            return true;
        }
        case REPEATER_OFF:
        case REPEATER_ON: {
            if (!cube(x, y - 1, z)) {
                pop();
                return true;
            }
            bool powered = repeaterPowered(x, y, z, meta);
            if ((id == REPEATER_ON) != powered) world.scheduleTick(x, y, z, id, repeaterDelay(meta));
            return true;
        }
        case LEVER:
        case STONE_BUTTON: {
            bool ok;
            switch (meta & 7) {
                case 1: ok = cube(x - 1, y, z); break;
                case 2: ok = cube(x + 1, y, z); break;
                case 3: ok = cube(x, y, z - 1); break;
                case 4: ok = cube(x, y, z + 1); break;
                default: ok = id == LEVER && cube(x, y - 1, z); break;
            }
            if (!ok) pop();
            return true;
        }
        case STONE_PLATE:
        case WOOD_PLATE:
            if (!cube(x, y - 1, z)) pop();
            return true;
        case WOOD_DOOR:
        case IRON_DOOR:
            if (changed != AIR && canProvidePower(changed)) {
                int by = (meta & 8) ? y - 1 : y;
                if (world.get(x, by, z) == id)
                    doorPowered(x, by, z, indirectlyPowered(x, by, z) || indirectlyPowered(x, by + 1, z));
            }
            return true;
        case NOTE_BLOCK:
            if (changed != AIR && canProvidePower(changed)) {
                bool powered = blockPowered(x, y, z);
                TileEntity* t = world.tileAt(x, y, z);
                if (!t) t = world.createTile(x, y, z, TILE_NOTE);
                if (t && (t->cookTime != 0) != powered) {
                    if (powered) notePlay(x, y, z);
                    t->cookTime = powered ? 1 : 0;
                }
            }
            return true;
        case DISPENSER:
            if (changed != AIR && canProvidePower(changed) &&
                (indirectlyPowered(x, y, z) || indirectlyPowered(x, y + 1, z)))
                world.scheduleTick(x, y, z, DISPENSER, 4);
            return true;
        case PISTON:
        case STICKY_PISTON:
            if (!pistonBusy_) pistonUpdate(x, y, z);
            return true;
        case PISTON_HEAD: {
            int dir = meta & 7;
            if (dir > 5) dir = 0;
            int bx = x - DX[dir], by = y - DY[dir], bz = z - DZ[dir];
            if (!blockIsPiston(world.get(bx, by, bz))) setBlockNotify(x, y, z, AIR);
            else if (!pistonBusy_) pistonUpdate(bx, by, bz);
            return true;
        }
        case RAIL:
        case POWERED_RAIL:
        case DETECTOR_RAIL: railNeighborChange(x, y, z, id, changed); return true;
        default: return false;
    }
}

void Game::redstoneAdded(int x, int y, int z, uint8_t id) {
    switch (id) {
        case REDSTONE_WIRE: {
            wireUpdate(x, y, z);
            notifyNeighbors(x, y + 1, z, id);
            notifyNeighbors(x, y - 1, z, id);
            wireNotifyIfWire(x - 1, y, z);
            wireNotifyIfWire(x + 1, y, z);
            wireNotifyIfWire(x, y, z - 1);
            wireNotifyIfWire(x, y, z + 1);
            static const int N4[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
            for (const auto& d : N4) {
                bool up = blockIsNormalCube(world.get(x + d[0], y, z + d[1]));
                wireNotifyIfWire(x + d[0], up ? y + 1 : y - 1, z + d[1]);
            }
            break;
        }
        case REDSTONE_TORCH_ON:
        case REPEATER_OFF:
        case REPEATER_ON: notifyAround(x, y, z, id); break;
        case PISTON:
        case STICKY_PISTON:
            if (!world.tileAt(x, y, z)) pistonUpdate(x, y, z);
            break;
        case RAIL:
        case POWERED_RAIL:
        case DETECTOR_RAIL: railRefresh(x, y, z, indirectlyPowered(x, y, z), true); break;
        default: break;
    }
}

void Game::redstoneRemoved(int x, int y, int z, uint8_t id, int meta) {
    switch (id) {
        case REDSTONE_WIRE: {
            notifyNeighbors(x, y + 1, z, id);
            notifyNeighbors(x, y - 1, z, id);
            wireUpdate(x, y, z);
            wireNotifyIfWire(x - 1, y, z);
            wireNotifyIfWire(x + 1, y, z);
            wireNotifyIfWire(x, y, z - 1);
            wireNotifyIfWire(x, y, z + 1);
            static const int N4[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
            for (const auto& d : N4) {
                bool up = blockIsNormalCube(world.get(x + d[0], y, z + d[1]));
                wireNotifyIfWire(x + d[0], up ? y + 1 : y - 1, z + d[1]);
            }
            break;
        }
        case REDSTONE_TORCH_ON: notifyAround(x, y, z, id); break;
        case LEVER:
        case STONE_BUTTON:
            if (meta & 8) notifyAttached(x, y, z, id, meta);
            break;
        case STONE_PLATE:
        case WOOD_PLATE:
            if (meta > 0) {
                notifyNeighbors(x, y, z, id);
                notifyNeighbors(x, y - 1, z, id);
            }
            break;
        case PISTON_HEAD: {
            static const int OPP[6] = {1, 0, 3, 2, 5, 4};
            int dir = meta & 7;
            if (dir > 5) break;
            int back = OPP[dir];
            int bx = x + DX[back], by = y + DY[back], bz = z + DZ[back];
            uint8_t b = world.get(bx, by, bz);
            if (blockIsPiston(b) && (world.getMeta(bx, by, bz) & 8)) {
                if (mode == MODE_SURVIVAL) dropBlockItems(bx, by, bz, b, world.getMeta(bx, by, bz));
                setBlockNotify(bx, by, bz, AIR);
            }
            break;
        }
        case PISTON_MOVING: world.removeTile(x, y, z); break;
        default: break;
    }
}

bool Game::redstoneTick(int x, int y, int z, uint8_t id) {
    switch (id) {
        case REDSTONE_TORCH_OFF:
        case REDSTONE_TORCH_ON: redstoneTorchTick(x, y, z, id); return true;
        case REPEATER_OFF:
        case REPEATER_ON: {
            int meta = world.getMeta(x, y, z);
            bool powered = repeaterPowered(x, y, z, meta);
            if (id == REPEATER_ON && !powered) {
                setBlockNotify(x, y, z, REPEATER_OFF, meta);
            } else if (id == REPEATER_OFF) {
                setBlockNotify(x, y, z, REPEATER_ON, meta);
                if (!powered) world.scheduleTick(x, y, z, REPEATER_ON, repeaterDelay(meta));
            }
            return true;
        }
        case STONE_BUTTON: {
            int meta = world.getMeta(x, y, z);
            if (meta & 8) {
                world.setMeta(x, y, z, meta & 7);
                notifyAttached(x, y, z, id, meta);
                playSound("random.click", x + 0.5f, y + 0.5f, z + 0.5f, 0.3f, 0.5f);
            }
            return true;
        }
        case STONE_PLATE:
        case WOOD_PLATE:
            if (world.getMeta(x, y, z) != 0) plateUpdate(x, y, z);
            return true;
        case DISPENSER:
            if (indirectlyPowered(x, y, z) || indirectlyPowered(x, y + 1, z)) dispense(x, y, z);
            return true;
        case DETECTOR_RAIL: {
            int meta = world.getMeta(x, y, z);
            if (meta & 8) detectorUpdate(x, y, z, meta);
            return true;
        }
        default: return false;
    }
}

bool Game::redstoneUse(int x, int y, int z, uint8_t id) {
    switch (id) {
        case LEVER: toggleLever(x, y, z); return true;
        case STONE_BUTTON: pressButton(x, y, z); return true;
        case REPEATER_OFF:
        case REPEATER_ON: {
            int meta = world.getMeta(x, y, z);
            world.setMeta(x, y, z, ((((meta >> 2) + 1) << 2) & 12) | (meta & 3));
            notifyNeighbors(x, y, z, id);
            return true;
        }
        case NOTE_BLOCK: {
            TileEntity* t = world.tileAt(x, y, z);
            if (!t) t = world.createTile(x, y, z, TILE_NOTE);
            if (!t) return true;
            t->burnTime = (int16_t)((t->burnTime + 1) % 25);
            if (Chunk* c = world.chunk(x >> 4, z >> 4)) c->flags |= CHUNK_NEEDS_SAVE;
            notePlay(x, y, z);
            return true;
        }
        default: return false;
    }
}

bool Game::redstoneClick(int x, int y, int z, uint8_t id) {
    switch (id) {
        case LEVER: toggleLever(x, y, z); return true;
        case STONE_BUTTON: pressButton(x, y, z); return true;
        case NOTE_BLOCK: notePlay(x, y, z); return true;
        default: return false;
    }
}

// Block.onBlockPlacedBy after the block and its metadata are in place.
void Game::redstonePlaced(int x, int y, int z, uint8_t id) {
    if (id == REPEATER_OFF && repeaterPowered(x, y, z, world.getMeta(x, y, z)))
        world.scheduleTick(x, y, z, REPEATER_OFF, 1);
}
