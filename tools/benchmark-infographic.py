import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.patches import Patch

SURF, TXT, TXT2, MUTED, GRID = '#1a1a19', '#f0efec', '#c3c2b7', '#8b8a84', '#34342f'
NEW, OLD = '#3987e5', '#d95926'   # 1.1 blue, 1.0 orange (validated dark pair)
plt.rcParams.update({'font.family': 'Segoe UI', 'text.color': TXT, 'xtick.color': TXT2, 'ytick.color': TXT2})

groups = ['Still', 'Slow\n(2 cm/s)', 'Fast\n(40 cm/s)']
v10 = [0.00, 0.98, 1.73]
v11 = [0.00, 0.15, 1.41]

fig, ax = plt.subplots(figsize=(10, 5.6), dpi=100, facecolor=SURF)
ax.set_facecolor(SURF)
fig.subplots_adjust(left=0.08, right=0.97, top=0.80, bottom=0.17)
fig.text(0.08, 0.91, 'CPU use while moving the mouse', fontsize=22, weight='bold')
fig.text(0.08, 0.855, 'Lower is better  ·  % of all CPU threads', fontsize=13, color=TXT2)

w, gap = 0.34, 0.02
for i, (a, b) in enumerate(zip(v10, v11)):
    for x, v, c in ((i - w / 2 - gap / 2, a, OLD), (i + w / 2 + gap / 2, b, NEW)):
        ax.bar(x, v, w, color=c, zorder=2)
        ax.text(x, v + 0.04, '%.2f%%' % v if v else '0%', ha='center', va='bottom', fontsize=13, color=TXT)

ax.set_xticks(range(3)); ax.set_xticklabels(groups, fontsize=13)
ax.set_ylim(0, 2.1); ax.set_yticks([0, 0.5, 1, 1.5, 2])
ax.yaxis.set_major_formatter(matplotlib.ticker.FuncFormatter(lambda v, _: '%g%%' % v))
ax.tick_params(length=0, labelsize=12)
ax.grid(axis='y', color=GRID, linewidth=1, zorder=1)
for s in ax.spines.values(): s.set_visible(False)
ax.legend(handles=[Patch(color=OLD, label='1.0'), Patch(color=NEW, label='1.1')], loc='upper left', frameon=False,
          fontsize=13, handlelength=1.2)

fig.text(0.08, 0.04, '200% laptop screen at 120 Hz. 1.1 draws no blur below "Minimum blur speed" (here 10 cm/s).',
         fontsize=11, color=MUTED)
fig.savefig(r'C:\Users\Siddartha\CursorMotionBlur\assets\benchmark-1.1.png', facecolor=SURF)
print('saved')
