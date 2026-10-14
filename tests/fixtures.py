#!/usr/bin/env python3
import pathlib, struct, json, hashlib
START=1767225600000000000

def crc(data):
    c=0xffffffff
    for v in data:
        c^=v
        for _ in range(8):c=(c>>1)^(0x82f63b78 if c&1 else 0)
    return c^0xffffffff

def align(n,a):return (n+a-1)//a*a

def make(root,slices,ground=(),case=None):
    """ground: optional list of (name,kind) appended after the 3 satellites (kind 1 user, 2 ground station).
    case: optional dict with case_name/rf_parameters (upstream v3.5 satnet-rf-annotation-1) written into graph metadata and manifest."""
    root=pathlib.Path(root);(root/'orbits').mkdir(parents=True,exist_ok=True);(root/'graphs').mkdir(exist_ok=True)
    M=len(slices);Ns=64;N=3;step=10000000000;end=START+M*step
    sat=bytearray(Ns*12)
    for i in range(Ns):struct.pack_into('<I',sat,i*4,i if i<N else 0xffffffff)
    for i in range(N):struct.pack_into('<H',sat,4*Ns+i*2,1);struct.pack_into('<H',sat,8*Ns+i*2,i)
    names=bytearray(Ns*24)
    for i in range(N):names[i*24:i*24+2]=f'n{i}'.encode()
    tensor=bytearray()
    for j in range(M):
        for c in range(6):
            for i in range(Ns):tensor+=struct.pack('<Q',0x7ff8000000000000) if i>=N else struct.pack('<d',7000000+i*299792.458 if c==0 else 0.)
    time=bytearray(align(M*8,64)+M*4)
    for j in range(M):struct.pack_into('<q',time,j*8,START+j*step);struct.pack_into('<I',time,align(M*8,64)+j*4,crc(tensor[j*Ns*48:(j+1)*Ns*48]))
    off=640;directory=bytearray();chunks=[]
    for typ,al,blob in [(1,6,sat),(2,6,names),(3,6,time),(16,12,tensor)]:
        off=align(off,1<<al);directory+=struct.pack('<IHHQQII',typ,al,0,off,len(blob),0 if typ==16 else crc(blob),0);chunks.append((off,blob));off+=len(blob)
    b=bytearray(off+32);b[:8]=b'SATCONT3';struct.pack_into('<HHI',b,8,3,0,512);b[16:20]=bytes([1,1,0,2]);struct.pack_into('<IQIIII',b,20,4,512,N,Ns,M,M);struct.pack_into('<qqqqQII',b,48,START,step,START,end,0,0,1);b[97:100]=bytes([6,63,0]);struct.pack_into('<QQq',b,120,Ns*8,Ns*48,M*step);b[200:206]=b'c-rk8\0';b[216:220]=b'cpu\0';b[232:242]=b'UTC-POSIX\0';struct.pack_into('<II',b,280,1,65536);b[512:640]=directory
    for o,v in chunks:b[o:o+len(v)]=v
    struct.pack_into('<4sIQQ8s',b,off,b'SEND',crc(directory),len(b),len(tensor),b'\0'*8);struct.pack_into('<I',b,508,crc(b[:508]));(root/'orbits/pred_000000.sbin').write_bytes(b);sha=hashlib.sha256(b).digest()
    nodes=[dict(id=i,kind=0,name=f'n{i}',gid=i,layer=1,plane=0,slot=i,P=1,S=3,uplink_bps=10000000000,downlink_bps=10000000000,antennas=6) for i in range(N)]
    nodes+=[dict(id=N+i,kind=k,name=nm,uplink_bps=50000000,downlink_bps=50000000,antennas=16,xyz=[0,0,0],up=[0,0,1]) for i,(nm,k) in enumerate(ground)]
    NT=len(nodes);ek=lambda a,z:1 if (a>=N)!=(z>=N) else 0
    meta=dict(format='satnet-metadata-1.1',software_version='synthetic-test-1',actual_backend='cpu',run_id='00'*32,satellite_xml_sha256='00'*32,ground_xml_sha256='00'*32,source_hash_available=True,source_orbit_sha256=sha.hex(),source_accuracy_class=2,source_frame_method='synthetic-fixture',nodes=nodes,effective_policy={},models={},assumptions={})
    if case:meta.update({k:case[k] for k in ('case_name','rf_parameters') if k in case})
    ms=json.dumps(meta,separators=(',',':')).encode();ti=align(256+len(ms),64);po=align(ti+M*32,4096);g=bytearray(po);g[:8]=b'SATNET1\0';struct.pack_into('<HHIIIIIQQQQqqQQQ',g,8,1,1,256,0,NT,M,32,256,len(ms),ti,po,START,step,0,sum(map(len,slices)),0);g[104:136]=sha;g[256:256+len(ms)]=ms
    for j,es in enumerate(slices):
        payload=b''.join(struct.pack('<IIIQdI',a,z,d,bw,dist,ek(a,z)) for a,z,d,bw,dist in sorted(es));struct.pack_into('<qQQII',g,ti+j*32,START+j*step,len(g),len(es),crc(payload),0);g+=payload
    n=sum(map(len,slices));ft=len(g);g+=struct.pack('<8sIIQQ',b'NETEND1\0',crc(g[ti:ti+M*32]),crc(ms),ft+32,n*32);struct.pack_into('<Q',g,96,len(g));struct.pack_into('<I',g,252,crc(g[:252]));(root/'graphs/pred_000000.ngb').write_bytes(g)
    product=dict(source_file='orbits/pred_000000.sbin',source_sha256=sha.hex(),graph_file='graphs/pred_000000.ngb',graph_sha256=hashlib.sha256(g).hexdigest(),M=M,t0_ns=START,global_index0=0,directed_edges=n)
    manifest=dict(format='SATNET-1.1',status='complete',run=dict(run_id='00'*32,start_ns=START,end_ns=end,step_ns=step),nodes=dict(satellites=3,ground=len(ground),total=NT),products=[product])
    if case:manifest.update({k:case[k] for k in ('case_name','rf_parameters') if k in case})
    (root/'manifest.json').write_text(json.dumps(manifest))
    return root
