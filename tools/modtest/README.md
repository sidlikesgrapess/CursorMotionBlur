# Mod and app tests

```
python tools/modtest/run.py                # the mod: the working copy, then 1.1 to compare
python tools/modtest/run.py --base none    # the mod: the working copy only
python tools/modtest/run.py --app          # the standalone app (src/)
python tools/modtest/run.py --app <dir>    # the app, then the app sources in <dir> to compare
```

The mod checks need `windhawk/pointer-afterimage-motion-blur.wh.cpp`, which lives on the `windhawk-mod` branch.

The app's `engine.c` and `render.c` go through the same checks, minus the ones about explorer's window. Its
crash-recovery flag goes under `HKCU\Software\CursorMotionBlurTest`, never the real app's key.

Run it with the PC unlocked and the screen on, and keep the mouse still for about two minutes. A white window covers
the monitor under the pointer and blur trails are drawn on it. The mouse is never moved.

`run.py` builds a test copy of `windhawk/pointer-afterimage-motion-blur.wh.cpp` with Windhawk's compiler. The copy has
its own window class names, so it never draws in the real mod's windows. `stub.h` stands in for Windhawk. `fake.c`
feeds the code under test a made-up pointer (`GetCursorInfo`) and pointer size (`CursorBaseSize`); its real threads run
unchanged. `mod_target.cpp` and `app_target.c` connect the mod and the app to `harness.cpp`, which holds the checks.
`bandsim.cpp` stands in for explorer.exe. A check for a 1.2 fix should fail on 1.1.

| Check | What it does | Passes when |
|---|---|---|
| `crash` | Kills a copy of the mod while it draws in explorer's window. | Explorer's window hides and nothing is left on screen. |
| `stale` | Draws right after the killed copy, after the idle trim and after explorer restarts. Every frame is captured through Desktop Duplication. | No frame shows blur away from the trail, and the screen is clean after the fade. |
| `size` | Compares the blur's size with the pointer shape Windows sends to the screen, at the current scaling and pointer size. | The two sizes are equal. |
| `flicker` | The blur runs onto the still real pointer and stops there, five times. A window under the pointer counts its mouse messages. | At most 3 per stretch (the blur appearing and going away). |
| `jump` | The pointer jumps from the right edge to the left one mid-swipe (PowerToys Cursor Wrap). | No frame draws across the middle of the screen, and the trail never holds a step over 40% of the screen. |
| `swipe` | Long fast swipes, so the window has to move along: 3, then 30 more (`MODTEST_SWIPES=n` to change). | No frame shows blur outside the trail. Without the 1 px resize on every move, about 1 swipe in 8 shows the blur ahead of the pointer for a frame. |
| `resize` | Pointer size 1 changed to 2 while the mouse is still. | The blur takes the new size on the next movement, and the size isn't read while the mouse is still. |
| `idle` | 5 s with a still pointer. | 0 cursor reads and under 20 ms CPU. |
| `memory` | Private memory after idle. | Within 1 MB of the start. |
| `exit` | The mod stops. | Explorer's window is hidden. |

Not covered: hiding the cursor when fast (it would swap the machine's real cursors), administrator windows, fullscreen
pause, several monitors.

## By hand, before a release

1. **Excel**: File > Open, move fast over the recent files, then stop on one. The highlight stays steady and the item
   clicks.
2. **Explorer address bar, Paint**: move fast over them, then stop. The pointer doesn't flicker to the arrow.
3. **Start menu**: open it and move fast over it. The blur shows above it.
4. **PowerToys Cursor Wrap**: push the pointer off one edge. No trail is drawn across the screen.
5. **Scaling 125%, pointer sizes 1-3**: run `python tools/modtest/run.py --base none` and check `size` passes. Then
   change the pointer size, pause, move fast, and see the blur match.
6. **Hiding on** ("Hide above speed"): move very fast. The real pointer hides and comes back when you slow down.
7. **Two monitors with different scaling**, if you can borrow a setup: cross between them while moving fast.
