// MonoMod 22.x on the .NET 9 class library.
//
// HarmonyX 2.10 (what MelonLoader and the Il2CppInterop fork are built on) uses
// MonoMod 22, whose CecilILGenerator makes LocalBuilder objects by calling the
// class's own constructor through reflection. In .NET 9, LocalBuilder became
// abstract (the runtime's own type is an internal subclass), so that call throws
// "Cannot create an instance of System.Reflection.Emit.LocalBuilder because it is
// an abstract class" -- every Harmony patch failed with "IL Compile Error".
// The phone's MelonLoader runs an older .NET and never hit this. Mono NX ships
// the .NET 9 class library.
//
// Fix: give CecilILGenerator a concrete LocalBuilder to construct (its 4-argument
// path: index, type, method, pinned). Only the identity of the object matters to
// MonoMod (it maps each handle to a Cecil VariableDefinition).
using System;
using System.Reflection;
using System.Reflection.Emit;
using System.Runtime.CompilerServices;
using MonoMod.Utils.Cil;

namespace WarpforgeNx
{
    sealed class NxLocalBuilder : LocalBuilder
    {
        readonly int _index; readonly Type _type; readonly bool _pinned;
        public NxLocalBuilder(int index, Type type, MethodInfo method, bool pinned) { _index = index; _type = type; _pinned = pinned; }
        public override Type LocalType => _type;
        public override bool IsPinned => _pinned;
        public override int LocalIndex => _index;
    }

    static class NxMonoModFixes
    {
        public static void Install()
        {
            var gen = typeof(CecilILGenerator);
            RuntimeHelpers.RunClassConstructor(gen.TypeHandle);   // let its own static init run first
            var ctorField = gen.GetField("c_LocalBuilder", BindingFlags.NonPublic | BindingFlags.Static);
            var paramsField = gen.GetField("c_LocalBuilder_params", BindingFlags.NonPublic | BindingFlags.Static);
            if (ctorField == null || paramsField == null) { Entry.Log("loader: CecilILGenerator fields not found -- MonoMod version changed?"); return; }
            var current = (ConstructorInfo)ctorField.GetValue(null);
            if (current != null && !current.DeclaringType.IsAbstract) { Entry.Log("loader: LocalBuilder is concrete here, no fix needed"); return; }
            var ours = typeof(NxLocalBuilder).GetConstructors()[0];
            Set(ctorField, ours);
            Set(paramsField, 4);
            var ok = ctorField.GetValue(null) == (object)ours && (int)paramsField.GetValue(null) == 4;
            Entry.Log($"loader: MonoMod LocalBuilder fix {(ok ? "installed" : "FAILED (fields unchanged)")}");
        }

        // static readonly: Mono allows FieldInfo.SetValue; if not, store it with a DynamicMethod.
        static void Set(FieldInfo f, object value)
        {
            try { f.SetValue(null, value); if (Equals(f.GetValue(null), value)) return; }
            catch (Exception e) { Entry.Log($"loader: SetValue {f.Name}: {e.GetType().Name}, using IL"); }
            var dm = new DynamicMethod("set_" + f.Name, null, new[] { typeof(object) }, typeof(NxMonoModFixes).Module, true);
            var il = dm.GetILGenerator();
            il.Emit(OpCodes.Ldarg_0);
            il.Emit(f.FieldType.IsValueType ? OpCodes.Unbox_Any : OpCodes.Castclass, f.FieldType);
            il.Emit(OpCodes.Stsfld, f);
            il.Emit(OpCodes.Ret);
            ((Action<object>)dm.CreateDelegate(typeof(Action<object>)))(value);
        }
    }
}
