#ifndef D3D8_SHADER_CACHE_H
#define D3D8_SHADER_CACHE_H

#if defined(_WIN32)
#ifndef COBJMACROS
#define COBJMACROS
#endif
#include <d3dcompiler.h>
#include <stddef.h>
#include <stdint.h>

uint64_t d3d8_shader_source_hash(const void *source, size_t source_size,
                                 const char *profile);
HRESULT d3d8_shader_cache_load(const char *kind, uint64_t source_hash,
                               ID3DBlob **blob);
void d3d8_shader_cache_store(const char *kind, uint64_t source_hash,
                             ID3DBlob *blob);
void d3d8_shader_cache_invalidate(const char *kind, uint64_t source_hash);
#endif
#endif
