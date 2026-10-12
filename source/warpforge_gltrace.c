/* warpforge_gltrace.c -- what textures does the engine really upload?
 *
 * The first practice match filled the 512 MB GPU arena (478 MB live) and then
 * `memalign FAILED size=21848 KB` three times: 21848 KB is exactly a 2048x2048
 * 32-bit texture with mipmaps. Card art was missing. On the phone those cards
 * are compressed (ASTC/ETC2, ~1.5 MB each); 21 MB each means either the engine
 * decided this GL has no compressed-texture support and inflated them on the
 * CPU, or the bundle really holds RGBA32. The two are told apart by the GL
 * calls: glCompressedTex* with an ASTC/ETC internal format, or glTexStorage2D /
 * glTexImage2D with RGBA8 at 2048x2048. The engine resolves these entry points
 * through dlsym (libc_shim.c), so this file hands back logging wrappers for them
 * and logs the first large ones.
 *
 * MIT.
 */
#include <stdint.h>
#include <string.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>
#include "util.h"

static unsigned g_logged;
#define TRACE_MAX 40
#define BIG 1024

static const char *fmt_name(GLenum f) {
  switch (f) {
    case GL_RGBA8: return "RGBA8";
    case GL_RGB8: return "RGB8";
    case GL_RGBA: return "RGBA";
    case GL_RGB: return "RGB";
    case GL_SRGB8_ALPHA8: return "SRGB8_ALPHA8";
    case GL_RGBA16F: return "RGBA16F";
    case GL_R8: return "R8";
    case GL_RG8: return "RG8";
    case GL_RGB565: return "RGB565";
    case GL_RGBA4: return "RGBA4";
    case GL_RGB5_A1: return "RGB5_A1";
    case GL_DEPTH24_STENCIL8: return "D24S8";
    case GL_DEPTH_COMPONENT24: return "D24";
    case GL_COMPRESSED_RGB8_ETC2: return "ETC2_RGB8";
    case GL_COMPRESSED_RGBA8_ETC2_EAC: return "ETC2_RGBA8";
    case GL_COMPRESSED_SRGB8_ALPHA8_ETC2_EAC: return "ETC2_SRGBA8";
    case GL_COMPRESSED_RGBA_ASTC_4x4_KHR: return "ASTC_4x4";
    case GL_COMPRESSED_RGBA_ASTC_5x5_KHR: return "ASTC_5x5";
    case GL_COMPRESSED_RGBA_ASTC_6x6_KHR: return "ASTC_6x6";
    case GL_COMPRESSED_RGBA_ASTC_8x8_KHR: return "ASTC_8x8";
    case GL_COMPRESSED_RGBA_ASTC_10x10_KHR: return "ASTC_10x10";
    case GL_COMPRESSED_RGBA_ASTC_12x12_KHR: return "ASTC_12x12";
    case GL_COMPRESSED_SRGB8_ALPHA8_ASTC_4x4_KHR: return "ASTC_SRGB_4x4";
    case GL_COMPRESSED_SRGB8_ALPHA8_ASTC_6x6_KHR: return "ASTC_SRGB_6x6";
    case GL_COMPRESSED_SRGB8_ALPHA8_ASTC_8x8_KHR: return "ASTC_SRGB_8x8";
    case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT: return "DXT5";
    case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT: return "DXT1a";
    case GL_COMPRESSED_RGB_S3TC_DXT1_EXT: return "DXT1";
    default: return NULL;
  }
}

static void once_extensions(void) {
  static int done; if (done) return; done = 1;
  const char *ext = (const char *)glGetString(GL_EXTENSIONS);
  if (!ext) { debugPrintf("[gltrace] GL_EXTENSIONS unavailable\n"); return; }
  debugPrintf("[gltrace] compressed formats offered: astc_ldr=%d astc_hdr=%d etc2(core GLES3) s3tc=%d bptc=%d rgtc=%d\n",
              strstr(ext, "KHR_texture_compression_astc_ldr") != NULL,
              strstr(ext, "KHR_texture_compression_astc_hdr") != NULL,
              strstr(ext, "texture_compression_s3tc") != NULL,
              strstr(ext, "texture_compression_bptc") != NULL,
              strstr(ext, "texture_compression_rgtc") != NULL);
}

static void note(const char *call, GLenum ifmt, GLsizei w, GLsizei h, GLsizei depth, GLsizei levels, GLsizei bytes) {
  if (w < BIG && h < BIG) return;
  if (g_logged >= TRACE_MAX) return;
  g_logged++;
  once_extensions();
  const char *n = fmt_name(ifmt);
  if (n) debugPrintf("[gltrace] %s %s %dx%d%s%d levels=%d bytes=%d\n", call, n, (int)w, (int)h, depth > 1 ? "x" : "", depth > 1 ? (int)depth : 0, (int)levels, (int)bytes);
  else   debugPrintf("[gltrace] %s 0x%04x %dx%d%s%d levels=%d bytes=%d\n", call, (unsigned)ifmt, (int)w, (int)h, depth > 1 ? "x" : "", depth > 1 ? (int)depth : 0, (int)levels, (int)bytes);
  if (g_logged == TRACE_MAX) debugPrintf("[gltrace] (quiet from here)\n");
}

/* The real entry points, looked up once. */
static void *real(const char *name) { return (void *)eglGetProcAddress(name); }

static void GL_APIENTRY t_glTexStorage2D(GLenum target, GLsizei levels, GLenum ifmt, GLsizei w, GLsizei h) {
  static void (GL_APIENTRY *r)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
  if (!r) r = real("glTexStorage2D");
  note("glTexStorage2D", ifmt, w, h, 1, levels, 0);
  r(target, levels, ifmt, w, h);
}
static void GL_APIENTRY t_glTexStorage3D(GLenum target, GLsizei levels, GLenum ifmt, GLsizei w, GLsizei h, GLsizei d) {
  static void (GL_APIENTRY *r)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLsizei);
  if (!r) r = real("glTexStorage3D");
  note("glTexStorage3D", ifmt, w, h, d, levels, 0);
  r(target, levels, ifmt, w, h, d);
}
void GL_APIENTRY wf_t_glTexImage2D(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border, GLenum fmt, GLenum type, const void *px) {
  static void (GL_APIENTRY *r)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
  if (!r) r = real("glTexImage2D");
  if (level == 0) note("glTexImage2D", (GLenum)ifmt, w, h, 1, 1, 0);
  r(target, level, ifmt, w, h, border, fmt, type, px);
}
void GL_APIENTRY wf_t_glCompressedTexImage2D(GLenum target, GLint level, GLenum ifmt, GLsizei w, GLsizei h, GLint border, GLsizei size, const void *data) {
  static void (GL_APIENTRY *r)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void *);
  if (!r) r = real("glCompressedTexImage2D");
  if (level == 0) note("glCompressedTexImage2D", ifmt, w, h, 1, 1, size);
  r(target, level, ifmt, w, h, border, size, data);
}
static void GL_APIENTRY t_glCompressedTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLsizei size, const void *data) {
  static void (GL_APIENTRY *r)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLsizei, const void *);
  if (!r) r = real("glCompressedTexSubImage2D");
  if (level == 0) note("glCompressedTexSubImage2D", fmt, w, h, 1, 1, size);
  r(target, level, x, y, w, h, fmt, size, data);
}
void GL_APIENTRY wf_t_glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type, const void *px) {
  static void (GL_APIENTRY *r)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *);
  if (!r) r = real("glTexSubImage2D");
  if (level == 0) note("glTexSubImage2D(upload)", fmt, w, h, 1, 1, 0);
  r(target, level, x, y, w, h, fmt, type, px);
}

/* dlsym(): a wrapper for the texture entry points, NULL for everything else. */
void *wf_gltrace_lookup(const char *symbol) {
  if (!symbol || symbol[0] != 'g' || symbol[1] != 'l') return NULL;
  if (!strcmp(symbol, "glTexStorage2D"))            return (void *)t_glTexStorage2D;
  if (!strcmp(symbol, "glTexStorage3D"))            return (void *)t_glTexStorage3D;
  if (!strcmp(symbol, "glTexImage2D"))              return (void *)wf_t_glTexImage2D;
  if (!strcmp(symbol, "glTexSubImage2D"))           return (void *)wf_t_glTexSubImage2D;
  if (!strcmp(symbol, "glCompressedTexImage2D"))    return (void *)wf_t_glCompressedTexImage2D;
  if (!strcmp(symbol, "glCompressedTexSubImage2D")) return (void *)t_glCompressedTexSubImage2D;
  return NULL;
}
