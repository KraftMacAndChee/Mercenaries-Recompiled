"""Execute production GPU selection with injected adapters and failures."""
from pathlib import Path
import subprocess, tempfile, shutil
ROOT=Path(__file__).resolve().parents[2]
PRE=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
typedef int32_t HRESULT;
typedef unsigned UINT;
typedef struct {int id;} IDXGIFactory6;
typedef struct {int id;} IDXGIAdapter1;
typedef IDXGIAdapter1 IDXGIAdapter;
typedef struct {unsigned Flags,VendorId,DeviceId;wchar_t Description[32];} DXGI_ADAPTER_DESC1;
typedef struct {int chosen;} D3D8DeviceState;
typedef struct {int dummy;} D3DPRESENT_PARAMETERS;
#define SUCCEEDED(h) ((h)>=0)
#define FAILED(h) ((h)<0)
#define DXGI_ADAPTER_FLAG_SOFTWARE 2u
#define DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE 2u
static const int IID_IDXGIFactory6=6,IID_IDXGIAdapter1=1;
static IDXGIFactory6 factory;
static IDXGIAdapter1 adapters[4]={{0},{1},{2},{3}};
static unsigned available,count,software,bad_desc,failed,default_fails,enum_error;
static unsigned frefs,refs[4],attempts[5],n,enum_calls;
static HRESULT CreateDXGIFactory1(const int *iid,void **out){
 assert(iid==&IID_IDXGIFactory6);if(!available)return -1;*out=&factory;frefs++;return 0;
}
static HRESULT IDXGIFactory6_EnumAdapterByGpuPreference(IDXGIFactory6 *f,UINT i,unsigned pref,const int *iid,void **out){
 assert(f==&factory && pref==DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE && iid==&IID_IDXGIAdapter1);enum_calls++;
 if(i>=count || i==enum_error)return -1;refs[i]++;*out=&adapters[i];return 0;
}
static void IDXGIFactory6_Release(IDXGIFactory6 *f){assert(f==&factory&&frefs);frefs--;}
static void IDXGIAdapter1_Release(IDXGIAdapter1 *a){assert(refs[a->id]);refs[a->id]--;}
static HRESULT IDXGIAdapter1_GetDesc1(IDXGIAdapter1 *a,DXGI_ADAPTER_DESC1 *d){
 if(bad_desc&(1u<<a->id))return -1;memset(d,0,sizeof(*d));d->Flags=(software&(1u<<a->id))?2:0;return 0;
}
static HRESULT d3d11_try_device_and_swap_chain(D3D8DeviceState *s,D3DPRESENT_PARAMETERS *pp,IDXGIAdapter *a){
 unsigned id=a?(unsigned)a->id:4u;(void)pp;assert(n<5);attempts[n++]=id;
 if(a?(failed&(1u<<id)):default_fails)return -1;s->chosen=(int)id;return 0;
}
static void xbox_preview_log_event(const char *cat,const char *fmt,...){(void)cat;(void)fmt;}
'''
POST=r'''
static void reset(unsigned size){available=1;count=size;software=bad_desc=failed=default_fails=0;enum_error=99;frefs=n=enum_calls=0;memset(refs,0,sizeof(refs));memset(attempts,0,sizeof(attempts));}
static void run(int expected){
 D3D8DeviceState s={-1};D3DPRESENT_PARAMETERS pp={0};HRESULT hr=d3d11_create_device_and_swap_chain(&s,&pp);
 assert((expected<0)==FAILED(hr));assert(s.chosen==expected);assert(!frefs);for(unsigned i=0;i<4;i++)assert(!refs[i]);
}
int main(void){
 /* The first adapter is DXGI's high-performance choice, independent of vendor/VRAM. */
 reset(2);run(0);assert(n==1 && attempts[0]==0 && enum_calls==1);
 reset(1);run(0);assert(n==1); /* Integrated-only hardware remains supported. */
 reset(3);software=1;run(1);assert(n==1&&attempts[0]==1);
 reset(3);bad_desc=1;run(1);assert(n==1&&attempts[0]==1);
 reset(2);failed=1;run(1);assert(n==2&&attempts[0]==0&&attempts[1]==1);
 reset(2);failed=3;run(4);assert(n==3&&attempts[2]==4);
 reset(2);software=3;run(4);assert(n==1&&attempts[0]==4);
 reset(0);run(4);assert(n==1);
 reset(2);available=0;run(4);assert(n==1&&enum_calls==0);
 reset(2);enum_error=0;run(4);assert(n==1);
 reset(2);failed=1;enum_error=1;run(4);assert(n==2);
 reset(2);failed=3;default_fails=1;run(-1);assert(n==3);
 puts("PASS: preferred GPU ordering, integrated-only, software/invalid adapters, adapter failures, missing API, enumeration failure, default fallback and reference ownership");
 return 0;
}
'''
def main():
 s=(ROOT/'src/d3d/d3d8_device.c').read_text(encoding='utf-8')
 a=s.index('static HRESULT d3d11_create_device_and_swap_chain(')
 b=s.index('static HRESULT d3d11_create_render_targets(',a)
 with tempfile.TemporaryDirectory(prefix='gpu-preference-') as td:
  p=Path(td)/'test.c';exe=p.with_suffix('.exe');p.write_text(PRE+s[a:b]+POST,encoding='utf-8')
  subprocess.run([shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe','-std=c11','-O2',str(p),'-o',str(exe)],check=True)
  subprocess.run([str(exe)],check=True)
if __name__=='__main__':main()
