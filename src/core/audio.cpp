#include "audio.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace audio {
namespace {

// ------------------------------------------------------------------ IMA ADPCM (WAV block layout)

const int16_t IMA_STEP[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
    544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
    9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
const int8_t IMA_INDEX[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

struct Ima {
    int pred, index;
    void init(const uint8_t* h) {
        pred = (int16_t)(h[0] | (h[1] << 8));
        index = h[2] > 88 ? 88 : h[2];
    }
    int16_t next(int n) {
        int step = IMA_STEP[index];
        int diff = step >> 3;
        if (n & 1) diff += step >> 2;
        if (n & 2) diff += step >> 1;
        if (n & 4) diff += step;
        pred += (n & 8) ? -diff : diff;
        if (pred > 32767) pred = 32767;
        if (pred < -32768) pred = -32768;
        index += IMA_INDEX[n];
        if (index < 0) index = 0;
        if (index > 88) index = 88;
        return (int16_t)pred;
    }
};

int decodeMono(const uint8_t* b, int bytes, int16_t* out) {
    Ima s;
    s.init(b);
    out[0] = (int16_t)s.pred;
    int n = 1;
    for (int i = 4; i < bytes; ++i) {
        out[n++] = s.next(b[i] & 15);
        out[n++] = s.next(b[i] >> 4);
    }
    return n;
}

// Interleaved L/R output; returns frames.
int decodeStereo(const uint8_t* b, int bytes, int16_t* out) {
    Ima l, r;
    l.init(b);
    r.init(b + 4);
    out[0] = (int16_t)l.pred;
    out[1] = (int16_t)r.pred;
    int frames = 1;
    for (int i = 8; i + 8 <= bytes; i += 8) {
        for (int k = 0; k < 4; ++k) {
            out[(frames + 2 * k) * 2] = l.next(b[i + k] & 15);
            out[(frames + 2 * k + 1) * 2] = l.next(b[i + k] >> 4);
            out[(frames + 2 * k) * 2 + 1] = r.next(b[i + 4 + k] & 15);
            out[(frames + 2 * k + 1) * 2 + 1] = r.next(b[i + 4 + k] >> 4);
        }
        frames += 8;
    }
    return frames;
}

constexpr int MAX_BLOCK_FRAMES = 4096;

// ------------------------------------------------------------------ bank

struct Entry {
    char name[32];
    const uint8_t* data;
    uint32_t bytes, samples;
    uint16_t rate, align;
};

uint8_t* g_bank = nullptr;
int g_bankBytes = 0;
Entry* g_entries = nullptr;
int g_entryCount = 0;

inline uint32_t rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
inline uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

// ------------------------------------------------------------------ voices

struct Voice {
    const Entry* e = nullptr;
    uint32_t block = 0, left = 0;  // next block, samples not yet decoded
    int16_t buf[MAX_BLOCK_FRAMES];
    int len = 0, pos = 0;
    int s0 = 0, s1 = 0;
    uint32_t frac = 0, step = 0;
    int gl = 0, gr = 0;  // Q12
    bool ending = false;
    uint32_t serial = 0;

    bool pull(int& s) {
        if (pos >= len) {
            if (!left) return false;
            uint32_t off = block * e->align;
            uint32_t bytes = e->bytes - off < e->align ? e->bytes - off : e->align;
            len = decodeMono(e->data + off, (int)bytes, buf);
            if ((uint32_t)len > left) len = (int)left;
            left -= (uint32_t)len;
            ++block;
            pos = 0;
        }
        s = buf[pos++];
        return true;
    }
};

Voice g_voices[MAX_VOICES];
uint32_t g_serial = 0;

// ------------------------------------------------------------------ music stream

struct Music {
    FILE* f = nullptr;
    char vbuf[16384];
    int channels = 0, align = 0;
    uint32_t left = 0;  // frames not yet decoded
    uint8_t block[MAX_BLOCK_FRAMES];
    int16_t pcm[MAX_BLOCK_FRAMES * 2];
    int len = 0, pos = 0;
    int l0 = 0, r0 = 0, l1 = 0, r1 = 0;
    uint32_t frac = 0, step = 0;

    void close() {
        if (f) std::fclose(f);
        f = nullptr;
    }
    bool open(const char* path) {
        close();
        f = std::fopen(path, "rb");
        if (!f) return false;
        std::setvbuf(f, vbuf, _IOFBF, sizeof(vbuf));
        uint8_t h[12];
        if (std::fread(h, 1, 12, f) != 12 || std::memcmp(h, "RIFF", 4) || std::memcmp(h + 8, "WAVE", 4)) {
            close();
            return false;
        }
        int rate = 0;
        left = 0;
        for (;;) {
            uint8_t c[8];
            if (std::fread(c, 1, 8, f) != 8) break;
            uint32_t size = rd32(c + 4);
            if (!std::memcmp(c, "fmt ", 4) && size >= 16) {
                uint8_t fmt[20] = {};
                std::fread(fmt, 1, size < 20 ? size : 20, f);
                if (size > 20) std::fseek(f, (long)(size - 20), SEEK_CUR);
                if (rd16(fmt) != 0x11) break;
                channels = rd16(fmt + 2);
                rate = (int)rd32(fmt + 4);
                align = rd16(fmt + 12);
            } else if (!std::memcmp(c, "fact", 4)) {
                uint8_t v[4];
                std::fread(v, 1, 4, f);
                left = rd32(v);
                if (size > 4) std::fseek(f, (long)(size - 4), SEEK_CUR);
            } else if (!std::memcmp(c, "data", 4)) {
                if (channels < 1 || channels > 2 || align <= 0 || align > (int)sizeof(block) || !rate) break;
                if (!left) left = size / align * ((align - 4 * channels) * 2 / channels + 1);
                step = (uint32_t)((double)rate / OUTPUT_RATE * 65536.0);
                len = pos = 0;
                frac = 0;
                l0 = r0 = l1 = r1 = 0;
                return true;
            } else {
                std::fseek(f, (long)(size + (size & 1)), SEEK_CUR);
            }
        }
        close();
        return false;
    }
    bool pull(int& l, int& r) {
        if (pos >= len) {
            if (!f || !left) return false;
            int got = (int)std::fread(block, 1, (size_t)align, f);
            if (got < 4 * channels) return false;
            if (channels == 2) {
                len = decodeStereo(block, got, pcm);
            } else {
                int16_t mono[MAX_BLOCK_FRAMES];
                len = decodeMono(block, got, mono);
                for (int i = 0; i < len; ++i) pcm[i * 2] = pcm[i * 2 + 1] = mono[i];
            }
            if ((uint32_t)len > left) len = (int)left;
            left -= (uint32_t)len;
            pos = 0;
        }
        l = pcm[pos * 2];
        r = pcm[pos * 2 + 1];
        ++pos;
        return true;
    }
};

Music g_music;
volatile int g_musicGain = 4096;  // Q12
volatile bool g_musicActive = false;

// ------------------------------------------------------------------ command queue (single producer/consumer)

enum CmdType : uint8_t { CMD_PLAY, CMD_MUSIC, CMD_STOP_MUSIC };
struct Cmd {
    uint8_t type;
    int16_t entry;
    int16_t gl, gr;
    uint32_t step;
    char path[64];
};
constexpr uint32_t QSIZE = 32;
Cmd g_cmds[QSIZE];
volatile uint32_t g_qHead = 0, g_qTail = 0;

bool enqueue(const Cmd& c) {
    uint32_t h = g_qHead;
    if (h - g_qTail >= QSIZE) return false;
    g_cmds[h % QSIZE] = c;
    __asm__ __volatile__("" ::: "memory");
    g_qHead = h + 1;
    return true;
}

void startVoice(const Cmd& c) {
    Voice* v = nullptr;
    for (Voice& x : g_voices)
        if (!x.e) {
            v = &x;
            break;
        }
    if (!v) {
        v = &g_voices[0];
        for (Voice& x : g_voices)
            if (x.serial < v->serial) v = &x;
    }
    v->e = &g_entries[c.entry];
    v->block = 0;
    v->left = v->e->samples;
    v->len = v->pos = 0;
    v->frac = 0;
    v->step = c.step;
    v->gl = c.gl;
    v->gr = c.gr;
    v->ending = false;
    v->serial = ++g_serial;
    v->s0 = 0;
    if (!v->pull(v->s1)) v->e = nullptr;
}

void drainCommands() {
    while (g_qTail != g_qHead) {
        const Cmd& c = g_cmds[g_qTail % QSIZE];
        switch (c.type) {
            case CMD_PLAY:
                if (c.entry >= 0 && c.entry < g_entryCount) startVoice(c);
                break;
            case CMD_MUSIC:
                if (!g_music.open(c.path)) g_musicActive = false;
                break;
            case CMD_STOP_MUSIC:
                g_music.close();
                g_musicActive = false;
                break;
        }
        __asm__ __volatile__("" ::: "memory");
        g_qTail = g_qTail + 1;
    }
}

int toQ12(float g) {
    if (g < 0) g = 0;
    if (g > 4) g = 4;
    return (int)(g * 4096.0f);
}

}  // namespace

bool loadBank(const char* path) {
    freeBank();
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size < 8) {
        std::fclose(f);
        return false;
    }
    g_bank = (uint8_t*)std::malloc((size_t)size);
    bool ok = g_bank && std::fread(g_bank, 1, (size_t)size, f) == (size_t)size;
    std::fclose(f);
    if (!ok || std::memcmp(g_bank, "MCSP", 4)) {
        freeBank();
        return false;
    }
    int n = (int)rd32(g_bank + 4);
    if (8 + (long)n * 48 > size) {
        freeBank();
        return false;
    }
    g_entries = (Entry*)std::calloc((size_t)n, sizeof(Entry));
    if (!g_entries) {
        freeBank();
        return false;
    }
    for (int i = 0; i < n; ++i) {
        const uint8_t* h = g_bank + 8 + i * 48;
        Entry& e = g_entries[i];
        std::memcpy(e.name, h, 31);
        uint32_t off = rd32(h + 32);
        e.bytes = rd32(h + 36);
        e.samples = rd32(h + 40);
        e.rate = rd16(h + 44);
        e.align = rd16(h + 46);
        if (off + e.bytes > (uint32_t)size || e.align < 8 || (e.align - 4) * 2 + 1 > MAX_BLOCK_FRAMES) {
            freeBank();
            return false;
        }
        e.data = g_bank + off;
    }
    g_entryCount = n;
    g_bankBytes = (int)size;
    return true;
}

void freeBank() {
    for (Voice& v : g_voices) v.e = nullptr;
    std::free(g_entries);
    std::free(g_bank);
    g_entries = nullptr;
    g_bank = nullptr;
    g_entryCount = 0;
    g_bankBytes = 0;
}

int bankBytes() { return g_bankBytes; }

int findGroup(const char* name, int& count) {
    int lo = 0, hi = g_entryCount;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (std::strcmp(g_entries[mid].name, name) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    int end = lo;
    while (end < g_entryCount && !std::strcmp(g_entries[end].name, name)) ++end;
    count = end - lo;
    return lo;
}

void play(int entry, float left, float right, float pitch) {
    if (entry < 0 || entry >= g_entryCount) return;
    if (pitch < 0.5f) pitch = 0.5f;
    if (pitch > 2.0f) pitch = 2.0f;
    Cmd c{};
    c.type = CMD_PLAY;
    c.entry = (int16_t)entry;
    c.gl = (int16_t)toQ12(left);
    c.gr = (int16_t)toQ12(right);
    if (!c.gl && !c.gr) return;
    c.step = (uint32_t)((double)g_entries[entry].rate * pitch / OUTPUT_RATE * 65536.0);
    enqueue(c);
}

void playMusic(const char* path, float volume) {
    Cmd c{};
    c.type = CMD_MUSIC;
    std::strncpy(c.path, path, sizeof(c.path) - 1);
    setMusicVolume(volume);
    if (enqueue(c)) g_musicActive = true;
}

void stopMusic() {
    Cmd c{};
    c.type = CMD_STOP_MUSIC;
    enqueue(c);
}

void setMusicVolume(float volume) { g_musicGain = toQ12(volume); }

bool musicActive() { return g_musicActive; }

void mix(int16_t* out, int frames) {
    drainCommands();
    static int32_t acc[MAX_MIX_FRAMES * 2];
    while (frames > 0) {
        int n = frames > MAX_MIX_FRAMES ? MAX_MIX_FRAMES : frames;
        std::memset(acc, 0, sizeof(int32_t) * 2 * (size_t)n);
        for (Voice& v : g_voices) {
            if (!v.e) continue;
            for (int i = 0; i < n; ++i) {
                int s = v.s0 + (int)(((int64_t)(v.s1 - v.s0) * (int64_t)v.frac) >> 16);
                acc[i * 2] += s * v.gl;
                acc[i * 2 + 1] += s * v.gr;
                v.frac += v.step;
                while (v.frac >= 65536) {
                    v.frac -= 65536;
                    v.s0 = v.s1;
                    if (v.ending) {
                        v.e = nullptr;
                        break;
                    }
                    if (!v.pull(v.s1)) {
                        v.s1 = 0;
                        v.ending = true;
                    }
                }
                if (!v.e) break;
            }
        }
        if (g_music.f) {
            int g = g_musicGain;
            for (int i = 0; i < n; ++i) {
                int l = g_music.l0 + (int)(((int64_t)(g_music.l1 - g_music.l0) * g_music.frac) >> 16);
                int r = g_music.r0 + (int)(((int64_t)(g_music.r1 - g_music.r0) * g_music.frac) >> 16);
                acc[i * 2] += l * g;
                acc[i * 2 + 1] += r * g;
                g_music.frac += g_music.step;
                while (g_music.frac >= 65536) {
                    g_music.frac -= 65536;
                    g_music.l0 = g_music.l1;
                    g_music.r0 = g_music.r1;
                    if (!g_music.pull(g_music.l1, g_music.r1)) {
                        g_music.close();
                        g_musicActive = false;
                        break;
                    }
                }
                if (!g_music.f) break;
            }
        }
        for (int i = 0; i < n * 2; ++i) {
            int s = acc[i] >> 12;
            out[i] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
        }
        out += n * 2;
        frames -= n;
    }
}

}  // namespace audio
