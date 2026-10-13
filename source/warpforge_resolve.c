/* warpforge_resolve.c -- find managed methods in libil2cpp BY NAME, at run time.
 *
 * WHY NOT RVAs FROM A DUMP
 * The CloverPit port carries hard-coded RVAs (from Il2CppDumper) for every
 * UnityEngine.Time / Input method it hooks, each guarded by its first opcode
 * word. That is safe but it ties the port to one build of the game, and for
 * Warpforge there was no dump to take them from.
 *
 * libil2cpp exports its own reflection API. Once the runtime has initialised
 * (il2cpp_init runs inside initJni, on the first nativeRender), the domain
 * exists and the engine's own assemblies can be opened and walked:
 *
 *   il2cpp_domain_assembly_open(domain, "UnityEngine.CoreModule.dll")
 *     -> il2cpp_assembly_get_image -> il2cpp_class_from_name("UnityEngine","Time")
 *     -> il2cpp_class_get_method_from_name(klass, "get_deltaTime", 0)
 *     -> MethodInfo, whose first field is the native method pointer.
 *
 * So the hooks that depend on these addresses are installed from the render
 * loop (warpforge_hooks.c), on the first frame where the domain is non-null,
 * rather than from main() before boot. A method that cannot be resolved is
 * skipped and logged; nothing is guessed.
 *
 * The MethodInfo layout assumption (methodPointer at offset 0) holds for every
 * IL2CPP version Unity has shipped; it is additionally sanity-checked by
 * requiring the pointer to fall inside libil2cpp's executable image.
 *
 * MIT.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "so_util.h"
#include "util.h"
#include "warpforge_resolve.h"

typedef void *(*fn_domain_get)(void);
typedef void *(*fn_domain_assembly_open)(void *domain, const char *name);
typedef void *(*fn_assembly_get_image)(void *assembly);
typedef void *(*fn_class_from_name)(void *image, const char *ns, const char *name);
typedef void *(*fn_class_get_method_from_name)(void *klass, const char *name, int argc);

static fn_domain_get                  p_domain_get;
static fn_domain_assembly_open        p_domain_assembly_open;
static fn_assembly_get_image          p_assembly_get_image;
static fn_class_from_name             p_class_from_name;
static fn_class_get_method_from_name  p_class_get_method_from_name;
static so_module *g_il2cpp;

int wf_resolve_bind(so_module *il2cpp) {
  g_il2cpp = il2cpp;
  p_domain_get = (fn_domain_get)so_try_find_addr_rx(il2cpp, "il2cpp_domain_get");
  p_domain_assembly_open =
      (fn_domain_assembly_open)so_try_find_addr_rx(il2cpp, "il2cpp_domain_assembly_open");
  p_assembly_get_image =
      (fn_assembly_get_image)so_try_find_addr_rx(il2cpp, "il2cpp_assembly_get_image");
  p_class_from_name = (fn_class_from_name)so_try_find_addr_rx(il2cpp, "il2cpp_class_from_name");
  p_class_get_method_from_name =
      (fn_class_get_method_from_name)so_try_find_addr_rx(il2cpp, "il2cpp_class_get_method_from_name");
  int ok = p_domain_get && p_domain_assembly_open && p_assembly_get_image &&
           p_class_from_name && p_class_get_method_from_name;
  debugPrintf("[resolve] il2cpp reflection API %s\n", ok ? "bound" : "MISSING -- name hooks unavailable");
  return ok ? 0 : -1;
}

/* il2cpp_init / il2cpp_init_utf16 are reached by libunity through dlsym; the
 * fake dlsym hands back these wrappers, which call the real export and then
 * raise the flag. NEVER probe the runtime for readiness instead: on the first
 * hardware run, calling il2cpp_domain_get() before il2cpp_init crashed the
 * process in frame 0 -- the domain is created lazily, not reported absent. */
static volatile int g_runtime_up;

static int wf_il2cpp_init_wrap(const char *domain_name) {
  typedef int (*fn)(const char *);
  fn real = (fn)so_resolve_external("il2cpp_init");
  debugPrintf("[resolve] il2cpp_init(%s) -> real %p\n", domain_name ? domain_name : "(null)", (void *)real);
  int r = real ? real(domain_name) : 0;
  g_runtime_up = 1;
  debugPrintf("[resolve] il2cpp_init returned %d -- runtime is up\n", r);
  return r;
}

static int wf_il2cpp_init_utf16_wrap(const void *domain_name) {
  typedef int (*fn)(const void *);
  fn real = (fn)so_resolve_external("il2cpp_init_utf16");
  debugPrintf("[resolve] il2cpp_init_utf16 -> real %p\n", (void *)real);
  int r = real ? real(domain_name) : 0;
  g_runtime_up = 1;
  debugPrintf("[resolve] il2cpp_init_utf16 returned %d -- runtime is up\n", r);
  return r;
}

void *wf_dlsym_interpose(const char *symbol) {
  if (!symbol) return NULL;
  if (!strcmp(symbol, "il2cpp_init"))       return (void *)wf_il2cpp_init_wrap;
  if (!strcmp(symbol, "il2cpp_init_utf16")) return (void *)wf_il2cpp_init_utf16_wrap;
  return NULL;
}

so_module *wf_resolve_module(void) { return g_il2cpp; }

int wf_resolve_ready(void) {
  return g_runtime_up && p_domain_get != NULL;
}

void *wf_resolve_method(const char *assembly, const char *ns, const char *klass,
                        const char *method, int argc) {
  if (!wf_resolve_ready())
    return NULL;
  void *asm_ = p_domain_assembly_open(p_domain_get(), assembly);
  if (!asm_) {
    debugPrintf("[resolve] assembly %s not found\n", assembly);
    return NULL;
  }
  void *img = p_assembly_get_image(asm_);
  void *k = img ? p_class_from_name(img, ns, klass) : NULL;
  if (!k) {
    debugPrintf("[resolve] class %s.%s not found in %s\n", ns, klass, assembly);
    return NULL;
  }
  void *mi = p_class_get_method_from_name(k, method, argc);
  if (!mi) {
    debugPrintf("[resolve] method %s.%s::%s/%d not found\n", ns, klass, method, argc);
    return NULL;
  }
  uintptr_t fn = *(uintptr_t *)mi;   /* MethodInfo.methodPointer */
  uintptr_t lo = (uintptr_t)g_il2cpp->load_virtbase, hi = lo + g_il2cpp->load_size;
  if (fn < lo || fn >= hi) {
    debugPrintf("[resolve] %s::%s pointer %p outside libil2cpp -- refused\n", klass, method, (void *)fn);
    return NULL;
  }
  return (void *)fn;
}
