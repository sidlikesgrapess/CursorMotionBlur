// The engine: three threads.
//   sampler  reads the cursor position on every raw mouse report and decides when the blur is drawn and when the real
//            cursor should be hidden
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
static volatile int monGap = 7;      // ms between frames: half a refresh of the monitor the cursor is on
static volatile LONG blankActive;    // the system cursors are currently swapped for an invisible one
static volatile LONG drawFailed;     // the renderer could not draw: then never hide the real cursor
static volatile LONG monStale, spritesStale;   // display or cursor settings changed: measure the monitor / take the pictures again
static volatile LONG recheck;        // settings or the active window changed: look at the window in front now
static volatile LONG drawing;        // the blur is drawn: above the minimum blur speed, or while the cursor is hidden
static volatile LONG stopping;
static HCURSOR volatile pictureOf;   // the cursor whose picture the blur has: hiding waits until it is the current one

static HANDLE moved, mouseWake, hideChanged;   // auto-reset events
static HANDLE threads[3];
static LARGE_INTEGER freq, start;
static int ownIntegrity;

static LONGLONG Now(void)   // ms since start
{
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return (t.QuadPart - start.QuadPart) * 1000 / freq.QuadPart;
}

void Engine_Wake(void) { if (mouseWake) SetEvent(mouseWake); }
void Engine_Recheck(void) { recheck = 1; Engine_Wake(); }
void Engine_DisplayChanged(void) { monStale = spritesStale = 1; }

static double Dist(Sample a, Sample b) { double dx = b.x - a.x, dy = b.y - a.y; return sqrt(dx * dx + dy * dy); }
static int Clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

// A monitor's refresh rate.
static int RefreshRate(HMONITOR mon)
{
    MONITORINFOEXW mi; mi.cbSize = sizeof mi;
    DEVMODEW dm = { .dmSize = sizeof dm };
    if (GetMonitorInfoW(mon, (MONITORINFO *)&mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
        return dm.dmDisplayFrequency;
    return 60;
}

// A monitor's physical pixel density, from the size Windows reports for it (its DPI setting if that looks wrong).
static double PixelsPerCm(HMONITOR mon, UINT dpi)
{
    double fallback = dpi / 2.54 < 20 ? 20 : dpi / 2.54;
    MONITORINFOEXW mi; mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(mon, (MONITORINFO *)&mi)) return fallback;
    HDC dc = CreateDCW(L"DISPLAY", mi.szDevice, NULL, NULL);
    if (!dc) return fallback;
    int mm = GetDeviceCaps(dc, HORZSIZE), px = GetDeviceCaps(dc, HORZRES);
    DeleteDC(dc);
    return mm >= 150 && mm <= 2500 && px >= 320 ? px / (mm / 10.0) : fallback;
}

static BOOL FullscreenAppRunning(void)
{
    QUERY_USER_NOTIFICATION_STATE s;
    return SHQueryUserNotificationState(&s) == S_OK && (s == QUNS_BUSY || s == QUNS_RUNNING_D3D_FULL_SCREEN || s == QUNS_PRESENTATION_MODE);
}

// The pointer size from Settings > Accessibility > Mouse pointer and touch, at 100% scaling: 32 px at size 1 and 16 px more
// per step. (The cursor pictures Windows hands out can stay at size 1 when this is larger.)
static int PointerSize(void)
{
    DWORD size = 0, len = sizeof size;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Cursors", L"CursorBaseSize", RRF_RT_REG_DWORD, NULL, &size, &len) == ERROR_SUCCESS &&
        size >= 32 && size <= 256)
        return (int)size;
    return 32;
}

// A process's integrity level (medium for normal apps, high for administrator ones), or -1 if its token can't be read.
static int Integrity(HANDLE process)
{
    int level = -1;
    HANDLE token;
    if (OpenProcessToken(process, TOKEN_QUERY, &token))
    {
        union { TOKEN_MANDATORY_LABEL label; BYTE bytes[SECURITY_MAX_SID_SIZE + sizeof(TOKEN_MANDATORY_LABEL)]; } buf;
        DWORD len;
        if (GetTokenInformation(token, TokenIntegrityLevel, &buf, sizeof buf, &len))
        {
            PSID sid = buf.label.Label.Sid;
            level = (int)*GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1);
        }
        CloseHandle(token);
    }
    return level;
}

// Whether a window belongs to a process with a higher integrity level than this one (an administrator app such as Task
// Manager or an admin terminal): Windows sends this process no raw mouse input meanwhile.
static BOOL HigherRights(HWND w)
{
    DWORD pid = 0;
    if (!w || !GetWindowThreadProcessId(w, &pid)) return FALSE;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return TRUE;
    int level = Integrity(p);
    CloseHandle(p);
    return level < 0 || level > ownIntegrity;
}

// ---- sampler ----

static DWORD WINAPI SampleLoop(void *unused)
{
    (void)unused;
    RECT monRect = { 0 };       // the monitor the cursor is on
    HWND lastFront = NULL;
    int pointerSize = 0;        // the pointer size set in Windows, at 100% scaling (0: not read yet)
    UINT mainDpi = 96;
    double mainPxPerCm = 0;     // pixel density of the main monitor (0: not measured yet)
    double hidePxPerCm = 38;    // turns the hide speed (cm/s on the main monitor) into px/s on the monitor in use
    LONGLONG lastMove = 0, lastFast = 0, lastDrawn = 0, lastLook = -1000, crossedAt = 0;
    BOOL paused = FALSE, timerHigh = FALSE, adminFront = FALSE;
    BOOL fastEnough = FALSE;    // above the minimum blur speed (switched only after 16 ms past the line)
    while (!stopping)
    {
        // The window in front: whenever another window is activated (the shell hook says so, administrator windows included),
        // and every half second while the mouse moves, as an app can also go fullscreen in place.
        BOOL forced = InterlockedExchange(&recheck, 0) != 0;
        LONGLONG t = Now();
        if (forced || t - lastLook >= 500)
        {
            lastLook = t;
            HWND front = GetForegroundWindow();
            BOOL frontChanged = front != lastFront;
            if (frontChanged)
            {
                lastFront = front;
                adminFront = HigherRights(front);
            }
            // A fullscreen app: looked for when the front window changes, and while the mouse moves or the app is paused.
            if (forced || frontChanged || paused || t - lastMove < 1000)
            {
                BOOL was = paused;
                paused = g_set.pauseFullscreen && FullscreenAppRunning();
                if (paused != was) PostMessageW(g_main, WM_APP_PAUSED, paused, 0);   // stop listening to the mouse during the game
            }
        }
        if (paused || !g_set.enabled)
        {
            EnterCriticalSection(&gate);
            curVisible = FALSE; histN = 0;
            if (hideWanted) { hideWanted = FALSE; SetEvent(hideChanged); }
            LeaveCriticalSection(&gate);
            fastEnough = FALSE; drawing = 0; crossedAt = 0;
            if (timerHigh) { timeEndPeriod(1); timerHigh = FALSE; }
            // nothing to do: look for the fullscreen app leaving twice a second, or sleep until switched back on
            WaitForSingleObject(mouseWake, g_set.enabled ? 500 : INFINITE);
            continue;
        }

        CURSORINFO ci; ci.cbSize = sizeof ci;
        BOOL showing = GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING);
        // Crossing to another monitor (or display or cursor settings changed): measure it here, outside the lock, as some of
        // these calls are slow.
        BOOL stale = showing && InterlockedExchange(&monStale, 0);
        BOOL newMon = showing && (stale || !PtInRect(&monRect, ci.ptScreenPos));
        int newCursor = 32;
        if (newMon)
        {
            HMONITOR mon = MonitorFromPoint(ci.ptScreenPos, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi; mi.cbSize = sizeof mi;
            if (GetMonitorInfoW(mon, &mi)) monRect = mi.rcMonitor;
            UINT dpi = 96, dy;
            GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpi, &dy);
            if (stale || !pointerSize) pointerSize = PointerSize();
            newCursor = MulDiv(pointerSize, dpi, 96);
            int hz = RefreshRate(mon);
            // The hide speed is set in cm/s as measured on the main monitor. Windows moves the pointer further on a monitor with
            // more scaling (twice the pixels at 200%), so scale it by the DPI to hide at the same hand speed on every monitor.
            if (stale || !mainPxPerCm)
            {
                HMONITOR mainMon = MonitorFromPoint((POINT){ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
                GetDpiForMonitor(mainMon, MDT_EFFECTIVE_DPI, &mainDpi, &dy);
                mainPxPerCm = PixelsPerCm(mainMon, mainDpi);
            }
            hidePxPerCm = mainPxPerCm * dpi / mainDpi;
            // Half a refresh: frames are not timed to the screen's refreshes, so a whole one would leave the blur fresh on some
            // refreshes and almost a refresh old on others, and it would jitter against the pointer.
            monGap = 500 / hz > 3 ? 500 / hz : 3;
        }

        EnterCriticalSection(&gate);
        if (!showing)
        {
            curVisible = FALSE; histN = 0;
            if (hideWanted) { hideWanted = FALSE; SetEvent(hideChanged); }
        }
        else
        {
            curVisible = TRUE;
            if (!blankActive) curHandle = ci.hCursor;   // keep the real cursor's picture while it is hidden
            BOOL changed = histN == 0 || hist[histN - 1].x != ci.ptScreenPos.x || hist[histN - 1].y != ci.ptScreenPos.y;

            if (newMon) { histN = 0; monCursor = newCursor; }   // drop the trail so it doesn't smear across the gap between monitors

            LONGLONG now = Now();
            if (changed && histN && hist[histN - 1].t == now)   // several reports within a ms (fast mice): keep the newest
            {
                hist[histN - 1].x = ci.ptScreenPos.x; hist[histN - 1].y = ci.ptScreenPos.y;
            }
            else if (changed || histN == 0 || now - hist[histN - 1].t >= 4)
            {
                if (histN == MAX_SAMPLES) { memmove(hist, hist + 1, (MAX_SAMPLES - 1) * sizeof *hist); histN--; }
                hist[histN++] = (Sample){ ci.ptScreenPos.x, ci.ptScreenPos.y, now };
            }
            if (changed)
            {
                if (now - lastMove >= 1000) lastLook = -1000;   // moving again: first see what is in front
                lastMove = now;
            }
            int drop = 0;
            while (histN - drop > 1 && now - hist[drop].t > g_set.trailMs) drop++;
            if (drop) { memmove(hist, hist + drop, (histN - drop) * sizeof *hist); histN -= drop; }

            // Speed over the trail window: the blur is drawn above the draw speed and stops below half of it, each only once the
            // speed has stayed past that line for 16 ms (mouse reports arrive unevenly, so a single reading can be a third off).
            // The real cursor is hidden when very fast; bringing it back looks at only the last few ms (from sample j on), so it
            // returns as soon as the mouse stops or slows.
            LONGLONG dt = hist[histN - 1].t - hist[0].t;
            int j = histN - 1;
            while (j > 0 && now - hist[j - 1].t <= SHOW_WINDOW_MS) j--;
            double path = 0, recentPath = 0;
            for (int i = 1; i < histN; i++)
            {
                double d = Dist(hist[i - 1], hist[i]);
                path += d;
                if (i > j) recentPath += d;
            }
            double speed = dt >= 4 ? path * 1000.0 / dt : 0;
            double drawAt = g_set.drawSpeedCm * hidePxPerCm * (fastEnough ? 0.5 : 1);
            BOOL want = histN > 1 && (g_set.drawSpeedCm == 0 || (dt >= 4 && speed >= drawAt));
            if (want == fastEnough || histN < 2 || g_set.drawSpeedCm == 0) { crossedAt = 0; fastEnough = want; }
            else if (!crossedAt) crossedAt = now;
            else if (now - crossedAt >= 16) { crossedAt = 0; fastEnough = want; }
            BOOL wasHidden = hideWanted;
            if (!g_set.hideWhenFast || drawFailed) hideWanted = FALSE;
            else if (dt >= 8)
            {
                double hide = g_set.hideSpeedCm * hidePxPerCm;   // px/s on this monitor, the same hand speed everywhere
                LONGLONG recentDt = now - hist[j].t;
                double recent = recentDt >= 4 ? recentPath * 1000.0 / recentDt : speed;
                if (speed > hide) { hideWanted = TRUE; lastFast = now; }
                else if (hideWanted && recent < hide * 0.5 && now - lastFast > HOLD_MS) hideWanted = FALSE;
            }
            if (hideWanted && now - lastMove > HOLD_MS + SHOW_WINDOW_MS) hideWanted = FALSE;   // stopped
            if (hideWanted != wasHidden) SetEvent(hideChanged);
            // The minimum blur speed only decides drawing; hiding goes by its own speed. A hidden cursor always gets its blur, so
            // the pointer never disappears.
            drawing = fastEnough || hideWanted;
            if ((changed || hideWanted != wasHidden) && drawing) { lastDrawn = now; SetEvent(moved); }   // draw now
        }
        BOOL hidden = hideWanted;
        LeaveCriticalSection(&gate);

        // drawing (or the trail still fading), or the real cursor hidden: look often; below the draw speed, only on reports
        if (Now() - lastDrawn < g_set.trailMs + 40 || hidden)
        {
            if (!timerHigh) { timeBeginPeriod(1); timerHigh = TRUE; }
            // look again on every mouse report; the timeout keeps the path ageing after a stop, and checks every 2 ms while the
            // real cursor is hidden so it comes back quickly
            WaitForSingleObject(mouseWake, hidden || !g_rawInput ? 2 : 10);
        }
        else
        {
            if (timerHigh) { timeEndPeriod(1); timerHigh = FALSE; }
            // Sleep until the mouse moves or another window is activated. While a window with more rights is in front no mouse
            // reports arrive, so look at the cursor every 10 ms instead, and every 100 ms once the mouse has been still for a
            // second. Without raw input, look every 10 ms.
            if (fastEnough) { fastEnough = FALSE; drawing = 0; crossedAt = 0; }   // the trail has faded: start below the minimum again
            DWORD poll = INFINITE;
            if (adminFront) poll = Now() - lastMove < 1000 ? 10 : 100;
            else if (!g_rawInput) poll = 10;
            WaitForSingleObject(mouseWake, poll);
        }
    }
    if (timerHigh) timeEndPeriod(1);
    return 0;
}

// ---- hider ----

static void ReloadCursors(void) { SystemParametersInfoW(SPI_SETCURSORS, 0, NULL, 0); }

// Whether our invisible cursor is in place, also kept in the registry: if the app is ever killed while hiding, the next start
// knows to put the user's cursors back (and otherwise leaves the cursor scheme alone). It is written once per hide and per
// restore, only while hiding is switched on.
#define FLAG_KEY  L"Software\\" APP_NAME
#define FLAG_NAME L"CursorsHidden"

static void SetBlank(BOOL on)
{
    blankActive = on;
    DWORD v = on;
    RegSetKeyValueW(HKEY_CURRENT_USER, FLAG_KEY, FLAG_NAME, REG_DWORD, &v, sizeof v);
}

static BOOL BlankFlag(void)
{
    DWORD v = 0, len = sizeof v;
    return RegGetValueW(HKEY_CURRENT_USER, FLAG_KEY, FLAG_NAME, RRF_RT_REG_DWORD, NULL, &v, &len) == ERROR_SUCCESS && v;
}

static DWORD WINAPI HideLoop(void *unused)
{
    (void)unused;
    // static system cursors blanked while the mouse is fast (the animated wait/app-starting ones are left alone)
    static const UINT ids[] = { 32512, 32513, 32515, 32516, 32642, 32643, 32644, 32645, 32646, 32648, 32649, 32651, 32671, 32672 };
    BYTE andMask[128], xorMask[128];
    memset(andMask, 0xFF, sizeof andMask); memset(xorMask, 0, sizeof xorMask);
    while (!stopping)
    {
        EnterCriticalSection(&gate);
        BOOL want = hideWanted;
        HCURSOR cur = curHandle;
        LeaveCriticalSection(&gate);
        // Swap only once the blur has this cursor's picture: from the invisible one it can't be taken. The render thread takes it
        // on its next frame (a few ms) and wakes this thread.
        if (want && !blankActive && pictureOf == cur)
        {
            SetBlank(TRUE);
            for (int i = 0; i < (int)(sizeof ids / sizeof *ids); i++)
            {
                HCURSOR c = CreateCursor(NULL, 0, 0, 32, 32, andMask, xorMask);
                if (c && !SetSystemCursor(c, ids[i])) DestroyCursor(c);   // on success the system owns it
            }
        }
        else if (!want && blankActive)
        {
            ReloadCursors();
            SetBlank(FALSE);
        }
        WaitForSingleObject(hideChanged, INFINITE);
    }
    if (blankActive) { ReloadCursors(); SetBlank(FALSE); }   // never leave the user with an invisible cursor
    return 0;
}

void Engine_RestoreCursors(void)
{
    if (!blankActive) return;
    ReloadCursors();
    SetBlank(FALSE);
}

// In the crash handler: give the cursors back, but leave the flag set so the next start checks again.
void Engine_CrashRestore(void) { if (blankActive) ReloadCursors(); }

// ---- cursor pictures (render thread only) ----

static struct { HCURSOR h; int size; Sprite *sp; } sprites[MAX_SPRITES];   // keyed by cursor and drawn size
static int nSprites, nextId;
// Cursors an app made invisible on purpose: no blur for them. Checked again after a while, because another tool that hides the
// cursor the way this app does (swapping the system cursors) would otherwise switch the blur off for good.
static struct { HCURSOR h; LONGLONG t; } empties[MAX_SPRITES];
static int nEmpties;
static Sprite *lastSprite;             // the last real picture, drawn while the real cursor is hidden

// The standard cursors, with their names in the cursor scheme (HKCU\Control Panel\Cursors).
static const struct { WORD id; PCWSTR name; } standardCursors[] = {
    { 32512, L"Arrow" },    { 32513, L"IBeam" },    { 32514, L"Wait" },     { 32515, L"Crosshair" }, { 32516, L"UpArrow" },
    { 32631, L"NWPen" },    { 32642, L"SizeNWSE" }, { 32643, L"SizeNESW" }, { 32644, L"SizeWE" },    { 32645, L"SizeNS" },
    { 32646, L"SizeAll" },  { 32648, L"No" },       { 32649, L"Hand" },     { 32650, L"AppStarting" }, { 32651, L"Help" },
    { 32671, L"Pin" },      { 32672, L"Person" },
};

// The cursor at size x size, for the caller to destroy: a standard cursor is loaded again from its scheme file (or from Windows'
// own cursors) at that size, so a large pointer stays sharp; any other is copied, enlarged if need be.
static HCURSOR AtSize(HCURSOR h, int size)
{
    for (int i = 0; i < (int)(sizeof standardCursors / sizeof *standardCursors); i++)
    {
        if (LoadCursorW(NULL, MAKEINTRESOURCEW(standardCursors[i].id)) != h) continue;
        WCHAR path[MAX_PATH];
        DWORD len = sizeof path;
        HANDLE loaded = NULL;
        if (RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Cursors", standardCursors[i].name, RRF_RT_REG_SZ, NULL, path, &len) == ERROR_SUCCESS && path[0])
            loaded = LoadImageW(NULL, path, IMAGE_CURSOR, size, size, LR_LOADFROMFILE);
        if (!loaded) loaded = LoadImageW(NULL, MAKEINTRESOURCEW(standardCursors[i].id), IMAGE_CURSOR, size, size, 0);
        if (loaded) return (HCURSOR)loaded;
        break;
    }
    return (HCURSOR)CopyImage(h, IMAGE_CURSOR, size, size, 0);
}

// A cursor's picture size and hotspot.
static BOOL CursorInfo(HCURSOR h, int *w, int *ht, POINT *hot)
{
    ICONINFO ii;
    if (!GetIconInfo(h, &ii)) return FALSE;
    BITMAP bm = { 0 };
    GetObjectW(ii.hbmColor ? ii.hbmColor : ii.hbmMask, sizeof bm, &bm);
    *w = bm.bmWidth;
    *ht = ii.hbmColor ? bm.bmHeight : bm.bmHeight / 2;   // a monochrome cursor's mask holds AND and XOR halves
    hot->x = (LONG)ii.xHotspot; hot->y = (LONG)ii.yHotspot;
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    return TRUE;
}

// Takes the cursor's picture by drawing it on black and on white: what shows through gives the opacity. It is taken at the size
// it will be drawn (up to 256 px): the cursor as it is when that is its size, else a copy at that size. *empty = it is invisible.
static Sprite *Grab(HCURSOR h, int size, BOOL *empty)
{
    *empty = FALSE;
    if (size > 256) size = 256;
    int w, ht;
    POINT hot;
    if (!CursorInfo(h, &w, &ht, &hot)) return NULL;
    HCURSOR sized = w != size ? AtSize(h, size) : NULL;
    if (sized && CursorInfo(sized, &w, &ht, &hot)) h = sized;
    if (w <= 0 || ht <= 0 || w > 256 || ht > 256)
    {
        if (sized) DestroyCursor(sized);
        return NULL;
    }

    int n = w * ht;
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), w, -ht, 1, 32 } };
    void *bits;
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP dib = dc ? CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0) : NULL;
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
            a = Clamp(a, 0, 255);
            UINT32 r = b >> 16 & 255, g = b >> 8 & 255, bl = b & 255, ua = (UINT32)a;   // on black = the premultiplied colour
            px[i] = ua << 24 | (r < ua ? r : ua) << 16 | (g < ua ? g : ua) << 8 | (bl < ua ? bl : ua);
            any |= a != 0;
        }
        SelectObject(dc, old);
        sp->w = w; sp->h = ht; sp->hx = hot.x; sp->hy = hot.y; sp->id = ++nextId; sp->px = px;
        if (!any) { free(sp); sp = NULL; *empty = TRUE; }
    }
    if (dib) DeleteObject(dib);
    if (dc) DeleteDC(dc);
    if (sized) DestroyCursor(sized);
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
    for (int i = 0; i < nEmpties; i++)
        if (empties[i].h == h)
        {
            if (Now() - empties[i].t < 2000) return TRUE;
            empties[i] = empties[--nEmpties];   // look at it again
            return FALSE;
        }
    return FALSE;
}

static Sprite *GetSprite(HCURSOR h, int size)
{
    for (int i = 0; i < nSprites; i++) if (sprites[i].h == h && sprites[i].size == size) return sprites[i].sp;
    if (IsEmpty(h)) return NULL;
    BOOL hiddenBefore = blankActive != 0, empty;
    Sprite *sp = Grab(h, size, &empty);
    // Only keep a picture taken while the real cursor is showing: blanking replaces the content under the same handle.
    if (sp && blankActive) { free(sp); sp = NULL; }
    if (sp)
    {
        if (nSprites == MAX_SPRITES) FreeSprites();
        sprites[nSprites].h = h; sprites[nSprites].size = size; sprites[nSprites].sp = sp; nSprites++;
    }
    // Empty while our own blanking was off the whole time: the app itself shows an invisible cursor. Remember that.
    if (empty && !hiddenBefore && !blankActive)
    {
        if (nEmpties == MAX_SPRITES) nEmpties = 0;
        empties[nEmpties].h = h; empties[nEmpties].t = Now(); nEmpties++;
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
    if (!visible || np < 2 || !drawing) return Hide();
    if (spritesStale && !blankActive) { spritesStale = 0; FreeSprites(); }   // the cursor scheme, size or colour may have changed

    Sprite *sp;
    if (blankActive) sp = lastSprite;   // the real cursor is hidden: keep drawing the picture taken before
    else
    {
        if (IsEmpty(handle)) return Hide();   // the app hid its cursor: don't blur the previous one
        sp = GetSprite(handle, size);
        if (sp)
        {
            lastSprite = sp;
            if (pictureOf != handle)   // the hider may be waiting for this picture
            {
                pictureOf = handle;
                SetEvent(hideChanged);
            }
        }
        else sp = lastSprite;
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
    double cover = COVER_PX * size / 32.0, peak = g_set.opacity / 100.0, trail = g_set.trailMs;
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
            double age = (now - (a.t + (b.t - a.t) * t)) / trail;   // 0 at the pointer, 1 at the end of the trail
            if (age >= 1) continue;
            double life = 1.0 - age * age;   // nearly full strength behind the pointer, fading out toward the end
            // About cover/step copies overlap at each point; layered on top of each other they add up to the target opacity when
            // each has 1 - (1 - target)^(step/cover).
            double target = peak * life, overlap = cover / step;
            double alpha = overlap > 1 ? 1 - pow(1 - target, 1 / overlap) : target;
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

// Draws when the mouse moves, at most once per half refresh of the monitor, and keeps drawing until the trail has faded after a
// stop. Idle, it wakes once more to give back memory, then sleeps until the mouse moves. It also runs the overlay window's messages.
static DWORD WINAPI RenderLoop(void *unused)
{
    (void)unused;
    LONGLONG lastDraw = -1000;
    BOOL trimmed = TRUE, pending = FALSE;   // pending: the mouse moved, but too soon after the last frame
    while (!stopping)
    {
        LONGLONG wait = monGap - (Now() - lastDraw);
        DWORD timeout = pending || shown ? (wait > 0 ? (DWORD)wait : 0) : trimmed ? INFINITE : 1500;
        DWORD r = MsgWaitForMultipleObjects(1, &moved, FALSE, timeout, QS_ALLINPUT);
        if (stopping) break;
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
    Hide();
    R_Trim();
    FreeSprites();
    return 0;
}

void Engine_Start(void)
{
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    InitializeCriticalSection(&gate);
    ownIntegrity = Integrity(GetCurrentProcess());
    if (BlankFlag())   // an earlier run was killed while hiding the cursor
    {
        Log("restoring the cursors after an earlier run");
        ReloadCursors();
        RegDeleteKeyValueW(HKEY_CURRENT_USER, FLAG_KEY, FLAG_NAME);
    }
    // Windows 11 ignores a finer timer for a process with no visible window; the short waits here need it while the mouse moves
    PROCESS_POWER_THROTTLING_STATE pt = { PROCESS_POWER_THROTTLING_CURRENT_VERSION, PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION, 0 };
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof pt);
    moved = CreateEventW(NULL, FALSE, FALSE, NULL);
    mouseWake = CreateEventW(NULL, FALSE, FALSE, NULL);
    hideChanged = CreateEventW(NULL, FALSE, FALSE, NULL);
    threads[0] = CreateThread(NULL, 0, SampleLoop, NULL, 0, NULL);
    threads[1] = CreateThread(NULL, 0, RenderLoop, NULL, 0, NULL);
    threads[2] = CreateThread(NULL, 0, HideLoop, NULL, 0, NULL);
    if (threads[0]) SetThreadPriority(threads[0], THREAD_PRIORITY_ABOVE_NORMAL);
    if (threads[1]) SetThreadPriority(threads[1], THREAD_PRIORITY_ABOVE_NORMAL);
}

// Stops the threads, and makes sure the user's cursors are back and nothing is left behind in the registry.
void Engine_Stop(void)
{
    stopping = 1;
    SetEvent(mouseWake); SetEvent(moved); SetEvent(hideChanged);
    for (int i = 0; i < 3; i++)
        if (threads[i])
        {
            WaitForSingleObject(threads[i], 5000);
            CloseHandle(threads[i]);
            threads[i] = NULL;
        }
    Engine_RestoreCursors();
    RegDeleteKeyW(HKEY_CURRENT_USER, FLAG_KEY);
}
