"""Generate typed startup combiner configurations; no captured ABI or DXBC needed."""
import json,sys
from pathlib import Path

def bounded(v,lo,hi):
    if type(v) is not int or not lo<=v<=hi:raise ValueError(f'Invalid numeric field {v}')
    return str(v)
def fields(v,schema):
    if not isinstance(v,dict) or set(v)!=set(schema):raise ValueError('Unexpected state fields')
    return '{'+','.join('.'+k+'='+f(v[k]) for k,f in schema.items())+'}'
def array(f,n):
    def emit(v):
        if not isinstance(v,list) or len(v)!=n:raise ValueError('Wrong array length')
        return '{'+','.join(f(x) for x in v)+'}'
    return emit
B=lambda v:bounded(v,0,1)
R=lambda v:bounded(v,0,15)
M=lambda v:bounded(v,0,7)
def inp(v):return fields(v,{'reg':R,'alpha_rep':B,'mapping':M})
def output(v):return fields(v,{**{k:R for k in ['ab_dst','cd_dst','sum_dst']},**{k:B for k in ['ab_dot','cd_dot','mux_sum','ab_blue_to_alpha','cd_blue_to_alpha']},'output_map':M})
def stage(v):return fields(v,{'rgb_input':array(inp,4),'alpha_input':array(inp,4),'rgb_output':output,'alpha_output':output})
def state(v):return fields({'screen_depth_stage':0,**v},{'num_stages':lambda v:bounded(v,0,8),'stages':array(stage,8),'final_input':array(inp,7),'tex_mode':array(lambda v:bounded(v,0,31),4),'input_tex':array(lambda v:bounded(v,-1,3),4),'dot_map':array(M,4),'flags':lambda v:bounded(v,0,0xffff),'final_clamp_sum':B,'final_inv_v1':B,'final_inv_r0':B,'polygon_offset':B,'flare_grid':lambda v:bounded(v,0,3),'guest_depth':B,'screen_depth_stage':lambda v:bounded(v,0,4)})
def vertex(v):
    if not isinstance(v.get('microcode'),list) or not 4<=len(v['microcode'])<=544 or len(v['microcode'])%4:raise ValueError('Invalid vertex program length')
    values={k:x for k,x in v.items() if k!='hash'}
    values['length']=len(v['microcode'])//4
    return fields(values,{'microcode':array(lambda x:bounded(x,0,0xffffffff)+'u',len(v['microcode'])),'length':lambda v:bounded(v,1,136),'formats':array(lambda v:bounded(v,0,191),16),'offsets':array(lambda v:bounded(v,0,65535),16),'components':array(lambda v:bounded(v,0,4),16),'bgra_mask':lambda v:bounded(v,0,65535)})
def generate(data):
    if data.get('version')!=1 or not isinstance(data.get('states'),list) or not 1<=len(data['states'])<=96:raise ValueError('Unsupported catalogue')
    if not 1<=len(data.get('vertex_states',[]))<=48:raise ValueError('Invalid vertex warm-up count')
    return '/* Generated from shader-warmup.json. Do not edit. */\nstatic const NV2ACombinerState warmup_states[] = {\n'+',\n'.join(state(s) for s in data['states'])+'\n};\nstatic const NV2AVshWarmup warmup_vertex_states[] = {\n'+',\n'.join(vertex(v) for v in data['vertex_states'])+'\n};\n'
if __name__=='__main__':Path(sys.argv[2]).write_text(generate(json.loads(Path(sys.argv[1]).read_text(encoding='utf-8'))),encoding='utf-8')
