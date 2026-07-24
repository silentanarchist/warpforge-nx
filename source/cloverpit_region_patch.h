/* cloverpit_region_patch.h -- 256MB -> 64MB memory-region granularity for
 * CloverPit 1.4.8 (Unity 6000.2.6f2).  GENERATED -- tools/gen_region_patch.py.
 *
 * WHY IT IS REQUIRED (a memory-layout change alone cannot work)
 * Unity's allocator rounds every virtual reservation up to a 256 MB region. On
 * hardware CloverPit issued EIGHT reservations that each rounded to 0x1FFFF000
 * (512 MB - one page): 4088 MB on a console reporting 3189 MB total. No division
 * of arena/pool/window satisfies that. At 64 MB granularity the same eight cost
 * 512 MB.
 *
 * THE GRANULARITY IS NOT ONE CONSTANT
 * It reaches the code as seven different encodings and at three bit levels
 * (28 = 256MB regions; 36 and 40 = the higher-level block directory):
 *
 *     mov  w9,  #0xfffffff            256MB-1 round-up addend
 *     mov  w8,  #0x10000000           256MB literal
 *     and  x21, x9, #0x...f0000000    round up / mask to a region
 *     lsr  x8,  x1, #0x1c             pointer -> region index
 *     lsr  x8,  x1, #0x28             pointer -> directory index
 *     ubfx x10, x1, #0x1c, #0xc       region index bitfield extract
 *     mov  x11, #-0x1000000000    +   movk x11, #0x1000, lsl #16
 *     sub  x8,  x8, x10, lsl #28      region index -> byte offset
 *
 * An earlier revision of this table carried only FOUR of those and covered ten
 * sites. That would have shifted the reserve path to 64 MB while leaving the
 * ubfx index extractions and the 36/40-bit directory at 256 MB -- the allocator
 * would have booked blocks under one geometry and looked them up under another.
 *
 * HOW THESE WERE DERIVED
 * The transform rules are general (every granularity-derived quantity shifted
 * down two bits) and were validated by REPRODUCING PvZ Fusion's proven 21-word
 * table exactly: 21/21 granularity words, zero incorrect. Applied to a
 * symbolized 6000.2.6f2 reference they find 21 sites across ten memory-manager
 * functions -- LocalLowLevelAllocator::ReserveMemoryBlock, DynamicHeapAllocator
 * (ctor + Allocate), MemoryManager::VirtualAllocator (ReserveMemoryBlock,
 * GetMemoryBlockFromPointer, GetBlockInfoFromPointer, MarkMemoryBlocks),
 * MemoryManager::GetAllocatorContainingPtr, InitializeDefaultAllocators and
 * TLSAllocator::ThreadInitialize.
 *
 * The game binary is a different build configuration, so its registers and
 * addresses differ. Scanning it with the same rules yields 111 candidate
 * clusters; the one below is the single cluster containing all five sites that
 * were independently located by masked opcode fingerprint, and it holds exactly
 * 21 sites -- the same count as the reference. The other clusters are ordinary
 * shift-by-28 code elsewhere in the engine and must NOT be touched.
 *
 * ALL 21 OR NONE. The installer verifies every `from` word before writing any.
 */
#ifndef CLOVERPIT_REGION_PATCH_H
#define CLOVERPIT_REGION_PATCH_H

#include <stdint.h>

typedef struct { uint32_t off, from, to; } NxPatchWord;

static const NxPatchWord CP_REGION_WORDS[] = {
  { 0x0561dc8, 0x12be0009, 0x12bf8009 },  /* mov w9, #0xfffffff                 LocalLowLevelAllocator::ReserveMemoryBlock [fp] */
  { 0x0561dd8, 0x92648d35, 0x92669535 },  /* and x21, x9, #0xfffffffff0000000   LocalLowLevelAllocator::ReserveMemoryBlock [fp] */
  { 0x05626bc, 0x52a20008, 0x52a08008 },  /* mov w8, #0x10000000                 */
  { 0x05640c0, 0x52a20009, 0x52a08009 },  /* mov w9, #0x10000000                 */
  { 0x0566610, 0x52a20009, 0x52a08009 },  /* mov w9, #0x10000000                DynamicHeapAllocator::DynamicHeapAllocator [fp] */
  { 0x0566adc, 0x12be000d, 0x12bf800d },  /* mov w13, #0xfffffff                 */
  { 0x0566b08, 0x92648d36, 0x92669536 },  /* and x22, x9, #0xfffffffff0000000    */
  { 0x0568ab8, 0xd35cdc33, 0xd35ad433 },  /* ubfx x19, x1, #0x1c, #0x1c          */
  { 0x0568ac0, 0xd35cfd15, 0xd35afd15 },  /* lsr x21, x8, #0x1c                  */
  { 0x0568b64, 0x52a20008, 0x52a08008 },  /* mov w8, #0x10000000                VirtualAllocator::ReserveMemoryBlock [fp] */
  { 0x0568e98, 0xd35cfc28, 0xd35afc28 },  /* lsr x8, x1, #0x1c                  VirtualAllocator::GetMemoryBlockFromPointer [fp] */
  { 0x0568ea8, 0x92646c28, 0x92667428 },  /* and x8, x1, #0xfffffff0000000       */
  { 0x0568eb0, 0xd35c9c2a, 0xd35a942a },  /* ubfx x10, x1, #0x1c, #0xc           */
  { 0x0568ec4, 0xd35cdc29, 0xd35ad429 },  /* ubfx x9, x1, #0x1c, #0x1c           */
  { 0x0568ec8, 0xb25c6feb, 0xb25e77eb },  /* mov x11, #-0x1000000000             */
  { 0x0568ecc, 0xf2a2000b, 0xf2a0800b },  /* movk x11, #0x1000, lsl #16          */
  { 0x0568f10, 0xcb0a7108, 0xcb0a6908 },  /* sub x8, x8, x10, lsl #28            */
  { 0x0568f24, 0xd368fc28, 0xd366fc28 },  /* lsr x8, x1, #0x28                   */
  { 0x0568f34, 0xd35c9c29, 0xd35a9429 },  /* ubfx x9, x1, #0x1c, #0xc            */
  { 0x056a8e0, 0xd368fc28, 0xd366fc28 },  /* lsr x8, x1, #0x28                   */
  { 0x056a8f8, 0xd35c9e89, 0xd35a9689 },  /* ubfx x9, x20, #0x1c, #0xc           */
};
#define CP_REGION_WORDS_N (sizeof(CP_REGION_WORDS)/sizeof(CP_REGION_WORDS[0]))

/* Must equal MMAP_ARENA_ALIGN in config.h: the arena hands back region-aligned
 * reservations and a mismatch means Unity trims pages the arena thinks are live. */
#define CP_REGION_GRANULARITY_MB 64

#endif /* CLOVERPIT_REGION_PATCH_H */
