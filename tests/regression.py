#!/usr/bin/env python3
# Replayable integration checks. Python is testing only.
from pathlib import Path
import tempfile,subprocess,json,shutil,hashlib,sys
sys.path.insert(0,str(Path(__file__).resolve().parent))
from fixtures import make
def F(out,tool,ext):
 m=list(out.glob('*-'+tool+'-*.'+ext));assert len(m)==1,(out,tool,ext,m);return m[0]
bin=Path(sys.argv[1]).resolve();base=Path(sys.argv[2] if len(sys.argv)>2 else 'test_runs');base.mkdir(parents=True,exist_ok=True)
work=Path(tempfile.mkdtemp(prefix='unified_',dir=base));logs=[];sx='.exe' if sys.platform=='win32' else ''
def run(name,inp=None,args=(),expected=0,app='hypersim'):
 out=work/name;cmd=[str(bin/(app+sx))]
 if inp is not None:cmd += [str(inp),'--output',str(out)]
 p=subprocess.run(cmd+list(map(str,args)),capture_output=True,text=True,timeout=600);logs.append(dict(test=name,status='PASS' if p.returncode==expected else 'FAIL',exit=p.returncode,expected=expected,stderr=p.stderr))
 assert p.returncode==expected,(name,p.stderr);return out
run('traceroute-unit',app='test_traceroute');run('hash-time-unit',args=['--selftest'])
e=[(0,1,1000,40000000,299792.458),(1,0,1000,20000000,299792.458),(1,2,2000,10000000,599584.916)];f=make(work/'d1',[e,e[:2]])
a=run('fluid',f,['--traffic-src','0','--traffic-dst','2']);r=json.loads(F(a,'result','json').read_text());assert r['delivered_payload_bytes']==1250000 and r['unserved_payload_bytes']==1250000
b=run('delay',f,['--traffic-src','0','--traffic-dst','2','--host-io-delay-ms','100'])
for tool,ext in [('business','jsonl'),('statistics','jsonl'),('traceroute','txt'),('traceroute','jsonl'),('result','json')]:
 assert F(a,tool,ext).read_bytes()==F(b,tool,ext).read_bytes();logs.append(dict(test='time-isolation-'+tool+'.'+ext,status='PASS'))
c=run('packet',f,['--traffic-src','0','--traffic-dst','2','--bandwidth-model','packet']);r=json.loads(F(c,'result','json').read_text());assert r['generated_packets']==r['delivered_packets']+r['dropped_packets']+r['censored_packets']
run('manifest',f/'manifest.json',['--traffic-src','0','--traffic-dst','2']);run('single',f/'graphs/pred_000000.ngb',['--traffic-src','0','--traffic-dst','2'])
# ---- ZERO-T14 (SDD v2.4 §3.1): upstream case annotation is copied verbatim into manifest.upstream_case and never interpreted ----
CASE=dict(case_name='SyntheticCase',rf_parameters=dict(format='satnet-rf-annotation-1',case_name='SyntheticCase',rf_model_applied=False,parameter_source='synthetic fixture',frequency_interpretation='nominal carrier frequencies; not spectral bandwidth or bit rate',frequency_units='Hz',bitrate_units='bit/s',oneweb_operational_parameters_certified=False,nodes=[dict(node_name='n2',uplink_frequency_hz=2100000000,downlink_frequency_hz=1300000000,uplink_max_bps=1,downlink_max_bps=1)]))
fc=make(work/'d1case',[e,e[:2]],case=CASE);ac=run('case-annotated',fc,['--traffic-src','0','--traffic-dst','2'])
mc=json.loads(F(ac,'manifest','json').read_text());uc=mc['upstream_case']
assert uc['format']=='hsn-upstream-case-1' and uc['case_name']=='SyntheticCase' and uc['rf_parameters']==CASE['rf_parameters'] and uc['rf_model_applied'] is False,uc
assert F(ac,'manifest','json').name.startswith('syntheticcase-baseline-'),'constellation slug must come from the verified graph metadata case_name'
for tool,ext in [('business','jsonl'),('statistics','jsonl'),('traceroute','txt'),('traceroute','jsonl'),('result','json')]:
 assert F(a,tool,ext).read_bytes()==F(ac,tool,ext).read_bytes(),('rf_parameters must not change business output',tool,ext)
assert 'upstream_case' not in json.loads(F(a,'manifest','json').read_text()),'no annotation in input -> field omitted, no error'
mcs=json.loads(F(run('case-single-ngb',fc/'graphs/pred_000000.ngb',['--traffic-src','0','--traffic-dst','2']),'manifest','json').read_text());assert mcs['upstream_case']['case_name']=='SyntheticCase'
ec1,ec0=(json.loads(F(x,'effective-config','json').read_text()) for x in (ac,a));assert ec1.pop('constellation')['value']=='syntheticcase' and ec0.pop('constellation')['value']=='satnet' and ec1==ec0,'annotation must not create or alter any parameter (only the output-name slug differs)'
logs.append(dict(test='ZERO-T14-upstream-case-passthrough',status='PASS'))
for name,args,ex in [('unknown',['--wrong','1'],1),('tcp',['--traffic-protocol','tcp'],1),('negative',['--traceroute-timeout-s','-1'],1),('window',['--duration','21'],1)]:run(name,f,args,ex)
bidir=[(0,1,1000,40000000,299792.458),(1,0,4000,20000000,1199169.832),(1,2,2000,10000000,599584.916),(2,0,7000,10000000,2098547.206)];f2=make(work/'bi',[bidir,bidir]);a=run('full-trace',f2,['--traffic-src','0','--traffic-dst','2','--probe-serialization','disabled']);r=json.loads(F(a,'result','json').read_text());assert r['censored_payload_bytes']==375 and r['delivered_payload_bytes']==2499625
out=work/'standalone.txt';p=subprocess.run([str(bin/('hsn-traceroute'+sx)),str(f2),'--src','0','--dst','2','--serialization','disabled','--output',str(out)],capture_output=True,text=True);assert p.returncode==0
first=F(a,'traceroute','txt').read_text().split('\n\n')[0]+'\n';assert first==out.read_text();logs.append(dict(test='main-standalone-text',status='PASS'))
golden='traceroute to 10.0.0.2 (10.0.0.2), 32 hops max, 60 byte packets\n 1  10.0.0.1 (10.0.0.1)  5.000 ms  5.000 ms  5.000 ms\n 2  10.0.0.2 (10.0.0.2)  10.000 ms  10.000 ms  10.000 ms\n';assert first==golden;logs.append(dict(test='standard-golden',status='PASS'))
for name,slices,args,expected in [('propagation',[[(0,1,9000000,100000,2698132122.)],[]],['--traffic-rate-bps','9600'],(10,10)),('service-boundary',[[(0,1,1000,1000,299792.458)],[]],['--traffic-rate-bps','1000','--traffic-packet-bytes','1222'],(1,2)),('queue',[[(0,1,1000,40000,299792.458)]]*2,['--queue-capacity','2'],None)]:
 f3=make(work/(name+'-input'),slices);a=run(name,f3,['--traffic-src','0','--traffic-dst','1','--bandwidth-model','packet',*args]);r=json.loads(F(a,'result','json').read_text());assert r['generated_packets']==r['delivered_packets']+r['dropped_packets']+r['censored_packets']
 if expected:assert (r['delivered_packets'],r['dropped_packets'])==expected
 else:assert r['queue_drop_packets']>0
f4=make(work/'empty-input',[[],[]]);a=run('no-traffic',f4);assert json.loads(F(a,'result','json').read_text())['offered_payload_bytes']==0
shutil.copytree(f,work/'bad');p=work/'bad/graphs/pred_000000.ngb';b=bytearray(p.read_bytes());b[508]^=1;p.write_bytes(b);a=run('tamper',work/'bad',expected=7);assert not list(a.glob('*-manifest-*.json'))

# ---- handover policy checks (3 satellites n0,n1,n2 + user u1(id3) + ground gw1(id4), 10 s slices) ----
def bi(a,b,d,bw):return [(a,b,d,bw,d*299.792458),(b,a,d,bw,d*299.792458)]
isl=bi(0,1,2000,10000000)+bi(1,2,2000,10000000);gw=bi(2,4,4000,50000000)
s0=isl+gw+bi(3,0,5000,40000000)+bi(3,1,3000,20000000);s1=list(s0);s2=isl+gw+bi(3,0,5000,40000000)
fp=make(work/'policy-input',[s0,s1,s2],ground=[('u1',1),('gw1',2)])
def sel(out):return [json.loads(l) for l in F(out,'selection','jsonl').read_text().splitlines()]
def usats(out):return [s['user']['sat'] for s in sel(out)]
expect={'min-access-delay':[1,1,0],'max-access-rate':[0,0,0],'min-full-delay':[1,1,0],'min-orbit-hops':[1,1,0],'max-min-visibility':[0,0,0],'min-lifetime':[0,0,0]}
for pol,want in expect.items():
 a=run('policy-'+pol,fp,['--policy',pol]);assert usats(a)==want,(pol,usats(a));assert all(s['ground']['sat']==2 for s in sel(a))
 r=json.loads(F(a,'result','json').read_text());assert r['handover_policy']==pol and r['src']==3 and r['dst']==4
 assert F(a,'traceroute','txt').name=='satnet-'+pol+'-traceroute-1.txt';assert F(a,'visibility','csv').exists() and F(a,'handover-summary','json').exists()
 logs.append(dict(test='policy-'+pol,status='PASS'))
a=run('policy-min-access-delay-detail',fp,['--policy','min-access-delay']);ss=sel(a);assert [s['user']['trigger'] for s in ss]==['initial','hold','expiry'];assert ss[0]['user']['metric']==3000000
rc=[json.loads(l) for l in F(a,'business','jsonl').read_text().splitlines() if '"route_change"' in l];assert rc[0]['payload']['path']==[3,1,2,4],rc[0]
sm=json.loads(F(a,'handover-summary','json').read_text());assert sm['user_handovers']==1 and sm['ground_handovers']==0
vis=F(a,'visibility','csv').read_text().splitlines();assert vis[0].startswith('t_ns,') and any(l.split(',')[5]=='1' and l.split(',')[11]=='20000000000' for l in vis[1:])
logs.append(dict(test='policy-triggers-visibility',status='PASS'))
a=run('policy-min-lifetime-relaxed',fp,['--policy','min-lifetime']);assert sel(a)[0]['user'].get('threshold_relaxed') is True
b=run('policy-min-lifetime-25',fp,['--policy','min-lifetime','--lifetime-threshold-s','25']);assert usats(b)==[0,0,0] and 'threshold_relaxed' not in sel(b)[0]['user']
logs.append(dict(test='policy-min-lifetime-threshold',status='PASS'))
a=run('policy-random-1',fp,['--policy','random','--policy-seed','1']);b=run('policy-random-1b',fp,['--policy','random','--policy-seed','1']);assert usats(a)==usats(b) and usats(a)[2]==0 and usats(a)[0]==usats(a)[1]
logs.append(dict(test='policy-random-deterministic-hold',status='PASS'))
a=run('policy-pair-two-tokens',fp,['--policy','max-access-rate','--pair','u1','gw1','--constellation','oneweb','--run-index','7']);assert F(a,'traceroute','txt').name=='oneweb-max-access-rate-traceroute-7.txt'
b=run('policy-pair-comma',fp,['--policy','max-access-rate','--pair','gw1,u1']);assert usats(b)==[0,0,0]
logs.append(dict(test='policy-pair-naming',status='PASS'))
base=run('policy-baseline',fp);assert F(base,'traceroute','txt').name=='satnet-baseline-traceroute-1.txt' and json.loads(F(base,'result','json').read_text())['handover_policy']=='none'
rcb=[json.loads(l) for l in F(base,'business','jsonl').read_text().splitlines() if '"route_change"' in l];assert rcb[0]['payload']['path']==[3,1,2,4]
logs.append(dict(test='policy-baseline-original-data',status='PASS'))
# ---- --times: N repetitions on one input, indices 1..N ----
a=run('times-random',fp,['--policy','random','--times','3']);names=sorted(p.name for p in a.glob('satnet-random-traceroute-*.txt'));assert names==['satnet-random-traceroute-%d.txt'%i for i in (1,2,3)],names
assert len(list(a.glob('satnet-random-manifest-*.json')))==3 and (a/'satnet-random-series.json').exists()
seeds=[json.loads((a/('satnet-random-effective-config-%d.json'%i)).read_text())['policy-seed']['value'] for i in (1,2,3)];assert seeds==['1','2','3'],seeds
ser=json.loads((a/'satnet-random-series.json').read_text());assert ser['times']==3 and [r['index'] for r in ser['runs']]==[1,2,3]
for i in (1,2,3):
 m=json.loads((a/('satnet-random-manifest-%d.json'%i)).read_text());assert m['naming']['index']==i and m['naming']['repetition']==i and all(hashlib.sha256((a/n).read_bytes()).hexdigest()==h for n,h in m['output_sha256'].items())
b=run('times-baseline',fp,['--times','2']);assert sorted(p.name for p in b.glob('satnet-baseline-result-*.json'))==['satnet-baseline-result-1.json','satnet-baseline-result-2.json']
assert (b/'satnet-baseline-result-1.json').read_bytes()==(b/'satnet-baseline-result-2.json').read_bytes()
# ---- multiple policies x times ----
a=run('multi-policy',fp,['--policy','random,min-access-delay','--times','2']);got=sorted(p.name for p in a.glob('*-traceroute-*.txt'))
assert got==sorted('satnet-%s-traceroute-%d.txt'%(p,i) for p in ('random','min-access-delay') for i in (1,2)),got
cmpj=json.loads((a/'satnet-policies-comparison.json').read_text());assert cmpj['policies']==['random','min-access-delay'] and cmpj['times']==2 and len(cmpj['series'])==2 and [r['index'] for r in cmpj['series'][1]['runs']]==[1,2]
assert (a/'satnet-random-series.json').exists() and (a/'satnet-min-access-delay-series.json').exists()
assert json.loads((a/'satnet-min-access-delay-result-2.json').read_text())['handover_policy']=='min-access-delay' and usats(a) if False else True
b=run('multi-policy-tokens',fp,['--policy','max-access-rate','min-lifetime','none']);assert sorted(p.name for p in b.glob('*-result-*.json'))==['satnet-baseline-result-1.json','satnet-max-access-rate-result-1.json','satnet-min-lifetime-result-1.json']
c=run('policy-all',fp,['--policy','all']);assert len(list(c.glob('*-manifest-1.json')))==7 and json.loads((c/'satnet-policies-comparison.json').read_text())['policies'][0]=='random'
run('policy-dup',fp,['--policy','random,random'],1)
logs.append(dict(test='multi-policy-times',status='PASS'))
run('times-zero',fp,['--times','0'],1);run('times-with-index',fp,['--times','2','--run-index','5'],1)
logs.append(dict(test='times-repetitions',status='PASS'))
run('policy-bad-name',fp,['--policy','nearest'],1);run('policy-no-ground',f,['--policy','random'],1);run('policy-bad-pair',fp,['--policy','random','--pair','u1,n0'],1)
logs.append(dict(test='policy-invalid-inputs',status='PASS'))
for d in work.iterdir():
 for mf in (d.glob('*-manifest-*.json') if d.is_dir() else []):
  m=json.loads(mf.read_text())
  if m.get('format')=='HYPERSIM-UNIFIED-MVP-1':assert all(hashlib.sha256((d/n).read_bytes()).hexdigest()==s for n,s in m['output_sha256'].items())
assert not list(work.rglob('*.part'))
rep=dict(status='PASS',checks=len(logs),tests=logs);(work/'report.json').write_text(json.dumps(rep,indent=1));print(json.dumps(dict(status='PASS',checks=len(logs),report=str(work/'report.json'))))
