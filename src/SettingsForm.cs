using System;
using System.Drawing;
using System.Runtime.InteropServices;
using System.Windows.Forms;

namespace CursorMotionBlur
{
    /// <summary>
    /// Settings window. Every change applies immediately, so you can tune it by moving the mouse.
    ///
    /// Display scaling is handled here, with no config file: the window is built from scratch for the DPI of the monitor it is
    /// on (all sizes are "design pixels at 96 DPI" times a scale factor, the font is given in pixels, and nothing is left to
    /// WinForms' automatic scaling). When Windows reports a DPI change (the window was dragged to a monitor with different
    /// scaling) it is simply rebuilt at the new scale, so nothing keeps stale sizes and no label gets clipped.
    /// </summary>
    sealed class SettingsForm : Form
    {
        [StructLayout(LayoutKind.Sequential)] struct POINT { public int x, y; }
        [StructLayout(LayoutKind.Sequential)] struct RECT { public int left, top, right, bottom; }
        [DllImport("user32.dll")] static extern IntPtr MonitorFromPoint(POINT pt, int flags);
        [DllImport("shcore.dll")] static extern int GetDpiForMonitor(IntPtr mon, int type, out uint dx, out uint dy);

        [DllImport("user32.dll")] static extern bool AdjustWindowRectExForDpi(ref RECT r, int style, bool menu, int exStyle, uint dpi);
        [DllImport("user32.dll")] static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
        [DllImport("user32.dll")] static extern int GetWindowLong(IntPtr h, int index);
        [DllImport("user32.dll")] static extern uint GetDpiForWindow(IntPtr h);

        const int WM_DPICHANGED = 0x02E0;
        const int WM_ENTERSIZEMOVE = 0x0231;
        const int WM_EXITSIZEMOVE = 0x0232;
        bool inMoveLoop;          // the user is dragging the window right now (Windows runs its own modal loop for that)
        bool dpiChangedInMove;    // the DPI changed while dragging, so the size must be corrected once the drag ends

        int dpi = 96;
        float scale = 1f;
        bool loading;
        Font ownFont;

        TableLayoutPanel table;
        CheckBox chkEnabled, chkHide, chkStartup;
        TrackBar barStrength, barTrail, barSpeed;
        Label lblStrength, lblTrail, lblSpeed, lblUpdate;
        Button btnUpdate, btnHotkey;
        CheckBox chkPause;
        ComboBox cmbQuality;
        Label lblHelp;
        ToolTip tip;
        bool capturing;                // waiting for the new shortcut to be pressed
        static readonly string QualityHelp = string.Join(Environment.NewLine, new[]
        {
            "Quality sets the most cursor copies drawn along the trail in one frame.",
            "Higher = a smoother streak in very fast movement, but more CPU.",
            "",
            "Fast: up to 35 copies (lightest)",
            "Balanced: up to 70 (recommended)",
            "Smooth: up to 100",
            "",
            "At slow and normal speeds all three look the same."
        });
        string updateMsg, updateUrl;   // result of the last update check
        bool checking;

        public SettingsForm()
        {
            Text = AppInfo.Title;
            FormBorderStyle = FormBorderStyle.FixedDialog;
            MaximizeBox = false;
            MinimizeBox = false;
            ShowInTaskbar = true;
            AutoScaleMode = AutoScaleMode.None;           // we do the scaling ourselves
            KeyPreview = true;                            // lets the window see the keys pressed while a new shortcut is being picked
            AutoSize = false;                             // sized explicitly in FitToContent (see there)
            StartPosition = FormStartPosition.Manual;
            try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch { }

            // open centred on the monitor the mouse is on, built for that monitor's DPI
            var wa = Screen.FromPoint(Cursor.Position).WorkingArea;
            BuildUi(DpiAt(Cursor.Position.X, Cursor.Position.Y));
            Location = new Point(wa.Left + (wa.Width - Width) / 2, wa.Top + (wa.Height - Height) / 2);
        }

        static int DpiAt(int x, int y)
        {
            try
            {
                uint dx, dy;
                IntPtr mon = MonitorFromPoint(new POINT { x = x, y = y }, 2);
                if (GetDpiForMonitor(mon, 0, out dx, out dy) == 0 && dx >= 48) return (int)dx;
            }
            catch { }
            return 96;
        }

        int P(int designPixels) { return (int)Math.Round(designPixels * scale); }

        protected override void WndProc(ref Message m)
        {
            if (m.Msg == WM_ENTERSIZEMOVE) inMoveLoop = true;
            if (m.Msg == WM_EXITSIZEMOVE)
            {
                inMoveLoop = false;
                if (dpiChangedInMove)
                {
                    // During the drag Windows keeps applying its own size to the window, so fit it only now that the drag is over.
                    dpiChangedInMove = false;
                    base.WndProc(ref m);
                    FitToContent();
                    return;
                }
            }
            if (m.Msg == WM_DPICHANGED)
            {
                int newDpi = (int)((long)m.WParam & 0xFFFF);
                var r = (RECT)Marshal.PtrToStructure(m.LParam, typeof(RECT));
                if (newDpi != dpi)
                {
                    if (inMoveLoop) dpiChangedInMove = true;
                    Location = new Point(r.left, r.top);   // Windows' suggested position on the new monitor
                    // Rebuild only after this message has been fully handled: then the window already is "on" the new DPI and
                    // everything is measured exactly as when the window is opened directly on that monitor.
                    BeginInvoke(new Action(delegate
                    {
                        BuildUi(newDpi);
                        var settle = new Timer { Interval = 80 };   // and once more after the frame has settled
                        settle.Tick += delegate { settle.Stop(); settle.Dispose(); table.PerformLayout(); FitToContent(); };
                        settle.Start();
                    }));
                }
                m.Result = IntPtr.Zero;
                return;
            }
            base.WndProc(ref m);
        }

        // (Re)creates every control for the given DPI.
        void BuildUi(int newDpi)
        {
            dpi = newDpi;
            scale = dpi / 96f;
            SuspendLayout();
            var oldFont = ownFont;   // only ever dispose a font this form created (the default font is shared by the whole app)
            while (Controls.Count > 0) { var c = Controls[0]; Controls.RemoveAt(0); c.Dispose(); }
            ownFont = new Font("Segoe UI", (float)Math.Round(12 * scale), FontStyle.Regular, GraphicsUnit.Pixel);   // 9 pt at 96 DPI
            Font = ownFont;
            Padding = new Padding(P(12));

            // One table, never nested. Every row spans both columns except the two buttons on the last row.
            table = new TableLayoutPanel { ColumnCount = 2, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, Dock = DockStyle.None, Location = new Point(Padding.Left, Padding.Top) };
            // Two fixed, equal columns (together as wide as a slider), so changing text in one row can never move the other rows.
            table.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, P(150)));
            table.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, P(150)));
            Controls.Add(table);

            chkEnabled = AddCheck("Enable motion blur");

            // shortcut: label left, button right (click it, then press the new keys)
            var lblHot = new Label { Text = "Toggle hotkey", AutoSize = true, Anchor = AnchorStyles.Left, Margin = new Padding(0, P(8), 0, 0) };
            btnHotkey = new Button { AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, Anchor = AnchorStyles.Right, Margin = new Padding(0, P(6), 0, 0), Padding = new Padding(P(6), P(2), P(6), P(2)) };
            btnHotkey.Click += delegate { capturing = true; btnHotkey.Text = "Press new keys..."; };
            btnHotkey.LostFocus += delegate { if (capturing) { capturing = false; ShowHotkey(); } };
            table.Controls.Add(lblHot);
            table.Controls.Add(btnHotkey);

            barStrength = AddSlider(1, 100, 10, out lblStrength);
            barTrail = AddSlider(10, 150, 10, out lblTrail);
            chkHide = AddCheck("Hide the real cursor when moving very fast");
            barSpeed = AddSlider(20, 300, 20, out lblSpeed);
            chkPause = AddCheck("Pause in fullscreen apps and games");

            // quality: "Quality ?" on the left (hover or click the ? for what it does), the choice on the right
            var qualityLabel = new FlowLayoutPanel { AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, WrapContents = false, FlowDirection = FlowDirection.LeftToRight, Anchor = AnchorStyles.Left, Margin = new Padding(0, P(8), 0, 0) };
            qualityLabel.Controls.Add(new Label { Text = "Quality", AutoSize = true, Margin = new Padding(0, 0, P(5), 0) });
            lblHelp = new Label { Text = "?", AutoSize = true, Cursor = Cursors.Hand, ForeColor = SystemColors.Highlight, Margin = new Padding(0) };
            if (tip != null) tip.Dispose();
            tip = new ToolTip { AutoPopDelay = 30000, InitialDelay = 150, ReshowDelay = 100 };
            tip.SetToolTip(lblHelp, QualityHelp);
            lblHelp.Click += delegate { tip.Show(QualityHelp, lblHelp, 0, lblHelp.Height + P(2), 15000); };
            qualityLabel.Controls.Add(lblHelp);
            cmbQuality = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Width = P(120), Anchor = AnchorStyles.Right, Margin = new Padding(0, P(6), 0, P(4)) };
            cmbQuality.Items.AddRange(new object[] { "Fast", "Balanced", "Smooth" });
            table.Controls.Add(qualityLabel);
            table.Controls.Add(cmbQuality);

            chkStartup = AddCheck("Launch CursorMotionBlur when Windows starts");

            // version + manual update check (one row: status on the left, button on the right)
            lblUpdate = new Label { AutoSize = false, AutoEllipsis = true, Anchor = AnchorStyles.Left, Margin = new Padding(0, P(12), P(6), 0), Size = new Size(P(150) - P(6), Font.Height + P(4)), TextAlign = ContentAlignment.MiddleLeft };
            btnUpdate = new Button { AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, Anchor = AnchorStyles.Right, Margin = new Padding(0, P(12), 0, 0), Padding = new Padding(P(6), P(2), P(6), P(2)) };
            btnUpdate.Click += delegate
            {
                if (updateUrl != null) { try { System.Diagnostics.Process.Start(updateUrl); } catch { } }
                else CheckForUpdates();
            };
            table.Controls.Add(lblUpdate);
            table.Controls.Add(btnUpdate);
            ShowUpdateState();

            var reset = new Button { Text = "Reset to defaults", AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, Anchor = AnchorStyles.Left, Margin = new Padding(0, P(12), 0, 0), Padding = new Padding(P(6), P(2), P(6), P(2)) };
            var close = new Button { Text = "Close", AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, Anchor = AnchorStyles.Right, Margin = new Padding(0, P(12), 0, 0), Padding = new Padding(P(10), P(2), P(10), P(2)) };
            reset.Click += delegate { Settings.ResetDefaults(); LoadValues(); };
            close.Click += delegate { Close(); };
            table.Controls.Add(reset);
            table.Controls.Add(close);
            AcceptButton = close;

            chkEnabled.CheckedChanged += delegate { if (!loading) { Settings.Enabled = chkEnabled.Checked; Settings.Save(); } };
            chkHide.CheckedChanged += delegate { if (!loading) { Settings.HideWhenFast = chkHide.Checked; barSpeed.Enabled = chkHide.Checked; Settings.Save(); } };
            chkPause.CheckedChanged += delegate { if (!loading) { Settings.PauseInFullscreen = chkPause.Checked; Settings.Save(); } };
            cmbQuality.SelectedIndexChanged += delegate { if (!loading) { Settings.Quality = cmbQuality.SelectedIndex; Settings.Save(); } };
            chkStartup.CheckedChanged += delegate { if (!loading) Settings.StartWithWindows = chkStartup.Checked; };
            barStrength.ValueChanged += delegate { if (!loading) { Settings.Strength = barStrength.Value; Settings.Save(); } UpdateLabels(); };
            barTrail.ValueChanged += delegate { if (!loading) { Settings.TrailMs = barTrail.Value; Settings.Save(); } UpdateLabels(); };
            barSpeed.ValueChanged += delegate { if (!loading) { Settings.HideSpeedCm = barSpeed.Value; Settings.Save(); } UpdateLabels(); };

            LoadValues();
            ResumeLayout(true);
            FitToContent();
            if (oldFont != null) oldFont.Dispose();
        }

        // Make the client area exactly as big as the content. WinForms (legacy mode) works out the size of the window frame (title
        // bar, borders) with the system DPI, which is wrong on a monitor with other scaling and clips the bottom row. So the outer
        // size is computed by Windows itself for the window's real DPI.
        void FitToContent()
        {
            var content = table.GetPreferredSize(Size.Empty);
            var want = new Size(content.Width + Padding.Horizontal, content.Height + Padding.Vertical);
            if (!IsHandleCreated) { ClientSize = want; return; }
            uint wdpi = GetDpiForWindow(Handle);
            var r = new RECT { left = 0, top = 0, right = want.Width, bottom = want.Height };
            AdjustWindowRectExForDpi(ref r, GetWindowLong(Handle, -16), false, GetWindowLong(Handle, -20), wdpi == 0 ? (uint)dpi : wdpi);
            SetWindowPos(Handle, IntPtr.Zero, 0, 0, r.right - r.left, r.bottom - r.top, 0x2 | 0x4 | 0x10);   // NOMOVE | NOZORDER | NOACTIVATE
        }

        protected override void OnHandleCreated(EventArgs e)
        {
            base.OnHandleCreated(e);
            FitToContent();   // the frame size is only known once the window exists
        }

        public void LoadValues()
        {
            loading = true;
            chkEnabled.Checked = Settings.Enabled;
            chkHide.Checked = Settings.HideWhenFast;
            chkStartup.Checked = Settings.StartWithWindows;
            chkPause.Checked = Settings.PauseInFullscreen;
            cmbQuality.SelectedIndex = Math.Max(0, Math.Min(2, Settings.Quality));
            ShowHotkey();
            barStrength.Value = Settings.Strength;
            barTrail.Value = Settings.TrailMs;
            barSpeed.Value = Math.Max(barSpeed.Minimum, Math.Min(barSpeed.Maximum, Settings.HideSpeedCm));
            barSpeed.Enabled = chkHide.Checked;
            loading = false;
            UpdateLabels();
        }

        void ShowHotkey()
        {
            btnHotkey.Text = Settings.HotkeyText(Settings.HotkeyMods, Settings.HotkeyKey);
        }

        // While a new shortcut is being picked, the next key combination becomes the shortcut (Esc cancels).
        protected override void OnKeyDown(KeyEventArgs e)
        {
            if (!capturing) { base.OnKeyDown(e); return; }
            e.Handled = true; e.SuppressKeyPress = true;
            Keys k = e.KeyCode;
            if (k == Keys.Escape) { capturing = false; ShowHotkey(); return; }
            if (k == Keys.ControlKey || k == Keys.Menu || k == Keys.ShiftKey || k == Keys.LWin || k == Keys.RWin) return;   // wait for the real key
            uint mods = (e.Control ? 0x2u : 0u) | (e.Alt ? 0x1u : 0u) | (e.Shift ? 0x4u : 0u);
            if (mods == 0) return;   // a shortcut without Ctrl, Alt or Shift would fire while typing in any program

            uint oldMods = Settings.HotkeyMods, oldKey = Settings.HotkeyKey;
            Settings.HotkeyMods = mods; Settings.HotkeyKey = (uint)k;
            capturing = false;
            if (!Settings.ApplyHotkey())
            {
                Settings.HotkeyMods = oldMods; Settings.HotkeyKey = oldKey; Settings.ApplyHotkey();   // keep the old one working
                ShowHotkey();
                MessageBox.Show(this, "That shortcut is already used by another program. Please pick a different one.", AppInfo.Title, MessageBoxButtons.OK, MessageBoxIcon.Information);
                return;
            }
            Settings.Save();
            ShowHotkey();
        }

        void CheckForUpdates()
        {
            checking = true;
            updateMsg = "Checking...";
            ShowUpdateState();
            UpdateCheck.Run(delegate(string message, string url)
            {
                try
                {
                    BeginInvoke(new Action(delegate
                    {
                        checking = false; updateMsg = message; updateUrl = url;
                        ShowUpdateState();
                    }));
                }
                catch { }   // the window was closed meanwhile
            });
        }

        // what the update row shows; kept in fields so it survives the window being rebuilt for another monitor's DPI
        void ShowUpdateState()
        {
            lblUpdate.Text = updateMsg ?? ("Version " + AppInfo.Version);
            btnUpdate.Text = updateUrl != null ? "Download" : "Check for updates";
            btnUpdate.Enabled = !checking;
        }

        void UpdateLabels()
        {
            lblStrength.Text = "Trail opacity: " + barStrength.Value + "%";
            lblTrail.Text = "Trail length: " + barTrail.Value + " ms";
            lblSpeed.Text = "Hide above speed: " + barSpeed.Value + " cm/s on screen";
        }

        CheckBox AddCheck(string text)
        {
            var c = new CheckBox { Text = text, AutoSize = true, Margin = new Padding(0, P(6), 0, P(2)) };
            table.Controls.Add(c);
            table.SetColumnSpan(c, 2);
            return c;
        }

        TrackBar AddSlider(int min, int max, int tick, out Label label)
        {
            label = new Label { AutoSize = true, Margin = new Padding(0, P(10), 0, 0) };
            var bar = new TrackBar
            {
                Minimum = min, Maximum = max, TickFrequency = tick, SmallChange = Math.Max(1, tick / 5), LargeChange = tick,
                AutoSize = false, Width = P(300), Height = P(32), Margin = new Padding(0)
            };
            table.Controls.Add(label);
            table.SetColumnSpan(label, 2);
            table.Controls.Add(bar);
            table.SetColumnSpan(bar, 2);
            return bar;
        }
    }
}
