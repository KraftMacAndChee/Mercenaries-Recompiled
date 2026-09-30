"""
Self-check for debug-build symbol recovery.

Run: py -3 tools/debug_symbols/test_recover.py
"""

import json
import os
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.debug_symbols.recover import (  # noqa: E402
    _compile_pattern,
    _extract_path,
    _module_name,
    _strip_common_root,
    _FunctionIndex,
    run,
)

CODE = _compile_pattern(include_headers=False)
WITH_H = _compile_pattern(include_headers=True)


def test_extract_path():
    # Plain __FILE__ expansion.
    assert _extract_path(r"c:\halo\SOURCE\ai\actors.c", CODE) == "c:/halo/SOURCE/ai/actors.c"
    # The string scanner glues a previous string's tail onto the front.
    assert _extract_path(r"-2>c:\halo\SOURCE\camera\observer.c", CODE) == "c:/halo/SOURCE/camera/observer.c"
    assert _extract_path(r"@@c:\halo\SOURCE\camera\dead_camera.c", CODE) == "c:/halo/SOURCE/camera/dead_camera.c"
    # Unix-style path, no drive letter.
    assert _extract_path("/src/engine/render.cpp", CODE) == "/src/engine/render.cpp"

    # RCS keyword banners must NOT match -- they are not __FILE__, and libtiff
    # ships hundreds of them. The trailing ",v 1.8 ..." is the tell.
    rcs = "$Header: /usr/people/sam/tiff/libtiff/RCS/tif_aux.c,v 1.8 92/03/27 14:53:02 sam Exp $"
    assert _extract_path(rcs, CODE) is None

    # A bare filename with no directory is too weak a signal to trust.
    assert _extract_path("actors.c", CODE) is None
    # Headers only when asked for.
    assert _extract_path(r"..\math\real_math.h", CODE) is None
    assert _extract_path(r"..\math\real_math.h", WITH_H) == "../math/real_math.h"
    print("ok  extract_path")


def test_strip_common_root():
    rel = _strip_common_root([
        "c:/halo/SOURCE/ai/actors.c",
        "c:/halo/SOURCE/physics/physics.c",
    ])
    assert rel["c:/halo/SOURCE/ai/actors.c"] == "ai/actors.c"
    assert rel["c:/halo/SOURCE/physics/physics.c"] == "physics/physics.c"
    # One path alone gives the prefix scan nothing to compare against, so it
    # keeps the immediate parent directory rather than stripping to a bare name.
    solo = _strip_common_root(["c:/halo/SOURCE/ai/actors.c"])
    assert solo["c:/halo/SOURCE/ai/actors.c"] == "ai/actors.c"
    # Same when many files share a single directory.
    same_dir = _strip_common_root(["c:/g/SOURCE/ai/a.c", "c:/g/SOURCE/ai/b.c"])
    assert same_dir["c:/g/SOURCE/ai/a.c"] == "ai/a.c"
    print("ok  strip_common_root")


def test_module_name_is_a_valid_c_identifier():
    assert _module_name("ai/actor_combat.c") == "actor_combat"
    # Halo ships bungie_net/common/64bit_math.c -- a leading digit does not
    # compile, and the generated header is where it shows up.
    assert _module_name("bungie_net/common/64bit_math.c") == "_64bit_math"
    # Anything else non-identifier gets folded to underscores.
    assert _module_name("saved games/game-state.c") == "game_state"
    assert _module_name("a/b c+d.cpp") == "b_c_d"
    print("ok  module_name_is_a_valid_c_identifier")


def _fn(start, end, section=".text"):
    return {"start": f"0x{start:08X}", "end": f"0x{end:08X}",
            "name": f"sub_{start:08X}", "section": section}


def test_function_index():
    idx = _FunctionIndex([_fn(0x1000, 0x1100), _fn(0x1200, 0x1300)])
    assert idx.owner(0x1000)["start"] == "0x00001000"
    assert idx.owner(0x10FF)["start"] == "0x00001000"
    assert idx.owner(0x1100) is None   # end is exclusive
    assert idx.owner(0x1150) is None   # in the gap between functions
    assert idx.owner(0x0500) is None   # before everything
    print("ok  function_index")


def test_end_to_end():
    functions = [
        _fn(0x1000, 0x1100),                  # references ai.c -> direct
        _fn(0x1100, 0x1200),                  # bracketed, same file -> interpolated
        _fn(0x1200, 0x1300),                  # references ai.c -> direct
        _fn(0x1300, 0x1400, section="D3D"),   # different section -> untouched
        _fn(0x1400, 0x1500),                  # references physics.c -> direct
    ]
    strings = [
        {"string": r"c:\g\SOURCE\ai\ai.c", "referenced_from": ["0x00001010", "0x00001210"]},
        {"string": r"c:\g\SOURCE\physics\physics.c", "referenced_from": ["0x00001410"]},
        # An RCS banner referenced from code must not create an attribution.
        {"string": "$Header: /x/RCS/tif.c,v 1.1 92/01/01 sam Exp $",
         "referenced_from": ["0x00001310"]},
    ]

    tmp = tempfile.mkdtemp()
    fpath = os.path.join(tmp, "functions.json")
    spath = os.path.join(tmp, "strings.json")
    opath = os.path.join(tmp, "out.json")
    json.dump(functions, open(fpath, "w"))
    json.dump(strings, open(spath, "w"))

    summary = run(fpath, spath, output_path=opath, apply_to=fpath, verbose=False)

    assert summary["attributed_direct"] == 3, summary
    assert summary["attributed_interpolated"] == 1, summary   # only 0x1100
    assert summary["source_paths"] == 2, summary              # RCS banner excluded

    out = json.load(open(opath))
    assert out["names"]["0x00001000"] == "ai__sub_00001000"
    assert out["attribution"]["0x00001100"]["origin"] == "interpolated"
    assert out["attribution"]["0x00001100"]["source_file"] == "ai/ai.c"
    # The D3D function sits between two attributed functions by address but in a
    # different section, and its only string ref was the RCS banner.
    assert "0x00001300" not in out["names"]

    applied = {f["start"]: f for f in json.load(open(fpath))}
    assert applied["0x00001000"]["name"] == "ai__sub_00001000"
    assert applied["0x00001000"]["source_file"] == "ai/ai.c"
    assert applied["0x00001300"]["name"] == "sub_00001300"     # untouched
    assert os.path.exists(fpath + ".bak")
    print("ok  end_to_end")


def test_apply_preserves_existing_names():
    functions = [_fn(0x1000, 0x1100), _fn(0x1200, 0x1300)]
    functions[0]["name"] = "already_named_by_ghidra"
    strings = [{"string": r"c:\g\SOURCE\ai\ai.c",
                "referenced_from": ["0x00001010", "0x00001210"]}]

    tmp = tempfile.mkdtemp()
    fpath = os.path.join(tmp, "functions.json")
    spath = os.path.join(tmp, "strings.json")
    json.dump(functions, open(fpath, "w"))
    json.dump(strings, open(spath, "w"))

    run(fpath, spath, apply_to=fpath, verbose=False)

    applied = {f["start"]: f for f in json.load(open(fpath))}
    # A name recovered by another pass must survive...
    assert applied["0x00001000"]["name"] == "already_named_by_ghidra"
    # ...but it still gets the source-file attribution.
    assert applied["0x00001000"]["source_file"] == "ai/ai.c"
    assert applied["0x00001200"]["name"] == "ai__sub_00001200"
    print("ok  apply_preserves_existing_names")


if __name__ == "__main__":
    test_extract_path()
    test_strip_common_root()
    test_module_name_is_a_valid_c_identifier()
    test_function_index()
    test_end_to_end()
    test_apply_preserves_existing_names()
    print("\nall passed")
