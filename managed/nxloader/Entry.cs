// Entry points the wrapper calls (source/warpforge_mono.c):
//   Boot(root)        after the Mono runtime is up, before the game loads
//   OnIl2CppReady()   on the first frame after il2cpp_init, engine main thread
//   OnUpdate()        every frame after that, engine main thread
//
// STACK PROBE (mod milestone 4a): bring Il2CppInterop up under the interpreter and
// exercise the three things the mod relies on -- generated proxies, a Harmony patch
// on a game method, DelegateSupport.ConvertDelegate (ClassInjector + xref scans).
using System;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using HarmonyLib;
using Il2CppInterop.Common;
using Il2CppInterop.HarmonySupport;
using Il2CppInterop.Runtime;
using Il2CppInterop.Runtime.Startup;
using Microsoft.Extensions.Logging;

namespace WarpforgeNx
{
    public static class Entry
    {
        [DllImport("warpforge")] static extern void wf_mono_log(string line);
        internal static void Log(string s) { try { wf_mono_log(s); } catch { } }

        public static string Root { get; private set; }
        public static string MelonDir => Path.Combine(Root, "MelonLoader");
        public static string Il2CppAssembliesDir => Path.Combine(MelonDir, "Il2CppAssemblies");

        static Il2CppInteropRuntime s_runtime;
        static long s_frames;

        public static int Boot(string root)
        {
            // The class library here is a Debug build: its file APIs assert that a full path
            // starts with '/', and "sdmc:/switch/..." made Directory.CreateDirectory kill the
            // process. libnx resolves "/switch/..." against the SD card (the default device).
            if (root.StartsWith("sdmc:", StringComparison.OrdinalIgnoreCase)) root = root.Substring(5);
            Root = root;
            // Debug.Assert in that Debug class library ends the process; log it and go on instead.
            InstallAssertHook();
            Log($"loader: boot, root={root}");
            Log($"loader: {RuntimeInformation.FrameworkDescription}, Il2CppInterop.Runtime {typeof(Il2CppInteropRuntime).Assembly.GetName().Version}, HarmonyX {typeof(Harmony).Assembly.GetName().Version}");
            AppDomain.CurrentDomain.AssemblyResolve += (_, e) =>
            {
                var name = new AssemblyName(e.Name).Name + ".dll";
                foreach (var dir in new[] { Il2CppAssembliesDir, Path.Combine(MelonDir, "Loader"), Path.Combine(MelonDir, "Mods") })
                {
                    var p = Path.Combine(dir, name);
                    if (File.Exists(p)) { try { return Assembly.LoadFrom(p); } catch (Exception ex) { Log($"loader: resolve {name} failed: {ex.Message}"); } }
                }
                return null;
            };
            AppDomain.CurrentDomain.UnhandledException += (_, e) => Log("loader: UNHANDLED " + e.ExceptionObject);
            SetTrustedPlatformAssemblies();
            return 0;
        }

        static bool s_modsLoaded;

        // Mono.Cecil (used by HarmonyX/MonoMod to build delegate types for every patch) finds
        // framework assemblies through the TRUSTED_PLATFORM_ASSEMBLIES list a normal .NET host
        // provides. Mono NX provides none, so the first patch failed with "Failed to resolve
        // assembly: System.Private.CoreLib". Hand it the class library folders.
        static void SetTrustedPlatformAssemblies()
        {
            try
            {
                var dirs = new System.Collections.Generic.List<string>();
                var corelibDir = Path.GetDirectoryName(typeof(object).Assembly.Location);
                if (!string.IsNullOrEmpty(corelibDir)) dirs.Add(corelibDir);
                foreach (var d in new[] { "/mono/lib_net9.0", "/mono/framework_net9.0" })
                    if (Directory.Exists(d) && !dirs.Contains(d)) dirs.Add(d);
                var files = dirs.SelectMany(d => Directory.GetFiles(d, "*.dll"))
                                .Where(f => f.IndexOf(Path.PathSeparator) < 0)   // "sdmc:" paths cannot be listed
                                .ToArray();
                AppDomain.CurrentDomain.SetData("TRUSTED_PLATFORM_ASSEMBLIES", string.Join(Path.PathSeparator, files));
                Log($"loader: {files.Length} framework assemblies listed for Cecil ({string.Join(", ", dirs)})");
            }
            catch (Exception e) { Log("loader: could not list framework assemblies: " + e.Message); }
        }

        // DebugProvider.s_FailCore is the hook .NET's own tests use to intercept Debug.Fail;
        // DebugProvider is not in the reference assemblies, so it is set by reflection.
        static void InstallAssertHook()
        {
            try
            {
                var t = typeof(System.Diagnostics.Debug).Assembly.GetType("System.Diagnostics.DebugProvider");
                var f = t?.GetField("s_FailCore", BindingFlags.NonPublic | BindingFlags.Static);
                if (f == null) { Log("loader: Debug.Assert hook not available"); return; }
                Action<string, string, string, string> hook = (stack, message, detail, source) =>
                    Log($"loader: Debug.Assert failed (ignored): {message} {detail}\n{stack}");
                f.SetValue(null, hook);
                Log("loader: Debug.Assert failures are logged instead of ending the process");
            }
            catch (Exception e) { Log("loader: could not hook Debug.Assert: " + e.Message); }
        }

        public static int OnIl2CppReady()
        {
            try { StackProbe(); }
            catch (Exception e) { Log("loader: stack probe FAILED -- " + e); }
            if (s_runtime == null) { Log("loader: Il2CppInterop is not running -- mods not loaded"); return -1; }
            Log("loader: starting MelonLoader-lite ...");
            try { StartMelonLite(); }
            catch (Exception e) { Log("loader: loading mods FAILED -- " + e); }
            return 0;
        }

        // separate method: the interpreter loads MelonLoader.dll when it compiles this one,
        // so a failure there is caught above, after the line saying where we are
        static void StartMelonLite()
        {
            {
                MelonLoader.Utils.MelonEnvironment.Initialize(MelonDir);
                Log("loader: MelonLoader-lite loaded, looking for mods ...");
                int n = MelonLoader.Switch.NxHost.LoadMods();
                s_modsLoaded = true;
                Log($"loader: {n} mod(s) loaded");
            }
        }

        public static void OnUpdate()
        {
            s_frames++;
            if (s_frames == 1 || s_frames == 600 || s_frames == 3600) Log($"loader: OnUpdate frame {s_frames}");
            if (s_modsLoaded) MelonLoader.Switch.NxHost.Update();
        }

        // Kept free of any Il2Cpp* type: the interpreter resolves every type a method
        // mentions before running its first line, so a missing Il2Cppmscorlib would
        // otherwise fail here before the folder check could say so.
        static void StackProbe()
        {
            // 1. the generated proxies (from the owner's refs\il2cpp, copied to the card)
            if (!Directory.Exists(Il2CppAssembliesDir)) { Log($"loader: {Il2CppAssembliesDir} missing -- copy refs\\il2cpp\\*.dll there"); return; }
            var files = Directory.GetFiles(Il2CppAssembliesDir, "*.dll");
            if (files.Length == 0) { Log($"loader: {Il2CppAssembliesDir} is empty -- copy refs\\il2cpp\\*.dll there"); return; }
            int loaded = 0;
            foreach (var f in files)
            {
                try { Assembly.LoadFrom(f); loaded++; }
                catch (Exception e) { Log($"loader: could not load {Path.GetFileName(f)}: {e.GetType().Name}: {e.Message}"); }
            }
            Log($"loader: {loaded}/{files.Length} Il2Cpp assemblies loaded ({string.Join(" ", files.Take(6).Select(Path.GetFileNameWithoutExtension))}...)");
            if (!files.Any(f => Path.GetFileName(f).Equals("Il2Cppmscorlib.dll", StringComparison.OrdinalIgnoreCase)))
            { Log("loader: Il2Cppmscorlib.dll is not among them -- these are not the generated Il2Cpp assemblies"); return; }
            Environment.SetEnvironmentVariable("IL2CPP_INTEROP_DATABASES_LOCATION", Il2CppAssembliesDir);
            InteropProbe();
        }

        static void InteropProbe()
        {
            // before anything touches HarmonyX or MonoMod (see NxMonoModPlatforms.cs)
            NxMonoModPlatforms.Install();
            Log("loader: MonoMod platforms replaced (no managed-code patching under the interpreter)");
            NxMonoModFixes.Install();
            // Harmony patches on game methods: native detour only (il2cppinterop-switch.patch)
            AppContext.SetSwitch("Il2CppInterop.HarmonySupport.NoManagedDetour", true);
            // Il2CppInterop's FastPath stubs come from the wrapper (il2cppinterop-switch.patch)
            AppContext.SetSwitch("Il2CppInterop.Switch", true);

            // 2. the interop runtime, the way MelonLoader's Il2Cpp support module does it
            s_runtime = Il2CppInteropRuntime.Create(new RuntimeConfiguration
            {
                DetourProvider = new NxDetourProvider(),
                UnityVersion = new Version(6000, 2, 6),
            }).AddLogger(new NxLogger()).AddHarmonySupport();
            s_runtime.Start();
            Log("loader: Il2CppInteropRuntime started");
            try { ProbeChecks(); }
            catch (Exception e) { Log("loader: probe check FAILED (mods still load) -- " + e); }
            try { ConvertDelegateCheck(); }
            catch (Exception e) { Log("loader: ConvertDelegate check FAILED (mods still load) -- " + e); }
            Log("loader: stack probe done");
        }

        static void ProbeChecks()
        {

            // 3. a static property through the generated proxy (UnityEngine.CoreModule.dll)
            var appType = Type.GetType("UnityEngine.Application, UnityEngine.CoreModule", throwOnError: false);
            if (appType == null) { Log("loader: UnityEngine.Application proxy not found"); return; }
            var versionProp = appType.GetProperty("version", BindingFlags.Public | BindingFlags.Static);
            Log($"loader: proxy Application.version = \"{versionProp?.GetValue(null)}\" (expected 1.35.0)");
            var unityVersionProp = appType.GetProperty("unityVersion", BindingFlags.Public | BindingFlags.Static);
            Log($"loader: proxy Application.unityVersion = \"{unityVersionProp?.GetValue(null)}\"");

            // 4. a Harmony patch on a game method through the proxies -> Il2CppDetourMethodPatcher -> NxDetourProvider.
            //    No MonoMod managed detour under the interpreter (see il2cppinterop-switch.patch), so the patch
            //    is seen by callers that go through il2cpp -- the game itself -- which is what mods need.
            Log("loader: patching Application.productName with Harmony ...");
            var harmony = new Harmony("WarpforgeNx.StackProbe");
            var getter = appType.GetProperty("productName", BindingFlags.Public | BindingFlags.Static).GetGetMethod();
            harmony.Patch(getter, postfix: new HarmonyMethod(typeof(Entry).GetMethod(nameof(ProductNamePostfix), BindingFlags.NonPublic | BindingFlags.Static)));
            Log("loader: patch applied");
            Log($"loader: productName as the game sees it = \"{ProductNameViaIl2Cpp()}\" (expected \"Warpforge [patched]\")");
        }

        static void ConvertDelegateCheck()
        {
            // 5. DelegateSupport.ConvertDelegate: a managed lambda as an Il2CppSystem.Action, invoked by il2cpp
            int fired = 0;
            var action = DelegateSupport.ConvertDelegate<Il2CppSystem.Action>(new Action(() => { fired++; Log("loader: Il2CppSystem.Action fired -> managed lambda ran"); }));
            action.Invoke();
            Log($"loader: ConvertDelegate {(fired == 1 ? "ok" : "did NOT fire")}");
        }

        static void ProductNamePostfix(ref string __result) { __result += " [patched]"; }

        // Call the getter the way the game does: il2cpp's own MethodInfo, through il2cpp_runtime_invoke.
        static unsafe string ProductNameViaIl2Cpp()
        {
            var klass = IL2CPP.GetIl2CppClass("UnityEngine.CoreModule.dll", "UnityEngine", "Application");
            var mi = IL2CPP.il2cpp_class_get_method_from_name(klass, "get_productName", 0);
            IntPtr exc = IntPtr.Zero;
            var r = IL2CPP.il2cpp_runtime_invoke(mi, IntPtr.Zero, null, ref exc);
            return exc != IntPtr.Zero ? "(threw)" : IL2CPP.Il2CppStringToManaged(r);
        }
    }

    sealed class NxLogger : ILogger
    {
        public IDisposable BeginScope<TState>(TState state) where TState : notnull => null;
        public bool IsEnabled(LogLevel logLevel) => logLevel >= LogLevel.Debug;
        public void Log<TState>(LogLevel logLevel, EventId eventId, TState state, Exception exception, Func<TState, Exception, string> formatter)
        {
            Entry.Log($"interop/{logLevel}: {formatter(state, exception)}{(exception != null ? " -- " + exception : "")}");
        }
    }
}
