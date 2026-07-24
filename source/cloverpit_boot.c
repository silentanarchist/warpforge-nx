/* cloverpit_boot.c -- Unity 6 boot lifecycle and render loop.
 *
 * Replaces the work the Java UnityPlayer normally does. The sequence below is
 * NOT the one badpiggies_nx / ZookeeperDX_NX use; Unity 6 changed it in two ways
 * that are silent failures rather than compile errors, so they are called out at
 * their call sites:
 *
 *   - initJni takes FOUR arguments here: (env, thiz, Context, int). 2020.3 and
 *     2022.3 take three. Calling the 3-arg form leaves the fourth parameter
 *     register holding garbage.
 *   - nativeUnityPlayerSetRunning(bool) exists and gates the frame loop. It does
 *     not exist at all in the older generations, so neither reference port calls
 *     it, and without it nativeRender does nothing useful.
 *
 * Natives are resolved BY NAME from the RegisterNatives capture rather than by
 * address. The RVAs in cloverpit_entrypoints.h are used only to cross-check the
 * name lookup and to produce a legible warning when a game update moves things.
 *
 * MIT.
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <switch.h>

#include "so_util.h"
#include "util.h"
#include "jni_fake.h"
#include "cloverpit_entrypoints.h"
#include "cloverpit_locate.h"
#include "cloverpit_offsets.h"
#include "cloverpit_jni.h"
#include "cloverpit_time.h"
#include "android_native_unity.h"
#include "diag.h"
#include "config.h"

/* Tripwire -- see the note in cloverpit_il2cpp.c. A feature flag whose header is
 * not included evaluates to 0 and silently removes the feature, with no warning
 * and no log line. That has now happened twice in this port; it does not get to
 * happen a third time. */
#ifndef CP_DISABLE_IL2CPP_GC
#error "config.h not included -- feature flags in this file would compile out silently"
#endif

extern so_module unity_mod, il2cpp_mod;
extern void *fake_env, *fake_vm;
extern void *fake_unityplayer_thiz, *fake_context_obj, *fake_surface_obj;
extern volatile int jni_quit_requested;

/* Input: implemented in the pointer/HID layer, called once per frame. Should
 * synthesise Android InputEvent jobjects and feed Unity_nativeInjectEvent. */
extern void cp_pump_input(void *env, void *thiz, fn_inject inject);

/* main.c owns these: the frame counter the crash handler reports, and the SD
 * commit. Both were per-frame duties in the reference render loop and have to
 * stay per-frame now that the loop lives here. */
extern void nx_sd_flush(void);
extern void port_frame_tick(void);
extern unsigned cp_gpua_peak_mb(void);
extern unsigned cp_gpua_live_mb(void);

/* cloverpit_patches.c captures the TimeManager instance the first time the
 * engine calls TimeManager::Update, and exposes the engine time we last fed it.
 * Reporting both is what distinguishes "frames are not running" from "frames run
 * but engine time is frozen" -- the two produce identical symptoms on screen (a
 * loading spinner that stops) but have completely different causes. */
extern void  *cp_clock_tm(void);
extern double cp_clock_last_time(void);

/* ---------------------------------------------------------------- natives */

static struct {
  fn_initJni_u6 initJni;
  fn_gfxstate   recreateGfxState;
  fn_v          sendSurfaceChanged;
  fn_vz         setRunning;          /* Unity 6 only */
  fn_z          render;
  fn_v          resume;
  fn_z          pause;
  fn_vz         focusChanged;
  fn_inject     injectEvent;
  fn_v          applicationUnload;
  fn_z          done;
} U;

/* Resolve one native by name, then sanity-check it against the RVA recovered
 * from this build's RegisterNatives table. A mismatch is not fatal -- the name
 * lookup is authoritative and the RVA is the stale half -- but it is the single
 * clearest signal in the log that the game has been updated. */
static void *resolve_native(const char *name, uint32_t expect_rva, bool required) {
  void *fn = jni_lookup_unity_native(name);
  if (!fn) {
    debugPrintf("[boot] native %-30s MISSING%s\n", name,
                required ? "  <-- FATAL" : " (optional)");
    return NULL;
  }
  uintptr_t rva = (uintptr_t)fn - (uintptr_t)unity_mod.load_virtbase;
  if (expect_rva && rva != expect_rva)
    debugPrintf("[boot] native %-30s @0x%06x (expected 0x%06x -- game updated?)\n",
                name, (unsigned)rva, expect_rva);
  else
    debugPrintf("[boot] native %-30s @0x%06x\n", name, (unsigned)rva);
  return fn;
}

#define RESOLVE(field, type, nm, req)                                          \
  U.field = (type)resolve_native(#nm, OFF_##nm, req)

static int resolve_all(void) {
  memset(&U, 0, sizeof(U));

  RESOLVE(initJni,            fn_initJni_u6, initJni,                       true);
  RESOLVE(recreateGfxState,   fn_gfxstate,   nativeRecreateGfxState,        true);
  RESOLVE(sendSurfaceChanged, fn_v,          nativeSendSurfaceChangedEvent, true);
  RESOLVE(setRunning,         fn_vz,         nativeUnityPlayerSetRunning,   true);
  RESOLVE(render,             fn_z,          nativeRender,                  true);
  RESOLVE(resume,             fn_v,          nativeResume,                  true);
  RESOLVE(done,               fn_z,          nativeDone,                    true);

  RESOLVE(pause,              fn_z,          nativePause,                   false);
  RESOLVE(focusChanged,       fn_vz,         nativeFocusChanged,            false);
  RESOLVE(injectEvent,        fn_inject,     nativeInjectEvent,             false);
  RESOLVE(applicationUnload,  fn_v,          nativeApplicationUnload,       false);

  if (!U.initJni || !U.recreateGfxState || !U.sendSurfaceChanged ||
      !U.setRunning || !U.render || !U.resume || !U.done) {
    debugPrintf("[boot] FATAL: a required UnityPlayer native did not register\n");
    return -1;
  }
  return 0;
}
#undef RESOLVE

/* -------------------------------------------------------------------- GC */

/* IL2CPP's collector stops the world with POSIX signals that Switch never
 * delivers, so a collection triggered mid-frame hangs the process instead of
 * pausing it. Disable it before the first render, exactly as the reference
 * ports do. The cost is that managed memory is never reclaimed -- acceptable
 * for a session-length game, and the alternative is a hang. */
static void disable_gc(void) {
  void (*gc_set_mode)(int) =
      (void (*)(int))so_try_find_addr_rx(&il2cpp_mod, "il2cpp_gc_set_mode");
  void (*gc_disable)(void) =
      (void (*)(void))so_try_find_addr_rx(&il2cpp_mod, "il2cpp_gc_disable");

#if CP_DISABLE_IL2CPP_GC
  if (gc_set_mode) gc_set_mode(1);       /* 1 == manual */
  if (gc_disable)  gc_disable();
  debugPrintf("[boot] il2cpp GC DISABLED (set_mode=%s disable=%s) -- "
              "UnloadUnusedAssets will stall; see CP_DISABLE_IL2CPP_GC\n",
              gc_set_mode ? "ok" : "absent", gc_disable ? "ok" : "absent");
#else
  (void)gc_set_mode; (void)gc_disable;
  debugPrintf("[boot] il2cpp GC LEFT ENABLED -- stop-the-world handled by the "
              "pthread_kill bridge in libc_shim.c\n");
#endif
}

/* ------------------------------------------------------------------ frames */

static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

#define TARGET_FRAME_NS 16666667ull   /* 60 Hz */

/* --------------------------------------------------------------- lifecycle */

int cp_boot_and_run(void) {
  if (resolve_all() < 0)
    return -1;

  /* Re-assert the main thread's stack-guard TLS immediately before the first
   * engine call. resolve_all() above only touched our own code, but the
   * reference re-asserts at exactly this point and the cost is a memset. */
  cp_reassert_main_tls();

  /* Sanity: initJni will fault on a null Context, and these three are `= 0` in
   * unity_glue.c until unity_environment_init() runs. Catch a missing call here
   * with a clear message rather than inside the engine. */
  if (!fake_unityplayer_thiz || !fake_context_obj || !fake_surface_obj) {
    debugPrintf("[boot] FATAL: fake objects NULL (thiz=%p ctx=%p surface=%p) -- "
                "unity_environment_init() did not run\n",
                fake_unityplayer_thiz, fake_context_obj, fake_surface_obj);
    return -1;
  }

  /* 1. initJni. FOUR arguments in Unity 6 -- see the file header. The trailing
   *    int is the API-level-ish hint the Java side passes; 0 is accepted. */
  debugPrintf("[boot] initJni(env, thiz, ctx, 0)\n");
  U.initJni(fake_env, fake_unityplayer_thiz, fake_context_obj, 0);

  /* 2. Hand the engine its surface, then let it build GL state. */
  debugPrintf("[boot] nativeRecreateGfxState + nativeSendSurfaceChangedEvent\n");
  U.recreateGfxState(fake_env, fake_unityplayer_thiz, 0, fake_surface_obj);
  U.sendSurfaceChanged(fake_env, fake_unityplayer_thiz);

  /* 3. GC off BEFORE the first frame. Doing it after means racing the first
   *    allocation-triggered collection. */
  disable_gc();

  /* 4. Unity 6 gate. Without this nativeRender returns immediately and the
   *    screen stays black with no error anywhere. */
  debugPrintf("[boot] nativeUnityPlayerSetRunning(true)\n");
  U.setRunning(fake_env, fake_unityplayer_thiz, 1);

  U.resume(fake_env, fake_unityplayer_thiz);
  if (U.focusChanged)
    U.focusChanged(fake_env, fake_unityplayer_thiz, 1);

  debugPrintf("[boot] entering render loop\n");

  /* Two counters on purpose: `frame_no` is monotonic and is what diag_frame
   * records, so a crash log reports the true frame the engine died on.
   * `frames` is a 5-second window used only for the fps average. Passing the
   * windowed one to diag_frame would make crash reports claim frame 47 when the
   * game had actually run for twenty thousand. */
  /* Drop FastLoad once the engine is actually rendering: it raises CPU clock at
   * the expense of GPU, which inverts the right trade-off after load, and it
   * runs the console hot. 30 consecutive frames under 33 ms == steady state. */
  int fast_frames = 0, boosted = 1;
  uint64_t frame_no = 0;
  uint64_t frames = 0, last_log = now_ns();
  while (appletMainLoop() && !jni_quit_requested) {
    uint64_t frame_start = now_ns();

    diag_frame((int)frame_no);
    port_frame_tick();

    /* Re-sizes the NWindow when the console is docked or undocked. The reference
     * calls this every frame rather than off the applet hook, because the hook
     * does not fire for operation-mode changes. */
    android_native_update_mode();

    /* Samples deltaTime for the managed Time hooks. Must be once per frame. */
    cp_time_tick();

    if (U.injectEvent)
      cp_pump_input(fake_env, fake_unityplayer_thiz, U.injectEvent);

    /* Android's vibrate(ms) implies a stop; the Switch's rumble is level-
     * triggered and runs until cleared, so expire it here. */
    cp_jni_rumble_tick();

    /* nativeRender returns false when the engine wants out (quit, fatal gfx
     * error). Treat that as authoritative rather than looping on a dead
     * engine. */
    /* Time the engine call itself. At 0.4 fps the open question is whether the
     * cost is inside nativeRender (asset streaming, GL, allocator) or in our own
     * per-frame work; this separates them without guesswork. */
    /* Log BEFORE the call for the first frames. Everything so far has only
     * logged after nativeRender returned, so a frame that never returns is
     * indistinguishable from a frame that was never started -- and the whole
     * system going silent at once (render loop AND the independent watchdog
     * thread, which was alive 2s earlier) points at a hard freeze rather than a
     * software wait. This line says whether frame N was entered at all. */
    /* Markers for the first 40 frames, then every 60th. The 8-frame window was
     * enough to prove the freeze was inside the present; now that frames flow,
     * the question is how far the game gets, so the trace has to outlive the
     * load phase without flooding the log forever. */
    int mark = (frame_no < 40) || (frame_no % 60 == 0);
    /* Frame 26 is where the console dies, reproducibly, on every build since the
     * pacing floor. Dump the window state across that boundary so the next log
     * says whether the swapchain is saturating (buffers all queued /
     * consumer_running_behind) rather than leaving it to inference. */
    if (frame_no >= 20 && frame_no <= 32)
      nx_window_report("pre-render");
    if (mark)
      debugPrintf("[loop] --> nativeRender frame %llu\n",
                  (unsigned long long)frame_no);
    uint64_t r0 = now_ns();
    uint8_t rendered = U.render(fake_env, fake_unityplayer_thiz);
    if (mark)
      debugPrintf("[loop] <-- nativeRender frame %llu returned\n",
                  (unsigned long long)frame_no);
    uint64_t rdt = now_ns() - r0;
    if (rdt > 250000000ull)
      debugPrintf("[loop] SLOW nativeRender: %llu ms (frame %llu)\n",
                  (unsigned long long)(rdt / 1000000ull),
                  (unsigned long long)frame_no);

    if (boosted) {
      fast_frames = (rdt < 33000000ull) ? fast_frames + 1 : 0;
      if (fast_frames >= 30) {
        cpu_boost(0);
        boosted = 0;
        debugPrintf("[loop] CPU boost OFF -- steady state reached at frame %llu\n",
                    (unsigned long long)frame_no);
      }
    }
    if (!rendered) {
      debugPrintf("[boot] nativeRender returned false -- engine requested exit\n");
      break;
    }

    frames++;
    frame_no++;

    /* Commit pending saves roughly every 2s. Without this the PlayerPrefs store
     * only reaches the SD card on focus-loss or clean exit, so a crash or a
     * yanked console loses the run -- which matters for a roguelike. */
    if ((frame_no % 120) == 0)
      nx_sd_flush();

    /* FRAME LIMITER. Pace to 60 Hz, and ALWAYS yield at least a slice even when
     * the frame overran: with no real display vsync, a run of fast frames after
     * a slow one can submit buffers faster than the compositor drains them, and
     * flooding vi takes the whole system down rather than just the game. That
     * matches what is being observed -- render loop and watchdog thread going
     * silent simultaneously. */
    uint64_t elapsed = now_ns() - frame_start;
    if (elapsed < TARGET_FRAME_NS)
      svcSleepThread(TARGET_FRAME_NS - elapsed);
    else
      /* Overran the budget. Still yield: this 1 ms floor is what stopped the
       * hard system freeze at the first real content present. Without it, a run
       * of fast frames following the slow load frames submitted buffers faster
       * than the compositor drained them and took the whole console down --
       * render loop and the independent watchdog thread stopping in the same
       * instant. Cheap insurance; do not remove it as an "optimisation". */
      svcSleepThread(1000000ull);

    /* Heartbeat every 2s. Deliberately tagged so it flushes immediately: a
     * heartbeat that only reaches the card on the next timer is useless for
     * diagnosing a hang, and its ABSENCE is itself the diagnosis. */
    uint64_t t = now_ns();
    if (t - last_log >= 2000000000ull) {
      void *tm = cp_clock_tm();
      if (tm) {
        uint64_t fc = *(uint64_t *)((uintptr_t)tm + TM_FIELD_FRAMECOUNT);
        uint32_t rc = *(uint32_t *)((uintptr_t)tm + TM_FIELD_RENDERCOUNT);
        uint8_t  pz = *(uint8_t  *)((uintptr_t)tm + TM_FIELD_PAUSE);
        debugPrintf("[loop] frame=%llu fps=%.1f | engine: frameCount=%llu "
                    "renderCount=%u paused=%u time=%.2fs\n",
                    (unsigned long long)frame_no,
                    frames * 1e9 / (double)(t - last_log),
                    (unsigned long long)fc, rc, pz, cp_clock_last_time());
      } else {
        debugPrintf("[loop] frame=%llu fps=%.1f | TimeManager not ticking; "
                    "managed Time driven by hooks: t=%.2fs dt=%.4f frames=%d\n",
                    (unsigned long long)frame_no,
                    frames * 1e9 / (double)(t - last_log),
                    cp_time_now(), cp_time_delta(), cp_time_frames());
      debugPrintf("[gpua] live=%u MB peak=%u MB\n",
                  cp_gpua_live_mb(), cp_gpua_peak_mb());
      }
      frames = 0;
      last_log = t;
    }
  }

  debugPrintf("[boot] shutting down\n");
  if (U.pause)
    U.pause(fake_env, fake_unityplayer_thiz);
  if (U.applicationUnload)
    U.applicationUnload(fake_env, fake_unityplayer_thiz);
  U.done(fake_env, fake_unityplayer_thiz);
  return 0;
}
