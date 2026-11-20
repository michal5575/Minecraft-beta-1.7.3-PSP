#pragma once
#include <cstdint>

#include "world.h"

double platformNow();  // seconds, monotonic
void platformSleepMs(int ms);
// Recent-phase breadcrumb for the PSP crash log. Empty on the host build.
void crashNote(const char* phase);
bool makeDirectory(const char* path);
bool fileExists(const char* path);
void removeWorldFiles(const char* dir);  // region files and level.dat

bool encodeChunk(const Chunk& c, uint8_t* out, uint32_t cap, uint32_t& size);
bool decodeChunk(Chunk& c, const uint8_t* in, uint32_t size);

// Loads and saves chunks on a background thread (PSP) or synchronously in pump() (host).
// Loading generates terrain when the chunk is not in the region files.
class ChunkStreamer {
public:
    ChunkStreamer(const char* dir, int64_t seed, bool hell = false);
    ~ChunkStreamer();
    bool start();

    void requestLoad(int cx, int cz);
    void requestSave(Chunk* c);  // takes ownership; the chunk is freed after writing
    // Takes ownership of an already encoded chunk (the bytes are freed after writing).
    void requestSaveEncoded(int cx, int cz, uint8_t* data, uint32_t size);
    bool pending(int cx, int cz) const;
    int inFlight() const { return pendingCount_; }
    // Jobs waiting or running on the worker, including saves that still hold their buffer.
    int backlog();
    Chunk* takeLoaded();
    bool failed();
    void pump();    // runs queued jobs when there is no worker thread
    void finish();  // blocks until every queued job is done

    struct Impl;

private:
    Impl* impl_;
    struct Pending {
        int cx, cz;
    } pending_[64];
    int pendingCount_ = 0;
};
