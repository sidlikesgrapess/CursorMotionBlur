using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Windows.Forms;
using Microsoft.Win32;

namespace CursorMotionBlur
{
    /// <summary>User settings, stored as a small key=value file in %APPDATA%\CursorMotionBlur.</summary>
    static class Settings
    {
        public const int DefaultStrength = 92, DefaultTrailMs = 30, DefaultHideSpeedCm = 40, DefaultQuality = 1;
        public const uint DefaultHotkeyMods = 0x1 | 0x2, DefaultHotkeyKey = 0x42;   // Alt + Ctrl + B (Windows' MOD_ALT, MOD_CONTROL; 0x42 = B)

        // read from the overlay's worker threads, so keep them simple fields
        static volatile bool enabled = true;
        public static bool Enabled { get { return enabled; } set { enabled = value; EnabledChanged(); } }
        public static Action EnabledChanged = delegate { };   // set by the overlay: it stops listening to the mouse while off
        public static volatile int Strength = DefaultStrength;        // 1-100, peak opacity of the blur in %
        public static volatile int TrailMs = DefaultTrailMs;          // how far back in time the blur reaches
        public static volatile bool HideWhenFast = true;              // hide the real cursor while moving very fast
        public static volatile int HideSpeedCm = DefaultHideSpeedCm;  // on-screen cm/s above which the real cursor is hidden
        public static volatile bool PauseInFullscreen = true;         // switch the blur off while a fullscreen app or game is running
        public static volatile int Quality = DefaultQuality;          // 0 fast, 1 balanced, 2 smooth: how many cursor copies a frame may use
        public static volatile uint HotkeyMods = DefaultHotkeyMods;   // the global on/off shortcut
        public static volatile uint HotkeyKey = DefaultHotkeyKey;

        /// <summary>Most cursor copies drawn per frame, from the Quality setting.</summary>
        public static int MaxCopies { get { int q = Quality; return q <= 0 ? 35 : q == 1 ? 70 : 100; } }

        /// <summary>Set by the overlay: (re)registers the shortcut with Windows. False if another app already owns it.</summary>
        public static Func<bool> ApplyHotkey = delegate { return true; };

        public static string HotkeyText(uint mods, uint key)
        {
            return ((mods & 0x2) != 0 ? "Ctrl+" : "") + ((mods & 0x1) != 0 ? "Alt+" : "") + ((mods & 0x4) != 0 ? "Shift+" : "") + ((Keys)key).ToString();
        }

        const string RunKey = @"Software\Microsoft\Windows\CurrentVersion\Run";
        const string RunValue = "CursorMotionBlur";

        static string FilePath
        {
            get { return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "CursorMotionBlur", "settings.ini"); }
        }

        public static bool FileExists { get { return File.Exists(FilePath); } }

        public static bool StartWithWindows
        {
            get
            {
                using (var k = Registry.CurrentUser.OpenSubKey(RunKey))
                    return k != null && k.GetValue(RunValue) != null;
            }
            set
            {
                using (var k = Registry.CurrentUser.OpenSubKey(RunKey, true))
                {
                    if (k == null) return;
                    if (value) k.SetValue(RunValue, "\"" + Application.ExecutablePath + "\"");
                    else k.DeleteValue(RunValue, false);
                }
            }
        }

        public static void ResetDefaults()
        {
            Strength = DefaultStrength;
            TrailMs = DefaultTrailMs;
            HideWhenFast = true;
            HideSpeedCm = DefaultHideSpeedCm;
            PauseInFullscreen = true;
            Quality = DefaultQuality;
            HotkeyMods = DefaultHotkeyMods; HotkeyKey = DefaultHotkeyKey; ApplyHotkey();
            Save();
        }

        public static void Load()
        {
            try
            {
                if (!File.Exists(FilePath)) return;
                var kv = new Dictionary<string, string>();
                foreach (var line in File.ReadAllLines(FilePath))
                {
                    int i = line.IndexOf('=');
                    if (i > 0) kv[line.Substring(0, i).Trim()] = line.Substring(i + 1).Trim();
                }
                Enabled = GetBool(kv, "Enabled", true);
                Strength = Clamp(GetInt(kv, "Strength", DefaultStrength), 1, 100);
                TrailMs = Clamp(GetInt(kv, "TrailMs", DefaultTrailMs), 10, 150);
                HideWhenFast = GetBool(kv, "HideWhenFast", true);
                HideSpeedCm = Clamp(GetInt(kv, "HideSpeedCm", DefaultHideSpeedCm), 20, 400);
                PauseInFullscreen = GetBool(kv, "PauseInFullscreen", true);
                Quality = Clamp(GetInt(kv, "Quality", DefaultQuality), 0, 2);
                int mods = GetInt(kv, "HotkeyMods", (int)DefaultHotkeyMods), key = GetInt(kv, "HotkeyKey", (int)DefaultHotkeyKey);
                if ((mods & 0x7) != 0 && key > 0) { HotkeyMods = (uint)(mods & 0x7); HotkeyKey = (uint)key; }
            }
            catch { /* corrupt file: keep defaults */ }
        }

        public static void Save()
        {
            try
            {
                Directory.CreateDirectory(Path.GetDirectoryName(FilePath));
                File.WriteAllLines(FilePath, new[]
                {
                    "Enabled=" + Enabled,
                    "Strength=" + Strength,
                    "TrailMs=" + TrailMs,
                    "HideWhenFast=" + HideWhenFast,
                    "HideSpeedCm=" + HideSpeedCm,
                    "PauseInFullscreen=" + PauseInFullscreen,
                    "Quality=" + Quality,
                    "HotkeyMods=" + HotkeyMods,
                    "HotkeyKey=" + HotkeyKey
                });
            }
            catch { }
        }

        static int Clamp(int v, int lo, int hi) { return Math.Max(lo, Math.Min(hi, v)); }

        static int GetInt(Dictionary<string, string> kv, string key, int def)
        {
            string s; int v;
            return kv.TryGetValue(key, out s) && int.TryParse(s, NumberStyles.Integer, CultureInfo.InvariantCulture, out v) ? v : def;
        }

        static bool GetBool(Dictionary<string, string> kv, string key, bool def)
        {
            string s; bool v;
            return kv.TryGetValue(key, out s) && bool.TryParse(s, out v) ? v : def;
        }
    }
}
