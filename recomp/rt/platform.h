/* State shared between the Android service layer and the Windows main loop. */
#pragma once
#include <stdint.h>

enum { EV_GAME_LOADED = 1, EV_GATE, EV_UPDATE_ID };

typedef struct Platform {
    uint64_t env, vm;            /* JNIEnv* / JavaVM* handed to native code */
    int width, height, hidden, use_angle;
    int fullscreen;
    int display_mode;            /* 0 windowed, 1 borderless fullscreen, 2 exclusive fullscreen */
    int win_w, win_h;            /* windowed size */
    int focused;                 /* window has input focus */
    int pause_on_focus_loss;     /* open the pause menu and slow down while in the background */
    void *window;
    int quit;
    int owner_declared;          /* the user's statement that they own the full game (default on, --trial clears it) */
    int no_audio;
    int in_ui;                   /* last CommonAPI.setInUI() value: menus own the cursor */
    int mouse_look;
    int pad_input; /* 1 when the last input came from a gamepad (menu hints follow it) */
    float mouse_sensitivity;
    int fps_cap;                 /* 0 = uncapped, otherwise frames per second limit */
    int vsync;                   /* 1 = also ask the driver to wait for vertical blank */
    int loading_visible;
    float loading_progress;
    unsigned frames;             /* eglSwapBuffers count */
    struct { int kind; uint64_t arg; } events[64]; /* Java-side callbacks delivered on the next frame */
    int nevents;
    const char *asset_dirs[4];
    int nasset_dirs;
} Platform;

extern Platform plat;
void platform_present(void);
void host_set_display_mode(int mode);
void present_frame(void);
void present_map_point(float *x, float *y);
uint64_t jni_string(const char *s);
uint64_t jni_string_array(const char **items, int n);
