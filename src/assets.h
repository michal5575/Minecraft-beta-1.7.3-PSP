#pragma once
#include <cstdint>

// Blobs produced by tools/convert_assets.py and linked in through assets.S. Every blob X also has an
// asset_X_end symbol. "zlib" blobs are inflated into VRAM at startup; indexed ones come with an X_clut palette
// of RGBA8888 colors (16 entries for 4-bit, 256 for 8-bit pixels).
#define ASSET(name)                    \
    extern const uint8_t asset_##name[]; \
    extern const uint8_t asset_##name##_end[];
#define ASSET_INDEXED(name) ASSET(name) ASSET(name##_clut)

extern "C" {
ASSET(terrain)        // zlib: 256x256 RGBA8888, swizzled
ASSET(gui)            // zlib: 256x256 RGBA8888
ASSET(items)          // zlib: 256x256 RGBA8888
ASSET(art)            // zlib: 256x256 RGBA4444 (art/kz.png)
ASSET(fontw)          // 256 glyph widths
ASSET(crosshair)      // 16x16 RGBA8888
ASSET(background)     // 16x16 RGBA8888
ASSET(grasscolor)     // 256x256 RGB888
ASSET(foliagecolor)   // 256x256 RGB888
ASSET(tilecolors)     // 256 x RGB888
ASSET(sun)            // 32x32 RGBA8888
ASSET(moon)           // 32x32 RGBA8888
ASSET(arrows)         // 32x32 RGBA8888
ASSET(sign)           // 64x32 RGBA8888
ASSET(dial)           // 16x16 RGBA8888 (misc/dial.png)
ASSET(mob_zombie)     // zlib: 64x32 RGBA8888 each
ASSET(mob_skeleton)
ASSET(mob_creeper)
ASSET(mob_spider)
ASSET(mob_pig)
ASSET(mob_cow)
ASSET(mob_sheep)
ASSET(mob_sheep_fur)
ASSET(mob_chicken)
ASSET(mob_char)
ASSET(mob_slime)
ASSET(mob_squid)
ASSET(mob_wolf)
ASSET(mob_wolf_tame)
ASSET(mob_wolf_angry)
ASSET(mob_saddle)
ASSET(mob_cart)       // item/cart.png
ASSET(mob_boat)       // item/boat.png
ASSET(mob_ghast)
ASSET(mob_ghast_fire)
ASSET(mob_pigzombie)
ASSET_INDEXED(font)           // 128x128, 4-bit
ASSET_INDEXED(logo)           // 256x128, 4-bit (title/mclogo.png, top half)
ASSET_INDEXED(icons)          // 256x32, 8-bit (top of gui/icons.png)
ASSET_INDEXED(gui_inventory)  // 256x256, 4-bit
ASSET_INDEXED(gui_crafting)
ASSET_INDEXED(gui_furnace)
ASSET_INDEXED(gui_container)
ASSET_INDEXED(gui_trap)
ASSET_INDEXED(particles)      // 128x128, 8-bit
ASSET_INDEXED(rain)           // 64x256, 8-bit (environment/rain.png)
ASSET_INDEXED(snow)           // 64x256, 4-bit
ASSET_INDEXED(clouds)         // 256x256, 4-bit
}

#undef ASSET_INDEXED
#undef ASSET
