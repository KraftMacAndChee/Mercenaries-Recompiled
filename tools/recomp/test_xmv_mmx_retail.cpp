#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
namespace {
constexpr std::uintptr_t kSnapshotBase=0x00010000u;
constexpr std::size_t kSnapshotSize=0x03FF0000u;
constexpr std::size_t kSmallSnapshotSize=0x00FF0000u;
constexpr std::uintptr_t kGuestBase=0x00800000u;
constexpr std::size_t kGuestSize=0x03800000u;
constexpr std::size_t kCompareOffset=0x000C0000u;
constexpr std::uint32_t kXmvVirtualAddress=0x00255620u;
constexpr std::uint32_t kXmvRawAddress=0x00246000u;
constexpr std::uint32_t kXmvRawSize=0x00027F84u;
constexpr std::uint32_t kXmvVirtualSize=0x00027F94u;
constexpr std::uint32_t kXmvRelocationOffsets[]={
#include "xmv_relocation_offsets.inc"
};
std::vector<std::uint8_t> read_file(const char* path){
 FILE* f=std::fopen(path,"rb");if(!f){std::fprintf(stderr,"failed to open %s\n",path);std::exit(2);}
 std::fseek(f,0,SEEK_END);const long n=std::ftell(f);std::rewind(f);std::vector<std::uint8_t>b(static_cast<std::size_t>(n));
 if(std::fread(b.data(),1,b.size(),f)!=b.size()){std::fprintf(stderr,"failed to read %s\n",path);std::exit(2);}std::fclose(f);return b;
}
LONG WINAPI crash_filter(EXCEPTION_POINTERS* i){const auto*c=i->ContextRecord;const auto access=i->ExceptionRecord->NumberParameters>1?i->ExceptionRecord->ExceptionInformation[1]:0u;std::fprintf(stderr,"XMV replay exception code=%08X address=%p access=%08X eip=%08X esp=%08X eax=%08X ebx=%08X ecx=%08X edx=%08X esi=%08X edi=%08X\n",i->ExceptionRecord->ExceptionCode,i->ExceptionRecord->ExceptionAddress,static_cast<unsigned>(access),c->Eip,c->Esp,c->Eax,c->Ebx,c->Ecx,c->Edx,c->Esi,c->Edi);std::fflush(stderr);return EXCEPTION_EXECUTE_HANDLER;}
std::uint8_t* g_nested_mmx_entry;
std::uint8_t g_nested_mmx_original;
bool g_block_final_mode;
bool g_block_residual_mode;
bool g_block_sign_mode;
LONG CALLBACK nested_mmx_watch(EXCEPTION_POINTERS* info){
 auto*context=info->ContextRecord;
 if(info->ExceptionRecord->ExceptionCode!=EXCEPTION_BREAKPOINT||info->ExceptionRecord->ExceptionAddress!=g_nested_mmx_entry)return EXCEPTION_CONTINUE_SEARCH;
 if(g_block_sign_mode){
  const auto bitreader=*reinterpret_cast<const std::uint32_t*>(context->Ebp+8u);
  const auto width_pointer=*reinterpret_cast<const std::uint32_t*>(context->Ebp+0x40u);
  std::fprintf(stderr,"native-block-sign result=%08X selector=%08X width=%08X reader=%08X bits=%08X available=%08X cursor=%08X\n",context->Eax,*reinterpret_cast<const std::uint32_t*>(context->Ebp+0x1Cu),*reinterpret_cast<const std::uint32_t*>(width_pointer),bitreader,*reinterpret_cast<const std::uint32_t*>(bitreader),*reinterpret_cast<const std::uint32_t*>(bitreader+4u),*reinterpret_cast<const std::uint32_t*>(bitreader+8u));
 }else if(g_block_residual_mode){
  std::fprintf(stderr,"native-block-residual address=%08X old=%08X value=%08X ebp=%08X index=%08X run=%08X bit=%08X scale=%08X round=%08X\n",context->Eax,*reinterpret_cast<const std::uint32_t*>(context->Eax),context->Esi,context->Ebp,*reinterpret_cast<const std::uint32_t*>(context->Ebp+0x34u),*reinterpret_cast<const std::uint32_t*>(context->Ebp+0x1Cu),*reinterpret_cast<const std::uint32_t*>(context->Ebp+0x30u),*reinterpret_cast<const std::uint32_t*>(context->Ebp+0x14u),*reinterpret_cast<const std::uint32_t*>(context->Ebp+0x24u));
 }else if(g_block_final_mode){
  std::fprintf(stderr,"native-block-final eax=%08X ebx=%08X ecx=%08X edx=%08X esi=%08X edi=%08X ebp=%08X round=%08X\n",context->Eax,context->Ebx,context->Ecx,context->Edx,context->Esi,context->Edi,context->Ebp,*reinterpret_cast<const std::uint32_t*>(context->Ebp+0x24u));
  for(unsigned index=1;index<8u;++index)std::fprintf(stderr,"native-block-final index=%u first=%08X second=%08X\n",index,*reinterpret_cast<const std::uint32_t*>(context->Ebx+context->Edi+index*4u),*reinterpret_cast<const std::uint32_t*>(context->Eax+context->Edi+index*4u));
 }else{
  const auto*stack=reinterpret_cast<const std::uint32_t*>(context->Esp);const auto*coefficients=reinterpret_cast<const std::int32_t*>(stack[3]);
  std::fprintf(stderr,"native-nested-mmx output=%08X stride=%08X coefficients=%08X\n",stack[1],stack[2],stack[3]);
  for(unsigned index=0;index<64u;++index)std::fprintf(stderr,"native-coeff[%02u]=%d\n",index,coefficients[index]);
 }
 DWORD old_protect;VirtualProtect(g_nested_mmx_entry,1u,PAGE_EXECUTE_READWRITE,&old_protect);*g_nested_mmx_entry=g_nested_mmx_original;FlushInstructionCache(GetCurrentProcess(),g_nested_mmx_entry,1u);context->Eip=reinterpret_cast<DWORD>(g_nested_mmx_entry);return EXCEPTION_CONTINUE_EXECUTION;
}
}
int main(int argc,char**argv){
 SetUnhandledExceptionFilter(crash_filter);if(argc!=4){std::fprintf(stderr,"usage: %s default.xbe xmv-before.bin xmv-after.bin\n",argv[0]);return 2;}
 void*guest=VirtualAlloc(reinterpret_cast<void*>(kGuestBase),kGuestSize,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);
 if(guest!=reinterpret_cast<void*>(kGuestBase)){std::fprintf(stderr,"failed to map guest heap at %p (got %p, error %lu)\n",reinterpret_cast<void*>(kGuestBase),guest,GetLastError());return 2;}
 const auto xbe=read_file(argv[1]),before=read_file(argv[2]),after=read_file(argv[3]);
 if(xbe.size()<kXmvRawAddress+kXmvRawSize||before.size()<kSmallSnapshotSize+12u||after.size()!=before.size()){std::fprintf(stderr,"invalid retail XBE or XMV snapshot size\n");return 2;}
 const std::size_t snapshot_size=before.size()>=kSnapshotSize+12u?kSnapshotSize:kSmallSnapshotSize;
 const std::size_t header_size=before.size()-snapshot_size;
 if((header_size&3u)!=0u){std::fprintf(stderr,"unaligned header size\n");return 2;}
 std::vector<std::uint32_t>header(header_size/4u);std::memcpy(header.data(),before.data(),header_size);
 if(header.size()<3u||(header_size!=(3u+header[2])*4u&&!(header[0]==0x002576F8u&&header[2]==25u&&header.size()==34u))||!((header[0]==0x00258E0Du&&header[2]==3u)||(header[0]==0x002573C8u&&header[2]==15u)||(header[0]==0x00257825u&&header[2]==2u)||(header[0]==0x002576F8u&&header[2]==25u))){std::fprintf(stderr,"unexpected routine header %08X argc=%u\n",header[0],header[2]);return 2;}
 const std::size_t guest_snapshot_size=snapshot_size-(kGuestBase-kSnapshotBase);
 std::memcpy(guest,before.data()+header_size+(kGuestBase-kSnapshotBase),guest_snapshot_size);
 auto*xmv=static_cast<std::uint8_t*>(VirtualAlloc(nullptr,kXmvVirtualSize,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE));if(!xmv){std::fprintf(stderr,"failed to allocate relocated XMV\n");return 2;}
 std::memcpy(xmv,xbe.data()+kXmvRawAddress,kXmvRawSize);std::memcpy(xmv+kXmvRawSize,before.data()+header_size+(kXmvVirtualAddress+kXmvRawSize-kSnapshotBase),kXmvVirtualSize-kXmvRawSize);
 unsigned relocations=0;for(const std::uint32_t off:kXmvRelocationOffsets){std::uint32_t absolute;std::memcpy(&absolute,xmv+off,4u);if(absolute<kXmvVirtualAddress||absolute>=kXmvVirtualAddress+kXmvVirtualSize){std::fprintf(stderr,"invalid relocation %08X=%08X\n",off,absolute);return 2;}const auto relocated=static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(xmv))+(absolute-kXmvVirtualAddress);std::memcpy(xmv+off,&relocated,4u);++relocations;}
 const auto rv=[xmv](std::uint32_t v){return v>=kXmvVirtualAddress&&v<kXmvVirtualAddress+kXmvVirtualSize?static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(xmv))+(v-kXmvVirtualAddress):v;};for(std::size_t i=3;i<header.size();++i)header[i]=rv(header[i]);FlushInstructionCache(GetCurrentProcess(),xmv,kXmvVirtualSize);
 if(std::getenv("XMV_TRACE_NESTED_MMX")){
  g_nested_mmx_entry=xmv+(0x00258E0Du-kXmvVirtualAddress);g_nested_mmx_original=*g_nested_mmx_entry;DWORD old_protect;VirtualProtect(g_nested_mmx_entry,1u,PAGE_EXECUTE_READWRITE,&old_protect);*g_nested_mmx_entry=0xCCu;FlushInstructionCache(GetCurrentProcess(),g_nested_mmx_entry,1u);AddVectoredExceptionHandler(1u,nested_mmx_watch);
 } if(std::getenv("XMV_TRACE_BLOCK_FINAL")){
  g_block_final_mode=true;g_nested_mmx_entry=xmv+(0x0025769Du-kXmvVirtualAddress);g_nested_mmx_original=*g_nested_mmx_entry;DWORD old_protect;VirtualProtect(g_nested_mmx_entry,1u,PAGE_EXECUTE_READWRITE,&old_protect);*g_nested_mmx_entry=0xCCu;FlushInstructionCache(GetCurrentProcess(),g_nested_mmx_entry,1u);AddVectoredExceptionHandler(1u,nested_mmx_watch);
 } if(std::getenv("XMV_TRACE_BLOCK_RESIDUAL")){
  g_block_residual_mode=true;g_nested_mmx_entry=xmv+(0x00257647u-kXmvVirtualAddress);g_nested_mmx_original=*g_nested_mmx_entry;DWORD old_protect;VirtualProtect(g_nested_mmx_entry,1u,PAGE_EXECUTE_READWRITE,&old_protect);*g_nested_mmx_entry=0xCCu;FlushInstructionCache(GetCurrentProcess(),g_nested_mmx_entry,1u);AddVectoredExceptionHandler(1u,nested_mmx_watch);
 } if(std::getenv("XMV_TRACE_BLOCK_SIGN")){
  g_block_sign_mode=true;g_nested_mmx_entry=xmv+(0x00257617u-kXmvVirtualAddress);g_nested_mmx_original=*g_nested_mmx_entry;DWORD old_protect;VirtualProtect(g_nested_mmx_entry,1u,PAGE_EXECUTE_READWRITE,&old_protect);*g_nested_mmx_entry=0xCCu;FlushInstructionCache(GetCurrentProcess(),g_nested_mmx_entry,1u);AddVectoredExceptionHandler(1u,nested_mmx_watch);
 }
 std::fprintf(stderr,"relocated_xmv=%p routine=%p argc=%u\n",xmv,xmv+(header[0]-kXmvVirtualAddress),header[2]);
 if(header[2]==25u){
  void* routine_address=xmv+(header[0]-kXmvVirtualAddress);
  std::uint32_t* call_args=header.data()+3;
  std::uint32_t reg_eax=header[28],reg_ecx=header[29],reg_edx=header[30];
  std::uint32_t reg_ebx=header[31],reg_esi=header[32],reg_edi=header[33];
  __asm {
   push ebx
   push esi
   push edi
   mov esi, call_args
   add esi, 96
   mov ecx, 25
  push_dispatch_args:
   push dword ptr [esi]
   sub esi, 4
   dec ecx
   jnz push_dispatch_args
   mov eax, reg_eax
   mov ecx, reg_ecx
   mov edx, reg_edx
   mov ebx, reg_ebx
   mov edi, reg_edi
   mov esi, reg_esi
   call routine_address
   pop edi
   pop esi
   pop ebx
  }
 }
 else if(header[2]==2u){using F=void(__stdcall*)(std::uint32_t,std::uint32_t);reinterpret_cast<F>(xmv+(header[0]-kXmvVirtualAddress))(header[3],header[4]);}
 else if(header[2]==3u){using F=void(__stdcall*)(std::uint32_t,std::uint32_t,std::uint32_t);reinterpret_cast<F>(xmv+(header[0]-kXmvVirtualAddress))(header[3],header[4],header[5]);}
 else{using F=void(__stdcall*)(std::uint32_t,std::uint32_t,std::uint32_t,std::uint32_t,std::uint32_t,std::uint32_t,std::uint32_t,std::uint32_t,std::uint32_t,std::uint32_t,std::uint32_t,std::uint32_t,std::uint32_t,std::uint32_t,std::uint32_t);reinterpret_cast<F>(xmv+(header[0]-kXmvVirtualAddress))(header[3],header[4],header[5],header[6],header[7],header[8],header[9],header[10],header[11],header[12],header[13],header[14],header[15],header[16],header[17]);}
 const auto*actual=reinterpret_cast<const std::uint8_t*>(kGuestBase);const auto*expected=after.data()+header_size+(kGuestBase-kSnapshotBase);const auto*initial=before.data()+header_size+(kGuestBase-kSnapshotBase);
 std::uint32_t differences=0,first=0xFFFFFFFFu,last=0u;for(std::uint32_t off=kCompareOffset;off<guest_snapshot_size;++off)if(actual[off]!=expected[off]){if(differences<128u)std::printf("diff %08X initial=%02X retail=%02X recomp=%02X\n",static_cast<unsigned>(kGuestBase+off),initial[off],actual[off],expected[off]);if(first==0xFFFFFFFFu)first=static_cast<std::uint32_t>(kGuestBase+off);last=static_cast<std::uint32_t>(kGuestBase+off);++differences;}
 std::printf("routine=%08X frame=%u argc=%u relocations=%u differing_guest_bytes=%u first=%08X last=%08X\n",header[0],header[1],header[2],relocations,differences,first,last);return differences==0u?0:1;
}
