/* n3ds_sdl.c - libctru implementation of the SDL2 subset declared in 3ds/include/SDL.h */
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include "SDL.h"

/* --- core --- */

extern void n3ds_platform_init(void);
int SDL_Init(Uint32 flags) { (void)flags; n3ds_platform_init(); return 0; }
void SDL_Quit(void) {}
void SDL_SetMainReady(void) {}
const char* SDL_GetError(void) { return "3DS"; }
SDL_bool SDL_SetHint(const char* name, const char* value) { (void)name; (void)value; return SDL_FALSE; }

/* --- timing --- */

Uint64 SDL_GetPerformanceCounter(void) { return svcGetSystemTick(); }
Uint64 SDL_GetPerformanceFrequency(void) { return SYSCLOCK_ARM11; }
Uint32 SDL_GetTicks(void) { return (Uint32)(svcGetSystemTick() / CPU_TICKS_PER_MSEC); }
void SDL_Delay(Uint32 ms) { svcSleepThread((s64)ms * 1000000LL); }

/* --- threads --- */

struct SDL_Thread {
    Thread handle;
    SDL_ThreadFunction fn;
    void* data;
    int status;
};

extern void n3ds_install_crash_handler(void);
static void n3ds_thread_entry(void* arg) {
    SDL_Thread* t = (SDL_Thread*)arg;
    n3ds_install_crash_handler();
    t->status = t->fn(t->data);
}

/* Worker threads go to a second core so they do not compete with the game
 * thread: core 2 on N3DS, core 1 (time-limited syscore) on O3DS. */
SDL_Thread* SDL_CreateThread(SDL_ThreadFunction fn, const char* name, void* data) {
    (void)name;
    SDL_Thread* t = (SDL_Thread*)calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->fn = fn;
    t->data = data;

    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    bool is_new = false;
    APT_CheckNew3DS(&is_new);

    t->handle = threadCreate(n3ds_thread_entry, t, 64 * 1024, prio - 1, is_new ? 2 : 1, false);
    if (!t->handle) {
        t->handle = threadCreate(n3ds_thread_entry, t, 64 * 1024, prio - 1, -2, false);
    }
    if (!t->handle) {
        free(t);
        return NULL;
    }
    return t;
}

void SDL_WaitThread(SDL_Thread* t, int* status) {
    if (!t) return;
    threadJoin(t->handle, U64_MAX);
    threadFree(t->handle);
    if (status) *status = t->status;
    free(t);
}

struct SDL_mutex { LightLock lock; };

SDL_mutex* SDL_CreateMutex(void) {
    SDL_mutex* m = (SDL_mutex*)malloc(sizeof(*m));
    if (m) LightLock_Init(&m->lock);
    return m;
}
void SDL_DestroyMutex(SDL_mutex* m) { free(m); }
int SDL_LockMutex(SDL_mutex* m) { LightLock_Lock(&m->lock); return 0; }
int SDL_UnlockMutex(SDL_mutex* m) { LightLock_Unlock(&m->lock); return 0; }

/* --- audio (ndsp, needs sdmc:/3ds/dspfirm.cdc) --- */

#define N3DS_AUDIO_BUFS 4

static SDL_AudioCallback audio_cb;
static void* audio_userdata;
static ndspWaveBuf audio_wbuf[N3DS_AUDIO_BUFS];
static s16* audio_mem;
static int audio_bytes_per_buf;
static volatile int audio_paused = 1;
static int audio_open;

static void n3ds_audio_fill(void* unused) {
    (void)unused;
    for (int i = 0; i < N3DS_AUDIO_BUFS; i++) {
        ndspWaveBuf* wb = &audio_wbuf[i];
        if (wb->status != NDSP_WBUF_FREE && wb->status != NDSP_WBUF_DONE) continue;
        if (audio_paused) {
            memset(wb->data_pcm16, 0, audio_bytes_per_buf);
        } else {
            audio_cb(audio_userdata, (Uint8*)wb->data_pcm16, audio_bytes_per_buf);
        }
        DSP_FlushDataCache(wb->data_pcm16, audio_bytes_per_buf);
        ndspChnWaveBufAdd(0, wb);
    }
}

SDL_AudioDeviceID SDL_OpenAudioDevice(const char* device, int iscapture, const SDL_AudioSpec* want,
                                      SDL_AudioSpec* have, int allowed_changes) {
    (void)device; (void)iscapture; (void)allowed_changes;
    if (audio_open) return 0;
    if (R_FAILED(ndspInit())) {
        printf("[3DS] ndspInit failed (is sdmc:/3ds/dspfirm.cdc present?)\n");
        return 0;
    }

    audio_cb = want->callback;
    audio_userdata = want->userdata;
    audio_bytes_per_buf = want->samples * want->channels * (int)sizeof(s16);
    audio_mem = (s16*)linearAlloc(audio_bytes_per_buf * N3DS_AUDIO_BUFS);
    if (!audio_mem) {
        ndspExit();
        return 0;
    }
    memset(audio_mem, 0, audio_bytes_per_buf * N3DS_AUDIO_BUFS);

    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(0);
    ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
    ndspChnSetRate(0, (float)want->freq);
    ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
    float mix[12] = { 1.0f, 1.0f };
    ndspChnSetMix(0, mix);

    memset(audio_wbuf, 0, sizeof(audio_wbuf));
    for (int i = 0; i < N3DS_AUDIO_BUFS; i++) {
        audio_wbuf[i].data_pcm16 = audio_mem + i * (audio_bytes_per_buf / (int)sizeof(s16));
        audio_wbuf[i].nsamples = want->samples;
        audio_wbuf[i].status = NDSP_WBUF_FREE;
    }

    if (have) *have = *want;
    audio_open = 1;
    ndspSetCallback(n3ds_audio_fill, NULL);
    n3ds_audio_fill(NULL);
    return 1;
}

void SDL_PauseAudioDevice(SDL_AudioDeviceID dev, int pause_on) { (void)dev; audio_paused = pause_on; }

void SDL_CloseAudioDevice(SDL_AudioDeviceID dev) {
    (void)dev;
    if (!audio_open) return;
    ndspSetCallback(NULL, NULL);
    ndspChnReset(0);
    ndspExit();
    linearFree(audio_mem);
    audio_mem = NULL;
    audio_open = 0;
}

/* --- window / GL (drawing is in n3ds_gl.c) --- */

static int dummy_window;
SDL_Window* SDL_CreateWindow(const char* t, int x, int y, int w, int h, Uint32 f) {
    (void)t; (void)x; (void)y; (void)w; (void)h; (void)f;
    return (SDL_Window*)&dummy_window;
}
void SDL_DestroyWindow(SDL_Window* w) { (void)w; }
/* pc_vi.c puts the FPS in the window title every 60 frames: use it as a log heartbeat */
void SDL_SetWindowTitle(SDL_Window* w, const char* t) { (void)w; printf("[title] %s\n", t); }
void SDL_SetWindowSize(SDL_Window* w, int a, int b) { (void)w; (void)a; (void)b; }
void SDL_SetWindowPosition(SDL_Window* w, int a, int b) { (void)w; (void)a; (void)b; }
void SDL_SetWindowBordered(SDL_Window* w, SDL_bool b) { (void)w; (void)b; }
int SDL_SetWindowFullscreen(SDL_Window* w, Uint32 f) { (void)w; (void)f; return 0; }
int SDL_SetWindowDisplayMode(SDL_Window* w, const SDL_DisplayMode* m) { (void)w; (void)m; return 0; }
int SDL_GetWindowDisplayIndex(SDL_Window* w) { (void)w; return 0; }
int SDL_GetDesktopDisplayMode(int d, SDL_DisplayMode* m) {
    (void)d;
    memset(m, 0, sizeof(*m));
    m->w = 400; m->h = 240; m->refresh_rate = 60;
    return 0;
}
SDL_DisplayMode* SDL_GetClosestDisplayMode(int d, const SDL_DisplayMode* m, SDL_DisplayMode* c) {
    (void)m;
    SDL_GetDesktopDisplayMode(d, c);
    return c;
}
int SDL_ShowSimpleMessageBox(Uint32 f, const char* title, const char* msg, SDL_Window* w) {
    (void)f; (void)w;
    printf("%s\n%s\n", title, msg);
    return 0;
}
int SDL_GL_SetAttribute(SDL_GLattr a, int v) { (void)a; (void)v; return 0; }
SDL_GLContext SDL_GL_CreateContext(SDL_Window* w) { (void)w; return (SDL_GLContext)&dummy_window; }
void SDL_GL_DeleteContext(SDL_GLContext c) { (void)c; }
int SDL_GL_SetSwapInterval(int i) { (void)i; return 0; }
extern void n3ds_gl_swap(void);
void SDL_GL_SwapWindow(SDL_Window* w) { (void)w; n3ds_gl_swap(); }
void SDL_GL_GetDrawableSize(SDL_Window* w, int* width, int* height) {
    (void)w;
    if (width) *width = 400;
    if (height) *height = 240;
}

/* --- keyboard / mouse --- */

static const Uint8 no_keys[SDL_NUM_SCANCODES];
const Uint8* SDL_GetKeyboardState(int* n) { if (n) *n = SDL_NUM_SCANCODES; return no_keys; }
Uint32 SDL_GetMouseState(int* x, int* y) { if (x) *x = 0; if (y) *y = 0; return 0; }
SDL_Scancode SDL_GetScancodeFromName(const char* name) { (void)name; return SDL_SCANCODE_UNKNOWN; }
const char* SDL_GetScancodeName(SDL_Scancode sc) { (void)sc; return ""; }
void SDL_StartTextInput(void) {}
void SDL_StopTextInput(void) {}

/* --- game controller: the 3DS itself --- */

static const u32 button_map[SDL_CONTROLLER_BUTTON_MAX] = {
    [SDL_CONTROLLER_BUTTON_A] = KEY_A,
    [SDL_CONTROLLER_BUTTON_B] = KEY_B,
    [SDL_CONTROLLER_BUTTON_X] = KEY_X,
    [SDL_CONTROLLER_BUTTON_Y] = KEY_Y,
    [SDL_CONTROLLER_BUTTON_BACK] = KEY_SELECT,
    [SDL_CONTROLLER_BUTTON_START] = KEY_START,
    [SDL_CONTROLLER_BUTTON_LEFTSHOULDER] = KEY_ZL,
    [SDL_CONTROLLER_BUTTON_RIGHTSHOULDER] = KEY_ZR,
    [SDL_CONTROLLER_BUTTON_DPAD_UP] = KEY_DUP,
    [SDL_CONTROLLER_BUTTON_DPAD_DOWN] = KEY_DDOWN,
    [SDL_CONTROLLER_BUTTON_DPAD_LEFT] = KEY_DLEFT,
    [SDL_CONTROLLER_BUTTON_DPAD_RIGHT] = KEY_DRIGHT,
};
static const char* const button_names[SDL_CONTROLLER_BUTTON_MAX] = {
    "a", "b", "x", "y", "back", "guide", "start", "leftstick", "rightstick",
    "leftshoulder", "rightshoulder", "dpup", "dpdown", "dpleft", "dpright",
};
static const char* const axis_names[SDL_CONTROLLER_AXIS_MAX] = {
    "leftx", "lefty", "rightx", "righty", "lefttrigger", "righttrigger",
};

static int dummy_controller;
int SDL_NumJoysticks(void) { return 1; }
SDL_bool SDL_IsGameController(int i) { return i == 0 ? SDL_TRUE : SDL_FALSE; }
SDL_GameController* SDL_GameControllerOpen(int i) { return i == 0 ? (SDL_GameController*)&dummy_controller : NULL; }
void SDL_GameControllerClose(SDL_GameController* c) { (void)c; }
SDL_bool SDL_GameControllerGetAttached(SDL_GameController* c) { (void)c; return SDL_TRUE; }
int SDL_GameControllerRumble(SDL_GameController* c, Uint16 lo, Uint16 hi, Uint32 ms) {
    (void)c; (void)lo; (void)hi; (void)ms;
    return -1;
}

Uint8 SDL_GameControllerGetButton(SDL_GameController* c, SDL_GameControllerButton b) {
    (void)c;
    if (b < 0 || b >= SDL_CONTROLLER_BUTTON_MAX || !button_map[b]) return 0;
    return (hidKeysHeld() & button_map[b]) ? 1 : 0;
}

/* circle pad / C-stick range is about +-156; scale to SDL's +-32767 */
static Sint16 scale_stick(int v) {
    int s = v * 210;
    if (s > 32767) s = 32767;
    if (s < -32768) s = -32768;
    return (Sint16)s;
}

Sint16 SDL_GameControllerGetAxis(SDL_GameController* c, SDL_GameControllerAxis a) {
    (void)c;
    circlePosition p;
    switch (a) {
        case SDL_CONTROLLER_AXIS_LEFTX:  hidCircleRead(&p); return scale_stick(p.dx);
        case SDL_CONTROLLER_AXIS_LEFTY:  hidCircleRead(&p); return scale_stick(-p.dy);
        case SDL_CONTROLLER_AXIS_RIGHTX: hidCstickRead(&p); return scale_stick(p.dx);
        case SDL_CONTROLLER_AXIS_RIGHTY: hidCstickRead(&p); return scale_stick(-p.dy);
        case SDL_CONTROLLER_AXIS_TRIGGERLEFT:  return (hidKeysHeld() & KEY_L) ? 32767 : 0;
        case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: return (hidKeysHeld() & KEY_R) ? 32767 : 0;
        default: return 0;
    }
}

SDL_GameControllerButton SDL_GameControllerGetButtonFromString(const char* s) {
    for (int i = 0; i < SDL_CONTROLLER_BUTTON_MAX; i++)
        if (s && strcasecmp(s, button_names[i]) == 0) return (SDL_GameControllerButton)i;
    return SDL_CONTROLLER_BUTTON_INVALID;
}
SDL_GameControllerAxis SDL_GameControllerGetAxisFromString(const char* s) {
    for (int i = 0; i < SDL_CONTROLLER_AXIS_MAX; i++)
        if (s && strcasecmp(s, axis_names[i]) == 0) return (SDL_GameControllerAxis)i;
    return SDL_CONTROLLER_AXIS_INVALID;
}
const char* SDL_GameControllerGetStringForButton(SDL_GameControllerButton b) {
    return (b >= 0 && b < SDL_CONTROLLER_BUTTON_MAX) ? button_names[b] : NULL;
}
const char* SDL_GameControllerGetStringForAxis(SDL_GameControllerAxis a) {
    return (a >= 0 && a < SDL_CONTROLLER_AXIS_MAX) ? axis_names[a] : NULL;
}

/* --- events: one hidScanInput per poll cycle, button presses become events --- */

static SDL_Event event_queue[SDL_CONTROLLER_BUTTON_MAX + 1];
static int event_count;
static int event_pos;

void SDL_PumpEvents(void) {}

int SDL_PollEvent(SDL_Event* ev) {
    if (event_pos == 0 && event_count == 0) {
        if (!aptMainLoop()) {
            event_queue[event_count++].type = SDL_QUIT;
        } else {
            hidScanInput();
            u32 down = hidKeysDown();
            for (int i = 0; i < SDL_CONTROLLER_BUTTON_MAX; i++) {
                if (button_map[i] && (down & button_map[i])) {
                    SDL_Event* e = &event_queue[event_count++];
                    memset(e, 0, sizeof(*e));
                    e->cbutton.type = SDL_CONTROLLERBUTTONDOWN;
                    e->cbutton.button = (Uint8)i;
                }
            }
        }
    }
    if (event_pos < event_count) {
        *ev = event_queue[event_pos++];
        return 1;
    }
    event_pos = event_count = 0;
    return 0;
}
