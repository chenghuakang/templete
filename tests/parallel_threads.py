#!/usr/bin/env python3
"""VT01-style checks for the parallel packet scheduler (SDD v2.3 §1.4 / §7.4 / §8.4 / §11).

Properties under test:
  * The worker count is auto-detected from the virtual processor count (os.cpu_count());
    HSN_SIM_THREADS pins it (1/2/4/...) for the equivalence checks.
  * Nodes are assigned to workers round-robin by their label, owner(n) = n % T, so adjacent
    node ids never share a worker when T > 1.
  * 1/2/4/auto threads produce byte-identical business/statistics/traceroute/result files and
    the same packet accounting, i.e. the parallel commit order cannot change a committed value.
HSN_SIM_THREADS_DEBUG=1 only adds one diagnostic line on stderr; the output format is untouched.
"""
import json,os,pathlib,subprocess,sys,tempfile
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parent))
from fixtures import make
bin=pathlib.Path(sys.argv[1]).resolve();base=pathlib.Path(sys.argv[2] if len(sys.argv)>2 else 'test_runs');base.mkdir(parents=True,exist_ok=True)
tmp=pathlib.Path(tempfile.mkdtemp(prefix='parallel_threads_',dir=base));sx='.exe' if sys.platform=='win32' else ''
e=[(0,1,1000,40000000,299792.458),(1,0,1000,20000000,299792.458),(1,2,2000,10000000,599584.916)]
inp=tmp/'d1';make(inp,[e,e[:2]])
def parse(stderr):
    line=next(l for l in stderr.splitlines() if l.startswith('hsn-sim-threads '))
    threads=int(line.split('threads=',1)[1].split()[0])
    owner={int(k):int(v) for k,v in (p.split(':') for p in line.split('map=',1)[1].strip().split(','))}
    return threads,owner
def run(name,threads):
    out=tmp/name
    env={k:v for k,v in os.environ.items() if k!='HSN_SIM_THREADS'}
    if threads is not None:env['HSN_SIM_THREADS']=str(threads)
    env['HSN_SIM_THREADS_DEBUG']='1'
    p=subprocess.run([str(bin/('hypersim'+sx)),str(inp),'--output',str(out),'--traffic-src','0','--traffic-dst','2','--bandwidth-model','packet'],env=env,capture_output=True,text=True,timeout=300)
    assert p.returncode==0,(name,p.stderr)
    return out,parse(p.stderr)
def F(out,tool,ext):
    m=list(out.glob('*-'+tool+'-*.'+ext));assert len(m)==1,(out,tool,ext,m);return m[0]
# ---- auto-detected worker count equals the virtual processor count ----
auto_out,(auto,owner)=run('auto',None)
ncpu=os.cpu_count() or 1
assert auto==ncpu,('auto worker count must equal the virtual processor count',auto,ncpu)
assert owner=={i:i%ncpu for i in range(len(owner))},owner
if ncpu>1:assert all(owner[i]!=owner[i+1] for i in range(len(owner)-1)),('adjacent nodes must not share a worker',owner)
# ---- pinned thread counts reproduce the auto run byte-for-byte ----
for t in (1,2,4):
    out,(thr,own)=run('t%d'%t,t)
    assert thr==t,(t,thr)
    assert own=={i:i%t for i in range(len(own))},(t,own)
    if t>1:assert all(own[i]!=own[i+1] for i in range(len(own)-1)),(t,own)
    for tool,ext in [('business','jsonl'),('statistics','jsonl'),('traceroute','txt'),('traceroute','jsonl'),('result','json')]:
        assert F(auto_out,tool,ext).read_bytes()==F(out,tool,ext).read_bytes(),('thread count must not change committed output',t,tool,ext)
    r=json.loads(F(out,'result','json').read_text())
    assert r['generated_packets']==r['delivered_packets']+r['dropped_packets']+r['censored_packets'],(t,r)
    assert r['generated_packets']>0 and r['delivered_packets']>0,(t,r)
print(json.dumps({'status':'PASS','threads':auto,'owner':owner,'checks':'auto-nproc,round-robin,adjacency,1/2/4-byte-identical,packet-conservation'}))
