/* warpforge_softinput.c -- Unity's on-screen keyboard, backed by the Switch's.
 *
 * A Unity text field (TouchScreenKeyboard.Open, which uGUI/TMP input fields use
 * on Android) asks the Java UnityPlayer for showSoftInput(text, type, correction,
 * multiline, secure, alert, placeholder, characterLimit, ...). The Java side then
 * reports back through the natives the engine registered: nativeSetInputString
 * as the text changes, nativeSoftInputClosed when the user is done,
 * nativeSoftInputCanceled when they back out, and nativeSetKeyboardIsVisible
 * around it all.
 *
 * Here showSoftInput shows the system keyboard (swkbd, modal and blocking on the
 * calling thread, which is the engine's main thread) and plays the same
 * callbacks once it closes. The game's chat box and the mod's sign-in fields
 * both come through this path.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>
#include "util.h"
#include "jni_fake.h"

extern void *jni_lookup_unity_native(const char *name);
extern const char *jni_string_utf(void *jstr);
extern void *jni_make_string(const char *utf);

typedef void (*fn_void)(void *env, void *thiz);
typedef void (*fn_bool)(void *env, void *thiz, uint8_t v);
typedef void (*fn_str)(void *env, void *thiz, void *jstr);

static int g_open;
static unsigned g_logged;

/* Unity's TouchScreenKeyboardType; only the few that change the keyboard. */
enum { KT_DEFAULT = 0, KT_ASCII = 1, KT_NUMBERS_AND_PUNCT = 2, KT_URL = 3, KT_NUMBER_PAD = 4,
       KT_PHONE_PAD = 5, KT_NAME_PHONE_PAD = 6, KT_EMAIL = 7, KT_SOCIAL = 8, KT_SEARCH = 9, KT_DECIMAL_PAD = 10 };

void wf_softinput_show(void *thiz, const char *text, int type, int multiline, int secure,
                       const char *placeholder, int char_limit) {
  fn_bool set_visible = jni_lookup_unity_native("nativeSetKeyboardIsVisible");
  fn_str  set_string  = jni_lookup_unity_native("nativeSetInputString");
  fn_void closed      = jni_lookup_unity_native("nativeSoftInputClosed");
  fn_void canceled    = jni_lookup_unity_native("nativeSoftInputCanceled");
  if (!set_string || !closed) {
    debugPrintf("[keyboard] the engine's keyboard natives are not registered; no keyboard\n");
    return;
  }
  if (g_open) return;
  g_open = 1;
  if (g_logged < 5) { g_logged++;
    debugPrintf("[keyboard] showSoftInput type=%d multiline=%d secure=%d limit=%d text=\"%s\" hint=\"%s\"\n",
                type, multiline, secure, char_limit, text ? text : "", placeholder ? placeholder : ""); }
  if (set_visible) set_visible(fake_env, thiz, 1);

  static char out[2048];
  out[0] = 0;
  SwkbdConfig kbd;
  Result rc = swkbdCreate(&kbd, 0);
  if (R_SUCCEEDED(rc)) {
    if (secure)                     swkbdConfigMakePresetPassword(&kbd);
    else if (type == KT_NUMBER_PAD || type == KT_PHONE_PAD || type == KT_DECIMAL_PAD) {
      swkbdConfigMakePresetDefault(&kbd);
      swkbdConfigSetType(&kbd, SwkbdType_NumPad);
    }
    else                            swkbdConfigMakePresetDefault(&kbd);
    if (type == KT_EMAIL || type == KT_URL || type == KT_ASCII) swkbdConfigSetType(&kbd, SwkbdType_QWERTY);
    if (text && *text)              swkbdConfigSetInitialText(&kbd, text);
    if (placeholder && *placeholder) swkbdConfigSetGuideText(&kbd, placeholder);
    int limit = (char_limit > 0 && char_limit < (int)sizeof out - 1) ? char_limit : (int)sizeof out - 1;
    swkbdConfigSetStringLenMax(&kbd, (u32)limit);
    if (!multiline) swkbdConfigSetReturnButtonFlag(&kbd, 0);
    rc = swkbdShow(&kbd, out, sizeof out);
    swkbdClose(&kbd);
  }
  if (R_SUCCEEDED(rc)) {
    set_string(fake_env, thiz, jni_make_string(out));
    closed(fake_env, thiz);
    if (g_logged <= 5) debugPrintf("[keyboard] entered %u characters\n", (unsigned)strlen(out));
  } else {
    if (canceled) canceled(fake_env, thiz); else closed(fake_env, thiz);
    if (g_logged <= 5) debugPrintf("[keyboard] closed without text (0x%x)\n", rc);
  }
  if (set_visible) set_visible(fake_env, thiz, 0);
  g_open = 0;
}

int wf_softinput_is_open(void) { return g_open; }
