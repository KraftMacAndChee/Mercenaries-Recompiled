"""Execute retail-disc bouncer Lua with public Lua and the production queue."""
from pathlib import Path
import re, subprocess, tempfile, unittest, sys, hashlib, json
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT))
from tools.diagnostics.inspect_retail_script import read_script
class VoiceFailureOrdering(unittest.TestCase):
 def test_queue_and_original_bouncers(self):
  lua=ROOT/'third_party/lua-5.0.3'
  upstream=json.loads((lua/'UPSTREAM.json').read_text())
  for name,digest in upstream['files'].items():
   self.assertEqual(hashlib.sha256((lua/name).read_bytes()).hexdigest(),digest,name)
  original=read_script(ROOT/'game_files/mercenaries-retail/DATAxbox/assets.dsk','bouncer')
  start=original.index('function BouncerUsed(ThisActor)')
  end=original.index('function BouncerBribeRequested(ThisActor)',start)
  entry=original[start:end]
  self.assertTrue(entry.rstrip().endswith('end'))
  fixture="TRUE=true FALSE=false\nbouncers={}\nfunction Actor_SetInvincible(a,v) end\nfunction Actor_IsDormant(a) return false end\nfunction Player_DisableControl() disabled=true return 7 end\nfunction Player_EnableControl(v) assert(v==7) disabled=false end\nfunction DetermineBouncerFaction(a) return a,false end\nfunction Utility_WriteStringToScribbleMemory(k,v) factionStored=v end\nfunction Utility_ReadStringFromScribbleMemory(k) return 'sw' end\nfunction Utility_ReadNumberFromScribbleMemory(k) return 1 end\nfunction util_MissionSeqNumToMissionAssetNum(a,b,c) return 1 end\nfunction Global_SetValue(k,v) end\nfunction Audio_StopMusic() end\nfunction util_GetRandomIntInRange(a,b) return line end\nfunction Audio_PlayVoiceoverCB(cue,cb,actor)\n assert(PreBriefing==nil) -- callback must wait for the script to finish\n pending=cb welcome=cue return 99,0\nend\nfunction Cinematic_FadeOut(t,cb) assert(t==0.5) fade=cb end\nfunction Briefing_Entry() entries=entries+1 end\nfunction EnableBouncers(v) end\nfunction SpawnBouncers(v) end\nfunction Boundary_Enable(k,v) end\nfunction Utility_OpenBriefingDataSets() end\nfunction Utility_ReadLevelFile(k) loaded[k]=true end\nfunction Briefing_SetAdditionalSoundBank(k) bank=k end\nfunction Briefing_Init(a,b) initialized=true end\nfunction Debug_PrintMemoryStatistics() end\n"
  fixture=fixture[:fixture.index('function Audio_PlayVoiceoverCB')]+fixture[fixture.index('function Cinematic_FadeOut'):]
  # A completed/fading welcome invokes the original EnterBriefing; no synthetic
  # teleport or forced controls are supplied by this fixture.
  checks=r"""
local factions={'sk','mafia','allies','china'}
local prefixes={'sks.bousks0','mso.boumso0','aso.bouaso0','cso.boucso0'}
for f=1,4 do for n=1,5 do
 line=n PreBriefing=nil EnterBriefing=nil fade=nil entries=0 loaded={} initialized=false
 InvokeBouncer(factions[f])
 assert(disabled and entries==0)
 if Fixed then
  assert(fade=='EnterBriefing')
  getfenv()[fade]()
  assert(not disabled and entries==1 and initialized)
  assert(loaded['template_hq-'..factions[f]])
 else
  assert(fade==nil) -- old synchronous failure loses this continuation
 end
end end
"""
  c=r'''
#include <assert.h>
#include <stdio.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "voice_callback_queue.h"
static lua_State *L;
static unsigned mode, delivered, missed;
static void dispatch(uint32_t owner,uint32_t vm,const char *name) {
 assert(owner==1 && vm==1);
 lua_getglobal(L,name);
 if(!lua_isfunction(L,-1)){lua_pop(L,1);missed++;return;}
 uint64_t mark=voice_call_begin();
 int error=lua_pcall(L,0,0,0);assert(!error);
 delivered++;voice_call_end(mark,1,dispatch);
}
static int vo(lua_State *state) {
 const char *name=lua_tostring(state,2);assert(name);
 if(mode) assert(voice_defer(1,1,name)); else dispatch(1,1,name);
 lua_pushnumber(state,0);lua_pushnumber(state,0);return 2;
}
static int invoke(lua_State *state) {
 const char *f=lua_tostring(state,1);
 lua_getglobal(state,"BouncerUsed");lua_pushstring(state,f);
 uint64_t mark=voice_call_begin();int error=lua_pcall(state,1,0,0);
 if(error)fprintf(stderr,"%s\n",lua_tostring(state,-1));
 assert(!error);voice_call_end(mark,1,dispatch);return 0;
}
static unsigned count;
static void counter(uint32_t owner,uint32_t vm,const char *name) {
 assert(owner==1 && vm==2);count++;
 if(!strcmp(name,"chain")){
  uint64_t mark=voice_call_begin();assert(voice_defer(1,2,"next"));
  voice_call_end(mark,1,counter);assert(count==1); /* no recursive drain */
 }
}
int main(int argc,char **argv) {
 /* Nested calls cannot invoke continuations while the outer caller is active. */
 uint64_t a=voice_call_begin();assert(voice_defer(1,2,"first"));
 uint64_t b=voice_call_begin();assert(voice_defer(1,2,"failed"));
 voice_call_end(b,0,counter);assert(!count);
 voice_call_end(a,1,counter);assert(count==1 && !voice_head);
 /* An error in the outer caller cancels all of its deferred work. */
 a=voice_call_begin();b=voice_call_begin();assert(voice_defer(1,2,"cancel"));
 voice_call_end(b,1,counter);voice_call_end(a,0,counter);assert(count==1);
 /* Script teardown cancels even if a later state reuses its address. */
 a=voice_call_begin();assert(voice_defer(1,2,"old"));voice_discard(1,0,0);
 voice_call_end(a,1,counter);assert(count==1);
 /* Queue owns names; cancellation retains valid entries in order. */
 count=0;a=voice_call_begin();char name[]="chain";
 assert(voice_defer(1,2,name));name[0]='!';
 assert(voice_defer(3,4,"removed"));voice_discard(3,0,0);
 voice_call_end(a,1,counter);assert(count==2 && !voice_head && !voice_depth);
 for(mode=0;mode<2;mode++){
  L=lua_open();luaopen_base(L);luaopen_table(L);luaopen_string(L);lua_settop(L,0);
  lua_register(L,"Audio_PlayVoiceoverCB",vo);lua_register(L,"InvokeBouncer",invoke);
  lua_pushboolean(L,mode);lua_setglobal(L,"Fixed");
  if(lua_dofile(L,argv[1])){fprintf(stderr,"%s\n",lua_tostring(L,-1));return 2;}
  lua_close(L);
 }
 assert(delivered==20 && missed==20);
 puts("20 retail-script HQ cases reproduce old silent-voice stall; all 20 complete after deferral. Nested/error/teardown/reentry cases pass.");
 return 0;
}
'''
  with tempfile.TemporaryDirectory(prefix='merc-hq-order-') as temp:
   out=Path(temp);(out/'test.c').write_text(c);(out/'test.lua').write_text(fixture+'\n'+entry+'\n'+checks)
   command=['C:/MinGW/bin/gcc.exe','-O1','-w','-I'+str(ROOT/'ports/mercenaries/src'),'-I'+str(lua/'include'),'-I'+str(lua/'src'),str(out/'test.c'),*map(str,(lua/'src').glob('*.c')),*map(str,(lua/'src/lib').glob('*.c')),'-o',str(out/'test.exe')]
   subprocess.run(command,check=True)
   subprocess.run([str(out/'test.exe'),str(out/'test.lua')],check=True)
if __name__=='__main__':unittest.main()
