/* cloverpit_touchhook.h -- Switch pointer -> UnityEngine.Input mouse API.
 * See cloverpit_touchhook.c for why the mouse path rather than the touch path. */
#ifndef CLOVERPIT_TOUCHHOOK_H
#define CLOVERPIT_TOUCHHOOK_H

#include <stdint.h>
#include "so_util.h"

/* UnityEngine.Input, CloverPit 1.4.8 (Il2CppDumper, verified against the binary). */
#define RVA_Input_GetMouseButton      0x5654700u
#define RVA_Input_GetMouseButtonDown  0x565473cu
#define RVA_Input_GetMouseButtonUp    0x5654778u
#define RVA_Input_get_mousePosition   0x5654bc0u
#define RVA_Input_get_mousePresent    0x5654fc0u
#define RVA_Input_get_touchSupported  0x565502cu
#define RVA_Input_get_touchCount      0x5655098u

#define GUARD_Input_str_prologue      0xf81e0ffeu  /* str x30,[sp,#-0x20]!      */
#define GUARD_Input_stp_prologue      0xa9bf4ffeu  /* stp x30,x19,[sp,#-0x10]!  */
#define GUARD_Input_get_mousePosition 0xd10083ffu  /* sub sp, sp, #0x20         */

int  cp_touchhook_install(so_module *il2cpp);

/* Sample the pointer. Call once per frame, AFTER cp_pump_input. */
void cp_touchhook_tick(void);

#endif /* CLOVERPIT_TOUCHHOOK_H */
