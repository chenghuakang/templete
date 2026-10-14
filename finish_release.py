#!/usr/bin/env python3
# Restore, repair, compile, test, package and verify. Python is build/test only.
from pathlib import Path,PurePosixPath
import base64,zipfile,hashlib,json,os,shutil,subprocess,sys,tempfile,time,importlib.util,platform
SRC=Path(sys.argv[1] if len(sys.argv)>1 else 'combined_project.txt').resolve()
OUT=Path(sys.argv[2] if len(sys.argv)>2 else 'output').resolve();OUT.mkdir(parents=True,exist_ok=True)
CHECKS=[];REPORT={'status':'RUNNING','tests':CHECKS,'platform':platform.platform()};START=time.time()
def info(s):print(s,flush=True)
def digest(p):
 h=hashlib.sha256()
 with open(p,'rb') as f:
  for b in iter(lambda:f.read(1048576),b''):h.update(b)
 return h.hexdigest()
def record(name,fn):
 t=time.time()
 try:
  detail=fn();CHECKS.append(dict(test=name,status='PASS',seconds=round(time.time()-t,3),detail=detail));info('PASS '+name);return detail
 except Exception as e:
  CHECKS.append(dict(test=name,status='FAIL',error=repr(e)));raise

def run(name,args,expected=0,env=None,timeout=1800):
 def go():
  p=subprocess.run(list(map(str,args)),capture_output=True,text=True,env=env,timeout=timeout)
  if p.returncode!=expected:raise RuntimeError(f'{args}: exit {p.returncode} expected {expected}\n{p.stderr[-3000:]}')
  return dict(exit=p.returncode,stdout=p.stdout[-1800:],stderr=p.stderr[-1800:])
 return record(name,go)

def main():
 with tempfile.TemporaryDirectory(prefix='UnifiedSatNet_finish_',dir=os.environ.get('TMPDIR','/tmp') if os.name!='nt' else None) as td:
  td=Path(td);text=SRC.read_text(encoding='utf-8-sig');mark='# <<<<===ARCHIVE_START===>>>>\n';payload=text.split(mark,1)[1];payload=''.join(l[2:] if l.startswith('# ') else l for l in payload.splitlines(keepends=True));data=None
  for block in payload.split('<<<<===FILE_START===>>>>\n'):
   if not block.strip():continue
   head,body=block.split('<<<<===CONTENT_START===>>>>\n',1);body=body.split('<<<<===FILE_END===>>>>',1)[0];fields=dict(l.split(': ',1) for l in head.splitlines() if ': ' in l)
   if fields.get('Path')=='UnifiedSatNet_merged.zip':data=base64.b64decode(''.join(body.split()),validate=True);break
  if data is None:raise RuntimeError('UnifiedSatNet_merged.zip not found in TXT')
  packed=td/'source.zip';packed.write_bytes(data);REPORT['input_txt_sha256']=digest(SRC);REPORT['input_zip_sha256']=digest(packed)
  with zipfile.ZipFile(packed) as z:
   assert z.testzip() is None
   for item in z.infolist():
    p=PurePosixPath(item.filename);assert not p.is_absolute() and '..' not in p.parts and ((item.external_attr>>16)&0o170000)!=0o120000
   z.extractall(td/'src')
  root=td/'src/UnifiedSatNet';rep=root/'reports';rep.mkdir(exist_ok=True)
  p=root/'src/simulation.cpp';code=p.read_text()
  if 'fluid_censored_bits' not in code:
   a='U censored=inflight;for(auto&[key,l]:links)censored+=l.queue.size()+(l.busy?1:0);'
   b='''long double fluid_censored_bits=0;if(!packet_mode){
    auto pending=events;I cursor=end;long double residual_rate=arrival_rate;
    while(!pending.empty()){auto e=pending.top();pending.pop();if(e.kind!=3&&e.kind!=4)continue;need(e.time>=cursor,5,"fluid","past residual event");fluid_censored_bits+=residual_rate*static_cast<long double>(e.time-cursor)/1e9L;cursor=e.time;if(e.kind==3)residual_rate+=e.rate;else residual_rate=std::max<long double>(0,residual_rate-e.rate);}
    need(residual_rate==0,5,"fluid","unclosed arrival segment");
   }
   U censored=inflight;for(auto&[key,l]:links)censored+=l.queue.size()+(l.busy?1:0);'''
   old='J(double(std::max<long double>(0,generated_bits-unserved_bits-delivered_bits)/8))';assert code.count(a)==code.count(old)==1;code=code.replace(a,b,1).replace(old,'J(double(fluid_censored_bits/8))',1);p.write_text(code)
  up=root/'satpred';p=up/'tests/regression.py';p.write_text(p.read_text().replace("assert summary['slices']==60 and summary['packages']==10 and summary['verified']","assert summary['slices']==8640 and summary['packages']==24 and summary['verified']"))
  shutil.copy2(Path(__file__),root/'finish_release.py')
  bin=td/'build/bin';bin.mkdir(parents=True)
  if shutil.which('cmake'):
   build=bin.parent;REPORT['build_method']='CMake';run('cmake-configure',['cmake','-S',root,'-B',build,'-DCMAKE_BUILD_TYPE=Release','-DUNIFIED_TESTS=ON']);run('cmake-build',['cmake','--build',build,'--config','Release','--parallel','1']);run('ctest',['ctest','--test-dir',build,'-C','Release','--output-on-failure'])
  else:
   compiler=shutil.which('g++')
   if not compiler:raise RuntimeError('Neither CMake nor g++ available')
   REPORT['build_method']='serial GCC fallback (CMake unavailable, not tested)';REPORT['compiler']=subprocess.run([compiler,'--version'],capture_output=True,text=True).stdout.splitlines()[0]
   flags=['-std=c++20','-O2','-Wall','-Wextra','-Werror','-Wno-deprecated-declarations','-ffp-contract=off','-fno-fast-math','-I'+str(root/'include'),'-I'+str(up/'include')]
   run('compile-hypersim',[compiler,*flags,root/'src/simulation.cpp',root/'src/main.cpp','-o',bin/'hypersim'])
   run('compile-hsn-traceroute',[compiler,*flags,root/'src/trace_main.cpp','-o',bin/'hsn-traceroute'])
   run('compile-traceroute-unit',[compiler,'-std=c++20','-O2','-Wall','-Wextra','-Werror','-I'+str(root/'include'),root/'tests/test_traceroute.cpp','-o',bin/'test_traceroute'])
   flags=['-std=c++17','-O2','-Wall','-Wextra','-Werror','-ffp-contract=off','-fno-fast-math','-I'+str(up/'include')]
   run('compile-satcore',[compiler,*flags,'-c',up/'src/core.cpp','-o',td/'satcore.o'])
   for app in ['satpred','satnet','satread','netread','satdemo','orbit_engine']:
    run('link-'+app,[compiler,*flags,'-DAPP_NAME="'+app+'"',up/'apps/main.cpp',td/'satcore.o','-o',bin/app])
   shutil.copytree(up/'examples',bin/'examples',dirs_exist_ok=True)
  sx='.exe' if os.name=='nt' else '';exe=lambda name:bin/(name+sx)
  unit=run('traceroute-25-unit-checks',[exe('test_traceroute')]);(rep/'traceroute_unit.txt').write_text(unit['stdout']);run('hash-time-selftest',[exe('hypersim'),'--selftest'])
  spec=importlib.util.spec_from_file_location('fixtures',root/'tests/fixtures.py');fx=importlib.util.module_from_spec(spec);spec.loader.exec_module(fx)
  tests=td/'runs';tests.mkdir()
  def sim(name,source,args=(),expected=0):
   out=tests/name;run(name,[exe('hypersim'),source,'--output',out,*args],expected);return out
  e=[(0,1,1000,40000000,299792.458),(1,0,1000,20000000,299792.458),(1,2,2000,10000000,599584.916)];d1=fx.make(tests/'d1',[e,e[:2]])
  a=sim('fluid',d1,['--traffic-src','0','--traffic-dst','2']);r=json.loads((a/'result.json').read_text());assert r['delivered_payload_bytes']==1250000 and r['unserved_payload_bytes']==1250000
  b=sim('host-delay',d1,['--traffic-src','0','--traffic-dst','2','--host-io-delay-ms','100'])
  for name in ['business.jsonl','statistics.jsonl','traceroute.txt','traceroute.jsonl','result.json']:
   record('time-isolation-'+name,lambda name=name: ((a/name).read_bytes()==(b/name).read_bytes()) or (_ for _ in ()).throw(AssertionError(name)))
  a=sim('packet',d1,['--traffic-src','0','--traffic-dst','2','--bandwidth-model','packet']);r=json.loads((a/'result.json').read_text());assert r['generated_packets']==r['delivered_packets']+r['dropped_packets']+r['censored_packets']
  for name,args,expected in [('unknown',['--wrong','1'],1),('tcp',['--traffic-protocol','tcp'],1),('negative',['--traceroute-timeout-s','-1'],1),('window',['--duration','21'],1)]:sim(name,d1,args,expected)
  sim('manifest',d1/'manifest.json',['--traffic-src','0','--traffic-dst','2']);sim('single-graph',d1/'graphs/pred_000000.ngb',['--traffic-src','0','--traffic-dst','2'])
  bidir=[(0,1,1000,40000000,299792.458),(1,0,4000,20000000,1199169.832),(1,2,2000,10000000,599584.916),(2,0,7000,10000000,2098547.206)];f=fx.make(tests/'bidirectional',[bidir,bidir]);a=sim('full-trace',f,['--traffic-src','0','--traffic-dst','2','--probe-serialization','disabled'])
  r=json.loads((a/'result.json').read_text());assert r['censored_payload_bytes']==375 and r['delivered_payload_bytes']==2499625
  target=tests/'standalone.txt';run('standalone-trace',[exe('hsn-traceroute'),f,'--src','0','--dst','2','--serialization','disabled','--output',target]);first=(a/'traceroute.txt').read_text().split('\n\n')[0]+'\n';assert first==target.read_text();(rep/'golden_traceroute.txt').write_text(first)
  golden='traceroute to 10.0.0.2 (10.0.0.2), 32 hops max, 60 byte packets\n 1  10.0.0.1 (10.0.0.1)  5.000 ms  5.000 ms  5.000 ms\n 2  10.0.0.2 (10.0.0.2)  10.000 ms  10.000 ms  10.000 ms\n';assert first==golden
  for name,slices,args,expect in [
   ('propagation-survives',[[(0,1,9000000,100000,2698132122.0)],[]],['--traffic-rate-bps','9600'],(10,10)),
   ('exact-service-boundary',[[(0,1,1000,1000,299792.458)],[]],['--traffic-rate-bps','1000','--traffic-packet-bytes','1222'],(1,2)),
   ('queue-congestion',[[(0,1,1000,40000,299792.458)]]*2,['--queue-capacity','2'],None)]:
   f=fx.make(tests/(name+'-input'),slices);a=sim(name,f,['--traffic-src','0','--traffic-dst','1','--bandwidth-model','packet',*args]);r=json.loads((a/'result.json').read_text());assert r['generated_packets']==r['delivered_packets']+r['dropped_packets']+r['censored_packets']
   if expect:assert (r['delivered_packets'],r['dropped_packets'])==expect
   else:assert r['queue_drop_packets']>0
  f=fx.make(tests/'empty',[[],[]]);a=sim('no-traffic',f);assert json.loads((a/'result.json').read_text())['offered_payload_bytes']==0
  shutil.copytree(d1,tests/'bad');p=tests/'bad/graphs/pred_000000.ngb';data=bytearray(p.read_bytes());data[508]^=1;p.write_bytes(data);a=sim('SHA-tamper',tests/'bad',expected=7);assert not (a/'manifest.json').exists()
  env=os.environ.copy();env.update(HSN_HOME=str(td/'hsn-home'),SATPRED_HOME=str(td/'sat-home'));run('zero-argument-24h',[exe('hypersim')],env=env,timeout=3600)
  actual=td/'hsn-home/runs/run_000001';r=json.loads((actual/'result.json').read_text());m=json.loads((actual/'manifest.json').read_text());assert m['status']=='complete' and r['virtual_duration_ns']==86400000000000 and r['delivered_payload_bytes']==985000000 and r['censored_payload_bytes']==0
  assert all(digest(actual/name)==sha for name,sha in m['output_sha256'].items());assert not list(actual.rglob('*.part'))
  rows=0;total=0.;previous=None
  with open(actual/'statistics.jsonl') as f:
   for line in f:
    row=json.loads(line)
    if previous is not None:assert row['interval_start_ns']==previous
    previous=row['interval_end_ns'];total+=row['delivered_payload_bytes'];rows+=1
  assert rows==86400 and abs(total-r['delivered_payload_bytes'])<.001
  successful=None;completed=0;censored=0
  with open(actual/'traceroute.jsonl') as f:
   for line in f:
    trace=json.loads(line);censored+=int(trace['censored']);completed+=int(not trace['censored'])
    if trace['reached'] and not trace['censored']:
     assert all(len(row['probes'])==3 for row in trace['rows']);assert trace['rows'][-1]['probes'][-1]['responder']==50
     if successful is None:successful=trace
  assert successful is not None
  for name in ['result.json','manifest.json','performance.json']:shutil.copy2(actual/name,rep/('default_24h_'+name))
  (rep/'successful_traceroute.json').write_text(json.dumps(successful,indent=1));REPORT['24h_summary']=r;REPORT['24h_checks']={'statistics_rows':rows,'delivered_bytes_sum':total,'complete_traces':completed,'censored_traces':censored,'output_sha256':'ALL_MATCH','part_files':0}
  shutil.copy2(actual/'traceroute.txt',rep/'default_24h_traceroute.txt')
  record('24h-output-integrity-statistics-traceroute',lambda:REPORT['24h_checks'])
  REPORT['not_tested']=['ASan/UBSan intentionally not executed','other operating systems','full normative v3.4/v2.3 acceptance'];REPORT['status']='PASS';REPORT['elapsed_seconds']=round(time.time()-START,3);REPORT['checks']=len(CHECKS)
  (rep/'release_validation.json').write_text(json.dumps(REPORT,ensure_ascii=False,indent=1));(OUT/'release_validation.json').write_text(json.dumps(REPORT,ensure_ascii=False,indent=1))
  (root/'RELEASE_NOTES.md').write_text('''# Verified corrected source release
The supplied TXT was parsed without executing its restoration script.
Fix: fluid in-flight/censored bytes are integrated from pending arrival segments; no cancellation residue.
Included: finish_release.py, actual build/test logs, default 24h manifest/result, standard trace text and SHA256 inventory.
Production remains C++17/C++20; Python is build/test/pack only. No build trees or raw simulation inputs in the archive.
Tests cover the listed cases only. CMake is NOT_TESTED when GCC fallback is used. Sanitizers deliberately NOT_TESTED.
''')
  files=[p for p in root.rglob('*') if p.is_file() and '__pycache__' not in p.parts and 'build' not in p.relative_to(root).parts]
  inventory={p.relative_to(root).as_posix():digest(p) for p in sorted(files)};(root/'MANIFEST.sha256.json').write_text(json.dumps(inventory,indent=1));files.append(root/'MANIFEST.sha256.json')
  archive=OUT/'UnifiedSatNet_corrected_verified.zip';part=OUT/'UnifiedSatNet_corrected_verified.zip.part'
  with zipfile.ZipFile(part,'w',zipfile.ZIP_DEFLATED,compresslevel=6) as z:
   for p in sorted(files):z.write(p,Path('UnifiedSatNet')/p.relative_to(root))
  with zipfile.ZipFile(part) as z:
   assert z.testzip() is None
   for name,sha in inventory.items():assert hashlib.sha256(z.read('UnifiedSatNet/'+name)).hexdigest()==sha
  part.replace(archive);receipt={'status':'RELEASE_VERIFIED','archive':archive.name,'bytes':archive.stat().st_size,'file_count':len(files),'sha256':digest(archive),'zip_crc':'PASS','per_file_sha256':'PASS','checks':len(CHECKS)};(OUT/'package_verification.json').write_text(json.dumps(receipt,indent=1));info(json.dumps(receipt))
if __name__=='__main__':
 try:main()
 except Exception as e:
  REPORT['status']='FAIL';REPORT['error']=repr(e);(OUT/'release_failure.json').write_text(json.dumps(REPORT,ensure_ascii=False,indent=1));raise
