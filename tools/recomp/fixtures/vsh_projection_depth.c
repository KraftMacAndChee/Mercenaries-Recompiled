
#undef NDEBUG
#include <assert.h>
static NV2AVshSrcOperand scalar(int bank, int reg, int component) {
 NV2AVshSrcOperand r = {0};r.reg_type=bank;r.reg_index=reg;
 r.swizzle.x=r.swizzle.y=r.swizzle.z=r.swizzle.w=(uint8_t)component;return r;
}
static void dst(NV2AVshDstOperand *d,int reg,unsigned mask) {
 d->temp_reg=reg;d->write_mask=(uint8_t)mask;d->output_reg=NV2A_VSH_OUT_NONE;d->output_write_mask=0;
}
static NV2AVshProgram projection(int camera,int product,int accum,int inverse) {
 NV2AVshProgram p={0};p.length=5;p.inputs_read=1;
 for(int j=0;j<p.length;j++){dst(&p.insns[j].mac_dst,-1,0);dst(&p.insns[j].ilu_dst,-1,0);}
 p.insns[0].mac_op=NV2A_VSH_MAC_MOV;p.insns[0].mac_src[0]=scalar(NV2A_VSH_REG_INPUT,0,2);dst(&p.insns[0].mac_dst,camera,2);
 p.insns[0].ilu_op=NV2A_VSH_ILU_MOV;p.insns[0].ilu_src=scalar(NV2A_VSH_REG_CONST,17,2);dst(&p.insns[0].ilu_dst,camera,1);
 p.insns[1].ilu_op=NV2A_VSH_ILU_RCC;p.insns[1].ilu_src=scalar(NV2A_VSH_REG_TEMP,camera,2);dst(&p.insns[1].ilu_dst,inverse,8);
 p.insns[2].mac_op=NV2A_VSH_MAC_MUL;p.insns[2].mac_src[0]=scalar(NV2A_VSH_REG_TEMP,camera,2);p.insns[2].mac_src[1]=scalar(NV2A_VSH_REG_CONST,26,2);dst(&p.insns[2].mac_dst,product,2);
 p.insns[3].mac_op=NV2A_VSH_MAC_MAD;p.insns[3].mac_src[0]=scalar(NV2A_VSH_REG_TEMP,camera,3);p.insns[3].mac_src[1]=scalar(NV2A_VSH_REG_CONST,27,2);p.insns[3].mac_src[2]=scalar(NV2A_VSH_REG_TEMP,product,2);dst(&p.insns[3].mac_dst,accum,2);
 p.insns[4].mac_op=NV2A_VSH_MAC_MUL;p.insns[4].mac_src[0]=scalar(NV2A_VSH_REG_TEMP,accum,2);p.insns[4].mac_src[1]=scalar(NV2A_VSH_REG_TEMP,inverse,0);p.insns[4].mac_dst.output_reg=NV2A_VSH_OUT_POS;p.insns[4].mac_dst.output_write_mask=2;
 return p;
}
int main(void) {
 unsigned cases=0;
 for(int shift=0;shift<8;shift++) {
  NV2AVshProgram good=projection(shift,(shift+1)%12,(shift+2)%12,(shift+3)%12),p;
  assert(vsh_projection_depth(&good).instruction==3);cases++;
  p=good;p.insns[4].mac_src[0]=good.insns[4].mac_src[1];p.insns[4].mac_src[1]=good.insns[4].mac_src[0];assert(vsh_projection_depth(&p).instruction==3);cases++;
  p=good;p.insns[2].mac_src[0]=good.insns[2].mac_src[1];p.insns[2].mac_src[1]=good.insns[2].mac_src[0];assert(vsh_projection_depth(&p).instruction==3);cases++;
  p=good;p.insns[3].mac_src[0]=good.insns[3].mac_src[1];p.insns[3].mac_src[1]=good.insns[3].mac_src[0];assert(vsh_projection_depth(&p).instruction==3);cases++;
  p=good;p.insns[1].ilu_op=NV2A_VSH_ILU_RSQ;assert(vsh_projection_depth(&p).instruction<0);cases++;
  p=good;p.insns[2].mac_src[0].negate=1;assert(vsh_projection_depth(&p).instruction<0);cases++;
  p=good;p.insns[2].mac_src[1].rel_addr=1;assert(vsh_projection_depth(&p).instruction<0);cases++;
  p=good;p.insns[3].mac_src[0]=scalar(NV2A_VSH_REG_TEMP,shift,0);assert(vsh_projection_depth(&p).instruction==3);cases++;
  p=good;p.insns[2].ilu_op=NV2A_VSH_ILU_MOV;p.insns[2].ilu_src=scalar(NV2A_VSH_REG_CONST,17,0);dst(&p.insns[2].ilu_dst,shift,2);assert(vsh_projection_depth(&p).instruction<0);cases++;
  p=good;p.length=6;p.insns[5]=p.insns[4];p.insns[5].mac_op=NV2A_VSH_MAC_MOV;assert(vsh_projection_depth(&p).instruction<0);cases++;
  p=good;p.insns[4].mac_dst.output_reg=NV2A_VSH_OUT_NONE;dst(&p.insns[4].mac_dst,12,2);assert(vsh_projection_depth(&p).instruction==3);cases++;
  p=good;p.insns[2].mac_dst.temp_reg=shift;p.insns[2].mac_dst.write_mask=2;assert(vsh_projection_depth(&p).instruction<0);cases++;
  p=good;p.insns[3].ilu_op=NV2A_VSH_ILU_MOV;p.insns[3].ilu_src=scalar(NV2A_VSH_REG_CONST,17,0);dst(&p.insns[3].ilu_dst,shift,2);assert(vsh_projection_depth(&p).instruction==3);cases++;
  p=good;p.insns[4].ilu_op=NV2A_VSH_ILU_MOV;p.insns[4].ilu_src=scalar(NV2A_VSH_REG_CONST,17,0);dst(&p.insns[4].ilu_dst,shift,2);assert(vsh_projection_depth(&p).instruction==3);cases++;
  p=good;p.insns[4].ilu_op=NV2A_VSH_ILU_MOV;p.insns[4].ilu_src=scalar(NV2A_VSH_REG_CONST,17,0);p.insns[4].ilu_src.rel_addr=1;dst(&p.insns[4].ilu_dst,(shift+4)%12,8);assert(vsh_projection_depth(&p).instruction==3);cases++;
  for(int zc=0;zc<4;zc++)for(int wc=0;wc<4;wc++)if(zc!=wc) {
   p=good;dst(&p.insns[0].mac_dst,shift,8u>>zc);dst(&p.insns[0].ilu_dst,shift,8u>>wc);
   p.insns[1].ilu_src=scalar(NV2A_VSH_REG_TEMP,shift,zc);
   p.insns[2].mac_src[0]=scalar(NV2A_VSH_REG_TEMP,shift,zc);
   p.insns[3].mac_src[0]=scalar(NV2A_VSH_REG_TEMP,shift,wc);
   VshProjectionDepth d=vsh_projection_depth(&p);assert(d.instruction==3 && d.camera_z.component==zc && d.camera_w.component==wc);cases++;
  }
  char hlsl[32768];assert(d3d8_vsh_generate_hlsl(&good,hlsl,sizeof(hlsl))>0);assert(strstr(hlsl,"projectionDepthValid"));
  ID3DBlob *code=NULL,*error=NULL;HRESULT hr=D3DCompile(hlsl,strlen(hlsl),"projection",NULL,NULL,"main","vs_5_0",0,0,&code,&error);
  if(FAILED(hr)&&error)fprintf(stderr,"%s",(char*)ID3D10Blob_GetBufferPointer(error));assert(SUCCEEDED(hr));if(error)ID3D10Blob_Release(error);ID3D10Blob_Release(code);
 }
 printf("projection matching: %u positive/negative data-flow cases; 8 register-renamed shaders compiled\n",cases);
 return 0;
}
