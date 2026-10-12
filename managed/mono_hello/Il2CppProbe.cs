// Il2CppProbe -- milestone 2: managed code (Mono, interpreter) calling INTO the
// game's IL2CPP runtime through its exported C API, by name. The wrapper calls
// OnIl2CppReady() from the render loop on the first frame after il2cpp_init, on
// the engine main thread (the one il2cpp_init ran on, so il2cpp knows it).
//
// This is the same walk warpforge_resolve.c does in C, done from C#:
//   domain -> assembly -> image -> class -> method -> methodPointer / invoke.
// It is the foundation Il2CppInterop builds its generated wrappers on.
using System;
using System.Runtime.InteropServices;
using System.Text;

namespace WarpforgeNx
{
    public static unsafe class Il2CppProbe
    {
        [DllImport("warpforge")] static extern void wf_mono_log(string line);
        internal static void Log(string s) { try { wf_mono_log(s); } catch { } }

        // "il2cpp" is routed by the wrapper's dlopen shim to the loaded libil2cpp.so.
        const string L = "il2cpp";
        [DllImport(L)] static extern IntPtr il2cpp_domain_get();
        [DllImport(L)] static extern IntPtr il2cpp_domain_get_assemblies(IntPtr domain, out nuint size);
        [DllImport(L)] static extern IntPtr il2cpp_domain_assembly_open(IntPtr domain, [MarshalAs(UnmanagedType.LPStr)] string name);
        [DllImport(L)] static extern IntPtr il2cpp_assembly_get_image(IntPtr assembly);
        [DllImport(L)] static extern IntPtr il2cpp_image_get_name(IntPtr image);
        [DllImport(L)] static extern int    il2cpp_image_get_class_count(IntPtr image);
        [DllImport(L)] internal static extern IntPtr il2cpp_class_from_name(IntPtr image, [MarshalAs(UnmanagedType.LPStr)] string ns, [MarshalAs(UnmanagedType.LPStr)] string name);
        [DllImport(L)] static extern IntPtr il2cpp_class_get_name(IntPtr klass);
        [DllImport(L)] internal static extern IntPtr il2cpp_class_get_method_from_name(IntPtr klass, [MarshalAs(UnmanagedType.LPStr)] string name, int argc);
        [DllImport(L)] internal static extern IntPtr il2cpp_runtime_invoke(IntPtr method, IntPtr obj, void** args, IntPtr* exc);
        [DllImport(L)] static extern IntPtr il2cpp_object_get_class(IntPtr obj);
        [DllImport(L)] internal static extern IntPtr il2cpp_string_new([MarshalAs(UnmanagedType.LPStr)] string s);
        [DllImport(L)] static extern int    il2cpp_string_length(IntPtr str);
        [DllImport(L)] static extern char*  il2cpp_string_chars(IntPtr str);

        internal static string CStr(IntPtr p) => p == IntPtr.Zero ? "(null)" : (Marshal.PtrToStringAnsi(p) ?? "");
        internal static string Il2CppStr(IntPtr s) => s == IntPtr.Zero ? "(null)" : new string(il2cpp_string_chars(s), 0, il2cpp_string_length(s));

        // Called by the wrapper (mono_runtime_invoke). Must not throw out.
        public static int OnIl2CppReady()
        {
            try { return Run(); }
            catch (Exception e) { Log("il2cpp probe: FAILED -- " + e.GetType().Name + ": " + e.Message); return -1; }
        }

        static int Run()
        {
            var domain = il2cpp_domain_get();
            Log($"il2cpp probe: domain {domain:x}");
            if (domain == IntPtr.Zero) return -1;

            var asms = (IntPtr*)il2cpp_domain_get_assemblies(domain, out var n);
            Log($"il2cpp probe: {n} assemblies in the game domain");
            var sb = new StringBuilder();
            int shown = 0;
            for (nuint i = 0; i < n; i++)
            {
                var img = il2cpp_assembly_get_image(asms[i]);
                var name = CStr(il2cpp_image_get_name(img));
                if (name.StartsWith("Assembly-CSharp") || name.Contains("Everguild") || name.Contains("Warpforge") || name == "UnityEngine.CoreModule.dll")
                    Log($"il2cpp probe:   {name}: {il2cpp_image_get_class_count(img)} classes");
                else if (shown++ < 40) sb.Append(name).Append(' ');
            }
            Log("il2cpp probe:   others: " + sb);

            // 1. A static getter the wrapper already hooks: UnityEngine.Time.get_deltaTime.
            var core = il2cpp_domain_assembly_open(domain, "UnityEngine.CoreModule");
            var coreImg = il2cpp_assembly_get_image(core);
            var time = il2cpp_class_from_name(coreImg, "UnityEngine", "Time");
            var getDelta = il2cpp_class_get_method_from_name(time, "get_deltaTime", 0);
            Log($"il2cpp probe: Time.get_deltaTime MethodInfo {getDelta:x}, methodPointer {(getDelta == IntPtr.Zero ? 0 : *(ulong*)getDelta):x}");
            if (getDelta != IntPtr.Zero)
            {
                // (a) through il2cpp_runtime_invoke (boxed float back)
                IntPtr exc = IntPtr.Zero;
                var boxed = il2cpp_runtime_invoke(getDelta, IntPtr.Zero, null, &exc);
                if (exc != IntPtr.Zero) Log("il2cpp probe: runtime_invoke threw " + CStr(il2cpp_class_get_name(il2cpp_object_get_class(exc))));
                else Log($"il2cpp probe: runtime_invoke Time.deltaTime = {*(float*)((byte*)boxed + 16)}");
                // (b) straight through the method pointer -- what a detour does
                var fp = (delegate* unmanaged[Cdecl]<IntPtr, float>)*(void**)getDelta;
                Log($"il2cpp probe: direct call Time.deltaTime = {fp(IntPtr.Zero)}");
            }

            // 2. A string-returning getter: Application.get_unityVersion / get_version.
            var app = il2cpp_class_from_name(coreImg, "UnityEngine", "Application");
            foreach (var g in new[] { "get_unityVersion", "get_version", "get_productName", "get_platform" })
            {
                var m = il2cpp_class_get_method_from_name(app, g, 0);
                if (m == IntPtr.Zero) { Log($"il2cpp probe: Application.{g} not found"); continue; }
                IntPtr exc = IntPtr.Zero;
                var r = il2cpp_runtime_invoke(m, IntPtr.Zero, null, &exc);
                if (exc != IntPtr.Zero) Log($"il2cpp probe: Application.{g} threw");
                else if (g == "get_platform") Log($"il2cpp probe: Application.{g} = {*(int*)((byte*)r + 16)} (11 = Android)");
                else Log($"il2cpp probe: Application.{g} = \"{Il2CppStr(r)}\"");
            }

            // 3. An il2cpp string made from managed code (allocation on the il2cpp GC heap).
            var s = il2cpp_string_new("hello from Mono");
            Log($"il2cpp probe: il2cpp_string_new -> \"{Il2CppStr(s)}\" ({il2cpp_string_length(s)} chars)");

            // 4. Something from the game itself.
            var gameAsm = il2cpp_domain_assembly_open(domain, "Assembly-CSharp");
            if (gameAsm != IntPtr.Zero)
            {
                var gImg = il2cpp_assembly_get_image(gameAsm);
                Log($"il2cpp probe: Assembly-CSharp opened, {il2cpp_image_get_class_count(gImg)} classes");
            }
            else Log("il2cpp probe: Assembly-CSharp not found by name");

            Log("il2cpp probe: done");
            DetourProbe.Run(coreImg);
            return 0;
        }
    }

    // MILESTONE 3: a detour installed from managed code on a game method, with the
    // replacement in C# and the original reachable through a trampoline. This is
    // the mechanism HarmonyLib needs under IL2CPP (what MelonLoader does with
    // Dobby + Il2CppInterop), reduced to one hand-written case.
    public static unsafe class DetourProbe
    {
        [DllImport("warpforge")] static extern IntPtr wf_detour_install(IntPtr target, IntPtr replacement);

        // IL2CPP native signature of a static getter: ret (const MethodInfo* method)
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate IntPtr StaticGetter(IntPtr method);
        // instance getter: ret (this, const MethodInfo* method)
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate IntPtr InstanceGetter(IntPtr self, IntPtr method);

        static StaticGetter? s_versionDetour, s_versionOrig;        // kept alive: the native side holds raw pointers
        static InstanceGetter? s_transformDetour, s_transformOrig;
        static long s_transformCalls;

        static IntPtr VersionDetour(IntPtr method)
        {
            var orig = s_versionOrig!(method);
            return Il2CppProbe.il2cpp_string_new(Il2CppProbe.Il2CppStr(orig) + "-switch");
        }

        static IntPtr TransformDetour(IntPtr self, IntPtr method)
        {
            long n = System.Threading.Interlocked.Increment(ref s_transformCalls);
            if (n == 1 || n == 10 || n == 100 || n == 1000 || n == 10000 || n == 100000)
                Il2CppProbe.Log($"detour probe: GameObject.get_transform called by the game {n} times through managed code");
            return s_transformOrig!(self, method);
        }

        public static void Run(IntPtr coreImg)
        {
            try
            {
                // 1. Application.get_version: hook, then call it the way the game would.
                var app = Il2CppProbe.il2cpp_class_from_name(coreImg, "UnityEngine", "Application");
                var getVersion = Il2CppProbe.il2cpp_class_get_method_from_name(app, "get_version", 0);
                var target = *(IntPtr*)getVersion;
                s_versionDetour = VersionDetour;
                var tramp = wf_detour_install(target, Marshal.GetFunctionPointerForDelegate(s_versionDetour));
                if (tramp == IntPtr.Zero) { Il2CppProbe.Log("detour probe: Application.get_version hook REFUSED"); }
                else
                {
                    s_versionOrig = Marshal.GetDelegateForFunctionPointer<StaticGetter>(tramp);
                    Il2CppProbe.Log("detour probe: original via trampoline = \"" + Il2CppProbe.Il2CppStr(s_versionOrig(getVersion)) + "\"");
                    IntPtr exc = IntPtr.Zero;
                    var r = Il2CppProbe.il2cpp_runtime_invoke(getVersion, IntPtr.Zero, null, &exc);
                    Il2CppProbe.Log("detour probe: Application.version through the hook = \"" + Il2CppProbe.Il2CppStr(r) + "\" (expected 1.35.0-switch)");
                }

                // 2. GameObject.get_transform: hooked and left in place, counting the game's own calls.
                var go = Il2CppProbe.il2cpp_class_from_name(coreImg, "UnityEngine", "GameObject");
                var getTransform = Il2CppProbe.il2cpp_class_get_method_from_name(go, "get_transform", 0);
                if (getTransform == IntPtr.Zero) { Il2CppProbe.Log("detour probe: GameObject.get_transform not found"); return; }
                s_transformDetour = TransformDetour;
                var tramp2 = wf_detour_install(*(IntPtr*)getTransform, Marshal.GetFunctionPointerForDelegate(s_transformDetour));
                if (tramp2 == IntPtr.Zero) { Il2CppProbe.Log("detour probe: GameObject.get_transform hook REFUSED"); return; }
                s_transformOrig = Marshal.GetDelegateForFunctionPointer<InstanceGetter>(tramp2);
                Il2CppProbe.Log("detour probe: GameObject.get_transform hooked; the game's calls now pass through C#");
            }
            catch (Exception e) { Il2CppProbe.Log("detour probe: FAILED -- " + e.GetType().Name + ": " + e.Message); }
        }
    }
}
