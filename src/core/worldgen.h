#pragma once
#include "noise.h"
#include "world.h"

enum Biome : uint8_t {
    BIOME_RAINFOREST,
    BIOME_SWAMPLAND,
    BIOME_SEASONAL_FOREST,
    BIOME_FOREST,
    BIOME_SAVANNA,
    BIOME_SHRUBLAND,
    BIOME_TAIGA,
    BIOME_DESERT,
    BIOME_PLAINS,
    BIOME_ICE_DESERT,
    BIOME_TUNDRA,
    BIOME_COUNT
};

const char* biomeName(int biome);
int biomeFromClimate(float temperature, float humidity);
// BlockSapling.growTree: type 1 spruce, 2 birch, otherwise oak.
bool growSapling(World& w, JRandom& r, int x, int y, int z, int type);

// WorldChunkManager
class BiomeSource {
public:
    explicit BiomeSource(int64_t seed);
    void load(int x, int z, int w, int h);
    int biomeAt(int x, int z) const;
    void temperatures(real* out, int x, int z, int w, int h) const;

    real temperature[256];
    real humidity[256];
    uint8_t biomes[256];

private:
    void loadInto(real* t, real* hm, real* o, uint8_t* b, int x, int z, int w, int h) const;
    SimplexOctaves tNoise_, hNoise_, oNoise_;
};

// ChunkProviderGenerate (or ChunkProviderHell for the Nether) for one chunk at a time. Not thread-safe:
// use one instance per thread.
class WorldGenerator {
public:
    explicit WorldGenerator(int64_t seed, bool hell = false);
    ~WorldGenerator();
    // Fills terrain, caves and climate; sections are allocated as needed, light is left at 0.
    bool provide(Chunk& c);
    // Decorates the area from (cx*16+8, cz*16+8) to 16 blocks beyond; touches chunks cx..cx+1, cz..cz+1.
    void populate(World& w, int cx, int cz);
    int biomeAt(int x, int z) const;

private:
    struct Impl;
    Impl* impl_;
};
