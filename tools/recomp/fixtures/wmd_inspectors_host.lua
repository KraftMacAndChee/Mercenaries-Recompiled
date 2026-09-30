
TRUE=true FALSE=false HERO='player0' VERB_DELIVER=9 VERB_DELIVER_HUM=5
HUD={STYLE={ATTACK=1,DEFEND=2},STATE={LIMIT=1}}
function noop() end
for _,name in ipairs({'Utility_LoadUtilityScript','Debug_EnableCallStack','mission_GenericInit','util_PrintDebugMsg','Objective_Remove','Actor_EnableScriptedUse','Ai_CommandClearAll','Ai_CommandDefault','Ai_EnableHornResponse','Ai_CommandQueue','Audio_PlayVoiceoverCB','Audio_PlayVoiceover','Audio_StopSound','Debug_Printf'}) do _G[name]=noop end
Debug_Assert=assert
function Utility_GetStringById(s) return s end
function Utility_GetActorAsInt(s)
 for i=1,4 do if s=='a1_inspector'..i then return 100+i end end
 return s
end
function actor_name(s)
 for i=1,4 do if s==100+i then return 'a1_inspector'..i end end
 return s
end
function Actor_IsAlive(s) s=actor_name(s) return not dead[s] and not removed[s] end
function Actor_Remove(s) removed[actor_name(s)]=true end
function HUD_RemoveTrayDisplay(h) trays[h]=nil end
function HUD_RemoveAllTrayDisplays() trays={} end
function Audio_PlayVoiceoverCB(cue, callback)
 if callback=='mission_MissionComplete' then completed=completed+1 end
end
Ai_Command=noop Ui_MessageBoxPrint=noop Objective_SetTargetActor=noop Objective_SetShortDescription=noop
util_ObjMsgPrint=noop TargetActorFollowingHero=noop
function event(...) nextEvent=nextEvent+1 events[nextEvent]=arg return nextEvent end
Event_ActorInLoadingZone=event Event_ActorIsSubdued=event Event_ActorToActorProximity=event Event_ActorToLocationProximity=event Event_RelativeTimer=event Event_ActorHitPointsLessThan=event
function Event_CancelEvent(h) if h then events[h]=nil end return nil end
function HUD_AddTrayDisplay(t) nextHUD=nextHUD+1 trays[nextHUD]=t t[1].nValue=t[1].nStart return nextHUD end
function HUD_GetTrayDisplay(h) return trays[h] end
function HUD_SetTrayDisplay(h,t) trays[h]=t end
function table_DeleteElement(t,item) for i=table.getn(t),1,-1 do if t[i]==item then table.remove(t,i) end end end
function mission_MissionFailure() failed=true failures=failures+1 end
-- Model objective creation only; retail removal and death dispatch run below.
function setup_delivery(args)
 args.verb=VERB_DELIVER args.targetActors={} args.targetsRemaining=table.getn(args.actorTable)
 mission_tObjectives[args.objectiveName]=args
 for _,target in ipairs(args.actorTable) do
  target.objectiveName=args.objectiveName target.verb=VERB_DELIVER_HUM target.destLoc=args.destLoc
  mission_tTargets[Utility_GetActorAsInt(target.actorName)]=target table.insert(args.targetActors,target.actorName)
  target.destroyedHandle=SetupEventActorDestroyed(target)
 end
end
function kill(name)
 dead[name]=true local callbacks={}
 for handle,e in pairs(events) do if e[1]=='TargetActorDestroyed' and e[3]==name then table.insert(callbacks,{handle,e}) end end
 for _,v in ipairs(callbacks) do events[v[1]]=nil _G[v[2][1]](name) end
end
function reset_mission()
 events={} trays={} nextEvent=0 nextHUD=0 dead={} removed={} failed=false failures=0 completed=0 mission_tObjectives={} mission_tTargets={}
end
