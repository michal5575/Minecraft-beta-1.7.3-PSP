#pragma once
#include <cmath>

// Column-major 4x4 matrix, memory-compatible with ScePspFMatrix4.
struct Mat4 {
    float m[16];

    static Mat4 identity() {
        Mat4 r{};
        r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
        return r;
    }

    static Mat4 perspective(float fovyDeg, float aspect, float zn, float zf) {
        Mat4 r{};
        float f = 1.0f / std::tan(fovyDeg * 3.14159265f / 360.0f);
        r.m[0] = f / aspect;
        r.m[5] = f;
        r.m[10] = (zf + zn) / (zn - zf);
        r.m[11] = -1.0f;
        r.m[14] = 2.0f * zf * zn / (zn - zf);
        return r;
    }

    static Mat4 translate(float x, float y, float z) {
        Mat4 r = identity();
        r.m[12] = x;
        r.m[13] = y;
        r.m[14] = z;
        return r;
    }

    static Mat4 rotateX(float deg) {
        Mat4 r = identity();
        float a = deg * 3.14159265f / 180.0f, c = std::cos(a), s = std::sin(a);
        r.m[5] = c;
        r.m[6] = s;
        r.m[9] = -s;
        r.m[10] = c;
        return r;
    }

    static Mat4 rotateY(float deg) {
        Mat4 r = identity();
        float a = deg * 3.14159265f / 180.0f, c = std::cos(a), s = std::sin(a);
        r.m[0] = c;
        r.m[2] = -s;
        r.m[8] = s;
        r.m[10] = c;
        return r;
    }

    static Mat4 rotateZ(float deg) {
        Mat4 r = identity();
        float a = deg * 3.14159265f / 180.0f, c = std::cos(a), s = std::sin(a);
        r.m[0] = c;
        r.m[1] = s;
        r.m[4] = -s;
        r.m[5] = c;
        return r;
    }

    static Mat4 rotate(float deg, float x, float y, float z) {
        float len = std::sqrt(x * x + y * y + z * z);
        x /= len;
        y /= len;
        z /= len;
        float a = deg * 3.14159265f / 180.0f, c = std::cos(a), s = std::sin(a), t = 1.0f - c;
        Mat4 r = identity();
        r.m[0] = x * x * t + c;
        r.m[1] = y * x * t + z * s;
        r.m[2] = x * z * t - y * s;
        r.m[4] = x * y * t - z * s;
        r.m[5] = y * y * t + c;
        r.m[6] = y * z * t + x * s;
        r.m[8] = x * z * t + y * s;
        r.m[9] = y * z * t - x * s;
        r.m[10] = z * z * t + c;
        return r;
    }

    static Mat4 scale(float x, float y, float z) {
        Mat4 r = identity();
        r.m[0] = x;
        r.m[5] = y;
        r.m[10] = z;
        return r;
    }

    Mat4 operator*(const Mat4& b) const {
        Mat4 r{};
        for (int c = 0; c < 4; ++c)
            for (int rr = 0; rr < 4; ++rr) {
                float s = 0;
                for (int k = 0; k < 4; ++k) s += m[k * 4 + rr] * b.m[c * 4 + k];
                r.m[c * 4 + rr] = s;
            }
        return r;
    }
};

struct Frustum {
    float p[6][4];

    void fromMatrix(const Mat4& c) {
        const float* m = c.m;
        auto row = [&](int r, int i) { return m[i * 4 + r]; };
        for (int i = 0; i < 4; ++i) {
            p[0][i] = row(3, i) + row(0, i);
            p[1][i] = row(3, i) - row(0, i);
            p[2][i] = row(3, i) + row(1, i);
            p[3][i] = row(3, i) - row(1, i);
            p[4][i] = row(3, i) + row(2, i);
            p[5][i] = row(3, i) - row(2, i);
        }
    }

    bool boxVisible(float x0, float y0, float z0, float x1, float y1, float z1) const {
        for (int i = 0; i < 6; ++i) {
            float x = p[i][0] >= 0 ? x1 : x0;
            float y = p[i][1] >= 0 ? y1 : y0;
            float z = p[i][2] >= 0 ? z1 : z0;
            if (p[i][0] * x + p[i][1] * y + p[i][2] * z + p[i][3] < 0) return false;
        }
        return true;
    }
};
