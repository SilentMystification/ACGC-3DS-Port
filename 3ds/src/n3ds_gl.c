/* n3ds_gl.c - the OpenGL subset that pc/src/pc_gx*.c calls, on citro3d.
 *
 * Fixed-function state (depth, blend, cull, viewport, scissor, clear), textures,
 * vertex upload and draws map to citro3d. Shader and uniform calls stay no-ops:
 * n3ds_tev.c sets the shader state. Draws happen only while the GX program
 * (N3DS_GX_PROG) is bound, so other GL users (the NES emulator) draw nothing.
 *
 * The top screen target is 240x400 (the LCD is mounted rotated). n3ds_tev.c folds
 * the rotation into the projection; viewport and scissor are rotated here.
 */
#include <3ds.h>
#include <citro3d.h>
#include <sys/stat.h>
#include "pc_gx_internal.h"
#include "test_min_shbin.h"
#include "test_flat_shbin.h"
#include "gx_nolit_shbin.h"
#include "test_out5_shbin.h"
#include "test_in4_shbin.h"
#include "test_out3_shbin.h"
#include "test_out4_shbin.h"

#define N3DS_GX_PROG 0x7FFF0001u
#define N3DS_CMDBUF_SIZE (2 * 1024 * 1024) /* 1 MB overflowed (GPUCMD_AddInternal) with the menu thread running */
#define N3DS_VTX_ARENA (1536 * 1024)
#define SCREEN_W 400
#define SCREEN_H 240

#define DISPLAY_TRANSFER_FLAGS                                                              \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |       \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

typedef struct N3DSTex {
    C3D_Tex tex;
    int valid;
    GPU_TEXTURE_WRAP_PARAM wrap_s, wrap_t;
    GPU_TEXTURE_FILTER_PARAM filter;
    int rot; /* screen capture: the color buffer tiles as is (memory row = screen x), see n3ds_gl_capture_screen */
    struct N3DSTex* next_free;
} N3DSTex;

/* GPU vertex, 40 bytes. PCGXVertex has the same layout on 3DS (pc_gx_internal.h). */
typedef struct {
    float pos[3];
    float nrm[3];
    u8 clr[4];
    float tc[2];
    float pal; /* batch palette offset (3 * slot) */
} N3DSVtx;

static C3D_RenderTarget* s_target;
static int s_in_frame;
static N3DSVtx* s_arena;
static u32 s_arena_used;
static N3DSVtx* s_draw_vtx; /* last uploaded GX batch, NULL if none */
static const N3DSVtx* s_buf_base; /* vertex buffer base the GPU has now (draw_ready) */
static void vertex_tail_set(u32 at, const PCGXVertex* src, int keep);
static u16* s_quad_idx;
static GLuint s_program;
static N3DSTex* s_bound_tex;
static N3DSTex* s_free_list; /* deleted during this frame; freed once the GPU is done */
static u32 s_stat_draws, s_stat_verts;
static u32 s_stat_cmdwords; /* [WORK]: GPU command words citro3d writes per draw (state it sends + the draw) */

static u32 cmd_offset(void) {
    u32* buf;
    u32 size, offset;
    GPUCMD_GetBuffer(&buf, &size, &offset);
    return offset;
}

static struct {
    int depth_test, depth_mask, blend, cull, scissor;
    GLenum depth_func, blend_src, blend_dst, blend_eq, cull_face;
    u8 color_mask;
    int vp[4], sc[4];
    float clear[4], clear_depth;
    float depth_near, depth_far;
} gs = {
    .depth_mask = 1, .depth_func = GL_LESS, .blend_src = GL_ONE, .blend_dst = GL_ZERO,
    .blend_eq = GL_FUNC_ADD, .cull_face = GL_BACK, .color_mask = GPU_WRITE_COLOR,
    .vp = { 0, 0, SCREEN_W, SCREEN_H }, .sc = { 0, 0, SCREEN_W, SCREEN_H },
    .clear_depth = 1.0f, .depth_far = 1.0f,
};

/* --- state --- */

static GPU_TESTFUNC map_func(GLenum f) {
    switch (f) {
        case GL_NEVER: return GPU_NEVER;
        case GL_LESS: return GPU_LESS;
        case GL_EQUAL: return GPU_EQUAL;
        case GL_LEQUAL: return GPU_LEQUAL;
        case GL_GREATER: return GPU_GREATER;
        case GL_NOTEQUAL: return GPU_NOTEQUAL;
        case GL_GEQUAL: return GPU_GEQUAL;
        default: return GPU_ALWAYS;
    }
}

static GPU_BLENDFACTOR map_factor(GLenum f) {
    switch (f) {
        case GL_ZERO: return GPU_ZERO;
        case GL_ONE: return GPU_ONE;
        case GL_SRC_COLOR: return GPU_SRC_COLOR;
        case GL_ONE_MINUS_SRC_COLOR: return GPU_ONE_MINUS_SRC_COLOR;
        case GL_DST_COLOR: return GPU_DST_COLOR;
        case GL_ONE_MINUS_DST_COLOR: return GPU_ONE_MINUS_DST_COLOR;
        case GL_SRC_ALPHA: return GPU_SRC_ALPHA;
        case GL_ONE_MINUS_SRC_ALPHA: return GPU_ONE_MINUS_SRC_ALPHA;
        case GL_DST_ALPHA: return GPU_DST_ALPHA;
        case GL_ONE_MINUS_DST_ALPHA: return GPU_ONE_MINUS_DST_ALPHA;
        default: return GPU_ONE;
    }
}

static void apply_depth(void) {
    /* GL: no depth writes while the depth test is off */
    u32 mask = gs.color_mask | (gs.depth_test && gs.depth_mask ? GPU_WRITE_DEPTH : 0);
    C3D_DepthTest(gs.depth_test, map_func(gs.depth_func), (GPU_WRITEMASK)mask);
}

static void apply_blend(void) {
    if (!gs.blend) {
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
        return;
    }
    GPU_BLENDEQUATION eq = gs.blend_eq == GL_FUNC_REVERSE_SUBTRACT ? GPU_BLEND_REVERSE_SUBTRACT
                         : gs.blend_eq == GL_FUNC_SUBTRACT ? GPU_BLEND_SUBTRACT : GPU_BLEND_ADD;
    GPU_BLENDFACTOR src = map_factor(gs.blend_src), dst = map_factor(gs.blend_dst);
    C3D_AlphaBlend(eq, eq, src, dst, src, dst);
}

static void apply_cull(void) {
    /* The projection rotation keeps GL winding, so GL front/back map directly */
    C3D_CullFace(!gs.cull ? GPU_CULL_NONE : gs.cull_face == GL_FRONT ? GPU_CULL_FRONT_CCW
               : gs.cull_face == GL_BACK ? GPU_CULL_BACK_CCW : GPU_CULL_NONE);
}

/* Real hardware can hang on states that Azahar accepts: empty viewports and scissor
 * rectangles, and draws with no vertices. Such draws are skipped (they draw nothing anyway). */
static int s_vp_empty, s_sc_empty;

/* GL window rect (y up, 400x240) -> rotated target (x = screen y from bottom, y = screen x from right) */
static void apply_viewport(void) {
    s_vp_empty = gs.vp[2] <= 0 || gs.vp[3] <= 0;
    if (s_vp_empty) return;
    C3D_SetViewport((u32)gs.vp[1], (u32)(SCREEN_W - gs.vp[0] - gs.vp[2]), (u32)gs.vp[3], (u32)gs.vp[2]);
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void apply_scissor(void) {
    extern int g_n3ds_dbg; /* 128 = noscissor */
    s_sc_empty = 0;
    if (!gs.scissor || (g_n3ds_dbg & 128)) {
        C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
        return;
    }
    int x0 = clampi(gs.sc[1], 0, SCREEN_H), x1 = clampi(gs.sc[1] + gs.sc[3], 0, SCREEN_H);
    int y0 = clampi(SCREEN_W - gs.sc[0] - gs.sc[2], 0, SCREEN_W), y1 = clampi(SCREEN_W - gs.sc[0], 0, SCREEN_W);
    if (x1 <= x0 || y1 <= y0) { s_sc_empty = 1; return; }
    C3D_SetScissor(GPU_SCISSOR_NORMAL, (u32)x0, (u32)y0, (u32)x1, (u32)y1);
}

static void apply_depth_range(void) {
    /* PICA clip z: near -1, far 0. depth = z * scale + offset maps them to [near, far] */
    C3D_DepthMap(true, gs.depth_far - gs.depth_near, gs.depth_far);
}

static void gl_set_cap(GLenum cap, int on) {
    switch (cap) {
        case GL_DEPTH_TEST: gs.depth_test = on; apply_depth(); break;
        case GL_BLEND: gs.blend = on; apply_blend(); break;
        case GL_CULL_FACE: gs.cull = on; apply_cull(); break;
        case GL_SCISSOR_TEST: gs.scissor = on; apply_scissor(); break;
        default: break;
    }
}
static void gl_enable(GLenum cap) { gl_set_cap(cap, 1); }
static void gl_disable(GLenum cap) { gl_set_cap(cap, 0); }
static void gl_depth_func(GLenum f) { gs.depth_func = f; apply_depth(); }
static void gl_depth_mask(GLboolean m) { gs.depth_mask = m; apply_depth(); }
static void gl_color_mask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) {
    gs.color_mask = (r ? GPU_WRITE_RED : 0) | (g ? GPU_WRITE_GREEN : 0) | (b ? GPU_WRITE_BLUE : 0) |
                    (a ? GPU_WRITE_ALPHA : 0);
    apply_depth();
}
static void gl_blend_func(GLenum s, GLenum d) { gs.blend_src = s; gs.blend_dst = d; apply_blend(); }
static void gl_blend_equation(GLenum e) { gs.blend_eq = e; apply_blend(); }
static void gl_cull_face(GLenum m) { gs.cull_face = m; apply_cull(); }
static void gl_viewport(GLint x, GLint y, GLsizei w, GLsizei h) {
    gs.vp[0] = x; gs.vp[1] = y; gs.vp[2] = w; gs.vp[3] = h;
    apply_viewport();
}
static void gl_scissor(GLint x, GLint y, GLsizei w, GLsizei h) {
    gs.sc[0] = x; gs.sc[1] = y; gs.sc[2] = w; gs.sc[3] = h;
    apply_scissor();
}
static void gl_depth_range(GLdouble n, GLdouble f) {
    gs.depth_near = (float)n;
    gs.depth_far = (float)f;
    apply_depth_range();
}
static void gl_clear_color(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
    gs.clear[0] = r; gs.clear[1] = g; gs.clear[2] = b; gs.clear[3] = a;
}
static void gl_clear_depth(GLdouble d) { gs.clear_depth = (float)d; }
static void gl_use_program(GLuint p) { s_program = p; }

/* --- frame --- */

static void free_deleted_textures(void) {
    if (s_free_list) {
        extern void n3ds_tex_units_detach(void);
        n3ds_tex_units_detach(); /* no unit may keep a pointer into memory freed below */
    }
    while (s_free_list) {
        N3DSTex* t = s_free_list;
        s_free_list = t->next_free;
        if (t->valid) C3D_TexDelete(&t->tex);
        free(t);
    }
}

/* --- hang diagnostics ---
 * Step marker: the last render step reached (the CPU watchdog thread reports it).
 * Draw records: the state of each draw in a frame; on a GPU timeout the records of the
 * frame the GPU could not finish are logged, so the report names the bad draw. */
static volatile const char* s_step = "boot";
static char s_step_buf[64];
static volatile u32 s_frames_done;
static int s_apt_ok = 1; /* last aptMainLoop result; 0 = the app must close */
int n3ds_apt_ok(void) { return s_apt_ok; }

#define DRAW_RECS 48
typedef struct {
    u8 prim, scissor, blend, depth;
    u16 count, first;
    s16 vp[4], sc[4];
    struct { u8 pica, fog, lit, alpha; u8 fmt[3]; u16 w[3], h[3]; } tev; /* = N3DSTevState */
} DrawRec;
extern struct N3DSTevState { u8 pica, fog, lit, alpha; u8 fmt[3]; u16 w[3], h[3]; } g_n3ds_tev_state;
static DrawRec s_recs[2][DRAW_RECS]; /* [0] frame being built, [1] frame submitted last */
static u32 s_nrecs[2];

static void record_draw(int prim, int first, int count) {
    u32 i = s_nrecs[0]++;
    if (i >= DRAW_RECS) i = DRAW_RECS - 1; /* keep the newest in the last slot */
    DrawRec* r = &s_recs[0][i];
    r->prim = (u8)prim; r->first = (u16)first; r->count = (u16)count;
    r->scissor = (u8)gs.scissor; r->blend = (u8)gs.blend;
    r->depth = (u8)(gs.depth_test | (gs.depth_mask << 1));
    for (int k = 0; k < 4; k++) { r->vp[k] = (s16)gs.vp[k]; r->sc[k] = (s16)gs.sc[k]; }
    memcpy(&r->tev, &g_n3ds_tev_state, sizeof(r->tev));
}

static void report_gpu_hang(u32 frame) {
    printf("[HANG] GPU did not finish frame %lu within 2 s. Its %lu draws (last %d kept; "
           "prim 0 tris 1 strip 2 fan 4 quads; tex f = GPU_TEXCOLOR, 255 none; depth bit0 test bit1 write):\n",
           (unsigned long)frame, (unsigned long)s_nrecs[1], DRAW_RECS);
    u32 n = s_nrecs[1] < DRAW_RECS ? s_nrecs[1] : DRAW_RECS;
    for (u32 i = 0; i < n; i++) {
        const DrawRec* r = &s_recs[1][i];
        printf("[HANG] #%lu prim %d first %u n %u vp %d,%d %dx%d sc%s %d,%d %dx%d blend %d depth %d | "
               "pica %u fog %u lit %u atest %u | tex0 f%u %ux%u tex1 f%u %ux%u tex2 f%u %ux%u\n",
               (unsigned long)i, r->prim, r->first, r->count, r->vp[0], r->vp[1], r->vp[2], r->vp[3],
               r->scissor ? "on" : "off", r->sc[0], r->sc[1], r->sc[2], r->sc[3], r->blend, r->depth,
               r->tev.pica, r->tev.fog, r->tev.lit, r->tev.alpha, r->tev.fmt[0], r->tev.w[0], r->tev.h[0],
               r->tev.fmt[1], r->tev.w[1], r->tev.h[1], r->tev.fmt[2], r->tev.w[2], r->tev.h[2]);
    }
}

/* CPU watchdog: own thread; if no frame finishes for 3 s, log the frame and last step */
static void watchdog_thread(void* arg) {
    extern void n3ds_log_raw(const char* s);
    (void)arg;
    extern u32 n3ds_log_lock_held_ms(u32* owner);
    u32 last = 0, still = 0, reported = 0, lock_reported = 0;
    u64 t_app = osGetTime(); /* osGetTime counts from console boot; uptime counts from here */
    for (;;) {
        svcSleepThread(1000000000LL);
        extern void n3ds_status(int row, const char* fmt, ...);
        n3ds_status(4, "uptime %lu s", (unsigned long)((osGetTime() - t_app) / 1000)); /* once a second, row 4 */
        u32 owner, held = n3ds_log_lock_held_ms(&owner);
        if (held > 500 && !lock_reported) {
            char msg[96];
            snprintf(msg, sizeof(msg), "[LOCK] log lock held %lu ms by thread %08lx (deadlock or a blocking call inside it)\n",
                     (unsigned long)held, (unsigned long)owner);
            n3ds_log_raw(msg);
            lock_reported = 1;
        } else if (!held) {
            lock_reported = 0;
        }
        u32 now = s_frames_done;
        if (now != last) { last = now; still = 0; reported = 0; continue; }
        if (++still >= 3 && !reported) {
            char msg[160];
            snprintf(msg, sizeof(msg), "[WATCHDOG] no frame finished for 3 s: %lu frames done, last render step: %s\n",
                     (unsigned long)now, (const char*)s_step);
            n3ds_log_raw(msg);
            reported = 1;
        }
    }
}

static void ensure_frame(void) {
    if (s_in_frame) return;
    s_step = "wait for GPU (FrameBegin)";
    u64 t0 = osGetTime();
    extern unsigned long long n3ds_gpu_wait_ticks;
    unsigned long long t_wait = svcGetSystemTick();
    int reported = 0;
    while (!C3D_FrameBegin(C3D_FRAME_NONBLOCK)) { /* the GPU still runs the previous frame */
        if (!reported && osGetTime() - t0 > 2000) {
            report_gpu_hang(s_frames_done);
            reported = 1;
        }
        svcSleepThread(200000);
    }
    n3ds_gpu_wait_ticks += svcGetSystemTick() - t_wait;
    s_step = "frame setup";
    s_nrecs[0] = 0;
    C3D_FrameDrawOn(s_target);
    s_in_frame = 1;
    s_arena_used = 0;
    s_draw_vtx = NULL;
    s_buf_base = NULL; /* the arena restarts: set the buffer base again on the first draw */
    /* the GPU is done with the arena: pc_gx's tail (and verts it already wrote) goes back to the start */
    vertex_tail_set(0, g_gx.vertex_buffer, g_gx.current_vertex_idx);
    free_deleted_textures();
    apply_viewport(); /* FrameDrawOn reset the viewport */
    apply_scissor();
}

static u8 to_u8(float f) { return f <= 0.0f ? 0 : f >= 1.0f ? 255 : (u8)(f * 255.0f + 0.5f); }

static void gl_clear(GLbitfield mask) {
    ensure_frame();
    int bits = ((mask & GL_COLOR_BUFFER_BIT) ? C3D_CLEAR_COLOR : 0) |
               ((mask & GL_DEPTH_BUFFER_BIT) ? C3D_CLEAR_DEPTH : 0);
    u32 color = ((u32)to_u8(gs.clear[0]) << 24) | (to_u8(gs.clear[1]) << 16) |
                (to_u8(gs.clear[2]) << 8) | to_u8(gs.clear[3]);
    if (bits) C3D_RenderTargetClear(s_target, (C3D_ClearBits)bits, color, (u32)(gs.clear_depth * 0xFFFFFF));
}

/* --- color buffer readback (EFB copies, screenshots) --- */

static u32* s_readback; /* linear copy of the target: 400 rows (screen x, left to right) of 240 px (screen y, bottom up) */
static u32 s_stat_copies;

/* Run the draws queued so far and wait for the GPU, without showing the frame.
 * The color buffer keeps its contents, so drawing continues on top of it. */
static void sync_midframe(void) {
    if (!s_in_frame) return;
    if (s_arena_used) GSPGPU_FlushDataCache(s_arena, s_arena_used * sizeof(N3DSVtx));
    C3D_RenderTargetDetachOutput(s_target);
    C3D_FrameEnd(0);
    s_in_frame = 0;
    C3D_RenderTargetSetOutput(s_target, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
    ensure_frame(); /* waits for the GPU */
}

/* Copies the current color buffer to s_readback; returns 0 if out of memory.
 * ponytail: one GPU sync per call. Fine for the game's few EFB copies; if a scene copies
 * every frame, render that pass to a texture target instead (the target is rotated, so a
 * GPU-only copy would need rotated texcoords). */
static int readback(void) {
    s_step = "color buffer readback";
    if (!s_readback) s_readback = (u32*)linearAlloc(SCREEN_W * SCREEN_H * 4);
    if (!s_readback) return 0;
    ensure_frame();
    sync_midframe();
    C3D_SyncDisplayTransfer((u32*)s_target->frameBuf.colorBuf, GX_BUFFER_DIM(SCREEN_H, SCREEN_W), s_readback,
                            GX_BUFFER_DIM(SCREEN_H, SCREEN_W),
                            GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
                                GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                                GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
    GSPGPU_InvalidateDataCache(s_readback, SCREEN_W * SCREEN_H * 4);
    return 1;
}

static void apply_tex_params(N3DSTex* t);

/* EFB copy of the whole screen, GPU only: the color buffer tiles go into a 256x512 RGBA8 texture
 * with one TextureCopy. No CPU readback (in Azahar that read returned the previous copy's pixels)
 * and no re-upload. The texture keeps the target's rotation: texture row m = screen x,
 * column = screen y (up). n3ds_tev.c rotates the texcoords of draws that sample it (rot = 1).
 * Returns 0 if out of memory; the caller then uses the readback path. */
GLuint n3ds_gl_capture_screen(void) {
    N3DSTex* t = (N3DSTex*)calloc(1, sizeof(N3DSTex));
    if (!t) return 0;
    if (!C3D_TexInitVRAM(&t->tex, 256, 512, GPU_RGBA8) && !C3D_TexInit(&t->tex, 256, 512, GPU_RGBA8)) {
        free(t);
        return 0;
    }
    s_step = "screen capture";
    ensure_frame();
    sync_midframe(); /* the frame's draws must be in the color buffer */
    const u32 line = SCREEN_H * 8 * 4; /* one row of 8x8 tiles of the target, bytes */
    C3D_SyncTextureCopy((u32*)s_target->frameBuf.colorBuf, GX_BUFFER_DIM(line >> 4, 0), (u32*)t->tex.data,
                        GX_BUFFER_DIM(line >> 4, (256 * 8 * 4 - line) >> 4), line * (SCREEN_W / 8),
                        GX_TRANSFER_RAW_COPY(1));
    t->valid = 1;
    t->rot = 1;
    t->wrap_s = t->wrap_t = GPU_CLAMP_TO_EDGE;
    t->filter = GPU_LINEAR;
    apply_tex_params(t);
    s_stat_copies++;
    return (GLuint)(uintptr_t)t;
}

int n3ds_gl_tex_valid(GLuint name) {
    N3DSTex* t = (N3DSTex*)(uintptr_t)name;
    return t && t->valid;
}

int n3ds_gl_tex_rot(GLuint name) {
    N3DSTex* t = (N3DSTex*)(uintptr_t)name;
    return t && t->valid && t->rot;
}

/* GL window pixel (x, y), y up -> 0xRRGGBBAA */
static u32 readback_px(int x, int y) { return s_readback[x * SCREEN_H + y]; }

/* debug switch "shots": top screen as BMP every 300 frames (shots/NNNNN.bmp, max 20) */
static void save_shot(u32 frame) {
    static int nshots;
    if (nshots >= 20 || !readback()) return;
    char path[48];
    if (nshots++ == 0) mkdir("shots", 0777);
    snprintf(path, sizeof(path), "shots/%05lu.bmp", (unsigned long)frame);
    FILE* f = fopen(path, "wb");
    if (!f) return;
    static u8 row[SCREEN_W * 3];
    u32 size = 54 + sizeof(row) * SCREEN_H;
    u8 hdr[54] = { 'B', 'M' };
    memcpy(hdr + 2, &size, 4);
    hdr[10] = 54; hdr[14] = 40;
    u32 w = SCREEN_W, h = SCREEN_H;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, 54, f);
    for (int y = 0; y < SCREEN_H; y++) { /* BMP rows go bottom up, as GL */
        for (int x = 0; x < SCREEN_W; x++) {
            u32 v = readback_px(x, y);
            row[x * 3] = (u8)(v >> 8); row[x * 3 + 1] = (u8)(v >> 16); row[x * 3 + 2] = (u8)(v >> 24);
        }
        fwrite(row, 1, sizeof(row), f);
    }
    fclose(f);
}

/* --- performance: bottom-screen status every 30 frames, [PERF] log line every 600 --- */

float g_n3ds_audio_fps, g_n3ds_audio_mix_ms; /* set by pc_audio.c */

static void perf_frame(void) {
    extern void n3ds_status(int row, const char* fmt, ...);
    extern u32 n3ds_heap_free(void);
    static u64 last;
    static u32 n, stutters, sum_stutters, sum_frames, sum_draws, sum_verts;
    static float worst, sum_worst;
    static u64 sum_ticks, win_ticks;
    u64 now = svcGetSystemTick();
    if (last) {
        u64 dt = now - last;
        float ms = (float)dt / (float)CPU_TICKS_PER_MSEC;
        win_ticks += dt;
        if (ms > worst) worst = ms;
        if (ms > 20.0f) stutters++;
    }
    last = now;
    ++n;
    if (win_ticks < SYSCLOCK_ARM11) return; /* windows of 1 s, whatever the frame rate */

    float secs = (float)win_ticks / (float)SYSCLOCK_ARM11;
    float fps = secs > 0.0f ? n / secs : 0.0f;
    u32 heap = n3ds_heap_free() >> 10, lin = (u32)(linearSpaceFree() >> 10);
    static int status_traced;
    if (!status_traced) printf("[TRACE] first status update\n");
    n3ds_status(0, "FPS %4.1f  avg %4.1fms  worst %4.1fms", fps, secs * 1000.0f / n, worst);
    n3ds_status(1, "stutter(>20ms) %lu/%lu  draws %lu  vtx %lu", (unsigned long)stutters, (unsigned long)n,
                (unsigned long)(s_stat_draws / n), (unsigned long)(s_stat_verts / n));
    n3ds_status(2, "heap %luK  linear %luK  EFB %lu", (unsigned long)heap, (unsigned long)lin,
                (unsigned long)s_stat_copies);
    n3ds_status(3, "audio %4.1f/s  mix %4.2fms", g_n3ds_audio_fps, g_n3ds_audio_mix_ms);
    if (!status_traced++) printf("[TRACE] first status update done\n");

    sum_frames += n; sum_ticks += win_ticks; sum_stutters += stutters;
    sum_draws += s_stat_draws; sum_verts += s_stat_verts;
    if (worst > sum_worst) sum_worst = worst;

    extern void pc_gx_texture_work_stats(u32* lookups, u32* scan_total, u32* uploads);
    u32 tex_lookups, tex_scan, tex_uploads;
    pc_gx_texture_work_stats(&tex_lookups, &tex_scan, &tex_uploads);
    printf("[WORK] vtx_bytes/f %lu  draws/f %lu  tex_lookups/f %lu scan_steps/f %lu (%.1f/lookup) uploads %lu/%luf\n",
           (unsigned long)(s_stat_verts * sizeof(N3DSVtx) / (n ? n : 1)), (unsigned long)(s_stat_draws / n),
           (unsigned long)(tex_lookups / n), (unsigned long)(tex_scan / n),
           tex_lookups ? (double)tex_scan / tex_lookups : 0.0, (unsigned long)tex_uploads, (unsigned long)n);
    printf("[WORK] gpu cmd words/draw %.1f\n", s_stat_draws ? (double)s_stat_cmdwords / s_stat_draws : 0.0);
    s_stat_cmdwords = 0;
    {   /* emu64 N64 -> GC conversions: .data results are kept, bss results are redone every frame */
        extern unsigned int pc_emu64_texconv_n[2], pc_emu64_texconv_bytes[2];
        printf("[TEXCONV] per frame: data %lu (%lu B)  bss %lu (%lu B)\n",
               (unsigned long)(pc_emu64_texconv_n[0] / n), (unsigned long)(pc_emu64_texconv_bytes[0] / n),
               (unsigned long)(pc_emu64_texconv_n[1] / n), (unsigned long)(pc_emu64_texconv_bytes[1] / n));
        pc_emu64_texconv_n[0] = pc_emu64_texconv_n[1] = pc_emu64_texconv_bytes[0] = pc_emu64_texconv_bytes[1] = 0;
    }

    /* first lines early (windows 2 and 6), so a hang in the first seconds still leaves numbers */
    static u32 total_frames, windows;
    total_frames += n;
    if (++windows == 2 || windows == 6)
        printf("[PERF] frame %lu: %.1f fps, worst %.1fms, %lu draws %lu vtx, GPU draw %.1fms proc %.1fms, heap %luK linear %luK\n",
               (unsigned long)total_frames, fps, worst, (unsigned long)(s_stat_draws / n),
               (unsigned long)(s_stat_verts / n), C3D_GetDrawingTime(), C3D_GetProcessingTime(),
               (unsigned long)heap, (unsigned long)lin);
    if (sum_ticks >= 20ULL * SYSCLOCK_ARM11) { /* long summary every 20 s */
        extern int g_n3ds_dbg; /* n3ds_tev.c; 2048 = calls */
        if (g_n3ds_dbg & 2048) { extern void n3ds_calls_report(void); extern void n3ds_emu64_report(void); n3ds_calls_report(); n3ds_emu64_report(); }
        float s = (float)sum_ticks / (float)SYSCLOCK_ARM11;
        printf("[PERF] %.0fs: %.1f fps, worst %.1fms, %lu stutters, %lu draws %lu vtx/frame, heap %luK linear %luK free, audio mix %.2fms\n",
               s, sum_frames / s, sum_worst, (unsigned long)sum_stutters, (unsigned long)(sum_draws / sum_frames),
               (unsigned long)(sum_verts / sum_frames), (unsigned long)heap, (unsigned long)lin, g_n3ds_audio_mix_ms);
        {   /* [FRAME] per-frame split of the window: GPU wait at frame start, and C3D_FrameEnd submit */
            extern unsigned long long n3ds_gpu_wait_ticks, n3ds_frame_end_ticks;
            double ms_wait = (double)n3ds_gpu_wait_ticks / (double)sum_frames / 268123.0;
            double ms_end = (double)n3ds_frame_end_ticks / (double)sum_frames / 268123.0;
            printf("[FRAME] %.0fs: gpu wait %.2f ms/frame, frame end %.2f ms/frame, wall %.2f ms/frame\n",
                   s, ms_wait, ms_end, s * 1000.0 / sum_frames);
            n3ds_gpu_wait_ticks = n3ds_frame_end_ticks = 0;
        }
        sum_frames = sum_stutters = sum_draws = sum_verts = 0;
        sum_ticks = 0;
        sum_worst = 0.0f;
    }
    n = stutters = 0;
    worst = 0.0f;
    win_ticks = 0;
    s_stat_draws = s_stat_verts = s_stat_copies = 0;
}

/* SDL_GL_SwapWindow */
void n3ds_gl_swap(void) {
    extern int g_n3ds_dbg; /* n3ds_tev.c debug switches; 64 = shots */
    static u32 frame;
    ++frame; /* shots: frames 60, 120, 180 (boot), then every 300 */
    if ((g_n3ds_dbg & 64) && (frame == 60 || frame == 120 || frame == 180 || frame % 300 == 0)) save_shot(frame);
    ensure_frame();
    if (s_arena_used) GSPGPU_FlushDataCache(s_arena, s_arena_used * sizeof(N3DSVtx));
    s_step = "FrameEnd (submit)";
    { extern void n3ds_dl_flush(void); n3ds_dl_flush(); } /* [DL] write the dumped frame, if this is it */
    { extern unsigned int n3ds_frame_id; n3ds_frame_id++; } /* [USE] per-frame counting (n3ds_calls.c) */
    extern unsigned long long n3ds_frame_end_ticks;
    unsigned long long t_end = svcGetSystemTick();
    C3D_FrameEnd(0);
    n3ds_frame_end_ticks += svcGetSystemTick() - t_end;
    memcpy(s_recs[1], s_recs[0], sizeof(s_recs[0]));
    s_nrecs[1] = s_nrecs[0];
    s_frames_done++;
    /* APT (HOME, sleep, exit) between frames, as devkitPro programs do: never inside a
     * C3D frame. A HOME press suspends this thread in here until the app is resumed. */
    s_step = "inside aptMainLoop (HOME/sleep handling)";
    s_apt_ok = aptMainLoop();
    s_step = "game code (after FrameEnd)";
    s_in_frame = 0;
    perf_frame();
}

/* --- buffers and draws --- */

static void gl_buffer_data(GLenum target, GLsizeiptr size, const void* data, GLenum usage) {
    (void)usage;
    if (target == GL_ELEMENT_ARRAY_BUFFER) { /* pc_gx_init: quad -> triangle indices, once */
        if (s_quad_idx) linearFree(s_quad_idx);
        s_quad_idx = (u16*)linearAlloc((size_t)size);
        if (s_quad_idx) {
            memcpy(s_quad_idx, data, (size_t)size);
            GSPGPU_FlushDataCache(s_quad_idx, (u32)size);
        }
        return;
    }
    if (target != GL_ARRAY_BUFFER || data != g_gx.vertex_buffer) return;

    _Static_assert(sizeof(PCGXVertex) == sizeof(N3DSVtx) && offsetof(PCGXVertex, pal) == offsetof(N3DSVtx, pal) &&
                       offsetof(PCGXVertex, texcoord) == offsetof(N3DSVtx, tc) &&
                       offsetof(PCGXVertex, color0) == offsetof(N3DSVtx, clr),
                   "PCGXVertex must match N3DSVtx (pc_gx_internal.h)");
    ensure_frame();
    /* pc_gx wrote the run at the arena tail (g_gx.vertex_buffer): it is used in place, no copy */
    u32 count = (u32)size / sizeof(PCGXVertex);
    s_draw_vtx = (N3DSVtx*)data;
    s_arena_used = (u32)(s_draw_vtx - s_arena) + count;
    s_stat_verts += count;
}

#define ARENA_VERTS ((u32)(N3DS_VTX_ARENA / sizeof(N3DSVtx)))

/* Point pc_gx's vertex buffer at arena vertex `at`, rounded up to 4 (4 x 40 = 160, so each run
 * starts 16-byte aligned), and move the `keep` verts already written at `src` there. */
static void vertex_tail_set(u32 at, const PCGXVertex* src, int keep) {
    static int warned;
    at = (at + 3) & ~3u;
    if (at + (u32)keep + 64 > ARENA_VERTS) { /* full: the rest of this frame's geometry is dropped */
        if (!warned) { warned = 1; printf("[3DS/GL] vertex arena full, draws dropped\n"); }
        at = ARENA_VERTS - 64;
        keep = 0;
    }
    PCGXVertex* dst = (PCGXVertex*)(s_arena + at);
    if (keep > 0 && src != dst) memmove(dst, src, (size_t)keep * sizeof(PCGXVertex));
    u32 cap = ARENA_VERTS - at;
    g_gx.vertex_buffer = dst;
    g_gx.vertex_cap = (int)(cap < PC_GX_MAX_VERTS ? cap : PC_GX_MAX_VERTS);
    g_gx.current_vertex_idx = keep;
}

/* pc_gx_init: g_gx was cleared */
void n3ds_gx_vertex_tail_init(void) { vertex_tail_set(s_arena_used, NULL, 0); }

/* pc_gx drew the first `count` verts at the tail: the verts written after them become the new tail */
void n3ds_gx_vertices_drawn(int count) {
    vertex_tail_set(s_arena_used, g_gx.vertex_buffer + count, g_gx.current_vertex_idx - count);
}

/* Vertex buffer base the GPU has now. C3D_GetBufInfo marks the buffer config dirty, and citro3d
 * then sends it again with the draw, so it is set only when the base changes: array draws use the
 * arena start plus an offset (once per frame); indexed quads need the batch start (no base vertex). */
static int draw_ready(const N3DSVtx* base) {
    if (s_program != N3DS_GX_PROG || !s_draw_vtx || s_vp_empty || s_sc_empty) return 0;
    if (gs.cull && gs.cull_face == GL_FRONT_AND_BACK) return 0;
    if (base != s_buf_base) {
        C3D_BufInfo* bi = C3D_GetBufInfo();
        BufInfo_Init(bi);
        BufInfo_Add(bi, base, sizeof(N3DSVtx), 5, 0x43210);
        s_buf_base = base;
    }
    s_stat_draws++;
    return 1;
}

static void gl_draw_arrays(GLenum mode, GLint first, GLsizei count) {
    static int warned;
    GPU_Primitive_t prim;
    switch (mode) {
        case GL_TRIANGLES: prim = GPU_TRIANGLES; break;
        case GL_TRIANGLE_STRIP: prim = GPU_TRIANGLE_STRIP; break;
        case GL_TRIANGLE_FAN: prim = GPU_TRIANGLE_FAN; break;
        default: /* PICA has no lines or points */
            if (!warned) { warned = 1; printf("[3DS/GL] line/point primitives skipped\n"); }
            return;
    }
    if (count >= 3 && draw_ready(s_arena)) {
        s_step = "draw (DrawArrays)";
        record_draw((int)prim >> 8, first, count);
        u32 off0 = cmd_offset();
        C3D_DrawArrays(prim, (int)(s_draw_vtx - s_arena) + first, count);
        u32 off1 = cmd_offset();
        if (off1 >= off0) s_stat_cmdwords += off1 - off0;
    }
}

static void gl_draw_elements(GLenum mode, GLsizei count, GLenum type, const void* offset) {
    (void)type;
    if (mode == GL_TRIANGLES && count >= 3 && s_quad_idx && draw_ready(s_draw_vtx))
    {
        s_step = "draw (DrawElements)";
        record_draw(4, 0, count); /* 4 = indexed quads */
        u32 off0 = cmd_offset();
        C3D_DrawElements(GPU_TRIANGLES, count, C3D_UNSIGNED_SHORT, (const u8*)s_quad_idx + (uintptr_t)offset);
        u32 off1 = cmd_offset();
        if (off1 >= off0) s_stat_cmdwords += off1 - off0;
    }
}

/* --- textures (GL name = N3DSTex pointer) --- */

C3D_Tex* n3ds_gl_tex(GLuint name) {
    N3DSTex* t = (N3DSTex*)(uintptr_t)name;
    return t && t->valid ? &t->tex : NULL;
}

static void gl_gen_textures(GLsizei n, GLuint* out) {
    for (GLsizei i = 0; i < n; i++) {
        N3DSTex* t = (N3DSTex*)calloc(1, sizeof(N3DSTex));
        if (t) {
            t->wrap_s = t->wrap_t = GPU_REPEAT;
            t->filter = GPU_LINEAR;
        }
        out[i] = (GLuint)(uintptr_t)t;
    }
}

static void gl_delete_textures(GLsizei n, const GLuint* names) {
    for (GLsizei i = 0; i < n; i++) {
        N3DSTex* t = (N3DSTex*)(uintptr_t)names[i];
        if (!t) continue;
        if (t == s_bound_tex) s_bound_tex = NULL;
        t->next_free = s_free_list;
        s_free_list = t;
    }
}

static void gl_bind_texture(GLenum target, GLuint name) {
    (void)target;
    s_bound_tex = (N3DSTex*)(uintptr_t)name;
}

static void apply_tex_params(N3DSTex* t) {
    if (!t->valid) return;
    C3D_TexSetWrap(&t->tex, t->wrap_s, t->wrap_t);
    C3D_TexSetFilter(&t->tex, t->filter, t->filter);
}

static void gl_tex_parameteri(GLenum target, GLenum pname, GLint param) {
    (void)target;
    N3DSTex* t = s_bound_tex;
    if (!t) return;
    GPU_TEXTURE_WRAP_PARAM wrap = param == GL_CLAMP_TO_EDGE ? GPU_CLAMP_TO_EDGE
                                : param == GL_MIRRORED_REPEAT ? GPU_MIRRORED_REPEAT : GPU_REPEAT;
    switch (pname) {
        case GL_TEXTURE_WRAP_S: t->wrap_s = wrap; break;
        case GL_TEXTURE_WRAP_T: t->wrap_t = wrap; break;
        case GL_TEXTURE_MAG_FILTER:
        case GL_TEXTURE_MIN_FILTER: t->filter = param == GL_NEAREST ? GPU_NEAREST : GPU_LINEAR; break;
        default: return;
    }
    apply_tex_params(t);
}

static u32 pow2_dim(int v) {
    u32 p = 8;
    while (p < (u32)v && p < 1024) p <<= 1;
    return p;
}

/* Index of pixel (x, y) inside an 8x8 PICA tile (Morton order) */
static u32 morton8(u32 x, u32 y) {
    return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
}

/* RGBA8 rows (row 0 = t 0) -> PICA tiled texture. Non power-of-two sizes are resampled
 * (nearest). Format: LA8 for gray, RGB565 opaque, RGBA5551 1-bit alpha, else RGBA8. */
static void gl_tex_image_2d(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border,
                            GLenum fmt, GLenum type, const void* data) {
    (void)target; (void)ifmt; (void)border; (void)fmt; (void)type;
    N3DSTex* t = s_bound_tex;
    if (!t || level != 0 || w <= 0 || h <= 0 || !data) return;
    snprintf(s_step_buf, sizeof(s_step_buf), "texture upload %ldx%ld", (long)w, (long)h);
    s_step = s_step_buf;
    const u8* px = (const u8*)data;
    int gray = 1, opaque = 1, binary = 1;
    for (int i = 0; i < w * h; i++) {
        const u8* p = px + i * 4;
        gray &= p[0] == p[1] && p[1] == p[2];
        opaque &= p[3] == 255;
        binary &= p[3] == 0 || p[3] == 255;
    }
    GPU_TEXCOLOR tf = gray ? GPU_LA8 : opaque ? GPU_RGB565 : binary ? GPU_RGBA5551 : GPU_RGBA8;
    u32 pw = pow2_dim(w), ph = pow2_dim(h);

    extern int g_n3ds_dbg; /* n3ds_tev.c debug switches; 32 = dumptex */
    static int ndump;
    if ((g_n3ds_dbg & 32) && ndump < 1500) { /* texdump/NNN_WxH_fmt.rgba: decoded RGBA8 input */
        char path[64];
        if (ndump == 0) mkdir("texdump", 0777);
        snprintf(path, sizeof(path), "texdump/%03d_%ldx%ld_%d.rgba", ndump++, (long)w, (long)h, (int)tf);
        FILE* f = fopen(path, "wb");
        if (f) { fwrite(px, 4, (size_t)w * h, f); fclose(f); }
    }

    if (t->valid) { /* re-specified: GPU may still read the old data this frame */
        N3DSTex* old = (N3DSTex*)malloc(sizeof(N3DSTex));
        if (old) {
            *old = *t;
            old->next_free = s_free_list;
            s_free_list = old;
        }
        t->valid = 0;
    }
    if (!C3D_TexInit(&t->tex, (u16)pw, (u16)ph, tf)) {
        static int warned;
        if (!warned) { warned = 1; printf("[3DS/GL] texture alloc failed (%ldx%ld), linear free %lu\n",
                                          (long)pw, (long)ph, (unsigned long)linearSpaceFree()); }
        return;
    }
    t->valid = 1;

    u8* out = (u8*)t->tex.data;
    u32 tiles_w = pw >> 3;
    /* Column source index per output column: a table, not a divide per texel. Most GC
     * textures are already power-of-two (w == pw), so this is then just mx (no table work). */
    static u32 colsrc[1024];
    int col_identity = (u32)w == pw;
    if (!col_identity) for (u32 mx = 0; mx < pw; mx++) colsrc[mx] = mx * (u32)w / pw;
    for (u32 my = 0; my < ph; my++) {
        /* PICA samples t = 0 at the last row in memory. ph is at most 1024, so this row
         * divide (not per texel) is cheap; only the per-texel column lookup matters. */
        const u8* row = px + (size_t)(((ph - 1 - my) * (u32)h / ph) * (u32)w) * 4;
        for (u32 mx = 0; mx < pw; mx++) {
            const u8* p = row + (size_t)(col_identity ? mx : colsrc[mx]) * 4;
            u32 idx = ((my >> 3) * tiles_w + (mx >> 3)) * 64 + morton8(mx & 7, my & 7);
            switch (tf) {
                case GPU_LA8: out[idx * 2] = p[3]; out[idx * 2 + 1] = p[0]; break;
                case GPU_RGB565:
                    ((u16*)out)[idx] = (u16)(((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3));
                    break;
                case GPU_RGBA5551:
                    ((u16*)out)[idx] = (u16)(((p[0] >> 3) << 11) | ((p[1] >> 3) << 6) | ((p[2] >> 3) << 1) | (p[3] >> 7));
                    break;
                default: ((u32*)out)[idx] = ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; break;
            }
        }
    }
    C3D_TexFlush(&t->tex);
    apply_tex_params(t);
}

/* --- GPU self-test (switch "gputest") ---
 * Runs before the game: one feature per frame. Each frame must finish on the GPU within
 * 1 s, else the log says HANG and names the test (the GPU stays hung, so this stops). */

extern void n3ds_tev_test_uniforms(int nlights);
extern void n3ds_tev_test_fog(int on);

static const char* s_test_name;

static void test_wait(void) {
    u64 t0 = osGetTime();
    while (!C3D_FrameBegin(C3D_FRAME_NONBLOCK)) {
        if (osGetTime() - t0 > 1000) {
            printf("[GPUTEST] HANG: GPU did not finish test '%s' within 1 s\n", s_test_name);
            for (;;) svcSleepThread(1000000000LL);
        }
        svcSleepThread(1000000);
    }
    if (s_test_name) printf("[GPUTEST] pass: %s\n", s_test_name);
    C3D_FrameDrawOn(s_target);
    s_in_frame = 1;
    s_arena_used = 0;
    C3D_RenderTargetClear(s_target, C3D_CLEAR_ALL, 0x204060FF, 0xFFFFFF);
}

static void test_submit(const char* name) {
    s_test_name = name;
    printf("[GPUTEST] start: %s\n", name);
    if (s_arena_used) GSPGPU_FlushDataCache(s_arena, s_arena_used * sizeof(N3DSVtx));
    C3D_FrameEnd(0);
    s_in_frame = 0;
    test_wait();
}

/* n vertices of a centered shape at depth z, color rgba */
static N3DSVtx* test_verts(int n, float z, u32 rgba) {
    static const float pos[4][2] = { { -0.5f, -0.5f }, { 0.5f, -0.5f }, { 0.5f, 0.5f }, { -0.5f, 0.5f } };
    s_arena_used = (s_arena_used + 3) & ~3u;
    N3DSVtx* v = s_arena + s_arena_used;
    for (int i = 0; i < n; i++) {
        v[i].pos[0] = pos[i & 3][0]; v[i].pos[1] = pos[i & 3][1]; v[i].pos[2] = z;
        v[i].nrm[0] = 0; v[i].nrm[1] = 0; v[i].nrm[2] = 1;
        v[i].clr[0] = (u8)(rgba >> 24); v[i].clr[1] = (u8)(rgba >> 16); v[i].clr[2] = (u8)(rgba >> 8); v[i].clr[3] = (u8)rgba;
        v[i].tc[0] = pos[i & 3][0] + 0.5f; v[i].tc[1] = pos[i & 3][1] + 0.5f;
    }
    s_arena_used += (u32)n;
    C3D_BufInfo* bi = C3D_GetBufInfo();
    BufInfo_Init(bi);
    BufInfo_Add(bi, v, sizeof(N3DSVtx), 4, 0x3210);
    return v;
}

static void test_env(GPU_TEVSRC src) { /* stage 0 = src, stages 1-5 pass */
    for (int i = 0; i < 6; i++) C3D_TexEnvInit(C3D_GetTexEnv(i));
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvSrc(env, C3D_Both, src, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    C3D_TexEnvBufUpdate(C3D_Both, 0);
}

static GLuint test_texture(int w, int h, int kind) { /* 0 RGBA8, 1 RGB565, 2 RGBA5551, 3 LA8 */
    static u8 px[64 * 64 * 4];
    for (int i = 0; i < w * h; i++) {
        u8 c = (u8)((i * 7) & 0xFF);
        px[i * 4 + 0] = c;
        px[i * 4 + 1] = kind == 3 ? c : (u8)(255 - c);
        px[i * 4 + 2] = kind == 3 ? c : 128;
        px[i * 4 + 3] = kind == 0 ? (u8)(i & 0xFF) : kind == 2 ? (i & 1 ? 255 : 0) : 255;
    }
    GLuint t;
    gl_gen_textures(1, &t);
    gl_bind_texture(GL_TEXTURE_2D, t);
    gl_tex_image_2d(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    return t;
}


void n3ds_gl_gputest(void) {
    printf("[GPUTEST] begin (O3DS GPU checks, one feature per frame)\n");
    s_test_name = NULL;
    test_wait();

    n3ds_tev_test_uniforms(0);
    apply_viewport();
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    C3D_CullFace(GPU_CULL_NONE);
    C3D_TexBind(0, NULL);
    test_env(GPU_PRIMARY_COLOR);
    test_submit("clear only");

    /* A-C isolate the first draw: pipeline setup, vertex layout, shader code */
    {
        static DVLB_s* dvlb[7];
        static shaderProgram_s prog[7]; /* min, flat, nolit, out5, in4, out3, out4 */
        const u32* bins[7] = { (const u32*)test_min_shbin, (const u32*)test_flat_shbin, (const u32*)gx_nolit_shbin,
                               (const u32*)test_out5_shbin, (const u32*)test_in4_shbin,
                               (const u32*)test_out3_shbin, (const u32*)test_out4_shbin };
        const u32 sizes[7] = { test_min_shbin_size, test_flat_shbin_size, gx_nolit_shbin_size,
                               test_out5_shbin_size, test_in4_shbin_size, test_out3_shbin_size, test_out4_shbin_size };
        for (int i = 0; i < 7; i++) {
            dvlb[i] = DVLB_ParseFile((u32*)bins[i], sizes[i]);
            shaderProgramInit(&prog[i]);
            shaderProgramSetVsh(&prog[i], &dvlb[i]->DVLE[0]);
        }

        /* A: 2 inputs (float3 position, ubyte4 color), 16-byte stride, pass-through shader */
        static const float tri[3][2] = { { -0.5f, -0.5f }, { 0.5f, -0.5f }, { 0.0f, 0.5f } };
        u8* buf = (u8*)linearAlloc(3 * 16);
        for (int i = 0; i < 3; i++) {
            float p[3] = { tri[i][0], tri[i][1], -0.5f };
            memcpy(buf + i * 16, p, 12);
            buf[i * 16 + 12] = 255; buf[i * 16 + 13] = 0; buf[i * 16 + 14] = 255; buf[i * 16 + 15] = 255;
        }
        GSPGPU_FlushDataCache(buf, 3 * 16);
        C3D_BindProgram(&prog[0]);
        C3D_AttrInfo* ai = C3D_GetAttrInfo();
        AttrInfo_Init(ai);
        AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 3);
        AttrInfo_AddLoader(ai, 1, GPU_UNSIGNED_BYTE, 4);
        C3D_BufInfo* bi = C3D_GetBufInfo();
        BufInfo_Init(bi);
        BufInfo_Add(bi, buf, 16, 2, 0x10);
        C3D_DrawArrays(GPU_TRIANGLES, 0, 3);
        test_submit("A: minimal shader, 2 attributes, 16-byte stride");

        /* B3a-e: A's inputs and layout with texcoord outputs. B3 (5 outputs, no texture
         * unit) hung on O3DS; these find which texcoord outputs and unit states hang. */
        GLuint ut[3];
        for (int k = 0; k < 3; k++) ut[k] = test_texture(32, 16, k);
        bi = C3D_GetBufInfo();
        BufInfo_Init(bi);
        BufInfo_Add(bi, buf, 16, 2, 0x10);

        C3D_BindProgram(&prog[5]);
        C3D_TexBind(0, NULL);
        C3D_DrawArrays(GPU_TRIANGLES, 0, 3);
        test_submit("B3a: texcoord0 output, no texture unit");

        C3D_BindProgram(&prog[5]);
        C3D_TexBind(0, n3ds_gl_tex(ut[0]));
        bi = C3D_GetBufInfo(); BufInfo_Init(bi); BufInfo_Add(bi, buf, 16, 2, 0x10);
        C3D_DrawArrays(GPU_TRIANGLES, 0, 3);
        test_submit("B3b: texcoord0 output, unit 0 enabled");

        C3D_BindProgram(&prog[6]);
        C3D_TexBind(0, NULL);
        bi = C3D_GetBufInfo(); BufInfo_Init(bi); BufInfo_Add(bi, buf, 16, 2, 0x10);
        C3D_DrawArrays(GPU_TRIANGLES, 0, 3);
        test_submit("B3c: texcoord0-1 outputs, no texture unit");

        C3D_BindProgram(&prog[3]);
        for (int k = 0; k < 3; k++) C3D_TexBind(k, n3ds_gl_tex(ut[k]));
        bi = C3D_GetBufInfo(); BufInfo_Init(bi); BufInfo_Add(bi, buf, 16, 2, 0x10);
        C3D_DrawArrays(GPU_TRIANGLES, 0, 3);
        test_submit("B3e: texcoord0-2 outputs, units 0-2 enabled");
        linearFree(buf);

        /* B1: the game's 36-byte layout read by the 2-input shader (padding skips normal, texcoord) */
        C3D_BindProgram(&prog[0]);
        ai = C3D_GetAttrInfo();
        AttrInfo_Init(ai);
        AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 3);
        AttrInfo_AddLoader(ai, 1, GPU_UNSIGNED_BYTE, 4);
        N3DSVtx* gv = test_verts(3, -0.5f, 0xFFFF00FF);
        bi = C3D_GetBufInfo();
        BufInfo_Init(bi);
        BufInfo_Add(bi, gv, sizeof(N3DSVtx), 4, 0xD1E0); /* pos, 12 B pad, color, 8 B pad */
        C3D_DrawArrays(GPU_TRIANGLES, 0, 3);
        test_submit("B1: game 36-byte layout via padding, 2 inputs, 2 outputs");

        /* B2: all 4 game attributes (float3, float3, ubyte4, float2), 2 outputs */
        extern void n3ds_tev_bind_main(void);
        n3ds_tev_bind_main();
        C3D_BindProgram(&prog[4]);
        test_verts(3, -0.5f, 0x00FF00FF);
        C3D_DrawArrays(GPU_TRIANGLES, 0, 3);
        test_submit("B2: game 4-attribute layout, 4 inputs, 2 outputs");

        /* B: the game's 4-attribute, 36-byte layout; shader without branch or loop, 5 outputs */
        n3ds_tev_bind_main();
        C3D_BindProgram(&prog[1]);
        test_verts(3, -0.5f, 0x00FFFFFF);
        C3D_DrawArrays(GPU_TRIANGLES, 0, 3);
        test_submit("B: game vertex layout, flat shader");

        /* C: the game shader without the ifu/for lighting block (same uniforms) */
        C3D_BindProgram(&prog[2]);
        n3ds_tev_test_uniforms(0);
        test_verts(3, -0.5f, 0xFF8000FF);
        C3D_DrawArrays(GPU_TRIANGLES, 0, 3);
        test_submit("C: game shader without lighting block");

        n3ds_tev_bind_main(); /* D (next): the full game shader */
        n3ds_tev_test_uniforms(0);
    }

    test_verts(3, -0.5f, 0xFF0000FF);
    C3D_DrawArrays(GPU_TRIANGLES, 0, 3);
    test_submit("D: game shader, triangle, vertex color, no texture");

    test_verts(4, -0.5f, 0x00FF00FF);
    C3D_DrawElements(GPU_TRIANGLES, 6, C3D_UNSIGNED_SHORT, s_quad_idx);
    test_submit("indexed quad (DrawElements)");

    test_verts(4, -0.5f, 0x0000FFFF);
    C3D_DrawArrays(GPU_TRIANGLE_FAN, 0, 4);
    test_verts(4, -0.5f, 0xFFFF00FF);
    C3D_DrawArrays(GPU_TRIANGLE_STRIP, 0, 4);
    test_submit("triangle fan and strip");

    static const char* const fmt_names[4] = { "texture RGBA8", "texture RGB565", "texture RGBA5551", "texture LA8" };
    GLuint tex[4];
    for (int k = 0; k < 4; k++) {
        tex[k] = test_texture(32, 16, k);
        C3D_TexBind(0, n3ds_gl_tex(tex[k]));
        test_env(GPU_TEXTURE0);
        test_verts(3, -0.5f, 0xFFFFFFFF);
        C3D_DrawArrays(GPU_TRIANGLES, 0, 3);
        test_submit(fmt_names[k]);
    }

    C3D_TexBind(1, n3ds_gl_tex(tex[1]));
    C3D_TexBind(2, n3ds_gl_tex(tex[2]));
    for (int i = 0; i < 6; i++) C3D_TexEnvInit(C3D_GetTexEnv(i));
    C3D_TexEnv* e = C3D_GetTexEnv(0);
    C3D_TexEnvSrc(e, C3D_Both, GPU_TEXTURE0, GPU_TEXTURE1, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(e, C3D_Both, GPU_MODULATE);
    e = C3D_GetTexEnv(1);
    C3D_TexEnvSrc(e, C3D_Both, GPU_PREVIOUS, GPU_TEXTURE2, GPU_CONSTANT);
    C3D_TexEnvFunc(e, C3D_Both, GPU_INTERPOLATE);
    C3D_TexEnvColor(e, 0x80FF8040);
    e = C3D_GetTexEnv(2);
    C3D_TexEnvSrc(e, C3D_Both, GPU_PREVIOUS, GPU_CONSTANT, GPU_PREVIOUS_BUFFER);
    C3D_TexEnvFunc(e, C3D_Both, GPU_MULTIPLY_ADD);
    C3D_TexEnvColor(e, 0xFF404040);
    C3D_TexEnvBufUpdate(C3D_Both, 1);
    test_verts(3, -0.5f, 0xFFFFFFFF);
    C3D_DrawArrays(GPU_TRIANGLES, 0, 3);
    test_submit("3 textures, 3 combiner stages, constants, combiner buffer");

    C3D_TexBind(1, n3ds_gl_tex(tex[3]));
    C3D_TexBind(2, n3ds_gl_tex(tex[3]));
    C3D_TexBind(0, n3ds_gl_tex(tex[0]));
    test_env(GPU_TEXTURE0);
    C3D_AlphaTest(true, GPU_GREATER, 128);
    test_verts(3, -0.5f, 0xFFFFFFFF);
    C3D_DrawArrays(GPU_TRIANGLES, 0, 3);
    test_submit("alpha test");
    C3D_AlphaTest(false, GPU_ALWAYS, 0);

    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
    test_verts(3, -0.5f, 0xFFFFFF80);
    C3D_DrawArrays(GPU_TRIANGLES, 0, 3);
    test_submit("alpha blend");
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);

    C3D_DepthTest(true, GPU_LEQUAL, GPU_WRITE_ALL);
    apply_depth_range();
    test_verts(3, -0.5f, 0xFFFFFFFF);
    C3D_DrawArrays(GPU_TRIANGLES, 0, 3);
    test_verts(3, -0.2f, 0x808080FF);
    C3D_DrawArrays(GPU_TRIANGLES, 0, 3);
    test_submit("depth test and depth write");
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);

    C3D_SetScissor(GPU_SCISSOR_NORMAL, 40, 100, 200, 300);
    test_verts(3, -0.5f, 0xFFFFFFFF);
    C3D_DrawArrays(GPU_TRIANGLES, 0, 3);
    test_submit("scissor");
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);

    C3D_CullFace(GPU_CULL_BACK_CCW);
    test_verts(3, -0.5f, 0xFFFFFFFF);
    C3D_DrawArrays(GPU_TRIANGLES, 0, 3);
    test_submit("cull back faces");
    C3D_CullFace(GPU_CULL_NONE);

    n3ds_tev_test_fog(1);
    test_verts(3, -0.5f, 0xFFFFFFFF);
    C3D_DrawArrays(GPU_TRIANGLES, 0, 3);
    test_submit("fog LUT");
    n3ds_tev_test_fog(0);

    n3ds_tev_test_uniforms(1);
    test_verts(3, -0.5f, 0xFFFFFFFF);
    C3D_DrawArrays(GPU_TRIANGLES, 0, 3);
    test_submit("vertex lighting, 1 light (shader loop)");
    n3ds_tev_test_uniforms(8);
    test_verts(3, -0.5f, 0xFFFFFFFF);
    C3D_DrawArrays(GPU_TRIANGLES, 0, 3);
    test_submit("vertex lighting, 8 lights");
    n3ds_tev_test_uniforms(0);

    test_verts(3, -0.5f, 0xFFFFFFFF);
    C3D_DrawArrays(GPU_TRIANGLES, 0, 3);
    readback(); /* EFB copy path: mid-frame sync + display transfer */
    test_submit("color buffer readback (EFB copy)");

    C3D_FrameEnd(0);
    s_in_frame = 0;
    gl_delete_textures(4, tex);
    printf("[GPUTEST] all tests passed\n");
}

/* --- init --- */

static void gl_init(void) {
    static int done;
    if (done) return;
    done = 1;
    C3D_Init(N3DS_CMDBUF_SIZE);
    {   /* CPU watchdog on the second core (O3DS: core 1, time-limited) */
        s32 prio = 0x30;
        svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
        if (!threadCreate(watchdog_thread, NULL, 8 * 1024, prio - 2, 1, true))
            threadCreate(watchdog_thread, NULL, 8 * 1024, prio - 2, -2, true);
    }
    s_target = C3D_RenderTargetCreate(SCREEN_H, SCREEN_W, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    C3D_RenderTargetSetOutput(s_target, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
    s_arena = (N3DSVtx*)linearAlloc(N3DS_VTX_ARENA);
    if (!s_target || !s_arena) printf("[3DS/GL] citro3d init failed\n");
    apply_depth();
    apply_blend();
    apply_cull();
    apply_depth_range();
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    printf("[3DS/GL] citro3d up, linear free %luKB\n", (unsigned long)(linearSpaceFree() >> 10));
}

/* --- no-op and dummy entry points --- */

static void gl_noop(void) {}

static GLuint next_name = 1;
static void gl_gen(GLsizei n, GLuint* out) {
    for (GLsizei i = 0; i < n; i++) out[i] = next_name++;
}
static GLuint gl_create(void) { return next_name++; }
static GLuint gl_create_shader(GLenum type) { (void)type; return next_name++; }
static GLenum gl_get_error(void) { return GL_NO_ERROR; }
static void gl_get_iv(GLuint obj, GLenum pname, GLint* out) {
    (void)obj;
    *out = (pname == GL_INFO_LOG_LENGTH) ? 0 : GL_TRUE;
}
static void gl_get_info_log(GLuint obj, GLsizei max, GLsizei* len, GLchar* log) {
    (void)obj;
    if (len) *len = 0;
    if (log && max > 0) log[0] = '\0';
}
static void gl_get_integerv(GLenum pname, GLint* out) { (void)pname; *out = 0; }
static const GLubyte* gl_get_stringi(GLenum name, GLuint i) { (void)name; (void)i; return (const GLubyte*)""; }
static GLint gl_get_uniform_location(GLuint prog, const GLchar* name) { (void)prog; (void)name; return 0; }
/* EFB copy (pc_gx.c GXCopyTex): GL_RGBA bytes, rows bottom up from y */
static void gl_read_pixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type, void* px) {
    (void)fmt; (void)type;
    u8* out = (u8*)px;
    s_stat_copies++;
    if (!readback()) {
        printf("[EFB] readback failed: linear free %luK\n", (unsigned long)(linearSpaceFree() >> 10));
        memset(px, 0, (size_t)w * (size_t)h * 4);
        return;
    }
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++, out += 4) {
            int sx = x + i, sy = y + j;
            u32 v = (sx >= 0 && sx < SCREEN_W && sy >= 0 && sy < SCREEN_H) ? readback_px(sx, sy) : 0;
            out[0] = (u8)(v >> 24); out[1] = (u8)(v >> 16); out[2] = (u8)(v >> 8); out[3] = (u8)v;
        }
    }
}

#define NOOP(name, type) type glad_##name = (type)gl_noop;
#define STUB(name, type, fn) type glad_##name = fn;

NOOP(glActiveTexture, PFNGLACTIVETEXTUREPROC)
NOOP(glAttachShader, PFNGLATTACHSHADERPROC)
NOOP(glBindAttribLocation, PFNGLBINDATTRIBLOCATIONPROC)
NOOP(glBindBuffer, PFNGLBINDBUFFERPROC)
STUB(glBindTexture, PFNGLBINDTEXTUREPROC, gl_bind_texture)
NOOP(glBindVertexArray, PFNGLBINDVERTEXARRAYPROC)
STUB(glBlendEquation, PFNGLBLENDEQUATIONPROC, gl_blend_equation)
STUB(glBlendFunc, PFNGLBLENDFUNCPROC, gl_blend_func)
STUB(glBufferData, PFNGLBUFFERDATAPROC, gl_buffer_data)
STUB(glClear, PFNGLCLEARPROC, gl_clear)
STUB(glClearColor, PFNGLCLEARCOLORPROC, gl_clear_color)
STUB(glClearDepth, PFNGLCLEARDEPTHPROC, gl_clear_depth)
STUB(glColorMask, PFNGLCOLORMASKPROC, gl_color_mask)
NOOP(glCompileShader, PFNGLCOMPILESHADERPROC)
NOOP(glCompressedTexImage2D, PFNGLCOMPRESSEDTEXIMAGE2DPROC)
STUB(glCreateProgram, PFNGLCREATEPROGRAMPROC, gl_create)
STUB(glCreateShader, PFNGLCREATESHADERPROC, gl_create_shader)
STUB(glCullFace, PFNGLCULLFACEPROC, gl_cull_face)
NOOP(glDeleteBuffers, PFNGLDELETEBUFFERSPROC)
NOOP(glDeleteProgram, PFNGLDELETEPROGRAMPROC)
NOOP(glDeleteShader, PFNGLDELETESHADERPROC)
STUB(glDeleteTextures, PFNGLDELETETEXTURESPROC, gl_delete_textures)
NOOP(glDeleteVertexArrays, PFNGLDELETEVERTEXARRAYSPROC)
STUB(glDepthFunc, PFNGLDEPTHFUNCPROC, gl_depth_func)
STUB(glDepthMask, PFNGLDEPTHMASKPROC, gl_depth_mask)
STUB(glDepthRange, PFNGLDEPTHRANGEPROC, gl_depth_range)
STUB(glDisable, PFNGLDISABLEPROC, gl_disable)
STUB(glDrawArrays, PFNGLDRAWARRAYSPROC, gl_draw_arrays)
STUB(glDrawElements, PFNGLDRAWELEMENTSPROC, gl_draw_elements)
STUB(glEnable, PFNGLENABLEPROC, gl_enable)
NOOP(glEnableVertexAttribArray, PFNGLENABLEVERTEXATTRIBARRAYPROC)
NOOP(glFlush, PFNGLFLUSHPROC)
STUB(glGenBuffers, PFNGLGENBUFFERSPROC, gl_gen)
STUB(glGenTextures, PFNGLGENTEXTURESPROC, gl_gen_textures)
STUB(glGenVertexArrays, PFNGLGENVERTEXARRAYSPROC, gl_gen)
STUB(glGetError, PFNGLGETERRORPROC, gl_get_error)
STUB(glGetIntegerv, PFNGLGETINTEGERVPROC, gl_get_integerv)
STUB(glGetProgramInfoLog, PFNGLGETPROGRAMINFOLOGPROC, gl_get_info_log)
STUB(glGetProgramiv, PFNGLGETPROGRAMIVPROC, gl_get_iv)
STUB(glGetShaderInfoLog, PFNGLGETSHADERINFOLOGPROC, gl_get_info_log)
STUB(glGetShaderiv, PFNGLGETSHADERIVPROC, gl_get_iv)
STUB(glGetStringi, PFNGLGETSTRINGIPROC, gl_get_stringi)
STUB(glGetUniformLocation, PFNGLGETUNIFORMLOCATIONPROC, gl_get_uniform_location)
NOOP(glLineWidth, PFNGLLINEWIDTHPROC)
NOOP(glLinkProgram, PFNGLLINKPROGRAMPROC)
NOOP(glPixelStorei, PFNGLPIXELSTOREIPROC)
NOOP(glPointSize, PFNGLPOINTSIZEPROC)
STUB(glReadPixels, PFNGLREADPIXELSPROC, gl_read_pixels)
STUB(glScissor, PFNGLSCISSORPROC, gl_scissor)
NOOP(glShaderSource, PFNGLSHADERSOURCEPROC)
STUB(glTexImage2D, PFNGLTEXIMAGE2DPROC, gl_tex_image_2d)
STUB(glTexParameteri, PFNGLTEXPARAMETERIPROC, gl_tex_parameteri)
NOOP(glUniform1f, PFNGLUNIFORM1FPROC)
NOOP(glUniform1i, PFNGLUNIFORM1IPROC)
NOOP(glUniform1iv, PFNGLUNIFORM1IVPROC)
NOOP(glUniform2f, PFNGLUNIFORM2FPROC)
NOOP(glUniform2i, PFNGLUNIFORM2IPROC)
NOOP(glUniform2iv, PFNGLUNIFORM2IVPROC)
NOOP(glUniform3f, PFNGLUNIFORM3FPROC)
NOOP(glUniform3fv, PFNGLUNIFORM3FVPROC)
NOOP(glUniform3i, PFNGLUNIFORM3IPROC)
NOOP(glUniform3iv, PFNGLUNIFORM3IVPROC)
NOOP(glUniform4f, PFNGLUNIFORM4FPROC)
NOOP(glUniform4fv, PFNGLUNIFORM4FVPROC)
NOOP(glUniform4i, PFNGLUNIFORM4IPROC)
NOOP(glUniform4iv, PFNGLUNIFORM4IVPROC)
NOOP(glUniformMatrix3fv, PFNGLUNIFORMMATRIX3FVPROC)
NOOP(glUniformMatrix4fv, PFNGLUNIFORMMATRIX4FVPROC)
STUB(glUseProgram, PFNGLUSEPROGRAMPROC, gl_use_program)
NOOP(glVertexAttribPointer, PFNGLVERTEXATTRIBPOINTERPROC)
STUB(glViewport, PFNGLVIEWPORTPROC, gl_viewport)

int gladLoadGL(GLADloadfunc load) { (void)load; gl_init(); return 1; }
void* SDL_GL_GetProcAddress(const char* proc) { (void)proc; return (void*)gl_noop; }
