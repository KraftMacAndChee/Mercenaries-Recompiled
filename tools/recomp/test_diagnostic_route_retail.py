from pathlib import Path
import json
import sys
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT))
from tools.recomp.regenerate_diagnostic_routes import regenerate


def test_retail_points_preserve_current_route():
    source=(ROOT/'ports/mercenaries/src/recomp_manual.c').read_text()
    manifest=json.loads((ROOT/'ports/mercenaries/data/diagnostic-route-retail-selectors.json').read_text())
    assert len(manifest['selectors'])==83
    assert len(manifest['world_selectors'])==4
    assert len(manifest['axis_selectors'])==3
    assert len(manifest['remaining_review'])==0
    assert len(manifest['project_policy_selectors'])==46
    assert regenerate(ROOT/'game_files/mercenaries-retail/DATAxbox/assets.dsk',source,manifest)==source


def test_briefing_values_are_in_retail_scripts():
    import hashlib
    import re
    from tools.diagnostics.inspect_retail_script import read_script
    archive=ROOT/'game_files/mercenaries-retail/DATAxbox/assets.dsk'
    briefing=read_script(archive,'briefing_scripts')
    assert hashlib.sha256(briefing.encode()).hexdigest()=='2623a36d7e7e886dc45934fb9b921807b5a688d1f17508f46ba8907e131af3f5'
    start=briefing.index('local function sw_allies1()')
    end=briefing.index('local function ',start+15)
    body=briefing[start:end]
    assert 'if playable_intro_state == STATE_ALLIES0 then' in body
    # Static script evidence, not an emulation of the event scheduler.
    assert re.findall(r'duration\s*=\s*([\d.]+)',body)[0]=='45.06666667'
    utilities=read_script(archive,'briefing_utilities')
    assert hashlib.sha256(utilities.encode()).hexdigest()=='fda11dc5436a7c07df7c601e5804ac64f65feda87f385dfa4dab35402a95e6ea'
    assert re.search(r"Actor_EnableScriptedUse\('starter_trigger',\s*TRUE,\s*'actions.briefingstarter',\s*false,\s*1\.5\)",utilities)


def test_project_detours_have_retail_obstacle_evidence():
    from tools.recomp.retail_route_obstacles import obstacles
    manifest=json.loads((ROOT/'ports/mercenaries/data/diagnostic-route-policy-evidence.json').read_text())
    rows={(r['world'],r['instance']):r for r in obstacles(ROOT/'game_files/mercenaries-retail/DATAxbox')}
    assert len(manifest['project_detours'])==46
    for point in manifest['project_detours']:
        assert point['purpose'] and len(point['retail_obstacles'])==3
        for anchor in point['retail_obstacles']:
            actual=rows[anchor['world'],anchor['instance']]
            for field in ('model','bounds','model_info_sha256'):assert actual[field]==anchor[field]
    fence=rows['swn',804]
    assert manifest['clearance_checks']['checkpoint_fence_north_edge']==fence['bounds'][5]
    assert 1135-fence['bounds'][5]>8
