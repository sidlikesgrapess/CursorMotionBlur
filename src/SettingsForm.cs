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
            AutoScaleMode = AutoScaleMode.Dpi;
            ClientSize = new Size(400, 390);
            try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch { }

            int y = 14;
            chkEnabled = AddCheck("Enable motion blur   (Ctrl+Alt+B toggles it anywhere)", ref y);
            y += 6;
            barStrength = AddSlider("Blur strength", 1, 100, 10, out lblStrength, ref y);
            barTrail = AddSlider("Trail length", 10, 150, 10, out lblTrail, ref y);
            y += 4;
            chkHide = AddCheck("Hide the real cursor when moving very fast", ref y);
            barSpeed = AddSlider("Hide above speed", 1000, 8000, 500, out lblSpeed, ref y);
            y += 4;
            chkStartup = AddCheck("Launch CursorMotionBlur when Windows starts", ref y);

            var reset = new Button { Text = "Reset to defaults", Location = new Point(16, y + 12), Size = new Size(140, 30) };
            reset.Click += delegate { Settings.ResetDefaults(); LoadValues(); };
            var close = new Button { Text = "Close", Location = new Point(ClientSize.Width - 106, y + 12), Size = new Size(90, 30) };
            close.Click += delegate { Close(); };
            Controls.Add(reset);
            Controls.Add(close);
            AcceptButton = close;
            ClientSize = new Size(ClientSize.Width, y + 58);

            chkEnabled.CheckedChanged += delegate { if (!loading) { Settings.Enabled = chkEnabled.Checked; Settings.Save(); } };
            chkHide.CheckedChanged += delegate { if (!loading) { Settings.HideWhenFast = chkHide.Checked; barSpeed.Enabled = chkHide.Checked; Settings.Save(); } };
            chkStartup.CheckedChanged += delegate { if (!loading) Settings.StartWithWindows = chkStartup.Checked; };
            barStrength.ValueChanged += delegate { if (!loading) { Settings.Strength = barStrength.Value; Settings.Save(); } UpdateLabels(); };
            barTrail.ValueChanged += delegate { if (!loading) { Settings.TrailMs = barTrail.Value; Settings.Save(); } UpdateLabels(); };
            barSpeed.ValueChanged += delegate { if (!loading) { Settings.HideSpeed = barSpeed.Value; Settings.Save(); } UpdateLabels(); };

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
            barSpeed.Value = Math.Max(barSpeed.Minimum, Math.Min(barSpeed.Maximum, Settings.HideSpeed));
            barSpeed.Enabled = chkHide.Checked;
            loading = false;
            UpdateLabels();
        }

        void UpdateLabels()
        {
            lblStrength.Text = "Blur strength: " + barStrength.Value + "%";
            lblTrail.Text = "Trail length: " + barTrail.Value + " ms";
            lblSpeed.Text = "Hide above speed: " + barSpeed.Value + " px/s";
        }

        CheckBox AddCheck(string text, ref int y)
        {
            var c = new CheckBox { Text = text, Location = new Point(16, y), AutoSize = true };
            Controls.Add(c);
            y += 28;
            return c;
        }

        TrackBar AddSlider(string text, int min, int max, int tick, out Label label, ref int y)
        {
            label = new Label { Text = text, Location = new Point(16, y), AutoSize = true };
            Controls.Add(label);
            var bar = new TrackBar
            {
                Minimum = min, Maximum = max, TickFrequency = tick, SmallChange = Math.Max(1, tick / 5), LargeChange = tick,
                Location = new Point(10, y + 18), Size = new Size(ClientSize.Width - 20, 42)
            };
            Controls.Add(bar);
            y += 64;
            return bar;
        }
    }
}
