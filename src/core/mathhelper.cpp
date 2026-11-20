#include "mathhelper.h"

#include <cmath>

namespace mh {

float SIN_TABLE[65536];

void init() {
    static bool done = false;
    if (done) return;
    done = true;
    for (int i = 0; i < 65536; ++i)
        SIN_TABLE[i] = (float)std::sin((double)i * 3.141592653589793 * 2.0 / 65536.0);
}

}  // namespace mh
