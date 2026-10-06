/* SDL.h - minimal SDL2 API subset for the 3DS build.
 *
 * The PC port layer (pc/src) is written against SDL2. devkitPro only ships
 * SDL 1.2 for 3DS, so this header declares the exact subset that pc/src uses
 * and n3ds_sdl.c implements it on libctru. Window/GL calls are no-ops; the
 * 3DS renderer replaces the GL backend directly.
 */
#ifndef N3DS_SDL_SHIM_H
#define N3DS_SDL_SHIM_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <strings.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t  Uint8;
typedef int8_t   Sint8;
typedef uint16_t Uint16;
typedef int16_t  Sint16;
typedef uint32_t Uint32;
typedef int32_t  Sint32;
typedef uint64_t Uint64;
typedef int64_t  Sint64;
typedef enum { SDL_FALSE = 0, SDL_TRUE = 1 } SDL_bool;

#define SDL_MAIN_HANDLED
#define SDL_INIT_TIMER          0x0001u
#define SDL_INIT_AUDIO          0x0010u
#define SDL_INIT_VIDEO          0x0020u
#define SDL_INIT_GAMECONTROLLER 0x2000u

#define SDL_HINT_WINDOWS_INTRESOURCE_ICON "SDL_WINDOWS_INTRESOURCE_ICON"

int  SDL_Init(Uint32 flags);
void SDL_Quit(void);
void SDL_SetMainReady(void);
const char* SDL_GetError(void);
SDL_bool SDL_SetHint(const char* name, const char* value);
#define SDL_strcasecmp strcasecmp

/* --- timing --- */
Uint32 SDL_GetTicks(void);
Uint64 SDL_GetPerformanceCounter(void);
Uint64 SDL_GetPerformanceFrequency(void);
void   SDL_Delay(Uint32 ms);

/* --- threads, mutexes, atomics --- */
typedef struct SDL_Thread SDL_Thread;
typedef struct SDL_mutex SDL_mutex;
typedef int (*SDL_ThreadFunction)(void* data);
typedef struct { volatile int value; } SDL_atomic_t;

SDL_Thread* SDL_CreateThread(SDL_ThreadFunction fn, const char* name, void* data);
void SDL_WaitThread(SDL_Thread* thread, int* status);
SDL_mutex* SDL_CreateMutex(void);
void SDL_DestroyMutex(SDL_mutex* m);
int  SDL_LockMutex(SDL_mutex* m);
int  SDL_UnlockMutex(SDL_mutex* m);

static inline int  SDL_AtomicGet(SDL_atomic_t* a) { return __atomic_load_n(&a->value, __ATOMIC_SEQ_CST); }
static inline int  SDL_AtomicSet(SDL_atomic_t* a, int v) { return __atomic_exchange_n(&a->value, v, __ATOMIC_SEQ_CST); }
#define SDL_MemoryBarrierAcquire() __atomic_thread_fence(__ATOMIC_ACQUIRE)
#define SDL_MemoryBarrierRelease() __atomic_thread_fence(__ATOMIC_RELEASE)

/* --- audio --- */
typedef Uint32 SDL_AudioDeviceID;
typedef Uint16 SDL_AudioFormat;
typedef void (*SDL_AudioCallback)(void* userdata, Uint8* stream, int len);
#define AUDIO_S16SYS 0x8010
typedef struct SDL_AudioSpec {
    int freq;
    SDL_AudioFormat format;
    Uint8 channels;
    Uint8 silence;
    Uint16 samples;
    Uint16 padding;
    Uint32 size;
    SDL_AudioCallback callback;
    void* userdata;
} SDL_AudioSpec;

SDL_AudioDeviceID SDL_OpenAudioDevice(const char* device, int iscapture, const SDL_AudioSpec* desired,
                                      SDL_AudioSpec* obtained, int allowed_changes);
void SDL_PauseAudioDevice(SDL_AudioDeviceID dev, int pause_on);
void SDL_CloseAudioDevice(SDL_AudioDeviceID dev);

/* --- window / GL (no-ops on 3DS) --- */
typedef struct SDL_Window SDL_Window;
typedef void* SDL_GLContext;
typedef struct SDL_DisplayMode { Uint32 format; int w, h, refresh_rate; void* driverdata; } SDL_DisplayMode;
typedef enum {
    SDL_GL_DOUBLEBUFFER = 5, SDL_GL_DEPTH_SIZE = 6, SDL_GL_MULTISAMPLEBUFFERS = 13,
    SDL_GL_MULTISAMPLESAMPLES = 14, SDL_GL_CONTEXT_MAJOR_VERSION = 17,
    SDL_GL_CONTEXT_MINOR_VERSION = 18, SDL_GL_CONTEXT_PROFILE_MASK = 21
} SDL_GLattr;
#define SDL_GL_CONTEXT_PROFILE_CORE 0x0001
#define SDL_WINDOW_FULLSCREEN         0x00000001u
#define SDL_WINDOW_OPENGL             0x00000002u
#define SDL_WINDOW_SHOWN              0x00000004u
#define SDL_WINDOW_RESIZABLE          0x00000020u
#define SDL_WINDOW_FULLSCREEN_DESKTOP (SDL_WINDOW_FULLSCREEN | 0x00001000u)
#define SDL_WINDOWPOS_CENTERED        0x2FFF0000u
#define SDL_MESSAGEBOX_ERROR          0x10

SDL_Window* SDL_CreateWindow(const char* title, int x, int y, int w, int h, Uint32 flags);
void SDL_DestroyWindow(SDL_Window* w);
void SDL_SetWindowTitle(SDL_Window* w, const char* title);
void SDL_SetWindowSize(SDL_Window* w, int width, int height);
void SDL_SetWindowPosition(SDL_Window* w, int x, int y);
void SDL_SetWindowBordered(SDL_Window* w, SDL_bool bordered);
int  SDL_SetWindowFullscreen(SDL_Window* w, Uint32 flags);
int  SDL_SetWindowDisplayMode(SDL_Window* w, const SDL_DisplayMode* mode);
int  SDL_GetWindowDisplayIndex(SDL_Window* w);
int  SDL_GetDesktopDisplayMode(int display, SDL_DisplayMode* mode);
SDL_DisplayMode* SDL_GetClosestDisplayMode(int display, const SDL_DisplayMode* mode, SDL_DisplayMode* closest);
int  SDL_ShowSimpleMessageBox(Uint32 flags, const char* title, const char* message, SDL_Window* w);
int  SDL_GL_SetAttribute(SDL_GLattr attr, int value);
SDL_GLContext SDL_GL_CreateContext(SDL_Window* w);
void SDL_GL_DeleteContext(SDL_GLContext ctx);
void* SDL_GL_GetProcAddress(const char* proc);
int  SDL_GL_SetSwapInterval(int interval);
void SDL_GL_SwapWindow(SDL_Window* w);
void SDL_GL_GetDrawableSize(SDL_Window* w, int* width, int* height);

/* --- keyboard / mouse (no keyboard on 3DS; state is always zero) --- */
typedef int SDL_Scancode;
typedef int SDL_Keycode;
enum {
    SDL_SCANCODE_UNKNOWN = 0,
    SDL_SCANCODE_A = 4, SDL_SCANCODE_D = 7, SDL_SCANCODE_E = 8, SDL_SCANCODE_I = 12,
    SDL_SCANCODE_J = 13, SDL_SCANCODE_K = 14, SDL_SCANCODE_L = 15, SDL_SCANCODE_Q = 20,
    SDL_SCANCODE_S = 22, SDL_SCANCODE_W = 26, SDL_SCANCODE_X = 27, SDL_SCANCODE_Y = 28,
    SDL_SCANCODE_Z = 29, SDL_SCANCODE_RETURN = 40, SDL_SCANCODE_ESCAPE = 41,
    SDL_SCANCODE_BACKSPACE = 42, SDL_SCANCODE_TAB = 43, SDL_SCANCODE_SPACE = 44,
    SDL_SCANCODE_DELETE = 76, SDL_SCANCODE_RIGHT = 79, SDL_SCANCODE_LEFT = 80,
    SDL_SCANCODE_DOWN = 81, SDL_SCANCODE_UP = 82, SDL_SCANCODE_LCTRL = 224,
    SDL_SCANCODE_LSHIFT = 225, SDL_NUM_SCANCODES = 512
};
enum {
    SDLK_BACKSPACE = '\b', SDLK_TAB = '\t', SDLK_RETURN = '\r', SDLK_ESCAPE = 27, SDLK_SPACE = ' ',
    SDLK_a = 'a', SDLK_d = 'd', SDLK_s = 's', SDLK_w = 'w',
    SDLK_F3 = (1 << 30) | 60, SDLK_RIGHT = (1 << 30) | 79, SDLK_LEFT = (1 << 30) | 80,
    SDLK_DOWN = (1 << 30) | 81, SDLK_UP = (1 << 30) | 82, SDLK_KP_ENTER = (1 << 30) | 88
};
#define SDL_BUTTON(x)      (1u << ((x) - 1))
#define SDL_BUTTON_LEFT    1
#define SDL_BUTTON_MIDDLE  2
#define SDL_BUTTON_RIGHT   3

const Uint8* SDL_GetKeyboardState(int* numkeys);
Uint32 SDL_GetMouseState(int* x, int* y);
SDL_Scancode SDL_GetScancodeFromName(const char* name);
const char* SDL_GetScancodeName(SDL_Scancode sc);
void SDL_StartTextInput(void);
void SDL_StopTextInput(void);

/* --- game controller (backed by the 3DS buttons) --- */
typedef struct SDL_GameController SDL_GameController;
typedef enum {
    SDL_CONTROLLER_BUTTON_INVALID = -1,
    SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_Y,
    SDL_CONTROLLER_BUTTON_BACK, SDL_CONTROLLER_BUTTON_GUIDE, SDL_CONTROLLER_BUTTON_START,
    SDL_CONTROLLER_BUTTON_LEFTSTICK, SDL_CONTROLLER_BUTTON_RIGHTSTICK,
    SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
    SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN,
    SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
    SDL_CONTROLLER_BUTTON_MAX
} SDL_GameControllerButton;
typedef enum {
    SDL_CONTROLLER_AXIS_INVALID = -1,
    SDL_CONTROLLER_AXIS_LEFTX, SDL_CONTROLLER_AXIS_LEFTY, SDL_CONTROLLER_AXIS_RIGHTX,
    SDL_CONTROLLER_AXIS_RIGHTY, SDL_CONTROLLER_AXIS_TRIGGERLEFT, SDL_CONTROLLER_AXIS_TRIGGERRIGHT,
    SDL_CONTROLLER_AXIS_MAX
} SDL_GameControllerAxis;

int  SDL_NumJoysticks(void);
SDL_bool SDL_IsGameController(int index);
SDL_GameController* SDL_GameControllerOpen(int index);
void SDL_GameControllerClose(SDL_GameController* c);
SDL_bool SDL_GameControllerGetAttached(SDL_GameController* c);
Uint8 SDL_GameControllerGetButton(SDL_GameController* c, SDL_GameControllerButton b);
Sint16 SDL_GameControllerGetAxis(SDL_GameController* c, SDL_GameControllerAxis a);
int  SDL_GameControllerRumble(SDL_GameController* c, Uint16 lo, Uint16 hi, Uint32 ms);
SDL_GameControllerButton SDL_GameControllerGetButtonFromString(const char* s);
SDL_GameControllerAxis SDL_GameControllerGetAxisFromString(const char* s);
const char* SDL_GameControllerGetStringForButton(SDL_GameControllerButton b);
const char* SDL_GameControllerGetStringForAxis(SDL_GameControllerAxis a);

/* --- events --- */
enum {
    SDL_QUIT = 0x100, SDL_WINDOWEVENT = 0x200, SDL_KEYDOWN = 0x300, SDL_TEXTINPUT = 0x303,
    SDL_MOUSEBUTTONDOWN = 0x401, SDL_CONTROLLERAXISMOTION = 0x650, SDL_CONTROLLERBUTTONDOWN = 0x651
};
enum { SDL_WINDOWEVENT_SIZE_CHANGED = 6 };
typedef struct { SDL_Scancode scancode; SDL_Keycode sym; Uint16 mod; } SDL_Keysym;
typedef union SDL_Event {
    Uint32 type;
    struct { Uint32 type; Uint8 event; } window;
    struct { Uint32 type; Uint8 repeat; SDL_Keysym keysym; } key;
    struct { Uint32 type; char text[32]; } text;
    struct { Uint32 type; Uint8 button; } button;
    struct { Uint32 type; Uint8 button; } cbutton;
    struct { Uint32 type; Uint8 axis; Sint16 value; } caxis;
    Uint8 padding[56];
} SDL_Event;

int  SDL_PollEvent(SDL_Event* event);
void SDL_PumpEvents(void);

#ifdef __cplusplus
}
#endif

#endif /* N3DS_SDL_SHIM_H */
