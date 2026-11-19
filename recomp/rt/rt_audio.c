/* OpenSL ES output service on top of SDL3 audio.  The game's own OpenAL Soft
 * (recompiled libopenal.so) does all mixing; this file only provides the
 * Android buffer-queue player it writes finished PCM into. */
#include "rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <SDL3/SDL.h>

enum { IID_ENGINE = 1, IID_BUFFERQUEUE, IID_CONFIG, IID_PLAY, IID_RECORD };
static uint64_t iid_value[6];

typedef struct Player Player;
typedef struct Itf { uint64_t vt; Player *owner; } Itf; /* handle = &itf (pointer to vtable pointer) */
struct Player {
    Itf object, queue, play, config;
    SDL_AudioStream *stream;
    CRITICAL_SECTION lock;
    uint32_t sizes[256];      /* enqueued buffer sizes, FIFO */
    unsigned head, count;
    uint64_t put_total, done_total; /* bytes handed to SDL / bytes whose buffers were reported finished */
    uint32_t index;
    uint64_t callback, context;
    uint32_t state;
    volatile LONG alive;
    HANDLE pump;
    /* No playback device (a PC without speakers/headphones, or audio disabled): the player still runs, buffers are
     * "played" at the real-time rate and thrown away, so the game's OpenAL keeps going without sound. */
    int silent;
    uint32_t bytes_per_sec;
    uint64_t silent_played;
    LARGE_INTEGER silent_last;
};
static LARGE_INTEGER qpc_freq;
/* bytes the device has consumed so far */
static uint64_t played_bytes(Player *p) {
    if (!p->silent) return p->put_total - (uint64_t)SDL_GetAudioStreamQueued(p->stream);
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (p->state == 3) p->silent_played += (uint64_t)((now.QuadPart - p->silent_last.QuadPart) * (double)p->bytes_per_sec / qpc_freq.QuadPart);
    p->silent_last = now;
    if (p->silent_played > p->put_total) p->silent_played = p->put_total;
    return p->silent_played;
}

static uint64_t vt_engine_obj[10], vt_engine[16], vt_mix_obj[10], vt_player_obj[10], vt_queue[4], vt_play[8], vt_config[2];
static Itf engine_obj = {0}, engine_itf = {0}, mix_obj = {0};

static DWORD WINAPI pump_main(void *param) {
    Player *p = param;
    CPU *c = rt_cpu();
    while (p->alive) {
        int fired = 0;
        EnterCriticalSection(&p->lock);
        if (p->count && (p->stream || p->silent)) {
            uint64_t played = played_bytes(p);
            uint32_t sz = p->sizes[p->head];
            if (played >= p->done_total + sz) {
                p->done_total += sz;
                p->head = (p->head + 1) & 255;
                p->count--;
                p->index++;
                fired = 1;
            }
        }
        uint64_t cb = p->callback, ctx = p->context;
        LeaveCriticalSection(&p->lock);
        if (fired) {
            if (cb) rt_invoke(c, cb, 2, (uint64_t)&p->queue, ctx);
        } else {
            Sleep(1);
        }
    }
    return 0;
}

#define SLF(name) static void sl_##name(CPU *c)
SLF(ok) { RET(0); }
SLF(unsupported) { RET(0x0C); }

SLF(engine_GetInterface) {
    if (A(1) == iid_value[IID_ENGINE]) { M64(A(2)) = (uint64_t)&engine_itf; RET(0); }
    else RET(0x0C);
}
SLF(CreateOutputMix) { M64(A(1)) = (uint64_t)&mix_obj; RET(0); }

SLF(CreateAudioPlayer) {
    /* (self, SLObjectItf *out, SLDataSource *src, SLDataSink *snk, n, ids, required) */
    uint64_t fmt = M64(A(2) + 8);
    uint32_t type = M32(fmt), channels = M32(fmt + 4), rate = M32(fmt + 8) / 1000, bits = M32(fmt + 12);
    uint32_t repr = type == 4 ? M32(fmt + 28) : (bits == 8 ? 2 : 1);
    SDL_AudioSpec spec;
    spec.channels = (int)channels;
    spec.freq = (int)rate;
    if (repr == 3 && bits == 32) spec.format = SDL_AUDIO_F32LE;
    else if (bits == 8) spec.format = SDL_AUDIO_U8;
    else if (bits == 16) spec.format = SDL_AUDIO_S16LE;
    else if (bits == 32) spec.format = SDL_AUDIO_S32LE;
    else { RET(0x0C); return; }
    Player *p = calloc(1, sizeof *p);
    p->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, 0, 0);
    if (!p->stream) {
        /* refusing the player makes OpenAL throw "Device reset failure" and the game abort: run silently instead */
        rt_log("audio: cannot open playback device (%s): running without sound\n", SDL_GetError());
        p->silent = 1;
        p->bytes_per_sec = rate * channels * (bits / 8);
        if (!qpc_freq.QuadPart) QueryPerformanceFrequency(&qpc_freq);
        QueryPerformanceCounter(&p->silent_last);
    }
    rt_log("audio: OpenSL player %u Hz, %u ch, %u-bit%s\n", rate, channels, bits, repr == 3 ? " float" : "");
    InitializeCriticalSection(&p->lock);
    p->object.vt = (uint64_t)vt_player_obj;
    p->queue.vt = (uint64_t)vt_queue;
    p->play.vt = (uint64_t)vt_play;
    p->config.vt = (uint64_t)vt_config;
    p->object.owner = p->queue.owner = p->play.owner = p->config.owner = p;
    p->state = 1;
    p->alive = 1;
    p->pump = CreateThread(0, 16u << 20, pump_main, p, STACK_SIZE_PARAM_IS_A_RESERVATION, 0);
    M64(A(1)) = (uint64_t)&p->object;
    RET(0);
}

SLF(player_GetInterface) {
    Player *p = ((Itf *)P(0))->owner;
    Itf *out = A(1) == iid_value[IID_BUFFERQUEUE] ? &p->queue : A(1) == iid_value[IID_PLAY] ? &p->play :
               A(1) == iid_value[IID_CONFIG] ? &p->config : 0;
    if (!out) { RET(0x0C); return; }
    M64(A(2)) = (uint64_t)out;
    RET(0);
}
SLF(player_Destroy) {
    Player *p = ((Itf *)P(0))->owner;
    p->alive = 0;
    WaitForSingleObject(p->pump, 2000);
    if (p->stream) SDL_DestroyAudioStream(p->stream);
    p->stream = 0;
}
SLF(queue_Enqueue) {
    Player *p = ((Itf *)P(0))->owner;
    uint32_t size = (uint32_t)A(2);
    EnterCriticalSection(&p->lock);
    if (p->count == 256) { LeaveCriticalSection(&p->lock); RET(0x0B); return; } /* SL_RESULT_BUFFER_INSUFFICIENT */
    if (p->stream) SDL_PutAudioStreamData(p->stream, P(1), (int)size);
    static int audible; /* diagnostic: report once when the mixer delivers non-silent 16-bit PCM */
    if (!audible) {
        const int16_t *s = P(1);
        for (uint32_t i = 0; i < size / 2; i++)
            if (s[i] > 1000 || s[i] < -1000) { audible = 1; rt_log("audio: first audible buffer after %llu bytes\n", (unsigned long long)p->put_total); break; }
    }
    p->sizes[(p->head + p->count) & 255] = size;
    p->count++;
    p->put_total += size;
    LeaveCriticalSection(&p->lock);
    RET(0);
}
SLF(queue_Clear) {
    Player *p = ((Itf *)P(0))->owner;
    EnterCriticalSection(&p->lock);
    if (p->stream) SDL_ClearAudioStream(p->stream);
    p->silent_played = 0;
    p->count = 0;
    p->head = 0;
    p->put_total = p->done_total = 0;
    LeaveCriticalSection(&p->lock);
    RET(0);
}
SLF(queue_GetState) {
    Player *p = ((Itf *)P(0))->owner;
    M32(A(1)) = p->count;
    M32(A(1) + 4) = p->index;
    RET(0);
}
SLF(queue_RegisterCallback) {
    Player *p = ((Itf *)P(0))->owner;
    EnterCriticalSection(&p->lock);
    p->callback = A(1);
    p->context = A(2);
    LeaveCriticalSection(&p->lock);
    RET(0);
}
SLF(play_SetPlayState) {
    Player *p = ((Itf *)P(0))->owner;
    EnterCriticalSection(&p->lock);
    if (p->silent) played_bytes(p); /* account the time played in the old state */
    p->state = (uint32_t)A(1);
    LeaveCriticalSection(&p->lock);
    if (p->stream) {
        if (p->state == 3) SDL_ResumeAudioStreamDevice(p->stream);
        else SDL_PauseAudioStreamDevice(p->stream);
    }
    RET(0);
}
SLF(play_GetPlayState) {
    Player *p = ((Itf *)P(0))->owner;
    M32(A(1)) = p->state;
    RET(0);
}

IMP(slCreateEngine) {
    M64(A(0)) = (uint64_t)&engine_obj;
    RET(0);
}

static void fill(uint64_t *vt, int n, RtFunc fn, const char *name) {
    for (int i = 0; i < n; i++) vt[i] = rt_thunk(fn, name);
}

void rt_audio_init(void) {
    static const char *names[] = {0, "SL_IID_ENGINE", "SL_IID_ANDROIDSIMPLEBUFFERQUEUE", "SL_IID_ANDROIDCONFIGURATION",
                                  "SL_IID_PLAY", "SL_IID_RECORD"};
    static uint8_t iid_storage[6][16];
    for (int i = 1; i < 6; i++) {
        iid_value[i] = (uint64_t)iid_storage[i]; /* SLInterfaceID is a pointer; identity is all that matters */
        rt_register_data(names[i], &iid_value[i]);
    }
    fill(vt_engine_obj, 10, sl_ok, "SLObject(engine)");
    vt_engine_obj[3] = rt_thunk(sl_engine_GetInterface, "SLEngineObject::GetInterface");
    fill(vt_mix_obj, 10, sl_ok, "SLObject(outputmix)");
    fill(vt_engine, 16, sl_unsupported, "SLEngine::unsupported");
    vt_engine[2] = rt_thunk(sl_CreateAudioPlayer, "SLEngine::CreateAudioPlayer");
    vt_engine[7] = rt_thunk(sl_CreateOutputMix, "SLEngine::CreateOutputMix");
    fill(vt_player_obj, 10, sl_ok, "SLObject(player)");
    vt_player_obj[3] = rt_thunk(sl_player_GetInterface, "SLPlayerObject::GetInterface");
    vt_player_obj[6] = rt_thunk(sl_player_Destroy, "SLPlayerObject::Destroy");
    vt_queue[0] = rt_thunk(sl_queue_Enqueue, "SLBufferQueue::Enqueue");
    vt_queue[1] = rt_thunk(sl_queue_Clear, "SLBufferQueue::Clear");
    vt_queue[2] = rt_thunk(sl_queue_GetState, "SLBufferQueue::GetState");
    vt_queue[3] = rt_thunk(sl_queue_RegisterCallback, "SLBufferQueue::RegisterCallback");
    fill(vt_play, 8, sl_ok, "SLPlay");
    vt_play[0] = rt_thunk(sl_play_SetPlayState, "SLPlay::SetPlayState");
    vt_play[1] = rt_thunk(sl_play_GetPlayState, "SLPlay::GetPlayState");
    fill(vt_config, 2, sl_ok, "SLAndroidConfiguration");
    engine_obj.vt = (uint64_t)vt_engine_obj;
    engine_itf.vt = (uint64_t)vt_engine;
    mix_obj.vt = (uint64_t)vt_mix_obj;
}
