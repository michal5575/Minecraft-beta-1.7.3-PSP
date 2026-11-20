// Desktop harness: streams an infinite world with the shared core code, runs survival ticks,
// checks save/load round trips and crafting, and writes a top-down map of the spawn area.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../assets.h"
#include "../core/audio.h"
#include "../core/blocks.h"
#include "../core/chunkio.h"
#include "../core/crafting.h"
#include "../core/fluid.h"
#include "../core/game.h"
#include "../core/mesher.h"
#include "../core/world.h"
#include "../core/worldgen.h"

static double now() { return platformNow(); }

static uint32_t areaChecksum(const World& w, int x0, int z0, int size) {
    uint32_t h = 2166136261u;
    for (int x = x0; x < x0 + size; ++x)
        for (int z = z0; z < z0 + size; ++z)
            for (int y = 0; y < World::SY; ++y) {
                h = (h ^ w.get(x, y, z)) * 16777619u;
                h = (h ^ (uint32_t)w.getMeta(x, y, z)) * 16777619u;
            }
    return h;
}

static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("  %-48s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) ++fails;
}

int main(int argc, char** argv) {
    int64_t seed = argc > 1 ? std::strtoll(argv[1], nullptr, 10) : 1234567;
    const char* mapPath = argc > 2 ? argv[2] : "map.ppm";
    const char* dir = "test_world";
    std::system("rm -rf test_world");
    initBlocks();
    mesherInit({asset_grasscolor, asset_foliagecolor});

    Game* g = new Game();
    if (!g->init() || !g->createWorld(dir, seed, MODE_SURVIVAL)) {
        std::printf("world setup failed\n");
        return 1;
    }
    const int R = 3;
    double t0 = now();
    while (!g->world.readyCount(0.5f, 0.5f, 1)) g->stream(R, 1.0);
    g->placeAtSpawn();
    while (!g->spawnReady(R)) g->stream(R, 1.0);
    double t1 = now();
    std::printf("seed %lld: spawn area (%d chunks loaded) ready in %.2fs, spawn %d %d %d\n", (long long)seed,
                g->world.loadedChunks, t1 - t0, g->spawnX, g->spawnY, g->spawnZ);

    // Mesh statistics for the visible area.
    CubeBuffer cub;
    MeshBuffer opq, trn;
    cub.init(65532);
    opq.init(32766);
    trn.init(16380);
    long long totalCub = 0, totalOpq = 0, totalTrn = 0;
    int maxCub = 0, maxOpq = 0, overflow = 0, sections = 0;
    int pcx = (int)std::floor(g->player.x / 16), pcz = (int)std::floor(g->player.z / 16);
    double t2 = now();
    for (int cx = pcx - R; cx <= pcx + R; ++cx)
        for (int cz = pcz - R; cz <= pcz + R; ++cz)
            for (int sy = 0; sy < World::SECTIONS_Y; ++sy) {
                buildSection(g->world, cx, sy, cz, cub, opq, trn);
                ++sections;
                totalCub += cub.count;
                totalOpq += opq.count;
                totalTrn += trn.count;
                if (cub.count > maxCub) maxCub = cub.count;
                if (opq.count > maxOpq) maxOpq = opq.count;
                if (cub.overflow || opq.overflow || trn.overflow) ++overflow;
            }
    double t3 = now();
    double mb = (totalCub * sizeof(CubeVertex) + (totalOpq + totalTrn) * sizeof(MeshVertex)) / (1024.0 * 1024.0);
    std::printf("meshed %d sections in %.2fs: %.1f MB of vertices, max/section %d cube %d other, overflow %d\n",
                sections, t3 - t2, mb, maxCub, maxOpq, overflow);
    size_t chunkBytes = 0;
    for (int i = 0; i < World::RING; ++i)
        for (int j = 0; j < World::RING; ++j)
            if (const Chunk* c = g->world.slot(i, j)) {
                chunkBytes += sizeof(Chunk);
                for (const Section* s : c->sec)
                    if (s) chunkBytes += sizeof(Section);
            }
    std::printf("chunk memory: %.1f MB for %d chunks\n", chunkBytes / (1024.0 * 1024.0), g->world.loadedChunks);

    // Top-down map around the spawn.
    {
        const int S = (2 * R + 1) * 16;
        int x0 = pcx * 16 - R * 16, z0 = pcz * 16 - R * 16;
        FILE* f = std::fopen(mapPath, "wb");
        std::fprintf(f, "P6\n%d %d\n255\n", S, S);
        for (int z = z0; z < z0 + S; ++z)
            for (int x = x0; x < x0 + S; ++x) {
                int y = World::SY - 1;
                while (y > 0 && g->world.get(x, y, z) == AIR) --y;
                uint8_t id = g->world.get(x, y, z);
                int tex = blockTexture(id, g->world.getMeta(x, y, z), 1);
                const uint8_t* c = asset_tilecolors + tex * 3;
                uint32_t tint = blockTint(&g->world, id, g->world.getMeta(x, y, z), 1, x, z);
                float shade = 0.6f + 0.4f * (float)y / 128.0f;
                unsigned char px[3];
                for (int k = 0; k < 3; ++k) {
                    float t = ((tint >> (16 - 8 * k)) & 255) / 255.0f;
                    float v = c[k] * t * shade;
                    px[k] = (unsigned char)(v > 255 ? 255 : v);
                }
                std::fwrite(px, 1, 3, f);
            }
        std::fclose(f);
    }

    // Block edits with relight.
    double t6 = now();
    int sx = (int)std::floor(g->player.x), sz = (int)std::floor(g->player.z);
    for (int i = 0; i < 20; ++i) {
        int x = sx + i - 10, z = sz + 3;
        int y = g->world.heightAt(x, z);
        g->world.setBlock(x, y, z, GLOWSTONE);
    }
    std::printf("20 block edits with relight: %.1f ms each\n", (now() - t6) * 1000.0 / 20);
    check(g->world.getLight(sx - 10, g->world.heightAt(sx - 10, sz + 3) + 1, sz + 3) != 0, "glowstone lights");

    // Mining by hand: grass takes 18 ticks (hardness 0.6).
    {
        Player& p = g->player;
        p.pitch = 90;
        p.mx = p.my = p.mz = 0;
        for (int i = 0; i < 5; ++i) g->tick();
        g->updateTarget(p.x, p.y + Player::EYE, p.z);
        int tx = g->blockTarget.x, ty = g->blockTarget.y, tz = g->blockTarget.z;
        uint8_t before = g->world.get(tx, ty, tz);
        g->attackHeld = g->attackPressed = true;
        int stageAt6 = -2, ticks = 0;
        while (ticks < 200 && g->world.get(tx, ty, tz) == before) {
            g->tick();
            g->updateTarget(p.x, p.y + Player::EYE, p.z);
            if (++ticks == 6) stageAt6 = g->crackStage();
        }
        g->attackHeld = false;
        std::printf("mined %s in %d ticks (crack stage after 6 ticks: %d, on ground %d)\n",
                    itemName(ItemStack(before)), ticks, stageAt6, p.onGround ? 1 : 0);
        check(stageAt6 > 0 && ticks < 200, "mining progresses with a crack");
        int hits = 0, breaks = 0;
        const char* expect = blockStepSound(before).broken;
        for (int i = 0; i < g->soundCount; ++i) {
            if (!std::strcmp(g->sounds[i].name, expect) && g->sounds[i].volume < 0.5f) ++hits;
            if (!std::strcmp(g->sounds[i].name, expect) && g->sounds[i].volume == 1.0f) ++breaks;
        }
        std::printf("mining sounds: %d hits, %d break (%d queued)\n", hits, breaks, g->soundCount);
        check(hits >= 4 && breaks == 1, "mining plays hit and break sounds");
        g->soundCount = 0;
    }

    // Survival ticks at night with mobs.
    g->world.time = 14000;
    int spawned = 0;
    double t7 = now();
    for (int i = 0; i < 2400; ++i) {
        g->tick();
        if (i % 4 == 0) g->stream(R, 0.01);
    }
    for (const Entity& e : g->ents)
        if (entityIsMob(e.type)) ++spawned;
    std::printf("2400 night ticks: %.2f ms/tick, %d mobs alive, health %d%s\n", (now() - t7) * 1000.0 / 2400, spawned,
                g->health, g->dead ? " (died)" : "");

    // Walk far away so the spawn chunks unload and get saved, then come back.
    int ref = (int)std::floor(g->player.x) - 8, refZ = (int)std::floor(g->player.z) - 8;
    g->world.setBlock(ref, 100, refZ, DIAMOND_BLOCK);
    uint32_t before = areaChecksum(g->world, ref - 24, refZ - 24, 48);
    float homeX = g->player.x, homeZ = g->player.z;
    double t8 = now();
    int walked = 0;
    for (int step = 0; step < 40; ++step) {
        g->player.setPosition(homeX + step * 16.0f, 120.0f, homeZ);
        for (int k = 0; k < 40; ++k) g->stream(R, 0.05);
        ++walked;
    }
    std::printf("walked %d chunks east in %.2fs, %d chunks loaded, ready around player: %d\n", walked, now() - t8,
                g->world.loadedChunks, g->world.readyCount(g->player.x, g->player.z, R));
    check(g->world.chunk(ref >> 4, refZ >> 4) == nullptr, "spawn chunk unloaded");
    g->player.setPosition(homeX, 120.0f, homeZ);
    while (!g->spawnReady(R)) g->stream(R, 1.0);
    check(areaChecksum(g->world, ref - 24, refZ - 24, 48) == before, "terrain identical after unload/reload");
    check(g->world.get(ref, 100, refZ) == DIAMOND_BLOCK, "edit survived unload");

    // Full close and reopen.
    g->inv.main[5] = ItemStack(I_IRON_PICKAXE, 1, 17);
    g->closeWorld();
    double t9 = now();
    check(g->loadWorld(dir), "level.dat loads");
    while (!g->spawnReady(R)) g->stream(R, 1.0);
    std::printf("reloaded in %.2fs\n", now() - t9);
    check(areaChecksum(g->world, ref - 24, refZ - 24, 48) == before, "terrain identical after reopen");
    check(g->inv.main[5].id == I_IRON_PICKAXE && g->inv.main[5].damage == 17, "inventory survived");

    // Crafting.
    ItemStack grid[9];
    grid[0] = ItemStack(LOG, 1, 2);
    check(craftResult(grid, 2).id == PLANKS && craftResult(grid, 2).count == 4, "birch log -> 4 planks");
    for (auto& s : grid) s.clear();
    grid[0] = grid[1] = grid[2] = ItemStack(COBBLESTONE);
    grid[4] = grid[7] = ItemStack(I_STICK);
    check(craftResult(grid, 3).id == I_STONE_PICKAXE, "stone pickaxe");
    for (auto& s : grid) s.clear();
    grid[1] = grid[2] = grid[5] = ItemStack(PLANKS);
    grid[4] = grid[7] = ItemStack(I_STICK);
    check(craftResult(grid, 3).id == I_WOOD_AXE, "mirrored wooden axe");
    for (auto& s : grid) s.clear();
    grid[0] = ItemStack(PLANKS);
    grid[3] = ItemStack(PLANKS);
    check(craftResult(grid, 2).empty(), "no recipe for 2 diagonal planks");
    check(smeltResult(ItemStack(IRON_ORE)).id == I_IRON_INGOT, "iron ore smelts");
    check(fuelValue(ItemStack(I_COAL)) == 1600, "coal fuel");
    for (auto& s : grid) s.clear();
    grid[0] = grid[1] = ItemStack(WOOL, 1, 14);
    grid[2] = ItemStack(WOOL);
    grid[3] = grid[4] = grid[5] = ItemStack(PLANKS);
    check(craftResult(grid, 3).id == I_BED && craftResult(grid, 3).count == 1, "bed from wool and planks");

    // Bed: place, sleep through the night, respawn next to it, break it.
    {
        int px = (int)std::floor(g->player.x), pz = (int)std::floor(g->player.z);
        for (int x = px - 4; x <= px + 4; ++x)
            for (int z = pz - 4; z <= pz + 4; ++z) {
                g->world.setBlock(x, 99, z, STONE);
                for (int y = 100; y < 104; ++y) g->world.setBlock(x, y, z, AIR);
            }
        for (Entity& e : g->ents) e = Entity();
        g->mode = MODE_CREATIVE;
        g->player.setPosition(px + 0.5f, 100.0f, pz + 0.5f);
        g->player.yaw = 0;
        g->inv.main[g->inv.selected] = ItemStack(I_BED);
        g->entityTarget = nullptr;
        g->hasBlockTarget = true;
        g->blockTarget = {px + 2, 99, pz, 1, 0};
        g->usePressed = true;
        g->tick();
        check(g->world.get(px + 2, 100, pz) == BED && g->world.getMeta(px + 2, 100, pz) == 0 &&
                  g->world.get(px + 2, 100, pz + 1) == BED && g->world.getMeta(px + 2, 100, pz + 1) == 8,
              "bed placed as foot + head");
        g->world.time = 12000;
        g->tick();
        g->blockTarget = {px + 2, 100, pz, 1, 0};
        g->usePressed = true;
        g->tick();
        check(!g->sleeping && g->message != nullptr, "no sleeping during the day");
        g->message = nullptr;
        g->world.time = 15000;
        g->tick();
        g->usePressed = true;
        g->tick();
        check(g->sleeping, "sleeping at night");
        int ticks = 0;
        while (g->sleeping && ticks < 200) g->tick(), ++ticks;
        std::printf("slept %d ticks, time of day now %lld\n", ticks, (long long)(g->world.time % 24000));
        check(!g->sleeping && g->world.time % 24000 < 10 && g->hasBed, "night skipped, spawn set");
        g->player.setPosition(px + 0.5f, 100.0f, pz - 3.5f);
        g->respawn();
        check(std::fabs(g->player.x - (px + 2.5f)) <= 2.0f && std::fabs(g->player.z - (pz + 1.5f)) <= 2.0f &&
                  g->player.y == 100.0f,
              "respawn next to the bed");
        g->mode = MODE_SURVIVAL;
        g->player.setPosition(px + 0.5f, 100.0f, pz + 0.5f);
        g->inv.main[g->inv.selected].clear();
        g->blockTarget = {px + 2, 100, pz + 1, 1, 0};
        g->attackHeld = true;
        for (int i = 0; i < 60 && g->world.get(px + 2, 100, pz + 1) == BED; ++i) g->tick();
        g->attackHeld = false;
        int beds = 0;
        for (const Entity& e : g->ents)
            if (e.type == ENT_ITEM && e.item.id == I_BED) beds += e.item.count;
        check(g->world.get(px + 2, 100, pz) == AIR && g->world.get(px + 2, 100, pz + 1) == AIR && beds == 1,
              "breaking the head removes both halves and drops one bed");
    }

    // Liquids and fire on a stone platform away from the player.
    {
        int cx = (int)std::floor(g->player.x) + 20, cz = (int)std::floor(g->player.z);
        auto platform = [&]() {
            for (int x = cx - 10; x <= cx + 10; ++x)
                for (int z = cz - 10; z <= cz + 10; ++z) {
                    g->world.setBlock(x, 99, z, STONE);
                    for (int y = 100; y < 108; ++y) g->world.setBlock(x, y, z, AIR);
                }
        };
        auto countAt = [&](int y, bool water) {
            int n = 0;
            for (int x = cx - 10; x <= cx + 10; ++x)
                for (int z = cz - 10; z <= cz + 10; ++z) n += fluidSame(g->world.get(x, y, z), water) ? 1 : 0;
            return n;
        };
        auto run = [&](int ticks) {
            for (int i = 0; i < ticks; ++i) g->tick();
        };
        g->mode = MODE_CREATIVE;
        platform();
        g->setBlockNotify(cx, 100, cz, WATER_FLOWING);
        double tw = now();
        g->world.relightCalls = g->world.relightVoxels = 0;
        run(60);
        std::printf("water spreading: %.2f ms/tick, %u relights, %u voxels\n", (now() - tw) * 1000.0 / 60,
                    g->world.relightCalls, g->world.relightVoxels);
        run(240);
        int spread = countAt(100, true);
        std::printf("water spread to %d blocks, edge level %d, scheduled %d\n", spread,
                    g->world.getMeta(cx + 7, 100, cz), g->world.scheduledCount());
        check(spread == 113 && g->world.getMeta(cx + 7, 100, cz) == 7 && g->world.get(cx + 8, 100, cz) == AIR,
              "water spreads 7 blocks on flat ground");
        g->setBlockNotify(cx, 100, cz, AIR);
        run(300);
        check(countAt(100, true) == 0, "water drains when the source is removed");

        platform();
        g->setBlockNotify(cx, 100, cz, WATER_FLOWING);
        g->setBlockNotify(cx + 2, 100, cz, WATER_FLOWING);
        run(300);
        check(g->world.getMeta(cx + 1, 100, cz) == 0 && blockIsWater(g->world.get(cx + 1, 100, cz)),
              "two sources make an infinite source between them");

        platform();
        g->setBlockNotify(cx, 100, cz, LAVA_FLOWING);
        run(5);
        g->setBlockNotify(cx + 3, 100, cz, WATER_FLOWING);
        run(400);
        int obsidian = 0, cobble = 0;
        for (int x = cx - 10; x <= cx + 10; ++x)
            for (int z = cz - 10; z <= cz + 10; ++z) {
                uint8_t b = g->world.get(x, 100, z);
                obsidian += b == OBSIDIAN;
                cobble += b == COBBLESTONE;
            }
        std::printf("lava + water: %d obsidian, %d cobblestone, %d lava left\n", obsidian, cobble, countAt(100, false));
        check(obsidian + cobble > 0, "water hardens lava");

        platform();
        for (int x = cx - 3; x <= cx + 3; ++x)
            for (int z = cz - 3; z <= cz + 3; ++z) g->world.setBlock(x, 100, z, PLANKS);
        g->setBlockNotify(cx, 101, cz, FIRE);
        int maxFire = 0;
        for (int i = 0; i < 3000; ++i) {
            g->tick();
            if (i % 50 == 0) {
                int f = 0;
                for (int x = cx - 5; x <= cx + 5; ++x)
                    for (int z = cz - 5; z <= cz + 5; ++z)
                        for (int y = 100; y < 103; ++y) f += g->world.get(x, y, z) == FIRE;
                if (f > maxFire) maxFire = f;
                // A single fire block can burn out before it spreads; light it again like a player would.
                for (int x = cx - 3; x <= cx + 3 && f == 0 && maxFire <= 3; ++x)
                    for (int z = cz - 3; z <= cz + 3 && f == 0; ++z)
                        if (g->world.get(x, 100, z) == PLANKS && g->world.get(x, 101, z) == AIR) {
                            g->setBlockNotify(x, 101, z, FIRE);
                            f = 1;
                        }
            }
        }
        int planks = 0;
        for (int x = cx - 3; x <= cx + 3; ++x)
            for (int z = cz - 3; z <= cz + 3; ++z) planks += g->world.get(x, 100, z) == PLANKS;
        std::printf("fire: up to %d fire blocks, %d of 49 planks left (raining %d, rain time %d)\n", maxFire, planks,
                    g->isRaining(), g->rainTime);
        check(maxFire > 3 && planks < 49, "fire spreads and burns planks");

        // Incremental relights must match a full recompute.
        platform();
        g->setBlockNotify(cx, 101, cz, WATER_FLOWING);
        g->setBlockNotify(cx + 4, 101, cz, LAVA_FLOWING);
        run(150);
        static uint8_t snap[41][41][World::SY];
        for (int x = 0; x < 41; ++x)
            for (int z = 0; z < 41; ++z)
                for (int y = 0; y < World::SY; ++y) snap[x][z][y] = g->world.getLight(cx - 20 + x, y, cz - 20 + z);
        g->world.relight(cx - 5, cz - 5);
        g->world.relight(cx + 5, cz + 5);
        g->world.relight(cx - 5, cz + 5);
        g->world.relight(cx + 5, cz - 5);
        int diff = 0;
        for (int x = 5; x < 36; ++x)
            for (int z = 5; z < 36; ++z)
                for (int y = 0; y < World::SY; ++y) diff += snap[x][z][y] != g->world.getLight(cx - 20 + x, y, cz - 20 + z);
        std::printf("incremental light vs full relight: %d differing voxels\n", diff);
        check(diff == 0, "incremental relight matches full relight");

        // Random edits around the surface and underground.
        int ex = cx + 40, ez = cz;
        uint32_t seed = 12345;
        auto rnd = [&](int n) {
            seed = seed * 1103515245u + 12345u;
            return (int)((seed >> 16) % (uint32_t)n);
        };
        static const uint8_t ids[] = {STONE, AIR, AIR, GLOWSTONE, WATER, LEAVES, GLASS, TORCH};
        double te = now();
        g->world.lightUpdates = g->world.relightCalls = 0;
        for (int i = 0; i < 300; ++i) {
            int x = ex + rnd(11) - 5, z = ez + rnd(11) - 5;
            int top = g->world.heightAt(x, z);
            int y = rnd(2) ? top - rnd(4) : 20 + rnd(30);
            g->world.setBlock(x, y, z, ids[rnd(sizeof(ids))]);
        }
        std::printf("300 random edits: %.3f ms each, %u incremental, %u fallbacks\n", (now() - te) * 1000.0 / 300,
                    g->world.lightUpdates, g->world.relightCalls);
        for (int x = 0; x < 41; ++x)
            for (int z = 0; z < 41; ++z)
                for (int y = 0; y < World::SY; ++y) snap[x][z][y] = g->world.getLight(ex - 20 + x, y, ez - 20 + z);
        g->world.relight(ex, ez);
        diff = 0;
        for (int x = 10; x < 31; ++x)
            for (int z = 10; z < 31; ++z)
                for (int y = 0; y < World::SY; ++y) diff += snap[x][z][y] != g->world.getLight(ex - 20 + x, y, ez - 20 + z);
        std::printf("random edits vs full relight: %d differing voxels\n", diff);
        check(diff == 0, "incremental light after random edits matches full relight");
        g->mode = MODE_SURVIVAL;
    }

    // Slime chunks against java.util.Random (reference: 462 hits on this grid for seed 1234567).
    if (seed == 1234567) {
        int n = 0;
        bool first = false;
        for (int x = -300; x <= 300; x += 7)
            for (int z = -300; z <= 300; z += 11)
                if (g->slimeChunk(x, z)) {
                    if (++n == 1) first = x == -300 && z == -223;
                }
        std::printf("slime chunks on grid: %d\n", n);
        check(n == 462 && first, "slime chunks match Java");
    }

    // Mobs: slimes split, wolves tame and form packs.
    {
        int px = (int)std::floor(g->player.x), pz = (int)std::floor(g->player.z);
        const int Y = 110;
        for (int x = -6; x <= 6; ++x)
            for (int z = -6; z <= 6; ++z) {
                g->world.setBlock(px + x, Y - 1, pz + z, STONE);
                for (int y = 0; y < 4; ++y) g->world.setBlock(px + x, Y + y, pz + z, AIR);
            }
        g->player.setPosition(px + 0.5f, Y, pz + 0.5f);
        g->mode = MODE_CREATIVE;
        for (Entity& e : g->ents) e = Entity();
        auto count = [&](uint8_t type, int size) {
            int n = 0;
            for (const Entity& e : g->ents)
                if (e.type == type && e.health > 0 && (size == 0 || e.size == size)) ++n;
            return n;
        };
        auto hit = [&](Entity* e, int item) {
            g->inv.held() = item ? ItemStack((uint16_t)item, 64) : ItemStack();
            g->entityTarget = e;
            g->attackPressed = true;
            g->tick();
        };
        Entity* s = g->spawn(ENT_SLIME, px + 3.5f, Y, pz + 0.5f);
        s->size = 4, s->width = s->height = 2.4f, s->health = 16;
        s->setPosition(s->x, s->y, s->z);
        for (int i = 0; i < 16 && s->health > 0; ++i) {
            s->hurtResist = 0;
            hit(s, 0);
        }
        for (int i = 0; i < 25; ++i) g->tick();
        std::printf("slime split into %d size-2 slimes\n", count(ENT_SLIME, 2));
        check(count(ENT_SLIME, 2) == 4 && count(ENT_SLIME, 4) == 0, "big slime splits in four");
        for (Entity& e : g->ents) e = Entity();
        s = g->spawn(ENT_SLIME, px + 3.5f, Y, pz + 0.5f);
        s->size = 2, s->width = s->height = 1.2f, s->health = 4;
        hit(s, I_DIAMOND_SWORD);
        for (int i = 0; i < 25; ++i) g->tick();
        check(count(ENT_SLIME, 0) == 0, "overkilled slime does not split (Beta)");

        for (Entity& e : g->ents) e = Entity();
        Entity* w = g->spawn(ENT_WOLF, px - 2.5f, Y, pz + 0.5f);
        Entity* w2 = g->spawn(ENT_WOLF, px - 4.5f, Y, pz + 2.5f);
        int bones = 0;
        while (!w->tamed && bones < 60) {
            g->inv.held() = ItemStack(I_BONE, 1);
            g->entityTarget = w;
            g->usePressed = true;
            g->tick();
            ++bones;
        }
        std::printf("wolf tamed after %d bones\n", bones);
        check(w->tamed && w->sitting && w->health == 20, "wolf tames with bones and sits");
        g->inv.held() = ItemStack();
        g->entityTarget = w;
        g->usePressed = true;
        g->tick();
        check(!w->sitting, "tamed wolf stands up on command");
        g->mode = MODE_SURVIVAL;
        hit(w2, 0);
        check(w2->angry && !w->angry && w->target == (int16_t)(w2 - g->ents), "hit wolf turns angry, tamed wolf defends");
        for (Entity& e : g->ents) e = Entity();
        g->mode = MODE_SURVIVAL;

        // Small items: signs, trapdoors, cake, cobweb, paintings, throwables.
        auto use = [&](int item, int x, int y, int z, int face) {
            g->inv.held() = item ? ItemStack((uint16_t)item, 1) : ItemStack();
            g->entityTarget = nullptr;
            g->hasBlockTarget = true;
            g->blockTarget = {x, y, z, face, 0};
            g->usePressed = true;
            g->tick();
        };
        g->mode = MODE_CREATIVE;
        use(I_SIGN, px + 2, Y - 1, pz, 1);
        TileEntity* sign = g->world.tileAt(px + 2, Y, pz);
        check(g->world.get(px + 2, Y, pz) == SIGN_POST && sign && sign->type == TILE_SIGN &&
                  g->lastUse == UseResult::Sign && g->openTile == sign,
              "sign post placed and opens the editor");
        g->lastUse = UseResult::None;
        if (sign) {
            std::strcpy(sign->text[0], "Hello");
            std::strcpy(sign->text[2], "PSP world 1.7.3");
            static uint8_t buf[256 * 1024];
            uint32_t size = 0;
            Chunk* c = g->world.chunk((px + 2) >> 4, pz >> 4);
            Chunk d{};
            d.cx = c->cx, d.cz = c->cz;
            bool ok = encodeChunk(*c, buf, sizeof(buf), size) && decodeChunk(d, buf, size);
            const TileEntity* back = nullptr;
            for (const TileEntity* t = d.tiles; t; t = t->next)
                if (t->type == TILE_SIGN && t->x == px + 2 && t->y == Y && t->z == pz) back = t;
            check(ok && back && !std::strcmp(back->text[0], "Hello") && !back->text[1][0] &&
                      !std::strcmp(back->text[2], "PSP world 1.7.3"),
                  "sign text survives a chunk save");
        }
        g->setBlockNotify(px + 2, Y - 1, pz, AIR);
        check(g->world.get(px + 2, Y, pz) == AIR && !g->world.tileAt(px + 2, Y, pz), "sign pops off without support");
        g->world.setBlock(px + 2, Y - 1, pz, STONE);

        // Trapdoor hinged on a stone pillar, toggled by hand, dropped when the pillar goes.
        g->world.setBlock(px - 2, Y, pz + 2, STONE);
        use(TRAPDOOR, px - 2, Y, pz + 2, 2);
        check(g->world.get(px - 2, Y, pz + 1) == TRAPDOOR, "trapdoor placed against a wall");
        int tm = g->world.getMeta(px - 2, Y, pz + 1);
        use(0, px - 2, Y, pz + 1, 1);
        check(g->world.getMeta(px - 2, Y, pz + 1) == (tm ^ 4), "trapdoor opens with use");
        g->setBlockNotify(px - 2, Y, pz + 2, AIR);
        check(g->world.get(px - 2, Y, pz + 1) == AIR, "trapdoor falls off without its hinge block");

        // Cake: six bites in survival.
        g->mode = MODE_SURVIVAL;
        g->world.setBlock(px + 1, Y, pz - 2, CAKE);
        g->health = 4;
        int bites = 0;
        while (g->world.get(px + 1, Y, pz - 2) == CAKE && bites < 10) use(0, px + 1, Y, pz - 2, 1), ++bites;
        std::printf("cake eaten in %d bites, health %d\n", bites, g->health);
        check(bites == 6 && g->health == 20, "cake has six slices of 3 health");

        // Cobweb slows the player down.
        g->world.setBlock(px, Y, pz, WEB);
        g->player.setPosition(px + 0.5f, Y, pz + 0.5f);
        g->player.forward = 1;
        for (int i = 0; i < 5; ++i) g->tick();
        float wx = g->player.x, wz = g->player.z;
        for (int i = 0; i < 10; ++i) g->tick();
        float moved = std::hypot(g->player.x - wx, g->player.z - wz);
        g->player.forward = 0;
        std::printf("moved %.3f blocks in 10 ticks inside a cobweb\n", moved);
        check(g->player.inWeb && moved < 0.4f, "cobweb slows movement");
        g->world.setBlock(px, Y, pz, AIR);
        g->player.setPosition(px + 0.5f, Y, pz + 0.5f);

        // Painting on a wall of planks.
        for (int x = -3; x <= 3; ++x)
            for (int y = 0; y < 4; ++y) g->world.setBlock(px + x, Y + y, pz + 4, PLANKS);
        use(I_PAINTING, px, Y + 1, pz + 4, 2);
        int paintings = 0;
        for (const Entity& e : g->ents) paintings += e.type == ENT_PAINTING;
        check(paintings == 1, "painting hangs on a wall");

        // Snowball thrown at a pig.
        for (Entity& e : g->ents) e = Entity();
        g->player.yaw = 0, g->player.pitch = 0;
        Entity* pig = g->spawn(ENT_PIG, px + 0.5f, Y, pz + 3.0f);
        g->hasBlockTarget = false;
        g->inv.held() = ItemStack(I_SNOWBALL, 16);
        g->usePressed = true;
        g->tick();
        int balls = 0;
        for (const Entity& e : g->ents) balls += e.type == ENT_SNOWBALL;
        check(balls == 1 && g->inv.held().count == 15, "snowball thrown");
        for (int i = 0; i < 20; ++i) g->tick();
        balls = 0;
        for (const Entity& e : g->ents) balls += e.type == ENT_SNOWBALL;
        check(balls == 0 && pig->type == ENT_PIG, "snowball hits and vanishes");

        // Fishing rod: cast, then reel back in.
        g->inv.held() = ItemStack(I_FISHING_ROD);
        g->usePressed = true;
        g->tick();
        check(g->fishHook >= 0 && g->ents[g->fishHook].type == ENT_FISH, "fishing rod casts a bobber");
        g->usePressed = true;
        g->tick();
        check(g->fishHook < 0, "fishing rod reels the bobber back");

        for (Entity& e : g->ents) e = Entity();
        g->mode = MODE_SURVIVAL;

        // Redstone: a fresh floor of stone with air above.
        for (int x = -7; x <= 7; ++x)
            for (int z = -7; z <= 7; ++z) {
                g->world.setBlock(px + x, Y - 1, pz + z, STONE);
                for (int y = 0; y < 5; ++y) g->world.setBlock(px + x, Y + y, pz + z, AIR);
            }
        g->mode = MODE_CREATIVE;
        g->player.setPosition(px + 0.5f, Y + 3, pz + 6.5f);
        g->player.flying = true;
        auto ticks = [&](int n) {
            for (int i = 0; i < n; ++i) g->tick();
        };
        // Lever -> wire line -> stone block with a redstone torch on its far side.
        int ax = px - 6, az = pz - 5;
        use(LEVER, ax, Y - 1, az, 1);
        check(g->world.get(ax, Y, az) == LEVER, "lever placed on the floor");
        for (int i = 1; i <= 4; ++i) use(I_REDSTONE, ax + i, Y - 1, az, 1);
        g->world.setBlock(ax + 5, Y, az, STONE);
        use(REDSTONE_TORCH_ON, ax + 5, Y, az, 5);
        ticks(4);
        check(g->world.get(ax + 6, Y, az) == REDSTONE_TORCH_ON && g->world.getMeta(ax + 6, Y, az) == 1,
              "redstone torch lit on the side of a block");
        check(g->world.getMeta(ax + 1, Y, az) == 0 && g->world.get(ax + 1, Y, az) == REDSTONE_WIRE, "wire unpowered");
        use(0, ax, Y, az, 1);
        ticks(4);
        std::printf("wire strength after the lever: %d %d %d %d\n", g->world.getMeta(ax + 1, Y, az),
                    g->world.getMeta(ax + 2, Y, az), g->world.getMeta(ax + 3, Y, az), g->world.getMeta(ax + 4, Y, az));
        check(g->world.getMeta(ax + 1, Y, az) == 15 && g->world.getMeta(ax + 4, Y, az) == 12, "wire carries power");
        check(g->world.get(ax + 6, Y, az) == REDSTONE_TORCH_OFF, "powered block turns the torch off");
        use(0, ax, Y, az, 1);
        ticks(4);
        check(g->world.getMeta(ax + 1, Y, az) == 0 && g->world.getMeta(ax + 4, Y, az) == 0, "wire loses power");
        check(g->world.get(ax + 6, Y, az) == REDSTONE_TORCH_ON, "torch relights");
        g->setBlockNotify(ax + 2, Y - 1, az, AIR);
        check(g->world.get(ax + 2, Y, az) == AIR, "wire drops without a floor");
        g->world.setBlock(ax + 2, Y - 1, az, STONE);

        // Iron door opened by a lever.
        int dx0 = px - 6, dz0 = pz - 2;
        g->world.setBlock(dx0, Y, dz0, IRON_DOOR, 0);
        g->world.setBlock(dx0, Y + 1, dz0, IRON_DOOR, 8);
        use(LEVER, dx0 + 1, Y - 1, dz0, 1);
        use(0, dx0 + 1, Y, dz0, 1);
        check((g->world.getMeta(dx0, Y, dz0) & 4) && (g->world.getMeta(dx0, Y + 1, dz0) & 4), "lever opens an iron door");
        use(0, dx0 + 1, Y, dz0, 1);
        check(!(g->world.getMeta(dx0, Y, dz0) & 4), "iron door closes");

        // Repeater with the longest delay (8 ticks).
        int rx = px - 6, rz = pz + 1;
        use(LEVER, rx, Y - 1, rz, 1);
        g->setBlockNotify(rx + 1, Y, rz, REPEATER_OFF, 1 | (3 << 2));
        use(I_REDSTONE, rx + 2, Y - 1, rz, 1);
        use(0, rx, Y, rz, 1);
        int delay = 0;
        while (g->world.getMeta(rx + 2, Y, rz) == 0 && delay < 40) g->tick(), ++delay;
        std::printf("repeater output after %d ticks\n", delay);
        check(g->world.get(rx + 1, Y, rz) == REPEATER_ON && delay >= 7 && delay <= 9, "repeater delays the signal");
        use(0, rx + 1, Y, rz, 1);
        check(((g->world.getMeta(rx + 1, Y, rz) >> 2) & 3) == 0, "repeater delay cycles on use");

        // Pistons push a cobblestone; the sticky one pulls it back.
        for (int k = 0; k < 2; ++k) {
            uint8_t pid = k ? STICKY_PISTON : PISTON;
            int qx = px - 1, qz = pz - 3 + k * 3;
            g->setBlockNotify(qx, Y, qz, pid, 5);
            g->world.setBlock(qx + 1, Y, qz, COBBLESTONE);
            use(LEVER, qx, Y - 1, qz - 1, 1);
            use(0, qx, Y, qz - 1, 1);
            check(g->world.get(qx + 1, Y, qz) == PISTON_MOVING && g->world.get(qx + 2, Y, qz) == PISTON_MOVING,
                  "piston starts extending");
            ticks(4);
            check(g->world.getMeta(qx, Y, qz) == 13 && g->world.get(qx + 1, Y, qz) == PISTON_HEAD &&
                      g->world.get(qx + 2, Y, qz) == COBBLESTONE,
                  k ? "sticky piston pushes" : "piston pushes a block");
            use(0, qx, Y, qz - 1, 1);
            ticks(4);
            if (k)
                check(g->world.get(qx, Y, qz) == STICKY_PISTON && g->world.get(qx + 1, Y, qz) == COBBLESTONE &&
                          g->world.get(qx + 2, Y, qz) == AIR,
                      "sticky piston pulls the block back");
            else
                check(g->world.get(qx, Y, qz) == PISTON && g->world.getMeta(qx, Y, qz) == 5 &&
                          g->world.get(qx + 1, Y, qz) == AIR && g->world.get(qx + 2, Y, qz) == COBBLESTONE,
                      "piston retracts and leaves the block");
        }

        // Note block on stone: bass drum, pitch rises with each use.
        int nx = px + 3, nz = pz - 5;
        use(NOTE_BLOCK, nx, Y - 1, nz, 1);
        g->soundCount = 0;
        use(0, nx, Y, nz, 1);
        const TileEntity* note = g->world.tileAt(nx, Y, nz);
        bool drum = false;
        for (int i = 0; i < g->soundCount; ++i) drum |= !std::strcmp(g->sounds[i].name, "note.bd");
        check(note && note->type == TILE_NOTE && note->burnTime == 1 && drum, "note block tunes and plays");

        // Dispenser shoots an arrow when powered.
        int ddx = px + 3, ddz = pz - 2;
        g->player.yaw = 90;  // facing -x: the dispenser faces +x (toward its front, away from the player)
        use(DISPENSER, ddx, Y - 1, ddz, 1);
        TileEntity* disp = g->world.tileAt(ddx, Y, ddz);
        check(g->world.get(ddx, Y, ddz) == DISPENSER && disp && disp->type == TILE_DISPENSER, "dispenser placed");
        if (disp) disp->items[4] = ItemStack(I_ARROW, 3);
        use(LEVER, ddx, Y - 1, ddz - 1, 1);
        for (Entity& e : g->ents) e = Entity();
        use(0, ddx, Y, ddz - 1, 1);
        ticks(6);
        int arrows = 0;
        for (const Entity& e : g->ents) arrows += e.type == ENT_ARROW;
        check(arrows == 1 && disp && disp->items[4].count == 2, "dispenser fires an arrow");

        // Stone pressure plate under the player powers a wire.
        int sx2 = px + 3, sz2 = pz + 2;
        use(STONE_PLATE, sx2, Y - 1, sz2, 1);
        use(I_REDSTONE, sx2 + 1, Y - 1, sz2, 1);
        g->player.flying = false;
        g->player.setPosition(sx2 + 0.5f, Y, sz2 + 0.5f);
        ticks(3);
        check(g->world.getMeta(sx2, Y, sz2) == 1 && g->world.getMeta(sx2 + 1, Y, sz2) == 15, "pressure plate powers wire");
        g->player.setPosition(px + 0.5f, Y + 3, pz + 6.5f);
        g->player.flying = true;
        ticks(25);
        check(g->world.getMeta(sx2, Y, sz2) == 0 && g->world.getMeta(sx2 + 1, Y, sz2) == 0, "pressure plate releases");

        // Rails: a straight line along +x with a detector rail and an unpowered powered rail, then a curve.
        for (Entity& e : g->ents) e = Entity();
        int lz = pz + 5, lx0 = px - 6;
        for (int x = lx0; x <= px + 1; ++x) {
            uint8_t id = x == px - 3 ? DETECTOR_RAIL : x == px ? POWERED_RAIL : RAIL;
            use(id, x, Y - 1, lz, 1);
        }
        check(g->world.get(lx0 + 1, Y, lz) == RAIL && g->world.getMeta(lx0 + 1, Y, lz) == 1 &&
                  g->world.getMeta(px - 3, Y, lz) == 1 && g->world.getMeta(px, Y, lz) == 1,
              "rails join into an east-west line");
        use(RAIL, px + 1, Y - 1, lz - 1, 1);
        check(g->world.getMeta(px + 1, Y, lz) == 8 && g->world.getMeta(px + 1, Y, lz - 1) == 0, "rail curves to a new neighbor");
        use(I_MINECART, lx0 + 1, Y, lz, 1);
        Entity* cart = nullptr;
        for (Entity& e : g->ents)
            if (e.type == ENT_MINECART) cart = &e;
        check(cart && cart->cartType == 0, "minecart placed on a rail");
        if (cart) {
            cart->mx = 0.4f;
            bool detected = false;
            float startX = cart->x;
            for (int i = 0; i < 60; ++i) {
                g->tick();
                detected |= (g->world.getMeta(px - 3, Y, lz) & 8) != 0;
            }
            std::printf("minecart rolled %.2f blocks, z %.3f, y %.3f\n", cart->x - startX, cart->z, cart->y);
            check(detected, "detector rail powers under a minecart");
            check(cart->x >= px && cart->x < px + 1 && std::fabs(cart->z - (lz + 0.5f)) < 0.01f &&
                      cart->mx * cart->mx + cart->mz * cart->mz < 1e-6f,
                  "minecart rolls along the rails and brakes on an unpowered powered rail");
            check(!(g->world.getMeta(px - 3, Y, lz) & 8), "detector rail turns off after the cart leaves");

            // Riding: use the cart to get in, use it again to get out.
            g->inv.held() = ItemStack();
            g->entityTarget = cart;
            g->usePressed = true;
            g->tick();
            int ci = (int)(cart - g->ents);
            check(g->riding == ci && cart->rider == RIDER_PLAYER && std::fabs(g->player.y - (cart->y - 0.45f)) < 0.01f,
                  "player rides the minecart");
            g->entityTarget = cart;
            g->usePressed = true;
            g->tick();
            check(g->riding == -1 && cart->rider == -1 && g->player.y >= cart->y + 0.69f, "player gets out on top");

            // Chest minecart contents survive the chunk unloading.
            cart->cartType = 1;
            cart->cartItems[3] = ItemStack(I_DIAMOND, 7);
            cart->cartItems[26] = ItemStack(COBBLESTONE, 64);
            for (int i = 0; i < 12; ++i) cart->cartItems[10 + i] = ItemStack(I_STICK, i + 1);
            float homeX = g->player.x, homeY = g->player.y, homeZ = g->player.z;
            g->player.flying = true;
            for (int step = 0; step < 20; ++step) {
                g->player.setPosition(homeX + step * 16.0f, 120.0f, homeZ);
                for (int k = 0; k < 40; ++k) g->stream(R, 0.05);
            }
            check(g->world.chunk(lx0 >> 4, lz >> 4) == nullptr, "minecart chunk unloaded");
            g->player.setPosition(homeX, homeY, homeZ);
            while (!g->spawnReady(R)) g->stream(R, 1.0);
            cart = nullptr;
            for (Entity& e : g->ents)
                if (e.type == ENT_MINECART) cart = &e;
            bool items = cart && cart->cartType == 1 && cart->cartItems[3].id == I_DIAMOND && cart->cartItems[3].count == 7 &&
                         cart->cartItems[26].count == 64 && cart->cartItems[21].id == I_STICK && cart->cartItems[21].count == 12;
            check(items, "chest minecart keeps its items through save/load");
            if (cart) {
                g->inv.held() = ItemStack();
                g->entityTarget = cart;
                g->usePressed = true;
                g->tick();
                check(g->openCart == (int)(cart - g->ents) && g->lastUse == UseResult::CartChest, "chest minecart opens");
                g->openCart = -1;
                g->lastUse = UseResult::None;
                // Breaking it drops the cart, the chest and the contents.
                g->mode = MODE_SURVIVAL;
                for (int i = 0; i < 6 && cart->type == ENT_MINECART; ++i) {
                    g->inv.held() = ItemStack();
                    g->entityTarget = cart->type == ENT_MINECART ? cart : nullptr;
                    g->attackPressed = true;
                    g->tick();
                }
                int carts = 0, chests = 0, diamonds = 0;
                for (const Entity& e : g->ents) {
                    if (e.type != ENT_ITEM) continue;
                    carts += e.item.id == I_MINECART, chests += e.item.id == CHEST;
                    if (e.item.id == I_DIAMOND) diamonds += e.item.count;
                }
                check(carts == 1 && chests == 1 && diamonds == 7, "broken chest minecart drops cart, chest and items");
                g->mode = MODE_CREATIVE;
            }
            g->player.flying = true;
        }

        // Saddle a pig and ride it.
        for (Entity& e : g->ents) e = Entity();
        Entity* hog = g->spawn(ENT_PIG, px + 3.5f, Y, pz + 5.5f);
        g->inv.held() = ItemStack(I_SADDLE, 1);
        g->entityTarget = hog;
        g->usePressed = true;
        g->tick();
        check(hog->saddled && g->inv.held().empty(), "saddle goes on a pig");
        g->entityTarget = hog;
        g->usePressed = true;
        g->tick();
        check(g->riding == (int)(hog - g->ents) && std::fabs(g->player.y - (hog->y + 0.175f)) < 0.01f, "player rides the pig");
        g->entityTarget = hog;
        g->usePressed = true;
        g->tick();
        check(g->riding == -1, "player gets off the pig");

        // Boat on a pool: it floats, carries the player and steers with the input.
        for (Entity& e : g->ents) e = Entity();
        int bx0 = px - 6, bz0 = pz + 6;
        for (int x = 0; x < 8; ++x)
            for (int z = 0; z < 2; ++z) {
                g->world.setBlock(bx0 + x, Y - 2, bz0 + z, STONE);
                g->world.setBlock(bx0 + x, Y - 1, bz0 + z, WATER);
            }
        Entity* boat = g->spawn(ENT_BOAT, bx0 + 1.5f, Y + 0.5f, bz0 + 1.0f);
        for (int i = 0; i < 60; ++i) g->tick();
        std::printf("boat floats at %.3f (water surface %d)\n", boat->y, Y);
        check(boat->type == ENT_BOAT && boat->y > Y - 1.0f && boat->y < Y, "boat floats on still water");
        g->inv.held() = ItemStack();
        g->entityTarget = boat;
        g->usePressed = true;
        g->tick();
        check(g->riding == (int)(boat - g->ents), "player boards the boat");
        float bx = boat->x;
        for (int i = 0; i < 40; ++i) {
            g->player.yaw = -90;  // keep facing +x while the boat turns the view
            g->player.forward = 1;
            g->tick();
        }
        g->player.forward = 0;
        std::printf("boat moved %.2f blocks\n", boat->x - bx);
        check(boat->x - bx > 1.0f && g->riding == (int)(boat - g->ents), "boat moves forward with the rider's input");
        g->player.descend = true;
        g->tick();
        g->player.descend = false;
        check(g->riding == -1, "sneaking leaves the boat");
        for (Entity& e : g->ents) e = Entity();

        // Recipes.
        ItemStack rg[9];
        rg[0] = rg[2] = ItemStack(PLANKS), rg[1] = ItemStack(PLANKS);
        rg[3] = ItemStack(COBBLESTONE), rg[4] = ItemStack(I_IRON_INGOT), rg[5] = ItemStack(COBBLESTONE);
        rg[6] = ItemStack(COBBLESTONE), rg[7] = ItemStack(I_REDSTONE), rg[8] = ItemStack(COBBLESTONE);
        check(craftResult(rg, 3).id == PISTON, "piston recipe");
        for (ItemStack& s : rg) s.clear();
        rg[0] = rg[2] = rg[3] = rg[5] = rg[6] = rg[8] = ItemStack(I_IRON_INGOT);
        rg[4] = ItemStack(I_STICK);
        check(craftResult(rg, 3).id == RAIL && craftResult(rg, 3).count == 16, "16 rails from iron and a stick");
        rg[0] = rg[2] = rg[3] = rg[5] = rg[6] = rg[8] = ItemStack(I_GOLD_INGOT);
        rg[7] = ItemStack(I_REDSTONE);
        check(craftResult(rg, 3).id == POWERED_RAIL && craftResult(rg, 3).count == 6, "6 powered rails");
        for (ItemStack& s : rg) s.clear();
        rg[3] = rg[5] = rg[6] = rg[7] = rg[8] = ItemStack(I_IRON_INGOT);
        check(craftResult(rg, 3).id == I_MINECART, "minecart recipe");
        rg[3] = rg[5] = rg[6] = rg[7] = rg[8] = ItemStack(PLANKS);
        check(craftResult(rg, 3).id == I_BOAT, "boat recipe");
        for (Entity& e : g->ents) e = Entity();
        g->player.flying = false;
        g->mode = MODE_SURVIVAL;
    }

    // Dungeons: walk across fresh terrain and inspect every spawner that gets generated.
    {
        int spawners = 0, bad = 0, chests = 0, loot = 0;
        static int seen[512][3];
        float sx = g->player.x;
        for (int step = 1; step <= 8; ++step) {
            g->player.setPosition(sx + step * 96.0f, 100, g->player.z);
            while (!g->spawnReady(R)) g->stream(R, 1.0);
            for (int i = 0; i < World::RING; ++i)
                for (int j = 0; j < World::RING; ++j) {
                    const Chunk* c = g->world.slot(i, j);
                    if (!c) continue;
                    for (const TileEntity* t = c->tiles; t; t = t->next) {
                        if (t->type != TILE_SPAWNER) continue;
                        bool dup = false;
                        for (int k = 0; k < spawners && k < 512; ++k)
                            dup |= seen[k][0] == t->x && seen[k][1] == t->y && seen[k][2] == t->z;
                        if (dup) continue;
                        if (spawners < 512) seen[spawners][0] = t->x, seen[spawners][1] = t->y, seen[spawners][2] = t->z;
                        ++spawners;
                        uint8_t below = g->world.get(t->x, t->y - 1, t->z);
                        if (g->world.get(t->x, t->y, t->z) != MOB_SPAWNER ||
                            (t->burnTotal != ENT_ZOMBIE && t->burnTotal != ENT_SKELETON && t->burnTotal != ENT_SPIDER) ||
                            (below != COBBLESTONE && below != MOSSY_COBBLESTONE && below != AIR))
                            ++bad;
                        for (int dx = -4; dx <= 4; ++dx)
                            for (int dz = -4; dz <= 4; ++dz)
                                if (const TileEntity* ch = g->world.tileAt(t->x + dx, t->y, t->z + dz))
                                    if (ch->type == TILE_CHEST) {
                                        ++chests;
                                        for (const ItemStack& it : ch->items) loot += !it.empty();
                                    }
                    }
                }
        }
        std::printf("dungeons: %d spawners (%d malformed), %d chests with %d stacks\n", spawners, bad, chests, loot);
        check(spawners > 0 && bad == 0 && chests > 0 && loot > 0, "dungeons generate with spawners and loot");
    }

    // The Nether: portal frame, travel, hell terrain, fireballs and zombie pigmen.
    {
        for (Entity& e : g->ents) e = Entity();
        g->mode = MODE_CREATIVE;
        g->player.flying = true;
        auto fl = [](float v) { return (int)std::floor(v); };
        int fx = fl(g->player.x) + 3, fz = fl(g->player.z), Y = 110;
        while (!g->spawnReady(R)) g->stream(R, 1.0);
        for (int i = -1; i <= 4; ++i)
            for (int j = -1; j <= 5; ++j)
                for (int k = -1; k <= 1; ++k) g->world.setBlock(fx + i, Y + j, fz + k, AIR);
        auto frame = [&](uint8_t id) {
            for (int i = 1; i <= 2; ++i) g->world.setBlock(fx + i, Y, fz, id), g->world.setBlock(fx + i, Y + 4, fz, id);
            for (int j = 1; j <= 3; ++j) g->world.setBlock(fx, Y + j, fz, id), g->world.setBlock(fx + 3, Y + j, fz, id);
        };
        auto portals = [&]() {
            int n = 0;
            for (int i = 1; i <= 2; ++i)
                for (int j = 1; j <= 3; ++j) n += g->world.get(fx + i, Y + j, fz) == PORTAL;
            return n;
        };
        frame(OBSIDIAN);
        g->setBlockNotify(fx + 1, Y + 1, fz, FIRE);
        check(portals() == 6, "fire in an obsidian frame makes a portal");
        g->setBlockNotify(fx + 3, Y + 2, fz, AIR);
        check(portals() == 0, "the portal collapses when the frame breaks");
        frame(OBSIDIAN);
        check(g->tryCreatePortal(fx + 2, Y + 1, fz) && portals() == 6, "portal relit");

        float ox = g->player.x, oz = g->player.z;
        g->player.setPosition(fx + 2.0f, Y + 1.0f, fz + 0.5f);
        g->player.mx = g->player.my = g->player.mz = 0;
        int ticks = 0;
        while (!g->portalPending && ticks < 200) g->tick(), ++ticks;
        std::printf("portal timer ran out after %d ticks\n", ticks);
        check(g->portalPending && ticks >= 75 && ticks <= 90, "standing in the portal for 4 s triggers travel");
        float px = g->player.x, pz = g->player.z;
        check(g->travelDimension() && g->dimension == -1 && g->world.dimension == -1, "travel to the Nether");
        check(std::fabs(g->player.x - px / 8) < 0.01f && std::fabs(g->player.z - pz / 8) < 0.01f,
              "position divided by 8");
        while (!g->spawnReady(R)) g->stream(R, 1.0);
        g->placeInPortal();
        int bx = fl(g->player.x), by = fl(g->player.y), bz = fl(g->player.z);
        std::printf("nether portal at %d %d %d\n", bx, by, bz);
        check(g->world.get(bx, by, bz) == PORTAL, "a portal is built in the Nether around the player");

        int counts[256] = {0};
        for (int x = bx - 48; x < bx + 48; ++x)
            for (int z = bz - 48; z < bz + 48; ++z)
                for (int y = 0; y < World::SY; ++y) ++counts[g->world.get(x, y, z)];
        std::printf("hell: netherrack %d, soul sand %d, gravel %d, glowstone %d, lava %d, bedrock %d, water %d, "
                    "stone %d\n",
                    counts[NETHERRACK], counts[SOUL_SAND], counts[GRAVEL], counts[GLOWSTONE],
                    counts[LAVA] + counts[LAVA_FLOWING], counts[BEDROCK], counts[WATER] + counts[WATER_FLOWING],
                    counts[STONE]);
        int soul = counts[SOUL_SAND];
        for (int step = 1; step <= 6 && !soul; ++step) {
            g->player.setPosition(bx + step * 96.0f, 100, bz + step * 48.0f);
            while (!g->spawnReady(R)) g->stream(R, 1.0);
            int x0 = fl(g->player.x), z0 = fl(g->player.z);
            for (int x = x0 - 48; x < x0 + 48; ++x)
                for (int z = z0 - 48; z < z0 + 48; ++z)
                    for (int y = 56; y < 68; ++y) soul += g->world.get(x, y, z) == SOUL_SAND;
        }
        std::printf("soul sand %d\n", soul);
        g->player.setPosition(bx + 0.5f, by + 0.0f, bz + 0.5f);
        while (!g->spawnReady(R)) g->stream(R, 1.0);
        check(counts[NETHERRACK] > 96 * 96 * 30 && soul > 0 && counts[BEDROCK] >= 96 * 96 * 2 &&
                  counts[LAVA] + counts[LAVA_FLOWING] > 0 && counts[WATER] + counts[WATER_FLOWING] == 0 &&
                  counts[STONE] == 0,
              "hell terrain generates");
        check(g->world.get(bx, 0, bz) == BEDROCK && g->world.get(bx, 127, bz) == BEDROCK, "bedrock floor and ceiling");

        g->closeWorld();
        check(g->loadWorld(dir) && g->dimension == -1, "the Nether is saved as the current dimension");
        while (!g->spawnReady(R)) g->stream(R, 1.0);

        // Fireball: dropped onto a netherrack floor in a cleared box, it explodes and leaves fire.
        int cx = bx + 12, cy = 64, cz = bz;
        for (int x = cx - 5; x <= cx + 5; ++x)
            for (int z = cz - 5; z <= cz + 5; ++z)
                for (int y = cy - 3; y <= cy + 8; ++y) g->world.setBlock(x, y, z, y < cy ? NETHERRACK : AIR);
        Entity* ball = g->spawn(ENT_FIREBALL, cx + 0.5f, cy + 4.0f, cz + 0.5f);
        ball->swimY = -0.1f;
        ball->target = -1;
        int life = 0;
        while (ball->type == ENT_FIREBALL && life < 200) g->tick(), ++life;
        int holes = 0, fires = 0;
        for (int x = cx - 5; x <= cx + 5; ++x)
            for (int z = cz - 5; z <= cz + 5; ++z) {
                holes += g->world.get(x, cy - 1, z) != NETHERRACK;
                for (int y = cy - 3; y <= cy + 2; ++y) fires += g->world.get(x, y, z) == FIRE;
            }
        std::printf("fireball hit after %d ticks: %d blocks gone, %d fires\n", life, holes, fires);
        check(ball->type != ENT_FIREBALL && holes > 0 && fires > 0, "fireball explodes with fire");

        // Hitting a fireball sends it along the player's look.
        g->player.setPosition(cx + 0.5f, cy + 0.0f, cz - 3.5f);
        g->player.yaw = 0, g->player.pitch = 0;  // facing +z
        ball = g->spawn(ENT_FIREBALL, cx + 0.5f, cy + 1.0f, cz + 0.5f);
        ball->swimZ = -0.1f;
        g->inv.held() = ItemStack();
        g->entityTarget = ball;
        g->attackPressed = true;
        g->tick();
        check(ball->swimZ > 0 && ball->mz > 0, "a punched fireball flies back");
        ball->type = ENT_NONE;

        // Hitting one zombie pigman angers the ones around it.
        Entity* a = g->spawn(ENT_PIGZOMBIE, cx + 0.5f, cy, cz + 2.5f);
        Entity* b = g->spawn(ENT_PIGZOMBIE, cx + 3.5f, cy, cz + 2.5f);
        check(!a->angry && !b->angry, "zombie pigmen start neutral");
        g->entityTarget = a;
        g->attackPressed = true;
        g->tick();
        check(a->angry && b->angry, "hitting a zombie pigman angers the group");
        for (Entity& e : g->ents) e = Entity();

        // A ghast that sees the player charges up and shoots.
        for (int x = cx - 12; x <= cx + 12; ++x)
            for (int z = cz - 12; z <= cz + 12; ++z)
                for (int y = cy; y <= cy + 10; ++y) g->world.setBlock(x, y, z, AIR);
        g->mode = MODE_SURVIVAL;
        g->player.flying = false;
        g->player.setPosition(cx + 0.5f, cy + 0.0f, cz - 8.5f);
        Entity* ghast = g->spawn(ENT_GHAST, cx + 0.5f, cy + 3.0f, cz + 4.5f);
        int shots = 0, t = 0;
        for (; t < 200 && !shots; ++t) {
            g->health = Game::MAX_HEALTH;
            g->tick();
            for (const Entity& e : g->ents) shots += e.type == ENT_FIREBALL;
        }
        std::printf("ghast fired after %d ticks (fuse %d)\n", t, ghast->fuse);
        check(shots > 0, "a ghast shoots fireballs at the player");
        for (Entity& e : g->ents) e = Entity();
        g->mode = MODE_CREATIVE;
        g->player.flying = true;

        g->player.setPosition(bx + 0.5f, by + 0.0f, bz + 0.5f);
        float npx = g->player.x, npz = g->player.z;
        check(g->travelDimension() && g->dimension == 0, "travel back to the overworld");
        check(std::fabs(g->player.x - npx * 8) < 0.01f && std::fabs(g->player.z - npz * 8) < 0.01f,
              "position multiplied by 8");
        // Start 100 blocks from the original portal, outside the loaded area: the remembered portal is used.
        g->player.setPosition(fx + 100.5f, Y, fz + 0.5f);
        while (!g->spawnReady(R)) g->stream(R, 1.0);
        int rounds = 1;
        while (!g->placeInPortal() && rounds < 5) {
            ++rounds;
            while (!g->spawnReady(R)) g->stream(R, 1.0);
        }
        std::printf("back at %.1f %.1f %.1f after %d searches (portal at %d %d %d)\n", g->player.x, g->player.y,
                    g->player.z, rounds, fx + 1, Y + 1, fz);
        check(g->world.get(fl(g->player.x), fl(g->player.y), fl(g->player.z)) == PORTAL && fl(g->player.x) >= fx + 1 &&
                  fl(g->player.x) <= fx + 2 && fl(g->player.z) == fz && rounds == 2,
              "the player returns to the remembered overworld portal");
        g->player.setPosition(ox, 100, oz);
        g->player.flying = false;
        g->mode = MODE_SURVIVAL;
    }

    // Weather: rain, lightning, snow and ice, particles, and level.dat.
    {
        auto fl = [](float v) { return (int)std::floor(v); };
        g->mode = MODE_CREATIVE;
        g->player.flying = true;
        while (!g->spawnReady(R)) g->stream(R, 1.0);
        // A chunk-sized stone pad open to the sky, next to the player's chunk.
        int x0 = ((fl(g->player.x) >> 4) + 1) * 16, z0 = (fl(g->player.z) >> 4) * 16;
        for (int x = x0; x < x0 + 16; ++x)
            for (int z = z0; z < z0 + 16; ++z) {
                g->world.setBlock(x, 99, z, STONE);
                for (int y = 100; y < World::SY; ++y) g->world.setBlock(x, y, z, AIR);
            }
        for (int x = x0; x < x0 + 16; ++x)
            for (int z = z0; z < z0 + 4; ++z) g->world.setBlock(x, 99, z, WATER);
        Chunk* pad = g->world.chunk(x0 >> 4, z0 >> 4);
        std::memset(pad->temperature, 200, sizeof(pad->temperature));
        std::memset(pad->humidity, 200, sizeof(pad->humidity));
        int cx = x0 + 8, cz = z0 + 10;
        check(biomeRains(g->biomeAt(cx, cz)) && !biomeSnows(g->biomeAt(cx, cz)), "warm, wet climate rains");

        g->raining = true;
        g->rainTime = 20000;
        for (int i = 0; i < 120; ++i) g->tick();
        check(g->rainStrength(1.0f) == 1.0f && g->isRaining() && !g->isThundering(), "rain fades in over 100 ticks");
        check(g->rainingAt(cx, 100, cz), "rain falls on the open pad");
        g->world.setBlock(cx + 2, 104, cz, STONE);
        check(!g->rainingAt(cx + 2, 100, cz), "no rain under a roof");
        g->world.setBlock(cx + 2, 104, cz, AIR);

        // Block break particles: 64 pieces that fall and fade.
        g->particles.clear();
        g->particles.level = 2;
        g->particles.viewX = cx + 0.5f, g->particles.viewY = 101, g->particles.viewZ = cz + 0.5f;
        g->particles.blockDestroy(g->world, cx, 100, cz, GRASS, 0);
        int spawned = g->particles.count, fallen = 0;
        for (int i = 0; i < 10; ++i) g->particles.tick(g->world);
        for (int i = 0; i < g->particles.count; ++i) fallen += g->particles.p[i].y < 100.6f;
        for (int i = 0; i < 100; ++i) g->particles.tick(g->world);
        std::printf("particles: %d spawned, %d fell, %d left after 110 ticks\n", spawned, fallen, g->particles.count);
        check(spawned == 64 && fallen > 0 && g->particles.count == 0, "block pieces fly, fall and expire");
        g->particles.blockDestroy(g->world, cx, 100, cz, STONE, 0);
        g->particles.viewX = cx + 100.0f;
        g->particles.spawn(g->world, PART_SMOKE, cx, 100, cz, 0, 0, 0);
        check(g->particles.count == 64, "minimal particles keep block pieces only, none beyond 16 blocks");
        g->particles.level = 0;
        g->particles.clear();

        // Lightning turns a pig into a zombie pigman and sets fire.
        for (Entity& e : g->ents) e = Entity();
        Entity* pig = g->spawn(ENT_PIG, cx + 0.5f, 100, cz + 0.5f);
        g->world.setBlock(cx, 99, cz + 1, PLANKS);
        g->strikeLightning((float)cx, 100, (float)cz);
        int fires = 0;
        for (int x = cx - 2; x <= cx + 2; ++x)
            for (int z = cz - 2; z <= cz + 2; ++z) fires += g->world.get(x, 100, z) == FIRE;
        for (int i = 0; i < 3; ++i) g->tick();
        int pigs = 0, pigmen = 0;
        for (const Entity& e : g->ents) pigs += e.type == ENT_PIG, pigmen += e.type == ENT_PIGZOMBIE;
        std::printf("lightning: %d fires, %d pigs, %d zombie pigmen, flash %d\n", fires, pigs, pigmen, g->lightningFlash);
        check(fires > 0 && pigs == 0 && pigmen == 1 && pig->type != ENT_PIG, "lightning strikes: fire, pig -> pigman");
        for (Entity& e : g->ents) e = Entity();
        for (int x = cx - 3; x <= cx + 3; ++x)
            for (int z = cz - 3; z <= cz + 3; ++z)
                if (g->world.get(x, 100, z) == FIRE) g->world.setBlock(x, 100, z, AIR);

        // Turned into tundra: snow covers the stone and still water freezes.
        std::memset(pad->temperature, 0, sizeof(pad->temperature));
        check(biomeSnows(g->biomeAt(cx, cz)) && !g->rainingAt(cx, 100, cz), "cold climate snows instead of raining");
        for (int i = 0; i < 4000; ++i) g->tick();
        int snow = 0, ice = 0;
        for (int x = x0; x < x0 + 16; ++x)
            for (int z = z0; z < z0 + 16; ++z) {
                snow += g->world.get(x, 100, z) == SNOW_LAYER;
                ice += g->world.get(x, 99, z) == ICE;
            }
        std::printf("tundra after 4000 ticks: %d snow layers, %d ice\n", snow, ice);
        check(snow > 20 && ice > 5, "snow piles up and water freezes");

        // Thunder darkens the sky enough for monsters.
        g->thundering = true;
        g->thunderTime = 20000;
        for (int i = 0; i < 110; ++i) g->tick();
        g->world.time = 6000;
        check(g->isThundering() && g->computeSkySubtracted() >= 5, "a thunderstorm darkens the day");

        // level.dat keeps the weather, the name and the date.
        std::snprintf(g->worldName, sizeof(g->worldName), "Rainy");
        g->lastPlayed = 1234567890;
        int rainTime = g->rainTime;
        g->closeWorld();
        Game::LevelInfo info;
        check(Game::readLevelInfo(dir, info) && !std::strcmp(info.name, "Rainy") && info.lastPlayed == 1234567890 &&
                  info.seed == seed,
              "world list reads name, date and seed");
        check(Game::renameLevel(dir, "Renamed") && Game::readLevelInfo(dir, info) && !std::strcmp(info.name, "Renamed"),
              "rename a world");
        check(g->loadWorld(dir), "reload after rename");
        check(!std::strcmp(g->worldName, "Renamed") && g->raining && g->thundering && g->rain == 1.0f &&
                  g->thunder == 1.0f && std::abs(g->rainTime - rainTime) <= 1,
              "weather survives saving");
        g->clearWeather();
    }

    g->closeWorld();
    {
        std::system("rm -rf test_world_del");
        Game* d = new Game();
        check(d->init() && d->createWorld("test_world_del", 42, MODE_CREATIVE), "second world slot");
        while (!d->world.readyCount(0.5f, 0.5f, 1)) d->stream(1, 1.0);
        d->closeWorld();
        check(Game::levelExists("test_world_del"), "second slot saved");
        Game::deleteLevel("test_world_del");
        Game::LevelInfo info;
        check(!Game::levelExists("test_world_del") && !Game::readLevelInfo("test_world_del", info), "delete a world");
        delete d;
    }

    // Sound bank and mixer (needs tools/convert_sounds.py output in dist/).
    const char* DIST = "../dist/PSP/GAME/MinecraftBeta/";
    char path[256];
    std::snprintf(path, sizeof(path), "%ssounds.pak", DIST);
    if (audio::loadBank(path)) {
        int count;
        int first = audio::findGroup("step.stone", count);
        check(count == 4, "four stone step variants");
        int missing = 0;
        static const char* used[] = {"step.grass", "step.gravel", "step.wood", "step.cloth", "step.sand", "random.glass",
                                     "random.pop", "random.explode", "random.fuse", "random.hurt", "random.bow",
                                     "random.drr", "random.splash", "random.fizz", "random.click", "random.door_open",
                                     "random.door_close", "fire.fire", "fire.ignite", "liquid.water", "ambient.cave.cave",
                                     "mob.zombie", "mob.zombiehurt", "mob.zombiedeath", "mob.skeleton",
                                     "mob.skeletonhurt", "mob.creeper", "mob.creeperdeath", "mob.spider",
                                     "mob.spiderdeath", "mob.pig", "mob.pigdeath", "mob.cow", "mob.cowhurt",
                                     "mob.sheep", "mob.chicken", "mob.chickenhurt", "mob.chickenplop", "note.bd",
                                     "note.harp", "note.snare", "note.hat", "note.bassattack", "tile.piston.in",
                                     "tile.piston.out"};
        for (const char* n : used) {
            int c;
            audio::findGroup(n, c);
            if (!c) std::printf("  missing sound %s\n", n), ++missing;
        }
        check(missing == 0, "all sounds the game plays are in the bank");
        static int16_t out[44100 * 2];
        audio::play(first, 1.0f, 1.0f, 1.0f);
        audio::mix(out, 44100);
        int last = 0;
        for (int i = 0; i < 44100; ++i)
            if (out[i * 2]) last = i;
        FILE* f = std::fopen("step_stone1.raw", "wb");
        for (int i = 0; i < 44100; i += 2) std::fwrite(&out[i * 2], 2, 1, f);
        std::fclose(f);
        std::printf("step.stone: %d frames of output (%.2f s)\n", last, last / 44100.0);
        check(last > 2000 && last < 44000, "effect plays and ends");
        std::snprintf(path, sizeof(path), "%smusic/calm1.wav", DIST);
        audio::playMusic(path, 1.0f);
        double rms = 0;
        for (int k = 0; k < 4; ++k) {
            audio::mix(out, 44100);
            if (k < 2) continue;
            for (int i = 0; i < 44100 * 2; ++i) rms += (double)out[i] * out[i];
        }
        rms = std::sqrt(rms / (44100.0 * 4));
        std::printf("music: rms %.0f after 2-4 s, active %d\n", rms, audio::musicActive() ? 1 : 0);
        check(rms > 10 && audio::musicActive(), "music streams");  // calm1 opens quietly (ffmpeg: rms 22)
        audio::stopMusic();
        audio::mix(out, 64);
        check(!audio::musicActive(), "music stops");
        audio::freeBank();
    } else {
        std::printf("(sounds.pak not found, skipping audio checks)\n");
    }

    std::printf("%s\n", fails ? "SOME CHECKS FAILED" : "all checks passed");
    return fails ? 1 : 0;
}
