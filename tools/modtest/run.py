"""Tests the Windhawk mod, or the standalone app's engine, without moving the mouse.

    python tools/modtest/run.py                  # the mod: the working copy, then 1.1 (89ee6c2) to compare
    python tools/modtest/run.py --base none      # the mod: the working copy only
    python tools/modtest/run.py --base <rev>     # the mod: compare with another commit
    python tools/modtest/run.py --app            # the app (src/): the working copy only
    python tools/modtest/run.py --app <dir>      # the app: the working copy, then the sources in <dir> to compare

The mod is built as a test copy (own window class names, so it never draws in the real mod's windows, and a stand-in for
explorer.exe). The app's engine.c and render.c are built as they are, with its crash-recovery flag under a key of its own.
Both get a made-up pointer (fake.c) and run against harness.cpp, which prints one line per check.

Keep the mouse still while it runs (about two minutes per version), with the PC unlocked: a white window covers the
monitor under the pointer and blur trails are drawn on it.
"""
import subprocess, sys, re, tempfile, pathlib

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent
MOD = 'windhawk/pointer-afterimage-motion-blur.wh.cpp'
CLANG = r'C:\Program Files\Windhawk\Compiler\bin\clang++.exe'
COMMON = ['-target', 'x86_64-w64-mingw32', '-O2', '-municode', f'-I{HERE}']
CXX = ['-std=c++23', '-DUNICODE', '-D_UNICODE', '-DWINVER=0x0A00', '-D_WIN32_WINNT=0x0A00', '-DNTDDI_VERSION=0x0A000008']
C = ['-x', 'c', '-std=c17']
LIBS = ['-static', '-lgdi32', '-lshell32', '-lshcore', '-lwinmm', '-luser32', '-ladvapi32', '-lpsapi', '-ld3d11', '-ldxgi']
BAND_ANCHOR = "    // The mod's process draws in the window and tells it which process it is"
FALLBACK = ('    if (!w) w = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | '
            'WS_EX_NOACTIVATE, BAND_CLASS, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);'
            '   // test: only explorer.exe gets a band\n')


def test_copy(src):
    """The mod with its own window classes, any process's band window accepted, and a plain band window fallback."""
    out = src.replace('L"PointerAfterimageMotionBlurBand"', 'L"PAMBTestBand"')
    out = out.replace('L"PointerAfterimageMotionBlur"', 'L"PAMBTestOverlay"')
    for old, new in (('(!shell || owner == shell)', 'true'), (BAND_ANCHOR, FALLBACK + BAND_ANCHOR)):
        if old not in out:
            sys.exit(f'run.py: the mod no longer contains {old!r}; update test_copy()')
        out = out.replace(old, new, 1)
    if 'PointerAfterimageMotionBlur' in out:
        sys.exit('run.py: a window class name was not renamed; update test_copy()')
    return out


def compile_(src, obj, flags):
    r = subprocess.run([CLANG, *COMMON, *flags, '-c', str(src), '-o', str(obj)], capture_output=True, text=True)
    if r.returncode:
        sys.exit(f'run.py: compiling {src} failed:\n{r.stdout}{r.stderr}')
    return obj


def link(objs, exe):
    r = subprocess.run([CLANG, *COMMON, *map(str, objs), '-o', str(exe), *LIBS], capture_output=True, text=True)
    if r.returncode:
        sys.exit(f'run.py: linking {exe} failed:\n{r.stdout}{r.stderr}')
    return exe


def workdir(label):
    d = pathlib.Path(tempfile.gettempdir()) / 'pamb-modtest' / re.sub(r'\W+', '_', label)
    d.mkdir(parents=True, exist_ok=True)
    return d


def build_mod(label, src):
    d = workdir(label)
    mod = d / 'mod.cpp'
    mod.write_text(test_copy(src) + '\n' + (HERE / 'mod_target.cpp').read_text(encoding='utf-8'), encoding='utf-8')
    sim = d / 'sim.cpp'
    sim.write_text(test_copy(src) + '\n' + (HERE / 'bandsim.cpp').read_text(encoding='utf-8'), encoding='utf-8')
    mod_flags = [*CXX, '-include', str(HERE / 'stub.h')]
    fake = compile_(HERE / 'fake.c', d / 'fake.o', [*C, '-DUNICODE', '-D_UNICODE', '-D_WIN32_WINNT=0x0A00'])
    harness = compile_(HERE / 'harness.cpp', d / 'harness.o', CXX)
    exe = link([compile_(mod, d / 'mod.o', mod_flags), harness, fake], d / 'harness.exe')
    simexe = link([compile_(sim, d / 'sim.o', mod_flags), fake], d / 'bandsim.exe')
    return [str(exe), str(simexe)]


def build_app(label, srcdir):
    d = workdir(label)
    inc = [*C, f'-I{srcdir}']
    fake = compile_(HERE / 'fake.c', d / 'fake.o', [*C, '-DUNICODE', '-D_UNICODE', '-D_WIN32_WINNT=0x0A00'])
    objs = [compile_(HERE / 'app_target.c', d / 'engine.o', inc), compile_(pathlib.Path(srcdir) / 'render.c', d / 'render.o', inc),
            compile_(HERE / 'harness.cpp', d / 'harness.o', CXX), fake]
    return [str(link(objs, d / 'harness.exe')), '-']


def run(label, cmd):
    print(f'== {label}: running (keep the mouse still)', flush=True)
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=400)
    results = []
    for line in r.stdout.splitlines():
        m = re.match(r'RESULT (\S+) (\S+) (.*)', line)
        if m:
            results.append(m.groups())
            print(f'  {m.group(2):5} {m.group(1):12} {m.group(3)}', flush=True)
        elif line.strip():
            print(f'  {line}', flush=True)
    if r.stderr.strip():
        print(r.stderr.rstrip())
    if any(t == 'setup' and s == 'FAIL' for t, s, _ in results):
        sys.exit('run.py: the test could not run (see setup above)')
    return label, results


def mod_label(name, src):
    v = re.search(r'@version\s+(\S+)', src)
    return f'mod {name} ({v.group(1) if v else "?"})'


def main():
    args = sys.argv[1:]
    runs = []
    if '--app' in args:
        i = args.index('--app')
        base = args[i + 1] if i + 1 < len(args) and not args[i + 1].startswith('--') else None
        targets = [('app working copy', REPO / 'src')] + ([(f'app {pathlib.Path(base).name}', base)] if base else [])
        for label, srcdir in targets:
            print(f'== {label}: building', flush=True)
            runs.append(run(label, build_app(label, srcdir)))
    else:
        base = args[args.index('--base') + 1] if '--base' in args else '89ee6c2'
        targets = [('working copy', (REPO / MOD).read_text(encoding='utf-8'))]
        if base != 'none':
            old = subprocess.run(['git', 'show', f'{base}:{MOD}'], cwd=REPO, capture_output=True, text=True, encoding='utf-8')
            if old.returncode:
                sys.exit(f'run.py: git show {base}:{MOD} failed: {old.stderr}')
            targets.append((f'commit {base}', old.stdout))
        for name, src in targets:
            label = mod_label(name, src)
            print(f'== {label}: building', flush=True)
            runs.append(run(label, build_mod(label, src)))

    print('\n== summary')
    tests = []
    for _, res in runs:
        for t, _, _ in res:
            if t not in tests:
                tests.append(t)
    print(f'  {"check":12} ' + ' '.join(f'{label:28}' for label, _ in runs))
    for t in tests:
        cells = ['/'.join(s for tt, s, _ in res if tt == t) or '-' for _, res in runs]
        print(f'  {t:12} ' + ' '.join(f'{c:28}' for c in cells))
    failed = any(s == 'FAIL' for _, s, _ in runs[0][1])
    print(f'\n{runs[0][0]}: ' + ('FAILED' if failed else 'all checks passed'))
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
