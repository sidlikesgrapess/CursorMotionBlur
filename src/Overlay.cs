using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Threading;
using System.Windows.Forms;

namespace CursorMotionBlur
{
    /// <summary>
    /// Click-through, always-on-top layered window that draws fading copies of the cursor along its
    /// recent path. A sampler thread records the cursor path (~1 kHz); a render thread redraws on every
    /// new mouse position; a third thread swaps the real cursor for a blank one at very high speed.
    /// </summary>
    sealed class Overlay : Form
    {
        const int MAX_COPIES = 70;        // dense, faint copies read as a smooth blur rather than separate ghosts
        const double COVER_PX = 10;       // ~how many px of travel one cursor copy "covers" (used to keep the total opacity independent of copy density)
        const int HOLD_MS = 10;           // how long after the last fast moment the cursor may come back (the speed band between hide and show speed already prevents flicker)
        const int SHOW_WINDOW_MS = 12;    // the speed that brings the cursor back is measured over just this many ms
        const int HOTKEY_ID = 1;
        const int MAX_CACHED = 8;         // cursor pictures kept (shapes x monitor sizes) before the caches are emptied

        [StructLayout(LayoutKind.Sequential)] struct POINT { public int x, y; }
        [StructLayout(LayoutKind.Sequential)] struct SIZE { public int cx, cy; }
        [StructLayout(LayoutKind.Sequential)] struct BLEND { public byte Op, Flags, Alpha, Format; }
        [StructLayout(LayoutKind.Sequential)] struct CURSORINFO { public int cbSize, flags; public IntPtr hCursor; public POINT pt; }
        [StructLayout(LayoutKind.Sequential)] struct ICONINFO { public bool fIcon; public int xHot, yHot; public IntPtr hbmMask, hbmColor; }

        [DllImport("user32.dll")] static extern bool GetCursorInfo(ref CURSORINFO ci);
        [DllImport("user32.dll")] static extern bool GetIconInfo(IntPtr h, out ICONINFO i);
        [DllImport("user32.dll")] static extern IntPtr MonitorFromPoint(POINT pt, int flags);
        [DllImport("user32.dll")] static extern IntPtr GetDC(IntPtr h);
        [DllImport("user32.dll")] static extern int ReleaseDC(IntPtr h, IntPtr dc);
        [DllImport("user32.dll")] static extern bool UpdateLayeredWindow(IntPtr h, IntPtr dst, ref POINT pd, ref SIZE sz, IntPtr src, ref POINT ps, int key, ref BLEND b, int flags);
        [DllImport("user32.dll")] static extern bool ShowWindow(IntPtr h, int cmd);
        [DllImport("user32.dll")] static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
        [DllImport("user32.dll")] static extern int GetSystemMetricsForDpi(int idx, int dpi);
        [DllImport("user32.dll")] static extern IntPtr CreateCursor(IntPtr inst, int xHot, int yHot, int w, int h, byte[] andMask, byte[] xorMask);
        [DllImport("user32.dll")] static extern bool SetSystemCursor(IntPtr cur, uint id);
        [DllImport("user32.dll")] static extern bool SystemParametersInfo(uint action, uint param, IntPtr pv, uint winIni);
        [DllImport("user32.dll")] static extern bool RegisterHotKey(IntPtr h, int id, uint mods, uint vk);
        [DllImport("user32.dll")] static extern bool UnregisterHotKey(IntPtr h, int id);
        [DllImport("user32.dll")] static extern bool RegisterRawInputDevices(RAWINPUTDEVICE[] devices, uint count, uint size);

        [StructLayout(LayoutKind.Sequential)]
        struct RAWINPUTDEVICE { public ushort usagePage, usage; public uint flags; public IntPtr target; }
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern bool GetMonitorInfo(IntPtr mon, ref MONITORINFOEX mi);
        [DllImport("gdi32.dll", CharSet = CharSet.Unicode)] static extern IntPtr CreateDC(string driver, string device, string port, IntPtr devMode);
        [DllImport("gdi32.dll")] static extern int GetDeviceCaps(IntPtr dc, int index);

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        struct MONITORINFOEX
        {
            public int cbSize;
            public int l1, t1, r1, b1, l2, t2, r2, b2;
            public int flags;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string szDevice;
        }

        [DllImport("shcore.dll")] static extern int GetDpiForMonitor(IntPtr mon, int type, out uint dx, out uint dy);
        [DllImport("gdi32.dll")] static extern IntPtr CreateCompatibleDC(IntPtr dc);
        [DllImport("gdi32.dll")] static extern IntPtr SelectObject(IntPtr dc, IntPtr o);
        [DllImport("gdi32.dll")] static extern bool DeleteObject(IntPtr o);
        [DllImport("gdi32.dll")] static extern bool DeleteDC(IntPtr dc);
        [DllImport("winmm.dll")] static extern uint timeBeginPeriod(uint ms);
        [DllImport("winmm.dll")] static extern uint timeEndPeriod(uint ms);
        [DllImport("gdi32.dll")] static extern IntPtr CreateDIBSection(IntPtr dc, ref BITMAPINFOHEADER bi, uint usage, out IntPtr bits, IntPtr section, uint offset);

        [StructLayout(LayoutKind.Sequential)]
        struct BITMAPINFOHEADER
        {
            public int biSize, biWidth, biHeight;
            public short biPlanes, biBitCount;
            public int biCompression, biSizeImage, biXPelsPerMeter, biYPelsPerMeter, biClrUsed, biClrImportant;
        }

        // static system cursors blanked while the mouse is fast (animated wait/appstarting are left alone)
        static readonly uint[] BLANK_IDS = { 32512, 32513, 32515, 32516, 32642, 32643, 32644, 32645, 32646, 32648, 32649, 32651, 32671, 32672 };

        // lv[i] = the sprite pre-faded to i/255 opacity, built on first use, so drawing a copy is a plain blit
        class Sprite
        {
            public Bitmap bmp; public int hx, hy; public Bitmap[] lv = new Bitmap[256];
            public void Dispose()   // GDI+ bitmaps hold native memory that the garbage collector does not see, so free it explicitly
            {
                bmp.Dispose();
                foreach (var l in lv) if (l != null) l.Dispose();
            }
        }
        struct Sample { public int x, y; public long t; }

        /// <summary>Raised when the global toggle hotkey (Ctrl+Alt+B) is pressed.</summary>
        public event Action HotkeyPressed;

        // shared between sampler and render threads (guarded by gate)
        readonly object gate = new object();
        readonly List<Sample> hist = new List<Sample>();
        IntPtr curHandle;
        bool curVisible;
        int monCursor = 32;
        double monPxPerCm = 38;
        bool hideWanted;
        long lastFast;
        volatile bool blankActive; // system cursors currently replaced by a blank one

        readonly AutoResetEvent hideChanged = new AutoResetEvent(false);
        long lastMove;
        bool timerHigh;

        // render thread only: one reusable canvas (a DIB section wrapped by a Bitmap) instead of a bitmap per frame
        IntPtr screenDc, memDc, dib, dibOld;
        Bitmap canvas;
        Graphics canvasG;
        int canvasW, canvasH;
        readonly Dictionary<IntPtr, Sprite> sprites = new Dictionary<IntPtr, Sprite>();
        readonly Dictionary<long, Sprite> scaled = new Dictionary<long, Sprite>();
        readonly ImageAttributes[] attrs = new ImageAttributes[256];
        bool shown;
        long lastDraw;
        Sprite lastSprite;
        IntPtr lastGoodHandle;   // cursor handle that lastSprite was built from

        readonly Stopwatch sw = Stopwatch.StartNew();
        IntPtr hwnd;
        volatile bool running = true;
        readonly AutoResetEvent moved = new AutoResetEvent(false);
        readonly AutoResetEvent mouseWake = new AutoResetEvent(false);   // set by Windows' raw mouse input: wakes the sampler from idle
        bool rawInputOk;
        IntPtr lastMon;

        protected override bool ShowWithoutActivation { get { return true; } }
        protected override CreateParams CreateParams
        {
            get
            {
                var cp = base.CreateParams;
                cp.ExStyle |= 0x80000 | 0x20 | 0x80 | 0x08000000 | 0x8; // layered, transparent, toolwindow, noactivate, topmost
                return cp;
            }
        }

        public Overlay()
        {
            FormBorderStyle = FormBorderStyle.None;
            ShowInTaskbar = false;
            StartPosition = FormStartPosition.Manual;
            Location = new Point(-32000, -32000);
            Size = new Size(1, 1);

            for (int i = 0; i <= 255; i++)
            {
                var m = new ColorMatrix(); m.Matrix33 = i / 255f;
                var a = new ImageAttributes(); a.SetColorMatrix(m); attrs[i] = a;
            }
        }

        protected override void OnHandleCreated(EventArgs e)
        {
            base.OnHandleCreated(e);
            hwnd = Handle;
            RegisterHotKey(hwnd, HOTKEY_ID, 0x1 | 0x2 | 0x4000, 0x42); // Ctrl+Alt+B, no auto-repeat
            // Ask Windows to tell this window about every mouse report (even while another app is active), so that the idle
            // sampler can sleep until the mouse really moves instead of checking every few ms.
            var mouse = new[] { new RAWINPUTDEVICE { usagePage = 1, usage = 2, flags = 0x100, target = hwnd } };   // generic desktop / mouse, RIDEV_INPUTSINK
            rawInputOk = RegisterRawInputDevices(mouse, 1, (uint)Marshal.SizeOf(typeof(RAWINPUTDEVICE)));
        }

        protected override void OnShown(EventArgs e)
        {
            base.OnShown(e);
            ShowWindow(hwnd, 0);
            // grab the current cursor's picture now, before any fast movement can hide it
            var ci = new CURSORINFO { cbSize = Marshal.SizeOf(typeof(CURSORINFO)) };
            if (GetCursorInfo(ref ci) && (ci.flags & 1) != 0) { lastSprite = GetSprite(ci.hCursor); lastGoodHandle = ci.hCursor; }
            new Thread(SampleLoop) { IsBackground = true, Priority = ThreadPriority.AboveNormal }.Start();
            new Thread(RenderLoop) { IsBackground = true, Priority = ThreadPriority.AboveNormal }.Start();
            new Thread(HideLoop) { IsBackground = true }.Start();
        }

        protected override void WndProc(ref Message m)
        {
            if (m.Msg == 0x312 && m.WParam.ToInt32() == HOTKEY_ID && HotkeyPressed != null) HotkeyPressed();
            if (m.Msg == 0x00FF) mouseWake.Set();   // WM_INPUT: the mouse reported something
            base.WndProc(ref m);
        }

        /// <summary>Stops the worker threads and puts the user's real cursors back.</summary>
        public void Shutdown()
        {
            running = false;
            if (hwnd != IntPtr.Zero) UnregisterHotKey(hwnd, HOTKEY_ID);
            RestoreCursors();
        }

        /// <summary>Reload the user's cursor scheme (used at startup in case an earlier run died while hiding it).</summary>
        public static void ReloadCursorScheme()
        {
            SystemParametersInfo(0x57, 0, IntPtr.Zero, 0); // SPI_SETCURSORS
        }

        // ~1 kHz cursor path recording and speed measurement
        void SampleLoop()
        {
            while (running)
            {
                var ci = new CURSORINFO { cbSize = Marshal.SizeOf(typeof(CURSORINFO)) };
                lock (gate)
                {
                    bool ok = GetCursorInfo(ref ci) && (ci.flags & 1) != 0;
                    if (!Settings.Enabled || !ok)
                    {
                        curVisible = false; hist.Clear();
                        if (hideWanted) { hideWanted = false; hideChanged.Set(); }
                    }
                    else
                    {
                        curVisible = true;
                        if (!blankActive) curHandle = ci.hCursor; // keep the real cursor image while it's hidden

                        // crossing to another monitor: drop the trail so it doesn't smear across the gap
                        int cnt = hist.Count;
                        bool samePos = cnt > 0 && hist[cnt - 1].x == ci.pt.x && hist[cnt - 1].y == ci.pt.y;
                        IntPtr mon = samePos ? lastMon : MonitorFromPoint(ci.pt, 2);
                        if (mon != lastMon)
                        {
                            hist.Clear(); lastMon = mon;
                            uint dx = 96, dy = 96;
                            try { GetDpiForMonitor(mon, 0, out dx, out dy); } catch { }
                            monCursor = GetSystemMetricsForDpi(13, (int)dx);
                            monPxPerCm = PixelsPerCm(mon, dx);
                        }

                        long now = sw.ElapsedMilliseconds;
                        int n = hist.Count;
                        bool changed = n == 0 || hist[n - 1].x != ci.pt.x || hist[n - 1].y != ci.pt.y;
                        if (changed || now - hist[n - 1].t >= 4)
                            hist.Add(new Sample { x = ci.pt.x, y = ci.pt.y, t = now });
                        if (changed) { lastMove = now; moved.Set(); } // new mouse position -> draw now
                        int trail = Settings.TrailMs;
                        while (hist.Count > 1 && now - hist[0].t > trail) hist.RemoveAt(0);

                        // speed over the trail window -> hide the real cursor when very fast
                        long dt = hist[hist.Count - 1].t - hist[0].t;
                        bool wasHidden = hideWanted;
                        if (!Settings.HideWhenFast) hideWanted = false;
                        else if (dt >= 8)
                        {
                            double path = 0;
                            for (int i = 1; i < hist.Count; i++)
                            {
                                double ddx = hist[i].x - hist[i - 1].x, ddy = hist[i].y - hist[i - 1].y;
                                path += Math.Sqrt(ddx * ddx + ddy * ddy);
                            }
                            double speed = path * 1000.0 / dt;
                            double hide = Settings.HideSpeedCm * monPxPerCm; // px/s on the monitor the cursor is on
                            // Hiding looks at the whole trail window (steady). Bringing the cursor back looks at only the last few ms,
                            // so it returns as soon as the mouse stops or slows, not 30 ms later when the fast part has left the window.
                            int j = hist.Count - 1;
                            while (j > 0 && now - hist[j - 1].t <= SHOW_WINDOW_MS) j--;
                            double recentPath = 0;
                            for (int i = j + 1; i < hist.Count; i++)
                            {
                                double ddx = hist[i].x - hist[i - 1].x, ddy = hist[i].y - hist[i - 1].y;
                                recentPath += Math.Sqrt(ddx * ddx + ddy * ddy);
                            }
                            long recentDt = now - hist[j].t;
                            double recent = recentDt >= 4 ? recentPath * 1000.0 / recentDt : speed;
                            if (speed > hide) { hideWanted = true; lastFast = now; }
                            else if (hideWanted && recent < hide * 0.5 && now - lastFast > HOLD_MS) hideWanted = false;
                        }
                        if (hideWanted != wasHidden) hideChanged.Set();
                    }
                }

                // poll fast while the mouse is moving; back off (and drop the 1 ms system timer) when idle
                long idleFor = sw.ElapsedMilliseconds - lastMove;
                if (idleFor < Settings.TrailMs + 40 || hideWanted)   // never go to sleep while the real cursor is hidden: it has to be given back
                {
                    if (!timerHigh) { timeBeginPeriod(1); timerHigh = true; }
                    Thread.Sleep(2); // the mouse reports at ~125 Hz, so 500 Hz sampling is plenty
                }
                else
                {
                    if (timerHigh) { timeEndPeriod(1); timerHigh = false; }
                    if (rawInputOk)
                    {
                        // sleep until the mouse reports something (the timeout only lets the loop notice that the app is closing)
                        if (mouseWake.WaitOne(500)) lastMove = sw.ElapsedMilliseconds;
                    }
                    else Thread.Sleep(10);                  // no raw input available: fall back to checking every few ms
                }
            }
        }

        // Set the environment variable CMB_DEBUG=1 to get %TEMP%\CursorMotionBlur.log (errors that are otherwise swallowed).
        static readonly bool debug = Environment.GetEnvironmentVariable("CMB_DEBUG") == "1";
        static void Log(string msg)
        {
            if (!debug) return;
            try { System.IO.File.AppendAllText(System.IO.Path.Combine(System.IO.Path.GetTempPath(), "CursorMotionBlur.log"), DateTime.Now.ToString("HH:mm:ss.fff ") + msg + "\r\n"); } catch { }
        }

        // Physical pixel density of a monitor, from the size Windows reports for it (EDID). If that looks wrong
        // (some monitors/drivers report nothing), fall back to its DPI setting.
        static double PixelsPerCm(IntPtr mon, uint dpi)
        {
            double fallback = Math.Max(20.0, dpi / 2.54);
            try
            {
                var mi = new MONITORINFOEX { cbSize = Marshal.SizeOf(typeof(MONITORINFOEX)) };
                if (!GetMonitorInfo(mon, ref mi)) return fallback;
                IntPtr dc = CreateDC("DISPLAY", mi.szDevice, null, IntPtr.Zero);
                if (dc == IntPtr.Zero) return fallback;
                int mm = GetDeviceCaps(dc, 4), px = GetDeviceCaps(dc, 8); // HORZSIZE (mm), HORZRES (px)
                DeleteDC(dc);
                if (mm < 150 || mm > 2500 || px < 320) return fallback;
                return px / (mm / 10.0);
            }
            catch { return fallback; }
        }

        // swaps the system cursors for a blank one / restores them; kept off the sampler thread because restoring is slow
        void HideLoop()
        {
            while (running)
            {
                bool want; lock (gate) want = hideWanted;
                if (want && !blankActive)
                {
                    blankActive = true;
                    foreach (uint id in BLANK_IDS)
                    {
                        var a = new byte[128]; for (int k = 0; k < a.Length; k++) a[k] = 0xFF;
                        SetSystemCursor(CreateCursor(IntPtr.Zero, 0, 0, 32, 32, a, new byte[128]), id);
                    }
                }
                else if (!want && blankActive)
                {
                    ReloadCursorScheme();
                    blankActive = false;
                }
                hideChanged.WaitOne(250);
            }
        }

        void RestoreCursors()
        {
            if (!blankActive) return;
            ReloadCursorScheme();
            blankActive = false;
        }

        // draw on every new mouse position (the mouse polling rate); the 4 ms timeout keeps the fade going after it stops
        void RenderLoop()
        {
            while (running)
            {
                // Draw on every new mouse position. The timer (one 120 Hz frame) only carries the fade after the mouse stops: Windows
                // shows one update per screen refresh, so drawing more often than that is wasted work.
                bool fresh = moved.WaitOne(shown ? 8 : 250);
                long sinceDraw = sw.ElapsedMilliseconds - lastDraw;
                if (!fresh && !shown)
                {
                    if (canvas != null && sinceDraw > 1500) ReleaseCanvas();   // give the memory back while idle
                    continue;
                }
                if (fresh ? sinceDraw < 3 : sinceDraw < 6) continue;   // just drawn (a very fast mouse reports faster than anyone can see)
                try { Render(); } catch (Exception ex) { Log("Render: " + ex); }
            }
        }

        // Cursor image scaled to what the system draws on a monitor with this DPI.
        Sprite GetScaledSprite(IntPtr h, int size)
        {
            var bs = GetSprite(h);
            if (bs == null || size <= 0 || size == bs.bmp.Width) return bs;
            long key = h.ToInt64() * 1000 + size;
            Sprite sp;
            if (scaled.TryGetValue(key, out sp)) return sp;
            float f = size / (float)bs.bmp.Width;
            int nw = (int)Math.Round(bs.bmp.Width * f), nh = (int)Math.Round(bs.bmp.Height * f);
            var b = new Bitmap(nw, nh, PixelFormat.Format32bppPArgb);
            using (var g = Graphics.FromImage(b))
            {
                g.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.HighQualityBicubic;
                g.PixelOffsetMode = System.Drawing.Drawing2D.PixelOffsetMode.HighQuality;
                g.DrawImage(bs.bmp, new Rectangle(0, 0, nw, nh), 0, 0, bs.bmp.Width, bs.bmp.Height, GraphicsUnit.Pixel);
            }
            sp = new Sprite { bmp = b, hx = (int)Math.Round(bs.hx * f), hy = (int)Math.Round(bs.hy * f) };
            scaled[key] = sp;
            Log("scaled sprite for handle " + h + " to " + nw + "x" + nh + (blankActive ? " (cursor hidden)" : ""));
            return sp;
        }

        Sprite GetSprite(IntPtr h)
        {
            Sprite sp;
            if (sprites.TryGetValue(h, out sp)) return sp;
            sp = null;
            try
            {
                ICONINFO ii; GetIconInfo(h, out ii);
                using (var ic = Icon.FromHandle(h))
                {
                    Bitmap b;
                    using (var src = ic.ToBitmap())
                    {
                        b = new Bitmap(src.Width, src.Height, PixelFormat.Format32bppPArgb);
                        using (var g = Graphics.FromImage(b)) g.DrawImage(src, 0, 0);
                    }
                    if (IsBlank(b)) { b.Dispose(); b = null; } // never keep an empty picture (the cursor may be swapped for the invisible one)
                    if (b != null) sp = new Sprite { bmp = b, hx = ii.xHot, hy = ii.yHot };
                    if (sp != null) Log("sprite for handle " + h + ": " + b.Width + "x" + b.Height + " hotspot " + ii.xHot + "," + ii.yHot);
                }
                if (ii.hbmMask != IntPtr.Zero) DeleteObject(ii.hbmMask);
                if (ii.hbmColor != IntPtr.Zero) DeleteObject(ii.hbmColor);
            }
            catch (Exception ex) { Log("GetSprite: " + ex); }
            // Only cache a picture taken while the real cursor is showing. Blanking replaces the cursor's content
            // under the same handle, so a picture grabbed during/after the swap could be the blank one.
            if (sp != null && blankActive) { sp.Dispose(); sp = null; }
            if (sp != null) sprites[h] = sp;
            return sp;
        }

        // Keep the picture caches small: past a handful of entries (cursor shapes x monitor sizes) release them all, with their
        // native memory, and rebuild what is needed. Not while the real cursor is hidden: then nothing can be rebuilt.
        void ForgetOldSprites()
        {
            if (blankActive || (sprites.Count <= MAX_CACHED && scaled.Count <= MAX_CACHED)) return;
            foreach (var s in sprites.Values) if (s != null) s.Dispose();
            foreach (var s in scaled.Values) s.Dispose();
            sprites.Clear(); scaled.Clear();
            lastSprite = null; lastGoodHandle = IntPtr.Zero;   // they pointed into the caches
        }

        static bool IsBlank(Bitmap b)
        {
            for (int y = 0; y < b.Height; y++)
                for (int x = 0; x < b.Width; x++)
                    if (b.GetPixel(x, y).A != 0) return false;
            return true;
        }

        void HideOverlay()
        {
            if (!shown) return;
            shown = false;
            ShowWindow(hwnd, 0);
        }

        void Render()
        {
            Sample[] pts;
            IntPtr handle; int size; bool visible;
            lock (gate)
            {
                visible = curVisible; handle = curHandle; size = monCursor;
                pts = hist.ToArray();
            }
            if (!visible || pts.Length < 2) { HideOverlay(); return; }
            // While the real cursor is hidden, keep drawing with the picture taken before it was.
            ForgetOldSprites();
            bool blank = blankActive;
            Sprite sp;
            if (blank)
            {
                // still scale the last real picture for the monitor the cursor is on now (the cursor may have changed monitors)
                sp = lastGoodHandle != IntPtr.Zero ? GetScaledSprite(lastGoodHandle, size) : null;
                if (sp == null) sp = lastSprite;
            }
            else
            {
                sp = GetScaledSprite(handle, size);
                if (sp != null) { lastSprite = sp; lastGoodHandle = handle; } else sp = lastSprite;
            }
            if (sp == null) { Log("no sprite for cursor handle " + handle); HideOverlay(); return; }

            long now = sw.ElapsedMilliseconds;
            var last = pts[pts.Length - 1];
            int trailMs = Settings.TrailMs;
            int strength = Settings.Strength;

            // stationary? nothing to blur
            bool anyMove = false;
            for (int i = 0; i < pts.Length; i++) if (pts[i].x != last.x || pts[i].y != last.y) { anyMove = true; break; }
            if (!anyMove) { HideOverlay(); return; }

            int minX = int.MaxValue, minY = int.MaxValue, maxX = int.MinValue, maxY = int.MinValue;
            foreach (var s in pts)
            {
                minX = Math.Min(minX, s.x - sp.hx); minY = Math.Min(minY, s.y - sp.hy);
                maxX = Math.Max(maxX, s.x - sp.hx + sp.bmp.Width); maxY = Math.Max(maxY, s.y - sp.hy + sp.bmp.Height);
            }
            int w = maxX - minX, h = maxY - minY;

            double total = 0;
            for (int i = 1; i < pts.Length; i++)
                total += Math.Sqrt(Math.Pow(pts[i].x - pts[i - 1].x, 2) + Math.Pow(pts[i].y - pts[i - 1].y, 2));
            double step = Math.Max(1.0, total / MAX_COPIES);
            double cover = COVER_PX * size / 32.0;
            double peak = strength / 100.0;

            EnsureCanvas(w, h);
            var g = canvasG;
            g.CompositingMode = System.Drawing.Drawing2D.CompositingMode.SourceCopy;
            g.FillRectangle(Brushes.Transparent, 0, 0, w, h);
            g.CompositingMode = System.Drawing.Drawing2D.CompositingMode.SourceOver;
            for (int i = 1; i < pts.Length; i++)
            {
                var a = pts[i - 1]; var b = pts[i];
                double ddx = b.x - a.x, ddy = b.y - a.y;
                double d = Math.Sqrt(ddx * ddx + ddy * ddy);
                if (d < 0.5) continue;
                int n = Math.Max(1, (int)(d / step));
                for (int k = 0; k < n; k++)
                {
                    double f = (double)k / n;
                    double x = a.x + ddx * f, y = a.y + ddy * f;
                    double t = a.t + (b.t - a.t) * f;
                    double life = 1.0 - (now - t) / trailMs;
                    if (life <= 0) continue;
                    life *= life; // steeper fade
                    // copies overlap by about cover/step, so each one gets that share of the opacity the user asked for
                    int ai = (int)Math.Round(Math.Min(1.0, peak * life * step / cover) * 255);
                    if (ai <= 0) continue;
                    g.DrawImageUnscaled(Level(sp, ai), (int)Math.Round(x) - sp.hx - minX, (int)Math.Round(y) - sp.hy - minY);
                }
            }

            var pd = new POINT { x = minX, y = minY };
            var sz = new SIZE { cx = w, cy = h };
            var ps = new POINT();
            var bl = new BLEND { Op = 0, Flags = 0, Alpha = 255, Format = 1 };
            UpdateLayeredWindow(hwnd, screenDc, ref pd, ref sz, memDc, ref ps, 0, ref bl, 2);
            lastDraw = sw.ElapsedMilliseconds;
            if (!shown)
            {
                // HWND_TOPMOST, NOMOVE|NOSIZE|NOACTIVATE|SHOWWINDOW
                SetWindowPos(hwnd, new IntPtr(-1), 0, 0, 0, 0, 0x1 | 0x2 | 0x10 | 0x40);
                shown = true;
            }
        }

        Bitmap Level(Sprite sp, int a)
        {
            var b = sp.lv[a];
            if (b != null) return b;
            b = new Bitmap(sp.bmp.Width, sp.bmp.Height, PixelFormat.Format32bppPArgb);
            using (var g = Graphics.FromImage(b))
                g.DrawImage(sp.bmp, new Rectangle(0, 0, b.Width, b.Height), 0, 0, b.Width, b.Height, GraphicsUnit.Pixel, attrs[a]);
            sp.lv[a] = b;
            return b;
        }

        // The canvas only ever grows (in 128 px steps); frames draw into its top-left w x h corner.
        void ReleaseCanvas()
        {
            if (canvasG != null) { canvasG.Dispose(); canvasG = null; }
            if (canvas != null) { canvas.Dispose(); canvas = null; }
            if (dib != IntPtr.Zero) { SelectObject(memDc, dibOld); DeleteObject(dib); dib = IntPtr.Zero; }
            canvasW = canvasH = 0;
        }

        void EnsureCanvas(int w, int h)
        {
            if (canvas != null && w <= canvasW && h <= canvasH) return;
            int nw = Math.Max(canvasW, (w + 127) / 128 * 128), nh = Math.Max(canvasH, (h + 127) / 128 * 128);

            if (screenDc == IntPtr.Zero) { screenDc = GetDC(IntPtr.Zero); memDc = CreateCompatibleDC(screenDc); }
            ReleaseCanvas();

            var bi = new BITMAPINFOHEADER { biSize = Marshal.SizeOf(typeof(BITMAPINFOHEADER)), biWidth = nw, biHeight = -nh, biPlanes = 1, biBitCount = 32 };
            IntPtr bits;
            dib = CreateDIBSection(screenDc, ref bi, 0, out bits, IntPtr.Zero, 0);
            dibOld = SelectObject(memDc, dib);
            canvas = new Bitmap(nw, nh, nw * 4, PixelFormat.Format32bppPArgb, bits);
            canvasG = Graphics.FromImage(canvas);
            canvasG.CompositingQuality = System.Drawing.Drawing2D.CompositingQuality.HighSpeed;
            canvasG.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.NearestNeighbor;
            canvasG.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.None;
            canvasG.PixelOffsetMode = System.Drawing.Drawing2D.PixelOffsetMode.None;
            canvasW = nw; canvasH = nh;
        }
    }
}
