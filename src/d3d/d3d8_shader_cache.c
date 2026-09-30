#include "d3d8_shader_cache.h"

#if defined(_WIN32)
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#define SHADER_CACHE_MAGIC 0x4D524353u
#define SHADER_CACHE_VERSION 1u

typedef struct ShaderCacheHeader {
    uint32_t magic;
    uint32_t version;
    uint64_t source_hash;
    uint64_t bytecode_size;
} ShaderCacheHeader;

static int shader_cache_path(char path[MAX_PATH], const char *kind,
                             uint64_t source_hash)
{
    char base[MAX_PATH];
    char root[MAX_PATH];
    DWORD length;
    if (getenv("MERCENARIES_DISABLE_SHADER_DISK_CACHE") != NULL)
        return 0;
    length = GetEnvironmentVariableA("LOCALAPPDATA", base, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return 0;
    if (_snprintf_s(root, sizeof(root), _TRUNCATE,
                    "%s\\MercenariesRecomp", base) < 0)
        return 0;
    CreateDirectoryA(root, NULL);
    if (_snprintf_s(base, sizeof(base), _TRUNCATE,
                    "%s\\shader-cache-v1", root) < 0)
        return 0;
    CreateDirectoryA(base, NULL);
    return _snprintf_s(path, MAX_PATH, _TRUNCATE,
                       "%s\\%s-%016llX.bin", base, kind,
                       (unsigned long long)source_hash) >= 0;
}

uint64_t d3d8_shader_source_hash(const void *source, size_t source_size,
                                 const char *profile)
{
    const unsigned char *bytes = (const unsigned char *)source;
    uint64_t hash = UINT64_C(1469598103934665603);
    size_t i;
    for (i = 0; i < source_size; ++i) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    while (*profile != '\0') {
        hash ^= (unsigned char)*profile++;
        hash *= UINT64_C(1099511628211);
    }
    hash ^= SHADER_CACHE_VERSION;
    return hash * UINT64_C(1099511628211);
}

HRESULT d3d8_shader_cache_load(const char *kind, uint64_t source_hash,
                               ID3DBlob **blob)
{
    ShaderCacheHeader header;
    char path[MAX_PATH];
    FILE *file = NULL;
    HRESULT hr;
    *blob = NULL;
    if (!shader_cache_path(path, kind, source_hash) ||
        fopen_s(&file, path, "rb") != 0)
        return S_FALSE;
    if (fread(&header, sizeof(header), 1, file) != 1 ||
        header.magic != SHADER_CACHE_MAGIC ||
        header.version != SHADER_CACHE_VERSION ||
        header.source_hash != source_hash || header.bytecode_size == 0 ||
        header.bytecode_size > 16u * 1024u * 1024u) {
        fclose(file);
        DeleteFileA(path);
        return S_FALSE;
    }
    hr = D3DCreateBlob((SIZE_T)header.bytecode_size, blob);
    if (FAILED(hr)) {
        fclose(file);
        return hr;
    }
    if (fread(ID3D10Blob_GetBufferPointer(*blob),
              (size_t)header.bytecode_size, 1, file) != 1 ||
        fgetc(file) != EOF) {
        ID3D10Blob_Release(*blob);
        *blob = NULL;
        fclose(file);
        DeleteFileA(path);
        return S_FALSE;
    }
    fclose(file);
    return S_OK;
}

void d3d8_shader_cache_store(const char *kind, uint64_t source_hash,
                             ID3DBlob *blob)
{
    ShaderCacheHeader header;
    char path[MAX_PATH];
    char temporary[MAX_PATH];
    FILE *file = NULL;
    size_t size;
    if (!shader_cache_path(path, kind, source_hash))
        return;
    size = ID3D10Blob_GetBufferSize(blob);
    if (size == 0 ||
        _snprintf_s(temporary, sizeof(temporary), _TRUNCATE,
                    "%s.tmp-%lu", path,
                    (unsigned long)GetCurrentProcessId()) < 0)
        return;
    header.magic = SHADER_CACHE_MAGIC;
    header.version = SHADER_CACHE_VERSION;
    header.source_hash = source_hash;
    header.bytecode_size = size;
    if (fopen_s(&file, temporary, "wb") != 0)
        return;
    if (fwrite(&header, sizeof(header), 1, file) != 1 ||
        fwrite(ID3D10Blob_GetBufferPointer(blob), size, 1, file) != 1 ||
        fflush(file) != 0) {
        fclose(file);
        DeleteFileA(temporary);
        return;
    }
    fclose(file);
    if (!MoveFileExA(temporary, path,
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        DeleteFileA(temporary);
}

void d3d8_shader_cache_invalidate(const char *kind, uint64_t source_hash)
{
    char path[MAX_PATH];
    if (shader_cache_path(path, kind, source_hash))
        DeleteFileA(path);
}
#endif
