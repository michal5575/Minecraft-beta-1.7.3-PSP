#include "chunkio.h"

#include <dirent.h>
#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "worldgen.h"

#ifdef __PSP__
#include <pspkernel.h>
#else
#include <chrono>
#include <thread>
#endif

// The PSP crash log replaces this. Kept weak so a build without that file still links.
__attribute__((weak)) void crashNote(const char*) {}

#ifdef __PSP__
double platformNow() { return (double)sceKernelGetSystemTimeWide() / 1e6; }
void platformSleepMs(int ms) { sceKernelDelayThread(ms * 1000); }
#else
double platformNow() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}
void platformSleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
#endif

bool makeDirectory(const char* path) {
#ifdef _WIN32
    return mkdir(path) == 0;
#else
    return mkdir(path, 0777) == 0;
#endif
}

bool fileExists(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

void removeWorldFiles(const char* dir) {
    DIR* d = opendir(dir);
    if (!d) return;
    char names[64][32];
    int n = 0;
    while (dirent* e = readdir(d)) {
        const char* s = e->d_name;
        bool region = s[0] == 'r' && s[1] == '.' && std::strstr(s, ".dat");
        bool level = !std::strcmp(s, "level.dat") || !std::strcmp(s, "level.tmp");
        if ((region || level) && std::strlen(s) < sizeof(names[0]) && n < 64) std::strcpy(names[n++], s);
        if (n == 64) {
            closedir(d);
            for (int i = 0; i < n; ++i) {
                char path[128];
                std::snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
                std::remove(path);
            }
            n = 0;
            d = opendir(dir);
            if (!d) return;
        }
    }
    closedir(d);
    for (int i = 0; i < n; ++i) {
        char path[128];
        std::snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
        std::remove(path);
    }
}

// ------------------------------------------------------------------ chunk encoding

namespace {

constexpr uint8_t CHUNK_VERSION = 1;

struct Writer {
    uint8_t* p;
    uint32_t cap, n = 0;
    bool ok = true;
    void byte(uint8_t v) {
        if (n < cap)
            p[n++] = v;
        else
            ok = false;
    }
    void u16(uint16_t v) {
        byte((uint8_t)v);
        byte((uint8_t)(v >> 8));
    }
    void u32(uint32_t v) {
        u16((uint16_t)v);
        u16((uint16_t)(v >> 16));
    }
    void bytes(const uint8_t* d, uint32_t len) {
        if (n + len > cap) {
            ok = false;
            return;
        }
        std::memcpy(p + n, d, len);
        n += len;
    }
    // Control byte < 128: that many + 1 literal bytes follow; >= 128: the next byte repeats (ctrl - 125) times.
    void packed(const uint8_t* in, int len) {
        int i = 0;
        while (i < len) {
            int run = 1;
            while (i + run < len && run < 130 && in[i + run] == in[i]) ++run;
            if (run >= 3) {
                byte((uint8_t)(125 + run));
                byte(in[i]);
                i += run;
                continue;
            }
            int start = i, lit = 0;
            while (i < len && lit < 128) {
                if (i + 2 < len && in[i] == in[i + 1] && in[i] == in[i + 2]) break;
                ++i;
                ++lit;
            }
            byte((uint8_t)(lit - 1));
            bytes(in + start, (uint32_t)lit);
        }
    }
};

struct Reader {
    const uint8_t* p;
    uint32_t size, n = 0;
    bool ok = true;
    uint8_t byte() {
        if (n < size) return p[n++];
        ok = false;
        return 0;
    }
    uint16_t u16() {
        uint16_t a = byte();
        return (uint16_t)(a | (byte() << 8));
    }
    uint32_t u32() {
        uint32_t a = u16();
        return a | ((uint32_t)u16() << 16);
    }
    void bytes(uint8_t* d, uint32_t len) {
        if (n + len > size) {
            ok = false;
            return;
        }
        std::memcpy(d, p + n, len);
        n += len;
    }
    void packed(uint8_t* out, int len) {
        int i = 0;
        while (i < len && ok) {
            int ctrl = byte();
            if (ctrl >= 128) {
                int run = ctrl - 125;
                uint8_t v = byte();
                if (i + run > len) {
                    ok = false;
                    return;
                }
                std::memset(out + i, v, run);
                i += run;
            } else {
                int lit = ctrl + 1;
                if (i + lit > len) {
                    ok = false;
                    return;
                }
                bytes(out + i, (uint32_t)lit);
                i += lit;
            }
        }
    }
};

}  // namespace

bool encodeChunk(const Chunk& c, uint8_t* out, uint32_t cap, uint32_t& size) {
    Writer w{out, cap};
    w.byte(CHUNK_VERSION);
    w.byte(c.flags & (CHUNK_POPULATED | CHUNK_LIT));
    uint8_t mask = 0;
    for (int i = 0; i < World::SECTIONS_Y; ++i)
        if (c.sec[i]) mask |= (uint8_t)(1 << i);
    w.byte(mask);
    w.byte(0);
    w.bytes(c.temperature, 256);
    w.bytes(c.humidity, 256);
    for (int i = 0; i < World::SECTIONS_Y; ++i) {
        const Section* s = c.sec[i];
        if (!s) continue;
        w.packed(s->blocks, 4096);
        w.packed(s->meta, 2048);
        w.packed(s->light, 4096);
    }
    int tiles = 0;
    for (const TileEntity* t = c.tiles; t; t = t->next) ++tiles;
    w.u16((uint16_t)tiles);
    for (const TileEntity* t = c.tiles; t; t = t->next) {
        w.byte((uint8_t)(((t->x & 15) << 4) | (t->z & 15)));
        w.byte((uint8_t)t->y);
        w.byte(t->type);
        w.u16((uint16_t)t->burnTime);
        w.u16((uint16_t)t->burnTotal);
        w.u16((uint16_t)t->cookTime);
        int n = 0;
        for (const ItemStack& s : t->items)
            if (!s.empty()) ++n;
        w.byte((uint8_t)n);
        for (int i = 0; i < 27; ++i) {
            const ItemStack& s = t->items[i];
            if (s.empty()) continue;
            w.byte((uint8_t)i);
            w.u16(s.id);
            w.byte(s.count);
            w.u16(s.damage);
        }
        if (t->type == TILE_SIGN)
            for (int i = 0; i < 4; ++i) {
                int len = (int)strnlen(t->text[i], 15);
                w.byte((uint8_t)len);
                w.bytes((const uint8_t*)t->text[i], len);
            }
    }
    w.u32(c.entityBytes);
    if (c.entityBytes) w.bytes(c.entityData, c.entityBytes);
    size = w.n;
    return w.ok;
}

bool decodeChunk(Chunk& c, const uint8_t* in, uint32_t size) {
    Reader r{in, size};
    if (r.byte() != CHUNK_VERSION) return false;
    c.flags = r.byte() & (CHUNK_POPULATED | CHUNK_LIT);
    uint8_t mask = r.byte();
    r.byte();
    r.bytes(c.temperature, 256);
    r.bytes(c.humidity, 256);
    for (int i = 0; i < World::SECTIONS_Y && r.ok; ++i) {
        if (!(mask & (1 << i))) continue;
        Section* s = (Section*)std::malloc(sizeof(Section));
        if (!s) return false;
        c.sec[i] = s;
        r.packed(s->blocks, 4096);
        r.packed(s->meta, 2048);
        r.packed(s->light, 4096);
    }
    int tiles = r.u16();
    for (int k = 0; k < tiles && r.ok; ++k) {
        TileEntity* t = (TileEntity*)std::calloc(1, sizeof(TileEntity));
        if (!t) return false;
        uint8_t xz = r.byte();
        t->x = c.cx * 16 + (xz >> 4);
        t->z = c.cz * 16 + (xz & 15);
        t->y = r.byte();
        t->type = r.byte();
        t->burnTime = (int16_t)r.u16();
        t->burnTotal = (int16_t)r.u16();
        t->cookTime = (int16_t)r.u16();
        int n = r.byte();
        for (int i = 0; i < n; ++i) {
            int slot = r.byte();
            ItemStack s;
            s.id = r.u16();
            s.count = r.byte();
            s.damage = r.u16();
            if (slot < 27) t->items[slot] = s;
        }
        if (t->type == TILE_SIGN)
            for (int i = 0; i < 4; ++i) {
                int len = r.byte();
                if (len > 15) r.ok = false, len = 0;
                r.bytes((uint8_t*)t->text[i], len);
            }
        t->next = c.tiles;
        c.tiles = t;
    }
    uint32_t eb = r.u32();
    if (eb && r.ok && eb <= size) {
        c.entityData = (uint8_t*)std::malloc(eb);
        if (c.entityData) {
            r.bytes(c.entityData, eb);
            c.entityBytes = eb;
        }
    }
    return r.ok;
}

// ------------------------------------------------------------------ region files

namespace {

enum JobType : uint8_t { JOB_LOAD, JOB_SAVE, JOB_SAVE_RAW, JOB_QUIT };

struct Job {
    uint8_t type;
    int cx, cz;
    void* data;  // Chunk* for JOB_SAVE, encoded bytes for JOB_SAVE_RAW
    uint32_t size;
};

struct Result {
    int cx, cz;
    Chunk* chunk;  // null when loading failed
};

// 16x16 chunks per file; header: per chunk {offset, size, capacity} as u32.
constexpr int REGION_HEADER = 256 * 12;

struct Region {
    int rx = 0, rz = 0;
    FILE* f = nullptr;
    bool used = false;
    bool missing = false;
    uint32_t hdr[256 * 3];
    uint32_t end = REGION_HEADER;
    unsigned lastUse = 0;
};

constexpr int JOB_CAP = 512;
constexpr int RESULT_CAP = 64;
constexpr uint32_t BUF_SIZE = 160 * 1024;

}  // namespace

struct ChunkStreamer::Impl {
    Impl(const char* d, int64_t seed, bool hell) : gen(seed, hell) { std::snprintf(dir, sizeof(dir), "%s", d); }
    char dir[64];
    WorldGenerator gen;
    Job jobs[JOB_CAP];
    int jobHead = 0, jobCount = 0;
    Result results[RESULT_CAP];
    int resHead = 0, resCount = 0;
    volatile int busy = 0;
    bool failed = false;
    uint8_t* buf = nullptr;
    Region regions[2];
    unsigned useCounter = 0;
#ifdef __PSP__
    SceUID mutex = -1, jobSema = -1, thread = -1;
    void lock() { sceKernelWaitSema(mutex, 1, nullptr); }
    void unlock() { sceKernelSignalSema(mutex, 1); }
#else
    void lock() {}
    void unlock() {}
#endif

    Region* region(int rx, int rz, bool create) {
        Region* lru = &regions[0];
        for (Region& r : regions) {
            if (r.used && r.rx == rx && r.rz == rz) {
                if (r.missing && create) {
                    r.used = false;
                    lru = &r;
                    break;
                }
                r.lastUse = ++useCounter;
                return r.missing ? nullptr : &r;
            }
            if (!r.used || r.lastUse < lru->lastUse) lru = &r;
        }
        Region& r = *lru;
        if (r.f) std::fclose(r.f);
        r = Region();
        r.used = true;
        r.rx = rx;
        r.rz = rz;
        r.lastUse = ++useCounter;
        char path[96];
        std::snprintf(path, sizeof(path), "%s/r.%d.%d.dat", dir, rx, rz);
        r.f = std::fopen(path, "r+b");
        if (r.f) {
            if (std::fread(r.hdr, 1, REGION_HEADER, r.f) != (size_t)REGION_HEADER) std::memset(r.hdr, 0, sizeof(r.hdr));
        } else if (create) {
            r.f = std::fopen(path, "w+b");
            if (!r.f) {
                r.used = false;
                return nullptr;
            }
            std::memset(r.hdr, 0, sizeof(r.hdr));
            std::fwrite(r.hdr, 1, REGION_HEADER, r.f);
        } else {
            r.missing = true;
            return nullptr;
        }
        r.end = REGION_HEADER;
        for (int i = 0; i < 256; ++i)
            if (r.hdr[i * 3 + 2] && r.hdr[i * 3] + r.hdr[i * 3 + 2] > r.end) r.end = r.hdr[i * 3] + r.hdr[i * 3 + 2];
        return &r;
    }

    void closeRegions() {
        for (Region& r : regions) {
            if (r.f) std::fclose(r.f);
            r = Region();
        }
    }

    bool readChunk(Chunk& c) {
        Region* r = region(c.cx >> 4, c.cz >> 4, false);
        if (!r) return false;
        int k = ((c.cx & 15) << 4) | (c.cz & 15);
        uint32_t off = r->hdr[k * 3], size = r->hdr[k * 3 + 1];
        if (!size || size > BUF_SIZE) return false;
        if (std::fseek(r->f, (long)off, SEEK_SET) != 0 || std::fread(buf, 1, size, r->f) != size) return false;
        if (decodeChunk(c, buf, size)) return true;
        for (Section*& s : c.sec) {
            std::free(s);
            s = nullptr;
        }
        c.flags = 0;
        return false;
    }

    void writeBytes(int cx, int cz, const uint8_t* data, uint32_t size) {
        Region* r = region(cx >> 4, cz >> 4, true);
        if (!r || !data || !size) return;
        int k = ((cx & 15) << 4) | (cz & 15);
        uint32_t off = r->hdr[k * 3], cap = r->hdr[k * 3 + 2];
        if (size > cap) {
            off = r->end;
            cap = (size + 4095) & ~4095u;
            r->end = off + cap;
        }
        if (std::fseek(r->f, (long)off, SEEK_SET) != 0) return;
        if (std::fwrite(data, 1, size, r->f) != size) return;
        static const uint8_t zeros[512] = {};
        if (off + cap == r->end) {
            for (uint32_t pad = cap - size; pad > 0;) {
                uint32_t n = pad > sizeof(zeros) ? (uint32_t)sizeof(zeros) : pad;
                std::fwrite(zeros, 1, n, r->f);
                pad -= n;
            }
        }
        r->hdr[k * 3] = off;
        r->hdr[k * 3 + 1] = size;
        r->hdr[k * 3 + 2] = cap;
        std::fseek(r->f, k * 12, SEEK_SET);
        std::fwrite(&r->hdr[k * 3], 4, 3, r->f);
        std::fflush(r->f);
    }

    void writeChunk(const Chunk& c) {
        uint32_t size;
        if (!encodeChunk(c, buf, BUF_SIZE, size)) return;
        writeBytes(c.cx, c.cz, buf, size);
    }

    // Returns false for JOB_QUIT.
    bool run(const Job& j) {
        switch (j.type) {
            case JOB_LOAD: {
                Chunk* c = newChunk(j.cx, j.cz);
                if (c && !readChunk(*c) && !gen.provide(*c)) {
                    freeChunk(c);
                    c = nullptr;
                }
                lock();
                if (resCount < RESULT_CAP) {
                    results[(resHead + resCount) % RESULT_CAP] = {j.cx, j.cz, c};
                    ++resCount;
                } else {
                    freeChunk(c);
                }
                unlock();
                return true;
            }
            case JOB_SAVE:
                writeChunk(*(Chunk*)j.data);
                freeChunk((Chunk*)j.data);
                return true;
            case JOB_SAVE_RAW:
                writeBytes(j.cx, j.cz, (const uint8_t*)j.data, j.size);
                std::free(j.data);
                return true;
            default: return false;
        }
    }

    bool pop(Job& j) {
        lock();
        bool ok = jobCount > 0;
        if (ok) {
            j = jobs[jobHead];
            jobHead = (jobHead + 1) % JOB_CAP;
            --jobCount;
            ++busy;
        }
        unlock();
        return ok;
    }

    void done() {
        lock();
        --busy;
        unlock();
    }
};

#ifdef __PSP__
namespace {
int workerMain(SceSize, void* argp) {
    ChunkStreamer::Impl* m = *(ChunkStreamer::Impl**)argp;
    for (;;) {
        sceKernelWaitSema(m->jobSema, 1, nullptr);
        Job j;
        if (!m->pop(j)) continue;
        bool more = m->run(j);
        m->done();
        if (!more) break;
    }
    m->closeRegions();
    return 0;
}
}  // namespace
#endif

ChunkStreamer::ChunkStreamer(const char* dir, int64_t seed, bool hell) : impl_(new Impl(dir, seed, hell)) {
    makeDirectory(dir);
    impl_->buf = (uint8_t*)std::malloc(BUF_SIZE);
}

ChunkStreamer::~ChunkStreamer() {
    finish();
#ifdef __PSP__
    if (impl_->thread >= 0) {
        impl_->lock();
        impl_->jobs[(impl_->jobHead + impl_->jobCount) % JOB_CAP] = {JOB_QUIT, 0, 0, nullptr};
        ++impl_->jobCount;
        impl_->unlock();
        sceKernelSignalSema(impl_->jobSema, 1);
        sceKernelWaitThreadEnd(impl_->thread, nullptr);
        sceKernelDeleteThread(impl_->thread);
    }
    if (impl_->jobSema >= 0) sceKernelDeleteSema(impl_->jobSema);
    if (impl_->mutex >= 0) sceKernelDeleteSema(impl_->mutex);
#endif
    impl_->closeRegions();
    for (int i = 0; i < impl_->resCount; ++i) freeChunk(impl_->results[(impl_->resHead + i) % RESULT_CAP].chunk);
    std::free(impl_->buf);
    delete impl_;
}

bool ChunkStreamer::start() {
    if (!impl_->buf) return false;
#ifdef __PSP__
    impl_->mutex = sceKernelCreateSema("chunk_mutex", 0, 1, 1, nullptr);
    impl_->jobSema = sceKernelCreateSema("chunk_jobs", 0, 0, JOB_CAP + 1, nullptr);
    // Lower priority than the main thread (32): runs while the main thread waits for vblank or the GE.
    impl_->thread = sceKernelCreateThread("chunk_worker", workerMain, 0x30, 128 * 1024, PSP_THREAD_ATTR_USER, nullptr);
    if (impl_->mutex < 0 || impl_->jobSema < 0 || impl_->thread < 0) return false;
    Impl* p = impl_;
    sceKernelStartThread(impl_->thread, sizeof(p), &p);
#endif
    return true;
}

static void pushJob(ChunkStreamer::Impl* m, const Job& j, ChunkStreamer* s) {
    bool noted = false;
    for (;;) {
        m->lock();
        if (m->jobCount < JOB_CAP) {
            m->jobs[(m->jobHead + m->jobCount) % JOB_CAP] = j;
            ++m->jobCount;
            m->unlock();
            break;
        }
        m->unlock();
        if (!noted) {
            crashNote("job-queue-full");
            noted = true;
        }
#ifdef __PSP__
        platformSleepMs(2);
#else
        s->pump();
#endif
    }
#ifdef __PSP__
    sceKernelSignalSema(m->jobSema, 1);
#endif
    (void)s;
}

void ChunkStreamer::requestLoad(int cx, int cz) {
    if (pendingCount_ >= 64) return;
    pending_[pendingCount_++] = {cx, cz};
    pushJob(impl_, {JOB_LOAD, cx, cz, nullptr}, this);
}

void ChunkStreamer::requestSave(Chunk* c) { pushJob(impl_, {JOB_SAVE, c->cx, c->cz, c, 0}, this); }

void ChunkStreamer::requestSaveEncoded(int cx, int cz, uint8_t* data, uint32_t size) {
    pushJob(impl_, {JOB_SAVE_RAW, cx, cz, data, size}, this);
}

int ChunkStreamer::backlog() {
    impl_->lock();
    int n = impl_->jobCount + impl_->busy;
    impl_->unlock();
    return n;
}

bool ChunkStreamer::pending(int cx, int cz) const {
    for (int i = 0; i < pendingCount_; ++i)
        if (pending_[i].cx == cx && pending_[i].cz == cz) return true;
    return false;
}

Chunk* ChunkStreamer::takeLoaded() {
    for (;;) {
        impl_->lock();
        if (!impl_->resCount) {
            impl_->unlock();
            return nullptr;
        }
        Result r = impl_->results[impl_->resHead];
        impl_->resHead = (impl_->resHead + 1) % RESULT_CAP;
        --impl_->resCount;
        impl_->unlock();
        for (int i = 0; i < pendingCount_; ++i)
            if (pending_[i].cx == r.cx && pending_[i].cz == r.cz) {
                pending_[i] = pending_[--pendingCount_];
                break;
            }
        if (r.chunk) return r.chunk;
        impl_->failed = true;
    }
}

bool ChunkStreamer::failed() {
    bool f = impl_->failed;
    impl_->failed = false;
    return f;
}

void ChunkStreamer::pump() {
#ifndef __PSP__
    Job j;
    while (impl_->pop(j)) {
        impl_->run(j);
        impl_->done();
    }
#endif
}

void ChunkStreamer::finish() {
#ifdef __PSP__
    for (;;) {
        impl_->lock();
        bool idle = impl_->jobCount == 0 && impl_->busy == 0;
        impl_->unlock();
        if (idle) break;
        platformSleepMs(2);
    }
#else
    pump();
#endif
}
