#pragma once
#include <cstdint>
#include <cstdlib>

#include "world.h"

// Matches GU_TEXTURE_16BIT | GU_COLOR_5650 | GU_VERTEX_16BIT. Texture coordinates
// are normalized (32768 = 1.0); positions are section-local in 1/1024 block units
// (the renderer scales the model matrix by 32 to undo the 1/32768 normalization).
struct MeshVertex {
    uint16_t u, v;
    uint16_t color;
    int16_t x, y, z;
};
static_assert(sizeof(MeshVertex) == 12, "vertex layout must match the GE vertex type");

// Matches GU_TEXTURE_8BIT | GU_COLOR_5650 | GU_VERTEX_8BIT, used for full opaque cube faces
// (integer corners, whole 16px tiles). Texture coordinates are in 2-texel units (128 = 1.0),
// positions in whole blocks (the renderer scales the model matrix by 128).
struct CubeVertex {
    uint8_t u, v;
    uint16_t color;
    int8_t x, y, z, pad;
};
static_assert(sizeof(CubeVertex) == 8, "vertex layout must match the GE vertex type");

template <typename V>
struct VertexBuffer {
    V* data = nullptr;
    int count = 0;
    int capacity = 0;
    bool overflow = false;

    bool init(int cap) {
        data = (V*)std::malloc(sizeof(V) * cap);
        capacity = data ? cap : 0;
        count = 0;
        return data != nullptr;
    }
    void reset() {
        count = 0;
        overflow = false;
    }
};
using MeshBuffer = VertexBuffer<MeshVertex>;
using CubeBuffer = VertexBuffer<CubeVertex>;

struct ColorMaps {
    const uint8_t* grass = nullptr;    // 256x256 RGB
    const uint8_t* foliage = nullptr;  // 256x256 RGB
};

void mesherInit(const ColorMaps& maps);
void buildSection(const World& w, int cx, int sy, int cz, CubeBuffer& cubes, MeshBuffer& opaque,
                  MeshBuffer& translucent);

enum : int { PISTON_SHORT_ARM = 1, PISTON_FORCE_EXTENDED = 2 };
// One block with its corner at the origin of `out` and every face emitted (blocks moved by pistons);
// lighting and connections still come from the world around (x, y, z).
void buildBlock(const World& w, int x, int y, int z, uint8_t id, int meta, MeshBuffer& out, int pistonFlags = 0);

// Shared with the GUI for item icons.
uint32_t grassColorAt(const World& w, int x, int z);
uint32_t foliageColorAt(const World& w, int x, int z);
uint32_t blockTint(const World* w, uint8_t id, int meta, int side, int x, int z);
