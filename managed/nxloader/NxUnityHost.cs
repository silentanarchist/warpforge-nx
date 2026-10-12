// A MonoBehaviour of our own inside the game, so Unity calls OnGUI (the mod's windows:
// sign-in, bug report) and OnApplicationQuit. Il2CppInterop injects it the way
// MelonLoader's SM_Component is injected; the type itself is emitted at run time
// because the generated UnityEngine proxies are only on the card, not at build time.
using System;
using System.Linq;
using System.Reflection;
using System.Reflection.Emit;
using Il2CppInterop.Runtime;
using Il2CppInterop.Runtime.Injection;

namespace WarpforgeNx
{
    public static class NxUnityHost
    {
        static object s_component;

        // Called from the emitted component's methods.
        public static void OnGUI() { TraceEvent(); MelonLoader.Switch.NxHost.OnGUI(); }

        // Diagnostic: the first pointer events IMGUI sees (type + position), so a dead
        // mod window can be told apart from touches that never reach IMGUI at all.
        static PropertyInfo s_evCurrent, s_evType, s_evPos;
        static int s_traced, s_evFailed;
        static void TraceEvent()
        {
            if (s_traced >= 24 || s_evFailed > 0) return;
            try
            {
                if (s_evCurrent == null)
                {
                    var imgui = AppDomain.CurrentDomain.GetAssemblies().FirstOrDefault(a => a.GetName().Name == "UnityEngine.IMGUIModule")
                                ?? Assembly.Load("UnityEngine.IMGUIModule");
                    var ev = imgui.GetType("UnityEngine.Event", true);
                    s_evCurrent = ev.GetProperty("current", BindingFlags.Public | BindingFlags.Static);
                    s_evType = ev.GetProperty("type", BindingFlags.Public | BindingFlags.Instance);
                    s_evPos = ev.GetProperty("mousePosition", BindingFlags.Public | BindingFlags.Instance);
                }
                var cur = s_evCurrent.GetValue(null);
                if (cur == null) return;
                var type = s_evType.GetValue(cur)?.ToString() ?? "?";
                if (type == "Repaint" || type == "Layout" || type == "Ignore" || type == "Used") return;
                s_traced++;
                Entry.Log($"loader: IMGUI event {type} at {s_evPos.GetValue(cur)}");
            }
            catch (Exception e) { s_evFailed++; Entry.Log("loader: IMGUI event trace off: " + e.Message); }
        }
        public static void OnApplicationQuit() { MelonLoader.Switch.NxHost.Quit(); }

        public static void Create()
        {
            var core = AppDomain.CurrentDomain.GetAssemblies().FirstOrDefault(a => a.GetName().Name == "UnityEngine.CoreModule")
                       ?? throw new Exception("UnityEngine.CoreModule proxy not loaded");
            var monoBehaviour = core.GetType("UnityEngine.MonoBehaviour", true);
            var gameObject = core.GetType("UnityEngine.GameObject", true);
            var unityObject = core.GetType("UnityEngine.Object", true);

            var ab = AssemblyBuilder.DefineDynamicAssembly(new AssemblyName("WarpforgeNx.Unity"), AssemblyBuilderAccess.Run);
            var mb = ab.DefineDynamicModule("WarpforgeNx.Unity");
            var tb = mb.DefineType("WarpforgeNx.NxComponent", TypeAttributes.Public | TypeAttributes.Class, monoBehaviour);

            // public NxComponent(IntPtr p) : base(p) {}
            var baseCtor = monoBehaviour.GetConstructor(new[] { typeof(IntPtr) }) ?? throw new Exception("MonoBehaviour(IntPtr) ctor not found");
            var ctor = tb.DefineConstructor(MethodAttributes.Public, CallingConventions.Standard, new[] { typeof(IntPtr) });
            var il = ctor.GetILGenerator();
            il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldarg_1); il.Emit(OpCodes.Call, baseCtor); il.Emit(OpCodes.Ret);

            foreach (var name in new[] { nameof(OnGUI), nameof(OnApplicationQuit) })
            {
                var m = tb.DefineMethod(name, MethodAttributes.Public | MethodAttributes.HideBySig, typeof(void), Type.EmptyTypes);
                var mil = m.GetILGenerator();
                mil.Emit(OpCodes.Call, typeof(NxUnityHost).GetMethod(name, BindingFlags.Public | BindingFlags.Static));
                mil.Emit(OpCodes.Ret);
            }
            var type = tb.CreateType();

            ClassInjector.RegisterTypeInIl2Cpp(type);

            // var go = new GameObject("WarpforgeNx"); Object.DontDestroyOnLoad(go); go.AddComponent(Il2CppType.From(type));
            var go = Activator.CreateInstance(gameObject, "WarpforgeNx");
            var ddol = unityObject.GetMethod("DontDestroyOnLoad", BindingFlags.Public | BindingFlags.Static, null, new[] { unityObject }, null)
                       ?? throw new Exception("Object.DontDestroyOnLoad not found");
            ddol.Invoke(null, new[] { go });
            var il2cppType = Il2CppType.From(type);
            var addComponent = gameObject.GetMethods(BindingFlags.Public | BindingFlags.Instance)
                .First(x => x.Name == "AddComponent" && !x.IsGenericMethod && x.GetParameters().Length == 1
                            && x.GetParameters()[0].ParameterType.IsInstanceOfType(il2cppType));
            s_component = addComponent.Invoke(go, new object[] { il2cppType });
            Entry.Log("loader: NxComponent injected (OnGUI/OnApplicationQuit now reach mods)");
        }
    }
}
