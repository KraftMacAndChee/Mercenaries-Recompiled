"""Exercise stale NG+ card acknowledgements through the real deck refresh and PDA."""
from pathlib import Path
import re, runpy, subprocess, tempfile
ROOT=Path(__file__).resolve().parents[2]
# Reuse the retail-memory/ABI harness; this also reruns its dossier regression.
n=runpy.run_path(str(ROOT/'tools/recomp/test_datapod_intel_native.py'))['main']()
s=(ROOT/'ports/mercenaries/src/recomp/gen/recomp_0003.c').read_text()
update=re.search(r'void sub_000C9700\(void\)\n\{.*?\n\}',s,re.S)[0]
pre=n['prelude']
pre=pre.replace('static float saved[4];static unsigned suit;', r'''static float saved[4];static unsigned suit;
static unsigned states[52],shown[52],writes;
static char keys[400][160];static unsigned key_count;
static const char *suits[]={"clubs","diamonds","hearts","spades"};
static int card_key(const char *key,const char *suffix){
 for(int i=0;i<52;i++){char k[80];sprintf(k,"%s%u%s",suits[i/13],i%13+2,suffix);if(!strcmp(k,key))return i;}return -1;
}
void sub_000C85F0(void){eax=25000;esp+=12;}
void sub_00121BD0(void){
 unsigned h=MEM32(esp+4);int i=h>=4?card_key(keys[h-4],"status"):-1;
 eax=i>=0&&states[i]==3?0x510000:i>=0&&states[i]==2?0x510020:0;esp+=8;
}
''')
pre=pre.replace('void sub_000C9700(void){esp+=8;}', 'void sub_000C9700(void);')
a=pre.index('void sub_000C8580');b=pre.index('void sub_002370B8',a)
pre=pre[:a]+'''void sub_000C8580(void){strcpy((char*)memory+MEM32(esp+8),suits[MEM32(esp+4)]);esp+=12;}
'''+pre[b:]
a=pre.index('void sub_001F29F0');b=pre.index('void sub_00121DA0',a)
pre=pre[:a]+r'''void sub_001F29F0(void){
 const char *p=MEM32(esp+4)?(char*)memory+MEM32(esp+4):"";eax=0;
 if(!strcmp(p,"captured"))eax=0x48705ebf;else if(!strcmp(p,"killed"))eax=0x2a95f870;
 else if(!strncmp(p,"displayedIntel",14)){
  eax=strstr(p,"Diamonds")?1:strstr(p,"Hearts")?2:strstr(p,"Spades")?3:0;
 }else if(*p){unsigned i;for(i=0;i<key_count;i++)if(!strcmp(keys[i],p))break;
  if(i==key_count){assert(key_count<400);strcpy(keys[key_count++],p);}eax=4+i;
 }esp+=4;
}
void sub_00121E00(void){unsigned h=MEM32(esp+4);int i=h>=4?card_key(keys[h-4],"statusdisplayed"):-1;assert(i>=0);shown[i]=MEM32(esp+8);writes++;esp+=12;}
'''+pre[b:]
main=n['harness'][:n['harness'].index('int main(')]+r'''
static void refresh(void){prep();ecx=VM;MEMF(esp+4)=0;sub_000C9700();check(4);}
int main(int argc,char **argv){g_xbox_mem_offset=(ptrdiff_t)memory;FILE*f=fopen(argv[1],"rb");assert(f);assert(fread(memory,1,sizeof(memory),f)==sizeof(memory));fclose(f);
strcpy((char*)memory+0x510000,"captured");strcpy((char*)memory+0x510020,"killed");
for(unsigned outcome=2;outcome<=3;outcome++)for(unsigned target=0;target<4;target++)for(unsigned fps=30;fps<=120;fps*=2){
 memset(memory+MODE,0,0x20000);memset(states,0,sizeof(states));memset(shown,0,sizeof(shown));memset(saved,0,sizeof(saved));MEM32(MODE+0x24)=VM;
 unsigned prior=target*13,missing=prior+1;states[prior]=3;shown[prior]=1;saved[target]=2;
 refresh();MEM8(VM+0xDB48+prior*40)=1;
 // Advancing to the final suit must not acknowledge a still-free number card.
 suit=3;enter();assert(!MEM8(VM+0xDB48+missing*40));leave();
 // Reproduce a saved stale flag from an older build, then refresh free cards.
 for(unsigned i=0;i<52;i++){shown[i]=1;MEM8(VM+0xDB48+i*40)=1;}
 writes=0;refresh();assert(!shown[missing]&&!MEM8(VM+0xDB48+missing*40));
 assert(shown[prior]&&MEM8(VM+0xDB48+prior*40));assert(writes==51);
 writes=0;refresh();assert(!writes);
 // In NG+, the previously skipped 3 is captured/killed for the first time.
 states[missing]=outcome;refresh();suit=target;enter();
 assert(MEMF(MODE+0x3B8)==2);
 for(unsigned frame=0;frame<fps*6;frame++)tick(1.f/fps);
 assert(shown[missing]&&MEM8(VM+0xDB48+missing*40));assert(saved[target]==5);leave();
 // Reload acknowledgements: no duplicate award for either captured card.
 for(unsigned i=0;i<52;i++)MEM8(VM+0xDB48+i*40)=shown[i];
 refresh();enter();for(unsigned frame=0;frame<fps*6;frame++)tick(1.f/fps);
 assert(saved[target]==5);leave();
}
puts("PASS: NG+ skipped number card awards once; chapter advance, stale-save repair, capture/kill, 4 suits, 30/60/120 FPS and reload/ABI");}
'''
with tempfile.TemporaryDirectory(prefix='merc-ngplus-intel-') as folder:
 d=Path(folder);(d/'memory.bin').write_bytes(n['mem']);(d/'test.c').write_text(pre+n['bodies']+update+main)
 subprocess.run(['C:/MinGW/bin/gcc.exe','-O1','-fno-strict-aliasing',str(d/'test.c'),'-o',str(d/'test.exe')],check=True)
 subprocess.run([str(d/'test.exe'),str(d/'memory.bin')],check=True)
 # Both independent old behaviors must fail this regression.
 mutations = {
  'chapter': n['bodies'].replace('MEM32(ecx + ebp + 0xDAFC) != 1u', '1u'),
  'refresh': n['bodies'],
 }
 for name,bodies in mutations.items():
  updater=update
  if name=='refresh':
   updater=re.sub(r'    /\* Repair acknowledgements.*?    SET_LO8\(eax, MEM8\(esi \+ 4\)\);',
                  '    SET_LO8(eax, MEM8(esi + 4));',update,flags=re.S)
   assert updater!=update
  (d/'old.c').write_text(pre+bodies+updater+main)
  subprocess.run(['C:/MinGW/bin/gcc.exe','-O1','-fno-strict-aliasing',str(d/'old.c'),'-o',str(d/'old.exe')],check=True)
  result=subprocess.run([str(d/'old.exe'),str(d/'memory.bin')],capture_output=True)
  assert result.returncode, f'{name} mutation escaped the regression'
 print('PASS: previous-chapter and stale-refresh mutations both reproduce the failure')

