/* cloverpit_imports.c -- the imports libunity.so (Unity 6000.2.6f2) needs that
 * the 2020.3/2022.3 reference substrate does not already provide.
 *
 * Scope check, done by diffing every UND symbol in Cloverpit's libraries against
 * the union of the badpiggies_nx + ZookeeperDX_NX import tables:
 *
 *   libmain.so            0 new
 *   libil2cpp.so          0 new   <-- the whole il2cpp surface is already covered
 *   lib_burst_generated   0 new
 *   libunity.so          69 new   <-- THIS FILE (media NDK + 2 strays)
 *   libswappywrapper.so  60 new   <-- all libc++; see note at the bottom
 *
 * Everything here is a REFUSAL stub, not an implementation. Unity's media path
 * is the VideoPlayer/AVPro backend and the ARCore/Camera2 image reader; Cloverpit
 * references UnityEngine.Video, so the calls can happen -- they must fail
 * cleanly (return an error, hand back NULL) rather than fault. Getting a stub
 * wrong here is a hang or a silent corruption, not a build error, so keep the
 * return values honest.
 *
 * Wire into main.c:
 *     so_resolve(&unity_mod, cloverpit_imports, cloverpit_imports_num, 1);
 * ... or just append cloverpit_imports[] to the existing default_dynlib[].
 */

#include <stdint.h>
#include <stddef.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include "so_util.h"
#include "util.h"

/* AMediaCodec / AMediaExtractor error codes (NdkMediaError.h) */
#define AMEDIA_OK                        0
#define AMEDIA_ERROR_UNSUPPORTED  -10005
#define AMEDIA_ERROR_INVALID_OBJECT -10002

/* ---- media format key strings ------------------------------------------
 * NOTE: these are OBJECT symbols, not FUNC. The engine does
 *   ldr x0,[got]        ; x0 = &AMEDIAFORMAT_KEY_MIME
 *   ldr x0,[x0]         ; x0 = "mime"
 * so the import table must hand back the ADDRESS OF A POINTER VARIABLE.
 * Registering a plain string literal here yields a wild dereference. */

static const char *k_AMEDIAFORMAT_KEY_CHANNEL_COUNT = "channel_count";
static const char *k_AMEDIAFORMAT_KEY_COLOR_FORMAT = "color_format";
static const char *k_AMEDIAFORMAT_KEY_COLOR_RANGE = "color_range";
static const char *k_AMEDIAFORMAT_KEY_COLOR_STANDARD = "color_standard";
static const char *k_AMEDIAFORMAT_KEY_DURATION = "duration";
static const char *k_AMEDIAFORMAT_KEY_ENCODER_DELAY = "encoder_delay";
static const char *k_AMEDIAFORMAT_KEY_FRAME_RATE = "frame_rate";
static const char *k_AMEDIAFORMAT_KEY_HEIGHT = "height";
static const char *k_AMEDIAFORMAT_KEY_LANGUAGE = "language";
static const char *k_AMEDIAFORMAT_KEY_MIME = "mime";
static const char *k_AMEDIAFORMAT_KEY_ROTATION = "rotation";
static const char *k_AMEDIAFORMAT_KEY_SAMPLE_RATE = "sample_rate";
static const char *k_AMEDIAFORMAT_KEY_SLICE_HEIGHT = "slice_height";
static const char *k_AMEDIAFORMAT_KEY_STRIDE = "stride";
static const char *k_AMEDIAFORMAT_KEY_WIDTH = "width";

/* ---- media NDK: refuse everything ---------------------------------------
 * Returning AMEDIA_ERROR_UNSUPPORTED makes Unity's VideoPlayer report an
 * unplayable clip and continue. A VideoPlayer.Prepare() that never completes
 * will deadlock a coroutine that waits on prepareCompleted, so if Cloverpit
 * gates anything on a video finishing, hook the C# side instead (see
 * PORT_PLAN.md [V]). */
static int   stub_media_err(void) { return AMEDIA_ERROR_UNSUPPORTED; }
static void *stub_null(void)      { return NULL; }
static void  stub_void(void)      { }
static int   stub_zero(void)      { return 0; }
static int64_t stub_neg1_64(void) { return -1; }
/* Which videos does the game try to play, and when? (the end-of-round win/loss
 * animation does not show; on the phone it plays -- probably a VideoPlayer) */
static unsigned g_media_logged;
/* The extractor itself is a dummy: Unity only checks it is non-NULL, then hands
 * over the clip (setDataSourceFd / setDataSourceCustom). Each clip is copied once
 * to <root>/videos/clip-<length>-<offset>.mp4 -- so stills can be made from the
 * win/loss clips, which live in the downloaded bundles -- and then refused as
 * before, so playback still fails cleanly. */
static uint32_t g_dummy_extractor[4];
static void *log_extractor_new(void) {
  if (g_media_logged < 30) { g_media_logged++;
    debugPrintf("[video] the engine opened a video (AMediaExtractor_new)\n"); }
  return g_dummy_extractor;
}
static void clip_path(char *out, size_t n, long long len, long long off) {
  extern const char *cp_game_root(void);
  char dir[300]; snprintf(dir, sizeof dir, "%s/videos", cp_game_root());
  mkdir(dir, 0777);
  snprintf(out, n, "%s/clip-%lld-%lld.mp4", dir, len, off);
}
static int set_source_fd(void *ex, int fd, int64_t off, int64_t len) {
  (void)ex;
  char path[400]; clip_path(path, sizeof path, (long long)len, (long long)off);
  struct stat st;
  if (len > 0 && len < (256ll << 20) && stat(path, &st) != 0) {
    FILE *o = fopen(path, "wb");
    long long done = 0;
    if (o) {
      static char buf[65536];
      off_t keep = lseek(fd, 0, SEEK_CUR);
      if (lseek(fd, (off_t)off, SEEK_SET) >= 0) {
        while (done < len) {
          long want = (long)(len - done > (long long)sizeof buf ? (long long)sizeof buf : len - done);
          long r = read(fd, buf, (size_t)want);
          if (r <= 0) break;
          fwrite(buf, 1, (size_t)r, o); done += r;
        }
      }
      if (keep >= 0) lseek(fd, keep, SEEK_SET);
      fclose(o);
    }
    debugPrintf("[video] clip saved: %s (%lld of %lld bytes)\n", path, done, (long long)len);
  } else if (g_media_logged < 30) { g_media_logged++;
    debugPrintf("[video] clip fd=%d offset=%lld length=%lld (already saved as %s)\n", fd, (long long)off, (long long)len, path); }
  return AMEDIA_ERROR_UNSUPPORTED;
}
/* AMediaDataSource: Unity's callbacks for clips it streams itself. */
typedef int64_t (*ds_readat_fn)(void *ud, int64_t off, void *buf, size_t size);
typedef int64_t (*ds_getsize_fn)(void *ud);
typedef struct { void *ud; ds_readat_fn readat; ds_getsize_fn getsize; } fake_ds;
static void *ds_new(void) { return calloc(1, sizeof(fake_ds)); }
static void ds_delete(void *d) { free(d); }
static void ds_set_ud(void *d, void *ud) { if (d) ((fake_ds *)d)->ud = ud; }
static void ds_set_readat(void *d, void *f) { if (d) ((fake_ds *)d)->readat = (ds_readat_fn)f; }
static void ds_set_getsize(void *d, void *f) { if (d) ((fake_ds *)d)->getsize = (ds_getsize_fn)f; }
static int set_source_custom(void *ex, void *d) {
  (void)ex;
  fake_ds *ds = d;
  if (!ds || !ds->readat || !ds->getsize) { debugPrintf("[video] custom clip source without callbacks\n"); return AMEDIA_ERROR_UNSUPPORTED; }
  int64_t len = ds->getsize(ds->ud);
  static unsigned n_custom;
  char path[400]; clip_path(path, sizeof path, (long long)len, (long long)(1000000 + n_custom++));
  if (len > 0 && len < (256ll << 20)) {
    FILE *o = fopen(path, "wb");
    long long done = 0;
    if (o) {
      static char buf[65536];
      while (done < len) {
        size_t want = (size_t)(len - done > (int64_t)sizeof buf ? (int64_t)sizeof buf : len - done);
        int64_t r = ds->readat(ds->ud, done, buf, want);
        if (r <= 0) break;
        fwrite(buf, 1, (size_t)r, o); done += r;
      }
      fclose(o);
    }
    debugPrintf("[video] clip saved (custom source): %s (%lld of %lld bytes)\n", path, done, (long long)len);
  } else debugPrintf("[video] custom clip source of %lld bytes -- not saved\n", (long long)len);
  return AMEDIA_ERROR_UNSUPPORTED;
}
static int log_set_source(void *ex, const char *loc) {
  (void)ex;
  if (g_media_logged < 30) { g_media_logged++; debugPrintf("[video] video source %s\n", loc ? loc : "(null)"); }
  return AMEDIA_ERROR_UNSUPPORTED;
}


/* ANativeWindow_toSurface: hand back the fake Surface jobject the wrapper
 * already uses everywhere else. Returning NULL is safer than a bogus handle --
 * callers null-check, they do not validate. */
extern void *fake_surface_obj;   /* jni_fake.c */
static void *nx_ANativeWindow_toSurface(void *env, void *window) {
  (void)env; (void)window;
  return fake_surface_obj;
}

/* nextafter: real libm, do not stub. Unity uses it in float compare paths. */

/* ======================================================================== */
DynLibFunction cloverpit_imports[] = {
  { "AMEDIAFORMAT_KEY_CHANNEL_COUNT", (uintptr_t)&k_AMEDIAFORMAT_KEY_CHANNEL_COUNT },
  { "AMEDIAFORMAT_KEY_COLOR_FORMAT", (uintptr_t)&k_AMEDIAFORMAT_KEY_COLOR_FORMAT },
  { "AMEDIAFORMAT_KEY_COLOR_RANGE", (uintptr_t)&k_AMEDIAFORMAT_KEY_COLOR_RANGE },
  { "AMEDIAFORMAT_KEY_COLOR_STANDARD", (uintptr_t)&k_AMEDIAFORMAT_KEY_COLOR_STANDARD },
  { "AMEDIAFORMAT_KEY_DURATION", (uintptr_t)&k_AMEDIAFORMAT_KEY_DURATION },
  { "AMEDIAFORMAT_KEY_ENCODER_DELAY", (uintptr_t)&k_AMEDIAFORMAT_KEY_ENCODER_DELAY },
  { "AMEDIAFORMAT_KEY_FRAME_RATE", (uintptr_t)&k_AMEDIAFORMAT_KEY_FRAME_RATE },
  { "AMEDIAFORMAT_KEY_HEIGHT", (uintptr_t)&k_AMEDIAFORMAT_KEY_HEIGHT },
  { "AMEDIAFORMAT_KEY_LANGUAGE", (uintptr_t)&k_AMEDIAFORMAT_KEY_LANGUAGE },
  { "AMEDIAFORMAT_KEY_MIME", (uintptr_t)&k_AMEDIAFORMAT_KEY_MIME },
  { "AMEDIAFORMAT_KEY_ROTATION", (uintptr_t)&k_AMEDIAFORMAT_KEY_ROTATION },
  { "AMEDIAFORMAT_KEY_SAMPLE_RATE", (uintptr_t)&k_AMEDIAFORMAT_KEY_SAMPLE_RATE },
  { "AMEDIAFORMAT_KEY_SLICE_HEIGHT", (uintptr_t)&k_AMEDIAFORMAT_KEY_SLICE_HEIGHT },
  { "AMEDIAFORMAT_KEY_STRIDE", (uintptr_t)&k_AMEDIAFORMAT_KEY_STRIDE },
  { "AMEDIAFORMAT_KEY_WIDTH", (uintptr_t)&k_AMEDIAFORMAT_KEY_WIDTH },

  { "AMediaCodec_configure", (uintptr_t)&stub_media_err },
  { "AMediaCodec_createDecoderByType", (uintptr_t)&stub_null },
  { "AMediaCodec_delete", (uintptr_t)&stub_void },
  { "AMediaCodec_dequeueInputBuffer", (uintptr_t)&stub_neg1_64 },
  { "AMediaCodec_dequeueOutputBuffer", (uintptr_t)&stub_neg1_64 },
  { "AMediaCodec_flush", (uintptr_t)&stub_media_err },
  { "AMediaCodec_getInputBuffer", (uintptr_t)&stub_null },
  { "AMediaCodec_getOutputBuffer", (uintptr_t)&stub_null },
  { "AMediaCodec_getOutputFormat", (uintptr_t)&stub_null },
  { "AMediaCodec_queueInputBuffer", (uintptr_t)&stub_media_err },
  { "AMediaCodec_releaseOutputBuffer", (uintptr_t)&stub_media_err },
  { "AMediaCodec_setOutputSurface", (uintptr_t)&stub_media_err },
  { "AMediaCodec_start", (uintptr_t)&stub_media_err },
  { "AMediaCodec_stop", (uintptr_t)&stub_media_err },
  { "AMediaDataSource_delete", (uintptr_t)&ds_delete },
  { "AMediaDataSource_new", (uintptr_t)&ds_new },
  { "AMediaDataSource_setClose", (uintptr_t)&stub_void },
  { "AMediaDataSource_setGetSize", (uintptr_t)&ds_set_getsize },
  { "AMediaDataSource_setReadAt", (uintptr_t)&ds_set_readat },
  { "AMediaDataSource_setUserdata", (uintptr_t)&ds_set_ud },
  { "AMediaExtractor_advance", (uintptr_t)&stub_zero },
  { "AMediaExtractor_delete", (uintptr_t)&stub_void },
  { "AMediaExtractor_getSampleTime", (uintptr_t)&stub_neg1_64 },
  { "AMediaExtractor_getSampleTrackIndex", (uintptr_t)&stub_neg1_64 },
  { "AMediaExtractor_getTrackCount", (uintptr_t)&stub_zero },
  { "AMediaExtractor_getTrackFormat", (uintptr_t)&stub_null },
  { "AMediaExtractor_new", (uintptr_t)&log_extractor_new },
  { "AMediaExtractor_readSampleData", (uintptr_t)&stub_neg1_64 },
  { "AMediaExtractor_seekTo", (uintptr_t)&stub_media_err },
  { "AMediaExtractor_selectTrack", (uintptr_t)&stub_media_err },
  { "AMediaExtractor_setDataSource", (uintptr_t)&log_set_source },
  { "AMediaExtractor_setDataSourceCustom", (uintptr_t)&set_source_custom },
  { "AMediaExtractor_setDataSourceFd", (uintptr_t)&set_source_fd },
  { "AMediaFormat_delete", (uintptr_t)&stub_void },
  { "AMediaFormat_getFloat", (uintptr_t)&stub_media_err },
  { "AMediaFormat_getInt32", (uintptr_t)&stub_media_err },
  { "AMediaFormat_getInt64", (uintptr_t)&stub_media_err },
  { "AMediaFormat_getString", (uintptr_t)&stub_media_err },
  { "AMediaFormat_setInt32", (uintptr_t)&stub_media_err },

  { "AHardwareBuffer_describe", (uintptr_t)&stub_media_err },
  { "AHardwareBuffer_release", (uintptr_t)&stub_void },
  { "AImageReader_acquireLatestImage", (uintptr_t)&stub_null },
  { "AImageReader_delete", (uintptr_t)&stub_void },
  { "AImageReader_getWindow", (uintptr_t)&stub_null },
  { "AImageReader_newWithUsage", (uintptr_t)&stub_media_err },
  { "AImageReader_setBufferRemovedListener", (uintptr_t)&stub_media_err },
  { "AImageReader_setImageListener", (uintptr_t)&stub_media_err },
  { "AImage_delete", (uintptr_t)&stub_void },
  { "AImage_deleteAsync", (uintptr_t)&stub_void },
  { "AImage_getHardwareBuffer", (uintptr_t)&stub_null },
  { "AImage_getTimestamp", (uintptr_t)&stub_media_err },
  { "AImage_getWidth", (uintptr_t)&stub_media_err },

  { "ANativeWindow_toSurface", (uintptr_t)&nx_ANativeWindow_toSurface },
  { "nextafter",               (uintptr_t)&nextafter },
};
const int cloverpit_imports_num = sizeof(cloverpit_imports) / sizeof(*cloverpit_imports);

/* =========================================================================
 * libswappywrapper.so -- DO NOT SHIM, DO NOT LOAD.
 *
 * Its 60 unresolved symbols are all libc++ (_ZNSt6__ndk1...). All 60 are
 * exported by the libc++_shared.so that ships beside it, so one option is to
 * so_load() libc++_shared.so as a 4th module and let so_resolve_external()
 * chain-resolve (so_util.h already carries so_dl_iterate_phdr specifically for
 * the libunwind inside libc++_shared).
 *
 * The better option is to skip it entirely. libswappywrapper is NOT in
 * libunity's DT_NEEDED -- libunity dlopen()s it and looks up
 * UnitySwappyWrapperInit(). Since the wrapper owns dlopen(), returning NULL for
 * "libswappywrapper.so" makes libunity log
 *     "SwappyWrapper: InitSwappyWrapper() failed"
 * and fall back to its non-Swappy present path, which is what you want anyway:
 * the port drives its own frame loop. Same trick for "libvulkan.so" (forces the
 * GLES backend that switch-mesa provides) and "libaaudio.so" (forces the
 * OpenSL/FMOD-callback audio path). Three NULLs in the dlopen shim replace an
 * entire module load and 60 symbol bindings.
 * ========================================================================= */
