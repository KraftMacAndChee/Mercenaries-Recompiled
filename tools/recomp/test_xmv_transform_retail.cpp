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
constexpr std::uint32_t kTransform8Address = 0x002598FCu;
constexpr std::uint32_t kTransform8Size = 0x00000277u;
constexpr std::uint32_t kTransform4Address = 0x00259B73u;
constexpr std::uint32_t kTransform4Size = 0x000002AAu;
constexpr std::size_t kCaptureSize = 8u + 128u + 128u;

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

std::uint32_t load_u32(const std::vector<std::uint8_t>& bytes,
                       std::size_t offset) {
    std::uint32_t value;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr,
                     "usage: %s default.xbe transform-before.bin transform-after.bin\n",
                     argv[0]);
        return 2;
    }
    const auto xbe = read_file(argv[1]);
    const auto before = read_file(argv[2]);
    const auto after = read_file(argv[3]);
    if (xbe.size() < kXmvRawAddress + kXmvRawSize ||
        before.size() != kCaptureSize || after.size() != kCaptureSize) {
        std::fprintf(stderr, "unexpected retail XBE or capture size\n");
        return 2;
    }
    const std::uint32_t address = load_u32(before, 0u);
    const std::uint32_t block_index = load_u32(before, 4u);
    const std::uint32_t routine_size = address == kTransform8Address
        ? kTransform8Size
        : address == kTransform4Address ? kTransform4Size : 0u;
    if (routine_size == 0u || load_u32(after, 0u) != address ||
        load_u32(after, 4u) != block_index) {
        std::fprintf(stderr, "invalid transform capture header\n");
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
    auto* routine = mapping + (address - kXmvVirtualAddress);
    unsigned relocations = 0u;
    for (std::uint32_t offset = 0u; offset + 4u <= routine_size; ++offset) {
        std::uint32_t absolute;
        std::memcpy(&absolute, routine + offset, sizeof(absolute));
        if (absolute >= kXmvVirtualAddress &&
            absolute < kXmvVirtualAddress + kXmvRawSize) {
            const std::uint32_t relocated = static_cast<std::uint32_t>(
                reinterpret_cast<std::uintptr_t>(mapping)) +
                (absolute - kXmvVirtualAddress);
            std::memcpy(routine + offset, &relocated, sizeof(relocated));
            ++relocations;
        }
    }
    FlushInstructionCache(GetCurrentProcess(), mapping, kXmvRawSize);

    alignas(16) std::int16_t coefficients[64];
    alignas(16) std::int16_t output[64];
    std::memcpy(coefficients, before.data() + 8u, sizeof(coefficients));
    std::memcpy(output, before.data() + 136u, sizeof(output));
    using Transform = void(__stdcall*)(std::int16_t*, const std::int16_t*,
                                       std::uint32_t);
    reinterpret_cast<Transform>(routine)(output, coefficients, block_index);

    const auto* expected = after.data() + 136u;
    unsigned differences = 0u;
    for (unsigned i = 0u; i < sizeof(output); ++i) {
        if (reinterpret_cast<const std::uint8_t*>(output)[i] != expected[i])
            ++differences;
    }
    std::printf("routine=%08X block=%u relocations=%u differing_output_bytes=%u\n",
                address, block_index, relocations, differences);
    if (differences != 0u) {
        for (unsigned i = 0u; i < 64u; ++i) {
            std::int16_t expected_word;
            std::memcpy(&expected_word, expected + i * 2u,
                        sizeof(expected_word));
            if (output[i] != expected_word)
                std::printf("  word[%u] retail=%d recomp=%d\n", i,
                            output[i], expected_word);
        }
        return 1;
    }
    return 0;
}