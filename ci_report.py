#!/usr/bin/env python3
"""95% confidence intervals for a hypersim benchmark directory (read-only, analysis only).

Usage: python3 ci_report.py [benchmark_dir]        # default: benchmark_runs/onewebsmall_24h_5x_pdes

For n=5 repetitions per policy the two-sided Student t critical value at 95% is t(0.975,4)=2.7764 and
CI = mean +/- t * s / sqrt(n) with s the sample standard deviation (ddof=1). Deterministic policies have
s=0 for every business statistic, so their interval collapses to a point; only host wall-clock time keeps
a non-zero half-width. The statistic keys are read from each run's result.json plus performance.json.
"""
import collections, glob, json, math, os, re, sys

base = sys.argv[1] if len(sys.argv) > 1 else 'benchmark_runs/onewebsmall_24h_5x_pdes'
# Two-sided 95% Student t critical values by degrees of freedom (n-1); extended lazily for larger n.
T95 = {1: 12.7062, 2: 4.3027, 3: 3.1824, 4: 2.7764, 5: 2.5706, 6: 2.4469, 7: 2.3646, 8: 2.3060,
       9: 2.2622, 10: 2.2281, 15: 2.1314, 20: 2.0860, 30: 2.0423, 60: 2.0003, 120: 1.9799}
FIELDS = [('goodput_bps', 'goodput_bps', '%.4f'), ('delivered_payload_bytes', 'delivered_B', '%.6g'),
          ('offered_payload_bytes', 'offered_B', '%.6g'), ('unserved_payload_bytes', 'unserved_B', '%.6g'),
          ('completed_traceroutes', 'completed_tr', '%.2f'), ('reached_traceroutes', 'reached_tr', '%.2f'),
          ('handover_events', 'ho_events', '%.2f'), ('user_handovers', 'user_ho', '%.2f'),
          ('ground_handovers', 'ground_ho', '%.2f'), ('host_execution_seconds', 'exec_s', '%.2f')]
ORDER = ['baseline', 'random', 'min-access-delay', 'min-full-delay', 'max-min-visibility',
         'min-orbit-hops', 'max-access-rate', 'min-lifetime']


def t95(df):
    if df in T95:
        return T95[df]
    return 1.96 if df > 120 else 2.0  # conservative fallback for df outside the table


def load(base):
    runs = collections.defaultdict(list)
    for mf in sorted(glob.glob(os.path.join(base, 'onewebsmall-*-manifest-*.json'))):
        mm = re.match(r'onewebsmall-(.+)-manifest-(\d+)\.json$', os.path.basename(mf))
        if not mm:
            continue
        pol, idx = mm.group(1), int(mm.group(2))
        r = json.load(open(mf))['result']
        pf = os.path.join(base, 'onewebsmall-%s-performance-%d.json' % (pol, idx))
        perf = json.load(open(pf)) if os.path.exists(pf) else {}
        rec = {k: r.get(k) for k, _, _ in FIELDS}
        rec['host_execution_seconds'] = perf.get('host_execution_seconds', 0.0)
        runs[pol].append(rec)
    return runs


def main():
    runs = load(base)
    if not runs:
        print('no runs found under', base, file=sys.stderr)
        return 1
    present = [p for p in ORDER if p in runs] + [p for p in runs if p not in ORDER]
    print('benchmark : %s' % base)
    print('Student t 95%% CI, interval = mean +/- t*s/sqrt(n) (s = sample stdev, ddof=1)')
    for pol in present:
        v = runs[pol]
        t = t95(len(v) - 1)
        print('\n== %s (n=%d, t=%.4f) ==' % (pol, len(v), t))
        print('%-22s %16s %16s %16s %10s' % ('statistic', 'mean', 'lower95', 'upper95', 'rel.halfwidth%'))
        for key, label, fmt in FIELDS:
            vals = [x[key] for x in v if x[key] is not None]
            if not vals:
                continue
            mean = sum(vals) / len(vals)
            s = math.sqrt(sum((x - mean) ** 2 for x in vals) / (len(vals) - 1)) if len(vals) > 1 else 0.0
            hw = t * s / math.sqrt(len(vals))
            rel = (hw / mean * 100.0) if mean else 0.0
            print('%-22s %16s %16s %16s %10.3f' % (label, fmt % mean, fmt % (mean - hw), fmt % (mean + hw), rel))
    return 0


if __name__ == '__main__':
    sys.exit(main())
