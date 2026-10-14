#!/usr/bin/env python3
"""Run every released test suite against an existing build directory and write a JSON summary."""
import argparse, json, subprocess, sys
from pathlib import Path
ap = argparse.ArgumentParser()
ap.add_argument('bin', nargs='?', default='build/bin')
ap.add_argument('--reports', default='build/verification')
ap.add_argument('--skip-default-flow', action='store_true', help='skip the ~2 minute clean-environment 24h run')
args = ap.parse_args()
root = Path(__file__).resolve().parent; bin = Path(args.bin).resolve(); rep = Path(args.reports).resolve(); rep.mkdir(parents=True, exist_ok=True)
sx = '.exe' if sys.platform == 'win32' else ''
cases = [('options_and_event_order', [bin/('test_options'+sx)]),
         ('traceroute_unit', [bin/('test_traceroute'+sx)]),
         ('hash_time_selftest', [bin/('hypersim'+sx), '--selftest']),
         ('upstream_regression', [sys.executable, root/'satpred/tests/regression.py', bin]),
         ('unified_integration', [sys.executable, root/'tests/regression.py', bin, rep/'integration'])]
if not args.skip_default_flow:
    cases.append(('default_flow_clean_env', [sys.executable, root/'tests/default_flow.py', bin, rep/'default']))
results = []
for name, cmd in cases:
    p = subprocess.run(list(map(str, cmd)), capture_output=True, text=True)
    (rep/(name+'.stdout.txt')).write_text(p.stdout); (rep/(name+'.stderr.txt')).write_text(p.stderr)
    results.append(dict(name=name, exit=p.returncode, status='PASS' if p.returncode == 0 else 'FAIL'))
    (rep/'summary.json').write_text(json.dumps(results, indent=2))
    if p.returncode: print(name, 'FAILED\n', p.stderr, file=sys.stderr); sys.exit(p.returncode)
print(json.dumps(results, indent=2))
