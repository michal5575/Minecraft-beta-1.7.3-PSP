#include "renderer.h"

#include <pspgu.h>
#include <pspkernel.h>
#include <psputils.h>

#include <malloc.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <utility>

#include "../core/chunkio.h"
#include "../core/jrandom.h"
#include "../core/mat4.h"
#include "../core/visibility.h"
#include "crashlog.h"
#include "gfx.h"

namespace {

constexpr int VTYPE = GU_TEXTURE_16BIT | GU_COLOR_5650 | GU_VERTEX_16BIT | GU_TRANSFORM_3D;
constexpr int CUBE_VTYPE = GU_TEXTURE_8BIT | GU_COLOR_5650 | GU_VERTEX_8BIT | GU_TRANSFORM_3D;
constexpr int SKY_VTYPE = GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D;
constexpr float FOV = 70.0f;
const int DIR[6][3] = {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};

inline int opposite(int f) { return f ^ 1; }

// Near the ocean the 16-bit water mesh locks the GE, including the part of the surface that is not
// touching the camera. That water is redrawn as float triangles, the same path as particles. Faces
// on the camera, and faces that run off the side of the view, are left out.
// The copy has to start on a 16-byte boundary and take a multiple of 16 bytes. Anything else leaves
// the hand's vertices misaligned, and that locks the GE the same way the water mesh did.
bool g_nearLiquid = false;
int g_waterLeft = 0;

uint32_t color565(uint16_t c) {
    int r = (c & 31) * 255 / 31;
    int g = ((c >> 5) & 63) * 255 / 63;
    int b = ((c >> 11) & 31) * 255 / 31;
    return gfx::rgba(r, g, b);
}

void alignGeList() {
    void* p = sceGuGetMemory(16);
    if (!p) return;
    int mis = (int)((uintptr_t)p & 15);
    if (mis) sceGuGetMemory(16 - mis);
}

// Plants, torches and the hillside faces beside the camera live in the 16-bit opaque mesh.
// A quad that crosses the eye locks the GE the same way a water face does. Sections the
// camera is standing in are redrawn without those triangles. Far sections are unchanged.
bool opaqueTriNearEye(const MeshVertex* s, float ox, float oy, float oz, const Camera& cam, float lx, float ly,
                      float lz) {
    float wx[3], wy[3], wz[3];
    for (int k = 0; k < 3; ++k) {
        wx[k] = ox + s[k].x / 1024.0f;
        wy[k] = oy + s[k].y / 1024.0f;
        wz[k] = oz + s[k].z / 1024.0f;
    }
    float minX = wx[0], maxX = wx[0], minY = wy[0], maxY = wy[0], minZ = wz[0], maxZ = wz[0];
    for (int k = 1; k < 3; ++k) {
        if (wx[k] < minX) minX = wx[k];
        if (wx[k] > maxX) maxX = wx[k];
        if (wy[k] < minY) minY = wy[k];
        if (wy[k] > maxY) maxY = wy[k];
        if (wz[k] < minZ) minZ = wz[k];
        if (wz[k] > maxZ) maxZ = wz[k];
    }
    const float pad = 0.25f;
    if (cam.x > minX - pad && cam.x < maxX + pad && cam.y > minY - pad && cam.y < maxY + pad && cam.z > minZ - pad &&
        cam.z < maxZ + pad)
        return true;
    for (int k = 0; k < 3; ++k) {
        float dx = wx[k] - cam.x, dy = wy[k] - cam.y, dz = wz[k] - cam.z;
        float dist2 = dx * dx + dy * dy + dz * dz;
        if (dist2 < 0.45f * 0.45f) return true;
        float depth = dx * lx + dy * ly + dz * lz;
        if (depth < 0.35f && dist2 < 1.2f * 1.2f) return true;
        float lat2 = dist2 - depth * depth;
        if (dist2 < 1.0f * 1.0f && lat2 > depth * depth * 4.0f) return true;
    }
    return false;
}

void drawTrans(const MeshVertex* src, int n, float ox, float oy, float oz, const Camera& cam) {
    if (!src || n <= 0) return;
    if (!g_nearLiquid) {
        sceGuDrawArray(GU_TRIANGLES, VTYPE, n, nullptr, src);
        return;
    }
    if (g_waterLeft < 3) return;
    constexpr float deg = 0.017453292f;
    float cp = std::cos(cam.pitch * deg);
    float lx = -std::sin(cam.yaw * deg) * cp;
    float ly = -std::sin(cam.pitch * deg);
    float lz = std::cos(cam.yaw * deg) * cp;
    auto bad = [&](const MeshVertex& s) {
        float dx = ox + s.x / 1024.0f - cam.x;
        float dy = oy + s.y / 1024.0f - cam.y;
        float dz = oz + s.z / 1024.0f - cam.z;
        float depth = dx * lx + dy * ly + dz * lz;
        if (depth < 0.6f) return true;
        float dist2 = dx * dx + dy * dy + dz * dz;
        if (dist2 < 1.5f * 1.5f) return true;
        // lat/depth > 2 is outside the view. A shore triangle that far off axis overflows the guard band.
        float lat2 = dist2 - depth * depth;
        return lat2 > depth * depth * 4.0f;
    };
    int kept = 0;
    for (int i = 0; i + 2 < n && kept < g_waterLeft; i += 3) {
        if (bad(src[i]) || bad(src[i + 1]) || bad(src[i + 2])) continue;
        kept += 3;
    }
    if (!kept) return;
    alignGeList();
    int bytes = (kept * (int)sizeof(gfx::ModelVertex) + 15) & ~15;
    gfx::ModelVertex* dst = (gfx::ModelVertex*)gfx::allocBytes(bytes);
    if (!dst) return;
    int w = 0;
    for (int i = 0; i + 2 < n && w < kept; i += 3) {
        if (bad(src[i]) || bad(src[i + 1]) || bad(src[i + 2])) continue;
        for (int k = 0; k < 3; ++k) {
            const MeshVertex& s = src[i + k];
            dst[w++] = {s.u / 32768.0f, s.v / 32768.0f, color565(s.color), ox + s.x / 1024.0f, oy + s.y / 1024.0f,
                        oz + s.z / 1024.0f};
        }
    }
    g_waterLeft -= w;
    gfx::drawModel(Mat4::identity(), gfx::terrain, dst, w);
}

inline int ifloor(float v) {
    int i = (int)v;
    return v < (float)i ? i - 1 : i;
}

Mat4 viewMatrix(const Camera& c) {
    return c.effects * Mat4::rotateX(c.pitch) * Mat4::rotateY(c.yaw + 180.0f) * Mat4::translate(-c.x, -c.y, -c.z);
}

struct SkyV {
    float u, v;
    uint32_t color;
    float x, y, z;
};

constexpr int STARS = 600;
SkyV* g_stars = nullptr;  // 6 vertices per star, unit brightness, rotated by the celestial angle at draw time

// RenderGlobal.renderStars: random points on a sphere of radius 100.
void buildStars() {
    g_stars = (SkyV*)memalign(16, STARS * 6 * sizeof(SkyV));
    if (!g_stars) return;
    JRandom r(10842LL);
    int n = 0;
    while (n < STARS) {
        double x = r.nextFloat() * 2.0f - 1.0f, y = r.nextFloat() * 2.0f - 1.0f, z = r.nextFloat() * 2.0f - 1.0f;
        double size = 0.25f + r.nextFloat() * 0.25f;
        double d = x * x + y * y + z * z;
        double spin = r.nextDouble() * 3.14159265 * 2.0;
        if (d >= 1.0 || d <= 0.01) continue;
        d = 1.0 / std::sqrt(d);
        x *= d, y *= d, z *= d;
        double px = x * 100, py = y * 100, pz = z * 100;
        double yawA = std::atan2(x, z), sy = std::sin(yawA), cy = std::cos(yawA);
        double pitchA = std::atan2(std::sqrt(x * x + z * z), y), sp = std::sin(pitchA), cp = std::cos(pitchA);
        double ss = std::sin(spin), cs = std::cos(spin);
        SkyV q[4];
        for (int k = 0; k < 4; ++k) {
            double a = ((k & 2) - 1) * size, b = (((k + 1) & 2) - 1) * size;
            double c1 = a * cs - b * ss, c2 = b * cs + a * ss;
            double yy = c1 * sp, rr = -c1 * cp;
            q[k] = {0, 0, 0xFFFFFFFF, (float)(px + rr * sy - c2 * cy), (float)(py + yy), (float)(pz + c2 * sy + rr * cy)};
        }
        SkyV* o = g_stars + n * 6;
        o[0] = q[0], o[1] = q[1], o[2] = q[2], o[3] = q[0], o[4] = q[2], o[5] = q[3];
        ++n;
    }
    sceKernelDcacheWritebackRange(g_stars, STARS * 6 * sizeof(SkyV));
}

}  // namespace

bool WorldRenderer::init() {
    buildStars();
    return tmpCubes_.init(65532) && tmpOpaque_.init(32766) && tmpTrans_.init(16380);
}

void WorldRenderer::freeMesh(SectionMesh& m) {
    if (m.cubes || m.opaque || m.trans) gfx::waitIdle();
    std::free(m.cubes);
    std::free(m.opaque);
    std::free(m.trans);
    m = SectionMesh();
}

void WorldRenderer::releaseChunk(Chunk* c) {
    if (!c->render) return;
    RenderChunk* rc = (RenderChunk*)c->render;
    for (SectionMesh& m : rc->sec) freeMesh(m);
    delete rc;
    c->render = nullptr;
}

void WorldRenderer::reset() { outOfMemory = false; }

template <typename V>
static void upload(const VertexBuffer<V>& src, V*& dst, uint16_t& count, bool& outOfMemory) {
    if (dst) gfx::waitIdle();
    std::free(dst);
    dst = nullptr;
    count = 0;
    if (!src.count) return;
    size_t bytes = src.count * sizeof(V);
    dst = (V*)memalign(16, bytes);
    if (!dst) {
        if (!outOfMemory) crashNote("oom-mesh");
        outOfMemory = true;
        return;
    }
    std::memcpy(dst, src.data, bytes);
    sceKernelDcacheWritebackRange(dst, bytes);
    count = (uint16_t)src.count;
}

// Lit chunks are ready. A chunk that only has terrain still has to be drawn: waiting for the
// light pass is what left the column beside the player as a wall of sky.
bool chunkHasTerrain(const Chunk* c) {
    if (!c) return false;
    if (c->flags & CHUNK_LIT) return true;
    for (Section* s : c->sec)
        if (s) return true;
    return false;
}

// True when this section should produce at least one face. A buried solid section is false,
// so an empty mesh there is not rebuilt every frame.
bool sectionExposed(const World& w, const Chunk* c, int sy) {
    const Section& s = *c->sec[sy];
    for (int i = 0; i < 4096; ++i)
        if (!blockIsOpaque(s.blocks[i])) return true;
    int x0 = c->cx * 16, y0 = sy * 16, z0 = c->cz * 16;
    for (int a = 0; a < 16; ++a)
        for (int b = 0; b < 16; ++b) {
            if (!blockIsOpaque(w.get(x0 - 1, y0 + b, z0 + a)) || !blockIsOpaque(w.get(x0 + 16, y0 + b, z0 + a)))
                return true;
            if (!blockIsOpaque(w.get(x0 + a, y0 + b, z0 - 1)) || !blockIsOpaque(w.get(x0 + a, y0 + b, z0 + 16)))
                return true;
            if (!blockIsOpaque(w.get(x0 + a, y0 - 1, z0 + b)) || !blockIsOpaque(w.get(x0 + a, y0 + 16, z0 + b)))
                return true;
        }
    return false;
}

void WorldRenderer::rebuild(World& w, Chunk* c, int sy) {
    if (!c->render) {
        c->render = new (std::nothrow) RenderChunk();
        if (!c->render) {
            if (!outOfMemory) crashNote("oom-mesh");
            outOfMemory = true;
            return;
        }
    }
    SectionMesh& m = ((RenderChunk*)c->render)->sec[sy];
    bool retry = m.phase == 3;
    if (!c->sec[sy]) {
        freeMesh(m);
        m.phase = 4;
        return;
    }
    buildSection(w, c->cx, sy, c->cz, tmpCubes_, tmpOpaque_, tmpTrans_);
    sectionConnectivity(w, c->cx, sy, c->cz, m.conn);
    upload(tmpCubes_, m.cubes, m.nCubes, outOfMemory);
    upload(tmpOpaque_, m.opaque, m.nOpaque, outOfMemory);
    upload(tmpTrans_, m.trans, m.nTrans, outOfMemory);
    bool any = m.nCubes || m.nOpaque || m.nTrans;
    m.phase = any ? 1 : (retry ? 4 : 2);
}

int WorldRenderer::update(World& w, const Camera& cam, int maxBuilds, int budgetUs) {
    crashlog::heartbeat();
    crashNote("mesh");
    uint32_t start = sceKernelGetSystemTimeLow();
    int pcx = ifloor(cam.x / 16.0f), pcz = ifloor(cam.z / 16.0f);
    int freeKb = crashlog::memoryFreeKb();
    // Meshes are the large contiguous blocks. Drop far ones before a rebuild asks for another.
    int meshRadius = renderDistance + 1;
    if (freeKb < 1280 && meshRadius > 2) meshRadius = 2;
    for (int i = 0; i < World::RING; ++i)
        for (int j = 0; j < World::RING; ++j) {
            Chunk* c = w.slot(i, j);
            if (!c || !c->render) continue;
            int dx = std::abs(c->cx - pcx), dz = std::abs(c->cz - pcz);
            if (dx > meshRadius || dz > meshRadius) {
                releaseChunk(c);
                c->dirty = 0xFF;
            }
        }
    // Under a megabyte, still mesh the column next to the player. Skipping the whole
    // rebuild is what kept that column invisible while the probe said memory was tight.
    bool nearOnly = freeKb > 0 && freeKb < 1024;
    if (nearOnly && maxBuilds > 1) maxBuilds = 1;
    // A chunk that was lit and then never meshed stays invisible. Putting the flag back is what
    // fills the holes left when the loading screen stopped early.
    for (int dx = -renderDistance; dx <= renderDistance; ++dx)
        for (int dz = -renderDistance; dz <= renderDistance; ++dz) {
            Chunk* c = w.chunk(pcx + dx, pcz + dz);
            if (!c || !chunkHasTerrain(c)) continue;
            uint8_t mask = 0;
            if (w.chunk(c->cx + 1, c->cz)) mask |= 1;
            if (w.chunk(c->cx - 1, c->cz)) mask |= 2;
            if (w.chunk(c->cx, c->cz + 1)) mask |= 4;
            if (w.chunk(c->cx, c->cz - 1)) mask |= 8;
            bool first = (c->neighborMask & 0x80) == 0;
            uint8_t old = c->neighborMask & 15;
            int cheb = std::abs(dx) > std::abs(dz) ? std::abs(dx) : std::abs(dz);
            // Rebuild when a neighbor arrives, and once when a chunk that was meshed further
            // out comes close. Faces baked while the next chunk was missing are the vertical cut.
            if ((first && c->render && cheb <= 2) || (!first && old != mask)) {
                c->neighborMask = (uint8_t)(mask | 0x80);
                if (c->render) {
                    c->dirty = 0xFF;
                    auto* rc = (RenderChunk*)c->render;
                    for (int sy = 0; sy < World::SECTIONS_Y; ++sy) {
                        uint8_t p = rc->sec[sy].phase;
                        if (p == 2 || p == 4) rc->sec[sy].phase = 0;
                    }
                }
            } else if (first && !c->render)
                c->neighborMask = (uint8_t)(mask | 0x80);
            if (!c->render) {
                if (c->dirty == 0) c->dirty = 0xFF;
                continue;
            }
            auto* rc = (RenderChunk*)c->render;
            for (int sy = 0; sy < World::SECTIONS_Y; ++sy) {
                if (!c->sec[sy]) continue;
                SectionMesh& m = rc->sec[sy];
                if (m.phase == 0) c->dirty = (uint8_t)(c->dirty | (1 << sy));
                else if (m.phase == 2) {
                    if (sectionExposed(w, c, sy)) {
                        m.phase = 3;
                        c->dirty = (uint8_t)(c->dirty | (1 << sy));
                    } else
                        m.phase = 4;
                }
            }
        }
    int built = 0;
    while (built < maxBuilds) {
        if (budgetUs > 0 && built > 0 && (int)(sceKernelGetSystemTimeLow() - start) > budgetUs) break;
        Chunk* best = nullptr;
        int bestSy = 0;
        float bestD = 1e30f;
        for (int dx = -renderDistance; dx <= renderDistance; ++dx)
            for (int dz = -renderDistance; dz <= renderDistance; ++dz) {
                Chunk* c = w.chunk(pcx + dx, pcz + dz);
                if (!c || !c->dirty || !chunkHasTerrain(c)) continue;
                if (nearOnly && (std::abs(dx) > 1 || std::abs(dz) > 1)) continue;
                for (int sy = 0; sy < World::SECTIONS_Y; ++sy) {
                    if (!(c->dirty & (1 << sy))) continue;
                    float ddx = c->cx * 16 + 8 - cam.x, ddy = sy * 16 + 8 - cam.y, ddz = c->cz * 16 + 8 - cam.z;
                    float d = ddx * ddx + ddy * ddy + ddz * ddz;
                    if (!c->sec[sy]) d = -1;  // free to "build"
                    if (d < bestD) {
                        bestD = d;
                        best = c;
                        bestSy = sy;
                    }
                }
            }
        if (!best) break;
        best->dirty &= (uint8_t) ~(1 << bestSy);
        bool empty = !best->sec[bestSy];
        rebuild(w, best, bestSy);
        if (!empty) ++built;
    }
    return built;
}

void WorldRenderer::buildNear(World& w, const Camera& cam, int radius) {
    int saved = renderDistance;
    // The whole view, not just the nearest chunks. Stopping at radius 2 left a ring of holes
    // that play mode then treated as already finished.
    renderDistance = radius;
    uint32_t start = sceKernelGetSystemTimeLow();
    while (update(w, cam, 4, 3000) > 0) {
        if ((int)(sceKernelGetSystemTimeLow() - start) > 1500000) break;
        sceKernelDelayThread(200);
    }
    renderDistance = saved;
}

void WorldRenderer::drawSky(const Camera& cam) {
    float focal = (gfx::SCREEN_H / 2.0f) / std::tan(FOV * 3.14159265f / 360.0f);
    float p = cam.pitch;
    if (p > 89.0f) p = 89.0f;
    if (p < -89.0f) p = -89.0f;
    float horizon = gfx::SCREEN_H / 2.0f - std::tan(p * 3.14159265f / 180.0f) * focal;
    float bandTop = horizon - 110.0f;
    gfx::begin2D();
    if (bandTop > 0) gfx::rect(0, 0, gfx::SCREEN_W, bandTop + 1, skyColor);
    float y0 = bandTop < -2000 ? -2000 : bandTop;
    float y1 = horizon > 4000 ? 4000 : horizon;
    if (y1 > y0) gfx::gradient(0, y0, gfx::SCREEN_W, y1 - y0, skyColor, fogColor);
    if (horizon < gfx::SCREEN_H) {
        float t = horizon < 0 ? 0 : horizon;
        gfx::rect(0, t, gfx::SCREEN_W, gfx::SCREEN_H - t + 1, fogColor);
    }
}

void WorldRenderer::drawCelestial(const Camera& cam) {
    Mat4 proj = Mat4::perspective(FOV, (float)gfx::SCREEN_W / gfx::SCREEN_H, 1.0f, 300.0f);
    Mat4 view = cam.effects * Mat4::rotateX(cam.pitch) * Mat4::rotateY(cam.yaw + 180.0f);
    // RenderGlobal.renderSky: rotate -90 around Y, then by the celestial angle around X.
    Mat4 model = Mat4::rotateY(-90.0f) * Mat4::rotateX(celestialAngle * 360.0f);
    gfx::begin3D(proj, view, false, 0, 0, 0);
    gfx::setModel(model);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_CULL_FACE);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0xFFFFFFFF, 0xFFFFFFFF);

    auto quad = [&](const gfx::Texture& t, float s, float y) {
        gfx::bind(t);
        SkyV* v = (SkyV*)gfx::allocModel(6);
        if (!v) return;
        const float tw = 1.0f;
        SkyV q[4] = {{0, 0, 0xFFFFFFFF, -s, y, -s},
                     {tw, 0, 0xFFFFFFFF, s, y, -s},
                     {tw, tw, 0xFFFFFFFF, s, y, s},
                     {0, tw, 0xFFFFFFFF, -s, y, s}};
        if (y < 0) {
            std::swap(q[1], q[3]);
        }
        v[0] = q[0], v[1] = q[1], v[2] = q[2], v[3] = q[0], v[4] = q[2], v[5] = q[3];
        sceGuDrawArray(GU_TRIANGLES, SKY_VTYPE, 6, nullptr, v);
    };
    quad(gfx::sun, 30.0f, 100.0f);
    quad(gfx::moon, 20.0f, -100.0f);

    if (g_stars && starBrightness > 0.01f) {
        int a = (int)(starBrightness * 255);
        sceGuDisable(GU_TEXTURE_2D);
        sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, gfx::rgba(a, a, a), 0xFFFFFFFF);
        sceGuDrawArray(GU_TRIANGLES, SKY_VTYPE, STARS * 6, nullptr, g_stars);
        sceGuEnable(GU_TEXTURE_2D);
    }
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuDisable(GU_BLEND);
    gfx::bind(gfx::terrain);
}

// RenderGlobal.renderClouds (fast clouds): a plane at y 108.33 that follows the camera, eight blocks per
// texel of clouds.png, drifting east. The GE cannot clip big triangles that reach behind the camera, so the
// plane is cut finer near the middle; the outer edge fades out instead of ending in a line.
void WorldRenderer::drawClouds(const Camera& cam, bool aboveTerrain) {
    const float H = 108.33f - cam.y, F = 1.0f / 2048.0f, EXTENT = 192.0f, FADE = 64.0f;
    static const int RINGS[4][2] = {{2, 8}, {8, 32}, {32, 128}, {64, 192}};  // cell size, half extent
    double ox = cam.x + cloudScroll, oz = cam.z;
    ox -= std::floor(ox / 2048.0) * 2048.0;
    oz -= std::floor(oz / 2048.0) * 2048.0;
    float u0 = (float)(ox * F), v0 = (float)(oz * F);
    Mat4 view = cam.effects * Mat4::rotateX(cam.pitch) * Mat4::rotateY(cam.yaw + 180.0f);
    if (aboveTerrain) {
        float dist = renderDistance * 16.0f;
        Mat4 proj = Mat4::perspective(FOV, (float)gfx::SCREEN_W / gfx::SCREEN_H, 0.1f, dist + 32.0f);
        gfx::begin3D(proj, view, true, dist * 0.4f, dist, fogColor);
    } else {
        Mat4 proj = Mat4::perspective(FOV, (float)gfx::SCREEN_W / gfx::SCREEN_H, 0.5f, 400.0f);
        gfx::begin3D(proj, view, true, 40.0f, 190.0f, fogColor);
        sceGuDisable(GU_DEPTH_TEST);
    }
    sceGuDisable(GU_CULL_FACE);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuEnable(GU_BLEND);
    int cells = 0;
    for (int r = 0; r < 4; ++r) {
        int n = RINGS[r][1] * 2 / RINGS[r][0], inner = r ? RINGS[r - 1][1] * 2 / RINGS[r][0] : 0;
        cells += n * n - inner * inner;
    }
    gfx::ModelVertex* v = gfx::allocModel(cells * 6);
    int k = 0;
    uint32_t rgb = cloudColor & 0x00FFFFFF;
    int alpha = (int)(cloudColor >> 24);
    auto vertex = [&](float x, float z) {
        float edge = std::fabs(x) > std::fabs(z) ? std::fabs(x) : std::fabs(z);
        float f = (EXTENT - edge) / FADE;
        int a = f >= 1 ? alpha : f <= 0 ? 0 : (int)(alpha * f);
        return gfx::ModelVertex{x * F + u0, z * F + v0, rgb | ((uint32_t)a << 24), x, H, z};
    };
    if (v) {
        for (int r = 0; r < 4; ++r) {
            float size = (float)RINGS[r][0];
            int ext = RINGS[r][1], hole = r ? RINGS[r - 1][1] : 0;
            for (int i = -ext; i < ext; i += RINGS[r][0])
                for (int j = -ext; j < ext; j += RINGS[r][0]) {
                    if (i >= -hole && i < hole && j >= -hole && j < hole) continue;
                    float x0 = (float)i, z0 = (float)j, x1 = x0 + size, z1 = z0 + size;
                    // Cells are camera-relative. A cloud quad that contains the eye hangs the GE's clipper.
                    if (std::fabs(H) < 4.0f && x0 < 4.0f && x1 > -4.0f && z0 < 4.0f && z1 > -4.0f) continue;
                    gfx::ModelVertex q[4] = {vertex(x0, z1), vertex(x1, z1), vertex(x1, z0), vertex(x0, z0)};
                    v[k++] = q[0], v[k++] = q[1], v[k++] = q[2], v[k++] = q[0], v[k++] = q[2], v[k++] = q[3];
                }
        }
        gfx::drawModel(Mat4::identity(), gfx::clouds, v, k);
    }
    sceGuDisable(GU_BLEND);
    sceGuEnable(GU_CULL_FACE);
}

void WorldRenderer::render(const World& w, const Camera& cam, const RayHit* selection) {
    ++frame_;
    g_nearLiquid = cam.underwater;
    if (!g_nearLiquid) {
        // The bank is two or three blocks above the surface, so the old 3-block probe never saw the
        // water and the full mesh was still submitted.
        int x = ifloor(cam.x), y = ifloor(cam.y), z = ifloor(cam.z);
        for (int dy = -8; dy <= 2 && !g_nearLiquid; ++dy)
            for (int dx = -3; dx <= 3 && !g_nearLiquid; ++dx)
                for (int dz = -3; dz <= 3 && !g_nearLiquid; ++dz)
                    if (blockIsLiquid(w.get(x + dx, y + dy, z + dz))) g_nearLiquid = true;
    }
    const uint32_t fogWater = gfx::rgba(10, 20, 60);
    float dist = renderDistance * 16.0f;
    if (!cam.underwater && !nether) {
        drawSky(cam);
        drawCelestial(cam);
    }
    bool cloudsAbove = clouds && !nether && !cam.underwater && cam.y > 108.33f;
    if (clouds && !nether && !cam.underwater && !cloudsAbove) drawClouds(cam, false);

    // The GE drops triangles that overflow its guard band, so keep the near plane a little farther than Beta's 0.05.
    Mat4 proj = Mat4::perspective(FOV, (float)gfx::SCREEN_W / gfx::SCREEN_H, 0.1f, dist + 32.0f);
    Mat4 view = viewMatrix(cam);
    Frustum fr;
    fr.fromMatrix(proj * view);
    auto worldPass = [&] {
        if (cam.underwater)
            gfx::begin3D(proj, view, true, 0.0f, 12.0f, fogWater);
        else
            gfx::begin3D(proj, view, true, nether ? 0.0f : dist * 0.4f, dist, fogColor);
    };
    worldPass();

    // Breadth-first walk through sections, entering a neighbor only if the
    // current section connects the entry face to the exit face.
    const int R = renderDistance < MAX_DISTANCE ? renderDistance : MAX_DISTANCE;
    int ccx = ifloor(cam.x / 16.0f), csy = ifloor(cam.y / 16.0f), ccz = ifloor(cam.z / 16.0f);
    auto slotOf = [&](int cx, int sy, int cz) {
        return ((cx - ccx + MAX_DISTANCE) * W + (cz - ccz + MAX_DISTANCE)) * 8 + sy;
    };
    bool fromAbove = csy >= World::SECTIONS_Y;
    bool fromBelow = csy < 0;
    int head = 0, tail = 0, visibleCount = 0;
    auto enqueue = [&](int cx, int sy, int cz, int entry, int dirs) {
        int i = slotOf(cx, sy, cz);
        if (visitedFrame_[i] == frame_) return;
        visitedFrame_[i] = frame_;
        queue_[tail] = (uint16_t)i;
        entry_[tail] = (uint8_t)entry;
        dirs_[tail] = (uint8_t)dirs;
        ++tail;
    };
    auto decode = [&](int i, int& cx, int& sy, int& cz) {
        sy = i & 7;
        int c = i >> 3;
        cz = c % W - MAX_DISTANCE + ccz;
        cx = c / W - MAX_DISTANCE + ccx;
    };
    if (fromAbove || fromBelow) {
        int sy = fromAbove ? World::SECTIONS_Y - 1 : 0;
        for (int cx = ccx - R; cx <= ccx + R; ++cx)
            for (int cz = ccz - R; cz <= ccz + R; ++cz)
                if (fr.boxVisible(cx * 16.0f, sy * 16.0f, cz * 16.0f, cx * 16 + 16.0f, sy * 16 + 16.0f, cz * 16 + 16.0f))
                    enqueue(cx, sy, cz, fromAbove ? 1 : 0, 0);
    } else {
        enqueue(ccx, csy, ccz, 6, 0);
    }
    while (head < tail) {
        int i = queue_[head];
        int entry = entry_[head];
        int dirs = dirs_[head];
        ++head;
        int cx, sy, cz;
        decode(i, cx, sy, cz);
        const Chunk* c = w.chunk(cx, cz);
        const SectionMesh* m = c && c->render ? &((const RenderChunk*)c->render)->sec[sy] : nullptr;
        // An empty mesh must not seal the flood. One section baked with no faces was hiding
        // every chunk behind it, which is the empty half of the view.
        if (m && !m->nCubes && !m->nOpaque && !m->nTrans) m = nullptr;
        // Bit 7 marks a section the frustum rejected. It is walked so the flood can reach the
        // chunk past it, and it is not drawn.
        if (m && !(dirs & 0x80) && visibleCount < SLOTS) {
            visibleChunk_[visibleCount] = c;
            visible_[visibleCount++] = (uint16_t)i;
        }
        int travel = dirs & 0x3F;
        for (int d = 0; d < 6; ++d) {
            if (travel & (1 << opposite(d))) continue;
            if (m && entry < 6 && !(m->conn[entry] & (1 << d))) continue;
            int nx = cx + DIR[d][0], ny = sy + DIR[d][1], nz = cz + DIR[d][2];
            if (ny < 0 || ny >= World::SECTIONS_Y) continue;
            int dcx = nx - ccx, dcz = nz - ccz;
            if (dcx > R || dcx < -R || dcz > R || dcz < -R) continue;
            int adx = dcx < 0 ? -dcx : dcx, adz = dcz < 0 ? -dcz : dcz;
            if (adx < adz) adx = adz;
            bool inFr = fr.boxVisible(nx * 16.0f, ny * 16.0f, nz * 16.0f, nx * 16 + 16.0f, ny * 16 + 16.0f,
                                      nz * 16 + 16.0f);
            // A section beside the camera is often outside the pyramid even though the chunk
            // in front of it is on screen. Dropping it here cut the world off in a straight wall.
            if (!inFr && adx > 1) continue;
            int child = travel | (1 << d);
            if (!inFr) child |= 0x80;
            enqueue(nx, ny, nz, opposite(d), child);
        }
    }
    // The flood stops at a solid section. The two chunks around the player are drawn anyway,
    // so a cliff face the flood never entered is still on screen. The depth buffer hides
    // whatever is actually behind a face.
    int nearR = R > 2 ? 2 : R;
    for (int dx = -nearR; dx <= nearR; ++dx)
        for (int dz = -nearR; dz <= nearR; ++dz)
            for (int sy = 0; sy < World::SECTIONS_Y && visibleCount < SLOTS; ++sy) {
                int cx = ccx + dx, cz = ccz + dz;
                int i = slotOf(cx, sy, cz);
                if ((unsigned)i >= (unsigned)SLOTS || visitedFrame_[i] == frame_) continue;
                const Chunk* c = w.chunk(cx, cz);
                if (!c || !c->render) continue;
                const SectionMesh& sm = ((const RenderChunk*)c->render)->sec[sy];
                if (!sm.nCubes && !sm.nOpaque && !sm.nTrans) continue;
                if ((dx || dz || sy != csy) &&
                    !fr.boxVisible(cx * 16.0f, sy * 16.0f, cz * 16.0f, cx * 16 + 16.0f, sy * 16 + 16.0f, cz * 16 + 16.0f))
                    continue;
                visitedFrame_[i] = frame_;
                visibleChunk_[visibleCount] = c;
                visible_[visibleCount++] = (uint16_t)i;
            }

    drawnSections = 0;
    drawnVertices = 0;
    crashNote("draw-solid");
    Mat4 model = Mat4::identity();
    model.m[0] = model.m[5] = model.m[10] = 32.0f;
    Mat4 cubeModel = Mat4::identity();
    cubeModel.m[0] = cubeModel.m[5] = cubeModel.m[10] = 128.0f;
    auto meshAt = [&](int k) -> const SectionMesh& {
        return ((const RenderChunk*)visibleChunk_[k]->render)->sec[visible_[k] & 7];
    };
    for (int k = 0; k < visibleCount; ++k) {
        const SectionMesh& s = meshAt(k);
        if (!s.nCubes) continue;
        const Chunk* c = visibleChunk_[k];
        cubeModel.m[12] = c->cx * 16.0f;
        cubeModel.m[13] = (visible_[k] & 7) * 16.0f;
        cubeModel.m[14] = c->cz * 16.0f;
        gfx::setModel(cubeModel);
        sceGuDrawArray(GU_TRIANGLES, CUBE_VTYPE, s.nCubes, nullptr, s.cubes);
        ++drawnSections;
        drawnVertices += s.nCubes;
    }
    // Plants are single-sided quads, so this pass draws without back-face culling.
    sceGuDisable(GU_CULL_FACE);
    {
        constexpr float deg = 0.017453292f;
        float cp = std::cos(cam.pitch * deg);
        float lx = -std::sin(cam.yaw * deg) * cp;
        float ly = -std::sin(cam.pitch * deg);
        float lz = std::cos(cam.yaw * deg) * cp;
        bool filtered = false;
        for (int k = 0; k < visibleCount; ++k) {
            const SectionMesh& s = meshAt(k);
            if (!s.nOpaque) continue;
            const Chunk* c = visibleChunk_[k];
            int sy = visible_[k] & 7;
            float ox = c->cx * 16.0f, oy = sy * 16.0f, oz = c->cz * 16.0f;
            model.m[12] = ox;
            model.m[13] = oy;
            model.m[14] = oz;
            gfx::setModel(model);
            float qx = cam.x < ox ? ox : cam.x > ox + 16.0f ? ox + 16.0f : cam.x;
            float qy = cam.y < oy ? oy : cam.y > oy + 16.0f ? oy + 16.0f : cam.y;
            float qz = cam.z < oz ? oz : cam.z > oz + 16.0f ? oz + 16.0f : cam.z;
            float qdx = qx - cam.x, qdy = qy - cam.y, qdz = qz - cam.z;
            bool close = qdx * qdx + qdy * qdy + qdz * qdz < 3.0f * 3.0f;
            if (!close) {
                sceGuDrawArray(GU_TRIANGLES, VTYPE, s.nOpaque, nullptr, s.opaque);
                drawnVertices += s.nOpaque;
                continue;
            }
            int kept = 0;
            for (int i = 0; i + 2 < s.nOpaque; i += 3)
                if (!opaqueTriNearEye(s.opaque + i, ox, oy, oz, cam, lx, ly, lz)) kept += 3;
            if (!kept) continue;
            alignGeList();
            int bytes = (kept * (int)sizeof(MeshVertex) + 15) & ~15;
            MeshVertex* dst = (MeshVertex*)gfx::allocBytes(bytes);
            // A 12-byte vertex from a misaligned list pointer locks the GE. Skip the section.
            if (!dst || ((uintptr_t)dst & 15)) continue;
            int w = 0;
            for (int i = 0; i + 2 < s.nOpaque; i += 3) {
                if (opaqueTriNearEye(s.opaque + i, ox, oy, oz, cam, lx, ly, lz)) continue;
                dst[w++] = s.opaque[i];
                dst[w++] = s.opaque[i + 1];
                dst[w++] = s.opaque[i + 2];
            }
            sceGuDrawArray(GU_TRIANGLES, VTYPE, w, nullptr, dst);
            drawnVertices += w;
            filtered = true;
        }
        if (filtered) {
            alignGeList();
            crashNote("solid-f");
        }
    }
    sceGuEnable(GU_CULL_FACE);

    if (drawEntities) {
        alignGeList();
        // Leave room for the float water pass and the hand. The mobs that do not fit are skipped.
        gfx::setListMargin(256 * 1024);
        drawEntities(drawUser, cam);
        gfx::setListMargin(0);
        alignGeList();
        worldPass();
    }

    sceGuEnable(GU_BLEND);
    // The 3-block probe misses the beach. Standing at sea level with the ocean a few blocks
    // away still submitted the 16-bit water mesh, and that mesh locks the GE.
    if (!g_nearLiquid) {
        for (int k = 0; k < visibleCount && !g_nearLiquid; ++k) {
            const SectionMesh& s = meshAt(k);
            if (!s.nTrans) continue;
            const Chunk* c = visibleChunk_[k];
            int sy = visible_[k] & 7;
            float x0 = c->cx * 16.0f, y0 = sy * 16.0f, z0 = c->cz * 16.0f;
            float qx = cam.x < x0 ? x0 : cam.x > x0 + 16.0f ? x0 + 16.0f : cam.x;
            float qy = cam.y < y0 ? y0 : cam.y > y0 + 16.0f ? y0 + 16.0f : cam.y;
            float qz = cam.z < z0 ? z0 : cam.z > z0 + 16.0f ? z0 + 16.0f : cam.z;
            float qdx = qx - cam.x, qdy = qy - cam.y, qdz = qz - cam.z;
            if (qdx * qdx + qdy * qdy + qdz * qdz < 32.0f * 32.0f) g_nearLiquid = true;
        }
    }
    crashNote(g_nearLiquid ? "water-f" : "draw-water");
    g_waterLeft = 9000;
    // Near water is drawn closest first so the budget keeps the surface in front of the player.
    // Far from water the original mesh is drawn far-to-near, which is the correct blend order.
    int waterBegin = g_nearLiquid ? 0 : visibleCount - 1;
    int waterEnd = g_nearLiquid ? visibleCount : -1;
    int waterStep = g_nearLiquid ? 1 : -1;
    for (int k = waterBegin; k != waterEnd; k += waterStep) {
        const SectionMesh& s = meshAt(k);
        if (!s.nTrans) continue;
        const Chunk* c = visibleChunk_[k];
        int sy = visible_[k] & 7;
        model.m[12] = c->cx * 16.0f;
        model.m[13] = sy * 16.0f;
        model.m[14] = c->cz * 16.0f;
        gfx::setModel(model);
        drawTrans(s.trans, s.nTrans, c->cx * 16.0f, sy * 16.0f, c->cz * 16.0f, cam);
        drawnVertices += s.nTrans;
    }
    if (g_nearLiquid) alignGeList();
    sceGuDisable(GU_BLEND);

    if (cloudsAbove) drawClouds(cam, true);
    // Splashes stay off next to water. The build that stayed up on the PSP drew none of them there,
    // and a splash quad on the camera locks the GE the same way a water face does.
    if (drawEffects && !g_nearLiquid) {
        worldPass();
        crashNote("draw-fx");
        drawEffects(drawUser, cam);
    } else if (g_nearLiquid)
        crashNote("fx-off");
    if (drawEffects || cloudsAbove) worldPass();

    if (selection) {
        AABB b;
        uint8_t id = w.get(selection->x, selection->y, selection->z);
        if (blockSelectionBox(w, selection->x, selection->y, selection->z, id, b)) {
            const float e = 0.01f;  // 16-bit depth buffer needs a larger gap than Beta's 0.002
            b = b.offset((float)selection->x, (float)selection->y, (float)selection->z);
            float x0 = b.x0 - e, y0 = b.y0 - e, z0 = b.z0 - e, x1 = b.x1 + e, y1 = b.y1 + e, z1 = b.z1 + e;
            // A selection box around the camera locks the clipper the same way a block face does.
            if (cam.x > x0 - 0.6f && cam.x < x1 + 0.6f && cam.y > y0 - 0.6f && cam.y < y1 + 0.6f && cam.z > z0 - 0.6f &&
                cam.z < z1 + 0.6f)
                return;
            const float lines[24][3] = {
                {x0, y0, z0}, {x1, y0, z0}, {x1, y0, z0}, {x1, y0, z1}, {x1, y0, z1}, {x0, y0, z1}, {x0, y0, z1}, {x0, y0, z0},
                {x0, y1, z0}, {x1, y1, z0}, {x1, y1, z0}, {x1, y1, z1}, {x1, y1, z1}, {x0, y1, z1}, {x0, y1, z1}, {x0, y1, z0},
                {x0, y0, z0}, {x0, y1, z0}, {x1, y0, z0}, {x1, y1, z0}, {x1, y0, z1}, {x1, y1, z1}, {x0, y0, z1}, {x0, y1, z1},
            };
            gfx::drawLines3D(&lines[0][0], 24, gfx::rgba(0, 0, 0, 102));
            if (crackStage >= 0) {
                AABB cb = {x0, y0, z0, x1, y1, z1};
                gfx::drawCrack(cb, 240 + (crackStage > 9 ? 9 : crackStage));
            }
        }
    }
}
