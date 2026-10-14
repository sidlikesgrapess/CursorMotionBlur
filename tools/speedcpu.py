# CPU of the mod running in Windhawk (and of dwm.exe) while the mouse circles at different speeds, input at ~125 Hz.
import ctypes, math, time, subprocess, os
from ctypes import wintypes
u = ctypes.windll.user32; k = ctypes.windll.kernel32
u.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4)); ctypes.windll.winmm.timeBeginPeriod(1)
ps = lambda q: subprocess.run(['powershell', '-NoProfile', '-Command', q], capture_output=True, text=True).stdout.split()
import sys, json
# "python speedcpu.py app" measures the standalone CursorMotionBlur.exe instead of the Windhawk mod.
# --cm=2,4,8 measures those speeds in cm/s (PX_PER_CM pixels each, 96 on the 2880x1800 laptop screen) instead of the default px/s list.
# --json=file also writes the results there.
PX_PER_CM = 96
opts = dict(a[2:].split('=', 1) for a in sys.argv[1:] if a.startswith('--'))
pid = int(ps("(Get-Process CursorMotionBlur).Id" if 'app' in sys.argv[1:] else"(Get-CimInstance Win32_Process -Filter \"Name='windhawk.exe'\" | ? CommandLine -like '*pointer-afterimage*').ProcessId")[0])
dwm = int(ps("(Get-Process dwm).Id")[0])
shell = wintypes.DWORD(); u.GetWindowThreadProcessId(u.GetShellWindow(), ctypes.byref(shell))
k.OpenProcess.restype = ctypes.c_void_p
hs = {n: k.OpenProcess(0x1410, False, p) for n, p in (('mod', pid), ('dwm', dwm), ('exp', shell.value))}
class FT(ctypes.Structure): _fields_ = [('lo', wintypes.DWORD), ('hi', wintypes.DWORD)]
class PMC(ctypes.Structure): _fields_ = [('cb', wintypes.DWORD), ('pf', wintypes.DWORD)] + [(n, ctypes.c_size_t) for n in ('peak', 'ws', 'qppp', 'qpp', 'qpnp', 'qnp', 'pfu', 'peakpf', 'private')]
def mem(h):
    m = PMC(); m.cb = ctypes.sizeof(m); ctypes.windll.psapi.GetProcessMemoryInfo(ctypes.c_void_p(h), ctypes.byref(m), m.cb)
    return m.ws / 1048576, m.private / 1048576
def cpu(h):
    a, b, c, d = FT(), FT(), FT(), FT(); k.GetProcessTimes(ctypes.c_void_p(h), ctypes.byref(a), ctypes.byref(b), ctypes.byref(c), ctypes.byref(d))
    return ((c.hi << 32 | c.lo) + (d.hi << 32 | d.lo)) / 1e7
vx, vy, vw, vh = (u.GetSystemMetrics(i) for i in (76, 77, 78, 79))
def to(x, y): u.mouse_event(0x0001 | 0x8000 | 0x4000, int((x - vx) * 65535 / (vw - 1)), int((y - vy) * 65535 / (vh - 1)), 0, 0)
u.SetForegroundWindow(u.GetShellWindow())
N = os.cpu_count(); R = 400   # circle radius in px; laptop screen is 200% (2880x1800)
print('%-20s %10s %12s %12s %10s %14s' % ('speed', 'mod: % CPU', 'mod: % 1 core', 'explorer: %', 'dwm: % CPU', 'mod RAM MB'))
speeds = [('still', 0), ('slow 500 px/s', 500), ('normal 1500 px/s', 1500), ('fast 4000 px/s', 4000), ('flick 8000 px/s', 8000)]
if 'cm' in opts: speeds = [('still', 0)] + [('%s cm/s' % c, float(c) * PX_PER_CM) for c in opts['cm'].split(',')]
results = []
for label, pxs in speeds:
    to(1440 + R, 900); time.sleep(1)   # where the circle starts, so there is no jump that would turn the blur on
    m0, d0, e0, t0 = cpu(hs['mod']), cpu(hs['dwm']), cpu(hs['exp']), time.perf_counter()
    while time.perf_counter() - t0 < 6:
        if pxs:
            a = pxs / R * (time.perf_counter() - t0); to(1440 + R * math.cos(a), 900 + R * math.sin(a))
        time.sleep(0.008)
    dt = time.perf_counter() - t0
    m, d, e = (cpu(hs['mod']) - m0) / dt * 100, (cpu(hs['dwm']) - d0) / dt * 100, (cpu(hs['exp']) - e0) / dt * 100
    ws, pv = mem(hs['mod'])
    print('%-20s %9.2f%% %11.1f%% %11.2f%% %9.2f%%   %4.1f ws / %4.1f private' % (label, m / N, m, e / N, d / N, ws, pv))
    results.append(dict(speed=label, mod=m / N, explorer=e / N, dwm=d / N, ws=ws, private=pv))
if 'json' in opts: json.dump(results, open(opts['json'], 'w'), indent=1)
print('(%d logical CPUs; hiding is on in your settings, so it kicks in at the faster speeds)' % N)
