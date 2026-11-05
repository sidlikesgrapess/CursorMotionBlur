// Appended to a test copy of the Windhawk mod.
#include "target.h"

extern "C" int T_Start(void) {
    if (!WhTool_ModInit()) return 0;
    WhTool_ModSettingsChanged();
    return 1;
}
extern "C" void T_Stop(void) { WhTool_ModUninit(); }
extern "C" void T_Wake(void) { if (g_mouseWake) SetEvent(g_mouseWake); }
extern "C" int T_MonCursor(void) { return g_monCursor; }
extern "C" int T_TrailMs(void) { return g_trailMs; }
extern "C" int T_MaxHistStep(void) {
    int m = 0;
    EnterCriticalSection(&g_gate);
    for (int i = 1; i < g_histN; i++)
        if (int d = (int)Dist(g_hist[i - 1], g_hist[i]); d > m) m = d;
    LeaveCriticalSection(&g_gate);
    return m;
}
extern "C" int T_HiddenFlag(void) { return g_whValue; }
extern "C" const wchar_t* T_BandClass(void) { return BAND_CLASS; }
extern "C" const wchar_t* T_OverlayClass(void) { return L"PAMBTestOverlay"; }
