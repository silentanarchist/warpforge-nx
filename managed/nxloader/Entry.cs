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
            Root = root;
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
            return 0;
        }

        public static int OnIl2CppReady()
        {
            try { StackProbe(); return 0; }
            catch (Exception e) { Log("loader: stack probe FAILED -- " + e); return -1; }
        }

        public static void OnUpdate()
        {
            s_frames++;
            if (s_frames == 1 || s_frames == 600 || s_frames == 3600) Log($"loader: OnUpdate frame {s_frames}");
        }

        static void StackProbe()
        {
            // 1. the generated proxies (from the owner's refs\il2cpp, copied to the card)
            if (!Directory.Exists(Il2CppAssembliesDir)) { Log($"loader: {Il2CppAssembliesDir} missing -- copy refs\\il2cpp\\*.dll there"); return; }
            var files = Directory.GetFiles(Il2CppAssembliesDir, "*.dll");
            int loaded = 0;
            foreach (var f in files)
            {
                try { Assembly.LoadFrom(f); loaded++; }
                catch (Exception e) { Log($"loader: could not load {Path.GetFileName(f)}: {e.GetType().Name}: {e.Message}"); }
            }
            Log($"loader: {loaded}/{files.Length} Il2Cpp assemblies loaded");
            Environment.SetEnvironmentVariable("IL2CPP_INTEROP_DATABASES_LOCATION", Il2CppAssembliesDir);

            // 2. the interop runtime, the way MelonLoader's Il2Cpp support module does it
            s_runtime = Il2CppInteropRuntime.Create(new RuntimeConfiguration
            {
                DetourProvider = new NxDetourProvider(),
                UnityVersion = new Version(6000, 2, 6),
            }).AddLogger(new NxLogger()).AddHarmonySupport();
            s_runtime.Start();
            Log("loader: Il2CppInteropRuntime started");

            // 3. a static property through the generated proxy (UnityEngine.CoreModule.dll)
            var appType = Type.GetType("UnityEngine.Application, UnityEngine.CoreModule", throwOnError: false);
            if (appType == null) { Log("loader: UnityEngine.Application proxy not found"); return; }
            var versionProp = appType.GetProperty("version", BindingFlags.Public | BindingFlags.Static);
            Log($"loader: proxy Application.version = \"{versionProp?.GetValue(null)}\" (expected 1.35.0)");
            var unityVersionProp = appType.GetProperty("unityVersion", BindingFlags.Public | BindingFlags.Static);
            Log($"loader: proxy Application.unityVersion = \"{unityVersionProp?.GetValue(null)}\"");

            // 4. a Harmony patch on a game method through the proxies -> Il2CppDetourMethodPatcher -> NxDetourProvider
            var harmony = new Harmony("WarpforgeNx.StackProbe");
            var getter = appType.GetProperty("productName", BindingFlags.Public | BindingFlags.Static).GetGetMethod();
            harmony.Patch(getter, postfix: new HarmonyMethod(typeof(Entry).GetMethod(nameof(ProductNamePostfix), BindingFlags.NonPublic | BindingFlags.Static)));
            var productName = appType.GetProperty("productName", BindingFlags.Public | BindingFlags.Static).GetValue(null);
            Log($"loader: Harmony-patched Application.productName = \"{productName}\" (expected \"Warpforge [patched]\")");

            // 5. DelegateSupport.ConvertDelegate: a managed lambda as an Il2CppSystem.Action, invoked by il2cpp
            int fired = 0;
            var action = DelegateSupport.ConvertDelegate<Il2CppSystem.Action>(new Action(() => { fired++; Log("loader: Il2CppSystem.Action fired -> managed lambda ran"); }));
            action.Invoke();
            Log($"loader: ConvertDelegate {(fired == 1 ? "ok" : "did NOT fire")}");

            Log("loader: stack probe done");
        }

        static void ProductNamePostfix(ref string __result) { __result += " [patched]"; }
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
