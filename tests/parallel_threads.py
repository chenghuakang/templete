#!/usr/bin/env python3
"""VT01-style checks for the decentralised lock-free PDES (SDD v2.3 §1.4 / §7.4 / §8.4 / §11.1).

Properties under test:
  * The worker count is auto-detected from the virtual processor count (os.cpu_count());
    HSN_SIM_THREADS pins it (1/2/4/...) for the thread-count checks.
  * Nodes are assigned to partitions round-robin by their label, owner(n) = n % T, so adjacent
    node ids never share a partition when T > 1.
  * Packet conservation (generated == delivered + dropped + censored) holds for every run and
    every thread count.
  * A decentralised PDES is not required to be bit-reproducible, so the checks below bound the
    spread instead of demanding identical bytes: 1/2/4/auto threads must stay inside the declared
    tolerance of each other, and repeated runs of the same input must stay inside the same band.
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
# ---- every thread count keeps packet conservation and stays inside the declared tolerance ----
TOL=0.02
KEY=('delivered_packets','dropped_packets','censored_packets','unreachable_drop_packets','link_down_drop_packets','queue_drop_packets','hop_limit_drop_packets')
def metrics(out):return json.loads(F(out,'result','json').read_text())
ref=metrics(auto_out);runs=[('auto',ref)]
for t in (1,2,4):
    out,(thr,own)=run('t%d'%t,t)
    assert thr==t,(t,thr)
    assert own=={i:i%t for i in range(len(own))},(t,own)
    if t>1:assert all(own[i]!=own[i+1] for i in range(len(own)-1)),(t,own)
    runs.append(('t%d'%t,metrics(out)))
# ---- repeated runs of the same input stay inside the same band (a decentralised PDES is not bit-reproducible) ----
for i in range(4):runs.append(('rep%d'%i,metrics(run('rep%d'%i,None)[0])))
for name,r in runs:
    assert r['generated_packets']==r['delivered_packets']+r['dropped_packets']+r['censored_packets'],('packet conservation must always hold',name,r)
    assert r['generated_packets']>0,(name,r)
    for k in KEY:
        assert abs(r[k]-ref[k])<=TOL*max(1,ref[k]),('metric outside the declared band',name,k,r[k],ref[k])
print(json.dumps({'status':'PASS','threads':auto,'owner':owner,'tolerance':TOL,'runs':len(runs),'checks':'auto-nproc,round-robin,adjacency,packet-conservation,spread-within-band'}))
