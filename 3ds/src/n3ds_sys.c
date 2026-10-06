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
static u64 s_boot_tick; /* process start, for [BOOT] times */

/* socInit turns its buffer into a shared memory block, and the kernel refuses linear
 * memory for that; the whole newlib heap is linear here. So this much of the quota is kept
 * out of the linear block and mapped as normal memory by the live log. The SOC service keeps
 * its socket buffers in it; we have 2 sockets at a low rate. Kept only for a netload start.
 * ponytail: 128 KB is a guess; if the hardware log shows send errors or gaps, try 256 KB. */
#define N3DS_SOC_SIZE 0x20000
static int s_soc_reserved;

/* Netload start: 3dslink adds a last argument "xxxxxxxx_3DSLINK_" (libctru initArgv.c), and
 * argv[0] starts with "3dslink:". Read from the raw argument list: argv is built after heaps. */
static int started_by_netload(void) {
    const char* p = envGetSystemArgList();
    if (!p) return 0;
    u32 argc = *(const u32*)p;
    const char* arg = p + 4;
    const char* last = NULL;
    for (u32 i = 0; i < argc; i++) {
        if (i == 0 && strncmp(arg, "3dslink:", 8) == 0) return 1;
        last = arg;
        arg += strlen(arg) + 1;
    }
    return last && argc > 1 && strlen(last) == 17 && strncmp(last + 8, "_3DSLINK_", 8) == 0;
}

void __system_allocateHeaps(void) {
    s_boot_tick = svcGetSystemTick();
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
    s_soc_reserved = started_by_netload();
    if (s_soc_reserved) total -= N3DS_SOC_SIZE; /* live-log network buffer (must not be linear) */
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

/* ---- Log output ----
 * printf on any thread -> tee_write: puts "#<seq> " at each line start and copies the bytes
 * into s_ring under s_log_lock. That is memcpy work only: no I/O, no wait inside the lock.
 * log_worker (core 1) then writes the new bytes, outside the lock, to:
 *   the SD log (sdmc:/3ds/AnimalCrossing/log.txt), the debugger (svcOutputDebugString),
 *   the bottom-screen console (without the "#seq " prefix) and the Wi-Fi client.
 * Positions are byte counts since start (u32, compared by subtraction); ring index = pos & mask.
 * The producer never overwrites bytes that an output has not written yet. A line that does not
 * fit is dropped (its sequence number is still used, so the gap shows) and the worker logs
 * "[LOG] dropped N lines".
 * Threading rules: s_log_lock is the only lock here; nothing blocks while it is held; no printf
 * inside it or inside the worker (the lock is not recursive). Fault paths (crash handler,
 * watchdog) use n3ds_log_raw, which never waits for the lock. */
static const devoptab_t* console_dev;
static int log_fd = -1; /* sdmc:/3ds/AnimalCrossing/log.txt, unbuffered */

/* Bottom screen: status rows 0-3 (n3ds_status) and a separator, then the live log in rows
 * 5-29. Only the log worker draws on it after start. */
#define STATUS_ROWS 4
#define STATUS_COLS 38 /* 40 columns from column 1; a full row would wrap */
static PrintConsole s_con_log, s_con_status;

/* Live log over Wi-Fi: when started from the netloader, the worker runs a log server on
 * TCP port 17492. 3ds/tools/livelog.py connects to it (PC -> 3DS, the same direction as the
 * 3dslink upload and FTP, so a PC firewall or VPN does not block it). A new client first gets
 * the last LOG_REPLAY bytes of the log, then each new line. */
#define LIVE_LOG_PORT 17492
static int s_netloaded; /* set in main: started by the Homebrew Launcher netloader */
#define LOG_RING (64 * 1024)
#define LOG_MASK (LOG_RING - 1)
#define LOG_REPLAY (LOG_RING / 2) /* half: the producer keeps room while a replay is sent */
static char s_ring[LOG_RING];
static LightLock s_log_lock;
static LightEvent s_log_event;
/* under s_log_lock */
static u32 s_head;           /* bytes added */
static u32 s_tail;           /* oldest byte that an output still needs */
static u32 s_seq;            /* line sequence number */
static int s_line_start = 1; /* the next byte starts a line */
static u32 s_dropped;        /* lines dropped, not reported yet */
static char s_status_txt[STATUS_ROWS][STATUS_COLS];
static u32 s_status_dirty;   /* bit per status row */
/* lock watch (read by the watchdog without the lock) */
static volatile u64 s_lock_tick; /* 0 = free */
static volatile u32 s_lock_owner;
/* worker */
static volatile u64 s_worker_beat; /* tick of the last worker loop; 0 = not started */
static int s_listen = -1;
static volatile int s_client = -1;
static char s_live_status[96];

static void log_lock(void) {
    LightLock_Lock(&s_log_lock);
    s_lock_owner = (u32)(uintptr_t)threadGetCurrent();
    s_lock_tick = svcGetSystemTick();
}
static int log_trylock(void) {
    if (LightLock_TryLock(&s_log_lock)) return 0; /* nonzero = busy */
    s_lock_owner = (u32)(uintptr_t)threadGetCurrent();
    s_lock_tick = svcGetSystemTick();
    return 1;
}
static void log_unlock(void) {
    s_lock_tick = 0;
    LightLock_Unlock(&s_log_lock);
}

/* ms the log lock has been held now (0 = free) and its owner; for the watchdog */
u32 n3ds_log_lock_held_ms(u32* owner) {
    u64 t = s_lock_tick;
    if (owner) *owner = s_lock_owner;
    return t ? (u32)((svcGetSystemTick() - t) / CPU_TICKS_PER_MSEC) : 0;
}

/* debug switch "locktest": hold the log lock for 1.5 s once; the watchdog must report it */
void n3ds_log_locktest(void) {
    log_lock();
    svcSleepThread(1500000000LL); /* deliberate rule break: this is the test */
    log_unlock();
}

static void ring_copy(const char* p, u32 n) {
    u32 i = s_head & LOG_MASK, k = LOG_RING - i;
    if (k > n) k = n;
    memcpy(s_ring + i, p, k);
    memcpy(s_ring, p + k, n - k);
    s_head += n;
}

static u32 fmt_seq(char* out, u32 v) { /* "#<v> " */
    char tmp[12];
    u32 n = 0, m = 0;
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    out[m++] = '#';
    while (n) out[m++] = tmp[--n];
    out[m++] = ' ';
    return m;
}

/* lock held */
static void ring_add(const char* p, size_t n) {
    while (n) {
        const char* nl = (const char*)memchr(p, '\n', n);
        u32 len = nl ? (u32)(nl - p) + 1 : (u32)n;
        char pre[16];
        u32 plen = s_line_start ? fmt_seq(pre, s_seq) : 0;
        if (LOG_RING - (s_head - s_tail) < plen + len) { /* full: drop the rest of this write */
            u32 lines = 0;
            for (size_t i = 0; i < n; i++) lines += p[i] == '\n';
            if (!lines) lines = 1;
            s_dropped += lines;
            s_seq += lines;
            s_line_start = p[n - 1] == '\n';
            return;
        }
        if (plen) { ring_copy(pre, plen); s_seq++; }
        ring_copy(p, len);
        s_line_start = nl != NULL;
        p += len;
        n -= len;
    }
}

static ssize_t tee_write(struct _reent* r, void* fd, const char* ptr, size_t len) {
    (void)r; (void)fd;
    log_lock();
    ring_add(ptr, len);
    log_unlock();
    LightEvent_Signal(&s_log_event);
    return (ssize_t)len;
}

/* --- worker outputs (worker thread only) --- */

static u32 s_pos_sd, s_pos_con, s_pos_net;
static int s_con_state; /* 0 line start, 1 inside "#seq " prefix, 2 text */
static u64 s_net_stall;  /* tick when sends started to fail with EAGAIN; 0 = none */

static void out_sd(const char* p, u32 n) {
    svcOutputDebugString(p, (s32)n);
    if (log_fd >= 0) write(log_fd, p, n);
}

static void out_con(const char* p, u32 n) {
    char buf[256];
    u32 m = 0;
    for (u32 i = 0; i < n; i++) {
        char c = p[i];
        if (s_con_state == 0) s_con_state = c == '#' ? 1 : 2;
        if (s_con_state == 1) { if (c == ' ') s_con_state = 2; continue; }
        buf[m++] = c;
        if (c == '\n') s_con_state = 0;
        if (m == sizeof(buf)) { console_dev->write_r(_REENT, 0, buf, m); m = 0; }
    }
    if (m) console_dev->write_r(_REENT, 0, buf, m);
}

static void drain(u32* pos, u32 head, void (*out)(const char*, u32)) {
    while (*pos != head) {
        u32 i = *pos & LOG_MASK, n = head - *pos;
        if (n > LOG_RING - i) n = LOG_RING - i;
        out(s_ring + i, n);
        *pos += n;
    }
}

/* worker only: appends one worker message (no printf in the worker) */
static void worker_note(const char* s) {
    log_lock();
    ring_add(s, strlen(s));
    log_unlock();
}

static void drop_client(const char* why) {
    char msg[96];
    close(s_client);
    s_client = -1;
    snprintf(msg, sizeof(msg), "[LOG] Wi-Fi client dropped: %s\n", why);
    worker_note(msg);
}

static void net_drain(u32 head) {
    while (s_client >= 0 && s_pos_net != head) {
        u32 i = s_pos_net & LOG_MASK, n = head - s_pos_net;
        if (n > LOG_RING - i) n = LOG_RING - i;
        int k = send(s_client, s_ring + i, n, 0);
        if (k > 0) { s_pos_net += (u32)k; s_net_stall = 0; continue; }
        if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            u64 now = svcGetSystemTick();
            if (!s_net_stall) s_net_stall = now;
            else if (now - s_net_stall > 2 * (u64)SYSCLOCK_ARM11) drop_client("no progress for 2 s");
            return;
        }
        drop_client("send error");
    }
}

static void log_worker(void* arg) {
    (void)arg;
    char msg[96];
    snprintf(msg, sizeof(msg), "[CORE] log worker on core %ld\n", (long)svcGetProcessorID());
    worker_note(msg);
    for (;;) {
        s_worker_beat = svcGetSystemTick();
        LightEvent_WaitTimeout(&s_log_event, 50000000LL); /* 50 ms: also polls accept */

        if (s_listen >= 0 && s_client < 0) {
            int c = accept(s_listen, NULL, NULL);
            if (c >= 0) {
                fcntl(c, F_SETFL, fcntl(c, F_GETFL, 0) | O_NONBLOCK);
                log_lock();
                u32 q = s_head - (s_head < LOG_REPLAY ? s_head : LOG_REPLAY);
                if (q) /* start the replay after a line end */
                    while (q != s_head && s_ring[(q - 1) & LOG_MASK] != '\n') q++;
                if (s_head - q > s_head - s_tail) s_tail = q;
                s_pos_net = q;
                s_client = c;
                s_net_stall = 0;
                static const char hello[] = "[LOG] Wi-Fi client connected\n";
                ring_add(hello, sizeof(hello) - 1);
                log_unlock();
            }
        }

        char rows[STATUS_ROWS][STATUS_COLS];
        u32 dirty, dropped, head;
        log_lock();
        dirty = s_status_dirty;
        s_status_dirty = 0;
        for (int i = 0; i < STATUS_ROWS; i++)
            if (dirty & (1u << i)) memcpy(rows[i], s_status_txt[i], STATUS_COLS);
        dropped = s_dropped;
        s_dropped = 0;
        if (dropped) {
            snprintf(msg, sizeof(msg), "[LOG] dropped %lu lines (log buffer full)\n", (unsigned long)dropped);
            ring_add(msg, strlen(msg));
        }
        head = s_head;
        log_unlock();

        int drew = dirty || s_pos_con != head;
        drain(&s_pos_sd, head, out_sd);
        consoleSelect(&s_con_log);
        drain(&s_pos_con, head, out_con);
        if (dirty) {
            consoleSelect(&s_con_status);
            for (int i = 0; i < STATUS_ROWS; i++) {
                if (!(dirty & (1u << i))) continue;
                s_con_status.cursorX = 1; /* libctru cursor columns start at 1 here: 0 cut the first character */
                s_con_status.cursorY = i;
                console_dev->write_r(_REENT, 0, rows[i], STATUS_COLS);
            }
            consoleSelect(&s_con_log);
        }
        if (drew) { /* this core's cache holds the console pixels; the display reads memory */
            u16 w, h;
            u8* fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &w, &h);
            GSPGPU_FlushDataCache(fb, (u32)w * h * 2);
        }
        if (s_client >= 0) net_drain(head);
        else s_pos_net = head;

        log_lock(); /* publish: the oldest byte any output still needs */
        u32 back = s_head - s_pos_sd;
        if (s_head - s_pos_con > back) back = s_head - s_pos_con;
        if (s_client >= 0 && s_head - s_pos_net > back) back = s_head - s_pos_net;
        s_tail = s_head - back;
        log_unlock();
    }
}

/* Log line for fault paths (watchdog, GPU hang). It never waits for the lock. While the
 * worker runs and the lock is free, the line goes through the ring like printf. Else it is
 * written directly to the SD log, the debugger and (without waiting) the Wi-Fi client. */
void n3ds_log_raw(const char* s) {
    size_t len = strlen(s);
    u64 beat = s_worker_beat;
    int worker_ok = beat && svcGetSystemTick() - beat < 2 * (u64)SYSCLOCK_ARM11;
    if (worker_ok && log_trylock()) {
        ring_add(s, len);
        log_unlock();
        LightEvent_Signal(&s_log_event);
        return;
    }
    svcOutputDebugString(s, (s32)len);
    if (log_fd >= 0) { write(log_fd, s, len); fsync(log_fd); }
    int c = s_client;
    if (c >= 0) send(c, s, len, MSG_DONTWAIT);
}

static void n3ds_init_live_log(int netloaded) {
    static const u32 soc_size = N3DS_SOC_SIZE;
    if (!netloaded) return;
    u32 soc_addr = 0;
    Result rc = svcControlMemory(&soc_addr, OS_HEAP_AREA_BEGIN, 0, soc_size, MEMOP_ALLOC, MEMPERM_READ | MEMPERM_WRITE);
    if (R_FAILED(rc)) {
        snprintf(s_live_status, sizeof(s_live_status), "live log: off, no network memory%s (0x%08lX)",
                 s_soc_reserved ? "" : " (kept only for a netload start)", (unsigned long)rc);
        return;
    }
    u32* soc_buf = (u32*)soc_addr;
    if (soc_buf) rc = socInit(soc_buf, soc_size);
    if (R_FAILED(rc)) {
        snprintf(s_live_status, sizeof(s_live_status), "live log: off, socInit(%luKB) failed (0x%08lX)",
                 (unsigned long)(soc_size >> 10), (unsigned long)rc);
        if (soc_buf) svcControlMemory(&soc_addr, soc_addr, 0, soc_size, MEMOP_FREE, 0);
        return;
    }
    int ls = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(LIVE_LOG_PORT);
    a.sin_addr.s_addr = INADDR_ANY;
    if (ls < 0 || bind(ls, (struct sockaddr*)&a, sizeof(a)) < 0 || listen(ls, 1) < 0) {
        snprintf(s_live_status, sizeof(s_live_status), "live log: off, socket setup failed (errno %d)", errno);
        return;
    }
    fcntl(ls, F_SETFL, fcntl(ls, F_GETFL, 0) | O_NONBLOCK);
    s_listen = ls;
    struct in_addr ip = { (u32)gethostid() };
    snprintf(s_live_status, sizeof(s_live_status), "live log: listening on %s:%d (net buffer %luKB)",
             inet_ntoa(ip), LIVE_LOG_PORT, (unsigned long)(soc_size >> 10));
}

/* APT events (HOME, sleep, exit) in the log, so a hang in a transition shows how far it got.
 * libctru runs these hooks on the thread that calls aptMainLoop (the game thread, between
 * frames), with no APT lock held, so the core-1 limit IPC is safe here. */
static aptHookCookie s_apt_cookie;
static u32 s_cpu_limit; /* core-1 limit accepted at init (percent), 0 = none */

static void apt_log_hook(APT_HookType type, void* param) {
    static const char* const names[] = { "suspend (HOME or applet)", "restore", "sleep", "wakeup", "exit" };
    char msg[96];
    (void)param;
    int n = snprintf(msg, sizeof(msg), "[APT] %s", type < 5 ? names[type] : "?");
    /* HOME menu needs core 1 time: give it back while suspended, take it again on restore */
    if (s_cpu_limit > 30 && (type == APTHOOK_ONSUSPEND || type == APTHOOK_ONRESTORE)) {
        u32 want = type == APTHOOK_ONSUSPEND ? 30 : s_cpu_limit;
        Result rc = APT_SetAppCpuTimeLimit(want);
        n += snprintf(msg + n, sizeof(msg) - n, ", core-1 limit %lu%%: %s (0x%08lX)", (unsigned long)want,
                      R_SUCCEEDED(rc) ? "ok" : "failed", (unsigned long)rc);
    }
    snprintf(msg + n, sizeof(msg) - n, "\n");
    n3ds_log_raw(msg);
}

/* "[BOOT] t=<ms since process start> <what>" (PC_BOOT_MARK in pc_platform.h) */
void n3ds_boot_mark(const char* fmt, ...) {
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf("[BOOT] t=%lums %s\n", (unsigned long)((svcGetSystemTick() - s_boot_tick) / CPU_TICKS_PER_MSEC), buf);
}

/* Core that runs the caller. Azahar returns no valid core number here, so values > 3 are
 * shown as unknown; then the requested (ideal) core is the best hint. */
void n3ds_log_core(const char* who) {
    s32 core = svcGetProcessorID(), ideal = -9;
    svcGetThreadIdealProcessor(&ideal, CUR_THREAD_HANDLE);
    if (core >= 0 && core <= 3) printf("[CORE] %s on core %ld (requested %ld)\n", who, (long)core, (long)ideal);
    else printf("[CORE] %s: core unknown (emulator?), requested %ld\n", who, (long)ideal);
}

/* Overwrites status row 0..STATUS_ROWS-1 on the bottom screen (not logged) */
void n3ds_status(int row, const char* fmt, ...) {
    char buf[64];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0 || row < 0 || row >= STATUS_ROWS) return;
    if (n > STATUS_COLS) n = STATUS_COLS;
    memset(buf + n, ' ', STATUS_COLS - n);
    log_lock();
    memcpy(s_status_txt[row], buf, STATUS_COLS);
    s_status_dirty |= 1u << row;
    log_unlock();
    LightEvent_Signal(&s_log_event);
}

static void n3ds_init_consoles(void) {
    LightLock_Init(&s_log_lock);
    LightEvent_Init(&s_log_event, RESET_ONESHOT);
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

/* Log worker on core 1 (time-limited syscore on O3DS). If that fails, core 0 below the game. */
static void n3ds_start_log_worker(void) {
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    if (threadCreate(log_worker, NULL, 16 * 1024, prio + 1, 1, true)) return;
    if (threadCreate(log_worker, NULL, 16 * 1024, prio + 1, -2, true)) {
        printf("[CORE] log worker: core 1 refused, runs on the game core\n");
        return;
    }
    /* no worker: print straight to the console and the SD log, as before */
    devoptab_list[STD_OUT] = console_dev;
    devoptab_list[STD_ERR] = console_dev;
    printf("[CORE] log worker: thread create failed, log on screen only\n");
}

/* Crash handler: the part of the ring that the worker has not written yet goes to the SD log
 * first, so the last lines before a crash are kept. No lock: another thread may hold it. */
static void crash_flush_ring(void) {
    u32 head = s_head;
    if (log_fd < 0 || head - s_pos_sd > LOG_RING) return;
    while (s_pos_sd != head) {
        u32 i = s_pos_sd & LOG_MASK, n = head - s_pos_sd;
        if (n > LOG_RING - i) n = LOG_RING - i;
        write(log_fd, s_ring + i, n);
        s_pos_sd += n;
    }
}

/* CPU exception -> "[CRASH]" lines in log.txt, then halt. 3ds/run_azahar.ps1 runs
 * addr2line on them. Stack words below 16MB are possible return addresses
 * (code and static data live there); addr2line drops the ones that are not code. */
/* ponytail: one handler stack for all threads; two faults at once would share it */
static u8 crash_stack[0x4000] __attribute__((aligned(8)));

static void crash_log(const char* s, int n) {
    svcOutputDebugString(s, n);
    if (log_fd >= 0) { write(log_fd, s, (size_t)n); fsync(log_fd); } /* emulators buffer SD writes */
    int c = s_client;
    if (c >= 0) send(c, s, (size_t)n, MSG_DONTWAIT);
}

static void crash_handler(ERRF_ExceptionInfo* excep, CpuRegisters* regs) {
    static const char* const names[] = { "prefetch abort", "data abort", "undefined instruction", "VFP" };
    char buf[160];
    crash_flush_ring();
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
    /* TODO(N3DS): osSetSpeedupEnable(true) gives 804 MHz and the 2 MB L2 cache, and N3DS has
     * core 2 for the app. Off for now: the target is 60 fps on O3DS at 268 MHz (3ds/PLAN.md). */
    /* Core 1 (system core) for the log worker and the audio mixer: the highest accepted limit.
     * The APT hook gives 30% back while the HOME menu runs. */
    static const u32 limits_o3ds[] = { 79, 69, 30 }, limits_n3ds[] = { 80, 79, 69, 30 };
    const u32* lim = is_new ? limits_n3ds : limits_o3ds;
    int nlim = is_new ? 4 : 3;
    Result lim_rc = 0;
    for (int i = 0; i < nlim && !s_cpu_limit; i++)
        if (R_SUCCEEDED(lim_rc = APT_SetAppCpuTimeLimit(lim[i]))) s_cpu_limit = lim[i];

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
    n3ds_start_log_worker();
    aptHook(&s_apt_cookie, apt_log_hook, NULL);
    if (s_live_status[0]) printf("%s\n", s_live_status);
    printf("Animal Crossing 3DS (%s)\n", is_new ? "N3DS" : "O3DS");
    if (s_cpu_limit) printf("[APT] core-1 limit %lu%%\n", (unsigned long)s_cpu_limit);
    else printf("[APT] core-1 limit: all values refused (0x%08lX), worker threads share core 0\n", (unsigned long)lim_rc);
    {
        u32 fpscr;
        __asm__ volatile("vmrs %0, fpscr" : "=r"(fpscr));
        printf("[FPSCR] %08lx: flush-to-zero %s, default NaN %s\n", (unsigned long)fpscr,
               (fpscr >> 24) & 1 ? "on" : "OFF (denormals trap to slow support code)", (fpscr >> 25) & 1 ? "on" : "off");
    }
    n3ds_log_core("game thread");
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
