/* Float literals inside the engine's shadow code, scaled by the draw distance (see CONST_SCALE in recomp/lift.py).
 * kind 0: a distance (x k), 1: a squared distance (x k^2). */
#include "rt.h"
#include "tweaks.h"
#include <string.h>

uint32_t rt_scale_f32(uint32_t bits, int kind) {
    float k = tweaks_draw_distance();
    k = k < 1.0f ? 1.0f : k > 3.0f ? 3.0f : k;
    float f;
    memcpy(&f, &bits, 4);
    f *= kind ? k * k : k;
    memcpy(&bits, &f, 4);
    return bits;
}
