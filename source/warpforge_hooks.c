/* warpforge_hooks.c -- install the managed-side hooks once the IL2CPP runtime
 * exists.
 *
 * The CloverPit port installed its UnityEngine.Time / Input hooks from main(),
 * before boot, because it knew the addresses from a dump. Here the addresses
 * come from the runtime itself (warpforge_resolve.c), which only answers after
 * il2cpp_init -- inside the engine's first nativeRender. So the render loop
 * calls wf_hooks_tick() every frame and the installation happens on the first
 * frame where the domain is up. One attempt per target set; a target that
 * does not resolve is logged and left alone.
 *
 * Thread safety: at that moment the main thread is between frames in our own
 * loop and no managed code is running on it; worker threads do not call these
 * getters. hook_arm64 writes a single branch word and flushes the cache.
 *
 * MIT.
 */
#include <stdint.h>
#include "util.h"
#include "warpforge_resolve.h"
#include "cloverpit_time.h"
#include "cloverpit_touchhook.h"

static int g_done;

void wf_hooks_tick(void) {
  if (g_done || !wf_resolve_ready())
    return;
  g_done = 1;
  debugPrintf("[hooks] il2cpp domain is up -- installing managed hooks by name\n");
  cp_time_install(NULL);
  cp_touchhook_install(NULL);
}
