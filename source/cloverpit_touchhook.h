/* cloverpit_touchhook.h -- Switch pointer -> UnityEngine.Input mouse API.
 * See cloverpit_touchhook.c for why the mouse path rather than the touch path. */
#ifndef CLOVERPIT_TOUCHHOOK_H
#define CLOVERPIT_TOUCHHOOK_H

#include <stdint.h>
#include "so_util.h"

/* UnityEngine.Input, CloverPit 1.4.8 (Il2CppDumper, verified against the binary). */


int  cp_touchhook_install(so_module *il2cpp);

/* Sample the pointer. Call once per frame, AFTER cp_pump_input. */
void cp_touchhook_tick(void);

#endif /* CLOVERPIT_TOUCHHOOK_H */
