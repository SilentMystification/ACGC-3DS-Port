/* n3ds_calls.c - call counters for slow ARM11 operations (debug3ds.txt switch "calls").
 *
 * The ARM11 has no integer divide instruction and no pipelined VFP11 divide/sqrt. Guessing
 * which call sites are hot wastes a hardware round trip, so this counts real calls instead:
 * each --wrap'd function (3ds/CMakeLists.txt) increments a counter and records up to 8
 * distinct callers (by return address) before falling back to an "other" bucket. divmod
 * variants are not wrapped: their ABI returns two values packed across registers in a way
 * a plain C wrapper cannot safely re-create, and idiv/uidiv alone already show whether
 * integer division is hot.
 *
 * [CALLS] prints every 600 frames (n3ds_gl.c perf_frame, same cadence as [PERF]), only when
 * "calls" is set; the wrappers always run (negligible: one counter bump), so a release
 * build pays nothing extra by leaving the switch off.
 */
#include <3ds.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

enum { C_UIDIV, C_IDIV, C_SINF, C_COSF, C_SQRTF, C_POWF, C_SIN, C_COS, C_SQRT, C_POW, C_COUNT };
static const char* const s_names[C_COUNT] = {
    "uidiv", "idiv", "sinf", "cosf", "sqrtf", "powf", "sin", "cos", "sqrt", "pow"
};

#define MAXC 8
typedef struct { u32 total, other; void* who[MAXC]; u32 hit[MAXC]; } CallStat;
static CallStat s_stat[C_COUNT];

static inline void record(int which, void* ra) {
    CallStat* c = &s_stat[which];
    c->total++;
    for (int i = 0; i < MAXC; i++) {
        if (c->who[i] == ra) { c->hit[i]++; return; }
        if (!c->who[i]) { c->who[i] = ra; c->hit[i] = 1; return; }
    }
    c->other++;
}

/* [CALLS] <name> total=<N>/600f  <addr>:<hits> ... (addresses: addr2line -e build3ds/ac_3ds.elf) */
void n3ds_calls_report(void) {
    for (int f = 0; f < C_COUNT; f++) {
        CallStat* c = &s_stat[f];
        if (!c->total) continue;
        char buf[200];
        int n = snprintf(buf, sizeof(buf), "[CALLS] %s total=%lu/600f", s_names[f], (unsigned long)c->total);
        for (int i = 0; i < MAXC && c->who[i] && n < (int)sizeof(buf) - 24; i++)
            n += snprintf(buf + n, sizeof(buf) - n, " %p:%lu", c->who[i], (unsigned long)c->hit[i]);
        if (c->other) snprintf(buf + n, sizeof(buf) - n, " other:%lu", (unsigned long)c->other);
        printf("%s\n", buf);
        memset(c, 0, sizeof(*c));
    }
}

/* [EMU64] top opcodes by handler time, last 600 frames (emu64.c dispatch, 3DS only, same switch).
 * Each opcode costs two tick reads, so the ms values are inflated. Rank by ms, do not trust the absolute time. */
#define N3DS_TICKS_PER_MS 268123.0 /* libctru system tick rate */
extern unsigned int pc_emu64_cmd_ticks[256];
extern unsigned int pc_emu64_cmd_calls[256];
extern int g_n3ds_dbg; /* n3ds_tev.c; 2048 = calls */
unsigned long long n3ds_emu64_tick(void) { return (g_n3ds_dbg & 2048) ? svcGetSystemTick() : 0; }

void n3ds_emu64_report(void) {
    static unsigned int ticks[256], calls[256];
    unsigned long long sum_ticks = 0, sum_calls = 0;
    memcpy(ticks, pc_emu64_cmd_ticks, sizeof(ticks));
    memcpy(calls, pc_emu64_cmd_calls, sizeof(calls));
    memset(pc_emu64_cmd_ticks, 0, sizeof(pc_emu64_cmd_ticks));
    memset(pc_emu64_cmd_calls, 0, sizeof(pc_emu64_cmd_calls));
    for (int i = 0; i < 256; i++) { sum_ticks += ticks[i]; sum_calls += calls[i]; }
    printf("[EMU64] 600f opcodes=%lu handler=%.1fms\n", (unsigned long)sum_calls, sum_ticks / N3DS_TICKS_PER_MS);
    for (int k = 0; k < 8; k++) {
        int best = -1;
        for (int i = 0; i < 256; i++)
            if (ticks[i] && (best < 0 || ticks[i] > ticks[best])) best = i;
        if (best < 0) break;
        printf("[EMU64] op %02X calls=%lu ms=%.1f\n", best, (unsigned long)calls[best], ticks[best] / N3DS_TICKS_PER_MS);
        ticks[best] = 0;
    }
}

extern int __real___aeabi_idiv(int, int);
extern unsigned __real___aeabi_uidiv(unsigned, unsigned);
extern float __real_sinf(float);
extern float __real_cosf(float);
extern float __real_sqrtf(float);
extern float __real_powf(float, float);
extern double __real_sin(double);
extern double __real_cos(double);
extern double __real_sqrt(double);
extern double __real_pow(double, double);

int __wrap___aeabi_idiv(int a, int b) { record(C_IDIV, __builtin_return_address(0)); return __real___aeabi_idiv(a, b); }
unsigned __wrap___aeabi_uidiv(unsigned a, unsigned b) { record(C_UIDIV, __builtin_return_address(0)); return __real___aeabi_uidiv(a, b); }
float __wrap_sinf(float x) { record(C_SINF, __builtin_return_address(0)); return __real_sinf(x); }
float __wrap_cosf(float x) { record(C_COSF, __builtin_return_address(0)); return __real_cosf(x); }
float __wrap_sqrtf(float x) { record(C_SQRTF, __builtin_return_address(0)); return __real_sqrtf(x); }
float __wrap_powf(float a, float b) { record(C_POWF, __builtin_return_address(0)); return __real_powf(a, b); }
double __wrap_sin(double x) { record(C_SIN, __builtin_return_address(0)); return __real_sin(x); }
double __wrap_cos(double x) { record(C_COS, __builtin_return_address(0)); return __real_cos(x); }
double __wrap_sqrt(double x) { record(C_SQRT, __builtin_return_address(0)); return __real_sqrt(x); }
double __wrap_pow(double a, double b) { record(C_POW, __builtin_return_address(0)); return __real_pow(a, b); }
