// Il2CppInterop's detour provider for warpforge_nx: every native hook the interop
// runtime (ClassInjector, DelegateSupport, HarmonySupport's Il2CppDetourMethodPatcher)
// asks for goes through the wrapper's wf_detour_install, which patches the function
// and returns a trampoline that runs the original (source/warpforge_detour.c).
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using Il2CppInterop.Runtime.Injection;

namespace WarpforgeNx
{
    public sealed class NxDetourProvider : IDetourProvider
    {
        [DllImport("warpforge")] static extern IntPtr wf_detour_install(IntPtr target, IntPtr replacement);
        [DllImport("warpforge")] static extern int wf_detour_remove(IntPtr target);

        public IDetour Create<TDelegate>(nint original, TDelegate target) where TDelegate : Delegate
            => new NxDetour(original, target);

        sealed class NxDetour : IDetour
        {
            // Both the managed delegate and the trampoline delegates must stay alive for as
            // long as the hook is in: the native side only holds raw pointers.
            static readonly List<object> s_keepAlive = new();

            readonly Delegate _target;
            public nint Target { get; }
            public nint Detour { get; }
            public nint OriginalTrampoline { get; private set; }

            public NxDetour(nint original, Delegate target)
            {
                Target = original;
                _target = target;
                lock (s_keepAlive) s_keepAlive.Add(target);
                Detour = Marshal.GetFunctionPointerForDelegate(target);
                // Il2CppInterop asks for the trampoline BEFORE calling Apply(), so the
                // patch goes in right away; Apply() is then a no-op.
                OriginalTrampoline = wf_detour_install(Target, Detour);
                if (OriginalTrampoline == IntPtr.Zero)
                    throw new InvalidOperationException($"wf_detour_install refused 0x{Target:x} (see [detour] in debug.log)");
            }

            public void Apply() { }

            public T GenerateTrampoline<T>() where T : Delegate
            {
                var d = Marshal.GetDelegateForFunctionPointer<T>(OriginalTrampoline);
                lock (s_keepAlive) s_keepAlive.Add(d);
                return d;
            }

            bool _removed;
            // Harmony disposes a method's old hook before it re-patches that method (a mod
            // patching the same method twice). Leaving it in made the old and the new patch
            // chain, so both ran.
            public void Dispose()
            {
                if (_removed) return;
                _removed = true;
                wf_detour_remove(Target);
            }
        }
    }
}
