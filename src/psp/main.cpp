#include <pspctrl.h>
#include <pspgu.h>
#include <pspkernel.h>
#include <psppower.h>
#include <psprtc.h>
#include <pspthreadman.h>

#include <malloc.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "../assets.h"
#include "../core/audio.h"
#include "../core/blocks.h"
#include "../core/chunkio.h"
#include "../core/crafting.h"
#include "../core/game.h"
#include "../core/jrandom.h"
#include "../core/mathhelper.h"
#include "../core/mesher.h"
#include "crashlog.h"
#include "entityrender.h"
#include "gfx.h"
#include "renderer.h"
#include "sound.h"

PSP_MODULE_INFO("MinecraftPSP", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
#if defined(MC_AUTOTEST) && !defined(MC_FAT_HEAP)
PSP_HEAP_SIZE_KB(19 * 1024);  // fixed heap so the scripted run fits a PSP-1000
#else
PSP_HEAP_SIZE_KB(-1024);  // all but 1 MB, same as the release EBOOT
#endif
PSP_MAIN_THREAD_STACK_SIZE_KB(512);

namespace {

void crashFrame();

const char* SETTINGS_PATH = "options.txt";
// Save slots: the first is the folder older versions used.
constexpr int WORLD_SLOTS = 5;
char g_worldDir[16] = "world";

void slotDir(int slot, char* out, int size) {
    if (slot == 0)
        std::snprintf(out, size, "world");
    else
        std::snprintf(out, size, "world%d", slot + 1);
}

volatile bool g_exitRequested = false;

int exitCallback(int, int, void*) {
    g_exitRequested = true;
    return 0;
}

int callbackThread(SceSize, void*) {
    int cb = sceKernelCreateCallback("Exit Callback", exitCallback, nullptr);
    sceKernelRegisterExitCallback(cb);
    sceKernelSleepThreadCB();
    return 0;
}

void setupCallbacks() {
    int th = sceKernelCreateThread("update_thread", callbackThread, 0x11, 0xFA0, 0, nullptr);
    if (th >= 0) sceKernelStartThread(th, 0, nullptr);
}

double nowSeconds() {
    u64 t;
    sceRtcGetCurrentTick(&t);
    return (double)t / (double)sceRtcGetTickResolution();
}

// Seconds since 1970 on the PSP's local clock (shown as "last played").
int64_t localNow() {
    ScePspDateTime t;
    if (sceRtcGetCurrentClockLocalTime(&t) < 0) return 0;
    time_t s = 0;
    sceRtcGetTime_t(&t, &s);
    return (int64_t)s;
}

inline int ifloor(float v) {
    int i = (int)v;
    return v < (float)i ? i - 1 : i;
}

// ------------------------------------------------------------------ input

struct Input {
    unsigned held = 0, pressed = 0;
    float ax = 0, ay = 0;
    double heldSince[32] = {};
    double lastRepeat[32] = {};

    void poll(double now) {
        SceCtrlData pad;
        sceCtrlPeekBufferPositive(&pad, 1);
        pressed = pad.Buttons & ~held;
        held = pad.Buttons;
        for (int i = 0; i < 32; ++i)
            if (pressed & (1u << i)) heldSince[i] = lastRepeat[i] = now;
        auto axis = [](int v) {
            float f = (v - 128) / 127.0f;
            const float dead = 0.25f;
            if (std::fabs(f) < dead) return 0.0f;
            f = (std::fabs(f) - dead) / (1.0f - dead) * (f < 0 ? -1.0f : 1.0f);
            return f > 1 ? 1.0f : f < -1 ? -1.0f : f;
        };
        ax = axis(pad.Lx);
        ay = axis(pad.Ly);
    }
    bool down(unsigned b) const { return (held & b) != 0; }
    bool hit(unsigned b) const { return (pressed & b) != 0; }
    bool repeat(unsigned b, double now) {
        if (hit(b)) return true;
        if (!down(b)) return false;
        int i = __builtin_ctz(b);
        if (now - heldSince[i] > 0.35 && now - lastRepeat[i] > 0.08) {
            lastRepeat[i] = now;
            return true;
        }
        return false;
    }
};

// ------------------------------------------------------------------ state

enum class State { Title, SelectWorld, CreateWorld, TextEntry, Playing, Paused, Picker, Container, Dead, Sign };
enum class Screen { Inventory, Workbench, Furnace, Chest, Dispenser };

struct Settings {
    int renderDistance = 1;  // index into RENDER_DISTANCES
    bool fancy = false;
    bool autoJump = true;
    bool analogLook = false;
    bool debug = false;
    int music = 4, sound = 4;  // quarters of full volume
    int particles = 1;         // All, Decreased, Minimal
    bool clouds = true;
};
const char* PARTICLE_NAMES[] = {"All", "Decreased", "Minimal"};

const int RENDER_DISTANCES[] = {2, 3, 4};
const char* RENDER_NAMES[] = {"Tiny", "Short", "Normal"};
constexpr int RENDER_OPTIONS = 3;

Game g_game;
WorldRenderer g_renderer;
HandState g_hand;
Input g_in;
Settings g_settings;
State g_state = State::Title;
int g_menu = 0;
int g_newMode = MODE_SURVIVAL;
int g_pickerCursor = 0;
double g_tickAcc = 0;
double g_useTimer = 0;
double g_itemNameTimer = 0;
float g_fps = 0;
int g_cpuPct = 0;
int g_gpuPct = 0;
uint32_t g_gpuWindowUs = 0;
uint32_t g_sleptWindowUs = 0;
// Last frame, and the average over the fps window.
uint32_t g_usTick = 0, g_usStream = 0, g_usMesh = 0, g_usDraw = 0;
uint32_t g_accTick = 0, g_accStream = 0, g_accMesh = 0, g_accDraw = 0;
int g_avgTick = 0, g_avgStream = 0, g_avgMesh = 0, g_avgDraw = 0;
float g_partial = 0;
const char* g_message = nullptr;
double g_messageTimer = 0;
#ifdef MC_AUTOTEST
#ifndef MC_AUTOTEST_SEED
#define MC_AUTOTEST_SEED 1234567
#endif
int g_testFrame = 0;
int g_testSounds = 0;
#ifdef MC_SHOWCASE
// The showcase build lingers on each scene so a recording can show it moving,
// instead of cutting away on the same frame the screenshot would have been taken.
int g_showcaseHold = 0;
unsigned g_holdButtons = 0;
float g_holdAx = 0, g_holdAy = 0;
#endif
float g_tickMs = 0, g_tickSum = 0;  // slowest and total tick time since the last report
int g_tickN = 0;
void reportTicks(const char* what) {
    std::printf("[%s] ticks: avg %.2f ms, slowest %.2f ms\n", what, g_tickN ? g_tickSum / g_tickN : 0.0f, g_tickMs);
    g_tickMs = g_tickSum = 0;
    g_tickN = 0;
}
#endif

int renderDistance() { return RENDER_DISTANCES[g_settings.renderDistance]; }

void loadSettings() {
    FILE* f = std::fopen(SETTINGS_PATH, "r");
    if (!f) return;
    int rd, fancy, aj, al, dbg, mus = 4, snd = 4, part = 1, clouds = 1;
    int n = std::fscanf(f, "%d %d %d %d %d %d %d %d %d", &rd, &fancy, &aj, &al, &dbg, &mus, &snd, &part, &clouds);
    if (n >= 5) {
        if (rd >= 0) g_settings.renderDistance = rd < RENDER_OPTIONS ? rd : RENDER_OPTIONS - 1;
        g_settings.fancy = fancy != 0;
        g_settings.autoJump = aj != 0;
        g_settings.analogLook = al != 0;
        g_settings.debug = dbg != 0;
    }
    if (n >= 7) {
        g_settings.music = mus < 0 ? 0 : mus > 4 ? 4 : mus;
        g_settings.sound = snd < 0 ? 0 : snd > 4 ? 4 : snd;
    }
    if (n >= 9) {
        g_settings.particles = part < 0 ? 0 : part > 2 ? 2 : part;
        g_settings.clouds = clouds != 0;
    }
    std::fclose(f);
}

void saveSettings() {
    FILE* f = std::fopen(SETTINGS_PATH, "w");
    if (!f) return;
    std::fprintf(f, "%d %d %d %d %d %d %d %d %d\n", g_settings.renderDistance, g_settings.fancy ? 1 : 0,
                 g_settings.autoJump ? 1 : 0, g_settings.analogLook ? 1 : 0, g_settings.debug ? 1 : 0, g_settings.music,
                 g_settings.sound, g_settings.particles, g_settings.clouds ? 1 : 0);
    std::fclose(f);
}

void applySettings() {
    g_renderer.renderDistance = renderDistance();
    g_game.player.autoJump = g_settings.autoJump;
    g_fancyLeaves = g_settings.fancy;
    g_game.fancyWeather = g_settings.fancy;
    g_game.particles.level = g_settings.particles;
    g_renderer.clouds = g_settings.clouds;
    sound::setVolumes(g_settings.sound / 4.0f, g_settings.music / 4.0f);
}

void showMessage(const char* m) {
    g_message = m;
    g_messageTimer = 3.0;
}

// ------------------------------------------------------------------ GUI widgets

void drawButton(float x, float y, float w, const char* label, bool hover, bool enabled = true) {
    float v = !enabled ? 46 : hover ? 86 : 66;
    gfx::image(gfx::gui, x, y, w / 2, 20, 0, v, w / 2, 20);
    gfx::image(gfx::gui, x + w / 2, y, w / 2, 20, 200 - w / 2, v, w / 2, 20);
    uint32_t c = !enabled ? gfx::rgba(160, 160, 160) : hover ? gfx::rgba(255, 255, 160) : gfx::rgba(224, 224, 224);
    gfx::textCentered(x + w / 2, y + 6, label, c);
}

int menuNavigate(int current, int count, double now) {
    if (g_in.repeat(PSP_CTRL_UP, now)) current = (current + count - 1) % count;
    if (g_in.repeat(PSP_CTRL_DOWN, now)) current = (current + 1) % count;
    return current;
}

void drawLoading(const char* title, const char* stage, float p) {
    crashlog::note(title ? title : "loading");
    crashlog::heartbeat();
    gfx::beginFrame(0xFF000000);
    gfx::begin2D();
    gfx::tiledBackground(gfx::rgba(64, 64, 64));
    float cy = gfx::SCREEN_H / 2.0f;
    gfx::textCentered(gfx::SCREEN_W / 2.0f, cy - 4 - 16, title, 0xFFFFFFFF);
    if (stage) gfx::textCentered(gfx::SCREEN_W / 2.0f, cy - 4 + 8, stage, 0xFFFFFFFF);
    if (p >= 0) {
        float x = gfx::SCREEN_W / 2.0f - 50, y = cy + 16;
        gfx::rect(x, y, 100, 2, gfx::rgba(128, 128, 128));
        gfx::rect(x, y, 100 * (p > 1 ? 1 : p), 2, gfx::rgba(128, 255, 128));
    }
    gfx::endFrame();
    gfx::present();  // the caller blocks until the next frame
}

// ------------------------------------------------------------------ world lifecycle

// EntityRenderer.hurtCameraEffect and setupViewBobbing.
Mat4 cameraEffects(float a) {
    const Game& g = g_game;
    const Player& p = g.player;
    constexpr float PI = 3.14159265f;
    Mat4 m = Mat4::identity();
    if (g.dead) m = m * Mat4::rotateZ(40.0f - 8000.0f / (g.deathTime + a + 200.0f));
    float h = g.hurtTime - a;
    if (h >= 0 && g.hurtTime > 0) {
        h /= 10.0f;
        h = std::sin(h * h * h * h * PI);
        m = m * Mat4::rotateY(-g.attackedAtYaw) * Mat4::rotateZ(-h * 14.0f) * Mat4::rotateY(g.attackedAtYaw);
    }
    if (!g.sleeping) {
        float d = -(p.walked + (p.walked - p.prevWalked) * a);
        float bob = p.prevBob + (p.bob - p.prevBob) * a;
        float tilt = p.prevBobPitch + (p.bobPitch - p.prevBobPitch) * a;
        m = m * Mat4::translate(std::sin(d * PI) * bob * 0.5f, -std::fabs(std::cos(d * PI) * bob), 0) *
            Mat4::rotateZ(std::sin(d * PI) * bob * 3.0f) *
            Mat4::rotateX(std::fabs(std::cos(d * PI - 0.2f) * bob) * 5.0f) * Mat4::rotateX(tilt);
    }
    float t = g.prevPortalTime + (g.portalTime - g.prevPortalTime) * a;
    if (t > 0) {
        float f = 5.0f / (t * t + 5.0f) - t * 0.04f;
        f *= f;
        float spin = ((float)(g.world.time % 3600) + a) * 20.0f;
        m = m * Mat4::rotate(spin, 0, 1, 1) * Mat4::scale(1.0f / f, 1, 1) * Mat4::rotate(-spin, 0, 1, 1);
    }
    return m;
}

Camera currentCamera() {
    const Player& p = g_game.player;
    float a = g_partial;
    Camera c;
    c.x = p.prevX + (p.x - p.prevX) * a;
    c.y = p.prevY + (p.y - p.prevY) * a + (g_game.sleeping ? 0.2f : Player::EYE);
    c.z = p.prevZ + (p.z - p.prevZ) * a;
    if (g_game.dead) c.y -= 1.4f * (g_game.deathTime > 20 ? 1.0f : g_game.deathTime / 20.0f);
    c.yaw = p.yaw;
    c.pitch = p.pitch;
    c.underwater = p.eyeInWater(g_game.world, c.y);
    c.effects = cameraEffects(a);
    return c;
}

// Streams chunks until the area around the player is lit, then meshes it.
bool loadArea(const char* title, bool findSpawn, bool portal = false) {
    const int R = renderDistance();
    double lastDraw = 0;
    auto progress = [&](const char* stage, float p) {
        double now = nowSeconds();
        if (now - lastDraw < 0.016) return;
        lastDraw = now;
        drawLoading(title, stage, p);
    };
    // Lighting is sliced, and each slice is followed by a redraw. The redraw waits for vblank,
    // so the processor is not held for the whole of a chunk the way a 50 ms budget held it.
    g_game.world.setLightSlice(true);
    auto pumpUntil = [&](int radius, int need) -> bool {
        const int total = (2 * radius + 1) * (2 * radius + 1);
        int stuck = 0, prev = -1;
        while (g_game.world.readyCount(g_game.player.x, g_game.player.z, radius) < need) {
            if (g_exitRequested) return false;
            crashFrame();
            int ready = g_game.world.readyCount(g_game.player.x, g_game.player.z, radius);
            g_game.stream(R, 0.004);
            progress("Building terrain", total > 0 ? ready / (float)total : 0.0f);
            sceKernelDelayThread(1000);
            bool idle = g_game.world.workerInFlight() == 0 && g_game.world.workerBacklog() == 0;
            if (ready == prev && idle)
                ++stuck;
            else
                stuck = 0;
            prev = ready;
            if (stuck > 200 && !g_game.world.outOfMemory) {
                std::printf("spawn stalled at %d/%d\n", ready, need);
                crashNote("spawn-stall");
                g_game.world.outOfMemory = true;
            }
            if (g_game.world.outOfMemory) break;
        }
        return !g_exitRequested;
    };
    if (findSpawn) {
        if (!pumpUntil(1, 9)) {
            g_game.world.setLightSlice(false);
            return false;
        }
        g_game.placeAtSpawn();
    }
    const int total = (2 * R + 1) * (2 * R + 1);
    if (!pumpUntil(R, total)) {
        g_game.world.setLightSlice(false);
        return false;
    }
    if (findSpawn) g_game.placeAtSpawn();
    if (portal) {
        for (int tries = 0;; ++tries) {
            progress("Placing portal", 1.0f);
            bool placed = tries >= 3 || g_game.placeInPortal();
            if (!pumpUntil(R, total)) {
                g_game.world.setLightSlice(false);
                return false;
            }
            if (placed) break;
        }
    }
    g_tickAcc = 0;
    g_partial = 0;
    g_game.player.prevX = g_game.player.x;
    g_game.player.prevY = g_game.player.y;
    g_game.player.prevZ = g_game.player.z;
    g_game.world.setLightSlice(false);
    drawLoading(title, "Preparing chunks", 1.0f);
    g_renderer.reset();
    g_renderer.buildNear(g_game.world, currentCamera(), R);
    g_state = State::Playing;
    return true;
}

void clearPlayerInput();

// Moves the player through a nether portal (or back to the overworld after dying there).
void changeDimension(bool portal) {
    const char* title = g_game.dimension < 0 ? "Leaving the Nether" : "Entering the Nether";
    clearPlayerInput();
    drawLoading(title, "Building terrain", 0);
    if (!g_game.travelDimension()) {
        showMessage("Could not open the dimension folder");
        crashlog::expectLong(true);
        g_game.closeWorld();
        crashlog::expectLong(false);
        g_renderer.reset();
        g_state = State::Title;
        return;
    }
    loadArea(title, !portal, portal);
}

void newWorld(const char* dir, const char* name, int64_t seed, int mode) {
    drawLoading("Generating level", "Building terrain", 0);
    std::snprintf(g_worldDir, sizeof(g_worldDir), "%s", dir);
    if (!g_game.createWorld(dir, seed, mode)) {
        showMessage("Could not create the world folder");
        g_state = State::Title;
        return;
    }
    std::snprintf(g_game.worldName, sizeof(g_game.worldName), "%s", name);
    g_game.lastPlayed = localNow();
    applySettings();
    loadArea("Generating level", true);
    g_game.saveLevel();
}

bool loadSlot(const char* dir) {
    drawLoading("Loading level", "Reading level.dat", -1);
    std::snprintf(g_worldDir, sizeof(g_worldDir), "%s", dir);
    if (!g_game.loadWorld(dir)) return false;
    g_game.lastPlayed = localNow();
    applySettings();
    return loadArea("Loading level", false);
}

void quitToTitle() {
    drawLoading("Saving level", "Saving chunks", -1);
    crashlog::expectLong(true);
    g_game.closeWorld();
    crashlog::expectLong(false);
    g_renderer.reset();
    g_state = State::Title;
    g_menu = 0;
}

// ------------------------------------------------------------------ containers

enum SlotKind : uint8_t { SK_NORMAL, SK_ARMOR, SK_RESULT, SK_OUTPUT, SK_GRID };

struct Slot {
    int16_t x, y;
    ItemStack* s;
    uint8_t kind, arg;
    bool player;
};

Screen g_screen = Screen::Inventory;
Slot g_slots[64];
int g_slotCount = 0;
int g_cursor = 0;
ItemStack g_carried;
ItemStack g_grid[9];
ItemStack g_result;
int g_gridSize = 2;
TileEntity* g_tile = nullptr;
int g_cart = -1;  // chest minecart behind the chest screen
int g_tileX = 0, g_tileY = 0, g_tileZ = 0;
int g_guiH = 166;

void addSlot(int x, int y, ItemStack* s, uint8_t kind, uint8_t arg, bool player) {
    if (g_slotCount < 64) g_slots[g_slotCount++] = {(int16_t)x, (int16_t)y, s, kind, arg, player};
}

void addPlayerSlots(int y0, int hotbarY) {
    Inventory& inv = g_game.inv;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 9; ++j) addSlot(8 + j * 18, y0 + i * 18, &inv.main[9 + i * 9 + j], SK_NORMAL, 0, true);
    for (int j = 0; j < 9; ++j) addSlot(8 + j * 18, hotbarY, &inv.main[j], SK_NORMAL, 0, true);
}

void updateCraft() { g_result = craftResult(g_grid, g_gridSize); }

void openScreen(Screen s) {
    g_screen = s;
    g_slotCount = 0;
    g_carried.clear();
    for (ItemStack& it : g_grid) it.clear();
    g_result.clear();
    g_tile = nullptr;
    g_cart = -1;
    g_guiH = 166;
    if (s == Screen::Chest && g_game.openCart >= 0) {
        g_cart = g_game.openCart;
        ItemStack* items = g_game.ents[g_cart].cartItems;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 9; ++j) addSlot(8 + j * 18, 18 + i * 18, &items[i * 9 + j], SK_NORMAL, 0, false);
        addPlayerSlots(85, 143);
        g_guiH = 168;
    }
    switch (g_cart >= 0 ? Screen::Chest : s) {
        case Screen::Inventory:
            g_gridSize = 2;
            addSlot(144, 36, &g_result, SK_RESULT, 0, false);
            for (int i = 0; i < 2; ++i)
                for (int j = 0; j < 2; ++j) addSlot(88 + j * 18, 26 + i * 18, &g_grid[i * 2 + j], SK_GRID, 0, false);
            for (int i = 0; i < 4; ++i) addSlot(8, 8 + i * 18, &g_game.inv.armor[i], SK_ARMOR, (uint8_t)i, false);
            addPlayerSlots(84, 142);
            break;
        case Screen::Workbench:
            g_gridSize = 3;
            addSlot(124, 35, &g_result, SK_RESULT, 0, false);
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j) addSlot(30 + j * 18, 17 + i * 18, &g_grid[i * 3 + j], SK_GRID, 0, false);
            addPlayerSlots(84, 142);
            break;
        case Screen::Furnace:
        case Screen::Chest:
        case Screen::Dispenser:
            if (g_cart >= 0) break;
            g_tile = g_game.openTile;
            if (!g_tile) return;
            g_tileX = g_tile->x, g_tileY = g_tile->y, g_tileZ = g_tile->z;
            if (s == Screen::Dispenser) {
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j) addSlot(62 + j * 18, 17 + i * 18, &g_tile->items[i * 3 + j], SK_NORMAL, 0, false);
                addPlayerSlots(84, 142);
            } else if (s == Screen::Furnace) {
                addSlot(56, 17, &g_tile->items[0], SK_NORMAL, 0, false);
                addSlot(56, 53, &g_tile->items[1], SK_NORMAL, 0, false);
                addSlot(116, 35, &g_tile->items[2], SK_OUTPUT, 0, false);
                addPlayerSlots(84, 142);
            } else {
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 9; ++j) addSlot(8 + j * 18, 18 + i * 18, &g_tile->items[i * 9 + j], SK_NORMAL, 0, false);
                addPlayerSlots(85, 143);
                g_guiH = 168;
            }
            break;
    }
    // Start on the selected hotbar slot.
    g_cursor = 0;
    for (int i = 0; i < g_slotCount; ++i)
        if (g_slots[i].s == &g_game.inv.main[g_game.inv.selected]) g_cursor = i;
    g_state = State::Container;
}

void giveOrDrop(ItemStack& s) {
    if (s.empty()) return;
    g_game.inv.add(s);
    if (!s.empty()) {
        const Player& p = g_game.player;
        g_game.dropItem(s, p.x, p.y + Player::EYE - 0.3f, p.z, true);
        s.clear();
    }
}

void closeScreen() {
    for (ItemStack& it : g_grid) giveOrDrop(it);
    giveOrDrop(g_carried);
    g_result.clear();
    g_tile = nullptr;
    g_cart = -1;
    g_game.openTile = nullptr;
    g_game.openCart = -1;
    if (Chunk* c = g_game.world.chunk(g_tileX >> 4, g_tileZ >> 4)) c->flags |= CHUNK_NEEDS_SAVE;
    g_state = State::Playing;
}

int slotMax(const Slot& sl, const ItemStack& s) { return sl.kind == SK_ARMOR ? 1 : itemMaxStack(s.id); }

bool slotAccepts(const Slot& sl, const ItemStack& s) {
    if (sl.kind == SK_RESULT || sl.kind == SK_OUTPUT) return false;
    if (sl.kind == SK_ARMOR) return itemDef(s.id).armorSlot == sl.arg;
    return true;
}

void consumeGrid() {
    for (ItemStack& it : g_grid) {
        if (it.empty()) continue;
        if (it.id == I_MILK_BUCKET)
            it = ItemStack(I_BUCKET);  // SlotCrafting: container item stays in the grid
        else if (--it.count == 0)
            it.clear();
    }
    updateCraft();
}

void takeResult() {
    if (g_result.empty()) return;
    if (g_carried.empty()) {
        g_carried = g_result;
    } else if (g_carried.sameItem(g_result) && g_carried.count + g_result.count <= itemMaxStack(g_carried.id)) {
        g_carried.count = (uint8_t)(g_carried.count + g_result.count);
    } else {
        return;
    }
    consumeGrid();
}

// Moves s into the given slots: first onto matching stacks, then into empty ones.
void mergeInto(ItemStack& s, ItemStack* const* targets, int n, int maxEach = 0) {
    for (int pass = 0; pass < 2 && !s.empty(); ++pass)
        for (int i = 0; i < n && !s.empty(); ++i) {
            ItemStack& t = *targets[i];
            int mx = maxEach ? maxEach : itemMaxStack(s.id);
            if (pass == 0 && !t.empty() && t.sameItem(s) && t.count < mx) {
                int k = mx - t.count < s.count ? mx - t.count : s.count;
                t.count = (uint8_t)(t.count + k);
                s.count = (uint8_t)(s.count - k);
            } else if (pass == 1 && t.empty()) {
                int k = s.count < mx ? s.count : mx;
                t = s;
                t.count = (uint8_t)k;
                s.count = (uint8_t)(s.count - k);
            }
        }
    if (s.count == 0) s.clear();
}

void quickMove(int idx) {
    Slot& sl = g_slots[idx];
    ItemStack& s = *sl.s;
    Inventory& inv = g_game.inv;
    if (sl.kind == SK_RESULT) {
        for (int guard = 0; guard < 64 && !g_result.empty(); ++guard) {
            ItemStack r = g_result;
            ItemStack test = r;
            ItemStack* all[36];
            for (int i = 0; i < 36; ++i) all[i] = &inv.main[i];
            // Only craft when the whole result fits.
            ItemStack backup[36];
            std::memcpy(backup, inv.main, sizeof(backup));
            mergeInto(test, all, 36);
            if (!test.empty()) {
                std::memcpy(inv.main, backup, sizeof(backup));
                break;
            }
            consumeGrid();
        }
        return;
    }
    if (s.empty()) return;
    ItemStack* targets[36];
    int n = 0;
    if (!sl.player) {
        for (int i = 8; i >= 0; --i) targets[n++] = &inv.main[i];
        for (int i = 35; i >= 9; --i) targets[n++] = &inv.main[i];
        mergeInto(s, targets, n);
        if (sl.kind == SK_GRID) updateCraft();
        return;
    }
    bool hotbar = sl.s >= &inv.main[0] && sl.s <= &inv.main[8];
    if (g_screen == Screen::Inventory && itemDef(s.id).armorSlot >= 0 && inv.armor[itemDef(s.id).armorSlot].empty()) {
        ItemStack* a = &inv.armor[itemDef(s.id).armorSlot];
        mergeInto(s, &a, 1, 1);
        return;
    }
    if (g_screen == Screen::Chest && g_cart >= 0) {
        for (int i = 0; i < 27; ++i) targets[n++] = &g_game.ents[g_cart].cartItems[i];
        mergeInto(s, targets, n);
        return;
    }
    if ((g_screen == Screen::Chest || g_screen == Screen::Dispenser) && g_tile) {
        for (int i = 0; i < (g_screen == Screen::Chest ? 27 : 9); ++i) targets[n++] = &g_tile->items[i];
        mergeInto(s, targets, n);
        return;
    }
    if (g_screen == Screen::Furnace && g_tile) {
        ItemStack* slot = nullptr;
        if (!smeltResult(s).empty())
            slot = &g_tile->items[0];
        else if (fuelValue(s) > 0)
            slot = &g_tile->items[1];
        if (slot) {
            mergeInto(s, &slot, 1);
            return;
        }
    }
    if (hotbar)
        for (int i = 9; i < 36; ++i) targets[n++] = &inv.main[i];
    else
        for (int i = 0; i < 9; ++i) targets[n++] = &inv.main[i];
    mergeInto(s, targets, n);
}

void clickSlot(int idx, bool right) {
    Slot& sl = g_slots[idx];
    ItemStack& s = *sl.s;
    if (sl.kind == SK_RESULT) {
        takeResult();
        return;
    }
    if (g_carried.empty()) {
        if (s.empty()) return;
        if (right) {
            int n = (s.count + 1) / 2;
            g_carried = s;
            g_carried.count = (uint8_t)n;
            s.count = (uint8_t)(s.count - n);
            if (s.count == 0) s.clear();
        } else {
            g_carried = s;
            s.clear();
        }
    } else if (sl.kind == SK_OUTPUT) {
        if (!s.empty() && g_carried.sameItem(s) && g_carried.count + s.count <= itemMaxStack(s.id)) {
            g_carried.count = (uint8_t)(g_carried.count + s.count);
            s.clear();
        }
    } else if (slotAccepts(sl, g_carried)) {
        int mx = slotMax(sl, g_carried);
        if (s.empty() || s.sameItem(g_carried)) {
            int room = mx - (s.empty() ? 0 : s.count);
            int k = right ? 1 : g_carried.count;
            if (k > room) k = room;
            if (k > 0) {
                int have = s.empty() ? 0 : s.count;
                s = g_carried;
                s.count = (uint8_t)(have + k);
                g_carried.count = (uint8_t)(g_carried.count - k);
                if (g_carried.count == 0) g_carried.clear();
            }
        } else if (g_carried.count <= mx) {
            ItemStack t = s;
            s = g_carried;
            g_carried = t;
        }
    }
    if (sl.kind == SK_GRID) updateCraft();
}

void moveCursor(int dx, int dy) {
    const Slot& c = g_slots[g_cursor];
    int best = -1;
    float bestScore = 1e9f;
    for (int i = 0; i < g_slotCount; ++i) {
        if (i == g_cursor) continue;
        int ox = g_slots[i].x - c.x, oy = g_slots[i].y - c.y;
        int along = dx ? ox * dx : oy * dy, across = dx ? oy : ox;
        if (along < 4) continue;
        float score = along + std::fabs((float)across) * 2.5f;
        if (score < bestScore) {
            bestScore = score;
            best = i;
        }
    }
    if (best >= 0) g_cursor = best;
}

// ------------------------------------------------------------------ HUD

void drawHotbar() {
    float hx = gfx::SCREEN_W / 2.0f - 91, hy = gfx::SCREEN_H - 22;
    gfx::image(gfx::gui, hx, hy, 182, 22, 0, 0, 182, 22);
    gfx::image(gfx::gui, hx - 1 + g_game.inv.selected * 20, hy - 1, 24, 22, 0, 22, 24, 22);
    for (int i = 0; i < 9; ++i) {
        const ItemStack& s = g_game.inv.main[i];
        if (s.empty()) continue;
        gfx::itemIcon(s, hx + 3 + i * 20, hy + 3);
        gfx::itemOverlay(s, hx + 3 + i * 20, hy + 3);
    }
}

// GuiIngame: hearts, armor and air bubbles.
void drawStatus() {
    const Game& g = g_game;
    static JRandom shake(312871);
    int w = gfx::SCREEN_W, h = gfx::SCREEN_H;
    bool flash = g.hurtResist >= 10 && (g.hurtResist / 3) % 2 == 1;
    int armor = g.inv.armorValue();
    for (int i = 0; i < 10; ++i) {
        float y = h - 32;
        if (armor > 0) {
            float ax = w / 2 + 91 - i * 8 - 9;
            float u = i * 2 + 1 < armor ? 34 : i * 2 + 1 == armor ? 25 : 16;
            gfx::image(gfx::icons, ax, y, 9, 9, u, 9, 9, 9);
        }
        float hx = w / 2 - 91 + i * 8;
        if (g.health <= 4) y += shake.nextInt(2);
        gfx::image(gfx::icons, hx, y, 9, 9, 16 + (flash ? 9 : 0), 0, 9, 9);
        if (flash) {
            if (i * 2 + 1 < g.prevHealth) gfx::image(gfx::icons, hx, y, 9, 9, 70, 0, 9, 9);
            else if (i * 2 + 1 == g.prevHealth) gfx::image(gfx::icons, hx, y, 9, 9, 79, 0, 9, 9);
        }
        if (i * 2 + 1 < g.health) gfx::image(gfx::icons, hx, y, 9, 9, 52, 0, 9, 9);
        else if (i * 2 + 1 == g.health) gfx::image(gfx::icons, hx, y, 9, 9, 61, 0, 9, 9);
    }
    Camera cam = currentCamera();
    if (g.player.eyeInWater(g.world, cam.y)) {
        int full = (int)std::ceil((g.air - 2) * 10.0 / Game::MAX_AIR);
        int popping = (int)std::ceil(g.air * 10.0 / Game::MAX_AIR) - full;
        for (int i = 0; i < full + popping; ++i)
            gfx::image(gfx::icons, w / 2 + 91 - i * 8 - 9, h - 32 - 9, 9, 9, i < full ? 16 : 25, 18, 9, 9);
    }
}

void drawHud(bool crosshair) {
    gfx::begin2D();
    if (g_game.hurtTime > 0) {
        int a = g_game.hurtTime * 10;
        gfx::rect(0, 0, gfx::SCREEN_W, gfx::SCREEN_H, gfx::rgba(160, 0, 0, a));
    }
    float portal = g_game.prevPortalTime + (g_game.portalTime - g_game.prevPortalTime) * g_partial;
    if (portal > 0) {
        if (portal < 1) portal = portal * portal * portal * portal * 0.8f + 0.2f;
        gfx::image(gfx::terrain, 0, 0, gfx::SCREEN_W, gfx::SCREEN_H, 14 * 16, 0, 16, 16,
                   gfx::rgba(255, 255, 255, (int)(portal * 255)));
    }
    drawHotbar();
    if (g_game.survival()) drawStatus();
    if (crosshair) {
        sceGuEnable(GU_ALPHA_TEST);
        sceGuBlendFunc(GU_ADD, GU_ONE_MINUS_DST_COLOR, GU_ONE_MINUS_SRC_COLOR, 0, 0);
        gfx::image(gfx::crosshair, gfx::SCREEN_W / 2.0f - 7, gfx::SCREEN_H / 2.0f - 7, 16, 16, 0, 0, 16, 16);
        sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
        sceGuDisable(GU_ALPHA_TEST);
    }
    const ItemStack& held = g_game.inv.held();
    if (g_itemNameTimer > 0 && !held.empty()) {
        int a = g_itemNameTimer > 0.5 ? 255 : (int)(g_itemNameTimer * 510);
        float y = gfx::SCREEN_H - 22 - (g_game.survival() ? 26 : 14);
        gfx::textCentered(gfx::SCREEN_W / 2.0f, y, itemName(held), gfx::rgba(255, 255, 255, a));
    }
    if (g_settings.debug) {
        char buf[96];
        gfx::text(2, 2, "Minecraft Beta 1.7.3 (PSP)", 0xFFFFFFFF);
        std::snprintf(buf, sizeof(buf), "%.0f fps, %d sections, %dk verts, %d chunks", g_fps, g_renderer.drawnSections,
                      g_renderer.drawnVertices / 1000, g_game.world.loadedChunks);
        gfx::text(2, 12, buf, 0xFFFFFFFF);
        const Player& p = g_game.player;
        std::snprintf(buf, sizeof(buf), "x: %.2f  y: %.2f  z: %.2f", p.x, p.y, p.z);
        gfx::text(2, 22, buf, 0xFFFFFFFF);
        int mobs = 0, ents = 0;
        for (const Entity& e : g_game.ents)
            if (e.type != ENT_NONE) ++ents, mobs += entityIsMob(e.type) ? 1 : 0;
        std::snprintf(buf, sizeof(buf), "Seed: %lld  time %lld  E: %d (%d mobs)", (long long)g_game.world.seed,
                      (long long)(g_game.world.time % 24000), ents, mobs);
        gfx::text(2, 32, buf, 0xFFFFFFFF);
        struct mallinfo mi = mallinfo();
        std::snprintf(buf, sizeof(buf), "Heap: %d KB used, %d KB free", mi.uordblks / 1024, crashlog::memoryFreeKb());
        gfx::text(2, 42, buf, 0xFFFFFFFF);
        std::snprintf(buf, sizeof(buf), "CPU %d%%  GPU %d%%", g_cpuPct, g_gpuPct);
        gfx::text(2, 52, buf, 0xFFFFFFFF);
        std::snprintf(buf, sizeof(buf), "tick %d.%dms  gen %d.%d  mesh %d.%d  draw %d.%d", g_avgTick / 1000,
                      (g_avgTick % 1000) / 100, g_avgStream / 1000, (g_avgStream % 1000) / 100, g_avgMesh / 1000,
                      (g_avgMesh % 1000) / 100, g_avgDraw / 1000, (g_avgDraw % 1000) / 100);
        gfx::text(2, 62, buf, 0xFFFFFFFF);
    }
    if (g_renderer.outOfMemory || g_game.world.outOfMemory)
        gfx::text(2, 74, "Out of memory: lower the render distance", gfx::rgba(255, 96, 96));
    if (g_game.sleepTimer > 0) {
        float f = g_game.sleepTimer / 100.0f;
        if (f > 1.0f) f = 1.0f - (g_game.sleepTimer - 100) / 10.0f;
        gfx::rect(0, 0, gfx::SCREEN_W, gfx::SCREEN_H, gfx::rgba(16, 16, 32, (int)(220 * f)));
    }
    if (g_game.sleeping) gfx::textCentered(gfx::SCREEN_W / 2.0f, gfx::SCREEN_H - 60, "X: Leave bed", 0xFFFFFFFF);
    if (g_message && g_messageTimer > 0) gfx::textCentered(gfx::SCREEN_W / 2.0f, 70, g_message, 0xFFFFFFFF);
}

// ------------------------------------------------------------------ world frame

void renderWorld(bool selection) {
    Camera cam = currentCamera();
    g_renderer.renderDistance = renderDistance();
    // Builds stop after ~4 ms so that a burst of dirty sections (walking, explosions) does not stall a frame.
    crashlog::note("mesh");
    uint32_t tMesh = sceKernelGetSystemTimeLow();
    g_renderer.update(g_game.world, cam, 6, 4000);
    g_usMesh = sceKernelGetSystemTimeLow() - tMesh;
    g_renderer.skyColor = gfx::fromRGB(g_game.skyColor(g_partial));
    g_renderer.fogColor = gfx::fromRGB(g_game.fogColor(g_partial));
    g_renderer.cloudColor = gfx::fromRGB(g_game.cloudColor(g_partial), 204);
    g_renderer.cloudScroll = (float)((g_game.world.time % 68267) + g_partial) * 0.03f;
    g_renderer.celestialAngle = g_game.celestialAngle();
    g_renderer.starBrightness = g_game.starBrightness();
    g_renderer.nether = g_game.dimension < 0;
    g_renderer.crackStage = selection ? g_game.crackStage() : -1;
    uint32_t clear = cam.underwater ? gfx::rgba(10, 20, 60) : g_renderer.fogColor;
    gfx::beginFrame(clear);
    // After present() has waited for the previous frame, so the portal and compass writes do not race the GE.
    gfx::updateDials(g_game.compassAngle, g_game.clockAngle);
    gfx::updatePortal();
    const RayHit* sel = selection && g_game.hasBlockTarget ? &g_game.blockTarget : nullptr;
    crashlog::note("draw");
    uint32_t tDraw = sceKernelGetSystemTimeLow();
    g_renderer.render(g_game.world, cam, sel);
    g_usDraw = sceKernelGetSystemTimeLow() - tDraw;
    crashlog::note("draw-hand");
    if (!g_game.sleeping && !g_game.dead) drawHand(g_game, cam, g_partial, g_hand);
    if (cam.underwater) {
        gfx::begin2D();
        gfx::rect(0, 0, gfx::SCREEN_W, gfx::SCREEN_H, gfx::rgba(20, 40, 160, 60));
    }
}

void runTicks(double dt) {
    g_tickAcc += dt;
    int ticks = 0;
    uint32_t tTick = sceKernelGetSystemTimeLow();
    while (g_tickAcc >= 0.05 && ticks < 5) {
        crashlog::note("tick");
#ifdef MC_AUTOTEST
        uint64_t t0 = sceKernelGetSystemTimeWide();
        g_game.tick();
        float ms = (float)(sceKernelGetSystemTimeWide() - t0) / 1000.0f;
        if (ms > g_tickMs) g_tickMs = ms;
        g_tickSum += ms;
        ++g_tickN;
#else
        g_game.tick();
#endif
        g_hand.tick(g_game);
        g_tickAcc -= 0.05;
        ++ticks;
    }
    if (g_tickAcc > 0.05) g_tickAcc = 0.05;
    g_partial = (float)(g_tickAcc / 0.05);
    g_usTick = sceKernelGetSystemTimeLow() - tTick;
    crashlog::note("stream");
    uint32_t tStream = sceKernelGetSystemTimeLow();
    g_game.stream(renderDistance(), g_game.spawnReady(1) ? 0.007 : 0.025);
    g_usStream = sceKernelGetSystemTimeLow() - tStream;
}

void clearPlayerInput() {
    Player& p = g_game.player;
    p.forward = p.strafe = 0;
    p.jump = p.descend = false;
    g_game.attackHeld = false;
}

void selectSlot(int s) {
    g_game.inv.selected = (s + 9) % 9;
    g_itemNameTimer = 2.0;
}

void playerControls(double now, double dt) {
    Player& p = g_game.player;
    if (g_settings.analogLook) {
        p.yaw += g_in.ax * 180.0f * (float)dt;
        p.pitch += g_in.ay * 140.0f * (float)dt;
        p.forward = (g_in.down(PSP_CTRL_TRIANGLE) ? 1.0f : 0.0f) - (g_in.down(PSP_CTRL_CROSS) ? 1.0f : 0.0f);
        p.strafe = (g_in.down(PSP_CTRL_SQUARE) ? 1.0f : 0.0f) - (g_in.down(PSP_CTRL_CIRCLE) ? 1.0f : 0.0f);
    } else {
        static double turnStart = 0;
        bool turning = g_in.held & (PSP_CTRL_TRIANGLE | PSP_CTRL_CROSS | PSP_CTRL_SQUARE | PSP_CTRL_CIRCLE);
        if (!turning) turnStart = now;
        float accel = (float)(now - turnStart);
        float speed = 140.0f * (1.0f + (accel > 0.5f ? 1.0f : accel * 2.0f) * 0.6f);
        if (g_in.down(PSP_CTRL_SQUARE)) p.yaw -= speed * (float)dt;
        if (g_in.down(PSP_CTRL_CIRCLE)) p.yaw += speed * (float)dt;
        if (g_in.down(PSP_CTRL_TRIANGLE)) p.pitch -= speed * 0.75f * (float)dt;
        if (g_in.down(PSP_CTRL_CROSS)) p.pitch += speed * 0.75f * (float)dt;
        p.forward = -g_in.ay;
        p.strafe = -g_in.ax;
    }
    if (p.pitch > 90) p.pitch = 90;
    if (p.pitch < -90) p.pitch = -90;
    p.yaw = std::fmod(p.yaw, 360.0f);
    p.jump = g_in.down(PSP_CTRL_UP);
    p.descend = g_in.down(PSP_CTRL_DOWN) && !g_game.survival();
    if (g_game.survival() && g_in.hit(PSP_CTRL_DOWN)) g_game.dropHeld(false);

    if (g_in.hit(PSP_CTRL_LEFT)) selectSlot(g_game.inv.selected - 1);
    if (g_in.hit(PSP_CTRL_RIGHT)) selectSlot(g_game.inv.selected + 1);

    g_game.attackHeld = g_in.down(PSP_CTRL_RTRIGGER);
    if (g_in.hit(PSP_CTRL_RTRIGGER)) g_game.attackPressed = true;
    g_useTimer -= dt;
    if (g_in.hit(PSP_CTRL_LTRIGGER) || (g_in.down(PSP_CTRL_LTRIGGER) && g_useTimer <= 0)) {
        g_game.usePressed = true;
        g_useTimer = 0.25;
    }
}

void updateTarget() {
    Camera cam = currentCamera();
    g_game.updateTarget(cam.x, cam.y, cam.z);
}

// ------------------------------------------------------------------ sign editor (GuiEditSign)

// On-screen keyboard: printable rows, then a row of four actions (Space / Delete / ... / Done).
const char* const KB_KEYS[6] = {"ABCDEFGHIJKLM", "NOPQRSTUVWXYZ", "abcdefghijklm",
                                "nopqrstuvwxyz", "0123456789.,!", "?'\"-+=:;()/*#"};
constexpr int KB_COLS = 13, KB_ROWS = 7;
const int KB_ACTION_START[5] = {0, 4, 7, 10, 13};
int g_kbX = 0, g_kbY = 0;

enum KeyboardEvent { KB_NONE, KB_CHAR, KB_SPACE, KB_DELETE, KB_ACTION3, KB_DONE, KB_CANCEL };

// Cursor movement plus X (type / press), [] delete, /\ space, O / START cancel.
KeyboardEvent keyboardInput(double now, double dt, char& ch) {
    if (g_in.repeat(PSP_CTRL_LEFT, now)) g_kbX = (g_kbX + KB_COLS - 1) % KB_COLS;
    if (g_in.repeat(PSP_CTRL_RIGHT, now)) g_kbX = (g_kbX + 1) % KB_COLS;
    if (g_in.repeat(PSP_CTRL_UP, now)) g_kbY = (g_kbY + KB_ROWS - 1) % KB_ROWS;
    if (g_in.repeat(PSP_CTRL_DOWN, now)) g_kbY = (g_kbY + 1) % KB_ROWS;
    static double stickTimer = 0;
    stickTimer -= dt;
    if (stickTimer <= 0 && (std::fabs(g_in.ax) > 0.6f || std::fabs(g_in.ay) > 0.6f)) {
        if (std::fabs(g_in.ax) > std::fabs(g_in.ay))
            g_kbX = (g_kbX + (g_in.ax > 0 ? 1 : KB_COLS - 1)) % KB_COLS;
        else
            g_kbY = (g_kbY + (g_in.ay > 0 ? 1 : KB_ROWS - 1)) % KB_ROWS;
        stickTimer = 0.15;
    }
    if (g_in.repeat(PSP_CTRL_SQUARE, now)) return KB_DELETE;
    if (g_in.repeat(PSP_CTRL_TRIANGLE, now)) return KB_SPACE;
    if (g_in.repeat(PSP_CTRL_CROSS, now)) {
        if (g_kbY < 6) {
            ch = KB_KEYS[g_kbY][g_kbX];
            return KB_CHAR;
        }
        int a = 0;
        while (g_kbX >= KB_ACTION_START[a + 1]) ++a;
        return (KeyboardEvent)(KB_SPACE + a);
    }
    if (g_in.hit(PSP_CTRL_START) || g_in.hit(PSP_CTRL_CIRCLE) || g_in.hit(PSP_CTRL_SELECT)) return KB_CANCEL;
    return KB_NONE;
}

void drawKeyboard(float ky, const char* const (&actions)[4]) {
    const float cw = 20, ch = 17, kx = (gfx::SCREEN_W - KB_COLS * cw) / 2;
    for (int r = 0; r < KB_ROWS; ++r) {
        if (r < 6) {
            for (int c = 0; c < KB_COLS; ++c) {
                bool sel = r == g_kbY && c == g_kbX;
                gfx::rect(kx + c * cw, ky + r * ch, cw - 2, ch - 2, sel ? gfx::rgba(160, 160, 220) : gfx::rgba(80, 80, 80, 220));
                char k[2] = {KB_KEYS[r][c], 0};
                gfx::textCentered(kx + c * cw + (cw - 2) / 2, ky + r * ch + 4, k, 0xFFFFFFFF);
            }
        } else {
            for (int a = 0; a < 4; ++a) {
                float x = kx + KB_ACTION_START[a] * cw, w = (KB_ACTION_START[a + 1] - KB_ACTION_START[a]) * cw;
                bool sel = r == g_kbY && g_kbX >= KB_ACTION_START[a] && g_kbX < KB_ACTION_START[a + 1];
                gfx::rect(x, ky + r * ch, w - 2, ch - 2, sel ? gfx::rgba(160, 160, 220) : gfx::rgba(80, 80, 80, 220));
                gfx::textCentered(x + (w - 2) / 2, ky + r * ch + 4, actions[a], 0xFFFFFFFF);
            }
        }
    }
}

void textAppend(char* s, int max, char ch) {
    size_t n = strnlen(s, max);
    if ((int)n < max) s[n] = ch, s[n + 1] = 0;
}

void textBackspace(char* s, int max) {
    size_t n = strnlen(s, max);
    if (n) s[n - 1] = 0;
}

// ------------------------------------------------------------------ sign editor (GuiEditSign)

TileEntity* g_signTile = nullptr;
int g_signX = 0, g_signY = 0, g_signZ = 0, g_signLine = 0, g_signTicks = 0;

void openSignEditor(TileEntity* t) {
    g_signTile = t;
    g_signX = t->x, g_signY = t->y, g_signZ = t->z;
    g_signLine = 0;
    g_kbX = g_kbY = 0;
    g_state = State::Sign;
}

void closeSignEditor() {
    if (g_signTile && g_game.world.tileAt(g_signX, g_signY, g_signZ) == g_signTile)
        if (Chunk* c = g_game.world.chunk(g_signX >> 4, g_signZ >> 4)) c->flags |= CHUNK_NEEDS_SAVE;
    g_signTile = nullptr;
    g_game.openTile = nullptr;
    g_state = State::Playing;
}

void signFrame(double now, double dt) {
    if (g_game.world.tileAt(g_signX, g_signY, g_signZ) != g_signTile || !g_signTile) {
        closeSignEditor();
        return;
    }
    ++g_signTicks;
    char* line = g_signTile->text[g_signLine];
    if (g_in.hit(PSP_CTRL_LTRIGGER)) g_signLine = (g_signLine + 3) & 3;
    if (g_in.hit(PSP_CTRL_RTRIGGER)) g_signLine = (g_signLine + 1) & 3;
    char ch = 0;
    switch (keyboardInput(now, dt, ch)) {
        case KB_CHAR: textAppend(line, 15, ch); break;
        case KB_SPACE: textAppend(line, 15, ' '); break;
        case KB_DELETE: textBackspace(line, 15); break;
        case KB_ACTION3: g_signLine = (g_signLine + 1) & 3; break;
        case KB_DONE:
        case KB_CANCEL: closeSignEditor(); return;
        default: break;
    }

    clearPlayerInput();
    renderWorld(false);
    drawHud(false);
    gfx::gradient(0, 0, gfx::SCREEN_W, gfx::SCREEN_H, gfx::rgba(16, 16, 16, 192), gfx::rgba(16, 16, 16, 208));
    gfx::textCentered(gfx::SCREEN_W / 2.0f, 6, "Edit sign message:", 0xFFFFFFFF);
    // The board's front face from item/sign.png at 6x.
    const float bw = 24 * 6, bh = 12 * 6, bx = (gfx::SCREEN_W - bw) / 2, by = 18;
    gfx::image(gfx::sign, bx, by, bw, bh, 2, 2, 24, 12);
    for (int i = 0; i < 4; ++i) {
        char buf[24];
        const char* s = g_signTile->text[i];
        if (i == g_signLine && (g_signTicks / 6) % 2 == 0)
            std::snprintf(buf, sizeof(buf), "> %.15s <", s);
        else
            std::snprintf(buf, sizeof(buf), "%.15s", s);
        gfx::textCentered(gfx::SCREEN_W / 2.0f, by + 16 + i * 10, buf, gfx::rgba(0, 0, 0), false);
    }
    static const char* const ACTIONS[4] = {"Space", "Del", "Line", "Done"};
    drawKeyboard(by + bh + 8, ACTIONS);
    gfx::text(4, gfx::SCREEN_H - 10, "X: type  []: delete  /\\: space  L/R: line  O: done", gfx::rgba(255, 255, 255, 200));
    gfx::endFrame();
}

void playingFrame(double now, double dt) {
    if (g_in.hit(PSP_CTRL_START)) {
        g_state = State::Paused;
        g_menu = 0;
        clearPlayerInput();
        return;
    }
    if (g_in.hit(PSP_CTRL_SELECT)) {
        clearPlayerInput();
        if (g_game.survival()) {
            openScreen(Screen::Inventory);
        } else {
            g_state = State::Picker;
            const ItemStack& h = g_game.inv.held();
            for (int i = 0; i < g_creativeItemCount; ++i)
                if (g_creativeItems[i].id == h.id && g_creativeItems[i].damage == h.damage) g_pickerCursor = i;
        }
        return;
    }
    if (g_game.sleeping) {
        clearPlayerInput();
        if (g_in.hit(PSP_CTRL_CROSS) || g_in.hit(PSP_CTRL_CIRCLE)) g_game.wakeUp(false);
    } else {
        playerControls(now, dt);
    }
    runTicks(dt);
    if (g_game.portalPending && !g_game.dead) {
        changeDimension(true);
        return;
    }
    if (g_game.message) {
        showMessage(g_game.message);
        g_game.message = nullptr;
    }
    updateTarget();
    UseResult use = g_game.lastUse;
    g_game.lastUse = UseResult::None;
    if (g_game.dead) {
        g_state = State::Dead;
        g_menu = 0;
        clearPlayerInput();
    } else if (use == UseResult::Workbench) {
        clearPlayerInput();
        openScreen(Screen::Workbench);
    } else if (use == UseResult::Furnace || use == UseResult::Chest || use == UseResult::Dispenser) {
        clearPlayerInput();
        openScreen(use == UseResult::Furnace ? Screen::Furnace
                   : use == UseResult::Chest ? Screen::Chest
                                             : Screen::Dispenser);
        if (!g_tile) g_state = State::Playing;
    } else if (use == UseResult::CartChest && g_game.openCart >= 0) {
        clearPlayerInput();
        openScreen(Screen::Chest);
    } else if (use == UseResult::Sign && g_game.openTile) {
        clearPlayerInput();
        openSignEditor(g_game.openTile);
    }
    renderWorld(g_state == State::Playing);
    drawHud(g_state == State::Playing);
    gfx::endFrame();
}

// World keeps running behind menus that don't pause (containers, death screen).
void backgroundFrame(double dt, bool tick) {
    clearPlayerInput();
    if (tick) runTicks(dt);
    renderWorld(false);
    drawHud(false);
}

// ------------------------------------------------------------------ container screen

void containerFrame(double now, double dt) {
    if (g_tile && g_game.world.tileAt(g_tileX, g_tileY, g_tileZ) != g_tile) {
        g_tile = nullptr;
        g_slotCount = 0;
        closeScreen();
        return;
    }
    if (g_cart >= 0) {
        const Entity& c = g_game.ents[g_cart];
        float dx = c.x - g_game.player.x, dy = c.y - g_game.player.y, dz = c.z - g_game.player.z;
        if (c.type != ENT_MINECART || c.dead || g_game.openCart != g_cart || dx * dx + dy * dy + dz * dz > 64.0f) {
            g_slotCount = 0;
            closeScreen();
            return;
        }
    }
    if (g_in.repeat(PSP_CTRL_LEFT, now)) moveCursor(-1, 0);
    if (g_in.repeat(PSP_CTRL_RIGHT, now)) moveCursor(1, 0);
    if (g_in.repeat(PSP_CTRL_UP, now)) moveCursor(0, -1);
    if (g_in.repeat(PSP_CTRL_DOWN, now)) moveCursor(0, 1);
    static double stickTimer = 0;
    stickTimer -= dt;
    if (stickTimer <= 0 && (std::fabs(g_in.ax) > 0.6f || std::fabs(g_in.ay) > 0.6f)) {
        if (std::fabs(g_in.ax) > std::fabs(g_in.ay))
            moveCursor(g_in.ax > 0 ? 1 : -1, 0);
        else
            moveCursor(0, g_in.ay > 0 ? 1 : -1);
        stickTimer = 0.15;
    }
    if (g_slotCount > 0) {
        if (g_in.hit(PSP_CTRL_CROSS)) clickSlot(g_cursor, false);
        if (g_in.hit(PSP_CTRL_SQUARE)) clickSlot(g_cursor, true);
        if (g_in.hit(PSP_CTRL_TRIANGLE)) quickMove(g_cursor);
    }
    if (g_in.hit(PSP_CTRL_CIRCLE) || g_in.hit(PSP_CTRL_SELECT) || g_in.hit(PSP_CTRL_START)) {
        closeScreen();
        return;
    }
    if (g_in.hit(PSP_CTRL_LTRIGGER)) selectSlot(g_game.inv.selected - 1);
    if (g_in.hit(PSP_CTRL_RTRIGGER)) selectSlot(g_game.inv.selected + 1);

    backgroundFrame(dt, true);
    if (g_game.dead) {
        closeScreen();
        g_state = State::Dead;
        return;
    }
    gfx::gradient(0, 0, gfx::SCREEN_W, gfx::SCREEN_H, gfx::rgba(16, 16, 16, 192), gfx::rgba(16, 16, 16, 208));
    const float x0 = (gfx::SCREEN_W - 176) / 2, y0 = (gfx::SCREEN_H - g_guiH) / 2;
    const uint32_t label = gfx::rgba(64, 64, 64);
    switch (g_screen) {
        case Screen::Inventory:
            gfx::image(gfx::invGui, x0, y0, 176, 166, 0, 0, 176, 166);
            gfx::text(x0 + 86, y0 + 16, "Crafting", label, false);
            break;
        case Screen::Workbench:
            gfx::image(gfx::craftGui, x0, y0, 176, 166, 0, 0, 176, 166);
            gfx::text(x0 + 28, y0 + 6, "Crafting", label, false);
            gfx::text(x0 + 8, y0 + 72, "Inventory", label, false);
            break;
        case Screen::Furnace:
            gfx::image(gfx::furnaceGui, x0, y0, 176, 166, 0, 0, 176, 166);
            gfx::text(x0 + 60, y0 + 6, "Furnace", label, false);
            gfx::text(x0 + 8, y0 + 72, "Inventory", label, false);
            if (g_tile) {
                if (g_tile->burnTime > 0 && g_tile->burnTotal > 0) {
                    int j = g_tile->burnTime * 12 / g_tile->burnTotal;
                    gfx::image(gfx::furnaceGui, x0 + 56, y0 + 36 + 12 - j, 14, j + 2, 176, 12 - j, 14, j + 2);
                }
                int cook = g_tile->cookTime * 24 / 200;
                gfx::image(gfx::furnaceGui, x0 + 79, y0 + 34, cook + 1, 16, 176, 14, cook + 1, 16);
            }
            break;
        case Screen::Chest:
            gfx::image(gfx::chestGui, x0, y0, 176, 3 * 18 + 17, 0, 0, 176, 3 * 18 + 17);
            gfx::image(gfx::chestGui, x0, y0 + 3 * 18 + 17, 176, 96, 0, 126, 176, 96);
            gfx::text(x0 + 8, y0 + 6, g_cart >= 0 ? "Minecart" : "Chest", label, false);
            gfx::text(x0 + 8, y0 + 3 * 18 + 17 + 2, "Inventory", label, false);
            break;
        case Screen::Dispenser:
            gfx::image(gfx::dispenserGui, x0, y0, 176, 166, 0, 0, 176, 166);
            gfx::text(x0 + 60, y0 + 6, "Dispenser", label, false);
            gfx::text(x0 + 8, y0 + 72, "Inventory", label, false);
            break;
    }
    for (int i = 0; i < g_slotCount; ++i) {
        const Slot& sl = g_slots[i];
        float sx = x0 + sl.x, sy = y0 + sl.y;
        if (sl.kind == SK_ARMOR && sl.s->empty()) {
            int icon = 15 + sl.arg * 16;  // SlotArmor.getBackgroundIconIndex
            gfx::image(gfx::items, sx, sy, 16, 16, (icon & 15) * 16.0f, (icon >> 4) * 16.0f, 16, 16);
        }
        gfx::itemIcon(*sl.s, sx, sy);
        gfx::itemOverlay(*sl.s, sx, sy);
        if (i == g_cursor) gfx::rect(sx, sy, 16, 16, gfx::rgba(255, 255, 255, 128));
    }
    if (g_slotCount > 0) {
        const Slot& c = g_slots[g_cursor];
        float cx = x0 + c.x, cy = y0 + c.y;
        if (!g_carried.empty()) {
            gfx::itemIcon(g_carried, cx + 6, cy - 6);
            gfx::itemOverlay(g_carried, cx + 6, cy - 6);
        } else if (!c.s->empty()) {
            const char* name = itemName(*c.s);
            float tw = gfx::textWidth(name);
            float tx = cx + 12, ty = cy - 12;
            if (tx + tw + 3 > gfx::SCREEN_W) tx = gfx::SCREEN_W - tw - 3;
            gfx::rect(tx - 3, ty - 3, tw + 6, 14, gfx::rgba(0, 0, 0, 192));
            gfx::text(tx, ty, name, 0xFFFFFFFF);
        }
    }
    gfx::text(4, gfx::SCREEN_H - 10, "X: take/put  []: half/one  /\\: move  O: close", gfx::rgba(255, 255, 255, 200));
    gfx::endFrame();
}

// ------------------------------------------------------------------ menus

struct SlotInfo {
    bool used;
    Game::LevelInfo info;
};
SlotInfo g_slotInfo[WORLD_SLOTS];
int g_worldSel = 0;
bool g_confirmDelete = false;

void refreshSlots() {
    for (int i = 0; i < WORLD_SLOTS; ++i) {
        char dir[16];
        slotDir(i, dir, sizeof(dir));
        g_slotInfo[i].used = Game::readLevelInfo(dir, g_slotInfo[i].info);
    }
}

void titleFrame(double now) {
    const char* labels[2] = {"Singleplayer", "Quit Game"};
    if (g_menu > 1) g_menu = 0;
    g_menu = menuNavigate(g_menu, 2, now);
    if (g_in.hit(PSP_CTRL_CROSS)) {
        if (g_menu == 0) {
            refreshSlots();
            g_confirmDelete = false;
            g_state = State::SelectWorld;
            return;
        }
        g_exitRequested = true;
        return;
    }

    gfx::beginFrame(0xFF000000);
    gfx::begin2D();
    gfx::tiledBackground(gfx::rgba(64, 64, 64));
    float lx = gfx::SCREEN_W / 2.0f - 155;
    gfx::image(gfx::logo, lx, 24, 155, 44, 0, 0, 155, 44);
    gfx::image(gfx::logo, lx + 155, 24, 155, 44, 0, 45, 155, 44);
    float pulse = 1.8f - std::fabs(std::sin((float)std::fmod(now, 1.0) * 6.2831853f) * 0.1f);
    const char* splash = "Now on PSP!";
    pulse = pulse * 100.0f / (gfx::textWidth(splash) + 32);
    gfx::textRotated(gfx::SCREEN_W / 2.0f + 90, 70, splash, gfx::rgba(255, 255, 0), -20.0f, pulse);
    for (int i = 0; i < 2; ++i) drawButton(gfx::SCREEN_W / 2.0f - 100, 122 + i * 24, 200, labels[i], g_menu == i);
    gfx::text(2, gfx::SCREEN_H - 10, "Minecraft Beta 1.7.3 (PSP)", gfx::rgba(255, 255, 255));
    const char* hint = "X: select";
    gfx::text(gfx::SCREEN_W - gfx::textWidth(hint) - 2, gfx::SCREEN_H - 10, hint, gfx::rgba(255, 255, 255));
    if (g_message && g_messageTimer > 0) gfx::textCentered(gfx::SCREEN_W / 2.0f, 100, g_message, gfx::rgba(255, 96, 96));
    gfx::endFrame();
}

// ------------------------------------------------------------------ text entry (world names, seeds)

char* g_textTarget = nullptr;
int g_textMax = 0;
char g_textBackup[32];
const char* g_textTitle = "";
State g_textReturn = State::Title;
void (*g_textDone)() = nullptr;

void openTextEntry(const char* title, char* target, int max, State back, void (*done)()) {
    g_textTitle = title;
    g_textTarget = target;
    g_textMax = max < (int)sizeof(g_textBackup) - 1 ? max : (int)sizeof(g_textBackup) - 1;
    std::snprintf(g_textBackup, sizeof(g_textBackup), "%s", target);
    g_textReturn = back;
    g_textDone = done;
    g_kbX = g_kbY = 0;
    g_state = State::TextEntry;
}

void textEntryFrame(double now, double dt) {
    char ch = 0;
    switch (keyboardInput(now, dt, ch)) {
        case KB_CHAR: textAppend(g_textTarget, g_textMax, ch); break;
        case KB_SPACE: textAppend(g_textTarget, g_textMax, ' '); break;
        case KB_DELETE: textBackspace(g_textTarget, g_textMax); break;
        case KB_ACTION3: g_textTarget[0] = 0; break;
        case KB_DONE:
            g_state = g_textReturn;
            if (g_textDone) g_textDone();
            return;
        case KB_CANCEL:
            std::snprintf(g_textTarget, g_textMax + 1, "%s", g_textBackup);
            g_state = g_textReturn;
            return;
        default: break;
    }
    gfx::beginFrame(0xFF000000);
    gfx::begin2D();
    gfx::tiledBackground(gfx::rgba(64, 64, 64));
    gfx::textCentered(gfx::SCREEN_W / 2.0f, 12, g_textTitle, 0xFFFFFFFF);
    const float fw = 240, fx = (gfx::SCREEN_W - fw) / 2, fy = 30;
    gfx::rect(fx - 1, fy - 1, fw + 2, 22, gfx::rgba(160, 160, 160));
    gfx::rect(fx, fy, fw, 20, gfx::rgba(0, 0, 0));
    char buf[48];
    bool blink = (int)(now * 3) % 2 == 0;
    std::snprintf(buf, sizeof(buf), "%s%s", g_textTarget, blink ? "_" : "");
    gfx::text(fx + 4, fy + 6, buf, gfx::rgba(224, 224, 224));
    static const char* const ACTIONS[4] = {"Space", "Del", "Clear", "Done"};
    drawKeyboard(64, ACTIONS);
    gfx::text(4, gfx::SCREEN_H - 10, "X: type  []: delete  /\\: space  O: cancel", gfx::rgba(255, 255, 255, 200));
    gfx::endFrame();
}

// ------------------------------------------------------------------ world list (GuiSelectWorld)

char g_renameBuf[32];

void renameDone() {
    char dir[16];
    slotDir(g_worldSel, dir, sizeof(dir));
    if (g_renameBuf[0] && !Game::renameLevel(dir, g_renameBuf)) showMessage("Could not rename the world");
    refreshSlots();
}

int g_createSlot = 0;
char g_newName[32] = "New World";
char g_newSeed[32] = "";

void openCreateWorld(int slot) {
    g_createSlot = slot;
    g_newMode = MODE_SURVIVAL;
    std::snprintf(g_newName, sizeof(g_newName), "New World");
    g_newSeed[0] = 0;
    g_menu = 0;
    g_state = State::CreateWorld;
}

void selectWorldFrame(double now) {
    const SlotInfo& sel = g_slotInfo[g_worldSel];
    if (g_confirmDelete) {
        g_menu = menuNavigate(g_menu, 2, now);
        if (g_in.hit(PSP_CTRL_CIRCLE)) {
            g_confirmDelete = false;
        } else if (g_in.hit(PSP_CTRL_CROSS)) {
            if (g_menu == 0) {
                char dir[16];
                slotDir(g_worldSel, dir, sizeof(dir));
                Game::deleteLevel(dir);
                refreshSlots();
            }
            g_confirmDelete = false;
        }
        gfx::beginFrame(0xFF000000);
        gfx::begin2D();
        gfx::tiledBackground(gfx::rgba(64, 64, 64));
        gfx::textCentered(gfx::SCREEN_W / 2.0f, 80, "Are you sure you want to delete this world?", 0xFFFFFFFF);
        char buf[96];
        std::snprintf(buf, sizeof(buf), "'%s' will be lost forever! (A long time!)", sel.info.name);
        gfx::textCentered(gfx::SCREEN_W / 2.0f, 100, buf, gfx::rgba(160, 160, 160));
        drawButton(gfx::SCREEN_W / 2.0f - 155, 140, 150, "Delete", g_menu == 0);
        drawButton(gfx::SCREEN_W / 2.0f + 5, 140, 150, "Cancel", g_menu == 1);
        gfx::endFrame();
        return;
    }
    g_worldSel = menuNavigate(g_worldSel, WORLD_SLOTS, now);
    const SlotInfo& s = g_slotInfo[g_worldSel];
    if (g_in.hit(PSP_CTRL_CIRCLE)) {
        g_state = State::Title;
        g_menu = 0;
        return;
    }
    if (g_in.hit(PSP_CTRL_CROSS)) {
        if (!s.used) {
            openCreateWorld(g_worldSel);
            return;
        }
        char dir[16];
        slotDir(g_worldSel, dir, sizeof(dir));
        if (!loadSlot(dir)) {
            showMessage("Could not load the world");
            g_state = State::SelectWorld;
            refreshSlots();
        }
        return;
    }
    if (s.used && g_in.hit(PSP_CTRL_TRIANGLE)) {
        std::snprintf(g_renameBuf, sizeof(g_renameBuf), "%s", s.info.name);
        openTextEntry("Rename World", g_renameBuf, 24, State::SelectWorld, renameDone);
        return;
    }
    if (s.used && g_in.hit(PSP_CTRL_SQUARE)) {
        g_confirmDelete = true;
        g_menu = 1;
        return;
    }

    gfx::beginFrame(0xFF000000);
    gfx::begin2D();
    gfx::tiledBackground(gfx::rgba(64, 64, 64));
    gfx::textCentered(gfx::SCREEN_W / 2.0f, 8, "Select World", 0xFFFFFFFF);
    const float w = 300, x = (gfx::SCREEN_W - w) / 2;
    for (int i = 0; i < WORLD_SLOTS; ++i) {
        float y = 24 + i * 44;
        const SlotInfo& o = g_slotInfo[i];
        if (i == g_worldSel) {
            gfx::rect(x - 2, y - 2, w + 4, 40, gfx::rgba(128, 128, 128));
            gfx::rect(x - 1, y - 1, w + 2, 38, gfx::rgba(0, 0, 0));
        }
        if (!o.used) {
            char buf[48];
            std::snprintf(buf, sizeof(buf), "Slot %d: - empty -", i + 1);
            gfx::text(x + 4, y + 4, buf, gfx::rgba(128, 128, 128));
            continue;
        }
        gfx::text(x + 4, y + 2, o.info.name, 0xFFFFFFFF);
        char dir[16], date[32] = "", line[96];
        slotDir(i, dir, sizeof(dir));
        if (o.info.lastPlayed > 0) {
            time_t t = (time_t)o.info.lastPlayed;
            struct tm tmv;
            gmtime_r(&t, &tmv);
            std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M", &tmv);
        }
        std::snprintf(line, sizeof(line), "%s (%s)", dir, date[0] ? date : "never");
        gfx::text(x + 4, y + 13, line, gfx::rgba(128, 128, 128));
        std::snprintf(line, sizeof(line), "%s Mode, seed %lld", o.info.mode == MODE_CREATIVE ? "Creative" : "Survival",
                      (long long)o.info.seed);
        gfx::text(x + 4, y + 24, line, gfx::rgba(128, 128, 128));
    }
    const char* hint = s.used ? "X: play  /\\: rename  []: delete  O: back" : "X: create new world  O: back";
    gfx::textCentered(gfx::SCREEN_W / 2.0f, gfx::SCREEN_H - 12, hint, gfx::rgba(255, 255, 255, 200));
    if (g_message && g_messageTimer > 0) gfx::textCentered(gfx::SCREEN_W / 2.0f, 12 + 8, g_message, gfx::rgba(255, 96, 96));
    gfx::endFrame();
}

// ------------------------------------------------------------------ GuiCreateWorld

// java.lang.String.hashCode
int32_t javaHash(const char* s) {
    uint32_t h = 0;
    for (; *s; ++s) h = h * 31u + (uint8_t)*s;
    return (int32_t)h;
}

void createWorldFrame(double now) {
    g_menu = menuNavigate(g_menu, 5, now);
    if (g_in.hit(PSP_CTRL_CIRCLE)) {
        g_state = State::SelectWorld;
        return;
    }
    if (g_in.hit(PSP_CTRL_CROSS) || (g_menu == 2 && (g_in.hit(PSP_CTRL_LEFT) || g_in.hit(PSP_CTRL_RIGHT)))) {
        switch (g_menu) {
            case 0: openTextEntry("World Name", g_newName, 24, State::CreateWorld, nullptr); return;
            case 1: openTextEntry("Seed for the World Generator", g_newSeed, 31, State::CreateWorld, nullptr); return;
            case 2: g_newMode = g_newMode == MODE_SURVIVAL ? MODE_CREATIVE : MODE_SURVIVAL; break;
            case 3: {
                JRandom r((int64_t)sceKernelGetSystemTimeWide());
                int64_t seed = r.nextLong();
                if (g_newSeed[0]) {
                    char* end = nullptr;
                    long long v = std::strtoll(g_newSeed, &end, 10);
                    if (end && *end == 0) {
                        if (v != 0) seed = v;
                    } else {
                        seed = javaHash(g_newSeed);
                    }
                }
                char dir[16];
                slotDir(g_createSlot, dir, sizeof(dir));
                newWorld(dir, g_newName[0] ? g_newName : "New World", seed, g_newMode);
                return;
            }
            default: g_state = State::SelectWorld; return;
        }
    }
    gfx::beginFrame(0xFF000000);
    gfx::begin2D();
    gfx::tiledBackground(gfx::rgba(64, 64, 64));
    gfx::textCentered(gfx::SCREEN_W / 2.0f, 12, "Create New World", 0xFFFFFFFF);
    char buf[64], dir[16];
    slotDir(g_createSlot, dir, sizeof(dir));
    std::snprintf(buf, sizeof(buf), "World Name: %s", g_newName);
    drawButton(gfx::SCREEN_W / 2.0f - 100, 34, 200, buf, g_menu == 0);
    std::snprintf(buf, sizeof(buf), "Will be saved in: %s", dir);
    gfx::textCentered(gfx::SCREEN_W / 2.0f, 58, buf, gfx::rgba(160, 160, 160));
    std::snprintf(buf, sizeof(buf), "Seed: %s", g_newSeed[0] ? g_newSeed : "(random)");
    drawButton(gfx::SCREEN_W / 2.0f - 100, 76, 200, buf, g_menu == 1);
    gfx::textCentered(gfx::SCREEN_W / 2.0f, 100, "Leave blank for a random seed", gfx::rgba(160, 160, 160));
    std::snprintf(buf, sizeof(buf), "Game Mode: %s", g_newMode == MODE_SURVIVAL ? "Survival" : "Creative");
    drawButton(gfx::SCREEN_W / 2.0f - 100, 118, 200, buf, g_menu == 2);
    const char* desc = g_newMode == MODE_SURVIVAL ? "Search for resources, crafting, gain health and survive"
                                                  : "Unlimited blocks, free flying, no damage";
    gfx::textCentered(gfx::SCREEN_W / 2.0f, 142, desc, gfx::rgba(160, 160, 160));
    drawButton(gfx::SCREEN_W / 2.0f - 100, 170, 200, "Create New World", g_menu == 3);
    drawButton(gfx::SCREEN_W / 2.0f - 100, 194, 200, "Cancel", g_menu == 4);
    gfx::endFrame();
}

void pausedFrame(double now) {
    char rd[48], gr[48], fly[48], aj[48], ctl[48], dbg[48], mus[48], snd[48], part[48], cld[48];
    auto percent = [](char* out, const char* what, int v) {
        if (v)
            std::snprintf(out, 48, "%s: %d%%", what, v * 25);
        else
            std::snprintf(out, 48, "%s: OFF", what);
    };
    percent(mus, "Music", g_settings.music);
    percent(snd, "Sound", g_settings.sound);
    std::snprintf(rd, sizeof(rd), "Render distance: %s", RENDER_NAMES[g_settings.renderDistance]);
    std::snprintf(gr, sizeof(gr), "Graphics: %s", g_settings.fancy ? "Fancy" : "Fast");
    std::snprintf(fly, sizeof(fly), "Flying: %s", g_game.player.flying ? "ON" : "OFF");
    std::snprintf(aj, sizeof(aj), "Auto-jump: %s", g_settings.autoJump ? "ON" : "OFF");
    std::snprintf(ctl, sizeof(ctl), "Analog stick: %s", g_settings.analogLook ? "Look" : "Move");
    std::snprintf(dbg, sizeof(dbg), "Debug info: %s", g_settings.debug ? "ON" : "OFF");
    std::snprintf(part, sizeof(part), "Particles: %s", PARTICLE_NAMES[g_settings.particles]);
    std::snprintf(cld, sizeof(cld), "Clouds: %s", g_settings.clouds ? "ON" : "OFF");
    enum { BACK, MUS, SND, RD, GR, PART, CLD, FLY, AJ, CTL, DBG, QUIT };
    int ids[12], n = 0;
    const char* labels[12];
    auto add = [&](int id, const char* l) { ids[n] = id, labels[n++] = l; };
    add(BACK, "Back to Game");
    add(MUS, mus);
    add(SND, snd);
    add(RD, rd);
    add(GR, gr);
    add(PART, part);
    add(CLD, cld);
    if (!g_game.survival()) add(FLY, fly);
    add(AJ, aj);
    add(CTL, ctl);
    add(DBG, dbg);
    add(QUIT, "Save and Quit to Title");
    if (g_menu >= n) g_menu = n - 1;
    g_menu = menuNavigate(g_menu, n, now);
    if (g_in.hit(PSP_CTRL_START) || g_in.hit(PSP_CTRL_CIRCLE)) {
        g_state = State::Playing;
        saveSettings();
        return;
    }
    if (ids[g_menu] == MUS || ids[g_menu] == SND) {
        int& v = ids[g_menu] == MUS ? g_settings.music : g_settings.sound;
        int d = g_in.hit(PSP_CTRL_RIGHT) || g_in.hit(PSP_CTRL_CROSS) ? 1 : g_in.hit(PSP_CTRL_LEFT) ? -1 : 0;
        if (d) {
            v = g_in.hit(PSP_CTRL_CROSS) ? (v + 1) % 5 : v + d < 0 ? 0 : v + d > 4 ? 4 : v + d;
            applySettings();
        }
    } else if (g_in.hit(PSP_CTRL_CROSS)) {
        switch (ids[g_menu]) {
            case BACK:
                g_state = State::Playing;
                saveSettings();
                return;
            case RD:
                g_settings.renderDistance = (g_settings.renderDistance + 1) % RENDER_OPTIONS;
                applySettings();
                break;
            case GR:
                g_settings.fancy = !g_settings.fancy;
                applySettings();
                g_game.world.markAllDirty();
                g_renderer.buildNear(g_game.world, currentCamera(), renderDistance());
                break;
            case PART:
                g_settings.particles = (g_settings.particles + 1) % 3;
                applySettings();
                break;
            case CLD:
                g_settings.clouds = !g_settings.clouds;
                applySettings();
                break;
            case FLY: g_game.player.flying = !g_game.player.flying; break;
            case AJ:
                g_settings.autoJump = !g_settings.autoJump;
                applySettings();
                break;
            case CTL: g_settings.analogLook = !g_settings.analogLook; break;
            case DBG: g_settings.debug = !g_settings.debug; break;
            case QUIT:
                saveSettings();
                quitToTitle();
                return;
        }
    }
    clearPlayerInput();
    renderWorld(false);
    drawHud(false);
    gfx::gradient(0, 0, gfx::SCREEN_W, gfx::SCREEN_H, gfx::rgba(16, 16, 16, 192), gfx::rgba(16, 16, 16, 208));
    gfx::textCentered(gfx::SCREEN_W / 2.0f, 10, "Game menu", 0xFFFFFFFF);
    float step = n > 10 ? 20.5f : 22.0f, y = 24 + (10 - n) * step / 2;
    if (y < 22) y = 22;
    for (int i = 0; i < n; ++i) drawButton(gfx::SCREEN_W / 2.0f - 100, y + i * step, 200, labels[i], g_menu == i);
    gfx::endFrame();
}

void pickerFrame(double now, double dt) {
    const int COLS = 9;
    int count = g_creativeItemCount;
    int rows = (count + COLS - 1) / COLS;
    if (g_in.repeat(PSP_CTRL_LEFT, now)) g_pickerCursor = (g_pickerCursor + count - 1) % count;
    if (g_in.repeat(PSP_CTRL_RIGHT, now)) g_pickerCursor = (g_pickerCursor + 1) % count;
    if (g_in.repeat(PSP_CTRL_UP, now)) g_pickerCursor = (g_pickerCursor - COLS + rows * COLS) % (rows * COLS);
    if (g_in.repeat(PSP_CTRL_DOWN, now)) g_pickerCursor = (g_pickerCursor + COLS) % (rows * COLS);
    if (g_pickerCursor >= count) g_pickerCursor = count - 1;
    if (g_in.hit(PSP_CTRL_LTRIGGER)) selectSlot(g_game.inv.selected - 1);
    if (g_in.hit(PSP_CTRL_RTRIGGER)) selectSlot(g_game.inv.selected + 1);
    if (g_in.hit(PSP_CTRL_CROSS)) {
        ItemStack s = g_creativeItems[g_pickerCursor];
        s.count = 1;
        g_game.inv.held() = s;
        g_itemNameTimer = 2.0;
        g_state = State::Playing;
        return;
    }
    if (g_in.hit(PSP_CTRL_SELECT) || g_in.hit(PSP_CTRL_CIRCLE)) {
        g_state = State::Playing;
        return;
    }
    backgroundFrame(dt, true);
    gfx::gradient(0, 0, gfx::SCREEN_W, gfx::SCREEN_H, gfx::rgba(16, 16, 16, 192), gfx::rgba(16, 16, 16, 208));
    const float CELL = 18;
    const int VISIBLE_ROWS = 11;
    int firstRow = g_pickerCursor / COLS - VISIBLE_ROWS / 2;
    if (firstRow > rows - VISIBLE_ROWS) firstRow = rows - VISIBLE_ROWS;
    if (firstRow < 0) firstRow = 0;
    int shownRows = rows < VISIBLE_ROWS ? rows : VISIBLE_ROWS;
    float w = COLS * CELL + 14, h = shownRows * CELL + 24;
    float x0 = (gfx::SCREEN_W - w) / 2, y0 = 4;
    gfx::rect(x0, y0, w, h, gfx::rgba(198, 198, 198));
    gfx::rect(x0, y0, w, 1, gfx::rgba(255, 255, 255));
    gfx::rect(x0, y0, 1, h, gfx::rgba(255, 255, 255));
    gfx::rect(x0, y0 + h - 1, w, 1, gfx::rgba(85, 85, 85));
    gfx::rect(x0 + w - 1, y0, 1, h, gfx::rgba(85, 85, 85));
    gfx::text(x0 + 7, y0 + 5, "Select item", gfx::rgba(64, 64, 64), false);
    for (int i = firstRow * COLS; i < count && i < (firstRow + shownRows) * COLS; ++i) {
        int r = i / COLS - firstRow;
        float cx = x0 + 7 + (i % COLS) * CELL, cy = y0 + 16 + r * CELL;
        gfx::rect(cx, cy, CELL - 1, CELL - 1, gfx::rgba(139, 139, 139));
        gfx::rect(cx, cy, CELL - 1, 1, gfx::rgba(55, 55, 55));
        gfx::rect(cx, cy, 1, CELL - 1, gfx::rgba(55, 55, 55));
        gfx::itemIcon(g_creativeItems[i], cx + 1, cy + 1);
        if (i == g_pickerCursor) gfx::rect(cx, cy, CELL - 1, CELL - 1, gfx::rgba(255, 255, 255, 128));
    }
    gfx::textCentered(gfx::SCREEN_W / 2.0f, y0 + h + 3, itemName(g_creativeItems[g_pickerCursor]), 0xFFFFFFFF);
    gfx::endFrame();
}

void deadFrame(double now, double dt) {
    bool ready = g_game.deathTime >= 20;
    g_menu = menuNavigate(g_menu, 2, now);
    if (ready && g_in.hit(PSP_CTRL_CROSS)) {
        if (g_menu == 0) {
            if (g_game.dimension != 0) {
                changeDimension(false);
                if (g_state == State::Title) return;
            }
            g_game.respawn();
            g_partial = 0;
            g_game.player.prevX = g_game.player.x;
            g_game.player.prevY = g_game.player.y;
            g_game.player.prevZ = g_game.player.z;
            g_state = State::Playing;
            if (!g_game.spawnReady(1)) {
                loadArea("Respawning", false);
                if (g_game.hasBed) g_game.placeAtSpawn();
            }
        } else {
            quitToTitle();
        }
        return;
    }
    backgroundFrame(dt, true);
    gfx::gradient(0, 0, gfx::SCREEN_W, gfx::SCREEN_H, gfx::rgba(80, 0, 0, 96), gfx::rgba(128, 48, 48, 160));
    gfx::textRotated(gfx::SCREEN_W / 2.0f, 50, "Game over!", 0xFFFFFFFF, 0.0f, 2.0f);
    char msg[64];
    std::snprintf(msg, sizeof(msg), "Player %s", g_game.deathMessage);
    gfx::textCentered(gfx::SCREEN_W / 2.0f, 78, msg, 0xFFFFFFFF);
    gfx::textCentered(gfx::SCREEN_W / 2.0f, 92, "Score: 0", gfx::rgba(255, 255, 85));
    drawButton(gfx::SCREEN_W / 2.0f - 100, gfx::SCREEN_H / 4.0f + 72, 200, "Respawn", g_menu == 0, ready);
    drawButton(gfx::SCREEN_W / 2.0f - 100, gfx::SCREEN_H / 4.0f + 96, 200, "Title menu", g_menu == 1, ready);
    gfx::endFrame();
}

#ifdef MC_AUTOTEST
// Scripted run for PPSSPPHeadless: injects input, dumps screenshots, then exits.
int g_testShot = 0;
const char* g_testShotName = nullptr;

void printHeap(const char* when) {
    struct mallinfo mi = mallinfo();
    std::printf("[%s] heap: arena %d KB, in use %d KB, free %d KB, top %d KB, kernel %d KB, chunks %d\n", when,
                mi.arena / 1024, mi.uordblks / 1024, mi.fordblks / 1024, mi.keepcost / 1024,
                (int)(sceKernelMaxFreeMemSize() / 1024), g_game.world.loadedChunks);
}

void spawnInFront(uint8_t type, float dist, float side) {
    const Player& p = g_game.player;
    float s = std::sin(p.yaw * 3.14159265f / 180.0f), c = std::cos(p.yaw * 3.14159265f / 180.0f);
    float x = p.x - s * dist - c * side, z = p.z + c * dist - s * side;
    int y = g_game.world.topSolidAt(ifloor(x), ifloor(z));
    if (Entity* e = g_game.spawn(type, x, (float)y, z)) {
        e->yaw = e->prevYaw = p.yaw + 180.0f;
        if (type == ENT_SHEEP) e->woolColor = 0;
    }
}

#ifdef MC_WATERTEST
// Loads the copied world, stands over the nearest water, then falls through the surface.
bool g_waterFound = false;
int g_waterX = 0, g_waterY = 64, g_waterZ = 0;

void findOrMakeWater() {
    const Player& p = g_game.player;
    int px = ifloor(p.x), py = ifloor(p.y), pz = ifloor(p.z);
    int bestDepth = 0, bestDist = 100000000;
    for (int y = py + 16; y >= 2 && y >= py - 48; --y)
        for (int dz = -40; dz <= 40; ++dz)
            for (int dx = -40; dx <= 40; ++dx) {
                int x = px + dx, z = pz + dz;
                if (!g_game.world.isLoaded(x, z)) continue;
                if (!blockIsWater(g_game.world.get(x, y, z))) continue;
                if (blockIsWater(g_game.world.get(x, y + 1, z))) continue;
                int depth = 0;
                while (depth < 12 && blockIsWater(g_game.world.get(x, y - depth, z))) ++depth;
                int dist = dx * dx + dz * dz;
                if (depth > bestDepth || (depth == bestDepth && dist < bestDist)) {
                    bestDepth = depth;
                    bestDist = dist;
                    g_waterX = x;
                    g_waterY = y;
                    g_waterZ = z;
                    g_waterFound = true;
                }
            }
    std::printf("deepest water %d at %d %d %d\n", bestDepth, g_waterX, g_waterY, g_waterZ);
    std::fflush(stdout);
    if (bestDepth >= 3) return;
    int y = py > 8 && py < World::SY - 6 ? py : 70;
    for (int dz = -1; dz <= 1; ++dz)
        for (int dx = -1; dx <= 1; ++dx) {
            int x = px + dx, z = pz + dz;
            if (!g_game.world.isLoaded(x, z)) continue;
            for (int h = 1; h <= 6; ++h) g_game.world.setBlock(x, y + h, z, AIR);
            for (int h = 0; h < 4; ++h) g_game.world.setBlock(x, y - h, z, WATER);
            g_game.world.setBlock(x, y - 4, z, STONE);
            g_waterX = px;
            g_waterY = y;
            g_waterZ = pz;
            g_waterFound = true;
        }
    std::printf("carved pool at %d %d %d\n", g_waterX, g_waterY, g_waterZ);
    std::fflush(stdout);
}

void standOverWater() {
    Player& p = g_game.player;
    p.setPosition(g_waterX + 0.5f, g_waterY + 5.0f, g_waterZ + 0.5f);
    p.mx = p.my = p.mz = 0;
    p.pitch = 75;
    p.yaw = 0;
}

void waterTestFrame(int f) {
    g_in.held = g_in.pressed = 0;
    g_in.ax = g_in.ay = 0;
    if (f == 3) {
        g_settings.debug = true;
        g_settings.renderDistance = 1;
        applySettings();
        bool ok = loadSlot("world");
        Player& p = g_game.player;
        std::printf("load %d player %.2f %.2f %.2f chunks %d\n", ok ? 1 : 0, p.x, p.y, p.z, g_game.world.loadedChunks);
        std::fflush(stdout);
        if (!ok) newWorld("world", "Water", 1234567, MODE_SURVIVAL);
        findOrMakeWater();
        std::printf("water %d at %d %d %d\n", g_waterFound ? 1 : 0, g_waterX, g_waterY, g_waterZ);
        std::fflush(stdout);
        if (g_waterFound) standOverWater();
    }
    // Stay above the surface while the faces get a mesh, then step the eye through it.
    if (g_waterFound && f > 3 && f < 25) standOverWater();
    if (g_waterFound && f >= 25 && f < 70) {
        float t = (f - 25) / 30.0f;
        if (t > 1) t = 1;
        float y = (g_waterY + 4.0f) * (1.0f - t) + (g_waterY - 2.5f) * t;
        Player& p = g_game.player;
        p.setPosition(g_waterX + 0.5f, y, g_waterZ + 0.5f);
        p.mx = p.mz = 0;
        p.my = -0.15f;
        p.pitch = 10;
    }
    if (f >= 4 && f <= 160 && (f % 5 == 0 || f < 30)) {
        const Player& p = g_game.player;
        bool eye = p.eyeInWater(g_game.world, p.y + Player::EYE);
        std::printf("f=%d pos %.2f %.2f %.2f inWater %d eye %d ground %d\n", f, p.x, p.y, p.z, p.inWater ? 1 : 0,
                    eye ? 1 : 0, p.onGround ? 1 : 0);
        std::fflush(stdout);
    }
    if (g_waterFound && f >= 40 && g_game.player.eyeInWater(g_game.world, g_game.player.y + Player::EYE)) {
        g_game.player.pitch = 20;
        g_in.ay = -0.6f;
    }
    if (f == 180) {
        std::printf("done\n");
        std::fflush(stdout);
        g_exitRequested = true;
    }
}
#endif

void autotestInput() {
#ifdef MC_SHOWCASE
    if (g_showcaseHold > 0) {
        --g_showcaseHold;
        g_in.held = g_holdButtons;
        g_in.pressed = 0;
        g_in.ax = g_holdAx;
        g_in.ay = g_holdAy;
        return;
    }
#endif
    int f = g_testFrame++;
#ifdef MC_WATERTEST
    waterTestFrame(f);
    return;
#endif
    g_in.held = g_in.pressed = 0;
    g_in.ax = g_in.ay = 0;
    auto press = [](unsigned b) { g_in.held |= b, g_in.pressed |= b; };
    auto hold = [](unsigned b) { g_in.held |= b; };
    if (f == 2) g_testShotName = "title";
    if (f == 3) {
        double t0 = nowSeconds();
        newWorld("world", "Autotest", MC_AUTOTEST_SEED, MODE_SURVIVAL);
        g_game.clearWeather();
        Player& p = g_game.player;
        std::printf("world ready in %.2f s, spawn %.1f %.1f %.1f\n", nowSeconds() - t0, p.x, p.y, p.z);
        printHeap("spawn");
#ifdef MC_SHOWCASE
        g_settings.debug = false;
        g_settings.renderDistance = 2;
#else
        g_settings.debug = true;
#endif
        g_game.world.time = 1000;
        // Face the direction with the most open space.
        int bestFree = -1;
        for (int a = 0; a < 8; ++a) {
            float yaw = a * 45.0f, s = std::sin(yaw / 57.29578f), c = std::cos(yaw / 57.29578f);
            int free = 0;
            while (free < 24 && g_game.world.get(ifloor(p.x - s * free), ifloor(p.y + 1.6f), ifloor(p.z + c * free)) == AIR)
                ++free;
            if (free > bestFree) bestFree = free, p.yaw = yaw;
        }
    }
    if (f == 10) g_testShotName = "spawn";
    if (f == 12) sound::updateMusic(1e6);
    if (f == 60) std::printf("music active %d\n", audio::musicActive() ? 1 : 0);
    if (f == 110) std::printf("sounds so far: %d\n", g_testSounds);
    if (f > 10 && f < 50) g_in.ay = -1.0f;
    if (f == 50) g_testShotName = "walked";
    if (f == 51) g_game.player.pitch = 60;
    if (f >= 52 && f < 100) hold(PSP_CTRL_RTRIGGER);
    if (f == 52) press(PSP_CTRL_RTRIGGER);
    if (f == 55) g_testShotName = "mining";
    if (f == 104) g_testShotName = "mined";
    if (f == 106) {
        Inventory& inv = g_game.inv;
        inv.main[1] = ItemStack(LOG, 3);
        inv.main[2] = ItemStack(I_IRON_PICKAXE, 1, 100);
        inv.main[3] = ItemStack(TORCH, 16);
        inv.main[4] = ItemStack(WORKBENCH, 1);
        inv.armor[0] = ItemStack(I_LEATHER_HELMET);
        inv.armor[1] = ItemStack(I_LEATHER_HELMET + 1);
        press(PSP_CTRL_SELECT);
    }
    if (f == 108) {
        // log into the 2x2 grid
        for (int i = 0; i < g_slotCount; ++i)
            if (g_slots[i].s == &g_game.inv.main[1]) g_cursor = i;
        press(PSP_CTRL_CROSS);
    }
    if (f == 109) {
        for (int i = 0; i < g_slotCount; ++i)
            if (g_slots[i].s == &g_grid[0]) g_cursor = i;
        press(PSP_CTRL_SQUARE);
    }
    if (f == 111) g_testShotName = "inventory";
    if (f == 112) press(PSP_CTRL_CIRCLE);
    if (f == 114) {
        openScreen(Screen::Workbench);
        g_grid[0] = g_grid[1] = g_grid[2] = ItemStack(COBBLESTONE);
        g_grid[4] = g_grid[7] = ItemStack(I_STICK);
        updateCraft();
        g_cursor = 0;
    }
    if (f == 116) g_testShotName = "workbench";
    if (f == 117) press(PSP_CTRL_CIRCLE);
    if (f == 119) {
        Player& p = g_game.player;
        p.pitch = 10;
        printHeap("before mobs");
        spawnInFront(ENT_ZOMBIE, 5, -3);
        spawnInFront(ENT_SKELETON, 5, -1.5f);
        spawnInFront(ENT_CREEPER, 5, 0);
        spawnInFront(ENT_SPIDER, 5, 2);
        spawnInFront(ENT_PIG, 8, -3);
        spawnInFront(ENT_COW, 8, -0.5f);
        spawnInFront(ENT_SHEEP, 8, 2);
        spawnInFront(ENT_CHICKEN, 6, 3.5f);
        g_game.dropItem(ItemStack(COBBLESTONE, 3), p.x - std::sin(p.yaw / 57.3f) * 2, p.y + 0.5f,
                        p.z + std::cos(p.yaw / 57.3f) * 2, false);
    }
    if (f == 121) g_testShotName = "mobs";
    if (f == 124) g_testShotName = "mobs2";
    if (f == 125) {
        g_game.world.time = 13000;
    }
    if (f == 128) g_testShotName = "sunset";
    if (f == 129) {
        g_game.world.time = 18000;
        g_game.player.pitch = -50;
    }
    if (f == 140) g_testShotName = "night_sky";
    if (f == 141) {
        g_game.world.time = 6000;
        g_game.player.pitch = 20;
        g_game.player.flying = true;
        g_game.player.setPosition(g_game.player.x, 100, g_game.player.z);
    }
    if (f > 141 && f < 300) {
        // Fly east quickly so new chunks stream in.
        g_game.player.yaw = -90;
        g_game.player.setPosition(g_game.player.x + 0.8f, 100, g_game.player.z);
    }
    if (f == 200) printHeap("flying");
    if (f == 299) {
        g_testShotName = "flown";
        int spawners = 0, chests = 0;
        for (int cx = -40; cx <= 200; ++cx)
            for (int cz = -40; cz <= 40; ++cz)
                if (const Chunk* c = g_game.world.chunk(cx, cz))
                    for (const TileEntity* t = c->tiles; t; t = t->next) {
                        spawners += t->type == TILE_SPAWNER;
                        chests += t->type == TILE_CHEST;
                    }
        std::printf("generated dungeons: %d spawners, %d chests\n", spawners, chests);
    }
    if (f == 300) {
        printHeap("flown");
        g_game.player.flying = false;
        g_game.hurtPlayer(100, g_game.player.x, g_game.player.z, true);
        g_game.deathMessage = "was slain by a test";
    }
    if (f == 325) g_testShotName = "dead";
    if (f == 326) press(PSP_CTRL_CROSS);
    if (f == 335) g_testShotName = "respawned";
    if (f == 336) press(PSP_CTRL_START);
    if (f == 338) g_testShotName = "paused";
    if (f == 339) press(PSP_CTRL_START);
    if (f == 341) {
        const Player& p = g_game.player;
        int x = ifloor(p.x) + 2, y = ifloor(p.y), z = ifloor(p.z);
        g_game.world.setBlock(x, y, z, FURNACE, 2);
        TileEntity* t = g_game.world.createTile(x, y, z, TILE_FURNACE);
        t->items[0] = ItemStack(IRON_ORE, 5);
        t->items[1] = ItemStack(I_COAL, 2);
        g_game.openTile = t;
        openScreen(Screen::Furnace);
    }
    if (f == 400) {
        g_testShotName = "furnace";
        TileEntity* t = g_tile;
        std::printf("furnace: in %d out %d burn %d cook %d\n", t ? t->items[0].count : -1,
                    t ? t->items[2].count : -1, t ? t->burnTime : -1, t ? t->cookTime : -1);
    }
    if (f == 401) press(PSP_CTRL_CIRCLE);
    if (f == 403) {
        const Player& p = g_game.player;
        int x = ifloor(p.x) - 2, y = ifloor(p.y), z = ifloor(p.z);
        g_game.world.setBlock(x, y, z, CHEST);
        TileEntity* t = g_game.world.createTile(x, y, z, TILE_CHEST);
        t->items[0] = ItemStack(DIAMOND_BLOCK, 64);
        t->items[13] = ItemStack(I_BREAD, 5);
        g_game.openTile = t;
        openScreen(Screen::Chest);
        g_cursor = 0;
    }
    if (f == 404) press(PSP_CTRL_TRIANGLE);
    if (f == 406) g_testShotName = "chest";
    if (f == 407) press(PSP_CTRL_CIRCLE);
    static const int HEAD[4][2] = {{0, 1}, {-1, 0}, {0, -1}, {1, 0}};
    if (f == 409) {
        Player& p = g_game.player;
        g_game.world.time = 6000;
        int q = ifloor(p.yaw * 4.0f / 360.0f + 0.5f) & 3;
        p.yaw = q * 90.0f;
        int dx = HEAD[q][0], dz = HEAD[q][1], sx = -dz, sz = dx;
        int x0 = ifloor(p.x), y = ifloor(p.y), z0 = ifloor(p.z);
        for (int a = 1; a <= 6; ++a)
            for (int b = -5; b <= 5; ++b) {
                int x = x0 + dx * a + sx * b, z = z0 + dz * a + sz * b;
                g_game.world.setBlock(x, y - 1, z, STONE);
                g_game.world.setBlock(x, y, z, AIR);
                g_game.world.setBlock(x, y + 1, z, AIR);
            }
        for (int k = 0; k < 4; ++k) {
            int bx = x0 + dx * 4 + sx * (k * 2 - 3), bz = z0 + dz * 4 + sz * (k * 2 - 3);
            g_game.world.setBlock(bx, y, bz, BED, k);
            g_game.world.setBlock(bx + HEAD[k][0], y, bz + HEAD[k][1], BED, k + 8);
        }
        g_game.world.setBlock(x0 + dx * 2, y, z0 + dz * 2, BED, q);
        g_game.world.setBlock(x0 + dx * 3, y, z0 + dz * 3, BED, q + 8);
        g_game.inv.main[g_game.inv.selected] = ItemStack(I_BED);
        p.pitch = 30;
    }
    if (f == 410) reportTicks("until beds");
    if (f == 412) g_testShotName = "beds";
    if (f == 413) g_game.world.time = 15000;
    if (f >= 405 && f <= 440)
        for (Entity& e : g_game.ents)
            if (entityIsHostile(e.type)) e.type = ENT_NONE;
    if (f == 416) press(PSP_CTRL_LTRIGGER);
    if (f == 440) {
        g_testShotName = "sleeping";
        std::printf("sleeping=%d timer=%d\n", g_game.sleeping ? 1 : 0, g_game.sleepTimer);
    }
    static int wokeAt = 0;
    if (f > 440 && !wokeAt && !g_game.sleeping) {
        wokeAt = f;
        g_game.mode = MODE_CREATIVE;
        std::printf("woke at frame %d, time of day %lld, hasBed %d\n", f, (long long)(g_game.world.time % 24000),
                    g_game.hasBed ? 1 : 0);
    }
    if (wokeAt && f == wokeAt + 20) g_testShotName = "woke";
    if (wokeAt && f == wokeAt + 22) {
        Player& p = g_game.player;
        g_game.world.time = 6000;
        int q = ifloor(p.yaw * 4.0f / 360.0f + 0.5f) & 3;
        p.yaw = q * 90.0f;
        p.pitch = 35;
        int dx = HEAD[q][0], dz = HEAD[q][1], sx = -dz, sz = dx;
        int x0 = ifloor(p.x), y = ifloor(p.y), z0 = ifloor(p.z);
        for (int a = 1; a <= 14; ++a)
            for (int b = -8; b <= 8; ++b) {
                int x = x0 + dx * a + sx * b, z = z0 + dz * a + sz * b;
                g_game.world.setBlock(x, y - 1, z, STONE);
                for (int h = 0; h < 4; ++h) g_game.world.setBlock(x, y + h, z, AIR);
            }
        for (int a = 9; a <= 11; ++a)
            for (int b = 3; b <= 5; ++b) g_game.world.setBlock(x0 + dx * a + sx * b, y, z0 + dz * a + sz * b, PLANKS);
        g_game.setBlockNotify(x0 + dx * 6 + sx * -4, y + 1, z0 + dz * 6 + sz * -4, WATER_FLOWING);
        g_game.setBlockNotify(x0 + dx * 10 + sx * -1, y, z0 + dz * 10 + sz * -1, LAVA_FLOWING);
        g_game.setBlockNotify(x0 + dx * 10 + sx * 4, y + 1, z0 + dz * 10 + sz * 4, FIRE);
    }
    if (wokeAt && f == wokeAt + 2) reportTicks("before fluids");
    if (wokeAt && f == wokeAt + 24) reportTicks("fluid setup");
    if (wokeAt && f == wokeAt + 60) g_testShotName = "fluids";
    if (wokeAt && f == wokeAt + 100) g_game.inv.held() = ItemStack(I_DIAMOND_SWORD);
    if (wokeAt && f == wokeAt + 120) g_testShotName = "sword";
    if (wokeAt && f == wokeAt + 122) g_game.attackPressed = true;
    if (wokeAt && f == wokeAt + 128) g_testShotName = "sword_swing";
    if (wokeAt && f == wokeAt + 140) {
        g_testShotName = "fluids_later";
        std::printf("scheduled ticks %d\n", g_game.world.scheduledCount());
        reportTicks("fluids");
    }
    if (wokeAt && f == wokeAt + 142) {
        // Dungeon mobs: a spawner, slimes, wolves and a squid pool on a fresh stone floor.
        Player& p = g_game.player;
        g_game.world.time = 6000;
        g_game.inv.held() = ItemStack(I_BONE);
        int q = ifloor(p.yaw * 4.0f / 360.0f + 0.5f) & 3;
        p.yaw = q * 90.0f;
        p.pitch = 25;
        int dx = HEAD[q][0], dz = HEAD[q][1], sx = -dz, sz = dx;
        int x0 = ifloor(p.x), y = ifloor(p.y), z0 = ifloor(p.z);
        auto at = [&](int a, int b, int h, uint8_t id) {
            g_game.world.setBlock(x0 + dx * a + sx * b, y + h, z0 + dz * a + sz * b, id);
        };
        for (int a = 1; a <= 14; ++a)
            for (int b = -8; b <= 8; ++b) {
                at(a, b, -1, STONE);
                for (int h = 0; h < 4; ++h) at(a, b, h, AIR);
            }
        for (int a = 7; a <= 9; ++a)
            for (int b = 2; b <= 5; ++b) {
                at(a, b, -3, STONE);
                at(a, b, -2, WATER);
                at(a, b, -1, WATER);
            }
        at(4, -3, 0, MOB_SPAWNER);
        if (TileEntity* t = g_game.world.createTile(x0 + dx * 4 + sx * -3, y, z0 + dz * 4 + sz * -3, TILE_SPAWNER)) {
            t->burnTime = 20;
            t->burnTotal = ENT_ZOMBIE;
        }
        for (int b = -6; b <= -4; ++b) at(6, b, 0, b == -5 ? MOSSY_COBBLESTONE : COBBLESTONE);
        auto put = [&](uint8_t type, float a, float b, float h) -> Entity* {
            Entity* e = g_game.spawn(type, x0 + 0.5f + dx * a + sx * b, y + h, z0 + 0.5f + dz * a + sz * b);
            if (e) e->yaw = e->prevYaw = p.yaw + 180.0f;
            return e;
        };
        if (Entity* s = put(ENT_SLIME, 6, -1, 0)) {
            s->size = 4, s->width = s->height = 2.4f, s->health = 16;
            s->setPosition(s->x, s->y, s->z);
        }
        if (Entity* s = put(ENT_SLIME, 4, 0.5f, 0)) {
            s->size = 2, s->width = s->height = 1.2f, s->health = 4;
            s->setPosition(s->x, s->y, s->z);
        }
        put(ENT_WOLF, 3, 2.5f, 0);
        if (Entity* w = put(ENT_WOLF, 5, 3.5f, 0)) w->tamed = w->sitting = true, w->health = 20;
        if (Entity* w = put(ENT_WOLF, 9, -2, 0)) w->angry = true;
        put(ENT_SQUID, 8, 3.5f, -2);
        g_game.mode = MODE_CREATIVE;
    }
    if (wokeAt && f == wokeAt + 160) {
        g_testShotName = "dungeon_mobs";
        int counts[ENT_TYPE_COUNT] = {};
        for (const Entity& e : g_game.ents) ++counts[e.type];
        std::printf("mobs: slime %d squid %d wolf %d zombie %d\n", counts[ENT_SLIME], counts[ENT_SQUID],
                    counts[ENT_WOLF], counts[ENT_ZOMBIE]);
    }
    if (wokeAt && f == wokeAt + 162) {
        for (const Entity& e : g_game.ents)
            if (e.type == ENT_SQUID) {
                Player& p = g_game.player;
                p.flying = true;
                p.setPosition(e.x - std::sin(p.yaw / 57.29578f) * -2.5f, e.y + 2.2f,
                              e.z + std::cos(p.yaw / 57.29578f) * -2.5f);
                p.pitch = 40;
                std::printf("squid at %.1f %.1f %.1f inWater %d\n", e.x, e.y, e.z, e.inWater ? 1 : 0);
            }
    }
    if (wokeAt && f == wokeAt + 166) g_testShotName = "squid";
    if (wokeAt && f == wokeAt + 175) {
        g_game.world.time = 18000;
        reportTicks("dungeon mobs");
    }
    if (wokeAt && f == wokeAt + 240) {
        g_testShotName = "spawner_night";
        int counts[ENT_TYPE_COUNT] = {};
        for (const Entity& e : g_game.ents) ++counts[e.type];
        std::printf("night mobs: zombie %d slime %d wolf %d squid %d\n", counts[ENT_ZOMBIE], counts[ENT_SLIME],
                    counts[ENT_WOLF], counts[ENT_SQUID]);
    }
    if (wokeAt && f == wokeAt + 242) {
        // Small items: signs, trapdoors, cake, cobweb, paintings, jukebox, and the dials in the hotbar.
        Player& p = g_game.player;
        for (Entity& e : g_game.ents)
            if (e.type != ENT_NONE) e.type = ENT_NONE;
        g_game.world.time = 6000;
        p.flying = false;
        int q = ifloor(p.yaw * 4.0f / 360.0f + 0.5f) & 3;
        p.yaw = q * 90.0f;
        p.pitch = 15;
        int dx = HEAD[q][0], dz = HEAD[q][1], sx = -dz, sz = dx;
        int x0 = ifloor(p.x), y = 100, z0 = ifloor(p.z);
        auto X = [&](int a, int b) { return x0 + dx * a + sx * b; };
        auto Z = [&](int a, int b) { return z0 + dz * a + sz * b; };
        auto at = [&](int a, int b, int h, uint8_t id, int meta = 0) { g_game.world.setBlock(X(a, b), y + h, Z(a, b), id, meta); };
        for (int a = -2; a <= 10; ++a)
            for (int b = -8; b <= 8; ++b) {
                at(a, b, -1, STONE);
                for (int h = 0; h < 6; ++h) at(a, b, h, a == 7 ? PLANKS : AIR);
            }
        p.setPosition(x0 + 0.5f, (float)y, z0 + 0.5f);
        p.prevX = p.x, p.prevY = p.y, p.prevZ = p.z;
        // Paintings on the plank wall, via the normal placement path.
        g_game.mode = MODE_CREATIVE;
        static const int FACE_TOWARD[4] = {2, 5, 3, 4};  // face of the wall pointing back at the player
        for (int b = -4; b <= 4; b += 4) {
            g_game.inv.held() = ItemStack(I_PAINTING);
            g_game.hasBlockTarget = true;
            g_game.entityTarget = nullptr;
            g_game.blockTarget = {X(7, b), y + 2, Z(7, b), FACE_TOWARD[q], 0};
            g_game.usePressed = true;
            g_game.tick();
        }
        // Standing sign facing the player, wall sign on the plank wall.
        at(3, -2, 0, SIGN_POST, ((q * 4 + 8) & 15));
        if (TileEntity* t = g_game.world.createTile(X(3, -2), y, Z(3, -2), TILE_SIGN)) {
            std::strcpy(t->text[0], "Welcome to");
            std::strcpy(t->text[1], "Minecraft");
            std::strcpy(t->text[2], "Beta 1.7.3");
            std::strcpy(t->text[3], "on PSP!");
        }
        static const int WALL_META[4] = {2, 5, 3, 4};
        at(6, 2, 1, WALL_SIGN, WALL_META[q]);
        if (TileEntity* t = g_game.world.createTile(X(6, 2), y + 1, Z(6, 2), TILE_SIGN)) std::strcpy(t->text[1], "<- Paintings");
        at(3, 1, 0, CAKE, 2);
        at(3, 3, 0, JUKEBOX);
        at(4, -5, 0, WEB);
        at(4, -6, 0, WEB);
        at(2, 4, 0, COBBLESTONE);
        static const int TRAP_META[4] = {3, 1, 2, 0}, TRAP_OPP[4] = {2, 0, 3, 1};
        at(2, 3, 0, TRAPDOOR, TRAP_META[q]);
        at(2, 5, 0, TRAPDOOR, TRAP_OPP[q] | 4);
        g_game.world.markAllDirty();
        g_game.inv.main[0] = ItemStack(I_COMPASS);
        g_game.inv.main[1] = ItemStack(I_CLOCK);
        g_game.inv.main[2] = ItemStack(I_SIGN);
        g_game.inv.main[3] = ItemStack(I_CAKE);
        g_game.inv.main[4] = ItemStack(TRAPDOOR);
        g_game.inv.main[5] = ItemStack(I_PAINTING);
        g_game.inv.main[6] = ItemStack(JUKEBOX);
        g_game.inv.main[7] = ItemStack(I_SNOWBALL, 16);
        g_game.inv.main[8] = ItemStack(I_FISHING_ROD);
        g_game.inv.selected = 8;
        g_game.hasBlockTarget = false;
        int paintings = 0;
        for (const Entity& e : g_game.ents) paintings += e.type == ENT_PAINTING;
        std::printf("small items: %d paintings placed\n", paintings);
    }
    if (wokeAt && f == wokeAt + 246) {
        g_game.hasBlockTarget = false;
        g_game.player.pitch = -5;
        g_game.usePressed = true;  // cast the rod
    }
    if (wokeAt && f == wokeAt + 262) {
        g_testShotName = "small_items";
        std::printf("fish hook %d, record serial %d\n", g_game.fishHook, g_game.record.serial);
    }
    if (wokeAt && f == wokeAt + 263) {
        g_game.inv.selected = 7;
        g_game.hasBlockTarget = false;
        g_game.usePressed = true;
    }
    if (wokeAt && f == wokeAt + 265) g_testShotName = "snowball";
    if (wokeAt && f == wokeAt + 266) {
        // Open the sign editor on a fresh sign and type a line.
        g_game.inv.selected = 2;
        g_game.mode = MODE_SURVIVAL;
        const Player& p = g_game.player;
        int q = ifloor(p.yaw * 4.0f / 360.0f + 0.5f) & 3;
        int bx = ifloor(p.x) + HEAD[q][0] * 2, bz = ifloor(p.z) + HEAD[q][1] * 2;
        g_game.hasBlockTarget = true;
        g_game.entityTarget = nullptr;
        g_game.blockTarget = {bx, 99, bz, 1, 0};
        g_game.usePressed = true;
    }
    if (wokeAt && f >= wokeAt + 268 && f < wokeAt + 274 && g_state == State::Sign) {
        g_kbY = (f - wokeAt) % 2 ? 0 : 2;
        g_kbX = (f * 5) % 13;
        press(PSP_CTRL_CROSS);
    }
    if (wokeAt && f == wokeAt + 276) {
        g_testShotName = "sign_editor";
        std::printf("sign editor open %d, line '%s'\n", g_state == State::Sign ? 1 : 0,
                    g_signTile ? g_signTile->text[0] : "-");
    }
    if (wokeAt && f == wokeAt + 277) press(PSP_CTRL_CIRCLE);
    if (wokeAt && f == wokeAt + 279) {
        // Records in a jukebox.
        const Player& p = g_game.player;
        int q = ifloor(p.yaw * 4.0f / 360.0f + 0.5f) & 3;
        int sx = -HEAD[q][1], sz = HEAD[q][0];
        int jx = ifloor(p.x) + HEAD[q][0] * 3 + sx * 3, jz = ifloor(p.z) + HEAD[q][1] * 3 + sz * 3;
        g_game.inv.held() = ItemStack(I_RECORD_13);
        g_game.hasBlockTarget = true;
        g_game.entityTarget = nullptr;
        g_game.blockTarget = {jx, 100, jz, 1, 0};
        g_game.usePressed = true;
    }
    if (wokeAt && f == wokeAt + 300) {
        g_testShotName = "jukebox";
        std::printf("record %d serial %d msg %s\n", g_game.record.record, g_game.record.serial,
                    g_message ? g_message : "-");
        reportTicks("small items");
    }
    static int redLever[3], redPlanks[3], redWire[3];
    if (wokeAt && f == wokeAt + 302) {
        // Redstone: a powered wire line, torches, repeaters, plates, a button, pistons and a dispenser.
        Player& p = g_game.player;
        for (Entity& e : g_game.ents) e.type = ENT_NONE;
        g_game.fishHook = -1;
        g_game.world.time = 6000;
        g_game.mode = MODE_CREATIVE;
        int q = ifloor(p.yaw * 4.0f / 360.0f + 0.5f) & 3;
        p.yaw = q * 90.0f;
        p.pitch = 14;
        g_message = nullptr;
        int dx = HEAD[q][0], dz = HEAD[q][1], sx = -dz, sz = dx;
        int x0 = ifloor(p.x), y = 100, z0 = ifloor(p.z);
        auto X = [&](int a, int b) { return x0 + dx * a + sx * b; };
        auto Z = [&](int a, int b) { return z0 + dz * a + sz * b; };
        auto raw = [&](int a, int b, int h, uint8_t id, int meta = 0) { g_game.world.setBlock(X(a, b), y + h, Z(a, b), id, meta); };
        auto put = [&](int a, int b, int h, uint8_t id, int meta = 0) { g_game.setBlockNotify(X(a, b), y + h, Z(a, b), id, meta); };
        for (int a = -6; a <= 10; ++a)
            for (int b = -9; b <= 9; ++b) {
                raw(a, b, -1, STONE);
                for (int h = 0; h < 6; ++h) raw(a, b, h, AIR);
            }
        p.setPosition(X(-3, 0) + 0.5f, (float)y, Z(-3, 0) + 0.5f);
        p.prevX = p.x, p.prevY = p.y, p.prevZ = p.z;
        // Face of a block pointing back at the player, and the side meta of things attached to the block ahead.
        int toward = dx ? (dx > 0 ? 4 : 5) : (dz > 0 ? 2 : 3);
        int attachAhead = dx ? (dx > 0 ? 2 : 1) : (dz > 0 ? 4 : 3);
        for (int b = -5; b <= 0; ++b) put(3, b, 0, REDSTONE_WIRE);
        put(4, -2, 0, REDSTONE_WIRE);
        put(5, -2, 0, REDSTONE_WIRE);
        put(3, 1, 0, NOTE_BLOCK);
        put(3, -6, 0, LEVER, 5 | 8);
        put(5, -5, 0, STONE);
        put(5, -5, 1, REDSTONE_TORCH_ON, 5);
        put(6, -1, 0, REPEATER_OFF, (q + 2) % 4 | 4);
        put(6, -3, 0, REPEATER_OFF, q | 8);
        put(2, 3, 0, STONE_PLATE);
        put(2, 4, 0, WOOD_PLATE);
        put(4, 6, 0, STONE);
        put(3, 6, 0, STONE_BUTTON, attachAhead);
        put(4, 3, 0, DISPENSER, toward);
        if (!g_game.world.tileAt(X(4, 3), y, Z(4, 3))) {
            if (TileEntity* t = g_game.world.createTile(X(4, 3), y, Z(4, 3), TILE_DISPENSER)) {
                t->items[0] = ItemStack(I_ARROW, 16);
                t->items[4] = ItemStack(I_SNOWBALL, 8);
            }
        }
        put(7, 1, 0, PISTON, 1);
        put(7, 0, 0, LEVER, 5 | 8);
        put(7, 3, 1, PLANKS);
        put(7, 3, 0, STICKY_PISTON, 1);
        put(7, 4, 0, LEVER, 5 | 8);
        put(8, -1, 0, PISTON, toward);
        put(8, -3, 0, STICKY_PISTON, toward);
        redLever[0] = X(7, 4), redLever[1] = y, redLever[2] = Z(7, 4);
        redWire[0] = X(3, 0), redWire[1] = y, redWire[2] = Z(3, 0);
        redPlanks[0] = X(7, 3), redPlanks[1] = y + 2, redPlanks[2] = Z(7, 3);
        Inventory& inv = g_game.inv;
        const uint16_t hotbar[9] = {I_REDSTONE, REDSTONE_TORCH_ON, I_REPEATER, LEVER, STONE_BUTTON,
                                    STONE_PLATE, PISTON, STICKY_PISTON, DISPENSER};
        for (int i = 0; i < 9; ++i) inv.main[i] = ItemStack(hotbar[i], 1);
        inv.selected = 1;
        g_game.hasBlockTarget = false;
        g_game.world.markAllDirty();
    }
    if (wokeAt && f == wokeAt + 330) {
        g_testShotName = "redstone";
        std::printf("redstone: wire end %d, sticky pushed planks %d\n", g_game.world.getMeta(redWire[0], redWire[1], redWire[2]),
                    g_game.world.get(redPlanks[0], redPlanks[1], redPlanks[2]) == PLANKS ? 1 : 0);
        reportTicks("redstone");
    }
    if (wokeAt && f == wokeAt + 331) g_game.setBlockNotify(redLever[0], redLever[1], redLever[2], LEVER, 5);
    if (wokeAt && f == wokeAt + 332) g_testShotName = "piston_moving";
    if (wokeAt && f == wokeAt + 345) {
        g_testShotName = "piston_pulled";
        std::printf("sticky pulled planks back %d\n",
                    g_game.world.get(redPlanks[0], redPlanks[1] - 1, redPlanks[2]) == PLANKS ? 1 : 0);
    }
    if (wokeAt && f == wokeAt + 347) {
        const Player& p = g_game.player;
        int q = ifloor(p.yaw * 4.0f / 360.0f + 0.5f) & 3;
        int dx = HEAD[q][0], dz = HEAD[q][1], sx = -dz, sz = dx;
        int x = ifloor(p.x) + dx * 7 + sx * 3, z = ifloor(p.z) + dz * 7 + sz * 3;
        g_game.openTile = g_game.world.tileAt(x, 100, z);
        if (g_game.openTile) openScreen(Screen::Dispenser);
        std::printf("dispenser gui %d\n", g_game.openTile ? 1 : 0);
    }
    if (wokeAt && f == wokeAt + 349) g_testShotName = "dispenser";
    if (wokeAt && f == wokeAt + 350) press(PSP_CTRL_CIRCLE);
    static int cartIdx = -1, chestCartIdx = -1;
    if (wokeAt && f == wokeAt + 353) {
        // Vehicles: a rail line with a slope and a curve, three kinds of minecart, a boat and a saddled pig.
        Player& p = g_game.player;
        for (Entity& e : g_game.ents) e = Entity();
        g_game.mode = MODE_CREATIVE;
        int q = ifloor(p.yaw * 4.0f / 360.0f + 0.5f) & 3;
        p.yaw = q * 90.0f;
        p.pitch = 20;
        int dx = HEAD[q][0], dz = HEAD[q][1], sx = -dz, sz = dx;
        int x0 = ifloor(p.x), y = 100, z0 = ifloor(p.z);
        auto X = [&](int a, int b) { return x0 + dx * a + sx * b; };
        auto Z = [&](int a, int b) { return z0 + dz * a + sz * b; };
        auto raw = [&](int a, int b, int h, uint8_t id, int meta = 0) { g_game.world.setBlock(X(a, b), y + h, Z(a, b), id, meta); };
        auto put = [&](int a, int b, int h, uint8_t id, int meta = 0) { g_game.setBlockNotify(X(a, b), y + h, Z(a, b), id, meta); };
        for (int a = -6; a <= 12; ++a)
            for (int b = -9; b <= 9; ++b) {
                raw(a, b, -2, STONE);
                raw(a, b, -1, STONE);
                for (int h = 0; h < 6; ++h) raw(a, b, h, AIR);
            }
        for (int a = 2; a <= 7; ++a)
            for (int b = 2; b <= 4; ++b) raw(a, b, -1, WATER);
        p.setPosition(X(-3, 0) + 0.5f, (float)y, Z(-3, 0) + 0.5f);
        p.prevX = p.x, p.prevY = p.y, p.prevZ = p.z;
        put(10, -2, 0, STONE);
        put(10, -2, 1, RAIL);
        put(10, -1, 1, RAIL);
        for (int a = 0; a <= 9; ++a) put(a, -2, 0, a == 3 ? POWERED_RAIL : a == 6 ? DETECTOR_RAIL : RAIL);
        put(3, -3, 0, REDSTONE_TORCH_ON, 5);
        auto spawnAt = [&](uint8_t type, int a, int b, float h) {
            Entity* e = g_game.spawn(type, X(a, b) + 0.5f, y + h, Z(a, b) + 0.5f);
            if (e) e->yaw = e->prevYaw = (float)(q * 90 + 90);
            return e;
        };
        if (Entity* c = spawnAt(ENT_MINECART, 1, -2, 0)) cartIdx = (int)(c - g_game.ents);
        if (Entity* c = spawnAt(ENT_MINECART, 5, -2, 0)) {
            c->cartType = 1;
            c->cartItems[0] = ItemStack(I_DIAMOND, 12);
            c->cartItems[4] = ItemStack(RAIL, 32);
            c->cartItems[13] = ItemStack(I_SADDLE, 1);
            chestCartIdx = (int)(c - g_game.ents);
        }
        if (Entity* c = spawnAt(ENT_MINECART, 8, -2, 0)) c->cartType = 2;
        spawnAt(ENT_BOAT, 4, 3, -0.3f);
        if (Entity* pig = spawnAt(ENT_PIG, 0, 3, 0)) pig->saddled = true;
        Inventory& inv = g_game.inv;
        const uint16_t hotbar[9] = {RAIL, POWERED_RAIL, DETECTOR_RAIL, I_MINECART, I_CHEST_MINECART,
                                    I_FURNACE_MINECART, I_BOAT, I_SADDLE, I_COAL};
        for (int i = 0; i < 9; ++i) inv.main[i] = ItemStack(hotbar[i], 1);
        inv.selected = 3;
        g_game.hasBlockTarget = false;
        g_game.world.markAllDirty();
    }
    if (wokeAt && f == wokeAt + 375) {
        g_testShotName = "vehicles";
        int metas[12];
        const Player& p = g_game.player;
        int q = ifloor(p.yaw * 4.0f / 360.0f + 0.5f) & 3;
        int dx = HEAD[q][0], dz = HEAD[q][1], sx = -dz, sz = dx;
        for (int a = 0; a < 12; ++a)
            metas[a] = g_game.world.getMeta(ifloor(p.x) + 3 * dx + dx * a - 2 * sx, 100, ifloor(p.z) + 3 * dz + dz * a - 2 * sz);
        std::printf("rail metas: %d %d %d %d %d %d %d %d %d %d\n", metas[0], metas[1], metas[2], metas[3], metas[4],
                    metas[5], metas[6], metas[7], metas[8], metas[9]);
        reportTicks("vehicles");
    }
    if (wokeAt && f == wokeAt + 376 && cartIdx >= 0) {
        Entity& c = g_game.ents[cartIdx];
        g_game.mount(cartIdx);
        const Player& p = g_game.player;
        int q = ifloor(p.yaw * 4.0f / 360.0f + 0.5f) & 3;
        c.mx = HEAD[q][0] * 0.3f, c.mz = HEAD[q][1] * 0.3f;
    }
    if (wokeAt && f == wokeAt + 384) {
        g_testShotName = "riding_cart";
        const Entity& c = g_game.ents[cartIdx];
        std::printf("riding %d cart at %.2f %.2f %.2f speed %.3f\n", g_game.riding, c.x, c.y, c.z,
                    std::sqrt(c.mx * c.mx + c.mz * c.mz));
    }
    if (wokeAt && f == wokeAt + 386 && chestCartIdx >= 0) {
        if (g_game.riding >= 0) g_game.mount(g_game.riding);
        const Entity& c = g_game.ents[chestCartIdx];
        g_game.player.setPosition(c.x - 1.5f, 100, c.z);
        g_game.openCart = chestCartIdx;
        openScreen(Screen::Chest);
    }
    if (wokeAt && f == wokeAt + 388) g_testShotName = "cart_chest";
    if (wokeAt && f == wokeAt + 389) press(PSP_CTRL_CIRCLE);
    // Nether: light an obsidian frame, step through, look at the mobs and come back.
    static int portalX, portalZ, portalDx, portalDz;
    if (wokeAt && f == wokeAt + 392) {
        Player& p = g_game.player;
        if (g_game.riding >= 0) g_game.mount(g_game.riding);
        int q = ifloor(p.yaw * 4.0f / 360.0f + 0.5f) & 3;
        int dx = HEAD[q][0], dz = HEAD[q][1], sx = -dz, sz = dx;
        int x0 = ifloor(p.x) + dx * 4, z0 = ifloor(p.z) + dz * 4, y = 100;
        portalX = x0, portalZ = z0, portalDx = sx, portalDz = sz;
        for (int a = -2; a <= 3; ++a)
            for (int h = 0; h <= 4; ++h) g_game.world.setBlock(x0 + sx * a, y + h, z0 + sz * a, AIR);
        for (int a = 0; a <= 1; ++a) {
            g_game.world.setBlock(x0 + sx * a, y - 1, z0 + sz * a, OBSIDIAN);
            g_game.world.setBlock(x0 + sx * a, y + 3, z0 + sz * a, OBSIDIAN);
        }
        for (int h = 0; h <= 2; ++h) {
            g_game.world.setBlock(x0 - sx, y + h, z0 - sz, OBSIDIAN);
            g_game.world.setBlock(x0 + sx * 2, y + h, z0 + sz * 2, OBSIDIAN);
        }
        g_game.setBlockNotify(x0, y, z0, FIRE);
        p.pitch = -10;
        g_game.world.markAllDirty();
    }
    if (wokeAt && f == wokeAt + 397) {
        g_testShotName = "portal";
        std::printf("portal block: %d\n", g_game.world.get(portalX, 100, portalZ));
    }
    if (wokeAt && f == wokeAt + 398) {
        g_game.player.setPosition(portalX + 0.5f + portalDx * 0.5f, 100, portalZ + 0.5f + portalDz * 0.5f);
        g_game.portalTime = g_game.prevPortalTime = 0.7f;
    }
    if (wokeAt && f == wokeAt + 401) g_testShotName = "portal_overlay";
    if (wokeAt && f == wokeAt + 402) g_game.portalPending = true;
    if (wokeAt && f == wokeAt + 404) {
        Player& p = g_game.player;
        std::printf("dimension %d at %.1f %.1f %.1f, portal under feet %d\n", g_game.dimension, p.x, p.y, p.z,
                    g_game.world.get(ifloor(p.x), ifloor(p.y), ifloor(p.z)) == PORTAL ? 1 : 0);
        int q = ifloor(p.yaw * 4.0f / 360.0f + 0.5f) & 3;
        int dx = HEAD[q][0], dz = HEAD[q][1], sx = -dz, sz = dx;
        int x0 = ifloor(p.x), y = ifloor(p.y), z0 = ifloor(p.z);
        for (int a = 2; a <= 16; ++a)
            for (int b = -7; b <= 7; ++b) {
                g_game.world.setBlock(x0 + dx * a + sx * b, y - 1, z0 + dz * a + sz * b, NETHERRACK);
                for (int h = 0; h <= 9; ++h) g_game.world.setBlock(x0 + dx * a + sx * b, y + h, z0 + dz * a + sz * b, AIR);
            }
        g_game.world.setBlock(x0 + dx * 5 - sx * 3, y, z0 + dz * 5 - sz * 3, SOUL_SAND);
        g_game.world.setBlock(x0 + dx * 5 - sx * 4, y, z0 + dz * 5 - sz * 4, GLOWSTONE);
        g_game.world.setBlock(x0 + dx * 5 + sx * 3, y, z0 + dz * 5 + sz * 3, NETHERRACK);
        g_game.setBlockNotify(x0 + dx * 5 + sx * 3, y + 1, z0 + dz * 5 + sz * 3, FIRE);
        p.pitch = -5;
        g_game.world.markAllDirty();
    }
    if (wokeAt && f == wokeAt + 426) {
        Player& p = g_game.player;
        int q = ifloor(p.yaw * 4.0f / 360.0f + 0.5f) & 3;
        int dx = HEAD[q][0], dz = HEAD[q][1], sx = -dz, sz = dx;
        int x0 = ifloor(p.x), y = ifloor(p.y), z0 = ifloor(p.z);
        auto spawnAt = [&](uint8_t type, int a, int b, float h) {
            Entity* e = g_game.spawn(type, x0 + dx * a + sx * b + 0.5f, y + h, z0 + dz * a + sz * b + 0.5f);
            if (e) e->yaw = e->prevYaw = p.yaw + 180.0f;
            return e;
        };
        spawnAt(ENT_PIGZOMBIE, 4, -1, 0);
        spawnAt(ENT_GHAST, 12, 2, 2);
        if (Entity* b = spawnAt(ENT_FIREBALL, 7, 0, 2)) b->target = -1;
    }
    if (wokeAt && f == wokeAt + 430) {
        g_testShotName = "nether";
        reportTicks("nether");
    }
    if (wokeAt && f == wokeAt + 431) g_game.portalPending = true;
    if (wokeAt && f == wokeAt + 435) {
        g_testShotName = "back_from_nether";
        const Player& p = g_game.player;
        std::printf("dimension %d at %.1f %.1f %.1f\n", g_game.dimension, p.x, p.y, p.z);
    }
    if (wokeAt && f == wokeAt + 440) {
        reportTicks("before weather");
        g_game.world.time = 6000;
        g_game.raining = g_game.thundering = true;
        g_game.rainTime = g_game.thunderTime = 20000;
        g_game.rain = g_game.prevRain = g_game.thunder = g_game.prevThunder = 1.0f;
        g_game.player.pitch = 10;
    }
    if (wokeAt && f == wokeAt + 452) {
        g_testShotName = "rain";
        reportTicks("rain");
        std::printf("particles alive: %d\n", g_game.particles.count);
    }
    if (wokeAt && f == wokeAt + 453) {
        const Player& p = g_game.player;
        float s = std::sin(p.yaw / 57.29578f), c = std::cos(p.yaw / 57.29578f);
        int x = ifloor(p.x - s * 8), z = ifloor(p.z + c * 8);
        g_game.strikeLightning((float)x, (float)g_game.world.topSolidAt(x, z), (float)z);
    }
    if (wokeAt && f == wokeAt + 454) g_testShotName = "lightning";
    if (wokeAt && f == wokeAt + 456) {
        // Turn the area into tundra to see snow fall.
        int pcx = ifloor(g_game.player.x) >> 4, pcz = ifloor(g_game.player.z) >> 4;
        for (int cx = pcx - 3; cx <= pcx + 3; ++cx)
            for (int cz = pcz - 3; cz <= pcz + 3; ++cz)
                if (Chunk* c = g_game.world.chunk(cx, cz)) std::memset(c->temperature, 0, sizeof(c->temperature));
    }
    if (wokeAt && f == wokeAt + 470) g_testShotName = "snow";
    if (wokeAt && f == wokeAt + 472) {
        g_game.clearWeather();
        g_game.rain = g_game.prevRain = g_game.thunder = g_game.prevThunder = 0;
        g_game.player.pitch = -35;
    }
    if (wokeAt && f == wokeAt + 475) g_testShotName = "clouds";
    if (wokeAt && f == wokeAt + 476) {
        Player& p = g_game.player;
        p.pitch = 15;
        float s = std::sin(p.yaw / 57.29578f), c = std::cos(p.yaw / 57.29578f);
        g_game.explode(p.x - s * 7, p.y + 1, p.z + c * 7, 3.0f);
    }
    if (wokeAt && f == wokeAt + 478) {
        g_testShotName = "explosion_particles";
        std::printf("particles alive: %d\n", g_game.particles.count);
    }
    if (wokeAt && f == wokeAt + 480) {
        quitToTitle();
        refreshSlots();
        g_state = State::SelectWorld;
        g_worldSel = 0;
    }
    if (wokeAt && f == wokeAt + 482) g_testShotName = "select_world";
    if (wokeAt && f == wokeAt + 483) openCreateWorld(1);
    if (wokeAt && f == wokeAt + 485) g_testShotName = "create_world";
    if ((wokeAt && f == wokeAt + 486) || f == 2100) {
        std::printf("done\n");
        g_exitRequested = true;
    }
#ifdef MC_SHOWCASE
    g_holdButtons = g_in.held;
    g_holdAx = g_in.ax;
    g_holdAy = g_in.ay;
#endif
}

void autotestAfterFrame() {
    if (!g_testShotName) return;
#ifdef MC_SHOWCASE
    std::printf("scene %s\n", g_testShotName);
    std::fflush(stdout);
    g_testShotName = nullptr;
    g_showcaseHold = 180;
    return;
#endif
    char path[64];
    std::snprintf(path, sizeof(path), "shot_%02d_%s.ppm", g_testShot++, g_testShotName);
    bool ok = gfx::screenshot(path);
    std::printf("%s %s  sections=%d verts=%d chunks=%d crack=%d target=%d onGround=%d\n", path, ok ? "ok" : "FAILED",
                g_renderer.drawnSections, g_renderer.drawnVertices, g_game.world.loadedChunks, g_game.crackStage(),
                g_game.hasBlockTarget ? 1 : 0, g_game.player.onGround ? 1 : 0);
    g_testShotName = nullptr;
}
#endif

const char* stateName() {
    switch (g_state) {
        case State::Title: return "title";
        case State::SelectWorld: return "select-world";
        case State::CreateWorld: return "create-world";
        case State::TextEntry: return "text";
        case State::Playing: return "playing";
        case State::Paused: return "paused";
        case State::Picker: return "picker";
        case State::Container: return "container";
        case State::Dead: return "dead";
        case State::Sign: return "sign";
    }
    return "unknown";
}

void crashFrame() {
    crashlog::FrameInfo info;
    info.state = stateName();
    info.chunks = g_game.world.loadedChunks;
    info.worldOom = g_game.world.outOfMemory ? 1 : 0;
    info.renderOom = g_renderer.outOfMemory ? 1 : 0;
    info.backlog = g_game.world.workerBacklog();
    info.inFlight = g_game.world.workerInFlight();
    info.renderDistance = renderDistance();
    info.fpsMilli = (int)(g_fps * 1000.0f);
    info.cpuPct = g_cpuPct;
    info.gpuPct = g_gpuPct;
    info.tickUs = g_avgTick;
    info.streamUs = g_avgStream;
    info.meshUs = g_avgMesh;
    info.drawUs = g_avgDraw;
    info.x = g_game.player.x;
    info.y = g_game.player.y;
    info.z = g_game.player.z;
    struct mallinfo mi = mallinfo();
    info.usedKb = mi.uordblks / 1024;
    info.freeKb = crashlog::memoryFreeKb();
    info.topKb = mi.fordblks / 1024;
    info.kernelKb = (int)(sceKernelMaxFreeMemSize() / 1024);
    g_game.world.setMemoryBudget(info.freeKb);
    crashlog::setFrame(info);
    crashlog::heartbeat();
}

}  // namespace

int main() {
    crashlog::install();
    setupCallbacks();
    scePowerSetClockFrequency(333, 333, 166);
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    mh::init();
    initBlocks();
    mesherInit({asset_grasscolor, asset_foliagecolor});
    gfx::init();
    loadSettings();
    sound::init();

    g_game.onChunkRelease = [](Chunk* c, void* u) { ((WorldRenderer*)u)->releaseChunk(c); };
    g_game.releaseUser = &g_renderer;
    g_renderer.drawEntities = [](void*, const Camera& cam) { drawEntities(g_game, cam, g_partial); };
    g_renderer.drawEffects = [](void*, const Camera& cam) { drawEffects(g_game, cam, g_partial, g_settings.fancy); };
    if (!g_game.init() || !g_renderer.init()) {
        while (!g_exitRequested) {
            crashlog::note("oom-startup");
            crashlog::heartbeat();
            gfx::beginFrame(0xFF000000);
            gfx::begin2D();
            gfx::textCentered(240, 130, "Not enough memory to start.", 0xFFFFFFFF);
            gfx::endFrame();
        }
        sceKernelExitGame();
        return 0;
    }
    applySettings();
#ifdef MC_AUTOTEST
    {
        struct mallinfo mi = mallinfo();
        std::printf("boot heap: used %d KB, allocatable %d KB, arena free %d KB, kernel %d KB\n", mi.uordblks / 1024,
                    crashlog::memoryFreeKb(), mi.fordblks / 1024, (int)(sceKernelMaxFreeMemSize() / 1024));
    }
#endif

    double last = nowSeconds();
    double fpsTimer = 0;
    int frames = 0;
    while (!g_exitRequested) {
        double now = nowSeconds();
        double dt = now - last;
        last = now;
        if (dt > 0.25) dt = 0.25;
        crashFrame();
        g_in.poll(now);
#ifdef MC_AUTOTEST
#ifdef MC_SHOWCASE
        dt = 1.0 / 60.0;
#else
        dt = 0.05;
#endif
        autotestInput();
#endif
        g_itemNameTimer -= dt;
        g_messageTimer -= dt;
        fpsTimer += dt;
        ++frames;
        g_gpuWindowUs += (uint32_t)gfx::lastGpuMicroseconds();
        g_sleptWindowUs += (uint32_t)gfx::takeSleptMicroseconds();
        g_accTick += g_usTick;
        g_accStream += g_usStream;
        g_accMesh += g_usMesh;
        g_accDraw += g_usDraw;
        if (fpsTimer >= 1.0) {
            g_fps = frames / (float)fpsTimer;
            g_avgTick = frames ? (int)(g_accTick / (uint32_t)frames) : 0;
            g_avgStream = frames ? (int)(g_accStream / (uint32_t)frames) : 0;
            g_avgMesh = frames ? (int)(g_accMesh / (uint32_t)frames) : 0;
            g_avgDraw = frames ? (int)(g_accDraw / (uint32_t)frames) : 0;
            g_accTick = g_accStream = g_accMesh = g_accDraw = 0;
            uint64_t wallUs = (uint64_t)(fpsTimer * 1000000.0);
            g_gpuPct = wallUs ? (int)((uint64_t)g_gpuWindowUs * 100 / wallUs) : 0;
            if (g_gpuPct > 100) g_gpuPct = 100;
            if (g_gpuPct < 0) g_gpuPct = 0;
            g_gpuWindowUs = 0;
            // Busy time is the part of the second this thread was not asleep in the vblank or GE wait.
            g_cpuPct = wallUs > g_sleptWindowUs ? (int)((wallUs - g_sleptWindowUs) * 100 / wallUs) : 0;
            if (g_cpuPct > 100) g_cpuPct = 100;
            if (g_cpuPct < 0) g_cpuPct = 0;
            g_sleptWindowUs = 0;
            frames = 0;
            fpsTimer = 0;
        }
        bool menu = g_state == State::Title || g_state == State::SelectWorld || g_state == State::CreateWorld ||
                    g_state == State::Paused || g_state == State::Dead;
        if (menu && g_in.hit(PSP_CTRL_CROSS)) sound::ui("random.click");
        switch (g_state) {
            case State::Title: titleFrame(now); break;
            case State::SelectWorld: selectWorldFrame(now); break;
            case State::CreateWorld: createWorldFrame(now); break;
            case State::TextEntry: textEntryFrame(now, dt); break;
            case State::Playing: playingFrame(now, dt); break;
            case State::Paused: pausedFrame(now); break;
            case State::Picker: pickerFrame(now, dt); break;
            case State::Container: containerFrame(now, dt); break;
            case State::Dead: deadFrame(now, dt); break;
            case State::Sign: signFrame(now, dt); break;
        }
        if (g_game.worldOpen()) {
            Camera c = currentCamera();
#ifdef MC_AUTOTEST
            g_testSounds += g_game.soundCount;
#endif
            sound::playQueued(g_game, c.x, c.y, c.z, c.yaw);
            sound::updateRecord(g_game, c.x, c.y, c.z);
        } else {
            g_game.soundCount = 0;
            sound::updateRecord(g_game, 0, 0, 0);
        }
        sound::updateMusic(dt);
        crashlog::heartbeat();
#ifdef MC_AUTOTEST
        autotestAfterFrame();
#endif
    }
    if (g_state == State::Container) closeScreen();
    if (g_game.worldOpen()) {
        drawLoading("Saving level", "Saving chunks", -1);
        crashlog::expectLong(true);
        g_game.closeWorld();
        crashlog::expectLong(false);
    }
    saveSettings();
    sound::shutdown();
    gfx::present();
    sceGuTerm();
    sceKernelExitGame();
    return 0;
}
