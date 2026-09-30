#include <windows.h>
#include <dbghelp.h>

#include <cstdio>
#include <cstdlib>
#include <cwchar>

int wmain(int argc, wchar_t **argv)
{
    if (argc != 3) {
        std::fwprintf(stderr, L"usage: CaptureProcessDump <pid> <path>\n");
        return 2;
    }

    const DWORD pid = static_cast<DWORD>(std::wcstoul(argv[1], nullptr, 10));
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ |
                                     PROCESS_DUP_HANDLE,
                                 FALSE, pid);
    if (process == nullptr) {
        std::fwprintf(stderr, L"OpenProcess failed: %lu\n", GetLastError());
        return 3;
    }

    HANDLE output = CreateFileW(argv[2], GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE) {
        std::fwprintf(stderr, L"CreateFile failed: %lu\n", GetLastError());
        CloseHandle(process);
        return 4;
    }

    const MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(
        MiniDumpWithFullMemory | MiniDumpWithHandleData |
        MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
    const BOOL ok = MiniDumpWriteDump(process, pid, output, type, nullptr,
                                      nullptr, nullptr);
    const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(output);
    CloseHandle(process);
    if (!ok) {
        std::fwprintf(stderr, L"MiniDumpWriteDump failed: %lu\n", error);
        return 5;
    }
    return 0;
}
