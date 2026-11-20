#!/usr/bin/env python3
"""Extract textures from minecraft.jar and convert them into raw PSP-friendly blobs.

Output (in OUT_DIR):
  *.bin.z          zlib streams the PSP inflates into VRAM at startup (terrain, items, gui, art, mobs)
  *_clut.bin       RGBA8888 palettes of the 4- and 8-bit indexed textures
  terrain.bin      256x256 RGBA8888, PSP-swizzled, water/lava tiles generated
  gui.bin          256x256 RGBA8888 (gui/gui.png)
  font.bin         128x128 RGBA8888 (font/default.png)
  fontw.bin        256 bytes of glyph widths
  crosshair.bin    16x16 RGBA8888
  background.bin   16x16 RGBA8888
  logo.bin         256x128 RGBA8888 (title/mclogo.png, top half)
  grasscolor.bin   256x256 RGB888
  foliagecolor.bin 256x256 RGB888
  tilecolors.bin   256 average RGB colors of terrain tiles (used by the host map dump)
  ICON0.png        144x80 XMB icon
  assets.S         .incbin wrapper exposing the blobs as symbols
"""
import io
import math
import os
import random
import sys
import zipfile
import zlib

from PIL import Image

# Order matches gfx::MobTexture.
MOB_TEXTURES = ["zombie", "skeleton", "creeper", "spider", "pig", "cow", "sheep", "sheep_fur", "chicken", "char", "slime", "squid",
                "wolf", "wolf_tame", "wolf_angry", "saddle", "cart", "boat", "ghast", "ghast_fire", "pigzombie"]
MOB_TEXTURE_PATHS = {"cart": "item/cart.png", "boat": "item/boat.png"}


def load(z, name):
    return Image.open(io.BytesIO(z.read(name))).convert("RGBA")


def swizzle(data, width_bytes, height):
    out = bytearray(len(data))
    bw = width_bytes // 16
    bh = height // 8
    o = 0
    for by in range(bh):
        for bx in range(bw):
            for j in range(8):
                s = (by * 8 + j) * width_bytes + bx * 16
                out[o:o + 16] = data[s:s + 16]
                o += 16
    return bytes(out)


def water_tile():
    """Port of TextureWaterFX, run for a while to reach a steady state."""
    rnd = random.Random(1234)
    g = [0.0] * 256
    h = [0.0] * 256
    i = [0.0] * 256
    j = [0.0] * 256
    for _ in range(200):
        for x in range(16):
            for y in range(16):
                s = 0.0
                for xx in range(x - 1, x + 2):
                    s += g[(xx & 15) + (y & 15) * 16]
                h[x + y * 16] = s / 3.3 + i[x + y * 16] * 0.8
        for x in range(16):
            for y in range(16):
                k = x + y * 16
                i[k] += j[k] * 0.05
                if i[k] < 0.0:
                    i[k] = 0.0
                j[k] -= 0.1
                if rnd.random() < 0.05:
                    j[k] = 0.5
        g, h = h, g
    px = []
    for k in range(256):
        v = max(0.0, min(1.0, g[k]))
        v2 = v * v
        px.append((int(32 + v2 * 32), int(50 + v2 * 64), 255, int(146 + v2 * 50)))
    return px


def lava_tile():
    """Port of TextureLavaFX."""
    rnd = random.Random(4321)
    g = [0.0] * 256
    h = [0.0] * 256
    i = [0.0] * 256
    j = [0.0] * 256
    for _ in range(200):
        for x in range(16):
            for y in range(16):
                s = 0.0
                a = int(math.sin(y * math.pi * 2.0 / 16.0) * 1.2)
                b = int(math.sin(x * math.pi * 2.0 / 16.0) * 1.2)
                for xx in range(x - 1, x + 2):
                    for yy in range(y - 1, y + 2):
                        s += g[((xx + a) & 15) + ((yy + b) & 15) * 16]
                k = x + y * 16
                h[k] = s / 10.0 + (i[((x + 0) & 15) + ((y + 0) & 15) * 16] + i[((x + 1) & 15) + ((y + 0) & 15) * 16]
                                    + i[((x + 1) & 15) + ((y + 1) & 15) * 16] + i[((x + 0) & 15) + ((y + 1) & 15) * 16]) / 4.0 * 0.8
                i[k] += j[k] * 0.01
                if i[k] < 0.0:
                    i[k] = 0.0
                j[k] -= 0.06
                if rnd.random() < 0.005:
                    j[k] = 1.5
        g, h = h, g
    px = []
    for k in range(256):
        v = max(0.0, min(1.0, g[k] * 2.0))
        r = int(v * 100.0 + 155.0)
        gg = int(v * v * 255.0)
        b = int(v * v * v * v * 128.0)
        px.append((r, gg, b, 255))
    return px


def fire_tile(seed):
    """Port of TextureFlamesFX (16x20 heat field, the bottom row is reseeded every step)."""
    rnd = random.Random(seed)
    g = [0.0] * 320
    h = [0.0] * 320
    for _ in range(100):
        for x in range(16):
            for y in range(20):
                n = 18
                s = g[x + (y + 1) % 20 * 16] * n
                for xx in range(x - 1, x + 2):
                    for yy in range(y, y + 2):
                        if 0 <= xx < 16 and 0 <= yy < 20:
                            s += g[xx + yy * 16]
                        n += 1
                h[x + y * 16] = s / (n * 1.06)
                if y >= 19:
                    h[x + y * 16] = (rnd.random() * rnd.random() * rnd.random() * 4.0 + rnd.random() * 0.1 + 0.2)
        g, h = h, g
    px = []
    for k in range(256):
        v = max(0.0, min(1.0, g[k] * 1.8))
        px.append((int(v * 155 + 100), int(v * v * 255), int(v ** 10 * 255), 255 if v >= 0.5 else 0))
    return px


def put_tile(img, index, pixels):
    tx = (index % 16) * 16
    ty = (index // 16) * 16
    for k, p in enumerate(pixels):
        img.putpixel((tx + k % 16, ty + k // 16), p)


def font_widths(font):
    widths = bytearray(256)
    for c in range(256):
        cx = (c % 16) * 8
        cy = (c // 16) * 8
        w = 7
        while w >= 0:
            empty = True
            for y in range(8):
                if font.getpixel((cx + w, cy + y))[3] > 0:
                    empty = False
                    break
            if not empty:
                break
            w -= 1
        if c == 32:
            w = 2
        widths[c] = w + 2
    return bytes(widths)


def main():
    if len(sys.argv) != 3:
        print("usage: convert_assets.py <minecraft.jar> <out_dir>")
        sys.exit(1)
    jar, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    z = zipfile.ZipFile(jar)

    terrain = load(z, "terrain.png")
    put_tile(terrain, 205, water_tile())
    put_tile(terrain, 237, lava_tile())
    put_tile(terrain, 31, fire_tile(31))
    put_tile(terrain, 47, fire_tile(47))
    def write_z(name, data):
        with open(os.path.join(out, name + ".bin"), "wb") as f:
            f.write(zlib.compress(data, 9))

    def write_clut(name, img, bits):
        """Indexed texture (T4 pixels packed low nibble first, or T8) plus a palette of RGBA8888 colors."""
        pixels = list(img.getdata())
        pal = sorted(set(pixels))
        assert len(pal) <= 1 << bits, (name, len(pal))
        index = {c: i for i, c in enumerate(pal)}
        idx = [index[c] for c in pixels]
        if bits == 4:
            data = bytes(idx[i] | (idx[i + 1] << 4) for i in range(0, len(idx), 2))
        else:
            data = bytes(idx)
        with open(os.path.join(out, name + ".bin"), "wb") as f:
            f.write(data)
        clut = bytearray()
        for c in pal + [(0, 0, 0, 0)] * ((1 << bits) - len(pal)):
            clut += bytes(c)
        with open(os.path.join(out, name + "_clut.bin"), "wb") as f:
            f.write(clut)

    write_z("terrain", swizzle(terrain.tobytes(), 256 * 4, 256))

    colors = bytearray()
    for t in range(256):
        tile = terrain.crop(((t % 16) * 16, (t // 16) * 16, (t % 16) * 16 + 16, (t // 16) * 16 + 16))
        r = g = b = n = 0
        for p in tile.getdata():
            if p[3] > 0:
                r += p[0]
                g += p[1]
                b += p[2]
                n += 1
        n = max(n, 1)
        colors += bytes((r // n, g // n, b // n))
    with open(os.path.join(out, "tilecolors.bin"), "wb") as f:
        f.write(colors)

    write_z("gui", load(z, "gui/gui.png").tobytes())

    font = load(z, "font/default.png")
    write_clut("font", font, 4)
    with open(os.path.join(out, "fontw.bin"), "wb") as f:
        f.write(font_widths(font))

    icons = load(z, "gui/icons.png")
    with open(os.path.join(out, "crosshair.bin"), "wb") as f:
        f.write(icons.crop((0, 0, 16, 16)).tobytes())

    bg = load(z, "gui/background.png")
    with open(os.path.join(out, "background.bin"), "wb") as f:
        f.write(bg.tobytes())

    logo = load(z, "title/mclogo.png")
    write_clut("logo", logo.crop((0, 0, 256, 128)), 4)

    for name in ("grasscolor", "foliagecolor"):
        img = load(z, "misc/%s.png" % name).convert("RGB")
        with open(os.path.join(out, name + ".bin"), "wb") as f:
            f.write(img.tobytes())

    def write(name, img):
        with open(os.path.join(out, name + ".bin"), "wb") as f:
            f.write(img.tobytes())

    write_z("items", load(z, "gui/items.png").tobytes())
    write_clut("icons", icons.crop((0, 0, 256, 32)), 8)
    for name in ("inventory", "crafting", "furnace", "container", "trap"):
        write_clut("gui_" + name, load(z, "gui/%s.png" % name), 4)
    write("sun", load(z, "terrain/sun.png"))
    write("moon", load(z, "terrain/moon.png"))
    write("arrows", load(z, "item/arrows.png"))
    for name in MOB_TEXTURES:
        write_z("mob_" + name, load(z, MOB_TEXTURE_PATHS.get(name, "mob/%s.png" % name)).tobytes())
    art = load(z, "art/kz.png")
    data = bytearray()
    for r, g, b, a in art.getdata():
        v = (r >> 4) | ((g >> 4) << 4) | ((b >> 4) << 8) | ((a >> 4) << 12)
        data += bytes((v & 255, v >> 8))
    write_z("art", bytes(data))
    write("sign", load(z, "item/sign.png"))
    write("dial", load(z, "misc/dial.png"))
    write_clut("particles", load(z, "particles.png"), 8)
    write_clut("rain", load(z, "environment/rain.png"), 8)
    write_clut("snow", load(z, "environment/snow.png"), 4)
    write_clut("clouds", load(z, "environment/clouds.png"), 4)

    icon = Image.new("RGBA", (144, 80))
    tile = bg.resize((32, 32), Image.NEAREST)
    for x in range(0, 144, 32):
        for y in range(0, 80, 32):
            icon.paste(tile, (x, y))
    icon = Image.blend(icon, Image.new("RGBA", (144, 80), (0, 0, 0, 255)), 0.45)
    lg = logo.crop((0, 0, 155, 44))
    lg2 = logo.crop((0, 45, 155, 89))
    full = Image.new("RGBA", (310, 44))
    full.paste(lg, (0, 0))
    full.paste(lg2, (155, 0))
    full = full.resize((136, 19), Image.LANCZOS)
    icon.alpha_composite(full, (4, 30))
    icon.save(os.path.join(out, "ICON0.png"))

    indexed = ["font", "logo", "icons", "gui_inventory", "gui_crafting", "gui_furnace", "gui_container", "gui_trap",
               "particles", "rain", "snow", "clouds"]
    blobs = ["terrain", "gui", "fontw", "crosshair", "background", "grasscolor", "foliagecolor", "tilecolors",
             "items", "sun", "moon", "arrows", "art", "sign", "dial"] + ["mob_" + n for n in MOB_TEXTURES]
    blobs += indexed + [n + "_clut" for n in indexed]
    with open(os.path.join(out, "assets.S"), "w") as f:
        f.write("\t.section .rodata\n")
        for b in blobs:
            path = os.path.abspath(os.path.join(out, b + ".bin"))
            f.write("\t.balign 64\n\t.global asset_%s\nasset_%s:\n\t.incbin \"%s\"\n" % (b, b, path))
            f.write("\t.global asset_%s_end\nasset_%s_end:\n" % (b, b))
        f.write("\t.section .note.GNU-stack,\"\",%progbits\n")


if __name__ == "__main__":
    main()
