#include "visibility.h"

#include <cstring>

void sectionConnectivity(const World& w, int cx, int sy, int cz, uint8_t conn[6]) {
    static uint8_t open[4096];
    static uint8_t visited[4096];
    static uint16_t stack[4096];
    const Chunk* chunk = w.chunk(cx, cz);
    if (!chunk || !chunk->sec[sy]) {
        std::memset(conn, 0x3F, 6);
        return;
    }
    const Section& sec = *chunk->sec[sy];
    int openCount = 0;
    for (int x = 0; x < 16; ++x)
        for (int z = 0; z < 16; ++z) {
            const uint8_t* col = sec.blocks + sectionIndex(x, 0, z);
            for (int y = 0; y < 16; ++y) {
                uint8_t o = blockIsOpaque(col[y]) ? 0 : 1;
                open[(x << 8) | (z << 4) | y] = o;
                openCount += o;
            }
        }
    std::memset(conn, 0, 6);
    if (openCount == 0) return;
    if (openCount == 4096) {
        std::memset(conn, 0x3F, 6);
        return;
    }
    std::memset(visited, 0, sizeof(visited));
    for (int start = 0; start < 4096; ++start) {
        if (!open[start] || visited[start]) continue;
        uint8_t faces = 0;
        int sp = 0;
        stack[sp++] = (uint16_t)start;
        visited[start] = 1;
        while (sp) {
            int c = stack[--sp];
            int x = c >> 8, z = (c >> 4) & 15, y = c & 15;
            if (y == 0) faces |= 1 << 0;
            if (y == 15) faces |= 1 << 1;
            if (z == 0) faces |= 1 << 2;
            if (z == 15) faces |= 1 << 3;
            if (x == 0) faces |= 1 << 4;
            if (x == 15) faces |= 1 << 5;
            const int nb[6] = {y > 0 ? c - 1 : -1,         y < 15 ? c + 1 : -1,         z > 0 ? c - 16 : -1,
                               z < 15 ? c + 16 : -1,       x > 0 ? c - 256 : -1,        x < 15 ? c + 256 : -1};
            for (int n : nb) {
                if (n < 0 || !open[n] || visited[n]) continue;
                visited[n] = 1;
                stack[sp++] = (uint16_t)n;
            }
        }
        for (int f = 0; f < 6; ++f)
            if (faces & (1 << f)) conn[f] |= faces;
    }
}
