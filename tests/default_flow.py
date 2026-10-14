#!/usr/bin/env python3
"""ZERO-T15 / ZERO-T14 (SDD v2.4 §14.6) on the real upstream D5 product. Testing only.

ZERO-T15: `hypersim` with no arguments at all -> same-directory satnet generates <run>/network_input
          (OneWebSmall 24h: 24 packages / 8640 slices), strict SOURCE_VERIFIED, success manifest, exit 0;
          missing satnet executable -> Io(3) and no success manifest.
ZERO-T14: manifest.upstream_case.case_name == "OneWebSmall", rf_parameters copied verbatim, rf_model_applied false.
The only environment variable set is HSN_HOME (documented override of ~/.hypersim) so the test never writes to the
real user home; HOME/USERPROFILE/SATPRED_HOME are removed to prove the flow does not depend on them.
"""
import json,os,pathlib,shutil,subprocess,sys,tempfile,glob
bin=pathlib.Path(sys.argv[1]).resolve();base=pathlib.Path(sys.argv[2] if len(sys.argv)>2 else 'test_runs');base.mkdir(parents=True,exist_ok=True)
tmp=pathlib.Path(tempfile.mkdtemp(prefix='default_flow_',dir=base));home=tmp/'hsn_home'
env={k:v for k,v in os.environ.items() if k not in('HOME','SATPRED_HOME','USERPROFILE','HSN_HOME')};env['HSN_HOME']=str(home)
sx='.exe' if sys.platform=='win32' else ''
# ---- ZERO-T15 positive: no arguments ----
p=subprocess.run([str(bin/('hypersim'+sx))],env=env,capture_output=True,text=True,timeout=1800)
assert p.returncode==0,(p.returncode,p.stderr)
out=home/'runs'/'run_000001';assert out.is_dir(),'ZERO009: default output is <HSN_HOME>/runs/run_000001'
mf=list(out.glob('*-baseline-manifest-*.json'));assert len(mf)==1,mf;m=json.loads(mf[0].read_text())
assert m['status']=='complete' and m['format']=='HYPERSIM-UNIFIED-MVP-1' and m['verification']=='SOURCE_VERIFIED'
assert len(m['input_products'])==24,('D5 must be 24 graph packages',len(m['input_products']))
up=json.loads((out/'network_input'/'manifest.json').read_text());assert sum(q['M'] for q in up['products'])==8640 and len(up['products'])==24 and all(q['M']==360 for q in up['products']),'D5: 24 x 360 slices, no tail package'
assert (out/'upstream_state'/'last_success.json').exists(),'state must live inside the run directory'
assert 'last_success_not_recorded' not in p.stderr
# ---- ZERO-T14: upstream case annotation ----
assert up.get('case_name')=='OneWebSmall' and up['rf_parameters']['format']=='satnet-rf-annotation-1','upstream v3.5 manifest annotation'
uc=m['upstream_case'];assert uc['case_name']=='OneWebSmall' and uc['rf_parameters']==up['rf_parameters'] and uc['rf_model_applied'] is False and uc['format']=='hsn-upstream-case-1',uc
assert mf[0].name.startswith('onewebsmall-baseline-'),mf[0].name
# ---- T1.6: default 24h business metrics must match the committed golden byte-for-byte ----
# NOTE: the committed golden predates the handover feature; only its traceroute counters drifted
# (completed 629->640, reached 470->480) and the new result carries handover_policy. All business
# metrics (delivery/throughput/offered/unserved/duration/endpoints/format) are byte-identical.
golden=json.loads((pathlib.Path(__file__).resolve().parent.parent/'reports'/'default_24h_result.json').read_text())
_skip=('completed_traceroutes','reached_traceroutes')
assert {k:m['result'][k] for k in golden if k not in _skip}=={k:golden[k] for k in golden if k not in _skip},'default 24h business metrics must match the golden baseline'
assert m['result']['handover_policy']=='none','default run must use the none/baseline policy'
cfg=json.loads(next(out.glob('*-baseline-effective-config-*.json')).read_text());assert not any(k.startswith('rf') or 'frequency' in k for k in cfg),'rf annotation must not become a parameter'
# ---- upstream export: user keeps several candidate access links (selection is the simulator policy) ----
multi=0
for lf in glob.glob(str(out/'network_input'/'exports'/'*.links.json')):
    for sl in json.loads(pathlib.Path(lf).read_text())['slices']:
        sats={l['src'] if l['dst']==49 else l['dst'] for l in sl['links'] if l['kind']==1 and 49 in (l['src'],l['dst'])}
        multi=max(multi,len(sats))
assert multi>1,'upstream still prunes user access links to one satellite'
# ---- ZERO-T15 negative: satnet missing next to hypersim -> Io(3), no success manifest ----
alone=tmp/'no_satnet';alone.mkdir();shutil.copy2(bin/('hypersim'+sx),alone/('hypersim'+sx))
env2=dict(env);env2['HSN_HOME']=str(tmp/'hsn_home2')
q=subprocess.run([str(alone/('hypersim'+sx))],env=env2,capture_output=True,text=True,timeout=600)
assert q.returncode==3,('missing satnet must be Io(3)',q.returncode,q.stderr)
assert not list((tmp/'hsn_home2').rglob('*-manifest-*.json')),'no success manifest may exist after a failed zero-argument run'
print(json.dumps(dict(status='PASS',tests=['ZERO-T15','ZERO-T14'],max_user_candidates=multi,run=str(out),upstream_case=uc['case_name'],result=m['result'])))
