#define FAKE_IMPL
#include "fake.h"
#include <string.h>

volatile LONG g_fakeOn, g_fakeX, g_fakeY, g_fakeReads, g_fakeBase, g_baseReads;

BOOL TestGetCursorInfo(PCURSORINFO ci)
{
    if (!g_fakeOn) return GetCursorInfo(ci);
    InterlockedIncrement(&g_fakeReads);
    ci->flags = CURSOR_SHOWING;
    ci->hCursor = LoadCursorW(NULL, IDC_ARROW);
    ci->ptScreenPos.x = g_fakeX;
    ci->ptScreenPos.y = g_fakeY;
    return TRUE;
}

LSTATUS TestRegGetValueW(HKEY key, LPCWSTR sub, LPCWSTR value, DWORD flags, LPDWORD type, PVOID data, LPDWORD len)
{
    if (value && !wcscmp(value, L"CursorBaseSize"))
    {
        InterlockedIncrement(&g_baseReads);
        LONG v = g_fakeBase;
        if (v)
        {
            if (data) *(DWORD *)data = (DWORD)v;
            if (len) *len = sizeof(DWORD);
            if (type) *type = REG_DWORD;
            return ERROR_SUCCESS;
        }
    }
    return RegGetValueW(key, sub, value, flags, type, data, len);
}
