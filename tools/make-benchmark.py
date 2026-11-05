# Draws the benchmark infographic from two speedcpu.py --json files:
#   python make-benchmark.py old.json new.json out.png [old label] [new label]
# Three panels: the app's CPU, dwm.exe's CPU and the app's private memory, per mouse speed.
import sys, json
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib import font_manager
from matplotlib.patches import Rectangle

for f in ('segoeui', 'seguisb', 'segoeuib'):
    try: font_manager.fontManager.addfont(r'C:\Windows\Fonts\%s.ttf' % f)
    except OSError: pass
plt.rcParams['font.family'] = ['Segoe UI', 'DejaVu Sans']

BG, GRID, TXT, DIM, SHADE = '#1b1b1b', '#333333', '#f2f2f2', '#a8a8a8', '#2a2a26'
OLD, NEW = '#d9582b', '#3b86e0'
old, new = (json.load(open(p)) for p in sys.argv[1:3])
oldName = sys.argv[4] if len(sys.argv) > 4 else '2.0.0'
newName = sys.argv[5] if len(sys.argv) > 5 else 'new'
labels = ['Still'] + [r['speed'].split()[0] for r in old[1:]]

fig = plt.figure(figsize=(19, 7.4), dpi=100, facecolor=BG)
fig.text(0.035, 0.945, 'CursorMotionBlur  %s vs %s' % (newName, oldName), color=TXT, fontsize=23, fontweight='bold', va='center')
fig.text(0.035, 0.88, 'CPU and memory while the mouse moves at different speeds', color=DIM, fontsize=13, va='center')
for x, color, text in ((0.74, OLD, 'Version ' + oldName), (0.86, NEW, 'Version ' + newName)):
    fig.add_artist(Rectangle((x, 0.896), 0.017, 0.04, transform=fig.transFigure, color=color))
    fig.text(x + 0.023, 0.916, text, color=TXT, fontsize=15, va='center')
fig.text(0.035, 0.095, 'Measured on an Intel Core 5 210H laptop, 2880×1800 screen at 200%% scaling and 120 Hz, about 125 mouse reports per '
         'second, opacity 92, trail 30 ms, hiding on; %s with "Minimum blur speed" at 10 cm/s.' % newName, color=DIM, fontsize=10, va='center')
fig.text(0.035, 0.062, 'Full data: github.com/sidlikesgrapess/CursorMotionBlur/blob/main/BENCHMARKS.md', color=DIM, fontsize=10, va='center')

def chart(left, key, top, step, title, fmt, note):
    ax = fig.add_axes([left, 0.189, 0.285, 0.52], facecolor=BG)
    fig.text(left, 0.748, title, color=TXT, fontsize=13.5, fontweight='bold', va='center')
    ax.axvspan(0.5, 3.5, color=SHADE, zorder=0)
    if note:
        ax.text(2, top * 0.975, 'below "Minimum blur speed" (10 cm/s):\nno blur drawn in %s' % newName, color='#777777', fontsize=10,
                ha='center', va='top')
    xs = range(len(labels))
    ax.bar([x - 0.19 for x in xs], [r[key] for r in old], 0.37, color=OLD, zorder=3)
    ax.bar([x + 0.19 for x in xs], [r[key] for r in new], 0.37, color=NEW, zorder=3)
    ax.set_xlim(-0.5, len(labels) - 0.5); ax.set_ylim(0, top)
    ticks = [i * step for i in range(int(round(top / step)) + 1)]
    ax.set_yticks(ticks); ax.set_yticklabels([fmt % t for t in ticks], color=DIM, fontsize=11)
    ax.set_xticks(list(xs)); ax.set_xticklabels(labels, color=DIM, fontsize=11)
    ax.tick_params(length=0)
    ax.grid(axis='y', color=GRID, linewidth=1, zorder=1); ax.set_axisbelow(True)
    for s in ('top', 'right', 'left'): ax.spines[s].set_visible(False)
    ax.spines['bottom'].set_color('#666666')
    ax.set_xlabel('mouse speed (cm/s)', color=DIM, fontsize=12, labelpad=8)
    return ax

ax = chart(0.035, 'mod', 2.5, 0.5, 'The app  (% of all CPU threads)', '%g%%', True)
o, n = old[1]['mod'], new[1]['mod']
if o > n:
    ax.annotate('−%d%%' % round((o - n) / o * 100), xy=(1.19, n + 0.03), xytext=(1.19, 1.05), color=TXT, fontsize=12, fontweight='bold',
                ha='center', arrowprops=dict(arrowstyle='->', color=TXT, lw=1.2))
chart(0.36, 'dwm', 4, 1, 'Windows compositor, dwm.exe  (% of all CPU threads)', '%g%%', True)
top = max(r['private'] for r in old + new)
chart(0.685, 'private', (int(top / 2) + 1) * 2, 2, 'The app\'s private memory  (MB)', '%g', False)
fig.savefig(sys.argv[3], facecolor=BG)
