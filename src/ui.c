// The settings window, and the manual update check.
//
// Every change applies at once, so it can be tuned by moving the mouse. The window is laid out by hand: sizes are "design
// pixels at 96 DPI" times the scale of the monitor it is on, and it is laid out again when Windows reports a new DPI (the
// window was dragged to a monitor with other scaling), so text is never clipped.
#include "app.h"
#include <commctrl.h>
#include <shellapi.h>
#include <winhttp.h>
#include <stdio.h>
#include <wchar.h>

enum { ID_ENABLED = 100, ID_LHOT, ID_HOTKEY, ID_LSTRENGTH, ID_STRENGTH, ID_LTRAIL, ID_TRAIL, ID_HIDE, ID_LSPEED, ID_SPEED,
       ID_PAUSE, ID_STARTUP, ID_LUPDATE, ID_UPDATE, ID_RESET, ID_CLOSE };
#define WM_UPDATE_DONE (WM_APP + 10)
#define HOTKEY_ID      1   // as registered by main.c

static HWND win;
static HFONT font;
static int dpi = 96;
static BOOL capturing;        // waiting for the new shortcut to be pressed
static UINT swallowUp;        // the key that finished picking a shortcut: ignore its release (Space would click the button again)
static WCHAR updMsg[80], updUrl[256];
static volatile BOOL checking;

static int P(int v) { return MulDiv(v, dpi, 96); }
static HWND Item(int id) { return GetDlgItem(win, id); }
static BOOL Checked(int id) { return SendMessageW(Item(id), BM_GETCHECK, 0, 0) == BST_CHECKED; }
static void Check(int id, BOOL on) { SendMessageW(Item(id), BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0); }

static void ShowHotkey(void)
{
    WCHAR t[64];
    HotkeyText(g_set.hotMods, g_set.hotKey, t, 64);
    SetDlgItemTextW(win, ID_HOTKEY, t);
}

static void ShowLabels(void)
{
    WCHAR t[64];
    swprintf(t, 64, L"Trail opacity: %d%%", g_set.strength);                    SetDlgItemTextW(win, ID_LSTRENGTH, t);
    swprintf(t, 64, L"Trail length: %d ms", g_set.trailMs);                     SetDlgItemTextW(win, ID_LTRAIL, t);
    swprintf(t, 64, L"Hide above speed: %d cm/s on the main screen", g_set.hideSpeedCm); SetDlgItemTextW(win, ID_LSPEED, t);
}

static void ShowUpdate(void)
{
    SetDlgItemTextW(win, ID_LUPDATE, updMsg[0] ? updMsg : L"Version " APP_VERSION);
    SetDlgItemTextW(win, ID_UPDATE, updUrl[0] ? L"Download" : L"Check for updates");
    EnableWindow(Item(ID_UPDATE), !checking);
}

static void LoadValues(void)   // setting a checkbox or slider in code sends no change notification, so nothing is saved back here
{
    Check(ID_ENABLED, g_set.enabled);
    Check(ID_HIDE, g_set.hideWhenFast);
    Check(ID_PAUSE, g_set.pauseFullscreen);
    Check(ID_STARTUP, Startup_Get());
    SendMessageW(Item(ID_STRENGTH), TBM_SETPOS, TRUE, g_set.strength);
    SendMessageW(Item(ID_TRAIL), TBM_SETPOS, TRUE, g_set.trailMs);
    SendMessageW(Item(ID_SPEED), TBM_SETPOS, TRUE, g_set.hideSpeedCm);
    EnableWindow(Item(ID_SPEED), g_set.hideWhenFast);
    ShowHotkey();
    ShowLabels();
}

void UI_Refresh(void) { if (win) LoadValues(); }
BOOL UI_IsDialogMessage(MSG *m)
{
    if (!win) return FALSE;
    HWND f = GetFocus();
    WCHAR cls[8] = L"";
    if (f) GetClassNameW(f, cls, 8);
    if (m->message == WM_KEYDOWN && m->wParam == VK_RETURN && !capturing && f && IsChild(win, f) && !lstrcmpiW(cls, L"Button")
        && (GetWindowLongW(f, GWL_STYLE) & BS_TYPEMASK) <= BS_DEFPUSHBUTTON && GetDlgCtrlID(f) != ID_HOTKEY)
    {
        SendMessageW(f, BM_CLICK, 0, 0);   // Enter presses the focused button (otherwise it would be Close)
        return TRUE;
    }
    return IsDialogMessageW(win, m);
}

// ---- update check: the releases page redirects to the newest version's tag (no API, so no rate limit) ----

static DWORD WINAPI UpdateThread(void *unused)
{
    (void)unused;
    WCHAR loc[256] = L"";
    DWORD status = 0, len;
    HINTERNET s = WinHttpOpen(APP_NAME L"/" APP_VERSION, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, NULL, NULL, 0), c = NULL, r = NULL;
    if (s) { WinHttpSetTimeouts(s, 8000, 8000, 8000, 8000); c = WinHttpConnect(s, L"github.com", INTERNET_DEFAULT_HTTPS_PORT, 0); }
    if (c) r = WinHttpOpenRequest(c, L"HEAD", L"/" REPO L"/releases/latest", NULL, NULL, NULL, WINHTTP_FLAG_SECURE);
    if (r) { DWORD off = WINHTTP_DISABLE_REDIRECTS; WinHttpSetOption(r, WINHTTP_OPTION_DISABLE_FEATURE, &off, sizeof off); }
    BOOL ok = r && WinHttpSendRequest(r, NULL, 0, NULL, 0, 0, 0) && WinHttpReceiveResponse(r, NULL);
    if (ok)
    {
        len = sizeof status; WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &status, &len, NULL);
        len = sizeof loc;    WinHttpQueryHeaders(r, WINHTTP_QUERY_LOCATION, NULL, loc, &len, NULL);
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);

    const WCHAR *tag = wcsstr(loc, L"/tag/v");
    int a = 0, b = 0, d = 0, x = 0, y = 0, z = 0;
    updUrl[0] = 0;
    if (!ok) wcscpy(updMsg, L"Can't check (offline?)");
    else if (status == 403 || status == 429) wcscpy(updMsg, L"GitHub is busy, try later");
    else if (!tag || swscanf(tag + 6, L"%d.%d.%d", &a, &b, &d) < 2) wcscpy(updMsg, L"Couldn't read version");
    else
    {
        swscanf(APP_VERSION, L"%d.%d.%d", &x, &y, &z);
        if (a != x ? a > x : b != y ? b > y : d > z) { swprintf(updMsg, 80, L"v%.20s is available", tag + 6); wcscpy(updUrl, loc); }
        else wcscpy(updMsg, L"Up to date (v" APP_VERSION L")");
    }
    checking = FALSE;
    if (win) PostMessageW(win, WM_UPDATE_DONE, 0, 0);
    return 0;
}

// ---- hotkey picking: click the button, then press the new keys (Esc cancels) ----

static void Capture(UINT vk)
{
    if (vk == VK_ESCAPE) { capturing = FALSE; App_ApplyHotkey(); ShowHotkey(); return; }
    if (vk == VK_CONTROL || vk == VK_MENU || vk == VK_SHIFT || vk == VK_LWIN || vk == VK_RWIN) return;   // wait for the real key
    UINT mods = (GetKeyState(VK_CONTROL) < 0 ? MOD_CONTROL : 0) | (GetKeyState(VK_MENU) < 0 ? MOD_ALT : 0) | (GetKeyState(VK_SHIFT) < 0 ? MOD_SHIFT : 0);
    if (!mods) return;   // without Ctrl, Alt or Shift it would fire while typing in any program
    UINT oldMods = g_set.hotMods, oldKey = g_set.hotKey;
    g_set.hotMods = mods; g_set.hotKey = vk;
    capturing = FALSE; swallowUp = vk;
    if (!App_ApplyHotkey())
    {
        g_set.hotMods = oldMods; g_set.hotKey = oldKey; App_ApplyHotkey();   // keep the old one working
        ShowHotkey();
        swallowUp = 0;
        MessageBoxW(win, L"That shortcut is already used by another program. Please pick a different one.", APP_TITLE, MB_OK | MB_ICONINFORMATION);
        return;
    }
    Settings_Save();
    ShowHotkey();
}

static LRESULT CALLBACK HotkeyProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data)
{
    (void)id; (void)data;
    if (swallowUp)   // the keystroke that finished picking: let none of it through (Space would click the button again)
        switch (msg)
        {
        case WM_GETDLGCODE: return DLGC_WANTALLKEYS;
        case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_CHAR: case WM_SYSCHAR: return 0;
        case WM_KEYUP: case WM_SYSKEYUP: if (wp == swallowUp) swallowUp = 0; return 0;
        case WM_KILLFOCUS: swallowUp = 0; break;
        }
    if (capturing)
        switch (msg)
        {
        case WM_GETDLGCODE: return DLGC_WANTALLKEYS;
        case WM_KEYDOWN: case WM_SYSKEYDOWN: Capture((UINT)wp); return 0;
        case WM_KEYUP: case WM_SYSKEYUP: case WM_CHAR: case WM_SYSCHAR: return 0;
        case WM_KILLFOCUS: capturing = FALSE; App_ApplyHotkey(); ShowHotkey(); break;
        }
    return DefSubclassProc(h, msg, wp, lp);
}

// ---- layout ----

static int TextW(const WCHAR *s)
{
    HDC dc = GetDC(win);
    HGDIOBJ old = SelectObject(dc, font);
    SIZE z; GetTextExtentPoint32W(dc, s, (int)wcslen(s), &z);
    SelectObject(dc, old);
    ReleaseDC(win, dc);
    return z.cx;
}

static void Place(int id, int x, int y, int w, int h) { SetWindowPos(Item(id), NULL, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE); }

static BOOL CALLBACK SetFontProc(HWND c, LPARAM f) { SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE); return TRUE; }

// Lays the window out for a DPI; returns the outer window size.
static SIZE Layout(int newDpi)
{
    dpi = newDpi;
    HFONT old = font;
    font = CreateFontW(-P(12), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");   // 9 pt at 96 DPI
    EnumChildWindows(win, SetFontProc, (LPARAM)font);
    if (old) DeleteObject(old);

    int m = P(12), W = P(300), y = m, row = P(22), btnH = P(26), lineH = P(18), barH = P(30);
    int updW = max(TextW(L"Check for updates"), TextW(L"Download")) + P(24);
    int resetW = TextW(L"Reset to defaults") + P(24), closeW = TextW(L"Close") + P(44);
#define CHECKBOX(id)        Place(id, m, y, W, row); y += row + P(4)
#define SLIDER(label, bar)  Place(label, m, y, W, lineH); y += lineH + P(2); Place(bar, m - P(4), y, W + P(8), barH); y += barH + P(4)
    CHECKBOX(ID_ENABLED);
    int hotW = W - TextW(L"Toggle hotkey") - P(16);
    Place(ID_LHOT, m, y + (btnH - lineH) / 2, W - hotW - P(4), lineH);
    Place(ID_HOTKEY, m + W - hotW, y, hotW, btnH);
    y += btnH + P(8);
    SLIDER(ID_LSTRENGTH, ID_STRENGTH);
    SLIDER(ID_LTRAIL, ID_TRAIL);
    CHECKBOX(ID_HIDE);
    SLIDER(ID_LSPEED, ID_SPEED);
    CHECKBOX(ID_PAUSE);
    CHECKBOX(ID_STARTUP);
    y += P(8);
    Place(ID_LUPDATE, m, y + (btnH - lineH) / 2, W - updW - P(8), lineH);
    Place(ID_UPDATE, m + W - updW, y, updW, btnH);
    y += btnH + P(12);
    Place(ID_RESET, m, y, resetW, btnH);
    Place(ID_CLOSE, m + W - closeW, y, closeW, btnH);
    y += btnH + m;
#undef CHECKBOX
#undef SLIDER
    RECT r = { 0, 0, W + 2 * m, y };
    AdjustWindowRectExForDpi(&r, GetWindowLongW(win, GWL_STYLE), FALSE, GetWindowLongW(win, GWL_EXSTYLE), dpi);
    SIZE s = { r.right - r.left, r.bottom - r.top };
    return s;
}

// ---- the window ----

static void Command(int id)
{
    switch (id)
    {
    case ID_ENABLED: App_SetEnabled(Checked(ID_ENABLED)); break;
    case ID_HIDE:    g_set.hideWhenFast = Checked(ID_HIDE); EnableWindow(Item(ID_SPEED), g_set.hideWhenFast); Settings_Save(); break;
    case ID_PAUSE:   g_set.pauseFullscreen = Checked(ID_PAUSE); Settings_Save(); break;
    case ID_STARTUP: Startup_Set(Checked(ID_STARTUP)); break;
    case ID_HOTKEY:
        capturing = TRUE;
        UnregisterHotKey(g_main, HOTKEY_ID);   // else pressing the current shortcut would toggle the blur instead of reaching us
        SetDlgItemTextW(win, ID_HOTKEY, L"Press new keys...");
        SetFocus(Item(ID_HOTKEY));
        break;
    case ID_UPDATE:
        if (updUrl[0]) ShellExecuteW(NULL, L"open", updUrl, NULL, NULL, SW_SHOWNORMAL);
        else if (!checking)
        {
            checking = TRUE;
            wcscpy(updMsg, L"Checking...");
            ShowUpdate();
            HANDLE t = CreateThread(NULL, 0, UpdateThread, NULL, 0, NULL);
            if (t) CloseHandle(t);
        }
        break;
    case ID_RESET: Settings_Reset(); LoadValues(); break;
    case ID_CLOSE: case IDCANCEL: DestroyWindow(win); break;
    }
}

static LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_COMMAND: if (HIWORD(wp) == BN_CLICKED) Command(LOWORD(wp)); return 0;
    case WM_HSCROLL:   // a slider moved
    {
        int v = (int)SendMessageW((HWND)lp, TBM_GETPOS, 0, 0), id = GetDlgCtrlID((HWND)lp);
        if (id == ID_STRENGTH) g_set.strength = v;
        if (id == ID_TRAIL) g_set.trailMs = v;
        if (id == ID_SPEED) g_set.hideSpeedCm = v;
        ShowLabels();
        if (LOWORD(wp) != TB_THUMBTRACK) Settings_Save();   // while dragging, save once at the end
        return 0;
    }
    case WM_DPICHANGED:   // dragged to a monitor with other scaling: lay out again at the new size, where Windows suggests
    {
        RECT *r = (RECT *)lp;
        SIZE s = Layout(HIWORD(wp));
        SetWindowPos(h, NULL, r->left, r->top, s.cx, s.cy, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case DM_GETDEFID: return MAKELONG(ID_CLOSE, DC_HASDEFID);   // Enter = Close
    case WM_ACTIVATE:   // remember which control had the keyboard, and give it back
    {
        static HWND saved;
        if (LOWORD(wp) == WA_INACTIVE) { HWND f = GetFocus(); if (f && IsChild(h, f)) saved = f; }
        else SetFocus(saved && IsChild(h, saved) && IsWindowEnabled(saved) ? saved : Item(ID_ENABLED));
        return 0;
    }
    case WM_UPDATE_DONE: ShowUpdate(); return 0;
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY:
        if (capturing) { capturing = FALSE; App_ApplyHotkey(); }
        win = NULL;
        DeleteObject(font); font = NULL;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void Child(const WCHAR *cls, const WCHAR *text, DWORD style, int id)
{
    CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, win, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
}

static void Slider(int label, int id, int lo, int hi, int tick)
{
    Child(L"STATIC", L"", SS_LEFT, label);
    Child(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS | WS_TABSTOP, id);
    SendMessageW(Item(id), TBM_SETRANGEMIN, FALSE, lo);
    SendMessageW(Item(id), TBM_SETRANGEMAX, FALSE, hi);
    SendMessageW(Item(id), TBM_SETTICFREQ, tick, 0);
    SendMessageW(Item(id), TBM_SETLINESIZE, 0, tick / 5 > 1 ? tick / 5 : 1);
    SendMessageW(Item(id), TBM_SETPAGESIZE, 0, tick);
}

void UI_Show(void)
{
    if (win) { LoadValues(); ShowWindow(win, SW_SHOWNORMAL); SetForegroundWindow(win); return; }
    HINSTANCE inst = GetModuleHandleW(NULL);
    static BOOL registered;
    if (!registered)
    {
        INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_BAR_CLASSES };
        InitCommonControlsEx(&icc);
        WNDCLASSW wc = { 0 };
        wc.lpfnWndProc = Proc;
        wc.hInstance = inst;
        wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"CursorMotionBlurSettings";
        registered = RegisterClassW(&wc) != 0;
    }
    // open on the monitor the mouse is on (created there, so it starts at that monitor's DPI), centred
    POINT p; GetCursorPos(&p);
    MONITORINFO mi = { sizeof mi };
    GetMonitorInfoW(MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST), &mi);
    win = CreateWindowExW(0, L"CursorMotionBlurSettings", APP_TITLE, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                          mi.rcWork.left, mi.rcWork.top, 100, 100, NULL, NULL, inst, NULL);
    if (!win) return;

    Child(L"BUTTON", L"Enable motion blur", BS_AUTOCHECKBOX | WS_TABSTOP, ID_ENABLED);
    Child(L"STATIC", L"Toggle hotkey", SS_LEFT, ID_LHOT);
    Child(L"BUTTON", L"", BS_PUSHBUTTON | WS_TABSTOP, ID_HOTKEY);
    SetWindowSubclass(Item(ID_HOTKEY), HotkeyProc, 1, 0);
    Slider(ID_LSTRENGTH, ID_STRENGTH, 1, 100, 10);
    Slider(ID_LTRAIL, ID_TRAIL, 10, 150, 10);
    Child(L"BUTTON", L"Hide the real cursor when moving very fast", BS_AUTOCHECKBOX | WS_TABSTOP, ID_HIDE);
    Slider(ID_LSPEED, ID_SPEED, 20, 300, 20);
    Child(L"BUTTON", L"Pause in fullscreen apps and games", BS_AUTOCHECKBOX | WS_TABSTOP, ID_PAUSE);
    Child(L"BUTTON", L"Launch CursorMotionBlur when Windows starts", BS_AUTOCHECKBOX | WS_TABSTOP, ID_STARTUP);
    Child(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, ID_LUPDATE);
    Child(L"BUTTON", L"", BS_PUSHBUTTON | WS_TABSTOP, ID_UPDATE);
    Child(L"BUTTON", L"Reset to defaults", BS_PUSHBUTTON | WS_TABSTOP, ID_RESET);
    Child(L"BUTTON", L"Close", BS_DEFPUSHBUTTON | WS_TABSTOP, ID_CLOSE);
    LoadValues();
    ShowUpdate();

    SIZE s = Layout(GetDpiForWindow(win));
    int x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - s.cx) / 2, y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - s.cy) / 2;
    SetWindowPos(win, NULL, x, y, s.cx, s.cy, SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(win, SW_SHOW);
    SetForegroundWindow(win);
}
