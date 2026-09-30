#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
constexpr std::uint32_t kXmvVirtualAddress = 0x00255620u;
constexpr std::uint32_t kXmvRawAddress = 0x00246000u;
constexpr std::uint32_t kXmvRawSize = 0x00027F84u;
constexpr std::uint32_t kRoutineAddress = 0x0025AB55u;
constexpr std::uint32_t kRoutineSize = 0x000001FFu;

std::vector<std::uint8_t> read_file(const char* path) {
    FILE* file = std::fopen(path, "rb");
    if (!file) {
        std::fprintf(stderr, "failed to open %s\n", path);
        std::exit(2);
    }
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::rewind(file);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty() &&
        std::fread(bytes.data(), 1, bytes.size(), file) != bytes.size()) {
        std::fprintf(stderr, "failed to read %s\n", path);
        std::exit(2);
    }
    std::fclose(file);
    return bytes;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: %s default.xbe mc-before.bin mc-after.bin\n", argv[0]);
        return 2;
    }
    const auto xbe = read_file(argv[1]);
    const auto before = read_file(argv[2]);
    const auto after = read_file(argv[3]);
    if (xbe.size() < kXmvRawAddress + kXmvRawSize || before.size() < 28u ||
        after.size() != before.size()) {
        std::fprintf(stderr, "unexpected retail XBE or capture size\n");
        return 2;
    }

    std::uint32_t header[7];
    std::memcpy(header, before.data(), sizeof(header));
    const std::uint32_t source_stride = header[0];
    const std::uint32_t destination_stride = header[1];
    const std::uint32_t horizontal = header[2];
    const std::uint32_t vertical = header[3];
    const std::uint32_t source_bytes = header[4];
    const std::uint32_t destination_bytes = header[5];
    const std::uint32_t residual_bytes = header[6];
    if (before.size() != 28u + source_bytes + destination_bytes + residual_bytes ||
        residual_bytes != 128u) {
        std::fprintf(stderr, "invalid motion-comp capture layout\n");
        return 2;
    }

    auto* mapping = static_cast<std::uint8_t*>(
        VirtualAlloc(nullptr, kXmvRawSize, MEM_RESERVE | MEM_COMMIT,
                     PAGE_EXECUTE_READWRITE));
    if (!mapping) {
        std::fprintf(stderr, "failed to allocate relocated XMV (error %lu)\n",
                     GetLastError());
        return 2;
    }
    std::memcpy(mapping, xbe.data() + kXmvRawAddress, kXmvRawSize);
    auto* routine = mapping + (kRoutineAddress - kXmvVirtualAddress);
    unsigned relocations = 0;
    for (std::uint32_t offset = 0; offset + 4u <= kRoutineSize; ++offset) {
        std::uint32_t absolute;
        std::memcpy(&absolute, routine + offset, sizeof(absolute));
        if (absolute >= kXmvVirtualAddress &&
            absolute < kXmvVirtualAddress + kXmvRawSize) {
            const std::uint32_t relocated =
                static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(mapping)) +
                (absolute - kXmvVirtualAddress);
            std::memcpy(routine + offset, &relocated, sizeof(relocated));
            ++relocations;
        }
    }
    FlushInstructionCache(GetCurrentProcess(), mapping, kXmvRawSize);

    std::vector<std::uint8_t> source(source_bytes);
    std::vector<std::uint8_t> destination(destination_bytes);
    alignas(16) std::int16_t residual[64];
    std::memcpy(source.data(), before.data() + 28u, source_bytes);
    std::memcpy(destination.data(), before.data() + 28u + source_bytes,
                destination_bytes);
    std::memcpy(residual,
                before.data() + 28u + source_bytes + destination_bytes,
                sizeof(residual));

    using MotionComp = void(__stdcall*)(const std::uint8_t*, std::uint32_t,
                                         std::uint8_t*, std::uint32_t,
                                         std::uint32_t, std::uint32_t,
                                         const std::int16_t*);
    reinterpret_cast<MotionComp>(routine)(
        source.data(), source_stride, destination.data(), destination_stride,
        horizontal, vertical, residual);

    const auto* expected = after.data() + 28u + source_bytes;
    unsigned differences = 0;
    for (std::uint32_t i = 0; i < destination_bytes; ++i) {
        if (destination[i] != expected[i])
            ++differences;
    }
    std::printf("half=%u,%u src_stride=%u dst_stride=%u relocations=%u differing_destination_bytes=%u\n",
                horizontal, vertical, source_stride, destination_stride,
                relocations, differences);
    if (differences != 0u) {
        for (std::uint32_t row = 0; row < 8u; ++row) {
            for (std::uint32_t column = 0; column < 8u; ++column) {
                const std::uint32_t offset = row * destination_stride + column;
                if (destination[offset] != expected[offset])
                    std::printf("  pixel[%u,%u] retail=%u recomp=%u\n",
                                column, row, destination[offset], expected[offset]);
            }
        }
        return 1;
    }
    return 0;
}