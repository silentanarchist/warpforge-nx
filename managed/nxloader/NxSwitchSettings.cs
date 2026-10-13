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
using System.Reflection;

namespace WarpforgeNx
{
    public static class NxSwitchSettings
    {
        public static int TextureMipLimit { get; private set; }

        static PropertyInfo s_mipProp;
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
                if (s_mipProp == null)
                {
                    var core = AppDomain.CurrentDomain.GetAssemblies().FirstOrDefault(a => a.GetName().Name == "UnityEngine.CoreModule");
                    if (core == null) return;
                    var qs = core.GetType("UnityEngine.QualitySettings", true);
                    s_mipProp = qs.GetProperty("globalTextureMipmapLimit", BindingFlags.Public | BindingFlags.Static)
                                ?? qs.GetProperty("masterTextureLimit", BindingFlags.Public | BindingFlags.Static)
                                ?? throw new Exception("QualitySettings has no globalTextureMipmapLimit/masterTextureLimit");
                }
                int cur = Convert.ToInt32(s_mipProp.GetValue(null));
                if (cur != TextureMipLimit)
                {
                    s_mipProp.SetValue(null, TextureMipLimit);
                    if (s_reapplied++ < 10)
                        Entry.Log($"loader: texture mip limit {cur} -> {TextureMipLimit} (switch.cfg)");
                }
            }
            catch (Exception e) { s_failed++; Entry.Log("loader: texture mip limit could not be set: " + e.Message); }
        }
    }
}
