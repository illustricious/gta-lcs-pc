/* Android platform services on Windows: JNI environment, asset manager,
 * native window, EGL (backed by an SDL3 OpenGL ES context) and OpenSL.
 * The Java side of the app is represented only by its service contract:
 * what each Java method returns to native code. */
#include "rt.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <SDL3/SDL.h>

int rt_gl_trace;
Platform plat;

void rt_gl_log(CPU *c, const char *name) {
    rt_log("GL %s(%llx, %llx, %llx, %llx)\n", name, (unsigned long long)c->x[0], (unsigned long long)c->x[1],
           (unsigned long long)c->x[2], (unsigned long long)c->x[3]);
}

/* ------------------------------------------------------------ Java objects */

enum { J_CLASS = 1, J_STRING, J_METHOD, J_FIELD, J_ARRAY, J_OBJARRAY, J_OBJECT };
typedef struct JObj {
    uint32_t magic;
    int kind;
    char *str;           /* class name, string value, or member name */
    char *sig;           /* member signature */
    struct JObj *cls;    /* owning class for members / objects */
    uint8_t *data;       /* primitive array storage or object array of JObj* */
    int count, elem;
} JObj;

static JObj *jnew(int kind, const char *str) {
    JObj *o = calloc(1, sizeof *o);
    o->magic = 0x4a4f424a;
    o->kind = kind;
    o->str = _strdup(str ? str : "");
    return o;
}

uint64_t jni_string(const char *s) { return (uint64_t)jnew(J_STRING, s); }

uint64_t jni_string_array(const char **items, int n) {
    JObj *a = jnew(J_OBJARRAY, "[Ljava/lang/String;");
    a->count = n;
    a->data = calloc(n ? n : 1, sizeof(JObj *));
    for (int i = 0; i < n; i++) ((JObj **)a->data)[i] = jnew(J_STRING, items[i]);
    return (uint64_t)a;
}

static JObj *class_named(const char *name) {
    static JObj *known[128];
    static int n;
    for (int i = 0; i < n; i++)
        if (!strcmp(known[i]->str, name)) return known[i];
    if (n == 128) rt_fatal("too many Java classes");
    return known[n++] = jnew(J_CLASS, name);
}

static const char *basename_of(const char *cls) {
    const char *p = strrchr(cls, '/');
    return p ? p + 1 : cls;
}

/* ------------------------------------------------------------ Java service contract */

typedef struct { uint64_t i; double d; } JArg;

static void queue_event(int kind, uint64_t arg) {
    if (plat.nevents < 64) {
        plat.events[plat.nevents].kind = kind;
        plat.events[plat.nevents++].arg = arg;
    }
}

/* Returns the integer/object result; floating results go through *fret. */
static uint64_t java_call(CPU *c, JObj *m, JArg *a, double *fret) {
    const char *cls = basename_of(m->cls ? m->cls->str : "?");
    const char *name = m->str;
    if (rt_verbose) rt_log("JNI call %s.%s%s\n", cls, name, m->sig);
#define IS(c_, n_) (!strcmp(cls, c_) && !strcmp(name, n_))
    if (IS("andHttp", "IsNetworkReachable")) return 0; /* no network service */
    if (IS("andFile", "getFile")) {
        const char *dir = a[0].i ? ((JObj *)a[0].i)->str : "", *nm = a[1].i ? ((JObj *)a[1].i)->str : "",
                   *ext = a[2].i ? ((JObj *)a[2].i)->str : "";
        rt_log("platform: andFile.getFile(dir=%s, name=%s, ext=%s)\n", dir, nm, ext);
        return 0;
    }
    if (IS("CommonAPI", "ShowLoadingScreen")) { plat.loading_visible = 1; return 0; }
    if (IS("CommonAPI", "HideLoadingScreen")) { plat.loading_visible = 0; return 0; }
    if (IS("CommonAPI", "ShowLoadingBar")) { plat.loading_visible = 1; plat.loading_progress = (float)a[0].d; return 0; }
    if (IS("CommonAPI", "ShowLoadingBarProgressHack")) return 0;
    if (IS("CommonAPI", "GetDeviceLanguage")) return jni_string("en");
    if (IS("CommonAPI", "GetVersionName")) return jni_string("2.4.379");
    if (IS("CommonAPI", "PlayVideo")) {
        /* MovieActivity has no Windows counterpart yet: the intro reports as finished. */
        rt_log("platform: PlayVideo(%s) - no movie player, treated as finished\n", a[0].i ? ((JObj *)a[0].i)->str : "");
        return 0;
    }
    if (IS("CommonAPI", "isVideoPlaying")) return 0;
    if (IS("CommonAPI", "PlaylistBeginInit")) {
        /* Java scans the user's music folder (none here), then reports completion synchronously. */
        uint64_t fn = rt_sym("Java_com_rockstargames_gtalcs_CommonAPI_HandlePlaylistFinishInit");
        if (fn) rt_invoke(c, fn, 3, plat.env, (uint64_t)class_named("com/rockstargames/gtalcs/CommonAPI"), 1ull);
        return 0;
    }
    if (IS("CommonAPI", "PlaylistFinishInit") || IS("CommonAPI", "PlaylistPlay") || IS("CommonAPI", "PlaylistPause")) return 0;
    if (IS("CommonAPI", "PlaylistPlaying") || IS("CommonAPI", "GooglePlaylistAvailable") || IS("CommonAPI", "PlaylistHasSongs")) return 0;
    if (IS("CommonAPI", "GoToUrl")) { rt_log("platform: GoToUrl(%s) ignored\n", a[0].i ? ((JObj *)a[0].i)->str : ""); return 0; }
    if (IS("CommonAPI", "QuitApp")) { plat.quit = 1; return 0; }
    if (IS("CommonAPI", "setInUI")) { plat.in_ui = (int)(a[0].i & 1); return 0; }
    if (IS("CommonAPI", "RemoveFrameRateLock") || IS("CommonAPI", "setKeepScreenOn")) return 0;

    /* Rockstar SDK.  There is no Social Club client on this platform: the
     * session is offline and no entitlement is asserted (see platform.h). */
    if (IS("RockstarJNIlib", "GameLoad")) { queue_event(EV_GAME_LOADED, 0); return 0; }
    /* Rockstar.inTrial(): ownership cannot be verified here, so it is "trial" unless the user declared --owned */
    if (IS("RockstarJNIlib", "CheckGate")) return plat.owner_declared ? 0 : 1;
    if (IS("RockstarJNIlib", "ShowGate")) { queue_event(EV_GATE, 0); return 0; }
    if (IS("RockstarJNIlib", "ShowGateBeforeLoad")) { queue_event(EV_GATE, 1); return 0; }
    if (IS("RockstarJNIlib", "UpdateRockstarID") || IS("RockstarJNIlib", "SignIn") || IS("RockstarJNIlib", "SignOut")) {
        queue_event(EV_UPDATE_ID, 0);
        return 0;
    }
    if (IS("RockstarJNIlib", "ShowPrompt") || IS("RockstarJNIlib", "SetLocalePriority")) return 0;
#undef IS
    static char seen[64][96];
    static int nseen;
    char key[96];
    snprintf(key, sizeof key, "%s.%s", cls, name);
    for (int i = 0; i < nseen; i++)
        if (!strcmp(seen[i], key)) return 0;
    if (nseen < 64) strcpy(seen[nseen++], key);
    rt_log("JNI: unhandled Java method %s.%s%s (returns 0)\n", cls, name, m->sig);
    (void)fret;
    return 0;
}

/* ------------------------------------------------------------ JNIEnv */

#define JARG_MAX 16
static void decode_args(JObj *m, int mode, CPU *c, JArg *out) {
    /* mode 0: variadic after (env, obj, mid); 1: va_list in x3; 2: jvalue array in x3 */
    ArgSrc src = mode == 0 ? rt_args_regs(c, 3, 0) : mode == 1 ? rt_args_valist(c->x[3]) : rt_args_regs(c, 3, 0);
    const uint64_t *jv = (const uint64_t *)c->x[3];
    const char *s = m->sig && m->sig[0] == '(' ? m->sig + 1 : ")";
    int n = 0;
    while (*s && *s != ')' && n < JARG_MAX) {
        char t = *s;
        if (t == 'L') { while (*s && *s != ';') s++; }
        else if (t == '[') { while (*s == '[') s++; if (*s == 'L') while (*s && *s != ';') s++; }
        if (*s) s++;
        if (mode == 2) {
            out[n].i = jv[n];
            if (t == 'F') { float f; memcpy(&f, &jv[n], 4); out[n].d = f; }
            else if (t == 'D') memcpy(&out[n].d, &jv[n], 8);
        } else if (t == 'F' || t == 'D') {
            out[n].d = rt_arg_double(&src);
            out[n].i = 0;
        } else {
            out[n].i = rt_arg_int(&src);
            if (t != 'J' && t != 'L' && t != '[') out[n].i = (uint32_t)out[n].i;
        }
        n++;
    }
}

static char ret_type(JObj *m) {
    const char *p = m->sig ? strchr(m->sig, ')') : 0;
    return p ? p[1] : 'V';
}

static void call_method(CPU *c, int mode) {
    JObj *m = (JObj *)c->x[2];
    JArg args[JARG_MAX] = {0};
    if (!m || m->magic != 0x4a4f424a || m->kind != J_METHOD) rt_fatal("JNI Call*Method with a bad method id");
    decode_args(m, mode, c, args);
    double fret = 0;
    uint64_t r = java_call(c, m, args, &fret);
    char t = ret_type(m);
    if (t == 'F') SET_S(0, (float)fret);
    else if (t == 'D') SET_D(0, fret);
    else c->x[0] = r;
}
static void jni_call(CPU *c) { call_method(c, 0); }
static void jni_callV(CPU *c) { call_method(c, 1); }
static void jni_callA(CPU *c) { call_method(c, 2); }

static void jni_unimpl(CPU *c, int slot) {
    char b[256];
    rt_fatal("JNIEnv function slot %d is not implemented (called from %s)", slot, rt_describe(c->x[30], b, sizeof b));
}
#define U(n) static void jni_u##n(CPU *c) { jni_unimpl(c, n); }
#define ROW(h) U(h##0) U(h##1) U(h##2) U(h##3) U(h##4) U(h##5) U(h##6) U(h##7) U(h##8) U(h##9) U(h##a) U(h##b) U(h##c) U(h##d) U(h##e) U(h##f)
ROW(0x0) ROW(0x1) ROW(0x2) ROW(0x3) ROW(0x4) ROW(0x5) ROW(0x6) ROW(0x7) ROW(0x8) ROW(0x9) ROW(0xa) ROW(0xb) ROW(0xc) ROW(0xd) ROW(0xe)
#undef U
#define U(n) jni_u##n,
static const RtFunc jni_default[] = {ROW(0x0) ROW(0x1) ROW(0x2) ROW(0x3) ROW(0x4) ROW(0x5) ROW(0x6) ROW(0x7) ROW(0x8) ROW(0x9) ROW(0xa) ROW(0xb) ROW(0xc) ROW(0xd) ROW(0xe)};
#undef U
#undef ROW

#define JF(name) static void jni_##name(CPU *c)
JF(GetVersion) { RET(0x00010006); }
JF(FindClass) {
    if (rt_verbose) rt_log("JNI FindClass(%s)\n", S(1));
    RET(class_named(S(1)));
}
JF(ret0) { RET(0); }
JF(ret_arg1) { RET(A(1)); }
JF(nop) { (void)c; }
JF(GetObjectClass) {
    JObj *o = P(1);
    RET(o && o->cls ? o->cls : class_named("java/lang/Object"));
}
JF(GetMethodID) {
    JObj *m = jnew(J_METHOD, S(2));
    m->sig = _strdup(S(3));
    m->cls = P(1);
    if (rt_verbose) rt_log("JNI Get(Static)MethodID(%s.%s%s)\n", m->cls ? m->cls->str : "?", S(2), S(3));
    RET(m);
}
JF(GetFieldID) {
    JObj *m = jnew(J_FIELD, S(2));
    m->sig = _strdup(S(3));
    m->cls = P(1);
    RET(m);
}
JF(GetField) {
    JObj *f = P(2);
    rt_log("JNI: field read %s.%s (returns 0)\n", f && f->cls ? f->cls->str : "?", f ? f->str : "?");
    RET(0);
}
JF(NewObject) {
    JObj *o = jnew(J_OBJECT, "");
    o->cls = P(1);
    RET(o);
}
JF(NewStringUTF) { RET(jnew(J_STRING, A(1) ? S(1) : "")); }
JF(GetStringUTFLength) { RET(strlen(((JObj *)P(1))->str)); }
JF(GetStringUTFChars) {
    if (A(2)) M8(A(2)) = 0;
    RET(A(1) ? ((JObj *)P(1))->str : 0);
}
JF(GetStringUTFRegion) {
    JObj *s = P(1);
    memcpy(P(4), s->str + I(2), I(3));
    M8(A(4) + I(3)) = 0;
}
JF(GetArrayLength) { RET(A(1) ? ((JObj *)P(1))->count : 0); }
JF(GetObjectArrayElement) {
    JObj *a = P(1);
    RET(a && I(2) < a->count ? ((JObj **)a->data)[I(2)] : 0);
}
JF(SetObjectArrayElement) {
    JObj *a = P(1);
    if (a && I(2) < a->count) ((JObj **)a->data)[I(2)] = P(3);
}
JF(NewObjectArray) {
    JObj *a = jnew(J_OBJARRAY, "");
    a->count = I(1);
    a->data = calloc(a->count ? a->count : 1, sizeof(JObj *));
    RET(a);
}
static JObj *new_array(int count, int elem) {
    JObj *a = jnew(J_ARRAY, "");
    a->count = count;
    a->elem = elem;
    a->data = calloc(count ? count : 1, elem);
    return a;
}
uint64_t jni_byte_array(const void *data, int n) {
    JObj *a = new_array(n, 1);
    memcpy(a->data, data, n);
    return (uint64_t)a;
}
JF(NewArray1) { RET(new_array(I(1), 1)); }
JF(NewArray2) { RET(new_array(I(1), 2)); }
JF(NewArray4) { RET(new_array(I(1), 4)); }
JF(NewArray8) { RET(new_array(I(1), 8)); }
JF(GetArrayElements) {
    if (A(2)) M8(A(2)) = 0;
    RET(((JObj *)P(1))->data);
}
JF(GetArrayRegion) {
    JObj *a = P(1);
    memcpy(P(4), a->data + (size_t)I(2) * a->elem, (size_t)I(3) * a->elem);
}
JF(SetArrayRegion) {
    JObj *a = P(1);
    memcpy(a->data + (size_t)I(2) * a->elem, P(4), (size_t)I(3) * a->elem);
}
JF(GetJavaVM) { M64(A(1)) = plat.vm; RET(0); }
JF(IsSameObject) { RET(A(1) == A(2)); }
JF(vm_GetEnv) { M64(A(1)) = plat.env; RET(0); }
JF(vm_Attach) { M64(A(1)) = plat.env; RET(0); }

static uint64_t env_table[240], vm_table[8], env_ptr, vm_ptr;

static void set_range(int first, int last, RtFunc fn, const char *name) {
    for (int i = first; i <= last; i++) env_table[i] = rt_thunk(fn, name);
}

static void init_jni(void) {
    static char names[240][16];
    for (int i = 0; i < 240; i++) {
        snprintf(names[i], sizeof names[i], "JNIEnv[%d]", i);
        env_table[i] = rt_thunk(jni_default[i], names[i]);
    }
#define SET(i, fn) env_table[i] = rt_thunk(jni_##fn, "JNIEnv::" #fn)
    SET(4, GetVersion); SET(6, FindClass);
    SET(15, ret0); SET(16, nop); SET(17, nop);          /* ExceptionOccurred/Describe/Clear */
    SET(19, ret0); SET(20, ret_arg1);                   /* Push/PopLocalFrame */
    SET(21, ret_arg1); SET(22, nop); SET(23, nop);      /* NewGlobalRef, DeleteGlobalRef, DeleteLocalRef */
    SET(24, IsSameObject); SET(25, ret_arg1); SET(26, ret0);
    SET(27, NewObject); SET(28, NewObject); SET(29, NewObject); SET(30, NewObject);
    SET(31, GetObjectClass); SET(32, ret0); SET(33, GetMethodID);
    for (int i = 34; i <= 63; i += 3) { SET(i, call); SET(i + 1, callV); SET(i + 2, callA); }
    SET(94, GetFieldID);
    set_range(95, 103, jni_GetField, "JNIEnv::GetField");
    set_range(104, 112, jni_nop, "JNIEnv::SetField");
    SET(113, GetMethodID);
    for (int i = 114; i <= 143; i += 3) { SET(i, call); SET(i + 1, callV); SET(i + 2, callA); }
    SET(144, GetFieldID);
    set_range(145, 153, jni_GetField, "JNIEnv::GetStaticField");
    set_range(154, 162, jni_nop, "JNIEnv::SetStaticField");
    SET(167, NewStringUTF); SET(168, GetStringUTFLength); SET(169, GetStringUTFChars); SET(170, nop);
    SET(164, GetStringUTFLength);
    SET(171, GetArrayLength); SET(172, NewObjectArray); SET(173, GetObjectArrayElement); SET(174, SetObjectArrayElement);
    SET(175, NewArray1); SET(176, NewArray1); SET(177, NewArray2); SET(178, NewArray2);
    SET(179, NewArray4); SET(180, NewArray8); SET(181, NewArray4); SET(182, NewArray8);
    set_range(183, 190, jni_GetArrayElements, "JNIEnv::GetArrayElements");
    set_range(191, 198, jni_nop, "JNIEnv::ReleaseArrayElements");
    set_range(199, 206, jni_GetArrayRegion, "JNIEnv::GetArrayRegion");
    set_range(207, 214, jni_SetArrayRegion, "JNIEnv::SetArrayRegion");
    SET(215, ret0); SET(216, ret0); SET(217, ret0); SET(218, ret0);
    SET(219, GetJavaVM); SET(221, GetStringUTFRegion);
    SET(222, GetArrayElements); SET(223, nop);
    SET(226, ret_arg1); SET(227, nop); SET(228, ret0);
#undef SET
    vm_table[3] = rt_thunk(jni_ret0, "JavaVM::DestroyJavaVM");
    vm_table[4] = rt_thunk(jni_vm_Attach, "JavaVM::AttachCurrentThread");
    vm_table[5] = rt_thunk(jni_ret0, "JavaVM::DetachCurrentThread");
    vm_table[6] = rt_thunk(jni_vm_GetEnv, "JavaVM::GetEnv");
    vm_table[7] = rt_thunk(jni_vm_Attach, "JavaVM::AttachCurrentThreadAsDaemon");
    env_ptr = (uint64_t)env_table;
    vm_ptr = (uint64_t)vm_table;
    plat.env = (uint64_t)&env_ptr;
    plat.vm = (uint64_t)&vm_ptr;
}

/* ------------------------------------------------------------ assets */

typedef struct Asset { FILE *f; int64_t size; } Asset;

IMP(AAssetManager_fromJava) { RET(0xA55E7); }
IMP(AAssetManager_open) {
    char path[1024];
    snprintf(path, sizeof path, "%s", S(1));
    FILE *f = fopen(path, "rb"); /* asset-pack files arrive as absolute paths */
    for (int i = 0; i < plat.nasset_dirs && !f; i++) {
        snprintf(path, sizeof path, "%s/%s", plat.asset_dirs[i], S(1));
        f = fopen(path, "rb");
    }
    if (rt_verbose > 1 || (rt_verbose && f)) rt_log("AAssetManager_open(%s) -> %s\n", S(1), f ? path : "not found");
    if (!f) { RET(0); return; }
    Asset *a = calloc(1, sizeof *a);
    a->f = f;
    _fseeki64(f, 0, SEEK_END);
    a->size = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    RET(a);
}
IMP(AAsset_close) { Asset *a = P(0); if (a) { fclose(a->f); free(a); } }
IMP(AAsset_read) { Asset *a = P(0); RETI((int)fread(P(1), 1, A(2), a->f)); }
IMP(AAsset_seek) {
    Asset *a = P(0);
    if (_fseeki64(a->f, (int64_t)A(1), I(2))) { RET(-1); return; }
    RET(_ftelli64(a->f));
}
IMP(AAsset_getLength) { RET(((Asset *)P(0))->size); }
IMP(AAsset_getRemainingLength) { Asset *a = P(0); RET(a->size - _ftelli64(a->f)); }

/* ------------------------------------------------------------ window, EGL */

IMP(ANativeWindow_fromSurface) { RET(0x57F0); }
IMP(ANativeWindow_getWidth) { RETI(plat.width); }
IMP(ANativeWindow_getHeight) { RETI(plat.height); }
IMP(ANativeWindow_release) { }
IMP(ANativeWindow_setBuffersGeometry) { RET(0); }

static SDL_Window *window;
static SDL_GLContext glctx;

static void create_context(void) {
    if (glctx) return;
    if (plat.use_angle) SDL_SetHint(SDL_HINT_OPENGL_ES_DRIVER, "1");
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    window = SDL_CreateWindow("Grand Theft Auto: Liberty City Stories", plat.fullscreen ? plat.width : plat.win_w, plat.fullscreen ? plat.height : plat.win_h,
                              SDL_WINDOW_OPENGL | (plat.hidden ? SDL_WINDOW_HIDDEN : 0) | (plat.fullscreen ? SDL_WINDOW_FULLSCREEN : SDL_WINDOW_RESIZABLE));
    if (!window) rt_fatal("SDL_CreateWindow: %s", SDL_GetError());
    glctx = SDL_GL_CreateContext(window);
    if (!glctx) rt_fatal("OpenGL ES 2.0 context creation failed: %s", SDL_GetError());
    for (int i = 0; rt_gl_names[i]; i++) {
        rt_gl_fn[i] = (void *)SDL_GL_GetProcAddress(rt_gl_names[i]);
        size_t n = strlen(rt_gl_names[i]);
        if (!rt_gl_fn[i] && n > 3 && (!strcmp(rt_gl_names[i] + n - 3, "OES") || !strcmp(rt_gl_names[i] + n - 3, "EXT"))) {
            char core[64]; /* the same entry point under its core ES 3 name */
            snprintf(core, sizeof core, "%.*s", (int)(n - 3), rt_gl_names[i]);
            rt_gl_fn[i] = (void *)SDL_GL_GetProcAddress(core);
        }
    }
    plat.window = window;
    { void present_init(int, int); present_init(plat.width, plat.height); }
    if (plat.display_mode == 2 && !plat.hidden) host_set_display_mode(2);
    SDL_GL_SetSwapInterval(plat.vsync ? 1 : 0);
    typedef const char *(__stdcall *GetString)(unsigned);
    GetString gs = (GetString)SDL_GL_GetProcAddress("glGetString");
    if (gs) rt_log("GL: %s | %s | %s\n", gs(0x1F00), gs(0x1F01), gs(0x1F02));
}

IMP(eglGetDisplay) { RET(0xD15); }
IMP(eglInitialize) {
    if (A(1)) MS32(A(1)) = 1;
    if (A(2)) MS32(A(2)) = 4;
    RET(1);
}
IMP(eglChooseConfig) {
    if (A(2) && I(3) > 0) M64(A(2)) = 0xC0F1;
    if (A(4)) MS32(A(4)) = 1;
    RET(1);
}
IMP(eglGetConfigAttrib) {
    int v = 0;
    switch (I(2)) {
    case 0x3020: v = 32; break;                                      /* BUFFER_SIZE */
    case 0x3021: case 0x3022: case 0x3023: case 0x3024: v = 8; break; /* A, B, G, R */
    case 0x3025: v = 24; break;                                      /* DEPTH */
    case 0x3026: v = 8; break;                                       /* STENCIL */
    case 0x3028: v = 1; break;                                       /* CONFIG_ID */
    case 0x302E: v = 1; break;                                       /* NATIVE_VISUAL_ID */
    case 0x3033: v = 4; break;                                       /* SURFACE_TYPE: window */
    case 0x3040: v = 4; break;                                       /* RENDERABLE_TYPE: ES2 */
    default: v = 0; break;
    }
    MS32(A(3)) = v;
    RET(1);
}
IMP(eglCreateWindowSurface) { create_context(); RET(0x5F1); }
IMP(eglCreateContext) { create_context(); RET(0xC7); }
IMP(eglMakeCurrent) {
    if (A(3)) { create_context(); SDL_GL_MakeCurrent(window, glctx); }
    else if (window) SDL_GL_MakeCurrent(window, 0);
    RET(1);
}
IMP(eglDestroySurface) { RET(1); }
/* The game asks for its own swap interval; the host frame limiter (lcs.ini fps_cap) decides instead. */
IMP(eglSwapInterval) { SDL_GL_SetSwapInterval(plat.vsync ? 1 : 0); RET(1); }
IMP(eglSwapBuffers) {
    plat.frames++;
    platform_present();
    RET(1);
}
IMP(eglGetProcAddress) {
    for (int i = 0; rt_gl_names[i]; i++)
        if (!strcmp(rt_gl_names[i], S(0)) && rt_gl_fn[i]) { RET(rt_thunk(rt_gl_shims[i], rt_gl_names[i])); return; }
    if (rt_verbose) rt_log("eglGetProcAddress(%s) -> not available\n", S(0));
    RET(0);
}

void rt_libc_init(void);
void rt_audio_init(void);
void rt_android_init(void) {
    rt_libc_init();
    rt_audio_init();
    init_jni();
}
