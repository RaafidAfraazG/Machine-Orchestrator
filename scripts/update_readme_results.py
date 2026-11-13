#!/usr/bin/env python3
"""Render measured sections only from saved verification evidence."""
import json
from pathlib import Path

root = Path(__file__).resolve().parents[1]
verification = root/'verification'
sanitizers = json.loads((verification/'sanitizers.json').read_text())
bugs = json.loads((verification/'bug_injection.json').read_text())
latency = json.loads((verification/'latency/comparison.json').read_text())
findings = json.loads((verification/'findings.json').read_text())
lines = ['Measured verification results ([JSON evidence](verification/sanitizers.json)):', '',
         '| Build | CTest tests | Fault scenarios | Result |', '|---|---:|---:|---|']
for row in sanitizers['results']:
    lines.append(f"| {row['mode']} | {row.get('tests','unrun')} | {row.get('scenarios','unrun')} | {'PASS' if row['passed'] else 'FAIL'} |")
for row in sanitizers['results']:
    if row.get('repeat_exit_code') == 0:
        lines += ['', f"Additional TSan repeat pass: {row['repeat_selected']} selected tests × {row['repeat']} executions = "
                  f"{row['repeat_selected'] * row['repeat']} passes, including every fault scenario in each repetition."]
lines += ['', 'No sanitizer suppressions are used. Analyzer arithmetic and dropped-event warning checks also run with '
          '`python3 tests/test_latency_analysis.py`.', '',
          f"Scenario-exposed production bugs identified: **{len(findings['production_bugs'])}**. "
          'The initial verification failure was a simulator port collision; its record is retained in '
          '[history](verification/history/initial_sanitizers.json). Test hardening fixed the port lease and acquisition gate; '
          'production changes are the documented injection/measurement hooks and deliberate mutations.', '',
          f"Mutation proof: **{bugs['caught']}/{bugs['injected']} caught**, from [actual runner output](verification/bug_injection.json).", '',
          '| Mutation | Result | Test that failed |', '|---|---|---|']
for row in bugs['results']:
    lines.append(f"| `{row['name']}` | {row['status']} | {', '.join(row['failed_tests']) or 'none'} |")
measurement = ['Measured results ([raw CSV and reports](verification/latency/)):', '',
               '| Requested mode | Samples | Mean ms | p50 ms | p95 ms | p99 ms | Max ms | Jitter ms |',
               '|---|---:|---:|---:|---:|---:|---:|---:|']
for name, row in latency.items():
    s=row['summary']
    if s['count']:
        values=' | '.join(f'{s[k]:.6f}' for k in ('mean','p50','p95','p99','max','jitter'))
        measurement.append(f"| {name} | {s['count']} | {values} |")
    measurement += []
measurement += ['']
for name, row in latency.items():
    measurement.append(f"- {name}: {'; '.join(row['metadata'])}; failed commands={row['failed_commands']}, unfinished={row['unfinished_commands']}, unmatched outcomes={row['unmatched_outcomes']}.")
measurement += ['', 'The FIFO result must be interpreted using the recorded grant/fallback counts, not the requested flag alone.']
readme=(root/'README.md').read_text()
for key, data in [('RUN_RESULTS',lines),('LATENCY_RESULTS',measurement)]:
    start='<!-- '+key+' -->'
    end='<!-- END_'+key+' -->'
    a=readme.index(start)+len(start)
    b=readme.index(end,a)
    readme=readme[:a]+'\n\n'+'\n'.join(data)+'\n\n'+readme[b:]
(root/'README.md').write_text(readme)
