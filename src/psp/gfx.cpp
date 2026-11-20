#include "gfx.h"

#include <pspdisplay.h>
#include <pspge.h>
#include <pspgu.h>
#include <pspkernel.h>
#include <psputils.h>

#include <malloc.h>
#include <zlib.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "../assets.h"
#include "../core/chunkio.h"
#include "../core/jrandom.h"
#include "../core/mathhelper.h"
#include "../core/mesher.h"

namespace gfx {

Texture terrain, gui, font, crosshair, background, logo;
Texture items, icons, invGui, craftGui, furnaceGui, chestGui, dispenserGui, sun, moon, arrows;
Texture art, sign, particles, rain, snow, clouds;
Texture mobs[MOB_TEXTURE_COUNT];

namespace {

constexpr int BUF_W = 512;
constexpr int FB_SIZE = BUF_W * SCREEN_H * 2;
constexpr int TERRAIN_VRAM = FB_SIZE * 3;

// Commands and vertex data share this list. Rain, clouds and a crowd of mobs together overflow 256 KB,
// and sceGuGetMemory does not check the end: on a real PSP that corrupts whatever follows and crashes.
unsigned int __attribute__((aligned(16))) g_list[0x40000];

struct V2 {
    float u, v;
    uint32_t color;
    float x, y, z;
};
constexpr int V2_TYPE = GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D;

struct V3 {
    float x, y, z;
};

const Texture* g_bound = nullptr;
bool g_texturing = false;
bool g_busy = false;     // a finished list may still be running on the GE
bool g_unshown = false;  // the frame drawn last has not been swapped in yet
volatile uint32_t g_submitUs = 0;
volatile uint32_t g_doneUs = 0;
uint32_t g_lastGpuUs = 0;
uint32_t g_sleptUs = 0;

// The GE calls this when the list submitted in endFrame has been drawn.
extern "C" void gfxGeFinished(int, void*) { g_doneUs = sceKernelGetSystemTimeLow(); }
uint8_t* g_vramTop = nullptr;

bool inVram(const void* p) {
    uintptr_t a = (uintptr_t)p;
    uintptr_t base = (uintptr_t)sceGeEdramGetAddr();
    return a >= base && a < base + sceGeEdramGetSize();
}

// The GE reads VRAM uncached. A data-cache writeback of a VRAM address faults on a real PSP-1000
// (PPSSPP treats the call as a no-op) and corrupts unrelated cache lines.
void flushForGe(const void* p, unsigned bytes) {
    if (p && bytes && !inVram(p)) sceKernelDcacheWritebackRange(p, bytes);
}

// Copies into the next free VRAM bytes, or returns `src` when it does not fit.
const void* place(const void* src, uint32_t size) {
    if (!src || !g_vramTop || !size) return src;
    uintptr_t base = (uintptr_t)sceGeEdramGetAddr();
    uintptr_t top = (uintptr_t)g_vramTop;
    if (top < base || top + size > base + sceGeEdramGetSize()) return src;
    std::memcpy(g_vramTop, src, size);
    const void* dst = g_vramTop;
    g_vramTop += (size + 15) & ~15u;
    return dst;
}

// Inflates a zlib asset into VRAM. zlib writes through the CPU cache, so the bytes land in RAM first
// and are copied; decompressing straight into uncached VRAM crashes on a real PSP.
const void* toVram(const uint8_t* z, const uint8_t* end, uint32_t size) {
    uint8_t* tmp = (uint8_t*)memalign(16, size);
    uLongf len = size;
    if (!tmp || uncompress(tmp, &len, z, (uLong)(end - z)) != Z_OK || len != size) {
        std::free(tmp);
        return nullptr;
    }
    const void* dst = place(tmp, size);
    if (dst != tmp) std::free(tmp);
    return dst;
}

// Vertex data and commands share g_list. sceGuGetMemory never checks the end, so a busy frame
// (rain, clouds, a crowd of mobs) writes past it and the game dies a moment later.
int g_listUsed = 0;
int g_listMargin = 0;
bool g_listBlocked = false;
constexpr int LIST_LIMIT = (int)sizeof(g_list) - 96 * 1024;

void* guMem(int bytes) {
    if (bytes <= 0) return nullptr;
    int need = (bytes + 31) & ~15;  // alignment, plus the jump the GE inserts on a 256 KB boundary
    // Matrix and draw commands sit in this same list and were not counted. A crowd of mobs
    // walked past the end of it and locked the GE.
    need += 256;
    if (need < bytes || g_listUsed > LIST_LIMIT - g_listMargin - need) {
        g_listBlocked = true;
        return nullptr;
    }
    g_listUsed += need;
    return sceGuGetMemory(bytes);
}

V2* alloc2(int n) { return (V2*)guMem(n * (int)sizeof(V2)); }

void texturing(bool on) {
    if (on == g_texturing) return;
    g_texturing = on;
    if (on)
        sceGuEnable(GU_TEXTURE_2D);
    else
        sceGuDisable(GU_TEXTURE_2D);
}

inline void setV(V2& v, float u, float vv, uint32_t c, float x, float y) {
    v.u = u;
    v.v = vv;
    v.color = c;
    v.x = x;
    v.y = y;
    v.z = 0;
}

uint32_t scaleColor(uint32_t c, float f) {
    int r = (int)((c & 255) * f), g = (int)(((c >> 8) & 255) * f), b = (int)(((c >> 16) & 255) * f);
    return rgba(r, g, b, (int)(c >> 24));
}

}  // namespace

void init() {
    g_vramTop = (uint8_t*)sceGeEdramGetAddr() + TERRAIN_VRAM;
    terrain = {toVram(asset_terrain, asset_terrain_end, 256 * 256 * 4), 256, 256, true};
    items = {toVram(asset_items, asset_items_end, 256 * 256 * 4), 256, 256, false};
    gui = {toVram(asset_gui, asset_gui_end, 256 * 256 * 4), 256, 256, false};
    art = {toVram(asset_art, asset_art_end, 256 * 256 * 2), 256, 256, false, true};
    font = {asset_font, 128, 128, false, false, asset_font_clut};
    crosshair = {asset_crosshair, 16, 16, false};
    background = {asset_background, 16, 16, false};
    logo = {asset_logo, 256, 128, false, false, asset_logo_clut};
    icons = {asset_icons, 256, 32, false, false, asset_icons_clut, true};
    invGui = {asset_gui_inventory, 256, 256, false, false, asset_gui_inventory_clut};
    craftGui = {asset_gui_crafting, 256, 256, false, false, asset_gui_crafting_clut};
    furnaceGui = {asset_gui_furnace, 256, 256, false, false, asset_gui_furnace_clut};
    chestGui = {asset_gui_container, 256, 256, false, false, asset_gui_container_clut};
    dispenserGui = {asset_gui_trap, 256, 256, false, false, asset_gui_trap_clut};
    sun = {asset_sun, 32, 32, false};
    moon = {asset_moon, 32, 32, false};
    arrows = {asset_arrows, 32, 32, false};
    sign = {asset_sign, 64, 32, false};
    particles = {asset_particles, 128, 128, false, false, asset_particles_clut, true};
    rain = {asset_rain, 64, 256, false, false, asset_rain_clut, true};
    snow = {asset_snow, 64, 256, false, false, asset_snow_clut};
    clouds = {asset_clouds, 256, 256, false, false, asset_clouds_clut};
    const uint8_t* mobData[MOB_TEXTURE_COUNT][2] = {
        {asset_mob_zombie, asset_mob_zombie_end},       {asset_mob_skeleton, asset_mob_skeleton_end},
        {asset_mob_creeper, asset_mob_creeper_end},     {asset_mob_spider, asset_mob_spider_end},
        {asset_mob_pig, asset_mob_pig_end},             {asset_mob_cow, asset_mob_cow_end},
        {asset_mob_sheep, asset_mob_sheep_end},         {asset_mob_sheep_fur, asset_mob_sheep_fur_end},
        {asset_mob_chicken, asset_mob_chicken_end},     {asset_mob_char, asset_mob_char_end},
        {asset_mob_slime, asset_mob_slime_end},         {asset_mob_squid, asset_mob_squid_end},
        {asset_mob_wolf, asset_mob_wolf_end},           {asset_mob_wolf_tame, asset_mob_wolf_tame_end},
        {asset_mob_wolf_angry, asset_mob_wolf_angry_end}, {asset_mob_saddle, asset_mob_saddle_end},
        {asset_mob_cart, asset_mob_cart_end},           {asset_mob_boat, asset_mob_boat_end},
        {asset_mob_ghast, asset_mob_ghast_end},         {asset_mob_ghast_fire, asset_mob_ghast_fire_end},
        {asset_mob_pigzombie, asset_mob_pigzombie_end}};
    for (int i = 0; i < MOB_TEXTURE_COUNT; ++i) mobs[i] = {toVram(mobData[i][0], mobData[i][1], 64 * 32 * 4), 64, 32, false};
    // Whatever VRAM the atlases left: indexed textures the GE samples every frame (font, weather, clouds).
    font.data = place(font.data, 128 * 128 / 2);
    logo.data = place(logo.data, 256 * 128 / 2);
    icons.data = place(icons.data, 256 * 32);
    particles.data = place(particles.data, 128 * 128);
    rain.data = place(rain.data, 64 * 256);
    snow.data = place(snow.data, 64 * 256 / 2);
    clouds.data = place(clouds.data, 256 * 256 / 2);
    invGui.data = place(invGui.data, 256 * 256 / 2);
    craftGui.data = place(craftGui.data, 256 * 256 / 2);
    furnaceGui.data = place(furnaceGui.data, 256 * 256 / 2);
    chestGui.data = place(chestGui.data, 256 * 256 / 2);
    dispenserGui.data = place(dispenserGui.data, 256 * 256 / 2);
    sceKernelDcacheWritebackAll();

    sceGuInit();
    PspGeCallbackData geCb{};
    geCb.finish_func = gfxGeFinished;
    sceGeSetCallback(&geCb);
    sceGuStart(GU_DIRECT, g_list);
    sceGuDrawBuffer(GU_PSM_5650, (void*)0, BUF_W);
    sceGuDispBuffer(SCREEN_W, SCREEN_H, (void*)FB_SIZE, BUF_W);
    sceGuDepthBuffer((void*)(FB_SIZE * 2), BUF_W);
    sceGuOffset(2048 - SCREEN_W / 2, 2048 - SCREEN_H / 2);
    sceGuViewport(2048, 2048, SCREEN_W, SCREEN_H);
    sceGuDepthRange(65535, 0);
    sceGuScissor(0, 0, SCREEN_W, SCREEN_H);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDepthFunc(GU_GEQUAL);
    sceGuFrontFace(GU_CW);
    sceGuShadeModel(GU_SMOOTH);
    sceGuEnable(GU_CLIP_PLANES);
    sceGuEnable(GU_DITHER);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuTexFilter(GU_NEAREST, GU_NEAREST);
    sceGuTexWrap(GU_REPEAT, GU_REPEAT);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuAlphaFunc(GU_GREATER, 0x10, 0xFF);
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
}

void waitIdle() {
    if (!g_busy) return;
    crashNote("ge-sync");
    // sceGuSync busy-waits. The main thread is above the chunk worker, so a spinning wait
    // pegs the CPU at 99% and the next chunks never generate. Sleep until the GE is done.
    uint32_t start = sceKernelGetSystemTimeLow();
    while (sceGeDrawSync(1) != 0) {
        if ((int)(sceKernelGetSystemTimeLow() - start) > 80000) break;
        sceKernelDelayThread(200);
    }
    sceGuSync(0, 0);
    uint32_t waited = sceKernelGetSystemTimeLow() - start;
    g_sleptUs += waited;
    g_lastGpuUs = waited < 500000 ? waited : 0;
    g_busy = false;
}

void present() {
    waitIdle();
    if (!g_unshown) return;
    // sceDisplayWaitVblankStart does not yield on this firmware: the main thread spun for the
    // whole frame, the CPU meter stayed at 99%, and the chunk worker never got the processor.
    int v = sceDisplayGetVcount();
    uint32_t start = sceKernelGetSystemTimeLow();
    while (sceDisplayGetVcount() == v) {
        if ((int)(sceKernelGetSystemTimeLow() - start) > 20000) break;
        sceKernelDelayThread(100);
    }
    g_sleptUs += sceKernelGetSystemTimeLow() - start;
    sceGuSwapBuffers();
    g_unshown = false;
}

void beginFrame(uint32_t clearColor) {
    present();
    g_listUsed = 0;
    g_listMargin = 0;
    g_listBlocked = false;
    sceGuStart(GU_DIRECT, g_list);
    g_bound = nullptr;
    g_texturing = true;
    sceGuEnable(GU_TEXTURE_2D);
    sceGuClearColor(clearColor);
    sceGuClearDepth(0);
    sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
}

void endFrame() {
    g_submitUs = sceKernelGetSystemTimeLow();
    sceGuFinish();
    g_busy = g_unshown = true;
}

int lastGpuMicroseconds() { return (int)g_lastGpuUs; }

int takeSleptMicroseconds() {
    int v = (int)g_sleptUs;
    g_sleptUs = 0;
    return v;
}

bool screenshot(const char* path) {
    void* addr;
    int stride, format;
    present();
    sceDisplayWaitVblankStart();
    if (sceDisplayGetFrameBuf(&addr, &stride, &format, PSP_DISPLAY_SETBUF_IMMEDIATE) < 0 || !addr) return false;
    const uint16_t* fb = (const uint16_t*)((uintptr_t)addr | 0x40000000);
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    std::fprintf(f, "P6\n%d %d\n255\n", SCREEN_W, SCREEN_H);
    static uint8_t row[SCREEN_W * 3];
    for (int y = 0; y < SCREEN_H; ++y) {
        for (int x = 0; x < SCREEN_W; ++x) {
            uint16_t p = fb[y * stride + x];
            row[x * 3 + 0] = (uint8_t)((p & 31) * 255 / 31);
            row[x * 3 + 1] = (uint8_t)(((p >> 5) & 63) * 255 / 63);
            row[x * 3 + 2] = (uint8_t)(((p >> 11) & 31) * 255 / 31);
        }
        std::fwrite(row, 1, sizeof(row), f);
    }
    std::fclose(f);
    return true;
}

void bind(const Texture& t) {
    texturing(true);
    if (g_bound == &t) return;
    g_bound = &t;
    if (t.clut) {
        sceGuClutMode(GU_PSM_8888, 0, t.clut8 ? 0xFF : 0x0F, 0);
        flushForGe(t.clut, t.clut8 ? 1024u : 64u);
        sceGuClutLoad(t.clut8 ? 32 : 2, t.clut);
        sceGuTexMode(t.clut8 ? GU_PSM_T8 : GU_PSM_T4, 0, 0, t.swizzled ? 1 : 0);
    } else {
        sceGuTexMode(t.rgba4444 ? GU_PSM_4444 : GU_PSM_8888, 0, 0, t.swizzled ? 1 : 0);
    }
    sceGuTexImage(0, t.width, t.height, t.width, t.data);
    sceGuTexFlush();
}

void begin3D(const Mat4& proj, const Mat4& view, bool fog, float fogNear, float fogFar, uint32_t fogColor) {
    sceGuSetMatrix(GU_PROJECTION, (const ScePspFMatrix4*)&proj);
    sceGuSetMatrix(GU_VIEW, (const ScePspFMatrix4*)&view);
    Mat4 id = Mat4::identity();
    sceGuSetMatrix(GU_MODEL, (const ScePspFMatrix4*)&id);
    sceGuSetMatrix(GU_TEXTURE, (const ScePspFMatrix4*)&id);
    sceGuEnable(GU_DEPTH_TEST);
    sceGuDepthMask(GU_FALSE);
    sceGuEnable(GU_CULL_FACE);
    sceGuDisable(GU_BLEND);
    sceGuEnable(GU_ALPHA_TEST);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);
    if (fog) {
        sceGuFog(fogNear, fogFar, fogColor & 0x00FFFFFF);
        sceGuEnable(GU_FOG);
    } else {
        sceGuDisable(GU_FOG);
    }
    bind(terrain);
}

void setModel(const Mat4& m) { sceGuSetMatrix(GU_MODEL, (const ScePspFMatrix4*)&m); }

void drawLines3D(const float* xyz, int n, uint32_t color) {
    struct CV {
        uint32_t color;
        float x, y, z;
    };
    CV* v = (CV*)guMem(n * (int)sizeof(CV));
    if (!v) return;
    for (int i = 0; i < n; ++i) v[i] = {color, xyz[i * 3], xyz[i * 3 + 1], xyz[i * 3 + 2]};
    Mat4 id = Mat4::identity();
    sceGuSetMatrix(GU_MODEL, (const ScePspFMatrix4*)&id);
    texturing(false);
    sceGuEnable(GU_BLEND);
    sceGuDrawArray(GU_LINES, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D, n, nullptr, v);
    sceGuDisable(GU_BLEND);
}

void begin2D() {
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_FOG);
    sceGuDisable(GU_CULL_FACE);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
}

void rect(float x, float y, float w, float h, uint32_t color) {
    texturing(false);
    V2* v = alloc2(2);
    if (!v) return;
    setV(v[0], 0, 0, color, x, y);
    setV(v[1], 0, 0, color, x + w, y + h);
    sceGuDrawArray(GU_SPRITES, V2_TYPE, 2, nullptr, v);
}

void gradient(float x, float y, float w, float h, uint32_t top, uint32_t bottom) {
    texturing(false);
    V2* v = alloc2(4);
    if (!v) return;
    setV(v[0], 0, 0, top, x, y);
    setV(v[1], 0, 0, top, x + w, y);
    setV(v[2], 0, 0, bottom, x, y + h);
    setV(v[3], 0, 0, bottom, x + w, y + h);
    sceGuDrawArray(GU_TRIANGLE_STRIP, V2_TYPE, 4, nullptr, v);
}

void image(const Texture& t, float x, float y, float w, float h, float u, float v0, float uw, float vh, uint32_t color) {
    bind(t);
    V2* v = alloc2(2);
    if (!v) return;
    setV(v[0], u, v0, color, x, y);
    setV(v[1], u + uw, v0 + vh, color, x + w, y + h);
    sceGuDrawArray(GU_SPRITES, V2_TYPE, 2, nullptr, v);
}

void tiledBackground(uint32_t color) {
    bind(background);
    sceGuTexWrap(GU_REPEAT, GU_REPEAT);
    V2* v = alloc2(2);
    if (!v) return;
    setV(v[0], 0, 0, color, 0, 0);
    setV(v[1], SCREEN_W / 2.0f, SCREEN_H / 2.0f, color, SCREEN_W, SCREEN_H);
    sceGuDrawArray(GU_SPRITES, V2_TYPE, 2, nullptr, v);
}

int textWidth(const char* s) {
    int w = 0;
    for (; *s; ++s) w += asset_fontw[(uint8_t)*s];
    return w;
}

static void textRaw(float x, float y, const char* s, uint32_t color) {
    int n = (int)std::strlen(s);
    if (!n) return;
    bind(font);
    V2* v = alloc2(n * 2);
    if (!v) return;
    int k = 0;
    for (int i = 0; i < n; ++i) {
        uint8_t c = (uint8_t)s[i];
        float u = (float)((c & 15) * 8), vv = (float)((c >> 4) * 8);
        setV(v[k++], u, vv, color, x, y);
        setV(v[k++], u + 8, vv + 8, color, x + 8, y + 8);
        x += asset_fontw[c];
    }
    sceGuDrawArray(GU_SPRITES, V2_TYPE, k, nullptr, v);
}

void text(float x, float y, const char* s, uint32_t color, bool shadow) {
    if (shadow) textRaw(x + 1, y + 1, s, (color & 0xFF000000) | ((color & 0x00FCFCFC) >> 2));
    textRaw(x, y, s, color);
}

void textCentered(float cx, float y, const char* s, uint32_t color, bool shadow) {
    text(cx - textWidth(s) / 2.0f, y, s, color, shadow);
}

void textRotated(float cx, float cy, const char* s, uint32_t color, float angleDeg, float scale) {
    int n = (int)std::strlen(s);
    if (!n) return;
    bind(font);
    float a = angleDeg * 3.14159265f / 180.0f, ca = std::cos(a) * scale, sa = std::sin(a) * scale;
    float start = -textWidth(s) / 2.0f;
    for (int pass = 0; pass < 2; ++pass) {
        uint32_t c = pass == 0 ? ((color & 0xFF000000) | ((color & 0x00FCFCFC) >> 2)) : color;
        float off = pass == 0 ? 1.0f : 0.0f;
        V2* v = alloc2(n * 6);
        if (!v) return;
        int k = 0;
        float px = start;
        for (int i = 0; i < n; ++i) {
            uint8_t ch = (uint8_t)s[i];
            float u = (float)((ch & 15) * 8), vv = (float)((ch >> 4) * 8);
            float lx[4] = {px + off, px + 8 + off, px + 8 + off, px + off};
            float ly[4] = {-4 + off, -4 + off, 4 + off, 4 + off};
            float uu[4] = {u, u + 8, u + 8, u};
            float vt[4] = {vv, vv, vv + 8, vv + 8};
            V2 q[4];
            for (int j = 0; j < 4; ++j) setV(q[j], uu[j], vt[j], c, cx + lx[j] * ca - ly[j] * sa, cy + lx[j] * sa + ly[j] * ca);
            v[k++] = q[0];
            v[k++] = q[1];
            v[k++] = q[2];
            v[k++] = q[0];
            v[k++] = q[2];
            v[k++] = q[3];
            px += asset_fontw[ch];
        }
        sceGuDrawArray(GU_TRIANGLES, V2_TYPE, k, nullptr, v);
    }
}

void blockIcon(ItemStack item, float x, float y, float size) {
    const BlockDef& d = g_blocks[item.id];
    bind(terrain);
    auto tileUV = [](int tex, float& u, float& v) {
        u = (float)((tex & 15) * 16);
        v = (float)((tex >> 4) * 16);
    };
    bool flat = d.render == RENDER_CROSS || d.render == RENDER_TORCH || d.render == RENDER_LADDER ||
                d.render == RENDER_CROPS || d.render == RENDER_DOOR || d.render == RENDER_LEVER ||
                d.render == RENDER_WIRE || d.render == RENDER_RAIL;
    if (flat) {
        int tex = blockTexture(item.id, item.damage, 2);
        float u, v;
        tileUV(tex, u, v);
        uint32_t c = fromRGB(blockTint(nullptr, item.id, item.damage, 2, 0, 0));
        V2* q = alloc2(2);
        if (!q) return;
        setV(q[0], u, v, c, x, y);
        setV(q[1], u + 16, v + 16, c, x + size, y + size);
        sceGuDrawArray(GU_SPRITES, V2_TYPE, 2, nullptr, q);
        return;
    }
    // Block.setBlockBoundsForItemRender
    AABB b = {0, 0, 0, 1, 1, 1};
    if (d.render == RENDER_SLAB) b.y1 = 0.5f;
    if (d.render == RENDER_SNOW) b.y1 = 0.125f;
    if (d.render == RENDER_TRAPDOOR) b.y1 = 0.1875f;
    if (blockIsPlate(item.id)) b = {0, 0.375f, 0, 1, 0.625f, 1};
    if (item.id == STONE_BUTTON) b = {0.3125f, 0.375f, 0.375f, 0.6875f, 0.625f, 0.625f};
    int meta = blockIsPiston(item.id) ? 1 : item.damage;
    float s = size / 16.0f;
    float cx = x + 8 * s, top = y + 1 * s;
    float hw = 7.0f * s, hh = 3.5f * s, side = 7.0f * s;
    auto sx = [&](float px, float pz) { return cx + hw * (px + pz - 1); };
    auto sy = [&](float px, float py, float pz) { return top + hh * (1 - px + pz) + side * (1 - py); };
    V2* v = alloc2(18);
    if (!v) return;
    int k = 0;
    // Corners as (x, y, z) in the block, texture coordinates in tile pixels.
    auto quad = [&](int tex, uint32_t color, const float (&p)[4][3], const float (&uv)[4][2]) {
        float u, vv;
        tileUV(tex, u, vv);
        V2 q[4];
        for (int i = 0; i < 4; ++i)
            setV(q[i], u + uv[i][0], vv + uv[i][1], color, sx(p[i][0], p[i][2]), sy(p[i][0], p[i][1], p[i][2]));
        v[k++] = q[0];
        v[k++] = q[1];
        v[k++] = q[2];
        v[k++] = q[0];
        v[k++] = q[2];
        v[k++] = q[3];
    };
    uint32_t topTint = fromRGB(blockTint(nullptr, item.id, meta, 1, 0, 0));
    uint32_t sideTint = fromRGB(blockTint(nullptr, item.id, meta, 2, 0, 0));
    {
        const float p[4][3] = {{b.x0, b.y1, b.z0}, {b.x1, b.y1, b.z0}, {b.x1, b.y1, b.z1}, {b.x0, b.y1, b.z1}};
        const float uv[4][2] = {{b.x0 * 16, b.z0 * 16}, {b.x1 * 16, b.z0 * 16}, {b.x1 * 16, b.z1 * 16}, {b.x0 * 16, b.z1 * 16}};
        quad(blockTexture(item.id, meta, 1), topTint, p, uv);
    }
    float v0 = (1 - b.y1) * 16, v1 = (1 - b.y0) * 16;
    {
        const float p[4][3] = {{b.x0, b.y1, b.z0}, {b.x0, b.y1, b.z1}, {b.x0, b.y0, b.z1}, {b.x0, b.y0, b.z0}};
        const float uv[4][2] = {{b.z0 * 16, v0}, {b.z1 * 16, v0}, {b.z1 * 16, v1}, {b.z0 * 16, v1}};
        quad(blockTexture(item.id, meta, 3), scaleColor(sideTint, 0.8f), p, uv);
    }
    {
        const float p[4][3] = {{b.x0, b.y1, b.z1}, {b.x1, b.y1, b.z1}, {b.x1, b.y0, b.z1}, {b.x0, b.y0, b.z1}};
        const float uv[4][2] = {{b.x0 * 16, v0}, {b.x1 * 16, v0}, {b.x1 * 16, v1}, {b.x0 * 16, v1}};
        quad(blockTexture(item.id, meta, 5), scaleColor(sideTint, 0.6f), p, uv);
    }
    sceGuEnable(GU_ALPHA_TEST);
    sceGuDrawArray(GU_TRIANGLES, V2_TYPE, k, nullptr, v);
    sceGuDisable(GU_ALPHA_TEST);
}

void itemIcon(const ItemStack& item, float x, float y) {
    if (item.empty()) return;
    if (item.id < 256) {
        blockIcon(item, x, y, 16);
        return;
    }
    int icon = itemIcon(item);
    image(items, x, y, 16, 16, (float)((icon & 15) * 16), (float)((icon >> 4) * 16), 16, 16);
}

void itemOverlay(const ItemStack& item, float x, float y) {
    if (item.empty()) return;
    if (item.count > 1) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%d", item.count);
        text(x + 19 - 2 - textWidth(buf), y + 6 + 3, buf, 0xFFFFFFFF);
    }
    int maxDamage = itemDef(item.id).maxDamage;
    if (item.id >= 256 && maxDamage > 0 && item.damage > 0) {
        int j = (int)std::lround(13.0 - item.damage * 13.0 / maxDamage);
        int k = (int)std::lround(255.0 - item.damage * 255.0 / maxDamage);
        rect(x + 2, y + 13, 13, 2, rgba(0, 0, 0));
        rect(x + 2, y + 13, 12, 1, rgba((255 - k) / 4, 64, 0));
        rect(x + 2, y + 13, (float)j, 1, rgba(255 - k, k, 0));
    }
}

void drawCrack(const AABB& b, int tex) {
    float u0 = (tex & 15) / 16.0f, v0 = (tex >> 4) / 16.0f, u1 = u0 + 1 / 16.0f, v1 = v0 + 1 / 16.0f;
    ModelVertex* v = allocModel(36);
    if (!v) return;
    int k = 0;
    auto quad = [&](float ax, float ay, float az, float bx, float by, float bz, float cx, float cy, float cz, float dx,
                    float dy, float dz) {
        ModelVertex q[4] = {{u0, v0, 0xFFFFFFFF, ax, ay, az},
                            {u1, v0, 0xFFFFFFFF, bx, by, bz},
                            {u1, v1, 0xFFFFFFFF, cx, cy, cz},
                            {u0, v1, 0xFFFFFFFF, dx, dy, dz}};
        v[k++] = q[0], v[k++] = q[1], v[k++] = q[2], v[k++] = q[0], v[k++] = q[2], v[k++] = q[3];
    };
    quad(b.x0, b.y1, b.z0, b.x1, b.y1, b.z0, b.x1, b.y1, b.z1, b.x0, b.y1, b.z1);
    quad(b.x0, b.y0, b.z1, b.x1, b.y0, b.z1, b.x1, b.y0, b.z0, b.x0, b.y0, b.z0);
    quad(b.x0, b.y1, b.z0, b.x1, b.y1, b.z0, b.x1, b.y0, b.z0, b.x0, b.y0, b.z0);
    quad(b.x1, b.y1, b.z1, b.x0, b.y1, b.z1, b.x0, b.y0, b.z1, b.x1, b.y0, b.z1);
    quad(b.x0, b.y1, b.z1, b.x0, b.y1, b.z0, b.x0, b.y0, b.z0, b.x0, b.y0, b.z1);
    quad(b.x1, b.y1, b.z0, b.x1, b.y1, b.z1, b.x1, b.y0, b.z1, b.x1, b.y0, b.z0);
    sceGuDisable(GU_CULL_FACE);
    sceGuEnable(GU_BLEND);
    sceGuEnable(GU_ALPHA_TEST);
    sceGuDisable(GU_FOG);
    sceGuBlendFunc(GU_ADD, GU_DST_COLOR, GU_SRC_COLOR, 0, 0);
    drawModel(Mat4::identity(), terrain, v, k);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuDisable(GU_BLEND);
    sceGuEnable(GU_CULL_FACE);
}

ModelVertex* allocModel(int n) { return (ModelVertex*)guMem(n * (int)sizeof(ModelVertex)); }

void setListMargin(int bytes) { g_listMargin = bytes > 0 ? bytes : 0; }

bool listBlocked() { return g_listBlocked; }

void* allocBytes(int bytes) { return guMem(bytes); }

void drawModel(const Mat4& model, const Texture& t, const ModelVertex* v, int n) {
    if (!v || n <= 0) return;
    bind(t);
    sceGuSetMatrix(GU_MODEL, (const ScePspFMatrix4*)&model);
    sceGuDrawArray(GU_TRIANGLES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D, n, nullptr, v);
}

ColorVertex* allocColor(int n) { return (ColorVertex*)guMem(n * (int)sizeof(ColorVertex)); }

void drawColored3D(int prim, const ColorVertex* v, int n) {
    if (!v || n <= 0) return;
    Mat4 id = Mat4::identity();
    sceGuSetMatrix(GU_MODEL, (const ScePspFMatrix4*)&id);
    texturing(false);
    sceGuDrawArray(prim, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D, n, nullptr, v);
}

void drawSprites3D(const Texture& t, const ModelVertex* v, int n) {
    if (n <= 0) return;
    bind(t);
    Mat4 id = Mat4::identity();
    sceGuSetMatrix(GU_MODEL, (const ScePspFMatrix4*)&id);
    sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D, n, nullptr, v);
}

int charWidth(uint8_t c) { return asset_fontw[c]; }

int text3D(ModelVertex* v, float x, float y, const char* s, uint32_t color) {
    int k = 0;
    for (; *s; ++s) {
        uint8_t c = (uint8_t)*s;
        float u0 = (c & 15) / 16.0f, v0 = (c >> 4) / 16.0f, u1 = u0 + 1 / 16.0f, v1 = v0 + 1 / 16.0f;
        ModelVertex q[4] = {{u0, v0, color, x, y, 0},
                            {u1, v0, color, x + 8, y, 0},
                            {u1, v1, color, x + 8, y + 8, 0},
                            {u0, v1, color, x, y + 8, 0}};
        v[k++] = q[0], v[k++] = q[1], v[k++] = q[2], v[k++] = q[0], v[k++] = q[2], v[k++] = q[3];
        x += asset_fontw[c];
    }
    return k;
}

// TextureCompassFX / TextureWatchFX: the compass and clock icons are redrawn into items.png every tick.
void updateDials(float compassAngle, float clockAngle) {
    // The needle moves by a pixel or so per degree. Rewriting the icon every frame, and
    // waiting on the GE to do it, was pure overhead while the player stood still.
    static float prevC = 1000.0f, prevK = 1000.0f;
    static bool saved = false;
    if (saved && std::fabs(compassAngle - prevC) < 0.02f && std::fabs(clockAngle - prevK) < 0.02f) return;
    prevC = compassAngle;
    prevK = clockAngle;
    waitIdle();  // items.png lives in VRAM; the GE locks up if the CPU writes it mid-draw
    constexpr int COMPASS = 3 * 16 + 6, CLOCK = 4 * 16 + 6;
    static uint32_t base[2][256];
    uint32_t* atlas = (uint32_t*)const_cast<void*>(items.data);
    auto texel = [&](int icon, int i) -> uint32_t& {
        return atlas[((icon >> 4) * 16 + i / 16) * 256 + (icon & 15) * 16 + i % 16];
    };
    if (!saved) {
        for (int i = 0; i < 256; ++i) base[0][i] = texel(COMPASS, i), base[1][i] = texel(CLOCK, i);
        saved = true;
    }
    auto pack = [](int r, int g, int b, int a) { return (uint32_t)(r | (g << 8) | (b << 16) | (a << 24)); };
    for (int i = 0; i < 256; ++i) texel(COMPASS, i) = base[0][i];
    float s = std::sin(compassAngle), c = std::cos(compassAngle);
    for (int k = -4; k <= 4; ++k) {
        int x = (int)(8.5f + c * k * 0.3f), y = (int)(7.5f - s * k * 0.3f * 0.5f);
        texel(COMPASS, y * 16 + x) = pack(100, 100, 100, 255);
    }
    for (int k = -8; k <= 16; ++k) {
        int x = (int)(8.5f + s * k * 0.3f), y = (int)(7.5f + c * k * 0.3f * 0.5f);
        texel(COMPASS, y * 16 + x) = k >= 0 ? pack(255, 20, 20, 255) : pack(100, 100, 100, 255);
    }
    s = std::sin(clockAngle), c = std::cos(clockAngle);
    const uint32_t* dial = (const uint32_t*)asset_dial;
    for (int i = 0; i < 256; ++i) {
        uint32_t p = base[1][i];
        int r = p & 255, g = (p >> 8) & 255, b = (p >> 16) & 255;
        if (r == b && g == 0 && b > 0) {
            float dx = -((i % 16) / 15.0f - 0.5f), dy = (i / 16) / 15.0f - 0.5f;
            int ux = (int)((dx * c + dy * s + 0.5f) * 16.0f), uy = (int)((dy * c - dx * s + 0.5f) * 16.0f);
            uint32_t d = dial[(ux & 15) + (uy & 15) * 16];
            p = pack((d & 255) * r / 255, ((d >> 8) & 255) * r / 255, ((d >> 16) & 255) * r / 255, d >> 24);
        }
        texel(CLOCK, i) = p;
    }
    flushForGe(&atlas[3 * 16 * 256], 2 * 16 * 256 * 4);
    if (g_bound == &items) g_bound = nullptr;
}

// TexturePortalFX: 32 precomputed frames copied into terrain tile 14 (swizzled, in VRAM).
void updatePortal() {
    constexpr float PI = 3.14159265f;
    static uint32_t frames[32][256];
    static bool built = false;
    static int counter = 0;
    if (!built) {
        JRandom r(100);
        for (int f = 0; f < 32; ++f)
            for (int x = 0; x < 16; ++x)
                for (int y = 0; y < 16; ++y) {
                    float v = 0;
                    for (int k = 0; k < 2; ++k) {
                        float dx = (x - k * 8) / 16.0f * 2.0f, dy = (y - k * 8) / 16.0f * 2.0f;
                        if (dx < -1) dx += 2;
                        if (dx >= 1) dx -= 2;
                        if (dy < -1) dy += 2;
                        if (dy >= 1) dy -= 2;
                        float d = dx * dx + dy * dy;
                        float a = (float)std::atan2((double)dy, (double)dx) +
                                  (f / 32.0f * PI * 2.0f - d * 10.0f + (float)(k * 2)) * (float)(k * 2 - 1);
                        a = (mh::sin(a) + 1.0f) / 2.0f;
                        a /= d + 1.0f;
                        v += a * 0.5f;
                    }
                    v += r.nextFloat() * 0.1f;
                    int al = (int)(v * 100 + 155), rr = (int)(v * v * 200 + 55), gg = (int)(v * v * v * v * 255),
                        bb = (int)(v * 100 + 155);
                    frames[f][y * 16 + x] =
                        (uint32_t)(rr & 255) | ((uint32_t)(gg & 255) << 8) | ((uint32_t)(bb & 255) << 16) |
                        ((uint32_t)(al & 255) << 24);
                }
        built = true;
    }
    // The portal tile is a 16KB uncached VRAM write. Half rate is still smooth and skips the
    // GE sync on the frames we leave the previous tile in place.
    if (counter & 1) {
        ++counter;
        return;
    }
    const uint32_t* src = frames[counter & 31];
    ++counter;
    waitIdle();
    uint8_t* base = (uint8_t*)const_cast<void*>(terrain.data);
    constexpr int TX = 14 * 16;
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) {
            int px = TX + x, off = ((y / 8) * 64 + (px * 4) / 16) * 128 + (y % 8) * 16 + (px * 4) % 16;
            std::memcpy(base + off, &src[y * 16 + x], 4);
        }
    flushForGe(base, 2 * 64 * 128);
    if (g_bound == &terrain) g_bound = nullptr;
}

}  // namespace gfx
