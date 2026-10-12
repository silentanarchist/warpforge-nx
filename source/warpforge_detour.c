/* warpforge_detour.c -- function detours WITH a way back to the original.
 *
 * hook_arm64 (so_util.c) overwrites the first 16 bytes of a function with
 *     LDR X17, #8 ; BR X17 ; .quad dst
 * and that is all the CloverPit-style hooks need: they replace a function
 * wholesale. A Harmony-style patch needs the original too ("prefix, then call
 * the real thing"). So this builds a TRAMPOLINE: the four displaced
 * instructions, re-encoded where they depend on their own address, followed
 * by a jump to target+16. Calling the trampoline is calling the original.
 *
 * Trampolines live in a small region made with libnx's JIT API (W^X: written
 * through the RW alias, executed through the RX alias). 64 slots of 128 bytes.
 *
 * Relocated instruction classes (AArch64, from the first 16 bytes of a
 * function, i.e. almost always a prologue):
 *   ADR / ADRP            -> LDR Xd, =absolute   (literal pool at slot end)
 *   B                     -> LDR X17, =dest ; BR X17
 *   BL                    -> LDR X17, =dest ; BLR X17
 *   LDR (literal)         -> LDR Xt, =addr ; LDR Xt/Wt/.., [Xt]  (int only;
 *                            FP/SIMD literal loads are refused)
 *   B.cond / CBZ / CBNZ / TBZ / TBNZ
 *                         -> inverted test over an absolute jump
 *   everything else        copied verbatim (not PC-relative)
 * An instruction that cannot be handled refuses the hook (returns NULL) and
 * logs the word; nothing is half-patched: the target is only written after the
 * trampoline is complete.
 *
 * MIT.
 */
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <switch.h>
#include "so_util.h"
#include "util.h"

#define SLOT_BYTES   128
#define SLOT_COUNT   1024        /* the mod alone applies ~215 Harmony hooks */
#define REGION_BYTES (SLOT_BYTES * SLOT_COUNT)
#define DISPLACED    4           /* instructions hook_arm64 overwrites */

/* One line per hook was ~215 lines (x2 with Il2CppInterop's own warning) for the
 * mod alone; the first few are enough to see that hooking works. */
static int g_verbose_left = 12;
#define g_verbose (g_verbose_left > 0 ? (g_verbose_left--, 1) : 0)

static Jit      g_jit;
static int      g_jit_ok;
static unsigned g_slots_used;

/* What each hook overwrote, so it can be taken out again (Harmony re-patching a
 * method first disposes its old hook; without this the old and new patches
 * chained and both ran). Newest last; a target hooked twice has two entries. */
typedef struct { uintptr_t target; uint32_t orig[4]; } hooked_t;
static hooked_t g_hooked[SLOT_COUNT];
static unsigned g_hooked_n;

static int region_init(void) {
  if (g_jit_ok) return 0;
  Result rc = jitCreate(&g_jit, REGION_BYTES);
  if (R_FAILED(rc)) {
    debugPrintf("[detour] jitCreate failed: 0x%x\n", rc);
    return -1;
  }
  g_jit_ok = 1;
  debugPrintf("[detour] trampoline region: rw %p rx %p (%u slots)\n",
              jitGetRwAddr(&g_jit), jitGetRxAddr(&g_jit), SLOT_COUNT);
  return 0;
}

/* ---- tiny assembler ------------------------------------------------------ */
typedef struct {
  uint32_t *code;      /* RW address of the slot */
  uintptr_t rx;        /* RX address of the slot (what the CPU runs) */
  unsigned  n;         /* instruction words emitted */
  uint64_t  lits[8];   /* literal pool, appended after the code */
  unsigned  nlits;
} asm_t;

static void emit(asm_t *a, uint32_t w) { a->code[a->n++] = w; }

/* LDR Xd, =value : literal placed at the pool; offset patched at finish. */
static void emit_ldr_lit(asm_t *a, unsigned rd, uint64_t value) {
  a->lits[a->nlits] = value;
  /* encode LDR (literal, 64-bit): 0x58000000 | imm19<<5 | Rt ; imm19 fixed later */
  emit(a, 0x58000000u | (a->nlits << 21) /* placeholder: pool index in bits 21..23 */ | rd);
  a->nlits++;
}

static int finish(asm_t *a) {
  /* pool starts after the code, 8-byte aligned */
  unsigned pool_word = (a->n + 1) & ~1u;
  if ((pool_word + a->nlits * 2) * 4 > SLOT_BYTES) return -1;
  unsigned li = 0;
  for (unsigned i = 0; i < a->n; i++) {
    uint32_t w = a->code[i];
    if ((w & 0xff000000u) == 0x58000000u && (w & 0x00ffffe0u) == (li << 21)) {
      int32_t off_words = (int32_t)(pool_word + li * 2) - (int32_t)i;
      a->code[i] = 0x58000000u | (((uint32_t)off_words & 0x7ffff) << 5) | (w & 0x1f);
      li++;
    }
  }
  if (li != a->nlits) return -1;
  for (unsigned k = 0; k < a->nlits; k++)
    memcpy(a->code + pool_word + k * 2, &a->lits[k], 8);
  return 0;
}

static void emit_abs_jump(asm_t *a, uint64_t dest) {   /* LDR X17,=dest ; BR X17 */
  emit_ldr_lit(a, 17, dest);
  emit(a, 0xd61f0220u);
}

static int sign_extend(uint32_t v, unsigned bits) {
  return (int)(v << (32 - bits)) >> (32 - bits);
}

/* Relocate one instruction at `pc` into the trampoline. 0 ok, -1 unsupported. */
static int reloc(asm_t *a, uint32_t ins, uint64_t pc) {
  /* ADR / ADRP */
  if ((ins & 0x1f000000u) == 0x10000000u) {
    unsigned rd = ins & 0x1f;
    int64_t imm = ((int64_t)sign_extend(((ins >> 5) & 0x7ffff), 19) << 2) | ((ins >> 29) & 3);
    uint64_t val = (ins & 0x80000000u) ? ((pc & ~0xfffull) + (imm << 12)) : (pc + imm);
    emit_ldr_lit(a, rd, val);
    return 0;
  }
  /* B / BL */
  if ((ins & 0x7c000000u) == 0x14000000u) {
    uint64_t dest = pc + ((int64_t)sign_extend(ins & 0x3ffffff, 26) << 2);
    emit_ldr_lit(a, 17, dest);
    emit(a, (ins & 0x80000000u) ? 0xd63f0220u /* BLR X17 */ : 0xd61f0220u /* BR X17 */);
    return 0;
  }
  /* LDR (literal), integer: opc 00=W, 01=X, 10=LDRSW */
  if ((ins & 0x3b000000u) == 0x18000000u) {
    unsigned opc = ins >> 30, rt = ins & 0x1f;
    if (ins & 0x04000000u) return -1;              /* SIMD/FP literal */
    uint64_t addr = pc + ((int64_t)sign_extend((ins >> 5) & 0x7ffff, 19) << 2);
    emit_ldr_lit(a, rt, addr);
    if (opc == 0)      emit(a, 0xb9400000u | (rt << 5) | rt);   /* LDR Wt, [Xt] */
    else if (opc == 1) emit(a, 0xf9400000u | (rt << 5) | rt);   /* LDR Xt, [Xt] */
    else if (opc == 2) emit(a, 0xb9800000u | (rt << 5) | rt);   /* LDRSW Xt, [Xt] */
    else return -1;
    return 0;
  }
  /* B.cond */
  if ((ins & 0xff000010u) == 0x54000000u) {
    uint64_t dest = pc + ((int64_t)sign_extend((ins >> 5) & 0x7ffff, 19) << 2);
    unsigned cond = ins & 0xf;
    emit(a, 0x54000000u | (3u << 5) | (cond ^ 1));   /* B.!cond +12 (skip 3 words) */
    emit_abs_jump(a, dest);
    return 0;
  }
  /* CBZ / CBNZ */
  if ((ins & 0x7e000000u) == 0x34000000u) {
    uint64_t dest = pc + ((int64_t)sign_extend((ins >> 5) & 0x7ffff, 19) << 2);
    emit(a, (ins & 0xff00001fu) ^ 0x01000000u | (3u << 5));   /* inverted, +12 */
    emit_abs_jump(a, dest);
    return 0;
  }
  /* TBZ / TBNZ */
  if ((ins & 0x7e000000u) == 0x36000000u) {
    uint64_t dest = pc + ((int64_t)sign_extend((ins >> 5) & 0x3fff, 14) << 2);
    emit(a, (ins & 0xfff8001fu) ^ 0x01000000u | (3u << 5));   /* inverted, +12 */
    emit_abs_jump(a, dest);
    return 0;
  }
  /* anything else does not depend on its address */
  emit(a, ins);
  return 0;
}

/* Install: returns the trampoline (call it to run the original), NULL on refusal. */
void *wf_detour_install(void *target, void *replacement) {
  if (!target || !replacement) return NULL;
  if (region_init() < 0) return NULL;
  if (g_slots_used >= SLOT_COUNT) { debugPrintf("[detour] out of trampoline slots\n"); return NULL; }

  uint32_t orig[DISPLACED];
  memcpy(orig, target, sizeof orig);

  jitTransitionToWritable(&g_jit);
  asm_t a = {0};
  a.code = (uint32_t *)((uint8_t *)jitGetRwAddr(&g_jit) + g_slots_used * SLOT_BYTES);
  a.rx   = (uintptr_t)jitGetRxAddr(&g_jit) + g_slots_used * SLOT_BYTES;
  int ok = 1;
  for (unsigned i = 0; i < DISPLACED && ok; i++) {
    if (reloc(&a, orig[i], (uint64_t)target + i * 4) < 0) {
      debugPrintf("[detour] %p: cannot relocate word %u (%08x) -- hook refused\n", target, i, orig[i]);
      ok = 0;
    }
  }
  if (ok) emit_abs_jump(&a, (uint64_t)target + DISPLACED * 4);
  if (ok && finish(&a) < 0) { debugPrintf("[detour] %p: trampoline overflow\n", target); ok = 0; }
  armDCacheFlush(a.code, SLOT_BYTES);
  jitTransitionToExecutable(&g_jit);
  if (!ok) return NULL;
  g_slots_used++;

  hook_arm64((uintptr_t)target, (uintptr_t)replacement);
  if (g_hooked_n < SLOT_COUNT) {
    g_hooked[g_hooked_n].target = (uintptr_t)target;
    memcpy(g_hooked[g_hooked_n].orig, orig, sizeof orig);
    g_hooked_n++;
  }
  if (g_verbose)
    debugPrintf("[detour] %p -> %p, original via trampoline %p (%08x %08x %08x %08x)\n",
                target, replacement, (void *)a.rx, orig[0], orig[1], orig[2], orig[3]);
  return (void *)a.rx;
}

/* Take the newest hook on `target` out: put back the 16 bytes it replaced.
 * The trampoline slot is not reused. Returns 1 if a hook was removed. */
int wf_detour_remove(void *target) {
  for (int i = (int)g_hooked_n - 1; i >= 0; i--) {
    if (g_hooked[i].target != (uintptr_t)target) continue;
    so_patch_code(target, g_hooked[i].orig, sizeof g_hooked[i].orig);
    memmove(&g_hooked[i], &g_hooked[i + 1], (g_hooked_n - (unsigned)i - 1) * sizeof g_hooked[0]);
    g_hooked_n--;
    if (g_verbose) debugPrintf("[detour] %p: hook removed\n", target);
    return 1;
  }
  return 0;
}

/* Il2CppInterop's FastPath (owner's fork, FastPath.cs): a few instructions in
 * front of a managed hook that send the common case straight back to the game's
 * function, so tens of millions of calls during loading never enter managed code
 * -- far more important here than on a phone, since managed code is interpreted.
 * The phone builds the stub with mmap/mprotect; here it lives in the JIT region.
 * kind 1: managed only when *(int64*)x0 < 0; kind 2: managed only when w0 < 0.
 * Returns the trampoline to the game's own function (NULL on refusal). */
void *wf_fastpath_install(void *target, void *managed, int kind) {
  static const uint32_t via_pointer[8] = {
    0xF9400010, 0xB7F80090, 0x580000D1, 0xF9400631, 0xD61F0220, 0x58000071, 0xF9400231, 0xD61F0220 };
  static const uint32_t via_number[8] = {
    0x37F80080, 0x580000F1, 0xF9400631, 0xD61F0220, 0x58000091, 0xF9400231, 0xD61F0220, 0xD503201F };
  if (kind != 1 && kind != 2) return NULL;
  if (region_init() < 0 || g_slots_used >= SLOT_COUNT) return NULL;
  uintptr_t *slots = malloc(2 * sizeof(uintptr_t));        /* plain data, read by the stub */
  if (!slots) return NULL;
  slots[0] = (uintptr_t)managed;
  slots[1] = (uintptr_t)managed;                             /* until the original is known */

  unsigned slot = g_slots_used++;
  jitTransitionToWritable(&g_jit);
  uint32_t *rw = (uint32_t *)((uint8_t *)jitGetRwAddr(&g_jit) + slot * SLOT_BYTES);
  memcpy(rw, kind == 1 ? via_pointer : via_number, 8 * 4);
  *(uint64_t *)(rw + 8) = (uint64_t)(uintptr_t)slots;        /* the "slots" literal after the code */
  armDCacheFlush(rw, SLOT_BYTES);
  jitTransitionToExecutable(&g_jit);
  void *stub = (uint8_t *)jitGetRxAddr(&g_jit) + slot * SLOT_BYTES;

  void *orig = wf_detour_install(target, stub);
  if (!orig) return NULL;
  slots[1] = (uintptr_t)orig;
  debugPrintf("[detour] %p: fast path in front of the managed hook (kind %d)\n", target, kind);
  return orig;
}
