using System;
using System.Drawing;
using System.Windows.Forms;

namespace CursorMotionBlur
{
    /// <summary>Settings window. Every change applies immediately, so you can tune it by moving the mouse.</summary>
    sealed class SettingsForm : Form
    {
        CheckBox chkEnabled, chkHide, chkStartup;
        TrackBar barStrength, barTrail, barSpeed;
        Label lblStrength, lblTrail, lblSpeed;
        bool loading;

        public SettingsForm()
        {
            Text = "CursorMotionBlur";
            FormBorderStyle = FormBorderStyle.FixedDialog;
            MaximizeBox = false;
            MinimizeBox = false;
            StartPosition = FormStartPosition.CenterScreen;
            Font = SystemFonts.MessageBoxFont;
            AutoScaleDimensions = new SizeF(96F, 96F);
            AutoScaleMode = AutoScaleMode.Dpi;
            AutoSize = true;
            AutoSizeMode = AutoSizeMode.GrowAndShrink;
            try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch { }

            // One auto-sized column; every row sizes itself, so nothing overlaps at any display scaling.
            var table = new TableLayoutPanel
            {
                ColumnCount = 1,
                AutoSize = true,
                AutoSizeMode = AutoSizeMode.GrowAndShrink,
                Padding = new Padding(16, 12, 16, 12),
                MinimumSize = new Size(440, 0),
                Dock = DockStyle.Fill
            };
            table.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            Controls.Add(table);

            chkEnabled = AddCheck(table, "Enable motion blur   (Ctrl+Alt+B)");
            barStrength = AddSlider(table, 1, 100, 10, out lblStrength);
            barTrail = AddSlider(table, 10, 150, 10, out lblTrail);
            chkHide = AddCheck(table, "Hide the real cursor when moving very fast");
            barSpeed = AddSlider(table, 20, 300, 20, out lblSpeed);
            chkStartup = AddCheck(table, "Launch CursorMotionBlur when Windows starts");

            var buttons = new FlowLayoutPanel
            {
                FlowDirection = FlowDirection.RightToLeft,
                AutoSize = true,
                AutoSizeMode = AutoSizeMode.GrowAndShrink,
                Dock = DockStyle.Fill,
                Margin = new Padding(0, 12, 0, 0)
            };
            var close = new Button { Text = "Close", AutoSize = true, MinimumSize = new Size(90, 30), Margin = new Padding(6, 0, 0, 0) };
            close.Click += delegate { Close(); };
            var reset = new Button { Text = "Reset to defaults", AutoSize = true, MinimumSize = new Size(130, 30), Margin = new Padding(0) };
            reset.Click += delegate { Settings.ResetDefaults(); LoadValues(); };
            buttons.Controls.Add(close);
            buttons.Controls.Add(reset);
            table.Controls.Add(buttons);
            AcceptButton = close;

            chkEnabled.CheckedChanged += delegate { if (!loading) { Settings.Enabled = chkEnabled.Checked; Settings.Save(); } };
            chkHide.CheckedChanged += delegate { if (!loading) { Settings.HideWhenFast = chkHide.Checked; barSpeed.Enabled = chkHide.Checked; Settings.Save(); } };
            chkStartup.CheckedChanged += delegate { if (!loading) Settings.StartWithWindows = chkStartup.Checked; };
            barStrength.ValueChanged += delegate { if (!loading) { Settings.Strength = barStrength.Value; Settings.Save(); } UpdateLabels(); };
            barTrail.ValueChanged += delegate { if (!loading) { Settings.TrailMs = barTrail.Value; Settings.Save(); } UpdateLabels(); };
            barSpeed.ValueChanged += delegate { if (!loading) { Settings.HideSpeedCm = barSpeed.Value; Settings.Save(); } UpdateLabels(); };

            LoadValues();
        }

        public void LoadValues()
        {
            loading = true;
            chkEnabled.Checked = Settings.Enabled;
            chkHide.Checked = Settings.HideWhenFast;
            chkStartup.Checked = Settings.StartWithWindows;
            barStrength.Value = Settings.Strength;
            barTrail.Value = Settings.TrailMs;
            barSpeed.Value = Math.Max(barSpeed.Minimum, Math.Min(barSpeed.Maximum, Settings.HideSpeedCm));
            barSpeed.Enabled = chkHide.Checked;
            loading = false;
            UpdateLabels();
        }

        void UpdateLabels()
        {
            lblStrength.Text = "Blur strength: " + barStrength.Value + "%";
            lblTrail.Text = "Trail length: " + barTrail.Value + " ms";
            lblSpeed.Text = "Hide above speed: " + barSpeed.Value + " cm/s on screen";
        }

        static CheckBox AddCheck(TableLayoutPanel table, string text)
        {
            var c = new CheckBox { Text = text, AutoSize = true, Margin = new Padding(0, 6, 0, 6) };
            table.Controls.Add(c);
            return c;
        }

        static TrackBar AddSlider(TableLayoutPanel table, int min, int max, int tick, out Label label)
        {
            label = new Label { AutoSize = true, Margin = new Padding(0, 8, 0, 0) };
            var bar = new TrackBar
            {
                Minimum = min, Maximum = max, TickFrequency = tick, SmallChange = Math.Max(1, tick / 5), LargeChange = tick,
                Dock = DockStyle.Fill, AutoSize = true, Margin = new Padding(0, 0, 0, 4)
            };
            table.Controls.Add(label);
            table.Controls.Add(bar);
            return bar;
        }
    }
}
