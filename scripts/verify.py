#!/usr/bin/env python3
"""Run real builds/tests and persist machine-readable evidence. No assumed passes."""
import argparse
import datetime
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'verification'
OUT.mkdir(exist_ok=True)
LOGS = ROOT / 'build-verification-logs'
LOGS.mkdir(exist_ok=True)
ENV = os.environ.copy()
ENV['TSAN_OPTIONS'] = 'halt_on_error=1:second_deadlock_stack=1'
ENV['UBSAN_OPTIONS'] = 'halt_on_error=1:print_stacktrace=1'
ENV['ASAN_OPTIONS'] = 'halt_on_error=1:detect_leaks=1'


def run(args, log):
    start = time.monotonic()
    with open(log, 'w') as output:
        result = subprocess.run(args, cwd=ROOT, env=ENV, stdout=output, stderr=subprocess.STDOUT)
    return result.returncode, time.monotonic()-start


def configure(build, mode, mutation=''):
    args = ['cmake', '-S', str(ROOT), '-B', str(build), '-DENABLE_ASAN='+('ON' if mode == 'asan-ubsan' else 'OFF'),
            '-DENABLE_UBSAN='+('ON' if mode == 'asan-ubsan' else 'OFF'),
            '-DENABLE_TSAN='+('ON' if mode == 'tsan' else 'OFF'), '-DMO_INJECT_BUG='+mutation]
    # Reuse a verified checkout when present; clean checkouts use FetchContent.
    source = ROOT/'build/_deps/googletest-src'
    if source.exists():
        args.append('-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST='+str(source))
    return args


def inventory(build):
    data = json.loads(subprocess.check_output(['ctest', '--test-dir', str(build), '--show-only=json-v1'], env=ENV))
    return data['tests']


def selected_tests(tests, label):
    return [t for t in tests if any(p['name'] == 'LABELS' and label in p['value'] for p in t['properties'])]


def failed_names(build):
    path = build/'Testing/Temporary/LastTestsFailed.log'
    return [line.split(':', 1)[1] for line in path.read_text().splitlines()] if path.exists() else []


def test_run(build, logfile, *selection):
    stale = build/'Testing/Temporary/LastTestsFailed.log'
    stale.unlink(missing_ok=True)
    code, seconds = run(['ctest', '--test-dir', str(build), '--output-on-failure', '--parallel', '1', *selection], logfile)
    return code, seconds, failed_names(build)


def sanitizers(repeat):
    results = []
    for mode in ('normal', 'asan-ubsan', 'tsan'):
        build = ROOT/('build-verify-'+mode)
        print('Building/testing', mode, flush=True)
        cfg, _ = run(configure(build, mode), LOGS/(mode+'-configure.log'))
        compiled = -1 if cfg else run(['cmake', '--build', str(build), '--target', 'mo_tests', '-j2'], LOGS/(mode+'-build.log'))[0]
        item = dict(mode=mode, configure=cfg, build=compiled, passed=False)
        if compiled == 0:
            tests = inventory(build)
            code, seconds, failed = test_run(build, LOGS/(mode+'-tests.log'))
            reports = re.findall(r'(?:WARNING: ThreadSanitizer|ERROR: (?:AddressSanitizer|LeakSanitizer)|runtime error:)', (LOGS/(mode+'-tests.log')).read_text())
            item['sanitizer_reports'] = len(reports)
            item.update(tests=len(tests), scenarios=len(selected_tests(tests, 'fault')), exit_code=code, seconds=seconds, failures=failed, passed=code == 0 and not reports)
            if mode == 'tsan' and repeat > 1 and code == 0:
                # Entire fault label is included, plus every integration/stress test.
                count = len([t for t in tests if any(p['name']=='LABELS' and set(p['value']) & {'fault','stress','integration'} for p in t['properties'])])
                rc, elapsed, failures = test_run(build, LOGS/'tsan-repeat.log', '-L', 'fault|stress|integration', '--repeat', 'until-fail:'+str(repeat))
                item.update(repeat=repeat, repeat_selected=count, repeat_exit_code=rc, repeat_seconds=elapsed, repeat_failures=failures)
                item['passed'] &= rc == 0
        results.append(item)
        print(mode, 'PASS' if item['passed'] else 'FAIL', 'logs:', LOGS, flush=True)
    data = dict(timestamp_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(), results=results)
    (OUT/'sanitizers.json').write_text(json.dumps(data, indent=2)+'\n')
    print('\nmode          result  test count  scenarios')
    for item in results:
        print(f"{item['mode']:<14}{'PASS' if item['passed'] else 'FAIL':<8}{item.get('tests','unrun')!s:<12}{item.get('scenarios','unrun')}")
    return 0 if all(item['passed'] for item in results) else 1


def mutations():
    results = []
    for bug in json.loads((ROOT/'scripts/bugs.json').read_text()):
        mode = 'tsan' if bug.get('tsan') else 'normal'
        build = ROOT/('build-mutations-'+mode)
        print('Injecting', bug['name'], flush=True)
        cfg, _ = run(configure(build, mode, bug['name']), LOGS/(bug['name']+'-configure.log'))
        compiled = -1 if cfg else run(['cmake', '--build', str(build), '--target', 'mo_tests', '-j2'], LOGS/(bug['name']+'-build.log'))[0]
        item = dict(name=bug['name'], mode=mode, status='error', failed_tests=[])
        if compiled == 0:
            tests = inventory(build)
            matched = [t for t in selected_tests(tests,bug['label']) if t['name'] == bug['test']]
            if matched:
                code, seconds, failed = test_run(build, LOGS/(bug['name']+'-tests.log'), '-L', bug['label'], '-R', '^'+re.escape(bug['test'])+'$')
                item.update(status='caught' if code != 0 and failed else 'missed', failed_tests=failed, exit_code=code, seconds=seconds)
        results.append(item)
        print(item['name'], item['status'], ','.join(item['failed_tests']), flush=True)
    data = dict(timestamp_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(), injected=len(results),
                caught=sum(r['status']=='caught' for r in results), results=results)
    (OUT/'bug_injection.json').write_text(json.dumps(data, indent=2)+'\n')
    print('\nbug                         result    test')
    for item in results:
        print(f"{item['name']:<28}{item['status']:<10}{','.join(item['failed_tests'])}")
    print('Caught',data['caught'],'of',data['injected'])
    return 0 if all(r['status']=='caught' for r in results) else 1


def main():
    p=argparse.ArgumentParser()
    p.add_argument('operation',choices=['sanitizers','mutations'])
    p.add_argument('--repeat',type=int,default=1)
    a=p.parse_args()
    if a.repeat<1:
        p.error('--repeat must be positive')
    return sanitizers(a.repeat) if a.operation=='sanitizers' else mutations()

if __name__=='__main__':
    sys.exit(main())
