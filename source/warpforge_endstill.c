/* warpforge_endstill.c -- the VICTORY / DEFEAT / DRAW banner, as a still.
 *
 * At the end of a match EndBattleDoors.SetupDoor picks one of three VideoClips
 * (Addressables: videos_assets_all.bundle) and plays it through its VideoPlayer
 * into a CustomRenderTexture shown by a RawImage (field videoImage, 0x98). The
 * clips are VP8 with the alpha packed side by side; the port has no video
 * decoder (the engine's media API is stubbed), so the banner never appeared.
 *
 * Instead of decoding video (CPU + memory the console does not have in battle),
 * the clips' final frames were extracted once on a PC and stored on the card as
 *   <root>/videos/victory.rgba, defeat.rgba, draw.rgba
 * (8-byte header: uint32 width, uint32 height; then width*height RGBA, straight
 * alpha). After SetupDoor runs, this builds a Texture2D from the right one and
 * puts it on videoImage -- re-asserting it for a few seconds, because the game
 * may assign its render texture again. One texture at a time; the previous one
 * is destroyed, and Apply(false, true) drops the CPU copy.
 *
 * Everything is looked up by name through libil2cpp's reflection API; nothing
 * here runs unless every method resolves. Main thread only (SetupDoor and the
 * render loop's tick both run there).
 *
 * MIT.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "so_util.h"
#include "util.h"
#include "cloverpit_root.h"
#include "warpforge_resolve.h"

extern void *wf_detour_install(void *target, void *replacement);

typedef void *(*fn_domain_get)(void);
typedef void *(*fn_domain_assembly_open)(void *, const char *);
typedef void *(*fn_assembly_get_image)(void *);
typedef void *(*fn_class_from_name)(void *, const char *, const char *);
typedef void *(*fn_class_get_methods)(void *, void **);
typedef const char *(*fn_method_get_name)(void *);
typedef uint32_t (*fn_method_get_param_count)(void *);
typedef void *(*fn_method_get_param)(void *, uint32_t);
typedef char *(*fn_type_get_name)(void *);
typedef void *(*fn_object_new)(void *);
typedef uint32_t (*fn_gchandle_new)(void *, uint8_t);
typedef void (*fn_gchandle_free)(uint32_t);
typedef void (*fn_il2cpp_free)(void *);

static fn_domain_get p_domain_get; static fn_domain_assembly_open p_asm_open; static fn_assembly_get_image p_asm_image;
static fn_class_from_name p_class; static fn_class_get_methods p_methods; static fn_method_get_name p_mname;
static fn_method_get_param_count p_mpc; static fn_method_get_param p_mparam; static fn_type_get_name p_tname;
static fn_object_new p_object_new; static fn_gchandle_new p_gch_new; static fn_gchandle_free p_gch_free;
static fn_il2cpp_free p_il2cpp_free;   /* il2cpp_type_get_name's string comes from il2cpp's allocator */

/* methods (MethodInfo*) and their native entry points */
typedef struct { void *mi; void *fn; } meth_t;
static void *k_texture2d;
static meth_t m_tex_ctor, m_tex_load, m_tex_apply, m_raw_set_tex, m_raw_get_tex, m_comp_go, m_go_active, m_beh_enabled, m_destroy;
static float (*orig_setup_door)(void *, void *, int, int, void *, int, void *, void *);

static void *g_tex;            /* our current Texture2D */
static uint32_t g_tex_handle;  /* keeps it alive */
static void *g_image;          /* the RawImage it is on */
static uint32_t g_image_handle;
static int g_reassert;         /* frames left to keep it there */

static void *find_class(const char *assembly, const char *ns, const char *name) {
  void *a = p_asm_open(p_domain_get(), assembly);
  void *img = a ? p_asm_image(a) : NULL;
  return img ? p_class(img, ns, name) : NULL;
}
/* name + param count, and (optionally) the type names of the first params */
static int find_method(meth_t *out, void *klass, const char *name, int argc, const char *t0, const char *t1, const char *t2, const char *t3) {
  if (!klass) return 0;
  void *iter = NULL, *m;
  const char *want[4] = { t0, t1, t2, t3 };
  while ((m = p_methods(klass, &iter))) {
    if (strcmp(p_mname(m), name) || (int)p_mpc(m) != argc) continue;
    int ok = 1;
    for (int i = 0; i < argc && i < 4 && ok; i++) {
      if (!want[i]) continue;
      char *tn = p_tname(p_mparam(m, (uint32_t)i));
      if (!tn || strcmp(tn, want[i])) ok = 0;
      if (tn) p_il2cpp_free(tn);
    }
    if (!ok) continue;
    out->mi = m; out->fn = *(void **)m;
    return out->fn != NULL;
  }
  return 0;
}

static void *load_still(const char *which, uint32_t *w, uint32_t *h, size_t *bytes) {
  char path[512]; snprintf(path, sizeof path, "%s/videos/%s.rgba", cp_game_root(), which);
  FILE *f = fopen(path, "rb");
  if (!f) { debugPrintf("[endstill] no %s -- the banner stays blank\n", path); return NULL; }
  uint32_t hdr[2];
  void *buf = NULL;
  if (fread(hdr, 4, 2, f) == 2 && hdr[0] && hdr[1] && hdr[0] <= 4096 && hdr[1] <= 4096) {
    *bytes = (size_t)hdr[0] * hdr[1] * 4;
    buf = malloc(*bytes);
    if (buf && fread(buf, 1, *bytes, f) != *bytes) { free(buf); buf = NULL; }
    *w = hdr[0]; *h = hdr[1];
  }
  fclose(f);
  if (!buf) debugPrintf("[endstill] %s is not a valid still\n", path);
  return buf;
}

static void apply_to_image(void) {
  void *(*get_tex)(void *, void *) = m_raw_get_tex.fn;
  void (*set_tex)(void *, void *, void *) = m_raw_set_tex.fn;
  if (g_image && g_tex && get_tex(g_image, m_raw_get_tex.mi) != g_tex) {
    set_tex(g_image, g_tex, m_raw_set_tex.mi);
    void (*set_enabled)(void *, uint8_t, void *) = m_beh_enabled.fn;
    set_enabled(g_image, 1, m_beh_enabled.mi);
    void *(*get_go)(void *, void *) = m_comp_go.fn;
    void (*set_active)(void *, uint8_t, void *) = m_go_active.fn;
    void *go = get_go(g_image, m_comp_go.mi);
    if (go) set_active(go, 1, m_go_active.mi);
  }
}

static float hk_setup_door(void *self, void *mode, int winner, int match_type, void *result, int skulls, void *warlord, void *mi) {
  float r = orig_setup_door(self, mode, winner, match_type, result, skulls, warlord, mi);
  const char *which = winner == 10 ? "victory" : winner == 20 ? "defeat" : winner == 30 ? "draw" : NULL;
  void *image = self ? *(void **)((uint8_t *)self + 0x98) : NULL;   /* EndBattleDoors.videoImage */
  debugPrintf("[endstill] SetupDoor winner=%d -> %s, image %p\n", winner, which ? which : "(none)", image);
  if (!which || !image) return r;

  uint32_t w = 0, h = 0; size_t bytes = 0;
  void *px = load_still(which, &w, &h, &bytes);
  if (!px) return r;

  /* drop the previous texture */
  if (g_tex) {
    void (*destroy)(void *, void *) = m_destroy.fn;
    destroy(g_tex, m_destroy.mi);
    p_gch_free(g_tex_handle); g_tex = NULL;
  }
  if (g_image) { p_gch_free(g_image_handle); g_image = NULL; }

  void *tex = p_object_new(k_texture2d);
  void (*ctor)(void *, int, int, int, uint8_t, void *) = m_tex_ctor.fn;
  ctor(tex, (int)w, (int)h, 4 /* RGBA32 */, 0 /* no mips */, m_tex_ctor.mi);
  void (*load)(void *, void *, int, void *) = m_tex_load.fn;
  load(tex, px, (int)bytes, m_tex_load.mi);
  void (*apply)(void *, uint8_t, uint8_t, void *) = m_tex_apply.fn;
  apply(tex, 0, 1 /* no longer readable: frees the CPU copy */, m_tex_apply.mi);
  free(px);

  g_tex = tex; g_tex_handle = p_gch_new(tex, 0);
  g_image = image; g_image_handle = p_gch_new(image, 0);
  apply_to_image();
  g_reassert = 300;   /* ~5 s: the game may put its own render texture back */
  debugPrintf("[endstill] %s banner shown (%ux%u)\n", which, w, h);
  return r;
}

/* Every frame from the render loop (main thread). */
void wf_endstill_tick(void) {
  if (g_reassert <= 0) return;
  g_reassert--;
  apply_to_image();
}

void wf_endstill_install(so_module *il2cpp) {
#define SYM(var, type, name) var = (type)so_try_find_addr_rx(il2cpp, name)
  SYM(p_domain_get, fn_domain_get, "il2cpp_domain_get");
  SYM(p_asm_open, fn_domain_assembly_open, "il2cpp_domain_assembly_open");
  SYM(p_asm_image, fn_assembly_get_image, "il2cpp_assembly_get_image");
  SYM(p_class, fn_class_from_name, "il2cpp_class_from_name");
  SYM(p_methods, fn_class_get_methods, "il2cpp_class_get_methods");
  SYM(p_mname, fn_method_get_name, "il2cpp_method_get_name");
  SYM(p_mpc, fn_method_get_param_count, "il2cpp_method_get_param_count");
  SYM(p_mparam, fn_method_get_param, "il2cpp_method_get_param");
  SYM(p_tname, fn_type_get_name, "il2cpp_type_get_name");
  SYM(p_object_new, fn_object_new, "il2cpp_object_new");
  SYM(p_gch_new, fn_gchandle_new, "il2cpp_gchandle_new");
  SYM(p_gch_free, fn_gchandle_free, "il2cpp_gchandle_free");
  SYM(p_il2cpp_free, fn_il2cpp_free, "il2cpp_free");
#undef SYM
  if (!p_domain_get || !p_asm_open || !p_asm_image || !p_class || !p_methods || !p_mname || !p_mpc ||
      !p_mparam || !p_tname || !p_object_new || !p_gch_new || !p_gch_free || !p_il2cpp_free) {
    debugPrintf("[endstill] il2cpp API incomplete -- end-screen stills off\n"); return;
  }
  void *k_doors = find_class("Assembly-CSharp.dll", "", "EndBattleDoors");
  k_texture2d   = find_class("UnityEngine.CoreModule.dll", "UnityEngine", "Texture2D");
  void *k_obj   = find_class("UnityEngine.CoreModule.dll", "UnityEngine", "Object");
  void *k_comp  = find_class("UnityEngine.CoreModule.dll", "UnityEngine", "Component");
  void *k_go    = find_class("UnityEngine.CoreModule.dll", "UnityEngine", "GameObject");
  void *k_beh   = find_class("UnityEngine.CoreModule.dll", "UnityEngine", "Behaviour");
  void *k_raw   = find_class("UnityEngine.UI.dll", "UnityEngine.UI", "RawImage");
  meth_t m_setup = {0};
  int ok = find_method(&m_setup, k_doors, "SetupDoor", 6, NULL, NULL, NULL, NULL)
        && find_method(&m_tex_ctor, k_texture2d, ".ctor", 4, "System.Int32", "System.Int32", "UnityEngine.TextureFormat", "System.Boolean")
        && find_method(&m_tex_load, k_texture2d, "LoadRawTextureData", 2, "System.IntPtr", "System.Int32", NULL, NULL)
        && find_method(&m_tex_apply, k_texture2d, "Apply", 2, "System.Boolean", "System.Boolean", NULL, NULL)
        && find_method(&m_raw_set_tex, k_raw, "set_texture", 1, NULL, NULL, NULL, NULL)
        && find_method(&m_raw_get_tex, k_raw, "get_texture", 0, NULL, NULL, NULL, NULL)
        && find_method(&m_comp_go, k_comp, "get_gameObject", 0, NULL, NULL, NULL, NULL)
        && find_method(&m_go_active, k_go, "SetActive", 1, NULL, NULL, NULL, NULL)
        && find_method(&m_beh_enabled, k_beh, "set_enabled", 1, NULL, NULL, NULL, NULL)
        && find_method(&m_destroy, k_obj, "Destroy", 1, "UnityEngine.Object", NULL, NULL, NULL);
  if (!ok) { debugPrintf("[endstill] a method did not resolve (doors %p, tex %p, raw %p) -- end-screen stills off\n", k_doors, k_texture2d, k_raw); return; }
  orig_setup_door = wf_detour_install(m_setup.fn, (void *)hk_setup_door);
  debugPrintf("[endstill] EndBattleDoors.SetupDoor %s\n", orig_setup_door ? "hooked -- end-screen banners from videos/*.rgba" : "could not be hooked");
}
