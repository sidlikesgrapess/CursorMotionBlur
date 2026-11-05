// Linked with the code under test (the mod's test copy, or the app's engine; see target.h and run.py). Runs its real
// threads against a made-up pointer path while the real pointer stays still, and checks what reaches the screen and the
// window under the pointer. Prints one "RESULT <test> <PASS|FAIL|SKIP|INFO> <detail>" line per check.
//
// harness.exe <bandsim.exe | ->   the tests (with the mod, starts and kills its own explorer stand-in)
// harness.exe --victim            a copy that draws until it is killed (for the crash test)

#include <windows.h>
#include <shellscalingapi.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <psapi.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <limits.h>
#include <math.h>
#include <atomic>
#include <vector>
#include <mutex>
#include <string>
#include <algorithm>
#include "fake.h"
#include "target.h"

// ---- time, results ----

static LARGE_INTEGER g_qf;
static double Ms() {   // QPC in ms, the same clock Desktop Duplication stamps frames with
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart * 1000.0 / g_qf.QuadPart;
}

static int g_fails;
static void Result(const wchar_t* test, const wchar_t* state, const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list a;
    va_start(a, fmt);
    vswprintf(buf, 1024, fmt, a);
    va_end(a);
    if (!wcscmp(state, L"FAIL")) g_fails++;
    wprintf(L"RESULT %ls %ls %ls\n", test, state, buf);
    fflush(stdout);
}

// ---- the white window under the still real pointer ----

static HWND g_W;
static RECT g_M;            // the monitor under the real pointer
static POINT g_P;           // where the real pointer is (and must stay)
static bool g_realMoved;    // the user moved the mouse: message counts are not trustworthy
static std::atomic<int> g_mm, g_sc;   // WM_MOUSEMOVE / WM_SETCURSOR delivered to the white window

static LRESULT CALLBACK WhiteProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_MOUSEMOVE) g_mm++;
    else if (m == WM_SETCURSOR) g_sc++;
    return DefWindowProcW(h, m, w, l);
}

static void CheckReal() {
    POINT p;
    if (GetCursorPos(&p) && (p.x != g_P.x || p.y != g_P.y)) g_realMoved = true;
}

static void Pump(double ms) {   // handles messages, waking every ms
    double end = Ms() + ms;
    for (;;) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        CheckReal();
        double left = end - Ms();
        if (left <= 0) return;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, left >= 1 ? 1 : 0, QS_ALLINPUT);
    }
}

static void QuietWait(double ms) {   // like Pump, but wakes only for messages, so it costs no CPU itself
    double end = Ms() + ms;
    for (;;) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
        double left = end - Ms();
        if (left <= 0) return;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, (DWORD)left, QS_ALLINPUT);
    }
}

// ---- the made-up pointer path ----

struct PathPt { double t; int x, y; };
static std::mutex g_pathLock;
static std::vector<PathPt> g_path;
static int g_maxStep;   // the largest step the mod's trail ever held (from g_hist)

static void WatchHist() { g_maxStep = std::max(g_maxStep, T_MaxHistStep()); }

static void MoveTo(int x, int y) {
    g_fakeX = x;
    g_fakeY = y;
    {
        std::lock_guard<std::mutex> l(g_pathLock);
        g_path.push_back({Ms(), x, y});
    }
    T_Wake();   // as a raw mouse report would
}

static void Swipe(POINT a, POINT b, double pxPerS) {
    double len = hypot((double)b.x - a.x, (double)b.y - a.y), dur = len / pxPerS * 1000, t0 = Ms();
    for (;;) {
        double f = dur > 0 ? (Ms() - t0) / dur : 1;
        if (f > 1) f = 1;
        MoveTo((int)lround(a.x + (b.x - a.x) * f), (int)lround(a.y + (b.y - a.y) * f));
        WatchHist();
        if (f >= 1) return;
        Pump(2);
    }
}

// The area the blur may cover at time t: where the pointer was during the trail before it (and a frame or two more,
// as a frame reaches the screen after it is drawn), plus the cursor's size around it.
static RECT Allowed(double t) {
    double from = t - T_TrailMs() - 80;
    RECT r = {INT_MAX, INT_MAX, INT_MIN, INT_MIN};
    std::lock_guard<std::mutex> l(g_pathLock);
    const PathPt* before = nullptr;
    for (const auto& p : g_path) {
        if (p.t < from) { before = &p; continue; }
        if (p.t > t) break;
        r.left = std::min(r.left, (LONG)p.x); r.top = std::min(r.top, (LONG)p.y);
        r.right = std::max(r.right, (LONG)p.x); r.bottom = std::max(r.bottom, (LONG)p.y);
    }
    if (before) {   // the pointer stayed there until the next point
        r.left = std::min(r.left, (LONG)before->x); r.top = std::min(r.top, (LONG)before->y);
        r.right = std::max(r.right, (LONG)before->x); r.bottom = std::max(r.bottom, (LONG)before->y);
    }
    if (r.left == INT_MAX) return {0, 0, 0, 0};
    int pad = T_MonCursor() + 4;
    InflateRect(&r, pad, pad);
    return r;
}

// ---- every frame Windows composes, through Desktop Duplication ----

#define BUCKETS 64
struct Frame {
    double t;
    RECT box;        // the blur's pixels (non-white) in the watched strip; empty if none
    int cols[BUCKETS];   // non-white pixels per column bucket of the strip
};
static std::mutex g_frameLock;
static std::vector<Frame> g_frames;
static std::atomic<bool> g_recording, g_capturePaused, g_captureStop;
static std::atomic<int> g_shapeCount, g_shapeW;
static RECT g_strip;   // the watched strip of the monitor (screen coordinates)
static HANDLE g_captureReady;
static bool g_captureOk;

static DWORD WINAPI CaptureThread(void*) {
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HMONITOR mon = MonitorFromPoint(g_P, MONITOR_DEFAULTTONEAREST);
    IDXGIFactory1* f = nullptr;
    IDXGIAdapter1* adapter = nullptr;
    IDXGIOutput* out = nullptr;
    DXGI_OUTPUT_DESC od = {};
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&f))) {
        IDXGIAdapter1* a;
        for (UINT i = 0; !out && f->EnumAdapters1(i, &a) == S_OK; i++) {
            IDXGIOutput* o;
            for (UINT j = 0; a->EnumOutputs(j, &o) == S_OK; j++) {
                o->GetDesc(&od);
                if (od.Monitor == mon) { out = o; adapter = a; break; }
                o->Release();
            }
            if (!out) a->Release();
        }
    }
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGIOutput1* o1 = nullptr;
    IDXGIOutputDuplication* dup = nullptr;
    ID3D11Texture2D* staging = nullptr;
    int sw = g_strip.right - g_strip.left, sh = g_strip.bottom - g_strip.top;
    HRESULT hr = out ? D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev,
                                         nullptr, &ctx) : E_FAIL;
    if (SUCCEEDED(hr)) hr = out->QueryInterface(__uuidof(IDXGIOutput1), (void**)&o1);
    if (SUCCEEDED(hr)) hr = o1->DuplicateOutput(dev, &dup);
    if (SUCCEEDED(hr)) {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = sw; td.Height = sh; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        hr = dev->CreateTexture2D(&td, nullptr, &staging);
    }
    g_captureOk = SUCCEEDED(hr);
    if (!g_captureOk) wprintf(L"Desktop Duplication: output %ls, error %08lx\n", out ? L"found" : L"not found", (unsigned long)hr);
    SetEvent(g_captureReady);
    if (!g_captureOk) return 0;

    std::vector<BYTE> shape(64 * 1024);
    while (!g_captureStop) {
        if (g_capturePaused) { Sleep(20); continue; }
        DXGI_OUTDUPL_FRAME_INFO fi;
        IDXGIResource* res = nullptr;
        HRESULT hr = dup->AcquireNextFrame(50, &fi, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
        if (FAILED(hr)) { Sleep(50); continue; }
        if (fi.PointerShapeBufferSize) {
            if (fi.PointerShapeBufferSize > shape.size()) shape.resize(fi.PointerShapeBufferSize);
            UINT need;
            DXGI_OUTDUPL_POINTER_SHAPE_INFO si;
            if (SUCCEEDED(dup->GetFramePointerShape((UINT)shape.size(), shape.data(), &need, &si))) {
                g_shapeW = (int)si.Width;
                g_shapeCount++;
            }
        }
        if (fi.LastPresentTime.QuadPart && g_recording) {
            ID3D11Texture2D* tex = nullptr;
            if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex))) {
                D3D11_BOX b = {(UINT)(g_strip.left - od.DesktopCoordinates.left), (UINT)(g_strip.top - od.DesktopCoordinates.top), 0,
                               (UINT)(g_strip.right - od.DesktopCoordinates.left), (UINT)(g_strip.bottom - od.DesktopCoordinates.top), 1};
                ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, tex, 0, &b);
                D3D11_MAPPED_SUBRESOURCE map;
                if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &map))) {
                    Frame fr = {fi.LastPresentTime.QuadPart * 1000.0 / g_qf.QuadPart, {INT_MAX, INT_MAX, INT_MIN, INT_MIN}, {}};
                    for (int y = 0; y < sh; y++) {
                        const UINT32* row = (const UINT32*)((BYTE*)map.pData + y * map.RowPitch);
                        for (int x = 0; x < sw; x++)
                            if ((row[x] & 0xFFFFFF) != 0xFFFFFF) {
                                if (x < fr.box.left) fr.box.left = x;
                                if (x >= fr.box.right) fr.box.right = x + 1;
                                if (y < fr.box.top) fr.box.top = y;
                                if (y >= fr.box.bottom) fr.box.bottom = y + 1;
                                fr.cols[x * BUCKETS / sw]++;
                            }
                    }
                    ctx->Unmap(staging, 0);
                    if (fr.box.left == INT_MAX) fr.box = {0, 0, 0, 0};
                    else OffsetRect(&fr.box, g_strip.left, g_strip.top);
                    std::lock_guard<std::mutex> l(g_frameLock);
                    g_frames.push_back(fr);
                }
                tex->Release();
            }
        }
        res->Release();
        dup->ReleaseFrame();
    }
    return 0;
}

static void StartRecording() {
    std::lock_guard<std::mutex> l(g_frameLock);
    g_frames.clear();
    g_recording = true;
}

static std::vector<Frame> StopRecording() {
    Pump(60);   // the last frames
    g_recording = false;
    std::lock_guard<std::mutex> l(g_frameLock);
    return g_frames;
}

// Every frame's blur inside the area the pointer path allows: no copies left behind, none shown ahead of the pointer.
static bool FramesInside(const std::vector<Frame>& frames, int* frameCount, int* worstPx) {
    *frameCount = 0;
    *worstPx = 0;
    for (const auto& fr : frames) {
        if (IsRectEmpty(&fr.box)) continue;
        (*frameCount)++;
        RECT ok = Allowed(fr.t);
        int out = std::max({ok.left - fr.box.left, ok.top - fr.box.top, fr.box.right - ok.right, fr.box.bottom - ok.bottom, 0L});
        if (out > *worstPx) *worstPx = out;
    }
    return *worstPx == 0;
}

// Non-white pixels left on the white window once everything has faded (the taskbar's corner of the screen excluded).
static int LeftOnScreen() {
    RECT a = g_M;
    a.bottom -= (a.bottom - a.top) / 10;
    int w = a.right - a.left, h = a.bottom - a.top;
    HDC s = GetDC(nullptr), m = CreateCompatibleDC(s);
    BITMAPINFO bi = {};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB, 0, 0, 0, 0, 0};
    void* bits;
    HBITMAP b = CreateDIBSection(s, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ o = SelectObject(m, b);
    BitBlt(m, 0, 0, w, h, s, a.left, a.top, SRCCOPY | CAPTUREBLT);
    SelectObject(m, o);
    int n = 0;
    for (int i = 0; i < w * h; i++) n += (((UINT32*)bits)[i] & 0xFFFFFF) != 0xFFFFFF;
    static bool told;
    if (n && !told) { told = true; wprintf(L"(screen readback: first pixel %08x, middle %08x)\n",((UINT32*)bits)[0], ((UINT32*)bits)[w * h / 2]); }
    DeleteObject(b);
    DeleteDC(m);
    ReleaseDC(nullptr, s);
    return n;
}

// ---- processes ----

static HANDLE Start(const wchar_t* exe, const wchar_t* args) {
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" " + args;
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return nullptr;
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

static void Kill(HANDLE& p) {   // only processes this test started
    if (!p) return;
    TerminateProcess(p, 1);
    WaitForSingleObject(p, 3000);
    CloseHandle(p);
    p = nullptr;
}

static HWND WindowOf(const wchar_t* cls, DWORD pid) {   // a window of this class that the process owns
    DWORD owner;
    if (cls)
        for (HWND b = FindWindowExW(nullptr, nullptr, cls, nullptr); b; b = FindWindowExW(nullptr, b, cls, nullptr))
            if (GetWindowThreadProcessId(b, &owner) && owner == pid) return b;
    return nullptr;
}

static HWND SimBand(HANDLE sim) { return sim ? WindowOf(T_BandClass(), GetProcessId(sim)) : nullptr; }   // the stand-in's band window

// The window the blur is drawn in: explorer's (the stand-in's), or the app's own.
static HWND DrawWindow(HANDLE sim, DWORD pid) {
    return T_BandClass() ? SimBand(sim) : WindowOf(T_OverlayClass(), pid);
}

static double CpuMs() {
    FILETIME a, b, k, u;
    GetProcessTimes(GetCurrentProcess(), &a, &b, &k, &u);
    return (((ULONGLONG)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((ULONGLONG)u.dwHighDateTime << 32 | u.dwLowDateTime)) / 1e4;
}

static double PrivateMb() {
    PROCESS_MEMORY_COUNTERS_EX pm = {sizeof(pm)};
    GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm, sizeof(pm));
    return pm.PrivateUsage / 1048576.0;
}

// ---- the victim: draws until killed ----

static int Victim() {
    GetCursorPos(&g_P);
    g_fakeX = g_P.x;
    g_fakeY = g_P.y;
    g_fakeOn = 1;
    if (!T_Start()) return 1;
    for (;;) {
        Swipe({g_P.x - 400, g_P.y - 150}, {g_P.x + 400, g_P.y - 150}, 3000);
        Swipe({g_P.x + 400, g_P.y - 150}, {g_P.x - 400, g_P.y - 150}, 3000);
    }
}

// ---- the tests ----

static int Strip(int dy) { return std::clamp((int)g_P.y + dy, (int)g_strip.top + 80, (int)g_strip.bottom - 80); }

// A swipe recorded frame by frame: PASS if no frame shows blur outside what the path allows, and nothing is left after.
static void CheckedSwipe(const wchar_t* test, const wchar_t* what, POINT a, POINT b, double speed) {
    StartRecording();
    Swipe(a, b, speed);
    Pump(T_TrailMs() + 400);
    auto frames = StopRecording();
    int n, worst, left = LeftOnScreen();
    bool inside = FramesInside(frames, &n, &worst);
    if (!n) Result(test, L"FAIL", L"%ls: no blur reached the screen", what);
    else if (!inside || left) Result(test, L"FAIL", L"%ls: blur up to %d px outside the trail in %d frames, %d px left after the fade", what, worst, n, left);
    else Result(test, L"PASS", L"%ls: %d frames, blur always on the trail, screen clean after", what, n);
}

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    timeBeginPeriod(1);
    QueryPerformanceFrequency(&g_qf);
    if (argc > 1 && !wcscmp(argv[1], L"--victim")) return Victim();
    if (argc < 2) { wprintf(L"usage: harness.exe <bandsim.exe | ->\n"); return 2; }
    const wchar_t* simExe = T_BandClass() && wcscmp(argv[1], L"-") ? argv[1] : nullptr;
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);

    GetCursorPos(&g_P);
    HMONITOR mon = MonitorFromPoint(g_P, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    g_M = mi.rcMonitor;
    UINT dpi = 96, dpiY;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpi, &dpiY);
    int mw = g_M.right - g_M.left;
    g_strip = {g_M.left, std::max(g_M.top, g_P.y - 320), g_M.right, std::min(g_M.bottom, g_P.y + 320)};
    wprintf(L"monitor %ldx%ld at %u%%, pointer at (%ld,%ld)\n", mw, g_M.bottom - g_M.top, dpi * 100 / 96, g_P.x, g_P.y);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WhiteProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PAMBTestWhite";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    RegisterClassW(&wc);
    g_W = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, wc.lpszClassName, L"", WS_POPUP | WS_VISIBLE,
                          g_M.left, g_M.top, mw, g_M.bottom - g_M.top, nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_W) { Result(L"setup", L"FAIL", L"could not create the white window (%lu)", GetLastError()); return 3; }
    Pump(400);
    // Locked, asleep or behind a UAC prompt, the desktop reads back black and can't be duplicated: nothing to test.
    if (int n = LeftOnScreen()) {
        Result(L"setup", L"FAIL", L"the screen doesn't show the white window (%d px differ): is the PC locked or the display off?", n);
        return 3;
    }

    g_captureReady = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE capture = CreateThread(nullptr, 0, CaptureThread, nullptr, 0, nullptr);
    WaitForSingleObject(g_captureReady, 5000);
    if (!g_captureOk) { Result(L"setup", L"FAIL", L"Desktop Duplication not available (PC locked?)"); return 3; }

    HANDLE sim = nullptr;
    if (T_BandClass()) {
        sim = simExe ? Start(simExe, L"") : nullptr;
        Pump(600);
        if (!sim || !SimBand(sim)) Result(L"setup", L"FAIL", L"the explorer stand-in has no window");
    }

    // 8b. The process killed while drawing: explorer's window hides (the mod), nothing is left on screen.
    {
        HANDLE victim = Start(self, L"--victim");
        bool shown = false;
        for (int i = 0; i < 40 && !shown; i++) {
            Pump(50);
            HWND b = DrawWindow(sim, GetProcessId(victim));
            shown = b && IsWindowVisible(b);
        }
        Kill(victim);
        Pump(500);
        HWND b = SimBand(sim);
        int left = LeftOnScreen();
        if (!shown) Result(L"crash", L"FAIL", L"the killed copy never drew");
        else if (b && IsWindowVisible(b)) Result(L"crash", L"FAIL", L"explorer's window stayed on screen after the process was killed");
        else if (left) Result(L"crash", L"FAIL", L"%d px of blur left on screen after the kill", left);
        else Result(L"crash", L"PASS", L"killed while drawing: nothing left on screen");
    }

    // The tests' own copy, following the made-up pointer.
    g_fakeX = g_P.x;
    g_fakeY = g_P.y;
    g_fakeOn = 1;
    if (!T_Start()) { Result(L"setup", L"FAIL", L"could not start"); return 1; }
    Pump(500);
    double baseMem = PrivateMb();

    // 3d + 5. First drawing after the killed copy (its last frame is still in the window), ending on the pointer; then
    // the blur's size against the pointer Windows really draws.
    CheckedSwipe(L"stale", L"first stretch after a killed copy", {g_P.x - 500, Strip(-100)}, {g_P.x, g_P.y}, 3000);
    {
        int mine = T_MonCursor();
        int before = g_shapeCount;
        for (int i = 0; i < 6 && g_shapeCount == before; i++) {   // a shape change makes Desktop Duplication send the shape
            SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
            Pump(80);
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            Pump(120);
        }
        Pump(200);
        if (g_shapeCount == before) Result(L"size", L"SKIP", L"Windows sent no pointer shape");
        else if (g_shapeW == mine) Result(L"size", L"PASS", L"blur %d px = pointer Windows draws %d px", mine, g_shapeW.load());
        else Result(L"size", L"FAIL", L"blur %d px, but Windows draws the pointer at %d px", mine, g_shapeW.load());
    }

    // 2. Office flicker: the blur runs onto the still pointer and stops there, five times. The window under the pointer
    // may hear of the blur appearing and going away, and nothing else.
    {
        POINT starts[] = {{g_P.x - 500, g_P.y}, {g_P.x + 500, g_P.y}, {g_P.x - 400, Strip(-180)}, {g_P.x + 400, Strip(180)},
                          {g_P.x - 300, Strip(120)}};
        int worst = 0, total = 0;
        bool moved = g_realMoved;
        for (const POINT& s : starts) {
            MoveTo(s.x, s.y);
            QuietWait(400);   // the jump to the start draws nothing
            int m0 = g_mm, c0 = g_sc;
            Swipe(s, g_P, 3000);
            Pump(T_TrailMs() + 500);
            int n = std::max(g_mm - m0, g_sc - c0);
            total += n;
            worst = std::max(worst, n);
        }
        if (g_realMoved && !moved) Result(L"flicker", L"SKIP", L"the real mouse moved during the test");
        else if (worst <= 3) Result(L"flicker", L"PASS", L"at most %d mouse messages per stretch (%d in 5)", worst, total);
        else Result(L"flicker", L"FAIL", L"up to %d mouse messages per stretch (%d in 5) under a still pointer", worst, total);
    }

    // 1. Cursor Wrap: the pointer jumps from the right edge to the left one in the middle of a swipe.
    {
        int y = Strip(-140);
        g_maxStep = 0;
        MoveTo(g_M.left + mw * 6 / 10, y);
        QuietWait(400);
        g_maxStep = 0;
        StartRecording();
        Swipe({g_M.left + mw * 6 / 10, y}, {g_M.right - 30, y}, 5000);
        double jumpAt = Ms();
        Swipe({g_M.left + 30, y}, {g_M.left + mw * 2 / 10, y}, 5000);
        Pump(T_TrailMs() + 400);
        auto frames = StopRecording();
        int smeared = 0;
        for (const auto& fr : frames) {   // blur across the middle of the screen, which the pointer never crossed
            if (fr.t < jumpAt) continue;
            int mid = 0;
            for (int i = BUCKETS * 35 / 100; i < BUCKETS / 2; i++) mid += fr.cols[i];
            if (mid) smeared++;
        }
        if (frames.empty()) Result(L"jump", L"FAIL", L"no frames recorded");
        else if (smeared || g_maxStep * 5 > mw * 2)
            Result(L"jump", L"FAIL", L"trail drawn across the jump in %d frames (largest step in the trail %d px)", smeared, g_maxStep);
        else Result(L"jump", L"PASS", L"no frame drew across the jump (largest step in the trail %d px)", g_maxStep);
    }

    // 4 + 3a. Long fast swipes across the monitor: the window has to move along; no frame may show the blur anywhere
    // but on the trail (a copy shown ahead of the pointer for a frame, or one left behind, fails).
    {
        int y1 = Strip(-60), y2 = Strip(60);
        MoveTo(g_M.left + 40, y1);
        QuietWait(400);
        CheckedSwipe(L"swipe", L"left to right", {g_M.left + 40, y1}, {g_M.right - 40, y1}, 6000);
        CheckedSwipe(L"swipe", L"right to left", {g_M.right - 40, y1}, {g_M.left + 40, y2}, 6000);
        CheckedSwipe(L"swipe", L"slow, left to right", {g_M.left + 40, y2}, {g_M.right - 40, y1}, 2500);
        // 30 more back-and-forth swipes (MODTEST_SWIPES=n for another number), one result for all: a copy shown ahead of
        // the pointer for a frame happens only now and then (in about 1 swipe in 8 without its fix)
        const char* e = getenv("MODTEST_SWIPES");
        if (int extra = e ? atoi(e) : 30) {
            int bad = 0, frames = 0, worstAll = 0;
            for (int i = 0; i < extra; i++) {
                POINT a = {g_M.left + 40, i % 2 ? y1 : y2}, b = {g_M.right - 40, i % 2 ? y2 : y1};
                if (i % 2) std::swap(a, b);
                StartRecording();
                Swipe(a, b, 4000 + 1000 * (i % 4));
                Pump(T_TrailMs() + 150);
                auto fr = StopRecording();
                int n, worst;
                if (!FramesInside(fr, &n, &worst)) bad++;
                frames += n;
                worstAll = std::max(worstAll, worst);
            }
            if (bad) Result(L"swipe", L"FAIL", L"%d more swipes: %d had blur outside the trail (up to %d px), %d frames", extra, bad, worstAll, frames);
            else Result(L"swipe", L"PASS", L"%d more swipes, %d frames: blur always on the trail", extra, frames);
        }
    }

    // 6. A new pointer size: picked up when the mouse moves after a pause, and never looked for while it is still.
    {
        int sm = GetSystemMetricsForDpi(SM_CXCURSOR, dpi);
        g_fakeBase = 32;
        QuietWait(1200);
        Swipe({g_P.x - 300, g_P.y}, {g_P.x, g_P.y}, 3000);
        Pump(150);
        int before = T_MonCursor();
        g_fakeBase = 48;
        int r0 = g_baseReads;
        QuietWait(1500);
        int stillReads = g_baseReads - r0;
        Swipe({g_P.x - 300, g_P.y}, {g_P.x, g_P.y}, 3000);
        Pump(150);
        int after = T_MonCursor(), want = MulDiv(48, sm, 32);
        if (after != want) Result(L"resize", L"FAIL", L"pointer size changed 1 -> 2: blur stays %d px (%d before), should be %d", after, before, want);
        else if (stillReads) Result(L"resize", L"FAIL", L"picked up (%d px), but the size was read %d times while still", after, stillReads);
        else Result(L"resize", L"PASS", L"pointer size 1 -> 2 picked up on the next movement: %d -> %d px", before, after);
        g_fakeBase = 0;
        QuietWait(1200);
        Swipe({g_P.x - 300, g_P.y}, {g_P.x, g_P.y}, 3000);
        Pump(T_TrailMs() + 400);
    }

    // 7. Idle: no CPU, no looking at the cursor, memory back to where it started.
    {
        g_capturePaused = true;
        QuietWait(3000);   // the trail fades; after 1.5 s the mod gives back its drawing memory
        int r0 = g_fakeReads;
        double c0 = CpuMs();
        QuietWait(5000);
        double cpu = CpuMs() - c0;
        int reads = g_fakeReads - r0;
        double mem = PrivateMb();
        g_capturePaused = false;
        if (reads || cpu > 20) Result(L"idle", L"FAIL", L"still pointer, 5 s: %.0f ms CPU, %d cursor reads", cpu, reads);
        else Result(L"idle", L"PASS", L"still pointer, 5 s: %.0f ms CPU, 0 cursor reads", cpu);
        if (mem > baseMem + 1.0) Result(L"memory", L"FAIL", L"private memory %.1f MB after idle, %.1f MB at start", mem, baseMem);
        else Result(L"memory", L"PASS", L"private memory %.1f MB after idle, %.1f MB at start", mem, baseMem);
        RECT r = {};
        if (HWND b = DrawWindow(sim, GetCurrentProcessId())) GetWindowRect(b, &r);
        Result(L"idle-window", L"INFO", L"overlay window while idle: %ldx%ld", r.right - r.left, r.bottom - r.top);
    }

    // 3b. Drawing again after the idle trim.
    CheckedSwipe(L"stale", L"after the idle trim", {g_P.x + 500, Strip(-80)}, {g_P.x - 300, Strip(40)}, 4000);

    // 3c. Explorer restarted: its window is a new one.
    if (sim) {
        Kill(sim);
        sim = Start(simExe, L"");
        Pump(700);
        bool shown = false;
        StartRecording();
        Swipe({g_P.x - 500, Strip(60)}, {g_P.x + 300, Strip(-60)}, 4000);
        if (HWND b = SimBand(sim)) shown = IsWindowVisible(b);
        Pump(T_TrailMs() + 400);
        auto frames = StopRecording();
        int n, worst, left = LeftOnScreen();
        bool inside = FramesInside(frames, &n, &worst);
        if (!shown) Result(L"stale", L"FAIL", L"after explorer restarted: the mod did not draw in its new window");
        else if (!n || !inside || left) Result(L"stale", L"FAIL", L"after explorer restarted: %d frames, up to %d px outside the trail, %d px left", n, worst, left);
        else Result(L"stale", L"PASS", L"after explorer restarted: drew in its new window, %d frames, screen clean after", n);
    }

    // 8a. It stops: the window hides (and gives back its memory).
    {
        T_Stop();
        g_fakeOn = 0;
        Pump(400);
        HWND b = DrawWindow(sim, GetCurrentProcessId());
        RECT r = {};
        if (b) GetWindowRect(b, &r);
        if (b && IsWindowVisible(b)) Result(L"exit", L"FAIL", L"the overlay window is still shown after stopping");
        else if (int left = LeftOnScreen()) Result(L"exit", L"FAIL", L"%d px of blur left on screen after stopping", left);
        else Result(L"exit", L"PASS", L"overlay window hidden after stopping (%ldx%ld)", r.right - r.left, r.bottom - r.top);
        if (T_HiddenFlag()) Result(L"exit", L"FAIL", L"the cursors-hidden flag was left set");
    }

    if (g_realMoved) wprintf(L"note: the real mouse moved during the run, so message counts may be off\n");
    g_captureStop = true;
    WaitForSingleObject(capture, 2000);
    Kill(sim);
    DestroyWindow(g_W);
    return g_fails ? 1 : 0;
}
