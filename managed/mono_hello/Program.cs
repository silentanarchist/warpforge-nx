// mono_hello -- milestone 1 probe for the Mono runtime inside warpforge_nx.
// Logs through the wrapper (wf_mono_log) so the lines land in debug.log, and
// exercises the things the mod will need: exceptions, threads, reflection,
// and -- the one that decides the Harmony question -- dynamic code
// (System.Reflection.Emit.DynamicMethod) under the interpreter.
using System;
using System.Reflection;
using System.Reflection.Emit;
using System.Runtime.InteropServices;
using System.Threading;

namespace WarpforgeNx
{
    static class Program
    {
        [DllImport("warpforge")] static extern void wf_mono_log(string line);

        static void Log(string s) { try { wf_mono_log(s); } catch { } }

        static int Main(string[] args)
        {
            Log("hello from managed code");
            Log($"runtime: {RuntimeInformation.FrameworkDescription} on {RuntimeInformation.OSDescription} / {RuntimeInformation.ProcessArchitecture}");
            Log($"clock: {DateTime.UtcNow:O}");

            try { throw new InvalidOperationException("expected"); }
            catch (InvalidOperationException e) { Log("exceptions: ok (" + e.Message + ")"); }

            int fromThread = 0;
            var t = new Thread(() => { Interlocked.Exchange(ref fromThread, 42); });
            t.Start(); t.Join(5000);
            Log("threads: " + (fromThread == 42 ? "ok" : "FAILED"));

            var m = typeof(Program).GetMethod(nameof(Add), BindingFlags.Static | BindingFlags.NonPublic);
            Log("reflection: " + (m != null && (int)m.Invoke(null, new object[] { 2, 3 })! == 5 ? "ok" : "FAILED"));

            try
            {
                var dm = new DynamicMethod("add", typeof(int), new[] { typeof(int), typeof(int) }, typeof(Program).Module, true);
                var il = dm.GetILGenerator();
                il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Ldarg_1); il.Emit(OpCodes.Add); il.Emit(OpCodes.Ret);
                var f = (Func<int, int, int>)dm.CreateDelegate(typeof(Func<int, int, int>));
                Log("DynamicMethod: " + (f(20, 22) == 42 ? "ok (IL emitted and run under the interpreter)" : "WRONG RESULT"));
            }
            catch (Exception e) { Log("DynamicMethod: FAILED -- " + e.GetType().Name + ": " + e.Message); }

            try
            {
                // Reverse P/Invoke: a managed delegate as a native function pointer.
                Func<int, int> twice = x => x * 2;
                var fp = Marshal.GetFunctionPointerForDelegate(twice);
                Log("function pointer for delegate: " + (fp != IntPtr.Zero ? "ok" : "null"));
            }
            catch (Exception e) { Log("function pointer for delegate: FAILED -- " + e.GetType().Name + ": " + e.Message); }

            Log("done");
            return 0;
        }

        static int Add(int a, int b) => a + b;
    }
}
