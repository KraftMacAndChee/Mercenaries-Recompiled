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
struct RoutineInfo { std::uint32_t address, size; unsigned kind; };
RoutineInfo routine_info(std::uint32_t address) {
    switch (address) {
    case 0x0025A55Cu: return {address, 0x281u, 6u};
    case 0x0025A7DDu: return {address, 0x378u, 6u};
    case 0x0025AB55u: return {address, 0x1FFu, 7u};
    case 0x0025AD54u: return {address, 0x1E5u, 7u};
    case 0x0025AF39u: return {address, 0x66Du, 70u};
    case 0x0025B5A6u: return {address, 0x83Du, 8u};
    default: std::fprintf(stderr, "unsupported predictor routine %08X\n", address); std::exit(2);
    }
}
std::vector<std::uint8_t> read_file(const char* path) {
    FILE* file = std::fopen(path, "rb");
    if (!file) { std::fprintf(stderr, "failed to open %s\n", path); std::exit(2); }
    std::fseek(file, 0, SEEK_END); const long size = std::ftell(file); std::rewind(file);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty() && std::fread(bytes.data(), 1, bytes.size(), file) != bytes.size()) {
        std::fprintf(stderr, "failed to read %s\n", path); std::exit(2);
    }
    std::fclose(file); return bytes;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: %s default.xbe predictor-before.bin predictor-after.bin\n", argv[0]);
        return 2;
    }
    const auto xbe = read_file(argv[1]); const auto before = read_file(argv[2]);
    const auto after = read_file(argv[3]); constexpr std::size_t kHeaderBytes = 48u;
    if (xbe.size() < kXmvRawAddress + kXmvRawSize || before.size() < kHeaderBytes || after.size() != before.size()) {
        std::fprintf(stderr, "unexpected retail XBE or capture size\n"); return 2;
    }
    std::uint32_t header[12]; std::memcpy(header, before.data(), sizeof(header));
    const auto info = routine_info(header[0]); const std::uint32_t source_stride = header[1];
    const std::uint32_t destination_stride = header[2], horizontal = header[3], vertical = header[4];
    const std::uint32_t auxiliary = header[5], source_offset = header[6], source_bytes = header[7];
    const std::uint32_t destination_bytes = header[8], residual_bytes = header[9];
    const std::size_t expected_size = kHeaderBytes + source_bytes + destination_bytes + residual_bytes;
    if (before.size() != expected_size || source_offset >= source_bytes) {
        std::fprintf(stderr, "invalid predictor capture layout\n"); return 2;
    }
    auto* mapping = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, kXmvRawSize, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    if (!mapping) { std::fprintf(stderr, "failed to allocate relocated XMV (error %lu)\n", GetLastError()); return 2; }
    std::memcpy(mapping, xbe.data() + kXmvRawAddress, kXmvRawSize);
    auto* routine = mapping + (info.address - kXmvVirtualAddress); unsigned relocations = 0;
    for (std::uint32_t offset = 0; offset + 4u <= info.size; ++offset) {
        std::uint32_t absolute; std::memcpy(&absolute, routine + offset, sizeof(absolute));
        if (absolute >= kXmvVirtualAddress && absolute < kXmvVirtualAddress + kXmvRawSize) {
            const std::uint32_t relocated = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(mapping)) + (absolute - kXmvVirtualAddress);
            std::memcpy(routine + offset, &relocated, sizeof(relocated)); ++relocations;
        }
    }
    FlushInstructionCache(GetCurrentProcess(), mapping, kXmvRawSize);
    std::vector<std::uint8_t> source(source_bytes), destination(destination_bytes), residual(residual_bytes);
    const auto* payload = before.data() + kHeaderBytes; std::memcpy(source.data(), payload, source_bytes);
    std::memcpy(destination.data(), payload + source_bytes, destination_bytes);
    if (residual_bytes) std::memcpy(residual.data(), payload + source_bytes + destination_bytes, residual_bytes);
    const auto* source_pointer = source.data() + source_offset; auto* residual_pointer = residual.empty() ? nullptr : residual.data();
    using P6 = void(__stdcall*)(const std::uint8_t*, std::uint32_t, std::uint8_t*, std::uint32_t, std::uint32_t, std::uint32_t);
    using P7R = void(__stdcall*)(const std::uint8_t*, std::uint32_t, std::uint8_t*, std::uint32_t, std::uint32_t, std::uint32_t, const std::uint8_t*);
    using P7A = void(__stdcall*)(const std::uint8_t*, std::uint32_t, std::uint8_t*, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t);
    using P8 = void(__stdcall*)(const std::uint8_t*, std::uint32_t, std::uint8_t*, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, const std::uint8_t*);
    if (info.kind == 6u) reinterpret_cast<P6>(routine)(source_pointer, source_stride, destination.data(), destination_stride, horizontal, vertical);
    else if (info.kind == 7u) reinterpret_cast<P7R>(routine)(source_pointer, source_stride, destination.data(), destination_stride, horizontal, vertical, residual_pointer);
    else if (info.kind == 70u) reinterpret_cast<P7A>(routine)(source_pointer, source_stride, destination.data(), destination_stride, horizontal, vertical, auxiliary);
    else reinterpret_cast<P8>(routine)(source_pointer, source_stride, destination.data(), destination_stride, horizontal, vertical, auxiliary, residual_pointer);
    const auto* expected = after.data() + kHeaderBytes + source_bytes; unsigned differences = 0;
    for (std::uint32_t i = 0; i < destination_bytes; ++i) differences += destination[i] != expected[i];
    std::printf("routine=%08X frame=%u half=%u,%u aux=%08X src_stride=%u dst_stride=%u destination_bytes=%u residual_bytes=%u relocations=%u differing_destination_bytes=%u\n", info.address, header[10], horizontal, vertical, auxiliary, source_stride, destination_stride, destination_bytes, residual_bytes, relocations, differences);
    return differences == 0u ? 0 : 1;
}