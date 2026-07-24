/* cloverpit_il2cpp.c -- present the Switch pad to Rewired as a UnityEngine.Input
 * joystick, by hooking UnityEngine.Input in the game's own libil2cpp.so.
 *
 * See cloverpit_il2cpp.h for why UnityEngine.Input is the right surface here
 * (short version: Rewired's Android path reads it).
 *
 * WHAT EACH HOOK IS FOR
 *   GetJoystickNames  Rewired's UnityInputJoystickSource calls this, diffs the
 *                     result against last frame, and creates/destroys Joysticks
 *                     accordingly. Return nothing and Rewired believes no
 *                     controller exists, which is the current symptom. This is
 *                     the single most load-bearing hook.
 *   GetKey/Down/Up    Rewired polls KeyCode.Joystick1Button0..19 for the button
 *                     elements of the joystick it created above.
 *   GetAxisRaw/GetAxis  stick and trigger axes. Rewired builds the axis name at
 *                     runtime by concatenation, so the literal never appears in
 *                     global-metadata; the hook therefore PARSES whatever name
 *                     it is handed rather than matching fixed strings.
 *
 * SAFETY
 * Every installer verifies the first instruction word against the guard recorded
 * from the shipped 1.4.8 binary before writing. Guard mismatch -> that hook is
 * skipped and logged. A missing hook means "no controller"; a wrong hook means
 * a detour into the middle of some unrelated managed method.
 *
 * !! NOT VERIFIED ON HARDWARE. The button ORDER in cp_button_map[] below is the
 * standard Unity-on-Android joystick ordering, but which physical Switch button
 * ends up on which index also depends on the controller template Rewired picks
 * from the reported name. Expect to reorder that one table on first boot; it is
 * deliberately the only thing you should need to touch. !!
 *
 * MIT.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <switch.h>

#include "so_util.h"
#include "util.h"
#include "config.h"
#include "cloverpit_il2cpp.h"
#include "cloverpit_video.h"

/* Tripwire. CP_OWN_UNHOLY_FUSION_DLC lives in config.h, and this file did not
 * include config.h -- so `#if CP_OWN_UNHOLY_FUSION_DLC` quietly evaluated to 0
 * and the entire DLC hook, install call and its log lines vanished from the
 * build with no warning at all. The symptom was a missing [dlc] line in a log
 * that was already being read for a different bug. Any feature flag used here
 * must fail loudly if its header is missing. */
#ifndef CP_OWN_UNHOLY_FUSION_DLC
#error "config.h not included -- feature flags in this file would compile out silently"
#endif

/* ------------------------------------------------------- il2cpp runtime API */

typedef void *(*fn_string_new)(const char *);
typedef void *(*fn_array_new)(void *klass, uintptr_t length);
typedef void *(*fn_class_from_name)(void *image, const char *ns, const char *name);
typedef void *(*fn_domain_get)(void);
typedef void *(*fn_domain_assembly_open)(void *domain, const char *name);
typedef void *(*fn_assembly_get_image)(void *assembly);
typedef void *(*fn_thread_current)(void);
typedef void *(*fn_thread_attach)(void *domain);

static fn_string_new           il2cpp_string_new;
static fn_array_new            il2cpp_array_new;
static fn_class_from_name      il2cpp_class_from_name;
static fn_domain_get           il2cpp_domain_get;
static fn_domain_assembly_open il2cpp_domain_assembly_open;
static fn_assembly_get_image   il2cpp_assembly_get_image;
static fn_thread_current       il2cpp_thread_current;
static fn_thread_attach        il2cpp_thread_attach;

static void *g_string_class;
static int   g_bound;

/* Il2CppArray layout: the element pointers begin one machine word past the
 * bounds/max_length header, i.e. at offset 0x20 on 64-bit. Il2CppDumper's
 * il2cpp.h agrees (Il2CppArray = { Il2CppObject obj; Il2CppArrayBounds *bounds;
 * il2cpp_array_size_t max_length; void *vector[]; }). */
#define IL2CPP_ARRAY_DATA(arr) ((void **)((uintptr_t)(arr) + 0x20))

int cp_il2cpp_bind(so_module *il2cpp) {
  il2cpp_string_new = (fn_string_new)so_try_find_addr_rx(il2cpp, "il2cpp_string_new");
  il2cpp_array_new = (fn_array_new)so_try_find_addr_rx(il2cpp, "il2cpp_array_new");
  il2cpp_class_from_name =
      (fn_class_from_name)so_try_find_addr_rx(il2cpp, "il2cpp_class_from_name");
  il2cpp_domain_get = (fn_domain_get)so_try_find_addr_rx(il2cpp, "il2cpp_domain_get");
  il2cpp_domain_assembly_open =
      (fn_domain_assembly_open)so_try_find_addr_rx(il2cpp, "il2cpp_domain_assembly_open");
  il2cpp_assembly_get_image =
      (fn_assembly_get_image)so_try_find_addr_rx(il2cpp, "il2cpp_assembly_get_image");
  il2cpp_thread_current = (fn_thread_current)so_try_find_addr_rx(il2cpp, "il2cpp_thread_current");
  il2cpp_thread_attach = (fn_thread_attach)so_try_find_addr_rx(il2cpp, "il2cpp_thread_attach");

  if (!il2cpp_string_new || !il2cpp_array_new || !il2cpp_class_from_name ||
      !il2cpp_domain_get || !il2cpp_domain_assembly_open || !il2cpp_assembly_get_image) {
    debugPrintf("[il2cpp] bind FAILED (string_new=%p array_new=%p class_from_name=%p)\n",
                (void *)il2cpp_string_new, (void *)il2cpp_array_new,
                (void *)il2cpp_class_from_name);
    return -1;
  }
  g_bound = 1;
  debugPrintf("[il2cpp] runtime API bound\n");
  return 0;
}

/* System.String class, needed to allocate the string[] GetJoystickNames returns.
 * Resolved lazily: the domain does not exist until the runtime has initialised,
 * which is after hooks are installed. */
static void *string_class(void) {
  if (g_string_class)
    return g_string_class;
  void *domain = il2cpp_domain_get();
  if (!domain)
    return NULL;
  void *asm_ = il2cpp_domain_assembly_open(domain, "mscorlib.dll");
  if (!asm_)
    return NULL;
  void *img = il2cpp_assembly_get_image(asm_);
  if (!img)
    return NULL;
  g_string_class = il2cpp_class_from_name(img, "System", "String");
  return g_string_class;
}

/* Managed code can only be entered from a thread the runtime knows about. Our
 * hooks run on the render thread, which il2cpp attached at startup, but the
 * check is cheap and the failure mode without it is a GC crash. */
static void ensure_attached(void) {
  if (il2cpp_thread_current && il2cpp_thread_attach && !il2cpp_thread_current())
    il2cpp_thread_attach(il2cpp_domain_get());
}

/* -------------------------------------------------------------- pad state */

static uint64_t g_buttons;
static float    g_lx, g_ly, g_rx, g_ry;

void cp_pad_set(uint64_t buttons, float lx, float ly, float rx, float ry) {
  g_buttons = buttons;
  g_lx = lx; g_ly = ly; g_rx = rx; g_ry = ry;
}

/* Joystick button index -> Switch button mask.
 *
 * The index order is Unity's Android mapping, where JoystickButtonN corresponds
 * to Android KEYCODE_BUTTON_A(96)+N: A, B, C, X, Y, Z, L1, R1, L2, R2, THUMBL,
 * THUMBR, START, SELECT, MODE.
 *
 * THIS IS THE TABLE TO EDIT ON FIRST BOOT. Two things can shift it: Nintendo
 * pads report A/B and X/Y transposed relative to the Xbox layout Android's
 * keycodes assume, and Rewired may remap again via whichever controller template
 * it matches from the reported joystick name. Rather than guess at both, this
 * ships the plain Android order and logs every button press with its index (see
 * CP_LOG_BUTTONS) so the real mapping can be read off the log in one session. */
static const uint64_t cp_button_map[KEYCODE_JOYSTICK_COUNT] = {
#if CP_BUTTON_MAP_UNITY_ORDER
  /* Unity's collapsed Android order. Unity's Input system does not expose the
   * raw KEYCODE_BUTTON_A+N sequence -- BUTTON_C and BUTTON_Z do not exist on any
   * real pad and Unity omits them, so L1/R1 land on 4/5, not 6/7. Binding them
   * at 6/7 put the shoulder buttons on Select/Start, which reads in game as
   * "L and R are not bound". A and B are 0/1 either way, which is why the face
   * buttons worked and concealed the shift. */
  HidNpadButton_B,          /*  0  A       = bottom face          */
  HidNpadButton_A,          /*  1  B       = right face           */
  HidNpadButton_Y,          /*  2  X       = left face            */
  HidNpadButton_X,          /*  3  Y       = top face             */
  HidNpadButton_L,          /*  4  L1 / LB                        */
  HidNpadButton_R,          /*  5  R1 / RB                        */
  HidNpadButton_Minus,      /*  6  Select                         */
  HidNpadButton_Plus,       /*  7  Start                          */
  HidNpadButton_StickL,     /*  8  L3                             */
  HidNpadButton_StickR,     /*  9  R3                             */
  HidNpadButton_ZL,         /* 10  L2 / LT                        */
  HidNpadButton_ZR,         /* 11  R2 / RT                        */
  0,                        /* 12  Mode (Home) -- do not bind     */
  0, 0, 0, 0, 0, 0, 0       /* 13..19 unused                      */
#else
  HidNpadButton_B,          /*  0  Android BUTTON_A  = bottom face */
  HidNpadButton_A,          /*  1  Android BUTTON_B  = right face  */
  0,                        /*  2  Android BUTTON_C  -- absent     */
  HidNpadButton_Y,          /*  3  Android BUTTON_X  = left face   */
  HidNpadButton_X,          /*  4  Android BUTTON_Y  = top face    */
  0,                        /*  5  Android BUTTON_Z  -- absent     */
  HidNpadButton_L,          /*  6  L1                              */
  HidNpadButton_R,          /*  7  R1                              */
  HidNpadButton_ZL,         /*  8  L2                              */
  HidNpadButton_ZR,         /*  9  R2                              */
  HidNpadButton_StickL,     /* 10  THUMBL                          */
  HidNpadButton_StickR,     /* 11  THUMBR                          */
  HidNpadButton_Plus,       /* 12  START                           */
  HidNpadButton_Minus,      /* 13  SELECT                          */
  0,                        /* 14  MODE (Home) -- do not bind      */
  0, 0, 0, 0, 0             /* 15..19 unused                       */
#endif
};
/* Confirmed on hardware from debug.log: A(0x1)->1, B(0x2)->0, X(0x4)->4,
 * R(0x80)->7, Plus(0x400)->12 -- all exactly as the table below declares. The
 * per-press line has done its job, and leaving it on would force an SD write on
 * every button press now that [input] flushes immediately. */
#define CP_LOG_BUTTONS 0

static uint64_t g_prev_buttons;

static int joystick_index_from_keycode(int keycode) {
  if (keycode >= KEYCODE_JOYSTICK1_BASE &&
      keycode < KEYCODE_JOYSTICK1_BASE + KEYCODE_JOYSTICK_COUNT)
    return keycode - KEYCODE_JOYSTICK1_BASE;
  if (keycode >= KEYCODE_JOYSTICK_ANY_BASE &&
      keycode < KEYCODE_JOYSTICK_ANY_BASE + KEYCODE_JOYSTICK_COUNT)
    return keycode - KEYCODE_JOYSTICK_ANY_BASE;
  return -1;
}

/* ------------------------------------------------------------------ hooks */

static void *(*orig_GetJoystickNames)(void);
static uint8_t (*orig_GetKey)(int);
static uint8_t (*orig_GetKeyDown)(int);
static uint8_t (*orig_GetKeyUp)(int);
static float (*orig_GetAxis)(void *);
static float (*orig_GetAxisRaw)(void *);

/* Rewired matches this against its controller templates. "Nintendo Switch Pro
 * Controller" is the name an actual Pro Controller reports over Bluetooth on
 * Android, and it is what Rewired's Nintendo template expects -- which is what
 * lights up the JOYSTICK_ELEMENT_NAME_NINTENDO_* prompts the game already ships.
 * If prompts come out as Xbox glyphs, this string is why. */
static const char *CP_JOYSTICK_NAME = "Nintendo Switch Pro Controller";

static void *hk_GetJoystickNames(void) {
  if (!g_bound)
    return orig_GetJoystickNames ? orig_GetJoystickNames() : NULL;
  ensure_attached();
  void *sc = string_class();
  if (!sc)
    return orig_GetJoystickNames ? orig_GetJoystickNames() : NULL;

  void *arr = il2cpp_array_new(sc, 1);
  if (!arr)
    return NULL;
  IL2CPP_ARRAY_DATA(arr)[0] = il2cpp_string_new(CP_JOYSTICK_NAME);
  return arr;
}

static uint8_t hk_GetKey(int keycode) {
  int idx = joystick_index_from_keycode(keycode);
  if (idx < 0)
    return orig_GetKey ? orig_GetKey(keycode) : 0;
  uint64_t mask = cp_button_map[idx];
  return mask && (g_buttons & mask) ? 1 : 0;
}

static uint8_t hk_GetKeyDown(int keycode) {
  int idx = joystick_index_from_keycode(keycode);
  if (idx < 0)
    return orig_GetKeyDown ? orig_GetKeyDown(keycode) : 0;
  uint64_t mask = cp_button_map[idx];
  return mask && (g_buttons & mask) && !(g_prev_buttons & mask) ? 1 : 0;
}

static uint8_t hk_GetKeyUp(int keycode) {
  int idx = joystick_index_from_keycode(keycode);
  if (idx < 0)
    return orig_GetKeyUp ? orig_GetKeyUp(keycode) : 0;
  uint64_t mask = cp_button_map[idx];
  return mask && !(g_buttons & mask) && (g_prev_buttons & mask) ? 1 : 0;
}

/* Il2CppString: { Il2CppObject obj; int32_t length; uint16_t chars[]; }
 * so on 64-bit, length is at +0x10 and the UTF-16 body at +0x14. */
static int managed_str_to_ascii(void *s, char *out, size_t cap) {
  if (!s || cap == 0)
    return 0;
  int32_t len = *(int32_t *)((uintptr_t)s + 0x10);
  const uint16_t *w = (const uint16_t *)((uintptr_t)s + 0x14);
  if (len < 0)
    return 0;
  size_t n = 0;
  for (int32_t i = 0; i < len && n + 1 < cap; i++)
    out[n++] = (w[i] < 128) ? (char)w[i] : '?';
  out[n] = 0;
  return (int)n;
}

/* Rewired composes axis names at runtime, so match structurally rather than
 * against fixed literals: any name containing "analog"/"axis" plus a trailing
 * index is treated as joystick axis N.
 *
 * THE INDEX IS 1-BASED. Confirmed on hardware -- Rewired asks for exactly
 * "Joy1Axis1" .. "Joy1Axis9" (plus "MouseAxis1..3", which we ignore). The
 * previous table was 0-based, so every axis was answered one slot early:
 * axis 1 got left-Y, axis 2 got right-X, axis 3 got right-Y, axis 4 got nothing.
 * Pushing the left stick sideways therefore produced NO horizontal value at all
 * and only diagonal pushes moved anything -- which is why the in-game cursor
 * felt both reversed and extremely slow.
 *
 * THE Y AXES ARE NEGATED. libnx reports the analogue sticks with +Y up; Android
 * AXIS_Y is normalised the other way, -1 at the top, and Rewired's template is
 * written against the Android convention. Without the negation the sticks are
 * correct horizontally and inverted vertically.
 *
 * Right-stick placement is the one part not pinned by the log: axis 3 is
 * AXIS_Z on every Android pad, but vertical is AXIS_RZ, which Unity exposes as
 * axis 4 on most devices and axis 6 on some. 4 is the common case and is what
 * ships here. If the right stick reads horizontally but not vertically, move
 * `case 4` to `case 6` -- that is the whole fix, and nothing else depends on it.
 * Axes 5..9 stay 0: the D-pad is somewhere in there (hat X/Y) but which pair is
 * not established, and feeding a stick value to a trigger axis would be worse
 * than leaving it silent. */
/* First full-deflection reading per axis, once each. The name log above only
 * ever samples at rest, so it says nothing about range; this confirms the game
 * actually sees ~1000 rather than a fraction. */
static void log_axis_peak_once(int axis, float v) {
  static unsigned seen;
  if (axis < 1 || axis > 8) return;
  if (v > -0.5f && v < 0.5f) return;
  if (seen & (1u << axis)) return;
  seen |= 1u << axis;
  debugPrintf("[input] axis %d reached %d/1000 (full deflection should be ~1000)\n",
              axis, (int)(v * 1000.0f));
}

static float axis_value(const char *name) {
  /* MOUSE AXES MUST READ ZERO.
   *
   * Rewired polls "MouseAxis1".."MouseAxis3" every frame alongside the joystick
   * axes, and the old trailing-digit parse answered them with live stick data.
   * That is wrong twice over. The touch hooks already report mousePresent=0 and
   * touchSupported=0, so claiming mouse movement contradicts them -- and Unity
   * mouse axes are per-frame DELTAS in pixels, not a normalised -1..1, so a
   * stick at full deflection was asking the cursor to move ONE PIXEL PER FRAME.
   * That is the "cursor crawls, like analog isn't mapped" symptom: the game has
   * native analog stick support and was being driven through the mouse channel
   * instead, at 1 px/frame, fighting it.
   *
   * The 1-based fix made this worse rather than better, because it fed the mouse
   * channel a clean left-stick X/Y instead of the previous scrambled pair. */
  if (strstr(name, "Mouse") || strstr(name, "mouse"))
    return 0.0f;

  const char *p = name + strlen(name);
  while (p > name && (p[-1] >= '0' && p[-1] <= '9'))
    p--;
  if (!*p)
    return 0.0f;
  int axis = atoi(p);
  float v;
  switch (axis) {
    case 1: v = g_lx; break;
    case 2: v = CP_STICK_Y_INVERT ? -g_ly : g_ly; break;
    case 3: v = g_rx; break;
    case 4: v = CP_STICK_Y_INVERT ? -g_ry : g_ry; break;
    default: return 0.0f;
  }
  log_axis_peak_once(axis, v);
  return v;
}

static void log_axis_name_once(const char *name, int axis, float v) {
  static char seen[12][32];
  static int  nseen;
  for (int i = 0; i < nseen; i++)
    if (!strcmp(seen[i], name)) return;
  if (nseen < 12) {
    strncpy(seen[nseen], name, sizeof seen[0] - 1);
    seen[nseen][sizeof seen[0] - 1] = 0;
    nseen++;
    debugPrintf("[input] axis query \"%s\" -> index %d, value %d/1000\n",
                name, axis, (int)(v * 1000.0f));
  }
}

static float hk_GetAxisRaw(void *name_str) {
  char buf[64];
  if (!managed_str_to_ascii(name_str, buf, sizeof buf))
    return orig_GetAxisRaw ? orig_GetAxisRaw(name_str) : 0.0f;
  if (!strstr(buf, "oystick") && !strstr(buf, "nalog") && !strstr(buf, "xis")) {
    log_axis_name_once(buf, -1, 0.0f);
    return orig_GetAxisRaw ? orig_GetAxisRaw(name_str) : 0.0f;
  }
  float v = axis_value(buf);
  { const char *p = buf + strlen(buf);
    while (p > buf && p[-1] >= '0' && p[-1] <= '9') p--;
    log_axis_name_once(buf, *p ? atoi(p) : -1, v); }
  return v;
}

static float hk_GetAxis(void *name_str) {
  return hk_GetAxisRaw(name_str);
}

/* Called from cloverpit_input.c after cp_pad_set, once per frame. */
void cp_il2cpp_frame_end(void) {
#if CP_LOG_BUTTONS
  uint64_t rising = g_buttons & ~g_prev_buttons;
  if (rising) {
    for (int i = 0; i < KEYCODE_JOYSTICK_COUNT; i++)
      if (cp_button_map[i] && (rising & cp_button_map[i]))
        debugPrintf("[input] joystick button %d down (HID mask 0x%llx)\n",
                    i, (unsigned long long)cp_button_map[i]);
  }
#endif
  g_prev_buttons = g_buttons;
}

/* -------------------------------------------------------------- installer */

static int install_one(so_module *m, const char *name, uint32_t rva,
                       uint32_t guard, void *hook, void **orig_out) {
  uintptr_t addr = (uintptr_t)m->load_virtbase + rva;
  uint32_t got = *(volatile uint32_t *)addr;
  if (got != guard) {
    debugPrintf("[il2cpp] %-20s SKIP (guard 0x%08x, found 0x%08x)\n",
                name, guard, got);
    return 0;
  }
  if (orig_out)
    *orig_out = NULL;   /* no trampoline: these hooks fully replace the method */
  hook_arm64(addr, (uintptr_t)hook);
  debugPrintf("[il2cpp] %-20s hooked @ +0x%06x\n", name, rva);
  return 1;
}

/* Two-word variant. VideoPlayer::Play, ::Stop and VideoClip::get_length all
 * begin with the identical `str x30,[sp,#-0x20]!` prologue, so a one-word guard
 * cannot tell a correct RVA from one that has slid onto a neighbouring method.
 * The word at +0x10 is each method's own initialised-class flag load and is
 * distinct; checking both is what makes a stale offset fail safe here. */
static int install_two(so_module *m, const char *name, uint32_t rva,
                       uint32_t guard0, uint32_t guard4, void *hook,
                       void **orig_out) {
  uintptr_t addr = (uintptr_t)m->load_virtbase + rva;
  uint32_t w0 = *(volatile uint32_t *)addr;
  uint32_t w4 = *(volatile uint32_t *)(addr + 0x10);
  if (w0 != guard0 || w4 != guard4) {
    debugPrintf("[il2cpp] %-24s SKIP (guard %08x/%08x, found %08x/%08x)\n",
                name, guard0, guard4, w0, w4);
    return 0;
  }
  if (orig_out)
    *orig_out = NULL;   /* full replacement, same as install_one */
  hook_arm64(addr, (uintptr_t)hook);
  debugPrintf("[il2cpp] %-24s hooked @ +0x%06x\n", name, rva);
  return 1;
}

/* Verify a method we intend to CALL rather than patch. Calling a stale address
 * is strictly worse than writing to one, so the same guard applies. */
static int verify_two(so_module *m, const char *name, uint32_t rva,
                      uint32_t guard0, uint32_t guard4) {
  uintptr_t addr = (uintptr_t)m->load_virtbase + rva;
  uint32_t w0 = *(volatile uint32_t *)addr;
  uint32_t w4 = *(volatile uint32_t *)(addr + 0x10);
  if (w0 != guard0 || w4 != guard4) {
    debugPrintf("[il2cpp] %-24s NOT CALLABLE (guard %08x/%08x, found %08x/%08x)\n",
                name, guard0, guard4, w0, w4);
    return 0;
  }
  return 1;
}

/* See the header for why Noone rather than "pretend auth succeeded": claiming
 * the platform is initialised would send later achievement/leaderboard calls
 * into the Play Games path, which is the thing that cannot work. */
int cp_il2cpp_force_platform_none(so_module *il2cpp) {
  static const struct { const char *name; uint32_t rva; } SITES[] = {
    { "PlatformAPI::ApiKindGet",  RVA_PlatformAPI_ApiKindGet },
    { "Master::get__ApiKind",     RVA_Master_get__ApiKind    },
  };
  const int n = (int)(sizeof(SITES) / sizeof(SITES[0]));
  uintptr_t base = (uintptr_t)il2cpp->load_virtbase;

  for (int i = 0; i < n; i++) {
    uint32_t got = *(volatile uint32_t *)(base + SITES[i].rva);
    if (got != GUARD_ApiKind_mov_w0_5) {
      debugPrintf("[platform] ABORT: %s @+0x%07x expected %08x, found %08x "
                  "-- ApiKind left as GooglePlay\n",
                  SITES[i].name, SITES[i].rva, GUARD_ApiKind_mov_w0_5, got);
      return 0;
    }
  }
  int done = 0;
  for (int i = 0; i < n; i++) {
    uint32_t w = PATCH_ApiKind_mov_w0_0;
    if (so_patch_code((void *)(base + SITES[i].rva), &w, 4) < 0) {
      debugPrintf("[platform] FAILED writing %s\n", SITES[i].name);
      break;
    }
    debugPrintf("[platform] %-24s ApiKind 5 (GooglePlay) -> 0 (Noone)\n",
                SITES[i].name);
    done++;
  }
  return done;
}


/* --- Unholy Fusion DLC entitlement ---------------------------------------
 * See CP_OWN_UNHOLY_FUSION_DLC in config.h for what this asserts and why it is
 * necessary. Three call sites read this, all covered by hooking the entry:
 *   GeneralUiScript.<IntroAndTitleScreen_Coroutine>d__43::MoveNext()  (x2)
 *   PlatformAPI_Android::UpdateOwnsDlcs()
 *
 * Scoped by ProductId rather than blanket-returning true. CloverPit happens to
 * have exactly one product of this type -- CloverPitMobileInAppPurchases
 * .UnholyFusionDlc -- so the distinction makes no difference today, but a
 * getter that answers "unlocked" for every non-consumable is the kind of thing
 * that quietly becomes wrong when a game updates. Anything that is not the
 * Fusion DLC falls through to the original test.
 *
 * The ProductId is logged once so the substring match can be tightened to the
 * exact string on a later pass. */
static uint8_t (*orig_IAP_IsUnlocked)(void *) = NULL;

static uint8_t hk_IAP_IsUnlocked(void *self) {
  if (!self)
    return 0;
  /* Faithful reimplementation of the four instructions the detour overwrote:
   *     return this->OrderState == 4; */
  const int32_t order = *(volatile int32_t *)((char *)self + IAP_OFF_OrderState);
  const uint8_t real = (order == IAP_ORDERSTATE_PURCHASED) ? 1u : 0u;

#if CP_OWN_UNHOLY_FUSION_DLC
  void *pid = *(void **)((char *)self + IAP_OFF_ProductId);
  char id[96];
  if (pid && managed_str_to_ascii(pid, id, sizeof id)) {
    int fusion = 0;
    for (char *q = id; *q; q++)
      if ((q[0] == 'f' || q[0] == 'F') && !strncasecmp(q, "fusion", 6)) { fusion = 1; break; }
    static int logged = 0;
    if (!logged) {
      logged = 1;
      debugPrintf("[dlc] IsUnlocked(\"%s\") store=%s -> %s\n", id,
                  real ? "owned" : "unknown",
                  fusion ? "OWNED (CP_OWN_UNHOLY_FUSION_DLC=1)" : "passthrough");
    }
    if (fusion)
      return 1;
  }
#endif
  return real;
}

/* --- Splash video ---------------------------------------------------------
 *
 * These two REPLACE VideoPlayer::Play and ::Stop; nothing is called through.
 * That is deliberate on both counts:
 *
 *  - the engine's own Play would walk into AndroidVideoMedia, whose AMedia*
 *    imports are all stubbed to AMEDIA_ERROR_UNSUPPORTED. Letting it run buys
 *    nothing and risks the "surface creation stalled" wait libunity carries a
 *    message for;
 *  - IntroScript never reads VideoPlayer state back (no isPrepared, no
 *    isPlaying, no events -- see the header), so there is nothing for a
 *    passthrough to keep consistent.
 *
 * The clip is identified by asking the engine for its length and matching that
 * against the staged manifest, rather than by call order or by a field offset
 * into IntroScript. VideoClip::length is serialized metadata, so it answers
 * correctly even with the media backend dead -- it is the same value the game
 * itself uses to time the intro. */
typedef void   *(*fn_vp_get_clip)(void *self, void *method);
typedef double  (*fn_vc_get_length)(void *self, void *method);
static fn_vp_get_clip   vp_get_clip;
static fn_vc_get_length vc_get_length;

static void hk_VideoPlayer_Play(void *self, void *method) {
  (void)method;
  double len = 0.0;
  if (self && vp_get_clip && vc_get_length) {
    void *clip = vp_get_clip(self, NULL);
    if (clip)
      len = vc_get_length(clip, NULL);
  }
  cp_video_play(len);
}

static void hk_VideoPlayer_Stop(void *self, void *method) {
  (void)self;
  (void)method;
  cp_video_stop();
}

int cp_il2cpp_install_video_hooks(so_module *il2cpp) {
#if CP_VIDEO
  uintptr_t base = (uintptr_t)il2cpp->load_virtbase;

  /* Identification is optional: without it the player falls back to play order,
   * which is still correct for a two-clip intro. So a guard failure here
   * degrades the feature instead of disabling it. */
  if (verify_two(il2cpp, "VideoPlayer.get_clip", RVA_VideoPlayer_get_clip,
                 GUARD_VideoPlayer_get_clip, GUARD_VideoPlayer_get_clip_w4) &&
      verify_two(il2cpp, "VideoClip.get_length", RVA_VideoClip_get_length,
                 GUARD_VideoClip_get_length, GUARD_VideoClip_get_length_w4)) {
    vp_get_clip   = (fn_vp_get_clip)(base + RVA_VideoPlayer_get_clip);
    vc_get_length = (fn_vc_get_length)(base + RVA_VideoClip_get_length);
  } else {
    debugPrintf("[video] clip identification unavailable -- will use play order\n");
  }

  /* VERIFY FIRST, BOTH OR NEITHER -- the same shape as
   * cp_il2cpp_force_platform_none. Half a pair is worse than none here: Play
   * replaced while Stop stayed live would raise the overlay with no way to take
   * it down, and a skipped intro would cover the game. Checking both before
   * writing either means a moved RVA leaves the engine completely stock. */
  if (!verify_two(il2cpp, "VideoPlayer.Play", RVA_VideoPlayer_Play,
                  GUARD_VideoPlayer_Play, GUARD_VideoPlayer_Play_w4) ||
      !verify_two(il2cpp, "VideoPlayer.Stop", RVA_VideoPlayer_Stop,
                  GUARD_VideoPlayer_Stop, GUARD_VideoPlayer_Stop_w4)) {
    debugPrintf("[video] VideoPlayer hooks NOT applied -- splash video INACTIVE "
                "(game unaffected; the intro stays black, as it does today)\n");
    return 0;
  }

  install_two(il2cpp, "VideoPlayer.Play", RVA_VideoPlayer_Play,
              GUARD_VideoPlayer_Play, GUARD_VideoPlayer_Play_w4,
              (void *)hk_VideoPlayer_Play, NULL);
  install_two(il2cpp, "VideoPlayer.Stop", RVA_VideoPlayer_Stop,
              GUARD_VideoPlayer_Stop, GUARD_VideoPlayer_Stop_w4,
              (void *)hk_VideoPlayer_Stop, NULL);
  debugPrintf("[video] VideoPlayer Play/Stop replaced -- splash video active\n");
  return 1;
#else
  (void)il2cpp;
  debugPrintf("[video] disabled at compile time (CP_VIDEO=0)\n");
  return 0;
#endif
}

int cp_il2cpp_install_hooks(so_module *il2cpp) {
  if (!g_bound) {
    debugPrintf("[il2cpp] not bound -- no input hooks installed\n");
    return 0;
  }
  int n = 0;
  n += install_one(il2cpp, "GetJoystickNames", RVA_Input_GetJoystickNames,
                   GUARD_Input_GetJoystickNames, (void *)hk_GetJoystickNames,
                   (void **)&orig_GetJoystickNames);
  n += install_one(il2cpp, "GetKey", RVA_Input_GetKey,
                   GUARD_Input_GetKey, (void *)hk_GetKey, (void **)&orig_GetKey);
  n += install_one(il2cpp, "GetKeyDown", RVA_Input_GetKeyDown,
                   GUARD_Input_GetKeyDown, (void *)hk_GetKeyDown, (void **)&orig_GetKeyDown);
  n += install_one(il2cpp, "GetKeyUp", RVA_Input_GetKeyUp,
                   GUARD_Input_GetKeyUp, (void *)hk_GetKeyUp, (void **)&orig_GetKeyUp);
  n += install_one(il2cpp, "GetAxisRaw", RVA_Input_GetAxisRaw,
                   GUARD_Input_GetAxisRaw, (void *)hk_GetAxisRaw, (void **)&orig_GetAxisRaw);
  n += install_one(il2cpp, "GetAxis", RVA_Input_GetAxis,
                   GUARD_Input_GetAxis, (void *)hk_GetAxis, (void **)&orig_GetAxis);

  debugPrintf("[il2cpp] %d/6 input hooks applied\n", n);

#if CP_OWN_UNHOLY_FUSION_DLC
  if (install_one(il2cpp, "IAP.get_IsUnlocked", RVA_IAP_get_IsUnlocked,
                  GUARD_IAP_get_IsUnlocked, (void *)hk_IAP_IsUnlocked,
                  (void **)&orig_IAP_IsUnlocked)) {
    n++;
    debugPrintf("[dlc] Unholy Fusion entitlement hook installed -- Play Billing "
                "is unreachable here, so ownership is asserted by config\n");
  } else {
    debugPrintf("[dlc] entitlement hook NOT applied (guard mismatch) -- DLC will "
                "read as unowned\n");
  }
#endif
  return n;
}
