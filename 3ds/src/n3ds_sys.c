/* n3ds_sys.c - 3DS process setup: memory layout, screens, and the game thread.
 *
 * emu64's seg2k0 treats any address in 0x03000000-0x0FFFFFFF as an N64
 * segment address. On 3DS the default newlib heap (0x08000000) and the main
 * thread stack (just below 0x10000000) both fall in that range. So:
 *   - all heap memory comes from the LINEAR region (0x14000000 or 0x30000000)
 *   - the game runs on a thread whose stack is allocated from that heap
 */
#include <3ds.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/iosupport.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

/* linearAlloc pool: GPU buffers, textures, audio. O3DS homebrew gets ~64MB total and the
 * game needs 24MB main RAM + 16MB ARAM + assets, so keep this small. */
#define N3DS_GPU_LINEAR_SIZE (12 * 1024 * 1024)
#define N3DS_GAME_STACK_SIZE (2 * 1024 * 1024)

extern char* fake_heap_start;
extern char* fake_heap_end;
extern u32 __ctru_heap, __ctru_heap_size;
extern u32 __ctru_linear_heap, __ctru_linear_heap_size;

/* Replaces libctru's weak default (libctru 2.7 allocateHeaps.c, same steps).
 * One linear allocation holds both pools: [ linearAlloc pool | newlib malloc heap ]. */
void __system_allocateHeaps(void) {
    Handle reslimit = 0;
    if (R_FAILED(svcGetResourceLimit(&reslimit, CUR_PROCESS_HANDLE))) svcBreak(USERBREAK_PANIC);
    s64 max_commit = 0, cur_commit = 0;
    ResourceLimitType type = RESLIMIT_COMMIT;
    svcGetResourceLimitLimitValues(&max_commit, reslimit, &type, 1);
    svcGetResourceLimitCurrentValues(&cur_commit, reslimit, &type, 1);
    svcCloseHandle(reslimit);

    u32 total = (u32)(max_commit - cur_commit) & ~0xFFFu;
    u32 base = 0;
    if (total <= N3DS_GPU_LINEAR_SIZE ||
        R_FAILED(svcControlMemory(&base, 0, 0, total, MEMOP_ALLOC_LINEAR, MEMPERM_READ | MEMPERM_WRITE))) {
        svcBreak(USERBREAK_PANIC);
    }

    __ctru_linear_heap = base;
    __ctru_linear_heap_size = N3DS_GPU_LINEAR_SIZE;
    __ctru_heap = base + N3DS_GPU_LINEAR_SIZE;
    __ctru_heap_size = total - N3DS_GPU_LINEAR_SIZE;

    mappableInit(OS_MAP_AREA_BEGIN, OS_MAP_AREA_END);

    fake_heap_start = (char*)__ctru_heap;
    fake_heap_end = fake_heap_start + __ctru_heap_size;
}

/* stdout/stderr -> bottom-screen console AND svcOutputDebugString
 * (emulators and the Luma3DS debugger show the latter in their log). */
static const devoptab_t* console_dev;

static int log_fd = -1; /* sdmc:/3ds/AnimalCrossing/log.txt, unbuffered */

static ssize_t tee_write(struct _reent* r, void* fd, const char* ptr, size_t len) {
    svcOutputDebugString(ptr, (s32)len);
    if (log_fd >= 0) write(log_fd, ptr, len);
    return console_dev->write_r(r, fd, ptr, len);
}

static devoptab_t tee_dev = { .name = "tee", .write_r = tee_write };

static void n3ds_install_log_tee(void) {
    log_fd = open("log.txt", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    console_dev = devoptab_list[STD_OUT];
    tee_dev.structSize = console_dev->structSize;
    devoptab_list[STD_OUT] = &tee_dev;
    devoptab_list[STD_ERR] = &tee_dev;
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
}

/* Called from SDL_Init (pc_platform_init). */
void n3ds_platform_init(void) {
    static int done;
    if (done) return;
    done = 1;

    bool is_new = false;
    APT_CheckNew3DS(&is_new);
    if (is_new) {
        osSetSpeedupEnable(true); /* 804 MHz + L2 cache */
    } else {
        APT_SetAppCpuTimeLimit(30); /* lets worker threads use core 1 */
    }

    gfxInitDefault();
    consoleInit(GFX_BOTTOM, NULL);
    n3ds_install_log_tee();
    printf("Animal Crossing 3DS (%s)\n", is_new ? "N3DS" : "O3DS");
    printf("heap %08lx +%luKB\n", __ctru_heap, __ctru_heap_size >> 10);
}

/* pc_main.c's main() is renamed via -Dmain=pc_main_entry */
extern int pc_main_entry(int argc, char* argv[]);

static int g_argc;
static char** g_argv;
static int g_ret;

static void game_thread(void* arg) {
    (void)arg;
    g_ret = pc_main_entry(g_argc, g_argv);
}

/* All runtime files (rom/, shaders/, save/, settings.ini) live here. */
#define N3DS_DATA_DIR "sdmc:/3ds/AnimalCrossing"

int main(int argc, char* argv[]) {
    /* hbmenu passes only the path; turn on the PC layer's diagnostic output */
    static char* default_argv[] = { "ac_3ds", "--verbose", NULL };
    g_argc = argc > 1 ? argc : 2;
    g_argv = argc > 1 ? argv : default_argv;

    mkdir("sdmc:/3ds", 0777);
    mkdir(N3DS_DATA_DIR, 0777);
    chdir(N3DS_DATA_DIR);

    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    Thread t = threadCreate(game_thread, NULL, N3DS_GAME_STACK_SIZE, prio, 0, false);
    if (!t) {
        gfxInitDefault();
        consoleInit(GFX_TOP, NULL);
        printf("failed to create game thread\nSTART to exit\n");
        while (aptMainLoop()) {
            hidScanInput();
            if (hidKeysDown() & KEY_START) break;
            gspWaitForVBlank();
        }
        gfxExit();
        return 1;
    }
    threadJoin(t, U64_MAX);
    threadFree(t);

    printf("exited (%d). START to quit\n", g_ret);
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        gspWaitForVBlank();
    }
    gfxExit();
    return g_ret;
}
