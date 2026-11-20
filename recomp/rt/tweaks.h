/* User settings (lcs.ini) applied to the original engine's own variables. */
#pragma once
#include "recomp.h"
void tweaks_load(const char *root);
void tweaks_set(const char *key, const char *value); /* same keys as lcs.ini */
void tweaks_apply(CPU *c);
float tweaks_world_fade(void);
float tweaks_draw_distance(void);
void tweaks_set_draw_distance(float k);
float tweaks_res_scale(void);
void tweaks_set_res_scale(float s);
int tweaks_radar_bottom(void);
float tweaks_radar_scale(void);
float tweaks_fov_scale(void);
int tweaks_touch_controls(void);
void tweaks_set_fov_scale(float s);
float tweaks_camera_distance(void);
void tweaks_set_camera_distance(float s);
void tweaks_set_radar_scale(float s);
void tweaks_set_radar_bottom(int bottom);
void tweaks_save(const char *key, const char *value);
