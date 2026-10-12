#include "core.h"
#include "dl_shim.h"

#define REGISTER_LIBRARY(name, string, id) \
	const intptr_t LibHandle_##name = (intptr_t)(0xABC00000 | id); \
	const char* LibName_##name = string; \
	void* getsym_##name(const char *name);

#define CHECK_LIB_NAME(value, name) \
do { \
	if (strcmp(value, LibName_##name) == 0) {\
		return (void *)LibHandle_##name; \
	}\
} while (0)

// Core libraries
REGISTER_LIBRARY(Libnx, "libnx", 0x01)
REGISTER_LIBRARY(Internal, "__Internal", 0x02)
REGISTER_LIBRARY(SystemNative, "libSystem.Native", 0x03)
REGISTER_LIBRARY(GlobalizationNative, "libSystem.Globalization.Native", 0x04)
REGISTER_LIBRARY(CompressionNative, "libSystem.IO.Compression.Native", 0x05)
/* WARPFORGE: the game's own loaded .so modules (libil2cpp etc.), see below */
static const intptr_t LibHandle_GameModules = (intptr_t)(0xABC00000 | 0x200);
extern void *so_resolve_external(const char *name);

// Optional libraries.
// Don't forget to also register them in the LoadLibrary and GetSymbol functions below, otherwise they won't be found.
#if defined(DLSHIM_SDL2)
REGISTER_LIBRARY(SDL2, "SDL2", 0x100)
#endif

#if defined(DLSHIM_SDL2_IMAGE)
REGISTER_LIBRARY(SDL2_image, "SDL2_image", 0x101)
#endif

#if defined(DLSHIM_CIMGUI)
REGISTER_LIBRARY(Cimgui, "cimgui", 0x102)
#endif

#if defined(DLSHIM_OPENGL)
REGISTER_LIBRARY(Egl, "libEGL.dll", 0x103)
REGISTER_LIBRARY(Glad, "glad", 0x104)
#endif

#if defined(DLSHIM_OPENAL)
REGISTER_LIBRARY(OpenAl, "openal32.dll", 0x105)
#endif

void *dlshim_loadLibrary(const char *name, int flags, char **err, void *user_data)
{
    if (!name)
        return (void *)LibHandle_Internal;

    CHECK_LIB_NAME(name, Libnx);
    CHECK_LIB_NAME(name, Internal);
    CHECK_LIB_NAME(name, SystemNative);
    CHECK_LIB_NAME(name, GlobalizationNative);
    CHECK_LIB_NAME(name, CompressionNative);

    #if defined(DLSHIM_SDL2)
	CHECK_LIB_NAME(name, SDL2);
	#endif

	#if defined(DLSHIM_SDL2_IMAGE)
	CHECK_LIB_NAME(name, SDL2_image);
	#endif

	#if defined(DLSHIM_CIMGUI)
	CHECK_LIB_NAME(name, Cimgui);
	#endif	

	#if defined(DLSHIM_OPENGL)
	CHECK_LIB_NAME(name, Egl);
	CHECK_LIB_NAME(name, Glad);
	#endif
	
	#if defined(DLSHIM_OPENAL)
	CHECK_LIB_NAME(name, OpenAl);
	#endif	

	/* WARPFORGE: the game's own modules and the wrapper's exports, by name
	 * ONLY. This used to be a catch-all, and that crashed the first run on
	 * hardware: Mono tries several spellings of "libSystem.Native"
	 * ("libSystem.Native.so", "System.Native"...), the misses fell through
	 * here, and libil2cpp.so EXPORTS Unity's own SystemNative_* -- so the BCL
	 * ended up inside il2cpp's copy of SystemNative_Stat2 on a thread il2cpp
	 * knows nothing about (null thread state + 0x135). Anything not listed
	 * must fail, so Mono goes on to its next spelling. */
	{
		static const char *const game_libs[] = {
			"warpforge", "libwarpforge", "libwarpforge.so",
			"il2cpp", "libil2cpp", "libil2cpp.so", "GameAssembly", "GameAssembly.dll",
			"unity", "libunity", "libunity.so",
			"main", "libmain", "libmain.so",
			"RF_CNative_andr", "libRF_CNative_andr", "libRF_CNative_andr.so",
			"burst_generated", "lib_burst_generated", "lib_burst_generated.so",
		};
		for (unsigned i = 0; i < sizeof game_libs / sizeof *game_libs; i++)
			if (!strcmp(name, game_libs[i])) {
				if (g_config.mononx_logging)
					io_debugf("dlshim_loadLibrary %s -> game modules", name);
				return (void *)LibHandle_GameModules;
			}
	}
	if (g_config.mononx_logging)
		io_debugf("dlshim_loadLibrary %s -> not found", name);
	return NULL;
}

void *dlshim_closeLibrary(void *handle, void *user_data)
{
    return NULL;
}

void *dlshim_getSymbol(void *handle, const char *name, char **err, void *user_data)
{
    void *symbol = NULL;
	const char* resolvedLibrary = "<none>";

#define CHECK_LIB_SYMBOL(libName) \
	case LibHandle_##libName: \
		resolvedLibrary = LibName_##libName; \
		symbol = getsym_##libName(name);  \
		break;

	if (!handle)
		return NULL;

	switch ((intptr_t)handle)
	{		
		CHECK_LIB_SYMBOL(Libnx)
		CHECK_LIB_SYMBOL(Internal)
		CHECK_LIB_SYMBOL(SystemNative)
		CHECK_LIB_SYMBOL(GlobalizationNative)
		CHECK_LIB_SYMBOL(CompressionNative)

	#if defined(DLSHIM_SDL2)
		CHECK_LIB_SYMBOL(SDL2)
	#endif

	#if defined(DLSHIM_SDL2_IMAGE)
		CHECK_LIB_SYMBOL(SDL2_image)
	#endif

	#if defined(DLSHIM_CIMGUI)
		CHECK_LIB_SYMBOL(Cimgui)
	#endif

	#if defined(DLSHIM_OPENGL)
		CHECK_LIB_SYMBOL(Egl)
		CHECK_LIB_SYMBOL(Glad)
	#endif

	#if defined(DLSHIM_OPENAL)
		CHECK_LIB_SYMBOL(OpenAl)
	#endif
	case 0xABC00200:   /* LibHandle_GameModules */
		resolvedLibrary = "game modules";
		{
			/* the wrapper's own exports for managed code come first */
			extern void  wf_mono_log(const char *line);
			extern void *wf_detour_install(void *target, void *replacement);
			if      (!strcmp(name, "wf_mono_log"))       symbol = (void *)wf_mono_log;
			else if (!strcmp(name, "wf_detour_install")) symbol = (void *)wf_detour_install;
		}
		if (!symbol && strncmp(name, "SystemNative_", 13) && strncmp(name, "GlobalizationNative_", 20))
			symbol = so_resolve_external(name);
		break;
	}

    if (symbol) {
		if (g_config.mononx_logging)
        	io_debugf("dlshim_getSymbol resolved: handle=%p lib=%s symbol=%s", handle, resolvedLibrary, name);

        return symbol;
	}

    if (g_config.mononx_logging)
        io_debugf("dlshim_getSymbol error: handle=%p lib=%s symbol=%s", handle, resolvedLibrary, name);

    return NULL;
}

void* getsym_Internal(const char *name)
{
	if (strcmp(name, "console_ensure_init") == 0) return (void *)console_ensure_init;
    else if (strcmp(name, "console_dispose") == 0) return (void *)console_dispose;
    else if (strcmp(name, "console_update") == 0) return(void *)console_update;

	return NULL;
}