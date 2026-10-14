# CursorMotionBlur: CPU and RAM

Measured 2026-10-14. The app as it was in v2.0.0 against the current build, one after the other, with the same settings
(opacity 92, trail 30 ms, hiding on); the new one with "Minimum blur speed" at 10 cm/s (about 960 px/s on this screen).

**Setup:** Intel Core 5 210H laptop, screen 2880x1800 at 200% scaling and 120 Hz, 12 logical CPUs. The mouse moved in
circles of 400 px radius for 6 s per speed, with input injected at about 125 reports per second. CPU is % of all 12
logical CPUs. Script: `tools/speedcpu.py app --cm=2,4,8,12,40,80`, image: `tools/make-benchmark.py`.

| Speed | App 2.0.0 | App new | dwm 2.0.0 | dwm new | Private RAM 2.0.0 | Private RAM new |
|---|---|---|---|---|---|---|
| Still | 0.00% | 0.00% | 0.26% | 0.33% | 2.0 MB | 2.0 MB |
| 2 cm/s | 0.91% | 0.11% | 3.88% | 1.13% | 2.0 MB | 2.0 MB |
| 4 cm/s | 0.89% | 0.13% | 4.21% | 0.85% | 2.5 MB | 2.0 MB |
| 8 cm/s | 0.78% | 0.20% | 2.73% | 0.59% | 2.0 MB | 2.0 MB |
| 12 cm/s | 1.02% | 0.97% | 3.66% | 3.66% | 2.9 MB | 2.9 MB |
| 40 cm/s | 1.19% | 1.80% | 3.69% | 3.71% | 2.9 MB | 2.9 MB |
| 80 cm/s | 1.50% | 1.17% | 3.73% | 3.80% | 2.9 MB | 2.9 MB |

The working set (including shared system DLL pages) was 11-12 MB for both.

The 8 cm/s row was measured again on its own: the first run jumped the pointer 400 px to the circle's edge before each
speed, and that jump turned the blur on (it stays on above half the minimum speed, 5 cm/s). The script now starts the
circle where the pointer already is. The other rows are from the first run; the jump doesn't matter for them (2 and 4 cm/s
are under half the minimum speed, 12 cm/s and up are over it).

- The saving is below the minimum blur speed: no blur is drawn there, so the app and the compositor stay almost idle.
- Above it the two versions cost about the same. Run-to-run noise on the moving rows is a few tenths of a percent
  (the 40 cm/s result for the new build is one such run; an earlier run of the same build read 1.5% at 4000 px/s).
- Another run with the new defaults (opacity 50, minimum speed 29 cm/s, trail 30 ms) gave 0.00% still, 0.09% at 500 px/s,
  1.1% at 1500 px/s, 1.9% at 4000 px/s and 1.6% at 8000 px/s.

![App 2.0.0 vs new](assets/benchmark-app.png)
