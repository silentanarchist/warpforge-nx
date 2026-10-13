// Switch-only settings, from MelonLoader/UserData/switch.cfg (created with defaults
// on first run). Kept apart from the mod's own WarpforgeRevival.cfg so the mod's
// preferences code never rewrites or drops them.
//
//   TextureMipLimit = 0   0 = full-resolution textures (as on the phone)
//                         1 = half width/height (a quarter of the memory)
//                         2 = quarter width/height
//
// Unity's QualitySettings.globalTextureMipmapLimit: the engine skips that many top
// mip levels of every texture. The game's own quality presets set it too
// (SetQualityLevel applies the preset's value), so it is checked once a second and
// put back if something changed it.
using System;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;

namespace WarpforgeNx
{
    public static class NxSwitchSettings
    {
        public static int TextureMipLimit { get; private set; }

        // Unity's own internal calls, resolved by name. Reflection over the generated
        // QualitySettings proxy made Mono NX abort ("should not be reached",
        // class.c:2324) on a member type it cannot map -- a black screen at boot.
        [DllImport("il2cpp")] static extern IntPtr il2cpp_resolve_icall([MarshalAs(UnmanagedType.LPStr)] string name);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int GetInt();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate void SetInt(int v);
        static GetInt s_get; static SetInt s_set;
        static int s_frames, s_failed, s_reapplied;

        public static void Load()
        {
            var path = Path.Combine(Entry.MelonDir, "UserData", "switch.cfg");
            try
            {
                if (!File.Exists(path))
                {
                    File.WriteAllText(path,
                        "# Switch-only settings (warpforge-nx)\n" +
                        "# TextureMipLimit: 0 = full-resolution textures, 1 = half (a quarter of the\n" +
                        "# texture memory), 2 = quarter. Takes effect at the next start.\n" +
                        "TextureMipLimit = 0\n");
                }
                foreach (var raw in File.ReadAllLines(path))
                {
                    var line = raw.Trim();
                    if (line.Length == 0 || line[0] == '#' || line[0] == ';') continue;
                    int eq = line.IndexOf('=');
                    if (eq <= 0) continue;
                    var key = line.Substring(0, eq).Trim();
                    var val = line.Substring(eq + 1).Trim().Trim('"');
                    if (key.Equals("TextureMipLimit", StringComparison.OrdinalIgnoreCase) && int.TryParse(val, out int n))
                        TextureMipLimit = Math.Max(0, Math.Min(3, n));
                }
                Entry.Log($"loader: switch.cfg: TextureMipLimit = {TextureMipLimit}");
            }
            catch (Exception e) { Entry.Log("loader: switch.cfg could not be read (" + e.Message + "); defaults used"); }
        }

        /// <summary>Every frame from Entry.OnUpdate; does real work about once a second.</summary>
        public static void Tick()
        {
            if (TextureMipLimit <= 0 || s_failed > 0) return;
            if (s_frames++ % 60 != 0) return;
            try
            {
                if (s_set == null)
                {
                    foreach (var name in new[] { "globalTextureMipmapLimit", "masterTextureLimit" })
                    {
                        IntPtr g = il2cpp_resolve_icall("UnityEngine.QualitySettings::get_" + name);
                        IntPtr p = il2cpp_resolve_icall("UnityEngine.QualitySettings::set_" + name);
                        if (p == IntPtr.Zero) continue;
                        s_set = Marshal.GetDelegateForFunctionPointer<SetInt>(p);
                        if (g != IntPtr.Zero) s_get = Marshal.GetDelegateForFunctionPointer<GetInt>(g);
                        Entry.Log("loader: texture mip limit through QualitySettings::set_" + name);
                        break;
                    }
                    if (s_set == null) { s_failed++; Entry.Log("loader: texture mip limit: no QualitySettings setter found -- setting ignored"); return; }
                }
                int cur = s_get != null ? s_get() : -1;
                if (cur != TextureMipLimit)
                {
                    s_set(TextureMipLimit);
                    if (s_reapplied++ < 10)
                        Entry.Log($"loader: texture mip limit {cur} -> {TextureMipLimit} (switch.cfg)");
                }
            }
            catch (Exception e) { s_failed++; Entry.Log("loader: texture mip limit could not be set: " + e.Message); }
        }
    }
}
