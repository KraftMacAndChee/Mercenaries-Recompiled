#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
constexpr std::uintptr_t kSnapshotBase = 0x00010000u;
constexpr std::size_t kSnapshotSize = 0x03FF0000u;
constexpr std::uintptr_t kGuestBase = 0x00800000u;
constexpr std::size_t kGuestSize = 0x03800000u;
constexpr std::size_t kCompareOffset = 0x000C0000u;
constexpr std::uint32_t kXmvVirtualAddress = 0x00255620u;
constexpr std::uint32_t kXmvRawAddress = 0x00246000u;
constexpr std::uint32_t kXmvRawSize = 0x00027F84u;
constexpr std::uint32_t kXmvVirtualSize = 0x00027F94u;
constexpr std::size_t kHeaderSize = 4u * sizeof(std::uint32_t);
constexpr std::uint32_t kXmvRelocationOffsets[] = {
#include "xmv_relocation_offsets.inc"
};
std::vector<std::uint8_t> read_file(const char* path) {
    FILE* file = std::fopen(path, "rb");
    if (!file) { std::fprintf(stderr, "failed to open %s\n", path); std::exit(2); }
    std::fseek(file, 0, SEEK_END); const long size = std::ftell(file); std::rewind(file);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (std::fread(bytes.data(), 1, bytes.size(), file) != bytes.size()) {
        std::fprintf(stderr, "failed to read %s\n", path); std::exit(2);
    }
    std::fclose(file); return bytes;
}
std::uint8_t* g_relocated_xmv;
bool g_guard_single_step;
std::uint8_t* g_interblock_entry;
std::uint8_t g_interblock_original_byte;
bool g_interblock_single_step;
bool g_interblock_captured;
unsigned g_interblock_trace_calls;
const char* g_interblock_dump_path;
std::uint32_t g_interblock_frame;
std::uint32_t g_interblock_target_output = 0x008DD500u;

void dump_interblock_entry(const CONTEXT* context) {
    DWORD output_page_protect;
    VirtualProtect(reinterpret_cast<void*>(0x00950000u), 0x1000u,
                   PAGE_EXECUTE_READWRITE, &output_page_protect);
    std::uint32_t header[34] = {};
    const auto* stack = reinterpret_cast<const std::uint32_t*>(context->Esp);
    header[0] = 0x002576F8u;
    header[1] = g_interblock_frame;
    header[2] = 25u;
    for (unsigned index = 0; index < 25u; ++index) header[3u + index] = stack[1u + index];
    header[28] = context->Eax;
    header[29] = context->Ecx;
    header[30] = context->Edx;
    header[31] = context->Ebx;
    header[32] = context->Esi;
    header[33] = context->Edi;
    FILE* dump = std::fopen(g_interblock_dump_path, "wb");
    if (!dump) {
        std::fprintf(stderr, "failed to create native interblock dump %s\n", g_interblock_dump_path);
        return;
    }
    std::fwrite(header, 1u, sizeof(header), dump);
    std::fwrite(reinterpret_cast<const void*>(kGuestBase), 1u, kGuestSize, dump);
    const std::uint32_t stack_page = context->Esp & ~0xFFFu;
    std::fwrite(&stack_page, 1u, sizeof(stack_page), dump);
    std::fwrite(reinterpret_cast<const void*>(stack_page), 1u, 0x1000u, dump);
    std::fclose(dump);
    std::fprintf(stderr,
                 "native-interblock-entry frame=%u output=%08X eax=%08X ecx=%08X edx=%08X ebx=%08X esi=%08X edi=%08X path=%s\n",
                 g_interblock_frame, header[12], context->Eax, context->Ecx,
                 context->Edx, context->Ebx, context->Esi, context->Edi,
                 g_interblock_dump_path);
    std::fflush(stderr);
}

LONG CALLBACK interblock_entry_watch(EXCEPTION_POINTERS* info) {
    auto* context = info->ContextRecord;
    const auto code = info->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_BREAKPOINT &&
        info->ExceptionRecord->ExceptionAddress == g_interblock_entry) {
        const auto* stack = reinterpret_cast<const std::uint32_t*>(context->Esp);
        if (g_interblock_trace_calls < 64u) {
            const auto* state_array = reinterpret_cast<const std::uint32_t*>(stack[14]);
            std::fprintf(stderr,
                         "native-interblock-args call=%u output=%08X state13_28=%08X\n",
                         g_interblock_trace_calls++, stack[10], state_array[7]);
        }
        if (!g_interblock_captured && stack[10] == g_interblock_target_output) {
            dump_interblock_entry(context);
            g_interblock_captured = true;
        }
        DWORD old_protect;
        VirtualProtect(g_interblock_entry, 1u, PAGE_EXECUTE_READWRITE, &old_protect);
        *g_interblock_entry = g_interblock_original_byte;
        FlushInstructionCache(GetCurrentProcess(), g_interblock_entry, 1u);
        context->Eip = reinterpret_cast<DWORD>(g_interblock_entry);
        context->EFlags |= 0x100u;
        g_interblock_single_step = true;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == EXCEPTION_SINGLE_STEP && g_interblock_single_step) {
        if (!g_interblock_captured) {
            DWORD old_protect;
            VirtualProtect(g_interblock_entry, 1u, PAGE_EXECUTE_READWRITE, &old_protect);
            *g_interblock_entry = 0xCCu;
            FlushInstructionCache(GetCurrentProcess(), g_interblock_entry, 1u);
        }
        context->EFlags &= ~0x100u;
        g_interblock_single_step = false;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
LONG CALLBACK output_write_watch(EXCEPTION_POINTERS* info) {
    auto* context = info->ContextRecord;
    if (info->ExceptionRecord->ExceptionCode == STATUS_GUARD_PAGE_VIOLATION) {
        const auto address = static_cast<std::uint32_t>(info->ExceptionRecord->ExceptionInformation[1]);
        if (address >= 0x00950640u && address < 0x00950660u) {
            const auto original_eip = kXmvVirtualAddress + context->Eip -
                                      reinterpret_cast<DWORD>(g_relocated_xmv);
            const auto* frame = reinterpret_cast<const std::uint32_t*>(context->Ebp);
            std::fprintf(stderr, "native-output-access type=%u address=%08X eip=%08X original=%08X ebp=%08X args=%08X/%08X/%08X\n",
                         static_cast<unsigned>(info->ExceptionRecord->ExceptionInformation[0]),
                         address, context->Eip, original_eip, context->Ebp,
                         frame[2], frame[3], frame[4]);
            std::fflush(stderr);
        }
        context->EFlags |= 0x100u;
        g_guard_single_step = true;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (info->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP &&
        g_guard_single_step) {
        DWORD old_protect;
        VirtualProtect(reinterpret_cast<void*>(0x00950000u), 0x1000u,
                       PAGE_EXECUTE_READWRITE | PAGE_GUARD, &old_protect);
        context->EFlags &= ~0x100u;
        g_guard_single_step = false;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}LONG WINAPI crash_filter(EXCEPTION_POINTERS* info) {
    const auto* context = info->ContextRecord;
    std::fprintf(stderr,
        "retail replay exception code=%08X address=%p eip=%08X esp=%08X eax=%08X ebx=%08X ecx=%08X edx=%08X esi=%08X edi=%08X\n",
        info->ExceptionRecord->ExceptionCode, info->ExceptionRecord->ExceptionAddress,
        context->Eip, context->Esp, context->Eax, context->Ebx, context->Ecx,
        context->Edx, context->Esi, context->Edi);
    std::fflush(stderr);
    const auto* stack = reinterpret_cast<const std::uint32_t*>(context->Esp);
    for (unsigned index = 0; index < 24u; ++index)
        std::fprintf(stderr, "stack[%02u]=%08X\n", index, stack[index]);
    std::fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}
}

int main(int argc, char** argv) {
    SetUnhandledExceptionFilter(crash_filter);
    if (argc != 4) {
        std::fprintf(stderr, "usage: %s default.xbe frame-before.bin frame-after.bin\n", argv[0]);
        return 2;
    }
    void* guest = VirtualAlloc(reinterpret_cast<void*>(kGuestBase), kGuestSize,
                               MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (guest != reinterpret_cast<void*>(kGuestBase)) {
        std::fprintf(stderr, "failed to map guest heap at %p (got %p, error %lu)\n",
                     reinterpret_cast<void*>(kGuestBase), guest, GetLastError());
        for (std::uintptr_t address = kGuestBase; address < kGuestBase + kGuestSize;) {
            MEMORY_BASIC_INFORMATION region = {};
            if (!VirtualQuery(reinterpret_cast<void*>(address), &region, sizeof(region))) break;
            if (region.State != MEM_FREE)
                std::fprintf(stderr, "occupied base=%p size=%08X state=%08X type=%08X protect=%08X\n",
                             region.BaseAddress, static_cast<unsigned>(region.RegionSize),
                             region.State, region.Type, region.Protect);
            address = reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize;
        }
        return 2;
    }
    const auto xbe = read_file(argv[1]); const auto before = read_file(argv[2]);
    const auto after = read_file(argv[3]);
    if (xbe.size() < kXmvRawAddress + kXmvRawSize ||
        before.size() != kHeaderSize + kSnapshotSize || after.size() != before.size()) {
        std::fprintf(stderr, "invalid retail XBE or frame snapshot size\n"); return 2;
    }
    std::uint32_t header[4]; std::memcpy(header, before.data(), sizeof(header));
    if (header[0] != 0x002582FFu || header[2] != 1u) {
        std::fprintf(stderr, "unexpected routine header\n"); return 2;
    }
    std::memcpy(guest, before.data() + kHeaderSize + (kGuestBase - kSnapshotBase), kGuestSize);
    auto* xmv = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, kXmvVirtualSize,
        MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    if (!xmv) { std::fprintf(stderr, "failed to allocate relocated XMV\n"); return 2; }
    std::memcpy(xmv, xbe.data() + kXmvRawAddress, kXmvRawSize);
    std::memcpy(xmv + kXmvRawSize,
                before.data() + kHeaderSize + (kXmvVirtualAddress + kXmvRawSize - kSnapshotBase),
                kXmvVirtualSize - kXmvRawSize);
    unsigned relocations = 0;
    for (const std::uint32_t offset : kXmvRelocationOffsets) {
        std::uint32_t absolute; std::memcpy(&absolute, xmv + offset, sizeof(absolute));
        if (absolute < kXmvVirtualAddress || absolute >= kXmvVirtualAddress + kXmvVirtualSize) {
            std::fprintf(stderr, "invalid generated relocation at %08X value=%08X\n", offset, absolute);
            return 2;
        }
        const auto relocated = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(xmv)) +
                               (absolute - kXmvVirtualAddress);
        std::memcpy(xmv + offset, &relocated, sizeof(relocated)); ++relocations;
    }
    FlushInstructionCache(GetCurrentProcess(), xmv, kXmvVirtualSize);
    g_relocated_xmv = xmv;
    g_interblock_dump_path = std::getenv("XMV_NATIVE_INTERBLOCK_DUMP");
    if (const char* output = std::getenv("XMV_NATIVE_INTERBLOCK_OUTPUT"))
        g_interblock_target_output = static_cast<std::uint32_t>(std::strtoul(output, nullptr, 0));
    g_interblock_frame = header[1];
    if (g_interblock_dump_path) {
        g_interblock_entry = xmv + (0x002576F8u - kXmvVirtualAddress);
        g_interblock_original_byte = *g_interblock_entry;
        DWORD breakpoint_protect;
        VirtualProtect(g_interblock_entry, 1u, PAGE_EXECUTE_READWRITE, &breakpoint_protect);
        *g_interblock_entry = 0xCCu;
        FlushInstructionCache(GetCurrentProcess(), g_interblock_entry, 1u);
        AddVectoredExceptionHandler(1u, interblock_entry_watch);
    }
    AddVectoredExceptionHandler(1u, output_write_watch);
    DWORD old_protect;
    VirtualProtect(reinterpret_cast<void*>(0x00950000u), 0x1000u,
                   PAGE_EXECUTE_READWRITE | PAGE_GUARD, &old_protect);
    using FrameDecode = void(__stdcall*)(std::uint32_t);
    auto routine = reinterpret_cast<FrameDecode>(xmv + (header[0] - kXmvVirtualAddress));
    std::fprintf(stderr, "relocated_xmv=%p routine=%p decoder=%08X relocations=%u\n",
                 xmv, routine, header[3], relocations);
    std::fflush(stderr);
    routine(header[3]);
    VirtualProtect(reinterpret_cast<void*>(0x00950000u), 0x1000u,
                   PAGE_EXECUTE_READWRITE, &old_protect);
    const auto* actual = reinterpret_cast<const std::uint8_t*>(kGuestBase);
    const auto* expected = after.data() + kHeaderSize + (kGuestBase - kSnapshotBase);
    const auto* initial = before.data() + kHeaderSize + (kGuestBase - kSnapshotBase);
    std::uint32_t differences = 0, first = 0xFFFFFFFFu, last = 0u;
    for (std::uint32_t offset = kCompareOffset; offset < kGuestSize; ++offset) {
        if (actual[offset] != expected[offset]) {
            if (differences < 96u) {
                std::printf("diff %08X initial=%02X retail=%02X recomp=%02X\n",
                            static_cast<unsigned>(kGuestBase + offset), initial[offset],
                            actual[offset], expected[offset]);
            }
            if (first == 0xFFFFFFFFu) first = static_cast<std::uint32_t>(kGuestBase + offset);
            last = static_cast<std::uint32_t>(kGuestBase + offset); ++differences;
        }
    }
    std::printf("routine=%08X frame=%u decoder=%08X relocations=%u differing_guest_bytes=%u first=%08X last=%08X\n",
                header[0], header[1], header[3], relocations, differences, first, last);
    return differences == 0u ? 0 : 1;
}