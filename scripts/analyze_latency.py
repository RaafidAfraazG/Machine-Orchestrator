#!/usr/bin/env python3
"""Analyze command_submitted -> response_received; all measurements in ms."""
import argparse
import csv
import json
import math
import os
import tempfile
from pathlib import Path
import statistics


def percentile(values, p):
    if not values:
        return None
    index = (len(values) - 1) * p
    lo, hi = math.floor(index), math.ceil(index)
    return values[lo] + (values[hi] - values[lo]) * (index - lo)


def summary(values):
    values = sorted(values)
    if not values:
        return {'count': 0}
    return dict(count=len(values), mean=statistics.mean(values),
                p50=percentile(values, .50), p95=percentile(values, .95),
                p99=percentile(values, .99), max=max(values), jitter=statistics.pstdev(values))


def analyze(path):
    metadata = []
    with open(path, newline='') as source:
        lines = []
        for line in source:
            if line.startswith('#'):
                metadata.append(line.strip('# \n'))
            else:
                lines.append(line)
    rows = csv.DictReader(lines)
    submitted = {}
    series = []
    failures = 0
    unmatched = 0
    for row in rows:
        command = int(row['command_id'])
        if not command:
            continue  # Probe traffic has no workflow command id.
        key = (int(row['device_id']), command)
        timestamp = int(row['timestamp_ns'])
        if row['type'] == 'command_submitted':
            submitted[key] = timestamp
        elif row['type'] in ('response_received', 'command_failed', 'command_timeout'):
            start = submitted.pop(key, None)
            if start is None:
                unmatched += 1
            elif row['type'] == 'response_received':
                if timestamp < start:
                    raise ValueError('negative command latency')
                series.append((start, key[0], command, (timestamp - start) / 1e6))
            else:
                failures += 1
    series.sort()
    dropped = sum(int(part.split('=', 1)[1]) for part in metadata if part.startswith('dropped_events='))
    result = dict(summary=summary([row[3] for row in series]), dropped_events=dropped,
                  failed_commands=failures, unmatched_outcomes=unmatched,
                  unfinished_commands=len(submitted), metadata=metadata,
                  per_device={str(device): summary([row[3] for row in series if row[1] == device])
                              for device in sorted({row[1] for row in series})})
    return result, series


def report(result, series):
    lines = list(result['metadata'])
    if result['dropped_events']:
        lines.append('WARNING: events were dropped; incomplete samples bias these results.')
    lines.append('Latency ms: command_submitted -> response_received; jitter = population stddev')
    def stats(label, s):
        return label + ' ' + ' '.join(f'{key}={value if key == "count" else f"{value:.6f}"}' for key, value in s.items())
    lines.append(stats('all', result['summary']))
    lines.append(f"failed={result['failed_commands']} unfinished={result['unfinished_commands']} unmatched={result['unmatched_outcomes']}")
    for device, data in result['per_device'].items():
        lines.append(stats('device ' + device, data))
    if series:
        low, high = min(row[3] for row in series), max(row[3] for row in series)
        width = (high - low) / 10 or 1
        bins = [0] * 10
        for row in series:
            bins[min(9, int((row[3] - low) / width))] += 1
        lines.append('Histogram ms (10 equal-width bins):')
        for i, count in enumerate(bins):
            lines.append(f'  {low+i*width:.6f} .. {low+(i+1)*width:.6f}: {count}')
    return '\n'.join(lines) + '\n'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('csv')
    parser.add_argument('--json')
    parser.add_argument('--report')
    parser.add_argument('--series')
    parser.add_argument('--png')
    args = parser.parse_args()
    result, series = analyze(args.csv)
    text = report(result, series)
    print(text, end='')
    if args.report:
        Path(args.report).write_text(text)
    if args.json:
        Path(args.json).write_text(json.dumps(result, indent=2) + '\n')
    if args.series:
        with open(args.series, 'w', newline='') as out:
            writer = csv.writer(out)
            writer.writerow(['elapsed_s', 'device_id', 'command_id', 'latency_ms'])
            for stamp, device, command, value in series:
                writer.writerow([(stamp-series[0][0])/1e9, device, command, value])
    if args.png and series:
        cache = tempfile.mkdtemp(prefix="mo-plot-")
        os.environ.setdefault("MPLCONFIGDIR", cache)
        os.environ.setdefault("XDG_CACHE_HOME", cache)
        try:
            import matplotlib
            matplotlib.use('Agg')
            import matplotlib.pyplot as plt
        except ImportError:
            print('matplotlib unavailable; PNG skipped (CSV time series is still available).')
        else:
            fig, axes = plt.subplots(2, 1, figsize=(9, 6))
            axes[0].hist([row[3] for row in series], bins=40)
            axes[0].set(xlabel='Command latency (ms)', ylabel='Count')
            axes[1].plot([(row[0]-series[0][0])/1e9 for row in series], [row[3] for row in series], '.', markersize=1)
            axes[1].set(xlabel='Elapsed time (s)', ylabel='Command latency (ms)')
            fig.tight_layout()
            fig.savefig(args.png)
            plt.close(fig)


if __name__ == '__main__':
    main()
