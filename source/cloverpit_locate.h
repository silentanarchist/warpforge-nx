/* cloverpit_locate.h -- runtime resolution of engine functions in libunity.so */
#ifndef CLOVERPIT_LOCATE_H
#define CLOVERPIT_LOCATE_H

#include <stdint.h>
#include "so_util.h"

/* Order MUST match CLOVERPIT_TARGETS[] in cloverpit_offsets.h. */
typedef enum {
  CP_TM_UPDATE = 0,      /* TimeManager::Update(double)                  */
  CP_CHOREO_GET,         /* ChoreographerBase::Get()                     */
  CP_AUDIO_OUTTYPE,      /* AndroidAudio::GetAndroidAudioOutputType(int) */
  CP_FMOD_BUFGEOM,       /* FMOD OpenSL buffer-geometry bound check      */
  CP_TARGET_COUNT
} cp_target_id;

typedef enum {
  CP_UNRESOLVED = 0,
  CP_BY_FINGERPRINT,     /* found by opcode scan -- update-tolerant       */
  CP_BY_RVA              /* fell back to the verified 1.4.8 address       */
} cp_resolve_how;

extern uintptr_t g_cp_addr[CP_TARGET_COUNT];

/* Resolve every target. Returns how many landed. Call once, after
 * so_finalize()/so_flush_caches() on libunity and BEFORE any patch installer. */
int cp_locate_all(so_module *unity);

uintptr_t      cp_addr(int id);   /* 0 if unresolved */
cp_resolve_how cp_how(int id);

/* 256MB -> 64MB allocator region granularity. Must run BEFORE the engine's
 * memory manager initialises, i.e. before the init arrays. */
int cp_install_region_patch(so_module *unity);

#endif /* CLOVERPIT_LOCATE_H */
