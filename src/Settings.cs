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
        public const int DefaultStrength = 92, DefaultTrailMs = 30, DefaultHideSpeedCm = 40;

        // read from the overlay's worker threads, so keep them simple fields
        public static volatile bool Enabled = true;
        public static volatile int Strength = DefaultStrength;        // 1-100, peak opacity of the blur in %
        public static volatile int TrailMs = DefaultTrailMs;          // how far back in time the blur reaches
        public static volatile bool HideWhenFast = true;              // hide the real cursor while moving very fast
        public static volatile int HideSpeedCm = DefaultHideSpeedCm;  // on-screen cm/s above which the real cursor is hidden

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
                    "HideSpeedCm=" + HideSpeedCm
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
