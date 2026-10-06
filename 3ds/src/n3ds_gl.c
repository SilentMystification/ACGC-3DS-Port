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
#include "pc_gx_internal.h"

#define N3DS_GX_PROG 0x7FFF0001u
#define N3DS_CMDBUF_SIZE (1024 * 1024)
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
    struct N3DSTex* next_free;
} N3DSTex;

/* GPU vertex: the PCGXVertex fields the shader reads (96 -> 36 bytes) */
typedef struct {
    float pos[3];
    float nrm[3];
    u8 clr[4];
    float tc[2];
} N3DSVtx;

static C3D_RenderTarget* s_target;
static int s_in_frame;
static N3DSVtx* s_arena;
static u32 s_arena_used;
static N3DSVtx* s_draw_vtx; /* last uploaded GX batch, NULL if none */
static u16* s_quad_idx;
static GLuint s_program;
static N3DSTex* s_bound_tex;
static N3DSTex* s_free_list; /* deleted during this frame; freed once the GPU is done */
static u32 s_stat_draws, s_stat_verts, s_stat_frames;

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

/* GL window rect (y up, 400x240) -> rotated target (x = screen y from bottom, y = screen x from right) */
static void apply_viewport(void) {
    C3D_SetViewport((u32)gs.vp[1], (u32)(SCREEN_W - gs.vp[0] - gs.vp[2]), (u32)gs.vp[3], (u32)gs.vp[2]);
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void apply_scissor(void) {
    if (!gs.scissor) {
        C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
        return;
    }
    int x0 = clampi(gs.sc[1], 0, SCREEN_H), x1 = clampi(gs.sc[1] + gs.sc[3], 0, SCREEN_H);
    int y0 = clampi(SCREEN_W - gs.sc[0] - gs.sc[2], 0, SCREEN_W), y1 = clampi(SCREEN_W - gs.sc[0], 0, SCREEN_W);
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
    while (s_free_list) {
        N3DSTex* t = s_free_list;
        s_free_list = t->next_free;
        if (t->valid) C3D_TexDelete(&t->tex);
        free(t);
    }
}

static void ensure_frame(void) {
    if (s_in_frame) return;
    C3D_FrameBegin(0); /* waits for the previous frame's GPU work */
    C3D_FrameDrawOn(s_target);
    s_in_frame = 1;
    s_arena_used = 0;
    s_draw_vtx = NULL;
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

/* SDL_GL_SwapWindow */
void n3ds_gl_swap(void) {
    ensure_frame();
    if (s_arena_used) GSPGPU_FlushDataCache(s_arena, s_arena_used * sizeof(N3DSVtx));
    C3D_FrameEnd(0);
    s_in_frame = 0;
    if (g_pc_verbose && ++s_stat_frames == 60) {
        printf("[3DS/GL] %lu draws, %lu verts per frame, linear free %luKB\n",
               (unsigned long)(s_stat_draws / 60), (unsigned long)(s_stat_verts / 60),
               (unsigned long)(linearSpaceFree() >> 10));
        s_stat_frames = s_stat_draws = s_stat_verts = 0;
    }
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

    static int warned;
    ensure_frame();
    u32 count = (u32)size / sizeof(PCGXVertex);
    s_draw_vtx = NULL;
    if ((s_arena_used + count) * sizeof(N3DSVtx) > N3DS_VTX_ARENA) {
        if (!warned) { warned = 1; printf("[3DS/GL] vertex arena full, draws dropped\n"); }
        return;
    }
    const PCGXVertex* src = (const PCGXVertex*)data;
    N3DSVtx* dst = s_arena + s_arena_used;
    for (u32 i = 0; i < count; i++) {
        memcpy(dst[i].pos, src[i].position, sizeof(dst[i].pos));
        memcpy(dst[i].nrm, src[i].normal, sizeof(dst[i].nrm));
        memcpy(dst[i].clr, src[i].color0, sizeof(dst[i].clr));
        memcpy(dst[i].tc, src[i].texcoord[0], sizeof(dst[i].tc));
    }
    s_draw_vtx = dst;
    s_arena_used += count;
    s_stat_verts += count;
}

static int draw_ready(void) {
    if (s_program != N3DS_GX_PROG || !s_draw_vtx) return 0;
    if (gs.cull && gs.cull_face == GL_FRONT_AND_BACK) return 0;
    C3D_BufInfo* bi = C3D_GetBufInfo();
    BufInfo_Init(bi);
    BufInfo_Add(bi, s_draw_vtx, sizeof(N3DSVtx), 4, 0x3210);
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
    if (draw_ready()) C3D_DrawArrays(prim, first, count);
}

static void gl_draw_elements(GLenum mode, GLsizei count, GLenum type, const void* offset) {
    (void)type;
    if (mode == GL_TRIANGLES && s_quad_idx && draw_ready())
        C3D_DrawElements(GPU_TRIANGLES, count, C3D_UNSIGNED_SHORT, (const u8*)s_quad_idx + (uintptr_t)offset);
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
    for (u32 my = 0; my < ph; my++) {
        /* PICA samples t = 0 at the last row in memory */
        const u8* row = px + (size_t)(((ph - 1 - my) * (u32)h / ph) * (u32)w) * 4;
        for (u32 mx = 0; mx < pw; mx++) {
            const u8* p = row + (size_t)(mx * (u32)w / pw) * 4;
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

/* --- init --- */

static void gl_init(void) {
    static int done;
    if (done) return;
    done = 1;
    C3D_Init(N3DS_CMDBUF_SIZE);
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
/* ponytail: EFB copies read black until render-to-texture lands (Phase 2 step 6) */
static void gl_read_pixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type, void* px) {
    (void)x; (void)y; (void)fmt; (void)type;
    memset(px, 0, (size_t)w * (size_t)h * 4);
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
