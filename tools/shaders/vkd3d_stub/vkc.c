/* minimal vkd3d-shader driver: vkc <src:fx|d3dbc> <tgt:asm|spirv|spirvtext|glsl> in out */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vkd3d_shader.h>
int main(int argc,char**argv){
  if(argc<5){fprintf(stderr,"usage\n");return 2;}
  FILE*f=fopen(argv[3],"rb"); if(!f) return 3; fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
  void*d=malloc(n); fread(d,1,n,f); fclose(f);
  struct vkd3d_shader_compile_info ci={0};
  ci.type=VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO;
  ci.source.code=d; ci.source.size=n;
  ci.source_type=!strcmp(argv[1],"fx")?VKD3D_SHADER_SOURCE_FX:VKD3D_SHADER_SOURCE_D3D_BYTECODE;
  if(!strcmp(argv[2],"asm")) ci.target_type=VKD3D_SHADER_TARGET_D3D_ASM;
  else if(!strcmp(argv[2],"spirv")) ci.target_type=VKD3D_SHADER_TARGET_SPIRV_BINARY;
  else if(!strcmp(argv[2],"spirvtext")) ci.target_type=VKD3D_SHADER_TARGET_SPIRV_TEXT;
  else ci.target_type=VKD3D_SHADER_TARGET_GLSL;
  struct vkd3d_shader_spirv_target_info sti={0};
  if(ci.target_type==VKD3D_SHADER_TARGET_SPIRV_BINARY||ci.target_type==VKD3D_SHADER_TARGET_SPIRV_TEXT){
    sti.type=VKD3D_SHADER_STRUCTURE_TYPE_SPIRV_TARGET_INFO; sti.environment=VKD3D_SHADER_SPIRV_ENVIRONMENT_VULKAN_1_0; ci.next=&sti;}
  ci.log_level=VKD3D_SHADER_LOG_WARNING;
  struct vkd3d_shader_code out={0}; char*msg=NULL;
  int r=vkd3d_shader_compile(&ci,&out,&msg);
  if(msg&&*msg) fprintf(stderr,"%s",msg);
  if(r<0){fprintf(stderr,"ERR %d\n",r);return 1;}
  f=fopen(argv[4],"wb"); fwrite(out.code,1,out.size,f); fclose(f); return 0;}
