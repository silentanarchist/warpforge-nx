// Load-time adjustments to mods for what the Switch's runtime cannot do. A mod's
// assembly is read with Mono.Cecil, the listed methods get a new body that calls
// the Switch replacement, and the result is loaded from memory. The file on the
// card is not changed (the mod's own hash of itself stays right).
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using Mono.Cecil;
using Mono.Cecil.Cil;

namespace MelonLoader.Switch
{
    static class NxModShims
    {
        // type, method, parameter types (full names) -> replacement (static, same signature)
        static readonly (string type, string method, string[] args, System.Reflection.MethodInfo with, string why)[] Shims =
        {
            ("WarpforgeRevival.Net", "Client", new[] { "System.TimeSpan", "System.Boolean" },
             typeof(NxHttp).GetMethod(nameof(NxHttp.Client)),
             "https through the wrapper's TLS (Mono NX has none)"),
        };

        public static byte[] Apply(string file)
        {
            var bytes = File.ReadAllBytes(file);
            var resolver = new DefaultAssemblyResolver();
            foreach (var d in new[] { Path.GetDirectoryName(file), Path.Combine(Utils.MelonEnvironment.MelonLoaderDirectory, "Loader"),
                                      Utils.MelonEnvironment.Il2CppAssembliesDirectory, "/mono/lib_net9.0", "/mono/framework_net9.0" })
                if (Directory.Exists(d)) resolver.AddSearchDirectory(d);
            using var asm = AssemblyDefinition.ReadAssembly(new MemoryStream(bytes), new ReaderParameters { AssemblyResolver = resolver, ReadSymbols = false });
            int changed = 0;
            foreach (var s in Shims)
            {
                var type = asm.MainModule.GetType(s.type);
                var m = type?.Methods.FirstOrDefault(x => x.Name == s.method && x.HasBody
                         && x.Parameters.Select(p => p.ParameterType.FullName).SequenceEqual(s.args));
                if (m == null || s.with == null) continue;
                var body = m.Body;
                body.Instructions.Clear(); body.Variables.Clear(); body.ExceptionHandlers.Clear();
                var il = body.GetILProcessor();
                for (int i = 0; i < m.Parameters.Count; i++) il.Emit(OpCodes.Ldarg, m.Parameters[i]);
                il.Emit(OpCodes.Call, asm.MainModule.ImportReference(s.with));
                il.Emit(OpCodes.Ret);
                MelonLogger.Msg($"[switch] {s.type}.{s.method} now uses {s.with.DeclaringType.Name}.{s.with.Name}: {s.why}");
                changed++;
            }
            if (changed == 0) return null;
            var ms = new MemoryStream();
            asm.Write(ms);
            return ms.ToArray();
        }
    }
}
