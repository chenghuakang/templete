#!/usr/bin/env python3
import argparse, json, math, os, pathlib, struct, subprocess, tempfile, diag_assets
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
    # backend=cuda: Device(6) when built without CUDA; when built with CUDA it runs and must produce
    # the same logical graph/edge payload as the CPU backend (SDD §3.3, L3-01). The G scope only
    # evaluates candidate distances; the host candidate set is retained unchanged.
    def satnet_raw(app,*extra,out):
        return subprocess.run([str(bin/(app+suffix)),'--output',str(out),'--duration','60','--package','60','--threads','2',*map(str,extra)],env=env,capture_output=True,text=True)
    cu=satnet_raw('satnet','--backend','cuda',out=pathlib.Path(tmp)/'gpu')
    assert cu.returncode in (0,6),(cu.returncode,cu.stderr)
    if cu.returncode==0:
        cp=satnet_raw('satnet','--backend','cpu',out=pathlib.Path(tmp)/'cpu')
        assert cp.returncode==0,cp.stderr
        gm=json.loads((pathlib.Path(tmp)/'gpu'/'manifest.json').read_text());cm=json.loads((pathlib.Path(tmp)/'cpu'/'manifest.json').read_text())
        assert gm['software']['actual_backend']=='cuda' and cm['software']['actual_backend']=='cpu'
        assert gm['gpu_candidate_eval']['max_distance_deviation_m']==0,'GPU/CPU distance must be bit-identical'
        assert gm['products'][0]['directed_edges']==cm['products'][0]['directed_edges']
        ga=json.loads(next((pathlib.Path(tmp)/'gpu'/'exports').glob('*.links.json')).read_text());ca=json.loads(next((pathlib.Path(tmp)/'cpu'/'exports').glob('*.links.json')).read_text())
        assert ga['run_id']==ca['run_id'] and ga['slices']==ca['slices'],'edge payload must match CPU'
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
    # ---- v3.5 external data files: mounting, override effect, error codes (SDD §5.1/§5.2/§7.1/§11.2) ----
    exsat=str(bin/'examples'/'satellites.xml');exset=str(bin/'examples'/'settings.json')
    shortrun=['--duration','60','--package','60','--step','10','--start','2026-01-01T00:00:00Z','--threads','2']
    def spred(out,*extra):
        return subprocess.run([str(bin/('satpred'+suffix)),'--output',str(out),*shortrun,*map(str,extra)],env=env,capture_output=True,text=True)
    def manifest(path):
        return json.loads((pathlib.Path(path)/'manifest.json').read_text())
    epoch=1767225600;times=[epoch-600+600*i for i in range(151)]
    ext=pathlib.Path(tmp)/'ext';ext.mkdir()
    (ext/'eop.csv').write_text('utc_seconds,dut1_s,xp_arcsec,yp_arcsec,tt_minus_utc_s\n'+'\n'.join(f'{t},0.001,0.01,0.02,69.184' for t in times)+'\n')
    (ext/'eph.csv').write_text('utc_seconds,sun_x,sun_y,sun_z,moon_x,moon_y,moon_z\n'+'\n'.join(f'{t},1.496e11,0,0,3.844e8,0,0' for t in times)+'\n')
    (ext/'den.csv').write_text('utc_seconds,rho_kg_m3\n'+'\n'.join(f'{t},1e-13' for t in times)+'\n')
    (ext/'phys.json').write_text(json.dumps({'1':{'mass_kg':160,'cd':2.3,'source':'documented','reference':'regression'}}))
    (ext/'gravity.json').write_text(json.dumps({'J':{'2':0.00108262617385222}}))
    (ext/'settings.json').write_text(json.dumps({'eop_csv':'eop.csv','ephemeris_csv':'eph.csv','density_csv':'den.csv','phys_json':'phys.json','gravity_json':'gravity.json'}))
    (ext/'phys_only.json').write_text(json.dumps({'phys_json':'phys.json'}))
    (ext/'missing.json').write_text(json.dumps({'eop_csv':'absent.csv'}))
    (ext/'short_eop.csv').write_text('utc_seconds,dut1_s,xp_arcsec,yp_arcsec,tt_minus_utc_s\n'+'\n'.join(f'{epoch-90000+600*i},0.0,0.0,0.0,69.184' for i in range(3))+'\n')
    (ext/'short.json').write_text(json.dumps({'eop_csv':'short_eop.csv'}))
    base=spred(pathlib.Path(tmp)/'ext_base','--config',exsat,'--settings',exset);assert base.returncode==0,base.stderr
    bm=manifest(pathlib.Path(tmp)/'ext_base')
    allx=spred(pathlib.Path(tmp)/'ext_all','--config',exsat,'--settings',str(ext/'settings.json'));assert allx.returncode==0,allx.stderr
    am=manifest(pathlib.Path(tmp)/'ext_all')
    assert sorted(q['role'] for q in am['inputs']['external_data'])==['density','eop','ephemeris','gravity','phys'],am['inputs']['external_data']
    assert all(len(q['sha256'])==64 and q['size']>0 for q in am['inputs']['external_data']),'external_data sha256/size recorded'
    assert [q for q in am['phys_sources'] if q['gid']==1]==[{'gid':1,'source':'documented','reference':'regression','not_used':False}],am['phys_sources']
    phys=spred(pathlib.Path(tmp)/'ext_phys','--config',exsat,'--settings',str(ext/'phys_only.json'));assert phys.returncode==0,phys.stderr
    pm=manifest(pathlib.Path(tmp)/'ext_phys')
    assert pm['products'][0]['sha256']!=bm['products'][0]['sha256'],'phys_json override must change the integrated product'
    assert pm['inputs']['config_sha256']==bm['inputs']['config_sha256'] and pm['inputs']['settings_sha256']!=bm['inputs']['settings_sha256'],'settings-only change touches settings_sha256 only'
    assert spred(pathlib.Path(tmp)/'ext_missing','--config',exsat,'--settings',str(ext/'missing.json')).returncode==3,'unreadable external file -> Io(3)'
    assert spred(pathlib.Path(tmp)/'ext_short','--config',exsat,'--settings',str(ext/'short.json')).returncode==2,'EOP table not covering the window -> Config(2)'

    # ---- v3.5 Walker layer per-satellite overrides (SDD §4.1/§4.2/§11.1) ----
    src=(bin/'examples'/'satellites.xml').read_text();assert src.count('  </layer>')==1,'default sample must hold exactly one Walker layer'
    wdir=pathlib.Path(tmp)/'walker';wdir.mkdir()
    (wdir/'ovr.xml').write_text(src.replace('  </layer>','    <satellite plane="0" slot="1" alt_km="1300" name="OVR1"/>\n  </layer>'))
    (wdir/'bad.xml').write_text(src.replace('  </layer>','    <satellite plane="3" slot="0"/>\n  </layer>'))
    (wdir/'dup.xml').write_text(src.replace('  </layer>','    <satellite plane="0" slot="1"/>\n    <satellite plane="0" slot="1"/>\n  </layer>'))
    ovr=spred(pathlib.Path(tmp)/'walk_ovr','--config',str(wdir/'ovr.xml'),'--settings',exset);assert ovr.returncode==0,ovr.stderr
    om=manifest(pathlib.Path(tmp)/'walk_ovr');m1=om['catalog']['members'][1]
    assert (m1['name'],m1['plane'],m1['slot'],m1['gid'])==('OVR1',0,1,1),m1
    assert om['inputs']['config_sha256']!=bm['inputs']['config_sha256'],'override must enter config_sha256'
    assert om['run']['run_id']!=bm['run']['run_id'],'override must enter run_id'
    assert spred(pathlib.Path(tmp)/'walk_bad','--config',str(wdir/'bad.xml'),'--settings',exset).returncode==2,'plane outside Walker grid -> Config(2)'
    assert spred(pathlib.Path(tmp)/'walk_dup','--config',str(wdir/'dup.xml'),'--settings',exset).returncode==2,'duplicate (plane,slot) -> Config(2)'


    # ---- v3.5 orbit_engine appendix-D diagnostic protocol (SDD appendix D) ----
    ddir=pathlib.Path(tmp)/'diag';req=diag_assets.build_diag(ddir)
    def diag(request,exit=0):
        p=subprocess.run([str(bin/('orbit_engine'+suffix)),str(request)],env=env,capture_output=True)
        assert p.returncode==exit,(request,p.returncode,p.stderr[:400])
        return p
    first=diag(req);r=diag_assets.parse_orbst01(first.stdout)
    assert (r['N'],r['Ns'],r['M'])==(2,64,3) and r['t0_ns']==diag_assets.RUN_START_NS and r['step_ns']==10**9 and r['global_index0']==0,r
    assert [s['global_index'] for s in r['slices']]==[0,1,2] and [s['t_ns'] for s in r['slices']]==[diag_assets.RUN_START_NS+i*10**9 for i in range(3)]
    assert r['slices'][0]['q']==[7.0e6,0.0,0.0,0.0,7500.0,0.0],'initial state echoed verbatim'
    for s in r['slices']:
        assert all(math.isfinite(v) for v in s['q']) and math.sqrt(sum(v*v for v in s['q'][:3]))>6.5e6,'finite, physical state'
    assert r['result']['failures']==0 and json.loads(r['result']['json'])==[],r['result']
    assert diag(req).stdout==first.stdout,'diagnostic stream must be reproducible'
    # window continuation: resume at global_index0=1 from the propagated state while keeping a prior failure.
    nan=float('nan');st=diag_assets.default_states()
    req2=diag_assets.build_diag(ddir/'cont',m=2,global_index0=1,states=[(tuple(r['slices'][1]['q']),st[0][1]),((nan,)*6,st[1][1])],
                                prior_failures=[{'index':1,'time_ns':diag_assets.RUN_START_NS+500000000,'reason':'verify'}])
    r2=diag_assets.parse_orbst01(diag(req2).stdout)
    assert [s['global_index'] for s in r2['slices']]==[1,2] and r2['t0_ns']==diag_assets.RUN_START_NS+10**9,r2
    assert struct.pack('<6d',*r2['slices'][0]['q'])==struct.pack('<6d',*r['slices'][1]['q']),'window continuation must overlap bit-exactly'
    fj=json.loads(r2['result']['json'])
    assert r2['result']['failures']==1 and len(fj)==1 and fj[0]['index']==1 and fj[0]['reason']=='verify' and 'global_index' not in fj[0],fj
    assert subprocess.run([str(bin/('orbit_engine'+suffix))],env=env,capture_output=True).returncode==1,'no request -> Arg(1)'
    assert subprocess.run([str(bin/('orbit_engine'+suffix)),str(ddir/'absent.json')],env=env,capture_output=True).returncode==3,'missing request -> Io(3)'
    bad=bytearray((ddir/'states.sbin').read_bytes());bad[100]^=1;(ddir/'corrupt.sbin').write_bytes(bytes(bad))
    (ddir/'corrupt.json').write_text(json.dumps(dict(json.loads(req.read_text()),states_file='corrupt.sbin')))
    diag(ddir/'corrupt.json',exit=7)

    print(json.dumps(dict(status='PASS',summary=summary,l2_01_annotation='PASS',l2_01b_fast_path='PASS',state_file_regression='PASS',external_data='PASS',walker_override='PASS',orbit_engine_appendix_d='PASS')))
