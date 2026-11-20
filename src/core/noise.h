#pragma once
#include "config.h"
#include "jrandom.h"

// NoiseGeneratorPerlin
struct PerlinNoise {
    int perm[512];
    real xo, yo, zo;

    void init(JRandom& r);
    real noise(real x, real y, real z) const;
    void add(real* out, real x, real y, real z, int nx, int ny, int nz,
             real sx, real sy, real sz, real amp) const;
};

// NoiseGeneratorOctaves
struct PerlinOctaves {
    PerlinNoise oct[16];
    int count = 0;

    void init(JRandom& r, int n);
    real noise2(real x, real z) const;
    void generate(real* out, real x, real y, real z, int nx, int ny, int nz,
                  real sx, real sy, real sz) const;
    void generate2(real* out, int x, int z, int nx, int nz, real sx, real sz) const {
        generate(out, (real)x, (real)10.0, (real)z, nx, 1, nz, sx, (real)1.0, sz);
    }
};

// NoiseGenerator2 (simplex)
struct SimplexNoise {
    int perm[512];
    real xo, yo, zo;

    void init(JRandom& r);
    void add(real* out, real x, real z, int nx, int nz, real sx, real sz, real amp) const;
};

// NoiseGeneratorOctaves2
struct SimplexOctaves {
    SimplexNoise oct[4];
    int count = 0;

    void init(JRandom& r, int n);
    void generate(real* out, real x, real z, int nx, int nz, real sx, real sz,
                  real freqMul, real ampMul = (real)0.5) const;
};
