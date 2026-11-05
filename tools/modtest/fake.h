// The made-up pointer and pointer size the tests feed the code under test (the mod or the app), in plain C so both can use
// it. Included before the code under test, it sends its GetCursorInfo and RegGetValueW calls here.
#pragma once
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

extern volatile LONG g_fakeOn;          // nonzero: GetCursorInfo reports the made-up pointer
extern volatile LONG g_fakeX, g_fakeY;
extern volatile LONG g_fakeReads;       // how often the code under test looked at the cursor
extern volatile LONG g_fakeBase;        // CursorBaseSize to report (0: the real one)
extern volatile LONG g_baseReads;       // how often it read CursorBaseSize

BOOL TestGetCursorInfo(PCURSORINFO ci);
LSTATUS TestRegGetValueW(HKEY key, LPCWSTR sub, LPCWSTR value, DWORD flags, LPDWORD type, PVOID data, LPDWORD len);

#ifdef __cplusplus
}
#endif

#ifndef FAKE_IMPL
#define GetCursorInfo TestGetCursorInfo
#define RegGetValueW TestRegGetValueW
#endif
