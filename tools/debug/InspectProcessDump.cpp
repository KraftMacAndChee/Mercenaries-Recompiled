#include <windows.h>
#include <dbghelp.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>

static const unsigned char *g_dump_base;
static const MINIDUMP_MEMORY64_LIST *g_dump_memory;
static const MINIDUMP_MEMORY_LIST *g_dump_memory_list;

static const unsigned char *find_memory(const unsigned char *base,
                                        const MINIDUMP_MEMORY64_LIST *list,
                                        std::uint64_t address,
                                        std::uint64_t size)
{
    if (list != nullptr) {
        std::uint64_t rva = list->BaseRva;
        for (ULONG64 i = 0; i < list->NumberOfMemoryRanges; ++i) {
            const MINIDUMP_MEMORY_DESCRIPTOR64 &range = list->MemoryRanges[i];
            if (address >= range.StartOfMemoryRange &&
                size <= range.DataSize &&
                address - range.StartOfMemoryRange <= range.DataSize - size)
                return base + rva + (address - range.StartOfMemoryRange);
            rva += range.DataSize;
        }
    }
    if (g_dump_memory_list != nullptr) {
        for (ULONG32 i = 0; i < g_dump_memory_list->NumberOfMemoryRanges; ++i) {
            const MINIDUMP_MEMORY_DESCRIPTOR &range =
                g_dump_memory_list->MemoryRanges[i];
            if (address >= range.StartOfMemoryRange &&
                size <= range.Memory.DataSize &&
                address - range.StartOfMemoryRange <=
                    range.Memory.DataSize - size)
                return base + range.Memory.Rva +
                       (address - range.StartOfMemoryRange);
        }
    }
    return nullptr;
}

static BOOL CALLBACK read_dump_memory(HANDLE, DWORD64 address, PVOID buffer,
                                      DWORD size, LPDWORD bytes_read)
{
    const unsigned char *source =
        find_memory(g_dump_base, g_dump_memory, address, size);
    if (source == nullptr) {
        *bytes_read = 0;
        return FALSE;
    }
    std::memcpy(buffer, source, size);
    *bytes_read = size;
    return TRUE;
}

int wmain(int argc, wchar_t **argv)
{
    if (argc != 2)
        return 2;
    HANDLE file = CreateFileW(argv[1], GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return 3;
    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0,
                                        nullptr);
    if (mapping == nullptr)
        return 4;
    const unsigned char *base = static_cast<const unsigned char *>(
        MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
    if (base == nullptr)
        return 5;

    void *stream = nullptr;
    ULONG stream_size = 0;
    MINIDUMP_DIRECTORY *directory = nullptr;
    MINIDUMP_MEMORY64_LIST *memory = nullptr;
    std::uint64_t image_base = 0;
    std::uint64_t image_end = 0;
    if (MiniDumpReadDumpStream(const_cast<unsigned char *>(base),
                               Memory64ListStream, &directory, &stream,
                               &stream_size)) {
        memory = static_cast<MINIDUMP_MEMORY64_LIST *>(stream);
    } else if (MiniDumpReadDumpStream(const_cast<unsigned char *>(base),
                                      MemoryListStream, &directory, &stream,
                                      &stream_size)) {
        g_dump_memory_list = static_cast<MINIDUMP_MEMORY_LIST *>(stream);
    } else {
        return 6;
    }
    g_dump_base = base;
    g_dump_memory = memory;

    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitializeW(GetCurrentProcess(), nullptr, FALSE);

    if (MiniDumpReadDumpStream(const_cast<unsigned char *>(base),
                               ModuleListStream, &directory, &stream,
                               &stream_size)) {
        const auto *modules = static_cast<MINIDUMP_MODULE_LIST *>(stream);
        for (ULONG32 i = 0; i < modules->NumberOfModules; ++i) {
            const MINIDUMP_MODULE &module = modules->Modules[i];
            if (i == 0) {
                image_base = module.BaseOfImage;
                image_end = module.BaseOfImage + module.SizeOfImage;
            }
            const auto *name = reinterpret_cast<const MINIDUMP_STRING *>(
                base + module.ModuleNameRva);
            ::wprintf(L"MODULE base=%016llX size=%08X name=%.*s\n",
                      module.BaseOfImage, module.SizeOfImage,
                      static_cast<int>(name->Length / sizeof(wchar_t)),
                      name->Buffer);
            wchar_t path[MAX_PATH];
            const unsigned int path_chars =
                name->Length / static_cast<unsigned int>(sizeof(wchar_t));
            const unsigned int copy_chars =
                path_chars < MAX_PATH - 1 ? path_chars : MAX_PATH - 1;
            std::wmemcpy(path, name->Buffer, copy_chars);
            path[copy_chars] = L'\0';
            SymLoadModuleExW(GetCurrentProcess(), nullptr, path, nullptr,
                             module.BaseOfImage, module.SizeOfImage, nullptr, 0);
        }
    }

    if (!MiniDumpReadDumpStream(const_cast<unsigned char *>(base),
                                ThreadListStream, &directory, &stream,
                                &stream_size))
        return 7;
    const auto *threads = static_cast<MINIDUMP_THREAD_LIST *>(stream);
    MINIDUMP_THREAD_INFO_LIST *thread_info = nullptr;
    if (MiniDumpReadDumpStream(const_cast<unsigned char *>(base),
                               ThreadInfoListStream, &directory, &stream,
                               &stream_size))
        thread_info = static_cast<MINIDUMP_THREAD_INFO_LIST *>(stream);
    DWORD busiest_thread = 0;
    ULONG64 busiest_user_time = 0;
    if (thread_info != nullptr) {
        const unsigned char *entry = reinterpret_cast<const unsigned char *>(
            thread_info) + thread_info->SizeOfHeader;
        for (ULONG32 item = 0; item < thread_info->NumberOfEntries; ++item) {
            const auto *candidate =
                reinterpret_cast<const MINIDUMP_THREAD_INFO *>(entry);
            if (candidate->UserTime > busiest_user_time) {
                busiest_user_time = candidate->UserTime;
                busiest_thread = candidate->ThreadId;
            }
            entry += thread_info->SizeOfEntry;
        }
    }
    for (ULONG32 i = 0; i < threads->NumberOfThreads; ++i) {
        const MINIDUMP_THREAD &thread = threads->Threads[i];
        const auto *context = reinterpret_cast<const CONTEXT *>(
            base + thread.ThreadContext.Rva);
        const MINIDUMP_THREAD_INFO *info = nullptr;
        if (thread_info != nullptr) {
            const unsigned char *entry = reinterpret_cast<const unsigned char *>(
                thread_info) + thread_info->SizeOfHeader;
            for (ULONG32 item = 0; item < thread_info->NumberOfEntries; ++item) {
                const auto *candidate =
                    reinterpret_cast<const MINIDUMP_THREAD_INFO *>(entry);
                if (candidate->ThreadId == thread.ThreadId) {
                    info = candidate;
                    break;
                }
                entry += thread_info->SizeOfEntry;
            }
        }
        std::printf("THREAD id=%08X rip=%016llX rsp=%016llX rbp=%016llX "
                    "rax=%016llX rbx=%016llX rcx=%016llX rdx=%016llX "
                    "rsi=%016llX rdi=%016llX r8=%016llX r9=%016llX "
                    "kernel=%llu user=%llu start=%016llX\n",
                    thread.ThreadId, context->Rip, context->Rsp, context->Rbp,
                    context->Rax, context->Rbx, context->Rcx, context->Rdx,
                    context->Rsi, context->Rdi, context->R8, context->R9,
                    info != nullptr ? info->KernelTime : 0,
                    info != nullptr ? info->UserTime : 0,
                    info != nullptr ? info->StartAddress : 0);
        std::printf("THREAD-STACK id=%08X start=%016llX size=%08X rva=%08X\n",
                    thread.ThreadId, thread.Stack.StartOfMemoryRange,
                    thread.Stack.Memory.DataSize, thread.Stack.Memory.Rva);
        const auto *stack = reinterpret_cast<const std::uint64_t *>(
            find_memory(base, memory, context->Rsp, 64u * sizeof(std::uint64_t)));
        if (stack != nullptr) {
            for (unsigned int word = 0; word < 64u; ++word)
                std::printf("STACK id=%08X offset=%04X value=%016llX\n",
                            thread.ThreadId, word * 8u, stack[word]);
        }
        constexpr std::uint64_t scan_size = 0x10000u;
        const auto *scan = reinterpret_cast<const std::uint64_t *>(
            find_memory(base, memory, context->Rsp, scan_size));
        if (scan != nullptr && image_base != 0) {
            for (unsigned int word = 0; word < scan_size / 8u; ++word) {
                if (scan[word] >= image_base && scan[word] < image_end)
                    std::printf("IMAGE-STACK id=%08X offset=%05X "
                                "value=%016llX rva=%08llX\n",
                                thread.ThreadId, word * 8u, scan[word],
                                scan[word] - image_base);
            }
        }
        if (thread.Stack.Memory.DataSize != 0 && image_base != 0) {
            const auto *full_stack = reinterpret_cast<const std::uint64_t *>(
                find_memory(base, memory, thread.Stack.StartOfMemoryRange,
                            thread.Stack.Memory.DataSize));
            const unsigned int words = thread.Stack.Memory.DataSize / 8u;
            for (unsigned int word = 0; full_stack != nullptr && word < words;
                 ++word) {
                if (full_stack[word] >= image_base && full_stack[word] < image_end)
                    std::printf("IMAGE-FULL-STACK id=%08X offset=%05X "
                                "value=%016llX rva=%08llX\n",
                                thread.ThreadId, word * 8u, full_stack[word],
                                full_stack[word] - image_base);
            }
        }
        if (thread.ThreadId == busiest_thread) {
            CONTEXT unwind_context = *context;
            STACKFRAME64 frame = {};
            frame.AddrPC.Offset = unwind_context.Rip;
            frame.AddrPC.Mode = AddrModeFlat;
            frame.AddrFrame.Offset = unwind_context.Rbp;
            frame.AddrFrame.Mode = AddrModeFlat;
            frame.AddrStack.Offset = unwind_context.Rsp;
            frame.AddrStack.Mode = AddrModeFlat;
            for (unsigned int depth = 0; depth < 96u; ++depth) {
                if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(),
                                 reinterpret_cast<HANDLE>(
                                     static_cast<uintptr_t>(thread.ThreadId)),
                                 &frame, &unwind_context, read_dump_memory,
                                 SymFunctionTableAccess64, SymGetModuleBase64,
                                 nullptr))
                    break;
                if (frame.AddrPC.Offset == 0)
                    break;
                char symbol_storage[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {};
                auto *symbol = reinterpret_cast<SYMBOL_INFO *>(symbol_storage);
                symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
                symbol->MaxNameLen = MAX_SYM_NAME;
                DWORD64 displacement = 0;
                if (SymFromAddr(GetCurrentProcess(), frame.AddrPC.Offset,
                                &displacement, symbol))
                    std::printf("UNWIND id=%08X depth=%u pc=%016llX "
                                "symbol=%s+%llX\n", thread.ThreadId, depth,
                                frame.AddrPC.Offset, symbol->Name, displacement);
                else
                    std::printf("UNWIND id=%08X depth=%u pc=%016llX\n",
                                thread.ThreadId, depth, frame.AddrPC.Offset);
            }
        }
    }

    SymCleanup(GetCurrentProcess());
    UnmapViewOfFile(base);
    CloseHandle(mapping);
    CloseHandle(file);
    return 0;
}
