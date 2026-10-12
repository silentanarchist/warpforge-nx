using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Reflection;
using MelonLoader.Logging;

namespace MelonLoader
{
    [AttributeUsage(AttributeTargets.Assembly)]
    public class MelonInfoAttribute : Attribute
    {
        public MelonInfoAttribute(Type type, string name, string version, string author, string downloadLink = null)
        { SystemType = type; Name = name; Version = version; Author = author; DownloadLink = downloadLink; }
        public Type SystemType { get; internal set; }
        public string Name { get; internal set; }
        public string Version { get; internal set; }
        public string Author { get; internal set; }
        public string DownloadLink { get; internal set; }
    }

    [AttributeUsage(AttributeTargets.Assembly, AllowMultiple = true)]
    public class MelonGameAttribute : Attribute
    {
        public MelonGameAttribute(string developer = null, string name = null) { Developer = developer; Name = name; }
        public string Developer { get; internal set; }
        public string Name { get; internal set; }
        public bool Universal => string.IsNullOrEmpty(Developer) || string.IsNullOrEmpty(Name);
    }

    [AttributeUsage(AttributeTargets.Assembly)]
    public class HarmonyDontPatchAllAttribute : Attribute { public HarmonyDontPatchAllAttribute() { } }

    public sealed class MelonAssembly
    {
        internal MelonAssembly(Assembly assembly, string location)
        {
            Assembly = assembly; Location = location;
            HarmonyDontPatchAll = assembly.GetCustomAttribute<HarmonyDontPatchAllAttribute>() != null;
        }
        public bool HarmonyDontPatchAll { get; private set; } = true;
        public string Hash { get; private set; } = "";
        public Assembly Assembly { get; private set; }
        public string Location { get; private set; }
        internal readonly List<MelonBase> loadedMelons = new();
        public ReadOnlyCollection<MelonBase> LoadedMelons => loadedMelons.AsReadOnly();
    }

    public abstract class MelonBase
    {
        internal static readonly List<MelonBase> _registeredMelons = new();
        public static ReadOnlyCollection<MelonBase> RegisteredMelons => _registeredMelons.AsReadOnly();

        public MelonAssembly MelonAssembly { get; internal set; }
        public int Priority { get; internal set; }
        public ColorARGB ConsoleColor { get; internal set; } = MelonLogger.DefaultMelonColor;
        public ColorARGB AuthorConsoleColor { get; internal set; } = MelonLogger.DefaultTextColor;
        public MelonInfoAttribute Info { get; internal set; }
        public MelonGameAttribute[] Games { get; internal set; } = Array.Empty<MelonGameAttribute>();
        public HarmonyLib.Harmony HarmonyInstance { get; internal set; }
        public MelonLogger.Instance LoggerInstance { get; internal set; }
        public string ID { get; internal set; }
        public bool Registered { get; internal set; }
        public abstract string MelonTypeName { get; }
        public Assembly Assembly => MelonAssembly?.Assembly;
        public bool HarmonyDontPatchAll => MelonAssembly?.HarmonyDontPatchAll ?? true;

        public virtual void OnPreSupportModule() { }
        public virtual void OnUpdate() { }
        public virtual void OnFixedUpdate() { }
        public virtual void OnLateUpdate() { }
        public virtual void OnGUI() { }
        public virtual void OnApplicationQuit() { }
        public virtual void OnPreferencesSaved() { }
        public virtual void OnPreferencesSaved(string filepath) { }
        public virtual void OnPreferencesLoaded() { }
        public virtual void OnPreferencesLoaded(string filepath) { }
        public virtual void OnEarlyInitializeMelon() { }
        public virtual void OnInitializeMelon() { }
        public virtual void OnLateInitializeMelon() { }
        public virtual void OnDeinitializeMelon() { }
        public virtual void OnApplicationLateStart() { }
        public virtual void OnApplicationStart() { }
        public virtual void OnModSettingsApplied() { }
    }

    public abstract class MelonTypeBase<T> : MelonBase where T : MelonTypeBase<T>
    {
        new public static ReadOnlyCollection<T> RegisteredMelons => _registeredMelons.AsReadOnly();
        new internal static List<T> _registeredMelons = new();
        public static string TypeName { get; protected internal set; }
        public sealed override string MelonTypeName => TypeName;
    }

    public abstract class MelonMod : MelonTypeBase<MelonMod>
    {
        static MelonMod() { TypeName = "Mod"; }
        public virtual void OnSceneWasLoaded(int buildIndex, string sceneName) { }
        public virtual void OnSceneWasInitialized(int buildIndex, string sceneName) { }
        public virtual void OnSceneWasUnloaded(int buildIndex, string sceneName) { }
    }

    public abstract class MelonPlugin : MelonTypeBase<MelonPlugin>
    {
        static MelonPlugin() { TypeName = "Plugin"; }
    }
}

namespace MelonLoader.Utils
{
    public static class MelonEnvironment
    {
        // Set by the Switch host (WarpforgeNx.Entry) before any mod loads.
        internal static string Base = "sdmc:/switch/warpforge_nx/MelonLoader";
        public static void Initialize(string melonLoaderDir) { Base = melonLoaderDir; }
        public static string MelonBaseDirectory => Base;
        public static string MelonLoaderDirectory => Base;
        public static string GameRootDirectory => System.IO.Path.GetDirectoryName(Base);
        public static string ModsDirectory => System.IO.Path.Combine(Base, "Mods");
        public static string PluginsDirectory => System.IO.Path.Combine(Base, "Plugins");
        public static string UserLibsDirectory => System.IO.Path.Combine(Base, "UserLibs");
        public static string UserDataDirectory => System.IO.Path.Combine(Base, "UserData");
        public static string MelonLoaderLogsDirectory => System.IO.Path.Combine(Base, "Logs");
        public static string Il2CppAssembliesDirectory => System.IO.Path.Combine(Base, "Il2CppAssemblies");
        public static string PackageName => "com.Everguild.WarhammerWarpforge";
        public static bool IsDotnetRuntime => true;
        public static bool IsMonoRuntime => false;
    }
}
