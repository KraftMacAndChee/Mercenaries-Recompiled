"""Present CPU frames through the production scaler and hidden swap chains."""
from pathlib import Path
import os, subprocess, tempfile
import test_flip_swapchain_integration as flip
root=Path(__file__).resolve().parents[2]
s=(root/'src/d3d/d3d8_device.c').read_text(encoding='utf-8')
# Compile through the production compiler wrapper used by the scaler.
pre=flip.PRE+'\n#include "d3d8_compiler.h"\n'
pre=pre.replace('static void xbox_preview_log_event(', 'void xbox_preview_log_event(')
pre=pre.replace('ID3D11Texture2D *default_depth;', '''ID3D11Texture2D *cpu_frame_texture;ID3D11ShaderResourceView *cpu_frame_srv;
 ID3D11VertexShader *resolve_vs;ID3D11PixelShader *resolve_ps;ID3D11SamplerState *resolve_sampler;
 ID3D11Texture2D *default_depth;''')
pre+='\nstatic void (*g_host_overlay_callback)(void);\nstatic UINT g_presentation_aspect_width=4,g_presentation_aspect_height=3;\n'
block=flip.function(s,'static HRESULT preview_present(', '\n\nstatic IDirect3DDevice8 g_device;')
block+=flip.function(s,'static BOOL d3d8_flip_presentation_requested(', 'static void d3d8_init_default_states(')
block+=flip.function(s,'static BOOL d3d8_ensure_resolve_pipeline(', 'static BOOL d3d8_ensure_packed_color_depth_pipeline(')
block+=flip.function(s,'BOOL d3d8_CopyTextureToBackbuffer(', 'typedef struct D3D8PvideoConstants')
block+=flip.function(s,'void d3d8_UploadFrameX8R8G8B8(', 'static void d3d8_write_backbuffer_bmp(')
post=flip.POST.replace('ID3D11DeviceContext_ClearRenderTargetView(s->d3d11_context,s->default_rtv,color);', '''
 BYTE *pixels=calloc(640u*480u,4);assert(pixels);
 for(unsigned i=0;i<640u*480u;i++)pixels[i*4+(2-channel)]=255;
 g_presentation_aspect_width=s->width;g_presentation_aspect_height=s->height;
 d3d8_UploadFrameX8R8G8B8(pixels,640u*4u,640u,480u);free(pixels);''')
post=post.replace('i==1?1920:640,height=i==0?720:i==1?1080:480', 'i==1?1920:3840,height=i==0?720:i==1?1080:2160')
post=post.replace('ID3D11DeviceContext_ClearState(s->d3d11_context);', '''ID3D11DeviceContext_ClearState(s->d3d11_context);
 ID3D11ShaderResourceView_Release(s->cpu_frame_srv);ID3D11Texture2D_Release(s->cpu_frame_texture);
 ID3D11VertexShader_Release(s->resolve_vs);ID3D11PixelShader_Release(s->resolve_ps);ID3D11SamplerState_Release(s->resolve_sampler);''')
with tempfile.TemporaryDirectory(prefix='cpu-frame-present-') as td:
 p=Path(td)/'test.c';exe=p.with_suffix('.exe');p.write_text(pre+block+post)
 subprocess.run(['cl','/nologo','/O2','/TC',str(p),str(root/'src/d3d/d3d8_compiler.c'),'/I'+str(root/'src'),'/I'+str(root/'src/d3d'),'/Fo'+str(Path(td))+os.sep,'/Fe'+str(exe),'/link','d3d11.lib','d3dcompiler.lib','dxgi.lib','dxguid.lib','user32.lib'],check=True)
 for mode in ('legacy','flip','fallback'):
  e=os.environ.copy();e.pop('MERCENARIES_DISABLE_FLIP_PRESENT',None);e.pop('TEST_FAIL_FLIP_CREATION',None)
  if mode=='legacy':e['MERCENARIES_DISABLE_FLIP_PRESENT']='1'
  if mode=='fallback':e['TEST_FAIL_FLIP_CREATION']='1'
  subprocess.run([str(exe)],env=e,check=True,timeout=30)
