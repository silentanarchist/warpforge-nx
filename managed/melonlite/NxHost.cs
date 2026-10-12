// The Switch host's side of MelonLoader-lite: load mods, run their callbacks.
// Called by WarpforgeNx.Entry (nxloader.dll) on the engine main thread.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;

namespace MelonLoader.Switch
{
    public static class NxHost
    {
        static readonly List<MelonBase> s_melons = new();
        static readonly Dictionary<MelonBase, int> s_updateErrors = new();

        public static int LoadMods()
        {
            var dir = Utils.MelonEnvironment.ModsDirectory;
            Directory.CreateDirectory(dir);
            Directory.CreateDirectory(Utils.MelonEnvironment.UserDataDirectory);
            var files = Directory.GetFiles(dir, "*.dll");
            MelonLogger.Msg($"MelonLoader-lite (Switch): {files.Length} file(s) in {dir}");
            foreach (var file in files)
            {
                try { LoadMod(file); }
                catch (Exception e) { MelonLogger.Error($"Failed to load {Path.GetFileName(file)}", e); }
            }
            return s_melons.Count;
        }

        static void LoadMod(string file)
        {
            MelonLogger.Msg($"Loading {Path.GetFileName(file)} ...");
            var asm = Assembly.LoadFrom(file);
            var info = asm.GetCustomAttribute<MelonInfoAttribute>();
            if (info == null) { MelonLogger.Warning($"{Path.GetFileName(file)} has no [MelonInfo] -- not a mod, skipped"); return; }
            if (!typeof(MelonBase).IsAssignableFrom(info.SystemType)) { MelonLogger.Error($"{info.Name}: {info.SystemType} is not a MelonMod"); return; }

            var melon = (MelonBase)Activator.CreateInstance(info.SystemType);
            var ma = new MelonAssembly(asm, file);
            melon.Info = info;
            melon.MelonAssembly = ma;
            melon.Games = asm.GetCustomAttributes<MelonGameAttribute>().ToArray();
            melon.ID = info.Name;
            melon.LoggerInstance = new MelonLogger.Instance(info.Name, melon.ConsoleColor);
            melon.HarmonyInstance = new HarmonyLib.Harmony($"{asm.FullName}:{info.Name}");
            ma.loadedMelons.Add(melon);
            MelonBase._registeredMelons.Add(melon);
            if (melon is MelonMod mod) MelonTypeBase<MelonMod>._registeredMelons.Add(mod);
            melon.Registered = true;
            s_melons.Add(melon);
            MelonLogger.Msg($"Loaded {melon.MelonTypeName} \"{info.Name}\" v{info.Version} by {info.Author}{(ma.HarmonyDontPatchAll ? " (applies its own patches)" : "")}");

            var clock = System.Diagnostics.Stopwatch.StartNew();
            MelonLogger.Msg($"{info.Name}: OnInitializeMelon ...");
            Call(melon, "OnEarlyInitializeMelon", m => m.OnEarlyInitializeMelon());
            Call(melon, "OnInitializeMelon", m => m.OnInitializeMelon());
            if (!ma.HarmonyDontPatchAll)
                Call(melon, "PatchAll", m => m.HarmonyInstance.PatchAll(asm));
            Call(melon, "OnApplicationStart", m => m.OnApplicationStart());
            Call(melon, "OnLateInitializeMelon", m => m.OnLateInitializeMelon());
            MelonLogger.Msg($"{info.Name} initialised in {clock.ElapsedMilliseconds} ms");
        }

        static void Call(MelonBase m, string what, Action<MelonBase> a)
        {
            try { a(m); }
            catch (Exception e) { MelonLogger.Error($"{m.Info?.Name}.{what} threw", e); }
        }

        public static void Update()
        {
            foreach (var m in s_melons)
            {
                try { m.OnUpdate(); }
                catch (Exception e)
                {
                    s_updateErrors.TryGetValue(m, out var n);
                    s_updateErrors[m] = ++n;
                    if (n <= 5 || n % 1000 == 0) MelonLogger.Error($"{m.Info?.Name}.OnUpdate threw (#{n})", e);
                }
            }
        }

        public static void Quit()
        {
            foreach (var m in s_melons) Call(m, "OnApplicationQuit", x => x.OnApplicationQuit());
        }
    }
}
