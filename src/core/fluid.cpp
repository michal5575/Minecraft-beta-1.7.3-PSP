#include "fluid.h"

#include <cmath>

float fluidCornerHeight(const World& w, int x, int y, int z, bool water) {
    int n = 0;
    float sum = 0;
    for (int i = 0; i < 4; ++i) {
        int xx = x - (i & 1), zz = z - ((i >> 1) & 1);
        if (fluidSame(w.get(xx, y + 1, zz), water)) return 1.0f;
        uint8_t id = w.get(xx, y, zz);
        if (!fluidSame(id, water)) {
            if (!blockMatSolid(id)) {
                sum += 1.0f;
                ++n;
            }
        } else {
            int m = w.getMeta(xx, y, zz);
            if (m >= 8 || m == 0) {
                sum += fluidHeightPercent(m) * 10.0f;
                n += 10;
            }
            sum += fluidHeightPercent(m);
            ++n;
        }
    }
    if (n == 0) return 0;
    return 1.0f - sum / (float)n;
}

void fluidFlow(const World& w, int x, int y, int z, bool water, float& fx, float& fy, float& fz) {
    fx = fy = fz = 0;
    int self = fluidEffectiveDecay(w, x, y, z, water);
    static const int D[4][2] = {{-1, 0}, {0, -1}, {1, 0}, {0, 1}};
    for (int i = 0; i < 4; ++i) {
        int nx = x + D[i][0], nz = z + D[i][1];
        int d = fluidEffectiveDecay(w, nx, y, nz, water);
        if (d < 0) {
            if (!blockMatSolid(w.get(nx, y, nz))) {
                d = fluidEffectiveDecay(w, nx, y - 1, nz, water);
                if (d >= 0) {
                    int k = d - (self - 8);
                    fx += D[i][0] * k;
                    fz += D[i][1] * k;
                }
            }
        } else {
            int k = d - self;
            fx += D[i][0] * k;
            fz += D[i][1] * k;
        }
    }
    if (w.getMeta(x, y, z) >= 8) {
        auto side = [&](int sx, int sy, int sz) {
            uint8_t id = w.get(sx, sy, sz);
            return !fluidSame(id, water) && id != ICE && blockMatSolid(id);
        };
        if (side(x, y, z - 1) || side(x, y, z + 1) || side(x - 1, y, z) || side(x + 1, y, z) ||
            side(x, y + 1, z - 1) || side(x, y + 1, z + 1) || side(x - 1, y + 1, z) || side(x + 1, y + 1, z)) {
            float l = std::sqrt(fx * fx + fz * fz);
            if (l > 1e-4f) fx /= l, fz /= l;
            fy -= 6.0f;
        }
    }
    float l = std::sqrt(fx * fx + fy * fy + fz * fz);
    if (l < 1e-4f) {
        fx = fy = fz = 0;
        return;
    }
    fx /= l, fy /= l, fz /= l;
}
