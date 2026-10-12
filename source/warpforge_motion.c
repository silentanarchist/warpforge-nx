/* warpforge_motion.c -- real touch events for the engine (nativeInjectEvent).
 *
 * WHY, when the pointer already works through the UnityEngine.Input hooks:
 * those hooks feed the MANAGED Input class (what uGUI reads), not the engine's
 * native input manager. Unity's IMGUI (GUI.Window / GUI.TextField -- the mod's
 * sign-in and bug-report windows) takes its mouse events from the native side,
 * so it drew but could not be touched. Android feeds the native side through
 * UnityPlayer.nativeInjectEvent(env, thiz, MotionEvent, deviceId); Unity then
 * reads the event back through JNI getters. This file is that MotionEvent: one
 * fake object whose getters answer from the state set just before injecting.
 *
 * Only the primary finger is injected (ACTION_DOWN / MOVE / UP); uGUI keeps the
 * mouse hooks as before (the managed touch API stays reported as unsupported, so
 * nothing is delivered twice). Coordinates: Android's origin is top-left, the
 * pointer stream's is bottom-left, hence the flip.
 *
 * Which getters libunity 6000.2 calls is known from its string table
 * (getActionMasked, getActionIndex, getPointerCount, getPointerId, getX, getY,
 * getPressure, getSize, getToolType, getSource, getDeviceId, getEventTime,
 * getDownTime, getButtonState, getMetaState, getFlags, getEdgeFlags,
 * getHistorySize, getAxisValue, getDevice...); every call is also logged by the
 * fake JNI ("[jni] call:"), so an unexpected one shows up in debug.log.
 *
 * MIT.
 */
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <switch.h>
#include "config.h"
#include "util.h"
#include "cloverpit_touch.h"

#define ASOURCE_TOUCHSCREEN 0x1002
#define WF_DEVICE_TOUCH     2
#define TOOL_TYPE_FINGER    1

typedef struct {
  int   action;          /* 0 down, 1 up, 2 move */
  int   count;
  int   ids[NX_MAX_TOUCH];
  float x[NX_MAX_TOUCH], y[NX_MAX_TOUCH];   /* Android screen px, top-left origin */
  int64_t down_time, event_time;
} wf_motion_t;

static wf_motion_t g_ev;
static int         g_down;          /* a finger is down (between DOWN and UP) */
static int64_t     g_down_time;
static unsigned    g_logged;

static int64_t now_ms(void) { return (int64_t)(armGetSystemTick() / (armGetSystemTickFreq() / 1000)); }

extern void *jni_make_object(const char *label);

/* Is `id->cls`/`recv` ours? The receiver is the pooled "wf/motion" object; the
 * method ids come from android/view/MotionEvent or its base InputEvent. */
int wf_motion_is_event(const void *recv, const char *cls) {
  if (recv) {
    const char *label = (const char *)recv + 4;             /* FakeObject{tag,label} */
    if (*(const uint32_t *)recv == 0x434c5331u /* TAG_CLASS */ && !strcmp(label, "wf/motion")) return 1;
  }
  return cls && (strstr(cls, "view/MotionEvent") || strstr(cls, "view/InputEvent"));
}

/* Each event object carries its own snapshot: the engine keeps a copy
 * (MotionEvent.obtain) and reads it later, after the next frame's event has
 * replaced g_ev. Keyed by the object pointer; the ring is far larger than the
 * engine holds at once. */
#define SNAP_N 64
static struct { const void *obj; wf_motion_t ev; } g_snaps[SNAP_N];
static unsigned g_snap_next;
static void snap_store(const void *obj, const wf_motion_t *ev) {
  g_snaps[g_snap_next % SNAP_N].obj = obj; g_snaps[g_snap_next % SNAP_N].ev = *ev; g_snap_next++;
}
static const wf_motion_t *snap_find(const void *obj) {
  for (unsigned i = 0; i < SNAP_N; i++) if (g_snaps[i].obj == obj) return &g_snaps[i].ev;
  return &g_ev;
}
/* MotionEvent.obtain(event): a copy with the same state. recycle(): nothing. */
void *wf_motion_obtain(const void *src) {
  void *o = jni_make_object("wf/motion");
  snap_store(o, snap_find(src));
  return o;
}

static int idx_arg(va_list va, const wf_motion_t *e) { int i = va_arg(va, int); return (i >= 0 && i < e->count) ? i : 0; }

/* The first questions the engine asks an injected event, so a rejected touch can
 * be traced to the one it failed. */
static unsigned g_traced;
static void trace_call(const char *kind, const char *name) {
  if (g_traced < 40) { g_traced++; debugPrintf("[motion]   engine asks %s %s\n", kind, name); }
}

uint64_t wf_motion_int(const void *recv, const char *name, va_list va) {
  trace_call("int", name);
  const wf_motion_t *e = snap_find(recv);
  if (!strcmp(name, "getActionMasked"))  return (uint64_t)e->action;
  if (!strcmp(name, "getAction"))        return (uint64_t)e->action;        /* index 0 -> no high bits */
  if (!strcmp(name, "getActionIndex"))   return 0;
  if (!strcmp(name, "getPointerCount"))  return (uint64_t)e->count;
  if (!strcmp(name, "getPointerId"))     return (uint64_t)e->ids[idx_arg(va, e)];
  if (!strcmp(name, "getSource"))        return ASOURCE_TOUCHSCREEN;
  if (!strcmp(name, "getDeviceId"))      return WF_DEVICE_TOUCH;
  if (!strcmp(name, "getToolType"))      return TOOL_TYPE_FINGER;
  if (!strcmp(name, "getEventTime"))     return (uint64_t)e->event_time;
  if (!strcmp(name, "getDownTime"))      return (uint64_t)e->down_time;
  if (!strcmp(name, "getHistorySize"))   return 0;
  /* getButtonState, getMetaState, getFlags, getEdgeFlags, getActionButton, isXPressed: 0 */
  return 0;
}

float wf_motion_float(const void *recv, const char *name, va_list va) {
  trace_call("float", name);
  const wf_motion_t *e = snap_find(recv);
  if (!strcmp(name, "getX") || !strcmp(name, "getRawX")) return e->x[idx_arg(va, e)];
  if (!strcmp(name, "getY") || !strcmp(name, "getRawY")) return e->y[idx_arg(va, e)];
  if (!strcmp(name, "getPressure")) return e->action == 1 ? 0.0f : 1.0f;
  if (!strcmp(name, "getSize"))     return e->action == 1 ? 0.0f : 0.1f;
  if (!strcmp(name, "getAxisValue")) {
    int axis = va_arg(va, int); int i = idx_arg(va, e);
    switch (axis) {
      case 0: return e->x[i];                               /* AXIS_X */
      case 1: return e->y[i];                               /* AXIS_Y */
      case 2: return e->action == 1 ? 0.0f : 1.0f;          /* AXIS_PRESSURE */
      case 3: return e->action == 1 ? 0.0f : 0.1f;          /* AXIS_SIZE */
      default: return 0.0f;
    }
  }
  return 0.0f;   /* getOrientation, getHistorical* */
}

/* Called once per frame with this frame's touches (cloverpit_touch phases).
 * Injects at most one event per frame for the primary finger. */
typedef uint8_t (*fn_inject)(void *, void *, void *, int32_t);
static void *g_env, *g_thiz; static fn_inject g_inject;

/* After the keyboard: if a finger was down when it opened, send its UP now, so the
 * next tap starts a fresh gesture (IMGUI otherwise sees drags). */
void wf_motion_release(void) {
  if (!g_down || !g_inject) { g_down = 0; return; }
  g_down = 0;
  g_ev.action = 1; g_ev.count = 1; g_ev.ids[0] = 0; g_ev.event_time = now_ms(); g_ev.down_time = g_down_time;
  void *ev = jni_make_object("wf/motion");
  snap_store(ev, &g_ev);
  uint8_t handled = g_inject(g_env, g_thiz, ev, WF_DEVICE_TOUCH);
  debugPrintf("[motion] released after the keyboard -> handled=%u\n", handled);
}

void wf_motion_pump(void *env, void *thiz, fn_inject inject, const cp_touch *t, int n) {
  if (!inject || n <= 0) return;
  g_env = env; g_thiz = thiz; g_inject = inject;
  { extern int wf_softinput_is_open(void); if (wf_softinput_is_open()) return; }   /* the applet has the screen */
  const cp_touch *p = &t[0];
  int action;
  if (p->phase == CP_TOUCH_DOWN)      { action = 0; g_down = 1; g_down_time = now_ms(); }
  else if (p->phase == CP_TOUCH_UP)   { if (!g_down) return; action = 1; g_down = 0; }
  else if (p->phase == CP_TOUCH_MOVE) { if (!g_down) return; action = 2; }
  else return;

  g_ev.action = action;
  g_ev.count = 1;
  g_ev.ids[0] = 0;
  g_ev.x[0] = p->x;
  /* Measured on hardware: IMGUI reports a touch injected at y as 1080 + y (the
   * engine's flip runs against a zero height; nativeSetInputArea did not change
   * it). Handing it y - 1080, i.e. minus the bottom-left y, lands it where it
   * was tapped. */
  g_ev.y[0] = -p->y;
  g_ev.event_time = now_ms();
  g_ev.down_time = g_down_time;

  /* The engine flips touch y against the input area the Java view reports with
   * nativeSetInputArea(x, y, w, h); nothing had reported one, so the flip ran
   * against a height of 0 and IMGUI saw taps at y = 1080 + y. Report the whole
   * screen once, before the first event. */
  {
    static int area_set;
    if (!area_set) {
      area_set = 1;
      extern void *jni_lookup_unity_native(const char *name);
      void (*set_area)(void *, void *, int, int, int, int) = jni_lookup_unity_native("nativeSetInputArea");
      if (set_area) { set_area(env, thiz, 0, 0, CP_FORCE_SCREEN_W, CP_FORCE_SCREEN_H);
                      debugPrintf("[motion] input area set to %dx%d\n", CP_FORCE_SCREEN_W, CP_FORCE_SCREEN_H); }
      else debugPrintf("[motion] nativeSetInputArea not registered\n");
    }
  }
  void *ev = jni_make_object("wf/motion");
  snap_store(ev, &g_ev);
  uint8_t handled = inject(env, thiz, ev, WF_DEVICE_TOUCH);
  if (g_logged < 6) {
    g_logged++;
    debugPrintf("[motion] injected action=%d at (%.0f,%.0f) -> handled=%u\n", action, g_ev.x[0], g_ev.y[0], handled);
  }
}
