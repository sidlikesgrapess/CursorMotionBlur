# Pointer Afterimage Motion Blur — CPU benchmarks

Measured 2026-10-14 on the mod running in Windhawk (1.1 plus the shell-hook change), with default
settings and "Hide the real cursor when moving very fast" on.

**Setup:** laptop screen 2880×1800 at 200% scaling (64 px blur copies), pointer size 1, 12 logical
CPUs. The mouse moved in circles of 400 px radius for 6 s per speed, with input injected at about
125 reports per second. "% of whole CPU" is out of all 12 logical CPUs; "% of one core" is the same
time out of a single CPU.

## 120 Hz

| Speed | Mod (% of whole CPU) | Mod (% of one core) | Compositor dwm.exe (% of whole CPU) |
|---|---|---|---|
| Still | 0.00% | 0% | 0.43% |
| Slow (500 px/s) | 0.87% | 10.4% | 2.88% |
| Normal (1500 px/s) | 1.02% | 12.2% | 4.19% |
| Fast (4000 px/s) | 1.67% | 20.0% | 3.93% |
| Flick (8000 px/s) | 1.89% | 22.7% | 4.41% |

## Earlier run, refresh rate not recorded (probably 60 Hz)

| Speed | Mod (% of whole CPU) | Mod (% of one core) | Compositor dwm.exe (% of whole CPU) |
|---|---|---|---|
| Still | 0.00% | 0% | 0.52% |
| Slow (500 px/s) | 0.69% | 8.3% | 2.13% |
| Normal (1500 px/s) | 0.91% | 10.9% | 2.54% |
| Fast (4000 px/s) | 1.28% | 15.4% | 2.69% |
| Flick (8000 px/s) | 1.15% | 13.8% | 2.84% |

## Other measurements, same day

| Check | Result |
|---|---|
| Idle CPU, mouse still, 10 s | 0.0 ms (no wake-ups) |
| Mouse still with Task Manager (admin) in front, 5 s | 31 ms (cursor checked every 100 ms) |
| Back on the desktop, mouse still, 5 s | 0.0 ms |
| Blur shown over Task Manager while moving | 99% of checks |
| Hide when fast | hidden 95–100% of checks, cursor back 28–52 ms after stopping |
| Handles / GDI / USER objects over 5 move-and-rest rounds | flat at 124 / 5 / 7 (no leaks) |

## Notes

- The mod draws at most one frame per half refresh: every 4 ms at 120 Hz against 8 ms at 60 Hz, so
  higher refresh rates cost more.
- Blur copies are capped at 100 per frame, so very fast flicks don't keep adding work.
- dwm.exe works harder whenever anything moves on screen, the pointer included, so only part of its
  number comes from the blur.
- On a 100% screen the copies are 32 px (a quarter of the pixels); expect roughly half to two-thirds
  of these numbers.
- Script: `tools/speedcpu.py` (circles the mouse and reads the processes' CPU times with `GetProcessTimes`).

## Explorer band-window version (2026-10-14, 120 Hz)

Same setup; the blur is drawn by the mod's process into a window that explorer.exe creates above the Start menu.
Two runs, CPU as % of all 12 logical CPUs; "private" is the mod process's committed private memory.

| Speed | Mod CPU (run 1 / 2) | explorer.exe | dwm.exe | Mod RAM (working set / private) |
|---|---|---|---|---|
| Still | 0.00% / 0.00% | 0.78% / 0.63% | 0.54% / 0.37% | 11.0 / 1.9 MB |
| Slow (500 px/s) | 1.00% / 1.06% | 0.93% / 0.95% | 4.34% / 4.34% | 11.1 / 2.0 MB |
| Normal (1500 px/s) | 1.08% / 1.00% | 1.06% / 0.89% | 4.45% / 3.88% | 11.3 / 2.9 MB |
| Fast (4000 px/s) | 1.63% / 1.30% | 1.02% / 1.00% | 4.08% / 3.99% | 11.6 / 2.9 MB |
| Flick (8000 px/s) | 1.95% / 1.32% | 0.61% / 0.39% | 4.49% / 4.32% | 12.0 / 2.9 MB |

- explorer.exe's own baseline (still) is 0.6-0.8%, so the band window adds only about 0.2-0.3% there while moving.
- The working set includes shared DLL pages (windhawk.exe and system DLLs); the private figure is the mod's own
  memory. It grows by ~1 MB while moving (the drawing canvas) and is given back after 1.5 s idle.
- Script: `tools/speedcpu.py`, extended with explorer.exe and memory columns.
