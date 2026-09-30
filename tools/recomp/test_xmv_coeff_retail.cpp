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
constexpr std::uint32_t kRoutineAddress = 0x00257DB9u;
constexpr std::uint32_t kMagic = 0x434D5658u;
constexpr std::size_t kHeaderWords = 12u;
constexpr std::size_t kCoefficientBytes = 128u;

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
                     "usage: %s default.xbe coeff-before.bin coeff-after.bin\n",
                     argv[0]);
        return 2;
    }
    const auto xbe = read_file(argv[1]);
    const auto before = read_file(argv[2]);
    const auto after = read_file(argv[3]);
    if (xbe.size() < kXmvRawAddress + kXmvRawSize ||
        before.size() < kHeaderWords * 4u + kCoefficientBytes ||
        after.size() != before.size() || load_u32(before, 0u) != kMagic ||
        load_u32(after, 0u) != kMagic) {
        std::fprintf(stderr, "unexpected retail XBE or capture layout\n");
        return 2;
    }

    const std::uint32_t stream_bytes = load_u32(before, 11u * 4u);
    const std::size_t expected_size =
        kHeaderWords * 4u + stream_bytes + kCoefficientBytes;
    if (before.size() != expected_size) {
        std::fprintf(stderr, "invalid captured stream length\n");
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

    auto relocate = [mapping](std::uint32_t address) -> std::uint32_t {
        if (address < kXmvVirtualAddress ||
            address >= kXmvVirtualAddress + kXmvRawSize) {
            std::fprintf(stderr, "XMV pointer %08X is outside the retail section\n",
                         address);
            std::exit(2);
        }
        return static_cast<std::uint32_t>(
            reinterpret_cast<std::uintptr_t>(mapping)) +
            (address - kXmvVirtualAddress);
    };

    const std::uint32_t original_vlc = load_u32(before, 2u * 4u);
    auto* relocated_vlc = reinterpret_cast<std::uint32_t*>(relocate(original_vlc));
    unsigned descriptor_relocations = 0u;
    for (unsigned i = 0; i < 9u; ++i) {
        if (relocated_vlc[i] >= kXmvVirtualAddress &&
            relocated_vlc[i] < kXmvVirtualAddress + kXmvRawSize) {
            relocated_vlc[i] = relocate(relocated_vlc[i]);
            ++descriptor_relocations;
        }
    }
    FlushInstructionCache(GetCurrentProcess(), mapping, kXmvRawSize);

    struct BitReader {
        std::uint32_t word;
        std::uint32_t bits;
        const std::uint8_t* stream;
    } reader;
    std::vector<std::uint8_t> stream(stream_bytes);
    std::memcpy(stream.data(), before.data() + kHeaderWords * 4u,
                stream.size());
    reader.word = load_u32(before, 7u * 4u);
    reader.bits = load_u32(before, 8u * 4u);
    reader.stream = stream.data();

    alignas(16) std::int16_t coefficients[64];
    std::memcpy(coefficients,
                before.data() + kHeaderWords * 4u + stream_bytes,
                sizeof(coefficients));
    std::uint32_t state1 = load_u32(before, 4u * 4u);
    std::uint32_t state2 = load_u32(before, 5u * 4u);
    const std::uint32_t quantizer = load_u32(before, 1u * 4u);
    const std::uint32_t relocated_scan = relocate(load_u32(before, 3u * 4u));

    using CoefficientDecoder = std::uint32_t(__stdcall*)(
        BitReader*, std::uint32_t, const void*, const std::uint8_t*,
        std::int16_t*, std::uint32_t*, std::uint32_t*);
    auto* routine = reinterpret_cast<CoefficientDecoder>(
        mapping + (kRoutineAddress - kXmvVirtualAddress));
    const std::uint32_t result = routine(
        &reader, quantizer, reinterpret_cast<const void*>(relocate(original_vlc)),
        reinterpret_cast<const std::uint8_t*>(relocated_scan), coefficients,
        &state1, &state2);

    const std::uint32_t expected_result = load_u32(after, 6u * 4u);
    const std::uint32_t expected_word = load_u32(after, 7u * 4u);
    const std::uint32_t expected_bits = load_u32(after, 8u * 4u);
    const std::uint32_t before_stream_address = load_u32(before, 10u * 4u);
    const std::uint32_t expected_stream_address = load_u32(after, 9u * 4u);
    const std::uint32_t expected_consumed =
        expected_stream_address - before_stream_address;
    const std::uint32_t consumed = static_cast<std::uint32_t>(
        reader.stream - stream.data());
    const std::uint32_t expected_state1 = load_u32(after, 4u * 4u);
    const std::uint32_t expected_state2 = load_u32(after, 5u * 4u);
    const auto* expected_coefficients =
        after.data() + kHeaderWords * 4u + stream_bytes;

    unsigned coefficient_differences = 0u;
    for (std::size_t i = 0; i < sizeof(coefficients); ++i) {
        if (reinterpret_cast<const std::uint8_t*>(coefficients)[i] !=
            expected_coefficients[i])
            ++coefficient_differences;
    }
    const bool state_matches =
        result == expected_result && reader.word == expected_word &&
        reader.bits == expected_bits && consumed == expected_consumed &&
        state1 == expected_state1 && state2 == expected_state2;
    std::printf(
        "q=%u desc_reloc=%u result=%08X/%08X word=%08X/%08X "
        "bits=%u/%u consumed=%u/%u state=%u,%u/%u,%u coeff_diff=%u\n",
        quantizer, descriptor_relocations, result, expected_result,
        reader.word, expected_word, reader.bits, expected_bits, consumed,
        expected_consumed, state1, state2, expected_state1, expected_state2,
        coefficient_differences);
    if (!state_matches || coefficient_differences != 0u) {
        for (unsigned i = 0; i < 64u; ++i) {
            std::int16_t expected;
            std::memcpy(&expected, expected_coefficients + i * 2u,
                        sizeof(expected));
            if (coefficients[i] != expected)
                std::printf("  coefficient[%u] retail=%d recomp=%d\n", i,
                            coefficients[i], expected);
        }
        return 1;
    }
    return 0;
}