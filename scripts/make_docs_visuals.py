#!/usr/bin/env python3
"""Generate documentation diagrams and charts from committed verification data.

Only stdlib + optional matplotlib. No simulator, benchmark, or build is run.
"""
import html
import importlib.util
import json
import os
from pathlib import Path
import tempfile

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'docs/images'
COLORS = {
    'blue': ('#eff6ff', '#2563eb'), 'teal': ('#f0fdfa', '#0f766e'),
    'purple': ('#f5f3ff', '#7c3aed'), 'amber': ('#fffbeb', '#b45309'),
    'red': ('#fff1f2', '#be123c'), 'gray': ('#f8fafc', '#64748b'),
}


class SVG:
    def __init__(self, name, title, width, height):
        self.name = name
        self.parts = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}" role="img" aria-labelledby="title">',
                      f'<title id="title">{html.escape(title)}</title>',
                      '<defs><marker id="arrow" markerWidth="10" markerHeight="10" refX="9" refY="5" orient="auto-start-reverse"><path d="M0 0 L10 5 L0 10 Z" fill="#475569"/></marker></defs>',
                      f'<rect x="2" y="2" width="{width-4}" height="{height-4}" rx="20" fill="white" stroke="#e2e8f0"/>',
                      '<g font-family="DejaVu Sans, sans-serif" fill="#0f172a">']
        self.text(width/2, 40, title, size=27, weight='bold')

    def text(self, x, y, value, size=20, weight='normal', anchor='middle', color='#0f172a'):
        self.parts.append(f'<text x="{x}" y="{y}" text-anchor="{anchor}" font-size="{size}" font-weight="{weight}" fill="{color}">{html.escape(value)}</text>')

    def box(self, x, y, w, h, title, detail='', color='blue'):
        fill, stroke = COLORS[color]
        self.parts.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="14" fill="{fill}" stroke="{stroke}" stroke-width="2"/>')
        lines = detail.split('\n') if detail else []
        first = y + h/2 - (len(lines)*12) + 7
        self.text(x+w/2, first, title, 21, 'bold')
        for i, line in enumerate(lines):
            self.text(x+w/2, first+26*(i+1), line, 17, color='#334155')

    def arrow(self, points, dashed=False, color='#475569'):
        path = 'M' + ' L'.join(f'{x},{y}' for x, y in points)
        dash = ' stroke-dasharray="7 6"' if dashed else ''
        self.parts.append(f'<path d="{path}" fill="none" stroke="{color}" stroke-width="2"{dash} marker-end="url(#arrow)"/>')

    def panel(self, x, y, w, h, label):
        self.parts.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="16" fill="#fafcff" stroke="#cbd5e1"/>')
        self.text(x+20, y+29, label, 18, 'bold', 'start', '#475569')

    def save(self):
        (OUT/self.name).write_text('\n'.join(self.parts+['</g></svg>'])+'\n')


def diagrams():
    s = SVG('architecture.svg', 'MachineOrchestrator architecture', 1200, 870)
    s.panel(30, 175, 1140, 550, 'Engine owns the runtime')
    # Dashed audit bus is routed through gutters behind the nodes.
    for points in [[(80,270),(60,270),(60,695),(990,695),(990,660)],
                   [(430,270),(405,270),(405,695)],
                   [(860,270),(835,270),(835,695)],
                   [(860,460),(835,460)], [(430,460),(405,460)],
                   [(80,460),(60,460)], [(430,615),(405,615)]]:
        s.arrow(points, True, '#94a3b8')
    s.text(150, 715, 'All component events feed the log', 15, anchor='start', color='#64748b')
    s.box(430, 75, 280, 70, 'Workflows submitted', 'Application / completion futures', 'gray')
    s.box(80, 225, 250, 90, 'Priority scheduler', 'Higher priority first\nFIFO ties + aging', 'purple')
    s.box(430, 225, 280, 90, 'Worker pool', 'Async workflow steps', 'purple')
    s.box(860, 225, 260, 90, 'Resource manager', 'Ordered device locks', 'teal')
    s.box(80, 410, 250, 100, 'Watchdogs', 'Reactor timers\nIsolation + recovery', 'amber')
    s.box(430, 410, 280, 100, 'Reactor thread', 'Level-triggered epoll\nOwns device I/O', 'blue')
    s.box(860, 410, 260, 100, 'Deadlock detector', 'Wait-for graph\nVictim selection', 'teal')
    s.box(430, 570, 280, 90, 'Devices', 'TCP / serial transports', 'blue')
    s.box(860, 570, 260, 90, 'Event log', 'Bounded ring + CSV\nDropped-event counter', 'gray')
    s.box(430, 770, 280, 70, 'Simulator', 'Asyncio TCP / PTY', 'gray')
    for points in [[(430,110),(205,110),(205,225)], [(330,270),(430,270)],
                   [(710,270),(860,270)], [(990,315),(990,410)],
                   [(570,315),(570,410)], [(430,460),(330,460)],
                   [(570,510),(570,570)], [(570,660),(570,770)]]:
        s.arrow(points)
    s.text(785, 253, 'acquire', 17)
    s.text(1060, 370, 'wait edges', 17)
    s.text(620, 368, 'post()', 17)
    s.text(380, 443, 'timers', 17)
    s.text(650, 746, 'MBAP / RTU-style frames', 17)
    s.save()

    s = SVG('threading_model.svg', 'A workflow step crosses threads without blocking a worker', 1200, 480)
    s.panel(30, 80, 1140, 140, 'Worker pool')
    s.panel(30, 270, 1140, 140, 'Reactor thread')
    s.box(70, 130, 260, 65, 'Advance workflow', 'Dispatch next step', 'purple')
    s.box(450, 130, 260, 65, 'Post request', 'Worker returns to queue', 'purple')
    s.box(860, 130, 260, 65, 'Continuation', 'Enqueue at workflow priority', 'purple')
    s.box(450, 320, 260, 65, 'Device I/O', 'Nonblocking send / receive', 'blue')
    s.box(860, 320, 260, 65, 'Device callback', 'Response / timeout / timer', 'blue')
    for points in [[(330,162),(450,162)],[(580,195),(580,320)],
                   [(710,352),(860,352)],[(990,320),(990,195)]]:
        s.arrow(points)
    s.text(630, 251, 'post()', 18)
    s.text(785, 335, 'complete', 17)
    s.text(1070, 251, 'enqueue()', 18)
    s.text(600, 452, 'No worker waits for device I/O; epoll and timers wake the reactor.', 20, color='#0f766e')
    s.save()

    s = SVG('workflow_lifecycle.svg', 'Workflow lifecycle', 1200, 510)
    for args in [(60,90,200,80,'Pending','Submitted','gray'),
                 (380,90,200,80,'Running','Advance steps','purple'),
                 (900,90,220,80,'Completed','All steps done','teal'),
                 (60,310,200,80,'Blocked','Waiting for locks','amber'),
                 (560,310,220,80,'Failed','Reason + failing step','red'),
                 (900,310,220,80,'Aborted','Reason + failing step','red')]:
        s.box(*args)
    for points in [[(260,130),(380,130)],[(580,130),(900,130)],[(160,170),(160,310)],
                   [(480,170),(480,235),(160,235),(160,310)],
                   [(260,350),(330,350),(330,130),(380,130)],
                   [(540,170),(670,310)],[(260,350),(560,350)],
                   [(580,155),(835,155),(835,350),(900,350)],
                   [(160,390),(160,445),(1010,445),(1010,390)]]:
        s.arrow(points)
    s.text(320, 111, 'dispatch', 17)
    s.text(740, 111, 'all steps succeed', 17)
    s.text(215, 220, 'lock wait', 17)
    s.text(95, 274, 'initial wait', 16)
    s.text(290, 275, 'locks', 16)
    s.text(290, 295, 'granted', 16)
    s.text(705, 228, 'step failure', 17)
    s.text(705, 252, 'DeviceFaulted', 17)
    s.text(410, 335, 'DeviceFaulted', 17)
    s.text(845, 260, 'stop / abort', 16, anchor='start')
    s.text(590, 474, 'deadlock victim / stop', 18)
    s.save()

    s = SVG('deadlock_resolution.svg', 'Deadlock resolution in test-only UNCHECKED mode', 1200, 470)
    s.panel(30, 75, 520, 325, 'Before: wait-for cycle A → B → A')
    s.panel(650, 75, 520, 325, 'After: victim aborted, locks released')
    s.box(65, 135, 190, 65, 'Workflow A', 'Higher priority', 'purple')
    s.box(320, 135, 190, 65, 'Workflow B', 'Lower priority: victim', 'red')
    s.box(65, 310, 190, 65, 'Device A', 'Held by A', 'teal')
    s.box(320, 310, 190, 65, 'Device B', 'Held by B', 'teal')
    for points in [[(160,310),(160,200)],[(415,310),(415,200)],
                   [(245,200),(385,310)],[(330,200),(190,310)]]:
        s.arrow(points)
    s.text(112, 260, 'holds', 16)
    s.text(467, 260, 'holds', 16)
    s.text(290, 285, 'waits', 17)
    s.arrow([(565,240),(635,240)])
    s.text(600, 215, 'resolve', 16)
    s.box(685, 145, 205, 80, 'Workflow A', 'Can continue', 'purple')
    s.box(945, 145, 190, 80, 'Device B', 'Available to A', 'teal')
    s.box(685, 310, 205, 65, 'Device A', 'Still held by A', 'teal')
    s.box(945, 310, 190, 65, 'Workflow B', 'Aborted: Deadlock', 'red')
    s.arrow([(945,185),(890,185)])
    s.arrow([(787,310),(787,225)])
    s.arrow([(1040,310),(1040,225)])
    s.text(1110, 272, 'release', 16)
    s.text(738, 272, 'kept', 16)
    s.text(600, 442, 'Victim policy: lowest priority; youngest submission breaks a tie.', 20)
    s.save()

    s = SVG('watchdog_states.svg', 'Per-device watchdog health', 1200, 350)
    for args in [(50,95,210,85,'Healthy','Requests succeed','teal'),
                 (340,95,210,85,'Suspect','Failures counted','amber'),
                 (630,95,210,85,'Faulted','Device isolated','red'),
                 (920,95,230,85,'Recovering','Reconnect + probe','blue')]:
        s.box(*args)
    for points in [[(260,137),(340,137)],[(550,137),(630,137)],[(840,137),(920,137)],
                   [(445,180),(445,220),(155,220),(155,180)],
                   [(1070,180),(1070,230),(735,230),(735,180)],
                   [(1035,230),(1035,290),(155,290),(155,220)]]:
        s.arrow(points)
    s.text(300, 119, 'failure', 16)
    s.text(590, 119, 'threshold', 15)
    s.text(880, 119, 'backoff', 16)
    s.text(300, 247, 'response succeeds', 16)
    s.text(880, 252, 'probe fails; retry backoff', 16)
    s.text(600, 322, 'probe succeeds → Healthy; allow new workflows', 19)
    s.save()


def charts():
    # Reuse the exact verified command matching and latency definition.
    spec = importlib.util.spec_from_file_location('mo_analysis', ROOT/'scripts/analyze_latency.py')
    analysis = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(analysis)
    samples = {name: analysis.analyze(ROOT/f'verification/latency/{name}.csv') for name in ('normal','fifo')}
    proof = json.loads((ROOT/'verification/bug_injection.json').read_text())
    os.environ.setdefault('MPLCONFIGDIR', tempfile.mkdtemp(prefix='mo-docs-plot-'))
    os.environ.setdefault('XDG_CACHE_HOME', os.environ['MPLCONFIGDIR'])
    try:
        import matplotlib
        matplotlib.use('Agg')
        import matplotlib.pyplot as plt
    except ImportError:
        print('matplotlib unavailable: writing SVG data-chart fallbacks instead of PNG.')
        fallback_charts(samples, proof)
        return
    plt.rcParams.update({'font.family':'DejaVu Sans','font.size':11,'axes.spines.top':False,'axes.spines.right':False,'figure.facecolor':'white','axes.facecolor':'white'})
    colors = {'normal':'#2563eb','fifo':'#0f766e'}
    labels = {'normal':'Normal','fifo':'FIFO requested (fell back)'}
    counts = '; '.join(f'{labels[name]} n={len(series):,}' for name, (_,series) in samples.items())
    upper = max(row[3] for _, series in samples.values() for row in series)
    bins = [upper*i/40 for i in range(41)]
    fig, ax = plt.subplots(figsize=(10,4.6),layout='constrained')
    for name, (_,series) in samples.items():
        ax.hist([row[3] for row in series],bins=bins,color=colors[name],alpha=.55,label=labels[name])
    ax.set(title='Command latency distribution\n'+counts,xlabel='Command latency (ms)',ylabel='Commands (count)')
    ax.legend();ax.grid(axis='y',alpha=.18)
    fig.savefig(OUT/'latency_histogram.png',dpi=160);plt.close(fig)
    fig, ax = plt.subplots(figsize=(10,4.6),layout='constrained')
    for name, (_,series) in samples.items():
        ax.scatter([(r[0]-series[0][0])/1e9 for r in series],[r[3] for r in series],s=4,alpha=.4,color=colors[name],label=labels[name])
    ax.set(title='Command latency over each run\n'+counts,xlabel='Elapsed time within run (s)',ylabel='Command latency (ms)')
    ax.legend(loc='upper center',bbox_to_anchor=(.5,-.17),ncol=2,frameon=False);ax.grid(alpha=.18)
    fig.savefig(OUT/'latency_timeseries.png',dpi=160);plt.close(fig)
    rows = proof['results']
    fig, ax = plt.subplots(figsize=(10,7.6),layout='constrained')
    positions = list(range(len(rows)))
    ax.barh(positions,[int(row['status']=='caught') for row in rows],color=['#0f766e' if row['status']=='caught' else '#be123c' for row in rows],height=.65)
    ax.set_yticks(positions,[row['name'] for row in rows]);ax.invert_yaxis()
    ax.set_xticks([0,1],['Not caught','Caught']);ax.set_xlim(0,1.23)
    ax.set_title(f"Mutation testing: {proof['caught']}/{proof['injected']} caught",pad=15)
    for y, row in enumerate(rows):
        ax.text(1.03,y,row['status'],va='center',fontsize=10)
    ax.grid(axis='x',alpha=.15)
    fig.savefig(OUT/'mutation_results.png',dpi=160);plt.close(fig)
    print('Charts generated from committed CSV and mutation JSON; no new measurements taken.')


def fallback_charts(samples, proof):
    # Standalone SVG fallbacks remain readable without an optional dependency.
    for kind in ('histogram','timeseries'):
        s = SVG('latency_'+kind+'.svg', 'Command latency — '+kind, 1200, 480)
        s.text(600,75,'Samples: '+', '.join(f'{n} n={len(v[1])}' for n,v in samples.items()),18)
        upper=max(r[3] for _,series in samples.values() for r in series)
        s.arrow([(100,100),(100,390),(1120,390)])
        if kind=='histogram':
            groups={}
            for name,(_,series) in samples.items():
                groups[name]=[0]*30
                for row in series:groups[name][min(29,int(row[3]/upper*30))]+=1
            ceiling=max(max(v) for v in groups.values())
            for name,bins in groups.items():
                for i,n in enumerate(bins):
                    height=n/ceiling*270
                    s.parts.append(f'<rect x="{102+i*33}" y="{390-height}" width="30" height="{height}" fill="{"#2563eb" if name=="normal" else "#0f766e"}" opacity="0.5"/>')
            s.text(600,435,f'Command latency (ms): 0 to {upper:.6f}; y = command count',18)
        else:
            duration=max((series[-1][0]-series[0][0])/1e9 for _,series in samples.values())
            for name,(_,series) in samples.items():
                for stamp,_,_,value in series:
                    x=100+(stamp-series[0][0])/1e9/duration*1000;y=390-value/upper*270
                    s.parts.append(f'<circle cx="{x}" cy="{y}" r="1.5" fill="{"#2563eb" if name=="normal" else "#0f766e"}" opacity="0.4"/>')
            s.text(600,435,f'Elapsed time (s): 0 to {duration:.6f}; y = command latency (ms)',18)
        s.text(600,465,'Blue: normal; teal: FIFO requested (fell back)',17);s.save()
    s = SVG('mutation_results.svg', f"Mutation testing: {proof['caught']}/{proof['injected']} caught",1000,800)
    for i,row in enumerate(proof['results']):
        s.text(55,95+i*42,row['name'],20,anchor='start')
        s.text(800,95+i*42,row['status'],20,color='#0f766e')
    s.save()


if __name__ == '__main__':
    OUT.mkdir(parents=True,exist_ok=True)
    diagrams()
    charts()
