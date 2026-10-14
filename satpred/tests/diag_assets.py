#!/usr/bin/env python3
"""In-memory builders/parsers for the satpred appendix-D diagnostic protocol and
external-data fixtures. Used by regression.py; no binary assets are committed."""
import json, pathlib, struct

RUN_START_NS = 1767225600 * 10**9  # 2026-01-01T00:00:00Z
_TAB = []
for _i in range(256):
    _c = _i
    for _ in range(8):
        _c = (_c >> 1) ^ (0x82F63B78 if _c & 1 else 0)
    _TAB.append(_c)


def crc32c(data: bytes) -> int:
    """CRC-32C as implemented by core.cpp crc(): reflected poly 0x82f63b78, init/xorout all-ones."""
    c = 0xFFFFFFFF
    for b in data:
        c = _TAB[(c ^ b) & 255] ^ (c >> 8)
    return (~c) & 0xFFFFFFFF


def _put(b: bytearray, o: int, v: int, n: int) -> None:
    b[o:o + n] = int(v).to_bytes(n, 'little')


def _header(magic: str, payload: bytes, extra) -> bytes:
    h = bytearray(64)
    h[0:len(magic)] = magic.encode()
    _put(h, 8, 1, 2)      # version
    _put(h, 10, 0, 2)     # flags
    _put(h, 12, 64, 4)    # header size
    extra(h, payload)
    _put(h, 60, crc32c(bytes(h[:60])), 4)
    return bytes(h)


def write_states(path, states) -> None:
    """ORBSTAT1: 64B header + N x 88B records (6 state doubles + 5 phys doubles)."""
    recs = bytearray()
    for q, phys in states:
        recs += struct.pack('<11d', *(list(q) + list(phys)))
    n = len(states)

    def extra(h, payload):
        _put(h, 16, n, 4)
        _put(h, 20, 88, 4)
        _put(h, 24, len(payload), 8)
        _put(h, 32, crc32c(bytes(payload)), 4)
    pathlib.Path(path).write_bytes(_header('ORBSTAT1', recs, extra) + recs)


def write_env(path, nodes) -> None:
    """ORBENV01: 64B header + nc x 248B nodes (31 doubles)."""
    payload = bytearray()
    for z in nodes:
        assert len(z) == 31
        payload += struct.pack('<31d', *z)
    nc = len(nodes)

    def extra(h, p):
        _put(h, 16, nc, 8)
        _put(h, 24, 248, 4)
        _put(h, 28, 0, 4)
        _put(h, 32, len(p), 8)
        _put(h, 40, crc32c(bytes(p)), 4)
    pathlib.Path(path).write_bytes(_header('ORBENV01', payload, extra) + payload)


def default_states(n=2):
    out = []
    for i in range(n):
        pos = (7.0e6, 0.0, 0.0) if i == 0 else (0.0, 7.0e6, 0.0)
        vel = (0.0, 7500.0, 0.0) if i == 0 else (-7500.0, 0.0, 0.0)
        out.append((pos + vel, (150.0, 6.0, 2.2, 6.0, 1.3)))
    return out


def default_env(rels=(0.0, 30.0, 120.0)):
    out = []
    for rel in rels:
        z = [0.0] * 31
        z[0] = rel
        z[1:10] = [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0]  # R = identity
        z[19:22] = [1.496e11, 0.0, 0.0]      # sun
        z[22:25] = [3.844e8, 0.0, 0.0]       # moon
        z[25] = -1.0                          # rho override disabled
        z[26:29] = [1.08262617385222e-3, -2.53241051856772e-6, -1.61989759991697e-6]
        z[29] = 1.0                           # density_scale
        out.append(z)
    return out


def build_diag(dirpath, *, m=3, step_ns=10**9, max_h_ns=10**8, flags=15, global_index0=0,
               run_start_ns=RUN_START_NS, states=None, nodes=None, prior_failures=(),
               request_name='request.json'):
    """Write ORBSTAT1/ORBENV01 + request.json into dirpath and return the request path."""
    d = pathlib.Path(dirpath)
    d.mkdir(parents=True, exist_ok=True)
    states = default_states() if states is None else states
    nodes = default_env() if nodes is None else nodes
    write_states(d / 'states.sbin', states)
    write_env(d / 'env.sbin', nodes)
    req = {
        'format': 'orbit-engine-request-1',
        'run_start_ns': run_start_ns,
        'window_t0_ns': run_start_ns + global_index0 * step_ns,
        'step_ns': step_ns,
        'max_h_ns': max_h_ns,
        'global_index0': global_index0,
        'M': m, 'N': len(states), 'batch': 1, 'flags': flags,
        'states_file': 'states.sbin', 'env_file': 'env.sbin',
        'prior_failures': list(prior_failures),
    }
    p = d / request_name
    p.write_text(json.dumps(req))
    return p


def parse_orbst01(raw: bytes) -> dict:
    """Validate an ORBST01 stream (header + slice frames + result frame, CRC32C) and return its content."""
    def g(o, n):
        assert o + n <= len(raw), ('truncated stream', o, n, len(raw))
        return int.from_bytes(raw[o:o + n], 'little')

    assert len(raw) >= 64 and raw[0:8].rstrip(b'\x00') == b'ORBST01', ('magic', raw[:8])
    assert (g(8, 2), g(10, 2), g(12, 4)) == (1, 0, 64), 'stream header version/flags/size'
    N, Ns, M = g(16, 4), g(20, 4), g(24, 4)
    assert g(28, 4) == 6, 'component count'
    assert g(60, 4) == crc32c(raw[:60]), 'stream header crc'
    out = dict(N=N, Ns=Ns, M=M, t0_ns=g(32, 8), step_ns=g(40, 8), global_index0=g(48, 8), slices=[], result=None)
    pos = 64
    for expect in range(M + 1):
        kind, psize, seq = g(pos, 4), g(pos + 8, 8), g(pos + 16, 8)
        assert seq == expect, ('frame sequence', seq, expect)
        assert g(pos + 4, 4) == 0, 'frame reserved'
        payload = raw[pos + 32:pos + 32 + psize]
        assert len(payload) == psize, 'truncated payload'
        assert g(pos + 24, 4) == crc32c(payload), 'payload crc'
        assert g(pos + 28, 4) == crc32c(raw[pos:pos + 28]), 'frame crc'
        if kind == 1:
            assert psize == 16 + 6 * Ns * 8, ('slice payload size', psize)
            q = [struct.unpack_from('<d', payload, 16 + c * Ns * 8)[0] for c in range(6)]
            out['slices'].append(dict(t_ns=g(pos + 32, 8), global_index=g(pos + 40, 8), q=q))
        elif kind == 2:
            assert seq == M, 'result frame must be last'
            out['result'] = dict(N=g(pos + 32, 4), failures=g(pos + 36, 4),
                                 json=payload[32 + N * 48:].decode())
        else:
            raise AssertionError(('unexpected frame kind', kind))
        pos += 32 + psize
    assert pos == len(raw), ('trailing bytes', pos, len(raw))
    return out
