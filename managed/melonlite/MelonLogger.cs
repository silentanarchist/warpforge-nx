using System;
using System.Runtime.InteropServices;
using MelonLoader.Logging;

namespace MelonLoader
{
    public class MelonLogger
    {
        public static readonly ColorARGB DefaultMelonColor = ColorARGB.Cyan;
        public static readonly ColorARGB DefaultTextColor = ColorARGB.LightGray;

        [DllImport("warpforge")] static extern void wf_mono_log(string line);
        static readonly object s_lock = new();

        internal static void Write(string level, string section, string txt)
        {
            var line = $"[{DateTime.Now:HH:mm:ss.fff}] {(section != null ? "[" + section + "] " : "")}{level}{txt}";
            lock (s_lock) { try { wf_mono_log(line); } catch { } }
        }

        internal static void NativeMsg(ColorARGB nameColor, ColorARGB textColor, string section, string txt, bool direct = false)
        {
            Write("", section, txt);
            try { MsgDrawingCallbackHandler?.Invoke(nameColor, textColor, section, txt); } catch { }
            try { MsgCallbackHandler?.Invoke(ConsoleColor.Cyan, ConsoleColor.Gray, section, txt); } catch { }
        }
        internal static void NativeWarning(string section, string txt)
        {
            Write("[WARNING] ", section, txt);
            try { WarningCallbackHandler?.Invoke(section, txt); } catch { }
        }
        internal static void NativeError(string section, string txt)
        {
            Write("[ERROR] ", section, txt);
            try { ErrorCallbackHandler?.Invoke(section, txt); } catch { }
        }
        static ColorARGB ToColor(ConsoleColor c) => DefaultTextColor;

        public static void Msg(object obj) => NativeMsg(DefaultMelonColor, DefaultTextColor, null, obj?.ToString());
        public static void Msg(string txt) => NativeMsg(DefaultMelonColor, DefaultTextColor, null, txt);
        public static void Msg(string txt, params object[] args) => NativeMsg(DefaultMelonColor, DefaultTextColor, null, string.Format(txt, args));
        public static void Msg(ConsoleColor txt_color, object obj) => NativeMsg(DefaultMelonColor, ToColor(txt_color), null, obj?.ToString());
        public static void Msg(ConsoleColor txt_color, string txt) => NativeMsg(DefaultMelonColor, ToColor(txt_color), null, txt);
        public static void Msg(ConsoleColor txt_color, string txt, params object[] args) => NativeMsg(DefaultMelonColor, ToColor(txt_color), null, string.Format(txt, args));
        public static void MsgDirect(string txt) => NativeMsg(DefaultMelonColor, DefaultTextColor, null, txt, true);
        public static void MsgDirect(ColorARGB txt_color, string txt) => NativeMsg(DefaultMelonColor, txt_color, null, txt, true);
        public static void Msg(ColorARGB txt_color, object obj) => NativeMsg(DefaultMelonColor, txt_color, null, obj?.ToString());
        public static void Msg(ColorARGB txt_color, string txt) => NativeMsg(DefaultMelonColor, txt_color, null, txt);
        public static void Msg(ColorARGB txt_color, string txt, params object[] args) => NativeMsg(DefaultMelonColor, txt_color, null, string.Format(txt, args));
        public static void Warning(object obj) => NativeWarning(null, obj?.ToString());
        public static void Warning(string txt) => NativeWarning(null, txt);
        public static void Warning(string txt, params object[] args) => NativeWarning(null, string.Format(txt, args));
        public static void Error(object obj) => NativeError(null, obj?.ToString());
        public static void Error(string txt) => NativeError(null, txt);
        public static void Error(string txt, params object[] args) => NativeError(null, string.Format(txt, args));
        public static void Error(string txt, Exception ex) => NativeError(null, $"{txt}\n{ex}");
        public static void WriteLine(int length = 30) => MsgDirect(new string('-', length));
        public static void WriteLine(ColorARGB color, int length = 30) => MsgDirect(color, new string('-', length));
        public static void WriteSpacer() => MsgDirect("");
        public static void BigError(string namesection, string txt) => NativeError(namesection, txt);

        public static event Action<ConsoleColor, ConsoleColor, string, string> MsgCallbackHandler;
        public static event Action<ColorARGB, ColorARGB, string, string> MsgDrawingCallbackHandler;
        public static event Action<string, string> WarningCallbackHandler;
        public static event Action<string, string> ErrorCallbackHandler;

        public class Instance
        {
            private string Name;
            private ColorARGB DrawingColor = DefaultMelonColor;
            public Instance(string name) => Name = name?.Replace(" ", "_");
            public Instance(string name, ConsoleColor color) : this(name) { }
            public Instance(string name, ColorARGB color) : this(name) => DrawingColor = color;
            public void Msg(object obj) => NativeMsg(DrawingColor, DefaultTextColor, Name, obj?.ToString());
            public void Msg(string txt) => NativeMsg(DrawingColor, DefaultTextColor, Name, txt);
            public void Msg(string txt, params object[] args) => NativeMsg(DrawingColor, DefaultTextColor, Name, string.Format(txt, args));
            public void Msg(ConsoleColor txt_color, object obj) => NativeMsg(DrawingColor, ToColor(txt_color), Name, obj?.ToString());
            public void Msg(ConsoleColor txt_color, string txt) => NativeMsg(DrawingColor, ToColor(txt_color), Name, txt);
            public void Msg(ConsoleColor txt_color, string txt, params object[] args) => NativeMsg(DrawingColor, ToColor(txt_color), Name, string.Format(txt, args));
            public void Msg(ColorARGB txt_color, object obj) => NativeMsg(DrawingColor, txt_color, Name, obj?.ToString());
            public void Msg(ColorARGB txt_color, string txt) => NativeMsg(DrawingColor, txt_color, Name, txt);
            public void Msg(ColorARGB txt_color, string txt, params object[] args) => NativeMsg(DrawingColor, txt_color, Name, string.Format(txt, args));
            public void Warning(object obj) => NativeWarning(Name, obj?.ToString());
            public void Warning(string txt) => NativeWarning(Name, txt);
            public void Warning(string txt, params object[] args) => NativeWarning(Name, string.Format(txt, args));
            public void Error(object obj) => NativeError(Name, obj?.ToString());
            public void Error(string txt) => NativeError(Name, txt);
            public void Error(string txt, params object[] args) => NativeError(Name, string.Format(txt, args));
            public void Error(string txt, Exception ex) => NativeError(Name, $"{txt}\n{ex}");
            public void WriteSpacer() => MelonLogger.WriteSpacer();
            public void WriteLine(int length = 30) => MelonLogger.WriteLine(length);
            public void WriteLine(ColorARGB color, int length = 30) => MelonLogger.WriteLine(color, length);
            public void BigError(string txt) => MelonLogger.BigError(Name, txt);
        }
    }
}
