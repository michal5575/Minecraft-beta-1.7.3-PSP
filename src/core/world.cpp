#include "world.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "chunkio.h"
#include "worldgen.h"

namespace {

// Light queue entries use ring coordinates: x and z wrap at RING * 16 = 256.
constexpr int QBITS = 17;
constexpr uint32_t QCAP = 1u << QBITS;
constexpr uint32_t QMASK = QCAP - 1;
constexpr int RMASK = World::RING * 16 - 1;

uint32_t* g_queue = nullptr;
uint32_t g_head = 0, g_count = 0;

inline void qpush(int rx, int y, int rz) {
    if (g_count >= QCAP) return;
    g_queue[(g_head + g_count) & QMASK] = ((uint32_t)rx << 15) | ((uint32_t)rz << 7) | (uint32_t)y;
    ++g_count;
}

constexpr int RELIGHT_R = 15;
constexpr int RELIGHT_W = RELIGHT_R * 2 + 1;
constexpr int RELIGHT_MAXW = RELIGHT_W;
uint8_t* g_oldLight = nullptr;  // RELIGHT_MAXW * RELIGHT_MAXW columns of 128

// Darkening stack of incremental light updates: ring position plus the removed light level.
constexpr uint32_t RQCAP = 1u << 15;
uint32_t* g_rqueue = nullptr;
uint32_t g_rqCount = 0;
bool g_rqOverflow = false;

inline void rpush(int rx, int y, int rz, int level) {
    if (g_rqCount >= RQCAP) {
        g_rqOverflow = true;
        return;
    }
    g_rqueue[g_rqCount++] = ((uint32_t)level << 23) | ((uint32_t)rx << 15) | ((uint32_t)rz << 7) | (uint32_t)y;
}

const int DX[6] = {1, -1, 0, 0, 0, 0};
const int DY[6] = {0, 0, 1, -1, 0, 0};
const int DZ[6] = {0, 0, 0, 0, 1, -1};

inline int floorDiv16(int v) { return v >> 4; }
inline int iabs(int v) { return v < 0 ? -v : v; }
inline int cheb(int dx, int dz) {
    dx = iabs(dx), dz = iabs(dz);
    return dx > dz ? dx : dz;
}

}  // namespace

Chunk* newChunk(int cx, int cz) {
    Chunk* c = (Chunk*)std::calloc(1, sizeof(Chunk));
    if (!c) return nullptr;
    c->cx = cx;
    c->cz = cz;
    std::memset(c->temperature, 128, sizeof(c->temperature));
    std::memset(c->humidity, 128, sizeof(c->humidity));
    return c;
}

void freeChunk(Chunk* c) {
    if (!c) return;
    for (Section* s : c->sec) std::free(s);
    TileEntity* t = c->tiles;
    while (t) {
        TileEntity* n = t->next;
        std::free(t);
        t = n;
    }
    std::free(c->entityData);
    std::free(c);
}

World::World() {}
World::~World() { close(); }

bool World::init() {
    if (!g_queue) g_queue = (uint32_t*)std::malloc(QCAP * sizeof(uint32_t));
    if (!g_oldLight) g_oldLight = (uint8_t*)std::malloc(RELIGHT_MAXW * RELIGHT_MAXW * SY);
    if (!g_rqueue) g_rqueue = (uint32_t*)std::malloc(RQCAP * sizeof(uint32_t));
    if (!sched_) sched_ = (ScheduledTick*)std::malloc(SCHED_MAX * sizeof(ScheduledTick));
    if (!schedKeys_) schedKeys_ = (uint64_t*)std::calloc(SCHED_HASH, sizeof(uint64_t));
    return g_queue && g_oldLight && sched_ && schedKeys_;
}

namespace {
inline uint64_t schedKey(int x, int y, int z, uint8_t id) {
    return (1ull << 63) | ((uint64_t)(x & 0xFFFFFF) << 39) | ((uint64_t)(z & 0xFFFFFF) << 15) |
           ((uint64_t)(y & 127) << 8) | id;
}
inline uint32_t schedSlot(uint64_t key, int size) { return (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 40) & (size - 1); }
}  // namespace

bool World::schedInsert(uint64_t key) {
    uint32_t i = schedSlot(key, SCHED_HASH);
    while (schedKeys_[i]) {
        if (schedKeys_[i] == key) return false;
        i = (i + 1) & (SCHED_HASH - 1);
    }
    schedKeys_[i] = key;
    return true;
}

void World::schedErase(uint64_t key) {
    uint32_t i = schedSlot(key, SCHED_HASH);
    while (schedKeys_[i] && schedKeys_[i] != key) i = (i + 1) & (SCHED_HASH - 1);
    if (!schedKeys_[i]) return;
    // Backward-shift deletion keeps linear probing chains intact.
    uint32_t j = i;
    for (;;) {
        j = (j + 1) & (SCHED_HASH - 1);
        if (!schedKeys_[j]) break;
        uint32_t home = schedSlot(schedKeys_[j], SCHED_HASH);
        bool between = i <= j ? (home > i && home <= j) : (home > i || home <= j);
        if (between) continue;
        schedKeys_[i] = schedKeys_[j];
        i = j;
    }
    schedKeys_[i] = 0;
}

void World::scheduleTick(int x, int y, int z, uint8_t id, int delay) {
    if (!sched_ || schedCount_ >= SCHED_MAX || (unsigned)y >= (unsigned)SY || !areaLoaded(x, z, 8)) return;
    if (!schedInsert(schedKey(x, y, z, id))) return;
    sched_[schedCount_++] = {x, z, (int16_t)y, id, time + delay};
}

int World::takeDueTicks(ScheduledTick* out, int max) {
    int n = 0, keep = 0;
    for (int i = 0; i < schedCount_; ++i) {
        const ScheduledTick& t = sched_[i];
        if (t.due <= time && n < max) {
            out[n++] = t;
            schedErase(schedKey(t.x, t.y, t.z, t.id));
        } else {
            sched_[keep++] = t;
        }
    }
    schedCount_ = keep;
    return n;
}

bool World::open(const char* dir, int64_t s, int dim) {
    close();
    std::snprintf(dir_, sizeof(dir_), "%s", dir);
    seed = s;
    dimension = dim;
    time = 0;
    skySubtracted = 0;
    outOfMemory = false;
    gen_ = new WorldGenerator(seed, dim < 0);
    streamer_ = new ChunkStreamer(dir_, seed, dim < 0);
    if (!streamer_->start()) {
        close();
        return false;
    }
    return true;
}

void World::close() {
    if (!streamer_) return;
    for (int i = 0; i < RING; ++i)
        for (int j = 0; j < RING; ++j)
            if (ring_[i][j]) unload(ring_[i][j]);
    streamer_->finish();
    while (Chunk* c = streamer_->takeLoaded()) freeChunk(c);
    delete streamer_;
    streamer_ = nullptr;
    delete gen_;
    gen_ = nullptr;
    loadedChunks = 0;
    lightPartial_ = nullptr;
    lightCol_ = 0;
    schedCount_ = 0;
    if (schedKeys_) std::memset(schedKeys_, 0, SCHED_HASH * sizeof(uint64_t));
}

Section* World::sectionFor(Chunk* c, int sy) {
    if (c->sec[sy]) return c->sec[sy];
    Section* s = (Section*)std::malloc(sizeof(Section));
    if (!s) {
        if (!outOfMemory) crashNote("oom-section");
        outOfMemory = true;
        return nullptr;
    }
    std::memset(s->blocks, 0, sizeof(s->blocks));
    std::memset(s->meta, 0, sizeof(s->meta));
    std::memset(s->light, 0xF0, sizeof(s->light));
    c->sec[sy] = s;
    return s;
}

void World::setRaw(int x, int y, int z, uint8_t id, int m) {
    if ((unsigned)y >= (unsigned)SY) return;
    Chunk* c = chunk(x >> 4, z >> 4);
    if (!c) return;
    Section* s = c->sec[y >> 4];
    if (!s) {
        if (id == AIR) return;
        s = sectionFor(c, y >> 4);
        if (!s) return;
    }
    int i = sectionIndex(x & 15, y & 15, z & 15);
    s->blocks[i] = id;
    uint8_t& b = s->meta[i >> 1];
    if (i & 1)
        b = (uint8_t)((b & 0x0F) | ((m & 15) << 4));
    else
        b = (uint8_t)((b & 0xF0) | (m & 15));
    c->flags |= CHUNK_NEEDS_SAVE;
}

void World::setMetaRaw(int x, int y, int z, int m) {
    if ((unsigned)y >= (unsigned)SY) return;
    Chunk* c = chunk(x >> 4, z >> 4);
    if (!c || !c->sec[y >> 4]) return;
    int i = sectionIndex(x & 15, y & 15, z & 15);
    uint8_t& b = c->sec[y >> 4]->meta[i >> 1];
    if (i & 1)
        b = (uint8_t)((b & 0x0F) | ((m & 15) << 4));
    else
        b = (uint8_t)((b & 0xF0) | (m & 15));
    c->flags |= CHUNK_NEEDS_SAVE;
}

void World::markBlockDirty(int x, int y, int z) {
    int cx0 = floorDiv16(x - 1), cx1 = floorDiv16(x + 1);
    int cz0 = floorDiv16(z - 1), cz1 = floorDiv16(z + 1);
    int sy0 = (y - 1) >> 4, sy1 = (y + 1) >> 4;
    if (sy0 < 0) sy0 = 0;
    if (sy1 > SECTIONS_Y - 1) sy1 = SECTIONS_Y - 1;
    uint8_t bits = 0;
    for (int sy = sy0; sy <= sy1; ++sy) bits |= (uint8_t)(1 << sy);
    for (int cx = cx0; cx <= cx1; ++cx)
        for (int cz = cz0; cz <= cz1; ++cz)
            if (Chunk* c = chunk(cx, cz)) c->dirty |= bits;
}

void World::markAllDirty() {
    for (auto& row : ring_)
        for (Chunk* c : row)
            if (c) c->dirty = 0xFF;
}

void World::markNeighborsDirty(const Chunk* c) {
    static const int N[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    for (const auto& d : N) {
        Chunk* n = chunk(c->cx + d[0], c->cz + d[1]);
        if (n && n->render) n->dirty = 0xFF;
    }
}

int World::heightAt(int x, int z) const {
    const Chunk* c = chunk(x >> 4, z >> 4);
    if (!c) return 0;
    int col = sectionIndex(x & 15, 0, z & 15);
    for (int sy = SECTIONS_Y - 1; sy >= 0; --sy) {
        const Section* s = c->sec[sy];
        if (!s) continue;
        for (int y = 15; y >= 0; --y)
            if (g_blocks[s->blocks[col + y]].opacity != 0) return sy * 16 + y + 1;
    }
    return 0;
}

int World::topSolidAt(int x, int z) const {
    const Chunk* c = chunk(x >> 4, z >> 4);
    if (!c) return -1;
    int col = sectionIndex(x & 15, 0, z & 15);
    for (int sy = SECTIONS_Y - 1; sy >= 0; --sy) {
        const Section* s = c->sec[sy];
        if (!s) continue;
        for (int y = 15; y >= 0; --y) {
            uint8_t id = s->blocks[col + y];
            if ((sy | y) && (blockMatSolid(id) || blockIsLiquid(id))) return sy * 16 + y + 1;
        }
    }
    return -1;
}

int World::biomeTemperature(int x, int z) const {
    const Chunk* c = chunk(x >> 4, z >> 4);
    return c ? c->temperature[(x & 15) * 16 + (z & 15)] : 128;
}

// ------------------------------------------------------------------ lighting

// Resets light in y0..y1 to the column's own sky light plus emission.
void World::lightColumn(int x, int z, int y0, int y1) {
    Chunk* c = chunk(x >> 4, z >> 4);
    if (!c) return;
    int col = sectionIndex(x & 15, 0, z & 15);
    int l = 15;
    for (int sy = SECTIONS_Y - 1; sy >= 0; --sy) {
        Section* s = c->sec[sy];
        bool inRange = sy * 16 <= y1 && sy * 16 + 15 >= y0;
        if (!s) {
            if (l == 15 || !inRange) continue;
            s = sectionFor(c, sy);
            if (!s) continue;
        }
        const uint8_t* bl = s->blocks + col;
        uint8_t* li = s->light + col;
        for (int y = 15; y >= 0; --y) {
            const BlockDef& d = g_blocks[bl[y]];
            if (l > 0 && d.opacity) {
                l -= d.opacity;
                if (l < 0) l = 0;
            }
            int wy = sy * 16 + y;
            if (wy >= y0 && wy <= y1) li[y] = (uint8_t)((l << 4) | d.emission);
        }
    }
}

// Lowest y whose direct sky light changed when the block at y went from opacity `oldOp` to its
// current one (the change runs down the column until both values agree).
int World::skyChangeLow(int x, int y, int z, int oldOp) const {
    int l = 15;
    for (int yy = SY - 1; yy > y && l > 0; --yy) {
        int op = g_blocks[get(x, yy, z)].opacity;
        if (op) l = l > op ? l - op : 0;
    }
    if (l == 0) return y;
    int lo = l > oldOp ? l - oldOp : 0;
    int ln = l;
    int op = g_blocks[get(x, y, z)].opacity;
    if (op) ln = ln > op ? ln - op : 0;
    if (!oldOp) lo = l;
    int low = y;
    for (int yy = y - 1; yy >= 0 && lo != ln; --yy) {
        low = yy;
        int o = g_blocks[get(x, yy, z)].opacity;
        if (o) {
            lo = lo > o ? lo - o : 0;
            ln = ln > o ? ln - o : 0;
        }
    }
    return low;
}

void World::seedRegion(int x0, int z0, int x1, int z1, const Chunk* skip, int y0, int y1) {
    (void)skip;
    int top = 0;
    for (int cx = floorDiv16(x0 - 1); cx <= floorDiv16(x1 + 1); ++cx)
        for (int cz = floorDiv16(z0 - 1); cz <= floorDiv16(z1 + 1); ++cz) {
            const Chunk* c = chunk(cx, cz);
            if (!c || !(c->flags & CHUNK_LIT)) continue;
            for (int sy = SECTIONS_Y - 1; sy >= 0; --sy)
                if (c->sec[sy]) {
                    if (sy * 16 + 16 > top) top = sy * 16 + 16;
                    break;
                }
        }
    if (top > y1 + 1) top = y1 + 1;
    if (y0 < 0) y0 = 0;
    for (int x = x0; x <= x1; ++x) {
        for (int z = z0; z <= z1; ++z) {
            Chunk* c = chunk(x >> 4, z >> 4);
            if (!c || !(c->flags & CHUNK_LIT)) continue;
            int rx = x & RMASK, rz = z & RMASK;
            int lx = x & 15, lz = z & 15;
            int col = sectionIndex(lx, 0, lz);
            // Matches DX,DY,DZ: +x -x +y -y +z -z, in sectionIndex order (x << 8) | (z << 4) | y.
            static const int STEP[6] = {256, -256, 1, -1, 16, -16};
            for (int y = y0; y < top; ++y) {
                const Section* s = c->sec[y >> 4];
                uint8_t l = s ? s->light[col + (y & 15)] : 0xF0;
                int sl = l >> 4, bl = l & 15;
                if (sl < 2 && bl < 2) continue;
                int ly = y & 15;
                bool inside = s && (unsigned)(lx - 1) < 14u && (unsigned)(ly - 1) < 14u && (unsigned)(lz - 1) < 14u;
                bool need = false;
                if (inside) {
                    int base = col + ly;
                    for (int d = 0; d < 6 && !need; ++d) {
                        int j = base + STEP[d];
                        uint8_t id = s->blocks[j];
                        uint8_t nl = s->light[j];
                        // Air is the common neighbor. Opacity 0 still spreads, so the check is the same.
                        if (id != AIR && g_blocks[id].opacity >= 15) continue;
                        if ((nl >> 4) < sl - 1 || (nl & 15) < bl - 1) need = true;
                    }
                } else {
                    for (int d = 0; d < 6 && !need; ++d) {
                        int ny = y + DY[d];
                        if ((unsigned)ny >= (unsigned)SY) continue;
                        int nrx = (rx + DX[d]) & RMASK, nrz = (rz + DZ[d]) & RMASK;
                        const Chunk* n = ring_[nrx >> 4][nrz >> 4];
                        if (!n || !(n->flags & CHUNK_LIT)) continue;
                        const Section* ns = n->sec[ny >> 4];
                        int j = sectionIndex(nrx & 15, ny & 15, nrz & 15);
                        uint8_t id = ns ? ns->blocks[j] : AIR;
                        if (g_blocks[id].opacity >= 15) continue;
                        uint8_t nl = ns ? ns->light[j] : 0xF0;
                        if ((nl >> 4) < sl - 1 || (nl & 15) < bl - 1) need = true;
                    }
                }
                if (need) {
                    qpush(rx, y, rz);
                    if (g_count > QCAP * 3 / 4) propagate(skip);
                }
            }
        }
    }
}

// Increase-only flood fill; only enters lit chunks. Changes outside `skip` mark meshes dirty
// unless skip is the sentinel `this` (relight() diffs the region itself).
void World::propagate(const Chunk* skip) {
    bool mark = skip != (const Chunk*)(const void*)this;
    while (g_count) {
        uint32_t e = g_queue[g_head];
        g_head = (g_head + 1) & QMASK;
        --g_count;
        int rx = (int)(e >> 15), rz = (int)((e >> 7) & 255), y = (int)(e & 127);
        Chunk* c = ring_[rx >> 4][rz >> 4];
        if (!c) continue;
        Section* cs = c->sec[y >> 4];
        int lx = rx & 15, ly = y & 15, lz = rz & 15;
        uint8_t l = cs ? cs->light[sectionIndex(lx, ly, lz)] : 0xF0;
        int s = l >> 4, b = l & 15;
        if (s <= 1 && b <= 1) continue;
        // Interior cells only touch this section, so the six neighbors are pointer arithmetic.
        if (cs && (unsigned)(lx - 1) < 14u && (unsigned)(ly - 1) < 14u && (unsigned)(lz - 1) < 14u) {
            static const int STEP[6] = {256, -256, 1, -1, 16, -16};
            int base = sectionIndex(lx, ly, lz);
            for (int d = 0; d < 6; ++d) {
                int j = base + STEP[d];
                uint8_t id = cs->blocks[j];
                int dec;
                if (id == AIR) {
                    dec = 1;
                } else {
                    int op = g_blocks[id].opacity;
                    if (op >= 15) continue;
                    dec = op > 1 ? op : 1;
                }
                uint8_t cur = cs->light[j];
                int cs2 = cur >> 4, cb = cur & 15;
                int ns2 = s - dec, nb = b - dec;
                bool changed = false;
                if (ns2 > cs2) {
                    cs2 = ns2;
                    changed = true;
                }
                if (nb > cb) {
                    cb = nb;
                    changed = true;
                }
                if (!changed) continue;
                cs->light[j] = (uint8_t)((cs2 << 4) | cb);
                c->flags |= CHUNK_NEEDS_SAVE;
                if (mark && c != skip) markBlockDirty(c->cx * 16 + (lx + DX[d]), y + DY[d], c->cz * 16 + (lz + DZ[d]));
                qpush((rx + DX[d]) & RMASK, y + DY[d], (rz + DZ[d]) & RMASK);
            }
            continue;
        }
        for (int d = 0; d < 6; ++d) {
            int ny = y + DY[d];
            if ((unsigned)ny >= (unsigned)SY) continue;
            int nrx = (rx + DX[d]) & RMASK, nrz = (rz + DZ[d]) & RMASK;
            Chunk* n = ring_[nrx >> 4][nrz >> 4];
            if (!n || !(n->flags & CHUNK_LIT)) continue;
            Section* ns = n->sec[ny >> 4];
            int j = sectionIndex(nrx & 15, ny & 15, nrz & 15);
            uint8_t id = ns ? ns->blocks[j] : AIR;
            int op = g_blocks[id].opacity;
            if (op >= 15) continue;
            int dec = op > 1 ? op : 1;
            uint8_t cur = ns ? ns->light[j] : 0xF0;
            int cs2 = cur >> 4, cb = cur & 15;
            int ns2 = s - dec, nb = b - dec;
            bool changed = false;
            if (ns2 > cs2) {
                cs2 = ns2;
                changed = true;
            }
            if (nb > cb) {
                cb = nb;
                changed = true;
            }
            if (!changed) continue;
            if (!ns) {
                ns = sectionFor(n, ny >> 4);
                if (!ns) continue;
            }
            ns->light[j] = (uint8_t)((cs2 << 4) | cb);
            n->flags |= CHUNK_NEEDS_SAVE;
            if (mark && n != skip) markBlockDirty(n->cx * 16 + (nrx & 15), ny, n->cz * 16 + (nrz & 15));
            qpush(nrx, ny, nrz);
        }
    }
}

void World::setLightSlice(bool on) {
    if (!on && lightPartial_) sliceLight(lightPartial_, platformNow(), 1.0);
    lightSlice_ = on;
}

// Columns first, a few per call, then the flood. The chunk stays unlit until the flood finishes,
// so a loading screen can sleep between calls without publishing a half-lit chunk.
void World::sliceLight(Chunk* c, double t0, double budget) {
    if (!c) return;
    int x0 = c->cx * 16, z0 = c->cz * 16;
    while (lightCol_ < 256 && platformNow() - t0 < budget) {
        int i = lightCol_++;
        lightColumn(x0 + (i & 15), z0 + (i >> 4));
    }
    if (lightCol_ < 256) return;
    c->flags |= CHUNK_LIT | CHUNK_NEEDS_SAVE;
    g_head = g_count = 0;
    seedRegion(x0 - 1, z0 - 1, x0 + 16, z0 + 16, c);
    propagate(c);
    c->dirty = 0xFF;
    markNeighborsDirty(c);
    lightPartial_ = nullptr;
    lightCol_ = 0;
}

void World::lightChunk(Chunk* c) {
    c->flags |= CHUNK_LIT | CHUNK_NEEDS_SAVE;
    int x0 = c->cx * 16, z0 = c->cz * 16;
    for (int x = 0; x < 16; ++x)
        for (int z = 0; z < 16; ++z) lightColumn(x0 + x, z0 + z);
    g_head = g_count = 0;
    seedRegion(x0 - 1, z0 - 1, x0 + 16, z0 + 16, c);
    propagate(c);
    c->dirty = 0xFF;
    markNeighborsDirty(c);
}

void World::relight(int cx, int cz) {
    relightRect(cx - RELIGHT_R, cz - RELIGHT_R, cx + RELIGHT_R, cz + RELIGHT_R, 0, SY - 1);
}

// Recomputes light inside the box from scratch, taking light from its outer layer as given.
void World::relightRect(int x0, int z0, int x1, int z1, int y0, int y1) {
    if (y0 < 0) y0 = 0;
    if (y1 > SY - 1) y1 = SY - 1;
    int w = x1 - x0 + 1, d = z1 - z0 + 1, h = y1 - y0 + 1;
    if (w > RELIGHT_MAXW || d > RELIGHT_MAXW || h <= 0) return;
    ++relightCalls;
    relightVoxels += (uint32_t)(w * d * h);
    for (int i = 0; i < w; ++i)
        for (int k = 0; k < d; ++k) {
            uint8_t* dst = g_oldLight + (i * d + k) * h;
            for (int y = 0; y < h; ++y) dst[y] = getLight(x0 + i, y0 + y, z0 + k);
        }
    for (int i = 0; i < w; ++i)
        for (int k = 0; k < d; ++k) {
            const Chunk* c = chunk((x0 + i) >> 4, (z0 + k) >> 4);
            if (c && (c->flags & CHUNK_LIT)) lightColumn(x0 + i, z0 + k, y0, y1);
        }
    g_head = g_count = 0;
    const Chunk* noMark = (const Chunk*)(const void*)this;
    seedRegion(x0 - 1, z0 - 1, x1 + 1, z1 + 1, noMark, y0 - 1, y1 + 1);
    propagate(noMark);
    for (int i = 0; i < w; ++i)
        for (int k = 0; k < d; ++k) {
            int x = x0 + i, z = z0 + k;
            Chunk* c = chunk(x >> 4, z >> 4);
            if (!c || !(c->flags & CHUNK_LIT)) continue;
            const uint8_t* old = g_oldLight + (i * d + k) * h;
            bool changed = false;
            for (int y = 0; y < h; ++y)
                if (old[y] != getLight(x, y0 + y, z)) {
                    markBlockDirty(x, y0 + y, z);
                    changed = true;
                }
            if (changed) c->flags |= CHUNK_NEEDS_SAVE;
        }
}

// Sky light the column itself delivers to (x, y, z), ignoring light spreading in from the side.
int World::directSky(int x, int y, int z) const {
    const Chunk* c = chunk(x >> 4, z >> 4);
    if (!c) return 15;
    int col = sectionIndex(x & 15, 0, z & 15);
    int l = 15;
    for (int sy = SECTIONS_Y - 1; sy >= (y >> 4); --sy) {
        const Section* s = c->sec[sy];
        if (!s) continue;
        int bottom = sy == (y >> 4) ? (y & 15) : 0;
        for (int yy = 15; yy >= bottom; --yy) {
            int op = g_blocks[s->blocks[col + yy]].opacity;
            if (op && (l -= op) <= 0) return 0;
        }
    }
    return l;
}

// Darkening pass of an incremental light update for one channel (shift 4 = sky, 0 = block): clears
// light that came from the removed levels on the stack and queues the voxels that will refill it.
void World::unlight(int shift) {
    while (g_rqCount) {
        uint32_t e = g_rqueue[--g_rqCount];
        int level = (int)(e >> 23), rx = (int)((e >> 15) & 255), rz = (int)((e >> 7) & 255), y = (int)(e & 127);
        for (int d = 0; d < 6; ++d) {
            int ny = y + DY[d];
            if ((unsigned)ny >= (unsigned)SY) continue;
            int nrx = (rx + DX[d]) & RMASK, nrz = (rz + DZ[d]) & RMASK;
            Chunk* n = ring_[nrx >> 4][nrz >> 4];
            if (!n || !(n->flags & CHUNK_LIT)) continue;
            Section* ns = n->sec[ny >> 4];
            int j = sectionIndex(nrx & 15, ny & 15, nrz & 15);
            uint8_t cur = ns ? ns->light[j] : 0xF0;
            int l = (cur >> shift) & 15;
            if (l == 0) continue;
            if (l >= level) {
                qpush(nrx, ny, nrz);
                continue;
            }
            int wx = n->cx * 16 + (nrx & 15), wz = n->cz * 16 + (nrz & 15);
            int src = shift ? directSky(wx, ny, wz) : g_blocks[ns ? ns->blocks[j] : AIR].emission;
            if (l <= src) {
                qpush(nrx, ny, nrz);
                continue;
            }
            if (!ns && !(ns = sectionFor(n, ny >> 4))) continue;
            ns->light[j] = (uint8_t)((cur & ~(15 << shift)) | (src << shift));
            n->flags |= CHUNK_NEEDS_SAVE;
            markBlockDirty(wx, ny, wz);
            rpush(nrx, ny, nrz, l);
            if (src > 0) qpush(nrx, ny, nrz);
        }
    }
}

// Incremental light update after the block at (x, y, z) changed from opacity `oldOp`. Returns false
// if the work queue overflowed and the area needs a full relight.
bool World::updateLight(int x, int y, int z, int oldOp) {
    Chunk* c = chunk(x >> 4, z >> 4);
    if (!c || !(c->flags & CHUNK_LIT)) return true;
    g_head = g_count = 0;
    g_rqCount = 0;
    g_rqOverflow = false;
    int rx = x & RMASK, rz = z & RMASK;
    const BlockDef& def = g_blocks[get(x, y, z)];
    c->flags |= CHUNK_NEEDS_SAVE;

    int lo = def.opacity != oldOp ? skyChangeLow(x, y, z, oldOp) : y + 1;
    int src = directSky(x, y, z);
    for (int yy = y; yy >= lo; --yy) {
        if (yy < y) {
            int op = g_blocks[get(x, yy, z)].opacity;
            if (op) src = src > op ? src - op : 0;
        }
        Section* s = sectionFor(c, yy >> 4);
        if (!s) return false;
        uint8_t& li = s->light[sectionIndex(x & 15, yy & 15, z & 15)];
        int old = li >> 4;
        if (old == src && yy != y) continue;
        li = (uint8_t)((src << 4) | (li & 15));
        markBlockDirty(x, yy, z);
        if (old > src) rpush(rx, yy, rz, old);
        if (src > 0) qpush(rx, yy, rz);
    }
    unlight(4);

    Section* s = sectionFor(c, y >> 4);
    if (!s) return false;
    uint8_t& li = s->light[sectionIndex(x & 15, y & 15, z & 15)];
    int old = li & 15;
    li = (uint8_t)((li & 0xF0) | def.emission);
    if (old > def.emission) rpush(rx, y, rz, old);
    if (def.emission) qpush(rx, y, rz);
    unlight(0);

    for (int d = 0; d < 6; ++d) {
        int ny = y + DY[d];
        if ((unsigned)ny < (unsigned)SY) qpush((rx + DX[d]) & RMASK, ny, (rz + DZ[d]) & RMASK);
    }
    if (g_rqOverflow) return false;
    propagate(nullptr);
    return true;
}

void World::setBlock(int x, int y, int z, uint8_t id, int m) {
    if ((unsigned)y >= (unsigned)SY || !isLoaded(x, z)) return;
    uint8_t old = get(x, y, z);
    setRaw(x, y, z, id, m);
    markBlockDirty(x, y, z);
    const BlockDef &a = g_blocks[old], &b = g_blocks[id];
    if (a.opacity == b.opacity && a.emission == b.emission) return;
    ++lightUpdates;
    if (!updateLight(x, y, z, a.opacity)) {
        int lo = a.opacity != b.opacity ? skyChangeLow(x, y, z, a.opacity) : y;
        relightRect(x - RELIGHT_R, z - RELIGHT_R, x + RELIGHT_R, z + RELIGHT_R, lo - RELIGHT_R, y + RELIGHT_R);
    }
}

void World::setMeta(int x, int y, int z, int m) {
    setMetaRaw(x, y, z, m);
    markBlockDirty(x, y, z);
}

// ------------------------------------------------------------------ tile entities

TileEntity* World::tileAt(int x, int y, int z) const {
    const Chunk* c = chunk(x >> 4, z >> 4);
    if (!c) return nullptr;
    for (TileEntity* t = c->tiles; t; t = t->next)
        if (t->x == x && t->y == y && t->z == z) return t;
    return nullptr;
}

TileEntity* World::createTile(int x, int y, int z, uint8_t type) {
    Chunk* c = chunk(x >> 4, z >> 4);
    if (!c) return nullptr;
    removeTile(x, y, z);
    TileEntity* t = (TileEntity*)std::calloc(1, sizeof(TileEntity));
    if (!t) return nullptr;
    t->x = x;
    t->y = y;
    t->z = z;
    t->type = type;
    t->next = c->tiles;
    c->tiles = t;
    c->flags |= CHUNK_NEEDS_SAVE;
    return t;
}

void World::removeTile(int x, int y, int z) {
    Chunk* c = chunk(x >> 4, z >> 4);
    if (!c) return;
    for (TileEntity** p = &c->tiles; *p; p = &(*p)->next) {
        TileEntity* t = *p;
        if (t->x == x && t->y == y && t->z == z) {
            *p = t->next;
            std::free(t);
            c->flags |= CHUNK_NEEDS_SAVE;
            return;
        }
    }
}

// ------------------------------------------------------------------ streaming

bool World::chunkRenderable(const Chunk* c) const {
    // Lit is enough. Waiting for the chunk past the frontier meant the last loaded column
    // never got a mesh, so the world ended in a flat wall of sky.
    return c && (c->flags & CHUNK_LIT);
}

int World::readyCount(float px, float pz, int radius) const {
    int pcx = floorDiv16((int)(px < 0 ? px - 1 : px)), pcz = floorDiv16((int)(pz < 0 ? pz - 1 : pz));
    int n = 0;
    for (int dx = -radius; dx <= radius; ++dx)
        for (int dz = -radius; dz <= radius; ++dz)
            if (chunkRenderable(chunk(pcx + dx, pcz + dz))) ++n;
    return n;
}

void World::integrate(Chunk* c) {
    Chunk*& slot = ring_[c->cx & (RING - 1)][c->cz & (RING - 1)];
    if (slot) unload(slot);
    slot = c;
    ++loadedChunks;
    c->dirty = 0xFF;
    if (onChunkLoaded) onChunkLoaded(*this, c, hookUser);
    if (c->flags & CHUNK_LIT) markNeighborsDirty(c);
}

void World::unload(Chunk* c) {
    if (onChunkUnload) onChunkUnload(*this, c, hookUser);
    ring_[c->cx & (RING - 1)][c->cz & (RING - 1)] = nullptr;
    if (c == lightPartial_) {
        lightPartial_ = nullptr;
        lightCol_ = 0;
    }
    --loadedChunks;
    if ((c->flags & CHUNK_NEEDS_SAVE) || c->entityBytes)
        streamer_->requestSave(c);
    else
        freeChunk(c);
}

static Chunk* copyForSave(const Chunk* c) {
    Chunk* n = newChunk(c->cx, c->cz);
    if (!n) return nullptr;
    n->flags = c->flags;
    std::memcpy(n->temperature, c->temperature, sizeof(n->temperature));
    std::memcpy(n->humidity, c->humidity, sizeof(n->humidity));
    for (int i = 0; i < World::SECTIONS_Y; ++i) {
        if (!c->sec[i]) continue;
        n->sec[i] = (Section*)std::malloc(sizeof(Section));
        if (!n->sec[i]) {
            freeChunk(n);
            return nullptr;
        }
        std::memcpy(n->sec[i], c->sec[i], sizeof(Section));
    }
    TileEntity** tail = &n->tiles;
    for (const TileEntity* t = c->tiles; t; t = t->next) {
        TileEntity* d = (TileEntity*)std::malloc(sizeof(TileEntity));
        if (!d) {
            freeChunk(n);
            return nullptr;
        }
        *d = *t;
        d->next = nullptr;
        *tail = d;
        tail = &d->next;
    }
    return n;
}

// Reused so a save does not malloc a second full copy of every section. Chunks that do not fit
// fall back to one section copy, and only while the worker is idle.
static uint8_t g_saveScratch[48 * 1024];

// Queues one chunk. False means out of memory; the chunk stays dirty.
static bool queueChunkSave(ChunkStreamer* streamer, Chunk* c) {
    uint32_t size = 0;
    if (encodeChunk(*c, g_saveScratch, sizeof(g_saveScratch), size)) {
        uint8_t* blob = (uint8_t*)std::malloc(size);
        if (!blob) return false;
        std::memcpy(blob, g_saveScratch, size);
        c->flags &= ~CHUNK_NEEDS_SAVE;
        streamer->requestSaveEncoded(c->cx, c->cz, blob, size);
        return true;
    }
    Chunk* copy = copyForSave(c);
    if (!copy) return false;
    c->flags &= ~CHUNK_NEEDS_SAVE;
    streamer->requestSave(copy);
    return true;
}

int World::workerBacklog() const { return streamer_ ? streamer_->backlog() : 0; }

int World::workerInFlight() const { return streamer_ ? streamer_->inFlight() : 0; }

bool World::autosaveStep(int maxChunks) {
    if (!streamer_) return false;
    crashNote("autosave");
    pauseLoads_ = true;
    // One save at a time. A chunk copy and a terrain generation together are what runs the PSP out of RAM.
    if (streamer_->backlog() > 0) return true;
    if (autosaveHold_ > 0) {
        --autosaveHold_;
        return true;
    }
    if (streamer_->inFlight() > 0) return true;
    int saved = 0;
    while (autosaveCursor_ < RING * RING) {
        Chunk* c = ring_[autosaveCursor_ / RING][autosaveCursor_ % RING];
        ++autosaveCursor_;
        if (!c || !(c->flags & CHUNK_NEEDS_SAVE)) continue;
        if (!queueChunkSave(streamer_, c)) {
            --autosaveCursor_;
            return true;
        }
        autosaveHold_ = 3;
        if (++saved >= maxChunks) return true;
    }
    autosaveCursor_ = 0;
    autosaveHold_ = 0;
    pauseLoads_ = false;
    return false;
}

void World::populateChunk(Chunk* c) {
    gen_->populate(*this, c->cx, c->cz);
    c->flags |= CHUNK_POPULATED | CHUNK_NEEDS_SAVE;
    if (onChunkPopulated) onChunkPopulated(*this, c, hookUser);
    for (int dx = 0; dx <= 1; ++dx)
        for (int dz = 0; dz <= 1; ++dz)
            if (Chunk* t = chunk(c->cx + dx, c->cz + dz)) markNeighborsDirty(t);
}

void World::update(float px, float pz, int loadRadius, double budget) {
    if (!streamer_) return;
    if (loadRadius > MAX_LOAD_RADIUS) loadRadius = MAX_LOAD_RADIUS;
    int pcx = floorDiv16((int)(px < 0 ? px - 1 : px)), pcz = floorDiv16((int)(pz < 0 ? pz - 1 : pz));
    centerX_ = pcx;
    centerZ_ = pcz;
    loadRadius_ = loadRadius;
    int keep = loadRadius + 1;
    int freeKb = memoryBudgetKb_;
    // freeKb is unused malloc space. Drop the skirt first, then the outer rings,
    // and stop generating once less than 2.5 MB is left.
    if (freeKb > 0) {
        if (freeKb < 2048) keep = loadRadius;
        if (freeKb < 1280 && keep > 3) keep = 3;
        if (freeKb < 768 && keep > 2) keep = 2;
        if (freeKb < 1280) crashNote("low-mem");
    }

    for (int i = 0; i < RING; ++i)
        for (int j = 0; j < RING; ++j) {
            Chunk* c = ring_[i][j];
            if (c && cheb(c->cx - pcx, c->cz - pcz) > keep) unload(c);
        }

    streamer_->pump();
    while (Chunk* c = streamer_->takeLoaded()) {
        if (cheb(c->cx - pcx, c->cz - pcz) > keep || chunk(c->cx, c->cz)) {
            freeChunk(c);
            continue;
        }
        integrate(c);
    }
    if (streamer_->failed()) {
        if (!outOfMemory) crashNote("oom-load");
        outOfMemory = true;
    }

    // A queued save still owns its buffer. Don't generate terrain until that write has freed it,
    // or while malloc has under 2.5 MB free.
    // 2048 KB still fits chunks. Stopping there froze the edge of the world while the probe
    // said a couple of megabytes were free. Only the tighter emergency levels halt loading.
    int cap = (pauseLoads_ && streamer_->backlog() > 0) || (freeKb > 0 && freeKb < 1280) ? 0 : 6;
    for (int d = 0; d <= loadRadius && streamer_->inFlight() < cap; ++d)
        for (int dx = -d; dx <= d && streamer_->inFlight() < cap; ++dx)
            for (int dz = -d; dz <= d; ++dz) {
                if (iabs(dx) != d && iabs(dz) != d) continue;
                int cx = pcx + dx, cz = pcz + dz;
                if (chunk(cx, cz) || streamer_->pending(cx, cz)) continue;
                if (ring_[cx & (RING - 1)][cz & (RING - 1)]) continue;
                streamer_->requestLoad(cx, cz);
                if (streamer_->inFlight() >= cap) break;
            }

    double t0 = platformNow();
    // Lighting writes into the chunk that is already allocated. Generating terrain is what
    // allocates, so that is the part that waits when malloc is tight. Zeroing the budget
    // here left the chunk in front of the player loaded, unlit and invisible.
    bool tight = freeKb > 0 && freeKb < 1280;
    if (tight && budget > 0.003) budget = 0.003;
    while (budget > 0) {
        if (lightPartial_ && chunk(lightPartial_->cx, lightPartial_->cz) != lightPartial_) {
            lightPartial_ = nullptr;
            lightCol_ = 0;
        }
        if (lightPartial_) {
            sliceLight(lightPartial_, t0, budget);
            if (platformNow() - t0 >= budget) break;
            continue;
        }
        Chunk* work = nullptr;
        bool populate = false;
        for (int d = 0; d <= loadRadius && !work; ++d)
            for (int dx = -d; dx <= d && !work; ++dx)
                for (int dz = -d; dz <= d; ++dz) {
                    if (iabs(dx) != d && iabs(dz) != d) continue;
                    Chunk* c = chunk(pcx + dx, pcz + dz);
                    if (!c) continue;
                    if (!tight && !(c->flags & CHUNK_POPULATED) && chunk(c->cx + 1, c->cz) && chunk(c->cx, c->cz + 1) &&
                        chunk(c->cx + 1, c->cz + 1)) {
                        work = c;
                        populate = true;
                        break;
                    }
                    if (!(c->flags & CHUNK_LIT) && (c->flags & CHUNK_POPULATED)) {
                        const Chunk* a = chunk(c->cx - 1, c->cz);
                        const Chunk* b = chunk(c->cx, c->cz - 1);
                        const Chunk* ab = chunk(c->cx - 1, c->cz - 1);
                        bool ready = a && b && ab && (a->flags & b->flags & ab->flags & CHUNK_POPULATED);
                        // The column next to the player is on screen even when the world past it
                        // has not streamed in yet. Light it from what is loaded; the missing
                        // side is filled in when that neighbor arrives.
                        if (ready || ((!a || !b || !ab) && d <= 2)) {
                            work = c;
                            break;
                        }
                    }
                }
        if (!work) break;
        if (populate)
            populateChunk(work);
        else if (lightSlice_) {
            lightPartial_ = work;
            lightCol_ = 0;
            sliceLight(work, t0, budget);
        } else
            lightChunk(work);
        if (platformNow() - t0 >= budget) break;
    }
}
