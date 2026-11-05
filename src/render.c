// Draws the copies: blends them by hand into a bitmap and shows it with a layered window (UpdateLayeredWindow). This
// beat a Direct3D + DirectComposition version on both CPU and memory (the graphics driver alone takes ~20 MB).
#include "app.h"
#include <stdlib.h>
#include <string.h>

static HWND wnd;
static HDC memDc;
static HBITMAP dib;
static HGDIOBJ dibOld;
static UINT32 *bits;            // the canvas: premultiplied BGRA, top-down, canvasW wide; frames use its top-left corner
static int canvasW, canvasH;
static UINT32 *pic;             // the cursor picture at the drawn size
static int *span;               // per row of pic: first and one-past-last visible pixel (most of a cursor is transparent)
static int picId, picW, picH;
static BOOL visible;            // the overlay window is shown
// Where the window sits (screen coordinates; placed: it is there and holds the canvas) and the part of it the last frame drew
// on (window and canvas coordinates). Windows tells the window under a still pointer about every move or resize of a window
// above it, as if the mouse had moved, and an app like Office then redraws its hovered menu item over and over. So the window
// is larger than the blur and stays put while the pointer is still.
static BOOL placed;
static RECT win, inked;

static void CreateOverlay(void)
{
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"CursorMotionBlurOverlay";
    RegisterClassW(&wc);
    wnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
                          wc.lpszClassName, L"", WS_POPUP, 0, 0, 1, 1, NULL, NULL, wc.hInstance, NULL);
    if (!memDc) memDc = CreateCompatibleDC(NULL);
}

static void ReleaseCanvas(void)
{
    if (dib) { SelectObject(memDc, dibOld); DeleteObject(dib); dib = NULL; }
    canvasW = canvasH = 0;
    SetRectEmpty(&inked);
}

static void EnsureCanvas(int w, int h)   // grows only, in 128 px steps
{
    if (dib && w <= canvasW && h <= canvasH) return;
    int nw = (w + 127) / 128 * 128, nh = (h + 127) / 128 * 128;
    if (nw < canvasW) nw = canvasW;
    if (nh < canvasH) nh = canvasH;
    ReleaseCanvas();
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), nw, -nh, 1, 32 } };
    dib = CreateDIBSection(memDc, &bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
    if (!dib) return;
    dibOld = SelectObject(memDc, dib);
    canvasW = nw; canvasH = nh;
}

// One pixel of the cursor picture resized to w x h, on premultiplied colours: the average of the pixels it covers when
// shrinking, bilinear when growing.
static UINT32 Pixel(const Sprite *sp, int x, int y, int w, int h)
{
    float acc[4] = { 0 }, n = 0;
    if (sp->w > w)
    {
        int x0 = x * sp->w / w, x1 = (x + 1) * sp->w / w, y0 = y * sp->h / h, y1 = (y + 1) * sp->h / h;
        if (x1 <= x0) x1 = x0 + 1;
        if (y1 <= y0) y1 = y0 + 1;
        for (int yy = y0; yy < y1; yy++)
            for (int xx = x0; xx < x1; xx++, n++)
                for (int s = 0; s < 4; s++) acc[s] += sp->px[yy * sp->w + xx] >> (8 * s) & 255;
    }
    else
    {
        float fx = (x + 0.5f) * sp->w / w - 0.5f, fy = (y + 0.5f) * sp->h / h - 0.5f;
        if (fx < 0) fx = 0;
        if (fy < 0) fy = 0;
        int x0 = (int)fx, y0 = (int)fy, x1 = x0 + 1 < sp->w ? x0 + 1 : x0, y1 = y0 + 1 < sp->h ? y0 + 1 : y0;
        float ax = fx - x0, ay = fy - y0, wt[4] = { (1 - ax) * (1 - ay), ax * (1 - ay), (1 - ax) * ay, ax * ay };
        UINT32 p[4] = { sp->px[y0 * sp->w + x0], sp->px[y0 * sp->w + x1], sp->px[y1 * sp->w + x0], sp->px[y1 * sp->w + x1] };
        for (int i = 0; i < 4; i++)
            for (int s = 0; s < 4; s++) acc[s] += (p[i] >> (8 * s) & 255) * wt[i];
        n = 1;
    }
    UINT32 out = 0;
    for (int s = 0; s < 4; s++) out |= (UINT32)(acc[s] / n + 0.5f) << (8 * s);
    return out;
}

// The cursor picture at the drawn size, made again only when the cursor or the size changes.
static BOOL Resize(const Sprite *sp, int w, int h)
{
    if (pic && picId == sp->id && picW == w && picH == h) return TRUE;
    free(pic);
    pic = malloc(w * h * 4 + h * 2 * sizeof(int));
    if (!pic) return FALSE;
    span = (int *)(pic + w * h);
    picId = sp->id; picW = w; picH = h;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) pic[y * w + x] = w == sp->w && h == sp->h ? sp->px[y * w + x] : Pixel(sp, x, y, w, h);
    for (int y = 0; y < h; y++)
    {
        int a = 0, b = w;
        while (a < w && !pic[y * w + a]) a++;
        while (b > a && !pic[y * w + b - 1]) b--;
        span[2 * y] = a; span[2 * y + 1] = b;
    }
    return TRUE;
}

// Draws the picture at (ox, oy) on the canvas at opacity a (1-255): premultiplied "source over", two channels per multiplication.
static void Blend(int a, int ox, int oy)
{
    UINT32 k = (UINT32)a + 1;   // 2-256, so full opacity is exact
    for (int y = 0; y < picH; y++)
    {
        UINT32 *row = bits + (oy + y) * canvasW + ox;
        const UINT32 *src = pic + y * picW;
        for (int x = span[2 * y]; x < span[2 * y + 1]; x++)
        {
            UINT32 s = src[x];
            if (!s) continue;
            s = ((s & 0xFF00FF) * k >> 8 & 0xFF00FF) | ((s >> 8 & 0xFF00FF) * k & 0xFF00FF00);
            UINT32 inv = 256 - (s >> 24), d = row[x];
            row[x] = s + (((d & 0xFF00FF) * inv >> 8 & 0xFF00FF) | ((d >> 8 & 0xFF00FF) * inv & 0xFF00FF00));
        }
    }
}

// Gives the window the canvas: all of it, or only the dirty part. Position and size are passed even when they stay the same, as
// Windows ignores the dirty rectangle without them.
static BOOL Update(const RECT *dirty)
{
    POINT dst = { win.left, win.top }, src = { 0, 0 };
    SIZE size = { win.right - win.left, win.bottom - win.top };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UPDATELAYEREDWINDOWINFO u = { sizeof u, NULL, &dst, &size, memDc, &src, 0, &bf, ULW_ALPHA, dirty };
    return UpdateLayeredWindowIndirect(wnd, &u);
}

static void Wipe(void)   // clears the last frame from the canvas
{
    for (int r = inked.top; r < inked.bottom; r++) memset(bits + r * canvasW + inked.left, 0, (inked.right - inked.left) * 4);
    SetRectEmpty(&inked);
}

// Makes the hidden window 1 x 1, which gives back the memory of its picture.
static void Shrink(void)
{
    if (!placed || !dib || visible) return;
    Wipe();
    win.right = win.left + 1;
    win.bottom = win.top + 1;
    if (!Update(NULL)) placed = FALSE;
}

BOOL R_Draw(const Sprite *sp, int sw, int sh, const Copy *c, int n, int x, int y, int w, int h, POINT at)
{
    if (!wnd) CreateOverlay();
    if (!wnd || !memDc) return FALSE;
    if (!Resize(sp, sw, sh)) return FALSE;
    RECT box = { x, y, x + w, y + h };
    // Moved only when the blur doesn't fit in it any more, so never while the pointer is still: the trail then only shrinks
    // toward the pointer.
    BOOL place = !dib || !placed || box.left < win.left || box.top < win.top || box.right > win.right || box.bottom > win.bottom;
    RECT old = inked;
    if (place)   // half the monitor's width and height around the pointer, within the monitor
    {
        MONITORINFO mi = { sizeof mi };
        if (!GetMonitorInfoW(MonitorFromPoint(at, MONITOR_DEFAULTTONEAREST), &mi)) return FALSE;
        RECT mon = mi.rcMonitor, next;
        int hw = (mon.right - mon.left) / 4, hh = (mon.bottom - mon.top) / 4, margin = sw > sh ? sw : sh;
        SetRect(&next, at.x - hw, at.y - hh, at.x + hw, at.y + hh);
        InflateRect(&mon, margin, margin);   // the cursor reaches past the edge of the monitor
        IntersectRect(&next, &next, &mon);
        UnionRect(&next, &next, &box);
        // Moved at the same size, the window can show up at its new place a frame before its new picture, and the blur jumps
        // ahead; with a new size Windows takes both at once.
        if (placed && next.right - next.left == win.right - win.left && next.bottom - next.top == win.bottom - win.top) next.right++;
        EnsureCanvas(next.right - next.left, next.bottom - next.top);
        if (!dib) return FALSE;
        win = next;
    }
    Wipe();
    int ox = x - win.left, oy = y - win.top;
    for (int i = 0; i < n; i++)
    {
        int a = (int)(c[i].a * 255 + 0.5f);
        if (a > 0 && c[i].x >= 0 && c[i].y >= 0 && c[i].x + sw <= w && c[i].y + sh <= h) Blend(a, c[i].x + ox, c[i].y + oy);
    }
    RECT dirty;
    SetRect(&inked, ox, oy, ox + w, oy + h);
    UnionRect(&dirty, &old, &inked);
    if (!Update(place ? NULL : &dirty)) { placed = FALSE; return FALSE; }
    placed = TRUE;
    if (!visible)
    {
        SetWindowPos(wnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        visible = TRUE;
    }
    return TRUE;
}

void R_Hide(void) { if (wnd) ShowWindow(wnd, SW_HIDE); visible = FALSE; }

void R_Trim(void)
{
    Shrink();
    ReleaseCanvas();
    free(pic); pic = NULL;
}
