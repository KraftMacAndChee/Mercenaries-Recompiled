
#undef NDEBUG
#include <assert.h>
static ID3DBlob *test_compile(const char *s,const char *profile){
 ID3DBlob *b=NULL,*e=NULL;HRESULT hr=D3DCompile(s,strlen(s),"subpixel",NULL,NULL,"main",profile,D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&b,&e);
 if(FAILED(hr)&&e)fprintf(stderr,"%s",(char*)ID3D10Blob_GetBufferPointer(e));assert(SUCCEEDED(hr));if(e)ID3D10Blob_Release(e);return b;
}
static ID3D11Buffer *test_buffer(ID3D11Device *d,const void *data,unsigned n,unsigned bind){
 D3D11_BUFFER_DESC b={0};D3D11_SUBRESOURCE_DATA s={0};ID3D11Buffer *out=NULL;b.ByteWidth=n;b.BindFlags=bind;b.Usage=D3D11_USAGE_DEFAULT;s.pSysMem=data;
 assert(SUCCEEDED(ID3D11Device_CreateBuffer(d,&b,data?&s:NULL,&out)));return out;
}
int main(void){
 NV2AVshProgram p={0};char hlsl[32768];p.length=1;p.inputs_read=1;p.insns[0].mac_op=NV2A_VSH_MAC_MOV;p.insns[0].mac_src[0].reg_type=NV2A_VSH_REG_INPUT;p.insns[0].mac_src[0].swizzle=(NV2AVshSwizzle){0,1,2,3};p.insns[0].mac_dst.temp_reg=-1;p.insns[0].mac_dst.output_reg=NV2A_VSH_OUT_POS;p.insns[0].mac_dst.output_write_mask=15;
 g_vsh_input_formats[0]=DXGI_FORMAT_R32G32B32A32_FLOAT;g_vsh_input_components[0]=4;
 assert(d3d8_vsh_generate_hlsl(&p,hlsl,sizeof(hlsl))>0);
 ID3D11Device *d=NULL;ID3D11DeviceContext *c=NULL;assert(SUCCEEDED(D3D11CreateDevice(NULL,D3D_DRIVER_TYPE_WARP,NULL,0,NULL,0,D3D11_SDK_VERSION,&d,NULL,&c)));
 ID3D11VertexShader *vs[2];ID3D11InputLayout *layout=NULL;ID3DBlob *b;
 for(unsigned i=0;i<2;i++){
  if(i){char *rounding=strstr(hlsl,"if (false || oPos.w == 1.0)");assert(rounding);memcpy(rounding+4,"true ",5);}
  b=test_compile(hlsl,"vs_5_0");assert(SUCCEEDED(ID3D11Device_CreateVertexShader(d,ID3D10Blob_GetBufferPointer(b),ID3D10Blob_GetBufferSize(b),NULL,&vs[i])));
  if(!i){D3D11_INPUT_ELEMENT_DESC e={"ATTR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0};assert(SUCCEEDED(ID3D11Device_CreateInputLayout(d,&e,1,ID3D10Blob_GetBufferPointer(b),ID3D10Blob_GetBufferSize(b),&layout)));}ID3D10Blob_Release(b);
 }
 ID3D11PixelShader *ps[2]={NULL,NULL};
 const char *pixel_source[2]={
  "cbuffer C:register(b3){float4 color;}float4 main():SV_TARGET{return color;}",
  "cbuffer C:register(b3){float4 color;}float4 main(float4 pos:SV_POSITION,float4 gd:TEXCOORD6,out float depth:SV_Depth):SV_TARGET{float z=1.0+gd.x/gd.y;if(!isfinite(z)||z<0.0||z>1.0)z=pos.z;depth=saturate(z);return color;}"
 };
 for(unsigned i=0;i<2;i++){b=test_compile(pixel_source[i],"ps_5_0");assert(SUCCEEDED(ID3D11Device_CreatePixelShader(d,ID3D10Blob_GetBufferPointer(b),ID3D10Blob_GetBufferSize(b),NULL,&ps[i])));ID3D10Blob_Release(b);}
 float host[68]={640,480,16777215,0};ID3D11Buffer *hostcb=test_buffer(d,host,sizeof(host),D3D11_BIND_CONSTANT_BUFFER),*colors=test_buffer(d,NULL,16,D3D11_BIND_CONSTANT_BUFFER),*verts=test_buffer(d,NULL,64,D3D11_BIND_VERTEX_BUFFER);
 D3D11_DEPTH_STENCIL_DESC dd={0};dd.DepthEnable=TRUE;dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;dd.DepthFunc=D3D11_COMPARISON_LESS_EQUAL;ID3D11DepthStencilState *ds;assert(SUCCEEDED(ID3D11Device_CreateDepthStencilState(d,&dd,&ds)));
 D3D11_RASTERIZER_DESC rd={0};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;ID3D11RasterizerState *rs;assert(SUCCEEDED(ID3D11Device_CreateRasterizerState(d,&rd,&rs)));ID3D11DeviceContext_RSSetState(c,rs);ID3D11DeviceContext_OMSetDepthStencilState(c,ds,0);
 ID3D11DeviceContext_IASetInputLayout(c,layout);ID3D11DeviceContext_IASetPrimitiveTopology(c,D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);unsigned stride=16,offset=0;ID3D11DeviceContext_IASetVertexBuffers(c,0,1,&verts,&stride,&offset);ID3D11DeviceContext_VSSetConstantBuffers(c,2,1,&hostcb);ID3D11DeviceContext_PSSetConstantBuffers(c,3,1,&colors);ID3D11DeviceContext_PSSetShader(c,ps[0],NULL,0);
 unsigned old_missing=0,new_missing=0,cases=0;
 for(unsigned depth_mode=0;depth_mode<2;depth_mode++){
 ID3D11DeviceContext_PSSetShader(c,ps[depth_mode],NULL,0);
 unsigned mode_missing=0,mode_old_missing=0;
 for(unsigned scale=1;scale<=2;scale++)for(unsigned aa=1;aa<=2;aa++){
  unsigned w=640*scale*aa,h=480*scale;D3D11_TEXTURE2D_DESC td={0};td.Width=w;td.Height=h;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.BindFlags=D3D11_BIND_RENDER_TARGET;ID3D11Texture2D *rt,*z,*read;ID3D11RenderTargetView *rtv;ID3D11DepthStencilView *dsv;
  assert(SUCCEEDED(ID3D11Device_CreateTexture2D(d,&td,NULL,&rt)));assert(SUCCEEDED(ID3D11Device_CreateRenderTargetView(d,(ID3D11Resource*)rt,NULL,&rtv)));td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;assert(SUCCEEDED(ID3D11Device_CreateTexture2D(d,&td,NULL,&read)));td.BindFlags=D3D11_BIND_DEPTH_STENCIL;td.Usage=D3D11_USAGE_DEFAULT;td.CPUAccessFlags=0;td.Format=DXGI_FORMAT_D24_UNORM_S8_UINT;assert(SUCCEEDED(ID3D11Device_CreateTexture2D(d,&td,NULL,&z)));assert(SUCCEEDED(ID3D11Device_CreateDepthStencilView(d,(ID3D11Resource*)z,NULL,&dsv)));
  ID3D11DeviceContext_OMSetRenderTargets(c,1,&rtv,dsv);D3D11_VIEWPORT vp={0};vp.Width=(float)w;vp.Height=(float)h;vp.MaxDepth=1;ID3D11DeviceContext_RSSetViewports(c,1,&vp);
  unsigned char *masks[4];for(unsigned m=0;m<4;m++)masks[m]=calloc(w*h,1);
  for(int ui=0;ui<3;ui++)for(int degrees=-70;degrees<=70;degrees+=10)for(int distance=0;distance<=6;distance+=3){
   float co=(float)cos(degrees*3.141592653589793/180),si=(float)sin(degrees*3.141592653589793/180);
   for(unsigned mode=0;mode<4;mode++){
    ID3D11DeviceContext_VSSetShader(c,vs[mode&1],NULL,0);float black[4]={0,0,0,1};ID3D11DeviceContext_ClearRenderTargetView(c,rtv,black);ID3D11DeviceContext_ClearDepthStencilView(c,dsv,D3D11_CLEAR_DEPTH,1,0);
    for(unsigned wall=0;wall<2;wall++){
     if(wall&&(mode>=2||ui))continue;float v[4][4];float left=wall?-2.0f:-.18f,right=wall?2.0f:.08f,bottom=wall?0:1.411f,top=wall?3:1.527f,zpos=wall?4.262f:4.2608f;
     for(unsigned j=0;j<4;j++){
      float x=(j&1)?right:left,y=(j&2)?top:bottom;
      if(ui){v[j][0]=((j&1)?639.5f:-.5f)+(degrees+70)*.0001f;v[j][1]=((j&2)?479.5f:-.5f)+distance*.0001f;if(ui==2){v[j][0]=((j&1)?420.141f:100.127f)+degrees/13.0f;v[j][1]=((j&2)?300.085f:200.048f)+distance*.13f;}v[j][2]=0;v[j][3]=1;}
      else {float cx=co*(x+.05f)-si*(zpos-4.2608f),cz=si*(x+.05f)+co*(zpos-4.2608f)+7.2608f+distance;v[j][0]=320.03125f+426.0f*cx/cz;v[j][1]=240.03125f-603.0f*(y-1.7f)/cz;v[j][2]=16788406.0f-335768.125f/cz;v[j][3]=cz;}
     }
     float color[4]={wall?.15f:1,wall?.15f:1,wall?.15f:0,1};ID3D11DeviceContext_UpdateSubresource(c,(ID3D11Resource*)verts,0,NULL,v,0,0);ID3D11DeviceContext_UpdateSubresource(c,(ID3D11Resource*)colors,0,NULL,color,0,0);ID3D11DeviceContext_Draw(c,4,0);
    }
    ID3D11DeviceContext_CopyResource(c,(ID3D11Resource*)read,(ID3D11Resource*)rt);D3D11_MAPPED_SUBRESOURCE map;assert(SUCCEEDED(ID3D11DeviceContext_Map(c,(ID3D11Resource*)read,0,D3D11_MAP_READ,0,&map)));for(unsigned y=0;y<h;y++){unsigned char *row=(unsigned char*)map.pData+y*map.RowPitch;for(unsigned x=0;x<w;x++)masks[mode][y*w+x]=row[4*x]==255&&row[4*x+1]==255;}ID3D11DeviceContext_Unmap(c,(ID3D11Resource*)read,0);
   }
   if(ui)assert(memcmp(masks[0],masks[1],w*h)==0);
   else {unsigned missing[2]={0};for(unsigned i=0;i<w*h;i++){missing[0]+=masks[2][i]&&!masks[0][i];missing[1]+=masks[3][i]&&!masks[1][i];}new_missing+=missing[0];old_missing+=missing[1];mode_missing+=missing[0];mode_old_missing+=missing[1];cases++;if(missing[0])printf("remaining scale=%u angle=%d distance=%d missing=%u old=%u\n",scale,degrees,distance,missing[0],missing[1]);}
  }
  for(unsigned m=0;m<4;m++)free(masks[m]);ID3D11DeviceContext_OMSetRenderTargets(c,0,NULL,NULL);ID3D11RenderTargetView_Release(rtv);ID3D11DepthStencilView_Release(dsv);ID3D11Texture2D_Release(rt);ID3D11Texture2D_Release(z);ID3D11Texture2D_Release(read);
 }
 printf("depth_mode=%u missing=%u old_missing=%u\n",depth_mode,mode_missing,mode_old_missing);assert(mode_missing==0);assert(mode_old_missing>0);
 }
 printf("cases=%u original_missing=%u improved_missing=%u; UI coverage identical\n",cases,old_missing,new_missing);assert(old_missing>0);assert(new_missing==0);
 ID3D11DeviceContext_ClearState(c);for(unsigned i=0;i<2;i++)ID3D11PixelShader_Release(ps[i]);ID3D11DeviceContext_Release(c);ID3D11Device_Release(d);return 0;
}
