import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.patches import Patch

SURF = '#1a1a19'
TXT, TXT2, MUTED, GRID = '#f0efec', '#c3c2b7', '#8b8a84', '#34342f'
NEW, OLD = '#3987e5', '#d95926'   # 1.1 blue, 1.0 orange (validated dark pair)
plt.rcParams.update({'font.family': 'Segoe UI', 'text.color': TXT, 'axes.labelcolor': TXT2,
                     'xtick.color': TXT2, 'ytick.color': TXT2})

speeds = ['Still', '2', '4', '8', '12', '40', '80']
cpu10 = [0.00, 0.98, 0.82, 1.24, 0.91, 1.73, 1.91]
cpu11 = [0.00, 0.15, 0.17, 0.37, 1.17, 1.41, 1.56]
dwm10 = [0.43, 3.84, 3.90, 4.05, 3.97, 4.10, 3.77]
dwm11 = [0.28, 0.78, 0.56, 1.41, 4.34, 4.36, 4.73]

fig = plt.figure(figsize=(16, 7.4), dpi=100, facecolor=SURF)
fig.text(0.04, 0.925, 'Pointer Afterimage Motion Blur  1.1 vs 1.0', fontsize=26, weight='bold', color=TXT)
fig.text(0.04, 0.87, 'CPU use while the mouse moves at different speeds', fontsize=14, color=TXT2)

def panel(rect, title, a10, a11, ymax):
    ax = fig.add_axes(rect, facecolor=SURF)
    n = len(speeds); w = 0.38
    xs = range(n)
    ax.axvspan(0.5, 3.5, color='#2a2a27', zorder=0)
    ax.text(2.0, ymax * 0.965, 'below "Minimum blur speed" (10 cm/s):\nno blur drawn in 1.1', ha='center', va='top',
            fontsize=10.5, color=MUTED)
    ax.bar([x - w / 2 - 0.01 for x in xs], a10, w, color=OLD, zorder=2)
    ax.bar([x + w / 2 + 0.01 for x in xs], a11, w, color=NEW, zorder=2)
    ax.set_ylim(0, ymax); ax.set_xlim(-0.6, n - 0.4)
    ax.set_xticks(list(xs)); ax.set_xticklabels(speeds, fontsize=11.5)
    ax.set_xlabel('mouse speed (cm/s)', fontsize=11.5, labelpad=8)
    ax.yaxis.set_major_formatter(matplotlib.ticker.FuncFormatter(lambda v, _: '%g%%' % v))
    ax.tick_params(axis='y', labelsize=11, length=0); ax.tick_params(axis='x', length=0)
    ax.grid(axis='y', color=GRID, linewidth=1, zorder=1)
    for s in ax.spines.values(): s.set_visible(False)
    ax.spines['bottom'].set_visible(True); ax.spines['bottom'].set_color(MUTED)
    ax.set_title(title, loc='left', fontsize=15, weight='bold', color=TXT, pad=14)
    return ax

a = panel([0.05, 0.19, 0.42, 0.52], 'The mod  (% of all CPU threads)', cpu10, cpu11, 2.5)
a.annotate('−85%', xy=(1.2, 0.17), xytext=(1.2, 1.25), ha='center', fontsize=12, weight='bold', color=TXT,
           arrowprops=dict(arrowstyle='-|>', color=TXT2, lw=1.2))
b = panel([0.55, 0.19, 0.42, 0.52], 'Windows compositor, dwm.exe  (% of all CPU threads)', dwm10, dwm11, 6)
fig.legend(handles=[Patch(color=OLD, label='Version 1.0'), Patch(color=NEW, label='Version 1.1')], loc='upper right',
           bbox_to_anchor=(0.97, 0.955), ncol=2, frameon=False, fontsize=16, handlelength=1.4, handleheight=1.0,
           columnspacing=1.8)

fig.text(0.04, 0.075, 'Measured on a 2880×1800 laptop screen at 200% scaling and 120 Hz, about 125 mouse reports per second, '
         'opacity 92, trail 30 ms, hiding on; 1.1 with "Minimum blur speed" at 10 cm/s.', fontsize=10.5, color=MUTED)
fig.text(0.04, 0.04, 'Full data: github.com/sidlikesgrapess/CursorMotionBlur/blob/main/windhawk/BENCHMARKS.md',
         fontsize=10.5, color=MUTED)
fig.savefig(r'C:\Users\Siddartha\CursorMotionBlur\assets\benchmark-1.1.png', facecolor=SURF)
print('saved')
