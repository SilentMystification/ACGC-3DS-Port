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

/* [FRAME] frame-time split, always on (two tick reads per frame): CPU waiting for the GPU at frame start,
 * and time inside C3D_FrameEnd (command submit). Ticks at 268123 per ms. */
#define N3DS_TICKS_PER_MS 268123.0 /* libctru system tick rate */
extern unsigned int pc_emu64_cmd_calls[256];
unsigned long long n3ds_gpu_wait_ticks, n3ds_frame_end_ticks;

/* [USE] distinct textures and position matrices per frame, counted between frame ends (n3ds_gl_swap bumps the id) */
unsigned int n3ds_frame_id;
static unsigned int s_use_frame, s_use_n[2], s_use_keys[2][64], s_use_overflow[2];
static unsigned long long s_use_distinct[2], s_use_frames, s_use_loads[2];
void n3ds_note_use(int kind, unsigned int key) {
    if (s_use_frame != n3ds_frame_id) {
        for (int k = 0; k < 2; k++) { s_use_distinct[k] += s_use_n[k]; s_use_n[k] = 0; }
        s_use_frames++;
        s_use_frame = n3ds_frame_id;
    }
    s_use_loads[kind]++;
    for (unsigned int i = 0; i < s_use_n[kind]; i++)
        if (s_use_keys[kind][i] == key) return;
    if (s_use_n[kind] < 64) s_use_keys[kind][s_use_n[kind]++] = key;
    else s_use_overflow[kind]++;
}

void n3ds_emu64_report(void) {
    static unsigned int calls[256];
    unsigned long long sum_calls = 0;
    memcpy(calls, pc_emu64_cmd_calls, sizeof(calls));
    memset(pc_emu64_cmd_calls, 0, sizeof(pc_emu64_cmd_calls));
    for (int i = 0; i < 256; i++) sum_calls += calls[i];
    printf("[EMU64] 600f opcodes=%lu\n", (unsigned long)sum_calls);

    /* [DRAW] forced draws in pc_gx_flush_vertices, reasons and dirty bits, same 600-frame window */
    extern unsigned int pc_gx_flush_reason[5];
    extern unsigned int pc_gx_dirty_bits[32];
    printf("[DRAW] forced=%u dirty=%u nondeferrable=%u prim=%u shader=%u\n", pc_gx_flush_reason[1] + pc_gx_flush_reason[2] + pc_gx_flush_reason[3] + pc_gx_flush_reason[4],
           pc_gx_flush_reason[1], pc_gx_flush_reason[2], pc_gx_flush_reason[3], pc_gx_flush_reason[4]);
    for (int b = 0; b < 32; b++)
        if (pc_gx_dirty_bits[b]) printf("[DRAW] dirty bit %d set on %u forced draws\n", b, pc_gx_dirty_bits[b]);
    extern unsigned int pc_gx_site[8];
    {   /* [USE] averages per frame over this window; switches come from the site counters read just below */
        double f = s_use_frames ? (double)s_use_frames : 1.0;
        printf("[USE] %llu frames: distinct tex %.1f/frame (overflow %u), distinct posmtx %.1f/frame (overflow %u)\n",
               s_use_frames, s_use_distinct[0] / f, s_use_overflow[0], s_use_distinct[1] / f, s_use_overflow[1]);
        printf("[USE] per frame: tex loads %.1f, tex slot switches %.1f, texmtx switches %.1f, posmtx loads %.1f, posmtx switches %.1f\n",
               s_use_loads[0] / f, pc_gx_site[0] / f, pc_gx_site[3] / f, s_use_loads[1] / f, pc_gx_site[6] / f);
        s_use_distinct[0] = s_use_distinct[1] = s_use_frames = s_use_loads[0] = s_use_loads[1] = 0;
        s_use_overflow[0] = s_use_overflow[1] = 0;
    }
    printf("[DRAW] sites: tex_slot=%u tex_params=%u texgen_mtx=%u texgen_num=%u texgen_coord=%u\n",
           pc_gx_site[0], pc_gx_site[1], pc_gx_site[3], pc_gx_site[4], pc_gx_site[5]);
    memset(pc_gx_site, 0, sizeof(pc_gx_site));
    memset(pc_gx_flush_reason, 0, sizeof(pc_gx_flush_reason));
    memset(pc_gx_dirty_bits, 0, sizeof(pc_gx_dirty_bits));
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
