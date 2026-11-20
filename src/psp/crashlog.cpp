#include "crashlog.h"

#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <psprtc.h>
#include <pspthreadman.h>

#include <unistd.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr int CRUMBS = 24;
constexpr int CRUMB_LEN = 48;

struct Crumb {
    char text[CRUMB_LEN];
};

Crumb g_crumbs[CRUMBS];
volatile int g_crumbCount = 0;
volatile uint32_t g_beat = 0;
volatile int g_expectLong = 0;
crashlog::FrameInfo g_frame;
int g_mainThread = -1;
char g_path[192] = "crash.log";
uint64_t g_lastFlush = 0;

void rememberPath() {
    char cwd[160];
    if (!getcwd(cwd, sizeof(cwd)) || !cwd[0]) return;
    std::snprintf(g_path, sizeof(g_path), "%s/crash.log", cwd);
}

struct Buf {
    char d[4096];
    int n = 0;
};

void bputs(Buf& b, const char* s) {
    if (!s) return;
    while (*s && b.n < (int)sizeof(b.d) - 1) b.d[b.n++] = *s++;
}

void bfmt(Buf& b, const char* fmt, ...) {
    int room = (int)sizeof(b.d) - b.n;
    if (room <= 1) return;
    va_list ap;
    va_start(ap, fmt);
    int r = std::vsnprintf(b.d + b.n, (size_t)room, fmt, ap);
    va_end(ap);
    if (r <= 0) return;
    b.n += r >= room ? room - 1 : r;
}

const char* threadStatus(int status) {
    switch (status) {
        case PSP_THREAD_RUNNING: return "running";
        case PSP_THREAD_READY: return "ready";
        case PSP_THREAD_WAITING: return "waiting";
        case PSP_THREAD_SUSPEND: return "suspended";
        case PSP_THREAD_STOPPED: return "stopped";
        case PSP_THREAD_KILLED: return "killed (stack overflow)";
        default: return "other";
    }
}

void writeCrumbs(Buf& b) {
    int count = g_crumbCount;
    if (count < 0) count = 0;
    int start = count > CRUMBS ? count - CRUMBS : 0;
    bputs(b, "breadcrumbs (oldest first):\n");
    if (start == count) bputs(b, "  (none)\n");
    for (int i = start; i < count; ++i) bfmt(b, "  %s\n", g_crumbs[i % CRUMBS].text);
}

void writeFrame(Buf& b) {
    bfmt(b, "state: %s\n", g_frame.state ? g_frame.state : "");
    bfmt(b, "player: %.2f %.2f %.2f\n", g_frame.x, g_frame.y, g_frame.z);
    bfmt(b, "chunks: %d  renderDistance: %d  fps: %d.%03d\n", g_frame.chunks, g_frame.renderDistance,
         g_frame.fpsMilli / 1000, g_frame.fpsMilli % 1000);
    bfmt(b, "cpu: %d%%  gpu: %d%%\n", g_frame.cpuPct, g_frame.gpuPct);
    bfmt(b, "frame ms: tick %d.%d  gen %d.%d  mesh %d.%d  draw %d.%d\n", g_frame.tickUs / 1000,
         (g_frame.tickUs % 1000) / 100, g_frame.streamUs / 1000, (g_frame.streamUs % 1000) / 100,
         g_frame.meshUs / 1000, (g_frame.meshUs % 1000) / 100, g_frame.drawUs / 1000,
         (g_frame.drawUs % 1000) / 100);
    bfmt(b, "oom: world %d  meshes %d\n", g_frame.worldOom, g_frame.renderOom);
    bfmt(b, "saveBacklog: %d  loadsInFlight: %d\n", g_frame.backlog, g_frame.inFlight);
    bfmt(b, "heap: %d KB used, probe %d KB, arena free %d KB, outside %d KB\n", g_frame.usedKb, g_frame.freeKb,
         g_frame.topKb, g_frame.kernelKb);
}

void writeThreads(Buf& b) {
    SceUID ids[16];
    int count = 0;
    if (sceKernelGetThreadmanIdList(SCE_KERNEL_TMID_Thread, ids, 16, &count) < 0) return;
    if (count > 16) count = 16;
    bputs(b, "threads:\n");
    for (int i = 0; i < count; ++i) {
        SceKernelThreadInfo info;
        std::memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelReferThreadStatus(ids[i], &info) < 0) continue;
        bfmt(b, "  %s  %s  pri %d  wait %d id %d%s\n", info.name, threadStatus(info.status), info.currentPriority,
             info.waitType, (int)info.waitId, ids[i] == g_mainThread ? "  (main)" : "");
    }
}

void writeClock(Buf& b) {
    uint64_t us = sceKernelGetSystemTimeWide();
    bfmt(b, "uptime: %u.%03u s\n", (unsigned)(us / 1000000ull), (unsigned)((us / 1000ull) % 1000ull));
    ScePspDateTime t;
    if (sceRtcGetCurrentClockLocalTime(&t) >= 0)
        bfmt(b, "clock: %04d-%02d-%02d %02d:%02d:%02d\n", t.year, t.month, t.day, t.hour, t.minute, t.second);
}

// fopen is the same path options.txt already uses. sceIoSync pushes it onto the memory stick
// before a reset. The absolute path is for the watchdog, which must not take the malloc lock.
void writeFile(const Buf& b, bool append) {
    FILE* f = std::fopen(g_path, append ? "ab" : "wb");
    if (!f) f = std::fopen("crash.log", append ? "ab" : "wb");
    if (f) {
        std::fwrite(b.d, 1, b.n, f);
        std::fflush(f);
        std::fclose(f);
    } else {
        SceUID fd = sceIoOpen(g_path, PSP_O_WRONLY | PSP_O_CREAT | (append ? PSP_O_APPEND : PSP_O_TRUNC), 0777);
        if (fd < 0) fd = sceIoOpen("crash.log", PSP_O_WRONLY | PSP_O_CREAT | (append ? PSP_O_APPEND : PSP_O_TRUNC), 0777);
        if (fd >= 0) {
            sceIoWrite(fd, b.d, b.n);
            sceIoClose(fd);
        }
    }
    sceIoSync("ms0:", 0);
}

void fillSnapshot(Buf& b, const char* heading) {
    b.n = 0;
    bputs(b, heading);
    writeClock(b);
    writeFrame(b);
    writeCrumbs(b);
    writeThreads(b);
}

}  // namespace

// Strong definition. The copy in chunkio.cpp is weak so the host build can omit this file.
void crashNote(const char* phase) { crashlog::note(phase); }

namespace crashlog {

int memoryFreeKb() {
    // A volatile function pointer stops GCC from deleting the malloc/free pair
    // and pretending the allocation worked. Probing more than twice a second
    // walks the heap on the main thread and keeps the chunk worker off the CPU.
    static int cached = -1;
    static uint32_t cachedAt = 0;
    static int round = 0;
    uint32_t now = sceKernelGetSystemTimeLow();
    if (cached >= 0 && now - cachedAt < 500000) return cached;
    using Alloc = void* (*)(size_t);
    using Free = void (*)(void*);
    static Alloc allocSlot = std::malloc;
    static Free freeSlot = std::free;
    Alloc alloc = *static_cast<Alloc volatile*>(&allocSlot);
    Free release = *static_cast<Free volatile*>(&freeSlot);
    static const int kTry[] = {8192, 3072, 2048, 1280, 768, 256};
    int start = 0;
    if (cached >= 256) {
        while (start < 5 && kTry[start] > cached) ++start;
        // Every fourth probe, try one size up so memory that has been freed is noticed.
        if ((round & 3) == 0 && start > 0) --start;
    }
    ++round;
    int found = 0;
    for (int i = start; i < 6; ++i) {
        int kb = kTry[i];
        size_t bytes = (size_t)kb * 1024;
        void* p = alloc(bytes);
        if (!p) continue;
        auto* b = static_cast<volatile unsigned char*>(p);
        b[0] = 1;
        b[bytes - 1] = 1;
        release(p);
        found = kb;
        break;
    }
    cached = found;
    cachedAt = now;
    return found;
}

int watchdog(SceSize, void*) {
    int ticks = 0;
    uint32_t seen = 0;
    int armed = 0;
    int longMode = 0;
    for (;;) {
        sceKernelDelayThread(500 * 1000);
        if (g_expectLong != longMode) {
            longMode = g_expectLong;
            ticks = 0;
        }
        uint32_t beat = g_beat;
        if (beat != seen) {
            seen = beat;
            ticks = 0;
            armed = 0;
            continue;
        }
        if (beat == 0) continue;
        ++ticks;
        int limit = longMode ? 60 : 8;  // 30 s while saving, otherwise 4 s
        if (ticks < limit || armed) continue;
        armed = 1;
        static Buf b;
        fillSnapshot(b, "\n==== frozen ====\nThe main thread stopped finishing frames.\n");
        SceUID fd = sceIoOpen(g_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
        if (fd < 0) fd = sceIoOpen("crash.log", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
        if (fd >= 0) {
            sceIoWrite(fd, b.d, b.n);
            sceIoClose(fd);
            sceIoSync("ms0:", 0);
        }
    }
    return 0;
}

void install() {
    g_mainThread = sceKernelGetThreadId();
    rememberPath();
    Buf b;
    fillSnapshot(b, "==== running ====\n");
    writeFile(b, false);
    int th = sceKernelCreateThread("crash_watch", watchdog, 0x18, 16 * 1024, PSP_THREAD_ATTR_USER, nullptr);
    if (th >= 0) sceKernelStartThread(th, 0, nullptr);
}

void note(const char* phase) {
    if (!phase || !phase[0]) return;
    int count = g_crumbCount;
    if (count > 0) {
        const char* prev = g_crumbs[(count - 1) % CRUMBS].text;
        const char* a = prev;
        const char* b = phase;
        while (*a && *a == *b) ++a, ++b;
        if (*a == *b) return;
    }
    Crumb& c = g_crumbs[count % CRUMBS];
    int i = 0;
    for (; phase[i] && i < CRUMB_LEN - 1; ++i) c.text[i] = phase[i];
    c.text[i] = 0;
    g_crumbCount = count + 1;
}

void setFrame(const FrameInfo& info) { g_frame = info; }

void heartbeat() {
    ++g_beat;
    uint64_t us = sceKernelGetSystemTimeWide();
    if (g_lastFlush && us - g_lastFlush < 2000000ull) return;
    g_lastFlush = us;
    Buf b;
    fillSnapshot(b, "==== running ====\n");
    writeFile(b, false);
}

void expectLong(bool on) { g_expectLong = on ? 1 : 0; }

}  // namespace crashlog
