// The engine: three threads.
//   sampler  records where the cursor is on every mouse report and decides when the real cursor should be hidden
//   hider    swaps the system cursors for an invisible one and back (restoring takes ~20 ms, so it runs on its own)
//   render   works out the fading copies along the recent path and hands them to the renderer; it owns the overlay window
#include "app.h"
#include <shellapi.h>
#include <shellscalingapi.h>
#include <mmsystem.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define HOLD_MS        10      // how long after the last fast moment the cursor may come back
#define SHOW_WINDOW_MS 12      // the speed that brings the cursor back is measured over just this many ms
#define COVER_PX       10.0    // ~how many px of travel one copy covers at cursor size 32 (keeps the opacity independent of copy density)
#define MAX_SAMPLES    512
#define MAX_SPRITES    16      // cursor pictures kept before the cache is emptied

typedef struct { int x, y; LONGLONG t; } Sample;

// shared between the threads (guarded by gate)
static CRITICAL_SECTION gate;
static Sample hist[MAX_SAMPLES];
static int histN;
static HCURSOR curHandle;
static BOOL curVisible, hideWanted;
static int monCursor = 32;           // cursor width on the monitor the cursor is on
static double mainPxPerCm = 38;      // pixel density of the main monitor: the hide speed is in cm/s there
static volatile int monGap = 7;      // ms between frames: one per refresh of the monitor the cursor is on
static volatile LONG blankActive;    // the system cursors are currently swapped for an invisible one
static volatile LONG drawFailed;     // the renderer could not draw: then never hide the real cursor
static volatile LONG monStale, spritesStale;   // display or cursor settings changed: measure the monitor / take the pictures again

static HANDLE moved, mouseWake, hideChanged;   // auto-reset events
static LARGE_INTEGER freq, start;

static LONGLONG Now(void)   // ms since start
{
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return (t.QuadPart - start.QuadPart) * 1000 / freq.QuadPart;
}

void Engine_Wake(void) { if (mouseWake) SetEvent(mouseWake); }
void Engine_DisplayChanged(void) { monStale = spritesStale = 1; }

static double Dist(Sample a, Sample b) { double dx = b.x - a.x, dy = b.y - a.y; return sqrt(dx * dx + dy * dy); }

// A monitor's physical pixel density, from the size Windows reports for it (its DPI setting if that looks wrong), and refresh rate.
static void MonitorMetrics(HMONITOR mon, UINT dpi, double *pxPerCm, int *hz)
{
    *pxPerCm = dpi / 2.54 < 20 ? 20 : dpi / 2.54;
    *hz = 60;
    MONITORINFOEXW mi; mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(mon, (MONITORINFO *)&mi)) return;
    DEVMODEW dm = { .dmSize = sizeof dm };
    if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) *hz = dm.dmDisplayFrequency;
    HDC dc = CreateDCW(L"DISPLAY", mi.szDevice, NULL, NULL);
    if (!dc) return;
    int mm = GetDeviceCaps(dc, HORZSIZE), px = GetDeviceCaps(dc, HORZRES);
    DeleteDC(dc);
    if (mm >= 150 && mm <= 2500 && px >= 320) *pxPerCm = px / (mm / 10.0);
}

static BOOL FullscreenAppRunning(void)
{
    QUERY_USER_NOTIFICATION_STATE s;
    return SHQueryUserNotificationState(&s) == S_OK && (s == QUNS_BUSY || s == QUNS_RUNNING_D3D_FULL_SCREEN || s == QUNS_PRESENTATION_MODE);
}

// ---- sampler ----

static DWORD WINAPI SampleLoop(void *unused)
{
    (void)unused;
    HMONITOR lastMon = NULL;
    LONGLONG lastMove = 0, lastFast = 0, lastFullscreenCheck = -1000;
    BOOL paused = FALSE, timerHigh = FALSE;
    for (;;)
    {
        LONGLONG checkedAt = Now();
        if (checkedAt - lastFullscreenCheck >= 500)
        {
            lastFullscreenCheck = checkedAt;
            BOOL was = paused;
            paused = g_set.pauseFullscreen && FullscreenAppRunning();
            if (paused != was) PostMessageW(g_main, WM_APP_PAUSED, paused, 0);   // stop listening to the mouse during the game
        }
        if (paused || !g_set.enabled)
        {
            EnterCriticalSection(&gate);
            curVisible = FALSE; histN = 0;
            if (hideWanted) { hideWanted = FALSE; SetEvent(hideChanged); }
            LeaveCriticalSection(&gate);
            if (timerHigh) { timeEndPeriod(1); timerHigh = FALSE; }
            // nothing to do: look for the fullscreen app leaving 4 times a second, or sleep until switched back on
            if (g_set.enabled) Sleep(250); else WaitForSingleObject(mouseWake, INFINITE);
            continue;
        }

        CURSORINFO ci; ci.cbSize = sizeof ci;
        EnterCriticalSection(&gate);
        if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING))
        {
            curVisible = FALSE; histN = 0;
            if (hideWanted) { hideWanted = FALSE; SetEvent(hideChanged); }
        }
        else
        {
            curVisible = TRUE;
            if (!blankActive) curHandle = ci.hCursor;   // keep the real cursor's picture while it is hidden
            BOOL changed = histN == 0 || hist[histN - 1].x != ci.ptScreenPos.x || hist[histN - 1].y != ci.ptScreenPos.y;

            // crossing to another monitor: drop the trail so it doesn't smear across the gap
            HMONITOR mon = changed ? MonitorFromPoint(ci.ptScreenPos, MONITOR_DEFAULTTONEAREST) : lastMon;
            if (mon != lastMon || InterlockedExchange(&monStale, 0))
            {
                histN = 0; lastMon = mon;
                UINT dx = 96, dy = 96;
                int hz, mainHz;
                double pxPerCm;
                GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
                monCursor = GetSystemMetricsForDpi(SM_CXCURSOR, dx);
                MonitorMetrics(mon, dx, &pxPerCm, &hz);
                // The mouse moves the pointer the same number of pixels for the same hand movement on every monitor, so one
                // speed in px/s hides it at the same hand speed everywhere. It is set in cm/s as measured on the main monitor.
                HMONITOR main = MonitorFromPoint((POINT){ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
                GetDpiForMonitor(main, MDT_EFFECTIVE_DPI, &dx, &dy);
                MonitorMetrics(main, dx, &mainPxPerCm, &mainHz);
                // Half a refresh: a whole one is too coarse, because the frames are not timed to the screen's refreshes, so the
                // blur would be fresh on some refreshes and almost a refresh old on others, and jitter against the pointer.
                monGap = 500 / hz > 3 ? 500 / hz : 3;
            }

            LONGLONG now = Now();
            if (changed && histN && hist[histN - 1].t == now)   // several reports within a ms (fast mice): keep the newest
            {
                hist[histN - 1].x = ci.ptScreenPos.x; hist[histN - 1].y = ci.ptScreenPos.y;
            }
            else if (changed || now - hist[histN - 1].t >= 4)
            {
                if (histN == MAX_SAMPLES) { memmove(hist, hist + 1, (MAX_SAMPLES - 1) * sizeof *hist); histN--; }
                hist[histN].x = ci.ptScreenPos.x; hist[histN].y = ci.ptScreenPos.y; hist[histN].t = now; histN++;
            }
            if (changed) { lastMove = now; SetEvent(moved); }   // new mouse position: draw now
            int drop = 0;
            while (histN - drop > 1 && now - hist[drop].t > g_set.trailMs) drop++;
            if (drop) { memmove(hist, hist + drop, (histN - drop) * sizeof *hist); histN -= drop; }

            // speed over the trail window -> hide the real cursor when very fast
            LONGLONG dt = hist[histN - 1].t - hist[0].t;
            BOOL wasHidden = hideWanted;
            if (!g_set.hideWhenFast || drawFailed) hideWanted = FALSE;
            else if (dt >= 8)
            {
                // Hiding looks at the whole trail window (steady). Bringing the cursor back looks at only the last few ms (from
                // sample j on), so it returns as soon as the mouse stops or slows, not 30 ms later.
                int j = histN - 1;
                while (j > 0 && now - hist[j - 1].t <= SHOW_WINDOW_MS) j--;
                double path = 0, recentPath = 0;
                for (int i = 1; i < histN; i++)
                {
                    double d = Dist(hist[i - 1], hist[i]);
                    path += d;
                    if (i > j) recentPath += d;
                }
                double speed = path * 1000.0 / dt;
                double hide = g_set.hideSpeedCm * mainPxPerCm;   // px/s, the same on every monitor
                LONGLONG recentDt = now - hist[j].t;
                double recent = recentDt >= 4 ? recentPath * 1000.0 / recentDt : speed;
                if (speed > hide) { hideWanted = TRUE; lastFast = now; }
                else if (hideWanted && recent < hide * 0.5 && now - lastFast > HOLD_MS) hideWanted = FALSE;
            }
            if (hideWanted && now - lastMove > HOLD_MS + SHOW_WINDOW_MS) hideWanted = FALSE;   // stopped, however the samples are spaced
            if (hideWanted != wasHidden) SetEvent(hideChanged);
        }
        BOOL visible = curVisible, hidden = hideWanted;
        LeaveCriticalSection(&gate);

        if (Now() - lastMove < g_set.trailMs + 40 || hidden)   // never sleep while the real cursor is hidden: it must be given back
        {
            if (!timerHigh) { timeBeginPeriod(1); timerHigh = TRUE; }
            // look again on every mouse report; the timeout keeps the path ageing after a stop, and every 2 ms while the real
            // cursor is hidden so it comes back quickly
            WaitForSingleObject(mouseWake, hidden || !g_rawInput ? 2 : 10);
        }
        else
        {
            if (timerHigh) { timeEndPeriod(1); timerHigh = FALSE; }
            if (!g_rawInput) Sleep(10);   // no raw input: fall back to checking every few ms
            // sleep until the mouse reports something; while an app hides the cursor, just look once per report
            else if (WaitForSingleObject(mouseWake, INFINITE) == WAIT_OBJECT_0 && visible) lastMove = Now();
        }
    }
}

// ---- hider ----

static void ReloadCursors(void) { SystemParametersInfoW(SPI_SETCURSORS, 0, NULL, 0); }

static DWORD WINAPI HideLoop(void *unused)
{
    (void)unused;
    // static system cursors blanked while the mouse is fast (the animated wait/app-starting ones are left alone)
    static const UINT ids[] = { 32512, 32513, 32515, 32516, 32642, 32643, 32644, 32645, 32646, 32648, 32649, 32651, 32671, 32672 };
    BYTE andMask[128], xorMask[128];
    memset(andMask, 0xFF, sizeof andMask); memset(xorMask, 0, sizeof xorMask);
    for (;;)
    {
        EnterCriticalSection(&gate);
        BOOL want = hideWanted;
        LeaveCriticalSection(&gate);
        if (want && !blankActive)
        {
            blankActive = TRUE;
            for (int i = 0; i < (int)(sizeof ids / sizeof *ids); i++)
            {
                HCURSOR c = CreateCursor(NULL, 0, 0, 32, 32, andMask, xorMask);
                if (c && !SetSystemCursor(c, ids[i])) DestroyCursor(c);   // on success the system owns it
            }
        }
        else if (!want && blankActive)
        {
            ReloadCursors();
            blankActive = FALSE;
        }
        WaitForSingleObject(hideChanged, INFINITE);
    }
}

void Engine_RestoreCursors(void)
{
    if (!blankActive) return;
    ReloadCursors();
    blankActive = FALSE;
}

// ---- cursor pictures (render thread only) ----

static struct { HCURSOR h; Sprite *sp; } sprites[MAX_SPRITES];
static int nSprites, nextId;
static HCURSOR empties[MAX_SPRITES];   // cursors an app made invisible on purpose: no blur for them
static int nEmpties;
static Sprite *lastSprite;             // the last real picture, drawn while the real cursor is hidden

// Takes the cursor's picture by drawing it on black and on white: what shows through tells the opacity. *empty = it is invisible.
static Sprite *Grab(HCURSOR h, BOOL *empty)
{
    *empty = FALSE;
    ICONINFO ii;
    if (!GetIconInfo(h, &ii)) return NULL;
    BITMAP bm = { 0 };
    GetObjectW(ii.hbmColor ? ii.hbmColor : ii.hbmMask, sizeof bm, &bm);
    int w = bm.bmWidth, ht = ii.hbmColor ? bm.bmHeight : bm.bmHeight / 2;   // a monochrome cursor's mask holds AND and XOR halves
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    if (w <= 0 || ht <= 0 || w > 256 || ht > 256) return NULL;

    int n = w * ht;
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), w, -ht, 1, 32 } };
    void *bits;
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    Sprite *sp = dib ? malloc(sizeof *sp + n * 4) : NULL;
    if (sp)
    {
        HGDIOBJ old = SelectObject(dc, dib);
        UINT32 *px = (UINT32 *)(sp + 1), *onWhite = bits;
        memset(bits, 0, n * 4); DrawIconEx(dc, 0, 0, h, w, ht, 0, NULL, DI_NORMAL); GdiFlush();
        memcpy(px, bits, n * 4);   // on black
        memset(bits, 0xFF, n * 4); DrawIconEx(dc, 0, 0, h, w, ht, 0, NULL, DI_NORMAL); GdiFlush();
        BOOL any = FALSE;
        for (int i = 0; i < n; i++)
        {
            UINT32 b = px[i];
            int a = 255 - ((int)(onWhite[i] >> 8 & 255) - (int)(b >> 8 & 255));   // how much of the background shows through
            a = a < 0 ? 0 : a > 255 ? 255 : a;
            UINT32 r = b >> 16 & 255, g = b >> 8 & 255, bl = b & 255;   // on black = the premultiplied colour
            px[i] = (UINT32)a << 24 | (r < (UINT32)a ? r : a) << 16 | (g < (UINT32)a ? g : a) << 8 | (bl < (UINT32)a ? bl : a);
            any |= a != 0;
        }
        SelectObject(dc, old);
        sp->w = w; sp->h = ht; sp->hx = ii.xHotspot; sp->hy = ii.yHotspot; sp->id = ++nextId; sp->px = px;
        if (!any) { free(sp); sp = NULL; *empty = TRUE; }
    }
    if (dib) DeleteObject(dib);
    DeleteDC(dc);
    return sp;
}

static void FreeSprites(void)
{
    for (int i = 0; i < nSprites; i++) free(sprites[i].sp);
    nSprites = nEmpties = 0;
    lastSprite = NULL;
}

static BOOL IsEmpty(HCURSOR h)
{
    for (int i = 0; i < nEmpties; i++) if (empties[i] == h) return TRUE;
    return FALSE;
}

static Sprite *GetSprite(HCURSOR h)
{
    for (int i = 0; i < nSprites; i++) if (sprites[i].h == h) return sprites[i].sp;
    if (IsEmpty(h)) return NULL;
    BOOL hiddenBefore = blankActive, empty;
    Sprite *sp = Grab(h, &empty);
    // Only keep a picture taken while the real cursor is showing: blanking replaces the content under the same handle.
    if (sp && blankActive) { free(sp); sp = NULL; }
    if (sp)
    {
        if (nSprites == MAX_SPRITES) FreeSprites();
        sprites[nSprites].h = h; sprites[nSprites].sp = sp; nSprites++;
    }
    // Empty while our own blanking was off the whole time: the app itself shows an invisible cursor. Remember that.
    if (empty && !hiddenBefore && !blankActive)
    {
        if (nEmpties == MAX_SPRITES) nEmpties = 0;
        empties[nEmpties++] = h;
    }
    return sp;
}

// ---- render ----

static BOOL shown;

static BOOL Hide(void)
{
    if (shown) { R_Hide(); shown = FALSE; }
    return FALSE;
}

// Draws one frame of the blur; FALSE if there is nothing to draw (then the overlay is hidden).
static BOOL Render(void)
{
    static Sample pts[MAX_SAMPLES];
    static Copy copies[COPY_BUF];
    int np, size; HCURSOR handle; BOOL visible;
    EnterCriticalSection(&gate);
    visible = curVisible; handle = curHandle; size = monCursor; np = histN;
    memcpy(pts, hist, np * sizeof *pts);
    LeaveCriticalSection(&gate);
    if (!visible || np < 2) return Hide();
    if (spritesStale && !blankActive) { spritesStale = 0; FreeSprites(); }   // the cursor scheme, size or colour may have changed

    Sprite *sp;
    if (blankActive) sp = lastSprite;   // the real cursor is hidden: keep drawing the picture taken before
    else
    {
        if (IsEmpty(handle)) return Hide();   // the app hid its cursor: don't blur the previous one
        sp = GetSprite(handle);
        if (sp) lastSprite = sp; else sp = lastSprite;
    }
    if (!sp) return Hide();

    Sample last = pts[np - 1];
    BOOL anyMove = FALSE;
    for (int i = 0; i < np && !anyMove; i++) anyMove = pts[i].x != last.x || pts[i].y != last.y;
    if (!anyMove) return Hide();   // stationary: nothing to blur

    // the picture at the size Windows draws the cursor on this monitor
    double f = (double)size / sp->w;
    int sw = size, sh = (int)lround(sp->h * f), hx = (int)lround(sp->hx * f), hy = (int)lround(sp->hy * f);

    int minX = INT_MAX, minY = INT_MAX, maxX = INT_MIN, maxY = INT_MIN;
    double total = 0;
    for (int i = 0; i < np; i++)
    {
        if (pts[i].x - hx < minX) minX = pts[i].x - hx;
        if (pts[i].y - hy < minY) minY = pts[i].y - hy;
        if (pts[i].x - hx + sw > maxX) maxX = pts[i].x - hx + sw;
        if (pts[i].y - hy + sh > maxY) maxY = pts[i].y - hy + sh;
        if (i) total += Dist(pts[i - 1], pts[i]);
    }
    double step = total / MAX_COPIES > 1 ? total / MAX_COPIES : 1;   // dense, faint copies read as a smooth blur, not separate ghosts
    double cover = COVER_PX * size / 32.0, peak = g_set.strength / 100.0, trail = g_set.trailMs;
    LONGLONG now = Now();
    int n = 0;
    for (int i = 1; i < np && n < COPY_BUF; i++)
    {
        Sample a = pts[i - 1], b = pts[i];
        double dx = b.x - a.x, dy = b.y - a.y, d = sqrt(dx * dx + dy * dy);
        if (d < 0.5) continue;
        int k = (int)(d / step);
        if (k < 1) k = 1;
        for (int c = 0; c < k && n < COPY_BUF; c++)
        {
            double t = (double)c / k;
            double life = 1.0 - (now - (a.t + (b.t - a.t) * t)) / trail;
            if (life <= 0) continue;
            life *= life;   // steeper fade
            // copies overlap by about cover/step, so each gets that share of the opacity asked for
            double alpha = peak * life * step / cover;
            if (alpha * 255 < 0.5) continue;
            copies[n].x = (int)lround(a.x + dx * t) - hx - minX;
            copies[n].y = (int)lround(a.y + dy * t) - hy - minY;
            copies[n].a = alpha > 1 ? 1.0f : (float)alpha;
            n++;
        }
    }
    if (!n) return Hide();
    drawFailed = !R_Draw(sp, sw, sh, copies, n, minX, minY, maxX - minX, maxY - minY);
    if (drawFailed) return Hide();
    shown = TRUE;
    return TRUE;
}

// Draws when the mouse moves, but at most once per refresh of the monitor (the screen shows no more than that); after the
// mouse stops it keeps drawing once per refresh until the trail has faded. Idle, it wakes once more to give back memory, then sleeps until the mouse moves.
// It also runs the overlay window's messages.
static DWORD WINAPI RenderLoop(void *unused)
{
    (void)unused;
    LONGLONG lastDraw = -1000;
    BOOL trimmed = TRUE, pending = FALSE;   // pending: the mouse moved, but too soon after the last frame
    for (;;)
    {
        LONGLONG wait = monGap - (Now() - lastDraw);
        DWORD timeout = pending || shown ? (wait > 0 ? (DWORD)wait : 0) : trimmed ? INFINITE : 1500;
        DWORD r = MsgWaitForMultipleObjects(1, &moved, FALSE, timeout, QS_ALLINPUT);
        if (r == WAIT_OBJECT_0 + 1)
        {
            MSG m;
            while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
            continue;
        }
        if (r == WAIT_OBJECT_0) pending = TRUE;
        if (!pending && !shown) { R_Trim(); trimmed = TRUE; continue; }
        if (Now() - lastDraw < monGap) continue;
        pending = FALSE;
        if (Render()) { lastDraw = Now(); trimmed = FALSE; }
    }
}

void Engine_Start(void)
{
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    InitializeCriticalSection(&gate);
    // Windows 11 ignores a finer timer for a process with no visible window; the short waits here need it while the mouse moves
    PROCESS_POWER_THROTTLING_STATE pt = { PROCESS_POWER_THROTTLING_CURRENT_VERSION, PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION, 0 };
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof pt);
    moved = CreateEventW(NULL, FALSE, FALSE, NULL);
    mouseWake = CreateEventW(NULL, FALSE, FALSE, NULL);
    hideChanged = CreateEventW(NULL, FALSE, FALSE, NULL);
    HANDLE t;
    t = CreateThread(NULL, 0, SampleLoop, NULL, 0, NULL); SetThreadPriority(t, THREAD_PRIORITY_ABOVE_NORMAL); CloseHandle(t);
    t = CreateThread(NULL, 0, RenderLoop, NULL, 0, NULL); SetThreadPriority(t, THREAD_PRIORITY_ABOVE_NORMAL); CloseHandle(t);
    t = CreateThread(NULL, 0, HideLoop, NULL, 0, NULL); CloseHandle(t);
}
