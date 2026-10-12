// MelonPreferences, reduced: categories, typed entries, one TOML-style file per category
// (string / bool / integer / floating-point values -- what mods keep in settings files).
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;

namespace MelonLoader.Preferences
{
    public abstract class ValueValidator
    {
        public abstract bool IsValid(object value);
        public abstract object EnsureValid(object value);
    }

    public interface IValueRange { object MinValue { get; } object MaxValue { get; } }

    public class ValueRange<T> : ValueValidator, IValueRange where T : IComparable
    {
        public T MinValue { get; }
        public T MaxValue { get; }
        object IValueRange.MinValue => MinValue;
        object IValueRange.MaxValue => MaxValue;
        public ValueRange(T minValue, T maxValue) { MinValue = minValue; MaxValue = maxValue; }
        public override bool IsValid(object value) => value is T v && v.CompareTo(MinValue) >= 0 && v.CompareTo(MaxValue) <= 0;
        public override object EnsureValid(object value)
        {
            var v = (T)value;
            if (v.CompareTo(MinValue) < 0) return MinValue;
            if (v.CompareTo(MaxValue) > 0) return MaxValue;
            return v;
        }
    }
}

namespace MelonLoader
{
    using MelonLoader.Preferences;

    public static class MelonPreferences
    {
        internal static readonly List<MelonPreferences_Category> Categories = new();
        public static MelonPreferences_Category CreateCategory(string identifier) => CreateCategory(identifier, null, false);
        public static MelonPreferences_Category CreateCategory(string identifier, string display_name = null) => CreateCategory(identifier, display_name, false);
        public static MelonPreferences_Category CreateCategory(string identifier, string display_name = null, bool is_hidden = false, bool should_save = true)
        {
            var c = GetCategory(identifier);
            if (c != null) return c;
            c = new MelonPreferences_Category(identifier, display_name ?? identifier, is_hidden, should_save);
            Categories.Add(c);
            return c;
        }
        public static MelonPreferences_Category GetCategory(string identifier) => Categories.FirstOrDefault(c => c.Identifier == identifier);
        public static void Save() { foreach (var c in Categories) c.SaveToFile(false); }
    }

    public class MelonPreferences_Category
    {
        public string Identifier { get; internal set; }
        public string DisplayName { get; set; }
        public bool IsHidden { get; set; }
        public bool IsInlined { get; set; }
        public string FilePath { get; private set; }
        readonly bool _shouldSave;
        public readonly List<MelonPreferences_Entry> Entries = new();
        readonly Dictionary<string, string> _loadedRaw = new();

        internal MelonPreferences_Category(string id, string name, bool hidden, bool save)
        { Identifier = id; DisplayName = name; IsHidden = hidden; _shouldSave = save; }

        public MelonPreferences_Entry CreateEntry<T>(string identifier, T default_value, string display_name, bool is_hidden)
            => CreateEntry(identifier, default_value, display_name, null, is_hidden, false, null, null);
        public MelonPreferences_Entry<T> CreateEntry<T>(string identifier, T default_value, string display_name,
            string description, bool is_hidden, bool dont_save_default, ValueValidator validator)
            => CreateEntry(identifier, default_value, display_name, description, is_hidden, dont_save_default, validator, null);
        public MelonPreferences_Entry<T> CreateEntry<T>(string identifier, T default_value, string display_name = null,
            string description = null, bool is_hidden = false, bool dont_save_default = false, ValueValidator validator = null, string oldIdentifier = null)
        {
            if (string.IsNullOrEmpty(identifier)) throw new Exception("identifier is null or empty when calling CreateEntry");
            if (GetEntry(identifier) is MelonPreferences_Entry<T> existing) return existing;
            var e = new MelonPreferences_Entry<T>
            {
                Identifier = identifier, DisplayName = display_name ?? identifier, Description = description,
                IsHidden = is_hidden, DontSaveDefault = dont_save_default, Validator = validator, Category = this,
                DefaultValue = default_value,
            };
            e.SetRaw(default_value);
            if (_loadedRaw.TryGetValue(identifier, out var raw) || (oldIdentifier != null && _loadedRaw.TryGetValue(oldIdentifier, out raw)))
                e.LoadText(raw);
            Entries.Add(e);
            return e;
        }

        public MelonPreferences_Entry GetEntry(string identifier) => Entries.FirstOrDefault(x => x.Identifier == identifier);
        public MelonPreferences_Entry<T> GetEntry<T>(string identifier) => (MelonPreferences_Entry<T>)GetEntry(identifier);
        public bool HasEntry(string identifier) => GetEntry(identifier) != null;

        public void SetFilePath(string filepath) => SetFilePath(filepath, true, true);
        public void SetFilePath(string filepath, bool autoload) => SetFilePath(filepath, autoload, true);
        public void SetFilePath(string filepath, bool autoload, bool printmsg)
        {
            FilePath = filepath;
            if (autoload) LoadFromFile(printmsg);
        }
        public void ResetFilePath() => FilePath = null;

        string EffectivePath => FilePath ?? Path.Combine(Utils.MelonEnvironment.UserDataDirectory, "MelonPreferences.cfg");

        public void LoadFromFile(bool printmsg = true)
        {
            var path = EffectivePath;
            if (!File.Exists(path)) return;
            string section = null;
            foreach (var rawLine in File.ReadAllLines(path))
            {
                var line = rawLine.Trim();
                if (line.Length == 0 || line.StartsWith("#")) continue;
                if (line.StartsWith("[") && line.EndsWith("]")) { section = line.Substring(1, line.Length - 2).Trim().Trim('"'); continue; }
                if (section != Identifier) continue;
                int eq = line.IndexOf('=');
                if (eq <= 0) continue;
                var key = line.Substring(0, eq).Trim().Trim('"');
                var val = line.Substring(eq + 1).Trim();
                _loadedRaw[key] = val;
                GetEntry(key)?.LoadText(val);
            }
            if (printmsg) MelonLogger.Msg($"Loaded preferences from {path}");
        }

        public void SaveToFile(bool printmsg = true)
        {
            if (!_shouldSave) return;
            var path = EffectivePath;
            try
            {
                Directory.CreateDirectory(Path.GetDirectoryName(path));
                var sb = new StringBuilder();
                sb.Append('[').Append(Identifier).Append("]\n");
                foreach (var e in Entries)
                {
                    if (!string.IsNullOrEmpty(e.Description))
                        foreach (var d in e.Description.Split('\n')) sb.Append("# ").Append(d).Append('\n');
                    sb.Append(e.Identifier).Append(" = ").Append(e.SaveText()).Append('\n');
                }
                // keep values for entries this run did not create
                foreach (var kv in _loadedRaw)
                    if (GetEntry(kv.Key) == null) sb.Append(kv.Key).Append(" = ").Append(kv.Value).Append('\n');
                File.WriteAllText(path, sb.ToString());
                if (printmsg) MelonLogger.Msg($"Saved preferences to {path}");
            }
            catch (Exception ex) { MelonLogger.Warning($"Could not save preferences to {path}: {ex.Message}"); }
        }
    }

    public abstract class MelonPreferences_Entry
    {
        public string Identifier { get; internal set; }
        public string DisplayName { get; set; }
        public string Description { get; set; }
        public string Comment { get; set; }
        public bool IsHidden { get; set; }
        public bool DontSaveDefault { get; set; }
        public MelonPreferences_Category Category { get; internal set; }
        public ValueValidator Validator { get; internal set; }
        public abstract object BoxedValue { get; set; }
        public abstract object BoxedEditedValue { get; set; }
        public abstract Type GetReflectedType();
        internal abstract void LoadText(string text);
        internal abstract string SaveText();
    }

    public class MelonPreferences_Entry<T> : MelonPreferences_Entry
    {
        private T myValue;
        public T Value
        {
            get => myValue;
            set
            {
                if (Validator != null) value = (T)Validator.EnsureValid(value);
                if (Equals(myValue, value)) return;
                var old = myValue; myValue = value; EditedValue = value;
                try { OnEntryValueChanged?.Invoke(old, value); } catch { }
            }
        }
        public T EditedValue { get; set; }
        public T DefaultValue { get; set; }
        public override object BoxedValue { get => myValue; set => Value = (T)value; }
        public override object BoxedEditedValue { get => EditedValue; set => EditedValue = (T)value; }
        public override Type GetReflectedType() => typeof(T);
        public event Action<T, T> OnEntryValueChanged;
        public void ResetToDefault() => Value = DefaultValue;
        internal void SetRaw(T v) { myValue = v; EditedValue = v; }

        internal override void LoadText(string text)
        {
            try
            {
                object v;
                var t = typeof(T);
                if (t == typeof(string)) v = Unquote(text);
                else if (t == typeof(bool)) v = text.Equals("true", StringComparison.OrdinalIgnoreCase);
                else if (t.IsEnum) v = Enum.Parse(t, Unquote(text), true);
                else v = Convert.ChangeType(Unquote(text), t, CultureInfo.InvariantCulture);
                SetRaw((T)v);
            }
            catch (Exception e) { MelonLogger.Warning($"Preference {Identifier}: could not read \"{text}\" ({e.Message}); keeping the default"); }
        }

        internal override string SaveText()
        {
            object v = myValue;
            switch (v)
            {
                case null: return "\"\"";
                case string s: return Quote(s);
                case bool b: return b ? "true" : "false";
                case Enum e: return Quote(e.ToString());
                case IFormattable f: return f.ToString(null, CultureInfo.InvariantCulture);
                default: return Quote(v.ToString());
            }
        }

        static string Unquote(string s)
        {
            s = s.Trim();
            if (s.Length >= 2 && ((s[0] == '"' && s[^1] == '"') || (s[0] == '\'' && s[^1] == '\'')))
            {
                var inner = s.Substring(1, s.Length - 2);
                return s[0] == '"' ? inner.Replace("\\\"", "\"").Replace("\\\\", "\\") : inner;
            }
            int hash = s.IndexOf(" #", StringComparison.Ordinal);
            return hash >= 0 ? s.Substring(0, hash).Trim() : s;
        }
        static string Quote(string s) => "\"" + s.Replace("\\", "\\\\").Replace("\"", "\\\"") + "\"";
    }
}
