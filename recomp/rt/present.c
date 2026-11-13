/* Scene framebuffer and window scaling.
 *
 * The Android engine sizes its render targets once, from the first surface it is given, and does not
 * rebuild them when viewOnSurfaceChanged reports a new size mid-game.  So the engine always renders at a
 * fixed internal size (the desktop resolution) into a host framebuffer: every glBindFramebuffer(0) it
 * makes is redirected there by the generated GL thunks (rt_gl_default_fb).  At present time that image is
 * scaled into the window, letterboxed to keep its aspect ratio.  Windowed, borderless and exclusive
 * fullscreen, and any window size, then all work without the engine noticing. */
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengles2.h>
#include "platform.h"
#include "rt.h"

extern unsigned rt_gl_default_fb;

#define F(ret, name, args) static ret (APIENTRY *p##name) args
F(void, GenFramebuffers, (GLsizei, GLuint *)); F(void, BindFramebuffer, (GLenum, GLuint));
F(void, FramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint));
F(void, FramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint)); F(GLenum, CheckFramebufferStatus, (GLenum));
F(void, GenRenderbuffers, (GLsizei, GLuint *)); F(void, BindRenderbuffer, (GLenum, GLuint));
F(void, RenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei));
F(void, GenTextures, (GLsizei, GLuint *)); F(void, BindTexture, (GLenum, GLuint)); F(void, ActiveTexture, (GLenum));
F(void, TexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *));
F(void, TexParameteri, (GLenum, GLenum, GLint));
F(void, GetIntegerv, (GLenum, GLint *)); F(void, GetBooleanv, (GLenum, GLboolean *)); F(GLboolean, IsEnabled, (GLenum));
F(void, Enable, (GLenum)); F(void, Disable, (GLenum)); F(void, Viewport, (GLint, GLint, GLsizei, GLsizei));
F(void, ClearColor, (GLfloat, GLfloat, GLfloat, GLfloat)); F(void, Clear, (GLbitfield));
F(void, ColorMask, (GLboolean, GLboolean, GLboolean, GLboolean));
F(GLuint, CreateShader, (GLenum)); F(void, ShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *));
F(void, CompileShader, (GLuint)); F(GLuint, CreateProgram, (void)); F(void, AttachShader, (GLuint, GLuint));
F(void, BindAttribLocation, (GLuint, GLuint, const GLchar *)); F(void, LinkProgram, (GLuint));
F(void, GetProgramiv, (GLuint, GLenum, GLint *)); F(void, UseProgram, (GLuint));
F(GLint, GetUniformLocation, (GLuint, const GLchar *)); F(void, Uniform1i, (GLint, GLint));
F(void, GenBuffers, (GLsizei, GLuint *)); F(void, BindBuffer, (GLenum, GLuint));
F(void, BufferData, (GLenum, GLsizeiptr, const void *, GLenum));
F(void, GetVertexAttribiv, (GLuint, GLenum, GLint *)); F(void, GetVertexAttribPointerv, (GLuint, GLenum, void **));
F(void, VertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *));
F(void, EnableVertexAttribArray, (GLuint)); F(void, DisableVertexAttribArray, (GLuint));
F(void, DrawArrays, (GLenum, GLint, GLsizei));
#undef F

static GLuint fbo, color, depth, program, vbo;
static int scene_w, scene_h, ready;

static int load(void) {
#define L(name) if (!(p##name = (void *)SDL_GL_GetProcAddress("gl" #name))) return 0
    L(GenFramebuffers); L(BindFramebuffer); L(FramebufferTexture2D); L(FramebufferRenderbuffer);
    L(CheckFramebufferStatus); L(GenRenderbuffers); L(BindRenderbuffer); L(RenderbufferStorage);
    L(GenTextures); L(BindTexture); L(ActiveTexture); L(TexImage2D); L(TexParameteri);
    L(GetIntegerv); L(GetBooleanv); L(IsEnabled); L(Enable); L(Disable); L(Viewport); L(ClearColor); L(Clear);
    L(ColorMask); L(CreateShader); L(ShaderSource); L(CompileShader); L(CreateProgram); L(AttachShader);
    L(BindAttribLocation); L(LinkProgram); L(GetProgramiv); L(UseProgram); L(GetUniformLocation); L(Uniform1i);
    L(GenBuffers); L(BindBuffer); L(BufferData); L(GetVertexAttribiv); L(GetVertexAttribPointerv);
    L(VertexAttribPointer); L(EnableVertexAttribArray); L(DisableVertexAttribArray); L(DrawArrays);
#undef L
    return 1;
}

static GLuint shader(GLenum type, const char *src) {
    GLuint s = pCreateShader(type);
    pShaderSource(s, 1, &src, 0); pCompileShader(s);
    return s;
}

/* Called once the GL context exists: create the scene framebuffer at the engine's size and make it the
 * engine's "framebuffer 0". */
void present_init(int w, int h) {
    if (ready || !load()) return;
    scene_w = w; scene_h = h;
    pGenTextures(1, &color); pBindTexture(GL_TEXTURE_2D, color);
    pTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    pTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    pTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    pTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    pTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    pBindTexture(GL_TEXTURE_2D, 0);
    pGenFramebuffers(1, &fbo); pBindFramebuffer(GL_FRAMEBUFFER, fbo);
    pFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
    pGenRenderbuffers(1, &depth); pBindRenderbuffer(GL_RENDERBUFFER, depth);
    pRenderbufferStorage(GL_RENDERBUFFER, 0x88F0 /* DEPTH24_STENCIL8 */, w, h);
    pFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
    pFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depth);
    GLenum status = pCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        rt_log("present: scene framebuffer incomplete (0x%x), rendering straight to the window\n", status);
        pBindFramebuffer(GL_FRAMEBUFFER, 0);
        return;
    }
    GLuint vs = shader(GL_VERTEX_SHADER, "attribute vec4 a; varying vec2 uv; void main(){gl_Position=vec4(a.xy,0.0,1.0);uv=a.zw;}");
    GLuint fs = shader(GL_FRAGMENT_SHADER, "precision mediump float; varying vec2 uv; uniform sampler2D t; void main(){gl_FragColor=vec4(texture2D(t,uv).rgb,1.0);}");
    program = pCreateProgram(); pAttachShader(program, vs); pAttachShader(program, fs);
    pBindAttribLocation(program, 0, "a"); pLinkProgram(program);
    GLint ok = 0; pGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) { rt_log("present: blit shader failed\n"); pBindFramebuffer(GL_FRAMEBUFFER, 0); return; }
    pGenBuffers(1, &vbo);
    rt_gl_default_fb = fbo;
    ready = 1;
    rt_log("present: engine renders at %dx%d, scaled to the window\n", w, h);
}

/* Letterboxed placement of the scene inside a w x h window area. */
static void placement(float w, float h, float *x, float *y, float *dw, float *dh) {
    float s = w / scene_w < h / scene_h ? w / scene_w : h / scene_h;
    *dw = scene_w * s; *dh = scene_h * s;
    *x = (w - *dw) * 0.5f; *y = (h - *dh) * 0.5f;
}

/* Window coordinates (SDL points) -> engine surface coordinates, for touches and the menu cursor. */
void present_map_point(float *px, float *py) {
    if (!ready || !plat.window) return;
    int ww, wh; SDL_GetWindowSize(plat.window, &ww, &wh);
    if (ww <= 0 || wh <= 0) return;
    float x, y, dw, dh; placement((float)ww, (float)wh, &x, &y, &dw, &dh);
    *px = (*px - x) * scene_w / dw; *py = (*py - y) * scene_h / dh;
    if (*px < 0) *px = 0; if (*py < 0) *py = 0;
    if (*px > scene_w - 1) *px = (float)(scene_w - 1); if (*py > scene_h - 1) *py = (float)(scene_h - 1);
}

/* Draw the finished scene into the window (before the swap). */
void present_frame(void) {
    if (!ready || !plat.window) return;
    int ww, wh; SDL_GetWindowSizeInPixels(plat.window, &ww, &wh);
    if (ww <= 0 || wh <= 0) return;
    GLint active, texture, prog, abuf, vp[4]; GLboolean mask[4];
    GLint en, size, type, norm, stride, buf; void *ptr;
    pGetIntegerv(GL_ACTIVE_TEXTURE, &active); pActiveTexture(GL_TEXTURE0); pGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
    pGetIntegerv(GL_CURRENT_PROGRAM, &prog); pGetIntegerv(GL_ARRAY_BUFFER_BINDING, &abuf);
    pGetIntegerv(GL_VIEWPORT, vp); pGetBooleanv(GL_COLOR_WRITEMASK, mask);
    pGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &en); pGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_SIZE, &size);
    pGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_TYPE, &type); pGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &norm);
    pGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &stride); pGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &buf);
    pGetVertexAttribPointerv(0, GL_VERTEX_ATTRIB_ARRAY_POINTER, &ptr);
    GLenum caps[] = {GL_BLEND, GL_DEPTH_TEST, GL_STENCIL_TEST, GL_SCISSOR_TEST, GL_CULL_FACE};
    GLboolean st[5];
    for (int i = 0; i < 5; i++) { st[i] = pIsEnabled(caps[i]); pDisable(caps[i]); }

    pBindFramebuffer(GL_FRAMEBUFFER, 0);
    pColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    pViewport(0, 0, ww, wh);
    pClearColor(0, 0, 0, 1); pClear(GL_COLOR_BUFFER_BIT);
    float x, y, dw, dh; placement((float)ww, (float)wh, &x, &y, &dw, &dh);
    pViewport((GLint)(x + 0.5f), (GLint)(y + 0.5f), (GLsizei)(dw + 0.5f), (GLsizei)(dh + 0.5f));
    static const float quad[] = {-1,-1,0,0, 1,-1,1,0, -1,1,0,1, 1,1,1,1};
    pBindTexture(GL_TEXTURE_2D, color);
    GLint filter = ((int)(dw + 0.5f) == scene_w && (int)(dh + 0.5f) == scene_h) ? GL_NEAREST : GL_LINEAR;
    pTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter); pTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    pUseProgram(program); pUniform1i(pGetUniformLocation(program, "t"), 0);
    pBindBuffer(GL_ARRAY_BUFFER, vbo); pBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STREAM_DRAW);
    pVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 4 * sizeof(float), 0); pEnableVertexAttribArray(0);
    pDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    pBindBuffer(GL_ARRAY_BUFFER, buf);
    pVertexAttribPointer(0, size, type, (GLboolean)norm, stride, ptr);
    if (!en) pDisableVertexAttribArray(0);
    pBindBuffer(GL_ARRAY_BUFFER, abuf); pUseProgram(prog);
    pBindTexture(GL_TEXTURE_2D, texture); pActiveTexture(active);
    pViewport(vp[0], vp[1], vp[2], vp[3]); pColorMask(mask[0], mask[1], mask[2], mask[3]);
    for (int i = 0; i < 5; i++) if (st[i]) pEnable(caps[i]);
    pBindFramebuffer(GL_FRAMEBUFFER, fbo); /* the engine's next frame starts in the scene framebuffer again */
}
