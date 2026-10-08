// CursorMotionBlur: a live motion blur for the Windows mouse cursor.
//
//   main.c        start-up, tray icon, hotkey, settings file, launch at startup
//   engine.c      records the cursor path, hides the real cursor when fast, works out the blur copies for each frame
//   render_*.c*   draws those copies: render_gpu.cpp (Direct3D + DirectComposition) or render_cpu.c (by hand + layered window)
//   ui.c          the settings window and the update check
#pragma once
#ifndef UNICODE
#define UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00   // Windows 10
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APP_NAME    L"CursorMotionBlur"
#define APP_VERSION L"1.1.0"
#define APP_TITLE   APP_NAME L" v" APP_VERSION
#define REPO        L"sidlikesgrapess/CursorMotionBlur"

// ---- settings (main.c) ----
typedef struct
{
    BOOL enabled;
    int  strength;          // 1-100: opacity of the blur right behind the pointer, in %
    int  trailMs;           // 10-150: how far back in time the blur reaches
    BOOL hideWhenFast;      // swap the real cursor for an invisible one while moving very fast
    int  hideSpeedCm;       // 20-300: pointer speed above which it is hidden, in cm/s on the main monitor (same hand speed everywhere)
    BOOL pauseFullscreen;   // no blur while a fullscreen app or game is in front
    UINT hotMods, hotKey;   // the global on/off shortcut (MOD_* flags, virtual key)
} Settings;

extern Settings g_set;       // written only by the UI thread; the worker threads just read single fields
extern HWND g_main;          // hidden main window: tray icon, hotkey and raw mouse input
extern volatile BOOL g_rawInput;   // Windows tells g_main about every mouse report (else the engine polls)

void Settings_Save(void);
void Settings_Reset(void);
BOOL Startup_Get(void);
void Startup_Set(BOOL on);
void App_SetEnabled(BOOL on);           // also refreshes the tray, the mouse listening and the settings window
BOOL App_ApplyHotkey(void);             // FALSE if another program already owns the shortcut
void HotkeyText(UINT mods, UINT vk, WCHAR *out, int n);
void Log(const char *fmt, ...);         // %TEMP%\CursorMotionBlur.log, only when the environment variable CMB_DEBUG=1

// ---- engine (engine.c) ----
void Engine_Start(void);
void Engine_Wake(void);                 // a mouse report arrived, or the settings changed
void Engine_RestoreCursors(void);       // put the real cursors back (exit, crash)
void Engine_DisplayChanged(void);       // monitors, scaling or cursor scheme changed
#define WM_APP_PAUSED (WM_APP + 2)      // engine -> main window: wParam = paused for a fullscreen app

// ---- renderer (render_gpu.cpp or render_cpu.c), called only from the engine's render thread ----
typedef struct { int w, h, hx, hy, id; UINT32 *px; } Sprite;   // cursor picture at its own size: premultiplied BGRA, hotspot
typedef struct { int x, y; float a; } Copy;                     // one copy: top-left (relative to the frame box), opacity 0-1

#define MAX_COPIES 100      // the most copies spread along the trail in one frame
#define COPY_BUF   512      // room for the copies of one frame (short segments add one each)

// Draws n copies of sp, each resized to sw x sh, into the box (x, y, w, h) in screen pixels, and shows it.
BOOL R_Draw(const Sprite *sp, int sw, int sh, const Copy *c, int n, int x, int y, int w, int h);   // FALSE: could not draw
void R_Hide(void);
void R_Trim(void);          // idle for a while: give back memory that can be rebuilt

// ---- settings window (ui.c) ----
void UI_Show(void);
BOOL UI_IsDialogMessage(MSG *m);
void UI_Refresh(void);

#ifdef __cplusplus
}
#endif
