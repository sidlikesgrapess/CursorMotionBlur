// What harness.cpp needs from the code under test: mod_target.cpp implements it for the Windhawk mod, app_target.c for the
// standalone app.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

int T_Start(void);                      // starts drawing the blur (0: failed)
void T_Stop(void);
void T_Wake(void);                      // a mouse report arrived
int T_MonCursor(void);                  // the size the blur is drawn at
int T_TrailMs(void);
int T_MaxHistStep(void);                // the largest step in the trail it holds now
int T_HiddenFlag(void);                 // the "cursors hidden" crash-recovery flag
const wchar_t *T_BandClass(void);       // explorer's window class (the mod), or NULL (the app)
const wchar_t *T_OverlayClass(void);    // its own overlay window class

#ifdef __cplusplus
}
#endif
