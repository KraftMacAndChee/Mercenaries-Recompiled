"""Run replacement regressions with locally supplied retail inputs.

This is a focused separation/behavior suite, not a substitute for gameplay QA.
Run from an exported source tree; logs default to ignored artifacts/.
"""
from pathlib import Path
import argparse,json,os,subprocess,sys,time
ROOT=Path(__file__).resolve().parents[2]
RETAIL=(
 'test_box_collision_retail','test_asset_hash_retail','test_player_actor_retail',
 'test_property_lookup_retail','test_retail_loop_limits','test_lua_intern_retail',
 'test_lua_snapshot_retail_abi','test_human_snapshot_retail','test_vehicle_audio_retail',
 'test_retail_script_decoder','test_retail_script_inspector','test_retail_model_bounds',
 'test_retail_template_metadata','test_retail_troop_catalog','test_dev_vehicle_retail',
 'test_diagnostic_route_retail','test_post_aircraft_gate_route','test_vertex_warmup_retail',
 'test_ps2_retail_mix','test_ps2_current_mix_native','test_boids_native',
 'test_boids_public_model_native','test_wmd_inspectors','test_source_package_policy',
 'test_archive_generated_provenance',
)
NATIVE=(
 'test_faction_enemy_memory_native','test_voice_failure_ordering_native',
 'test_lua_protected_unwind_native','test_notification_cursor_native',
 'test_lua_release_callstack_native','test_shader_warmup_native',
 'test_vertex_warmup_native','test_ps2_upgrades_native','test_ps2_options_native',
 'test_fps_cap_native','test_water_frame_initialization_native',
 'test_sky_screen_depth_native','test_sky_cloud_transition_native',
 'test_xact_stop_recovery_native','test_vehicle_sound_effect_range',
)
def main():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--output',type=Path,default=ROOT/'artifacts/independent-dependency-validation')
 a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
 env=os.environ.copy();env['PYTHONPATH']=str(ROOT)+os.pathsep+str(ROOT/'tools/recomp')
 env['PYTEST_DISABLE_PLUGIN_AUTOLOAD']='1';results=[]
 stages=[('retail-and-packaging',[sys.executable,'-m','pytest','-q',*[str(ROOT/'tools/recomp'/(n+'.py')) for n in RETAIL]])]
 stages += [(n,[sys.executable,str(ROOT/'tools/recomp'/(n+'.py'))]) for n in NATIVE]
 for name,command in stages:
  begin=time.monotonic()
  with (out/(name+'.log')).open('w',encoding='utf-8') as stream:
   run=subprocess.run(command,cwd=ROOT,env=env,stdout=stream,stderr=subprocess.STDOUT)
  row={'stage':name,'exit_code':run.returncode,'seconds':round(time.monotonic()-begin,2)}
  results.append(row);(out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
  print(f"{name}: {'PASS' if run.returncode==0 else 'FAIL'} ({row['seconds']}s)",flush=True)
 return int(any(r['exit_code'] for r in results))
if __name__=='__main__':raise SystemExit(main())
