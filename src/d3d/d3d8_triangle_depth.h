#ifndef D3D8_TRIANGLE_DEPTH_H
#define D3D8_TRIANGLE_DEPTH_H
#ifndef COBJMACROS
#define COBJMACROS
#endif
#include <d3d11.h>
typedef struct D3D8TriangleDepthScope {
    ID3D11GeometryShader *shader;
    ID3D11Buffer *constants;
    BOOL active;
} D3D8TriangleDepthScope;
void d3d8_triangle_depth_begin(ID3D11Device *, ID3D11DeviceContext *, float, float, D3D8TriangleDepthScope *);
void d3d8_triangle_depth_end(ID3D11DeviceContext *, D3D8TriangleDepthScope *);
void d3d8_triangle_depth_shutdown(void);
#endif
