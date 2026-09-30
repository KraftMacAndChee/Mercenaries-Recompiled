#ifndef XBOX_D3D8_COMPILER_H
#define XBOX_D3D8_COMPILER_H
#include <d3dcompiler.h>
HRESULT d3d8_compile_shader(LPCVOID source, SIZE_T size, LPCSTR name,
    const D3D_SHADER_MACRO *defines, ID3DInclude *include, LPCSTR entry,
    LPCSTR target, UINT flags1, UINT flags2, ID3DBlob **code, ID3DBlob **errors);
#endif
