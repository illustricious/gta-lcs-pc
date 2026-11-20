/* Population ranges (traffic and pedestrians) scaled by the draw distance setting.
 * GetPopulationCullRange / ...OffScreen / ...FarAway / GetPopulationAddMin/MaxRadius / GetDriverHighDetailRange read
 * fields +0x30..+0x44 of the device detail settings object; cars and peds are created in the ring between the Add
 * radii and removed beyond the cull ranges, so scaling the returned values moves the whole ring outwards. */
#include "rt.h"
#include "tweaks.h"

static float pop_scale(void) {
    float k = tweaks_draw_distance();
    return k < 1.0f ? 1.0f : k > 3.0f ? 3.0f : k;
}

static void scale_ret(CPU *c, const char *name, int *logged) {
    float b = c->v[0].f[0];
    c->v[0].f[0] = b * pop_scale();
    if (rt_verbose && !*logged) { *logged = 1; rt_log("population: %s %.1f -> %.1f\n", name, b, c->v[0].f[0]); }
}

#define POP_GETTER(addr) \
    extern void G_##addr##_orig(CPU *c); \
    void G_##addr(CPU *c) { static int logged; G_##addr##_orig(c); scale_ret(c, #addr, &logged); }

POP_GETTER(4623d0)
POP_GETTER(4623f0)
POP_GETTER(462410)
POP_GETTER(462430)
POP_GETTER(462450)
POP_GETTER(462470)
