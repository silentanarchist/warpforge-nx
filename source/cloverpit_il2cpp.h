/* cloverpit_il2cpp.h -- managed-side hook targets in CloverPit's libil2cpp.so.
 *
 * RVAs come from Il2CppDumper's dump.cs for CloverPit 1.4.8 and were each checked
 * against libil2cpp.so by disassembling the entry. GUARD_* is the first
 * instruction word at that RVA; every installer verifies it before writing, so a
 * game update that moves these leaves the engine stock rather than corrupted.
 *
 * WHY HOOK MANAGED CODE AT ALL, given cloverpit_input.c prefers nativeInjectEvent?
 * Because the dump settles a question that guesswork had got wrong. CloverPit
 * drives input through Rewired, and Rewired's Android path is
 * UnityInputJoystickSource + ThreadSafeUnityInput + AndroidUnityInputHelper --
 * i.e. it reads UnityEngine.Input, plus a JNI query to android.hardware.input
 * .InputManager for device metadata. So UnityEngine.Input IS the surface Rewired
 * consults, and hooking it is the badpiggies_nx approach after all, just aimed at
 * the joystick methods rather than the touch ones.
 *
 * The two paths are complementary, not alternatives:
 *   - these hooks make a controller EXIST and report state;
 *   - nativeInjectEvent (cloverpit_input.c) is still the right route for touch.
 *
 * A NICE SURPRISE: CloverPit already ships Nintendo button prompts. Its
 * global-metadata.dat contains JOYSTICK_ELEMENT_NAME_NINTENDO_A / _B / _X / _Y /
 * _ZL / _ZR / _L / _R / _PLUS / _MINUS / _LEFT_STICK_* / _HOME. If the joystick
 * we present is identified by Rewired as a Nintendo pad, the game draws correct
 * Switch prompts with no further work.
 */
#ifndef CLOVERPIT_IL2CPP_H
#define CLOVERPIT_IL2CPP_H

#include <stdint.h>
#include "so_util.h"

typedef struct {
  const char *name;
  uint32_t    rva;
  uint32_t    guard;
} il2cpp_target;

/* UnityEngine.Input, CloverPit 1.4.8. Verified by disassembly. */
#define RVA_Input_GetJoystickNames   0x56547b4u
#define RVA_Input_GetKey             0x5654958u
#define RVA_Input_GetKeyDown         0x56549d0u
#define RVA_Input_GetKeyUp           0x5654994u
#define RVA_Input_GetAxis            0x5653f04u
#define RVA_Input_GetAxisRaw         0x5654074u
#define RVA_Input_GetMouseButton     0x5654700u
#define RVA_Input_get_mousePosition  0x5654bc0u
#define RVA_Input_get_touchCount     0x5655098u

#define GUARD_Input_GetJoystickNames  0xa9bf4ffeu  /* stp x30,x19,[sp,#-0x10]! */
#define GUARD_Input_GetKey            0xf81e0ffeu  /* str x30,[sp,#-0x20]!     */
#define GUARD_Input_GetKeyDown        0xf81e0ffeu
#define GUARD_Input_GetKeyUp          0xf81e0ffeu
#define GUARD_Input_GetAxis           0x14000001u  /* b .+4 (il2cpp pad thunk) */
#define GUARD_Input_GetAxisRaw        0x14000001u
#define GUARD_Input_GetMouseButton    0xf81e0ffeu
#define GUARD_Input_get_mousePosition 0xd10083ffu
#define GUARD_Input_get_touchCount    0xa9bf4ffeu

/* UnityEngine.KeyCode values for joystick buttons.
 *   JoystickButton0..19  = 330..349  (any joystick)
 *   Joystick1Button0..19 = 350..369  (joystick 1 specifically)
 * global-metadata.dat confirms Joystick1Button0..19 are present, which is what
 * Rewired's Unity joystick source polls. Both ranges are answered. */
#define KEYCODE_JOYSTICK_ANY_BASE   330
#define KEYCODE_JOYSTICK1_BASE      350
#define KEYCODE_JOYSTICK_COUNT      20

/* Bind the il2cpp runtime exports the hooks need to allocate managed values.
 * Call after libil2cpp is loaded + finalized. Returns 0 if all required APIs
 * resolved. */
int cp_il2cpp_bind(so_module *il2cpp);

/* Verify guards and install the hooks. Returns how many were applied.
 * Requires cp_il2cpp_bind() to have succeeded. */
int cp_il2cpp_install_hooks(so_module *il2cpp);

/* Called once per frame by cloverpit_input.c with the current pad state.
 * `buttons` is a libnx HidNpadButton mask; sticks are -1.0 .. 1.0. */
void cp_pad_set(uint64_t buttons, float lx, float ly, float rx, float ry);

/* Latch this frame's buttons as "previous" so GetKeyDown/GetKeyUp can report
 * edges. Call once per frame AFTER the engine has had a chance to poll. */
void cp_il2cpp_frame_end(void);

/* ---- Panik.PlatformAPI: force ApiKind GooglePlay(5) -> Noone(0) ----------
 * CloverPit ships one PlatformAPI with a backend per store; the enum is
 *     Noone=0 Steam=1 Gog=2 Epic=3 Itch=4 GooglePlay=5 AppleStore=6
 *     PlayStation=7 Xbox=8
 * and the Android build hard-returns 5. Both accessors are literally
 * `mov w0, #5 ; ret`, so this is a one-word patch each.
 *
 * WHY: on hardware the loading screen reached "Initializing PlatformAPI" and
 * stopped. PlatformAPI carries `_isAuthenticating` and a 20s LEADERBOARDS_TIMEOUT,
 * and the previous run logged
 *     [Play Games Plugin 2.1.0] Starting Auth using the method isAuthenticated
 * Google Play Games authentication cannot complete on a Switch: the plugin waits
 * on a Java callback that our JNI never delivers, so the gate never opens.
 *
 * Noone is the honest answer -- there IS no store platform here -- and it is a
 * path the developers ship and test for the DRM-free desktop builds, so it
 * degrades to local achievements (PlatformAPI has AchievementUnlockState_Local_*
 * for exactly this) rather than to an error screen. */
#define RVA_PlatformAPI_ApiKindGet   0x29b7d40u

/* NonConsumableInAppPurchase::get_IsUnlocked() -- the DLC entitlement read.
 * The whole method is four instructions:
 *     ldr w8, [x0, #0x20]   ; this->OrderState
 *     cmp w8, #4            ; == 4 (purchased)
 *     cset w0, eq
 *     ret
 * i.e. exactly 16 bytes, 0x29060a8..0x29060b8, with the next method's prologue
 * at 0x29060b8. A 16-byte hook_arm64 detour fits precisely and does not spill
 * into the neighbour -- but it does overwrite all four instructions, so the
 * fallback path below reimplements them rather than calling through. */
#define RVA_IAP_get_IsUnlocked       0x29060a8u
#define GUARD_IAP_get_IsUnlocked     0xb9402008u  /* ldr w8,[x0,#0x20]        */
#define IAP_OFF_ProductId            0x10u        /* readonly string ProductId */
#define IAP_OFF_OrderState           0x20u        /* OrderState enum           */
#define IAP_ORDERSTATE_PURCHASED     4
#define RVA_Master_get__ApiKind      0x29b762cu
#define GUARD_ApiKind_mov_w0_5       0x528000a0u   /* mov w0, #5 */
#define PATCH_ApiKind_mov_w0_0       0x52800000u   /* mov w0, #0 (Noone) */

/* Verify-first, both-or-neither. Returns the number of accessors patched. */
int cp_il2cpp_force_platform_none(so_module *il2cpp);

/* ---- UnityEngine.Video.VideoPlayer ---------------------------------------
 * Only Panik.IntroScript uses VideoPlayer, and disassembling
 * IntroScript.<IntroCoroutine>d__32::MoveNext (0x29D1A30, 9904 bytes) shows the
 * COMPLETE set of VideoPlayer calls it makes:
 *
 *     Play x2   Stop x4   SetDirectAudioVolume x2   get_clip x1 -> get_length x1
 *
 * There is no Prepare, no prepareCompleted, no isPrepared, no isPlaying and no
 * loopPointReached anywhere in it. The intro is gated on a float timer compared
 * against developerVideoPlayer.clip.length, i.e. wall clock, and VideoClip
 * .length is serialized metadata (m_FrameCount / m_FrameRate) that answers
 * correctly with the media backend dead. THAT is why the game already walks
 * past the splash today, and why the hang risk recorded in milestone [V] does
 * not apply: nothing needs stubbing and nothing needs a completion signal.
 *
 * Play/Stop are REPLACED, not detoured. Replacing them also means libunity
 * never enters AndroidVideoMedia at all, so none of the AMedia stubs are
 * reached and "surface creation stalled" cannot happen.
 *
 * TWO-WORD GUARDS. The entry word of Play, Stop and get_length is the same
 * `str x30,[sp,#-0x20]!` prologue, so the first word alone cannot tell them
 * apart -- a shifted RVA would pass. The word at +0x10 is the per-method
 * initialised-class flag load, and it is distinct for each. Both are checked,
 * the same shape as GUARD_WaitVSync / GUARD_WaitVSync_ldr in cloverpit_patches.c.
 *
 * get_clip and get_length are NOT patched -- they are CALLED, from inside the
 * Play hook, to ask the engine how long the clip is so the right staged file is
 * chosen. Calling a stale address is worse than patching one, so they are
 * guard-checked before the first call and the whole identification step is
 * skipped (falling back to play order) if either fails. */
#define RVA_VideoPlayer_Play          0x58ed9dcu
#define RVA_VideoPlayer_Stop          0x58eda90u
#define RVA_VideoPlayer_get_clip      0x58ed3c0u
#define RVA_VideoClip_get_length      0x58ed30cu

#define GUARD_VideoPlayer_Play        0xf81e0ffeu  /* str  x30,[sp,#-0x20]!    */
#define GUARD_VideoPlayer_Play_w4     0x39501e68u  /* ldrb w8,[x19,#0x407]     */
#define GUARD_VideoPlayer_Stop        0xf81e0ffeu
#define GUARD_VideoPlayer_Stop_w4     0x39502268u  /* ldrb w8,[x19,#0x408]     */
#define GUARD_VideoPlayer_get_clip    0xa9be57feu  /* stp  x30,x21,[sp,#-0x20]! */
#define GUARD_VideoPlayer_get_clip_w4 0x39500268u  /* ldrb w8,[x19,#0x400]     */
#define GUARD_VideoClip_get_length    0xf81e0ffeu
#define GUARD_VideoClip_get_length_w4 0x394fc668u  /* ldrb w8,[x19,#0x3f1]     */

/* Install the splash-video hooks. Separate from cp_il2cpp_install_hooks so a
 * video failure can never take the input hooks down with it. Returns 1 if the
 * Play/Stop pair was applied. No-op (returns 0) when CP_VIDEO is 0. */
int cp_il2cpp_install_video_hooks(so_module *il2cpp);

#endif /* CLOVERPIT_IL2CPP_H */
