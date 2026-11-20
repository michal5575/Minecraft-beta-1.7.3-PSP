#pragma once
#include <cstdint>

#include "world.h"

// For each face of a 16^3 section, a bitmask of the faces reachable through
// non-opaque blocks. Used to skip sections hidden behind solid ground (caves).
void sectionConnectivity(const World& w, int cx, int sy, int cz, uint8_t conn[6]);
