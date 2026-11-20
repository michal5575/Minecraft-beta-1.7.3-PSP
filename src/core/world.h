#pragma once
#include <cstdint>

#include "blocks.h"
#include "config.h"
#include "items.h"

typedef void (*ProgressFn)(void* user, const char* title, const char* stage, float progress);

// 16x16x16 blocks; index (x << 8) | (z << 4) | y so a column is contiguous.
struct Section {
    uint8_t blocks[4096];
    uint8_t meta[2048];
    uint8_t light[4096];  // high nibble sky light, low nibble block light
};
inline int sectionIndex(int x, int y, int z) { return (x << 8) | (z << 4) | y; }

enum ChunkFlags : uint8_t {
    CHUNK_POPULATED = 1,
    CHUNK_LIT = 2,
    CHUNK_NEEDS_SAVE = 4,
};

enum TileType : uint8_t { TILE_CHEST, TILE_FURNACE, TILE_SPAWNER, TILE_SIGN, TILE_JUKEBOX, TILE_NOTE, TILE_DISPENSER,
                          TILE_PISTON };

struct TileEntity {
    int x, y, z;
    uint8_t type;
    ItemStack items[27];  // chest: 27 slots; furnace: 0 input, 1 fuel, 2 output; jukebox: 0 record; dispenser: 9
    // spawner: delay, entity type, unused; note block: note, unused, powered;
    // piston: see the piston helpers below, moved block id, moved block metadata
    int16_t burnTime, burnTotal, cookTime;
    float spin, prevSpin;                   // spawner mob rotation (not saved)
    char text[4][16];                       // sign lines, at most 15 characters each
    TileEntity* next;
};

// TileEntityPiston state packed into burnTime: facing (3 bits), extending, source (the retracting piston base),
// then progress and last tick's progress in half steps (0..2).
inline int pistonFacing(const TileEntity& t) { return t.burnTime & 7; }
inline bool pistonExtending(const TileEntity& t) { return (t.burnTime & 8) != 0; }
inline bool pistonSource(const TileEntity& t) { return (t.burnTime & 16) != 0; }
inline int pistonSteps(const TileEntity& t) { return (t.burnTime >> 5) & 3; }
inline int pistonLastSteps(const TileEntity& t) { return (t.burnTime >> 7) & 3; }
inline void setPistonSteps(TileEntity& t, int last, int now) {
    t.burnTime = (int16_t)((t.burnTime & 31) | (now << 5) | (last << 7));
}

struct Chunk {
    int cx, cz;
    uint8_t flags;
    uint8_t dirty;  // per section: mesh must be rebuilt
    // Not saved. Low 4 bits are which neighbors were loaded last time this chunk was meshed
    // (+x, -x, +z, -z). The high bit is set once the chunk has been considered.
    uint8_t neighborMask;
    Section* sec[8];  // null: all air, full sky light
    uint8_t temperature[256];  // [x * 16 + z], 0..255
    uint8_t humidity[256];
    TileEntity* tiles;
    uint8_t* entityData;  // serialized entities between the game and the save file
    uint32_t entityBytes;
    void* render;  // owned by the renderer
};

// World.scheduleBlockUpdate entry. Like Beta, pending updates are not saved.
struct ScheduledTick {
    int32_t x, z;
    int16_t y;
    uint8_t id;
    int64_t due;
};

// Chunk streaming state shared with the I/O thread.
class ChunkStreamer;

class World {
public:
    static constexpr int SY = 128;
    static constexpr int SECTIONS_Y = 8;
    static constexpr int RING = 16;  // loaded chunks live in a RING x RING table indexed by (cx & 15, cz & 15)
    static constexpr int MAX_LOAD_RADIUS = 6;

    World();
    ~World();
    bool init();  // allocates the lighting queue; once at startup

    // Opens a world directory (creating it if needed); chunks are generated or loaded on demand.
    // Dimension -1 is the Nether.
    bool open(const char* dir, int64_t seed, int dimension = 0);
    // Saves every modified chunk and frees everything.
    void close();
    bool isOpen() const { return streamer_ != nullptr; }
    const char* dir() const { return dir_; }

    // Streams chunks around (px, pz): loads, populates and lights, spending at most `budget` seconds
    // on generation work on this thread.
    void update(float px, float pz, int loadRadius, double budget);
    // Unused malloc space, in KB. Zero leaves the budget off (host tests).
    // Below a couple of megabytes the outer chunks are dropped and new loads wait.
    void setMemoryBudget(int freeKb) { memoryBudgetKb_ = freeKb < 0 ? 0 : freeKb; }
    // Spread chunk lighting across update() calls. The loading screen sleeps between slices.
    // Playing leaves this off, so a chunk is still lit in one call.
    void setLightSlice(bool on);
    // Number of chunks within `radius` of (px, pz) that are lit and have loaded neighbors.
    int readyCount(float px, float pz, int radius) const;
    bool chunkRenderable(const Chunk* c) const;

    inline Chunk* chunk(int cx, int cz) const {
        Chunk* c = ring_[cx & (RING - 1)][cz & (RING - 1)];
        return c && c->cx == cx && c->cz == cz ? c : nullptr;
    }
    Chunk* slot(int i, int j) const { return ring_[i][j]; }
    bool isLoaded(int x, int z) const { return chunk(x >> 4, z >> 4) != nullptr; }

    inline uint8_t get(int x, int y, int z) const {
        if ((unsigned)y >= (unsigned)SY) return y < 0 ? BEDROCK : AIR;
        const Chunk* c = chunk(x >> 4, z >> 4);
        // A missing chunk is air. Bedrock here was an invisible wall: the crosshair locked onto it
        // and the mesher hid the real face on the near side of the hole.
        if (!c) return AIR;
        const Section* s = c->sec[y >> 4];
        return s ? s->blocks[sectionIndex(x & 15, y & 15, z & 15)] : AIR;
    }
    inline int getMeta(int x, int y, int z) const {
        if ((unsigned)y >= (unsigned)SY) return 0;
        const Chunk* c = chunk(x >> 4, z >> 4);
        if (!c) return 0;
        const Section* s = c->sec[y >> 4];
        if (!s) return 0;
        int i = sectionIndex(x & 15, y & 15, z & 15);
        uint8_t b = s->meta[i >> 1];
        return (i & 1) ? (b >> 4) : (b & 15);
    }
    inline uint8_t getLight(int x, int y, int z) const {
        if ((unsigned)y >= (unsigned)SY) return y < 0 ? 0 : 0xF0;
        const Chunk* c = chunk(x >> 4, z >> 4);
        if (!c) return 0xF0;
        const Section* s = c->sec[y >> 4];
        return s ? s->light[sectionIndex(x & 15, y & 15, z & 15)] : 0xF0;
    }
    // Light level after the time-of-day sky subtraction (World.getBlockLightValue).
    inline int getBrightness(int x, int y, int z) const {
        uint8_t l = getLight(x, y, z);
        int s = (l >> 4) - skySubtracted, b = l & 15;
        return s > b ? s : b;
    }
    inline int getSkyLight(int x, int y, int z) const { return getLight(x, y, z) >> 4; }

    // Raw edit (world generation, batch edits); marks the chunk for saving.
    void setRaw(int x, int y, int z, uint8_t id, int m = 0);
    void setMetaRaw(int x, int y, int z, int m);
    // Gameplay edit: updates lighting and marks render sections dirty.
    void setBlock(int x, int y, int z, uint8_t id, int m = 0);
    void setMeta(int x, int y, int z, int m);
    // Batch editing: setRaw + markBlockDirty, then call relight() once per column.
    void markBlockDirty(int x, int y, int z);
    void relight(int x, int z);
    void markAllDirty();
    uint32_t lightUpdates = 0, relightCalls = 0, relightVoxels = 0;  // statistics

    int heightAt(int x, int z) const;  // first y above the top light-blocking block
    int topSolidAt(int x, int z) const;
    int biomeTemperature(int x, int z) const;  // 0..255

    TileEntity* tileAt(int x, int y, int z) const;
    TileEntity* createTile(int x, int y, int z, uint8_t type);
    void removeTile(int x, int y, int z);

    // Called on the main thread when a chunk enters or leaves the world (entities, renderer data).
    void (*onChunkLoaded)(World&, Chunk*, void*) = nullptr;
    void (*onChunkUnload)(World&, Chunk*, void*) = nullptr;
    void (*onChunkPopulated)(World&, Chunk*, void*) = nullptr;
    void* hookUser = nullptr;

    int64_t seed = 0;
    int dimension = 0;
    int64_t time = 0;  // world ticks
    int skySubtracted = 0;
    bool outOfMemory = false;
    int loadedChunks = 0;
    int workerBacklog() const;
    int workerInFlight() const;

    // Queues a block update `delay` ticks from now (deduplicated by position and block id); dropped if
    // the area within 8 blocks is not loaded.
    void scheduleTick(int x, int y, int z, uint8_t id, int delay);
    // Moves up to `max` updates that are due into `out`.
    int takeDueTicks(ScheduledTick* out, int max);
    int scheduledCount() const { return schedCount_; }
    bool areaLoaded(int x, int z, int r) const {
        return isLoaded(x - r, z - r) && isLoaded(x + r, z - r) && isLoaded(x - r, z + r) && isLoaded(x + r, z + r);
    }

    // Writes up to `maxChunks` modified chunks through the save thread.
    // Returns false once a full sweep over the loaded chunks is complete.
    // While a sweep is running, new terrain loads are held back so a moving boat does not
    // generate chunks at the same time the save is allocating.
    bool autosaveStep(int maxChunks);

private:
    friend class ChunkStreamer;
    Section* sectionFor(Chunk* c, int sy);
    void lightChunk(Chunk* c);
    void sliceLight(Chunk* c, double t0, double budget);
    void populateChunk(Chunk* c);
    void lightColumn(int x, int z, int y0 = 0, int y1 = 127);
    int skyChangeLow(int x, int y, int z, int oldOp) const;
    int directSky(int x, int y, int z) const;
    void unlight(int shift);
    bool updateLight(int x, int y, int z, int oldOp);
    void relightRect(int x0, int z0, int x1, int z1, int y0, int y1);
    void seedRegion(int x0, int z0, int x1, int z1, const Chunk* skip, int y0 = 0, int y1 = 127);
    void propagate(const Chunk* skip);
    void integrate(Chunk* c);
    void unload(Chunk* c);
    void markNeighborsDirty(const Chunk* c);

    Chunk* ring_[RING][RING] = {};
    ChunkStreamer* streamer_ = nullptr;
    class WorldGenerator* gen_ = nullptr;
    char dir_[64] = {};
    int centerX_ = 0, centerZ_ = 0, loadRadius_ = 3;
    int autosaveCursor_ = 0;
    int autosaveHold_ = 0;    // ticks yielded to terrain loading between saves
    bool pauseLoads_ = false; // an autosave sweep is in progress
    bool lightSlice_ = false;
    Chunk* lightPartial_ = nullptr; // chunk whose columns are lit but whose flood has not run
    int lightCol_ = 0;              // next column of lightPartial_, 0..256
    int memoryBudgetKb_ = 0;  // 0: unlimited. Otherwise the largest free block, in KB.

    static constexpr int SCHED_MAX = 4096, SCHED_HASH = 8192;
    ScheduledTick* sched_ = nullptr;
    uint64_t* schedKeys_ = nullptr;  // open addressing set of queued (x, y, z, id); 0 = empty
    int schedCount_ = 0;
    bool schedInsert(uint64_t key);
    void schedErase(uint64_t key);
};

Chunk* newChunk(int cx, int cz);
void freeChunk(Chunk* c);
