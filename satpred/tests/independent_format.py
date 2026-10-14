#!/usr/bin/env python3
"""Independent format smoke verifier. Tests only; never invoked by product."""
import argparse, hashlib, json, pathlib, struct

def crc32c(data):
    c=0xffffffff
    for v in data:
        c ^= v
        for _ in range(8):
            c=(c>>1) ^ (0x82f63b78 if c&1 else 0)
    return c ^ 0xffffffff

def orbit(path):
    b=path.read_bytes()
    assert b[:8]==b'SATCONT3'
    assert crc32c(b[:508])==struct.unpack_from('<I',b,508)[0]
    N,Ns,M,full=struct.unpack_from('<IIII',b,32)
    t0,step,start,end,k0=struct.unpack_from('<qqqqQ',b,48)
    sections={}
    sc=struct.unpack_from('<I',b,20)[0]
    for i in range(sc):
        typ,al,flags,off,size,crc,res=struct.unpack_from('<IHHQQII',b,512+i*32)
        assert off%(1<<al)==0 and not flags and not res
        if typ!=16: assert crc32c(b[off:off+size])==crc
        sections[typ]=(off,size)
    assert Ns==(N+63)//64*64
    off,size=sections[16]
    ti,_=sections[3]
    for j in range(M):
        assert struct.unpack_from('<q',b,ti+8*j)[0]==t0+j*step
        c=struct.unpack_from('<I',b,ti+(8*M+63)//64*64+4*j)[0]
        assert c==crc32c(b[off+j*Ns*48:off+(j+1)*Ns*48])
    assert b[-32:-28]==b'SEND'
    assert struct.unpack_from('<Q',b,len(b)-24)[0]==len(b)
    assert crc32c(b[512:512+sc*32])==struct.unpack_from('<I',b,len(b)-28)[0]
    return dict(N=N,Ns=Ns,M=M,full=full,k0=k0,t0=t0,step=step,start=start,end=end,sections=sections,bytes=b)

def graph(path,source):
    b=path.read_bytes()
    assert b[:8]==b'SATNET1\0'
    assert crc32c(b[:252])==struct.unpack_from('<I',b,252)[0]
    assert b[104:136]==hashlib.sha256(source.read_bytes()).digest()
    N,M=struct.unpack_from('<II',b,20)
    mo,ms,ti,po=struct.unpack_from('<QQQQ',b,32)
    meta=json.loads(b[mo:mo+ms])
    assert len(meta['nodes'])==N
    off=po; total=0
    for j in range(M):
        t,a,n,c,res=struct.unpack_from('<qQQII',b,ti+j*32)
        assert a==off and not res and crc32c(b[a:a+n*32])==c
        previous=None
        for k in range(n):
            src,dst,delay,bw,distance,kind=struct.unpack_from('<IIIQdI',b,a+k*32)
            assert src<N and dst<N and src!=dst and bw>0 and distance>0
            assert previous is None or (src,dst)>previous
            previous=(src,dst)
        off+=n*32; total+=n
    assert off==len(b)-32
    magic,tc,mc,size,payload=struct.unpack_from('<8sIIQQ',b,len(b)-32)
    assert magic==b'NETEND1\0' and size==len(b) and payload==total*32
    assert tc==crc32c(b[ti:ti+M*32]) and mc==crc32c(b[mo:mo+ms])
    return M,total

def main():
    ap=argparse.ArgumentParser();ap.add_argument('run');args=ap.parse_args()
    root=pathlib.Path(args.run);m=json.loads((root/'manifest.json').read_text())
    total=edges=packages=0
    for p in m['products']:
        for field,sha in [('source_file','source_sha256'),('graph_file','graph_sha256'),('links_file','links_sha256')]:
            assert hashlib.sha256((root/p[field]).read_bytes()).hexdigest()==p[sha]
        o=orbit(root/p['source_file']);M,E=graph(root/p['graph_file'],root/p['source_file'])
        assert M==o['M']
        links=json.loads((root/p['links_file']).read_text())
        assert len(links['slices'])==M
        assert sum(len(s['links']) for s in links['slices'])==E
        for s in links['slices']:
            assert s['valid_from_ns']==s['timestamp_ns']
            assert s['valid_to_ns']==min(s['timestamp_ns']+o['step'],o['end'])
        total+=M;edges+=E;packages+=1
    print(json.dumps(dict(status='PASS',slices=total,packages=packages,directed_edges=edges)))
if __name__=='__main__':main()
