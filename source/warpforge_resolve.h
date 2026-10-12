/* warpforge_resolve.h -- managed method lookup by name through libil2cpp's own
 * reflection API. See warpforge_resolve.c. */
#ifndef WARPFORGE_RESOLVE_H
#define WARPFORGE_RESOLVE_H

#include "so_util.h"

/* Bind the API exports. Call once after libil2cpp is loaded. 0 on success. */
int wf_resolve_bind(so_module *il2cpp);

/* Non-zero once il2cpp_init has run (the domain exists). */
int wf_resolve_ready(void);

/* Native entry point of <ns>.<klass>::<method>(argc params) in <assembly>
 * (e.g. "UnityEngine.CoreModule.dll"), or NULL (logged). */
void *wf_resolve_method(const char *assembly, const char *ns, const char *klass,
                        const char *method, int argc);

/* Deferred hook installation (warpforge_hooks.c): called once per frame from
 * the render loop; installs the Time and pointer hooks on the first frame where
 * the runtime is up. */
void wf_hooks_tick(void);

#endif
