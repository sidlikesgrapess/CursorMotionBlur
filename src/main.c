// Start-up, tray icon, global hotkey, the settings file and "launch when Windows starts".
#include "app.h"
#include <shellapi.h>
#include <shlobj.h>
#include <commctrl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define WM_TRAY   (WM_APP + 1)
#define HOTKEY_ID 1

static const Settings defaults = { TRUE, 92, 30, TRUE, 40, TRUE, MOD_CONTROL | MOD_ALT, 'B' };
Settings g_set;
HWND g_main;
volatile BOOL g_rawInput;
static BOOL paused;   // a fullscreen app or game is in front
static NOTIFYICONDATAW nid;
static UINT msgTaskbarCreated;
static const WCHAR runKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

// ---- settings file: %APPDATA%\CursorMotionBlur\settings.ini, Key=Value lines (same file as the earlier .NET versions) ----

static void SettingsPath(WCHAR *p)   // p: MAX_PATH + 40
{
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, p))) p[0] = 0;
    wcscat(p, L"\\" APP_NAME);
    CreateDirectoryW(p, NULL);
    wcscat(p, L"\\settings.ini");
}

static int Clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static BOOL Settings_Load(void)   // FALSE if there is no file yet
{
    WCHAR path[MAX_PATH + 40]; SettingsPath(path);
    FILE *f = _wfopen(path, L"r");
    if (!f) return FALSE;
    char line[128];
    int mods = 0, key = 0;
    while (fgets(line, sizeof line, f))
    {
        char *v = strchr(line, '=');
        if (!v) continue;
        *v++ = 0;
        BOOL b = _strnicmp(v, "true", 4) == 0;
        int n = atoi(v);
        if      (!strcmp(line, "Enabled"))           g_set.enabled = b;
        else if (!strcmp(line, "Strength"))          g_set.strength = Clamp(n, 1, 100);
        else if (!strcmp(line, "TrailMs"))           g_set.trailMs = Clamp(n, 10, 150);
        else if (!strcmp(line, "HideWhenFast"))      g_set.hideWhenFast = b;
        else if (!strcmp(line, "HideSpeedCm"))       g_set.hideSpeedCm = Clamp(n, 20, 300);
        else if (!strcmp(line, "PauseInFullscreen")) g_set.pauseFullscreen = b;
        else if (!strcmp(line, "HotkeyMods"))        mods = n;
        else if (!strcmp(line, "HotkeyKey"))         key = n;
    }
    fclose(f);
    if ((mods & 7) && key > 0) { g_set.hotMods = mods & 7; g_set.hotKey = key; }
    return TRUE;
}

void Settings_Save(void)
{
    WCHAR path[MAX_PATH + 40]; SettingsPath(path);
    FILE *f = _wfopen(path, L"w");
    if (!f) return;
#define B(x) ((x) ? "True" : "False")
    fprintf(f, "Enabled=%s\nStrength=%d\nTrailMs=%d\nHideWhenFast=%s\nHideSpeedCm=%d\nPauseInFullscreen=%s\nHotkeyMods=%u\nHotkeyKey=%u\n",
            B(g_set.enabled), g_set.strength, g_set.trailMs, B(g_set.hideWhenFast), g_set.hideSpeedCm, B(g_set.pauseFullscreen),
            g_set.hotMods, g_set.hotKey);
#undef B
    fclose(f);
}

void Settings_Reset(void)   // everything except on/off
{
    BOOL on = g_set.enabled;
    g_set = defaults;
    g_set.enabled = on;
    App_ApplyHotkey();
    Settings_Save();
}

BOOL Startup_Get(void)
{
    return RegGetValueW(HKEY_CURRENT_USER, runKey, APP_NAME, RRF_RT_REG_SZ, NULL, NULL, NULL) == ERROR_SUCCESS;
}

void Startup_Set(BOOL on)
{
    if (!on) { RegDeleteKeyValueW(HKEY_CURRENT_USER, runKey, APP_NAME); return; }
    WCHAR cmd[MAX_PATH + 2] = L"\"";
    GetModuleFileNameW(NULL, cmd + 1, MAX_PATH);
    wcscat(cmd, L"\"");
    RegSetKeyValueW(HKEY_CURRENT_USER, runKey, APP_NAME, REG_SZ, cmd, (DWORD)(wcslen(cmd) + 1) * sizeof(WCHAR));
}

void Log(const char *fmt, ...)
{
    static int on = -1;
    if (on < 0) { char v[4]; on = GetEnvironmentVariableA("CMB_DEBUG", v, sizeof v) == 1 && v[0] == '1'; }
    if (!on) return;
    char path[MAX_PATH];
    GetTempPathA(MAX_PATH - 24, path);
    strcat(path, "CursorMotionBlur.log");
    FILE *f = fopen(path, "a");
    if (!f) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(f, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a; va_start(a, fmt); vfprintf(f, fmt, a); va_end(a);
    fputc('\n', f);
    fclose(f);
}

// ---- hotkey, tray, on/off ----

void HotkeyText(UINT mods, UINT vk, WCHAR *out, int n)
{
    WCHAR name[32];
    UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC_EX);
    LONG lp = (sc & 0xFF) << 16;
    if ((sc >> 8) == 0xE0 || (vk >= VK_PRIOR && vk <= VK_DOWN) || vk == VK_INSERT || vk == VK_DELETE || vk == VK_DIVIDE || vk == VK_NUMLOCK) lp |= 1 << 24;
    if (vk >= VK_F1 && vk <= VK_F24) swprintf(name, 32, L"F%u", vk - VK_F1 + 1);
    else if (!sc || !GetKeyNameTextW(lp, name, 32)) swprintf(name, 32, L"Key %u", vk);
    swprintf(out, n, L"%s%s%s%s", (mods & MOD_CONTROL) ? L"Ctrl+" : L"", (mods & MOD_ALT) ? L"Alt+" : L"", (mods & MOD_SHIFT) ? L"Shift+" : L"", name);
}

BOOL App_ApplyHotkey(void)
{
    UnregisterHotKey(g_main, HOTKEY_ID);
    return RegisterHotKey(g_main, HOTKEY_ID, g_set.hotMods | MOD_NOREPEAT, g_set.hotKey);
}

// While enabled, Windows tells g_main about every mouse report (even while other apps are active), so the engine can sleep
// until the mouse really moves. While off or paused for a game, it stops listening altogether.
static void ListenToMouse(void)
{
    BOOL on = g_set.enabled && !paused;
    RAWINPUTDEVICE mouse = { 1, 2, on ? RIDEV_INPUTSINK : RIDEV_REMOVE, on ? g_main : NULL };   // generic desktop / mouse
    BOOL ok = RegisterRawInputDevices(&mouse, 1, sizeof mouse);
    if (on) g_rawInput = ok;
}

static void Tray(DWORD action)
{
    nid.cbSize = sizeof nid;
    nid.hWnd = g_main;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    nid.uCallbackMessage = WM_TRAY;
    if (!nid.hIcon) LoadIconMetric(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1), LIM_SMALL, &nid.hIcon);
    swprintf(nid.szTip, 128, L"%s - %s", APP_TITLE, g_set.enabled ? L"on" : L"off");
    Shell_NotifyIconW(action, &nid);
}

void App_SetEnabled(BOOL on)
{
    g_set.enabled = on;
    Settings_Save();
    ListenToMouse();
    Tray(NIM_MODIFY);
    UI_Refresh();
    Engine_Wake();
}

static void TrayMenu(void)
{
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (g_set.enabled ? MF_CHECKED : 0), 1, L"Enabled");
    AppendMenuW(m, MF_STRING, 2, L"Settings...");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, 3, L"Exit");
    POINT p; GetCursorPos(&p);
    SetForegroundWindow(g_main);   // so the menu closes when you click elsewhere
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, g_main, NULL);
    DestroyMenu(m);
    PostMessageW(g_main, WM_NULL, 0, 0);   // as TrackPopupMenu's documentation asks
    if (cmd == 1) App_SetEnabled(!g_set.enabled);
    if (cmd == 2) UI_Show();
    if (cmd == 3) DestroyWindow(g_main);
}

static LRESULT CALLBACK MainProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_INPUT:  Engine_Wake(); break;   // then DefWindowProc, which frees the input data
    case WM_HOTKEY: if (wp == HOTKEY_ID) App_SetEnabled(!g_set.enabled); return 0;
    case WM_TRAY:
        if (lp == WM_RBUTTONUP) TrayMenu();
        if (lp == WM_LBUTTONDBLCLK) UI_Show();
        return 0;
    case WM_APP_PAUSED: paused = (BOOL)wp; ListenToMouse(); return 0;
    case WM_SETTINGCHANGE: case WM_DISPLAYCHANGE: Engine_DisplayChanged(); break;
    case WM_ENDSESSION: Engine_RestoreCursors(); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    default:
        if (msg == msgTaskbarCreated) Tray(NIM_ADD);   // Explorer restarted: put the icon back
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static LONG WINAPI Crash(EXCEPTION_POINTERS *e) { (void)e; Engine_RestoreCursors(); return EXCEPTION_CONTINUE_SEARCH; }

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdLine, int show)
{
    (void)prev; (void)show;
    CreateMutexW(NULL, TRUE, L"CursorMotionBlur.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;   // already running (it lives in the tray)

    g_set = defaults;
    BOOL firstRun = !Settings_Load();
    if (firstRun) Settings_Save();   // so the settings window only opens by itself once
    SystemParametersInfoW(SPI_SETCURSORS, 0, NULL, 0);   // undo a blank cursor left behind by a crashed earlier run
    SetUnhandledExceptionFilter(Crash);

    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = MainProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"CursorMotionBlurMain";
    RegisterClassW(&wc);
    g_main = CreateWindowExW(0, wc.lpszClassName, APP_NAME, WS_POPUP, 0, 0, 0, 0, NULL, NULL, inst, NULL);
    msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    App_ApplyHotkey();
    ListenToMouse();
    Tray(NIM_ADD);
    Engine_Start();
    if (firstRun || wcsstr(cmdLine, L"--settings")) UI_Show();

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0)
    {
        if (UI_IsDialogMessage(&m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    Engine_RestoreCursors();
    Shell_NotifyIconW(NIM_DELETE, &nid);
    return 0;
}
