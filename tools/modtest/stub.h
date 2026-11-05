// Stands in for Windhawk when a copy of the mod is built as a test program (force-included before the mod). fake.h sends
// the mod's GetCursorInfo and CursorBaseSize reads to the test's made-up pointer.
#pragma once
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include "fake.h"

#define WH_MOD_ID L"pointer-afterimage-motion-blur-test"

inline bool g_whVerbose = getenv("MODTEST_VERBOSE") != nullptr;
inline void Wh_Log(const wchar_t* f, ...) {
    if (!g_whVerbose) return;
    va_list a;
    va_start(a, f);
    fwprintf(stderr, L"  [mod] ");
    vfwprintf(stderr, f, a);
    fwprintf(stderr, L"\n");
    va_end(a);
}

// Settings for every test run. Hiding stays off: it would swap the real system cursors of the machine.
inline int Wh_GetIntSetting(const wchar_t* n, ...) {
    if (!wcscmp(n, L"trailOpacity")) return 60;
    if (!wcscmp(n, L"drawSpeed")) return 10;   // cm/s: low, so every test path draws
    if (!wcscmp(n, L"trailMs")) return 60;
    if (!wcscmp(n, L"hideWhenFast")) return 0;
    if (!wcscmp(n, L"hideSpeed")) return 30;
    if (!wcscmp(n, L"pauseInFullscreen")) return 0;
    return 0;
}
inline int g_whValue;   // the mod's only stored value is the "cursors hidden" flag
inline int Wh_GetIntValue(const wchar_t*, int d) { return g_whValue ? g_whValue : d; }
inline BOOL Wh_SetIntValue(const wchar_t*, int v) { g_whValue = v; return TRUE; }
inline BOOL Wh_SetFunctionHook(void*, void*, void**) { return TRUE; }
