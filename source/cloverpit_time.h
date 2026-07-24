/* cloverpit_time.h -- UnityEngine.Time driven from a wall clock.
 *
 * The engine's TimeManager::Update is never called on this port (proved by the
 * render-loop heartbeat), so every managed Time.* value stays frozen. These
 * hooks answer the managed getters from our own monotonic clock instead.
 * RVAs are from Il2CppDumper output for CloverPit 1.4.8 and each is guarded.
 */
#ifndef CLOVERPIT_TIME_H
#define CLOVERPIT_TIME_H

#include <stdint.h>
#include "so_util.h"

/* UnityEngine.Time accessors, CloverPit 1.4.8. All share the same prologue
 * guard (stp x30, x19, [sp, #-0x10]! == 0xa9bf4ffe). */
#define RVA_Time_get_time                   0x55e60f4u
#define RVA_Time_get_deltaTime              0x55e619cu
#define RVA_Time_get_unscaledTime           0x55e61c4u
#define RVA_Time_get_unscaledDeltaTime      0x55e61ecu
#define RVA_Time_get_fixedUnscaledDeltaTime 0x55e6214u
#define RVA_Time_get_fixedDeltaTime         0x55e623cu
#define RVA_Time_get_smoothDeltaTime        0x55e6264u
#define RVA_Time_get_timeScale              0x55e628cu
#define RVA_Time_get_frameCount             0x55e62ecu
#define RVA_Time_get_renderedFrameCount     0x55e6314u
#define RVA_Time_get_realtimeSinceStartup   0x55e3b78u

/* Install the hooks. Each is verified against its guard word independently, so a
 * moved getter is skipped rather than mispatched. Returns how many landed. */
int cp_time_install(so_module *il2cpp);

/* Sample deltaTime. Call ONCE per frame from the render loop. */
void cp_time_tick(void);

int     cp_time_installed(void);
float   cp_time_now(void);
float   cp_time_delta(void);
int32_t cp_time_frames(void);

#endif /* CLOVERPIT_TIME_H */
