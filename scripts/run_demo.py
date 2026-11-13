#!/usr/bin/env python3
"""Run a demo against a temporary simulator and always reap the simulator."""
import argparse
from pathlib import Path
import selectors
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser()
p.add_argument('--build', default='build')
p.add_argument('--devices', type=int, default=40)
p.add_argument('--base-port', type=int, default=15020)
a = p.parse_args()
with subprocess.Popen([sys.executable, str(root / 'sim/device_sim.py'),
                       '--tcp-count', str(a.devices), '--base-port', str(a.base_port)],
                      stdout=subprocess.PIPE, text=True) as sim:
    try:
        with selectors.DefaultSelector() as selector:
            selector.register(sim.stdout, selectors.EVENT_READ)
            if not selector.select(5) or sim.stdout.readline().strip() != 'READY':
                raise RuntimeError('simulator failed to start')
        status = subprocess.run([str(root / a.build / 'mo_demo'), '--devices', str(a.devices),
                                 '--base-port', str(a.base_port)], timeout=15).returncode
    finally:
        sim.terminate()
        sim.wait(timeout=5)
sys.exit(status)
