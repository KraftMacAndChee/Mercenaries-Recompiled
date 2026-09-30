/* Bounded preview diagnostics. No guest-function tracing or GPU readbacks.
 * Only the writer touches session files; a separate crash handle avoids its
 * queue/locks. Loss under a warning storm is counted rather than stalling play. */
#include <windows.h>
#include <bcrypt.h>
#include <psapi.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "preview_log.h"
#define RECORD_SIZE 768
#define QUEUE_SIZE 256u
#ifndef PREVIEW_LOG_FILE_LIMIT
#define PREVIEW_LOG_FILE_LIMIT (4u * 1024u * 1024u)
#endif
#define FILE_LIMIT PREVIEW_LOG_FILE_LIMIT
#define RETAIN_FILES 16
static SRWLOCK queue_lock = SRWLOCK_INIT;
static char queue[QUEUE_SIZE][RECORD_SIZE];
static unsigned head, tail, count;
static volatile LONG enabled, stopping, lost;
static HANDLE wake, worker, crash_file = INVALID_HANDLE_VALUE;
static wchar_t log_dir[MAX_PATH], stem[64];
static ULONGLONG start_ms, rate_second;
static unsigned rate_count, part;
static char crash_context[16384];
static volatile LONG crash_context_length;
static char header[512], session_utc[40];
static struct {const char *format; ULONGLONG last;} throttle[128];

int xbox_preview_log_enabled(void) { return enabled != 0; }

/* Accept only this writer's fixed-format filenames; never directories/links. */
static int owned_name(const wchar_t *name) {
    unsigned i;
    if (wcslen(name) != 42 || wcsncmp(name,L"preview-",8) ||
        name[24]!=L'-' || name[33]!=L'-' || wcscmp(name+38,L".log")) return 0;
    for (i=8;i<38;i++) {
        if(i==24 || i==33) continue;
        if(!((name[i]>=L'0' && name[i]<=L'9') ||
             (i<34 && name[i]>=L'A' && name[i]<=L'F'))) return 0;
    }
    return 1;
}
static void prune_logs(void) {
    WIN32_FIND_DATAW data; wchar_t pattern[MAX_PATH], oldest[MAX_PATH], path[MAX_PATH];
    unsigned files;
    /* Each pass removes one oldest closed log. Active handles deny deletion. */
    do {
        files=0; oldest[0]=0;
        swprintf(pattern,MAX_PATH,L"%ls\\preview-*.log",log_dir);
        HANDLE find=FindFirstFileW(pattern,&data);
        if(find==INVALID_HANDLE_VALUE) return;
        do {
            if((data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)) || !owned_name(data.cFileName)) continue;
            ++files;
            swprintf(path,MAX_PATH,L"%ls\\%ls",log_dir,data.cFileName);
            HANDLE available=CreateFileW(path,DELETE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
            if(available==INVALID_HANDLE_VALUE) continue;
            CloseHandle(available);
            if(!oldest[0] || wcscmp(data.cFileName,oldest)<0) wcscpy(oldest,data.cFileName);
        } while(FindNextFileW(find,&data));
        FindClose(find);
        if(files<RETAIN_FILES || !oldest[0]) return;
        swprintf(path,MAX_PATH,L"%ls\\%ls",log_dir,oldest);
        if(!DeleteFileW(path)) return;
    } while(files>=RETAIN_FILES);
}
static HANDLE open_part(unsigned index) {
    wchar_t path[MAX_PATH];
    prune_logs();
    if(swprintf(path,MAX_PATH,L"%ls\\%ls-%04u.log",log_dir,stem,index)<0) return INVALID_HANDLE_VALUE;
    return CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
}
static int write_bytes(HANDLE file,const char *data,DWORD size) {
    DWORD wrote=0;
    return WriteFile(file,data,size,&wrote,NULL) && wrote==size;
}
static void executable_hash(char result[65]) {
    wchar_t path[MAX_PATH]; BYTE buffer[65536],digest[32]; DWORD got;
    BCRYPT_ALG_HANDLE algorithm=NULL; BCRYPT_HASH_HANDLE hash=NULL;
    HANDLE file=INVALID_HANDLE_VALUE; int ok=0;
    strcpy(result,"unavailable");
    if(!GetModuleFileNameW(NULL,path,MAX_PATH)) return;
    file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,NULL);
    if(file==INVALID_HANDLE_VALUE) return;
    if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,NULL,0)<0) goto done;
    if(BCryptCreateHash(algorithm,&hash,NULL,0,NULL,0,0)<0) goto done;
    for(;;) {
        if(!ReadFile(file,buffer,sizeof(buffer),&got,NULL)) goto done;
        if(!got) break;
        if(BCryptHashData(hash,buffer,got,0)<0) goto done;
    }
    ok=BCryptFinishHash(hash,digest,sizeof(digest),0)>=0;
    if(ok) {for(unsigned i=0;i<32;i++) sprintf(result+i*2,"%02X",digest[i]); result[64]=0;}
done:
    if(hash) BCryptDestroyHash(hash);
    if(algorithm) BCryptCloseAlgorithmProvider(algorithm,0);
    CloseHandle(file);
}
static DWORD WINAPI writer_main(void *unused) {
    char batch[RECORD_SIZE*32], hash[65]; DWORD bytes=0; ULONGLONG last_health=start_ms;
    HANDLE file; (void)unused;
    executable_hash(hash);
    snprintf(header,sizeof(header),"preview-log-v1 session=%ls utc=%s pid=%lu exe_sha256=%s\n"
             "Times below are monotonic milliseconds since session start. F8 marks a glitch.\n",stem,session_utc,GetCurrentProcessId(),hash);
    file=open_part(part++);
    if(file==INVALID_HANDLE_VALUE || !write_bytes(file,header,(DWORD)strlen(header))) goto done;
    bytes=(DWORD)strlen(header);
    for(;;) {
        size_t used=0; unsigned pending;
        WaitForSingleObject(wake,1000);
        do {
            AcquireSRWLockExclusive(&queue_lock);
            while(count && used+RECORD_SIZE<=sizeof(batch)) {
                size_t len=strlen(queue[tail]); memcpy(batch+used,queue[tail],len);used+=len;
                tail=(tail+1)%QUEUE_SIZE;--count;
            }
            pending=count; ReleaseSRWLockExclusive(&queue_lock);
            if(used) {
                if(bytes+used>FILE_LIMIT) {
                    CloseHandle(file);file=INVALID_HANDLE_VALUE;
                    if(part>=9999) goto done;
                    file=open_part(part++);
                    if(file==INVALID_HANDLE_VALUE || !write_bytes(file,header,(DWORD)strlen(header))) goto done;
                    bytes=(DWORD)strlen(header);
                }
                if(!write_bytes(file,batch,(DWORD)used)) goto done;
                bytes+=(DWORD)used;used=0;
            }
        } while(pending);
        if(GetTickCount64()-last_health>=5000) {
            PROCESS_MEMORY_COUNTERS_EX memory={0}; memory.cb=sizeof(memory);
            GetProcessMemoryInfo(GetCurrentProcess(),(PROCESS_MEMORY_COUNTERS*)&memory,sizeof(memory));
            xbox_preview_log_event("host","private_mb=%llu resident_mb=%llu dropped_or_rate_limited=%ld",
                (unsigned long long)(memory.PrivateUsage/(1024*1024)),
                (unsigned long long)(memory.WorkingSetSize/(1024*1024)),InterlockedExchange(&lost,0));
            last_health=GetTickCount64();
        }
        if(stopping) {
            AcquireSRWLockExclusive(&queue_lock);pending=count;ReleaseSRWLockExclusive(&queue_lock);
            if(!pending) break;
        }
    }
done:
    InterlockedExchange(&enabled,0);
    if(file!=INVALID_HANDLE_VALUE) CloseHandle(file);
    return 0;
}
int xbox_preview_log_init(const wchar_t *directory) {
    FILETIME ft; ULARGE_INTEGER time; SYSTEM_INFO system; MEMORYSTATUSEX memory={sizeof(memory)};
    if(enabled || worker || getenv("MERCENARIES_DISABLE_PREVIEW_LOGS")) return enabled!=0;
    if(!directory || wcslen(directory)>MAX_PATH-80) return 0;
    wcscpy(log_dir,directory);
    if(!CreateDirectoryW(log_dir,NULL) && GetLastError()!=ERROR_ALREADY_EXISTS) return 0;
    if(GetFileAttributesW(log_dir)&FILE_ATTRIBUTE_REPARSE_POINT) return 0;
    head=tail=count=part=rate_count=0; rate_second=0;
    InterlockedExchange(&crash_context_length,0);
    memset(throttle,0,sizeof(throttle));InterlockedExchange(&stopping,0);InterlockedExchange(&lost,0);
    GetSystemTimeAsFileTime(&ft);time.LowPart=ft.dwLowDateTime;time.HighPart=ft.dwHighDateTime;
    swprintf(stem,64,L"preview-%016llX-%08lX",time.QuadPart,GetCurrentProcessId());
    {
        SYSTEMTIME utc;FileTimeToSystemTime(&ft,&utc);
        snprintf(session_utc,sizeof(session_utc),"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
            utc.wYear,utc.wMonth,utc.wDay,utc.wHour,utc.wMinute,utc.wSecond,utc.wMilliseconds);
    }
    start_ms=GetTickCount64();
    crash_file=open_part(9999);
    wake=CreateEventW(NULL,FALSE,FALSE,NULL);
    if(!wake) {if(crash_file!=INVALID_HANDLE_VALUE)CloseHandle(crash_file);crash_file=INVALID_HANDLE_VALUE;return 0;}
    InterlockedExchange(&enabled,1);
    worker=CreateThread(NULL,0,writer_main,NULL,0,NULL);
    if(!worker){InterlockedExchange(&enabled,0);CloseHandle(wake);wake=NULL;if(crash_file!=INVALID_HANDLE_VALUE)CloseHandle(crash_file);crash_file=INVALID_HANDLE_VALUE;return 0;}
    SetThreadPriority(worker,THREAD_PRIORITY_BELOW_NORMAL);
    GetNativeSystemInfo(&system);GlobalMemoryStatusEx(&memory);
    xbox_preview_log_event("session","utc_filetime=%llu logical_cpus=%lu ram_mb=%llu limit_mb=4 retained_files=16",
        time.QuadPart,system.dwNumberOfProcessors,memory.ullTotalPhys/(1024*1024));
    return 1;
}
static void enqueue_event(const char *category,const char *format,va_list args,int coalesce) {
    ULONGLONG now; unsigned bucket; char *record; int offset,priority; size_t len;
    if(!enabled) return;
    if(!TryAcquireSRWLockExclusive(&queue_lock)){InterlockedIncrement(&lost);return;}
    priority=!strncmp(category,"USER-MARKER",11) || !strcmp(category,"renderer-recovery") || !strcmp(category,"session");
    now=GetTickCount64();bucket=((uintptr_t)format>>4)&127;
    if((!priority && coalesce && throttle[bucket].format==format && now-throttle[bucket].last<1000) ||
        count >= (priority ? QUEUE_SIZE : QUEUE_SIZE-8)) {InterlockedIncrement(&lost);ReleaseSRWLockExclusive(&queue_lock);return;}
    if(now/1000!=rate_second){rate_second=now/1000;rate_count=0;}
    if(!priority && rate_count++>=64){InterlockedIncrement(&lost);ReleaseSRWLockExclusive(&queue_lock);return;}
    if(coalesce){throttle[bucket].format=format;throttle[bucket].last=now;}
    record=queue[head];offset=snprintf(record,RECORD_SIZE,"[%llu] [%s] ",now-start_ms,category);
    if(offset<0 || offset>RECORD_SIZE-3) offset=0;
    vsnprintf(record+offset,RECORD_SIZE-offset-2,format,args);
    record[RECORD_SIZE-3]=0;
    for(char *p=record;*p;p++) if(*p=='\n'||*p=='\r') *p=' ';
    len=strlen(record);record[len++]='\n';record[len]=0;
    head=(head+1)%QUEUE_SIZE;++count;
    ReleaseSRWLockExclusive(&queue_lock);SetEvent(wake);
}
void xbox_preview_log_eventv(const char *category,const char *format,va_list args) {
    enqueue_event(category,format,args,1);
}
void xbox_preview_log_sample(const char *category,const char *format,...) {
    va_list args;va_start(args,format);enqueue_event(category,format,args,0);va_end(args);
}
void xbox_preview_log_event(const char *category,const char *format,...) {
    va_list args;va_start(args,format);xbox_preview_log_eventv(category,format,args);va_end(args);
}
void xbox_preview_log_set_crash_context(const char *text) {
    size_t length=0;
    /* Publish only a complete snapshot. No queue lock, formatting or disk IO. */
    InterlockedExchange(&crash_context_length,0);
    if(!enabled || !text) return;
    while(length<sizeof(crash_context)-1 && text[length]) ++length;
    memcpy(crash_context,text,length);crash_context[length]=0;
    InterlockedExchange(&crash_context_length,(LONG)length);
}
void xbox_preview_log_crash(uint32_t code,uintptr_t pc,uint32_t guest_pc,
                            uint32_t eax,uint32_t ecx,uint32_t edx,uint32_t esp) {
    char record[512]; int n;
    if(crash_file==INVALID_HANDLE_VALUE) return;
    n=snprintf(record,sizeof(record),"session=%ls elapsed_ms=%llu exception=%08X native_rva=%llX guest_pc=%08X eax=%08X ecx=%08X edx=%08X esp=%08X\n",
        stem,GetTickCount64()-start_ms,code,(unsigned long long)(pc-(uintptr_t)GetModuleHandleW(NULL)),guest_pc,eax,ecx,edx,esp);
    if(n>0 && n<(int)sizeof(record)) {
        write_bytes(crash_file,record,(DWORD)n);
        LONG context_length=InterlockedCompareExchange(&crash_context_length,0,0);
        if(context_length>0 && context_length<(LONG)sizeof(crash_context))
            write_bytes(crash_file,crash_context,(DWORD)context_length);
        FlushFileBuffers(crash_file);
    }
}
void xbox_preview_log_shutdown(void) {
    if(!worker) return;
    xbox_preview_log_event("session","clean shutdown");
    InterlockedExchange(&enabled,0);InterlockedExchange(&stopping,1);SetEvent(wake);
    /* No unbounded wait if storage is unavailable. Process teardown closes
     * remaining handles; never destroy a queue still used by a blocked writer. */
    if(WaitForSingleObject(worker,2000)==WAIT_OBJECT_0) {
        CloseHandle(worker);worker=NULL;CloseHandle(wake);wake=NULL;
        if(crash_file!=INVALID_HANDLE_VALUE) {CloseHandle(crash_file);crash_file=INVALID_HANDLE_VALUE;}
    }
}
