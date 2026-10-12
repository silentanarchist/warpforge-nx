// MonoMod platforms for an interpreter-only Mono on the Switch.
//
// MonoMod's default Mono platforms find the JIT's machine code for a managed method
// and overwrite it, unlocking memory through MonoPosixHelper/mprotect. Under Mono NX
// there is no machine code (interpreter) and code memory cannot be unlocked that
// way, and touching it took the whole process down -- HarmonyX does this from its
// static constructor (StackTraceFixes: two Detours + a NativeDetour on BCL methods).
//
// These replacements make every such request a NotSupportedException, which the
// callers catch (StackTraceFixes logs "Failed to apply stack trace fix" and moves on).
// Harmony patches on GAME methods do not use MonoMod at all here: they go through
// Il2CppDetourMethodPatcher -> NxDetourProvider -> wf_detour_install.
using System;
using System.Reflection;
using MonoMod.RuntimeDetour;

namespace WarpforgeNx
{
    static class NxMonoModPlatforms
    {
        public static void Install()
        {
            DetourHelper.Runtime = new RuntimePlatform();
            DetourHelper.Native = new NativePlatform();
        }

        static Exception No(string what) =>
            new NotSupportedException($"MonoMod {what} is not available under the Switch's interpreter-only runtime");

        sealed class RuntimePlatform : IDetourRuntimePlatform
        {
            public MethodBase GetIdentifiable(MethodBase method) => method;
            public IntPtr GetNativeStart(MethodBase method) => throw No("GetNativeStart");
            public MethodInfo CreateCopy(MethodBase method) => throw No("CreateCopy");
            public bool TryCreateCopy(MethodBase method, out MethodInfo dm) { dm = null; return false; }
            public void Pin(MethodBase method) { }
            public void Unpin(MethodBase method) { }
            public MethodBase GetDetourTarget(MethodBase from, MethodBase to) => to;
            public uint TryMemAllocScratchCloseTo(IntPtr target, out IntPtr ptr, int size) { ptr = IntPtr.Zero; return 0; }
            public bool OnMethodCompiledWillBeCalled => false;
            public event OnMethodCompiledEvent OnMethodCompiled { add { } remove { } }
        }

        sealed class NativePlatform : IDetourNativePlatform
        {
            public NativeDetourData Create(IntPtr from, IntPtr to, byte? type = null) => throw No("native detour");
            public void Free(NativeDetourData detour) { }
            public void Apply(NativeDetourData detour) => throw No("native detour");
            public void Copy(IntPtr src, IntPtr dst, byte type) => throw No("native detour");
            public void MakeWritable(IntPtr src, uint size) => throw No("MakeWritable");
            public void MakeExecutable(IntPtr src, uint size) => throw No("MakeExecutable");
            public void MakeReadWriteExecutable(IntPtr src, uint size) => throw No("MakeReadWriteExecutable");
            public void FlushICache(IntPtr src, uint size) { }
            public IntPtr MemAlloc(uint size) => throw No("MemAlloc");
            public void MemFree(IntPtr ptr) { }
        }
    }
}
