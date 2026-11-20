#pragma once
#include <cstdint>

#include "../core/blocks.h"
#include "../core/items.h"
#include "../core/mat4.h"
#include "../core/player.h"

namespace gfx {

constexpr int SCREEN_W = 480;
constexpr int SCREEN_H = 272;

struct Texture {
    const void* data;
    int width, height;
    bool swizzled;
    bool rgba4444 = false;
    const void* clut = nullptr;  // indexed pixels: 16 (4-bit) or 256 (8-bit, clut8) RGBA8888 colors
    bool clut8 = false;
};

enum MobTexture { MOB_ZOMBIE, MOB_SKELETON, MOB_CREEPER, MOB_SPIDER, MOB_PIG, MOB_COW, MOB_SHEEP, MOB_SHEEP_FUR, MOB_CHICKEN,
                  MOB_PLAYER, MOB_SLIME, MOB_SQUID, MOB_WOLF, MOB_WOLF_TAME, MOB_WOLF_ANGRY, MOB_SADDLE, MOB_CART,
                  MOB_BOAT, MOB_GHAST, MOB_GHAST_FIRE, MOB_PIGZOMBIE, MOB_TEXTURE_COUNT };

extern Texture terrain, gui, font, crosshair, background, logo;
extern Texture items, icons, invGui, craftGui, furnaceGui, chestGui, dispenserGui, sun, moon, arrows;
extern Texture art, sign, particles, rain, snow, clouds;
extern Texture mobs[MOB_TEXTURE_COUNT];

inline uint32_t rgba(int r, int g, int b, int a = 255) {
    return (uint32_t)(r & 255) | ((uint32_t)(g & 255) << 8) | ((uint32_t)(b & 255) << 16) | ((uint32_t)(a & 255) << 24);
}
// 0xRRGGBB (Minecraft style) with alpha, to the GE's ABGR layout.
inline uint32_t fromRGB(uint32_t rgb, int a = 255) { return rgba((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, a); }

void init();
// endFrame only queues the frame; it is shown (after the GE finishes and the next vblank) by present(),
// which beginFrame calls, so the CPU runs game logic while the GE draws.
void beginFrame(uint32_t clearColor);
void endFrame();
void present();
// How long the GE spent on the list that just finished, in microseconds.
int lastGpuMicroseconds();
// Microseconds this thread has spent blocked since the previous call. The main loop turns that
// into the CPU figure: a vblank wait must not count as the game using the processor.
int takeSleptMicroseconds();
// Blocks until the GE has drawn everything queued; call before freeing memory the GE may still read.
void waitIdle();
bool screenshot(const char* path);  // PPM of the frame on screen

void bind(const Texture& t);

// 3D
void begin3D(const Mat4& proj, const Mat4& view, bool fog, float fogNear, float fogFar, uint32_t fogColor);
void setModel(const Mat4& m);
void drawLines3D(const float* xyz, int vertexCount, uint32_t color);

// 2D (screen pixels)
void begin2D();
void rect(float x, float y, float w, float h, uint32_t color);
void gradient(float x, float y, float w, float h, uint32_t top, uint32_t bottom);
void image(const Texture& t, float x, float y, float w, float h, float u, float v, float uw, float vh,
           uint32_t color = 0xFFFFFFFF);
void tiledBackground(uint32_t color);
int textWidth(const char* s);
void text(float x, float y, const char* s, uint32_t color, bool shadow = true);
void textCentered(float cx, float y, const char* s, uint32_t color, bool shadow = true);
void textRotated(float cx, float cy, const char* s, uint32_t color, float angleDeg, float scale);
void blockIcon(ItemStack item, float x, float y, float size);
// Any item at 16x16 (blocks as isometric cubes, items from items.png).
void itemIcon(const ItemStack& item, float x, float y);
// Stack count and durability bar (RenderItem.renderItemOverlayIntoGUI).
void itemOverlay(const ItemStack& item, float x, float y);

// Breaking animation on the faces of a box, terrain tile `tex`.
void drawCrack(const AABB& b, int tex);

// Textured triangles in model space (u, v normalized), for entity models.
struct ModelVertex {
    float u, v;
    uint32_t color;
    float x, y, z;
};
ModelVertex* allocModel(int n);
void* allocBytes(int bytes);
// Hold this many extra bytes back so later passes (water, the hand) still fit.
void setListMargin(int bytes);
// True after an allocation was refused because the display list is full.
bool listBlocked();
void drawModel(const Mat4& model, const Texture& t, const ModelVertex* v, int n);
// Untextured colored primitives in world space (GU_TRIANGLES, GU_TRIANGLE_STRIP, ...).
struct ColorVertex {
    uint32_t color;
    float x, y, z;
};
ColorVertex* allocColor(int n);
void drawColored3D(int prim, const ColorVertex* v, int n);
// Pairs of opposite corners in world space, drawn as screen-aligned rectangles (billboards).
void drawSprites3D(const Texture& t, const ModelVertex* v, int n);

int charWidth(uint8_t c);
// Font quads (8x8 units per glyph, y down) for drawModel with the font texture; needs 6 vertices per
// character, returns the count written.
int text3D(ModelVertex* v, float x, float y, const char* s, uint32_t color);
// Redraws the compass and clock icons in items.png (angles in radians).
void updateDials(float compassAngle, float clockAngle);
// Advances the nether portal animation in terrain tile 14.
void updatePortal();

}  // namespace gfx
