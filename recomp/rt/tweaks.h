/* User settings (lcs.ini) applied to the original engine's own variables. */
#pragma once
#include "recomp.h"
void tweaks_load(const char *root);
void tweaks_set(const char *key, const char *value); /* same keys as lcs.ini */
void tweaks_apply(CPU *c);
float tweaks_world_fade(void);
void tweaks_fx(float out[6]); /* sharpen, fxaa, vignette, brightness, contrast, saturation */
float tweaks_fx_get(int i);
void tweaks_fx_set(int i, float v);
int tweaks_look(void);
void tweaks_set_look(int look);
int tweaks_hud_shown(int part); /* 0 whole HUD, 1 clock, 2 money, 3 health, 4 armour */
void tweaks_set_hud_shown(int part, int shown);
int tweaks_hud_modern(void);
void tweaks_set_hud_modern(int modern);
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
