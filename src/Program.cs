using System;
using System.Drawing;
using System.Runtime.InteropServices;
using System.Threading;
using System.Windows.Forms;

namespace CursorMotionBlur
{
    /// <summary>Tray application: owns the overlay, the tray icon and the settings window.</summary>
    sealed class TrayApp : ApplicationContext
    {
        readonly Overlay overlay = new Overlay();
        readonly NotifyIcon tray = new NotifyIcon();
        readonly ToolStripMenuItem enabledItem;
        SettingsForm settingsForm;

        public TrayApp(bool firstRun)
        {
            overlay.Show(); // hidden layered window; needed to host the hotkey and the drawing
            overlay.HotkeyPressed += delegate { Settings.Enabled = !Settings.Enabled; Settings.Save(); RefreshTray(); };

            enabledItem = new ToolStripMenuItem("Enabled", null, delegate { Settings.Enabled = !Settings.Enabled; Settings.Save(); RefreshTray(); });
            var menu = new ContextMenuStrip();
            menu.Items.Add(enabledItem);
            menu.Items.Add("Settings...", null, delegate { ShowSettings(); });
            menu.Items.Add(new ToolStripSeparator());
            menu.Items.Add("Exit", null, delegate { Quit(); });
            menu.Opening += delegate { enabledItem.Checked = Settings.Enabled; };

            try { tray.Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch { tray.Icon = SystemIcons.Application; }
            tray.ContextMenuStrip = menu;
            tray.DoubleClick += delegate { ShowSettings(); };
            tray.Visible = true;
            RefreshTray();

            AppDomain.CurrentDomain.ProcessExit += delegate { overlay.Shutdown(); };
            AppDomain.CurrentDomain.UnhandledException += delegate { overlay.Shutdown(); };

            if (firstRun) ShowSettings();
        }

        void RefreshTray()
        {
            enabledItem.Checked = Settings.Enabled;
            tray.Text = "CursorMotionBlur - " + (Settings.Enabled ? "on" : "off");
        }

        void ShowSettings()
        {
            if (settingsForm == null || settingsForm.IsDisposed) settingsForm = new SettingsForm();
            else settingsForm.LoadValues();
            settingsForm.Show();
            settingsForm.Activate();
        }

        void Quit()
        {
            overlay.Shutdown();
            tray.Visible = false;
            tray.Dispose();
            ExitThread();
        }
    }

    static class Program
    {
        [DllImport("user32.dll")] static extern bool SetProcessDPIAware();
        [DllImport("user32.dll")] static extern bool SetProcessDpiAwarenessContext(IntPtr ctx);

        [STAThread]
        static void Main()
        {
            bool created;
            using (new Mutex(true, "CursorMotionBlur.SingleInstance", out created))
            {
                if (!created) return; // already running (it lives in the tray)

                // per-monitor DPI awareness so coordinates are physical pixels on every monitor
                try { if (!SetProcessDpiAwarenessContext((IntPtr)(-4))) SetProcessDPIAware(); }
                catch { SetProcessDPIAware(); }

                bool firstRun = !Settings.FileExists;
                Settings.Load();
                if (firstRun) Settings.Save(); // so the settings window only auto-opens once
                Overlay.ReloadCursorScheme(); // undo a blank cursor left behind by a crashed earlier run

                Application.EnableVisualStyles();
                Application.SetCompatibleTextRenderingDefault(false);
                Application.Run(new TrayApp(firstRun));
            }
        }
    }
}
