// The standalone app's engine and renderer as test subject: engine.c is compiled in here (with the made-up pointer), so the
// tests can look at its trail. Built with -I pointing at the app's src folder.
#include "app.h"
#undef APP_NAME
#define APP_NAME L"CursorMotionBlurTest"   // the crash-recovery flag goes under a key of its own, never the real app's
#include "fake.h"
#include "engine.c"
#include "target.h"

Settings g_set;
HWND g_main;
volatile BOOL g_rawInput = TRUE;   // the tests report every movement (Engine_Wake), like raw input
void Log(const char *fmt, ...) { (void)fmt; }

int T_Start(void)
{
    g_set.enabled = TRUE;
    g_set.opacity = 60;
    g_set.drawSpeedCm = 10;
    g_set.trailMs = 60;
    g_set.hideWhenFast = FALSE;   // would swap the machine's real cursors
    g_set.hideSpeedCm = 30;
    g_set.pauseFullscreen = FALSE;
    Engine_Start();
    return 1;
}
void T_Stop(void) { Engine_Stop(); }
void T_Wake(void) { Engine_Wake(); }
int T_MonCursor(void) { return monCursor; }
int T_TrailMs(void) { return g_set.trailMs; }
int T_MaxHistStep(void)
{
    int m = 0;
    EnterCriticalSection(&gate);
    for (int i = 1; i < histN; i++)
    {
        int d = (int)Dist(hist[i - 1], hist[i]);
        if (d > m) m = d;
    }
    LeaveCriticalSection(&gate);
    return m;
}
int T_HiddenFlag(void)
{
    DWORD v = 0, len = sizeof v;
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\CursorMotionBlurTest", L"CursorsHidden", RRF_RT_REG_DWORD, NULL, &v,
                        &len) == ERROR_SUCCESS && v;
}
const wchar_t *T_BandClass(void) { return NULL; }
const wchar_t *T_OverlayClass(void) { return L"CursorMotionBlurOverlay"; }
