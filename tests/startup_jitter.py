#!/usr/bin/env python3
"""Startup-jitter test suite (opt-in `--tool-start-jitter enabled`).

The three tools (business / traceroute / statistics) are not started at nanosecond-precise
simultaneity: when the option is enabled each tool receives an independent random 0..5 s startup
offset derived from the system clock (nanosecond resolution). The default (`disabled`) leaves the
schedule untouched, so the committed baseline output tests stay valid.

Checks
  J1  disabled -> tool_start_offsets_ns == {0,0,0} and two disabled runs are byte-identical.
  J2  enabled  -> each offset is a nanosecond value in [0, 5e9]; at least one is not second-aligned.
  J3  enabled  -> offsets are derived from the system clock, so two runs differ (non-deterministic).
  J4  enabled  -> the business flow starts at begin+offset_business: offered_payload_bytes tracks
                 rate*(duration-offset_business) instead of rate*duration.
  J5  enabled  -> the first statistics interval starts at begin+offset_statistics.
  J6  enabled  -> the first traceroute round is issued at begin+offset_traceroute.
  J7  enabled  -> validation rejects an unknown --tool-start-jitter value (Arg).
"""
import glob, json, pathlib, subprocess, sys, tempfile
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from fixtures import make

bin = pathlib.Path(sys.argv[1]).resolve()
base = pathlib.Path(sys.argv[2] if len(sys.argv) > 2 else 'test_runs'); base.mkdir(parents=True, exist_ok=True)
tmp = pathlib.Path(tempfile.mkdtemp(prefix='startup_jitter_', dir=base))
sx = '.exe' if sys.platform == 'win32' else ''
hypersim = str(bin / ('hypersim' + sx))

bi = lambda a, b, d, bw: [(a, b, d, bw, d * 299.792458), (b, a, d, bw, d * 299.792458)]
edges = bi(0, 1, 2000, 10000000) + bi(1, 2, 2000, 10000000) + bi(3, 0, 5000, 40000000) + bi(2, 4, 4000, 50000000)
inp = make(tmp / 'input', [list(edges)] * 6, ground=[('u1', 1), ('gw1', 2)])   # 6 x 10 s = 60 s
DURATION_S = 60
RATE_BPS = 1000000


def run(name, extra):
    out = tmp / name
    p = subprocess.run([hypersim, str(inp), '--duration', str(DURATION_S), '--probe-interval-s', '10',
                        *extra, '--output', str(out)], capture_output=True, text=True, timeout=600)
    assert p.returncode == 0, (name, p.returncode, p.stderr)
    return out


def result(out):
    return json.load(open(glob.glob(str(out / '*-result-1.json'))[0]))


def first_statistics_interval_start(out):
    f = glob.glob(str(out / '*-statistics-1.jsonl'))[0]
    return json.loads(open(f).readline())['interval_start_ns']


def first_trace_issued(out):
    f = glob.glob(str(out / '*-traceroute-1.jsonl'))[0]
    return json.loads(open(f).readline())['issued_ns']


# ---- J1: disabled keeps the committed baseline schedule (all offsets zero, deterministic) ----
off_a = run('off_a', [])
off_b = run('off_b', [])
ra, rb = result(off_a), result(off_b)
assert ra['tool_start_offsets_ns'] == {'business': 0, 'traceroute': 0, 'statistics': 0}, ra['tool_start_offsets_ns']
assert rb['tool_start_offsets_ns'] == {'business': 0, 'traceroute': 0, 'statistics': 0}, rb['tool_start_offsets_ns']
for key in ('offered_payload_bytes', 'delivered_payload_bytes', 'goodput_bps', 'completed_traceroutes', 'reached_traceroutes'):
    assert ra[key] == rb[key], ('disabled runs must be identical', key, ra[key], rb[key])

# ---- J2/J3: enabled derives a nanosecond 0..5 s offset per tool from the system clock ----
on_a = run('on_a', ['--tool-start-jitter', 'enabled'])
on_b = run('on_b', ['--tool-start-jitter', 'enabled'])
oa, ob = result(on_a)['tool_start_offsets_ns'], result(on_b)['tool_start_offsets_ns']
for run_name, offs in (('on_a', oa), ('on_b', ob)):
    assert set(offs) == {'business', 'traceroute', 'statistics'}, (run_name, offs)
    for tool, v in offs.items():
        assert isinstance(v, int) and 0 <= v <= 5000000000, (run_name, tool, v)
assert any(v % 1000000000 for v in oa.values()), ('nanosecond resolution', oa)
assert oa != ob, ('offsets come from the system clock, two runs must differ', oa, ob)

# ---- J4: the business flow starts at begin+offset_business ----
begin = ra['start_ns']
def expected_offered(off_b):
    seconds = DURATION_S - off_b / 1e9
    return RATE_BPS / 8.0 * seconds
for run_name, offs in (('on_a', oa), ('on_b', ob)):
    off = result(on_a if run_name == 'on_a' else on_b)
    want = expected_offered(offs['business'])
    got = off['offered_payload_bytes']
    assert abs(got - want) <= RATE_BPS / 8.0 * 0.5, ('offered must track begin+offset_business', run_name, got, want)
    assert got < ra['offered_payload_bytes'], ('jitter must delay the flow', run_name, got, ra['offered_payload_bytes'])

# ---- J5: first statistics interval starts at begin+offset_statistics ----
assert first_statistics_interval_start(on_a) - begin == oa['statistics'], 'first statistics interval'
assert first_statistics_interval_start(off_a) - begin == 0, 'disabled first statistics interval'

# ---- J6: first traceroute round issued at begin+offset_traceroute ----
assert first_trace_issued(on_a) - begin == oa['traceroute'], 'first traceroute issue time'
assert first_trace_issued(off_a) - begin == 0, 'disabled first traceroute issue time'

# ---- J7: unknown enum value is rejected (Arg) ----
bad = subprocess.run([hypersim, str(inp), '--tool-start-jitter', 'maybe', '--output', str(tmp / 'bad')],
                     capture_output=True, text=True, timeout=120)
assert bad.returncode == 1, ('unknown jitter value must be Arg', bad.returncode, bad.stderr)

print(json.dumps(dict(status='PASS', tests=['J1', 'J2', 'J3', 'J4', 'J5', 'J6', 'J7'],
                      disabled_offsets=ra['tool_start_offsets_ns'], run_a_offsets=oa, run_b_offsets=ob,
                      offered_disabled=ra['offered_payload_bytes'], offered_run_a=result(on_a)['offered_payload_bytes'],
                      offered_run_b=result(on_b)['offered_payload_bytes'], workdir=str(tmp))))
