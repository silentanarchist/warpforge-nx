/* main.c -- CloverPit Nintendo Switch wrapper entry point.
 *
 * Unity 6000.2.6f2 / IL2CPP, arm64-v8a. Loads libmain + libunity + libil2cpp +
 * lib_burst_generated, resolves their imports against native Switch
 * implementations, and drives the lifecycle the Java UnityPlayer normally runs.
 * The UnityPlayer natives are recovered BY NAME from libunity's RegisterNatives
 * table (jni_fake.c); the RVAs in cloverpit_entrypoints.h only cross-check that.
 *
 * Forked from badpiggies_nx / colorsheep_nx / laytonbmr_nx / vln_nx (MIT). The
 * memory / heap / overcommit / crash-handler scaffolding above main() is
 * inherited verbatim -- it is engine-generation-generic. Everything CloverPit
 * and Unity 6 specific lives below the banner comment in main(), and in the
 * cloverpit_*.c units:
 *
 *   cloverpit_locate.c    find the engine functions we patch, at runtime
 *   cloverpit_patches.c   choreographer / audio-output / TimeManager clock
 *   cloverpit_boot.c      the Unity 6 lifecycle + render loop
 *   cloverpit_imports.c   the 69 media-NDK imports Unity 6 adds
 *   cloverpit_input.c     HID sampling (event injection NOT yet implemented)
 *
 * Deltas vs the 2020.3 reference this was forked from -- each one fails silently
 * rather than loudly if you get it wrong:
 *   - initJni is 4-ARG (env, thiz, Context, int), not 3-arg;
 *   - nativeUnityPlayerSetRunning(bool) exists and gates the frame loop;
 *   - a fourth module, lib_burst_generated.so, is loaded (optional);
 *   - libswappywrapper / libvulkan / libaaudio are refused at dlopen;
 *   - engine patch sites are located by fingerprint, not hard-coded RVA;
 *   - the region-granularity 256MB->64MB table is NOT derived for Unity 6
 *     (see PORT_PLAN milestone [R]) -- 8GB consoles are the practical target.
 */
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <unistd.h>
#include <time.h>
#include <stdio.h>
#include <sys/stat.h>
#include <switch.h>
#include <SDL2/SDL.h>

#include "config.h"
#include "util.h"
#include "error.h"
#include "so_util.h"
#include "imports.h"
#include "jni_fake.h"
#include "android_native_unity.h"
#include "opensles.h"
#include "cloverpit_entrypoints.h"
#include "diag.h"
#include "cloverpit_locate.h"
#include "cloverpit_root.h"
#include "cloverpit_il2cpp.h"
#include "cloverpit_video.h"
#include "cloverpit_time.h"
void cp_gpua_enable(void);   /* cloverpit_gpuarena.c */
#include "cloverpit_touchhook.h"
#include "cloverpit_screen.h"
#include "cloverpit_offsets.h"
#include "warpforge_resolve.h"

/* Resolved at runtime from the .nro's own location -- see cloverpit_root.c.
 * Compiling the folder name in meant the SD directory had to be named exactly
 * "cloverpit_nx" or check_data() reported libmain.so missing while it sat right
 * there, and debug.log went to the same nonexistent folder so there was nothing
 * to diagnose with. */
#define DATA_ROOT  cp_game_root()
#define LIB_MAIN   "libmain.so"
#define LIB_UNITY  "libunity.so"
#define LIB_IL2CPP "libil2cpp.so"

/* jni_fake.c: recover a UnityPlayer native by name from the captured RegisterNatives table */
void *jni_lookup_unity_native(const char *name);
/* unity_glue.c */
void unity_environment_init(const char *data_root);

static void *heap_so_base = NULL;
static size_t heap_so_limit = 0;

/* mmap arena (consumed by mmap_fake/munmap_fake in libc_shim.c). */
void  *g_mmap_arena_base = NULL;
size_t g_mmap_arena_size = 0;
int    g_overcommit      = 0;
u64    g_alias_base = 0, g_alias_size = 0;
unsigned g_oc_heap_mb = 0, g_oc_freed_mb = 0;
int      g_oc_hint_map = 0, g_oc_hint_unmap = 0;
unsigned g_oc_alias_mb = 0;
void    *g_oc_win = NULL;
int      g_oc_probe_tried = 0, g_oc_shrink_tried = 0;
extern int oc_arena_init(void *window, size_t window_bytes, void *pool, size_t pool_bytes);
extern int oc_arena_add_window(void *window, size_t window_bytes);
unsigned g_oc_probe_rc = 0, g_oc_shrink_rc = 0;
unsigned long g_oc_win_addr = 0;
u64      g_oc_sysres = 0;

so_module main_mod, unity_mod, il2cpp_mod;
extern so_module burst_mod, rayfire_mod;

/* Strong override of nx_crash_handler.c's weak stub: name addresses that fall inside
 * our loaded .so images (creport can't, since they aren't real modules). */
int crash_resolve_module(uintptr_t addr, char *name_out, size_t name_cap, uintptr_t *base_out) {
  const struct { const char *n; so_module *m; } mods[] = {
    { "libmain.so", &main_mod }, { "libunity.so", &unity_mod }, { "libil2cpp.so", &il2cpp_mod },
    { "lib_burst_generated.so", &burst_mod }, { "libRF_CNative_andr.so", &rayfire_mod },
  };
  for (unsigned i = 0; i < sizeof(mods)/sizeof(*mods); i++) {
    uintptr_t b = (uintptr_t)mods[i].m->load_virtbase;
    if (b && addr >= b && addr < b + mods[i].m->load_size) {
      snprintf(name_out, name_cap, "%s", mods[i].n);
      *base_out = b;
      return 1;
    }
  }
  /* Our own image (the nro: wrapper + Mono): offsets resolve against warpforge_nx.elf. */
  {
    /* __start__ is the link address (0); the load address comes from the memory
     * map of our own code, and __end__ (also link-relative) is the image size. */
    extern char __end__[];   /* libnx switch.ld */
    static uintptr_t nro_base;
    if (!nro_base) {
      MemoryInfo mi; u32 pi;
      if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, (u64)(uintptr_t)&crash_resolve_module))) nro_base = (uintptr_t)mi.addr;
    }
    uintptr_t b = nro_base, e = nro_base + (uintptr_t)__end__;
    if (b && addr >= b && addr < e) {
      snprintf(name_out, name_cap, "warpforge_nx.nro");
      *base_out = b;
      return 1;
    }
  }
#if WF_MONO
  /* Detour trampolines (relocated first instructions of hooked methods). */
  {
    extern int wf_detour_region(uintptr_t *rx_base, size_t *size);
    uintptr_t b = 0; size_t sz = 0;
    if (wf_detour_region(&b, &sz) && addr >= b && addr < b + sz) {
      snprintf(name_out, name_cap, "detour-trampolines");
      *base_out = b;
      return 1;
    }
  }
#endif
  return 0;
}

extern uintptr_t g_il2cpp_base;       /* libc_shim.c: GC stop-the-world bridge */
extern size_t    g_il2cpp_size;       /* libc_shim.c: bounds guard for the same */
extern void nx_sd_flush(void);        /* libc_shim.c: periodic SD commit       */

/* audio warmup gate for opensles.c (frames since boot) */
static volatile uint32_t g_frame_count = 0;
uint32_t port_frame_count(void) { return g_frame_count; }
/* The render loop lives in cloverpit_boot.c, so it cannot touch the static
 * above directly; the crash handler reads it to report which frame died. */
void port_frame_tick(void) { g_frame_count++; }

/* Main-thread bionic TLS, shared with cloverpit_boot.c so the engine entry can
 * re-assert it without reaching into main()'s locals. */
static void *g_main_tls;
void cp_set_main_tls(void *buf) { g_main_tls = buf; }
void cp_reassert_main_tls(void) { if (g_main_tls) install_bionic_tls(g_main_tls); }

/* libunity ~19M + libil2cpp ~48M + headroom for relocated segments */
#define SO_REGION_BYTES (240u * 1024 * 1024)

/* ==========================================================================
 * Inherited memory/heap/overcommit scaffolding (verbatim from the vln/cr3_nx
 * base, MIT). Engine-generation-generic; not Color-Sheep-specific.
 * ========================================================================== */
static void *oc_find_stack_window(size_t want, size_t *out_size) {
  *out_size = 0;
  u64 sbase = 0, ssize = 0;
  svcGetInfo(&sbase, InfoType_StackRegionAddress, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&ssize, InfoType_StackRegionSize,    CUR_PROCESS_HANDLE, 0);
  if (!sbase || !ssize) return NULL;
  u64 end = sbase + ssize, a = sbase, best_a = 0, best_l = 0, free_total = 0;
  int holes = 0, mapped = 0;
  while (a < end) {
    MemoryInfo mi; u32 pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, a))) break;
    u64 ms = mi.addr, me = mi.addr + mi.size;
    if (me <= a) break;
    if (mi.type == MemType_Unmapped) {
      u64 hs = ms < sbase ? sbase : ms, he = me > end ? end : me;
      if (he > hs) {
        free_total += he - hs;
        if (he - hs > best_l) { best_l = he - hs; best_a = hs; }
        if (holes < 8)
          debugPrintf("[oc] stack hole %d: %p .. %p (%u MB)\n",
                      holes++, (void *)hs, (void *)he, (unsigned)((he - hs) >> 20));
      }
    } else mapped++;
    a = me;
  }
  debugPrintf("[oc] stack scan: base=%p size=%u MB, %d holes, %d mapped spans, largest=%u MB\n",
              (void *)sbase, (unsigned)(ssize >> 20), holes, mapped, (unsigned)(best_l >> 20));
  if (!best_a) return NULL;
  u64 aligned = (best_a + (MMAP_ARENA_ALIGN - 1)) & ~(MMAP_ARENA_ALIGN - 1);
  if (aligned >= best_a + best_l) return NULL;
  u64 avail = ((best_a + best_l) - aligned) & ~(MMAP_ARENA_ALIGN - 1);
  if (!avail) return NULL;
  if (avail > want) avail = want;
  /* WARPFORGE: thread stacks live in this same region (libnx mirrors every
   * stack into it, including the ones pthread_create_fake allocates from the
   * heap). The window took whatever the largest hole offered, and with the
   * per-launch address layout that was once 1984 MB of a 2046 MB hole: ~62 MB
   * left for ~30 engine threads at 2 MB each plus Mono's. Background
   * Job.Worker 11 never came up and UnityMain waited for it forever in frame
   * 0. Always leave OC_STACK_RESERVE of the region free. */
  u64 cap = free_total > OC_STACK_RESERVE ? free_total - OC_STACK_RESERVE : 0;
  cap &= ~(MMAP_ARENA_ALIGN - 1);
  if (avail > cap) {
    debugPrintf("[oc] window capped %u -> %u MB to keep %u MB of the stack region for thread stacks\n",
                (unsigned)(avail >> 20), (unsigned)(cap >> 20), (unsigned)(OC_STACK_RESERVE >> 20));
    avail = cap;
  }
  if (!avail) return NULL;
  *out_size = avail;
  return (void *)aligned;
}

/* TLS pages vs the OC window. The kernel keeps every thread's TLS block in
 * 4 KB pages of 8 slots, allocated in the STACK region at a random free address
 * (ASLR) -- and "free" includes our overcommit window, which is only reserved in
 * libnx's allocator, not mapped. A TLS page that lands inside a range the engine
 * has reserved but not yet committed makes the later svcMapMemory fail
 * (0xd401) and the engine faults on the unbacked page: the "crash on loading in"
 * seen about one run in five, with a type=0xc block in the window listing.
 *
 * The kernel only allocates a new TLS page when no existing page has a free
 * slot, so pages made BEFORE the window exists and kept partially used serve
 * every later thread. Here: batches of 8 threads are started (filling one new
 * page); if that page lies outside the window, one thread is kept parked for
 * the rest of the run (7 slots free), otherwise all 8 exit (page freed) and we
 * try again. The parked threads' own stacks are placed by libnx, which honours
 * the window reservation. 7 x SEED_PAGES slots cover the engine's ~40 threads,
 * Mono's and ours with room for churn. */
#define SEED_PAGES 24
static volatile int  g_seed_cmd[SEED_PAGES * 8 * 4];     /* 0 wait, 1 exit, 2 stay */
static volatile uintptr_t g_seed_tls[SEED_PAGES * 8 * 4];
static void seed_thread(void *arg) {
  int i = (int)(intptr_t)arg;
  g_seed_tls[i] = (uintptr_t)armGetTls();
  while (!g_seed_cmd[i]) svcSleepThread(200000);
  if (g_seed_cmd[i] == 2) for (;;) svcSleepThread(60ull * 1000000000ull);
}
/* The Thread structs of the kept threads must outlive this function: libnx keeps
 * every live thread on a linked list through its Thread struct (threadTlsAlloc
 * walks it). The first version kept them in a local array -- Mono's first
 * threadTlsAlloc then walked freed stack memory and crashed at boot. */
static Thread g_seed_threads[SEED_PAGES * 8 * 4];
static void oc_seed_tls_pages(const uintptr_t *win_lo, const uintptr_t *win_hi, int nwin) {
  int kept = 0, batches = 0, idx = 0;
  u64 t0 = armGetSystemTick();
  while (kept < SEED_PAGES && batches < SEED_PAGES * 4 && idx + 8 <= (int)(sizeof g_seed_cmd / sizeof g_seed_cmd[0])) {
    Thread *th = &g_seed_threads[idx]; int made = 0;
    for (int k = 0; k < 8; k++) {
      Result rc = threadCreate(&th[k], seed_thread, (void *)(intptr_t)(idx + k), NULL, 0x1000, 0x3B, -2);
      if (R_SUCCEEDED(rc)) rc = threadStart(&th[k]);
      if (R_FAILED(rc)) break;
      made++;
    }
    for (int spin = 0; spin < 2000; spin++) {        /* wait until all report their TLS */
      int ok = 1; for (int k = 0; k < made; k++) if (!g_seed_tls[idx + k]) ok = 0;
      if (ok) break; svcSleepThread(100000);
    }
    /* which pages did this batch land in? keep one thread per page outside the window */
    int keep[8] = {0};
    for (int k = 0; k < made; k++) {
      uintptr_t page = g_seed_tls[idx + k] & ~(uintptr_t)0xfff;
      if (!page) continue;
      int inside = 0; for (int w = 0; w < nwin; w++) if (page >= win_lo[w] && page < win_hi[w]) inside = 1;
      if (inside) continue;
      int dup = 0; for (int j = 0; j < k; j++) if (keep[j] && (g_seed_tls[idx + j] & ~(uintptr_t)0xfff) == page) dup = 1;
      if (!dup && kept < SEED_PAGES) { keep[k] = 1; kept++; }
    }
    for (int k = 0; k < made; k++) g_seed_cmd[idx + k] = keep[k] ? 2 : 1;
    for (int k = 0; k < made; k++) if (!keep[k]) { threadWaitForExit(&th[k]); threadClose(&th[k]); }
    idx += 8; batches++;
    if (made < 8) break;
  }
  debugPrintf("[oc] TLS seed: %d pages kept outside the window (%d free slots) in %d batches, %u ms\n",
              kept, kept * 7, batches, (unsigned)((armGetSystemTick() - t0) / (armGetSystemTickFreq() / 1000)));
}

/* A second (third) OC window: the largest 256 MB-aligned free run in the stack
 * region outside the windows already taken, keeping OC_STACK_RESERVE of the
 * region free overall. */
static void *oc_find_extra_window(const uintptr_t *lo, const uintptr_t *hi, int n, size_t *out_size) {
  *out_size = 0;
  u64 sbase = 0, ssize = 0;
  svcGetInfo(&sbase, InfoType_StackRegionAddress, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&ssize, InfoType_StackRegionSize,    CUR_PROCESS_HANDLE, 0);
  if (!sbase || !ssize) return NULL;
  u64 end = sbase + ssize, a = sbase, free_total = 0, best_a = 0, best_l = 0, taken = 0;
  for (int k = 0; k < n; k++) taken += hi[k] - lo[k];
  while (a < end) {
    MemoryInfo mi; u32 pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, a))) break;
    u64 me = mi.addr + mi.size;
    if (me <= a) break;
    if (mi.type == MemType_Unmapped) {
      u64 hs = mi.addr < sbase ? sbase : mi.addr, he = me > end ? end : me;
      free_total += he - hs;
      /* cut the taken windows out of this hole; consider each remaining piece */
      u64 ps = hs;
      while (ps < he) {
        u64 pe = he;
        for (int k = 0; k < n; k++) {
          if (lo[k] <= ps && hi[k] > ps) { ps = hi[k]; pe = 0; break; }
          if (lo[k] > ps && lo[k] < pe) pe = lo[k];
        }
        if (!pe) continue;
        u64 al = (ps + (MMAP_ARENA_ALIGN - 1)) & ~(MMAP_ARENA_ALIGN - 1);
        if (al < pe) {
          u64 l = (pe - al) & ~(MMAP_ARENA_ALIGN - 1);
          if (l > best_l) { best_l = l; best_a = al; }
        }
        ps = pe;
      }
    }
    a = me;
  }
  u64 room = free_total > taken + OC_STACK_RESERVE ? free_total - taken - OC_STACK_RESERVE : 0;
  room &= ~(MMAP_ARENA_ALIGN - 1);
  if (best_l > room) best_l = room;
  if (!best_a || best_l < MMAP_ARENA_ALIGN) return NULL;
  *out_size = best_l;
  return (void *)best_a;
}

static int overcommit_setup(void *addr, size_t size, size_t so_zone,
                            void **out_addr, size_t *out_fake) {
  (void)addr; (void)size; (void)so_zone; (void)out_addr; (void)out_fake;
  g_oc_hint_map   = envIsSyscallHinted(0x2c);
  g_oc_hint_unmap = envIsSyscallHinted(0x2d);
  svcGetInfo(&g_alias_base, InfoType_AliasRegionAddress, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&g_alias_size, InfoType_AliasRegionSize,    CUR_PROCESS_HANDLE, 0);
  g_oc_alias_mb = (unsigned)(g_alias_size >> 20);
  svcGetInfo(&g_oc_sysres, InfoType_SystemResourceSizeTotal, CUR_PROCESS_HANDLE, 0);
  return 0;   /* no system resource -> svcMapPhysicalMemory unusable; heap-backed */
}

/* Captured in __libnx_initheap and logged from main(): the log file is not open
 * yet at heap-setup time, and the previous reserve attempt failed SILENTLY --
 * it edited a branch that never executes under hbmenu. Reporting the actual
 * outcome is the difference between "reserved" and "believed to have
 * reserved". */

void __libnx_initheap(void) {
  void *addr;
  size_t size = 0;
  size_t mem_available = 0, mem_used = 0;

  if (envHasHeapOverride()) {
    addr = envGetHeapOverrideAddr();
    size = envGetHeapOverrideSize();
  } else {
    svcGetInfo(&mem_available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    /* Hold back GFX_RESERVE_MB for the graphics driver -- see config.h. The
     * stock 0x200000 (2 MB) margin starves switch-mesa, and a driver allocation
     * failing in the compositor path wedges the whole console. */
    const size_t gfx_reserve = (size_t)GFX_RESERVE_MB * 1024 * 1024;
    if (mem_available > mem_used + gfx_reserve)
      size = (mem_available - mem_used - gfx_reserve) & ~0x1FFFFF;
    if (size == 0)
      size = 0x2000000 * 16;
    Result rc = svcSetHeapSize(&addr, size);
    if (R_FAILED(rc))
      diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
  }

  const size_t MB = 1024 * 1024;
  size_t so_zone = SO_REGION_BYTES;
  if (so_zone > size / 2)
    so_zone = size / 2;

  extern char *fake_heap_start;
  extern char *fake_heap_end;

  void *oc_addr; size_t oc_fake;
  if (overcommit_setup(addr, size, so_zone, &oc_addr, &oc_fake)) {
    fake_heap_start = (char *)oc_addr;
    fake_heap_end   = (char *)oc_addr + oc_fake;
    heap_so_base    = (void *)ALIGN_MEM((uintptr_t)oc_addr + oc_fake, 0x1000);
    heap_so_limit   = so_zone;
    return;
  }

  const size_t big_align    = MMAP_ARENA_ALIGN;
  const size_t newlib_floor = 384 * MB;
  size_t arena_sz = MMAP_ARENA_RESERVE;
  size_t fake_heap_size;

  if (size > so_zone + big_align + newlib_floor + 256 * MB) {
    size_t avail = size - so_zone - big_align - newlib_floor;
    if (arena_sz > avail) arena_sz = avail & ~(big_align - 1);
    size_t usable    = size - so_zone - big_align;
    size_t arena_cap = ((usable * 30) / 100) & ~(big_align - 1);
    if (arena_sz > arena_cap) arena_sz = arena_cap;
    /* Leave GFX_RESERVE_MB unclaimed so the shrink below hands it back to the
     * system for switch-mesa rather than to newlib. */
    {
      size_t gfx = (size_t)GFX_RESERVE_MB * MB;
      size_t body = size - so_zone - arena_sz - big_align;
      fake_heap_size = (body > gfx + newlib_floor) ? body - gfx : body;
    }
  } else {
    fake_heap_size = (size > so_zone) ? size - so_zone : size / 2;
    arena_sz = 0;
  }

  /* DO NOT call svcSetHeapSize here.
   *
   * An attempt to shrink the heap at this point produced a build that started
   * and died with NO LOG AT ALL -- __libnx_initheap runs before the log file
   * exists, so the failure was completely silent and cost a full test cycle.
   *
   * Why it cannot work here: under hbmenu envHasHeapOverride() is set, and the
   * heap region is owned by hbloader, not by us. `addr` points into a mapping
   * this process did not create. Calling svcSetHeapSize against it either fails
   * or returns a different base, and adopting that base pulls the ground out
   * from under the loader's own allocations before main() ever runs.
   *
   * pvz_fusion CAN shrink because it does so from inside overcommit_setup(),
   * after that function has secured its own alias-region window and verified a
   * test commit -- i.e. it owns what it is shrinking. The correct fix here is to
   * find why overcommit_setup() fails on this payload (its success would give
   * both the graphics headroom and on-demand commit), NOT to shrink blind.
   *
   * Left as sizing only: newlib is given GFX_RESERVE_MB less, which costs
   * nothing and is harmless, though it does not by itself return physical pages
   * to the system. */

#if WF_MONO
  /* WARPFORGE: Mono's fake mmap gets one contiguous slice, taken off the top of
   * newlib's heap, 4 MB aligned (its largest granularity). */
  {
    const size_t mono_sz = (size_t)WF_MONO_HEAP_MB * MB;
    if (fake_heap_size > mono_sz + newlib_floor) {
      fake_heap_size -= mono_sz;
      uintptr_t lo = ((uintptr_t)addr + fake_heap_size + 0x3FFFFF) & ~(uintptr_t)0x3FFFFF;
      uintptr_t hi = (uintptr_t)addr + fake_heap_size + mono_sz;
      extern void wf_mono_heap_init(uintptr_t lo, uintptr_t hi);
      wf_mono_heap_init(lo, hi);
    }
  }
#endif

  fake_heap_start = (char *)addr;
  fake_heap_end   = (char *)addr + fake_heap_size;

  heap_so_base  = (void *)ALIGN_MEM((uintptr_t)addr + fake_heap_size + (WF_MONO ? (size_t)WF_MONO_HEAP_MB * MB : 0), 0x1000);
  heap_so_limit = so_zone;

  if (arena_sz) {
    g_mmap_arena_base = (void *)ALIGN_MEM((uintptr_t)heap_so_base + so_zone, big_align);
    g_mmap_arena_size = arena_sz;
  }
}

static void check_syscalls(void) {
  if (!envIsSyscallHinted(0x77)) fatal_error("svcMapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x78)) fatal_error("svcUnmapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x73)) fatal_error("svcSetProcessMemoryPermission is unavailable.");
  if (envGetOwnProcessHandle() == INVALID_HANDLE) fatal_error("Own process handle is unavailable.");
}

/* ==========================================================================
 * Color-Sheep-specific: data layout, module load, region no-op.
 * ========================================================================== */

/* Bad Piggies 2.4.3297 ships data.unity3d (UnityFS) + sharedassets*.resource +
 * sharedassets0.resource + the il2cpp metadata. NO OBB, NO split levels. All staged
 * flat under assets/bin/Data/ by tools/stage_sd.py. */
/* Warpforge 1.35.0 ships its data as split files (globalgamemanagers.assets.splitN,
 * sharedassets0.assets.splitN ...), which libunity joins itself (AndroidSplitFile)
 * through the asset-manager path, plus the Addressables bundles under assets/aa/
 * from the data pack. prepare_switch.py (android patch/switch) builds the layout. */
static void check_data(void) {
  const char *files[] = {
    LIB_MAIN, LIB_UNITY, LIB_IL2CPP,
    "assets/bin/Data/globalgamemanagers",
    "assets/bin/Data/globalgamemanagers.assets.split0",
    "assets/bin/Data/level0",
    "assets/bin/Data/Managed/Metadata/global-metadata.dat",
    "assets/bin/Data/boot.config",
    "assets/bin/Data/unity_app_guid",
    "assets/aa/catalog.bin",
    "assets/aa/Android/scenes_scenes_mainmenuwarpforge.bundle",
  };
  char path[768];
  struct stat st;
  for (unsigned i = 0; i < sizeof(files)/sizeof(*files); i++) {
    snprintf(path, sizeof path, "%s/%s", DATA_ROOT, files[i]);
    if (stat(path, &st) < 0)
      fatal_error("Missing data file:\n  %s\n\nSearched in:\n  %s\n\n"
                  "If your files are somewhere else, that path is the problem --\n"
                  "put the .nro in the SAME folder as libmain.so and the assets,\n"
                  "or see README.md. tools/stage_sd.py builds the layout for you.",
                  files[i], DATA_ROOT);
  }
}

static int load_module(so_module *mod, const char *name) {
  char path[768];
  snprintf(path, sizeof path, "%s/%s", DATA_ROOT, name);
  if (so_load(mod, path, heap_so_base, heap_so_limit) < 0)
    return -1;
  size_t used = ALIGN_MEM(mod->load_size, 0x1000);
  heap_so_base = (char *)heap_so_base + used;
  heap_so_limit -= used;
  crx_resolve_imports(mod);
  return 0;
}

/* Save persistence: commit the SD periodically. (Color Sheep saves via PlayerPrefs;
 * the managed PlayerPrefs flush hook is a TODO -- see below -- but committing the SD
 * still persists whatever reached the prefs file.) */
static AppletHookCookie g_applet_cookie;
static void nx_applet_hook(AppletHookType hook, void *param) {
  (void)param;
  if (hook == AppletHookType_OnFocusState || hook == AppletHookType_OnExitRequest)
    nx_sd_flush();
}

/* ===========================================================================
 * CloverPit boot.
 *
 * Everything above this line is inherited from badpiggies_nx unchanged (heap /
 * overcommit / mmap arena / crash handler / SD hygiene) -- it is engine-generic.
 * Everything below is Unity 6 and CloverPit specific.
 *
 * Differences from the 2020.3 and 2022.3 reference ports, all of which fail
 * silently rather than loudly if you get them wrong:
 *
 *   - a FOURTH module, lib_burst_generated.so, must be loaded (Burst jobs)
 *   - libswappywrapper.so / libvulkan.so / libaaudio.so must be refused at
 *     dlopen so the engine falls back to the paths this port implements
 *   - initJni takes four arguments, and nativeUnityPlayerSetRunning must be
 *     called -- see cloverpit_boot.c
 *   - the engine patches are located at runtime, not by hard-coded RVA --
 *     see cloverpit_locate.c
 * =========================================================================== */

#define LIB_BURST  "lib_burst_generated.so"
/* RayFire (mesh shattering, P/Invoked as "RF_CNative_andr"). Self-contained C++:
 * libc/libm only. Optional: without it those DllImports throw inside the game's
 * destruction effects rather than at boot. */
#define LIB_RAYFIRE "libRF_CNative_andr.so"

so_module burst_mod, rayfire_mod;

/* Implemented in cloverpit_boot.c */
int cp_boot_and_run(void);
/* Implemented in cloverpit_patches.c */
int  cp_install_patches(so_module *unity);
void cp_shutdown_patches(void);

int main(int argc, char *argv[]) {
  /* FIRST: work out where we are. debug.log lives under the resolved root, so
   * nothing may log before this returns. */
  cp_resolve_game_root(argc, argv);

  /* BSD sessions: libnx's default is 3, and every thread inside a blocking
   * socket call (a recv, a poll slice) holds one for the whole wait. The match
   * tunnel alone keeps two busy, the TLS readers and Photon's threads more, so
   * the engine's main thread waited for a free session inside an ordinary
   * send() -- a condition-variable wait under il2cpp's Socket.Send, 54% of the
   * samples taken during battle hitches (19:01). 16 is libnx's maximum. */
  int bsd_sessions = 16;
  {
    SocketInitConfig sc = *socketGetDefaultInitConfig();
    sc.num_bsd_sessions = 16;
    if (R_FAILED(socketInitialize(&sc))) {
      sc.num_bsd_sessions = 8;
      bsd_sessions = 8;
      if (R_FAILED(socketInitialize(&sc))) { socketInitializeDefault(); bsd_sessions = 3; }
    }
  }
  {
    /* The game's TLS checks certificate validity against this clock; a console
     * whose date is off fails verification with Unity's "Unknown error". */
    time_t now = time(NULL);
    struct tm *g = gmtime(&now);
    debugPrintf("[boot] network: %d BSD sessions\n", bsd_sessions);
    if (g) debugPrintf("[boot] clock: %04d-%02d-%02d %02d:%02d:%02d UTC (epoch %lld)\n",
                       g->tm_year + 1900, g->tm_mon + 1, g->tm_mday, g->tm_hour, g->tm_min, g->tm_sec,
                       (long long)now);
  }
  debugPrintf("[boot] === warpforge_nx start (Warpforge 1.35.0, Unity 6000.2.6f2 / IL2CPP) ===\n");
  cp_root_report(argc, argv);

  /* CWD fix: title-override leaves cwd at the .nro folder or SD root; Unity reads
   * many files via relative paths, so chdir into DATA_ROOT. */
  {
    char cwd[256] = {0};
    getcwd(cwd, sizeof cwd);
    int rc = chdir(DATA_ROOT);
    struct stat st;
    int reach_meta = stat("assets/bin/Data/Managed/Metadata/global-metadata.dat", &st) == 0;
    int reach_data = stat("assets/bin/Data/globalgamemanagers", &st) == 0;
    debugPrintf("[boot] cwd '%s' -> chdir(%s)=%d; metadata=%d globalgamemanagers=%d\n",
                cwd, DATA_ROOT, rc, reach_meta, reach_data);
    if (rc != 0)
      debugPrintf("[boot] WARNING: chdir failed -- data root may be wrong\n");
  }

  /* No config.txt. It was inherited from the badpiggies_nx lineage and its only
   * key was `language`, which CloverPit already exposes in its own options --
   * along with resolution and frame rate, none of which were ever config keys
   * here anyway. A file whose every setting is also in the game's own menu is
   * just a second place for the two to disagree. The Switch system language is
   * the default and the in-game selector overrides it. */

  /* FastLoad clocks (CPU 1020 -> 1785 MHz) for the whole load path: module
   * relocation, 467+22 ctors, il2cpp metadata parsing and Unity's first-scene
   * asset decompression are all CPU-bound, and the hardware timing showed
   * nativeRender at 1188 ms and 2987 ms for the first two frames. FastLoad
   * trades GPU clock for CPU, which is the right way round while loading and
   * the wrong way round once rendering starts -- cloverpit_boot.c drops back to
   * Normal as soon as frames are consistently fast. */
  cpu_boost(1);
  debugPrintf("[boot] CPU boost ON (FastLoad) for the load path\n");

  check_syscalls();
  debugPrintf("[boot] syscalls ok\n");
  {
    extern char *fake_heap_start, *fake_heap_end;
    debugPrintf("[boot] mem: newlib=%u MB, mmap arena=%u MB @ %p\n",
                (unsigned)((fake_heap_end - fake_heap_start) / (1024 * 1024)),
                (unsigned)(g_mmap_arena_size / (1024 * 1024)), g_mmap_arena_base);
    u64 tot = 0, used = 0;
    svcGetInfo(&tot,  InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize,  CUR_PROCESS_HANDLE, 0);
    debugPrintf("[boot] phys: total=%u MB used=%u MB free=%u MB "
                "(target >= %u MB for switch-mesa)\n",
                (unsigned)(tot >> 20), (unsigned)(used >> 20),
                (unsigned)((tot - used) >> 20), GFX_RESERVE_MB);
    if (((tot - used) >> 20) < GFX_RESERVE_MB / 2)
      debugPrintf("[boot] WARNING: less than %u MB free -- the graphics driver "
                  "may fail an allocation mid-frame and wedge the console\n",
                  GFX_RESERVE_MB / 2);
    if ((tot >> 20) < 1000)
      fatal_error("Only %u MB of memory: this was launched from the album.\n\n"
                  "Unity needs the memory of a full application. Hold R while\n"
                  "starting any installed game, then pick warpforge_nx.",
                  (unsigned)(tot >> 20));
    if ((tot >> 20) < 5000)
      debugPrintf("[boot] NOTE: 4GB console. Warpforge's payload is ~150 MB of "
                  "libs + ~770 MB of assets (loaded on demand through Addressables).\n");
  }

  /* Stack-region overcommit arena. Any failure -> heap-backed arena. */
  {
    void *pool = NULL;
    size_t winsz = 0;
    void *win = oc_find_stack_window(OC_WINDOW_BYTES, &winsz);
    VirtmemReservation *rv = NULL;
    if (win && winsz) {
      virtmemLock();
      rv = virtmemAddReservation(win, winsz);
      virtmemUnlock();
    }
    if (win && rv && winsz) {
      pool = memalign(0x1000, OC_POOL_BYTES);
      if (pool && oc_arena_init(win, winsz, pool, OC_POOL_BYTES)) {
        debugPrintf("[oc] ARMED: window %u MB @ %p, pool %u MB @ %p\n",
                    (unsigned)(winsz >> 20), win, (unsigned)(OC_POOL_BYTES >> 20), pool);
        uintptr_t lo[3] = { (uintptr_t)win }, hi[3] = { (uintptr_t)win + winsz };
        int nwin = 1;
        while (nwin < 3) {
          size_t xs = 0;
          void *xw = oc_find_extra_window(lo, hi, nwin, &xs);
          if (!xw) break;
          virtmemLock();
          VirtmemReservation *xr = virtmemAddReservation(xw, xs);
          virtmemUnlock();
          if (!xr || !oc_arena_add_window(xw, xs)) break;
          debugPrintf("[oc] extra window %d: %u MB @ %p\n", nwin, (unsigned)(xs >> 20), xw);
          lo[nwin] = (uintptr_t)xw; hi[nwin] = (uintptr_t)xw + xs; nwin++;
        }
        oc_seed_tls_pages(lo, hi, nwin);
      } else
        debugPrintf("[oc] DISABLED: pool=%p init failed -> heap-backed only\n", pool);
    } else {
      debugPrintf("[oc] DISABLED: no usable stack hole -> heap-backed only\n");
    }
  }

#if WF_MONO
  /* Mod milestone 1: the Mono runtime in this process, a managed hello-world
   * before the engine loads anything. */
  {
    extern int wf_mono_start(void), wf_mono_run_hello(void);
    if (wf_mono_start() == 0) wf_mono_run_hello();
  }
#endif

  screen_width  = CP_FORCE_SCREEN_W;
  screen_height = CP_FORCE_SCREEN_H;
  debugPrintf("[gfx] mode=%s render=%dx%d (forced)\n",
              appletGetOperationMode() == AppletOperationMode_Console ? "DOCKED" : "HANDHELD",
              screen_width, screen_height);

  SDL_SetMainReady();
  if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0)
    debugPrintf("SDL_Init failed: %s\n", SDL_GetError());

  /* FORCE SINGLE-THREADED RENDERING.
   *
   * CloverPit ships boot.config with gfx-threading-mode=4 (multithreaded). In
   * that mode Unity's RENDER THREAD submits presents independently of this
   * loop, so the frame limiter here throttles frame *generation* but not buffer
   * *submission* -- and outrunning the compositor takes the whole console down,
   * not just the game.
   *
   * That is exactly the observed progression: adding a 1 ms pacing floor moved
   * the hard freeze from frame 4 to frame 26 (a real improvement, since it slows
   * how fast the main loop feeds the render thread) but could not remove it,
   * because the submissions were never under this loop's control.
   *
   * badpiggies_nx's payload shipped with mt rendering already disabled, which is
   * why the inherited substrate never had to do this. Rewriting one line of a
   * plain-text config in the user's own staged copy is reversible and logged;
   * tools/stage_sd.py --force-st does the same thing ahead of time. */
  {
    char bc[600];
    snprintf(bc, sizeof bc, "%s/assets/bin/Data/boot.config", DATA_ROOT);
    FILE *f = fopen(bc, "r");
    if (f) {
      char buf[2048];
      size_t n = fread(buf, 1, sizeof buf - 1, f);
      fclose(f);
      buf[n] = 0;
      char *hit = strstr(buf, "gfx-threading-mode=4");
      if (hit) {
        hit[strlen("gfx-threading-mode=")] = '0';   /* 4 -> 0, same length */
        f = fopen(bc, "w");
        if (f) {
          fwrite(buf, 1, n, f);
          fclose(f);
          debugPrintf("[gfx] boot.config: gfx-threading-mode 4 -> 0 "
                      "(single-threaded rendering; MT submits presents outside "
                      "our frame pacing and wedges the compositor)\n");
        } else {
          debugPrintf("[gfx] WARNING: could not rewrite boot.config "
                      "(read-only SD?) -- MT rendering stays on\n");
        }
      } else {
        debugPrintf("[gfx] boot.config: threading already non-4\n");
      }
    } else {
      debugPrintf("[gfx] WARNING: boot.config not readable at %s\n", bc);
    }
  }

  check_data();

  /* Sweep Unity's case-sensitivity probe files so they don't accumulate. */
  {
    DIR *dd = opendir(DATA_ROOT);
    int swept = 0;
    if (dd) {
      struct dirent *de;
      while ((de = readdir(dd))) {
        if (strncasecmp(de->d_name, "CASESENSITIVETEST", 17) == 0 ||
            strcmp(de->d_name, ".casetest") == 0) {
          char pth[320];
          snprintf(pth, sizeof pth, "%s/%s", DATA_ROOT, de->d_name);
          if (unlink(pth) == 0) swept++;
        }
      }
      closedir(dd);
    }
    if (swept) debugPrintf("[boot] swept %d case-sensitivity probe file(s)\n", swept);
  }

  debugPrintf("[boot] loading modules...\n");
  if (load_module(&main_mod,   LIB_MAIN)   < 0) fatal_error("Could not load %s", LIB_MAIN);
  if (load_module(&unity_mod,  LIB_UNITY)  < 0) fatal_error("Could not load %s", LIB_UNITY);
  if (load_module(&il2cpp_mod, LIB_IL2CPP) < 0) fatal_error("Could not load %s", LIB_IL2CPP);

  /* Burst is OPTIONAL. Its jobs are resolved by name at runtime; if the module is
   * absent the engine falls back to the managed implementations -- slower, but it
   * boots. Making this fatal would block bring-up for no reason. */
  if (load_module(&burst_mod, LIB_BURST) < 0) {
    debugPrintf("[boot] %s not loaded -- Burst jobs fall back to managed\n", LIB_BURST);
    memset(&burst_mod, 0, sizeof burst_mod);
  }

  if (load_module(&rayfire_mod, LIB_RAYFIRE) < 0) {
    debugPrintf("[boot] %s not loaded -- RayFire effects will throw when used\n", LIB_RAYFIRE);
    memset(&rayfire_mod, 0, sizeof rayfire_mod);
  }

  g_il2cpp_base = (uintptr_t)il2cpp_mod.load_virtbase;
  g_il2cpp_size = il2cpp_mod.load_size;
  debugPrintf("[boot] libmain=%p libunity=%p libil2cpp=%p burst=%p\n",
              main_mod.load_virtbase, unity_mod.load_virtbase,
              il2cpp_mod.load_virtbase, burst_mod.load_virtbase);

  so_finalize(&main_mod);   so_flush_caches(&main_mod);
  so_finalize(&unity_mod);  so_flush_caches(&unity_mod);
  so_finalize(&il2cpp_mod); so_flush_caches(&il2cpp_mod);
  if (burst_mod.load_virtbase) { so_finalize(&burst_mod); so_flush_caches(&burst_mod); }
  if (rayfire_mod.load_virtbase) { so_finalize(&rayfire_mod); so_flush_caches(&rayfire_mod); }
  debugPrintf("[boot] modules finalized + flushed\n");

  /* Locate the engine functions we patch, then patch them. Both steps are
   * fail-safe: an unresolved target is skipped, never guessed at. */
  cp_locate_all(&unity_mod);
  cp_install_patches(&unity_mod);

  /* Region granularity must be patched BEFORE any engine code runs, since the
   * memory manager reserves its first blocks during the init arrays. */
  cp_install_region_patch(&unity_mod);

  /* MAIN-THREAD BIONIC TLS -- must precede any engine code.
   *
   * Unity's stack-protector prologues do:
   *     mrs x19, tpidr_el0
   *     ldr x9, [x19, #0x28]      <- the canary
   * With TPIDR_EL0 unset that reads address 0x28 and takes a level-1
   * translation fault. It is exactly what happened at libunity+0x7042fc during
   * ctor ~100 of libunity's init_array, with x19 = 0 in the dump.
   *
   * Every thread that runs engine code needs its OWN zeroed block (a shared one
   * races: one thread's writes corrupt another's in-flight canary). The pthread
   * shim, the OpenSL audio thread and the clock thread each do this already;
   * the main thread was the one left out. */
  static uint8_t main_tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));
  install_bionic_tls(main_tls);
  cp_set_main_tls(main_tls);
  debugPrintf("[boot] main-thread bionic TLS @ %p\n", (void *)main_tls);

  so_execute_init_array(&main_mod);
  so_execute_init_array(&unity_mod);
  so_execute_init_array(&il2cpp_mod);
  if (burst_mod.load_virtbase) so_execute_init_array(&burst_mod);
  if (rayfire_mod.load_virtbase) so_execute_init_array(&rayfire_mod);

  /* Release the raw ELF staging buffers now that everything is relocated and
   * constructed. This is ~123 MB for CloverPit (libil2cpp alone is 100 MB) and
   * it comes straight out of the newlib heap the engine is about to want. */
  so_free_temp(&main_mod);
  so_free_temp(&unity_mod);
  so_free_temp(&il2cpp_mod);
  if (burst_mod.load_virtbase) so_free_temp(&burst_mod);
  if (rayfire_mod.load_virtbase) so_free_temp(&rayfire_mod);
  debugPrintf("[boot] init arrays run; staging buffers freed\n");

  jni_init();
  /* Creates fake_unityplayer_thiz / fake_context_obj / fake_surface_obj and
   * registers the asset + PlayerPrefs + Display JNI handlers against the data
   * root. Without it those three globals stay NULL (they are `= 0` in
   * unity_glue.c) and initJni is handed a null Context. */
  unity_environment_init(DATA_ROOT);

  /* Order matters and is inherited from the reference: update_mode() sizes the
   * NWindow from screen_width/height, and input_init() then binds HID against
   * that size. Calling input_init() alone leaves the window at its 720x1280
   * PORTRAIT default while Unity renders 1920x1080 landscape -- the game draws
   * into a buffer of the wrong shape and comes out cropped/rotated. */
  android_native_update_mode();
  android_native_input_init();
  appletHook(&g_applet_cookie, nx_applet_hook, NULL);

  /* Re-assert the guard: jni_init / HID / applet setup all ran in between and
   * any of them could have left tpidr in an unexpected state. The reference
   * re-asserts at exactly these two points too. */
  install_bionic_tls(main_tls);

  /* libunity's JNI_OnLoad registers the UnityPlayer natives; the capture in
   * jni_fake.c is what cloverpit_boot.c later resolves by name. */
  {
    typedef int (*fn_jnionload)(void *, void *);
    fn_jnionload Unity_JNI_OnLoad =
        (fn_jnionload)so_try_find_addr_rx(&unity_mod, "JNI_OnLoad");
    if (!Unity_JNI_OnLoad) fatal_error("libunity JNI_OnLoad not found");
    debugPrintf("[boot] libunity JNI_OnLoad(fake_vm)...\n");
    int jver = Unity_JNI_OnLoad(fake_vm, NULL);
    debugPrintf("[boot] JNI_OnLoad returned 0x%x\n", jver);
  }
  {
    typedef int (*fn_jnionload)(void *, void *);
    fn_jnionload Il2cpp_JNI_OnLoad =
        (fn_jnionload)so_try_find_addr_rx(&il2cpp_mod, "JNI_OnLoad");
    if (Il2cpp_JNI_OnLoad) {
      debugPrintf("[boot] libil2cpp JNI_OnLoad(fake_vm)...\n");
      Il2cpp_JNI_OnLoad(fake_vm, NULL);
    } else {
      debugPrintf("[boot] WARNING: libil2cpp JNI_OnLoad missing (managed JNI may fail)\n");
    }
  }

  /* Managed-side hooks (UnityEngine.Time driven from our clock, the touchscreen
   * presented as a mouse) are WARPFORGE-resolved by name through libil2cpp's
   * reflection API and installed from the render loop on the first frame after
   * il2cpp_init -- see warpforge_hooks.c / warpforge_resolve.c. CloverPit's
   * Rewired joystick hooks, Panik PlatformAPI override, VideoPlayer hooks and
   * RenderingMaster screen fix were CloverPit-specific and are not installed. */
  cp_il2cpp_bind(&il2cpp_mod);
  wf_resolve_bind(&il2cpp_mod);
#if CP_VIDEO
  cp_video_init(cp_game_root());
  cp_il2cpp_install_video_hooks(&il2cpp_mod);
#endif

  /* Arm the GPU arena only now. Everything before this point -- the overcommit
   * pool, the mmap arena, module loading -- makes large page-aligned
   * allocations that are NOT graphics buffers, and letting those trigger the
   * arena reserved 512 MB for nothing and regressed the game to a frame-0
   * freeze. From here on the only page-aligned allocator of this size class is
   * libdrm_nouveau. */
  cp_gpua_enable();

  diag_thread_register(NULL, 1);       /* mark this as the engine main thread */
  diag_set_name(NULL, "warpforge-main");
  diag_watchdog_start();               /* logs a backtrace if a frame wedges */

  int rc = cp_boot_and_run();   /* re-asserts TLS itself, right before initJni */

  cp_shutdown_patches();
  nx_sd_flush();
  debugPrintf("[boot] exit rc=%d\n", rc);
  opensles_shutdown();
  SDL_Quit();
  socketExit();

  /* The engine leaves threads and atexit handlers behind that will fault if the
   * normal C runtime teardown runs, so leave the same way the reference does. */
  extern void NX_NORETURN __libnx_exit(int rc);
  __libnx_exit(0);
  return 0;
}
