"""Production flat-DSK merger: header offsets, layout guards, and load order.
Optional local archives are read only and never included in test distributions.
"""
from pathlib import Path
import argparse, hashlib, json, shutil, struct, subprocess, tempfile
from tools.recomp.test_mod_loader_native import msvc
ROOT = Path(__file__).resolve().parents[2]
HARNESS = r'''
#define UNICODE
#define _UNICODE
#include "mod_loader.cpp"
namespace mercmods { BOOL selector(HWND,const fs::path&,const fs::path&,Selection){return FALSE;} }
int wmain(int argc,wchar_t **argv) {
    try {
        if(argc<3)return 3;
        std::vector<mercmods::fs::path> layers;
        for(int i=3;i<argc;++i)layers.emplace_back(argv[i]);
        mercmods::merge_dsk(argv[2],layers,argv[1]);
        return 0;
    } catch(const std::exception &e) { fprintf(stderr,"%s\n",e.what());return 2; }
}
'''

def write_archive(path, records, offset=0, tail=bytes(2048)):
    path.write_bytes(struct.pack('<II',len(records),offset)+
        b''.join(struct.pack('<III',len(data),key,kind) for key,kind,data in records)+
        b''.join(data for key,kind,data in records)+tail)

def inspect(path):
    with path.open('rb') as f:
        count,offset=struct.unpack('<II',f.read(8))
        directory=[struct.unpack('<III',f.read(12)) for _ in range(count)]
        records={}
        for size,key,kind in directory:
            data=f.read(size);assert len(data)==size
            assert (key,kind) not in records
            records[key,kind]=(size,hashlib.sha256(data).hexdigest())
        return offset,records,f.read()

def expected(base,layers):
    current=dict(base)
    for layer in layers:
        for key in base.keys()-layer.keys():current.pop(key,None)
        for key,data in layer.items():
            if base.get(key)!=data:current[key]=data
    return current

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--retail-data',type=Path)
    parser.add_argument('--first-mod',type=Path)
    parser.add_argument('--second-mod',type=Path)
    parser.add_argument('--report',type=Path)
    args=parser.parse_args()
    assert all((args.retail_data,args.first_mod,args.second_mod)) or not any((args.retail_data,args.first_mod,args.second_mod))
    report={'synthetic':'pending','realArchives':[]}
    with tempfile.TemporaryDirectory(prefix='merc-dsk-') as td:
        root=Path(td);cpp=root/'test.cpp';cpp.write_text(HARNESS);exe=root/'test.exe';env=msvc()
        subprocess.run([shutil.which('cl',path=env['PATH']),'/nologo','/std:c++17','/EHsc','/O2',
            '/I'+str(ROOT/'ports/mercenaries/src'),str(cpp),'/Fo'+str(root/'test.obj'),
            '/Fe'+str(exe),'/link','bcrypt.lib','user32.lib'],env=env,check=True)
        base,a,b,out=[root/n for n in ('base.dsk','a.dsk','b.dsk','out.dsk')]
        original=[(1,1,b'base'),(2,1,b'keep'),(3,1,b'delete'),(1,2,b'other type')]
        first=[(1,1,b'first'),(2,1,b'keep'),(4,1,b'addition'),(1,2,b'other type')]
        second=[(1,1,b'second longer'),*original[1:]]
        def run(base,layers,ok=True,reason=None):
            if out.exists():out.unlink()
            result=subprocess.run([str(exe),str(out),'' if base is None else str(base),*[str(p) for p in layers]],capture_output=True,text=True)
            assert (result.returncode==0)==ok,result.stderr
            if not ok:
                assert reason in result.stderr,result.stderr
                assert str(base if layers==[] else layers[-1]) in result.stderr,result.stderr
                assert not out.exists()
                return
            header,records,tail=inspect(out)
            assert header==0
            assert records==expected({} if base is None else inspect(base)[1],[inspect(p)[1] for p in layers])
            assert not any(tail)
        # Differing header words are not asset edits or compatibility versions.
        for header in (0,0x0D07FAEC,0xFFFFFFFF):
            write_archive(base,original,offset=header)
            write_archive(a,first,offset=0x0D07FAEC,tail=bytes(2044)+b'SIGN')
            write_archive(b,second,offset=17,tail=b'')
            run(base,[a,b]);run(base,[b,a])
        # Header-only differences must not erase unrelated earlier edits.
        write_archive(b,original,offset=0x0D07FAEC);run(base,[a,b])
        assert inspect(out)[1]==inspect(a)[1]
        # Grid index is located after assets, regardless of header offset/equality.
        for header in (0,0xFFFFFFFF):
            for tail in (struct.pack('<I',0x00FF8040)+bytes(2044),struct.pack('<I',0x00FF8040)+bytes(4092)):
                write_archive(b,second,offset=header,tail=tail)
                run(base,[a,b],False,'Grid-indexed archives')
                # A grid-bearing retail base is also unsupported.
                run(b,[],False,'Grid-indexed archives')
        write_archive(b,second,tail=b'unknown'+bytes(2041));run(base,[a,b],False,'non-padding')
        write_archive(b,second,tail=b'x');run(base,[a,b],False,'Unsupported data')
        write_archive(b,second[:-1]+[(1,1,b'duplicate')]);run(base,[a,b],False,'duplicate')
        b.write_bytes(struct.pack('<II',1,0));run(base,[a,b],False,'directory')
        b.write_bytes(b'1234');run(base,[a,b],False,'Truncated')
        write_archive(b,[(1,1,b'payload')]);b.write_bytes(b.read_bytes()[:21]);run(base,[a,b],False,'bounds')
        write_archive(base,[],offset=5,tail=b'');write_archive(b,[],offset=6,tail=b'');run(base,[b])
        # New archives (no retail base) are unioned; later layers win shared asset IDs.
        new_first=[(1,1,b'new first'),(4,1,b'addition')]
        new_second=[(1,1,b'new second'),(5,1,b'other')]
        write_archive(a,new_first,tail=bytes(2048));write_archive(b,new_second,tail=b'')
        run(None,[a,b])
        assert inspect(out)[1]==expected({},[inspect(a)[1],inspect(b)[1]])
        assert inspect(out)[1][1,1][1]==hashlib.sha256(b'new second').hexdigest()
        assert len(inspect(out)[2])==2048
        # Without padding in any layer, a new archive has no trailer.
        write_archive(a,new_first,tail=b'');run(None,[a,b])
        assert len(inspect(out)[2])==0
        # Grid-indexed layers are rejected for new archives as well.
        write_archive(b,new_second,tail=struct.pack('<I',0x00FF8040)+bytes(2044))
        run(None,[a,b],False,'Grid-indexed archives')
        write_archive(b,new_second,tail=b'')
        report['synthetic']='passed: unequal offsets, normalized output, byte-level precedence, additions/deletions, asset types, signature padding, real-grid rejection, malformed inputs'
        if args.retail_data:
            for name in ('assets.dsk','english.dsk','locked.dsk','streamed.dsk'):
                paths=[folder/name for folder in (args.retail_data,args.first_mod,args.second_mod)]
                before=[hashlib.sha256(p.read_bytes()).hexdigest() for p in paths]
                for order in ([paths[1],paths[2]],[paths[2],paths[1]]):
                    run(paths[0],order)
                    report['realArchives'].append({'archive':name,'order':[p.parent.name for p in order],
                        'recordCount':len(inspect(out)[1]),'outputHeader':inspect(out)[0],
                        'allAssetHashesMatchIndependentOracle':True})
                assert before==[hashlib.sha256(p.read_bytes()).hexdigest() for p in paths]
    if args.report:args.report.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))
if __name__=='__main__':main()
