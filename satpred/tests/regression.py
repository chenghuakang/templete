#!/usr/bin/env python3
import argparse, json, os, pathlib, subprocess, tempfile
ap=argparse.ArgumentParser();ap.add_argument('bin');args=ap.parse_args()
bin=pathlib.Path(args.bin).resolve();suffix='.exe' if os.name=='nt' else ''
with tempfile.TemporaryDirectory(prefix='satpred test ') as tmp:
    env=os.environ.copy();env['SATPRED_HOME']=str(pathlib.Path(tmp)/'app_home')
    def run(app,*args,exit=0):
        p=subprocess.run([str(bin/(app+suffix)),*map(str,args)],env=env,capture_output=True,text=True)
        assert p.returncode==exit,(app,args,p.returncode,p.stderr)
        return p
    run('satpred','--selftest');run('satdemo')
    mark=json.loads((pathlib.Path(env['SATPRED_HOME'])/'last_success.json').read_text())
    run('satread','verify',mark['orbit'])
    run('netread','verify',mark['graph'],'--source-orbit',mark['orbit'])
    run('satread');run('netread')
    run('satpred','--threads','1','--threads','2',exit=1)
    run('satpred','--step','0',exit=1)
    run('satpred','--dump-defaults','--threads','2',exit=1)
    run('satnet','--backend','cuda',exit=6)
    run('orbit_engine',exit=1)
    bad=pathlib.Path(tmp)/'bad.sbin';b=bytearray(pathlib.Path(mark['orbit']).read_bytes());b[508]^=1;bad.write_bytes(b)
    run('satread','verify',bad,exit=7)
    summary=json.loads((pathlib.Path(mark['run_dir'])/'summary.json').read_text())
    assert summary['slices']==8640 and summary['packages']==24 and summary['verified']
    # L2-01 (v3.5): default OneWebSmall run carries the case annotation in manifest and summary; it is annotation only.
    run_manifest=json.loads((pathlib.Path(mark['run_dir'])/'manifest.json').read_text())
    assert run_manifest['case_name']=='OneWebSmall' and run_manifest['rf_parameters']['format']=='satnet-rf-annotation-1'
    assert summary['case_name']=='OneWebSmall' and summary['rf_parameters']==run_manifest['rf_parameters'] and summary['numeric_accuracy_certified'] is False
    assert run_manifest['rf_parameters']['rf_model_applied'] is False and all(q['M']==360 for q in run_manifest['products'])
    # L2-01b (v3.5 §13): fast CI path, default sample with --duration 600 --package 60 -> 60 slices / 10 packages / 10 graphs.
    fast=pathlib.Path(tmp)/'l2_01b';run('satnet','--output',fast,'--duration','600','--package','60')
    fm=json.loads((fast/'manifest.json').read_text());assert len(fm['products'])==10 and sum(q['M'] for q in fm['products'])==60 and all(q['M']==6 for q in fm['products']),'L2-01b: 60 slices in 10 packages'
    assert len(list((fast/'graphs').glob('*.ngb')))==10 and len(list((fast/'orbits').glob('*.sbin')))==10
    assert fm['case_name']=='OneWebSmall' and fm['rf_parameters']['format']=='satnet-rf-annotation-1','annotation also on explicit-interval runs of the default sample'
    run('netread','verify',fast/fm['products'][0]['graph_file'],'--source-orbit',fast/fm['products'][0]['source_file'])
    # F.3-3: a different XML pair (OneWeb648 regression set) must not carry the OneWebSmall annotation keys.
    ex=bin/'examples'/'OneWeb648';assert (ex/'satellites.xml').exists() and (ex/'nodes.xml').exists(),'F.3-2: examples/OneWeb648 must be installed next to the binaries'
    other=pathlib.Path(tmp)/'oneweb648';run('satnet','--output',other,'--satellites',ex/'satellites.xml','--nodes',ex/'nodes.xml','--duration','60','--package','60','--threads','2')
    om=json.loads((other/'manifest.json').read_text());assert 'case_name' not in om and 'rf_parameters' not in om,'annotation only when both XML SHA256 match case.json'
    # Defect regression: state-file failure must not fail a complete run (default-flow exit 3).
    def run_env(app,*args,env_override,exit=0):
        e={k:v for k,v in env.items() if k not in('SATPRED_HOME','HOME','USERPROFILE')};e.update(env_override)
        p=subprocess.run([str(bin/(app+suffix)),*map(str,args)],env=e,capture_output=True,text=True)
        assert p.returncode==exit,(app,args,p.returncode,p.stderr)
        return p
    short=['--duration','600','--package','600','--threads','1']
    p=run_env('satnet','--output',pathlib.Path(tmp)/'no_home',*short,env_override={})
    assert (pathlib.Path(tmp)/'no_home'/'manifest.json').exists() and 'last_success_not_recorded' in p.stderr
    blocker=pathlib.Path(tmp)/'blocker';blocker.write_text('file')
    p=run_env('satnet','--output',pathlib.Path(tmp)/'bad_state',*short,env_override={'SATPRED_HOME':str(blocker/'state')})
    assert (pathlib.Path(tmp)/'bad_state'/'manifest.json').exists() and 'last_success_not_recorded' in p.stderr
    run_env('satnet','--output',pathlib.Path(tmp)/'good_state',*short,env_override={'SATPRED_HOME':str(pathlib.Path(tmp)/'state_ok')})
    assert (pathlib.Path(tmp)/'state_ok'/'last_success.json').exists()
    print(json.dumps(dict(status='PASS',summary=summary,l2_01_annotation='PASS',l2_01b_fast_path='PASS',state_file_regression='PASS')))
