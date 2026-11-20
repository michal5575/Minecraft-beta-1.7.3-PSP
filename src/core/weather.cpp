// Rain, snow and thunderstorms (World.updateWeather, the weather parts of World.updateBlocks,
// EntityLightningBolt and EntityRenderer.addRainParticles).
#include <cmath>

#include "game.h"
#include "mathhelper.h"
#include "worldgen.h"

namespace {

inline int ifloor(float v) {
    int i = (int)v;
    return v < (float)i ? i - 1 : i;
}

}  // namespace

// BiomeGenBase.enableSnow: Taiga, Tundra and Ice Desert.
bool biomeSnows(int biome) { return biome == BIOME_TAIGA || biome == BIOME_TUNDRA || biome == BIOME_ICE_DESERT; }

// BiomeGenBase.canSpawnLightningBolt: snowy biomes and the desert get no rain.
bool biomeRains(int biome) { return biome >= 0 && !biomeSnows(biome) && biome != BIOME_DESERT; }

float Game::rainStrength(float partial) const { return prevRain + (rain - prevRain) * partial; }

float Game::thunderStrength(float partial) const {
    return (prevThunder + (thunder - prevThunder) * partial) * rainStrength(partial);
}

// World.canLightningStrikeAt, also used for "is this block rained on".
bool Game::rainingAt(int x, int y, int z) const {
    if (!isRaining()) return false;
    if (y < world.heightAt(x, z)) return false;
    if (world.topSolidAt(x, z) > y) return false;
    return biomeRains(biomeAt(x, z));
}

void Game::clearWeather() {
    rainTime = thunderTime = 0;
    raining = thundering = false;
}

// World.updateWeather
void Game::tickWeather() {
    if (lightningFlash > 0) --lightningFlash;
    if (dimension < 0) return;
    if (thunderTime <= 0) {
        thunderTime = thundering ? rand.nextInt(12000) + 3600 : rand.nextInt(168000) + 12000;
    } else if (--thunderTime <= 0) {
        thundering = !thundering;
    }
    if (rainTime <= 0) {
        rainTime = raining ? rand.nextInt(12000) + 12000 : rand.nextInt(168000) + 12000;
    } else if (--rainTime <= 0) {
        raining = !raining;
    }
    prevRain = rain;
    rain += raining ? 0.01f : -0.01f;
    rain = rain < 0 ? 0 : rain > 1 ? 1 : rain;
    prevThunder = thunder;
    thunder += thundering ? 0.01f : -0.01f;
    thunder = thunder < 0 ? 0 : thunder > 1 ? 1 : thunder;
}

// World.updateBlocks: lightning, snow cover and ice for one chunk.
void Game::weatherChunk(Chunk* c) {
    int bx = c->cx * 16, bz = c->cz * 16;
    if (rand.nextInt(16) == 0) {
        int x = bx + rand.nextInt(16), z = bz + rand.nextInt(16);
        int y = world.topSolidAt(x, z);
        if (biomeSnows(biomeAt(x, z)) && y >= 1 && y < World::SY && (world.getLight(x, y, z) & 15) < 10) {
            uint8_t below = world.get(x, y - 1, z), at = world.get(x, y, z);
            if (isRaining() && at == AIR && below != ICE && blockMatSolid(below) &&
                blockCanStay(x, y, z, SNOW_LAYER, 0))
                setBlockNotify(x, y, z, SNOW_LAYER);
            if (below == WATER && world.getMeta(x, y - 1, z) == 0) setBlockNotify(x, y - 1, z, ICE);
        }
    }
}

// Lightning picks a chunk anywhere within 9 chunks of the player like Beta's updateBlocks walk (one roll
// per tick at the combined odds); strikes in chunks that are not loaded are only seen and heard.
void Game::rollLightning() {
    if (!isRaining() || !isThundering()) return;
    if (rand.nextInt(277) != 0) return;
    int pcx = ifloor(player.x) >> 4, pcz = ifloor(player.z) >> 4;
    int cx = pcx + rand.nextInt(19) - 9, cz = pcz + rand.nextInt(19) - 9;
    int x = cx * 16 + rand.nextInt(16), z = cz * 16 + rand.nextInt(16);
    if (!world.isLoaded(x, z)) {
        lightningFlash = 2;
        playSound("ambient.weather.thunder", x + 0.5f, player.y, z + 0.5f, 10000.0f, 0.8f + rand.nextFloat() * 0.2f);
        return;
    }
    int y = world.topSolidAt(x, z);
    if (rainingAt(x, y, z)) strikeLightning((float)x, (float)y, (float)z);
}

void Game::lightningFire(int x, int y, int z) {
    if (world.get(x, y, z) == AIR && fireCanStay(x, y, z)) setBlockNotify(x, y, z, FIRE);
}

// EntityLightningBolt constructor.
void Game::strikeLightning(float x, float y, float z) {
    if (boltCount >= MAX_BOLTS) return;
    Bolt& b = bolts[boltCount++];
    b.x = x, b.y = y, b.z = z;
    b.life = 2;
    b.seed = rand.nextLong();
    b.flashes = rand.nextInt(3) + 1;
    int ix = ifloor(x), iy = ifloor(y), iz = ifloor(z);
    if (world.areaLoaded(ix, iz, 10)) {
        lightningFire(ix, iy, iz);
        for (int i = 0; i < 4; ++i)
            lightningFire(ix + rand.nextInt(3) - 1, iy + rand.nextInt(3) - 1, iz + rand.nextInt(3) - 1);
    }
}

// EntityLightningBolt.onUpdate
void Game::tickBolts() {
    for (int i = 0; i < boltCount;) {
        Bolt& b = bolts[i];
        if (b.life == 2) {
            playSound("ambient.weather.thunder", b.x, b.y, b.z, 10000.0f, 0.8f + rand.nextFloat() * 0.2f);
            playSound("random.explode", b.x, b.y, b.z, 2.0f, 0.5f + rand.nextFloat() * 0.2f);
        }
        --b.life;
        if (b.life < 0) {
            if (b.flashes == 0) {
                bolts[i] = bolts[--boltCount];
                continue;
            }
            if (b.life < -rand.nextInt(10)) {
                --b.flashes;
                b.life = 1;
                b.seed = rand.nextLong();
                int ix = ifloor(b.x), iy = ifloor(b.y), iz = ifloor(b.z);
                if (world.areaLoaded(ix, iz, 10)) lightningFire(ix, iy, iz);
            }
        }
        if (b.life >= 0) {
            AABB area = {b.x - 3, b.y - 3, b.z - 3, b.x + 3, b.y + 9, b.z + 3};
            for (int k = 0; k < MAX_ENTITIES; ++k) {
                Entity& e = ents[k];
                if (e.type == ENT_NONE || e.dead || !e.box().intersects(area)) continue;
                struckByLightning(e);
            }
            if (!dead && player.box().intersects(area)) {
                // Entity.onStruckByLightning: 5 fire damage. The player's fire resistance of 20 ticks
                // keeps a single bolt from setting it alight.
                if (fire > 0) ++fire;
                if (hurtPlayer(5, b.x, b.z, false)) deathMessage = "was struck by lightning";
            }
            lightningFlash = 2;
        }
        ++i;
    }
}

void Game::struckByLightning(Entity& e) {
    if (!entityIsMob(e.type) || e.health <= 0) return;
    if (e.type == ENT_PIG) {
        // EntityPig.onStruckByLightning
        Entity* z = spawn(ENT_PIGZOMBIE, e.x, e.y, e.z);
        if (z) z->yaw = z->prevYaw = e.yaw;
        removeMob(e);
        return;
    }
    if (e.type == ENT_CREEPER) e.powered = true;
    if (e.type != ENT_GHAST && e.type != ENT_PIGZOMBIE) {
        hurtEntity(e, 5, e.x, e.z, false);
        e.fire = e.fire <= 0 ? 300 : e.fire + 1;
    }
}

// EntityRenderer.addRainParticles: splashes on the ground around the player, smoke over lava, and the
// rain sound from one of the splash points.
void Game::rainEffects() {
    float r = rainStrength(1.0f);
    if (!fancyWeather) r /= 2.0f;
    if (r <= 0 || dimension < 0) return;
    JRandom rr((int64_t)rainTicks_ * 312987231LL);
    ++rainTicks_;
    int px = ifloor(player.x), py = ifloor(player.y), pz = ifloor(player.z);
    const int R = 10;
    float sx = 0, sy = 0, sz = 0;
    int splashes = 0;
    int n = (int)(100.0f * r * r);
    for (int i = 0; i < n; ++i) {
        int x = px + rr.nextInt(R) - rr.nextInt(R), z = pz + rr.nextInt(R) - rr.nextInt(R);
        int y = world.topSolidAt(x, z);
        uint8_t below = world.get(x, y - 1, z);
        if (y > py + R || y < py - R || !biomeRains(biomeAt(x, z))) continue;
        float fx = rr.nextFloat(), fz = rr.nextFloat();
        if (below == AIR) continue;
        AABB box;
        float top = blockShapeBox(below, world.getMeta(x, y - 1, z), box) ? box.y0 : 0.0f;
        float yy = y + 0.1f - top;
        if (blockIsLava(below)) {
            particles.spawn(world, PART_SMOKE, x + fx, yy, z + fz, 0, 0, 0);
        } else {
            ++splashes;
            if (rr.nextInt(splashes) == 0) sx = x + fx, sy = yy, sz = z + fz;
            particles.spawn(world, PART_RAIN, x + fx, yy, z + fz, 0, 0, 0);
        }
    }
    if (splashes > 0 && rr.nextInt(3) < rainSoundTimer_++) {
        rainSoundTimer_ = 0;
        if (sy > player.y + 1.0f && world.topSolidAt(px, pz) > py)
            playSound("ambient.weather.rain", sx, sy, sz, 0.1f, 0.5f);
        else
            playSound("ambient.weather.rain", sx, sy, sz, 0.2f, 1.0f);
    }
}
