#include "mesher.h"

#include "fluid.h"
#include "player.h"

#include <cmath>
#include <cstdlib>

namespace {

float g_brightTables[2][16];  // WorldProvider.lightBrightnessTable: overworld, Nether
const float* g_bright = g_brightTables[0];
ColorMaps g_maps;

const float FACE_SHADE[6] = {0.5f, 1.0f, 0.8f, 0.8f, 0.6f, 0.6f};
const int FACE_DIR[6][3] = {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};

inline uint16_t packColor(uint32_t rgb, float b) {
    int r = (int)(((rgb >> 16) & 255) * b * (31.0f / 255.0f) + 0.5f);
    int g = (int)(((rgb >> 8) & 255) * b * (63.0f / 255.0f) + 0.5f);
    int bl = (int)((rgb & 255) * b * (31.0f / 255.0f) + 0.5f);
    if (r > 31) r = 31;
    if (g > 63) g = 63;
    if (bl > 31) bl = 31;
    return (uint16_t)(r | (g << 5) | (bl << 11));
}

inline int16_t toPos(float v) {
    float s = v * 1024.0f;
    return (int16_t)(s >= 0 ? (int)(s + 0.5f) : (int)(s - 0.5f));
}

struct Quad {
    float p[4][3];
    int u0, v0, u1, v1;  // GE units, 128 per texel
};

void emit(MeshBuffer& b, const Quad& q, uint16_t color, bool doubleSided) {
    int need = doubleSided ? 12 : 6;
    if (b.count + need > b.capacity) {
        b.overflow = true;
        return;
    }
    MeshVertex c[4];
    const int us[4] = {q.u0, q.u1, q.u1, q.u0};
    const int vs[4] = {q.v0, q.v0, q.v1, q.v1};
    for (int i = 0; i < 4; ++i) {
        c[i].u = (uint16_t)us[i];
        c[i].v = (uint16_t)vs[i];
        c[i].color = color;
        c[i].x = toPos(q.p[i][0]);
        c[i].y = toPos(q.p[i][1]);
        c[i].z = toPos(q.p[i][2]);
    }
    MeshVertex* o = b.data + b.count;
    o[0] = c[0];
    o[1] = c[1];
    o[2] = c[2];
    o[3] = c[0];
    o[4] = c[2];
    o[5] = c[3];
    if (doubleSided) {
        o[6] = c[0];
        o[7] = c[2];
        o[8] = c[1];
        o[9] = c[0];
        o[10] = c[3];
        o[11] = c[2];
    }
    b.count += need;
}

// Sub-rectangle (0..16 in tile pixels) of a terrain tile, converted to GE units with a
// small inset so nearest filtering never samples the neighboring tile.
void setUV(Quad& q, int tex, float a0, float b0, float a1, float b1) {
    float tu = (float)((tex & 15) * 16), tv = (float)((tex >> 4) * 16);
    q.u0 = (int)((tu + a0) * 128.0f) + 2;
    q.u1 = (int)((tu + a1) * 128.0f) - 2;
    q.v0 = (int)((tv + b0) * 128.0f) + 2;
    q.v1 = (int)((tv + b1) * 128.0f) - 2;
}

inline void setP(float* p, float x, float y, float z) {
    p[0] = x;
    p[1] = y;
    p[2] = z;
}

// Quad with per-corner texture coordinates in tile pixels (0..16), inset toward the middle.
void emitUV(MeshBuffer& b, const float (&p)[4][3], const float (&uv)[4][2], int tex, uint16_t color,
            bool doubleSided) {
    int need = doubleSided ? 12 : 6;
    if (b.count + need > b.capacity) {
        b.overflow = true;
        return;
    }
    float tu = (float)((tex & 15) * 16), tv = (float)((tex >> 4) * 16);
    float mu = (uv[0][0] + uv[1][0] + uv[2][0] + uv[3][0]) * 0.25f;
    float mv = (uv[0][1] + uv[1][1] + uv[2][1] + uv[3][1]) * 0.25f;
    MeshVertex c[4];
    for (int i = 0; i < 4; ++i) {
        c[i].u = (uint16_t)((tu + uv[i][0]) * 128.0f + (uv[i][0] > mu ? -2 : 2));
        c[i].v = (uint16_t)((tv + uv[i][1]) * 128.0f + (uv[i][1] > mv ? -2 : 2));
        c[i].color = color;
        c[i].x = toPos(p[i][0]);
        c[i].y = toPos(p[i][1]);
        c[i].z = toPos(p[i][2]);
    }
    MeshVertex* o = b.data + b.count;
    o[0] = c[0];
    o[1] = c[1];
    o[2] = c[2];
    o[3] = c[0];
    o[4] = c[2];
    o[5] = c[3];
    if (doubleSided) {
        o[6] = c[0];
        o[7] = c[2];
        o[8] = c[1];
        o[9] = c[0];
        o[10] = c[3];
        o[11] = c[2];
    }
    b.count += need;
}

// Box face with UVs derived from the bounds, like RenderBlocks in Beta.
void boxFace(Quad& q, int face, float x, float y, float z, float x0, float y0, float z0, float x1, float y1, float z1,
             int tex) {
    float X0 = x + x0, Y0 = y + y0, Z0 = z + z0, X1 = x + x1, Y1 = y + y1, Z1 = z + z1;
    switch (face) {
        case 0:
            setP(q.p[0], X0, Y0, Z1);
            setP(q.p[1], X1, Y0, Z1);
            setP(q.p[2], X1, Y0, Z0);
            setP(q.p[3], X0, Y0, Z0);
            setUV(q, tex, x0 * 16, (1 - z1) * 16, x1 * 16, (1 - z0) * 16);
            break;
        case 1:
            setP(q.p[0], X0, Y1, Z0);
            setP(q.p[1], X1, Y1, Z0);
            setP(q.p[2], X1, Y1, Z1);
            setP(q.p[3], X0, Y1, Z1);
            setUV(q, tex, x0 * 16, z0 * 16, x1 * 16, z1 * 16);
            break;
        case 2:
            setP(q.p[0], X1, Y1, Z0);
            setP(q.p[1], X0, Y1, Z0);
            setP(q.p[2], X0, Y0, Z0);
            setP(q.p[3], X1, Y0, Z0);
            setUV(q, tex, (1 - x1) * 16, (1 - y1) * 16, (1 - x0) * 16, (1 - y0) * 16);
            break;
        case 3:
            setP(q.p[0], X0, Y1, Z1);
            setP(q.p[1], X1, Y1, Z1);
            setP(q.p[2], X1, Y0, Z1);
            setP(q.p[3], X0, Y0, Z1);
            setUV(q, tex, x0 * 16, (1 - y1) * 16, x1 * 16, (1 - y0) * 16);
            break;
        case 4:
            setP(q.p[0], X0, Y1, Z0);
            setP(q.p[1], X0, Y1, Z1);
            setP(q.p[2], X0, Y0, Z1);
            setP(q.p[3], X0, Y0, Z0);
            setUV(q, tex, z0 * 16, (1 - y1) * 16, z1 * 16, (1 - y0) * 16);
            break;
        default:
            setP(q.p[0], X1, Y1, Z1);
            setP(q.p[1], X1, Y1, Z0);
            setP(q.p[2], X1, Y0, Z0);
            setP(q.p[3], X1, Y0, Z1);
            setUV(q, tex, (1 - z1) * 16, (1 - y1) * 16, (1 - z0) * 16, (1 - y0) * 16);
            break;
    }
}

inline bool faceVisible(uint8_t id, uint8_t nid) {
    if (blockIsOpaque(nid)) return false;
    if (nid == id && (g_blocks[id].flags & BF_SAME_CULL)) return false;
    return true;
}

struct Ctx {
    const World& w;
    int ox, oy, oz;
    MeshBuffer& opaque;
    MeshBuffer& trans;
    CubeBuffer& cubes;
    bool noCull = false;  // a block drawn on its own (moving piston), every face is emitted
    int pistonFlags = 0;
};

void emitCube(CubeBuffer& b, const Quad& q, int tex, uint16_t color) {
    if (b.count + 6 > b.capacity) {
        b.overflow = true;
        return;
    }
    uint8_t u0 = (uint8_t)((tex & 15) * 8), v0 = (uint8_t)((tex >> 4) * 8);
    const uint8_t us[4] = {u0, (uint8_t)(u0 + 8), (uint8_t)(u0 + 8), u0};
    const uint8_t vs[4] = {v0, v0, (uint8_t)(v0 + 8), (uint8_t)(v0 + 8)};
    CubeVertex c[4];
    for (int i = 0; i < 4; ++i)
        c[i] = {us[i], vs[i], color, (int8_t)q.p[i][0], (int8_t)q.p[i][1], (int8_t)q.p[i][2], 0};
    CubeVertex* o = b.data + b.count;
    o[0] = c[0];
    o[1] = c[1];
    o[2] = c[2];
    o[3] = c[0];
    o[4] = c[2];
    o[5] = c[3];
    b.count += 6;
}

inline float lightAt(const World& w, int x, int y, int z) { return g_bright[w.getBrightness(x, y, z)]; }

inline float lightMax(const World& w, int x, int y, int z, int nx, int ny, int nz) {
    int a = w.getBrightness(x, y, z), b = w.getBrightness(nx, ny, nz);
    return g_bright[a > b ? a : b];
}

// Same faces as renderCube, reading the six neighbors out of this section. lx/ly/lz are in 1..14.
void renderCubeInside(Ctx& c, const Section& sec, uint8_t id, int meta, int lx, int ly, int lz) {
    int i = sectionIndex(lx, ly, lz);
    static const int STEP[6] = {-1, 1, -16, 16, -256, 256};  // -y +y -z +z -x +x
    bool translucent = (g_blocks[id].flags & BF_TRANSLUCENT) != 0;
    uint8_t above = sec.blocks[i + 1];
    int sub = c.w.skySubtracted;
    for (int f = 0; f < 6; ++f) {
        int ni = i + STEP[f];
        uint8_t nid = sec.blocks[ni];
        if (!faceVisible(id, nid)) continue;
        int tex = blockTexture(id, meta, f);
        if (id == GRASS && f >= 2 && (above == SNOW_LAYER || above == SNOW_BLOCK)) tex = 68;
        int nx = c.ox + lx + FACE_DIR[f][0], ny = c.oy + ly + FACE_DIR[f][1], nz = c.oz + lz + FACE_DIR[f][2];
        uint32_t tint = blockTint(&c.w, id, meta, f, nx - FACE_DIR[f][0], nz - FACE_DIR[f][2]);
        Quad q;
        boxFace(q, f, (float)lx, (float)ly, (float)lz, 0, 0, 0, 1, 1, 1, tex);
        uint8_t lv = sec.light[ni];
        int sky = (lv >> 4) - sub, block = lv & 15;
        uint16_t color = packColor(tint, g_bright[(sky > block ? sky : block)] * FACE_SHADE[f]);
        if (translucent)
            emit(c.trans, q, color, false);
        else
            emitCube(c.cubes, q, tex, color);
    }
}

void renderCube(Ctx& c, uint8_t id, int meta, int lx, int ly, int lz) {
    int wx = c.ox + lx, wy = c.oy + ly, wz = c.oz + lz;
    bool translucent = (g_blocks[id].flags & BF_TRANSLUCENT) != 0;
    uint8_t above = c.w.get(wx, wy + 1, wz);
    for (int f = 0; f < 6; ++f) {
        int nx = wx + FACE_DIR[f][0], ny = wy + FACE_DIR[f][1], nz = wz + FACE_DIR[f][2];
        uint8_t nid = c.w.get(nx, ny, nz);
        if (!c.noCull && !faceVisible(id, nid)) continue;
        int tex = blockTexture(id, meta, f);
        if (id == GRASS && f >= 2 && (above == SNOW_LAYER || above == SNOW_BLOCK)) tex = 68;
        uint32_t tint = blockTint(&c.w, id, meta, f, wx, wz);
        Quad q;
        boxFace(q, f, (float)lx, (float)ly, (float)lz, 0, 0, 0, 1, 1, 1, tex);
        uint16_t color = packColor(tint, lightAt(c.w, nx, ny, nz) * FACE_SHADE[f]);
        if (translucent)
            emit(c.trans, q, color, false);
        else if (c.noCull)
            emit(c.opaque, q, color, false);
        else
            emitCube(c.cubes, q, tex, color);
    }
}

void renderPartialBox(Ctx& c, uint8_t id, int meta, int lx, int ly, int lz, float height) {
    int wx = c.ox + lx, wy = c.oy + ly, wz = c.oz + lz;
    for (int f = 0; f < 6; ++f) {
        int nx = wx + FACE_DIR[f][0], ny = wy + FACE_DIR[f][1], nz = wz + FACE_DIR[f][2];
        if (f != 1 && !c.noCull) {
            uint8_t nid = c.w.get(nx, ny, nz);
            if (!faceVisible(id, nid)) continue;
            if (f >= 2 && nid == id) continue;  // same height, so the shared side is hidden
        }
        int tex = blockTexture(id, meta, f);
        Quad q;
        boxFace(q, f, (float)lx, (float)ly, (float)lz, 0, 0, 0, 1, height, 1, tex);
        float l = lightMax(c.w, wx, wy, wz, nx, ny, nz);
        emit(c.opaque, q, packColor(0xFFFFFF, l * FACE_SHADE[f]), false);
    }
}

void renderCactus(Ctx& c, int lx, int ly, int lz) {
    int wx = c.ox + lx, wy = c.oy + ly, wz = c.oz + lz;
    const float in = 1.0f / 16.0f;
    for (int f = 0; f < 6; ++f) {
        int nx = wx + FACE_DIR[f][0], ny = wy + FACE_DIR[f][1], nz = wz + FACE_DIR[f][2];
        if (f <= 1 && !faceVisible(CACTUS, c.w.get(nx, ny, nz))) continue;
        float x0 = 0, y0 = 0, z0 = 0, x1 = 1, y1 = 1, z1 = 1;
        switch (f) {
            case 2: z0 = in; break;
            case 3: z1 = 1 - in; break;
            case 4: x0 = in; break;
            case 5: x1 = 1 - in; break;
            default: break;
        }
        Quad q;
        boxFace(q, f, (float)lx, (float)ly, (float)lz, x0, y0, z0, x1, y1, z1, blockTexture(CACTUS, 0, f));
        float l = lightMax(c.w, wx, wy, wz, nx, ny, nz);
        emit(c.opaque, q, packColor(0xFFFFFF, l * FACE_SHADE[f]), false);
    }
}

// RenderBlocks.renderBlockFluids: corner heights from the surrounding levels.
void renderLiquid(Ctx& c, uint8_t id, int lx, int ly, int lz) {
    int wx = c.ox + lx, wy = c.oy + ly, wz = c.oz + lz;
    bool water = blockIsWater(id);
    // A block with this liquid on every side draws nothing. An ocean column is almost all such
    // blocks; skipping them avoids four corner-height walks per block.
    if (fluidSame(c.w.get(wx, wy + 1, wz), water) && fluidSame(c.w.get(wx, wy - 1, wz), water) &&
        fluidSame(c.w.get(wx + 1, wy, wz), water) && fluidSame(c.w.get(wx - 1, wy, wz), water) &&
        fluidSame(c.w.get(wx, wy, wz + 1), water) && fluidSame(c.w.get(wx, wy, wz - 1), water))
        return;
    MeshBuffer& out = water ? c.trans : c.opaque;
    int tex = blockTexture(id, 0, 1);
    // Corners: (x0,z0) (x0,z1) (x1,z1) (x1,z0).
    float h00 = fluidCornerHeight(c.w, wx, wy, wz, water);
    float h01 = fluidCornerHeight(c.w, wx, wy, wz + 1, water);
    float h11 = fluidCornerHeight(c.w, wx + 1, wy, wz + 1, water);
    float h10 = fluidCornerHeight(c.w, wx + 1, wy, wz, water);
    float x = (float)lx, y = (float)ly, z = (float)lz;
    for (int f = 0; f < 6; ++f) {
        int nx = wx + FACE_DIR[f][0], ny = wy + FACE_DIR[f][1], nz = wz + FACE_DIR[f][2];
        uint8_t nid = c.w.get(nx, ny, nz);
        if (fluidSame(nid, water)) continue;
        if (f != 1 && (blockIsOpaque(nid) || nid == ICE)) continue;
        Quad q;
        float l = lightMax(c.w, wx, wy, wz, nx, ny, nz);
        uint16_t color = packColor(0xFFFFFF, l * FACE_SHADE[f]);
        if (f == 0) {
            boxFace(q, 0, x, y, z, 0, 0, 0, 1, 1, 1, tex);
            emit(out, q, color, false);
            continue;
        }
        // Top and sides are emitted with explicit per-vertex heights.
        float p[4][3];
        float hv[4];
        if (f == 1) {
            setP(p[0], x, y + h00, z);
            setP(p[1], x + 1, y + h10, z);
            setP(p[2], x + 1, y + h11, z + 1);
            setP(p[3], x, y + h01, z + 1);
            hv[0] = hv[1] = hv[2] = hv[3] = 1;
        } else {
            float ha, hb, ax, az, bx, bz;
            switch (f) {
                case 2: ha = h10, hb = h00, ax = 1, az = 0, bx = 0, bz = 0; break;
                case 3: ha = h01, hb = h11, ax = 0, az = 1, bx = 1, bz = 1; break;
                case 4: ha = h00, hb = h01, ax = 0, az = 0, bx = 0, bz = 1; break;
                default: ha = h11, hb = h10, ax = 1, az = 1, bx = 1, bz = 0; break;
            }
            setP(p[0], x + ax, y + ha, z + az);
            setP(p[1], x + bx, y + hb, z + bz);
            setP(p[2], x + bx, y, z + bz);
            setP(p[3], x + ax, y, z + az);
            hv[0] = ha, hv[1] = hb, hv[2] = hv[3] = 0;
        }
        if (out.count + 6 > out.capacity) {
            out.overflow = true;
            return;
        }
        float tu = (float)((tex & 15) * 16), tv = (float)((tex >> 4) * 16);
        MeshVertex v[4];
        for (int i = 0; i < 4; ++i) {
            float u, vv;
            if (f == 1) {
                u = (i == 1 || i == 2) ? 16.0f : 0.0f;
                vv = (i >= 2) ? 16.0f : 0.0f;
            } else {
                u = (i == 1 || i == 2) ? 16.0f : 0.0f;
                vv = (1.0f - hv[i]) * 16.0f;
            }
            v[i].u = (uint16_t)((tu + u) * 128.0f + (u > 8 ? -2 : 2));
            v[i].v = (uint16_t)((tv + vv) * 128.0f + (vv > 8 ? -2 : 2));
            v[i].color = color;
            v[i].x = toPos(p[i][0]);
            v[i].y = toPos(p[i][1]);
            v[i].z = toPos(p[i][2]);
        }
        MeshVertex* o = out.data + out.count;
        o[0] = v[0];
        o[1] = v[1];
        o[2] = v[2];
        o[3] = v[0];
        o[4] = v[2];
        o[5] = v[3];
        out.count += 6;
    }
}

void renderCross(Ctx& c, uint8_t id, int meta, int lx, int ly, int lz) {
    int wx = c.ox + lx, wy = c.oy + ly, wz = c.oz + lz;
    int tex = blockTexture(id, meta, 2);
    uint16_t color = packColor(blockTint(&c.w, id, meta, 2, wx, wz), lightAt(c.w, wx, wy, wz));
    const float a = 0.5f - 0.45f, b = 0.5f + 0.45f;
    float x = (float)lx, y = (float)ly, z = (float)lz;
    Quad q;
    setUV(q, tex, 0, 0, 16, 16);
    setP(q.p[0], x + a, y + 1, z + a);
    setP(q.p[1], x + b, y + 1, z + b);
    setP(q.p[2], x + b, y, z + b);
    setP(q.p[3], x + a, y, z + a);
    emit(c.opaque, q, color, false);
    setP(q.p[0], x + a, y + 1, z + b);
    setP(q.p[1], x + b, y + 1, z + a);
    setP(q.p[2], x + b, y, z + a);
    setP(q.p[3], x + a, y, z + b);
    emit(c.opaque, q, color, false);
}

// RenderBlocks.renderTorchAtAngle: four sheets and the cap, the foot shifted by (dx, dz).
void drawTorch(MeshBuffer& out, int tex, uint16_t color, float bx, float by, float bz, float dx, float dz) {
    const float s = 1.0f / 16.0f;
    float cx = bx + 0.5f, cz = bz + 0.5f;
    float top = by + 1.0f, bot = by;
    Quad q;
    setUV(q, tex, 0, 0, 16, 16);
    // west
    setP(q.p[0], cx - s, top, cz - 0.5f);
    setP(q.p[1], cx - s, top, cz + 0.5f);
    setP(q.p[2], cx - s + dx, bot, cz + 0.5f + dz);
    setP(q.p[3], cx - s + dx, bot, cz - 0.5f + dz);
    emit(out, q, color, false);
    // east
    setP(q.p[0], cx + s, top, cz + 0.5f);
    setP(q.p[1], cx + s, top, cz - 0.5f);
    setP(q.p[2], cx + s + dx, bot, cz - 0.5f + dz);
    setP(q.p[3], cx + s + dx, bot, cz + 0.5f + dz);
    emit(out, q, color, false);
    // north
    setP(q.p[0], cx + 0.5f, top, cz - s);
    setP(q.p[1], cx - 0.5f, top, cz - s);
    setP(q.p[2], cx - 0.5f + dx, bot, cz - s + dz);
    setP(q.p[3], cx + 0.5f + dx, bot, cz - s + dz);
    emit(out, q, color, false);
    // south
    setP(q.p[0], cx - 0.5f, top, cz + s);
    setP(q.p[1], cx + 0.5f, top, cz + s);
    setP(q.p[2], cx + 0.5f + dx, bot, cz + s + dz);
    setP(q.p[3], cx - 0.5f + dx, bot, cz + s + dz);
    emit(out, q, color, false);
    // cap
    float capY = by + 10.0f / 16.0f;
    float tx = cx + dx * (6.0f / 16.0f), tz = cz + dz * (6.0f / 16.0f);
    setUV(q, tex, 7, 6, 9, 8);
    setP(q.p[0], tx - s, capY, tz - s);
    setP(q.p[1], tx + s, capY, tz - s);
    setP(q.p[2], tx + s, capY, tz + s);
    setP(q.p[3], tx - s, capY, tz + s);
    emit(out, q, color, false);
}

void renderTorch(Ctx& c, uint8_t id, int meta, int lx, int ly, int lz) {
    int wx = c.ox + lx, wy = c.oy + ly, wz = c.oz + lz;
    float bx = (float)lx, by = (float)ly, bz = (float)lz, dx = 0, dz = 0;
    switch (meta) {
        case 1: bx -= 0.1f; by += 0.2f; dx = -0.4f; break;
        case 2: bx += 0.1f; by += 0.2f; dx = 0.4f; break;
        case 3: bz -= 0.1f; by += 0.2f; dz = -0.4f; break;
        case 4: bz += 0.1f; by += 0.2f; dz = 0.4f; break;
        default: break;
    }
    float l = g_blocks[id].emission > 0 ? 1.0f : lightAt(c.w, wx, wy, wz);
    drawTorch(c.opaque, blockTexture(id, meta, 0), packColor(0xFFFFFF, l), bx, by, bz, dx, dz);
}

// Axis-aligned box in block-local units; faces on the block boundary are culled by opaque neighbors.
void renderBox(Ctx& c, uint8_t id, int meta, int lx, int ly, int lz, float x0, float y0, float z0, float x1, float y1,
               float z1, int texOverride = -1, int faceMask = 63) {
    int wx = c.ox + lx, wy = c.oy + ly, wz = c.oz + lz;
    const float bounds[6] = {y0, 1 - y1, z0, 1 - z1, x0, 1 - x1};
    for (int f = 0; f < 6; ++f) {
        if (!(faceMask & (1 << f))) continue;
        int nx = wx + FACE_DIR[f][0], ny = wy + FACE_DIR[f][1], nz = wz + FACE_DIR[f][2];
        bool edge = bounds[f] < 0.001f;
        if (edge && !c.noCull && blockIsOpaque(c.w.get(nx, ny, nz))) continue;
        int tex = texOverride >= 0 ? texOverride : blockTexture(id, meta, f);
        Quad q;
        boxFace(q, f, (float)lx, (float)ly, (float)lz, x0, y0, z0, x1, y1, z1, tex);
        float l = edge ? lightMax(c.w, wx, wy, wz, nx, ny, nz) : lightAt(c.w, wx, wy, wz);
        emit(c.opaque, q, packColor(0xFFFFFF, l * FACE_SHADE[f]), false);
    }
}

// BlockPortal: a thin pane across the frame; only its two broad faces are visible inside a frame.
void renderPortal(Ctx& c, int lx, int ly, int lz) {
    int wx = c.ox + lx, wy = c.oy + ly, wz = c.oz + lz;
    bool alongX = c.w.get(wx - 1, wy, wz) == PORTAL || c.w.get(wx + 1, wy, wz) == PORTAL;
    float x0 = alongX ? 0 : 0.375f, x1 = alongX ? 1 : 0.625f, z0 = alongX ? 0.375f : 0, z1 = alongX ? 0.625f : 1;
    float l = lightAt(c.w, wx, wy, wz);
    for (int f = alongX ? 2 : 4, e = f + 2; f < e; ++f) {
        Quad q;
        boxFace(q, f, (float)lx, (float)ly, (float)lz, x0, 0, z0, x1, 1, z1, 14);
        emit(c.trans, q, packColor(0xFFFFFF, l * FACE_SHADE[f]), false);
    }
}

void renderLadder(Ctx& c, int meta, int lx, int ly, int lz) {
    int wx = c.ox + lx, wy = c.oy + ly, wz = c.oz + lz;
    uint16_t color = packColor(0xFFFFFF, lightAt(c.w, wx, wy, wz));
    const float d = 0.05f;
    float x = (float)lx, y = (float)ly, z = (float)lz;
    Quad q;
    setUV(q, 83, 0, 0, 16, 16);
    switch (meta) {
        case 5:
            setP(q.p[0], x + d, y + 1, z + 1), setP(q.p[1], x + d, y + 1, z), setP(q.p[2], x + d, y, z),
                setP(q.p[3], x + d, y, z + 1);
            break;
        case 4:
            setP(q.p[0], x + 1 - d, y + 1, z), setP(q.p[1], x + 1 - d, y + 1, z + 1), setP(q.p[2], x + 1 - d, y, z + 1),
                setP(q.p[3], x + 1 - d, y, z);
            break;
        case 3:
            setP(q.p[0], x, y + 1, z + d), setP(q.p[1], x + 1, y + 1, z + d), setP(q.p[2], x + 1, y, z + d),
                setP(q.p[3], x, y, z + d);
            break;
        default:
            setP(q.p[0], x + 1, y + 1, z + 1 - d), setP(q.p[1], x, y + 1, z + 1 - d), setP(q.p[2], x, y, z + 1 - d),
                setP(q.p[3], x + 1, y, z + 1 - d);
            break;
    }
    emit(c.opaque, q, color, false);
}

// RenderBlocks.renderBlockCropsImpl: four planes at 1/4 and 3/4, sunk by 1/16.
void renderCrops(Ctx& c, int meta, int lx, int ly, int lz) {
    int wx = c.ox + lx, wy = c.oy + ly, wz = c.oz + lz;
    uint16_t color = packColor(0xFFFFFF, lightAt(c.w, wx, wy, wz));
    int tex = blockTexture(CROPS, meta, 2);
    float x = (float)lx, y = (float)ly - 1.0f / 16.0f, z = (float)lz;
    Quad q;
    setUV(q, tex, 0, 0, 16, 16);
    for (int i = 0; i < 2; ++i) {
        float o = i ? 0.75f : 0.25f;
        setP(q.p[0], x + o, y + 1, z), setP(q.p[1], x + o, y + 1, z + 1), setP(q.p[2], x + o, y, z + 1),
            setP(q.p[3], x + o, y, z);
        emit(c.opaque, q, color, false);
        setP(q.p[0], x, y + 1, z + o), setP(q.p[1], x + 1, y + 1, z + o), setP(q.p[2], x + 1, y, z + o),
            setP(q.p[3], x, y, z + o);
        emit(c.opaque, q, color, false);
    }
}

void renderFence(Ctx& c, int lx, int ly, int lz) {
    int wx = c.ox + lx, wy = c.oy + ly, wz = c.oz + lz;
    const float a = 0.375f, b = 0.625f;
    renderBox(c, FENCE, 0, lx, ly, lz, a, 0, a, b, 1, b);
    bool px = c.w.get(wx + 1, wy, wz) == FENCE, nx = c.w.get(wx - 1, wy, wz) == FENCE;
    bool pz = c.w.get(wx, wy, wz + 1) == FENCE, nz = c.w.get(wx, wy, wz - 1) == FENCE;
    const float lo = 0.4375f, hi = 0.5625f;
    for (int bar = 0; bar < 2; ++bar) {
        float y0 = bar ? 0.375f : 0.75f, y1 = bar ? 0.5625f : 0.9375f;
        if (px) renderBox(c, FENCE, 0, lx, ly, lz, b, y0, lo, 1, y1, hi);
        if (nx) renderBox(c, FENCE, 0, lx, ly, lz, 0, y0, lo, a, y1, hi);
        if (pz) renderBox(c, FENCE, 0, lx, ly, lz, lo, y0, b, hi, y1, 1);
        if (nz) renderBox(c, FENCE, 0, lx, ly, lz, lo, y0, 0, hi, y1, a);
    }
}

void renderStairs(Ctx& c, uint8_t id, int meta, int lx, int ly, int lz) {
    renderBox(c, id, meta, lx, ly, lz, 0, 0, 0, 1, 0.5f, 1);
    switch (meta & 3) {
        case 0: renderBox(c, id, meta, lx, ly, lz, 0.5f, 0.5f, 0, 1, 1, 1); break;
        case 1: renderBox(c, id, meta, lx, ly, lz, 0, 0.5f, 0, 0.5f, 1, 1); break;
        case 2: renderBox(c, id, meta, lx, ly, lz, 0, 0.5f, 0.5f, 1, 1, 1); break;
        default: renderBox(c, id, meta, lx, ly, lz, 0, 0.5f, 0, 1, 1, 0.5f); break;
    }
}

void renderDoor(Ctx& c, uint8_t id, int meta, int lx, int ly, int lz) {
    int wx = c.ox + lx, wy = c.oy + ly, wz = c.oz + lz;
    int m = (meta & 8) ? c.w.getMeta(wx, wy - 1, wz) : meta;
    const float t = 0.1875f;
    int tex = blockTexture(id, meta, 2);
    switch (doorState(m & 7)) {
        case 0: renderBox(c, id, meta, lx, ly, lz, 0, 0, 0, 1, 1, t, tex); break;
        case 1: renderBox(c, id, meta, lx, ly, lz, 1 - t, 0, 0, 1, 1, 1, tex); break;
        case 2: renderBox(c, id, meta, lx, ly, lz, 0, 0, 1 - t, 1, 1, 1, tex); break;
        default: renderBox(c, id, meta, lx, ly, lz, 0, 0, 0, t, 1, 1, tex); break;
    }
}

inline bool fireFeeds(uint8_t id) { return blockFireSpread(id) > 0; }

// RenderBlocks.renderBlockFire: slanted flame sheets on the ground, or leaning on burning sides.
void renderFire(Ctx& c, int lx, int ly, int lz) {
    int wx = c.ox + lx, wy = c.oy + ly, wz = c.oz + lz;
    uint16_t color = packColor(0xFFFFFF, lightAt(c.w, wx, wy, wz));
    float x = (float)lx, y = (float)ly, z = (float)lz;
    const float H = 1.4f;
    Quad q;
    // Top edge (a) and bottom edge (b) of a sheet, each as two points.
    auto sheet = [&](int tex, float ax0, float ay0, float az0, float ax1, float ay1, float az1, float bx0, float by0,
                     float bz0, float bx1, float by1, float bz1) {
        setUV(q, tex, 0, 0, 16, 16);
        setP(q.p[0], x + ax0, y + ay0, z + az0);
        setP(q.p[1], x + ax1, y + ay1, z + az1);
        setP(q.p[2], x + bx1, y + by1, z + bz1);
        setP(q.p[3], x + bx0, y + by0, z + bz0);
        emit(c.opaque, q, color, true);
    };
    uint8_t below = c.w.get(wx, wy - 1, wz);
    int t0 = ((wx + wy + wz) & 1) ? 47 : 31, t1 = t0 == 31 ? 47 : 31;
    if (blockIsNormalCube(below) || fireFeeds(below)) {
        sheet(31, 0.2f, H, 1, 0.2f, H, 0, 0.7f, 0, 1, 0.7f, 0, 0);
        sheet(31, 0.8f, H, 0, 0.8f, H, 1, 0.3f, 0, 0, 0.3f, 0, 1);
        sheet(47, 1, H, 0.8f, 0, H, 0.8f, 1, 0, 0.3f, 0, 0, 0.3f);
        sheet(47, 0, H, 0.2f, 1, H, 0.2f, 0, 0, 0.7f, 1, 0, 0.7f);
        sheet(47, 0.1f, H, 0, 0.1f, H, 1, 0, 0, 0, 0, 0, 1);
        sheet(47, 0.9f, H, 1, 0.9f, H, 0, 1, 0, 1, 1, 0, 0);
        sheet(31, 0, H, 0.9f, 1, H, 0.9f, 0, 0, 1, 1, 0, 1);
        sheet(31, 1, H, 0.1f, 0, H, 0.1f, 1, 0, 0, 0, 0, 0);
        return;
    }
    const float s = 0.2f, o = 1.0f / 16.0f;
    if (fireFeeds(c.w.get(wx - 1, wy, wz))) sheet(t0, s, H + o, 1, s, H + o, 0, 0, o, 1, 0, o, 0);
    if (fireFeeds(c.w.get(wx + 1, wy, wz))) sheet(t0, 1 - s, H + o, 0, 1 - s, H + o, 1, 1, o, 0, 1, o, 1);
    if (fireFeeds(c.w.get(wx, wy, wz - 1))) sheet(t1, 0, H + o, s, 1, H + o, s, 0, o, 0, 1, o, 0);
    if (fireFeeds(c.w.get(wx, wy, wz + 1))) sheet(t1, 1, H + o, 1 - s, 0, H + o, 1 - s, 1, o, 1, 0, o, 1);
    if (fireFeeds(c.w.get(wx, wy + 1, wz))) {
        sheet(t0, 0, 0.8f, 0, 0, 0.8f, 1, 1, 1, 0, 1, 1, 1);
        sheet(t1, 0, 1, 0, 0, 1, 1, 1, 0.8f, 0, 1, 0.8f, 1);
    }
}

// RenderBlocks.renderBlockBed: the top is rotated by direction, the bottom sits on the legs
// and the side facing the other half is skipped.
void renderBed(Ctx& c, int meta, int lx, int ly, int lz) {
    int wx = c.ox + lx, wy = c.oy + ly, wz = c.oz + lz;
    int dir = meta & 3;
    bool head = (meta & 8) != 0;
    static const int TOWARD_HEAD[4] = {3, 4, 2, 5};
    static const int OPPOSITE[4] = {2, 3, 0, 1};
    static const int FLIPPED[4] = {5, 3, 4, 2};
    int hidden = TOWARD_HEAD[head ? OPPOSITE[dir] : dir];
    const float h = 0.5625f;
    float own = lightAt(c.w, wx, wy, wz);
    float x = (float)lx, y = (float)ly, z = (float)lz;

    Quad q;
    boxFace(q, 0, x, y + 0.1875f, z, 0, 0, 0, 1, 1, 1, 4);
    emit(c.opaque, q, packColor(0xFFFFFF, own * FACE_SHADE[0]), false);

    // Corner order X0Z0, X1Z0, X1Z1, X0Z1 mapped to UV corners (0,0) (1,0) (1,1) (0,1).
    static const uint8_t TOP_UV[4][4] = {{0, 3, 2, 1}, {1, 0, 3, 2}, {2, 1, 0, 3}, {3, 2, 1, 0}};
    boxFace(q, 1, x, y, z, 0, 0, 0, 1, h, 1, blockTexture(BED, meta, 1));
    if (c.opaque.count + 6 <= c.opaque.capacity) {
        const int us[4] = {q.u0, q.u1, q.u1, q.u0}, vs[4] = {q.v0, q.v0, q.v1, q.v1};
        uint16_t color = packColor(0xFFFFFF, lightAt(c.w, wx, wy + 1, wz));
        MeshVertex v[4];
        for (int i = 0; i < 4; ++i) {
            int k = TOP_UV[dir][i];
            v[i].u = (uint16_t)us[k];
            v[i].v = (uint16_t)vs[k];
            v[i].color = color;
            v[i].x = toPos(q.p[i][0]);
            v[i].y = toPos(q.p[i][1]);
            v[i].z = toPos(q.p[i][2]);
        }
        MeshVertex* o = c.opaque.data + c.opaque.count;
        o[0] = v[0];
        o[1] = v[1];
        o[2] = v[2];
        o[3] = v[0];
        o[4] = v[2];
        o[5] = v[3];
        c.opaque.count += 6;
    } else {
        c.opaque.overflow = true;
    }

    for (int f = 2; f < 6; ++f) {
        if (f == hidden) continue;
        int nx = wx + FACE_DIR[f][0], nz = wz + FACE_DIR[f][2];
        if (blockIsOpaque(c.w.get(nx, wy, nz))) continue;
        boxFace(q, f, x, y, z, 0, 0, 0, 1, h, 1, blockTexture(BED, meta, f));
        if (f == FLIPPED[dir]) {
            int t = q.u0;
            q.u0 = q.u1;
            q.u1 = t;
        }
        emit(c.opaque, q, packColor(0xFFFFFF, lightMax(c.w, wx, wy, wz, nx, wy, nz) * FACE_SHADE[f]), false);
    }
}

// BlockRedstoneWire.isPowerProviderOrWire, with wires always counted as power providers.
bool wireLinks(const World& w, int x, int y, int z, int dir) {
    uint8_t id = w.get(x, y, z);
    switch (id) {
        case REDSTONE_WIRE: case REDSTONE_TORCH_OFF: case REDSTONE_TORCH_ON: case LEVER: case STONE_BUTTON:
        case STONE_PLATE: case WOOD_PLATE: case DETECTOR_RAIL:
            return true;
        default: break;
    }
    if (!blockIsRepeater(id)) return false;
    static const int REMAP[4] = {2, 3, 0, 1};
    return dir == REMAP[w.getMeta(x, y, z) & 3];
}

// RenderBlocks.renderBlockRedstoneWire: a cross clipped to the connected sides or a straight line,
// tinted by the power level, plus lines running up the sides of blocks.
void renderWire(Ctx& c, int meta, int lx, int ly, int lz) {
    const World& w = c.w;
    int x = c.ox + lx, y = c.oy + ly, z = c.oz + lz;
    auto cube = [&](int ax, int ay, int az) { return blockIsNormalCube(w.get(ax, ay, az)); };
    bool cw = wireLinks(w, x - 1, y, z, 1) || (!cube(x - 1, y, z) && wireLinks(w, x - 1, y - 1, z, -1));
    bool ce = wireLinks(w, x + 1, y, z, 3) || (!cube(x + 1, y, z) && wireLinks(w, x + 1, y - 1, z, -1));
    bool cn = wireLinks(w, x, y, z - 1, 2) || (!cube(x, y, z - 1) && wireLinks(w, x, y - 1, z - 1, -1));
    bool cs = wireLinks(w, x, y, z + 1, 0) || (!cube(x, y, z + 1) && wireLinks(w, x, y - 1, z + 1, -1));
    bool open = !cube(x, y + 1, z);
    if (open) {
        if (cube(x - 1, y, z) && wireLinks(w, x - 1, y + 1, z, -1)) cw = true;
        if (cube(x + 1, y, z) && wireLinks(w, x + 1, y + 1, z, -1)) ce = true;
        if (cube(x, y, z - 1) && wireLinks(w, x, y + 1, z - 1, -1)) cn = true;
        if (cube(x, y, z + 1) && wireLinks(w, x, y + 1, z + 1, -1)) cs = true;
    }
    float p = meta / 15.0f;
    float r = meta == 0 ? 0.3f : p * 0.6f + 0.4f;
    float g = p * p * 0.7f - 0.5f, b = p * p * 0.6f - 0.7f;
    if (g < 0) g = 0;
    if (b < 0) b = 0;
    uint32_t rgb = ((uint32_t)(r * 255) << 16) | ((uint32_t)(g * 255) << 8) | (uint32_t)(b * 255);
    uint16_t color = packColor(rgb, lightAt(w, x, y, z));

    int mode = 0;
    if ((cw || ce) && !cn && !cs) mode = 1;
    if ((cn || cs) && !ce && !cw) mode = 2;
    float x0 = 0, x1 = 1, z0 = 0, z1 = 1;
    const float k = 5.0f / 16.0f;
    if (mode == 0 && (cw || ce || cn || cs)) {
        if (!cw) x0 += k;
        if (!ce) x1 -= k;
        if (!cn) z0 += k;
        if (!cs) z1 -= k;
    }
    int tex = mode == 0 ? 164 : 165;
    float fx = (float)lx, fy = ly + 1.0f / 64.0f, fz = (float)lz;
    float pts[4][3] = {{fx + x0, fy, fz + z0}, {fx + x1, fy, fz + z0}, {fx + x1, fy, fz + z1}, {fx + x0, fy, fz + z1}};
    float uv[4][2];
    const float cx[4] = {x0, x1, x1, x0}, cz[4] = {z0, z0, z1, z1};
    for (int i = 0; i < 4; ++i) {
        uv[i][0] = (mode == 2 ? cz[i] : cx[i]) * 16;
        uv[i][1] = (mode == 2 ? cx[i] : cz[i]) * 16;
    }
    emitUV(c.opaque, pts, uv, tex, color, false);

    if (!open) return;
    const float d = 1.0f / 64.0f, h = 1.021875f;
    auto wall = [&](float ax, float az, float bx, float bz) {
        float q[4][3] = {{fx + ax, ly + h, fz + az}, {fx + bx, ly + h, fz + bz}, {fx + bx, (float)ly, fz + bz},
                         {fx + ax, (float)ly, fz + az}};
        float t[4][2] = {{16, 0}, {16, 16}, {0, 16}, {0, 0}};
        emitUV(c.opaque, q, t, 165, color, true);
    };
    if (cube(x - 1, y, z) && w.get(x - 1, y + 1, z) == REDSTONE_WIRE) wall(d, 0, d, 1);
    if (cube(x + 1, y, z) && w.get(x + 1, y + 1, z) == REDSTONE_WIRE) wall(1 - d, 1, 1 - d, 0);
    if (cube(x, y, z - 1) && w.get(x, y + 1, z - 1) == REDSTONE_WIRE) wall(1, d, 0, d);
    if (cube(x, y, z + 1) && w.get(x, y + 1, z + 1) == REDSTONE_WIRE) wall(0, 1 - d, 1, 1 - d);
}

// RenderBlocks.renderBlockMinecartTrack: one double-sided quad 1/16 above the floor, corners raised on slopes.
void renderRail(Ctx& c, uint8_t id, int meta, int lx, int ly, int lz) {
    int tex = blockTexture(id, meta, 0);
    if (id != RAIL) meta &= 7;
    float x0 = lx + 1.0f, x1 = lx + 1.0f, x2 = (float)lx, x3 = (float)lx;
    float z0 = (float)lz, z1 = lz + 1.0f, z2 = lz + 1.0f, z3 = (float)lz;
    float y0 = ly + 0.0625f, y1 = y0, y2 = y0, y3 = y0;
    if (meta == 1 || meta == 2 || meta == 3 || meta == 7) {
        x0 = x3 = lx + 1.0f;
        x1 = x2 = (float)lx;
        z0 = z1 = lz + 1.0f;
        z2 = z3 = (float)lz;
    } else if (meta == 8) {
        x0 = x1 = (float)lx;
        x2 = x3 = lx + 1.0f;
        z0 = z3 = lz + 1.0f;
        z1 = z2 = (float)lz;
    } else if (meta == 9) {
        x0 = x3 = (float)lx;
        x1 = x2 = lx + 1.0f;
        z0 = z1 = (float)lz;
        z2 = z3 = lz + 1.0f;
    }
    if (meta == 2 || meta == 4) {
        y0 += 1, y3 += 1;
    } else if (meta == 3 || meta == 5) {
        y1 += 1, y2 += 1;
    }
    float p[4][3] = {{x0, y0, z0}, {x1, y1, z1}, {x2, y2, z2}, {x3, y3, z3}};
    float uv[4][2] = {{16, 0}, {16, 16}, {0, 16}, {0, 0}};
    uint16_t color = packColor(0xFFFFFF, lightAt(c.w, c.ox + lx, c.oy + ly, c.oz + lz));
    emitUV(c.opaque, p, uv, tex, color, true);
}

// RenderBlocks.renderBlockLever: a cobblestone base and the tilted handle.
void renderLever(Ctx& c, int meta, int lx, int ly, int lz) {
    int dir = meta & 7;
    bool on = (meta & 8) != 0;
    const float a = 0.25f, b = 0.1875f, t = 0.1875f;
    switch (dir) {
        case 6: renderBox(c, LEVER, meta, lx, ly, lz, 0.5f - a, 0, 0.5f - b, 0.5f + a, t, 0.5f + b, 16); break;
        case 4: renderBox(c, LEVER, meta, lx, ly, lz, 0.5f - b, 0.5f - a, 1 - t, 0.5f + b, 0.5f + a, 1, 16); break;
        case 3: renderBox(c, LEVER, meta, lx, ly, lz, 0.5f - b, 0.5f - a, 0, 0.5f + b, 0.5f + a, t, 16); break;
        case 2: renderBox(c, LEVER, meta, lx, ly, lz, 1 - t, 0.5f - a, 0.5f - b, 1, 0.5f + a, 0.5f + b, 16); break;
        case 1: renderBox(c, LEVER, meta, lx, ly, lz, 0, 0.5f - a, 0.5f - b, t, 0.5f + a, 0.5f + b, 16); break;
        default: renderBox(c, LEVER, meta, lx, ly, lz, 0.5f - b, 0, 0.5f - a, 0.5f + b, t, 0.5f + a, 16); break;
    }
    const float s = 0.0625f, h = 0.625f, PI = 3.14159265f;
    float v[8][3] = {{-s, 0, -s}, {s, 0, -s}, {s, 0, s}, {-s, 0, s}, {-s, h, -s}, {s, h, -s}, {s, h, s}, {-s, h, s}};
    auto rotX = [](float* p, float ang) {
        float co = std::cos(ang), si = std::sin(ang), y = p[1] * co + p[2] * si, z = p[2] * co - p[1] * si;
        p[1] = y, p[2] = z;
    };
    auto rotY = [](float* p, float ang) {
        float co = std::cos(ang), si = std::sin(ang), x = p[0] * co + p[2] * si, z = p[2] * co - p[0] * si;
        p[0] = x, p[2] = z;
    };
    for (auto& p : v) {
        if (on) {
            p[2] -= 0.0625f;
            rotX(p, PI * 2 / 9);
        } else {
            p[2] += 0.0625f;
            rotX(p, -PI * 2 / 9);
        }
        if (dir == 6) rotY(p, PI / 2);
        if (dir >= 1 && dir < 5) {
            p[1] -= 0.375f;
            rotX(p, PI / 2);
            if (dir == 3) rotY(p, PI);
            if (dir == 2) rotY(p, PI / 2);
            if (dir == 1) rotY(p, -PI / 2);
            p[0] += lx + 0.5f, p[1] += ly + 0.5f, p[2] += lz + 0.5f;
        } else {
            p[0] += lx + 0.5f, p[1] += ly + 0.125f, p[2] += lz + 0.5f;
        }
    }
    uint16_t color = packColor(0xFFFFFF, lightAt(c.w, c.ox + lx, c.oy + ly, c.oz + lz));
    static const uint8_t FACES[6][4] = {{0, 1, 2, 3}, {7, 6, 5, 4}, {1, 0, 4, 5},
                                        {2, 1, 5, 6}, {3, 2, 6, 7}, {0, 3, 7, 4}};
    for (int i = 0; i < 6; ++i) {
        float v1 = i < 2 ? 8.0f : 16.0f;
        float q[4][3], uv[4][2] = {{7, v1}, {9, v1}, {9, 6}, {7, 6}};
        for (int j = 0; j < 4; ++j) setP(q[j], v[FACES[i][j]][0], v[FACES[i][j]][1], v[FACES[i][j]][2]);
        emitUV(c.opaque, q, uv, 96, color, true);
    }
}

// RenderBlocks.renderBlockRepeater: the slab sides, two torches (the second one moved by the delay)
// and the top turned to the facing.
void renderRepeater(Ctx& c, uint8_t id, int meta, int lx, int ly, int lz) {
    renderBox(c, id, meta, lx, ly, lz, 0, 0, 0, 1, 0.125f, 1, -1, 0x3C);
    int dir = meta & 3, delay = (meta >> 2) & 3;
    static const float OFF[4] = {-0.0625f, 0.0625f, 0.1875f, 0.3125f};
    float ax = 0, az = 0, bx = 0, bz = 0;
    switch (dir) {
        case 0: bz = -0.3125f, az = OFF[delay]; break;
        case 1: bx = 0.3125f, ax = -OFF[delay]; break;
        case 2: bz = 0.3125f, az = -OFF[delay]; break;
        default: bx = -0.3125f, ax = OFF[delay]; break;
    }
    float l = lightAt(c.w, c.ox + lx, c.oy + ly, c.oz + lz);
    if (g_blocks[id].emission > 0) l = (l + 1) * 0.5f;
    uint16_t color = packColor(0xFFFFFF, l);
    int torch = blockTexture(id, meta, 0);
    drawTorch(c.opaque, torch, color, lx + ax, ly - 0.1875f, lz + az, 0, 0);
    drawTorch(c.opaque, torch, color, lx + bx, ly - 0.1875f, lz + bz, 0, 0);
    float y = ly + 0.125f;
    float q[4][3] = {{(float)lx, y, (float)lz}, {lx + 1.0f, y, (float)lz}, {lx + 1.0f, y, lz + 1.0f}, {(float)lx, y, lz + 1.0f}};
    const float cx[4] = {0, 1, 1, 0}, cz[4] = {0, 0, 1, 1};
    float uv[4][2];
    for (int i = 0; i < 4; ++i) {
        float u, v;
        switch (dir) {
            case 0: u = cx[i], v = cz[i]; break;
            case 1: u = cz[i], v = 1 - cx[i]; break;
            case 2: u = 1 - cx[i], v = 1 - cz[i]; break;
            default: u = 1 - cz[i], v = cx[i]; break;
        }
        uv[i][0] = u * 16, uv[i][1] = v * 16;
    }
    emitUV(c.opaque, q, uv, blockTexture(id, meta, 1), color, false);
}

// Face of a piston part with the texture turned so its top edge points along `up`
// (the uvRotate tables of RenderBlocks.renderPistonBase).
void orientedFace(Ctx& c, int f, int lx, int ly, int lz, const AABB& b, int up, int tex, uint16_t color) {
    Quad q;
    boxFace(q, f, (float)lx, (float)ly, (float)lz, b.x0, b.y0, b.z0, b.x1, b.y1, b.z1, tex);
    if ((f >> 1) == (up >> 1)) {
        emit(c.opaque, q, color, false);
        return;
    }
    const int* U = FACE_DIR[up];
    const int* N = FACE_DIR[f];
    const int R[3] = {U[1] * N[2] - U[2] * N[1], U[2] * N[0] - U[0] * N[2], U[0] * N[1] - U[1] * N[0]};
    float uv[4][2];
    for (int i = 0; i < 4; ++i) {
        float rx = q.p[i][0] - lx - 0.5f, ry = q.p[i][1] - ly - 0.5f, rz = q.p[i][2] - lz - 0.5f;
        uv[i][0] = (rx * R[0] + ry * R[1] + rz * R[2] + 0.5f) * 16;
        uv[i][1] = (0.5f - (rx * U[0] + ry * U[1] + rz * U[2])) * 16;
    }
    emitUV(c.opaque, q.p, uv, tex, color, false);
}

// The faces of box b, culled against opaque neighbors where they lie on the block boundary.
template <typename TexFn>
void orientedBox(Ctx& c, int lx, int ly, int lz, const AABB& b, int up, TexFn tex) {
    int wx = c.ox + lx, wy = c.oy + ly, wz = c.oz + lz;
    const float bounds[6] = {b.y0, 1 - b.y1, b.z0, 1 - b.z1, b.x0, 1 - b.x1};
    for (int f = 0; f < 6; ++f) {
        int nx = wx + FACE_DIR[f][0], ny = wy + FACE_DIR[f][1], nz = wz + FACE_DIR[f][2];
        bool edge = bounds[f] < 0.001f;
        if (edge && !c.noCull && blockIsOpaque(c.w.get(nx, ny, nz))) continue;
        float l = edge ? lightMax(c.w, wx, wy, wz, nx, ny, nz) : lightAt(c.w, wx, wy, wz);
        orientedFace(c, f, lx, ly, lz, b, up, tex(f), packColor(0xFFFFFF, l * FACE_SHADE[f]));
    }
}

// RenderBlocks.renderPistonBase: shortened to 3/4 with the inner face showing while extended.
void renderPiston(Ctx& c, uint8_t id, int meta, int lx, int ly, int lz) {
    int dir = meta & 7;
    if (dir > 5) dir = 0;
    bool ext = (meta & 8) || (c.pistonFlags & PISTON_FORCE_EXTENDED);
    AABB b = {0, 0, 0, 1, 1, 1};
    if (ext) blockShapeBox(PISTON, dir | 8, b);
    int front = ext ? 110 : id == STICKY_PISTON ? 106 : 107;
    orientedBox(c, lx, ly, lz, b, dir, [&](int f) { return f == dir ? front : f == (dir ^ 1) ? 109 : 108; });
}

// RenderBlocks.renderPistonExtension: the head plate and the arm reaching back into the base.
void renderPistonHead(Ctx& c, int meta, int lx, int ly, int lz) {
    int dir = meta & 7;
    if (dir > 5) dir = 0;
    int front = (meta & 8) ? 106 : 107;
    AABB b;
    blockShapeBox(PISTON_HEAD, dir, b);
    orientedBox(c, lx, ly, lz, b, dir, [&](int f) { return f == dir ? front : f == (dir ^ 1) ? 107 : 108; });

    float len = (c.pistonFlags & PISTON_SHORT_ARM) ? 0.5f : 1.0f;
    int axis = dir >> 1;  // 0: y, 1: z, 2: x
    bool positive = dir & 1;
    float plate = positive ? 0.75f : 0.25f;
    float a0 = positive ? plate - len : plate, a1 = positive ? plate : plate + len;
    float lo[3] = {0.375f, 0.375f, 0.375f}, hi[3] = {0.625f, 0.625f, 0.625f};  // x, y, z
    int ai = axis == 0 ? 1 : axis == 1 ? 2 : 0;
    lo[ai] = a0, hi[ai] = a1;
    AABB arm = {lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]};
    float l = lightAt(c.w, c.ox + lx, c.oy + ly, c.oz + lz);
    const int* U = FACE_DIR[dir];
    for (int f = 0; f < 6; ++f) {
        if ((f >> 1) == axis) continue;
        Quad q;
        boxFace(q, f, (float)lx, (float)ly, (float)lz, arm.x0, arm.y0, arm.z0, arm.x1, arm.y1, arm.z1, 108);
        const int* N = FACE_DIR[f];
        const int R[3] = {U[1] * N[2] - U[2] * N[1], U[2] * N[0] - U[0] * N[2], U[0] * N[1] - U[1] * N[0]};
        float uv[4][2];
        for (int i = 0; i < 4; ++i) {
            float rel[3] = {q.p[i][0] - lx, q.p[i][1] - ly, q.p[i][2] - lz};
            float along = rel[ai] - plate;
            uv[i][0] = (along < 0 ? -along : along) * 16;
            float across = (rel[0] - 0.5f) * R[0] + (rel[1] - 0.5f) * R[1] + (rel[2] - 0.5f) * R[2];
            uv[i][1] = (across + 0.125f) * 16;
        }
        emitUV(c.opaque, q.p, uv, 108, packColor(0xFFFFFF, l * FACE_SHADE[f]), false);
    }
}

void renderBlock(Ctx& c, uint8_t id, int meta, int lx, int ly, int lz) {
    switch (g_blocks[id].render) {
        case RENDER_CUBE: renderCube(c, id, meta, lx, ly, lz); break;
        case RENDER_CROSS: renderCross(c, id, meta, lx, ly, lz); break;
        case RENDER_LIQUID: renderLiquid(c, id, lx, ly, lz); break;
        case RENDER_TORCH: renderTorch(c, id, meta, lx, ly, lz); break;
        case RENDER_SLAB: renderPartialBox(c, id, meta, lx, ly, lz, 0.5f); break;
        case RENDER_SNOW: renderPartialBox(c, id, meta, lx, ly, lz, 2.0f / 16.0f); break;
        case RENDER_CACTUS: renderCactus(c, lx, ly, lz); break;
        case RENDER_LADDER: renderLadder(c, meta, lx, ly, lz); break;
        case RENDER_CROPS: renderCrops(c, meta, lx, ly, lz); break;
        case RENDER_FENCE: renderFence(c, lx, ly, lz); break;
        case RENDER_STAIRS: renderStairs(c, id, meta, lx, ly, lz); break;
        case RENDER_DOOR: renderDoor(c, id, meta, lx, ly, lz); break;
        case RENDER_BED: renderBed(c, meta, lx, ly, lz); break;
        case RENDER_FIRE: renderFire(c, lx, ly, lz); break;
        case RENDER_FARMLAND: renderBox(c, id, meta, lx, ly, lz, 0, 0, 0, 1, 15.0f / 16.0f, 1); break;
        case RENDER_WIRE: renderWire(c, meta, lx, ly, lz); break;
        case RENDER_LEVER: renderLever(c, meta, lx, ly, lz); break;
        case RENDER_REPEATER: renderRepeater(c, id, meta, lx, ly, lz); break;
        case RENDER_PISTON: renderPiston(c, id, meta, lx, ly, lz); break;
        case RENDER_PISTON_HEAD: renderPistonHead(c, meta, lx, ly, lz); break;
        case RENDER_RAIL: renderRail(c, id, meta, lx, ly, lz); break;
        case RENDER_PORTAL: renderPortal(c, lx, ly, lz); break;
        case RENDER_CAKE:
        case RENDER_TRAPDOOR:
        case RENDER_SHAPE: {
            AABB b;
            blockShapeBox(id, meta, b);
            renderBox(c, id, meta, lx, ly, lz, b.x0, b.y0, b.z0, b.x1, b.y1, b.z1);
            break;
        }
        default: break;
    }
}

uint32_t sampleMap(const uint8_t* map, float t, float h) {
    if (!map) return 0x7FB238;
    h *= t;
    int i = (int)((1.0f - t) * 255.0f);
    int j = (int)((1.0f - h) * 255.0f);
    const uint8_t* p = map + ((j << 8) | i) * 3;
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

}  // namespace

void mesherInit(const ColorMaps& maps) {
    g_maps = maps;
    for (int t = 0; t < 2; ++t) {
        float floor = t ? 0.1f : 0.05f;
        for (int i = 0; i < 16; ++i) {
            float f = 1.0f - (float)i / 15.0f;
            g_brightTables[t][i] = (1.0f - f) / (f * 3.0f + 1.0f) * (1.0f - floor) + floor;
        }
    }
}

uint32_t grassColorAt(const World& w, int x, int z) {
    const Chunk* c = w.chunk(x >> 4, z >> 4);
    if (!c) return sampleMap(g_maps.grass, 0.5f, 0.5f);
    int i = (x & 15) * 16 + (z & 15);
    return sampleMap(g_maps.grass, c->temperature[i] / 255.0f, c->humidity[i] / 255.0f);
}

uint32_t foliageColorAt(const World& w, int x, int z) {
    const Chunk* c = w.chunk(x >> 4, z >> 4);
    if (!c) return sampleMap(g_maps.foliage, 0.5f, 0.5f);
    int i = (x & 15) * 16 + (z & 15);
    return sampleMap(g_maps.foliage, c->temperature[i] / 255.0f, c->humidity[i] / 255.0f);
}

uint32_t blockTint(const World* w, uint8_t id, int meta, int side, int x, int z) {
    switch (id) {
        case GRASS:
            if (side != 1) return 0xFFFFFF;
            return w ? grassColorAt(*w, x, z) : sampleMap(g_maps.grass, 0.5f, 1.0f);
        case LEAVES:
            if ((meta & 3) == 1) return 0x619961;
            if ((meta & 3) == 2) return 0x80A755;
            return w ? foliageColorAt(*w, x, z) : 0x48B518;
        case TALL_GRASS:
            if (meta == 0) return 0xFFFFFF;
            return w ? grassColorAt(*w, x, z) : sampleMap(g_maps.grass, 0.5f, 1.0f);
        default:
            return 0xFFFFFF;
    }
}

void buildSection(const World& w, int cx, int sy, int cz, CubeBuffer& cubes, MeshBuffer& opaque,
                  MeshBuffer& translucent) {
    cubes.reset();
    opaque.reset();
    translucent.reset();
    const Chunk* chunk = w.chunk(cx, cz);
    if (!chunk || !chunk->sec[sy]) return;
    g_bright = g_brightTables[w.dimension < 0 ? 1 : 0];
    const Section& sec = *chunk->sec[sy];
    Ctx c{w, cx * 16, sy * 16, cz * 16, opaque, translucent, cubes};
    for (int lx = 0; lx < 16; ++lx) {
        for (int lz = 0; lz < 16; ++lz) {
            const uint8_t* col = sec.blocks + sectionIndex(lx, 0, lz);
            for (int ly = 0; ly < 16; ++ly) {
                uint8_t id = col[ly];
                if (id == AIR) continue;
                const BlockDef& d = g_blocks[id];
                if (d.render == RENDER_NONE) continue;
                int mi = sectionIndex(lx, ly, lz);
                int meta = (mi & 1) ? (sec.meta[mi >> 1] >> 4) : (sec.meta[mi >> 1] & 15);
                // Most of a section is solid cubes away from the edge. Their neighbors live in this array.
                if (d.render == RENDER_CUBE && (unsigned)(lx - 1) < 14u && (unsigned)(ly - 1) < 14u &&
                    (unsigned)(lz - 1) < 14u) {
                    renderCubeInside(c, sec, id, meta, lx, ly, lz);
                    continue;
                }
                // Submerged liquid inside the section has this liquid on every side, so it has no faces.
                if (d.render == RENDER_LIQUID && (unsigned)(lx - 1) < 14u && (unsigned)(ly - 1) < 14u &&
                    (unsigned)(lz - 1) < 14u) {
                    int i = sectionIndex(lx, ly, lz);
                    static const int STEP[6] = {-1, 1, -16, 16, -256, 256};
                    bool water = blockIsWater(id);
                    bool covered = true;
                    for (int f = 0; f < 6; ++f)
                        if (!fluidSame(sec.blocks[i + STEP[f]], water)) {
                            covered = false;
                            break;
                        }
                    if (covered) continue;
                }
                renderBlock(c, id, meta, lx, ly, lz);
            }
        }
    }
}

void buildBlock(const World& w, int x, int y, int z, uint8_t id, int meta, MeshBuffer& out, int pistonFlags) {
    static CubeBuffer unused;
    out.reset();
    g_bright = g_brightTables[w.dimension < 0 ? 1 : 0];
    Ctx c{w, x, y, z, out, out, unused};
    c.noCull = true;
    c.pistonFlags = pistonFlags;
    if (g_blocks[id].render != RENDER_NONE) renderBlock(c, id, meta & 15, 0, 0, 0);
}
