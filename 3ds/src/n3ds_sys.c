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
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/iosupport.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

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
static s64 s_commit_max, s_commit_cur; /* logged in n3ds_platform_init */

/* socInit turns its buffer into a shared memory block, and the kernel refuses linear
 * memory for that; the whole newlib heap is linear here. So this much of the quota is kept
 * out of the linear block and mapped as normal memory by the live log.
 * ponytail: 1 MB of heap for a development feature; set 0 for a release build. */
#define N3DS_SOC_SIZE 0x100000

void __system_allocateHeaps(void) {
    Handle reslimit = 0;
    if (R_FAILED(svcGetResourceLimit(&reslimit, CUR_PROCESS_HANDLE))) svcBreak(USERBREAK_PANIC);
    s64 max_commit = 0, cur_commit = 0;
    ResourceLimitType type = RESLIMIT_COMMIT;
    svcGetResourceLimitLimitValues(&max_commit, reslimit, &type, 1);
    svcGetResourceLimitCurrentValues(&cur_commit, reslimit, &type, 1);
    svcCloseHandle(reslimit);
    /* Azahar gives a 3dsx a 96 MB region even in O3DS mode; a retail O3DS gives 64 MB.
     * Use the retail size on O3DS (128 MB total) so emulator runs hit the real limit. */
    if (osGetMemRegionSize(MEMREGION_ALL) <= 128u * 1024 * 1024 && max_commit > 64ll * 1024 * 1024)
        max_commit = 64ll * 1024 * 1024;
    s_commit_max = max_commit;
    s_commit_cur = cur_commit;

    u32 total = (u32)(max_commit - cur_commit) & ~0xFFFu;
    total -= N3DS_SOC_SIZE; /* kept for the live-log network buffer (must not be linear) */
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

/* Bottom screen: status rows 0-3 (updated in place, n3ds_status) and a separator,
 * then the live log in rows 5-29. The lock keeps threads from mixing the two. */
#define STATUS_ROWS 4
static PrintConsole s_con_log, s_con_status;
static LightLock s_con_lock;

/* Live log over Wi-Fi: when started from the netloader, the game runs a log server on
 * TCP port 17492. 3ds/push_3ds.sh connects to it (PC -> 3DS, the same direction as the
 * 3dslink upload and FTP, so a PC firewall or VPN does not block it). The last 64 KB of
 * the log are kept and sent on connect, then each new line is sent live. */
#define LIVE_LOG_PORT 17492
static int s_netloaded; /* set in main: started by the Homebrew Launcher netloader */
#define LOG_RING (64 * 1024)
static char s_ring[LOG_RING];
static u32 s_ring_pos;
static int s_ring_full;
static int s_listen = -1;
static int s_client = -1;
static char s_live_status[96];

static void ring_add(const char* p, size_t n) {
    while (n) {
        size_t k = LOG_RING - s_ring_pos;
        if (k > n) k = n;
        memcpy(s_ring + s_ring_pos, p, k);
        s_ring_pos = (s_ring_pos + k) % LOG_RING;
        if (s_ring_pos == 0) s_ring_full = 1;
        p += k;
        n -= k;
    }
}

static int send_all(int sock, const char* p, size_t n) {
    while (n) {
        int k = send(sock, p, n, 0);
        if (k <= 0) return -1;
        p += k;
        n -= (size_t)k;
    }
    return 0;
}

/* Accepts one client at a time; on connect sends the kept log, then tee_write streams */
static void log_server_thread(void* arg) {
    (void)arg;
    for (;;) {
        if (s_client < 0) {
            int c = accept(s_listen, NULL, NULL);
            if (c >= 0) {
                LightLock_Lock(&s_con_lock);
                int bad = 0;
                if (s_ring_full) bad |= send_all(c, s_ring + s_ring_pos, LOG_RING - s_ring_pos);
                bad |= send_all(c, s_ring, s_ring_pos);
                if (bad) close(c);
                else s_client = c;
                LightLock_Unlock(&s_con_lock);
            }
        }
        svcSleepThread(100000000LL);
    }
}

static void n3ds_init_live_log(int netloaded) {
    static const u32 soc_size = N3DS_SOC_SIZE;
    if (!netloaded) return;
    u32 soc_addr = 0;
    Result rc = svcControlMemory(&soc_addr, OS_HEAP_AREA_BEGIN, 0, soc_size, MEMOP_ALLOC, MEMPERM_READ | MEMPERM_WRITE);
    u32* soc_buf = R_SUCCEEDED(rc) ? (u32*)soc_addr : NULL;
    if (soc_buf) rc = socInit(soc_buf, soc_size);
    if (R_FAILED(rc)) {
        snprintf(s_live_status, sizeof(s_live_status), "live log: off, socInit failed (0x%08lX)", (unsigned long)rc);
        if (soc_buf) svcControlMemory(&soc_addr, soc_addr, 0, soc_size, MEMOP_FREE, 0);
        return;
    }
    s_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(LIVE_LOG_PORT);
    a.sin_addr.s_addr = INADDR_ANY;
    if (s_listen < 0 || bind(s_listen, (struct sockaddr*)&a, sizeof(a)) < 0 || listen(s_listen, 1) < 0) {
        snprintf(s_live_status, sizeof(s_live_status), "live log: off, socket setup failed (errno %d)", errno);
        return;
    }
    fcntl(s_listen, F_SETFL, fcntl(s_listen, F_GETFL, 0) | O_NONBLOCK);
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    if (!threadCreate(log_server_thread, NULL, 16 * 1024, prio - 1, 1, true) &&
        !threadCreate(log_server_thread, NULL, 16 * 1024, prio - 1, -2, true)) {
        snprintf(s_live_status, sizeof(s_live_status), "live log: off, thread create failed");
        return;
    }
    struct in_addr ip = { (u32)gethostid() };
    snprintf(s_live_status, sizeof(s_live_status), "live log: listening on %s:%d", inet_ntoa(ip), LIVE_LOG_PORT);
}

static ssize_t tee_write(struct _reent* r, void* fd, const char* ptr, size_t len) {
    svcOutputDebugString(ptr, (s32)len);
    if (log_fd >= 0) write(log_fd, ptr, len);
    LightLock_Lock(&s_con_lock);
    ring_add(ptr, len);
    if (s_client >= 0 && send_all(s_client, ptr, len) < 0) { /* PC client went away */
        close(s_client);
        s_client = -1;
    }
    consoleSelect(&s_con_log);
    ssize_t n = console_dev->write_r(r, fd, ptr, len);
    LightLock_Unlock(&s_con_lock);
    return n;
}

/* Log line without the console lock (watchdog: works even if a thread holds the lock) */
void n3ds_log_raw(const char* s) {
    size_t len = strlen(s);
    svcOutputDebugString(s, (s32)len);
    if (log_fd >= 0) { write(log_fd, s, len); fsync(log_fd); }
    if (s_client >= 0) send(s_client, s, len, 0);
}

/* Overwrites status row 0..STATUS_ROWS-1 on the bottom screen (not logged) */
void n3ds_status(int row, const char* fmt, ...) {
    char buf[48];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0 || row < 0 || row >= STATUS_ROWS || !console_dev) return;
    if (n > 38) n = 38; /* 40 columns from column 1; a full row would wrap */
    memset(buf + n, ' ', 38 - n);
    LightLock_Lock(&s_con_lock);
    consoleSelect(&s_con_status);
    s_con_status.cursorX = 1; /* libctru cursor columns start at 1 here: 0 cut the first character */
    s_con_status.cursorY = row;
    console_dev->write_r(_REENT, 0, buf, 38);
    consoleSelect(&s_con_log);
    LightLock_Unlock(&s_con_lock);
}

static void n3ds_init_consoles(void) {
    LightLock_Init(&s_con_lock);
    consoleInit(GFX_BOTTOM, &s_con_log);
    s_con_status = s_con_log;
    consoleSetWindow(&s_con_status, 0, 0, 40, STATUS_ROWS + 1);
    consoleSetWindow(&s_con_log, 0, STATUS_ROWS + 1, 40, 30 - STATUS_ROWS - 1);
    consoleSelect(&s_con_status);
    s_con_status.cursorY = STATUS_ROWS;
    printf("---------------------------------------"); /* 39: a 40th column would scroll the window */
    consoleSelect(&s_con_log);
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

/* CPU exception -> "[CRASH]" lines in log.txt, then halt. 3ds/run_azahar.ps1 runs
 * addr2line on them. Stack words below 16MB are possible return addresses
 * (code and static data live there); addr2line drops the ones that are not code. */
/* ponytail: one handler stack for all threads; two faults at once would share it */
static u8 crash_stack[0x4000] __attribute__((aligned(8)));

static void crash_log(const char* s, int n) {
    svcOutputDebugString(s, n);
    if (log_fd >= 0) { write(log_fd, s, (size_t)n); fsync(log_fd); } /* emulators buffer SD writes */
}

static void crash_handler(ERRF_ExceptionInfo* excep, CpuRegisters* regs) {
    static const char* const names[] = { "prefetch abort", "data abort", "undefined instruction", "VFP" };
    char buf[160];
    int n = snprintf(buf, sizeof(buf), "\n[CRASH] %s thread=%lx pc=%08lx lr=%08lx sp=%08lx far=%08lx\n",
                     excep->type < 4 ? names[excep->type] : "?", (unsigned long)threadGetCurrent(),
                     (unsigned long)regs->pc, (unsigned long)regs->lr, (unsigned long)regs->sp,
                     (unsigned long)excep->far);
    crash_log(buf, n);
    for (int i = 0; i < 13; i += 4) {
        n = snprintf(buf, sizeof(buf), "[CRASH] r%d-r%d %08lx %08lx %08lx %08lx\n", i, i + 3,
                     (unsigned long)regs->r[i], (unsigned long)(i + 1 < 13 ? regs->r[i + 1] : 0),
                     (unsigned long)(i + 2 < 13 ? regs->r[i + 2] : 0), (unsigned long)(i + 3 < 13 ? regs->r[i + 3] : 0));
        crash_log(buf, n);
    }
    const u32* sp = (const u32*)(regs->sp & ~3u);
    n = snprintf(buf, sizeof(buf), "[CRASH] stack");
    for (int i = 0, found = 0; i < 1024 && found < 48; i++) {
        u32 v = sp[i];
        if (v < 0x00100000 || v >= 0x01000000) continue;
        n += snprintf(buf + n, sizeof(buf) - n, " %08lx", (unsigned long)v);
        if (++found % 12 == 0) {
            buf[n++] = '\n';
            crash_log(buf, n);
            n = snprintf(buf, sizeof(buf), "[CRASH] stack");
        }
    }
    buf[n++] = '\n';
    crash_log(buf, n);
    svcBreak(USERBREAK_PANIC);
    for (;;) {}
}

/* Per thread: call at the start of every thread the game creates. */
void n3ds_install_crash_handler(void) {
    threadOnException(crash_handler, crash_stack + sizeof(crash_stack), WRITE_DATA_TO_HANDLER_STACK);
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
    /* Both top framebuffers still hold the Homebrew Launcher image until the first frame */
    for (int i = 0; i < 2; i++) {
        u16 w, h;
        u8* fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &w, &h);
        memset(fb, 0, (size_t)w * h * 3);
        gfxFlushBuffers();
        gfxSwapBuffers();
    }
    n3ds_init_consoles();
    {   /* debug switch "livelog" in debug3ds.txt forces the log server on (Azahar tests) */
        char sw[256] = { 0 };
        FILE* f = fopen("debug3ds.txt", "r");
        if (f) { fread(sw, 1, sizeof(sw) - 1, f); fclose(f); }
        if (strstr(sw, "livelog")) s_netloaded = 1;
    }
    n3ds_init_live_log(s_netloaded);
    n3ds_install_log_tee();
    if (s_live_status[0]) printf("%s\n", s_live_status);
    printf("Animal Crossing 3DS (%s)\n", is_new ? "N3DS" : "O3DS");
    printf("heap %08lx +%luKB\n", __ctru_heap, __ctru_heap_size >> 10);
    printf("commit max %lluKB, used at start %lluKB, app region %luKB\n", s_commit_max >> 10, s_commit_cur >> 10,
           (unsigned long)(osGetMemRegionSize(MEMREGION_APPLICATION) >> 10));
}

char g_n3ds_args[256]; /* command-line arguments joined by spaces */

/* pc_main.c's main() is renamed via -Dmain=pc_main_entry */
extern int pc_main_entry(int argc, char* argv[]);

static int g_argc;
static char** g_argv;
static int g_ret;

static void game_thread(void* arg) {
    (void)arg;
    n3ds_install_crash_handler();
    g_ret = pc_main_entry(g_argc, g_argv);
}

/* All runtime files (rom/, shaders/, save/, settings.ini) live here. */
#define N3DS_DATA_DIR "sdmc:/3ds/AnimalCrossing"

int main(int argc, char* argv[]) {
    /* hbmenu passes only the path; turn on the PC layer's diagnostic output */
    static char* default_argv[] = { "ac_3ds", "--verbose", NULL };
    s_netloaded = __3dslink_host.s_addr != 0 || (argc > 0 && argv[0] && strncmp(argv[0], "3dslink:", 8) == 0);
    g_argc = argc > 1 ? argc : 2;
    g_argv = argc > 1 ? argv : default_argv;
    /* 3dslink arguments also carry render debug switches (n3ds_tev.c), e.g. "-- --verbose shots" */
    for (int i = 1; i < argc; i++) {
        strncat(g_n3ds_args, argv[i], sizeof(g_n3ds_args) - strlen(g_n3ds_args) - 2);
        strcat(g_n3ds_args, " ");
    }

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

/* Free newlib heap in bytes: never-claimed space plus free blocks */
u32 n3ds_heap_free(void) {
    struct mallinfo mi = mallinfo();
    return (u32)(__ctru_heap_size - mi.arena + mi.fordblks);
}
