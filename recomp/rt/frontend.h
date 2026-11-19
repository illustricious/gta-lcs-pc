/* Windows frontend presentation; original engine owns submenus and actions. */
#pragma once
#include "recomp.h"
void frontend_set_enabled(int enabled);
int frontend_button(CPU *c, int button, int down);
int frontend_touch(CPU *c, int phase, float x, float y);
void frontend_present(void); /* before screenshots and SDL_GL_SwapWindow */
int frontend_back(CPU *c); /* Esc / B: returns 1 if the host menu consumed it */
int frontend_title(void); /* title screen tap-to-continue prompt is showing */
unsigned frontend_first_frame(void); /* frame of the first host menu draw (0 = not yet), for scripted tests */
void frontend_hover(CPU *c, float x, float y); /* mouse over the host menu list */
int frontend_wheel(CPU *c, float dy); /* mouse wheel: zoom the pause map, or scroll the list */
int frontend_key(int scancode); /* returns 1 when the key was taken as a new binding */
int frontend_mouse(int button); /* returns 1 when the mouse button was taken as an action binding */
int host_bind_get(int btn, int slot);
void host_bind_set(int btn, int scancode);
void host_bind_reset(void);
int host_mouse_bind(int i);
void host_mouse_bind_set(int i, int btn);
void host_bind_ini(const char *key, int value);
