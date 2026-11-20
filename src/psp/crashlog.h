#pragma once

// crash.log is created next to EBOOT.PBP as soon as the game starts and rewritten
// every two seconds. A freeze adds a "frozen" section after about five seconds.

namespace crashlog {

struct FrameInfo {
    const char* state = "";
    int chunks = 0;
    int worldOom = 0;
    int renderOom = 0;
    int backlog = 0;
    int inFlight = 0;
    int freeKb = 0;
    int topKb = 0;
    int kernelKb = 0;
    int usedKb = 0;
    int renderDistance = 0;
    int fpsMilli = 0;
    int cpuPct = 0;
    int gpuPct = 0;
    // Average microseconds per frame, over the last second: game tick, chunk light, mesh, draw.
    int tickUs = 0, streamUs = 0, meshUs = 0, drawUs = 0;
    float x = 0, y = 0, z = 0;
};

void install();
// Largest of 8, 3, 2, 1.25, 0.75 and 0.25 MB that malloc can really hand out.
// The result is capped at 8192. GCC -O2 deleted the previous probe and the
// function compiled to "return 24576", which a 32 MB PSP cannot have free.
int memoryFreeKb();
void note(const char* phase);
void setFrame(const FrameInfo& info);
void heartbeat();
// A save that is allowed to block. Stalls are still reported, after a longer wait.
void expectLong(bool on);

}  // namespace crashlog
