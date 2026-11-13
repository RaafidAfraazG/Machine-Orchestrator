#!/usr/bin/env python3
import argparse
import json
from pathlib import Path
import selectors
import subprocess
import sys
from analyze_latency import analyze, report

root = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser()
p.add_argument('--build', default='build')
p.add_argument('--devices', type=int, default=40)
p.add_argument('--workflows', type=int, default=500)
p.add_argument('--duration', type=float, default=1)
p.add_argument('--base-port', type=int, default=15020)
p.add_argument('--output', default='verification/latency')
a = p.parse_args()
out = root / a.output
out.mkdir(parents=True, exist_ok=True)
subprocess.run(['cmake', '-S', str(root), '-B', str(root/a.build)], check=True)
subprocess.run(['cmake', '--build', str(root/a.build), '--target', 'mo_bench_latency', '-j2'], check=True)
reports = []
results = {}
for name in ('normal', 'fifo'):
    with subprocess.Popen([sys.executable, str(root/'sim/device_sim.py'), '--tcp-count', str(a.devices),
                           '--base-port', str(a.base_port), '--latency-ms', '0', '--jitter-ms', '0'],
                          stdout=subprocess.PIPE, text=True) as sim:
        try:
            with selectors.DefaultSelector() as selector:
                selector.register(sim.stdout, selectors.EVENT_READ)
                if not selector.select(10) or sim.stdout.readline().strip() != 'READY':
                    raise RuntimeError('simulator failed to start')
            args = [str(root/a.build/'mo_bench_latency'), '--devices', str(a.devices), '--workflows', str(a.workflows),
                    '--duration', str(a.duration), '--base-port', str(a.base_port), '--csv', str(out/(name+'.csv'))]
            if name == 'fifo':
                args += ['--fifo']
            subprocess.run(args, check=True, timeout=max(60, a.duration+30))
        finally:
            sim.terminate()
            sim.wait(timeout=5)
    subprocess.run([sys.executable, str(root/'scripts/analyze_latency.py'), str(out/(name+'.csv')),
                    '--json', str(out/(name+'.json')), '--report', str(out/(name+'.txt')),
                    '--series', str(out/(name+'-series.csv')), '--png', str(out/(name+'.png'))], check=True)
    result, series = analyze(out/(name+'.csv'))
    results[name] = result
    reports.append(report(result, series).splitlines())
(out/'comparison.json').write_text(json.dumps(results, indent=2)+'\n')
print('\nNORMAL' + ' '*80 + 'SCHED_FIFO REQUESTED')
for i in range(max(map(len, reports))):
    left = reports[0][i] if i < len(reports[0]) else ''
    right = reports[1][i] if i < len(reports[1]) else ''
    print(f'{left:<100} | {right}')
