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
        static void Log(string s) { try { wf_mono_log(s); } catch { } }

        // "il2cpp" is routed by the wrapper's dlopen shim to the loaded libil2cpp.so.
        const string L = "il2cpp";
        [DllImport(L)] static extern IntPtr il2cpp_domain_get();
        [DllImport(L)] static extern IntPtr il2cpp_domain_get_assemblies(IntPtr domain, out nuint size);
        [DllImport(L)] static extern IntPtr il2cpp_domain_assembly_open(IntPtr domain, [MarshalAs(UnmanagedType.LPStr)] string name);
        [DllImport(L)] static extern IntPtr il2cpp_assembly_get_image(IntPtr assembly);
        [DllImport(L)] static extern IntPtr il2cpp_image_get_name(IntPtr image);
        [DllImport(L)] static extern int    il2cpp_image_get_class_count(IntPtr image);
        [DllImport(L)] static extern IntPtr il2cpp_class_from_name(IntPtr image, [MarshalAs(UnmanagedType.LPStr)] string ns, [MarshalAs(UnmanagedType.LPStr)] string name);
        [DllImport(L)] static extern IntPtr il2cpp_class_get_name(IntPtr klass);
        [DllImport(L)] static extern IntPtr il2cpp_class_get_method_from_name(IntPtr klass, [MarshalAs(UnmanagedType.LPStr)] string name, int argc);
        [DllImport(L)] static extern IntPtr il2cpp_runtime_invoke(IntPtr method, IntPtr obj, void** args, IntPtr* exc);
        [DllImport(L)] static extern IntPtr il2cpp_object_get_class(IntPtr obj);
        [DllImport(L)] static extern IntPtr il2cpp_string_new([MarshalAs(UnmanagedType.LPStr)] string s);
        [DllImport(L)] static extern int    il2cpp_string_length(IntPtr str);
        [DllImport(L)] static extern char*  il2cpp_string_chars(IntPtr str);

        static string CStr(IntPtr p) => p == IntPtr.Zero ? "(null)" : (Marshal.PtrToStringAnsi(p) ?? "");
        static string Il2CppStr(IntPtr s) => s == IntPtr.Zero ? "(null)" : new string(il2cpp_string_chars(s), 0, il2cpp_string_length(s));

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
            return 0;
        }
    }
}
