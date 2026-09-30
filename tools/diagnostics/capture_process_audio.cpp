// Capture only one verified test process through Windows process loopback.
// No microphone or system-wide loopback is opened. Windows build 20348+.
#define _WIN32_WINNT 0x0A00
#define NTDDI_VERSION 0x0A00000A
#include <windows.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <cstdio>
#include <cwchar>
#include <vector>
using Microsoft::WRL::ComPtr;
class Completion final : public IActivateAudioInterfaceCompletionHandler {
 volatile LONG refs=1;
public:
 HANDLE ready=CreateEventW(nullptr,TRUE,FALSE,nullptr);
 HRESULT result=E_PENDING;
 ComPtr<IAudioClient> client;
 ~Completion(){CloseHandle(ready);}
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out) override {
  if(!out)return E_POINTER;*out=nullptr;
  if(id==__uuidof(IUnknown)||id==__uuidof(IActivateAudioInterfaceCompletionHandler)||id==__uuidof(IAgileObject)) {*out=static_cast<IActivateAudioInterfaceCompletionHandler*>(this);AddRef();return S_OK;}
  return E_NOINTERFACE;
 }
 ULONG STDMETHODCALLTYPE AddRef() override{return InterlockedIncrement(&refs);}
 ULONG STDMETHODCALLTYPE Release() override{ULONG n=InterlockedDecrement(&refs);if(!n)delete this;return n;}
 HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) override {
  ComPtr<IUnknown> unknown;HRESULT activation=E_FAIL;
  result=operation->GetActivateResult(&activation,&unknown);
  if(SUCCEEDED(result))result=activation;
  if(SUCCEEDED(result))result=unknown.As(&client);
  SetEvent(ready);return S_OK;
 }
};
static void word(FILE* f,unsigned short v){fwrite(&v,2,1,f);}
static void dword(FILE* f,unsigned v){fwrite(&v,4,1,f);}
static void header(FILE* f,unsigned bytes){
 rewind(f);fwrite("RIFF",1,4,f);dword(f,36+bytes);fwrite("WAVEfmt ",1,8,f);dword(f,16);
 word(f,1);word(f,2);dword(f,48000);dword(f,192000);word(f,4);word(f,16);fwrite("data",1,4,f);dword(f,bytes);
}
int wmain(int argc,wchar_t** argv){
 if(argc!=5){fprintf(stderr,"Usage: capture_process_audio PID EXPECTED_EXE OUTPUT.wav SECONDS\n");return 2;}
 wchar_t* end=nullptr;DWORD pid=wcstoul(argv[1],&end,10);if(!pid||*end)return 2;
 unsigned seconds=wcstoul(argv[4],&end,10);if(*end||seconds<1||seconds>180)return 2;
 HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,pid);
 if(!process){fprintf(stderr,"OpenProcess failed %lu\n",GetLastError());return 3;}
 wchar_t actual[32768],expected[32768];DWORD n=32768;
 if(!QueryFullProcessImageNameW(process,0,actual,&n)||!GetFullPathNameW(argv[2],32768,expected,nullptr)||_wcsicmp(actual,expected)){
  fprintf(stderr,"Refusing capture: PID executable does not match the requested test executable\n");CloseHandle(process);return 3;
 }
 if(GetFileAttributesW(argv[3])!=INVALID_FILE_ATTRIBUTES){fprintf(stderr,"Output already exists\n");CloseHandle(process);return 3;}
 HRESULT hr=CoInitializeEx(nullptr,COINIT_MULTITHREADED);if(FAILED(hr))return 4;
 AUDIOCLIENT_ACTIVATION_PARAMS params={};params.ActivationType=AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
 params.ProcessLoopbackParams.TargetProcessId=pid;
 params.ProcessLoopbackParams.ProcessLoopbackMode=PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
 PROPVARIANT prop={};prop.vt=VT_BLOB;prop.blob.cbSize=sizeof(params);prop.blob.pBlobData=reinterpret_cast<BYTE*>(&params);
 auto* completion=new Completion;ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
 hr=ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,__uuidof(IAudioClient),&prop,completion,&operation);
 if(FAILED(hr)){fprintf(stderr,"ActivateAudioInterfaceAsync %08lX\n",hr);return 4;}
 if(WaitForSingleObject(completion->ready,15000)!=WAIT_OBJECT_0){fprintf(stderr,"Activation timeout\n");return 4;}
 if(FAILED(completion->result)){fprintf(stderr,"Activation result %08lX\n",completion->result);return 4;}
 ComPtr<IAudioClient> client=completion->client;completion->Release();
 WAVEFORMATEX format={WAVE_FORMAT_PCM,2,48000,192000,4,16,0};
 hr=client->Initialize(AUDCLNT_SHAREMODE_SHARED,AUDCLNT_STREAMFLAGS_LOOPBACK|AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM|AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,200000,0,&format,nullptr);
 if(FAILED(hr)){fprintf(stderr,"Initialize %08lX\n",hr);return 4;}
 ComPtr<IAudioCaptureClient> capture;hr=client->GetService(IID_PPV_ARGS(&capture));if(FAILED(hr))return 4;
 FILE* output=nullptr;_wfopen_s(&output,argv[3],L"wb");if(!output)return 5;header(output,0);
 hr=client->Start();if(FAILED(hr)){fclose(output);return 4;}
 fprintf(stderr,"Capturing only verified process %lu for up to %u seconds\n",pid,seconds);
 ULONGLONG start=GetTickCount64();unsigned bytes=0,discontinuities=0;std::vector<BYTE> silence;
 while(GetTickCount64()-start<(ULONGLONG)seconds*1000&&WaitForSingleObject(process,0)==WAIT_TIMEOUT){
  UINT32 available=0;hr=capture->GetNextPacketSize(&available);if(FAILED(hr))break;
  while(available){
   BYTE* data=nullptr;UINT32 frames=0;DWORD flags=0;hr=capture->GetBuffer(&data,&frames,&flags,nullptr,nullptr);if(FAILED(hr))break;
   unsigned size=frames*4;if(flags&AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)discontinuities++;
   if(flags&AUDCLNT_BUFFERFLAGS_SILENT){silence.assign(size,0);data=silence.data();}
   bool ok=fwrite(data,1,size,output)==size;capture->ReleaseBuffer(frames);if(!ok){hr=E_FAIL;break;}bytes+=size;
   hr=capture->GetNextPacketSize(&available);if(FAILED(hr))break;
  }
  if(FAILED(hr))break;Sleep(5);
 }
 client->Stop();header(output,bytes);fclose(output);CloseHandle(process);
 fprintf(stderr,"Captured %u bytes, %.3f seconds; discontinuities=%u result=%08lX\n",bytes,bytes/192000.0,discontinuities,hr);
 capture.Reset();client.Reset();operation.Reset();CoUninitialize();return FAILED(hr)?6:0;
}
