
#define COBJMACROS
#include "kernel.h"
#include "d3d8_compiler.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
extern BOOL xbox_prepare_empty_cache_volume(const char *path);
void xbox_log(int level,const char* subsystem,const char* fmt,...){}
void xbox_preview_log_event(const char* category,const char* fmt,...){}
/* Production helpers are inserted here by the Python test driver. */
static ptrdiff_t g_xbox_mem_offset;
static uint32_t g_esp, g_eax;
/* PRODUCTION_HELPERS */
int main(int argc,char**argv){
 assert(argc==2);
 xbox_path_init(argv[1],argv[1]);
 assert(xbox_prepare_empty_cache_volume("\\Device\\Harddisk0\\Partition5"));
 assert(xbox_prepare_empty_cache_volume("\\device\\harddisk0\\partition5\\"));
 assert(!xbox_prepare_empty_cache_volume("\\Device\\Harddisk0\\Partition50"));
 assert(!xbox_prepare_empty_cache_volume("\\Device\\Harddisk0\\Partition5\\..\\UserData"));
 assert(!xbox_prepare_empty_cache_volume("\\Device\\Harddisk0\\Partition1"));
 assert(!xbox_prepare_empty_cache_volume("Z:\\"));
 uint8_t *guest=(uint8_t*)calloc(1,0x40000);assert(guest);
 g_xbox_mem_offset=(ptrdiff_t)guest;g_esp=0x11000;g_eax=0x12345678;
 const char *volume="\\Device\\Harddisk0\\Partition4";
 *(uint32_t*)(guest+g_esp+4)=0x12000;
 *(uint16_t*)(guest+0x12000)=(uint16_t)strlen(volume);
 *(uint32_t*)(guest+0x12004)=0x13000;
 memcpy(guest+0x13000,volume,strlen(volume));
 assert(recomp_prepare_empty_cache_volume()==1&&g_esp==0x1100c&&g_eax==1);
 g_esp=0x11000;g_eax=0x12345678;*(uint32_t*)(guest+0x12004)=0xfffffff0;
 assert(!recomp_prepare_empty_cache_volume()&&g_esp==0x11000&&g_eax==0x12345678);
 *(uint32_t*)(guest+g_esp+4)=0xfffffff0;
 assert(!recomp_prepare_empty_cache_volume()&&g_esp==0x11000);
 free(guest);
 uintptr_t arena=0x1000000000ull;
 assert(xbox_guest_arena_available(arena));
 void *block=VirtualAlloc((void*)(arena+0xF0000000ull),65536,MEM_RESERVE,PAGE_NOACCESS);
 assert(block);assert(!xbox_guest_arena_available(arena));assert(VirtualFree(block,0,MEM_RELEASE));
 assert(xbox_guest_arena_available(arena));
 assert(!xbox_guest_arena_available(UINTPTR_MAX-0x1000));
 WCHAR path[MAX_PATH];
 assert(xbox_translate_path("\\Device\\Harddisk0\\Partition5\\preserve.bin",path,MAX_PATH));
 HANDLE file=CreateFileW(path,GENERIC_WRITE,0,0,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,0);
 assert(file!=INVALID_HANDLE_VALUE);DWORD n;assert(WriteFile(file,"saved",5,&n,0)&&n==5);CloseHandle(file);
 assert(!xbox_prepare_empty_cache_volume("\\Device\\Harddisk0\\Partition5"));
 file=CreateFileW(path,GENERIC_READ,0,0,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,0);assert(file!=INVALID_HANDLE_VALUE);
 char data[5];assert(ReadFile(file,data,5,&n,0)&&n==5&&!memcmp(data,"saved",5));CloseHandle(file);
 const char *shader="float4 main(float4 p:SV_POSITION):SV_TARGET{return float4(isfinite(p.x),isnan(p.y),0,1);}";
 ID3DBlob *a=0,*b=0,*error=0;
 HRESULT ra=D3DCompile(shader,strlen(shader),"comparison",0,0,"main","ps_5_0",0,0,&a,&error);
 assert(SUCCEEDED(ra));if(error){ID3D10Blob_Release(error);error=0;}
 HRESULT rb=d3d8_compile_shader(shader,strlen(shader),"comparison",0,0,"main","ps_5_0",0,0,&b,&error);
 assert(ra==rb&&a&&b);
 assert(ID3D10Blob_GetBufferSize(a)==ID3D10Blob_GetBufferSize(b));
 assert(!memcmp(ID3D10Blob_GetBufferPointer(a),ID3D10Blob_GetBufferPointer(b),ID3D10Blob_GetBufferSize(a)));
 assert(GetModuleHandleW(L"d3dcompiler_47_native.dll")==NULL);
 ID3D10Blob_Release(a);ID3D10Blob_Release(b);if(error)ID3D10Blob_Release(error);
 puts("PASS: fresh cache setup, occupied-cache preservation, path rejection, byte-identical Windows shader compilation, Wine DLL never loaded on Windows");
 return 0;
}
