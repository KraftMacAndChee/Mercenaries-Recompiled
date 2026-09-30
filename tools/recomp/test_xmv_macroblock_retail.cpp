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
constexpr std::uint32_t kXmvVirtualAddress = 0x00255620u;
constexpr std::uint32_t kXmvRawAddress = 0x00246000u;
constexpr std::uint32_t kXmvRawSize = 0x00027F84u;
constexpr std::uint32_t kXmvVirtualSize = 0x00027F94u;
constexpr std::uint32_t kXmvRelocationOffsets[] = {
#include "xmv_relocation_offsets.inc"
};
struct GuestRange { std::uintptr_t base; std::size_t size; };
constexpr GuestRange kRanges[] = {
    {0x00800000u, 0x03800000u},
};
constexpr std::size_t kHeaderSize = 21u * sizeof(std::uint32_t);
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
LONG WINAPI crash_filter(EXCEPTION_POINTERS* info) {
    const auto* context = info->ContextRecord;
    std::fprintf(stderr,
        "macroblock replay exception code=%08X address=%p eip=%08X esp=%08X eax=%08X ebx=%08X ecx=%08X edx=%08X esi=%08X edi=%08X\n",
        info->ExceptionRecord->ExceptionCode, info->ExceptionRecord->ExceptionAddress,
        context->Eip, context->Esp, context->Eax, context->Ebx, context->Ecx,
        context->Edx, context->Esi, context->Edi);
    std::fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}
}

int main(int argc, char** argv) {
    SetUnhandledExceptionFilter(crash_filter);
    if (argc != 4) {
        std::fprintf(stderr, "usage: %s default.xbe macroblock-before.bin macroblock-after.bin\n", argv[0]);
        return 2;
    }
    for (const auto& range : kRanges) {
        void* mapped = VirtualAlloc(reinterpret_cast<void*>(range.base), range.size,
                                    MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (mapped != reinterpret_cast<void*>(range.base)) {
            std::fprintf(stderr, "failed to map exact guest range at %p size %08X (got %p, error %lu)\n",
                         reinterpret_cast<void*>(range.base), static_cast<unsigned>(range.size),
                         mapped, GetLastError());
            MEMORY_BASIC_INFORMATION mbi{};
            for (std::uintptr_t cursor = 0x00010000u; cursor < 0x01000000u; ) {
                if (!VirtualQuery(reinterpret_cast<void*>(cursor), &mbi, sizeof(mbi))) break;
                const auto base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
                if (mbi.State != MEM_FREE) {
                    std::fprintf(stderr, "occupied %08X..%08X state=%08X type=%08X protect=%08X\n",
                                 static_cast<unsigned>(base), static_cast<unsigned>(base + mbi.RegionSize),
                                 mbi.State, mbi.Type, mbi.Protect);
                }
                cursor = base + mbi.RegionSize;
            }
            return 2;
        }
    }
    const auto xbe = read_file(argv[1]); const auto before = read_file(argv[2]);
    const auto after = read_file(argv[3]);
    if (xbe.size() < kXmvRawAddress + kXmvRawSize ||
        before.size() != kHeaderSize + kSnapshotSize || after.size() != before.size()) {
        std::fprintf(stderr, "invalid macroblock snapshot size\n"); return 2;
    }
    std::uint32_t header[21]; std::memcpy(header, before.data(), sizeof(header));
    if (header[0] != 0x00257F93u || header[2] != 18u) {
        std::fprintf(stderr, "unexpected routine header\n"); return 2;
    }
    for (const auto& range : kRanges) {
        void* mapped = reinterpret_cast<void*>(range.base);
        const std::size_t snapshot_offset = range.base - kSnapshotBase;
        std::memcpy(mapped, before.data() + kHeaderSize + snapshot_offset, range.size);
        FlushInstructionCache(GetCurrentProcess(), mapped, range.size);
    }
    using Macroblock = void(__stdcall*)(std::uint32_t, std::uint32_t, std::uint32_t,
        std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t,
        std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t,
        std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t);
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
        if (absolute < kXmvVirtualAddress || absolute >= kXmvVirtualAddress + kXmvVirtualSize)
            return 2;
        const auto relocated = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(xmv)) +
                               (absolute - kXmvVirtualAddress);
        std::memcpy(xmv + offset, &relocated, sizeof(relocated)); ++relocations;
    }
    const auto relocate_value = [xmv](std::uint32_t value) {
        if (value >= kXmvVirtualAddress && value < kXmvVirtualAddress + kXmvVirtualSize)
            return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(xmv)) +
                   (value - kXmvVirtualAddress);
        return value;
    };
    for (unsigned index = 3u; index < 21u; ++index)
        header[index] = relocate_value(header[index]);
    const std::uint32_t pointer_objects[] = { header[19], header[20] };
    const std::uint32_t object_sizes[] = { 0x100u, 0x100u };
    for (unsigned object = 0u; object < 2u; ++object) {
        if (pointer_objects[object] < 0x00800000u || pointer_objects[object] >= 0x04000000u)
            continue;
        for (std::uint32_t offset = 0u; offset < object_sizes[object]; offset += 4u) {
            auto* value = reinterpret_cast<std::uint32_t*>(pointer_objects[object] + offset);
            *value = relocate_value(*value);
        }
    }    FlushInstructionCache(GetCurrentProcess(), xmv, kXmvVirtualSize);
    auto routine = reinterpret_cast<Macroblock>(xmv + (header[0] - kXmvVirtualAddress));
    routine(header[3], header[4], header[5], header[6], header[7], header[8],
            header[9], header[10], header[11], header[12], header[13], header[14],
            header[15], header[16], header[17], header[18], header[19], header[20]);
    std::uint32_t differences = 0, first = 0xFFFFFFFFu, last = 0u;
    for (const auto& range : kRanges) {
        const auto* actual = reinterpret_cast<const std::uint8_t*>(range.base);
        const auto* expected = after.data() + kHeaderSize + (range.base - kSnapshotBase);
        const auto* initial = before.data() + kHeaderSize + (range.base - kSnapshotBase);
        for (std::uint32_t offset = 0; offset < range.size; ++offset) {
            const std::uint32_t va = static_cast<std::uint32_t>(range.base + offset);
            if (va >= 0x00700000u && va < 0x008C0000u) continue;
            if (actual[offset] != expected[offset]) {
                if (differences < 96u) {
                    std::printf("diff %08X initial=%02X retail=%02X recomp=%02X\n",
                                va, initial[offset], actual[offset], expected[offset]);
                }
                if (first == 0xFFFFFFFFu) first = va;
                last = va; ++differences;
            }
        }
    }
    std::printf("routine=%08X frame=%u relocations=%u differing_guest_bytes=%u first=%08X last=%08X\n",
                header[0], header[1], relocations, differences, first, last);
    return differences == 0u ? 0 : 1;
}