// CPU renderer: blends the copies by hand into a bitmap and shows it with a layered window (UpdateLayeredWindow).
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
static int lastW, lastH;        // size of the last frame: the window keeps the canvas size, so that part is cleared too

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
static void Resize(const Sprite *sp, int w, int h)
{
    if (pic && picId == sp->id && picW == w && picH == h) return;
    free(pic);
    pic = malloc(w * h * 4 + h * 2 * sizeof(int));
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

BOOL R_Draw(const Sprite *sp, int sw, int sh, const Copy *c, int n, int x, int y, int w, int h)
{
    if (!wnd) CreateOverlay();
    if (!wnd || !memDc) return FALSE;
    Resize(sp, sw, sh);
    EnsureCanvas(w, h);
    if (!dib || !pic) return FALSE;
    int cw = w > lastW ? w : lastW, ch = h > lastH ? h : lastH;
    if (cw > canvasW) cw = canvasW;
    if (ch > canvasH) ch = canvasH;
    for (int r = 0; r < ch; r++) memset(bits + r * canvasW, 0, cw * 4);
    lastW = w; lastH = h;
    for (int i = 0; i < n; i++)
    {
        int a = (int)(c[i].a * 255 + 0.5f);
        if (a > 0 && c[i].x >= 0 && c[i].y >= 0 && c[i].x + sw <= w && c[i].y + sh <= h) Blend(a, c[i].x, c[i].y);
    }
    // The window keeps the size of the canvas: Windows then reuses its surface instead of making a new one every frame.
    POINT dst = { x, y }, src = { 0, 0 };
    SIZE size = { canvasW, canvasH };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(wnd, NULL, &dst, &size, memDc, &src, 0, &bf, ULW_ALPHA);
    if (!IsWindowVisible(wnd)) SetWindowPos(wnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    return TRUE;
}

void R_Hide(void) { if (wnd) ShowWindow(wnd, SW_HIDE); }

void R_Trim(void)
{
    ReleaseCanvas();
    free(pic); pic = NULL;
    lastW = lastH = 0;
}
