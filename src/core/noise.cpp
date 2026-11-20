#include "noise.h"

#include <cmath>
#include <cstring>

namespace {

inline real lerp(real t, real a, real b) { return a + t * (b - a); }

inline real fade(real t) { return t * t * t * (t * (t * (real)6.0 - (real)15.0) + (real)10.0); }

inline real grad2(int h, real x, real z) {
    h &= 15;
    real a = (real)(1 - ((h & 8) >> 3)) * x;
    real b = h < 4 ? (real)0.0 : (h != 12 && h != 14 ? z : x);
    return ((h & 1) == 0 ? a : -a) + ((h & 2) == 0 ? b : -b);
}

inline real grad(int h, real x, real y, real z) {
    h &= 15;
    real a = h < 8 ? x : y;
    real b = h < 4 ? y : (h != 12 && h != 14 ? z : x);
    return ((h & 1) == 0 ? a : -a) + ((h & 2) == 0 ? b : -b);
}

inline int floorInt(real v) {
    int i = (int)v;
    return v < (real)i ? i - 1 : i;
}

void initPerm(int* perm, JRandom& r) {
    for (int i = 0; i < 256; ++i) perm[i] = i;
    for (int i = 0; i < 256; ++i) {
        int j = r.nextInt(256 - i) + i;
        int t = perm[i];
        perm[i] = perm[j];
        perm[j] = t;
        perm[i + 256] = perm[i];
    }
}

}  // namespace

void PerlinNoise::init(JRandom& r) {
    xo = (real)(r.nextDouble() * 256.0);
    yo = (real)(r.nextDouble() * 256.0);
    zo = (real)(r.nextDouble() * 256.0);
    initPerm(perm, r);
}

real PerlinNoise::noise(real x, real y, real z) const {
    x += xo;
    y += yo;
    z += zo;
    int ix = floorInt(x), iy = floorInt(y), iz = floorInt(z);
    int X = ix & 255, Y = iy & 255, Z = iz & 255;
    x -= (real)ix;
    y -= (real)iy;
    z -= (real)iz;
    real u = fade(x), v = fade(y), w = fade(z);
    int A = perm[X] + Y, AA = perm[A] + Z, AB = perm[A + 1] + Z;
    int B = perm[X + 1] + Y, BA = perm[B] + Z, BB = perm[B + 1] + Z;
    return lerp(w,
                lerp(v, lerp(u, grad(perm[AA], x, y, z), grad(perm[BA], x - 1, y, z)),
                     lerp(u, grad(perm[AB], x, y - 1, z), grad(perm[BB], x - 1, y - 1, z))),
                lerp(v, lerp(u, grad(perm[AA + 1], x, y, z - 1), grad(perm[BA + 1], x - 1, y, z - 1)),
                     lerp(u, grad(perm[AB + 1], x, y - 1, z - 1), grad(perm[BB + 1], x - 1, y - 1, z - 1))));
}

void PerlinNoise::add(real* out, real x0, real y0, real z0, int nx, int ny, int nz,
                      real sx, real sy, real sz, real amp) const {
    real inv = (real)1.0 / amp;
    if (ny == 1) {
        int idx = 0;
        for (int i = 0; i < nx; ++i) {
            real x = (x0 + (real)i) * sx + xo;
            int ix = floorInt(x);
            int X = ix & 255;
            x -= (real)ix;
            real u = fade(x);
            for (int k = 0; k < nz; ++k) {
                real z = (z0 + (real)k) * sz + zo;
                int iz = floorInt(z);
                int Z = iz & 255;
                z -= (real)iz;
                real w = fade(z);
                int A = perm[X] + 0;
                int AA = perm[A] + Z;
                int B = perm[X + 1] + 0;
                int BA = perm[B] + Z;
                real a = lerp(u, grad2(perm[AA], x, z), grad(perm[BA], x - 1, 0, z));
                real b = lerp(u, grad(perm[AA + 1], x, 0, z - 1), grad(perm[BA + 1], x - 1, 0, z - 1));
                out[idx++] += lerp(w, a, b) * inv;
            }
        }
        return;
    }

    int idx = 0;
    int lastY = -1;
    real a1 = 0, a2 = 0, b1 = 0, b2 = 0;
    for (int i = 0; i < nx; ++i) {
        real x = (x0 + (real)i) * sx + xo;
        int ix = floorInt(x);
        int X = ix & 255;
        x -= (real)ix;
        real u = fade(x);
        for (int k = 0; k < nz; ++k) {
            real z = (z0 + (real)k) * sz + zo;
            int iz = floorInt(z);
            int Z = iz & 255;
            z -= (real)iz;
            real w = fade(z);
            for (int j = 0; j < ny; ++j) {
                real y = (y0 + (real)j) * sy + yo;
                int iy = floorInt(y);
                int Y = iy & 255;
                y -= (real)iy;
                real v = fade(y);
                if (j == 0 || Y != lastY) {
                    lastY = Y;
                    int A = perm[X] + Y, AA = perm[A] + Z, AB = perm[A + 1] + Z;
                    int B = perm[X + 1] + Y, BA = perm[B] + Z, BB = perm[B + 1] + Z;
                    a1 = lerp(u, grad(perm[AA], x, y, z), grad(perm[BA], x - 1, y, z));
                    a2 = lerp(u, grad(perm[AB], x, y - 1, z), grad(perm[BB], x - 1, y - 1, z));
                    b1 = lerp(u, grad(perm[AA + 1], x, y, z - 1), grad(perm[BA + 1], x - 1, y, z - 1));
                    b2 = lerp(u, grad(perm[AB + 1], x, y - 1, z - 1), grad(perm[BB + 1], x - 1, y - 1, z - 1));
                }
                real c1 = lerp(v, a1, a2);
                real c2 = lerp(v, b1, b2);
                out[idx++] += lerp(w, c1, c2) * inv;
            }
        }
    }
}

void PerlinOctaves::init(JRandom& r, int n) {
    count = n;
    for (int i = 0; i < n; ++i) oct[i].init(r);
}

real PerlinOctaves::noise2(real x, real z) const {
    real sum = 0, f = 1;
    for (int i = 0; i < count; ++i) {
        sum += oct[i].noise(x * f, z * f, 0) / f;
        f /= (real)2.0;
    }
    return sum;
}

void PerlinOctaves::generate(real* out, real x, real y, real z, int nx, int ny, int nz,
                             real sx, real sy, real sz) const {
    std::memset(out, 0, sizeof(real) * nx * ny * nz);
    real f = 1;
    for (int i = 0; i < count; ++i) {
        oct[i].add(out, x, y, z, nx, ny, nz, sx * f, sy * f, sz * f, f);
        f /= (real)2.0;
    }
}

namespace {
const int GRAD3[12][3] = {{1, 1, 0}, {-1, 1, 0}, {1, -1, 0}, {-1, -1, 0}, {1, 0, 1}, {-1, 0, 1},
                          {1, 0, -1}, {-1, 0, -1}, {0, 1, 1}, {0, -1, 1}, {0, 1, -1}, {0, -1, -1}};
const real F2 = (real)(0.5 * (1.7320508075688772 - 1.0));
const real G2 = (real)((3.0 - 1.7320508075688772) / 6.0);

inline int wrap(real v) { return v > 0 ? (int)v : (int)v - 1; }
inline real dot2(const int* g, real x, real y) { return (real)g[0] * x + (real)g[1] * y; }
}  // namespace

void SimplexNoise::init(JRandom& r) {
    xo = (real)(r.nextDouble() * 256.0);
    yo = (real)(r.nextDouble() * 256.0);
    zo = (real)(r.nextDouble() * 256.0);
    initPerm(perm, r);
}

void SimplexNoise::add(real* out, real x0, real z0, int nx, int nz, real sx, real sz, real amp) const {
    int idx = 0;
    for (int i = 0; i < nx; ++i) {
        real x = (x0 + (real)i) * sx + xo;
        for (int k = 0; k < nz; ++k) {
            real y = (z0 + (real)k) * sz + yo;
            real s = (x + y) * F2;
            int ii = wrap(x + s);
            int jj = wrap(y + s);
            real t = (real)(ii + jj) * G2;
            real X0 = (real)ii - t;
            real Y0 = (real)jj - t;
            real dx0 = x - X0;
            real dy0 = y - Y0;
            int i1, j1;
            if (dx0 > dy0) {
                i1 = 1;
                j1 = 0;
            } else {
                i1 = 0;
                j1 = 1;
            }
            real dx1 = dx0 - (real)i1 + G2;
            real dy1 = dy0 - (real)j1 + G2;
            real dx2 = dx0 - (real)1.0 + (real)2.0 * G2;
            real dy2 = dy0 - (real)1.0 + (real)2.0 * G2;
            int ia = ii & 255;
            int ja = jj & 255;
            int g0 = perm[ia + perm[ja]] % 12;
            int g1 = perm[ia + i1 + perm[ja + j1]] % 12;
            int g2 = perm[ia + 1 + perm[ja + 1]] % 12;
            real n0, n1, n2;
            real t0 = (real)0.5 - dx0 * dx0 - dy0 * dy0;
            if (t0 < 0) {
                n0 = 0;
            } else {
                t0 *= t0;
                n0 = t0 * t0 * dot2(GRAD3[g0], dx0, dy0);
            }
            real t1 = (real)0.5 - dx1 * dx1 - dy1 * dy1;
            if (t1 < 0) {
                n1 = 0;
            } else {
                t1 *= t1;
                n1 = t1 * t1 * dot2(GRAD3[g1], dx1, dy1);
            }
            real t2 = (real)0.5 - dx2 * dx2 - dy2 * dy2;
            if (t2 < 0) {
                n2 = 0;
            } else {
                t2 *= t2;
                n2 = t2 * t2 * dot2(GRAD3[g2], dx2, dy2);
            }
            out[idx++] += (real)70.0 * (n0 + n1 + n2) * amp;
        }
    }
}

void SimplexOctaves::init(JRandom& r, int n) {
    count = n;
    for (int i = 0; i < n; ++i) oct[i].init(r);
}

void SimplexOctaves::generate(real* out, real x, real z, int nx, int nz, real sx, real sz,
                              real freqMul, real ampMul) const {
    sx /= (real)1.5;
    sz /= (real)1.5;
    std::memset(out, 0, sizeof(real) * nx * nz);
    real a = 1, f = 1;
    for (int i = 0; i < count; ++i) {
        oct[i].add(out, x, z, nx, nz, sx * f, sz * f, (real)0.55 / a);
        f *= freqMul;
        a *= ampMul;
    }
}
