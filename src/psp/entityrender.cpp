// Ports of the Beta 1.7.3 models (ModelBiped, ModelZombie, ModelSkeleton, ModelCreeper, ModelSpider,
// ModelQuadruped, ModelPig, ModelCow, ModelSheep1/2, ModelChicken) and RenderLiving/RenderItem/RenderArrow/RenderTNTPrimed.
#include "entityrender.h"

#include <pspgu.h>

#include <cmath>
#include <cstring>
#include <utility>

#include "../core/blocks.h"
#include "../core/jrandom.h"
#include "../core/mesher.h"
#include "gfx.h"

namespace {

constexpr float PI = 3.14159265f;
constexpr float RAD = 57.295776f;

inline int ifloor(float v) {
    int i = (int)v;
    return v < (float)i ? i - 1 : i;
}

// WorldProvider.generateLightBrightnessTable; the Nether raises the floor to 0.1.
float g_lightFloor = 0.05f;
// Eye position for this frame. A box, arrow, or painting that reaches the eye locks the GE clipper.
float g_eyeX = 0, g_eyeY = 0, g_eyeZ = 0;

void transformPoint(const Mat4& m, float x, float y, float z, float& ox, float& oy, float& oz) {
    ox = m.m[0] * x + m.m[4] * y + m.m[8] * z + m.m[12];
    oy = m.m[1] * x + m.m[5] * y + m.m[9] * z + m.m[13];
    oz = m.m[2] * x + m.m[6] * y + m.m[10] * z + m.m[14];
}

// Distance from the eye to the closest point of the transformed box. Inside the box this is zero.
bool modelNearEye(const Mat4& m, float x0, float y0, float z0, float x1, float y1, float z1) {
    float minX = 1e9f, minY = 1e9f, minZ = 1e9f, maxX = -1e9f, maxY = -1e9f, maxZ = -1e9f;
    const float c[8][3] = {{x0, y0, z0}, {x1, y0, z0}, {x1, y1, z0}, {x0, y1, z0},
                           {x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}};
    for (int i = 0; i < 8; ++i) {
        float x, y, z;
        transformPoint(m, c[i][0], c[i][1], c[i][2], x, y, z);
        if (x < minX) minX = x;
        if (y < minY) minY = y;
        if (z < minZ) minZ = z;
        if (x > maxX) maxX = x;
        if (y > maxY) maxY = y;
        if (z > maxZ) maxZ = z;
    }
    float x = g_eyeX < minX ? minX : g_eyeX > maxX ? maxX : g_eyeX;
    float y = g_eyeY < minY ? minY : g_eyeY > maxY ? maxY : g_eyeY;
    float z = g_eyeZ < minZ ? minZ : g_eyeZ > maxZ ? maxZ : g_eyeZ;
    float dx = x - g_eyeX, dy = y - g_eyeY, dz = z - g_eyeZ;
    return dx * dx + dy * dy + dz * dz < 0.45f * 0.45f;
}

float lightBrightness(int l) {
    float f = 1.0f - l / 15.0f;
    return (1.0f - f) / (f * 3.0f + 1.0f) * (1.0f - g_lightFloor) + g_lightFloor;
}

uint32_t shade(float r, float g, float b, float a = 1.0f) {
    auto c = [](float v) { return v < 0 ? 0 : v > 1 ? 255 : (int)(v * 255); };
    return gfx::rgba(c(r), c(g), c(b), c(a));
}

// ModelRenderer: rotation point in model pixels, angles in radians, applied Z, Y, X
// (Y, X, Z with yxz set, as ModelRenderer.renderWithRotation).
struct Part {
    float px = 0, py = 0, pz = 0;
    float rx = 0, ry = 0, rz = 0;
    bool yxz = false;
};

struct Model {
    Mat4 root;
    const gfx::Texture* tex;
    uint32_t color;

    void box(const Part& p, int u, int v, float x, float y, float z, int w, int h, int d, float inflate = 0,
             bool mirror = false) const {
        const float S = 1.0f / 16.0f;
        float x0 = (x - inflate) * S, y0 = (y - inflate) * S, z0 = (z - inflate) * S;
        float x1 = (x + w + inflate) * S, y1 = (y + h + inflate) * S, z1 = (z + d + inflate) * S;
        if (mirror) std::swap(x0, x1);
        const float P[8][3] = {{x0, y0, z0}, {x1, y0, z0}, {x1, y1, z0}, {x0, y1, z0},
                               {x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}};
        const int Q[6][4] = {{5, 1, 2, 6}, {0, 4, 7, 3}, {5, 4, 0, 1}, {2, 3, 7, 6}, {1, 0, 3, 2}, {4, 5, 6, 7}};
        const int UV[6][4] = {{u + d + w, v + d, u + d + w + d, v + d + h},
                              {u, v + d, u + d, v + d + h},
                              {u + d, v, u + d + w, v + d},
                              {u + d + w, v, u + d + w + w, v + d},
                              {u + d, v + d, u + d + w, v + d + h},
                              {u + d + w + d, v + d, u + d + w + d + w, v + d + h}};
        float tw = (float)tex->width, th = (float)tex->height;
        Mat4 m = root * Mat4::translate(p.px * S, p.py * S, p.pz * S);
        if (p.yxz) {
            if (p.ry != 0) m = m * Mat4::rotateY(p.ry * RAD);
            if (p.rx != 0) m = m * Mat4::rotateX(p.rx * RAD);
            if (p.rz != 0) m = m * Mat4::rotateZ(p.rz * RAD);
        } else {
            if (p.rz != 0) m = m * Mat4::rotateZ(p.rz * RAD);
            if (p.ry != 0) m = m * Mat4::rotateY(p.ry * RAD);
            if (p.rx != 0) m = m * Mat4::rotateX(p.rx * RAD);
        }
        // A limb that reaches the camera is left out. The rest of the mob still draws, so a sheep
        // you are close enough to punch stays visible.
        if (modelNearEye(m, x0, y0, z0, x1, y1, z1)) return;
        gfx::ModelVertex* out = gfx::allocModel(36);
        if (!out) return;
        int k = 0;
        for (int f = 0; f < 6; ++f) {
            float U1 = UV[f][0] / tw, V1 = UV[f][1] / th, U2 = UV[f][2] / tw, V2 = UV[f][3] / th;
            const float uv[4][2] = {{U2, V1}, {U1, V1}, {U1, V2}, {U2, V2}};
            const int order[6] = {0, 1, 2, 0, 2, 3};
            for (int i : order) {
                const float* pt = P[Q[f][i]];
                out[k++] = {uv[i][0], uv[i][1], color, pt[0], pt[1], pt[2]};
            }
        }
        gfx::drawModel(m, *tex, out, 36);
    }
};

// f = limb swing, f1 = limb amount, f2 = age, f3 = head yaw, f4 = head pitch (ModelBase.setRotationAngles).
void biped(const Model& m, float f, float f1, float f2, float f3, float f4, bool skeleton, Part* rightArm = nullptr) {
    Part head{0, 0, 0, f4 / RAD, f3 / RAD, 0};
    Part body{0, 0, 0};
    Part rArm{-5, 2, 0}, lArm{5, 2, 0}, rLeg{-2, 12, 0}, lLeg{2, 12, 0};
    rLeg.rx = std::cos(f * 0.6662f) * 1.4f * f1;
    lLeg.rx = std::cos(f * 0.6662f + PI) * 1.4f * f1;
    // ModelZombie arms
    rArm.ry = -0.1f;
    lArm.ry = 0.1f;
    rArm.rx = lArm.rx = -PI / 2;
    rArm.rz = std::cos(f2 * 0.09f) * 0.05f + 0.05f;
    lArm.rz = -(std::cos(f2 * 0.09f) * 0.05f + 0.05f);
    rArm.rx += std::sin(f2 * 0.067f) * 0.05f;
    lArm.rx -= std::sin(f2 * 0.067f) * 0.05f;
    if (rightArm) *rightArm = rArm;
    m.box(head, 0, 0, -4, -8, -4, 8, 8, 8);
    m.box(body, 16, 16, -4, 0, -2, 8, 12, 4);
    if (skeleton) {
        m.box(rArm, 40, 16, -1, -2, -1, 2, 12, 2);
        m.box(lArm, 40, 16, -1, -2, -1, 2, 12, 2, 0, true);
        m.box(rLeg, 0, 16, -1, 0, -1, 2, 12, 2);
        m.box(lLeg, 0, 16, -1, 0, -1, 2, 12, 2, 0, true);
    } else {
        m.box(rArm, 40, 16, -3, -2, -2, 4, 12, 4);
        m.box(lArm, 40, 16, -1, -2, -2, 4, 12, 4, 0, true);
        m.box(rLeg, 0, 16, -2, 0, -2, 4, 12, 4);
        m.box(lLeg, 0, 16, -2, 0, -2, 4, 12, 4, 0, true);
    }
    m.box(head, 32, 0, -4, -8, -4, 8, 8, 8, 0.5f);
}

void creeper(const Model& m, float f, float f1, float f3, float f4) {
    Part head{0, 4, 0, f4 / RAD, f3 / RAD, 0};
    m.box(head, 0, 0, -4, -8, -4, 8, 8, 8);
    m.box(Part{0, 4, 0}, 16, 16, -4, 0, -2, 8, 12, 4);
    const float LEG[4][3] = {{-2, 16, 4}, {2, 16, 4}, {-2, 16, -4}, {2, 16, -4}};
    const float PHASE[4] = {0, PI, PI, 0};
    for (int i = 0; i < 4; ++i) {
        Part p{LEG[i][0], LEG[i][1], LEG[i][2], std::cos(f * 0.6662f + PHASE[i]) * 1.4f * f1, 0, 0};
        m.box(p, 0, 16, -2, 0, -2, 4, 6, 4);
    }
}

void spider(const Model& m, float f, float f1, float f3, float f4) {
    m.box(Part{0, 15, -3, f4 / RAD, f3 / RAD, 0}, 32, 4, -4, -4, -8, 8, 8, 8);
    m.box(Part{0, 15, 0}, 0, 0, -3, -3, -3, 6, 6, 6);
    m.box(Part{0, 15, 9}, 0, 12, -5, -4, -6, 10, 8, 12);
    const float q = 0.7853982f, e = 0.3926991f;
    float rz[8] = {-q, q, -q * 0.74f, q * 0.74f, -q * 0.74f, q * 0.74f, -q, q};
    float ry[8] = {e * 2, -e * 2, e, -e, -e, e, -e * 2, e * 2};
    float a[4], b[4];
    const float PH[4] = {0, PI, PI / 2, 4.712389f};
    for (int i = 0; i < 4; ++i) {
        a[i] = -(std::cos(f * 0.6662f * 2.0f + PH[i]) * 0.4f) * f1;
        b[i] = std::fabs(std::sin(f * 0.6662f + PH[i]) * 0.4f) * f1;
    }
    const float Z[4] = {2, 1, 0, -1};
    for (int i = 0; i < 8; ++i) {
        int pair = i / 2;
        float sgn = (i & 1) ? -1.0f : 1.0f;
        Part p{(i & 1) ? 4.0f : -4.0f, 15, Z[pair], 0, ry[i] + sgn * a[pair], rz[i] + sgn * b[pair]};
        if (i & 1)
            m.box(p, 18, 0, -1, -1, -1, 16, 2, 2);
        else
            m.box(p, 18, 0, -15, -1, -1, 16, 2, 2);
    }
}

// ModelQuadruped legs (legHeight i) with optional leg inflation; cows shift their legs outward.
void quadLegs(const Model& m, float f, float f1, int i, float inflate, bool cow) {
    float lx[4] = {-3, 3, -3, 3}, lz[4] = {7, 7, -5, -5};
    if (cow) {
        lx[0] -= 1, lx[1] += 1, lx[2] -= 1, lx[3] += 1;
        lz[2] -= 1, lz[3] -= 1;
    }
    const float PHASE[4] = {0, PI, PI, 0};
    for (int k = 0; k < 4; ++k) {
        Part p{lx[k], (float)(24 - i), lz[k], std::cos(f * 0.6662f + PHASE[k]) * 1.4f * f1, 0, 0};
        m.box(p, 0, 16, -2, 0, -2, 4, i, 4, inflate);
    }
}

void pig(const Model& m, float f, float f1, float f3, float f4, float inflate = 0) {
    m.box(Part{0, 12, -6, f4 / RAD, f3 / RAD, 0}, 0, 0, -4, -4, -8, 8, 8, 8, inflate);
    m.box(Part{0, 11, 2, PI / 2, 0, 0}, 28, 8, -5, -10, -7, 10, 16, 8, inflate);
    quadLegs(m, f, f1, 6, inflate, false);
}

// ModelMinecart: floor, four sides and the inner floor (cart.png).
void minecart(const Model& m) {
    m.box(Part{0, 4, 0, PI / 2, 0, 0}, 0, 10, -10, -8, -1, 20, 16, 2);
    m.box(Part{-9, 4, 0, 0, PI * 1.5f, 0}, 0, 0, -8, -9, -1, 16, 8, 2);
    m.box(Part{9, 4, 0, 0, PI / 2, 0}, 0, 0, -8, -9, -1, 16, 8, 2);
    m.box(Part{0, 4, -7, 0, PI, 0}, 0, 0, -8, -9, -1, 16, 8, 2);
    m.box(Part{0, 4, 7}, 0, 0, -8, -9, -1, 16, 8, 2);
    m.box(Part{0, 4.1f, 0, -PI / 2, 0, 0}, 44, 10, -9, -7, -1, 18, 14, 1);
}

// ModelBoat: bottom and four sides (boat.png).
void boat(const Model& m) {
    m.box(Part{0, 4, 0, PI / 2, 0, 0}, 0, 8, -12, -8, -3, 24, 16, 4);
    m.box(Part{-11, 4, 0, 0, PI * 1.5f, 0}, 0, 0, -10, -7, -1, 20, 6, 2);
    m.box(Part{11, 4, 0, 0, PI / 2, 0}, 0, 0, -10, -7, -1, 20, 6, 2);
    m.box(Part{0, 4, -9, 0, PI, 0}, 0, 0, -10, -7, -1, 20, 6, 2);
    m.box(Part{0, 4, 9}, 0, 0, -10, -7, -1, 20, 6, 2);
}

void cow(const Model& m, float f, float f1, float f3, float f4) {
    Part head{0, 4, -8, f4 / RAD, f3 / RAD, 0};
    m.box(head, 0, 0, -4, -4, -6, 8, 8, 6);
    Part horn{0, 3, -7, head.rx, head.ry, 0};
    m.box(horn, 22, 0, -4, -5, -4, 1, 3, 1);
    m.box(horn, 22, 0, 3, -5, -4, 1, 3, 1);
    m.box(Part{0, 5, 2, PI / 2, 0, 0}, 18, 4, -6, -10, -7, 12, 18, 10);
    quadLegs(m, f, f1, 12, 0, true);
    m.box(Part{0, 14, 6, PI / 2, 0, 0}, 52, 0, -2, -3, 0, 4, 6, 2);
}

void sheepBody(const Model& m, float f, float f1, float f3, float f4) {
    m.box(Part{0, 6, -8, f4 / RAD, f3 / RAD, 0}, 0, 0, -3, -4, -6, 6, 6, 8);
    m.box(Part{0, 5, 2, PI / 2, 0, 0}, 28, 8, -4, -10, -7, 8, 16, 6);
    quadLegs(m, f, f1, 12, 0, false);
}

void sheepFur(const Model& m, float f, float f1, float f3, float f4) {
    m.box(Part{0, 6, -8, f4 / RAD, f3 / RAD, 0}, 0, 0, -3, -4, -4, 6, 6, 6, 0.6f);
    m.box(Part{0, 5, 2, PI / 2, 0, 0}, 28, 8, -4, -10, -7, 8, 16, 6, 1.75f);
    const float LX[4] = {-3, 3, -3, 3}, LZ[4] = {7, 7, -5, -5}, PHASE[4] = {0, PI, PI, 0};
    for (int k = 0; k < 4; ++k)
        m.box(Part{LX[k], 12, LZ[k], std::cos(f * 0.6662f + PHASE[k]) * 1.4f * f1, 0, 0}, 0, 16, -2, 0, -2, 4, 6, 4,
              0.5f);
}

void chicken(const Model& m, float f, float f1, float wing, float f3, float f4) {
    Part head{0, 15, -4, -(f4 / RAD), f3 / RAD, 0};
    m.box(head, 0, 0, -2, -6, -2, 4, 6, 3);
    m.box(head, 14, 0, -2, -4, -4, 4, 2, 2);
    m.box(head, 14, 4, -1, -2, -3, 2, 2, 2);
    m.box(Part{0, 16, 0, PI / 2, 0, 0}, 0, 9, -3, -4, -3, 6, 8, 6);
    m.box(Part{-2, 19, 1, std::cos(f * 0.6662f) * 1.4f * f1, 0, 0}, 26, 0, -1, 0, -3, 3, 5, 3);
    m.box(Part{1, 19, 1, std::cos(f * 0.6662f + PI) * 1.4f * f1, 0, 0}, 26, 0, -1, 0, -3, 3, 5, 3);
    m.box(Part{-4, 13, 0, 0, 0, wing}, 24, 13, 0, 0, -3, 1, 4, 6);
    m.box(Part{4, 13, 0, 0, 0, -wing}, 24, 13, -1, 0, -3, 1, 4, 6);
}

// ModelSlime(16) inside, then ModelSlime(0) blended over it (RenderSlime.shouldRenderPass).
void slime(const Model& m) {
    m.box(Part{}, 0, 16, -3, 17, -3, 6, 6, 6);
    m.box(Part{}, 32, 0, -3.25f, 18, -3.5f, 2, 2, 2);
    m.box(Part{}, 32, 4, 1.25f, 18, -3.5f, 2, 2, 2);
    m.box(Part{}, 32, 8, 0, 21, -3.5f, 1, 1, 1);
    sceGuEnable(GU_BLEND);
    m.box(Part{}, 0, 0, -4, 16, -4, 8, 8, 8);
    sceGuDisable(GU_BLEND);
}

void squid(const Model& m, float tentacle) {
    m.box(Part{0, 8, 0}, 0, 0, -6, -8, -6, 12, 16, 12);
    for (int i = 0; i < 8; ++i) {
        float a = i * PI * 2.0f / 8.0f;
        Part p{std::cos(a) * 5.0f, 15, std::sin(a) * 5.0f, tentacle, i * PI * -2.0f / 8.0f + PI / 2, 0};
        m.box(p, 48, 0, -1, 0, -1, 2, 18, 2);
    }
}

// ModelGhast: the body and nine tentacles of random length (Random(1660)).
void ghast(const Model& m, float age) {
    m.box(Part{0, 8, 0}, 0, 0, -8, -8, -8, 16, 16, 16);
    static int len[9] = {0};
    if (!len[0]) {
        JRandom r(1660);
        for (int& l : len) l = r.nextInt(7) + 8;
    }
    for (int i = 0; i < 9; ++i) {
        float px = ((i % 3 - (i / 3 % 2) * 0.5f + 0.25f) - 1.0f) * 5.0f, pz = ((i / 3) - 1.0f) * 5.0f;
        m.box(Part{px, 15, pz, 0.2f * std::sin(age * 0.3f + i) + 0.4f, 0, 0}, 0, 0, -1, 0, -1, 2, len[i], 2);
    }
}

// ModelWolf.setLivingAnimations / setRotationAngles; f2 is the tail angle.
void wolf(const Model& m, const Entity& e, float f, float f1, float f2, float f4, float partial) {
    float shakeT = e.prevShake + (e.shake - e.prevShake) * partial;
    auto shakeRoll = [&](float offset) {
        float t = (shakeT + offset) / 1.8f;
        t = t < 0 ? 0 : t > 1 ? 1 : t;
        return std::sin(t * PI) * std::sin(t * PI * 11.0f) * 0.15f * PI;
    };
    float headRoll = (e.prevBeg + (e.beg - e.prevBeg) * partial) * 0.15f * PI + shakeRoll(0);
    Part head{-1, 13.5f, -7, f4 / RAD, 0, headRoll, true};
    m.box(head, 0, 0, -3, -3, -2, 6, 6, 4);
    m.box(head, 16, 14, -3, -5, 0, 2, 2, 1);
    m.box(head, 16, 14, 1, -5, 0, 2, 2, 1);
    Part nose = head;
    nose.px = -0.5f;
    m.box(nose, 0, 10, -2, 0, -5, 3, 3, 4);
    Part body, mane, tail, legs[4];
    if (e.sitting) {
        body = Part{0, 18, 0, 0.7853982f, 0, shakeRoll(-0.16f)};
        mane = Part{-1, 16, -3, 1.2566371f, 0, shakeRoll(-0.08f)};
        tail = Part{-1, 21, 6, f2, 0, shakeRoll(-0.2f), true};
        legs[0] = Part{-2.5f, 22, 2, 4.712389f};
        legs[1] = Part{0.5f, 22, 2, 4.712389f};
        legs[2] = Part{-2.49f, 17, -4, 5.811947f};
        legs[3] = Part{0.51f, 17, -4, 5.811947f};
    } else {
        body = Part{0, 14, 2, PI / 2, 0, shakeRoll(-0.16f)};
        mane = Part{-1, 14, -3, PI / 2, 0, shakeRoll(-0.08f)};
        float wag = e.angry ? 0.0f : std::cos(f * 0.6662f) * 1.4f * f1;
        tail = Part{-1, 12, 8, f2, wag, shakeRoll(-0.2f), true};
        float a = std::cos(f * 0.6662f) * 1.4f * f1, b = std::cos(f * 0.6662f + PI) * 1.4f * f1;
        legs[0] = Part{-2.5f, 16, 7, a};
        legs[1] = Part{0.5f, 16, 7, b};
        legs[2] = Part{-2.5f, 16, -4, b};
        legs[3] = Part{0.5f, 16, -4, a};
    }
    m.box(body, 18, 14, -4, -2, -3, 6, 9, 6);
    for (const Part& p : legs) m.box(p, 0, 18, -1, 0, -1, 2, 8, 2);
    m.box(tail, 9, 18, -1, 0, -1, 2, 8, 2);
    m.box(mane, 21, 0, -4, -3, -3, 8, 6, 7);
}

void flatItem(const Mat4& m, const gfx::Texture& t, int tile, uint32_t c);

const float FLEECE[16][3] = {{1.0f, 1.0f, 1.0f},    {0.95f, 0.7f, 0.2f}, {0.9f, 0.5f, 0.85f}, {0.6f, 0.7f, 0.95f},
                             {0.9f, 0.9f, 0.2f},    {0.5f, 0.8f, 0.1f},  {0.95f, 0.7f, 0.8f}, {0.3f, 0.3f, 0.3f},
                             {0.6f, 0.6f, 0.6f},    {0.3f, 0.6f, 0.7f},  {0.7f, 0.4f, 0.9f},  {0.2f, 0.4f, 0.8f},
                             {0.5f, 0.4f, 0.3f},    {0.4f, 0.5f, 0.2f},  {0.8f, 0.3f, 0.3f},  {0.1f, 0.1f, 0.1f}};

void drawMob(Game& g, const Entity& e, float ex, float ey, float ez, float partial, float light,
             const Mat4& pre = Mat4::identity()) {
    float yaw = e.prevYaw + (e.yaw - e.prevYaw) * partial;
    float amount = e.prevLimbAmount + (e.limbAmount - e.prevLimbAmount) * partial;
    float swing = e.limbSwing - e.limbAmount * (1.0f - partial);
    if (amount > 1) amount = 1;
    float age = e.age + partial;

    Mat4 root = pre * Mat4::translate(ex, ey, ez);
    if (e.type == ENT_SQUID) {
        // RenderSquid.rotateCorpse
        float pitch = e.prevBodyPitch + (e.bodyPitch - e.prevBodyPitch) * partial;
        float spin = e.prevBodySpin + (e.bodySpin - e.prevBodySpin) * partial;
        root = root * Mat4::translate(0, 0.5f, 0) * Mat4::rotateY(180.0f - yaw) * Mat4::rotateX(pitch) *
               Mat4::rotateY(spin) * Mat4::translate(0, -1.2f, 0);
    } else {
        root = root * Mat4::rotateY(180.0f - yaw);
        if (e.deathTime > 0) {
            float f = (e.deathTime + partial - 1.0f) / 20.0f * 1.6f;
            f = std::sqrt(f > 0 ? f : 0);
            if (f > 1) f = 1;
            root = root * Mat4::rotateZ(f * (e.type == ENT_SPIDER ? 180.0f : 90.0f));
        }
    }
    root = root * Mat4::scale(-1, -1, 1);
    if (e.type == ENT_SLIME) {
        // RenderSlime.preRenderCallback
        float size = e.size;
        float v = (e.prevSquish + (e.squish - e.prevSquish) * partial) / (size * 0.5f + 1.0f);
        float f = 1.0f / (v + 1.0f);
        root = root * Mat4::scale(f * size, size / f, f * size);
    }
    float r = light, gr = light, b = light;
    if (e.type == ENT_WOLF && e.wet) {
        float s = e.prevShake + (e.shake - e.prevShake) * partial;
        float d = 0.75f + s / 2.0f * 0.25f;
        r *= d, gr *= d, b *= d;
    }
    if (e.type == ENT_CREEPER) {
        // RenderCreeper.preRenderCallback and the white flash while the fuse burns
        float f = (e.prevFuse + (e.fuse - e.prevFuse) * partial) / 28.0f;
        if (f < 0) f = 0;
        if (f > 1) f = 1;
        float wob = 1.0f + std::sin(f * 100.0f) * f * 0.01f;
        f *= f;
        f *= f;
        float sxz = (1.0f + f * 0.4f) * wob, sy = (1.0f + f * 0.1f) / wob;
        root = root * Mat4::scale(sxz, sy, sxz);
        float fl = (e.prevFuse + (e.fuse - e.prevFuse) * partial) / 28.0f;
        if (e.fuse > 0 && ((int)(fl * 10.0f)) % 2 == 1) r = gr = b = light + (1.0f - light) * 0.6f;
    }
    if (e.type == ENT_GHAST) {
        // RenderGhast.preRenderCallback: swells while charging a fireball
        float f = (e.prevFuse + (e.fuse - e.prevFuse) * partial) / 20.0f;
        if (f < 0) f = 0;
        f = 1.0f / (f * f * f * f * f * 2.0f + 1.0f);
        float sy = (8.0f + f) / 2.0f, sxz = (8.0f + 1.0f / f) / 2.0f;
        root = root * Mat4::scale(sxz, sy, sxz);
    }
    root = root * Mat4::translate(0, -24.0f / 16.0f - 0.0078125f, 0);
    if (e.hurtTime > 0 || e.deathTime > 0) gr *= 0.6f, b *= 0.6f;
    uint32_t color = shade(r, gr, b);
    float headPitch = e.pitch;

    Model m{root, nullptr, color};
    switch (e.type) {
        case ENT_ZOMBIE:
            m.tex = &gfx::mobs[gfx::MOB_ZOMBIE];
            biped(m, swing, amount, age, 0, headPitch, false);
            break;
        case ENT_SKELETON:
            m.tex = &gfx::mobs[gfx::MOB_SKELETON];
            biped(m, swing, amount, age, 0, headPitch, true);
            break;
        case ENT_CREEPER:
            m.tex = &gfx::mobs[gfx::MOB_CREEPER];
            creeper(m, swing, amount, 0, headPitch);
            break;
        case ENT_SPIDER:
            m.tex = &gfx::mobs[gfx::MOB_SPIDER];
            spider(m, swing, amount, 0, headPitch);
            break;
        case ENT_PIG:
            m.tex = &gfx::mobs[gfx::MOB_PIG];
            pig(m, swing, amount, 0, headPitch);
            if (e.saddled) {
                // RenderPig: ModelPig(0.5) with saddle.png as the second pass
                Model saddle{root, &gfx::mobs[gfx::MOB_SADDLE], color};
                pig(saddle, swing, amount, 0, headPitch, 0.5f);
            }
            break;
        case ENT_COW:
            m.tex = &gfx::mobs[gfx::MOB_COW];
            cow(m, swing, amount, 0, headPitch);
            break;
        case ENT_SHEEP: {
            m.tex = &gfx::mobs[gfx::MOB_SHEEP];
            sheepBody(m, swing, amount, 0, headPitch);
            if (!e.sheared) {
                const float* c = FLEECE[e.woolColor & 15];
                Model fur{root, &gfx::mobs[gfx::MOB_SHEEP_FUR], shade(r * c[0], gr * c[1], b * c[2])};
                sheepFur(fur, swing, amount, 0, headPitch);
            }
            break;
        }
        case ENT_CHICKEN: {
            m.tex = &gfx::mobs[gfx::MOB_CHICKEN];
            float wing = e.onGround ? 0.0f : (std::sin(age * 1.3f) + 1.0f) * 0.6f;
            chicken(m, swing, amount, wing, 0, headPitch);
            break;
        }
        case ENT_SLIME: {
            m.tex = &gfx::mobs[gfx::MOB_SLIME];
            slime(m);
            break;
        }
        case ENT_SQUID: {
            m.tex = &gfx::mobs[gfx::MOB_SQUID];
            squid(m, e.prevTentacle + (e.tentacle - e.prevTentacle) * partial);
            break;
        }
        case ENT_GHAST:
            m.tex = &gfx::mobs[e.fuse > 10 ? gfx::MOB_GHAST_FIRE : gfx::MOB_GHAST];
            ghast(m, age);
            break;
        case ENT_PIGZOMBIE: {
            m.tex = &gfx::mobs[gfx::MOB_PIGZOMBIE];
            Part arm;
            biped(m, swing, amount, age, 0, headPitch, false, &arm);
            // RenderBiped.renderEquippedItems: the golden sword in the right hand (a full-3D item)
            const float S = 1.0f / 16.0f;
            Mat4 h = root * Mat4::translate(arm.px * S, arm.py * S, arm.pz * S) * Mat4::rotateZ(arm.rz * RAD) *
                     Mat4::rotateY(arm.ry * RAD) * Mat4::rotateX(arm.rx * RAD) *
                     Mat4::translate(-0.0625f, 0.4375f, 0.0625f) * Mat4::translate(0, 0.1875f, 0) *
                     Mat4::scale(0.625f, -0.625f, 0.625f) * Mat4::rotateX(-100.0f) * Mat4::rotateY(45.0f) *
                     Mat4::translate(0, -0.3f, 0) * Mat4::scale(1.5f, 1.5f, 1.5f) * Mat4::rotateY(50.0f) *
                     Mat4::rotateZ(335.0f) * Mat4::translate(-0.9375f, -0.0625f, 0);
            flatItem(h, gfx::items, itemIcon(ItemStack(I_GOLD_SWORD)), color);
            break;
        }
        case ENT_WOLF:
            m.tex = &gfx::mobs[e.tamed ? gfx::MOB_WOLF_TAME : e.angry ? gfx::MOB_WOLF_ANGRY : gfx::MOB_WOLF];
            wolf(m, e, swing, amount, Game::wolfTailAngle(e), headPitch, partial);
            break;
        default: break;
    }
    (void)g;
}

// Billboard of a terrain or items.png tile, size in blocks, facing the camera.
void sprite(float x, float y, float z, float size, const gfx::Texture& t, int tile, uint32_t color, float camYaw) {
    float dx = x - g_eyeX, dy = y + size * 0.5f - g_eyeY, dz = z - g_eyeZ;
    float reach = 0.4f + size * 0.5f;
    if (dx * dx + dy * dy + dz * dz < reach * reach) return;
    float u0 = (tile & 15) / 16.0f, v0 = (tile >> 4) / 16.0f, u1 = u0 + 1 / 16.0f, v1 = v0 + 1 / 16.0f;
    gfx::ModelVertex* v = gfx::allocModel(6);
    if (!v) return;
    float h = size / 2;
    gfx::ModelVertex q[4] = {{u0, v1, color, -h, 0, 0}, {u1, v1, color, h, 0, 0},
                             {u1, v0, color, h, size, 0}, {u0, v0, color, -h, size, 0}};
    v[0] = q[0], v[1] = q[1], v[2] = q[2], v[3] = q[0], v[4] = q[2], v[5] = q[3];
    gfx::drawModel(Mat4::translate(x, y, z) * Mat4::rotateY(-camYaw), t, v, 6);
}

// Unit cube from (-0.5, 0, -0.5) with the block's face textures (RenderBlocks.renderBlockOnInventory).
void blockCube(const Mat4& m, uint8_t id, int meta, float light, float white) {
    if (modelNearEye(m, -0.5f, 0, -0.5f, 0.5f, 1, 0.5f)) return;
    gfx::ModelVertex* v = gfx::allocModel(36);
    if (!v) return;
    int k = 0;
    static const float SHADE[6] = {0.5f, 1.0f, 0.8f, 0.8f, 0.6f, 0.6f};
    static const float F[6][4][3] = {
        {{-0.5f, 0, 0.5f}, {-0.5f, 0, -0.5f}, {0.5f, 0, -0.5f}, {0.5f, 0, 0.5f}},
        {{0.5f, 1, 0.5f}, {0.5f, 1, -0.5f}, {-0.5f, 1, -0.5f}, {-0.5f, 1, 0.5f}},
        {{0.5f, 1, -0.5f}, {0.5f, 0, -0.5f}, {-0.5f, 0, -0.5f}, {-0.5f, 1, -0.5f}},
        {{-0.5f, 1, 0.5f}, {-0.5f, 0, 0.5f}, {0.5f, 0, 0.5f}, {0.5f, 1, 0.5f}},
        {{-0.5f, 1, -0.5f}, {-0.5f, 0, -0.5f}, {-0.5f, 0, 0.5f}, {-0.5f, 1, 0.5f}},
        {{0.5f, 1, 0.5f}, {0.5f, 0, 0.5f}, {0.5f, 0, -0.5f}, {0.5f, 1, -0.5f}},
    };
    for (int f = 0; f < 6; ++f) {
        int tex = blockTexture(id, meta, f);
        uint32_t tint = blockTint(nullptr, id, meta, f, 0, 0);
        float s = SHADE[f] * light;
        float tr = ((tint >> 16) & 255) / 255.0f, tg = ((tint >> 8) & 255) / 255.0f, tb = (tint & 255) / 255.0f;
        uint32_t c = shade(s * tr + (1 - s * tr) * white, s * tg + (1 - s * tg) * white, s * tb + (1 - s * tb) * white);
        float u0 = (tex & 15) / 16.0f, v0 = (tex >> 4) / 16.0f, u1 = u0 + 1 / 16.0f, v1 = v0 + 1 / 16.0f;
        const float uv[4][2] = {{u0, v0}, {u0, v1}, {u1, v1}, {u1, v0}};
        const int order[6] = {0, 1, 2, 0, 2, 3};
        for (int i : order) v[k++] = {uv[i][0], uv[i][1], c, F[f][i][0], F[f][i][1], F[f][i][2]};
    }
    gfx::drawModel(m, gfx::terrain, v, k);
}

bool cubeItem(uint16_t id) {
    if (id >= 256) return false;
    uint8_t r = g_blocks[id].render;
    return r != RENDER_CROSS && r != RENDER_TORCH && r != RENDER_LADDER && r != RENDER_CROPS && r != RENDER_DOOR &&
           r != RENDER_LEVER && r != RENDER_WIRE && r != RENDER_RAIL;
}

// Block.setBlockBoundsForItemRender as a scale of the unit cube; pistons show their front on top.
void blockItemShape(const ItemStack& s, float& sx, float& sy, float& sz, int& meta) {
    sx = sy = sz = 1;
    meta = blockIsPiston((uint8_t)s.id) ? 1 : s.damage;
    uint8_t r = g_blocks[s.id].render;
    if (r == RENDER_SLAB) sy = 0.5f;
    else if (r == RENDER_SNOW) sy = 0.125f;
    else if (blockIsPlate((uint8_t)s.id)) sy = 0.25f;
    else if (s.id == STONE_BUTTON) sx = 0.375f, sy = 0.25f, sz = 0.25f;
}

void drawItem(const Entity& e, float x, float y, float z, float partial, float light, float camYaw) {
    float age = e.age + partial;
    float bob = std::sin(age / 10.0f + e.spin) * 0.1f + 0.1f;
    const ItemStack& s = e.item;
    int copies = s.count > 20 ? 4 : s.count > 5 ? 3 : s.count > 1 ? 2 : 1;
    uint32_t c = shade(light, light, light);
    if (cubeItem(s.id)) {
        float rot = (age / 20.0f + e.spin) * RAD;
        for (int i = 0; i < copies; ++i) {
            float ox = i ? ((i * 37) % 7 - 3) * 0.05f : 0, oz = i ? ((i * 53) % 7 - 3) * 0.05f : 0;
            float oy = i ? ((i * 71) % 7 - 3) * 0.05f : 0;
            float sc = 0.25f, bx, by, bz;
            int meta;
            blockItemShape(s, bx, by, bz, meta);
            Mat4 m = Mat4::translate(x + ox, y + bob + oy, z + oz) * Mat4::rotateY(rot) *
                     Mat4::scale(sc * bx, sc * by, sc * bz);
            blockCube(m, (uint8_t)s.id, meta, light, 0);
        }
        return;
    }
    bool block = s.id < 256;
    int tile = block ? blockTexture((uint8_t)s.id, s.damage, 2) : itemIcon(s);
    if (block) {
        uint32_t t = blockTint(nullptr, (uint8_t)s.id, s.damage, 2, 0, 0);
        c = shade(((t >> 16) & 255) / 255.0f * light, ((t >> 8) & 255) / 255.0f * light, (t & 255) / 255.0f * light);
    }
    for (int i = 0; i < copies; ++i) {
        float ox = i ? ((i * 37) % 7 - 3) * 0.03f : 0, oy = i ? ((i * 71) % 7 - 3) * 0.03f : 0;
        sprite(x + ox, y + bob + oy, z, 0.5f, block ? gfx::terrain : gfx::items, tile, c, camYaw);
    }
}

void drawArrow(const Entity& e, float x, float y, float z, float light) {
    const float s = 0.05625f;
    Mat4 m = Mat4::translate(x, y, z) * Mat4::rotateY(e.yaw - 90.0f) * Mat4::rotateZ(e.pitch) * Mat4::scale(s, s, s) *
             Mat4::rotateX(45.0f);
    // The shaft is aimed at the player. Skip it once any part reaches the eye.
    if (modelNearEye(m, -8, -2, -2, 8, 2, 2)) return;
    uint32_t c = shade(light, light, light);
    const float v0 = 0, v1 = 5 / 32.0f;
    gfx::ModelVertex* v = gfx::allocModel(12);
    if (!v) return;
    gfx::ModelVertex a[4] = {{0, v0, c, -8, -2, 0}, {0.5f, v0, c, 8, -2, 0}, {0.5f, v1, c, 8, 2, 0}, {0, v1, c, -8, 2, 0}};
    gfx::ModelVertex b[4] = {{0, v0, c, -8, 0, -2}, {0.5f, v0, c, 8, 0, -2}, {0.5f, v1, c, 8, 0, 2}, {0, v1, c, -8, 0, 2}};
    v[0] = a[0], v[1] = a[1], v[2] = a[2], v[3] = a[0], v[4] = a[2], v[5] = a[3];
    v[6] = b[0], v[7] = b[1], v[8] = b[2], v[9] = b[0], v[10] = b[2], v[11] = b[3];
    gfx::drawModel(m, gfx::arrows, v, 12);
}

void drawTnt(const Entity& e, float x, float y, float z, float partial, float light) {
    float sc = 1.0f;
    float left = e.fuse - partial + 1.0f;
    if (left < 10.0f) {
        float f = 1.0f - left / 10.0f;
        if (f < 0) f = 0;
        if (f > 1) f = 1;
        f *= f;
        f *= f;
        sc = 1.0f + f * 0.3f;
    }
    float white = (e.fuse / 5) % 2 == 0 ? (1.0f - left / 100.0f) * 0.8f : 0.0f;
    if (white < 0) white = 0;
    blockCube(Mat4::translate(x, y + 0.5f, z) * Mat4::scale(sc, sc, sc) * Mat4::translate(0, -0.5f, 0), TNT, 0, light,
              white);
}

// RenderMinecart / RenderBoat. x, y, z are the interpolated position with y at the box bottom.
void drawVehicle(const Game& g, const Entity& e, float x, float y, float z, float partial, float light) {
    bool cart = e.type == ENT_MINECART;
    y += cart ? 0.35f : 0.3f;
    float yaw = e.prevYaw + (e.yaw - e.prevYaw) * partial, pitch = 0;
    if (cart) {
        float rx = x, ry = y, rz = z;
        if (minecartRailPos(g.world, rx, ry, rz)) {
            float ax, ay, az, bx, by, bz;
            if (!minecartRailPosAhead(g.world, x, y, z, 0.3f, ax, ay, az)) ax = rx, ay = ry, az = rz;
            if (!minecartRailPosAhead(g.world, x, y, z, -0.3f, bx, by, bz)) bx = rx, by = ry, bz = rz;
            x = rx, y = (ay + by) / 2, z = rz;
            float dx = bx - ax, dy = by - ay, dz = bz - az;
            float len = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (len != 0) {
                dx /= len, dy /= len, dz /= len;
                yaw = std::atan2(dz, dx) * RAD;
                pitch = std::atan(dy) * 73.0f;
            }
        }
    }
    Mat4 root = Mat4::translate(x, y, z) * Mat4::rotateY(180.0f - yaw) * Mat4::rotateZ(-pitch);
    float t = e.rollTime - partial, dmg = e.rollDamage - partial;
    if (dmg < 0) dmg = 0;
    if (t > 0) root = root * Mat4::rotateX(std::sin(t) * t * dmg / 10.0f * e.rollDir);
    if (cart && e.cartType != 0) {
        Mat4 m = root * Mat4::scale(0.75f, 0.75f, 0.75f) * Mat4::translate(0, 0.3125f, 0) * Mat4::rotateY(90.0f) *
                 Mat4::translate(0, -0.5f, 0);
        blockCube(m, e.cartType == 1 ? CHEST : FURNACE, 0, light, 0);
    }
    Model m{root * Mat4::scale(-1, -1, 1), &gfx::mobs[cart ? gfx::MOB_CART : gfx::MOB_BOAT], shade(light, light, light)};
    if (cart)
        minecart(m);
    else
        boat(m);
}

// RenderPainting: the art on the front, planks-like back and edges from the rest of kz.png, lit per 16x16 cell.
void drawPainting(const Game& g, const Entity& e) {
    const PaintingArt& a = g_paintingArt[e.woolColor < g_paintingArtCount ? e.woolColor : 0];
    Mat4 pm = Mat4::translate(e.x, e.y, e.z) * Mat4::rotateY(e.yaw) * Mat4::scale(1 / 16.0f, 1 / 16.0f, 1 / 16.0f);
    if (modelNearEye(pm, -a.w / 2.0f, -a.h / 2.0f, -0.5f, a.w / 2.0f, a.h / 2.0f, 0.5f)) return;
    int cells = (a.w / 16) * (a.h / 16);
    gfx::ModelVertex* v = gfx::allocModel(cells * 36);
    if (!v) return;
    int k = 0;
    float fx = -a.w / 2.0f, fy = -a.h / 2.0f;
    const float z0 = -0.5f, z1 = 0.5f;
    for (int i = 0; i < a.w / 16; ++i)
        for (int j = 0; j < a.h / 16; ++j) {
            float x1 = fx + (i + 1) * 16, x0 = fx + i * 16, y1 = fy + (j + 1) * 16, y0 = fy + j * 16;
            float cx = (x0 + x1) / 2 / 16.0f;
            int bx = ifloor(e.x), by = ifloor(e.y + (y0 + y1) / 2 / 16.0f), bz = ifloor(e.z);
            if (e.fuse == 0) bx = ifloor(e.x + cx);
            if (e.fuse == 1) bz = ifloor(e.z - cx);
            if (e.fuse == 2) bx = ifloor(e.x - cx);
            if (e.fuse == 3) bz = ifloor(e.z + cx);
            float l = lightBrightness(g.world.getBrightness(bx, by, bz));
            uint32_t c = shade(l, l, l);
            float u0 = (a.u + a.w - i * 16) / 256.0f, u1 = (a.u + a.w - (i + 1) * 16) / 256.0f;
            float v0 = (a.v + a.h - j * 16) / 256.0f, v1 = (a.v + a.h - (j + 1) * 16) / 256.0f;
            auto quad = [&](const gfx::ModelVertex (&q)[4]) {
                v[k++] = q[0], v[k++] = q[1], v[k++] = q[2], v[k++] = q[0], v[k++] = q[2], v[k++] = q[3];
            };
            quad({{u1, v0, c, x1, y0, z0}, {u0, v0, c, x0, y0, z0}, {u0, v1, c, x0, y1, z0}, {u1, v1, c, x1, y1, z0}});
            quad({{0.75f, 0, c, x1, y1, z1}, {0.8125f, 0, c, x0, y1, z1}, {0.8125f, 0.0625f, c, x0, y0, z1},
                  {0.75f, 0.0625f, c, x1, y0, z1}});
            const float e0 = 0.001953125f, e1 = 0.7519531f;
            quad({{0.75f, e0, c, x1, y1, z0}, {0.8125f, e0, c, x0, y1, z0}, {0.8125f, e0, c, x0, y1, z1},
                  {0.75f, e0, c, x1, y1, z1}});
            quad({{0.75f, e0, c, x1, y0, z1}, {0.8125f, e0, c, x0, y0, z1}, {0.8125f, e0, c, x0, y0, z0},
                  {0.75f, e0, c, x1, y0, z0}});
            quad({{e1, 0, c, x1, y1, z1}, {e1, 0.0625f, c, x1, y0, z1}, {e1, 0.0625f, c, x1, y0, z0},
                  {e1, 0, c, x1, y1, z0}});
            quad({{e1, 0, c, x0, y1, z0}, {e1, 0.0625f, c, x0, y0, z0}, {e1, 0.0625f, c, x0, y0, z1},
                  {e1, 0, c, x0, y1, z1}});
        }
    Mat4 m = Mat4::translate(e.x, e.y, e.z) * Mat4::rotateY(e.yaw) * Mat4::scale(1 / 16.0f, 1 / 16.0f, 1 / 16.0f);
    gfx::drawModel(m, gfx::art, v, k);
}

// RenderFish: the bobber billboard and the line back to the rod tip (first person: ahead of the camera).
void drawFish(const Game& g, const Entity& e, float x, float y, float z, float partial, const Camera& cam) {
    float fdx = x - g_eyeX, fdy = y - g_eyeY, fdz = z - g_eyeZ;
    if (fdx * fdx + fdy * fdy + fdz * fdz < 0.45f * 0.45f) return;
    const float u0 = 8 / 128.0f, u1 = 16 / 128.0f, v0 = 16 / 128.0f, v1 = 24 / 128.0f;
    gfx::ModelVertex* v = gfx::allocModel(6);
    if (!v) return;
    const uint32_t c = 0xFFFFFFFF;
    gfx::ModelVertex q[4] = {{u0, v1, c, -0.5f, -0.5f, 0}, {u1, v1, c, 0.5f, -0.5f, 0},
                             {u1, v0, c, 0.5f, 0.5f, 0}, {u0, v0, c, -0.5f, 0.5f, 0}};
    v[0] = q[0], v[1] = q[1], v[2] = q[2], v[3] = q[0], v[4] = q[2], v[5] = q[3];
    gfx::drawModel(Mat4::translate(x, y, z) * Mat4::scale(0.5f, 0.5f, 0.5f) * Mat4::rotateY(180.0f - cam.yaw) *
                       Mat4::rotateX(-cam.pitch),
                   gfx::particles, v, 6);
    if ((int)(&e - g.ents) != g.fishHook) return;
    float sp = g.swingProgress(partial);
    float sw = std::sin(std::sqrt(sp) * PI);
    float vx = -0.5f, vy = 0.03f, vz = 0.8f;
    auto rotX = [&](float a) {
        float c = std::cos(a), s = std::sin(a), ny = vy * c + vz * s, nz = vz * c - vy * s;
        vy = ny, vz = nz;
    };
    auto rotY = [&](float a) {
        float c = std::cos(a), s = std::sin(a), nx = vx * c + vz * s, nz = vz * c - vx * s;
        vx = nx, vz = nz;
    };
    rotX(-cam.pitch / RAD);
    rotY(-cam.yaw / RAD);
    rotY(sw * 0.5f);
    rotX(-sw * 0.7f);
    float sx = cam.x + vx, sy = cam.y + vy, sz = cam.z + vz;
    float ex = x, ey = y + 0.25f, ez = z;
    float dx = sx - ex, dy = sy - ey, dz = sz - ez;
    float pts[32 * 3];
    int n = 0;
    float prev[3] = {ex, ey, ez};
    for (int i = 1; i <= 16; ++i) {
        float t = i / 16.0f;
        float cur[3] = {ex + dx * t, ey + dy * (t * t + t) * 0.5f, ez + dz * t};
        for (int j = 0; j < 3; ++j) pts[n * 3 + j] = prev[j], pts[(n + 1) * 3 + j] = cur[j];
        n += 2;
        for (int j = 0; j < 3; ++j) prev[j] = cur[j];
    }
    gfx::drawLines3D(pts, n, 0xFF000000);
}

// TileEntitySignRenderer: ModelSign (board and stick) scaled by 2/3, and up to four lines of black text.
void drawSign(const Game& g, const TileEntity& t) {
    uint8_t id = g.world.get(t.x, t.y, t.z);
    if (!blockIsSign(id)) return;
    int meta = g.world.getMeta(t.x, t.y, t.z);
    const float f1 = 0.6666667f;
    Mat4 base = Mat4::translate(t.x + 0.5f, t.y + 0.75f * f1, t.z + 0.5f);
    if (id == SIGN_POST) {
        base = base * Mat4::rotateY(-(meta * 360) / 16.0f);
    } else {
        float r = meta == 2 ? 180.0f : meta == 4 ? 90.0f : meta == 5 ? -90.0f : 0.0f;
        base = base * Mat4::rotateY(-r) * Mat4::translate(0, -0.3125f, -0.4375f);
    }
    float l = lightBrightness(g.world.getBrightness(t.x, t.y, t.z));
    Model m{base * Mat4::scale(f1, -f1, -f1), &gfx::sign, shade(l, l, l)};
    m.box(Part{}, 0, 0, -12, -14, -1, 24, 12, 2);
    if (id == SIGN_POST) m.box(Part{}, 0, 14, -1, -2, -1, 2, 14, 2);
    int chars = 0;
    for (const auto& line : t.text) chars += (int)strnlen(line, 15);
    if (!chars) return;
    gfx::ModelVertex* v = gfx::allocModel(chars * 6);
    if (!v) return;
    int k = 0;
    for (int i = 0; i < 4; ++i) {
        char s[16];
        std::memcpy(s, t.text[i], 15);
        s[15] = 0;
        int w = 0;
        for (const char* c = s; *c; ++c) w += gfx::charWidth((uint8_t)*c);
        k += gfx::text3D(v + k, -w / 2.0f, i * 10 - 20.0f, s, 0xFF000000);
    }
    const float f4 = 0.016666668f * f1;
    sceGuDepthMask(GU_TRUE);
    gfx::drawModel(base * Mat4::translate(0, 0.5f * f1, 0.07f * f1) * Mat4::scale(f4, -f4, f4), gfx::font, v, k);
    sceGuDepthMask(GU_FALSE);
}

// TileEntityRendererPiston: the moved block between its old and new position; a retracting piston
// draws its head sliding back and the base standing still.
void drawPistonTile(const Game& g, const TileEntity& t, float partial) {
    uint8_t id = (uint8_t)t.burnTotal;
    if (id == AIR) return;
    float last = pistonLastSteps(t) * 0.5f, now = pistonSteps(t) * 0.5f;
    float p = last + (now - last) * partial;
    if (p >= 1.0f) return;
    static const int DX[6] = {0, 0, 0, 0, -1, 1}, DY[6] = {-1, 1, 0, 0, 0, 0}, DZ[6] = {0, 0, -1, 1, 0, 0};
    int dir = pistonFacing(t);
    if (dir > 5) return;
    float f = pistonExtending(t) ? p - 1 : 1 - p;
    static MeshBuffer buf;
    if (!buf.data && !buf.init(2048)) return;
    auto draw = [&](uint8_t bid, int bmeta, int flags, float ox, float oy, float oz) {
        buildBlock(g.world, t.x, t.y, t.z, bid, bmeta, buf, flags);
        if (buf.count == 0) return;
        // An odd vertex count leaves the next model 8 bytes off a 16-byte boundary and locks the GE.
        int n = (buf.count + 1) & ~1;
        gfx::ModelVertex* v = gfx::allocModel(n);
        if (!v) return;
        for (int i = 0; i < buf.count; ++i) {
            const MeshVertex& m = buf.data[i];
            int r = m.color & 31, gr = (m.color >> 5) & 63, b = m.color >> 11;
            v[i] = {m.u / 32768.0f, m.v / 32768.0f, gfx::rgba(r * 255 / 31, gr * 255 / 63, b * 255 / 31),
                    m.x / 1024.0f, m.y / 1024.0f, m.z / 1024.0f};
        }
        gfx::drawModel(Mat4::translate(t.x + ox, t.y + oy, t.z + oz), gfx::terrain, v, buf.count);
    };
    float ox = DX[dir] * f, oy = DY[dir] * f, oz = DZ[dir] * f;
    int meta = t.cookTime & 15;
    if (id == PISTON_HEAD && p < 0.5f) {
        draw(id, meta, PISTON_SHORT_ARM, ox, oy, oz);
    } else if (pistonSource(t) && !pistonExtending(t)) {
        draw(PISTON_HEAD, dir | (id == STICKY_PISTON ? 8 : 0), p < 0.5f ? 0 : PISTON_SHORT_ARM, ox, oy, oz);
        draw(id, meta, PISTON_FORCE_EXTENDED, 0, 0, 0);
    } else {
        draw(id, meta, 0, ox, oy, oz);
    }
}

// ItemRenderer.renderItem for flat items: the icon extruded 1/16 deep, one strip per pixel row and column.
void flatItem(const Mat4& m, const gfx::Texture& t, int tile, uint32_t c) {
    if (modelNearEye(m, 0, 0, -1.0f / 16.0f, 1, 1, 0)) return;
    const float th = 1.0f / 16.0f, px = 1.0f / 512.0f;
    float u0 = (tile & 15) / 16.0f, v0 = (tile >> 4) / 16.0f;
    float u1 = u0 + 15.99f / 256.0f, v1 = v0 + 15.99f / 256.0f;
    gfx::ModelVertex* v = gfx::allocModel(66 * 6);
    if (!v) return;
    int k = 0;
    auto quad = [&](const gfx::ModelVertex (&q)[4]) {
        v[k++] = q[0], v[k++] = q[1], v[k++] = q[2], v[k++] = q[0], v[k++] = q[2], v[k++] = q[3];
    };
    quad({{u1, v1, c, 0, 0, 0}, {u0, v1, c, 1, 0, 0}, {u0, v0, c, 1, 1, 0}, {u1, v0, c, 0, 1, 0}});
    quad({{u1, v0, c, 0, 1, -th}, {u0, v0, c, 1, 1, -th}, {u0, v1, c, 1, 0, -th}, {u1, v1, c, 0, 0, -th}});
    uint32_t cs = shade(((c & 255) / 255.0f) * 0.8f, (((c >> 8) & 255) / 255.0f) * 0.8f,
                        (((c >> 16) & 255) / 255.0f) * 0.8f);
    for (int i = 0; i < 16; ++i) {
        float f = i / 16.0f;
        float u = u1 + (u0 - u1) * f - px, vv = v1 + (v0 - v1) * f - px;
        float a = f, b = f + th;
        quad({{u, v1, cs, a, 0, -th}, {u, v1, cs, a, 0, 0}, {u, v0, cs, a, 1, 0}, {u, v0, cs, a, 1, -th}});
        quad({{u, v0, cs, b, 1, -th}, {u, v0, cs, b, 1, 0}, {u, v1, cs, b, 0, 0}, {u, v1, cs, b, 0, -th}});
        quad({{u1, vv, cs, 0, b, 0}, {u0, vv, cs, 1, b, 0}, {u0, vv, cs, 1, b, -th}, {u1, vv, cs, 0, b, -th}});
        quad({{u0, vv, cs, 1, a, 0}, {u1, vv, cs, 0, a, 0}, {u1, vv, cs, 0, a, -th}, {u0, vv, cs, 1, a, -th}});
    }
    gfx::drawModel(m, t, v, k);
}

}  // namespace

void HandState::tick(const Game& g) {
    prevEquip = equip;
    const ItemStack& cur = g.inv.main[g.inv.selected];
    bool same = slot == g.inv.selected && cur.empty() == item.empty() &&
                (cur.empty() || (cur.id == item.id && (itemDef(cur.id).maxDamage > 0 || cur.damage == item.damage)));
    if (!cur.empty() && !item.empty() && cur.sameItem(item)) {
        item = cur;
        same = true;
    }
    if (cur.empty() && item.empty()) same = true;
    float d = (same ? 1.0f : 0.0f) - equip;
    if (d < -0.4f) d = -0.4f;
    if (d > 0.4f) d = 0.4f;
    equip += d;
    if (equip < 0.1f) {
        item = cur.empty() ? ItemStack() : cur;
        slot = g.inv.selected;
    }
    if (resets != g.handResets) {
        resets = g.handResets;
        equip = 0;
    }
}

void drawHand(Game& g, const Camera& cam, float partial, const HandState& h) {
    g_lightFloor = g.world.dimension < 0 ? 0.1f : 0.05f;
    float equip = h.prevEquip + (h.equip - h.prevEquip) * partial;
    float light = lightBrightness(g.world.getBrightness(ifloor(cam.x), ifloor(cam.y), ifloor(cam.z)));
    float sp = g.swingProgress(partial);
    float s1 = std::sin(sp * PI), s2 = std::sin(std::sqrt(sp) * PI), s3 = std::sin(sp * sp * PI);
    float s4 = std::sin(std::sqrt(sp) * PI * 2.0f);

    Mat4 proj = Mat4::perspective(70.0f, (float)gfx::SCREEN_W / gfx::SCREEN_H, 0.2f, 10.0f);
    gfx::begin3D(proj, cam.effects, false, 0, 0, 0);
    sceGuClear(GU_DEPTH_BUFFER_BIT);
    sceGuDisable(GU_CULL_FACE);

    const ItemStack& s = h.item;
    if (!s.empty()) {
        Mat4 m = Mat4::translate(-s2 * 0.4f, s4 * 0.2f, -s1 * 0.2f) *
                 Mat4::translate(0.7f * 0.8f, -0.65f * 0.8f - (1.0f - equip) * 0.6f, -0.9f * 0.8f) *
                 Mat4::rotateY(45.0f) * Mat4::rotateY(-s3 * 20.0f) * Mat4::rotateZ(-s2 * 20.0f) *
                 Mat4::rotateX(-s2 * 80.0f) * Mat4::scale(0.4f, 0.4f, 0.4f);
        if (cubeItem(s.id)) {
            float bx, by, bz;
            int meta;
            blockItemShape(s, bx, by, bz, meta);
            blockCube(m * Mat4::translate(0, -0.5f, 0) * Mat4::scale(bx, by, bz), (uint8_t)s.id, meta, light, 0);
        } else {
            bool block = s.id < 256;
            int tile = block ? blockTexture((uint8_t)s.id, s.damage, 2) : itemIcon(s);
            // EntityPlayer.getItemIcon: the cast rod; Item.shouldRotateAroundWhenRendering
            if (s.id == I_FISHING_ROD) {
                if (g.fishHook >= 0) tile += 16;
                m = m * Mat4::rotateY(180.0f);
            }
            uint32_t t = block ? blockTint(nullptr, (uint8_t)s.id, s.damage, 2, 0, 0) : 0xFFFFFF;
            uint32_t c = shade(((t >> 16) & 255) / 255.0f * light, ((t >> 8) & 255) / 255.0f * light,
                               (t & 255) / 255.0f * light);
            m = m * Mat4::translate(0, -0.3f, 0) * Mat4::scale(1.5f, 1.5f, 1.5f) * Mat4::rotateY(50.0f) *
                Mat4::rotateZ(335.0f) * Mat4::translate(-0.9375f, -0.0625f, 0);
            flatItem(m, block ? gfx::terrain : gfx::items, tile, c);
        }
    } else {
        // RenderPlayer.drawFirstPersonArm: the right arm of ModelBiped at rest.
        Mat4 m = Mat4::translate(-s2 * 0.3f, s4 * 0.4f, -s1 * 0.4f) *
                 Mat4::translate(0.8f * 0.8f, -0.75f * 0.8f - (1.0f - equip) * 0.6f, -0.9f * 0.8f) *
                 Mat4::rotateY(45.0f) * Mat4::rotateY(s2 * 70.0f) * Mat4::rotateZ(-s3 * 20.0f) *
                 Mat4::translate(-1.0f, 3.6f, 3.5f) * Mat4::rotateZ(120.0f) * Mat4::rotateX(200.0f) *
                 Mat4::rotateY(-135.0f) * Mat4::translate(5.6f, 0, 0);
        Model arm{m, &gfx::mobs[gfx::MOB_PLAYER], shade(light, light, light)};
        arm.box(Part{-5, 2, 0, 0, 0, 0.1f}, 40, 16, -3, -2, -2, 4, 12, 4);
    }
    sceGuEnable(GU_CULL_FACE);
}

void drawEntities(Game& g, const Camera& cam, float partial) {
    g_lightFloor = g.world.dimension < 0 ? 0.1f : 0.05f;
    g_eyeX = cam.x, g_eyeY = cam.y, g_eyeZ = cam.z;
    float maxD = (g.renderDistance * 16.0f + 8.0f);
    float fx = -std::sin(cam.yaw / RAD) * std::cos(cam.pitch / RAD), fy = -std::sin(cam.pitch / RAD),
          fz = std::cos(cam.yaw / RAD) * std::cos(cam.pitch / RAD);
    sceGuDisable(GU_CULL_FACE);
    sceGuEnable(GU_ALPHA_TEST);
    // Nearest first, so a full display list drops distant mobs and keeps the one in front of the player.
    struct Cand {
        int index;
        float d2;
    };
    Cand cand[Game::MAX_ENTITIES];
    int nc = 0;
    for (int i = 0; i < Game::MAX_ENTITIES; ++i) {
        const Entity& e = g.ents[i];
        if (e.type == ENT_NONE) continue;
        float x = e.prevX + (e.x - e.prevX) * partial, y = e.prevY + (e.y - e.prevY) * partial,
              z = e.prevZ + (e.z - e.prevZ) * partial;
        float dx = x - cam.x, dy = y + e.height * 0.5f - cam.y, dz = z - cam.z;
        float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 > maxD * maxD) continue;
        float r = e.width + e.height + (e.type == ENT_PAINTING ? 3.0f : 0.0f);
        if (dx * fx + dy * fy + dz * fz < -r) continue;
        if (!g.world.isLoaded(ifloor(x), ifloor(z))) continue;
        cand[nc++] = {i, d2};
    }
    for (int a = 1; a < nc; ++a) {
        Cand key = cand[a];
        int b = a;
        while (b > 0 && cand[b - 1].d2 > key.d2) cand[b] = cand[b - 1], --b;
        cand[b] = key;
    }
    for (int n = 0; n < nc; ++n) {
        if (gfx::listBlocked()) break;
        const Entity& e = g.ents[cand[n].index];
        float x = e.prevX + (e.x - e.prevX) * partial, y = e.prevY + (e.y - e.prevY) * partial,
              z = e.prevZ + (e.z - e.prevZ) * partial;
        float light = lightBrightness(g.world.getBrightness(ifloor(x), ifloor(y + e.height * 0.66f), ifloor(z)));
        switch (e.type) {
            case ENT_ITEM: drawItem(e, x, y, z, partial, light, cam.yaw + 180.0f); break;
            case ENT_ARROW: drawArrow(e, x, y, z, light); break;
            case ENT_TNT: drawTnt(e, x, y, z, partial, light); break;
            case ENT_SNOWBALL:
            case ENT_EGG: {
                // RenderSnowball: the item icon as a half-size billboard
                ItemStack icon(e.type == ENT_SNOWBALL ? I_SNOWBALL : I_EGG);
                sprite(x, y - 0.125f, z, 0.5f, gfx::items, itemIcon(icon), shade(light, light, light),
                       cam.yaw + 180.0f);
                break;
            }
            case ENT_FIREBALL: {
                // RenderFireball: the snowball icon at twice the size, facing the camera
                ItemStack icon(I_SNOWBALL);
                float fdx = x - g_eyeX, fdy = y - g_eyeY, fdz = z - g_eyeZ;
                if (fdx * fdx + fdy * fdy + fdz * fdz < 1.2f * 1.2f) break;
                int tile = itemIcon(icon);
                float u0 = (tile & 15) / 16.0f, v0 = (tile >> 4) / 16.0f, u1 = u0 + 1 / 16.0f, v1 = v0 + 1 / 16.0f;
                uint32_t c = shade(light, light, light);
                gfx::ModelVertex* v = gfx::allocModel(6);
                if (!v) break;
                gfx::ModelVertex q[4] = {{u0, v1, c, -0.5f, -0.25f, 0}, {u1, v1, c, 0.5f, -0.25f, 0},
                                         {u1, v0, c, 0.5f, 0.75f, 0}, {u0, v0, c, -0.5f, 0.75f, 0}};
                v[0] = q[0], v[1] = q[1], v[2] = q[2], v[3] = q[0], v[4] = q[2], v[5] = q[3];
                gfx::drawModel(Mat4::translate(x, y, z) * Mat4::scale(2, 2, 2) * Mat4::rotateY(180.0f - cam.yaw) *
                                   Mat4::rotateX(-cam.pitch),
                               gfx::items, v, 6);
                break;
            }
            case ENT_FISH: drawFish(g, e, x, y, z, partial, cam); break;
            case ENT_PAINTING: drawPainting(g, e); break;
            case ENT_MINECART:
            case ENT_BOAT: drawVehicle(g, e, x, y, z, partial, light); break;
            default: drawMob(g, e, x, y, z, partial, light); break;
        }
    }
    // TileEntitySignRenderer, within 64 blocks.
    {
        int pcx = ifloor(cam.x) >> 4, pcz = ifloor(cam.z) >> 4, rd = g.renderDistance;
        for (int cx = pcx - rd; cx <= pcx + rd; ++cx)
            for (int cz = pcz - rd; cz <= pcz + rd; ++cz) {
                const Chunk* c = g.world.chunk(cx, cz);
                if (!c) continue;
                for (const TileEntity* t = c->tiles; t; t = t->next) {
                    if (t->type != TILE_SIGN && t->type != TILE_PISTON) continue;
                    float dx = t->x + 0.5f - cam.x, dy = t->y + 0.5f - cam.y, dz = t->z + 0.5f - cam.z;
                    if (dx * dx + dy * dy + dz * dz > 64.0f * 64.0f || dx * fx + dy * fy + dz * fz < -2.0f) continue;
                    if (t->type == TILE_PISTON)
                        drawPistonTile(g, *t, partial);
                    else
                        drawSign(g, *t);
                }
            }
    }
    // TileEntityMobSpawnerRenderer: a small spinning copy of the spawned mob.
    int pcx = ifloor(cam.x) >> 4, pcz = ifloor(cam.z) >> 4, rd = g.renderDistance;
    for (int cx = pcx - rd; cx <= pcx + rd; ++cx)
        for (int cz = pcz - rd; cz <= pcz + rd; ++cz) {
            const Chunk* c = g.world.chunk(cx, cz);
            if (!c) continue;
            for (const TileEntity* t = c->tiles; t; t = t->next) {
                if (t->type != TILE_SPAWNER || t->burnTotal < ENT_ZOMBIE || !entityIsMob((uint8_t)t->burnTotal)) continue;
                float dx = t->x + 0.5f - cam.x, dy = t->y + 0.5f - cam.y, dz = t->z + 0.5f - cam.z;
                if (dx * dx + dy * dy + dz * dz > 64.0f * 64.0f || dx * fx + dy * fy + dz * fz < -1.0f) continue;
                Entity mob;
                mob.type = (uint8_t)t->burnTotal;
                float spin = t->prevSpin + (t->spin - t->prevSpin) * partial;
                Mat4 pre = Mat4::translate(t->x + 0.5f, t->y + 0.4f, t->z + 0.5f) * Mat4::rotateY(spin * 10.0f) *
                           Mat4::rotateX(-30.0f) * Mat4::translate(0, -0.4f, 0) * Mat4::scale(0.4375f, 0.4375f, 0.4375f);
                float light = lightBrightness(g.world.getBrightness(t->x, t->y, t->z));
                drawMob(g, mob, 0, 0, 0, partial, light, pre);
            }
        }
    sceGuEnable(GU_CULL_FACE);
}
