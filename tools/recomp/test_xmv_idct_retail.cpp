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
constexpr std::uint32_t kIdctAddress = 0x0025927Au;
constexpr std::uint32_t kIdctSize = 0x00000682u;

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
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s default.xbe idct-before.bin [idct-after.bin]\n", argv[0]);
        return 2;
    }

    const auto xbe = read_file(argv[1]);
    const auto before = read_file(argv[2]);
    if (xbe.size() < kXmvRawAddress + kXmvRawSize || before.size() != 260u) {
        std::fprintf(stderr, "unexpected retail XBE or capture size\n");
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
    auto* idct_code = mapping + (kIdctAddress - kXmvVirtualAddress);
    unsigned relocations = 0;
    for (std::uint32_t offset = 0; offset + 4u <= kIdctSize; ++offset) {
        std::uint32_t absolute;
        std::memcpy(&absolute, idct_code + offset, sizeof(absolute));
        if (absolute >= kXmvVirtualAddress &&
            absolute < kXmvVirtualAddress + kXmvRawSize) {
            const std::uint32_t relocated =
                static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(mapping)) +
                (absolute - kXmvVirtualAddress);
            std::memcpy(idct_code + offset, &relocated, sizeof(relocated));
            ++relocations;
        }
    }
    std::fprintf(stderr, "relocated %u retail XMV absolute operands\n", relocations);
    FlushInstructionCache(GetCurrentProcess(), mapping, kXmvRawSize);
    const std::uint32_t mask = *reinterpret_cast<const std::uint32_t*>(before.data());
    alignas(16) std::int16_t coefficients[64];
    alignas(16) std::int16_t output[64];
    std::memcpy(coefficients, before.data() + 4u, sizeof(coefficients));
    std::memcpy(output, before.data() + 132u, sizeof(output));

    using Idct = void(__stdcall*)(std::int16_t*, const std::int16_t*, std::uint32_t);
    reinterpret_cast<Idct>(idct_code)(output, coefficients, mask);

    if (argc >= 4) {
        const auto after = read_file(argv[3]);
        if (after.size() != 260u) {
            std::fprintf(stderr, "unexpected after-capture size\n");
            return 2;
        }
        const auto* expected = after.data() + 132u;
        unsigned differences = 0;
        for (unsigned i = 0; i < sizeof(output); ++i) {
            if (reinterpret_cast<const std::uint8_t*>(output)[i] != expected[i])
                ++differences;
        }
        std::printf("mask=%08X differing_output_bytes=%u\n", mask, differences);
        if (differences != 0u) {
            for (unsigned i = 0; i < 64u; ++i) {
                const auto expected_word =
                    reinterpret_cast<const std::int16_t*>(expected)[i];
                if (output[i] != expected_word)
                    std::printf("  word[%u] retail=%d recomp=%d\n",
                                i, output[i], expected_word);
            }
            return 1;
        }
    } else {
        for (unsigned i = 0; i < 64u; ++i)
            std::printf("%d%c", output[i], i == 63u ? '\n' : ',');
    }

    return 0;
}