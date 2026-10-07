/* n3ds_tev.c - GX TEV, lighting, texgen and fog state -> citro3d.
 *
 * Replaces pc/src/pc_gx_tev.c (GLSL uber shader) on the 3DS. pc_gx_flush_vertices
 * calls n3ds_gx_upload() where the PC build uploads uniforms; the citro3d state set
 * here is latched into the command buffer by the next draw, like GL uniforms.
 *
 * TEV: each GX stage is d OP ((1-c)*a + b*c). It is folded into at most three PICA
 * combiner stages. REG0-2 and KONST are constants: AC only writes TEV PREV (see the
 * PC shader seed), so their values are known when the combiners are built.
 */
#include <3ds.h>
#include <citro3d.h>
#include "pc_gx_internal.h"
#include "gx_shader_shbin.h"

int g_pc_uber_shader_only = 0; /* PC command-line flag; no meaning here */

#define N3DS_GX_PROG 0x7FFF0001u /* fake GL program name; n3ds_gl.c draws only while it is bound */

static PCGXShaderVariant s_variant;
static DVLB_s* s_dvlb;
static shaderProgram_s s_prog;

static struct {
    int proj, pmv, pnrm, texmtx0, texmtx1, tgsrc, unitsel; /* pmv/pnrm: batch palette, 3 rows per pair */
    int matReg, matSel, ambReg, ambSel, unlitAcc, lightDir, lightCol, lightLoop, lit;
} s_u;

static C3D_FogLut s_fog_lut;

/* Current combiner and shader state; n3ds_gl.c copies it into each draw record so a GPU
 * hang report names the state of every draw in the frame. Texture fmt 0xFF = none. */
struct N3DSTevState { u8 pica, fog, lit, alpha; u8 fmt[3]; u16 w[3], h[3]; } g_n3ds_tev_state;

C3D_Tex* n3ds_gl_tex(GLuint name); /* n3ds_gl.c: NULL if no usable texture */

/* Render debug switches: words in sdmc:/3ds/AnimalCrossing/debug3ds.txt
 * (run_azahar.ps1 -Debug "..."): nofog nolight notex texonly logtev dumptex shots noscissor gputest locktest profile calls */
enum { DBG_NOFOG = 1, DBG_NOLIGHT = 2, DBG_NOTEX = 4, DBG_TEXONLY = 8, DBG_LOGTEV = 16, DBG_DUMPTEX = 32, DBG_SHOTS = 64, DBG_NOSCISSOR = 128, DBG_GPUTEST = 256, DBG_LOCKTEST = 512, DBG_PROFILE = 1024, DBG_CALLS = 2048 };
int g_n3ds_dbg; /* also read by n3ds_gl.c (dumptex) */
#define s_dbg g_n3ds_dbg

static void read_debug_switches(void) {
    static const char* const names[] = { "nofog", "nolight", "notex", "texonly", "logtev", "dumptex", "shots", "noscissor", "gputest", "locktest", "profile", "calls", "nomvflush", "nogxvtx", "palflush", "palone", "dldump", "nomemo", "pad", "threads", "nomissflush" };
    extern char g_n3ds_args[]; /* n3ds_sys.c: 3dslink arguments */
    char buf[512] = { 0 };
    FILE* f = fopen("debug3ds.txt", "r");
    if (f) {
        fread(buf, 1, 255, f);
        fclose(f);
    }
    strncat(buf, g_n3ds_args, 255);
    if (!buf[0]) return;
    for (int i = 0; i < 21; i++)
        if (strstr(buf, names[i])) s_dbg |= 1 << i;
    printf("[3DS/TEV] debug switches: %s (0x%x)\n", buf, s_dbg);
}

/* logtev: print each new TEV setup once (GX args per stage, PICA stage count) */
static void log_tev(int ns, int pica) {
    static u32 seen[64];
    static int nseen;
    u32 h = 2166136261u;
    for (int s = 0; s < ns; s++) {
        const PCGXTevStage* t = &g_gx.tev_stages[s];
        int v[] = { t->color_a, t->color_b, t->color_c, t->color_d, t->alpha_a, t->alpha_b, t->alpha_c,
                    t->alpha_d, t->color_op, t->alpha_op, t->tex_map, t->tex_coord };
        for (int i = 0; i < 12; i++) h = (h ^ (u32)v[i]) * 16777619u;
    }
    h ^= (u32)ns;
    for (int i = 0; i < nseen; i++)
        if (seen[i] == h) return;
    if (nseen == 64) return;
    seen[nseen++] = h;
    printf("[3DS/TEV] cfg %d: %d GX stages -> %d PICA stages, chans=%d light=%d\n", nseen, ns, pica,
           g_gx.num_chans, g_gx.chan_ctrl_enable[0]);
    for (int s = 0; s < ns; s++) {
        const PCGXTevStage* t = &g_gx.tev_stages[s];
        int map = t->tex_map;
        printf("[3DS/TEV]   s%d C(%d,%d,%d,%d)%s A(%d,%d,%d,%d)%s map=%d tc=%d tex=%s\n", s, t->color_a,
               t->color_b, t->color_c, t->color_d, t->color_op ? "sub" : "", t->alpha_a, t->alpha_b,
               t->alpha_c, t->alpha_d, t->alpha_op ? "sub" : "", map, t->tex_coord,
               n3ds_gl_tex((map >= 0 && map < 8) ? g_gx.gl_textures[map] : 0) ? "yes" : "no");
    }
}


static void warn_once(int* flag, const char* msg) {
    if (!*flag) {
        *flag = 1;
        printf("[3DS/TEV] %s\n", msg);
    }
}

/* --- combiner builder --- */

enum { A_ZERO, A_ONE, A_CONST, A_PREV, A_TEX, A_RAS, A_SUB };
typedef struct {
    u8 kind;
    u8 alpha; /* color channel only: read the source's alpha */
    u8 inv;   /* 1 - x (non-constant args only; constants are folded) */
    float v[3];
} TevArg;

typedef struct {
    u8 func; /* GPU_COMBINEFUNC */
    u8 n;    /* sources used */
    TevArg src[3];
    u8 scale;
} TevOp;

typedef struct { TevArg x, y; } Term; /* x * y; y.kind == A_ONE for a single arg */

static int is_const(const TevArg* a) { return a->kind <= A_CONST; }

static float cval(const TevArg* a, int i) {
    return a->kind == A_ZERO ? 0.0f : a->kind == A_ONE ? 1.0f : a->v[i];
}

static TevArg mk_const(const float* v, int n) {
    TevArg a = { A_CONST };
    int zero = 1, one = 1;
    for (int i = 0; i < n; i++) {
        a.v[i] = v[i];
        zero &= v[i] == 0.0f;
        one &= v[i] == 1.0f;
    }
    if (zero) a.kind = A_ZERO;
    else if (one) a.kind = A_ONE;
    return a;
}

static TevArg mk_kind(int kind, int alpha) {
    TevArg a = { (u8)kind, (u8)alpha };
    return a;
}

static int arg_eq(const TevArg* a, const TevArg* b, int n) {
    if (is_const(a) && is_const(b)) {
        for (int i = 0; i < n; i++)
            if (cval(a, i) != cval(b, i)) return 0;
        return 1;
    }
    return a->kind == b->kind && a->alpha == b->alpha && a->inv == b->inv;
}

static TevArg arg_inv(TevArg a, int n) {
    if (is_const(&a)) {
        float v[3];
        for (int i = 0; i < n; i++) v[i] = 1.0f - cval(&a, i);
        return mk_const(v, n);
    }
    a.inv ^= 1;
    return a;
}

/* Appends x*y to t[] with constant folding; returns new count */
static int add_term(Term* t, int nt, TevArg x, TevArg y, int n) {
    if (x.kind == A_ZERO || y.kind == A_ZERO) return nt;
    if (x.kind == A_ONE) { TevArg s = x; x = y; y = s; }
    if (is_const(&x) && is_const(&y)) {
        float v[3];
        for (int i = 0; i < n; i++) v[i] = cval(&x, i) * cval(&y, i);
        x = mk_const(v, n);
        y = mk_kind(A_ONE, 0);
        if (x.kind == A_ZERO) return nt;
    } else if (is_const(&x)) {
        TevArg s = x; x = y; y = s; /* constant goes second */
    }
    /* merge constant singles */
    if (y.kind == A_ONE && is_const(&x)) {
        for (int i = 0; i < nt; i++) {
            if (t[i].y.kind == A_ONE && is_const(&t[i].x)) {
                float v[3];
                for (int c = 0; c < n; c++) v[c] = cval(&t[i].x, c) + cval(&x, c);
                t[i].x = mk_const(v, n);
                return nt;
            }
        }
    }
    t[nt].x = x;
    t[nt].y = y;
    return nt + 1;
}

/* Distinct constant values in one op must be <= 1 (one constant per PICA stage) */
static int op_fits(const TevArg* a, int na, int n) {
    const TevArg* k = NULL;
    for (int i = 0; i < na; i++) {
        if (!is_const(&a[i])) continue;
        if (k && !arg_eq(k, &a[i], n)) return 0;
        k = &a[i];
    }
    return 1;
}

static void set_op(TevOp* op, int func, int na, TevArg a, TevArg b, TevArg c) {
    op->func = (u8)func;
    op->n = (u8)na;
    op->src[0] = a;
    op->src[1] = b;
    op->src[2] = c;
    op->scale = GPU_TEVSCALE_1;
}

/* Emits sum of terms; returns op count */
static int emit_sum(TevOp* ops, Term* t, int nt, int n) {
    TevArg sub = mk_kind(A_SUB, 0), one = mk_kind(A_ONE, 0);
    if (nt == 0) {
        set_op(&ops[0], GPU_REPLACE, 1, mk_kind(A_ZERO, 0), one, one);
        return 1;
    }
    if (nt == 2) {
        Term* p = &t[0];
        Term* q = &t[1];
        if (p->y.kind == A_ONE) { Term* s = p; p = q; q = s; }
        if (q->y.kind == A_ONE) {
            if (p->y.kind == A_ONE) { /* x + y */
                TevArg a[2] = { p->x, q->x };
                if (op_fits(a, 2, n)) {
                    set_op(&ops[0], GPU_ADD, 2, p->x, q->x, one);
                    return 1;
                }
            } else { /* x*y + z */
                TevArg a[3] = { p->x, p->y, q->x };
                if (op_fits(a, 3, n)) {
                    set_op(&ops[0], GPU_MULTIPLY_ADD, 3, p->x, p->y, q->x);
                    return 1;
                }
            }
        } else {
            /* x*c + z*(1-c), with c on either side of each product */
            for (int i = 0; i < 4; i++) {
                TevArg px = i & 1 ? p->y : p->x, c = i & 1 ? p->x : p->y;
                TevArg qx = i & 2 ? q->y : q->x, qc = i & 2 ? q->x : q->y;
                TevArg inv_c = arg_inv(c, n);
                TevArg a[3] = { px, qx, c };
                if (arg_eq(&inv_c, &qc, n) && op_fits(a, 3, n)) {
                    set_op(&ops[0], GPU_INTERPOLATE, 3, px, qx, c);
                    return 1;
                }
            }
        }
    }
    /* sequential: term0, then accumulate */
    int no = 0;
    for (int i = 0; i < nt; i++) {
        int single = t[i].y.kind == A_ONE;
        if (i == 0) set_op(&ops[no], single ? GPU_REPLACE : GPU_MODULATE, single ? 1 : 2, t[i].x, t[i].y, one);
        else if (single) set_op(&ops[no], GPU_ADD, 2, t[i].x, sub, one);
        else set_op(&ops[no], GPU_MULTIPLY_ADD, 3, t[i].x, t[i].y, sub);
        no++;
    }
    return no;
}

/* One channel of one GX stage: d (+/-) lerp(a, b, c). Returns op count (<= 4). */
static int compile_channel(TevOp* ops, TevArg a, TevArg b, TevArg c, TevArg d, int sub, int n) {
    Term t[4];
    int nt = 0;
    if (arg_eq(&a, &b, n)) {
        nt = add_term(t, nt, a, mk_kind(A_ONE, 0), n);
    } else {
        nt = add_term(t, nt, b, c, n);
        nt = add_term(t, nt, a, arg_inv(c, n), n);
    }
    if (!sub) {
        nt = add_term(t, nt, d, mk_kind(A_ONE, 0), n);
        return emit_sum(ops, t, nt, n);
    }
    /* d - L */
    if (d.kind == A_ZERO || nt == 0) {
        Term dt[1];
        int nd = d.kind == A_ZERO ? 0 : add_term(dt, 0, d, mk_kind(A_ONE, 0), n);
        return emit_sum(ops, dt, nd, n);
    }
    if (nt == 1 && t[0].y.kind == A_ONE) {
        TevArg s[2] = { d, t[0].x };
        if (op_fits(s, 2, n)) {
            set_op(&ops[0], GPU_SUBTRACT, 2, d, t[0].x, mk_kind(A_ONE, 0));
            return 1;
        }
    }
    int no = emit_sum(ops, t, nt, n);
    set_op(&ops[no], GPU_SUBTRACT, 2, d, mk_kind(A_SUB, 0), mk_kind(A_ONE, 0));
    return no + 1;
}

static void konst_color(int sel, float* out) {
    if (sel <= 7) {
        out[0] = out[1] = out[2] = (8 - sel) / 8.0f;
    } else if (sel <= 11) {
        out[0] = out[1] = out[2] = 0.0f;
    } else if (sel <= 15) {
        memcpy(out, g_gx.tev_k_colors[sel - 12], 3 * sizeof(float));
    } else {
        float v = g_gx.tev_k_colors[(sel - 16) & 3][((sel - 16) >> 2) & 3];
        out[0] = out[1] = out[2] = v;
    }
}

static float konst_alpha(int sel) {
    if (sel <= 7) return (8 - sel) / 8.0f;
    if (sel <= 15) return 0.0f;
    return g_gx.tev_k_colors[(sel - 16) & 3][((sel - 16) >> 2) & 3];
}

/* GXTevColorArg -> TevArg */
static TevArg color_arg(int id, int stage, int has_tex) {
    float v[3];
    const float* r;
    switch (id) {
        case 0: case 1: /* CPREV / APREV: before stage 0, PREV is the register value */
            if (stage == 0) {
                r = g_gx.tev_colors[0];
                v[0] = v[1] = v[2] = r[3];
                return mk_const(id == 0 ? r : v, 3);
            }
            return mk_kind(A_PREV, id == 1);
        case 2: case 3: case 4: case 5: case 6: case 7:
            r = g_gx.tev_colors[1 + (id - 2) / 2];
            if (id & 1) {
                v[0] = v[1] = v[2] = r[3];
                return mk_const(v, 3);
            }
            return mk_const(r, 3);
        case 8: case 9: return has_tex ? mk_kind(A_TEX, id == 9) : mk_kind(A_ONE, 0);
        case 10: case 11: return mk_kind(A_RAS, id == 11);
        case 12: return mk_kind(A_ONE, 0);
        case 13: v[0] = v[1] = v[2] = 0.5f; return mk_const(v, 3);
        case 14: konst_color(g_gx.tev_stages[stage].k_color_sel, v); return mk_const(v, 3);
        default: return mk_kind(A_ZERO, 0);
    }
}

/* GXTevAlphaArg -> TevArg */
static TevArg alpha_arg(int id, int stage, int has_tex) {
    float v;
    switch (id) {
        case 0:
            if (stage == 0) return mk_const(&g_gx.tev_colors[0][3], 1);
            return mk_kind(A_PREV, 1);
        case 1: case 2: case 3: return mk_const(&g_gx.tev_colors[id][3], 1);
        case 4: return has_tex ? mk_kind(A_TEX, 1) : mk_kind(A_ONE, 0);
        case 5: return mk_kind(A_RAS, 1);
        case 6: v = konst_alpha(g_gx.tev_stages[stage].k_alpha_sel); return mk_const(&v, 1);
        default: return mk_kind(A_ZERO, 0);
    }
}

static u8 to_u8(float f) {
    if (f <= 0.0f) return 0;
    if (f >= 1.0f) return 255;
    return (u8)(f * 255.0f + 0.5f);
}

/* Sources and operands of one channel op into a C3D_TexEnv */
static void apply_op(C3D_TexEnv* env, const TevOp* op, int is_alpha, int first, int stage,
                     float* kc, int* need_buf) {
    int src[3] = { GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR };
    int opr[3] = { 0, 0, 0 };
    for (int i = 0; i < op->n; i++) {
        const TevArg* a = &op->src[i];
        switch (a->kind) {
            case A_PREV:
                src[i] = first ? GPU_PREVIOUS : GPU_PREVIOUS_BUFFER;
                if (!first) *need_buf = 1;
                break;
            case A_SUB: src[i] = GPU_PREVIOUS; break;
            case A_TEX: src[i] = GPU_TEXTURE0 + stage; break;
            case A_RAS: src[i] = GPU_PRIMARY_COLOR; break;
            default: /* constant */
                src[i] = GPU_CONSTANT;
                if (is_alpha) kc[3] = cval(a, 0);
                else for (int c = 0; c < 3; c++) kc[c] = cval(a, c);
                break;
        }
        if (is_alpha) opr[i] = a->inv ? GPU_TEVOP_A_ONE_MINUS_SRC_ALPHA : GPU_TEVOP_A_SRC_ALPHA;
        else opr[i] = (a->alpha ? GPU_TEVOP_RGB_SRC_ALPHA : GPU_TEVOP_RGB_SRC_COLOR) + a->inv;
    }
    C3D_TexEnvMode mode = is_alpha ? C3D_Alpha : C3D_RGB;
    C3D_TexEnvSrc(env, mode, (GPU_TEVSRC)src[0], (GPU_TEVSRC)src[1], (GPU_TEVSRC)src[2]);
    if (is_alpha) C3D_TexEnvOpAlpha(env, (GPU_TEVOP_A)opr[0], (GPU_TEVOP_A)opr[1], (GPU_TEVOP_A)opr[2]);
    else C3D_TexEnvOpRgb(env, (GPU_TEVOP_RGB)opr[0], (GPU_TEVOP_RGB)opr[1], (GPU_TEVOP_RGB)opr[2]);
    C3D_TexEnvFunc(env, mode, (GPU_COMBINEFUNC)op->func);
    C3D_TexEnvScale(env, mode, (GPU_TEVSCALE)op->scale);
}

static GPU_TEVSCALE gx_scale(int s) {
    static int warned;
    if (s == 1) return GPU_TEVSCALE_2;
    if (s == 2) return GPU_TEVSCALE_4;
    if (s == 3) warn_once(&warned, "TEV scale 1/2 not supported");
    return GPU_TEVSCALE_1;
}

static void n3ds_tev_build(void) {
    static int warn_stages, warn_reg, warn_bias, warn_buf;
    int ns = g_gx.num_tev_stages;
    if (ns > PC_GX_MAX_TEV_STAGES) ns = PC_GX_MAX_TEV_STAGES;
    int pica = 0, buf_mask = 0;
    TevArg pass = mk_kind(A_SUB, 0), one = mk_kind(A_ONE, 0);

    for (int s = 0; s < ns; s++) {
        const PCGXTevStage* ts = &g_gx.tev_stages[s];
        int map = ts->tex_map;
        int has_tex = !(s_dbg & DBG_NOTEX) && n3ds_gl_tex((map >= 0 && map < 8) ? g_gx.gl_textures[map] : 0) != NULL;
        TevOp cops[5], aops[5];

        if (ts->color_out != 0 || ts->alpha_out != 0)
            warn_once(&warn_reg, "TEV stage writes REG0-2; treated as PREV");
        if (ts->color_bias != 0 || ts->alpha_bias != 0)
            warn_once(&warn_bias, "TEV bias not supported");

        int nc = compile_channel(cops,
            color_arg(ts->color_a, s, has_tex), color_arg(ts->color_b, s, has_tex),
            color_arg(ts->color_c, s, has_tex), color_arg(ts->color_d, s, has_tex),
            ts->color_op == 1, 3);
        int na = compile_channel(aops,
            alpha_arg(ts->alpha_a, s, has_tex), alpha_arg(ts->alpha_b, s, has_tex),
            alpha_arg(ts->alpha_c, s, has_tex), alpha_arg(ts->alpha_d, s, has_tex),
            ts->alpha_op == 1, 1);
        cops[nc - 1].scale = gx_scale(ts->color_scale);
        aops[na - 1].scale = gx_scale(ts->alpha_scale);

        int nsub = nc > na ? nc : na;
        if (pica + nsub > 6) {
            warn_once(&warn_stages, "TEV config needs more than 6 PICA stages; truncated");
            break;
        }
        int need_buf = 0;
        for (int k = 0; k < nsub; k++) {
            C3D_TexEnv* env = C3D_GetTexEnv(pica + k);
            float kc[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
            TevOp pass_op;
            set_op(&pass_op, GPU_REPLACE, 1, pass, one, one);
            C3D_TexEnvInit(env);
            apply_op(env, k < nc ? &cops[k] : &pass_op, 0, k == 0, s, kc, &need_buf);
            apply_op(env, k < na ? &aops[k] : &pass_op, 1, k == 0, s, kc, &need_buf);
            C3D_TexEnvColor(env, to_u8(kc[0]) | (to_u8(kc[1]) << 8) | (to_u8(kc[2]) << 16) |
                                 ((u32)to_u8(kc[3]) << 24));
        }
        /* GX PREV for this stage's later sub-stages comes from the combiner buffer,
         * which holds the last sub-stage of the previous GX stage */
        if (need_buf) {
            if (pica - 1 < 0 || pica - 1 >= 4) warn_once(&warn_buf, "TEV needs combiner buffer past stage 3");
            else buf_mask |= 1 << (pica - 1);
        }
        pica += nsub;
    }

    if (pica == 0) { /* no stages: output PREV register */
        C3D_TexEnv* env = C3D_GetTexEnv(0);
        const float* p = g_gx.tev_colors[0];
        C3D_TexEnvInit(env);
        C3D_TexEnvSrc(env, C3D_Both, GPU_CONSTANT, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
        C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
        C3D_TexEnvColor(env, to_u8(p[0]) | (to_u8(p[1]) << 8) | (to_u8(p[2]) << 16) | ((u32)to_u8(p[3]) << 24));
        pica = 1;
    }
    if (s_dbg & DBG_TEXONLY) { /* stage 0 texture (or vertex color) only */
        C3D_TexEnv* env = C3D_GetTexEnv(0);
        int map = ns > 0 ? g_gx.tev_stages[0].tex_map : -1;
        int tex = n3ds_gl_tex((map >= 0 && map < 8) ? g_gx.gl_textures[map] : 0) != NULL;
        C3D_TexEnvInit(env);
        C3D_TexEnvSrc(env, C3D_Both, tex ? GPU_TEXTURE0 : GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
        C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
        pica = 1;
    }
    if (s_dbg & DBG_LOGTEV) log_tev(ns, pica);
    for (int i = pica; i < 6; i++) C3D_TexEnvInit(C3D_GetTexEnv(i));
    C3D_TexEnvBufUpdate(C3D_Both, buf_mask);
    g_n3ds_tev_state.pica = (u8)pica;
}

/* --- textures, alpha test --- */



/* C3D_TexBind(unit > 0, NULL) reads the NULL texture's type (a data abort on hardware;
 * Azahar returns 0). Units 1 and 2 get this 8x8 white texture instead. */
static C3D_Tex s_white;

/* What each unit has now. C3D_TexBind always marks the unit dirty, and citro3d then sends the
 * unit's whole texture setup with the next draw; most draws change only unit 0. The data pointer
 * catches a texture uploaded again into the same C3D_Tex. */
static struct { const C3D_Tex* tex; const void* data; u32 param; float sel0; } s_unit[3];

static void unit_bind(int u, C3D_Tex* tex) {
    const void* data = tex ? tex->data : NULL;
    u32 param = tex ? tex->param : 0; /* wrap and filter: citro3d reads them only for a dirty unit */
    if (s_unit[u].tex == tex && s_unit[u].data == data && s_unit[u].param == param) return;
    C3D_TexBind(u, tex);
    s_unit[u].tex = tex;
    s_unit[u].data = data;
    s_unit[u].param = param;
}

/* Called before deleted textures are freed: a unit still bound to a freed texture would read freed
 * memory on the next draw. Unit 0 takes NULL, units 1 and 2 the white texture. The next texture
 * flush binds the real textures again. */
void n3ds_tex_units_detach(void) {
    if (!s_white.data && C3D_TexInit(&s_white, 8, 8, GPU_L8)) {
        memset(s_white.data, 0xFF, 64);
        C3D_TexFlush(&s_white);
    }
    unit_bind(0, NULL);
    for (int s = 1; s < 3; s++) unit_bind(s, s_white.data ? &s_white : NULL);
}

static void n3ds_bind_textures(void) {
    int ns = g_gx.num_tev_stages;
    if (!s_white.data && C3D_TexInit(&s_white, 8, 8, GPU_L8)) {
        memset(s_white.data, 0xFF, 64);
        C3D_TexFlush(&s_white);
    }
    for (int s = 0; s < 3; s++) {
        C3D_Tex* tex = NULL;
        float sel0 = 1.0f, sel1 = 0.0f;
        if (s < ns) {
            int map = g_gx.tev_stages[s].tex_map;
            tex = n3ds_gl_tex((map >= 0 && map < 8) ? g_gx.gl_textures[map] : 0);
            if (pc_gx_tc_src_normalize(g_gx.tev_stages[s].tex_coord, s) == 1) {
                sel0 = 0.0f;
                sel1 = 1.0f;
            }
        }
        unit_bind(s, tex || s == 0 ? tex : &s_white);
        g_n3ds_tev_state.fmt[s] = tex ? (u8)tex->fmt : 0xFF;
        g_n3ds_tev_state.w[s] = tex ? tex->width : 0;
        g_n3ds_tev_state.h[s] = tex ? tex->height : 0;
        if (s_unit[s].sel0 != sel0 + 1.0f) { /* stored +1: 0 = never set */
            C3D_FVUnifSet(GPU_VERTEX_SHADER, s_u.unitsel + s, sel0, sel1, 0.0f, 0.0f);
            s_unit[s].sel0 = sel0 + 1.0f;
        }
    }
}

static GPU_TESTFUNC gx_compare(int c) {
    static const GPU_TESTFUNC map[8] = {
        GPU_NEVER, GPU_LESS, GPU_EQUAL, GPU_LEQUAL, GPU_GREATER, GPU_NOTEQUAL, GPU_GEQUAL, GPU_ALWAYS
    };
    return map[c & 7];
}

static void n3ds_alpha_test(void) {
    static int warned;
    int c0 = g_gx.alpha_comp0, c1 = g_gx.alpha_comp1, op = g_gx.alpha_op;
    int r0 = g_gx.alpha_ref0, r1 = g_gx.alpha_ref1;
    g_n3ds_tev_state.alpha = !((c0 == 7 && c1 == 7) || (op == 1 && (c0 == 7 || c1 == 7)));
    if (c0 == 7 && c1 == 7) { C3D_AlphaTest(false, GPU_ALWAYS, 0); return; }
    if (op == 1 && (c0 == 7 || c1 == 7)) { C3D_AlphaTest(false, GPU_ALWAYS, 0); return; } /* OR ALWAYS */
    if (op == 0 && c0 == 7) { C3D_AlphaTest(true, gx_compare(c1), r1); return; }
    if (!(op == 0 && c1 == 7)) warn_once(&warned, "two-sided alpha compare approximated by comp0");
    C3D_AlphaTest(true, gx_compare(c0), r0);
}

/* --- vertex uniforms --- */

static void set_rows(int loc, const float* m, int rows, int stride) {
    for (int r = 0; r < rows; r++)
        C3D_FVUnifSet(GPU_VERTEX_SHADER, loc + r, m[r * stride + 0], m[r * stride + 1],
                      m[r * stride + 2], stride == 4 ? m[r * stride + 3] : 0.0f);
}

static void n3ds_projection(void) {
    float (*p)[4] = g_gx.projection_mtx;
    /* 3DS screens are rotated: clip x' = y, y' = -x (as citro3d's Mtx_*Tilt) */
    C3D_FVUnifSet(GPU_VERTEX_SHADER, s_u.proj + 0, p[1][0], p[1][1], p[1][2], p[1][3]);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, s_u.proj + 1, -p[0][0], -p[0][1], -p[0][2], -p[0][3]);
    /* Ortho: GL-style z [-1,1] -> PICA [-1,0]. emu64 draws 2D (speech text) with z outside the
     * GX range; GL clips at [-w,w] so the PC build draws it, the PICA clips at [-w,0]. Order is kept. */
    if (g_gx.projection_type == 1) {
        C3D_FVUnifSet(GPU_VERTEX_SHADER, s_u.proj + 2, 0.5f * p[2][0], 0.5f * p[2][1], 0.5f * p[2][2],
                      0.5f * p[2][3] - 0.5f);
        set_rows(s_u.proj + 3, &p[3][0], 1, 4);
    } else {
        set_rows(s_u.proj + 2, &p[2][0], 2, 4);
    }
}

/* GX tex matrix id -> slot: raw 0..9, GX_TEXMTX0(30)..(57) stride 3, GX_IDENTITY(60) = none */
static int tex_mtx_slot(int id) {
    if (id >= 0 && id < 10) return id;
    if (id >= 30 && id < 60) return (id - 30) / 3;
    return -1;
}

/* Texgens (bit 0, bit 1) whose stage samples a screen capture (n3ds_gl_capture_screen) */
static int s_rot_mask;
static int rot_mask(void) {
    extern int n3ds_gl_tex_rot(GLuint name);
    int mask = 0;
    for (int s = 0; s < g_gx.num_tev_stages && s < 3; s++) {
        int map = g_gx.tev_stages[s].tex_map;
        if (map >= 0 && map < 8 && n3ds_gl_tex_rot(g_gx.gl_textures[map]))
            mask |= 1 << (pc_gx_tc_src_normalize(g_gx.tev_stages[s].tex_coord, s) == 1);
    }
    return mask;
}

static void n3ds_texgen(void) {
    static const float ident[8] = { 1, 0, 0, 0, 0, 1, 0, 0 };
    s_rot_mask = rot_mask();
    for (int tg = 0; tg < 2; tg++) {
        int slot = tex_mtx_slot(g_gx.tex_gen_mtx[tg]);
        const float* rows = slot >= 0 ? &g_gx.tex_mtx[slot][0][0] : ident;
        if (s_rot_mask & (1 << tg)) {
            /* Capture texture: (s, t) of the upright screen image -> texel of the rotated target.
             * u = (1 - t) * 240/256 (column = screen y, up), v = 1 - s * 400/512 (row = screen x;
             * PICA t = 0 is the last row in memory). The input w is 1, so column 3 is the offset. */
            const float a = 240.0f / 256.0f, c = 400.0f / 512.0f;
            float r[8];
            for (int i = 0; i < 4; i++) {
                r[i] = -a * rows[4 + i];
                r[4 + i] = -c * rows[i];
            }
            r[3] += a;
            r[7] += 1.0f;
            set_rows(tg ? s_u.texmtx1 : s_u.texmtx0, r, 2, 4);
            continue;
        }
        set_rows(tg ? s_u.texmtx1 : s_u.texmtx0, rows, 2, 4);
    }
    /* GX_TG_NRM = 1 */
    C3D_FVUnifSet(GPU_VERTEX_SHADER, s_u.tgsrc, g_gx.tex_gen_src[0] == 1 ? 1.0f : 0.0f,
                  g_gx.tex_gen_src[1] == 1 ? 1.0f : 0.0f, 0.0f, 0.0f);
}

static void n3ds_lighting(void) {
    const float* mat = g_gx.chan_mat_color[0];
    const float* amb = g_gx.chan_amb_color[0];
    int lit = g_gx.num_chans > 0 && g_gx.chan_ctrl_enable[0] && !(s_dbg & DBG_NOLIGHT);
    float ms = g_gx.chan_ctrl_mat_src[0] ? 1.0f : 0.0f;
    float mas = g_gx.chan_ctrl_mat_src[1] ? 1.0f : 0.0f;
    float as = g_gx.chan_ctrl_amb_src[0] ? 1.0f : 0.0f;
    float k = g_gx.chan_ctrl_enable[1] ? amb[3] : 1.0f;

    if (g_gx.num_chans == 0) {
        C3D_FVUnifSet(GPU_VERTEX_SHADER, s_u.matReg, 1.0f, 1.0f, 1.0f, 1.0f);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, s_u.matSel, 0.0f, 0.0f, 0.0f, 0.0f);
        k = 1.0f;
    } else {
        C3D_FVUnifSet(GPU_VERTEX_SHADER, s_u.matReg, mat[0], mat[1], mat[2], mat[3]);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, s_u.matSel, ms, ms, ms, mas);
    }
    C3D_FVUnifSet(GPU_VERTEX_SHADER, s_u.ambReg, amb[0], amb[1], amb[2], k);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, s_u.ambSel, as, as, as, 0.0f);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, s_u.unlitAcc, 1.0f, 1.0f, 1.0f, k);
    C3D_BoolUnifSet(GPU_VERTEX_SHADER, s_u.lit, lit);
    g_n3ds_tev_state.lit = (u8)lit;
    if (!lit) return;

    int n = 0;
    for (int i = 0; i < 8; i++) {
        if (!(g_gx.chan_ctrl_light_mask[0] & (1 << i))) continue;
        const float* p = g_gx.lights[i].pos;
        float len = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
        float inv = len > 0.0f ? 1.0f / len : 0.0f;
        const float* c = g_gx.lights[i].color;
        C3D_FVUnifSet(GPU_VERTEX_SHADER, s_u.lightDir + n, p[0] * inv, p[1] * inv, p[2] * inv, 0.0f);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, s_u.lightCol + n, c[0], c[1], c[2], 0.0f);
        n++;
    }
    if (n == 0) { /* the loop runs at least once */
        C3D_FVUnifSet(GPU_VERTEX_SHADER, s_u.lightCol, 0.0f, 0.0f, 0.0f, 0.0f);
        n = 1;
    }
    C3D_IVUnifSet(GPU_VERTEX_SHADER, s_u.lightLoop, n - 1, 0, 1, 0);
}

/* Linear GX fog on eye distance, through the PICA fog LUT (indexed by depth).
 * ponytail: assumes viewport depth range 0..1 (depth = z_ndc + 1); pass the range in if AC uses another. */
static void n3ds_fog(void) {
    g_n3ds_tev_state.fog = !(g_gx.fog_type == 0 || (s_dbg & DBG_NOFOG));
    if (!g_n3ds_tev_state.fog) {
        C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
        return;
    }
    const float* fc = g_gx.fog_color;
    float (*p)[4] = g_gx.projection_mtx;
    int persp = g_gx.projection_type == 0; /* GX_PERSPECTIVE */
    float range = g_gx.fog_end - g_gx.fog_start;
    if (range < 1e-6f) range = 1e-6f;
    float data[256];
    for (int i = 0; i <= 128; i++) {
        float z = i / 128.0f - 1.0f;
        float dist = persp ? p[2][3] / (z + p[2][2]) : (p[2][3] - z) / p[2][2];
        float f = (dist - g_gx.fog_start) / range;
        float vis = 1.0f - (f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f);
        if (i < 128) data[i] = vis;
        if (i > 0) data[i + 127] = vis - data[i - 1];
    }
    FogLut_FromArray(&s_fog_lut, data);
    C3D_FogGasMode(GPU_FOG, GPU_PLAIN_DENSITY, false);
    C3D_FogColor(to_u8(fc[0]) | (to_u8(fc[1]) << 8) | (to_u8(fc[2]) << 16));
    C3D_FogLutBind(&s_fog_lut);
}

/* The batch palette (pc_gx.c): n pairs of position 3x4 and normal 3x3, one pair per slot. Slot k uses
 * shader rows 3k..3k+2. Called right before the batch draws. */
/* Copy of what the GPU palette holds. Only entries that differ are written (every draw calls this). */
static float s_up_pos[9][12], s_up_nrm[9][9];
static int s_up_n; /* entries written since the cache was cleared */
void n3ds_gx_upload_palette(const float* pos, const float* nrm, int n) {
    for (int k = 0; k < n; k++) {
        const float* p = pos + 12 * k;
        const float* m = nrm + 9 * k;
        if (k < s_up_n && !memcmp(s_up_pos[k], p, sizeof(s_up_pos[k])) && !memcmp(s_up_nrm[k], m, sizeof(s_up_nrm[k])))
            continue;
        set_rows(s_u.pmv + 3 * k, p, 3, 4);
        set_rows(s_u.pnrm + 3 * k, m, 3, 3);
        memcpy(s_up_pos[k], p, sizeof(s_up_pos[k]));
        memcpy(s_up_nrm[k], m, sizeof(s_up_nrm[k]));
    }
    if (n > s_up_n) s_up_n = n;
}

/* Called from pc_gx_flush_vertices with the dirty groups. MODELVIEW is not here: the palette carries it. */
void n3ds_gx_upload(unsigned int dirty) {
    if (dirty & PC_GX_DIRTY_PROJECTION) n3ds_projection();
    /* a texture change can start or stop a capture draw: its texcoords live in the texgen matrix */
    if ((dirty & PC_GX_DIRTY_TEXGEN) ||
        ((dirty & (PC_GX_DIRTY_TEXTURES | PC_GX_DIRTY_TEV_STAGES)) && rot_mask() != s_rot_mask))
        n3ds_texgen();
    if (dirty & PC_GX_DIRTY_LIGHTING) n3ds_lighting();
    if (dirty & (PC_GX_DIRTY_FOG | PC_GX_DIRTY_PROJECTION)) n3ds_fog();
    if (dirty & (PC_GX_DIRTY_TEV_STAGES | PC_GX_DIRTY_TEV_COLORS | PC_GX_DIRTY_KONST |
                 PC_GX_DIRTY_TEXTURES)) {
        /* The combiners depend on which stages have a texture, not on which texture: a texture
         * change alone keeps them. A rebuild re-inits every TexEnv, and citro3d then re-sends them all. */
        static int s_built_mask = -1;
        int mask = 0;
        for (int s = 0; s < g_gx.num_tev_stages && s < PC_GX_MAX_TEV_STAGES; s++) {
            int map = g_gx.tev_stages[s].tex_map;
            if (n3ds_gl_tex((map >= 0 && map < 8) ? g_gx.gl_textures[map] : 0)) mask |= 1 << s;
        }
        if ((dirty & (PC_GX_DIRTY_TEV_STAGES | PC_GX_DIRTY_TEV_COLORS | PC_GX_DIRTY_KONST)) || mask != s_built_mask) {
            n3ds_tev_build();
            s_built_mask = mask;
        }
        n3ds_bind_textures();
    }
    if (dirty & PC_GX_DIRTY_ALPHA_CMP) n3ds_alpha_test();
}

/* The game shader and its vertex layout (n3ds_gl.c N3DSVtx); the self-test binds others */
void n3ds_tev_bind_main(void) {
    C3D_BindProgram(&s_prog);
    C3D_AttrInfo* ai = C3D_GetAttrInfo();
    AttrInfo_Init(ai);
    AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 3);         /* position */
    AttrInfo_AddLoader(ai, 1, GPU_FLOAT, 3);         /* normal */
    AttrInfo_AddLoader(ai, 2, GPU_UNSIGNED_BYTE, 4); /* color0 */
    AttrInfo_AddLoader(ai, 3, GPU_FLOAT, 2);         /* texcoord0 */
    AttrInfo_AddLoader(ai, 4, GPU_FLOAT, 1);         /* batch palette offset */
}

/* GPU self-test (n3ds_gl.c): identity transforms (vertex position = clip position),
 * vertex color as material, texgen 0 to every unit, and nlights white lights (0 = unlit). */
void n3ds_tev_test_uniforms(int nlights) {
    static const float id[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    set_rows(s_u.proj, id, 4, 4);
    s_up_n = 0; /* the self-test overwrites the palette: forget what the GPU holds */
    set_rows(s_u.pmv, id, 3, 4); /* palette slot 0 = identity; self-test vertices use index 0 */
    set_rows(s_u.pnrm, id, 3, 4);
    set_rows(s_u.texmtx0, id, 2, 4);
    set_rows(s_u.texmtx1, id, 2, 4);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, s_u.tgsrc, 0, 0, 0, 0);
    for (int u = 0; u < 3; u++) C3D_FVUnifSet(GPU_VERTEX_SHADER, s_u.unitsel + u, 1, 0, 0, 0);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, s_u.matReg, 1, 1, 1, 1);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, s_u.matSel, 1, 1, 1, 1);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, s_u.ambReg, 0.2f, 0.2f, 0.2f, 1);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, s_u.ambSel, 0, 0, 0, 0);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, s_u.unlitAcc, 1, 1, 1, 1);
    for (int i = 0; i < 8; i++) {
        C3D_FVUnifSet(GPU_VERTEX_SHADER, s_u.lightDir + i, 0, 0, 1, 0);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, s_u.lightCol + i, 1, 1, 1, 0);
    }
    C3D_IVUnifSet(GPU_VERTEX_SHADER, s_u.lightLoop, nlights > 0 ? nlights - 1 : 0, 0, 1, 0);
    C3D_BoolUnifSet(GPU_VERTEX_SHADER, s_u.lit, nlights > 0);
    C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
}

/* Fog with a ramp LUT (self-test only) */
void n3ds_tev_test_fog(int on) {
    if (!on) {
        C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
        return;
    }
    float data[256];
    for (int i = 0; i <= 128; i++) {
        float v = 1.0f - i / 128.0f;
        if (i < 128) data[i] = v;
        if (i > 0) data[i + 127] = v - data[i - 1];
    }
    FogLut_FromArray(&s_fog_lut, data);
    C3D_FogGasMode(GPU_FOG, GPU_PLAIN_DENSITY, false);
    C3D_FogColor(0x808080);
    C3D_FogLutBind(&s_fog_lut);
}

/* --- pc_gx_tev.c interface --- */

void pc_gx_tev_init(void) {
    read_debug_switches();
    s_dvlb = DVLB_ParseFile((u32*)gx_shader_shbin, gx_shader_shbin_size);
    shaderProgramInit(&s_prog);
    shaderProgramSetVsh(&s_prog, &s_dvlb->DVLE[0]);
    C3D_BindProgram(&s_prog);

    shaderInstance_s* vs = s_prog.vertexShader;
#define LOC(n) s_u.n = shaderInstanceGetUniformLocation(vs, #n)
    LOC(proj); LOC(pmv); LOC(pnrm); LOC(texmtx0); LOC(texmtx1); LOC(tgsrc); LOC(unitsel);
    LOC(matReg); LOC(matSel); LOC(ambReg); LOC(ambSel); LOC(unlitAcc);
    LOC(lightDir); LOC(lightCol); LOC(lightLoop); LOC(lit);
#undef LOC

    n3ds_tev_bind_main();

    memset(&s_variant, 0, sizeof(s_variant));
    s_variant.used = 1;
    s_variant.prog = N3DS_GX_PROG;
    memset(s_variant.uploaded_seq, 0xFF, sizeof(s_variant.uploaded_seq));

    /* before pc_gx_init sets its GL state and marks all GX state dirty */
    if (s_dbg & DBG_GPUTEST) {
        extern void n3ds_gl_gputest(void);
        n3ds_gl_gputest();
    }
    if (s_dbg & DBG_PROFILE) { /* pc_profiler.c [PROFILE] lines */
        extern int g_pc_profile_enabled;
        g_pc_profile_enabled = 1;
    }
    if (s_dbg & DBG_LOCKTEST) { /* the watchdog must log "[LOCK] ... held" */
        extern void n3ds_log_locktest(void);
        n3ds_log_locktest();
    }
}

void pc_gx_tev_shutdown(void) {
    shaderProgramFree(&s_prog);
    DVLB_Free(s_dvlb);
}

PCGXShaderVariant* pc_gx_tev_get_variant(void) { return &s_variant; }

void pc_gx_tev_seq_reset(void) {
    memset(s_variant.uploaded_seq, 0xFF, sizeof(s_variant.uploaded_seq));
    memset(g_gx.group_seq, 0, sizeof(g_gx.group_seq));
}
