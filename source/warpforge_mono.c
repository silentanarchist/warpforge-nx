/* warpforge_mono.c -- host the Mono runtime (exelix11/mono-nx) inside the wrapper.
 *
 * MILESTONE 1: start the interpreter-only Mono runtime in this process, next to
 * the game, and run one managed assembly from the SD card before the game boots.
 * That proves the runtime, its BCL and its W^X handling on the real memory
 * budget. Nothing mod-related is attempted here yet.
 *
 * Memory: Mono's "fake mmap" needs one contiguous range handed over at heap-init
 * time (mono-nx steals half of newlib's heap for it). The wrapper owns
 * __libnx_initheap (main.c), so it carves WF_MONO_HEAP_MB off the top of the
 * newlib heap and passes it to mono_nx_fakemmap_init -- see wf_mono_heap_init.
 *
 * Files on the card (from mono-nx's sd_files.zip): /mono/config.ini,
 * /mono/lib_net9.0, /mono/framework_net9.0, /mono/etc/icudt77l.dat. The
 * assembly to run is WF_MONO_HELLO under the game folder.
 *
 * MIT.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>
#include <sys/stat.h>
#include "config.h"
#include "util.h"
#include "cloverpit_root.h"

#if WF_MONO
#include "core.h"          /* mono-nx shared host: g_config, application_* */

void mono_nx_fakemmap_init(intptr_t memory_start, intptr_t memory_end);

static intptr_t g_mono_lo, g_mono_hi;

/* mono-nx's core.c calls this (its heap.c defined it; the wrapper's main.c owns
 * __libnx_initheap instead). */
void heap_debug(void) {
  debugPrintf("[mono] fake-mmap range %p..%p (%u MB)\n", (void *)g_mono_lo, (void *)g_mono_hi,
              (unsigned)((g_mono_hi - g_mono_lo) >> 20));
}

void wf_mono_heap_init(uintptr_t lo, uintptr_t hi) {
  g_mono_lo = (intptr_t)lo;
  g_mono_hi = (intptr_t)hi;
  mono_nx_fakemmap_init(g_mono_lo, g_mono_hi);
}

static MonoDomain   *g_domain;
static MonoAssembly *g_hello;      /* mono_hello.dll, kept for the il2cpp-ready call */

/* Managed code logs through this (DllImport("warpforge")). */
void wf_mono_log(const char *line) { debugPrintf("[managed] %s\n", line ? line : "(null)"); }

int wf_mono_start(void) {
  debugPrintf("[mono] heap slice %p..%p (%u MB)\n", (void *)g_mono_lo, (void *)g_mono_hi,
              (unsigned)((g_mono_hi - g_mono_lo) >> 20));
  {
    struct stat st;
    if (stat(CONFIG_INI_PATH, &st) != 0) {
      debugPrintf("[mono] no %s on the card -- Mono not started (game runs without it)\n", CONFIG_INI_PATH);
      return -1;
    }
  }
  if (!application_initialize(CONFIG_INI_PATH)) {
    debugPrintf("[mono] application_initialize failed (is /mono/config.ini on the card?)\n");
    return -1;
  }
  mono_jit_set_aot_mode(MONO_AOT_MODE_INTERP_ONLY);
  application_configure_mono();
  g_domain = mono_jit_init("warpforge_nx");
  if (!g_domain) {
    debugPrintf("[mono] mono_jit_init failed\n");
    return -1;
  }
  debugPrintf("[mono] runtime up (interpreter only)\n");
  return 0;
}

int wf_mono_run_hello(void) {
  if (!g_domain) return -1;
  char path[640];
  snprintf(path, sizeof path, "%s/%s", cp_game_root(), WF_MONO_HELLO);
  debugPrintf("[mono] running %s\n", path);
  MonoAssembly *as = mono_domain_assembly_open(g_domain, path);
  if (!as) {
    debugPrintf("[mono] could not open %s\n", path);
    return -1;
  }
  g_hello = as;
  char *argv[] = { path };
  int rc = mono_jit_exec(g_domain, as, 1, argv);
  debugPrintf("[mono] %s returned %d\n", WF_MONO_HELLO, rc);
  return rc;
}

/* MILESTONE 2: called by warpforge_hooks.c on the first frame after il2cpp_init,
 * on the engine main thread. Invokes WarpforgeNx.Il2CppProbe.OnIl2CppReady()
 * in mono_hello.dll, which walks the game's IL2CPP domain through the il2cpp
 * C API (DllImport("il2cpp") -> libil2cpp.so via the dlopen shim). */
void wf_mono_on_il2cpp_ready(void) {
  if (!g_domain || !g_hello) return;
  MonoImage *img = mono_assembly_get_image(g_hello);
  MonoClass *k = mono_class_from_name(img, "WarpforgeNx", "Il2CppProbe");
  if (!k) { debugPrintf("[mono] WarpforgeNx.Il2CppProbe not in %s\n", WF_MONO_HELLO); return; }
  MonoMethod *m = mono_class_get_method_from_name(k, "OnIl2CppReady", 0);
  if (!m) { debugPrintf("[mono] Il2CppProbe.OnIl2CppReady not found\n"); return; }
  debugPrintf("[mono] il2cpp is up -- calling Il2CppProbe.OnIl2CppReady()\n");
  MonoObject *exc = NULL;
  MonoObject *ret = mono_runtime_invoke(m, NULL, NULL, &exc);
  if (exc) {
    debugPrintf("[mono] OnIl2CppReady threw %s\n", mono_class_get_name(mono_object_get_class(exc)));
    return;
  }
  debugPrintf("[mono] OnIl2CppReady returned %d\n", ret ? *(int *)mono_object_unbox(ret) : -1);
}
#else
void wf_mono_heap_init(uintptr_t lo, uintptr_t hi) { (void)lo; (void)hi; }
int  wf_mono_start(void)     { return -1; }
int  wf_mono_run_hello(void) { return -1; }
void wf_mono_on_il2cpp_ready(void) {}
#endif
