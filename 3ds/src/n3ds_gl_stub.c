/* n3ds_gl_stub.c - no-op OpenGL entry points for the 3DS build.
 *
 * pc_gx*.c still issue GL calls through glad's function pointers. Here every
 * pointer the port uses targets a stub, so the GX state tracking runs but
 * nothing draws. The citro3d backend replaces these draw paths.
 * Getters return values that keep the PC code on its success paths.
 */
#include <glad/gl.h>
#include <string.h>

static void gl_noop(void) {}

static GLuint next_name = 1;
static void GLAD_API_PTR gl_gen(GLsizei n, GLuint* out) {
    for (GLsizei i = 0; i < n; i++) out[i] = next_name++;
}
static GLuint GLAD_API_PTR gl_create(void) { return next_name++; }
static GLuint GLAD_API_PTR gl_create_shader(GLenum type) { (void)type; return next_name++; }
static GLenum GLAD_API_PTR gl_get_error(void) { return GL_NO_ERROR; }
static void GLAD_API_PTR gl_get_iv(GLuint obj, GLenum pname, GLint* out) {
    (void)obj;
    *out = (pname == GL_INFO_LOG_LENGTH) ? 0 : GL_TRUE;
}
static void GLAD_API_PTR gl_get_info_log(GLuint obj, GLsizei max, GLsizei* len, GLchar* log) {
    (void)obj;
    if (len) *len = 0;
    if (log && max > 0) log[0] = '\0';
}
static void GLAD_API_PTR gl_get_integerv(GLenum pname, GLint* out) { (void)pname; *out = 0; }
static const GLubyte* GLAD_API_PTR gl_get_stringi(GLenum name, GLuint i) { (void)name; (void)i; return (const GLubyte*)""; }
static GLint GLAD_API_PTR gl_get_uniform_location(GLuint prog, const GLchar* name) { (void)prog; (void)name; return 0; }
static void GLAD_API_PTR gl_read_pixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type, void* px) {
    (void)x; (void)y; (void)fmt; (void)type;
    memset(px, 0, (size_t)w * (size_t)h * 4);
}

#define NOOP(name, type) type glad_##name = (type)gl_noop;
#define STUB(name, type, fn) type glad_##name = fn;

NOOP(glActiveTexture, PFNGLACTIVETEXTUREPROC)
NOOP(glAttachShader, PFNGLATTACHSHADERPROC)
NOOP(glBindAttribLocation, PFNGLBINDATTRIBLOCATIONPROC)
NOOP(glBindBuffer, PFNGLBINDBUFFERPROC)
NOOP(glBindTexture, PFNGLBINDTEXTUREPROC)
NOOP(glBindVertexArray, PFNGLBINDVERTEXARRAYPROC)
NOOP(glBlendEquation, PFNGLBLENDEQUATIONPROC)
NOOP(glBlendFunc, PFNGLBLENDFUNCPROC)
NOOP(glBufferData, PFNGLBUFFERDATAPROC)
NOOP(glClear, PFNGLCLEARPROC)
NOOP(glClearColor, PFNGLCLEARCOLORPROC)
NOOP(glClearDepth, PFNGLCLEARDEPTHPROC)
NOOP(glColorMask, PFNGLCOLORMASKPROC)
NOOP(glCompileShader, PFNGLCOMPILESHADERPROC)
NOOP(glCompressedTexImage2D, PFNGLCOMPRESSEDTEXIMAGE2DPROC)
STUB(glCreateProgram, PFNGLCREATEPROGRAMPROC, gl_create)
STUB(glCreateShader, PFNGLCREATESHADERPROC, gl_create_shader)
NOOP(glCullFace, PFNGLCULLFACEPROC)
NOOP(glDeleteBuffers, PFNGLDELETEBUFFERSPROC)
NOOP(glDeleteProgram, PFNGLDELETEPROGRAMPROC)
NOOP(glDeleteShader, PFNGLDELETESHADERPROC)
NOOP(glDeleteTextures, PFNGLDELETETEXTURESPROC)
NOOP(glDeleteVertexArrays, PFNGLDELETEVERTEXARRAYSPROC)
NOOP(glDepthFunc, PFNGLDEPTHFUNCPROC)
NOOP(glDepthMask, PFNGLDEPTHMASKPROC)
NOOP(glDepthRange, PFNGLDEPTHRANGEPROC)
NOOP(glDisable, PFNGLDISABLEPROC)
NOOP(glDrawArrays, PFNGLDRAWARRAYSPROC)
NOOP(glDrawElements, PFNGLDRAWELEMENTSPROC)
NOOP(glEnable, PFNGLENABLEPROC)
NOOP(glEnableVertexAttribArray, PFNGLENABLEVERTEXATTRIBARRAYPROC)
NOOP(glFlush, PFNGLFLUSHPROC)
STUB(glGenBuffers, PFNGLGENBUFFERSPROC, gl_gen)
STUB(glGenTextures, PFNGLGENTEXTURESPROC, gl_gen)
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
NOOP(glScissor, PFNGLSCISSORPROC)
NOOP(glShaderSource, PFNGLSHADERSOURCEPROC)
NOOP(glTexImage2D, PFNGLTEXIMAGE2DPROC)
NOOP(glTexParameteri, PFNGLTEXPARAMETERIPROC)
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
NOOP(glUseProgram, PFNGLUSEPROGRAMPROC)
NOOP(glVertexAttribPointer, PFNGLVERTEXATTRIBPOINTERPROC)
NOOP(glViewport, PFNGLVIEWPORTPROC)

int gladLoadGL(GLADloadfunc load) { (void)load; return 1; }
void* SDL_GL_GetProcAddress(const char* proc) { (void)proc; return (void*)gl_noop; }
