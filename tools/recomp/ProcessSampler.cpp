#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>
#pragma comment(lib, "dbghelp.lib")
struct Sample { std::string symbol; std::uint64_t address; unsigned count; };
int main(int argc, char **argv) {
    if (argc != 4) { std::fprintf(stderr, "usage: ProcessSampler <pid> <tid> <milliseconds>\n"); return 2; }
    const DWORD pid = std::strtoul(argv[1], nullptr, 0);
    const DWORD tid = std::strtoul(argv[2], nullptr, 0);
    const DWORD duration_ms = std::strtoul(argv[3], nullptr, 0);
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!process || !thread) { std::fprintf(stderr, "open failed: %lu\n", GetLastError()); return 3; }
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
    if (!SymInitialize(process, nullptr, TRUE)) { std::fprintf(stderr, "SymInitialize failed: %lu\n", GetLastError()); return 4; }
    std::unordered_map<std::string, Sample> samples;
    const ULONGLONG deadline = GetTickCount64() + duration_ms;
    unsigned captured = 0, missed = 0;
    while (GetTickCount64() < deadline) {
        if (SuspendThread(thread) == static_cast<DWORD>(-1)) { ++missed; Sleep(1); continue; }
        CONTEXT context = {}; context.ContextFlags = CONTEXT_FULL;
        const BOOL got_context = GetThreadContext(thread, &context);
        if (!got_context) { ResumeThread(thread); ++missed; Sleep(1); continue; }
#if defined(_M_X64)
        const DWORD64 address = context.Rip;
        STACKFRAME64 frame = {};
        frame.AddrPC.Offset = context.Rip; frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = context.Rbp; frame.AddrFrame.Mode = AddrModeFlat;
        frame.AddrStack.Offset = context.Rsp; frame.AddrStack.Mode = AddrModeFlat;
        std::vector<DWORD64> callers;
        CONTEXT walk_context = context;
        for (unsigned depth = 0; depth < 12; ++depth) {
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame,
                             &walk_context, nullptr, SymFunctionTableAccess64,
                             SymGetModuleBase64, nullptr) || frame.AddrPC.Offset == 0)
                break;
            callers.push_back(frame.AddrPC.Offset);
        }
#else
        const DWORD64 address = context.Eip;
#endif
        ResumeThread(thread);
        char storage[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {};
        auto *info = reinterpret_cast<SYMBOL_INFO *>(storage); info->SizeOfStruct = sizeof(SYMBOL_INFO); info->MaxNameLen = MAX_SYM_NAME;
        DWORD64 displacement = 0; std::string key;
        if (SymFromAddr(process, address, &displacement, info)) key = info->Name;
        else { char raw[40]; std::snprintf(raw, sizeof(raw), "0x%llX", static_cast<unsigned long long>(address)); key = raw; }
#if defined(_M_X64)
        for (DWORD64 caller : callers) {
            char caller_storage[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {};
            auto *caller_info = reinterpret_cast<SYMBOL_INFO *>(caller_storage);
            caller_info->SizeOfStruct = sizeof(SYMBOL_INFO); caller_info->MaxNameLen = MAX_SYM_NAME;
            DWORD64 caller_displacement = 0;
            key += " <- ";
            if (SymFromAddr(process, caller, &caller_displacement, caller_info)) key += caller_info->Name;
            else { char raw[40]; std::snprintf(raw, sizeof(raw), "0x%llX", static_cast<unsigned long long>(caller)); key += raw; }
        }
#endif
        auto &sample = samples[key]; sample.symbol = key; sample.address = address; ++sample.count; ++captured; Sleep(1);
    }
    std::vector<Sample> sorted; sorted.reserve(samples.size()); for (const auto &entry : samples) sorted.push_back(entry.second);
    std::sort(sorted.begin(), sorted.end(), [](const Sample &a, const Sample &b) { return a.count > b.count; });
    std::printf("captured=%u missed=%u symbols=%zu\n", captured, missed, sorted.size());
    for (std::size_t i = 0; i < sorted.size() && i < 100; ++i) { const double percent = captured ? 100.0 * sorted[i].count / captured : 0.0; std::printf("%7u %6.2f%% 0x%llX %s\n", sorted[i].count, percent, static_cast<unsigned long long>(sorted[i].address), sorted[i].symbol.c_str()); }
    SymCleanup(process); CloseHandle(thread); CloseHandle(process); return 0;
}