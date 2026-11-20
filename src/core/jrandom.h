#pragma once
#include <cstdint>

// Bit-exact port of java.util.Random, so world seeds behave like Beta 1.7.3.
class JRandom {
public:
    explicit JRandom(int64_t seed = 0) { setSeed(seed); }

    void setSeed(int64_t seed) { seed_ = ((uint64_t)seed ^ 0x5DEECE66DULL) & MASK; }

    int32_t next(int bits) {
        seed_ = (seed_ * 0x5DEECE66DULL + 0xBULL) & MASK;
        return (int32_t)(uint32_t)(seed_ >> (48 - bits));
    }

    int32_t nextInt() { return next(32); }

    int32_t nextInt(int32_t n) {
        if ((n & -n) == n)
            return (int32_t)(((int64_t)n * (int64_t)next(31)) >> 31);
        int32_t bits, val;
        do {
            bits = next(31);
            val = bits % n;
        } while ((int32_t)((uint32_t)bits - (uint32_t)val + (uint32_t)(n - 1)) < 0);
        return val;
    }

    int64_t nextLong() {
        uint64_t hi = (uint64_t)(int64_t)next(32);
        int64_t lo = next(32);
        return (int64_t)((hi << 32) + (uint64_t)lo);
    }

    double nextDouble() {
        return (double)(((int64_t)next(26) << 27) + next(27)) * (1.0 / (double)(1LL << 53));
    }

    float nextFloat() { return (float)next(24) / (float)(1 << 24); }

private:
    static constexpr uint64_t MASK = (1ULL << 48) - 1;
    uint64_t seed_;
};

inline int64_t jmul(int64_t a, int64_t b) { return (int64_t)((uint64_t)a * (uint64_t)b); }
inline int64_t jadd(int64_t a, int64_t b) { return (int64_t)((uint64_t)a + (uint64_t)b); }
