// EffectRenderer.renderParticles, EntityRenderer.renderRainSnow and RenderLightningBolt.
#include <pspgu.h>

#include <cmath>

#include "../core/jrandom.h"
#include "entityrender.h"
#include "gfx.h"

namespace {

inline int ifloor(float v) {
    int i = (int)v;
    return v < (float)i ? i - 1 : i;
}

float lightBrightness(const Game& g, int l) {
    float floor = g.dimension < 0 ? 0.1f : 0.05f;
    float f = 1.0f - l / 15.0f;
    return (1.0f - f) / (f * 3.0f + 1.0f) * (1.0f - floor) + floor;
}

uint32_t color(float r, float g, float b, float a) {
    auto c = [](float v) { return v < 0 ? 0 : v > 1 ? 255 : (int)(v * 255); };
    return gfx::rgba(c(r), c(g), c(b), c(a));
}

// java.util.Random.nextGaussian, which hands out its values in pairs.
struct Gaussian {
    JRandom& r;
    bool haveNext = false;
    double next = 0;
    double operator()() {
        if (haveNext) {
            haveNext = false;
            return next;
        }
        double v1, v2, s;
        do {
            v1 = 2 * r.nextDouble() - 1;
            v2 = 2 * r.nextDouble() - 1;
            s = v1 * v1 + v2 * v2;
        } while (s >= 1 || s == 0);
        double m = std::sqrt(-2 * std::log(s) / s);
        next = v2 * m;
        haveNext = true;
        return v1 * m;
    }
};

Mat4 viewMatrix(const Camera& c) {
    return c.effects * Mat4::rotateX(c.pitch) * Mat4::rotateY(c.yaw + 180.0f) * Mat4::translate(-c.x, -c.y, -c.z);
}

// Billboards facing the camera, one batch per texture (particles.png, terrain.png, items.png).
// Triangles, not GU_SPRITES: the GE hangs on a real PSP when a 3D sprite crosses the guard band.
void drawParticles(const Game& g, const Camera& cam, float partial) {
    const Particles& P = g.particles;
    if (!P.count) return;
    Mat4 view = viewMatrix(cam);
    float rx = view.m[0], ry = view.m[4], rz = view.m[8];
    float ux = view.m[1], uy = view.m[5], uz = view.m[9];
    float fx = -view.m[2], fy = -view.m[6], fz = -view.m[10];
    const gfx::Texture* tex[3] = {&gfx::particles, &gfx::terrain, &gfx::items};
    // The billboard is bigger than its center. Keep the whole quad in front of the near plane.
    auto inFront = [&](const Particle& q) {
        float x = q.px + (q.x - q.px) * partial, y = q.py + (q.y - q.py) * partial, z = q.pz + (q.z - q.pz) * partial;
        float s = 0.1f * P.renderScale(q, partial);
        float dx = x - cam.x, dy = y - cam.y, dz = z - cam.z;
        float d = dx * fx + dy * fy + dz * fz;
        if (d < 1.2f + s || dx * dx + dy * dy + dz * dz < (1.6f + s) * (1.6f + s)) return false;
        float lat2 = dx * dx + dy * dy + dz * dz - d * d;
        float lim = d - s;
        return lat2 <= lim * lim * 36.0f;
    };
    int perLayer[3] = {0, 0, 0};
    for (int i = 0; i < P.count; ++i)
        if (inFront(P.p[i])) ++perLayer[P.p[i].layer];
    sceGuDisable(GU_CULL_FACE);
    for (int layer = 0; layer < 3; ++layer) {
        if (!perLayer[layer]) continue;
        gfx::ModelVertex* v = gfx::allocModel(perLayer[layer] * 6);
        if (!v) continue;
        int k = 0;
        for (int i = 0; i < P.count; ++i) {
            const Particle& q = P.p[i];
            if (q.layer != layer || !inFront(q)) continue;
            float x = q.px + (q.x - q.px) * partial, y = q.py + (q.y - q.py) * partial, z = q.pz + (q.z - q.pz) * partial;
            float s = 0.1f * P.renderScale(q, partial);
            float u0, v0, u1, v1;
            if (layer == LAYER_PARTICLES) {
                u0 = (q.tex & 15) / 16.0f, v0 = (q.tex >> 4) / 16.0f;
                u1 = u0 + 0.0624375f, v1 = v0 + 0.0624375f;
            } else {
                u0 = ((q.tex & 15) + q.u / 4.0f) / 16.0f, v0 = ((q.tex >> 4) + q.v / 4.0f) / 16.0f;
                u1 = u0 + 0.01560938f, v1 = v0 + 0.01560938f;
            }
            float b = P.brightness(q, lightBrightness(g, g.world.getBrightness(ifloor(x), ifloor(y), ifloor(z))), partial);
            uint32_t c = color(q.r * b, q.g * b, q.b * b, 1.0f);
            float rxs = rx * s, rys = ry * s, rzs = rz * s, uxs = ux * s, uys = uy * s, uzs = uz * s;
            gfx::ModelVertex qv[4] = {
                {u0, v1, c, x - rxs - uxs, y - rys - uys, z - rzs - uzs},
                {u1, v1, c, x + rxs - uxs, y + rys - uys, z + rzs - uzs},
                {u1, v0, c, x + rxs + uxs, y + rys + uys, z + rzs + uzs},
                {u0, v0, c, x - rxs + uxs, y - rys + uys, z - rzs + uzs},
            };
            v[k++] = qv[0], v[k++] = qv[1], v[k++] = qv[2], v[k++] = qv[0], v[k++] = qv[2], v[k++] = qv[3];
        }
        gfx::drawModel(Mat4::identity(), *tex[layer], v, k);
    }
    sceGuEnable(GU_CULL_FACE);
}

// Crossed quads per column around the player, from the top solid block (or a few blocks below the
// camera) up to a few blocks above it; snow drifts, rain streaks down.
void drawRainSnow(const Game& g, const Camera& cam, float partial, bool fancy) {
    float strength = g.rainStrength(partial);
    if (strength <= 0 || g.dimension < 0) return;
    const int R = fancy ? 7 : 5;
    int px = ifloor(cam.x), py = ifloor(cam.y), pz = ifloor(cam.z);
    int ticks = (int)(g.world.time & 0xFFFFF);
    struct Column {
        int x, z, y0, y1;
        bool snow;
    };
    static Column cols[(2 * 7 + 1) * (2 * 7 + 1)];
    int n = 0;
    for (int x = px - R; x <= px + R; ++x)
        for (int z = pz - R; z <= pz + R; ++z) {
            int biome = g.biomeAt(x, z);
            bool snow = biomeSnows(biome);
            if (!snow && !biomeRains(biome)) continue;
            int top = g.world.topSolidAt(x, z);
            if (top < 0) top = 0;
            int y0 = py - R, y1 = py + R;
            if (y0 < top) y0 = top;
            if (y1 < top) y1 = top;
            if (y0 != y1) cols[n++] = {x, z, y0, y1, snow};
        }
    if (!n) return;

    // Drop the fractional part so a long-lived world cannot feed the GE a texture coordinate in the tens
    // of thousands; that overflows the rasterizer on hardware and locks it up.
    auto frac = [](float u) { return u - std::floor(u); };
    Mat4 view = viewMatrix(cam);
    float fx = -view.m[2], fy = -view.m[6], fz = -view.m[10];
    // In front of the near plane and not so far to the side that the GE's guard band overflows.
    auto ahead = [&](float x, float y, float z) {
        float dx = x - cam.x, dy = y - cam.y, dz = z - cam.z;
        float d = dx * fx + dy * fy + dz * fz;
        if (d < 0.8f) return false;
        float lat2 = dx * dx + dy * dy + dz * dz - d * d;
        return lat2 <= d * d * 36.0f;
    };
    // Every corner in front of the near plane: a convex quad then cannot cross it. A tall column is cut
    // into 2-block spans so a span that fails the test is only a small hole, not a triangle through the eye.
    auto spanOk = [&](const Column& c, int ya, int yb) {
        float x = (float)c.x, z = (float)c.z, y0 = (float)ya, y1 = (float)yb;
        return ahead(x, y0, z + 0.5f) && ahead(x + 1.0f, y0, z + 0.5f) && ahead(x + 1.0f, y1, z + 0.5f) &&
               ahead(x, y1, z + 0.5f) && ahead(x + 0.5f, y0, z) && ahead(x + 0.5f, y0, z + 1.0f) &&
               ahead(x + 0.5f, y1, z + 1.0f) && ahead(x + 0.5f, y1, z);
    };
    sceGuDisable(GU_CULL_FACE);
    sceGuEnable(GU_BLEND);
    sceGuAlphaFunc(GU_GREATER, 2, 0xFF);
    sceGuDepthMask(GU_TRUE);
    for (int pass = 0; pass < 2; ++pass) {
        bool snowPass = pass == 0;
        auto spanCount = [&](const Column& c) {
            int spans = 0;
            for (int y = c.y0; y < c.y1; y += 2) {
                int yb = y + 2 < c.y1 ? y + 2 : c.y1;
                if (spanOk(c, y, yb)) ++spans;
            }
            return spans;
        };
        int spans = 0;
        for (int i = 0; i < n; ++i)
            if (cols[i].snow == snowPass) spans += spanCount(cols[i]);
        if (!spans) continue;
        gfx::ModelVertex* v = gfx::allocModel(spans * 12);
        if (!v) continue;
        int k = 0;
        JRandom r;
        for (int i = 0; i < n; ++i) {
            const Column& c = cols[i];
            if (c.snow != snowPass || !spanCount(c)) continue;
            uint32_t hash = (uint32_t)c.x * (uint32_t)c.x * 3121u + (uint32_t)c.x * 45238971u +
                            (uint32_t)c.z * (uint32_t)c.z * 418711u + (uint32_t)c.z * 13761u;
            r.setSeed((int64_t)(int32_t)hash);
            float dx = c.x + 0.5f - g.player.x, dz = c.z + 0.5f - g.player.z;
            float d = std::sqrt(dx * dx + dz * dz) / R;
            float du = 0, scroll = 0, drift = 0;
            uint32_t col;
            if (snowPass) {
                Gaussian gauss{r};
                float t = ticks + partial;
                scroll = ((ticks & 511) + partial) / 512.0f;
                du = frac(r.nextFloat() + t * 0.01f * (float)gauss());
                drift = r.nextFloat() + t * (float)gauss() * 0.001f;
                int ly = c.y0 > py ? c.y0 : py;
                float b = lightBrightness(g, g.world.getBrightness(c.x, ly, c.z));
                col = color(b, b, b, ((1.0f - d * d) * 0.3f + 0.5f) * strength);
            } else {
                scroll = ((float)((uint32_t)(ticks + (int32_t)hash) & 31) + partial) / 32.0f * (3.0f + r.nextFloat());
                float b = lightBrightness(g, g.world.getBrightness(c.x, World::SY - 1, c.z)) * 0.85f + 0.15f;
                col = color(b, b, b, ((1.0f - d * d) * 0.5f + 0.5f) * strength);
            }
            auto emit = [&](int ya, int yb) {
                float base = snowPass ? ya / 4.0f + scroll + drift : ya / 4.0f + scroll;
                float dv0 = frac(base), dv1 = dv0 + (yb - ya) / 4.0f;
                float x = (float)c.x, z = (float)c.z, y0 = (float)ya, y1 = (float)yb;
                gfx::ModelVertex q[8] = {
                    {du, dv0, col, x, y0, z + 0.5f},         {1 + du, dv0, col, x + 1, y0, z + 0.5f},
                    {1 + du, dv1, col, x + 1, y1, z + 0.5f}, {du, dv1, col, x, y1, z + 0.5f},
                    {du, dv0, col, x + 0.5f, y0, z},         {1 + du, dv0, col, x + 0.5f, y0, z + 1},
                    {1 + du, dv1, col, x + 0.5f, y1, z + 1}, {du, dv1, col, x + 0.5f, y1, z},
                };
                for (int f = 0; f < 2; ++f) {
                    const gfx::ModelVertex* o = q + f * 4;
                    v[k++] = o[0], v[k++] = o[1], v[k++] = o[2], v[k++] = o[0], v[k++] = o[2], v[k++] = o[3];
                }
            };
            for (int y = c.y0; y < c.y1; y += 2) {
                int yb = y + 2 < c.y1 ? y + 2 : c.y1;
                if (spanOk(c, y, yb)) emit(y, yb);
            }
        }
        gfx::drawModel(Mat4::identity(), snowPass ? gfx::snow : gfx::rain, v, k);
    }
    sceGuDepthMask(GU_FALSE);
    sceGuAlphaFunc(GU_GREATER, 0x10, 0xFF);
    sceGuDisable(GU_BLEND);
    sceGuEnable(GU_CULL_FACE);
}

// RenderLightningBolt: 4 glow layers of a jagged column of 16-block segments with two side branches.
void drawBolt(const Game::Bolt& bolt, float camX, float camY, float camZ, float fx, float fy, float fz) {
    double sx[8], sz[8];
    double ox = 0, oz = 0;
    JRandom shape(bolt.seed);
    for (int i = 7; i >= 0; --i) {
        sx[i] = ox, sz[i] = oz;
        ox += shape.nextInt(11) - 5;
        oz += shape.nextInt(11) - 5;
    }
    const uint32_t c = color(0.45f, 0.45f, 0.5f, 0.3f);
    for (int layer = 0; layer < 4; ++layer) {
        JRandom r(bolt.seed);
        for (int branch = 0; branch < 3; ++branch) {
            int top = branch > 0 ? 7 - branch : 7;
            int bottom = branch > 0 ? top - 2 : 0;
            double x = sx[top] - ox, z = sz[top] - oz;
            for (int seg = top; seg >= bottom; --seg) {
                double px = x, pz = z;
                if (branch == 0) {
                    x += r.nextInt(11) - 5;
                    z += r.nextInt(11) - 5;
                } else {
                    x += r.nextInt(31) - 15;
                    z += r.nextInt(31) - 15;
                }
                double w0 = 0.1 + layer * 0.2, w1 = 0.1 + layer * 0.2;
                if (branch == 0) {
                    w0 *= seg * 0.1 + 1.0;
                    w1 *= (seg - 1) * 0.1 + 1.0;
                }
                // Four short strips instead of one 16-block strip, and nothing that contains the camera.
                for (int sub = 0; sub < 4; ++sub) {
                    float f0 = sub / 4.0f, f1 = (sub + 1) / 4.0f;
                    double x0 = x + (px - x) * f0, z0 = z + (pz - z) * f0;
                    double x1 = x + (px - x) * f1, z1 = z + (pz - z) * f1;
                    float y0 = bolt.y + seg * 16.0f + sub * 4.0f, y1 = y0 + 4.0f;
                    float mx = (float)(bolt.x + 0.5 + (x0 + x1) * 0.5);
                    float mz = (float)(bolt.z + 0.5 + (z0 + z1) * 0.5);
                    float hdx = camX - mx, hdz = camZ - mz;
                    auto endOk = [&](float x, float y, float z) {
                        float dx = x - camX, dy = y - camY, dz = z - camZ;
                        float d = dx * fx + dy * fy + dz * fz;
                        if (d < 2.0f) return false;
                        float lat2 = dx * dx + dy * dy + dz * dz - d * d;
                        return lat2 <= d * d * 36.0f;
                    };
                    // Both ends inside the guard band: the strip is convex, so the whole thing is too.
                    if (camY > y0 - 2.0f && camY < y1 + 2.0f && hdx * hdx + hdz * hdz < 4.0f) continue;
                    if (!endOk((float)(bolt.x + 0.5 + x0), y0, (float)(bolt.z + 0.5 + z0)) ||
                        !endOk((float)(bolt.x + 0.5 + x1), y1, (float)(bolt.z + 0.5 + z1)))
                        continue;
                    double hw0 = w1 + (w0 - w1) * f0, hw1 = w1 + (w0 - w1) * f1;
                    gfx::ColorVertex* v = gfx::allocColor(10);
                    if (!v) continue;
                    for (int k = 0; k < 5; ++k) {
                        double ax = bolt.x + 0.5 - hw1, az = bolt.z + 0.5 - hw1;
                        if (k == 1 || k == 2) ax += hw1 * 2;
                        if (k == 2 || k == 3) az += hw1 * 2;
                        double bx = bolt.x + 0.5 - hw0, bz = bolt.z + 0.5 - hw0;
                        if (k == 1 || k == 2) bx += hw0 * 2;
                        if (k == 2 || k == 3) bz += hw0 * 2;
                        v[k * 2] = {c, (float)(bx + x0), y0, (float)(bz + z0)};
                        v[k * 2 + 1] = {c, (float)(ax + x1), y1, (float)(az + z1)};
                    }
                    gfx::drawColored3D(GU_TRIANGLE_STRIP, v, 10);
                }
            }
        }
    }
}

void drawBolts(const Game& g, const Camera& cam) {
    if (!g.boltCount) return;
    sceGuDisable(GU_CULL_FACE);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_FIX, 0, 0xFFFFFFFF);
    sceGuDepthMask(GU_TRUE);
    Mat4 view = viewMatrix(cam);
    float fx = -view.m[2], fy = -view.m[6], fz = -view.m[10];
    for (int i = 0; i < g.boltCount; ++i) drawBolt(g.bolts[i], cam.x, cam.y, cam.z, fx, fy, fz);
    sceGuDepthMask(GU_FALSE);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuDisable(GU_BLEND);
    sceGuEnable(GU_CULL_FACE);
}

}  // namespace

void drawEffects(const Game& g, const Camera& cam, float partial, bool fancy) {
    drawParticles(g, cam, partial);
    drawBolts(g, cam);
    drawRainSnow(g, cam, partial, fancy);
}
