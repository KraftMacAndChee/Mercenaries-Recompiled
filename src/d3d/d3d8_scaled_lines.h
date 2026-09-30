#ifndef D3D8_SCALED_LINES_H
#define D3D8_SCALED_LINES_H
#ifndef COBJMACROS
#define COBJMACROS
#endif
#include <d3d11.h>
typedef struct D3D8ScaledLineScope {
    ID3D11GeometryShader *shader;
    ID3D11Buffer *constants;
    ID3D11RasterizerState *rasterizer;
    BOOL active;
} D3D8ScaledLineScope;
void d3d8_scaled_lines_begin(ID3D11Device *device, ID3D11DeviceContext *context,
                            float width, D3D8ScaledLineScope *scope);
void d3d8_scaled_lines_end(ID3D11DeviceContext *context, D3D8ScaledLineScope *scope);
void d3d8_scaled_lines_shutdown(void);
#endif
