"""Apply reproducible Mercenaries-specific fixes to lifted retail C."""

from __future__ import annotations

import argparse
import re
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class GeneratedPatch:
    name: str
    before: str
    after: str


XMV_DIRECT_CALL_REPLACEMENTS = {
    "PUSH32(esp, 0); sub_00259E1D(); /* call 0x00259E1D */":
        "PUSH32(esp, 0); recomp_xmv_get_bit_fast(); /* call 0x00259E1D */",
    "PUSH32(esp, 0); sub_00259F01(); /* call 0x00259F01 */":
        "PUSH32(esp, 0); recomp_xmv_vlc_fast(); /* call 0x00259F01 */",
    "PUSH32(esp, 0); sub_00256833(); /* call 0x00256833 */":
        "PUSH32(esp, 0); recomp_xmv_zero32_fast(); /* call 0x00256833 */",
    "PUSH32(esp, 0); sub_0025686D(); /* call 0x0025686D */":
        "PUSH32(esp, 0); recomp_xmv_zero128_fast(); /* call 0x0025686D */",
    "PUSH32(esp, 0); sub_002568B2(); /* call 0x002568B2 */":
        "PUSH32(esp, 0); recomp_xmv_zero256_fast(); /* call 0x002568B2 */",
}
TEARDOWN_TRACE_CALLS = (
    ("00034B50", "00034B70", 0),
    ("00034B70", "001FA0A0", 0),
    ("00034BA0", "001F7D00", 0),
    ("00034BB0", "000311A0", 0),
    ("000311A0", "001F7D00", 0),
    ("000311D0", "00011070", 0),
    ("001EA1F0", "001F3C60", 0),
    ("001F3C60", "001F3C40", 0),
    ("001F3C90", "001F7D00", 0),
    ("001F3C9D", "001F3680", 0),
    ("00030E48", "001EE0E0", 0),
    ("00035C00", "00031450", 0),
    ("00035C08", "00032B00", 0),
    ("00035C9F", "00035970", 4),
    ("00035CAA", "001F7D20", 4),
)


def patch_traffic_vehicle_teardown(text: str) -> str:
    """Dead ruin replacement must not masquerade as healthy hibernation."""
    m = re.search(r'loc_0006512A: ;\n.*?(?=    PUSH32\(esp, ecx\);)', text, re.S)
    if m is None:
        return text
    legacy_trace = "    recomp_traffic_release_checkpoint(0x0006512Au, esi, MEM8(esp + 8) == 0);\n"
    raw = """loc_0006512A: ;
    SET_LO8(ecx, MEM8(esp + 8));
    (void)0; /* test LO8(ecx), LO8(ecx) - flags set for next jcc */
    SET_LO8(ecx, (TEST_Z(LO8(ecx), LO8(ecx))) ? 1 : 0); /* sete */
"""
    fixed = raw + """    /* Ruin replacement also uses vehicle teardown; dead actors still count. */
    if (MEM32(esi + 0x10) && !(MEMF(MEM32(esi + 0x10) + 0x98) > 0.0f))
        SET_LO8(ecx, 1);
    recomp_traffic_release_checkpoint(0x0006512Au, esi, LO8(ecx));
"""
    # Accept fresh output and both previous diagnostic layouts. Emit one event
    # only after computing the final destruction flag, including on replay.
    fragment = m[0].replace(legacy_trace, "")
    if fragment not in (raw, fixed) or m[0].count(legacy_trace) > 1:
        raise RuntimeError("Vehicle teardown accounting shape changed")
    return text[:m.start()] + fixed + text[m.end():]


def patch_crt_memmove_entry(text: str) -> str:
    """Copy once at the CRT entry; a restarted overlapping prefix corrupts data."""
    match = re.search(r'void sub_00238C00\(void\)\n\{.*?\n\}', text, re.S)
    if match is None:
        return text
    replacement = """void sub_00238C00(void)
{
    extern void recomp_crt_memmove(void);
    RECOMP_TRACE_FUNC(0x00238C00u);
    recomp_crt_memmove();
}"""
    if match[0] == replacement:
        return text
    if ('loc_00238C00:' not in match[0] or
            'MEM32(ebp + 0x10)' not in match[0]):
        raise RuntimeError('CRT memmove entry shape changed')
    return text[:match.start()] + replacement + text[match.end():]


def patch_apu_voice_parameter_mmio(text: str) -> str:
    """Keep retail arithmetic/order; bypass VEH only for this proven APU loop."""
    match = re.search(r'void sub_002A3A5A\(void\)\n\{.*?\n\}', text, re.S)
    if match is None or 'recomp_apu_read32' in match[0]:
        return text
    body = match[0]
    replacements = {
        'ecx = MEM32(-25034736);': 'ecx = recomp_apu_read32((uint32_t)-25034736);',
        'MEM32(0xFE820160u) = eax;': 'recomp_apu_write32(0xFE820160u, eax);',
        'MEM32(ebx) = edx;': 'recomp_apu_write32(ebx, edx);',
        'MEM32(0xFE82043Cu) = eax;': 'recomp_apu_write32(0xFE82043Cu, eax);',
        'MEM32(0xFE8202F8u) = eax;': 'recomp_apu_write32(0xFE8202F8u, eax);',
        'MEM32(0xFE82031Cu) = eax;': 'recomp_apu_write32(0xFE82031Cu, eax);',
    }
    for before, after in replacements.items():
        if body.count(before) != 1:
            raise RuntimeError('APU voice parameter MMIO anchor changed: '+before)
        body = body.replace(before, after)
    body = body.replace('{\n', '{\n    extern uint32_t recomp_apu_read32(uint32_t);\n    extern void recomp_apu_write32(uint32_t, uint32_t);\n', 1)
    return text[:match.start()] + body + text[match.end():]


def patch_apu_voice_update_mmio(text: str) -> str:
    """Route the retail voice update through the existing ordered APU bridge."""
    match = re.search(r'void sub_002A4473\(void\)\n\{.*?\n\}', text, re.S)
    if match is None:
        return text
    body = match[0]
    replacements = {
        'edx = MEM32(-25034736);':
            'edx = recomp_apu_read32((uint32_t)-25034736);',
        'MEM32(-25033992) = edx;':
            'recomp_apu_write32((uint32_t)-25033992, edx);',
        'MEM32(-25033988) = 1;':
            'recomp_apu_write32((uint32_t)-25033988, 1);',
        'MEM32(-25033888) = edx;':
            'recomp_apu_write32((uint32_t)-25033888, edx);',
        'MEM32(-25033884) = edx;':
            'recomp_apu_write32((uint32_t)-25033884, edx);',
        'MEM32(-25033880) = edx;':
            'recomp_apu_write32((uint32_t)-25033880, edx);',
        'MEM32(-25033988) = 0;':
            'recomp_apu_write32((uint32_t)-25033988, 0);',
    }
    # Keep the FIFO poll, voice lock/unlock, and all guest arithmetic intact.
    # Reject partially patched or changed lifts instead of accepting a subset.
    fresh = all(body.count(before) == 1 and after not in body
                for before, after in replacements.items())
    applied = all(body.count(after) == 1 and before not in body
                  for before, after in replacements.items())
    declarations = ('    extern uint32_t recomp_apu_read32(uint32_t);\n'
                    '    extern void recomp_apu_write32(uint32_t, uint32_t);\n')
    if applied and body.count(declarations) == 1:
        return text
    if not fresh or 'recomp_apu_' in body:
        raise RuntimeError('APU voice update MMIO shape changed')
    for before, after in replacements.items():
        body = body.replace(before, after)
    body = body.replace('{\n', '{\n' + declarations, 1)
    return text[:match.start()] + body + text[match.end():]


def patch_hud_brush_proportions(text: str) -> str:
    for address in ('00209BD0', '0020B810'):
        match = re.search(r'void sub_'+address+r'\(void\)\n\{.*?\n\}', text, re.S)
        if match is None:
            continue
        body = match[0]
        if 'recomp_ui_' in body:
            continue
        if address == '00209BD0':
            before = '    POP32(esp, esi);\n    esp = esp + 8;\n    esp += 20; return; /* ret 16 */'
            if body.count(before) != 5:
                raise RuntimeError(f'HUD position return count changed: {body.count(before)}')
            body = body.replace(before,
                '    recomp_ui_record_position(esi, MEMF(esp + 0x10u), MEM32(esp + 0x18u));\n'+before)
        else:
            body = body.replace('loc_0020B837: ;',
                'loc_0020B837: ;\n    recomp_ui_begin_brush(MEM32(esi + 8u));')
            body = body.replace('loc_0020B847: ;',
                'loc_0020B847: ;\n    recomp_ui_end_brush();')
        text = text[:match.start()] + body + text[match.end():]
    return text


def patch_reticle_proportions(text: str) -> str:
    """Scale reticle geometry about its actual aim point, never its screen position.

    Retail RsHudMainCrossHair::PaintDumbTarget draws a static bracket and a
    separately projected dynamic aim marker. They must not share an anchor.
    PaintCrossHairRing is the independent on-foot/tank textured reticle.
    """
    for address in ('000FED40', '000FEB90'):
        match = re.search(r'void sub_'+address+r'\(void\)\n\{.*?\n\}', text, re.S)
        if match is None:
            continue
        body = match[0]
        marker = '/* preserve retail reticle proportions */'
        if marker in body:
            continue
        if address == '000FED40':
            split = body.index('loc_000FF2C2: ;')
            count = 0
            def replace_call(m):
                nonlocal count
                count += 1
                anchor = '0x4C' if m.start() < split else '0x38'
                return (f'    {marker}\n'
                        f'    MEMF(esp) = recomp_options_ui_x(MEMF(esp), MEMF(esi + {anchor}));\n'
                        + m[0])
            body = re.sub(r'    PUSH32\(esp, 0\); sub_0020C160\(\); /\* call 0x0020C160 \*/', replace_call, body)
            if count != 30:
                raise RuntimeError(f'Reticle PaintTo count changed: {count}')
        else:
            before = '    PUSH32(esp, 0); sub_0020A1C0(); /* call 0x0020A1C0 */'
            if body.count(before) != 1:
                raise RuntimeError('Ring reticle PaintBox anchor changed')
            # Width is arg 3, center is the original left edge plus half width.
            body = body.replace(before,
                f'    {marker}\n'
                '    MEMF(esp) = recomp_options_ui_x(MEMF(esp), MEMF(esp) + 0.5f * MEMF(esp + 8u));\n'
                '    MEMF(esp + 8u) = recomp_options_satellite_center_width(MEMF(esp + 8u));\n'+before)
        text = text[:match.start()] + body + text[match.end():]
    return text


def patch_homing_reticle_proportions(text: str) -> str:
    """Correct the lock box and target diamond around their separate centers."""
    match = re.search(r'void sub_000FE630\(void\)\n\{.*?\n\}', text, re.S)
    if match is None or '/* homing reticle proportions */' in match[0]:
        return text
    body = match[0]
    split = body.index('loc_000FE8E9: ;')
    counts = [0, 0]
    def line(m):
        target = m.start() > split
        counts[int(target)] += 1
        # Target X is ESP+0x14 before the two PaintTo arguments are pushed.
        anchor = 'MEMF(esp + 0x1Cu)' if target else 'MEMF(esi + 0x4Cu)'
        return ('    /* homing reticle proportions */\n'
                f'    MEMF(esp) = recomp_options_ui_x(MEMF(esp), {anchor});\n' + m[0])
    body = re.sub(r'    PUSH32\(esp, 0\); sub_0020C160\(\); /\* call 0x0020C160 \*/', line, body)
    if counts != [12, 10]:
        raise RuntimeError(f'Homing bracket/diamond call counts changed: {counts}')
    call = '    PUSH32(esp, 0); sub_0020A1C0(); /* call 0x0020A1C0 */'
    if body.count(call) != 1:
        raise RuntimeError('Homing background box call changed')
    body = body.replace(call,
        '    /* homing reticle proportions */\n'
        '    MEMF(esp) = recomp_options_ui_x(MEMF(esp), MEMF(esi + 0x4Cu));\n'
        '    MEMF(esp + 8u) = recomp_options_satellite_center_width(MEMF(esp + 8u));\n' + call)
    return text[:match.start()] + body + text[match.end():]


def patch_save_browser_stack_flags(text: str) -> str:
    """Keep button event type separate from button identity after PUSH ESI."""
    for address, target in (('00188690','001886DC'),('00188760','001887A6')):
        match = re.search(r'void sub_'+address+r'\(void\)\n\{.*?\n\}',text,re.S)
        if match is None:
            continue
        body = match[0]
        marker = '/* save browser event before PUSH */'
        if marker in body:
            continue
        native = ('    _flags = (CMP_NE(MEM32(esp + 8), 1)); '
                  '/* preserve cmp flags across 2 instruction(s) */')
        if native in body:
            continue
        before = ('    (void)0; /* cmp MEM32(esp + 8), 1 - flags set for next jcc */\n'
                  '    PUSH32(esp, esi);')
        branch = f'if (CMP_NE(MEM32(esp + 8), 1)) goto loc_{target};'
        if body.count(before)!=1 or body.count(branch)!=1:
            raise RuntimeError(f'Save browser event anchor changed: {address}')
        body = body.replace(before,before.replace('    PUSH32(esp, esi);',
            '    _flags = CMP_NE(MEM32(esp + 8), 1); '+marker+'\n    PUSH32(esp, esi);'),1)
        body = body.replace(branch,f'if (_flags != 0) goto loc_{target};',1)
        text = text[:match.start()]+body+text[match.end():]
    return text


def patch_save_directory_stack_flags(text: str) -> str:
    """Preserve the retail directory-status CMP before PUSH changes ESP.

    The current lifter handles implicit ESP writes; this retains that fix in
    the older generated save unit without changing the game's success rules.
    """
    match = re.search(r'void sub_0018CC80\(void\)\n\{.*?\n\}', text, re.S)
    if match is None:
        return text
    body = match[0]
    for site, operand in (('0018CD63', '0'), ('0018CDA0', 'edi')):
        before = (
            f'loc_{site}: ;\n'
            '    (void)0; /* cmp MEM32(esp + 0xC), 1 - flags set for next jcc */\n'
            f'    PUSH32(esp, {operand});\n'
            '    if (CMP_NE(MEM32(esp + 0xC), 1)) goto loc_0018CD30; /* jne: not equal / not zero */'
        )
        after = before.replace(
            f'    PUSH32(esp, {operand});',
            '    _flags = (CMP_NE(MEM32(esp + 0xC), 1)); /* save directory status before PUSH */\n'
            f'    PUSH32(esp, {operand});'
        ).replace('if (CMP_NE(MEM32(esp + 0xC), 1))', 'if (_flags != 0)')
        if after in body:
            continue
        native = before.replace(
            f'    PUSH32(esp, {operand});',
            '    _flags = (CMP_NE(MEM32(esp + 0xC), 1)); '
            '/* preserve cmp flags across 1 instruction(s) */\n'
            f'    PUSH32(esp, {operand});'
        ).replace('if (CMP_NE(MEM32(esp + 0xC), 1))', 'if (_flags != 0)')
        if native in body:
            continue
        if before not in body:
            raise RuntimeError(f'Save directory status anchor changed: {site}')
        body = body.replace(before, after, 1)
    return text[:match.start()] + body + text[match.end():]


def patch_pose_traversal_capacity(text: str) -> str:
    """Keep RedPose's fixed 256-entry traversal stack inside its bounds.

    Retail RedPose::BuildWorldMatricesRecurse uses a 256-entry temporary
    array. A malformed/cyclic hierarchy can overwrite the guest stack.
    Guard both the loop head and each write at the 256-entry capacity, and bound total visits so a single-edge cycle cannot spin with a
    constant stack depth. Leave through the retail epilogue.
    """
    match = re.search(r'void sub_00204420\(void\)\n\{.*?\n\}', text, re.S)
    if match is None:
        return text
    body = match[0]
    loop_marker = '/* RedPose traversal validity: loop head */'
    child_marker = '/* RedPose traversal capacity: child */'
    sibling_marker = '/* RedPose traversal capacity: sibling */'
    if loop_marker in body and child_marker in body and sibling_marker in body:
        return text

    if loop_marker not in body:
        declaration = '    int _flags = 0; /* fallback flag var */\n'
        if body.count(declaration) != 1:
            raise RuntimeError('RedPose traversal visit declaration anchor changed')
        body = body.replace(
            declaration,
            declaration + '    uint32_t _recomp_pose_visits = 0u;\n',
            1,
        )
        loop_before = (
            'loc_00204442: ;\n'
            '    edx = MEM32(esp + 0xC);'
        )
        loop_after = (
            'loc_00204442: ;\n'
            f'    {loop_marker}\n'
            '    if (ebx == 0u || ebx > 256u || '\
            '++_recomp_pose_visits > 256u) {\n'
            '        recomp_pose_traversal_overflow(MEM32(esp + 0xC), 0u, '\
            'ebx, 0xFFFFFFFFu);\n'
            '        goto loc_00204632;\n'
            '    }\n'
            '    edx = MEM32(esp + 0xC);'
        )
        if body.count(loop_before) != 1:
            raise RuntimeError('RedPose traversal loop-head anchor changed')
        body = body.replace(loop_before, loop_after, 1)

    sites = (
        (
            'loc_0020460D: ;\n'
            '    ecx = SX8(LO8(eax));\n'
            '    MEM32(esp + ebx * 4 + 0xE0) = ecx;\n'
            '    ebx++;',
            'loc_0020460D: ;\n'
            '    ecx = SX8(LO8(eax));\n'
            f'    {child_marker}\n'
            '    if (ebx >= 256u) {\n'
            '        recomp_pose_traversal_overflow(MEM32(esp + 0xC), edi, '
            'ebx, ecx);\n'
            '        goto loc_00204632;\n'
            '    }\n'
            '    MEM32(esp + ebx * 4 + 0xE0) = ecx;\n'
            '    ebx++;',
            child_marker,
        ),
        (
            'loc_0020461F: ;\n'
            '    edx = SX8(LO8(eax));\n'
            '    MEM32(esp + ebx * 4 + 0xE0) = edx;\n'
            '    ebx++;',
            'loc_0020461F: ;\n'
            '    edx = SX8(LO8(eax));\n'
            f'    {sibling_marker}\n'
            '    if (ebx >= 256u) {\n'
            '        recomp_pose_traversal_overflow(MEM32(esp + 0xC), edi, '
            'ebx, edx);\n'
            '        goto loc_00204632;\n'
            '    }\n'
            '    MEM32(esp + ebx * 4 + 0xE0) = edx;\n'
            '    ebx++;',
            sibling_marker,
        ),
    )
    for before, after, marker in sites:
        if marker in body:
            continue
        if body.count(before) != 1:
            raise RuntimeError(
                f'RedPose traversal capacity anchor missing or ambiguous: {marker}'
            )
        body = body.replace(before, after, 1)
    return text[:match.start()] + body + text[match.end():]


def patch_path_spawn_death_accounting(text: str) -> str:
    """Retire exactly one live path-spawn slot for one departing actor.

    RsPath::NotifySpawnDeath only receives the actor's permanent object type,
    not the template key that created it.  Retail scans every configured
    template and decrements every matching type.  Mixed vehicle/aircraft paths
    can therefore turn several counters negative for one departure and make
    UpdateSpawner refill all of those false deficits.  Consume the first
    matching slot whose current quantity is positive, then leave through the
    retail epilogue.  This preserves the total live count without inventing
    template identity that the actor does not retain.
    """
    match = re.search(r'void sub_00122CF0\(void\)\n\{.*?\n\}', text, re.S)
    if match is None:
        return text
    body = match[0]
    marker = '/* RsPath departure accounting: retire one live slot */'
    if marker in body:
        return text

    before = (
        'loc_00122D19: ;\n'
        '    edx = MEM32(edi + 0x44);\n'
        '    MEM16(edx + esi * 2) = MEM16(edx + esi * 2) - 1;\n'
        '    eax = edx + esi * 2;'
    )
    after = (
        'loc_00122D19: ;\n'
        '    edx = MEM32(edi + 0x44);\n'
        f'    {marker}\n'
        '    if ((int16_t)MEM16(edx + esi * 2) > 0) {\n'
        '        MEM16(edx + esi * 2) = MEM16(edx + esi * 2) - 1;\n'
        '        eax = edx + esi * 2;\n'
        '        goto loc_00122D2C;\n'
        '    }\n'
        '    eax = edx + esi * 2;'
    )
    if body.count(before) != 1:
        raise RuntimeError('RsPath departure accounting anchor changed')
    body = body.replace(before, after, 1)
    return text[:match.start()] + body + text[match.end():]


def trace_pda_store_renderer(text: str) -> str:
    """Add opt-in, read-only tracing around the retail Store row traversal."""
    match = re.search(r'void sub_000C69A0\(void\)\n\{.*?\n\}', text, re.S)
    if match is None:
        return text
    body = match[0]
    if 'recomp_pda_store_render_checkpoint(' in body:
        return text
    anchors = (
        (
            '    MEM32(esp + 0x14) = 4;\n',
            '    MEM32(esp + 0x14) = 4;\n'
            '    recomp_pda_store_render_checkpoint(0u, ebx, edi, ebp, 0u, '
            'MEM32(esp + 0x14), esp);\n',
        ),
        (
            'loc_000C6BBA: ;\n',
            'loc_000C6BBA: ;\n'
            '    recomp_pda_store_render_checkpoint(1u, ebx, '
            'MEM32(esp + 0x20), esi, edi, MEM32(esp + 0x14), esp);\n',
        ),
        (
            'loc_000C6BF6: ;\n',
            'loc_000C6BF6: ;\n'
            '    recomp_pda_store_render_checkpoint(2u, ebx, '
            'MEM32(esp + 0x20), esi, edi, eax, esp);\n',
        ),
        (
            'loc_000C6C06: ;\n    esi = eax;\n',
            'loc_000C6C06: ;\n'
            '    recomp_pda_store_render_checkpoint(3u, ebx, '
            'MEM32(esp + 0x20), esi, edi, eax, esp);\n'
            '    esi = eax;\n',
        ),
    )
    for before, after in anchors:
        if body.count(before) != 1:
            raise RuntimeError('PDA Store renderer trace anchor changed')
        body = body.replace(before, after, 1)
    return text[:match.start()] + body + text[match.end():]


def trace_screen_flash_setter(text: str) -> str:
    """Trace the retail RsHudScreenDimmer::FlashScreen setter at its entry.

    This is observation-only and remains inert unless the runtime trace
    environment variable is present.  Keeping the insertion here makes the
    diagnostic reproducible whenever the retail XBE is translated again.
    """
    match = re.search(r'void sub_000F26F0\(void\)\n\{.*?\n\}', text, re.S)
    if match is None:
        return text
    body = match[0]
    marker = 'recomp_screen_flash_checkpoint('
    if marker in body:
        return text
    anchor = 'loc_000F26F0: ;\n'
    if body.count(anchor) != 1:
        raise RuntimeError('Screen flash setter trace anchor changed')
    call = (
        'loc_000F26F0: ;\n'
        '    { extern void recomp_screen_flash_checkpoint(uint32_t, uint32_t, '
        'uint32_t, uint32_t);\n'
        '      recomp_screen_flash_checkpoint(ecx, MEM32(esp + 4), '
        'MEM32(esp + 8), MEM32(esp + 0xC)); }\n'
    )
    body = body.replace(anchor, call, 1)
    return text[:match.start()] + body + text[match.end():]


def trace_survivor_state(text: str) -> str:
    """Trace the retail survivor decision and entry without changing either."""
    marker = 'recomp_survivor_state_checkpoint('
    declaration = (
        '    { extern void recomp_survivor_state_checkpoint(uint32_t, '
        'uint32_t, uint32_t, uint32_t, float, float, uint32_t, uint32_t);\n'
    )

    update_match = re.search(r'void sub_000917C0\(void\)\n\{.*?\n\}', text, re.S)
    if update_match is not None:
        body = update_match[0]
        original_body = body
        # An earlier diagnostic revision sampled the raw hitpoints at 91B99 on
        # every AI update.  Migrate that revision out deterministically: the
        # ratio checkpoint below contains the useful value and is reached only
        # at the retail survivor decision, avoiding a needless host call in
        # normal builds even when tracing is disabled.
        legacy_raw_checkpoint = (
            'loc_00091B99: ;\n'
            '    ecx = MEM32(eax + 0x98);\n'
            '    edx = MEM32(edi);\n'
            '    MEM32(esp + 0x10) = ecx;\n' + declaration +
            '      recomp_survivor_state_checkpoint(0u, esi, eax, edi, '
            'MEMF(esp + 0x10), MEMF(esi + 0xD78), MEM32(esi + 0x18), '
            'esi + 0xC8C); }\n'
        )
        if legacy_raw_checkpoint in body:
            body = body.replace(
                legacy_raw_checkpoint,
                'loc_00091B99: ;\n'
                '    ecx = MEM32(eax + 0x98);\n'
                '    edx = MEM32(edi);\n'
                '    MEM32(esp + 0x10) = ecx;\n',
                1,
            )
        if marker not in body:
            anchors = (
                (
                    '    MEMF(esp + 0x10) = (float)fp_top(); fp_pop(); /* fstp */\n'
                    '    { uint32_t _icall_esp = g_esp;\n',
                    '    MEMF(esp + 0x10) = (float)fp_top(); fp_pop(); /* fstp */\n' +
                    declaration +
                    '      recomp_survivor_state_checkpoint(1u, esi, 0u, edi, '
                    'MEMF(esp + 0x10), MEMF(esi + 0xD78), MEM32(esi + 0x18), '
                    'esi + 0xC8C); }\n'
                    '    { uint32_t _icall_esp = g_esp;\n',
                ),
                (
                    'loc_00091BF3: ;\n'
                    '    PUSH32(esp, eax);\n',
                    'loc_00091BF3: ;\n' + declaration +
                    '      recomp_survivor_state_checkpoint(2u, esi, 0u, edi, '
                    'MEMF(esp + 0x10), MEMF(esi + 0xD78), MEM32(esi + 0x18), '
                    'esi + 0xC8C); }\n'
                    '    PUSH32(esp, eax);\n',
                ),
            )
            for before, after in anchors:
                if body.count(before) != 1:
                    raise RuntimeError('Survivor update trace anchor changed')
                body = body.replace(before, after, 1)
        if body != original_body:
            text = text[:update_match.start()] + body + text[update_match.end():]

    enter_match = re.search(r'void sub_0008EF40\(void\)\n\{.*?\n\}', text, re.S)
    if enter_match is not None:
        body = enter_match[0]
        if marker not in body:
            anchor = (
                'loc_0008EF5B: ;\n'
                '    fp_top() *= MEMF(0x2E39F8); /* fmul memory */\n'
                '    eax = MEM32(esi + 8);\n'
            )
            replacement = (
                'loc_0008EF5B: ;\n'
                '    fp_top() *= MEMF(0x2E39F8); /* fmul memory */\n'
                '    eax = MEM32(esi + 8);\n' + declaration +
                '      recomp_survivor_state_checkpoint(3u, eax, 0u, 0u, '
                'MEMF(eax + 0xD78), (float)fp_top(), MEM32(eax + 0x18), '
                'eax + 0xC8C); }\n'
            )
            if body.count(anchor) != 1:
                raise RuntimeError('Survivor entry trace anchor changed')
            body = body.replace(anchor, replacement, 1)
            text = text[:enter_match.start()] + body + text[enter_match.end():]
    return text

def patch_weapon_fire_continuation(text: str) -> str:
    """Keep 34BD0's retail tail in its aligned frame, including dry-fire paths.

    The original 357E4/357F8/35832 addresses are basic blocks, not calls.
    A minimal-ret stub leaves the 0xAB8-byte weapon frame on the guest stack.
    Reuse the lifted tail, retaining its SEH, sound, rumble and RET 20 logic.
    """
    pattern = r'void sub_00034BD0\(void\)\n\{.*?\n\}'
    match = re.search(pattern, text, re.S)
    marker = '/* retail weapon fire continuation repair */'
    if match is None or marker in match[0]:
        return text
    parent = match[0]
    tail_match = re.search(r'void sub_000357E2\(void\)\n\{.*?\n\}', text, re.S)
    if tail_match is None:
        raise RuntimeError('Weapon fire shared tail missing')
    tail = tail_match[0][tail_match[0].index('loc_000357E2: ;'):].rsplit('\n}', 1)[0]
    tail = tail.replace('    eax = 0; /* xor self */\n',
                        '    eax = 0; /* xor self */\n\nloc_000357E4: ;\n', 1)
    tail = tail.replace('    goto loc_00035832;\n\n    edx = MEM32(esi);',
                        '    goto loc_00035832;\n\nloc_000357F8: ;\n    edx = MEM32(esi);', 1)
    if 'loc_000357F8:' not in tail or 'esp += 24; return;' not in tail:
        raise RuntimeError('Weapon fire shared epilogue anchors changed')
    cases = ''
    for address, value in (('000357C6', 4), ('000357CD', 1),
                           ('000357D4', 2), ('000357DB', 3)):
        cases += f'loc_{address}: ;\n    eax = {value};\n    goto loc_000357E4;\n\n'
    for address, count in (('000357F8', 1), ('00035832', 2), ('000357E2', 1)):
        before = f'{{ sub_{address}(); return; }}'
        if parent.count(before) != count:
            raise RuntimeError(f'Weapon fire branch count changed: {address}')
        parent = parent.replace(before, f'goto loc_{address};')
    switch = ('    g_seh_ebp = ebp; RECOMP_ITAIL(MEM32(eax * 4 + 0x35874)); '
              'return; /* indirect tail jmp */')
    local_switch = '    { const uint32_t _fire_tail = MEM32(eax * 4 + 0x35874);\n'
    for address in ('000357C6','000357CD','000357D4','000357DB','000357E2'):
        local_switch += f'      if (_fire_tail == 0x{address}u) goto loc_{address};\n'
    local_switch += '      g_seh_ebp = ebp; RECOMP_ITAIL(_fire_tail); return; }'
    if parent.count(switch) != 1:
        raise RuntimeError('Weapon fire feedback switch missing')
    parent = parent.replace(switch, local_switch)
    parent = parent.replace('\n    #undef fp_push',
                            '\n    ' + marker + '\n\n' + cases + tail + '\n\n    #undef fp_push', 1)
    return text[:match.start()] + parent + text[match.end():]


def trace_transition_pool(text: str) -> str:
    """Keep the default-off pool watch/reset probes across regeneration."""
    for site, statement, call in (
        ('00043957', '', 'recomp_arm_transition_pool_watchpoint(esi + 0x6E8);'),
        ('00043AB2', '    ecx = esi + 0x6E8;\n',
         'recomp_transition_pool_reset_checkpoint(ecx);'),
    ):
        anchor = f'loc_{site}: ;\n'
        start = text.find(anchor)
        if start < 0:
            continue
        end = text.find('\nloc_', start + len(anchor))
        if end < 0:
            end = len(text)
        block = text[start:end]
        if call in block:
            continue
        insertion = statement or anchor
        if insertion not in block:
            raise RuntimeError(f'Transition pool probe anchor missing: {site}')
        block = block.replace(insertion, insertion + f'    {call}\n', 1)
        text = text[:start] + block + text[end:]
    return text


def trace_property_slot_lookup(text: str) -> str:
    """Trace the retail property lookup that bounds the 0x12C-byte slot pool.

    This is diagnostic-only: the host checkpoint is gated by
    MERCENARIES_TRACE_TEMP_ARRAY and never changes guest state.
    """
    match = re.search(r'void sub_00168030\(void\)\n\{.*?\n\}', text, re.S)
    if match is None:
        return text
    body = match[0]
    marker = '/* retail property-slot lookup diagnostic */'
    if marker in body:
        return text
    anchor = (
        '    edx = MEM32(edi);\n'
        '    esp = esp + 0x10;\n'
        '    { uint32_t _icall_esp = g_esp;\n'
        '    PUSH32(esp, eax);\n'
        '    ecx = edi;\n'
        '    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(edx + 0x10), _icall_esp); /* indirect call */\n'
        '    }\n'
    )
    if body.count(anchor) != 2:
        raise RuntimeError('Property-slot lookup anchors changed')
    for before_stage, after_stage in ((30, 31), (32, 33)):
        replacement = (
            '    edx = MEM32(edi);\n'
            '    esp = esp + 0x10;\n'
            f'    {marker}\n'
            f'    recomp_temp_array_checkpoint({before_stage}u, esi, edi, eax, MEM32(edx + 0x10));\n'
            '    { uint32_t _icall_esp = g_esp;\n'
            '    PUSH32(esp, eax);\n'
            '    ecx = edi;\n'
            '    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(edx + 0x10), _icall_esp); /* indirect call */\n'
            '    }\n'
            f'    recomp_temp_array_checkpoint({after_stage}u, esi, edi, eax,\n'
            '                                 (uint32_t)(int32_t)SMEM8(esi + 0x8C9));\n'
        )
        body = body.replace(anchor, replacement, 1)
    return text[:match.start()] + body + text[match.end():]


# Opt-in call-boundary evidence. Cleanup counts come from retail RETs.
EVENT_ABI_CALLERS = ('00111D90', '001113A0', '0010FD90', '0010FE40', '0010E000', '0010E1E0', '00120E30', '001135E0', '001DD5A0', '001DF4F0', '0011EA30', '00113A50')
EVENT_ABI_CLEANUP = {'000EB750': 8, '0010E000': 4, '0010E0B0': 4, '0010E1E0': 4, '0010E220': 4, '0010E260': 4, '0010E290': 4, '0010E340': 4, '0010E3C0': 4, '0010E420': 4, '0010E460': 4, '0010E4D0': 4, '0010E510': 4, '0010E550': 4, '0010E570': 4, '0010E600': 4, '0010E610': 4, '0010E690': 4, '0010E6A0': 4, '0010E6E0': 4, '0010E800': 4, '0010E950': 4, '0010E990': 4, '0010E9F0': 4, '0010EA10': 4, '0010EA60': 4, '0010EAD0': 4, '0010EB50': 4, '0010EB90': 4, '0010EC40': 4, '0010ECA0': 4, '0010F4B0': 4, '0010F4F0': 4, '0010F880': 4, '0010F990': 4, '0010FA80': 0, '0010FD90': 0, '0010FE40': 0, '00110000': 4, '001100D0': 4, '00110200': 4, '00110490': 4, '001104D0': 4, '001105B0': 4, '00110620': 4, '00110690': 4, '00110720': 4, '001107A0': 4, '00110810': 4, '00110850': 4, '00110980': 4, '00110B90': 4, '00110D00': 4, '00110D60': 4, '00110DC0': 4, '00110E00': 4, '00110E40': 4, '00110EB0': 4, '00110F30': 4, '00110F90': 4, '00111020': 4, '001110B0': 4, '00111140': 4, '001111A0': 4, '001111F0': 4, '00111280': 4, '001112D0': 4, '001113A0': 4, '00112120': 0, '00112930': 0, '00112AD0': 0, '00112E20': 0, '001135E0': 0, '00113A50': 4, '00113B60': 0, '00113BA0': 0, '0011D390': 4, '00120E30': 0, '0014E6D0': 12, '00152210': 0, '001DB990': 0, '001DC820': 0, '001DC970': 0, '001DC980': 0, '001DCA40': 0, '001DCB30': 0, '001DCC10': 0, '001DCF90': 0, '001DD000': 0, '001DD190': 0, '001DD380': 0, '001DD480': 0, '001DD5A0': 0, '001DD7A0': 0, '001DE8A0': 0, '001DE970': 0, '001DF400': 0, '001DF4F0': 0, '001E28A0': 0, '001F29F0': 0, '001FECD0': 0, '001FED70': 0, '001FFA60': 0, '00237FC0': 0}


def trace_event_abi_calls(text: str) -> str:
    for caller in EVENT_ABI_CALLERS:
        match = re.search(r'void sub_' + caller + r'\(void\)\n\{.*?\n\}', text, re.S)
        if not match or 'recomp_event_abi_checkpoint(' in match[0]:
            continue
        def wrap(call):
            target = call[1]
            if target not in EVENT_ABI_CLEANUP:
                return call[0]
            cleanup = EVENT_ABI_CLEANUP[target]
            return ('{ const uint32_t _event_call_esp = esp;\n'
                    '      const uint32_t _event_call_esi = esi;\n'
                    '      const uint32_t _event_call_edi = edi;\n'
                    '      void recomp_event_abi_checkpoint(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);\n'
                    '      ' + call[0] + '\n'
                    f'      recomp_event_abi_checkpoint(0x{caller}u, 0x{target}u, '
                    f'_event_call_esp + {cleanup}u, _event_call_esi, _event_call_edi); }}')
        body = re.sub(r'PUSH32\(esp, 0\); sub_([0-9A-F]{8})\(\); /\* call 0x[0-9A-F]{8} \*/', wrap, match[0])
        text = text[:match.start()] + body + text[match.end():]
    return text

def trace_teardown_calls(text: str) -> str:
    """Opt-in observation only: preserve original calls and guest state."""
    for site, callee, cleanup in TEARDOWN_TRACE_CALLS:
        start = text.find(f"loc_{site}: ;")
        if start < 0:
            continue
        call = f"    PUSH32(esp, 0); sub_{callee}(); /* call 0x{callee} */"
        end = text.find("\nloc_", start + 1)
        if end < 0:
            end = len(text)
        block = text[start:end]
        if "recomp_teardown_call_checkpoint(" in block:
            continue
        if call not in block:
            raise RuntimeError(f"Teardown trace call missing: {site}/{callee}")
        replacement = ("    { const uint32_t _teardown_esp = esp;\n"
                       "      const uint32_t _teardown_esi = esi;\n" + call + "\n"
                       f"      recomp_teardown_call_checkpoint(0x{site}u, "
                       f"_teardown_esp + {cleanup}u, _teardown_esi); }}")
        text = text[:start] + block.replace(call, replacement, 1) + text[end:]
    for site, actor in (("00030E40", "ecx"), ("00030E4F", "esi"),
                        ("00035C00", "ecx"), ("00035CB7", "esi"),
                        ("00034B50", "ecx"), ("000511F0", "ecx"),
                        ("00058000", "ecx")):
        anchor = f"loc_{site}: ;\n"
        call = f"    recomp_teardown_item_checkpoint(0x{site}u, {actor});\n"
        if anchor in text and anchor + call not in text:
            text = text.replace(anchor, anchor + call, 1)
    return text


EXTRACTION_PROBE_SITES = (
    ('0001BEB0', 0, 'MEM32(esp + 4u)', 'MEM32(esp + 8u)'),
    ('0001BEF9', 1, 'esi', 'MEM32(esp + 0x14u)'),
    ('0001BFAA', 2, 'esi', 'MEM32(ebp + 0xCu)'),
    ('0001BFC1', 3, 'esi', 'eax'),
    ('0001BFDA', 4, 'esi', 'MEM32(ebp + 0xCu)'),
    ('0007CAC0', 5, 'ecx', '0u'),
)


def trace_extraction_flight(text: str) -> str:
    """Default-off, read-only spawn/approach evidence with unique retail labels."""
    for site, stage, subject, value in EXTRACTION_PROBE_SITES:
        anchor = f'loc_{site}: ;\n'
        call = ('    { extern void recomp_extraction_probe(uint32_t, uint32_t, uint32_t);\n'
                f'      recomp_extraction_probe({stage}u, {subject}, {value}); }}\n')
        if anchor in text and anchor + call not in text:
            if text.count(anchor) != 1:
                raise RuntimeError(f'Ambiguous extraction probe site: {site}')
            text = text.replace(anchor, anchor + call, 1)
    return text


def trace_vehicle_audio_update(text: str) -> str:
    """Default-off observation of the retail RsSoundEffectCar2 update state."""
    anchor = 'loc_0014F720: ;\n'
    call = (
        '    { extern void recomp_vehicle_audio_update_checkpoint(uint32_t);\n'
        '      recomp_vehicle_audio_update_checkpoint(ecx); }\n'
    )
    if anchor not in text or anchor + call in text:
        return text
    if text.count(anchor) != 1:
        raise RuntimeError('Ambiguous RsSoundEffectCar2::Update trace site')
    return text.replace(anchor, anchor + call, 1)

EXTRACTION_USE_PROBE_SITES = (
    # Extraction vehicle: observe the resolved hp_dock_lr matrix before any
    # eligibility branch, then the action-list state immediately after append.
    ('000409C0', 0, 'esi', 'MEM32(esp + 0x20u)', 'esp + 0x30u', 'eax'),
    ('00040A10', 1, 'esi', 'MEM32(esp + 0x40u)', 'esp + 0x50u',
     'MEM32(esp + 0x3Cu)'),
    # World use selection: observe the extraction vehicle's returned list,
    # the no-match exit, and the action that wins the radius test.
    ('000407C5', 2, 'MEM32(esp + 0x10u)', 'edi', 'esp + 0x2Cu', 'eax'),
    ('00040871', 3, 'MEM32(esp + 0x10u)', 'edi', 'esp + 0x2Cu', 'ebx'),
    ('00040880', 4, 'MEM32(esp + 0x10u)', 'edi', 'esi - 0x14u', '1u'),
)


def trace_extraction_use(text: str) -> str:
    """Default-off evidence for extraction action generation and selection."""
    for site, stage, actor, user, data, count in EXTRACTION_USE_PROBE_SITES:
        anchor = f'loc_{site}: ;\n'
        call = ('    { extern void recomp_extraction_use_probe(uint32_t, uint32_t, '
                'uint32_t, uint32_t, uint32_t);\n'
                f'      recomp_extraction_use_probe({stage}u, {actor}, {user}, '
                f'{data}, {count}); }}\n')
        if anchor in text and anchor + call not in text:
            if text.count(anchor) != 1:
                raise RuntimeError(f'Ambiguous extraction-use probe site: {site}')
            text = text.replace(anchor, anchor + call, 1)
    return text


def trace_ai_update_calls(text: str) -> str:
    """Observe retail AI call ABI boundaries; never repair or suppress a call."""
    direct_cleanup = {'00064060': 4, '00011A20': 0, '00064130': 0,
                      '00065EB0': 4, '00064170': 0, '00067720': 0,
                      '0006C090': 4, '00065EF0': 8, '00065350': 0,
                      '0006C420': 4, '000966A0': 4}
    slot_cleanup = {4: 0, 0x4C: 0, 0x13C: 0, 0x138: 4, 0x30: 0,
                    0x134: 0, 0x170: 4, 0x174: 4, 0x1C: 0, 8: 4,
                    0x17C: 0, 0x254: 4, 0x24: 0}
    for function in ('0006C420', '00076540', '0006E930'):
        pattern = re.compile(r'void sub_' + function + r'\(void\)\n\{.*?\n\}', re.S)
        match = pattern.search(text)
        if not match or 'recomp_ai_call_checkpoint(' in match[0]:
            continue
        site = function
        lines = []
        for line in match[0].splitlines():
            label = re.match(r'loc_([0-9A-F]{8}):', line)
            if label:
                site = label[1]
            if 'PUSH32(esp, 0);' not in line or ('sub_' not in line and 'RECOMP_ICALL_SAFE' not in line):
                lines.append(line)
                continue
            # Observe the airplane/base-AI boundary too. The inner observer
            # cannot see corruption discovered only when the base epilogue
            # reloads its caller's saved ESI. Do not repair that saved state.
            if function == '0006E930' and 'sub_0006C420();' not in line:
                lines.append(line)
                continue
            direct = re.search(r'sub_([0-9A-F]{8})\(\)', line)
            if direct:
                cleanup = direct_cleanup[direct[1]]
            else:
                slot = re.search(r'RECOMP_ICALL_SAFE\(MEM32\(\w+ \+ (0x[0-9A-F]+|[0-9]+)\)', line)
                if not slot:
                    raise RuntimeError(f'Unrecognized AI call at {site}: {line}')
                cleanup = slot_cleanup[int(slot[1], 0)]
            lines += ['    { const uint32_t _ai_esp = esp;',
                      '      const uint32_t _ai_esi = esi, _ai_edi = edi;',
                      '      extern void recomp_ai_call_checkpoint(uint32_t, uint32_t, uint32_t, uint32_t);',
                      line,
                      f'      recomp_ai_call_checkpoint(0x{site}u, _ai_esp + {cleanup}u, _ai_esi, _ai_edi);',
                      '    }']
        text = text[:match.start()] + '\n'.join(lines) + text[match.end():]
    return text


def patch_simple_scalar_delete_flags(text: str) -> str:
    """Repair only the exact 31-byte retail scalar deleting-dtor template.

    The lifter already handles implicit ESP writes now, but older generated
    units still re-read [ESP+4] after PUSH ESI. Leave larger/custom destructors
    alone. Native tests verify every matched template against retail bytes.
    """
    functions = re.compile(r"void sub_([0-9A-F]{8})\(void\)\n\{.*?\n\}", re.S)

    def repair(match: re.Match) -> str:
        body = match.group(0)
        address = int(match.group(1), 16)
        vtable = re.search(r"MEM32\(esi\) = (0x[0-9A-F]+);", body)
        if not vtable:
            return body
        expected = f"""void sub_{address:08X}(void)
{{
    int _flags = 0; /* fallback flag var */
    RECOMP_TRACE_FUNC(0x{address:08X}u);

loc_{address:08X}: ;
    (void)0; /* test MEM8(esp + 4), 1 - flags set for next jcc */
    PUSH32(esp, esi);
    esi = ecx;
    MEM32(esi) = {vtable.group(1)};
    if (TEST_Z(MEM8(esp + 4), 1)) goto loc_{address + 25:08X}; /* je: equal / zero */

loc_{address + 16:08X}: ;
    PUSH32(esp, esi);
    PUSH32(esp, 0); sub_001F6B60(); /* call 0x001F6B60 */

loc_{address + 22:08X}: ;
    esp = esp + 4;

loc_{address + 25:08X}: ;
    eax = esi;
    POP32(esp, esi);
    esp += 8; return; /* ret 4 */

}}"""
        if body != expected:
            return body
        return body.replace(
            "    PUSH32(esp, esi);", 
            "    _flags = TEST_Z(MEM8(esp + 4), 1); /* scalar-delete flag before PUSH */\n"
            "    PUSH32(esp, esi);", 1).replace(
                "if (TEST_Z(MEM8(esp + 4), 1))", "if (_flags != 0)")

    return functions.sub(repair, text)


SIMPLE_SHARED_EPILOGUES = (
    ('00040626', '5f5e83c440c20400', ('edi', 'esi'), 0x40, 4),
    ('000D9303', '5f5e5bc20800', ('edi', 'esi', 'ebx'), 0, 8),
    ('000E0B84', '5e83c408c20400', ('esi',), 8, 4),
    ('000E1D98', '5e83c410c20400', ('esi',), 0x10, 4),
    ('0014559E', '5e83c454c20400', ('esi',), 0x54, 4),
)


def simple_shared_epilogue_patch(address, retail_bytes, registers, locals_size, arguments):
    body = [f'void sub_{address}(void)', '{',
            f'    /* Retail shared epilogue: {retail_bytes.upper()}. */']
    body += [f'    POP32(esp, {register});' for register in registers]
    if locals_size:
        body.append(f'    esp += 0x{locals_size:X};')
    body += [f'    esp += {arguments + 4}; return; /* ret {arguments} */', '}']
    return GeneratedPatch(
        f'Retail shared epilogue {address} restores its actual frame',
        f'void sub_{address}(void) {{ esp += 4; /* 0x{address}: not detected; minimal guest ret */ }}',
        '\n'.join(body),
    )


def simple_shared_epilogue_inline(address, retail_bytes, registers, locals_size, arguments):
    """Return the exact correct form when the translator owns this interior label."""
    body = [f'loc_{address}: ;']
    body += [f'    POP32(esp, {register});' for register in registers]
    if locals_size:
        body.append(f'    esp = esp + 0x{locals_size:X};')
    body.append(f'    esp += {arguments + 4}; return; /* ret {arguments} */')
    return '\n'.join(body)


TRANSLATOR_INLINED_PATCHES = {
    simple_shared_epilogue_patch(*entry).name:
        simple_shared_epilogue_inline(*entry)
    for entry in SIMPLE_SHARED_EPILOGUES
}
TRANSLATOR_INLINED_PATCHES.update({
    'RedSoundSystem omitted dataset wrappers':
        '''g_seh_ebp = ebp; sub_001FE7D0(); return; /* tail jmp 0x001FE7D0 */

}

/**
 * sub_001FFDB0
 * Original: 0x001FFDB0 - 0x001FFDD1 (33 bytes, 13 insns)
 * CC: cdecl, 0 params, returns int_or_void
 * Frame: fpo_leaf
 */
void sub_001FFDB0(void)
{
    int _flags = 0; /* fallback flag var */
    RECOMP_TRACE_FUNC(0x001FFDB0u);''',    'CRT x87 shared exception continuation':
        '''loc_0023C03C: ;
    MEMD(ebp + -8) = fp_top(); fp_pop(); /* fstp */
    MEM32(ebp + -28) = ecx;''',    'CRT logarithm shared completion':
        '''loc_0023C159: ;
    esp = esp - 8;
    MEMD(esp) = fp_top(); /* fst */
    eax = MEM32(esp + 4);''',    'retail float setter tail at 0x0010E540':
        '''loc_0010E540: ;
    recomp_xmm_loadss(xmm0v, esp + 4); /* movss */
    MEMF(ecx) = xmm0; /* movss */
    esp += 8; return; /* ret 4 */''',
    'Retail extraction helicopter shared return restores its full frame':        '''loc_0007CDF7: ;
    POP32(esp, edi);
    POP32(esp, esi);
    esp = esp + 0x1C;
    esp += 8; return; /* ret 4 */''',
    'Havok breakable constraint preserves mid-function EBX save':
        '''loc_001C2592: ;
    ecx = MEM32(esi + 0x20);
    eax = MEM32(ecx);
    PUSH32(esp, ebx);
    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(eax + 0x14), _icall_esp); /* indirect call */
    }''',
    'Vehicle physics shared continuations and epilogues':
        '''loc_0014FEC8: ;
    edx = MEM32(esi + 0xBC);
    MEM32(esi + 0xC0) = edx;

loc_0014FED4: ;
    POP32(esp, edi);
    POP32(esp, esi);
    POP32(esp, ebp);
    POP32(esp, ebx);
    esp = esp + 0x40;
    esp += 8; return; /* ret 4 */''',
    'Vehicle physics shared body interior entries':
        '''loc_0014FA2B: ;
    ecx = MEM32(esi + 8);
    PUSH32(esp, 0x740DC48D);
    PUSH32(esp, 0); sub_00212760(); /* call 0x00212760 */''',
    'Vehicle physics shared body entry dispatch':
        '''loc_0014FA2B: ;
    ecx = MEM32(esi + 8);''',
    'Vehicle physics 0x14FA3D interior label':
        '''loc_0014FA38: ;
    PUSH32(esp, 0xA8965862u);

loc_0014FA3D: ;
    ecx = MEM32(esi + 8);''',
    'Vehicle physics 0x14FA49 interior label':
        '''loc_0014FA47: ;
    ebp = eax;

loc_0014FA49: ;
    eax = MEM32(esi + 0x138);''',    'Camera update missing retail no-target continuation':
        '''loc_0008DC8D: ;
    PUSH32(esp, 8);
    ecx = esi + 0x9C4;
    PUSH32(esp, 0); sub_001546C0(); /* call 0x001546C0 */

loc_0008DC9A: ;
    POP32(esp, edi);
    POP32(esp, esi);
    esp += 4; return; /* ret */''',
    'Camera update missing retail shared continuations':
        '''loc_0008DBFA: ;
    ecx = esi + 0x9C4;

loc_0008DC00: ;
    PUSH32(esp, 0); sub_001546C0(); /* call 0x001546C0 */''',
    'Camera object update missing retail shared continuation 0x40187':
        '''loc_00040187: ;
    SET_LO8(eax, MEM8(esi + 0x1AA));
    if (TEST_Z(LO8(eax), LO8(eax))) goto loc_000401E9; /* je: equal / zero */

loc_00040191: ;
    if (TEST_NZ(LO8(ebx), LO8(ebx))) goto loc_000401E9; /* jne: not equal / not zero */''',
    'Lua string-format shared continuations':
        '''loc_001DB046: ;
    eax = esp + 0x14;
    PUSH32(esp, eax);

loc_001DB04B: ;
    PUSH32(esp, 0); sub_002370B8(); /* call 0x002370B8 */

loc_001DB050: ;
    esp = esp + 0x14;

loc_001DB053: ;
    ecx = esp + 4;
    PUSH32(esp, ecx);

loc_001DB058: ;
    PUSH32(esp, esi);
    PUSH32(esp, 0); sub_001DD000(); /* call 0x001DD000 */

loc_001DB05E: ;
    esp = esp + 8;

loc_001DB061: ;
    eax = 1;
    POP32(esp, esi);
    esp = esp + 0x80;''',
    'Lua topointer shared continuations':
        '''loc_001DCF24: ;
    ecx = esi;
    PUSH32(esp, 0); sub_001DC820(); /* call 0x001DC820 */

loc_001DCF2B: ;
    if (TEST_Z(eax, eax)) goto loc_001DCF1F; /* je: equal / zero */

loc_001DCF2F: ;
    ecx = MEM32(eax);
    ecx = ecx + 0xFFFFFFFEu;
    if (CMP_A(ecx, 6)) goto loc_001DCF1F; /* ja: above (unsigned >) */''',
    'Retail comparator shared tails':
        '''loc_000659E6: ;
    edx = MEM32(esi + 0xC);
    (void)0; /* cmp edx, MEM32(edi + 0xC) - flags set for next jcc */
    _flags = (CMP_NE(edx, MEM32(edi + 0xC))); /* preserve flags for jne at CFG join 0x00065A5D */
    goto loc_00065A5D;''',
    'Retail actor update omitted 0x000401FE continuation':
        '''loc_000401FE: ;
    SET_LO8(eax, MEM8(esp + 0xB));
    if (TEST_NZ(LO8(eax), LO8(eax))) goto loc_00040255; /* jne: not equal / not zero */

loc_00040206: ;
    edx = MEM32(edi + 0x8BC);
    if (CMP_NE(MEM32(edx + 0x118), 6)) goto loc_00040255; /* jne: not equal / not zero */''',
    'Retail actor update shared 0x00040260 epilogue':
        '''loc_00040260: ;
    POP32(esp, edi);
    POP32(esp, esi);
    esp = esp + 0x14;
    esp += 8; return; /* ret 4 */''',
    'retail load save invalid operation shared epilogue restores its frame':
        '''loc_0018D354: ;
    eax = edi;
    POP32(esp, edi);
    POP32(esp, esi);
    POP32(esp, ecx);
    esp += 4; return; /* ret */''',
    'retail load save pending and error shared epilogue restores its frame':
        '''loc_0018D3FE: ;
    if (CMP_NE(eax, 7)) goto loc_0018D353; /* jne: not equal / not zero */

loc_0018D407: ;
    edi = 3;
    POP32(esp, ebp);
    eax = edi;
    POP32(esp, edi);
    MEM32(esi) = 0;
    POP32(esp, esi);
    POP32(esp, ecx);
    esp += 4; return; /* ret */''',
    'retail save completion shared epilogue restores its full frame':
        '''loc_0018D435: ;
    edi = 2;
    POP32(esp, ebp);
    eax = edi;
    POP32(esp, edi);
    MEM32(esi) = 0;
    POP32(esp, esi);
    POP32(esp, ecx);
    esp += 4; return; /* ret */''',
    'retail DataPod Status shared-tail interior entries':
        '''loc_000CB444: ;
    eax = esp + 0x10;
    PUSH32(esp, eax);

loc_000CB449: ;
    PUSH32(esp, 0); sub_002370B8(); /* call 0x002370B8 */

loc_000CB44E: ;
    esp = esp + 8;

loc_000CB451: ;
    ecx = esp + 0xC;
    PUSH32(esp, ecx);
    PUSH32(esp, 0); sub_001F29F0(); /* call 0x001F29F0 */''',
})


SSE_STACK_COMPARE_REPAIRS = (
    ('000146C5', 0x146CD, '0f2f4424185e5b760a', 8,
     'loc_000146C5: ;\n    recomp_xmm_loadss(xmm0v, 0x2DC3B4); /* movss */\n    /* comiss xmm0, MEMF(esp + 0x18) - sets EFLAGS */\n    POP32(esp, esi);\n    POP32(esp, ebx);\n    if (((isnan((double)(xmm0)) || isnan((double)(MEMF(esp + 0x18)))) || (xmm0 <= MEMF(esp + 0x18)))) goto loc_000146E0; /* jbe: below or equal (unsigned <=) */',
     'loc_000146C5: ;\n    recomp_xmm_loadss(xmm0v, 0x2DC3B4); /* movss */\n    /* comiss xmm0, MEMF(esp + 0x18) - sets EFLAGS */\n    _flags = (((isnan((double)(xmm0)) || isnan((double)(MEMF(esp + 0x18)))) || (xmm0 <= MEMF(esp + 0x18)))); /* preserve comiss flags across 2 instruction(s) */\n    POP32(esp, esi);\n    POP32(esp, ebx);\n    if (_flags != 0) goto loc_000146E0; /* jbe: below or equal (unsigned <=) */'),
    ('00018226', 0x18231, '0f2f4424305d760c', 4,
     'loc_00018226: ;\n    recomp_xmm_loadss(xmm0v, 0x2DC3C4); /* movss */\n    esp = esp + 0x14;\n    /* comiss xmm0, MEMF(esp + 0x30) - sets EFLAGS */\n    POP32(esp, ebp);\n    if (((isnan((double)(xmm0)) || isnan((double)(MEMF(esp + 0x30)))) || (xmm0 <= MEMF(esp + 0x30)))) goto loc_00018245; /* jbe: below or equal (unsigned <=) */',
     'loc_00018226: ;\n    recomp_xmm_loadss(xmm0v, 0x2DC3C4); /* movss */\n    esp = esp + 0x14;\n    /* comiss xmm0, MEMF(esp + 0x30) - sets EFLAGS */\n    _flags = (((isnan((double)(xmm0)) || isnan((double)(MEMF(esp + 0x30)))) || (xmm0 <= MEMF(esp + 0x30)))); /* preserve comiss flags across 1 instruction(s) */\n    POP32(esp, ebp);\n    if (_flags != 0) goto loc_00018245; /* jbe: below or equal (unsigned <=) */'),
    ('00081855', 0x81858, '0f2f4424305e5d5b7613', 12,
     'loc_00081855: ;\n    recomp_xmm_zero(xmm0v); /* xorps self = zero */\n    /* comiss xmm0, MEMF(esp + 0x30) - sets EFLAGS */\n    POP32(esp, esi);\n    POP32(esp, ebp);\n    POP32(esp, ebx);\n    if (((isnan((double)(xmm0)) || isnan((double)(MEMF(esp + 0x30)))) || (xmm0 <= MEMF(esp + 0x30)))) goto loc_00081875; /* jbe: below or equal (unsigned <=) */',
     'loc_00081855: ;\n    recomp_xmm_zero(xmm0v); /* xorps self = zero */\n    /* comiss xmm0, MEMF(esp + 0x30) - sets EFLAGS */\n    _flags = (((isnan((double)(xmm0)) || isnan((double)(MEMF(esp + 0x30)))) || (xmm0 <= MEMF(esp + 0x30)))); /* preserve comiss flags across 3 instruction(s) */\n    POP32(esp, esi);\n    POP32(esp, ebp);\n    POP32(esp, ebx);\n    if (_flags != 0) goto loc_00081875; /* jbe: below or equal (unsigned <=) */'),
    ('001344B0', 0x1344B6, '0f2f44242c568bf10f8301010000', -4,
     'loc_001344B0: ;\n    recomp_xmm_zero(xmm0v); /* xorps self = zero */\n    esp = esp - 0x28;\n    /* comiss xmm0, MEMF(esp + 0x2C) - sets EFLAGS */\n    PUSH32(esp, esi);\n    esi = ecx;\n    if ((!(isnan((double)(xmm0)) || isnan((double)(MEMF(esp + 0x2C)))) && (xmm0 >= MEMF(esp + 0x2C)))) goto loc_001345C5; /* jae: above or equal (unsigned >=) */',
     'loc_001344B0: ;\n    recomp_xmm_zero(xmm0v); /* xorps self = zero */\n    esp = esp - 0x28;\n    /* comiss xmm0, MEMF(esp + 0x2C) - sets EFLAGS */\n    _flags = ((!(isnan((double)(xmm0)) || isnan((double)(MEMF(esp + 0x2C)))) && (xmm0 >= MEMF(esp + 0x2C)))); /* preserve comiss flags across 2 instruction(s) */\n    PUSH32(esp, esi);\n    esi = ecx;\n    if (_flags != 0) goto loc_001345C5; /* jae: above or equal (unsigned >=) */'),
    ('001346B0', 0x1346B6, '0f2f442458568bf10f8354080000', -4,
     'loc_001346B0: ;\n    recomp_xmm_zero(xmm0v); /* xorps self = zero */\n    esp = esp - 0x54;\n    /* comiss xmm0, MEMF(esp + 0x58) - sets EFLAGS */\n    PUSH32(esp, esi);\n    esi = ecx;\n    if ((!(isnan((double)(xmm0)) || isnan((double)(MEMF(esp + 0x58)))) && (xmm0 >= MEMF(esp + 0x58)))) goto loc_00134F18; /* jae: above or equal (unsigned >=) */',
     'loc_001346B0: ;\n    recomp_xmm_zero(xmm0v); /* xorps self = zero */\n    esp = esp - 0x54;\n    /* comiss xmm0, MEMF(esp + 0x58) - sets EFLAGS */\n    _flags = ((!(isnan((double)(xmm0)) || isnan((double)(MEMF(esp + 0x58)))) && (xmm0 >= MEMF(esp + 0x58)))); /* preserve comiss flags across 2 instruction(s) */\n    PUSH32(esp, esi);\n    esi = ecx;\n    if (_flags != 0) goto loc_00134F18; /* jae: above or equal (unsigned >=) */'),
)


SSE_ADDRESS_COMPARE_REPAIRS = (
    ('000657B9', 0x657BF, '0f2f83d40200005f5e5b760b',
     'loc_000657B9: ;\n    recomp_xmm_loadss(xmm0v, esp + 0x28); /* movss */\n    /* comiss xmm0, MEMF(ebx + 0x2D4) - sets EFLAGS */\n    POP32(esp, edi);\n    POP32(esp, esi);\n    POP32(esp, ebx);\n    if (((isnan((double)(xmm0)) || isnan((double)(MEMF(ebx + 0x2D4)))) || (xmm0 <= MEMF(ebx + 0x2D4)))) goto loc_000657D6; /* jbe: below or equal (unsigned <=) */',
     'loc_000657B9: ;\n    recomp_xmm_loadss(xmm0v, esp + 0x28); /* movss */\n    /* comiss xmm0, MEMF(ebx + 0x2D4) - sets EFLAGS */\n    _flags = (((isnan((double)(xmm0)) || isnan((double)(MEMF(ebx + 0x2D4)))) || (xmm0 <= MEMF(ebx + 0x2D4)))); /* preserve comiss flags across 3 instruction(s) */\n    POP32(esp, edi);\n    POP32(esp, esi);\n    POP32(esp, ebx);\n    if (_flags != 0) goto loc_000657D6; /* jbe: below or equal (unsigned <=) */'),
    ('001493C6', 0x1493CD, '0f2f4c8d008d4c8d007606',
     'loc_001493C6: ;\n    eax = (uint32_t)(int32_t)SMEM16(ebx + -2);\n    ecx = eax + eax * 2;\n    /* comiss xmm1, MEMF(ebp + ecx * 4) - sets EFLAGS */\n    ecx = ebp + ecx * 4;\n    if (((isnan((double)(xmm1)) || isnan((double)(MEMF(ebp + ecx * 4)))) || (xmm1 <= MEMF(ebp + ecx * 4)))) goto loc_001493DE; /* jbe: below or equal (unsigned <=) */',
     'loc_001493C6: ;\n    eax = (uint32_t)(int32_t)SMEM16(ebx + -2);\n    ecx = eax + eax * 2;\n    /* comiss xmm1, MEMF(ebp + ecx * 4) - sets EFLAGS */\n    _flags = (((isnan((double)(xmm1)) || isnan((double)(MEMF(ebp + ecx * 4)))) || (xmm1 <= MEMF(ebp + ecx * 4)))); /* preserve comiss flags across 1 instruction(s) */\n    ecx = ebp + ecx * 4;\n    if (_flags != 0) goto loc_001493DE; /* jbe: below or equal (unsigned <=) */'),
    ('00149432', 0x149439, '0f2f4c8d008d4c8d007606',
     'loc_00149432: ;\n    eax = (uint32_t)(int32_t)SMEM16(ebx + 2);\n    ecx = eax + eax * 2;\n    /* comiss xmm1, MEMF(ebp + ecx * 4) - sets EFLAGS */\n    ecx = ebp + ecx * 4;\n    if (((isnan((double)(xmm1)) || isnan((double)(MEMF(ebp + ecx * 4)))) || (xmm1 <= MEMF(ebp + ecx * 4)))) goto loc_0014944A; /* jbe: below or equal (unsigned <=) */',
     'loc_00149432: ;\n    eax = (uint32_t)(int32_t)SMEM16(ebx + 2);\n    ecx = eax + eax * 2;\n    /* comiss xmm1, MEMF(ebp + ecx * 4) - sets EFLAGS */\n    _flags = (((isnan((double)(xmm1)) || isnan((double)(MEMF(ebp + ecx * 4)))) || (xmm1 <= MEMF(ebp + ecx * 4)))); /* preserve comiss flags across 1 instruction(s) */\n    ecx = ebp + ecx * 4;\n    if (_flags != 0) goto loc_0014944A; /* jbe: below or equal (unsigned <=) */'),
    ('00149490', 0x14949B, '0f2f4c8d008d4c8d007606',
     'loc_00149490: ;\n    eax = MEM32(esp + 0x10);\n    eax = (uint32_t)(int32_t)SMEM16(eax + edx * 2);\n    ecx = eax + eax * 2;\n    /* comiss xmm1, MEMF(ebp + ecx * 4) - sets EFLAGS */\n    ecx = ebp + ecx * 4;\n    if (((isnan((double)(xmm1)) || isnan((double)(MEMF(ebp + ecx * 4)))) || (xmm1 <= MEMF(ebp + ecx * 4)))) goto loc_001494AC; /* jbe: below or equal (unsigned <=) */',
     'loc_00149490: ;\n    eax = MEM32(esp + 0x10);\n    eax = (uint32_t)(int32_t)SMEM16(eax + edx * 2);\n    ecx = eax + eax * 2;\n    /* comiss xmm1, MEMF(ebp + ecx * 4) - sets EFLAGS */\n    _flags = (((isnan((double)(xmm1)) || isnan((double)(MEMF(ebp + ecx * 4)))) || (xmm1 <= MEMF(ebp + ecx * 4)))); /* preserve comiss flags across 1 instruction(s) */\n    ecx = ebp + ecx * 4;\n    if (_flags != 0) goto loc_001494AC; /* jbe: below or equal (unsigned <=) */'),
)


PATCHES = tuple(GeneratedPatch(
    f'Retail COMISS {site} retains its original address registers', before, after)
    for site, va, raw, before, after in SSE_ADDRESS_COMPARE_REPAIRS
) + tuple(GeneratedPatch(
    f'Retail stack-relative COMISS {site} survives ESP changes', before, after)
    for site, va, raw, delta, before, after in SSE_STACK_COMPARE_REPAIRS
) + tuple(simple_shared_epilogue_patch(*entry) for entry in SIMPLE_SHARED_EPILOGUES) + (
    GeneratedPatch('Optional M1 texture at RedRenderer binding',
        'loc_00210560: ;\n    PUSH32(esp, esi);\n    esi = MEM32(esp + 0xC);',
        'loc_00210560: ;\n    PUSH32(esp, esi);\n    esi = recomp_ps2_texture(MEM32(esp + 0xC));'),
    GeneratedPatch('PS2 authored sound bank variants',
        'loc_00280141: ;\n    PUSH32(esp, esi);',
        'loc_00280141: ;\n    MEM32(esp + 4) = recomp_ps2_sound_bank(MEM32(esp + 4), esp + 8);\n    PUSH32(esp, esi);'),
    GeneratedPatch('Select PS2 cue at friendly-name resolution',
        'loc_0027FA38: ;\n    eax = MEM32(edi + 4);',
        'loc_0027FA38: ;\n    eax = recomp_ps2_cue_index(ebx, MEM32(edi + 4));'),
    GeneratedPatch('Player PS2 wave before format preparation',
        'loc_00287A87: ;\n    ecx = MEM32(esi + 0x74);',
        'loc_00287A87: ;\n    recomp_ps2_wave(edi, esi);\n    ecx = MEM32(esi + 0x74);'),
    GeneratedPatch('Bind owned PS2 PCM with its complete play region',
        'loc_002821ED: ;\n    PUSH32(esp, MEM32(esp + 4));',
        'loc_002821ED: ;\n    {\n        const uint32_t replacement = recomp_ps2_wave_data(MEM32(esp + 8));\n        if (replacement) {\n            const uint32_t buffer = MEM32(esp + 4);\n            const uint32_t length = MEM32(MEM32(esp + 8) + 12);\n            PUSH32(esp, length);\n            PUSH32(esp, replacement);\n            ecx = buffer;\n            PUSH32(esp, 0); sub_00280FD5();\n            if ((int32_t)eax >= 0) {\n                PUSH32(esp, length);\n                PUSH32(esp, 0);\n                ecx = buffer;\n                PUSH32(esp, 0); sub_0028100F();\n            }\n            esp += 12; return;\n        }\n    }\n    PUSH32(esp, MEM32(esp + 4));'),
    GeneratedPatch('Preserve human corpse cleanup policy during hibernation',
        'loc_0004D3C3: ;\n    eax = MEM32(esi);',
        'loc_0004D3C3: ;\n'
        '    /* Keep mission corpse retention across streaming and save/load. Store the\n'
        "       raw setting, not the getter's temporary suppression for verifiable actors. */\n"
        '    PUSH32(esp, MEM8(esi + 0x6A4) ? 0x4DB211E5u : 0x0B069958u);\n'
        '    PUSH32(esp, 0x20D5747Du); /* CorpseCleanUpAllowed */\n'
        '    ecx = esp + 0x18;\n'
        '    PUSH32(esp, 0); sub_001EA730();\n'
        '    eax = esp + 0x10;\n'
        '    PUSH32(esp, eax);\n'
        '    ecx = esp + 0x44;\n'
        '    PUSH32(esp, 0); sub_001EB080();\n'
        '    eax = MEM32(esi);'),
    GeneratedPatch('Province reset snapshots current loaded map',
        '    RECOMP_TRACE_FUNC(0x0011B810u);',
        '    RECOMP_TRACE_FUNC(0x0011B810u);\n    const uint32_t province_from_map=MEM32(0x403970u);\n    uint32_t province_to_map=0;'),
    GeneratedPatch('Province reset records explicit destination',
        'loc_0011B836: ;\n    PUSH32(esp, eax);',
        'loc_0011B836: ;\n    province_to_map=eax;\n    PUSH32(esp, eax);'),
    GeneratedPatch('Accepted province reset preserves authored arrival',
        'loc_0011B84D: ;\n    MEM8(eax + 0x36) = 1;',
        'loc_0011B84D: ;\n    { void recomp_chapter_return_province(uint32_t,uint32_t); recomp_chapter_return_province(province_from_map,province_to_map); }\n    MEM8(eax + 0x36) = 1;'),
    GeneratedPatch('Ace movie records validated return map',
        '    RECOMP_TRACE_FUNC(0x0011B700u);',
        '    RECOMP_TRACE_FUNC(0x0011B700u);\n    uint32_t chapter_return_map=0;'),
    GeneratedPatch('Ace return arms only for an accepted movie command',
        'loc_0011B779: ;\n    MEM32(eax + 0xA0) = edi;',
        'loc_0011B779: ;\n    { extern void recomp_chapter_return_movie(uint32_t,uint32_t); recomp_chapter_return_movie(edi,chapter_return_map); }\n    MEM32(eax + 0xA0) = edi;'),
    GeneratedPatch('Ace movie clears previous return destination',
        'loc_0011B700: ;\n    PUSH32(esp, esi);',
        'loc_0011B700: ;\n    { extern void recomp_chapter_return_movie(uint32_t,uint32_t); recomp_chapter_return_movie(0,0); }\n    PUSH32(esp, esi);'),
    GeneratedPatch('Ace of Clubs schedules authored AN HQ return',
        'loc_0011B739: ;\n    PUSH32(esp, eax);',
        'loc_0011B739: ;\n    chapter_return_map=eax;\n    PUSH32(esp, eax);'),
    GeneratedPatch('Consume chapter return only for next game arrival',
        '    RECOMP_TRACE_FUNC(0x00183D40u);',
        '    RECOMP_TRACE_FUNC(0x00183D40u);\n    extern uint32_t recomp_chapter_return_consume(void);\n    uint32_t chapter_return_location=recomp_chapter_return_consume();'),
    GeneratedPatch('Chapter return takes precedence over stale manual save location',
        'loc_00183D74: ;\n    if (TEST_Z(esi, esi)) goto loc_00183DD5; /* je: equal / zero */\n\nloc_00183D78: ;\n    PUSH32(esp, esi);\n    PUSH32(esp, 0); sub_001F29F0(); /* call 0x001F29F0 */',
        'loc_00183D74: ;\n    if (chapter_return_location == UINT32_MAX) goto loc_00183DD5; /* authored province spawn */\n    if (TEST_Z(esi, esi) && !chapter_return_location) goto loc_00183DD5;\n\nloc_00183D78: ;\n    PUSH32(esp, esi);\n    if(chapter_return_location)eax=chapter_return_location;\n    else { PUSH32(esp, 0); sub_001F29F0(); } /* retail save-name hash */'),
    GeneratedPatch('Player camera pitch friction obeys Aim Assist option',
        'loc_0008FDE0: ;\n    recomp_xmm_loadss(xmm0v, 0x2DC08C);',
        'loc_0008FDE0: ;\n    if (!recomp_controls_aim_assist()) MEMF(esp + 0x10) = 0.f;\n    recomp_xmm_loadss(xmm0v, 0x2DC08C);'),
    GeneratedPatch('Mouse look releases retail crouch pitch hold',
        'loc_000901D0: ;\n    recomp_xmm_loadss(xmm0v, esi + 0xD74);',
        'loc_000901D0: ;\n    if (recomp_controls_mouse_look_pending()) goto loc_0009025B;\n    recomp_xmm_loadss(xmm0v, esi + 0xD74);'),
    GeneratedPatch('Player target yaw assistance obeys Aim Assist option',
        'loc_0009040F: ;\n    fp_push(MEMF(esp + 0x10));',
        'loc_0009040F: ;\n    if (!recomp_controls_aim_assist()) MEMF(esp + 0x10) = 0.f;\n    fp_push(MEMF(esp + 0x10));'),
    GeneratedPatch("Failed voice playback completes its Lua wait before pool release",
        'loc_00206527: ;\n    eax = MEM32(esi);',
        'loc_00206527: ;\n    /* A managed voice may have a handle even when its low-level sound never\n     * started (bank unavailable / PlayCue failure). Retire its Lua wait on the\n     * next update, after the calling script has installed its continuation.\n     * Successful playback and explicit stops of real sounds stay untouched. */\n    if (MEM32(esi + 8) == 0u && MEM32(esi + 4) != 0u &&\n        MEM32(esi + 0x4C) == 0x0011EA00u) {\n        MEM32(esi + 0x4C) = 0u;\n        const uint32_t _voice_callback_esp = g_esp;\n        PUSH32(esp, MEM32(esi + 4));\n        PUSH32(esp, 0); RECOMP_ICALL_SAFE(0x0011EA00u, _voice_callback_esp);\n        esp += 4; /* retail voice callback is cdecl */\n    }\n    eax = MEM32(esi);'),
    GeneratedPatch(
        "Raw mouse pitch adds an angle after retail stick inertia",
        """    MEMF(ebp + 0xD2C) = xmm0; /* movss */
    PUSH32(esp, 0); sub_00097880(); /* call 0x00097880 */""",
        """    MEMF(ebp + 0xD2C) = xmm0; /* movss */
    { const float _mouse_before = MEMF(esp + 0x14);
      MEMF(esp + 0x14) = recomp_controls_mouse_delta(1u, _mouse_before);
      /* Also cancel if raw input arrived after the earlier hold check. */
      if (MEMF(esp + 0x14) != _mouse_before) MEMF(ebp + 0xD74) = 0.f; }
    PUSH32(esp, 0); sub_00097880(); /* call 0x00097880 */""",
    ),
    GeneratedPatch('Mouse yaw releases retail crouch aim hold before consuming motion',
        'loc_0009031C: ;\n    recomp_xmm_zero(xmm0v);',
        'loc_0009031C: ;\n    if (recomp_controls_mouse_look_pending()) MEMF(ebx + 0xD74) = 0.f;\n    recomp_xmm_zero(xmm0v);'),
    GeneratedPatch(
        "Raw mouse yaw adds an angle after retail stick inertia",
        """    MEMF(esi + 0xD28) = xmm0; /* movss */
    PUSH32(esp, 0); sub_00097870(); /* call 0x00097870 */""",
        """    MEMF(esi + 0xD28) = xmm0; /* movss */
    { const float _mouse_before = MEMF(esp + 0x30);
      MEMF(esp + 0x30) = recomp_controls_mouse_delta(0u, _mouse_before);
      /* Also cancel if raw input arrived after the earlier hold check. */
      if (MEMF(esp + 0x30) != _mouse_before) MEMF(esi + 0xD74) = 0.f; }
    PUSH32(esp, 0); sub_00097870(); /* call 0x00097870 */""",
    ),
    GeneratedPatch(
        "Raw mouse vehicle and scope axes use simulation event cadence",
        """loc_000A3AD0: ;
    PUSH32(esp, ecx);""",
        """loc_000A3AD0: ;
    MEMF(esp + 8u) = recomp_controls_mouse_axis(MEM32(esp + 4u), MEMF(esp + 8u), MEMF(0x00413FA0u));
    PUSH32(esp, ecx);""",
    ),
    GeneratedPatch(
        'Options slider selection retains TEST BL flags across coordinate work',
        '''loc_000E94B0: ;
    fp_top() -= MEMF(0x2DD3E4); /* fsub memory */
    (void)0; /* test LO8(ebx), LO8(ebx) - flags set for next jcc */
    MEMF(esp + 0x3C) = (float)fp_top(); /* fst */
    fp_top() += MEMF(0x2EB5F8); /* fadd memory */
    MEMF(esp + 0x18) = (float)fp_top(); fp_pop(); /* fstp */
    ebx = MEM32(esp + 0x18);
    if (TEST_Z(LO8(ebx), LO8(ebx))) goto loc_000E954A; /* je: equal / zero */''',
        '''loc_000E94B0: ;
    fp_top() -= MEMF(0x2DD3E4); /* fsub memory */
    (void)0; /* test LO8(ebx), LO8(ebx) - flags set for next jcc */
    _flags = (TEST_Z(LO8(ebx), LO8(ebx))); /* preserve test flags across 4 instruction(s) */
    MEMF(esp + 0x3C) = (float)fp_top(); /* fst */
    fp_top() += MEMF(0x2EB5F8); /* fadd memory */
    MEMF(esp + 0x18) = (float)fp_top(); fp_pop(); /* fstp */
    ebx = MEM32(esp + 0x18);
    if (_flags != 0) goto loc_000E954A; /* je: equal / zero */''',
    ),
    GeneratedPatch(
        'Airstrike approach retains waypoint comparison across taken JNE',
        '''loc_0006F633: ;
    if (((int32_t)esp <= 0)) goto loc_0006F546; /* jle: less or equal (signed <=) */''',
        '''loc_0006F633: ;
    if (CMP_LE(eax, 2)) goto loc_0006F546; /* jle: less or equal (signed <=) */''',
    ),
    GeneratedPatch(
        'Helicopter physics retains delta-time comparison before PUSH',
        '''loc_0013ADD0: ;
    recomp_xmm_zero(xmm0v); /* xorps self = zero */
    esp = esp - 0x24;
    /* comiss xmm0, MEMF(esp + 0x28) - sets EFLAGS */
    PUSH32(esp, esi);
    esi = ecx;
    if ((!(isnan((double)(xmm0)) || isnan((double)(MEMF(esp + 0x28)))) && (xmm0 >= MEMF(esp + 0x28)))) goto loc_0013AFF4; /* jae: above or equal (unsigned >=) */''',
        '''loc_0013ADD0: ;
    recomp_xmm_zero(xmm0v); /* xorps self = zero */
    esp = esp - 0x24;
    /* comiss xmm0, MEMF(esp + 0x28) - sets EFLAGS */
    _flags = ((!(isnan((double)(xmm0)) || isnan((double)(MEMF(esp + 0x28)))) && (xmm0 >= MEMF(esp + 0x28)))); /* preserve comiss flags across 2 instruction(s) */
    PUSH32(esp, esi);
    esi = ecx;
    if (_flags != 0) goto loc_0013AFF4; /* jae: above or equal (unsigned >=) */''',
    ),
    GeneratedPatch(
        'Human yaw increasing step keeps its own COMISS at the merge',
        '''loc_0013DEBD: ;
    xmm0 = xmm0 + xmm2; /* addss */
    /* comiss xmm0, xmm1 - sets EFLAGS */
    goto loc_0013DED2;''',
        '''loc_0013DEBD: ;
    xmm0 = xmm0 + xmm2; /* addss */
    /* comiss xmm0, xmm1 - sets EFLAGS */
    _flags = isnan(xmm0) || isnan(xmm1) || xmm0 <= xmm1;
    goto loc_0013DED2;''',
    ),
    GeneratedPatch(
        'Human yaw decreasing step keeps its own COMISS at the merge',
        '''loc_0013DECB: ;
    xmm0 = xmm0 - xmm2; /* subss */
    /* comiss xmm1, xmm0 - sets EFLAGS */

loc_0013DED2: ;
    if (((isnan((double)(xmm1)) || isnan((double)(xmm0))) || (xmm1 <= xmm0))) goto loc_0013DED7; /* jbe: below or equal (unsigned <=) */''',
        '''loc_0013DECB: ;
    xmm0 = xmm0 - xmm2; /* subss */
    /* comiss xmm1, xmm0 - sets EFLAGS */
    _flags = isnan(xmm1) || isnan(xmm0) || xmm1 <= xmm0;

loc_0013DED2: ;
    if (_flags) goto loc_0013DED7; /* JBE inherits the executed yaw-step comparison */''',
    ),
    GeneratedPatch(
        'Human lean increasing step keeps its own COMISS at the merge',
        '''loc_0013DFE1: ;
    xmm0 = xmm0 + xmm2; /* addss */
    /* comiss xmm0, xmm1 - sets EFLAGS */
    goto loc_0013DFF6;''',
        '''loc_0013DFE1: ;
    xmm0 = xmm0 + xmm2; /* addss */
    /* comiss xmm0, xmm1 - sets EFLAGS */
    _flags = isnan(xmm0) || isnan(xmm1) || xmm0 <= xmm1;
    goto loc_0013DFF6;''',
    ),
    GeneratedPatch(
        'Human lean decreasing step keeps its own COMISS at the merge',
        '''loc_0013DFEF: ;
    xmm0 = xmm0 - xmm2; /* subss */
    /* comiss xmm1, xmm0 - sets EFLAGS */

loc_0013DFF6: ;
    if (((isnan((double)(xmm1)) || isnan((double)(xmm0))) || (xmm1 <= xmm0))) goto loc_0013DFFB; /* jbe: below or equal (unsigned <=) */''',
        '''loc_0013DFEF: ;
    xmm0 = xmm0 - xmm2; /* subss */
    /* comiss xmm1, xmm0 - sets EFLAGS */
    _flags = isnan(xmm1) || isnan(xmm0) || xmm1 <= xmm0;

loc_0013DFF6: ;
    if (_flags) goto loc_0013DFFB; /* JBE inherits the executed lean-step comparison */''',
    ),
    GeneratedPatch(
        'Retail extraction helicopter shared return restores its full frame',
        'void sub_0007CDF7(void) { esp += 4; /* 0x0007CDF7: not detected; minimal guest ret */ }',
        '''void sub_0007CDF7(void)
{
    /* Interior epilogue of RsAiHelicopter::StateExtraction::Update.
     * Retail 5F 5E 83 C4 1C C2 04 00; reached by a branch, not a call. */
    POP32(esp, edi);
    POP32(esp, esi);
    esp += 0x1C;
    esp += 8; return; /* ret 4 */
}''',
    ),
    GeneratedPatch(
        'AI priority lower-bound branch retains its COMISS predecessor',
        '''    xmm1 = xmm1 * MEMF(0x2DC340); /* mulss */
    /* comiss xmm1, xmm0 - sets EFLAGS */
    goto loc_0006C4D7;''',
        '''    xmm1 = xmm1 * MEMF(0x2DC340); /* mulss */
    /* comiss xmm1, xmm0 - sets EFLAGS */
    _flags = isnan(xmm1) || isnan(xmm0) || xmm1 <= xmm0;
    goto loc_0006C4D7;''',
    ),
    GeneratedPatch(
        'AI priority upper-bound branch retains its COMISS predecessor',
        '''    xmm1 = xmm1 * MEMF(0x2DC090); /* mulss */
    /* comiss xmm0, xmm1 - sets EFLAGS */

loc_0006C4D7: ;''',
        '''    xmm1 = xmm1 * MEMF(0x2DC090); /* mulss */
    /* comiss xmm0, xmm1 - sets EFLAGS */
    _flags = isnan(xmm0) || isnan(xmm1) || xmm0 <= xmm1;

loc_0006C4D7: ;''',
    ),
    GeneratedPatch(
        'AI priority shared JBE uses the actual incoming comparison',
        '''loc_0006C4D7: ;
    MEMF(esi + 0x564) = xmm0; /* movss */
    if (((isnan((double)(xmm0)) || isnan((double)(xmm1))) || (xmm0 <= xmm1))) goto loc_0006C4E9; /* jbe: below or equal (unsigned <=) */''',
        '''loc_0006C4D7: ;
    MEMF(esi + 0x564) = xmm0; /* movss */
    if (_flags) goto loc_0006C4E9; /* JBE inherits its executed COMISS */''',
    ),
    GeneratedPatch(
        'Human-region scalar destructor 00049AD0 retains stack delete flag',
        'loc_00049AD0: ;\n    (void)0; /* test MEM8(esp + 4), 1 - flags set for next jcc */\n    PUSH32(esp, esi);\n    esi = ecx;\n    MEM32(esi) = 0x2E1EE0;\n    if (TEST_Z(MEM8(esp + 4), 1)) goto loc_00049AE9; /* je: equal / zero */',
        'loc_00049AD0: ;\n    (void)0; /* test MEM8(esp + 4), 1 - flags set for next jcc */\n    _flags = (TEST_Z(MEM8(esp + 4), 1)); /* preserve test flags across 3 instruction(s) */\n    PUSH32(esp, esi);\n    esi = ecx;\n    MEM32(esi) = 0x2E1EE0;\n    if (_flags != 0) goto loc_00049AE9; /* je: equal / zero */',
    ),
    GeneratedPatch(
        'Human-region scalar destructor 0004D160 retains stack delete flag',
        'loc_0004D160: ;\n    (void)0; /* test MEM8(esp + 4), 1 - flags set for next jcc */\n    PUSH32(esp, esi);\n    esi = ecx;\n    MEM32(esi) = 0x2E2480;\n    if (TEST_Z(MEM8(esp + 4), 1)) goto loc_0004D179; /* je: equal / zero */',
        'loc_0004D160: ;\n    (void)0; /* test MEM8(esp + 4), 1 - flags set for next jcc */\n    _flags = (TEST_Z(MEM8(esp + 4), 1)); /* preserve test flags across 3 instruction(s) */\n    PUSH32(esp, esi);\n    esi = ecx;\n    MEM32(esi) = 0x2E2480;\n    if (_flags != 0) goto loc_0004D179; /* je: equal / zero */',
    ),
    GeneratedPatch(
        'Human-region scalar destructor 0005DDB0 retains stack delete flag',
        'loc_0005DDB0: ;\n    (void)0; /* test MEM8(esp + 4), 1 - flags set for next jcc */\n    PUSH32(esp, esi);\n    esi = ecx;\n    MEM32(esi) = 0x2E3734;\n    if (TEST_Z(MEM8(esp + 4), 1)) goto loc_0005DDC9; /* je: equal / zero */',
        'loc_0005DDB0: ;\n    (void)0; /* test MEM8(esp + 4), 1 - flags set for next jcc */\n    _flags = (TEST_Z(MEM8(esp + 4), 1)); /* preserve test flags across 3 instruction(s) */\n    PUSH32(esp, esi);\n    esi = ecx;\n    MEM32(esi) = 0x2E3734;\n    if (_flags != 0) goto loc_0005DDC9; /* je: equal / zero */',
    ),
    GeneratedPatch(
        'Human global broadcast retains message comparison across PUSH',
        'loc_0004DB50: ;\n    (void)0; /* cmp MEM32(esp + 4), 0xEC013C64u - flags set for next jcc */\n    PUSH32(esp, esi);\n    esi = ecx;\n    if (CMP_NE(MEM32(esp + 4), 0xEC013C64u)) goto loc_0004DB8F; /* jne: not equal / not zero */',
        'loc_0004DB50: ;\n    (void)0; /* cmp MEM32(esp + 4), 0xEC013C64u - flags set for next jcc */\n    _flags = (CMP_NE(MEM32(esp + 4), 0xEC013C64u)); /* preserve cmp flags across 2 instruction(s) */\n    PUSH32(esp, esi);\n    esi = ecx;\n    if (_flags != 0) goto loc_0004DB8F; /* jne: not equal / not zero */',
    ),
    GeneratedPatch(
        "DirectSound scalar destructor retains delete flag before PUSH",
        """    (void)0; /* test MEM8(esp + 4), 1 - flags set for next jcc */
    PUSH32(esp, esi);
    esi = ecx;
    MEM32(esi) = 0x302ED4;
    if (TEST_Z(MEM8(esp + 4), 1)) goto loc_0029DDF3; /* je: equal / zero */
""",
        """    (void)0; /* test MEM8(esp + 4), 1 - flags set for next jcc */
    _flags = (TEST_Z(MEM8(esp + 4), 1)); /* preserve test flags across 3 instruction(s) */
    PUSH32(esp, esi);
    esi = ecx;
    MEM32(esi) = 0x302ED4;
    if (_flags != 0) goto loc_0029DDF3; /* je: equal / zero */
""",
    ),
    GeneratedPatch(
        "DirectSound update retains deferred flag across POP EBX",
        """    (void)0; /* test MEM8(esp + 0x18), 1 - flags set for next jcc */
    POP32(esp, ebx);
    if (TEST_NZ(MEM8(esp + 0x18), 1)) goto loc_0029FBB1; /* jne: not equal / not zero */
""",
        """    (void)0; /* test MEM8(esp + 0x18), 1 - flags set for next jcc */
    _flags = (TEST_NZ(MEM8(esp + 0x18), 1)); /* preserve test flags across 1 instruction(s) */
    POP32(esp, ebx);
    if (_flags != 0) goto loc_0029FBB1; /* jne: not equal / not zero */
""",
    ),
    GeneratedPatch(
        "DirectSound voice mode retains flags before EDI reload",
        """    (void)0; /* test MEM32(edi + 8), 0x200010 - flags set for next jcc */
    edi = MEM32(ebp + -32);
    if (TEST_Z(MEM32(edi + 8), 0x200010)) goto loc_002A3F02; /* je: equal / zero */
""",
        """    (void)0; /* test MEM32(edi + 8), 0x200010 - flags set for next jcc */
    _flags = (TEST_Z(MEM32(edi + 8), 0x200010)); /* preserve test flags across 1 instruction(s) */
    edi = MEM32(ebp + -32);
    if (_flags != 0) goto loc_002A3F02; /* je: equal / zero */
""",
    ),
    GeneratedPatch(
        "DirectSound operation argument survives implicit ESP change",
        """    (void)0; /* cmp MEM32(esp + 4), 0 - flags set for next jcc */
    PUSH32(esp, esi);
    esi = ecx;
    if (CMP_NE(MEM32(esp + 4), 0)) goto loc_002A58DD; /* jne: not equal / not zero */
""",
        """    (void)0; /* cmp MEM32(esp + 4), 0 - flags set for next jcc */
    _flags = (CMP_NE(MEM32(esp + 4), 0)); /* preserve cmp flags across 2 instruction(s) */
    PUSH32(esp, esi);
    esi = ecx;
    if (_flags != 0) goto loc_002A58DD; /* jne: not equal / not zero */
""",
    ),
    GeneratedPatch(
        "XACT resource accounting preserves comparison before EAX reload",
        """    (void)0; /* cmp MEM32(eax + 0x1C), 0 - flags set for next jcc */
    eax = ZX16(MEM16(eax + 0x10));
    if (CMP_EQ(MEM32(eax + 0x1C), 0)) goto loc_0027DF19; /* je: equal / zero */
""",
        """    (void)0; /* cmp MEM32(eax + 0x1C), 0 - flags set for next jcc */
    _flags = (CMP_EQ(MEM32(eax + 0x1C), 0)); /* preserve cmp flags across 1 instruction(s) */
    eax = ZX16(MEM16(eax + 0x10));
    if (_flags != 0) goto loc_0027DF19; /* je: equal / zero */
""",
    ),
    GeneratedPatch(
        "XACT variation bitmap preserves TEST before ECX reload",
        """    (void)0; /* test MEM32(edi + ecx * 4), edx - flags set for next jcc */
    ecx = MEM32(ebp + 0xC);
    if (TEST_Z(MEM32(edi + ecx * 4), edx)) goto loc_0027F89F; /* je: equal / zero */
""",
        """    (void)0; /* test MEM32(edi + ecx * 4), edx - flags set for next jcc */
    _flags = (TEST_Z(MEM32(edi + ecx * 4), edx)); /* preserve test flags across 1 instruction(s) */
    ecx = MEM32(ebp + 0xC);
    if (_flags != 0) goto loc_0027F89F; /* je: equal / zero */
""",
    ),
    GeneratedPatch(
        "D3D resource access flags survive implicit ESP change",
        """loc_0028C000: ;
    (void)0; /* test MEM8(esp + 8), 0xA0 - flags set for next jcc */
    PUSH32(esp, esi);
    esi = MEM32(esp + 8);
    if (TEST_NZ(MEM8(esp + 8), 0xA0)) goto loc_0028C012; /* jne: not equal / not zero */
""",
        """loc_0028C000: ;
    (void)0; /* test MEM8(esp + 8), 0xA0 - flags set for next jcc */
    _flags = (TEST_NZ(MEM8(esp + 8), 0xA0)); /* preserve test flags across 2 instruction(s) */
    PUSH32(esp, esi);
    esi = MEM32(esp + 8);
    if (_flags != 0) goto loc_0028C012; /* jne: not equal / not zero */
""",
    ),
    GeneratedPatch(
        'Effect-joint eligibility preserves incoming CMP 000B2885',
        'loc_000B2885: ;\n    (void)0; /* cmp eax, 0xD5CD1B6 - flags set for next jcc */\n    goto loc_000B2937;\n',
        'loc_000B2885: ;\n    /* Preserve this incoming CMP at the shared effect-joint return. */\n    if (CMP_EQ(eax, 0xD5CD1B6)) goto loc_000B293E;\n    goto loc_000B2939;\n',
    ),
    GeneratedPatch(
        'Effect-joint eligibility preserves incoming CMP 000B289A',
        'loc_000B289A: ;\n    (void)0; /* cmp eax, 0x105CD66F - flags set for next jcc */\n    goto loc_000B2937;\n',
        'loc_000B289A: ;\n    /* Preserve this incoming CMP at the shared effect-joint return. */\n    if (CMP_EQ(eax, 0x105CD66F)) goto loc_000B293E;\n    goto loc_000B2939;\n',
    ),
    GeneratedPatch(
        'Effect-joint eligibility preserves incoming CMP 000B28C3',
        'loc_000B28C3: ;\n    (void)0; /* cmp eax, 0x28CB2231 - flags set for next jcc */\n    goto loc_000B2937;\n',
        'loc_000B28C3: ;\n    /* Preserve this incoming CMP at the shared effect-joint return. */\n    if (CMP_EQ(eax, 0x28CB2231)) goto loc_000B293E;\n    goto loc_000B2939;\n',
    ),
    GeneratedPatch(
        'Effect-joint eligibility preserves incoming CMP 000B28D1',
        'loc_000B28D1: ;\n    (void)0; /* cmp eax, 0x7294E511 - flags set for next jcc */\n    goto loc_000B2937;\n',
        'loc_000B28D1: ;\n    /* Preserve this incoming CMP at the shared effect-joint return. */\n    if (CMP_EQ(eax, 0x7294E511)) goto loc_000B293E;\n    goto loc_000B2939;\n',
    ),
    GeneratedPatch(
        'Effect-joint eligibility preserves incoming CMP 000B28F8',
        'loc_000B28F8: ;\n    (void)0; /* cmp eax, 0x81CF83C9u - flags set for next jcc */\n    goto loc_000B2937;\n',
        'loc_000B28F8: ;\n    /* Preserve this incoming CMP at the shared effect-joint return. */\n    if (CMP_EQ(eax, 0x81CF83C9u)) goto loc_000B293E;\n    goto loc_000B2939;\n',
    ),
    GeneratedPatch(
        'Effect-joint eligibility preserves incoming CMP 000B2906',
        'loc_000B2906: ;\n    (void)0; /* cmp eax, 0xA4896014u - flags set for next jcc */\n    goto loc_000B2937;\n',
        'loc_000B2906: ;\n    /* Preserve this incoming CMP at the shared effect-joint return. */\n    if (CMP_EQ(eax, 0xA4896014u)) goto loc_000B293E;\n    goto loc_000B2939;\n',
    ),
    GeneratedPatch(
        'Effect-joint eligibility preserves incoming CMP 000B2924',
        'loc_000B2924: ;\n    (void)0; /* cmp eax, 0xDB286AFAu - flags set for next jcc */\n    goto loc_000B2937;\n',
        'loc_000B2924: ;\n    /* Preserve this incoming CMP at the shared effect-joint return. */\n    if (CMP_EQ(eax, 0xDB286AFAu)) goto loc_000B293E;\n    goto loc_000B2939;\n',
    ),
    GeneratedPatch(
        "D3D texture-state dispatch inherits predecessor comparison",
        """loc_0028D627: ;
    if ((ecx != 0)) goto loc_0028D65C; /* jne: not equal / not zero */
""",
        """loc_0028D627: ;
    if (CMP_NE(eax, 0xC)) goto loc_0028D65C; /* jne: not equal / not zero */
""",
    ),
    GeneratedPatch(
        "RedRenderer special render-state dispatch inherits predecessor comparison",
        """loc_002102B4: ;
    if ((ecx != 0)) goto loc_002102BF; /* jne: not equal / not zero */
""",
        """loc_002102B4: ;
    if (CMP_NE(esi, 0x88)) goto loc_002102BF; /* jne: not equal / not zero */
""",
    ),
    GeneratedPatch(
        "D3D texture-state dirty-mask carry follows original CMP",
        """    (void)0; /* cmp eax, 0x19 - flags set for next jcc */
    ecx = _cf ? 0xFFFFFFFF : 0; /* sbb self (CF extend) */
""",
        """    _cf = ((uint32_t)(eax) < (uint32_t)(0x19)); /* preserve cmp carry across 0 instruction(s) */
    ecx = _cf ? 0xFFFFFFFF : 0; /* sbb self (CF extend) */
""",
    ),
    GeneratedPatch(
        "D3D context mask survives callee-saved register restoration",
        """    (void)0; /* test MEM32(esi + 0x104), ecx - flags set for next jcc */
    POP32(esp, esi);
    POP32(esp, ebx);
    if (TEST_Z(MEM32(esi + 0x104), ecx)) goto loc_002954D8; /* je: equal / zero */
""",
        """    (void)0; /* test MEM32(esi + 0x104), ecx - flags set for next jcc */
    _flags = (TEST_Z(MEM32(esi + 0x104), ecx)); /* preserve test flags across 2 instruction(s) */
    POP32(esp, esi);
    POP32(esp, ebx);
    if (_flags != 0) goto loc_002954D8; /* je: equal / zero */
""",
    ),
    GeneratedPatch(
        "secondary inventory capacity inherits actual predecessor CMP flags",
        """loc_0005B332: ;
    edi = MEM32(ebp + 8);
    if (((int8_t)((LO8(eax) & LO8(eax))) >= (int8_t)(0))) goto loc_0005B344; /* jge: greater or equal (signed >=) */
""",
        """loc_0005B332: ;
    edi = MEM32(ebp + 8);
    if (CMP_GE(eax, 2)) goto loc_0005B344; /* jge: greater or equal (signed >=) */
""",
    ),
    GeneratedPatch(
        "PblThread scalar destructor delete flag survives implicit ESP change",
        """loc_001FA0D0: ;
    (void)0; /* test MEM8(esp + 4), 1 - flags set for next jcc */
    PUSH32(esp, esi);
    esi = ecx;
    MEM32(esi) = 0x300DE0;
    if (TEST_Z(MEM8(esp + 4), 1)) goto loc_001FA0E9; /* je: equal / zero */
""",
        """loc_001FA0D0: ;
    (void)0; /* test MEM8(esp + 4), 1 - flags set for next jcc */
    _flags = (TEST_Z(MEM8(esp + 4), 1)); /* preserve test flags across 3 instruction(s) */
    PUSH32(esp, esi);
    esi = ecx;
    MEM32(esi) = 0x300DE0;
    if (_flags != 0) goto loc_001FA0E9; /* je: equal / zero */
""",
    ),
    GeneratedPatch(
        "DirectSound hardware-voice flags survive restored address register",
        """    edx = edx << 7;
    (void)0; /* test MEM32(edx + esi + 4), 0x800000 - flags set for next jcc */
    POP32(esp, esi);
    if (TEST_NZ(MEM32(edx + esi + 4), 0x800000)) goto loc_002A1259; /* jne: not equal / not zero */
""",
        """    edx = edx << 7;
    (void)0; /* test MEM32(edx + esi + 4), 0x800000 - flags set for next jcc */
    _flags = (TEST_NZ(MEM32(edx + esi + 4), 0x800000)); /* preserve test flags across 1 instruction(s) */
    POP32(esp, esi);
    if (_flags != 0) goto loc_002A1259; /* jne: not equal / not zero */
""",
    ),
    GeneratedPatch(
        "DirectSound parent-voice status flags survive restored address register",
        """    esi = MEM32(edx + 0x70);
    (void)0; /* test MEM32(esi + 8), 0x82000 - flags set for next jcc */
    POP32(esp, esi);
    if (TEST_Z(MEM32(esi + 8), 0x82000)) goto loc_002A362E; /* je: equal / zero */
""",
        """    esi = MEM32(edx + 0x70);
    (void)0; /* test MEM32(esi + 8), 0x82000 - flags set for next jcc */
    _flags = (TEST_Z(MEM32(esi + 8), 0x82000)); /* preserve test flags across 1 instruction(s) */
    POP32(esp, esi);
    if (_flags != 0) goto loc_002A362E; /* je: equal / zero */
""",
    ),
    GeneratedPatch(
        "retail SetTexture invalid guest-resource guard",
        """void sub_00289E20(void)
{
    uint32_t ebp;
    int _flags = 0; /* fallback flag var */
    ebp = g_seh_ebp; /* inherit guest caller frame */
""",
        """void sub_00289E20(void)
{
    uint32_t ebp;
    int _flags = 0; /* fallback flag var */
    ebp = g_seh_ebp; /* inherit guest caller frame */
    MEM32(esp + 8) = recomp_validate_texture_bind(
        g_recomp_current_func, MEM32(esp + 4), MEM32(esp + 8));
""",
    ),
    GeneratedPatch(
        "retail Lua number formatter native boundary",
        """    PUSH32(esp, 0x2FBC98);
    PUSH32(esp, eax);
    PUSH32(esp, 0); sub_002370B8(); /* call 0x002370B8 */

loc_001E01AC: ;
""",
        """    recomp_native_format_lua_number(eax, MEMD(esp));
    PUSH32(esp, 0x2FBC98);
    PUSH32(esp, eax);

loc_001E01AC: ;
""",
    ),
    GeneratedPatch(
        "retail Lua concat penultimate number formatter native boundary",
        """    PUSH32(esp, 0x2FBC98);
    PUSH32(esp, eax);
    PUSH32(esp, 0); sub_002370B8(); /* call 0x002370B8 */

loc_001E09AF: ;
""",
        """    recomp_native_format_lua_number(eax, MEMD(esp));
    PUSH32(esp, 0x2FBC98);
    PUSH32(esp, eax);

loc_001E09AF: ;
""",
    ),
    GeneratedPatch(
        "retail Lua concat earlier number formatter native boundary",
        """    PUSH32(esp, 0x2FBC98);
    PUSH32(esp, eax);
    PUSH32(esp, 0); sub_002370B8(); /* call 0x002370B8 */

loc_001E0AAA: ;
""",
        """    recomp_native_format_lua_number(eax, MEMD(esp));
    PUSH32(esp, 0x2FBC98);
    PUSH32(esp, eax);

loc_001E0AAA: ;
""",
    ),
    GeneratedPatch(
        "retail CRT strtod native boundary",
        """    RECOMP_TRACE_FUNC(0x00237EE3u);

loc_00237EE3: ;
""",
        """    RECOMP_TRACE_FUNC(0x00237EE3u);

    fp_push(recomp_native_strtod(MEM32(esp + 4), MEM32(esp + 8)));
    esp += 4; return;

loc_00237EE3: ;
""",
    ),
    GeneratedPatch(
        "retail Lua longjmp host-frame transfer",
        """loc_00239390: ;
    ebx = MEM32(esp + 4);
""",
        """loc_00239390: ;
    if (recomp_lua_host_longjmp(MEM32(esp + 4), MEM32(esp + 8))) return;
    ebx = MEM32(esp + 4);
""",
    ),
    GeneratedPatch(
        "retail Lua protected-call host setjmp bridge",
        """loc_001DE998: ;
    esp = esp + 8;
""",
        """loc_001DE998: ;
    {
        const uint32_t _lua_jmpbuf = ebp + -68;
        const int _lua_jmp_result =
            RECOMP_LUA_HOST_SETJMP(_lua_jmpbuf);
        if (_lua_jmp_result != 0)
            eax = (uint32_t)_lua_jmp_result;
    }
    esp = esp + 8;
""",
    ),
    GeneratedPatch(
        "retail Lua protected-call normal host-jump pop",
        """    MEM32(esi + 0x58) = edx;
    POP32(esp, esi);
""",
        """    MEM32(esi + 0x58) = edx;
    recomp_lua_host_jmp_pop(ebp + -68);
    POP32(esp, esi);
""",
    ),
    GeneratedPatch(
        "retail Lua protected-call error host-jump pop",
        """    MEM32(eax + 0x58) = ecx;
    eax = MEM32(ebp + -4);
    POP32(esp, esi);
""",
        """    MEM32(eax + 0x58) = ecx;
    eax = MEM32(ebp + -4);
    recomp_lua_host_jmp_pop(ebp + -68);
    POP32(esp, esi);
""",
    ),
    GeneratedPatch(
        "retail RedModel roadblock soldier render diagnostics",
        """loc_001F5020: ;
    PUSH32(esp, esi);
    esi = ecx;
    ecx = MEM32(0x643844);
""",
        """loc_001F5020: ;
    PUSH32(esp, esi);
    esi = ecx;
    recomp_redmodel_render_checkpoint(
        0u, esi, MEM32(esi + 0x1Cu), MEM32(esi + 0x14u));
    ecx = MEM32(0x643844);
""",
    ),
    GeneratedPatch(
        "retail RedModel roadblock soldier resource diagnostics",
        """loc_001F505D: ;
    ecx = MEM32(eax);
    (void)0; /* test ecx, ecx - flags set for next jcc */
    MEM32(edi + 4) = ecx;
""",
        """loc_001F505D: ;
    ecx = MEM32(eax);
    (void)0; /* test ecx, ecx - flags set for next jcc */
    MEM32(edi + 4) = ecx;
    recomp_redmodel_render_checkpoint(
        1u, esi, MEM32(esi + 0x1Cu), ecx);
""",
    ),
    GeneratedPatch(
        "retail RedModel roadblock soldier early-draw diagnostics",
        """    edi = ecx;
    MEM32(esp + 0x30) = edi;
    if (TEST_Z(esi, esi)) goto loc_002215B7; /* je: equal / zero */
""",
        """    edi = ecx;
    MEM32(esp + 0x30) = edi;
    recomp_redmodel_render_checkpoint(
        2u, edi, esi, MEM32(edi + 0x80u));
    if (TEST_Z(esi, esi)) goto loc_002215B7; /* je: equal / zero */
""",
    ),
    GeneratedPatch(
        "retail RedModel roadblock soldier distance diagnostics",
        """    MEMF(esp + 0x18) = xmm0; /* movss */
    if (((isnan((double)(xmm2)) || isnan((double)(xmm0))) || (xmm2 < xmm0))) goto loc_002215B7; /* jb: below (unsigned <) */
""",
        """    MEMF(esp + 0x18) = xmm0; /* movss */
    recomp_redmodel_render_checkpoint(
        3u, edi, MEM32(esp + 0x18u), MEM32(ebp + 0x1Cu));
    if (((isnan((double)(xmm2)) || isnan((double)(xmm0))) || (xmm2 < xmm0))) goto loc_002215B7; /* jb: below (unsigned <) */
""",
    ),
    GeneratedPatch(
        "retail RedModel roadblock soldier frustum diagnostics",
        """loc_00220984: ;
    if (TEST_S(eax, eax)) goto loc_002215B7; /* jl: less (signed <) */
""",
        """loc_00220984: ;
    recomp_redmodel_render_checkpoint(4u, edi, eax, 0u);
    if (TEST_S(eax, eax)) goto loc_002215B7; /* jl: less (signed <) */
""",
    ),
    GeneratedPatch(
        "retail RedModel roadblock soldier LOD diagnostics",
        """loc_00220E2D: ;
    eax = MEM32(esp + 0x10);
    eax = eax | MEM32(esp + 0x24);
    MEM32(esp + 0x7C) = eax;
    goto loc_00220E43;
""",
        """loc_00220E2D: ;
    eax = MEM32(esp + 0x10);
    eax = eax | MEM32(esp + 0x24);
    MEM32(esp + 0x7C) = eax;
    recomp_redmodel_render_checkpoint(
        5u, MEM32(esp + 0x30u), eax,
        (uint32_t)(int32_t)SMEM16(MEM32(esp + 0x30u) + 0x78u));
    goto loc_00220E43;
""",
    ),
    GeneratedPatch(
        "retail RedModel roadblock soldier segment diagnostics",
        """    edx = (uint32_t)((int32_t)edx * (int32_t)0x58);
    edi = edx + ecx;
    if (TEST_Z(MEM8(edi + 0x54), LO8(eax))) goto loc_0022159E; /* je: equal / zero */
""",
        """    edx = (uint32_t)((int32_t)edx * (int32_t)0x58);
    edi = edx + ecx;
    recomp_redmodel_render_checkpoint(
        6u, MEM32(esp + 0x30u), edi, eax);
    if (TEST_Z(MEM8(edi + 0x54), LO8(eax))) goto loc_0022159E; /* je: equal / zero */
""",
    ),
    GeneratedPatch(
        "retail RedModel roadblock soldier texture diagnostics",
        """loc_00220E71: ;
    (void)0; /* cmp eax, ebx - flags set for next jcc */
    MEM32(esp + 0x78) = eax;
    if (CMP_EQ(eax, ebx)) goto loc_0022159E; /* je: equal / zero */
""",
        """loc_00220E71: ;
    (void)0; /* cmp eax, ebx - flags set for next jcc */
    MEM32(esp + 0x78) = eax;
    recomp_redmodel_render_checkpoint(
        7u, MEM32(esp + 0x30u), edi, eax);
    if (CMP_EQ(eax, ebx)) goto loc_0022159E; /* je: equal / zero */
""",
    ),
    GeneratedPatch(
        "retail RedModel roadblock soldier attribute diagnostics",
        """loc_00220FC1: ;
    SET_LO8(eax, MEM8(ebx + 0xC));
""",
        """loc_00220FC1: ;
    recomp_redmodel_render_checkpoint(
        8u, MEM32(esp + 0x30u), edi, ebx);
    SET_LO8(eax, MEM8(ebx + 0xC));
""",
    ),
    GeneratedPatch(
        "retail RedModel roadblock soldier render-item diagnostics",
        """loc_0022135A: ;
    ebx = MEM32(esp + 0x6C);
""",
        """loc_0022135A: ;
    recomp_redmodel_render_checkpoint(
        9u, MEM32(esp + 0x30u), edi, esi);
    ebx = MEM32(esp + 0x6C);
""",
    ),
    GeneratedPatch(
        "retail RedModel roadblock soldier submission diagnostics",
        """loc_002214AE: ;
    eax = MEM32(esp + 0x1C);
    MEM32(esi + 0x78) = eax;
""",
        """loc_002214AE: ;
    eax = MEM32(esp + 0x1C);
    MEM32(esi + 0x78) = eax;
    recomp_redmodel_render_checkpoint(
        10u, MEM32(esp + 0x30u), edi, esi);
""",
    ),
    GeneratedPatch(
        "retail RedRenderer roadblock queue dispatch diagnostics",
        """loc_002100E0: ;
    eax = MEM32(edi);
    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, esi);
    ecx = edi;
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(eax + 0xC), _icall_esp); /* indirect call */
    }
""",
        """loc_002100E0: ;
    eax = MEM32(edi);
    { const uint32_t _render_queue_esi = esi;
    const uint32_t _render_queue_edi = edi;
    const uint32_t _render_queue_esp = esp;
    const uint32_t _render_queue_target = MEM32(eax + 0xCu);
    uint32_t _icall_esp = g_esp;
    recomp_redmodel_queue_checkpoint(
        0u, ebp, ebx, esi, edi, _render_queue_target,
        _render_queue_esi, esi, _render_queue_edi, edi,
        _render_queue_esp, esp);
    PUSH32(esp, esi);
    ecx = edi;
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(_render_queue_target, _icall_esp); /* indirect call */
    recomp_redmodel_queue_checkpoint(
        1u, ebp, ebx, _render_queue_esi, _render_queue_edi,
        _render_queue_target, _render_queue_esi, esi,
        _render_queue_edi, edi, _render_queue_esp, esp);
    esi = _render_queue_esi;
    edi = _render_queue_edi;
    esp = _render_queue_esp;
    }
""",
    ),
    GeneratedPatch(
        "retail automatic building-gate AI diagnostics",
        """    edi = ecx;
    ebx = MEM32(edi + 0x10);
    (void)0; /* test ebx, ebx - flags set for next jcc */
""",
        """    edi = ecx;
    ebx = MEM32(edi + 0x10);
    recomp_gate_ai_checkpoint(0u, edi, ebx, 0u, 0u,
                              MEMF(esp + 0x1C), MEMF(edi + 0x590));
    (void)0; /* test ebx, ebx - flags set for next jcc */
""",
    ),
    GeneratedPatch(
        "retail automatic building-gate presence diagnostics",
        """loc_00071AAE: ;
    edx = MEM32(edi);
""",
        """loc_00071AAE: ;
    recomp_gate_ai_checkpoint(1u, edi, MEM32(edi + 0x10), esi, ebx,
                              MEMF(edi + 0x588), MEMF(edi + 0x590));
    edx = MEM32(edi);
""",
    ),
    GeneratedPatch(
        "retail automatic building-gate range diagnostics",
        """loc_00071ABA: ;
    recomp_xmm_loadss(xmm2v, esi + 0x18); /* movss */
    xmm2 = xmm2 - MEMF(eax + 8); /* subss */
    recomp_xmm_loadss(xmm1v, esi + 0x14); /* movss */
    xmm1 = xmm1 - MEMF(eax + 4); /* subss */
    recomp_xmm_loadss(xmm0v, esi + 0x10); /* movss */
    xmm0 = xmm0 - MEMF(eax); /* subss */
    recomp_xmm_loadss(xmm3v, edi + 0x590); /* movss */
    recomp_xmm_copy(xmm4v, xmm2v); /* movaps */
    xmm4 = xmm4 * xmm2; /* mulss */
    recomp_xmm_copy(xmm2v, xmm1v); /* movaps */
    xmm2 = xmm2 * xmm1; /* mulss */
    recomp_xmm_copy(xmm1v, xmm0v); /* movaps */
    xmm1 = xmm1 * xmm0; /* mulss */
    xmm4 = xmm4 + xmm2; /* addss */
    recomp_xmm_copy(xmm0v, xmm3v); /* movaps */
    xmm4 = xmm4 + xmm1; /* addss */
    xmm0 = xmm0 * xmm3; /* mulss */
    /* comiss xmm4, xmm0 - sets EFLAGS */
""",
        """loc_00071ABA: ;
    recomp_xmm_loadss(xmm2v, esi + 0x18); /* movss */
    xmm2 = xmm2 - MEMF(eax + 8); /* subss */
    recomp_xmm_loadss(xmm1v, esi + 0x14); /* movss */
    xmm1 = xmm1 - MEMF(eax + 4); /* subss */
    recomp_xmm_loadss(xmm0v, esi + 0x10); /* movss */
    xmm0 = xmm0 - MEMF(eax); /* subss */
    recomp_xmm_loadss(xmm3v, edi + 0x590); /* movss */
    recomp_xmm_copy(xmm4v, xmm2v); /* movaps */
    xmm4 = xmm4 * xmm2; /* mulss */
    recomp_xmm_copy(xmm2v, xmm1v); /* movaps */
    xmm2 = xmm2 * xmm1; /* mulss */
    recomp_xmm_copy(xmm1v, xmm0v); /* movaps */
    xmm1 = xmm1 * xmm0; /* mulss */
    xmm4 = xmm4 + xmm2; /* addss */
    recomp_xmm_copy(xmm0v, xmm3v); /* movaps */
    xmm4 = xmm4 + xmm1; /* addss */
    xmm0 = xmm0 * xmm3; /* mulss */
    recomp_gate_ai_checkpoint(2u, edi, MEM32(edi + 0x10), esi, ebx,
                              xmm4, xmm0);
    /* comiss xmm4, xmm0 - sets EFLAGS */
""",
    ),
    GeneratedPatch(
        "retail automatic building-gate open diagnostics",
        """loc_00071B7E: ;
    recomp_xmm_zero(xmm0v); /* xorps self = zero */
""",
        """loc_00071B7E: ;
    recomp_gate_ai_checkpoint(3u, edi, MEM32(edi + 0x10), esi,
                              0u, MEMF(edi + 0x588),
                              MEMF(edi + 0x580));
    recomp_xmm_zero(xmm0v); /* xorps self = zero */
""",
    ),
    GeneratedPatch(
        "retail building-gate open-state motion diagnostics",
        """loc_0002AFE4: ;
    ecx = esi + 0x6F0;
""",
        """loc_0002AFE4: ;
    recomp_gate_motion_checkpoint(0u, esi, 0u, 0u, 0u,
                                  0.0f, 0.0f, 0.0f);
    ecx = esi + 0x6F0;
""",
    ),
    GeneratedPatch(
        "retail building-gate resolved-panel diagnostics",
        """loc_0002B032: ;
    eax = MEM32(esp + 0x18);
""",
        """loc_0002B032: ;
    recomp_gate_motion_checkpoint(1u, MEM32(esp + 0x18), ebp, esi, 0u,
                                  0.0f, 0.0f, 0.0f);
    eax = MEM32(esp + 0x18);
""",
    ),
    GeneratedPatch(
        "retail building-gate panel-distance diagnostics",
        """loc_0002B058: ;
    recomp_xmm_loadss(xmm2v, eax + 8); /* movss */
""",
        """loc_0002B058: ;
    recomp_gate_motion_checkpoint(2u, MEM32(esp + 0x18), ebp, esi, 0u,
                                  MEMF(eax), MEMF(eax + 4), MEMF(eax + 8));
    recomp_xmm_loadss(xmm2v, eax + 8); /* movss */
""",
    ),
    GeneratedPatch(
        "retail building-gate rigid-body diagnostics",
        """loc_0002B0A8: ;
    ecx = MEM32(eax + 0x1C);
""",
        """loc_0002B0A8: ;
    recomp_gate_motion_checkpoint(3u, MEM32(esp + 0x18), ebp, esi, eax,
                                  xmm3, MEMF(0x2DE264), 0.0f);
    ecx = MEM32(eax + 0x1C);
""",
    ),
    GeneratedPatch(
        "retail building-gate force diagnostics",
        """loc_0002B0F1: ;
    fp_push(MEMF(esp + 0x24)); /* fld float */
""",
        """loc_0002B0F1: ;
    recomp_gate_motion_checkpoint(4u, MEM32(esp + 0x18), ebp, esi, eax,
                                  MEMF(esp + 0x14), MEMF(esi + 0x560),
                                  MEMF(esi + 0x568));
    fp_push(MEMF(esp + 0x24)); /* fld float */
""",
    ),
    GeneratedPatch(
        "Havok breakable constraint preserves mid-function EBX save",
        """loc_001C2592: ;
    ecx = MEM32(esi + 0x20);
    eax = MEM32(ecx);
    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, ebx);
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(eax + 0x14), _icall_esp); /* indirect call */
    }
""",
        """loc_001C2592: ;
    ecx = MEM32(esi + 0x20);
    eax = MEM32(ecx);
    { const uint32_t _nested_esi = esi;
    const uint32_t _saved_slot = g_esp + 4u;
    const uint32_t _saved_value = MEM32(_saved_slot);
    const uint32_t _nested_target = MEM32(eax + 0x14);
    PUSH32(esp, ebx);
    uint32_t _icall_esp = g_esp;
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(_nested_target, _icall_esp); /* indirect call */
    recomp_havok_nested_abi_checkpoint(0u, _nested_target,
        _nested_esi, esi, _saved_slot, _saved_value, MEM32(_saved_slot),
        _icall_esp, g_esp);
    g_esp = _icall_esp;
    MEM32(_saved_slot) = _saved_value;
    esi = _nested_esi;
    }
""",
    ),
    GeneratedPatch(
        "Havok breakable constraint nested size-call ABI guard",
        """loc_001C259B: ;
    ecx = MEM32(esi + 0x20);
    edx = MEM32(ecx);
    ebx = eax;
    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(edx + 0x18), _icall_esp); /* indirect call */
    }
""",
        """loc_001C259B: ;
    ecx = MEM32(esi + 0x20);
    edx = MEM32(ecx);
    ebx = eax;
    { const uint32_t _nested_esi = esi;
    const uint32_t _saved_slot = g_esp + 8u;
    const uint32_t _saved_value = MEM32(_saved_slot);
    const uint32_t _nested_target = MEM32(edx + 0x18);
    uint32_t _icall_esp = g_esp;
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(_nested_target, _icall_esp); /* indirect call */
    recomp_havok_nested_abi_checkpoint(1u, _nested_target,
        _nested_esi, esi, _saved_slot, _saved_value, MEM32(_saved_slot),
        _icall_esp, g_esp);
    g_esp = _icall_esp;
    MEM32(_saved_slot) = _saved_value;
    esi = _nested_esi;
    }
""",
    ),
    GeneratedPatch(
        "Havok breakable constraint callback ABI guard",
        """    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, edx);
    MEMF(esp + 0x10) = (float)fp_top(); fp_pop(); /* fstp */
    eax = MEM32(ecx);
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(eax), _icall_esp); /* indirect call */
    }
""",
        """    { const uint32_t _nested_esi = esi;
    const uint32_t _saved_slot = g_esp + 4u;
    const uint32_t _saved_value = MEM32(_saved_slot);
    uint32_t _icall_esp = g_esp;
    PUSH32(esp, edx);
    MEMF(esp + 0x10) = (float)fp_top(); fp_pop(); /* fstp */
    eax = MEM32(ecx);
    const uint32_t _nested_target = MEM32(eax);
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(_nested_target, _icall_esp); /* indirect call */
    recomp_havok_nested_abi_checkpoint(2u, _nested_target,
        _nested_esi, esi, _saved_slot, _saved_value, MEM32(_saved_slot),
        _icall_esp, g_esp);
    g_esp = _icall_esp;
    MEM32(_saved_slot) = _saved_value;
    esi = _nested_esi;
    }
""",
    ),
    GeneratedPatch(
        "Havok wheel constraint keeps SEH prologue outside ICALL cleanup",
        """    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, 0xFFFFFFFFu);
    PUSH32(esp, 0x243308);
    eax = MEM32(0);
    PUSH32(esp, eax);
    MEM32(0) = esp;
    esp = esp - 0xC;
    PUSH32(esp, esi);
    esi = ecx;
    MEM32(esp + 4) = 0;
    MEM32(esp + 8) = 0;
    MEM32(esp + 0xC) = 0x80000000u;
    eax = MEM32(esp + 0x20);
    ecx = MEM32(eax);
    edx = MEM32(ecx);
    eax = esp + 4;
    PUSH32(esp, eax);
    MEM32(esp + 0x1C) = 0;
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(edx + 0x1C), _icall_esp); /* indirect call */
    }
""",
        """    { PUSH32(esp, 0xFFFFFFFFu);
    PUSH32(esp, 0x243308);
    eax = MEM32(0);
    PUSH32(esp, eax);
    MEM32(0) = esp;
    esp = esp - 0xC;
    PUSH32(esp, esi);
    esi = ecx;
    MEM32(esp + 4) = 0;
    MEM32(esp + 8) = 0;
    MEM32(esp + 0xC) = 0x80000000u;
    eax = MEM32(esp + 0x20);
    ecx = MEM32(eax);
    edx = MEM32(ecx);
    eax = esp + 4;
    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, eax);
    MEM32(esp + 0x1C) = 0;
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(edx + 0x1C), _icall_esp); /* indirect call */
    }
    }
""",
    ),
    GeneratedPatch(
        "Havok rigid-body motion setter keeps aligned SEH prologue outside ICALL cleanup",
        """loc_001C4100: ;
    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, ebp);
    ebp = esp;
    esp = esp & 0xFFFFFFF0u;
    PUSH32(esp, 0xFFFFFFFFu);
    PUSH32(esp, 0x245CBE);
    eax = MEM32(0);
    PUSH32(esp, eax);
    MEM32(0) = esp;
    esp = esp - 0x68;
    PUSH32(esp, ebx);
    PUSH32(esp, esi);
    esi = ecx;
    ecx = MEM32(esi + 0x3C);
    eax = MEM32(ecx);
    PUSH32(esp, edi);
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(eax + 0x18), _icall_esp); /* indirect call */
    }
""",
        """loc_001C4100: ;
    { PUSH32(esp, ebp);
    ebp = esp;
    esp = esp & 0xFFFFFFF0u;
    PUSH32(esp, 0xFFFFFFFFu);
    PUSH32(esp, 0x245CBE);
    eax = MEM32(0);
    PUSH32(esp, eax);
    MEM32(0) = esp;
    esp = esp - 0x68;
    PUSH32(esp, ebx);
    PUSH32(esp, esi);
    esi = ecx;
    ecx = MEM32(esi + 0x3C);
    eax = MEM32(ecx);
    PUSH32(esp, edi);
    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(eax + 0x18), _icall_esp); /* indirect call */
    }
    }
""",
    ),
    GeneratedPatch(
        "Havok constraint builder nonvolatile ESI guard",
        """    ecx = MEM32(esi);
    edx = MEM32(ecx);
    ecx = esp + 0x1C;
    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, ecx);
    ecx = MEM32(esi);
    PUSH32(esp, eax);
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(edx + 0x24), _icall_esp); /* indirect call */
    }
""",
        """    ecx = MEM32(esi);
    edx = MEM32(ecx);
    ecx = esp + 0x1C;
    { const uint32_t _constraint_esi = esi;
    const uint32_t _constraint = MEM32(esi);
    const uint32_t _constraint_target = MEM32(edx + 0x24);
    uint32_t _icall_esp = g_esp;
    PUSH32(esp, ecx);
    ecx = MEM32(esi);
    PUSH32(esp, eax);
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(_constraint_target, _icall_esp); /* indirect call */
    recomp_havok_constraint_esi_checkpoint(_constraint, _constraint_target,
                                           _constraint_esi, esi);
    esi = _constraint_esi;
    }
""",
    ),
    GeneratedPatch(
        "HumanGeneral virtual update ESI guard",
        """loc_000539AB: ;
    edx = MEM32(esi);
    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(edx + 0x260), _icall_esp); /* indirect call */
    }
""",
        """loc_000539AB: ;
    edx = MEM32(esi);
    { const uint32_t _human_general_virtual_esi = esi;
    uint32_t _icall_esp = g_esp;
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(edx + 0x260), _icall_esp); /* indirect call */
    recomp_update_esi_checkpoint(0x000539ABu,
                                 _human_general_virtual_esi, esi);
    esi = _human_general_virtual_esi;
    }
""",
    ),
    GeneratedPatch(
        "HumanGeneral update nested callback ESI guard",
        """loc_00053A06: ;
    PUSH32(esp, edi);
    ecx = esi;
    PUSH32(esp, 0); sub_0004F1B0(); /* call 0x0004F1B0 */
""",
        """loc_00053A06: ;
    { const uint32_t _human_general_esi = esi;
    PUSH32(esp, edi);
    ecx = esi;
    PUSH32(esp, 0); sub_0004F1B0(); /* call 0x0004F1B0 */
    recomp_update_esi_checkpoint(0x000538F0u,
                                 _human_general_esi, esi);
    esi = _human_general_esi;
    }
""",
    ),
    GeneratedPatch(
        "HumanGeneral animation pointer before update tail",
        """loc_00053A16: ;
    eax = MEM32(esi + 0x79C);
""",
        """loc_00053A16: ;
    recomp_human_animation_pointer_checkpoint(0u, esi);
    eax = MEM32(esi + 0x79C);
""",
    ),
    GeneratedPatch(
        "retail vertical-aim StepToValue merged-comiss clamp",
        """loc_00062857: ;
    xmm0 = xmm0 + xmm2; /* addss */
    /* comiss xmm0, xmm1 - sets EFLAGS */
    goto loc_0006286C;
""",
        """loc_00062857: ;
    xmm0 = xmm0 + xmm2; /* addss */
    /* comiss xmm0, xmm1 - sets EFLAGS */
    MEMF(esp + 4) = xmm0; /* movss */
    if (((isnan((double)(xmm0)) || isnan((double)(xmm1))) || (xmm0 <= xmm1))) goto loc_0006287D; /* jbe: below or equal (unsigned <=) */
    goto loc_00062874;
""",
    ),
    GeneratedPatch(
        "retail StepToValue merged-comiss head clamp",
        """loc_0006261A: ;
    xmm0 = xmm0 + xmm2; /* addss */
    /* comiss xmm0, xmm1 - sets EFLAGS */
    goto loc_0006262F;

loc_00062623: ;
    /* comiss xmm0, xmm1 - sets EFLAGS */
    if (((isnan((double)(xmm0)) || isnan((double)(xmm1))) || (xmm0 <= xmm1))) goto loc_00062634; /* jbe: below or equal (unsigned <=) */

loc_00062628: ;
    xmm0 = xmm0 - xmm2; /* subss */
    /* comiss xmm1, xmm0 - sets EFLAGS */

loc_0006262F: ;
    if (((isnan((double)(xmm1)) || isnan((double)(xmm0))) || (xmm1 <= xmm0))) goto loc_00062634; /* jbe: below or equal (unsigned <=) */

loc_00062631: ;
""",
        """loc_0006261A: ;
    xmm0 = xmm0 + xmm2; /* addss */
    /* comiss xmm0, xmm1 - sets EFLAGS */
    if (((isnan((double)(xmm0)) || isnan((double)(xmm1))) || (xmm0 <= xmm1))) goto loc_00062634; /* jbe: below or equal (unsigned <=) */
    goto loc_00062631;

loc_00062623: ;
    /* comiss xmm0, xmm1 - sets EFLAGS */
    if (((isnan((double)(xmm0)) || isnan((double)(xmm1))) || (xmm0 <= xmm1))) goto loc_00062634; /* jbe: below or equal (unsigned <=) */

loc_00062628: ;
    xmm0 = xmm0 - xmm2; /* subss */
    /* comiss xmm1, xmm0 - sets EFLAGS */
    if (((isnan((double)(xmm1)) || isnan((double)(xmm0))) || (xmm1 <= xmm0))) goto loc_00062634; /* jbe: below or equal (unsigned <=) */

loc_00062631: ;
""",
    ),
    GeneratedPatch(
        "retail human head tweak state diagnostics",
        """    PUSH32(esp, esi);
    esi = ecx;
    SET_LO8(eax, MEM8(esi + 0x2981));
""",
        """    PUSH32(esp, esi);
    esi = ecx;
    recomp_human_head_checkpoint(0u, esi, MEM32(ebp + 8u));
    SET_LO8(eax, MEM8(esi + 0x2981));
""",
    ),
    GeneratedPatch(
        "retail human head tweak result diagnostics",
        """    MEMF(esi + 0x2938) = xmm0; /* movss */
    if (!EVEN_PARITY8((uint8_t)(HI8(eax) & 0x44))) goto loc_0006269E; /* jnp: not parity */
""",
        """    MEMF(esi + 0x2938) = xmm0; /* movss */
    recomp_human_head_checkpoint(1u, esi, MEM32(ebp + 8u));
    if (!EVEN_PARITY8((uint8_t)(HI8(eax) & 0x44))) goto loc_0006269E; /* jnp: not parity */
""",
    ),
    GeneratedPatch(
        "HumanGeneral animation pointer after aim update",
        """loc_00053A43: ;
    recomp_xmm_zero(xmm0v); /* xorps self = zero */
""",
        """loc_00053A43: ;
    recomp_human_animation_pointer_checkpoint(1u, esi);
    recomp_xmm_zero(xmm0v); /* xorps self = zero */
""",
    ),
    GeneratedPatch(
        "HumanGeneral animation pointer after renderable update",
        """loc_00053A56: ;
    edx = MEM32(esi);
""",
        """loc_00053A56: ;
    recomp_human_animation_pointer_checkpoint(2u, esi);
    edx = MEM32(esi);
""",
    ),
    GeneratedPatch(
        "HumanGeneral animation pointer after vcall 410",
        """loc_00053A61: ;
    eax = MEM32(esi);
""",
        """loc_00053A61: ;
    recomp_human_animation_pointer_checkpoint(3u, esi);
    eax = MEM32(esi);
""",
    ),
    GeneratedPatch(
        "HumanGeneral animation pointer after vcall 3E4",
        """loc_00053A6B: ;
    edx = MEM32(esi);
""",
        """loc_00053A6B: ;
    recomp_human_animation_pointer_checkpoint(4u, esi);
    edx = MEM32(esi);
""",
    ),
    GeneratedPatch(
        "HumanGeneral vtable 3EC callback nonvolatile guard",
        """loc_00053A6B: ;
    recomp_human_animation_pointer_checkpoint(4u, esi);
    edx = MEM32(esi);
    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, edi);
    ecx = esi;
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(edx + 0x3EC), _icall_esp); /* indirect call */
    }
""",
        """loc_00053A6B: ;
    recomp_human_animation_pointer_checkpoint(4u, esi);
    edx = MEM32(esi);
    { const uint32_t _human_3ec_saved_ebx = ebx;
    const uint32_t _human_3ec_saved_esi = esi;
    const uint32_t _human_3ec_saved_edi = edi;
    uint32_t _icall_esp = g_esp;
    PUSH32(esp, edi);
    ecx = esi;
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(edx + 0x3EC), _icall_esp); /* indirect call */
    recomp_update_esi_checkpoint(0x00053A6Bu,
                                 _human_3ec_saved_esi, esi);
    ebx = _human_3ec_saved_ebx;
    esi = _human_3ec_saved_esi;
    edi = _human_3ec_saved_edi;
    }
""",
    ),
    GeneratedPatch(
        "HumanGeneral animation pointer before pose use",
        """loc_00053A76: ;
    eax = MEM32(esi + 0x79C);
""",
        """loc_00053A76: ;
    recomp_human_animation_pointer_checkpoint(5u, esi);
    eax = MEM32(esi + 0x79C);
""",
    ),
    GeneratedPatch(
        "HumanGeneral pose-state virtual callback nonvolatile guard",
        """loc_00053A80: ;
    eax = MEM32(esi);
    ecx = esi;
    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(eax + 0x324), _icall_esp); /* indirect call */
    }
""",
        """loc_00053A80: ;
    eax = MEM32(esi);
    ecx = esi;
    { const uint32_t _human_pose_saved_ebx = ebx;
    const uint32_t _human_pose_saved_esi = esi;
    const uint32_t _human_pose_saved_edi = edi;
    uint32_t _icall_esp = g_esp;
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(eax + 0x324), _icall_esp); /* indirect call */
    recomp_update_esi_checkpoint(0x00053A80u,
                                 _human_pose_saved_esi, esi);
    ebx = _human_pose_saved_ebx;
    esi = _human_pose_saved_esi;
    edi = _human_pose_saved_edi;
    }
""",
    ),
    GeneratedPatch(
        "retail transient updater virtual callback ABI guard",
        """loc_000974C1: ;
    eax = MEM32(edi);
    ecx = esp + 8;
    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, ecx);
    ecx = edi;
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(eax + 0xC8), _icall_esp); /* indirect call */
    }
""",
        """loc_000974C1: ;
    eax = MEM32(edi);
    ecx = esp + 8;
    { const uint32_t _transient_esi = esi;
    const uint32_t _transient_edi = edi;
    const uint32_t _transient_esp = esp;
    uint32_t _icall_esp = g_esp;
    PUSH32(esp, ecx);
    ecx = edi;
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(eax + 0xC8), _icall_esp); /* indirect call */
    recomp_transient_update_abi_checkpoint(
        0x000974C1u, _transient_esi, esi, _transient_edi, edi,
        _transient_esp, esp);
    esi = _transient_esi;
    edi = _transient_edi;
    esp = _transient_esp;
    }
""",
    ),
    GeneratedPatch(
        "retail vehicle transition nonvolatile ESI snapshot",
        """void sub_0002E4D0(void)
{
    uint32_t ebp;
""",
        """void sub_0002E4D0(void)
{
    uint32_t ebp;
    const uint32_t _saved_esi_nonvolatile = esi;
""",
    ),
    GeneratedPatch(
        "retail vehicle transition nonvolatile ESI restore",
        """loc_0002E678: ;
    POP32(esp, edi);
    POP32(esp, esi);
    POP32(esp, ebp);
""",
        """loc_0002E678: ;
    POP32(esp, edi);
    recomp_update_esi_checkpoint(0x0002E4D0u,
                                 _saved_esi_nonvolatile, MEM32(esp));
    POP32(esp, esi);
    esi = _saved_esi_nonvolatile;
    POP32(esp, ebp);
""",
    ),
    GeneratedPatch(
        "retail property updater nonvolatile ESI snapshot",
        """void sub_00012520(void)
{
    uint32_t ebp;
""",
        """void sub_00012520(void)
{
    uint32_t ebp;
    const uint32_t _saved_esi_nonvolatile = esi;
""",
    ),
    GeneratedPatch(
        "retail property updater nonvolatile ESI restore",
        """loc_00012700: ;
    POP32(esp, esi);
    esp = ebp;
""",
        """loc_00012700: ;
    recomp_update_esi_checkpoint(0x00012520u,
                                 _saved_esi_nonvolatile, MEM32(esp));
    POP32(esp, esi);
    esi = _saved_esi_nonvolatile;
    esp = ebp;
""",
    ),
    GeneratedPatch(        "Camera Mario64 mode transition checkpoint",
        """    RECOMP_TRACE_FUNC(0x000976E0u);

loc_000976E0: ;
""",
        """    RECOMP_TRACE_FUNC(0x000976E0u);
    recomp_camera_mode_checkpoint(0x000976E0u);

loc_000976E0: ;
""",
    ),
    GeneratedPatch(
        "Camera cinematic mode transition checkpoint",
        """    RECOMP_TRACE_FUNC(0x000977B0u);

loc_000977B0: ;
""",
        """    RECOMP_TRACE_FUNC(0x000977B0u);
    recomp_camera_mode_checkpoint(0x000977B0u);

loc_000977B0: ;
""",
    ),
    GeneratedPatch(
        "Camera Mario64 post-Enter checkpoint",
        """    PUSH32(esp, 0); sub_00097550(); /* call 0x00097550 */

loc_000976F5: ;
""",
        """    PUSH32(esp, 0); sub_00097550(); /* call 0x00097550 */
    recomp_camera_mode_post_checkpoint(0x000976E0u);

loc_000976F5: ;
""",
    ),
    GeneratedPatch(
        "RsBriefing AddActor spawned actor checkpoint",
        """loc_00176250: ;
    esi = eax;
    if (TEST_Z(esi, esi)) goto loc_00176294; /* je: equal / zero */
""",
        """loc_00176250: ;
    esi = eax;
    recomp_briefing_actor_checkpoint(2u, MEM32(ebp + 8u),
                                     MEM32(ebp + 0xCu),
                                     MEM32(ebp + 0x10u), esi);
    if (TEST_Z(esi, esi)) goto loc_00176294; /* je: equal / zero */
""",
    ),
    GeneratedPatch(
        "RedXactCue managed return diagnostics",
        """loc_001FF793: ;
    eax = MEM32(esi + 4);
    POP32(esp, edi);
""",
        """loc_001FF793: ;
    eax = MEM32(esi + 4);
    recomp_xact_managed_checkpoint(3u, eax, MEM32(MEM32(esi + 0x34) + 4), esi);
    POP32(esp, edi);
""",
    ),
    GeneratedPatch(
        "CRT memmove backward MOVSD path 238E3C",
        """loc_00238E3C: ;
    /* std - direction flag */
    XBOX_MEMCPY(edi, esi, ecx * 4);
    esi += ecx * 4; edi += ecx * 4; ecx = 0; /* rep movsd */
""",
        """loc_00238E3C: ;
    /* std - direction flag */
    { uint32_t _i; for (_i = 0; _i < ecx; _i++) MEM32(edi - _i*4) = MEM32(esi - _i*4); }
    esi -= ecx * 4; edi -= ecx * 4; ecx = 0; /* rep movsd */
""",
    ),
    GeneratedPatch(
        "CRT memmove backward MOVSD path 238E6E",
        """loc_00238E6E: ;
    /* std - direction flag */
    XBOX_MEMCPY(edi, esi, ecx * 4);
    esi += ecx * 4; edi += ecx * 4; ecx = 0; /* rep movsd */
""",
        """loc_00238E6E: ;
    /* std - direction flag */
    { uint32_t _i; for (_i = 0; _i < ecx; _i++) MEM32(edi - _i*4) = MEM32(esi - _i*4); }
    esi -= ecx * 4; edi -= ecx * 4; ecx = 0; /* rep movsd */
""",
    ),
    GeneratedPatch(
        "RedXactManager cue-wrapper pool shared generated-handle flags",
        """loc_00226451: ;
    (void)0; /* test edi, edi - flags set for next jcc */

loc_00226453: ;
    if (TEST_NZ(edi, edi)) goto loc_00226499; /* jne: not equal / not zero */
""",
        """loc_00226451: ;
    _flags = TEST_NZ(edi, edi); /* preserve shared jne source */

loc_00226453: ;
    if (_flags != 0) goto loc_00226499; /* jne: not equal / not zero */
""",
    ),
    GeneratedPatch(
        "RedXactManager cue-wrapper pool regenerated-handle paths",
        """loc_00226486: ;
    (void)0; /* test edx, edx - flags set for next jcc */
    goto loc_00226453;

loc_0022648A: ;
    ecx = MEM32(esi + eax * 8 + 0x1C88);
    if (TEST_NZ(ecx, ecx)) goto loc_00226455; /* jne: not equal / not zero */

loc_00226495: ;
    (void)0; /* test edx, edx - flags set for next jcc */
    goto loc_00226453;
""",
        """loc_00226486: ;
    _flags = TEST_NZ(edx, edx); /* generated handle feeds shared jne */
    goto loc_00226453;

loc_0022648A: ;
    ecx = MEM32(esi + eax * 8 + 0x1C88);
    if (TEST_NZ(ecx, ecx)) goto loc_00226455; /* jne: not equal / not zero */

loc_00226495: ;
    _flags = TEST_NZ(edx, edx); /* generated handle feeds shared jne */
    goto loc_00226453;
""",
    ),
    GeneratedPatch(
        "RedXactCue pool shared generated-handle flags",
        """loc_001FF091: ;
    (void)0; /* test edi, edi - flags set for next jcc */

loc_001FF093: ;
    if (TEST_NZ(edi, edi)) goto loc_001FF0D9; /* jne: not equal / not zero */
""",
        """loc_001FF091: ;
    _flags = TEST_NZ(edi, edi); /* preserve shared jne source */

loc_001FF093: ;
    if (_flags != 0) goto loc_001FF0D9; /* jne: not equal / not zero */
""",
    ),
    GeneratedPatch(
        "RedXactCue pool regenerated-handle paths",
        """loc_001FF0C6: ;
    (void)0; /* test edx, edx - flags set for next jcc */
    goto loc_001FF093;

loc_001FF0CA: ;
    ecx = MEM32(esi + eax * 8 + 0x1448);
    if (TEST_NZ(ecx, ecx)) goto loc_001FF095; /* jne: not equal / not zero */

loc_001FF0D5: ;
    (void)0; /* test edx, edx - flags set for next jcc */
    goto loc_001FF093;
""",
        """loc_001FF0C6: ;
    _flags = TEST_NZ(edx, edx); /* generated handle feeds shared jne */
    goto loc_001FF093;

loc_001FF0CA: ;
    ecx = MEM32(esi + eax * 8 + 0x1448);
    if (TEST_NZ(ecx, ecx)) goto loc_001FF095; /* jne: not equal / not zero */

loc_001FF0D5: ;
    _flags = TEST_NZ(edx, edx); /* generated handle feeds shared jne */
    goto loc_001FF093;
""",
    ),
    GeneratedPatch(
        "RedSoundSystem omitted dataset wrappers",
        """void sub_001FFDA0(void) { esp += 4; /* 0x001FFDA0: not detected; minimal guest ret */ }
void sub_001FFDB0(void) { esp += 4; /* 0x001FFDB0: not detected; minimal guest ret */ }
""",
        """void sub_001FFDA0(void)
{
    eax = MEM32(esp + 4);
    PUSH32(esp, eax);
    PUSH32(esp, 0); sub_001FE7D0();
    esp += 4;
    esp += 4; return;
}

void sub_001FFDB0(void)
{
    eax = MEM32(esp + 4);
    PUSH32(esp, eax);
    PUSH32(esp, 0); sub_001FF450();
    esp += 4;
    if (MEM8(esp + 8) == 0u) {
        eax = MEM32(esp + 4);
        PUSH32(esp, eax);
        PUSH32(esp, 0); sub_00227660();
        esp += 4;
    }
    esp += 4; return;
}
""",
    ),
    GeneratedPatch(
        "RedSoundSystem omitted general bank loader wrapper",
        """loc_001FFE20: ;
    PUSH32(esp, 0);
    ecx = 0x85C600;
    PUSH32(esp, 0); sub_00225B90(); /* call 0x00225B90 */

loc_001FFE2C: ;
    esp += 4; return; /* ret */
""",
        """loc_001FFE20: ;
    PUSH32(esp, 0);
    ecx = 0x85C600;
    recomp_xact_bank_checkpoint(1u, ecx);
    PUSH32(esp, 0); sub_00225B90(); /* call 0x00225B90 */
    recomp_xact_bank_checkpoint(2u, 0x85C600u);

loc_001FFE2C: ;
    esp += 4; return; /* ret */
""",
    ),
    GeneratedPatch(
        "Camera object update missing retail shared continuation 0x40187",
        """void sub_00040187(void) { esp += 4; /* 0x00040187: not detected; minimal guest ret */ }
""",
        """
void sub_00040187(void)
{
    int _flags = 0;
    RECOMP_TRACE_FUNC(0x00040187u);

loc_00040187: ;
    SET_LO8(eax, MEM8(esi + 0x1AA));
    if (TEST_Z(LO8(eax), LO8(eax))) goto loc_000401E9;

loc_00040191: ;
    if (TEST_NZ(LO8(ebx), LO8(ebx))) goto loc_000401E9;

loc_00040195: ;
    eax = MEM32(esi);
    ecx = esp + 0x14;
    {
        uint32_t _icall_esp = g_esp;
        PUSH32(esp, ecx);
        ecx = esi;
        PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(eax + 0x34), _icall_esp);
    }

loc_000401A1: ;
    recomp_xmm_loadss(xmm1v, esi + 0x1B8);
    xmm1 = xmm1 - MEMF(eax + 8);
    recomp_xmm_loadss(xmm0v, esi + 0x1B0);
    xmm0 = xmm0 - MEMF(eax);
    recomp_xmm_copy(xmm2v, xmm1v);
    xmm2 = xmm2 * xmm1;
    recomp_xmm_copy(xmm1v, xmm0v);
    xmm1 = xmm1 * xmm0;
    recomp_xmm_loadss(xmm0v, 0x314300);
    xmm2 = xmm2 + xmm1;
    recomp_xmm_copy(xmm1v, xmm0v);
    xmm1 = xmm1 * xmm0;
    if (((isnan((double)(xmm2)) || isnan((double)(xmm1))) ||
         (xmm2 < xmm1))) goto loc_000401E9;

loc_000401E0: ;
    PUSH32(esp, 0);
    ecx = esi;
    PUSH32(esp, 0); sub_0003E9E0();

loc_000401E9: ;
    SET_LO8(eax, MEM8(esi + 0x1A9));
    POP32(esp, ebx);
    if (TEST_NZ(LO8(eax), LO8(eax))) {
        sub_00040260();
        return;
    }

loc_000401F4: ;
    SET_LO8(eax, MEM8(esi + 0x1AA));
    if (TEST_NZ(LO8(eax), LO8(eax))) {
        sub_00040260();
        return;
    }

loc_000401FE: ;
    SET_LO8(eax, MEM8(esp + 0xB));
    if (TEST_NZ(LO8(eax), LO8(eax))) goto loc_00040255;

loc_00040206: ;
    edx = MEM32(edi + 0x8BC);
    if (CMP_NE(MEM32(edx + 0x118), 6)) goto loc_00040255;

loc_00040215: ;
    eax = MEM32(esi + 0xF4);
    PUSH32(esp, eax);
    ecx = esi;
    PUSH32(esp, 0); sub_0003FDB0();

loc_00040223: ;
    if (TEST_NZ(LO8(eax), LO8(eax))) goto loc_00040255;

loc_00040227: ;
    recomp_xmm_loadss(xmm0v, esi + 0x1AC);
    xmm0 = xmm0 + MEMF(esp + 0x20);
    MEMF(esi + 0x1AC) = xmm0;
    if (((isnan((double)(xmm0)) || isnan((double)(MEMF(0x30C33C)))) ||
         (xmm0 < MEMF(0x30C33C)))) {
        sub_00040260();
        return;
    }

loc_00040246: ;
    ecx = esi;
    PUSH32(esp, 0); sub_0003FE00();
    sub_00040260();
    return;

loc_00040255: ;
    recomp_xmm_zero(xmm0v);
    MEMF(esi + 0x1AC) = xmm0;
    sub_00040260();
}


""",
    ),
    GeneratedPatch(
        "Camera update missing retail no-target continuation",
        """void sub_0008DC8D(void) { esp += 4; /* 0x0008DC8D: not detected; minimal guest ret */ }
""",
        """void sub_0008DC8D(void)
{
    /* Retail 0x0008DC8D is an interior branch of sub_0008DB70.  It
     * selects camera mode 8, then executes that function's shared
     * EDI/ESI/return epilogue. */
    PUSH32(esp, 8);
    ecx = esi + 0x9C4;
    PUSH32(esp, 0); sub_001546C0();
    POP32(esp, edi);
    POP32(esp, esi);
    esp += 4; return;
}
""",
    ),
    GeneratedPatch(
        "Camera update missing retail shared continuations",
        """void sub_0008DBFA(void) { esp += 4; /* 0x0008DBFA: not detected; minimal guest ret */ }
void sub_0008DC00(void) { esp += 4; /* 0x0008DC00: not detected; minimal guest ret */ }
""",
        """void sub_0008DBFA(void)
{
    ecx = esi + 0x9C4;
    sub_0008DC00(); return;
}

void sub_0008DC00(void)
{
    int _flags = 0;
    PUSH32(esp, 0); sub_001546C0();

    edx = MEM32(esi);
    ecx = esi;
    {
        uint32_t _icall_esp = g_esp;
        PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(edx + 0x144), _icall_esp);
    }
    if (TEST_Z(LO8(eax), LO8(eax))) goto loc_0008DC9A;

    if (CMP_NE(MEM32(edi + 0x1A0), 8)) goto loc_0008DC34;
    ecx = 0x323608;
    PUSH32(esp, 0);
    if (CMP_NE(MEM32(edi + 0x200), 1)) goto loc_0008DC5E;
    PUSH32(esp, 6);
    goto loc_0008DC6B;

loc_0008DC34: ;
    recomp_xmm_loadss(xmm0v, edi + 0x1BC);
    if (!(xmm0 >= MEMF(0x2DC08C))) goto loc_0008DC62;
    ecx = 0x4140E8;
    PUSH32(esp, 0); sub_000976A0();
    ecx = 0x323608;
    PUSH32(esp, 0);
    if (TEST_Z(LO8(eax), LO8(eax))) goto loc_0008DC5E;
    PUSH32(esp, 0xA);
    goto loc_0008DC6B;

loc_0008DC5E: ;
    PUSH32(esp, 9);
    goto loc_0008DC6B;

loc_0008DC62: ;
    PUSH32(esp, 0);
    PUSH32(esp, 4);
    ecx = 0x323608;

loc_0008DC6B: ;
    PUSH32(esp, 0); sub_000A3F80();
    ecx = 0x4140E8;
    PUSH32(esp, 0); sub_000976C0();
    if (TEST_Z(LO8(eax), LO8(eax))) goto loc_0008DC9A;
    PUSH32(esp, 0);
    ecx = 0x4140E8;
    PUSH32(esp, 0); sub_000976E0();

loc_0008DC9A: ;
    POP32(esp, edi);
    POP32(esp, esi);
    esp += 4; return;
}
""",
    ),
    GeneratedPatch(
        "XMV single-predecessor interpolation flags",
        """loc_002576A8: ;
    if (((int32_t)MEM32(ecx) <= 0)) goto loc_002576B2; /* jle: less or equal (signed <=) */
""",
        """loc_002576A8: ;
    if (CMP_LE((eax & eax), 0)) goto loc_002576B2; /* jle: less or equal (signed <=) */
""",
    ),
    GeneratedPatch(
        "XMV second single-predecessor interpolation flags",
        """loc_002576C8: ;
    if (((int32_t)MEM32(edx) <= 0)) goto loc_002576D2; /* jle: less or equal (signed <=) */
""",
        """loc_002576C8: ;
    if (CMP_LE((eax & eax), 0)) goto loc_002576D2; /* jle: less or equal (signed <=) */
""",
    ),
    GeneratedPatch(
        "Resource iterator shared continuations",
        """void sub_00176AE0(void) { esp += 4; /* 0x00176AE0: not detected; minimal guest ret */ }
void sub_00176AED(void) { esp += 4; /* 0x00176AED: not detected; minimal guest ret */ }
""",
        """void sub_00176AE0(void)
{
    PUSH32(esp, 0); sub_001DCB30();
    esp += 8;
    if (eax != 6u) {
        sub_00176AED();
        return;
    }
    PUSH32(esp, 4);
    PUSH32(esp, 0xFFFFFFFEu);
    PUSH32(esp, esi);
    edi = 0;
    PUSH32(esp, 0); sub_001DD230();
    PUSH32(esp, 0xFFFFFFFFu);
    PUSH32(esp, esi);
    PUSH32(esp, 0); sub_001DCB30();
    esp += 0x14;
    if (eax != 0u) {
        ebx = 0xFFFFFFFEu;
        do {
            ++edi;
            eax = edi + 4u;
            PUSH32(esp, eax);
            --ebx;
            PUSH32(esp, ebx);
            PUSH32(esp, esi);
            PUSH32(esp, 0); sub_001DD230();
            PUSH32(esp, 0xFFFFFFFFu);
            PUSH32(esp, esi);
            PUSH32(esp, 0); sub_001DCB30();
            esp += 0x14;
        } while (eax != 0u);
    }
    PUSH32(esp, 0xFFFFFFFEu);
    PUSH32(esp, esi);
    PUSH32(esp, 0); sub_001DC980();
    PUSH32(esp, 0x2F3E80u);
    PUSH32(esp, 0);
    PUSH32(esp, edi);
    PUSH32(esp, esi);
    PUSH32(esp, 0); sub_001135E0();
    esp += 0x18;
    POP32(esp, edi);
    POP32(esp, esi);
    POP32(esp, ebx);
    POP32(esp, ecx);
    esp += 4; return;
}
void sub_00176AED(void)
{
    PUSH32(esp, 0xFFFFFFFEu);
    PUSH32(esp, esi);
    PUSH32(esp, 0); sub_001DC980();
    esp += 8;
    POP32(esp, edi);
    POP32(esp, esi);
    POP32(esp, ebx);
    POP32(esp, ecx);
    esp += 4; return;
}
""",
    ),
    GeneratedPatch(
        "Vehicle physics shared continuations and epilogues",
        """void sub_0014FA3D(void) { esp += 4; /* 0x0014FA3D: not detected; minimal guest ret */ }
void sub_0014FA49(void) { esp += 4; /* 0x0014FA49: not detected; minimal guest ret */ }
void sub_0014FEC8(void) { esp += 4; /* 0x0014FEC8: not detected; minimal guest ret */ }
void sub_0014FED4(void) { esp += 4; /* 0x0014FED4: not detected; minimal guest ret */ }
""",
        """void sub_0014FA3D(void)
{
    g_recomp_14fa2b_entry = 1u;
    sub_0014FA2B();
}

void sub_0014FA49(void)
{
    g_recomp_14fa2b_entry = 2u;
    sub_0014FA2B();
}

void sub_0014FEC8(void)
{
    edx = MEM32(esi + 0xBC);
    MEM32(esi + 0xC0) = edx;
    sub_0014FED4();
}

void sub_0014FED4(void)
{
    POP32(esp, edi);
    POP32(esp, esi);
    g_seh_ebp = MEM32(esp); esp += 4; /* pop ebp */
    POP32(esp, ebx);
    esp += 0x40;
    esp += 8; return; /* ret 4 */
}
""",
    ),
    GeneratedPatch(
        "Vehicle physics shared body interior entries",
        """void sub_0014FA2B(void)
{
    uint32_t ebp;
    int _flags = 0; /* fallback flag var */
""",
        """void sub_0014FA2B(void)
{
    uint32_t ebp;
    uint32_t _entry = g_recomp_14fa2b_entry;
    int _flags = 0; /* fallback flag var */
""",
    ),
    GeneratedPatch(
        "Vehicle physics shared body entry dispatch",
        """    ebp = g_seh_ebp; /* inherit guest caller frame */
    RECOMP_TRACE_FUNC(0x0014FA2Bu);

loc_0014FA2B: ;
""",
        """    ebp = g_seh_ebp; /* inherit guest caller frame */
    g_recomp_14fa2b_entry = 0u;
    RECOMP_TRACE_FUNC(0x0014FA2Bu);

    if (_entry == 1u) goto loc_0014FA3D;
    if (_entry == 2u) goto loc_0014FA49;

loc_0014FA2B: ;
""",
    ),
    GeneratedPatch(
        "Vehicle physics 0x14FA3D interior label",
        """loc_0014FA38: ;
    PUSH32(esp, 0xA8965862u);
    ecx = MEM32(esi + 8);
""",
        """loc_0014FA38: ;
    PUSH32(esp, 0xA8965862u);
loc_0014FA3D: ;
    ecx = MEM32(esi + 8);
""",
    ),
    GeneratedPatch(
        "Vehicle physics 0x14FA49 interior label",
        """loc_0014FA47: ;
    ebp = eax;
    eax = MEM32(esi + 0x138);
""",
        """loc_0014FA47: ;
    ebp = eax;
loc_0014FA49: ;
    eax = MEM32(esi + 0x138);
""",
    ),
    GeneratedPatch(
        "Vehicle physics full duplicate event ring fast path",
        """loc_0014FA7F: ;
    ecx = MEM32(ebx + 4);
""",
        """loc_0014FA7F: ;
    /* The retail vehicle state keeps a ten-entry circular history here.  A
       full history containing only the incoming event has the same outcome
       as exhausting the iterator below, but the lifted hot path can spend an
       unbounded amount of host time repeatedly revisiting that state. */
    if (MEM8(ebx) != 0) {
        uint32_t _all_duplicate_events = 1u;
        for (uint32_t _event_i = 0; _event_i < 10u; ++_event_i) {
            if (MEM32(ebx + 0xCu + _event_i * 4u) != edi) {
                _all_duplicate_events = 0u;
                break;
            }
        }
        if (_all_duplicate_events != 0u) goto loc_0014FAC3;
    }
    ecx = MEM32(ebx + 4);
""",
    ),
    GeneratedPatch(
        "Handle allocator shared jne flags",
        """    (void)0; /* test edi, edi - flags set for next jcc */
    esi = ecx;
    if (CMP_BE((edi & edi), 0)) goto loc_001527F1; /* jbe: below or equal (unsigned <=) */

loc_001527DF: ;
""",
        """    _flags = (TEST_NZ(edi, edi)); /* shared jne source */
    esi = ecx;
    if (CMP_BE((edi & edi), 0)) goto loc_001527F1; /* jbe: below or equal (unsigned <=) */

loc_001527DF: ;
""",
    ),
    GeneratedPatch(
        "Handle allocator shared jne branch",
        """loc_00152803: ;
    if (TEST_NZ(edi, edi)) goto loc_00152849; /* jne: not equal / not zero */
""",
        """loc_00152803: ;
    if (_flags != 0) goto loc_00152849; /* jne: not equal / not zero */
""",
    ),
    GeneratedPatch(
        "Handle allocator generated-id flag paths",
        """loc_00152836: ;
    (void)0; /* test edx, edx - flags set for next jcc */
    goto loc_00152803;

loc_0015283A: ;
    ecx = MEM32(esi + eax * 8 + 0x1648);
    if (TEST_NZ(ecx, ecx)) goto loc_00152805; /* jne: not equal / not zero */

loc_00152845: ;
    (void)0; /* test edx, edx - flags set for next jcc */
    goto loc_00152803;
""",
        """loc_00152836: ;
    _flags = (TEST_NZ(edx, edx)); /* shared jne source */
    goto loc_00152803;

loc_0015283A: ;
    ecx = MEM32(esi + eax * 8 + 0x1648);
    if (TEST_NZ(ecx, ecx)) goto loc_00152805; /* jne: not equal / not zero */

loc_00152845: ;
    _flags = (TEST_NZ(edx, edx)); /* shared jne source */
    goto loc_00152803;
""",
    ),
    GeneratedPatch(
        "Lua string-format shared continuations",
        """void sub_001DB046(void) { esp += 4; /* 0x001DB046: not detected; minimal guest ret */ }
void sub_001DB04B(void) { esp += 4; /* 0x001DB04B: not detected; minimal guest ret */ }
void sub_001DB053(void) { esp += 4; /* 0x001DB053: not detected; minimal guest ret */ }
void sub_001DB058(void) { esp += 4; /* 0x001DB058: not detected; minimal guest ret */ }
void sub_001DB061(void) { esp += 4; /* 0x001DB061: not detected; minimal guest ret */ }
""",
        """void sub_001DB046(void)
{
    eax = esp + 0x14;
    PUSH32(esp, eax);
    PUSH32(esp, 0); sub_002370B8();
    esp += 0x14;
    sub_001DB053();
}

void sub_001DB04B(void)
{
    PUSH32(esp, 0); sub_002370B8();
    esp += 0x14;
    sub_001DB053();
}

void sub_001DB053(void)
{
    ecx = esp + 4;
    PUSH32(esp, ecx);
    sub_001DB058();
}

void sub_001DB058(void)
{
    PUSH32(esp, esi);
    PUSH32(esp, 0); sub_001DD000();
    esp += 8;
    sub_001DB061();
}

void sub_001DB061(void)
{
    eax = 1;
    POP32(esp, esi);
    esp += 0x80;
    esp += 4; return;
}
""",
    ),
    GeneratedPatch(
        "Lua topointer shared continuations",
        """void sub_001DCF24(void) { esp += 4; /* 0x001DCF24: not detected; minimal guest ret */ }
void sub_001DCF2B(void) { esp += 4; /* 0x001DCF2B: not detected; minimal guest ret */ }
""",
        """void sub_001DCF24(void)
{
    ecx = esi;
    PUSH32(esp, 0); sub_001DC820();
    sub_001DCF2B();
}

void sub_001DCF2B(void)
{
    if (eax == 0u) {
        sub_001DCF1F();
        return;
    }
    ecx = MEM32(eax) - 2u;
    if (ecx > 6u) {
        sub_001DCF1F();
        return;
    }
    RECOMP_ITAIL(MEM32(ecx * 4u + 0x001DCF54u));
}
""",
    ),
    GeneratedPatch(
        "PblDiscFile open handle checkpoint",
        """loc_0020986D: ;
    MEM32(esi + 0x88) = eax;
    MEM8(esi + 4) = 1;
""",
        """loc_0020986D: ;
    MEM32(esi + 0x88) = eax;
    MEM8(esi + 4) = 1;
    recomp_pbl_file_checkpoint(1u, esi, eax);
""",
    ),
    GeneratedPatch(
        "PblDiscFile post-open checkpoint",
        """loc_00209890: ;
    MEM32(esi + 0xA0) = eax;
    SET_LO8(eax, MEM8(esi + 4));
""",
        """loc_00209890: ;
    MEM32(esi + 0xA0) = eax;
    recomp_pbl_file_checkpoint(2u, esi, eax);
    SET_LO8(eax, MEM8(esi + 4));
""",
    ),
    GeneratedPatch(
        "PblDiscFile raw-read handle checkpoint",
        """loc_00209A70: ;
    PUSH32(esp, ebx);
    ebx = MEM32(esp + 8);
""",
        """loc_00209A70: ;
    recomp_pbl_file_checkpoint(3u, ecx, MEM32(ecx + 0x88));
    PUSH32(esp, ebx);
    ebx = MEM32(esp + 8);
""",
    ),
    GeneratedPatch(
        "Retail comparator shared tails",
        """void sub_00065915(void) { esp += 4; /* 0x00065915: not detected; minimal guest ret */ }
void sub_00065965(void) { esp += 4; /* 0x00065965: not detected; minimal guest ret */ }
void sub_00065979(void) { esp += 4; /* 0x00065979: not detected; minimal guest ret */ }
void sub_00065A5D(void) { esp += 4; /* 0x00065A5D: not detected; minimal guest ret */ }
""",
        """void sub_00065915(void)
{
    POP32(esp, ebx);
    POP32(esp, edi);
    eax = 0;
    POP32(esp, esi);
    esp += 0x48;
    esp += 8; return; /* ret 4 */
}

void sub_00065965(void)
{
    uint32_t _icall_esp = g_esp + 4u;
    edx = MEM32(esi);
    PUSH32(esp, eax);
    ecx = esi;
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(edx + 4), _icall_esp);
    PUSH32(esp, eax);
    ecx = esi;
    PUSH32(esp, 0); sub_00064D00();
    if (TEST_Z(LO8(eax), LO8(eax))) {
        sub_00065915();
        return;
    }
    sub_00065979();
}

void sub_00065979(void)
{
    POP32(esp, ebx);
    POP32(esp, edi);
    eax = 1;
    POP32(esp, esi);
    esp += 0x48;
    esp += 8; return; /* ret 4 */
}

void sub_00065A5D(void)
{
    if (CMP_NE(edx, MEM32(edi + 0xC))) {
        sub_00065915();
        return;
    }
    POP32(esp, ebx);
    POP32(esp, edi);
    eax = 1;
    POP32(esp, esi);
    esp += 0x48;
    esp += 8; return; /* ret 4 */
}
""",
    ),
    GeneratedPatch(
        "Initialize rain camera history before first velocity sample",
        """loc_000AB3CD: ;
    eax = MEM32(0x6437B4);
    recomp_xmm_loadss(xmm3v, 0x2DC08C); /* movss */
    ecx = eax + 0x40;
    edx = MEM32(ecx);
""",
        """loc_000AB3CD: ;
    eax = MEM32(0x6437B4);
    recomp_xmm_loadss(xmm3v, 0x2DC08C); /* movss */
    ecx = eax + 0x40;
    {
        static int recomp_rain_camera_initialized;
        if (!recomp_rain_camera_initialized) {
            MEMF(0x33C6FC) = MEMF(ecx);
            MEMF(0x33C700) = MEMF(ecx + 4);
            MEMF(0x33C704) = MEMF(ecx + 8);
            recomp_rain_camera_initialized = 1;
        }
    }
    edx = MEM32(ecx);
""",
    ),
    GeneratedPatch(
        "Scene-object type compare flag preservation",
        """loc_000626DF: ;
    (void)0; /* cmp MEM8(eax + 9), 1 - flags set for next jcc */
    eax = MEM32(eax);
    if (((uint8_t)(MEM8(eax + 9)) <= (uint8_t)(1))) goto loc_000626E9; /* jbe: below or equal (unsigned <=) */
""",
        """loc_000626DF: ;
    (void)0; /* cmp MEM8(eax + 9), 1 - flags set for next jcc */
    _flags = (((uint8_t)(MEM8(eax + 9)) <= (uint8_t)(1))); /* preserve cmp flags across 1 instruction(s) */
    eax = MEM32(eax);
    if (_flags != 0) goto loc_000626E9; /* jbe: below or equal (unsigned <=) */
""",
    ),
    GeneratedPatch(
        "RsMain SetState transition checkpoint",
        """loc_0017A0A0: ;
    eax = MEM32(esp + 4);
""",
        """loc_0017A0A0: ;
    recomp_main_state_checkpoint(MEM32(esp + 4), MEM32(esp + 8));
    eax = MEM32(esp + 4);
""",
    ),
    GeneratedPatch(
        "D3D vertex constant source diagnostics",
        """loc_00288974: ;
    MEM32(0x299380) = eax;
    g_mm0 = (uint64_t)(MEM64(edx)); /* movq */
""",
        """loc_00288974: ;
    recomp_d3d_constant_checkpoint(ecx, edx, 4u);
    MEM32(0x299380) = eax;
    g_mm0 = (uint64_t)(MEM64(edx)); /* movq */
""",
    ),
    GeneratedPatch(
        "D3D generic vertex constant source diagnostics",
        """    eax = MEM32(esp + 0xC);
    if (CMP_AE(edi, MEM32(0x299384))) goto loc_00288AD0; /* jae: above or equal (unsigned >=) */
""",
        """    eax = MEM32(esp + 0xC);
    recomp_d3d_constant_checkpoint(ecx, edx, eax);
    if (CMP_AE(edi, MEM32(0x299384))) goto loc_00288AD0; /* jae: above or equal (unsigned >=) */
""",
    ),
    GeneratedPatch(
        "XACT stream descriptor checkpoint",
        """loc_0028276D: ;
    esi = MEM32(eax + 0x20);
""",
        """loc_0028276D: ;
    recomp_xact_stream_checkpoint(ebx, ecx, eax);
    esi = MEM32(eax + 0x20);
""",
    ),
    GeneratedPatch(
        "RedXactCue config dataset open diagnostics",
        """loc_001FE7D0: ;
    PUSH32(esp, esi);
""",
        """loc_001FE7D0: ;
    recomp_xact_managed_checkpoint(10u, MEM32(esp + 4), MEM32(0x690ACC), MEM32(0x690AD0));
    PUSH32(esp, esi);
""",
    ),
    GeneratedPatch(
        "RedXactCue config dataset read entry diagnostics",
        """loc_001FF450: ;
    esp = esp - 0x1C;
    PUSH32(esp, edi);
    edi = MEM32(esp + 0x24);
    ecx = edi;
""",
        """loc_001FF450: ;
    esp = esp - 0x1C;
    PUSH32(esp, edi);
    edi = MEM32(esp + 0x24);
    recomp_xact_managed_checkpoint(11u, edi, MEM32(0x690ACC), MEM32(0x690AD0));
    ecx = edi;
""",
    ),
    GeneratedPatch(
        "RedXactCue config dataset read exit diagnostics",
        """loc_001FF5CC: ;
    POP32(esp, edi);
""",
        """loc_001FF5CC: ;
    recomp_xact_managed_checkpoint(12u, edi, MEM32(0x690ACC), MEM32(0x690AD0));
    POP32(esp, edi);
""",
    ),
    GeneratedPatch(
        "RedXactCue config dataset close diagnostics",
        """loc_001FF5E0: ;
    eax = MEM32(0x690ACC);
""",
        """loc_001FF5E0: ;
    recomp_xact_managed_checkpoint(13u, MEM32(0x690B38), MEM32(0x690ACC), MEM32(0x690AD0));
    eax = MEM32(0x690ACC);
""",
    ),
    GeneratedPatch(
        "RedXactCue managed-play entry diagnostics",
        """loc_001FF6E0: ;
    PUSH32(esp, ecx);
""",
        """loc_001FF6E0: ;
    PUSH32(esp, ecx);
    recomp_xact_managed_checkpoint(1u, MEM32(esp + 8), MEM32(esp + 0xC), 0u);
""",
    ),
    GeneratedPatch(
        "RedXactCue managed-play config diagnostics",
        """loc_001FF748: ;
    edi = eax;
    esp = esp + 4;
    if (CMP_EQ(edi, ebx)) goto loc_001FF79B; /* je: equal / zero */
""",
        """loc_001FF748: ;
    edi = eax;
    esp = esp + 4;
    recomp_xact_managed_checkpoint(2u, MEM32(esp + 0x14), MEM32(esp + 0x18), edi);
    if (CMP_EQ(edi, ebx)) goto loc_001FF79B; /* je: equal / zero */
""",
    ),
    GeneratedPatch(
        "RedXactCue stale managed-handle release",
        """loc_00206486: ;
    MEMF(esi + 0x40) = (float)fp_top(); fp_pop(); /* fstp */
    ecx = MEM32(esi + 8);
""",
        """loc_00206486: ;
    MEMF(esi + 0x40) = (float)fp_top(); fp_pop(); /* fstp */
    recomp_xact_managed_update_checkpoint(esi);
    /* A vanished low-level handle cannot produce the stop notification that
       normally retires this managed wrapper. Preserve authored on-stop
       behavior by entering the existing normal-stop path once its finite
       playback time is exhausted. A zero length by itself is not sufficient:
       delayed XACT events can report zero before their low-level cue starts. */
    if (MEM32(esi + 8) != 0u && MEMF(esi + 0x40) == 0.0f &&
        MEMF(esi + 0x44) <= 0.0f &&
        !recomp_xact_low_level_handle_exists(MEM32(esi + 8)))
        goto loc_002064CA;
    ecx = MEM32(esi + 8);
""",
    ),
    GeneratedPatch(
        "RedXactCue play entry diagnostics",
        """loc_00206570: ;
    PUSH32(esp, ecx);
    PUSH32(esp, esi);
    esi = ecx;
    ecx = MEM32(esi + 0x34);
""",
        """loc_00206570: ;
    PUSH32(esp, ecx);
    PUSH32(esp, esi);
    esi = ecx;
    ecx = MEM32(esi + 0x34);
    recomp_xact_play_checkpoint(1u, esi, ecx, 0u);
""",
    ),
    GeneratedPatch(
        "RedXactCue wave-bank readiness diagnostics",
        """loc_0020657C: ;
    if (TEST_Z(LO8(eax), LO8(eax))) goto loc_002065DF; /* je: equal / zero */
""",
        """loc_0020657C: ;
    recomp_xact_play_checkpoint(2u, esi, MEM32(esi + 0x34), eax);
    if (TEST_Z(LO8(eax), LO8(eax))) goto loc_002065DF; /* je: equal / zero */
""",
    ),
    GeneratedPatch(
        "RedXactCue low-level handle diagnostics",
        """loc_002065DC: ;
    MEM32(esi + 8) = eax;
""",
        """loc_002065DC: ;
    recomp_xact_play_checkpoint(3u, esi, MEM32(esi + 0x34), eax);
    MEM32(esi + 8) = eax;
""",
    ),
    GeneratedPatch(
        "RedXactManager cue property diagnostics",
        """loc_0022558D: ;
    eax = MEM32(esp + 0x38);
""",
        """loc_0022558D: ;
    recomp_xact_properties_checkpoint(esi, esp + 4, eax);
    eax = MEM32(esp + 0x38);
""",
    ),
    GeneratedPatch(
        "CRT strrchr reverse SCAS direction",
        """    /* std - direction flag */
    while (ecx != 0u) {
        uint32_t _scas_lhs = LO8(eax);
        uint32_t _scas_rhs = MEM8(edi);
        _flags = (_scas_lhs == _scas_rhs);
        edi += 1u; --ecx;
        if (_flags != 0) break;
    } /* repne scasb */
""",
        """    /* std - direction flag */
    while (ecx != 0u) {
        uint32_t _scas_lhs = LO8(eax);
        uint32_t _scas_rhs = MEM8(edi);
        _flags = (_scas_lhs == _scas_rhs);
        edi -= 1u; --ecx;
        if (_flags != 0) break;
    } /* repne scasb */
""",
    ),
    GeneratedPatch(
        "RedLookUp cue-name store diagnostics",
        """loc_00204D2D: ;
    PUSH32(esp, 0xFFFF);
""",
        """loc_00204D2D: ;
    recomp_xact_setup_checkpoint(16u, 0u, ebx, esi, MEM32(0x713490));
    PUSH32(esp, 0xFFFF);
""",
    ),
    GeneratedPatch(
        "Empty dynamic hash lookup returns not found",
        """    edx = MEM32(esp + 0x14);
    esi = MEM32(esp + 0x10);
    eax = edi;
""",
        """    edx = MEM32(esp + 0x14);
    esi = MEM32(esp + 0x10);
    /* PblHashTable::Find on an empty or stale dynamic table means \"not found\".
     * Optional resources may be queried after their counted-resource table has
     * no backing storage.  A restored object can likewise retain a released
     * backing pointer, so validate the complete probe table before indexing it. */
    if (esi < 0x00010000u || esi >= 0x04000000u ||
        edx == 0xFFFFFFFFu ||
        edx > (0x04000000u - esi - 4u) / 4u) {
        eax = 0xFFFFFFFFu;
        goto loc_001F2A51;
    }
    eax = edi;
""",
    ),
    GeneratedPatch(
        "Ambient civilian path list uses its 30-entry capacity",
        """    (void)0; /* cmp MEM32(esi + 0x22CA4), 0x50 - flags set for next jcc */
    ecx = MEM32(esi + 0x22C9C);
    MEM32(esp + 0x14) = ecx;
    if (CMP_GE(MEM32(esi + 0x22CA4), 0x50)) goto loc_0016AC09; /* jge: greater or equal (signed >=) */
""",
        """    /* The ambient civilian path array has 30 entries.  The retail
     * code compared its path count with the separate 80-civilian
     * capacity, allowing busy areas to overwrite the adjacent network state. */
    (void)0; /* cmp MEM32(esi + 0x22CA4), 0x1E - flags set for next jcc */
    ecx = MEM32(esi + 0x22C9C);
    MEM32(esp + 0x14) = ecx;
    if (MEM32(esi + 0x22CA4) >= 0x1Eu) goto loc_0016AC09;
""",
    ),
    GeneratedPatch(
        "Invalid Spore permanent-property pointer returns not found",
        """loc_001ECD0E: ;
    ecx = MEM32(esi + 4);
    if (TEST_Z(ecx, ecx)) goto loc_001ECD20; /* je: equal / zero */
""",
        """loc_001ECD0E: ;
    ecx = MEM32(esi + 4);
    /* RedWorld::Spore::FindPropertyHash falls back to no
     * permanent-property result when _pPPD is absent.  A stale/corrupt Spore
     * must not turn an optional property lookup into a host access violation. */
    if (!recomp_spore_ppd_is_valid(0x001ECD0Eu, esi, ecx, edi))
        goto loc_001ECD20;
    if (TEST_Z(ecx, ecx)) goto loc_001ECD20; /* je: equal / zero */
""",
    ),
    GeneratedPatch(
        "Fixed hash lookup result diagnostics",
        """loc_001F2A51: ;
    POP32(esp, edi);
""",
        """loc_001F2A51: ;
    recomp_xact_setup_checkpoint(18u, 0u, esi, edi, eax);
    POP32(esp, edi);
""",
    ),
    GeneratedPatch(
        "Fixed hash insertion result diagnostics",
        """loc_001F2A80: ;
    MEM32(ecx + eax * 4) = esi;
    POP32(esp, esi);
""",
        """loc_001F2A80: ;
    MEM32(ecx + eax * 4) = esi;
    recomp_xact_setup_checkpoint(17u, 0u, ecx, esi, eax);
    POP32(esp, esi);
""",
    ),
    GeneratedPatch(
        "XACT SetupCue entry diagnostics",
        """loc_002266B0: ;
    eax = MEM32(esp + 4);
    esp = esp - 8;
""",
        """loc_002266B0: ;
    eax = MEM32(esp + 4);
    recomp_xact_setup_checkpoint(10u, ecx, eax, MEM32(esp + 8), MEM32(esp + 0xC));
    esp = esp - 8;
""",
    ),
    GeneratedPatch(
        "XACT SetupCue config diagnostics",
        """loc_002266C1: ;
    ebx = eax;
    esp = esp + 4;
""",
        """loc_002266C1: ;
    ebx = eax;
    recomp_xact_setup_checkpoint(11u, esi, 0u, ebx, 0u);
    esp = esp + 4;
""",
    ),
    GeneratedPatch(
        "XACT SetupCue bank diagnostics",
        """    MEM32(esp + 0xC) = edi;
    if (TEST_NZ(edi, edi)) goto loc_00226713; /* jne: not equal / not zero */
""",
        """    MEM32(esp + 0xC) = edi;
    recomp_xact_setup_checkpoint(12u, esi, 0u, edi, ebx);
    if (TEST_NZ(edi, edi)) goto loc_00226713; /* jne: not equal / not zero */
""",
    ),
    GeneratedPatch(
        "XACT SetupCue friendly-name diagnostics",
        """loc_00226739: ;
    if (TEST_S(eax, eax)) goto loc_00226708; /* jl: less (signed <) */
""",
        """loc_00226739: ;
    recomp_xact_setup_checkpoint(13u, esi, 0u, eax, MEM32(esp + 0x10));
    if (TEST_S(eax, eax)) goto loc_00226708; /* jl: less (signed <) */
""",
    ),
    GeneratedPatch(
        "XACT SetupCue friendly-name input diagnostics",
        """    edx = esp + 0x10;
    PUSH32(esp, edx);
    eax++;
    PUSH32(esp, eax);
""",
        """    edx = esp + 0x10;
    PUSH32(esp, edx);
    eax++;
    recomp_xact_setup_checkpoint(15u, esi, eax, ecx, edi);
    PUSH32(esp, eax);
""",
    ),
    GeneratedPatch(
        "XACT SetupCue result diagnostics",
        """    MEM32(ecx + 0x1C) = edx;
    eax = ecx;
    POP32(esp, ebx);
""",
        """    MEM32(ecx + 0x1C) = edx;
    recomp_xact_setup_checkpoint(14u, esi, MEM32(ebx + 4), ecx, MEM32(ecx + 0x18));
    eax = ecx;
    POP32(esp, ebx);
""",
    ),
    GeneratedPatch(
        "XACT prepared-cue lifetime prepare trace",
        """loc_0022591F: ;
    if (CMP_GE((eax & eax), 0)) goto loc_0022592D;""",
        """loc_0022591F: ;
    recomp_xact_cue_lifetime_checkpoint(1u, esi, eax);
    if (CMP_GE((eax & eax), 0)) goto loc_0022592D;""",
    ),
    GeneratedPatch(
        "XACT prepared-cue lifetime destructor trace",
        """loc_00284650: ;
    PUSH32(esp, esi);""",
        """loc_00284650: ;
    recomp_xact_cue_lifetime_checkpoint(2u, ecx, MEM32(esp + 4u));
    PUSH32(esp, esi);""",
    ),
    GeneratedPatch(
        "XACT cue play diagnostics",
        """    MEM32(esp + 0x28) = ecx;
    MEM32(esp + 0x2C) = 0;
    PUSH32(esp, 0); sub_0027DA01(); /* call 0x0027DA01 */

loc_00226CE9: ;
    if (TEST_S(eax, eax)) goto loc_00226D7B; /* jl: less (signed <) */
""",
        """    MEM32(esp + 0x28) = ecx;
    MEM32(esp + 0x2C) = 0;
    recomp_xact_cue_checkpoint(1u, edi, esi, 0u);
    PUSH32(esp, 0); sub_0027DA01(); /* call 0x0027DA01 */

loc_00226CE9: ;
    recomp_xact_cue_checkpoint(2u, edi, esi, eax);
    if (TEST_S(eax, eax)) goto loc_00226D7B; /* jl: less (signed <) */
""",
    ),
    GeneratedPatch(
        "XACT explicit-stop retirement diagnostics",
        """loc_0022690E: ;
    PUSH32(esp, esi);
""",
        """loc_0022690E: ;
    recomp_xact_cue_checkpoint(4u, edi, esi, MEM32(esp + 0x28));
    PUSH32(esp, esi);
""",
    ),
    GeneratedPatch(
        "XACT failed-play retirement diagnostics",
        """loc_00226D9F: ;
    PUSH32(esp, esi);
""",
        """loc_00226D9F: ;
    recomp_xact_cue_checkpoint(5u, edi, esi, eax);
    PUSH32(esp, esi);
""",
    ),
    GeneratedPatch(
        "XACT stop-notification retirement diagnostics",
        """loc_00227136: ;
    edx = MEM32(esi + 0x99F0);
""",
        """loc_00227136: ;
    recomp_xact_cue_checkpoint(3u, esi, edi, MEM32(esp + 0x50));
    edx = MEM32(esi + 0x99F0);
""",
    ),
    GeneratedPatch(
        "XACT variation selector CMP/SBB carry preservation",
        """    (void)0; /* cmp eax, ecx - flags set for next jcc */
    POP32(esp, edi);
    eax = _cf ? 0xFFFFFFFF : 0; /* sbb self (CF extend) */
""",
        """    _cf = ((uint32_t)(eax) < (uint32_t)(ecx)); /* preserve cmp carry across 1 instruction(s) */
    POP32(esp, edi);
    eax = _cf ? 0xFFFFFFFF : 0; /* sbb self (CF extend) */
""",
    ),
    GeneratedPatch(
        "XACT cue event-construction result diagnostics",
        """loc_0028063F: ;
    (void)0; /* test eax, eax - flags set for next jcc */
    MEM32(ebp + -4) = eax;
""",
        """loc_0028063F: ;
    recomp_xact_alloc_checkpoint(20u, edi, MEM32(ebp + 8), eax);
    (void)0; /* test eax, eax - flags set for next jcc */
    MEM32(ebp + -4) = eax;
""",
    ),
    GeneratedPatch(
        "XACT cue activation result diagnostics",
        """loc_0028069A: ;
    MEM32(ebp + -4) = eax;

loc_0028069D: ;
""",
        """loc_0028069A: ;
    recomp_xact_alloc_checkpoint(21u, edi, MEM32(ebp + 0xC), eax);
    MEM32(ebp + -4) = eax;

loc_0028069D: ;
""",
    ),
    GeneratedPatch(
        "XACT sound activation event-build diagnostics",
        """loc_0028755E: ;
    (void)0; /* test MEM8(edi), 7 - flags set for next jcc */
""",
        """loc_0028755E: ;
    recomp_xact_alloc_checkpoint(30u, ebx, edi, eax);
    (void)0; /* test MEM8(edi), 7 - flags set for next jcc */
""",
    ),
    GeneratedPatch(
        "Invalid permanent-property lookup root returns not found",
        """loc_001EC900: ;
    eax = MEM32(esp + 8);
    edx = MEM32(esp + 4);
    PUSH32(esp, eax);
""",
        """loc_001EC900: ;
    eax = MEM32(esp + 8);
    edx = MEM32(esp + 4);
    /* All PermanentPropertyData::_FindPropertyHash callers converge here.
     *  define a missing lookup as value 0 with
     * bSuccess=false; do that before a corrupt/stale _pPPD can be followed. */
    if (!recomp_spore_ppd_is_valid(0x001EC900u, 0u, ecx, edx)) {
        if (eax >= 0x00010000u && eax <= 0x03FFFFFFu)
            MEM8(eax) = 0u;
        eax = 0u;
        esp += 12; return; /* ret 8 */
    }
    PUSH32(esp, eax);
""",
    ),
    GeneratedPatch(
        "XACT sound activation first queue-node diagnostics",
        """loc_00287599: ;
    MEM32(ebp + -8) = eax;
""",
        """loc_00287599: ;
    recomp_xact_alloc_checkpoint(31u, ebx, edi, eax);
    MEM32(ebp + -8) = eax;
""",
    ),
    GeneratedPatch(
        "XACT sound activation event-submit diagnostics",
        """loc_0028778A: ;
    MEM32(ebp + -8) = MEM32(ebp + -8) & 0;
""",
        """loc_0028778A: ;
    recomp_xact_alloc_checkpoint(32u, ebx, edi, eax);
    MEM32(ebp + -8) = MEM32(ebp + -8) & 0;
""",
    ),
    GeneratedPatch(
        "XACT sound activation event-start diagnostics",
        """loc_00287841: ;
    goto loc_0028778E;
""",
        """loc_00287841: ;
    recomp_xact_alloc_checkpoint(33u, ebx, edi, eax);
    goto loc_0028778E;
""",
    ),
    GeneratedPatch(
        "XACT sound activation second queue-node diagnostics",
        """loc_0028786A: ;
    MEM32(ebp + -8) = eax;
""",
        """loc_0028786A: ;
    recomp_xact_alloc_checkpoint(34u, ebx, edi, eax);
    MEM32(ebp + -8) = eax;
""",
    ),
    GeneratedPatch(
        "XACT sound activation deferred-submit diagnostics",
        """loc_002878C7: ;
    MEM32(ebp + -8) = MEM32(ebp + -8) & 0;
""",
        """loc_002878C7: ;
    recomp_xact_alloc_checkpoint(35u, ebx, edi, eax);
    MEM32(ebp + -8) = MEM32(ebp + -8) & 0;
""",
    ),
    GeneratedPatch(
        "XACT sound activation empty-event diagnostics",
        """loc_00287960: ;
    MEM32(ebp + -4) = eax;
""",
        """loc_00287960: ;
    recomp_xact_alloc_checkpoint(36u, ebx, edi, eax);
    MEM32(ebp + -4) = eax;
""",
    ),
    GeneratedPatch(
        "XACT engine critical-section predicate preserves cmp carry",
        """loc_0027DB23: ;
    PUSH32(esp, esi);
    esi = ecx;
    eax = ZX8(MEM8(0x24));
    SET_LO8(ecx, 2);
    (void)0; /* cmp LO8(eax), LO8(ecx) - flags set for next jcc */
    eax = _cf ? 0xFFFFFFFF : 0; /* sbb self (CF extend) */
    { uint32_t _neg_value = (uint32_t)(eax);
      _cf = (_neg_value != 0);
    eax = (uint32_t)(-(int32_t)_neg_value); }
    MEM32(esi + 4) = eax;
""",
        """loc_0027DB23: ;
    PUSH32(esp, esi);
    esi = ecx;
    eax = ZX8(MEM8(0x24));
    SET_LO8(ecx, 2);
    _cf = ((uint8_t)(LO8(eax)) < (uint8_t)(LO8(ecx))); /* preserve cmp carry across 0 instruction(s) */
    eax = _cf ? 0xFFFFFFFF : 0; /* sbb self (CF extend) */
    { uint32_t _neg_value = (uint32_t)(eax);
      _cf = (_neg_value != 0);
    eax = (uint32_t)(-(int32_t)_neg_value); }
    MEM32(esi + 4) = eax;
""",
    ),
    GeneratedPatch(
        "XACT short-stream EOF diagnostic",
        """loc_00285CDF: ;
""",
        """loc_00285CDF: ;
    recomp_xact_sound_update_checkpoint(102u, MEM32(ebp + -12), esi, eax);
""",
    ),
    GeneratedPatch(
        "XACT EOF preserves completed but unsubmitted reads",
        """loc_00285CDF: ;
    recomp_xact_sound_update_checkpoint(102u, MEM32(ebp + -12), esi, eax);
    if (TEST_NZ(eax, eax)) goto loc_00285CE6; /* jne: not equal / not zero */
""",
        """loc_00285CDF: ;
    recomp_xact_sound_update_checkpoint(102u, MEM32(ebp + -12), esi, eax);
    /* Preserve ready data that completed inline but has not reached Process. */
    if (TEST_NZ(eax, eax) ||
        recomp_xact_stream_has_unsubmitted_read(MEM32(esi + 0x40)))
        goto loc_00285CE6;
""",
    ),
    GeneratedPatch(
        "XACT short-stream readiness diagnostic",
        """loc_0028731A: ;
""",
        """loc_0028731A: ;
    recomp_xact_sound_update_checkpoint(103u, MEM32(ebp + -4), esi, eax);
""",
    ),
    GeneratedPatch(
        "XACT short-stream read-result diagnostic",
        """loc_00285AA1: ;
""",
        """loc_00285AA1: ;
    recomp_xact_sound_update_checkpoint(104u, MEM32(ebp + -12), esi, eax);
""",
    ),
    GeneratedPatch(
        "XACT simple-wave dispatch diagnostics",
        """loc_002839D3: ;
    MEM8(esi + 0x84) = MEM8(esi + 0x84) & 0xF7;
""",
        """loc_002839D3: ;
    recomp_xact_sound_update_checkpoint(100u, edi, esi, MEM32(ebp + 0xC));
    MEM8(esi + 0x84) = MEM8(esi + 0x84) & 0xF7;
""",
    ),
    GeneratedPatch(
        "XACT stream packet submission diagnostics",
        """loc_002812B7: ;
    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, MEM32(ebp + 0xC));
    esi = MEM32(esi + 0x20);
    PUSH32(esp, MEM32(ebp + 8));
""",
        """loc_002812B7: ;
    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, MEM32(ebp + 0xC));
    esi = MEM32(esi + 0x20);
    recomp_xact_alloc_checkpoint(50u, esi, MEM32(ebp + 8), 0u);
    PUSH32(esp, MEM32(ebp + 8));
""",
    ),
    GeneratedPatch(
        "XACT stream packet submission result diagnostics",
        """loc_002812C6: ;
    (void)0; /* cmp MEM32(ebp + -4), 0 - flags set for next jcc */
""",
        """loc_002812C6: ;
    recomp_xact_alloc_checkpoint(51u, esi, MEM32(ebp + 8), eax);
    (void)0; /* cmp MEM32(ebp + -4), 0 - flags set for next jcc */
""",
    ),
    GeneratedPatch(
        "XACT simple-wave DirectSound play result diagnostics",
        """loc_002839E4: ;
    ecx = MEM32(ebp + -12);
""",
        """loc_002839E4: ;
    recomp_xact_sound_update_checkpoint(101u, edi, esi, eax);
    ecx = MEM32(ebp + -12);
""",
    ),
    GeneratedPatch(
        "XACT event heap capacity for recomp scheduling",
        """loc_0027DCB2: ;
    PUSH32(esp, 0x32);
    ecx = ebx;
""",
        """loc_0027DCB2: ;
    /* The retail 50-event heap assumes Xbox scheduling latency. Preserve
       queued events across tighter recomp bursts instead of returning E_OUTOFMEMORY. */
    PUSH32(esp, 0x200);
    ecx = ebx;
""",
    ),
    GeneratedPatch(
        "XACT engine immediate-event predicate diagnostics",
        """loc_00283823: ;
    eax = MEM32(esi);
""",
        """loc_00283823: ;
    recomp_xact_alloc_checkpoint(40u, esi, ebx, eax);
    eax = MEM32(esi);
""",
    ),
    GeneratedPatch(
        "XACT engine immediate-event dispatch diagnostics",
        """loc_0028382A: ;
    edi = eax;
""",
        """loc_0028382A: ;
    recomp_xact_alloc_checkpoint(41u, esi, ebx, eax);
    edi = eax;
""",
    ),
    GeneratedPatch(
        "XACT engine queued-event submit diagnostics",
        """loc_00283866: ;
    edi = eax;
""",
        """loc_00283866: ;
    recomp_xact_alloc_checkpoint(42u, esi, edi, eax);
    edi = eax;
""",
    ),
    GeneratedPatch(
        "Audio_PlayMusic hash and state diagnostics",
        """loc_00118549: ;
    esp = esp + 0x10;
    PUSH32(esp, eax);
    ecx = 0x37B9B4;
    PUSH32(esp, 0); sub_00151190(); /* call 0x00151190 */

loc_00118557: ;
    eax = 0; /* xor self */
""",
        """loc_00118549: ;
    recomp_music_checkpoint(1u, 0x0037B9B4u, eax);
    esp = esp + 0x10;
    PUSH32(esp, eax);
    ecx = 0x37B9B4;
    PUSH32(esp, 0); sub_00151190(); /* call 0x00151190 */

loc_00118557: ;
    recomp_music_checkpoint(2u, 0x0037B9B4u, MEM32(0x0037B9B8u));
    eax = 0; /* xor self */
""",
    ),
    GeneratedPatch(
        "RsDJ StartMusic entry diagnostics",
        """loc_00151190: ;
    PUSH32(esp, ebx);
    ebx = MEM32(esp + 8);
""",
        """loc_00151190: ;
    recomp_music_checkpoint(10u, ecx, MEM32(esp + 4));
    PUSH32(esp, ebx);
    ebx = MEM32(esp + 8);
""",
    ),
    GeneratedPatch(
        "RsDJ StartMusic exit diagnostics",
        """loc_001511D9: ;
    POP32(esp, esi);
    POP32(esp, ebx);
""",
        """loc_001511D9: ;
    recomp_music_checkpoint(11u, esi, ebx);
    POP32(esp, esi);
    POP32(esp, ebx);
""",
    ),
    GeneratedPatch(
        "XACT cue instance allocation diagnostics",
        """    PUSH32(esp, 0); sub_001787C0(); /* call 0x001787C0 */

loc_0027F93B: ;
    if (TEST_Z(eax, eax)) goto loc_0027F94A; /* je: equal / zero */
""",
        """    PUSH32(esp, 0); sub_001787C0(); /* call 0x001787C0 */

loc_0027F93B: ;
    recomp_xact_alloc_checkpoint(1u, ebx, eax, 0x40u);
    if (TEST_Z(eax, eax)) goto loc_0027F94A; /* je: equal / zero */
""",
    ),
    GeneratedPatch(
        "XACT cue initializer result diagnostics",
        """    PUSH32(esp, 0); sub_00284670(); /* call 0x00284670 */

loc_0027F970: ;
    edi = eax;
""",
        """    PUSH32(esp, 0); sub_00284670(); /* call 0x00284670 */

loc_0027F970: ;
    recomp_xact_alloc_checkpoint(2u, ebx, esi, eax);
    edi = eax;
""",
    ),
    GeneratedPatch(
        "XACT simple-event allocation diagnostics",
        """loc_002847D4: ;
    PUSH32(esp, 0x6484A002);
    PUSH32(esp, 0x60);
    PUSH32(esp, 0); sub_001787C0(); /* call 0x001787C0 */

loc_002847E0: ;
    if (CMP_EQ(eax, ebx)) goto loc_002847ED; /* je: equal / zero */
""",
        """loc_002847D4: ;
    PUSH32(esp, 0x6484A002);
    PUSH32(esp, 0x60);
    PUSH32(esp, 0); sub_001787C0(); /* call 0x001787C0 */

loc_002847E0: ;
    recomp_xact_alloc_checkpoint(3u, esi, eax, 0x60u);
    if (CMP_EQ(eax, ebx)) goto loc_002847ED; /* je: equal / zero */
""",
    ),
    GeneratedPatch(
        "XACT simple-event initializer diagnostics",
        """    PUSH32(esp, 0); sub_00286289(); /* call 0x00286289 */

loc_00284816: ;
    (void)0; /* cmp eax, ebx - flags set for next jcc */
""",
        """    PUSH32(esp, 0); sub_00286289(); /* call 0x00286289 */

loc_00284816: ;
    recomp_xact_alloc_checkpoint(4u, esi, MEM32(esi + 0x28), eax);
    (void)0; /* cmp eax, ebx - flags set for next jcc */
""",
    ),
    GeneratedPatch(
        "XACT complex-event slot diagnostics",
        """    PUSH32(esp, 0); sub_00284397(); /* call 0x00284397 */

loc_00284707: ;
    ebx = eax;
""",
        """    PUSH32(esp, 0); sub_00284397(); /* call 0x00284397 */

loc_00284707: ;
    recomp_xact_alloc_checkpoint(5u, esi, edi, eax);
    ebx = eax;
""",
    ),
    GeneratedPatch(
        "XACT complex-event selection diagnostics",
        """    PUSH32(esp, 0); sub_002844A3(); /* call 0x002844A3 */

loc_00284727: ;
    edx = ZX16(MEM16(esi + 0x18));
""",
        """    PUSH32(esp, 0); sub_002844A3(); /* call 0x002844A3 */

loc_00284727: ;
    recomp_xact_alloc_checkpoint(6u, esi, edi, eax);
    edx = ZX16(MEM16(esi + 0x18));
""",
    ),
    GeneratedPatch(
        "XACT complex-event primary creation diagnostics",
        """    PUSH32(esp, 0); sub_0028456D(); /* call 0x0028456D */

loc_00284769: ;
    (void)0; /* test eax, eax - flags set for next jcc */
""",
        """    PUSH32(esp, 0); sub_0028456D(); /* call 0x0028456D */

loc_00284769: ;
    recomp_xact_alloc_checkpoint(7u, esi, MEM32(esi + 0x2C), eax);
    (void)0; /* test eax, eax - flags set for next jcc */
""",
    ),
    GeneratedPatch(
        "XACT complex-event secondary creation diagnostics",
        """    PUSH32(esp, 0); sub_0028456D(); /* call 0x0028456D */

loc_0028478C: ;
    (void)0; /* test eax, eax - flags set for next jcc */
""",
        """    PUSH32(esp, 0); sub_0028456D(); /* call 0x0028456D */

loc_0028478C: ;
    recomp_xact_alloc_checkpoint(8u, esi, MEM32(esi + 0x2C), eax);
    (void)0; /* test eax, eax - flags set for next jcc */
""",
    ),
    GeneratedPatch(
        "XACT stream list insertion diagnostics",
        """loc_0027E7C6: ;
    edi = eax;
    if (TEST_S(edi, edi)) goto loc_0027E7E9; /* jl: less (signed <) */

loc_0027E7CC: ;
    ecx = MEM32(esp + 0x10);
""",
        """loc_0027E7C6: ;
    edi = eax;
    if (TEST_S(edi, edi)) goto loc_0027E7E9; /* jl: less (signed <) */

loc_0027E7CC: ;
    recomp_xact_list_checkpoint(1u, MEM32(esp + 0x10), esi);
    ecx = MEM32(esp + 0x10);
""",
    ),
    GeneratedPatch(
        "XACT stream list insertion result diagnostics",
        """loc_0027E7CC: ;
    recomp_xact_list_checkpoint(1u, MEM32(esp + 0x10), esi);
    ecx = MEM32(esp + 0x10);
    eax = esi + 0x58;
    { uint32_t _alu_dst = (uint32_t)(ecx);
      uint32_t _alu_src = (uint32_t)(0x44);
      _cf = ((uint64_t)_alu_dst + (uint64_t)_alu_src > 0xFFFFFFFFull);
    ecx = _alu_dst + _alu_src; }
    MEM32(eax) = ecx;
    ecx = MEM32(ecx + 4);
    MEM32(eax + 4) = ecx;
    MEM32(ecx) = eax;
    ecx = MEM32(eax);
    MEM32(ecx + 4) = eax;
    MEM32(ebx) = esi;
""",
        """loc_0027E7CC: ;
    recomp_xact_list_checkpoint(1u, MEM32(esp + 0x10), esi);
    ecx = MEM32(esp + 0x10);
    eax = esi + 0x58;
    { uint32_t _alu_dst = (uint32_t)(ecx);
      uint32_t _alu_src = (uint32_t)(0x44);
      _cf = ((uint64_t)_alu_dst + (uint64_t)_alu_src > 0xFFFFFFFFull);
    ecx = _alu_dst + _alu_src; }
    MEM32(eax) = ecx;
    ecx = MEM32(ecx + 4);
    MEM32(eax + 4) = ecx;
    MEM32(ecx) = eax;
    ecx = MEM32(eax);
    MEM32(ecx + 4) = eax;
    recomp_xact_list_checkpoint(2u, MEM32(esp + 0x10), esi);
    MEM32(ebx) = esi;
""",
    ),
    GeneratedPatch(
        "XACT stream list polling diagnostics",
        """loc_0027F34B: ;
    ecx = edi + -88;
    edi = MEM32(edi);
    PUSH32(esp, 0); sub_002829B1(); /* call 0x002829B1 */
""",
        """loc_0027F34B: ;
    ecx = edi + -88;
    recomp_xact_list_checkpoint(3u, esi, ecx);
    edi = MEM32(edi);
    PUSH32(esp, 0); sub_002829B1(); /* call 0x002829B1 */
""",
    ),
    GeneratedPatch(
        "RedSpace cylinder query diagnostics",
        """loc_002245D1: ;
    if (CMP_L(ecx, ebx)) goto loc_002247C3; /* jl: less (signed <) */
""",
        """loc_002245D1: ;
    recomp_redspace_bounds_checkpoint(
        ebp, MEM32(esp + 0x48), (int32_t)MEM32(esp + 0x20),
        (int32_t)edi, (int32_t)MEM32(esp + 0x14), (int32_t)esi,
        (int32_t)MEM32(esp + 0x1C), (int32_t)ebx, (int32_t)ecx,
        (int32_t)MEM32(esp + 0x24));
    if (CMP_L(ecx, ebx)) goto loc_002247C3; /* jl: less (signed <) */
""",
    ),
    GeneratedPatch(
        "RedSpace cylinder probe diagnostics",
        """loc_00224644: ;
    if (TEST_Z(MEM8(eax + 0x31), 1)) goto loc_0022465C; /* je: equal / zero */
""",
        """loc_00224644: ;
    recomp_redspace_probe_checkpoint(
        ebp, (int32_t)MEM32(esp + 0x10),
        (int32_t)MEM32(esp + 0x50), esi, eax);
    if (TEST_Z(MEM8(eax + 0x31), 1)) goto loc_0022465C; /* je: equal / zero */
""",
    ),
    GeneratedPatch(
        "RedSpace cylinder query entry diagnostics",
        """loc_002252D0: ;
    SET_LO8(eax, MEM8(esp + 0x10));
""",
        """loc_002252D0: ;
    recomp_redspace_query_checkpoint(0u, ecx, MEM32(esp + 4),
                                     MEM32(esp + 8), 0u);
    SET_LO8(eax, MEM8(esp + 0x10));
""",
    ),
    GeneratedPatch(
        "RedSpace cylinder free ABI diagnostics",
        """loc_0022539F: ;
    esi = MEM32(0x85C580);
""",
        """loc_0022539F: ;
    recomp_redspace_query_checkpoint(1u, ebx, MEM32(0x85C588),
                                     edi, MEM32(esp + 0x1C));
    esi = MEM32(0x85C580);
""",
    ),
    GeneratedPatch(
        "RedSpace cylinder consumer diagnostics",
        """loc_001EE729: ;
    ebx = eax;
""",
        """loc_001EE729: ;
    recomp_redspace_query_checkpoint(2u, 0u, esi, eax, 0u);
    ebx = eax;
""",
    ),
    GeneratedPatch(
        "AI common initialization stack entry diagnostics",
        """    edi = ecx;
    PUSH32(esp, 0); sub_00063F30(); /* call 0x00063F30 */
""",
        """    edi = ecx;
    recomp_ai_init_stack_checkpoint(0u, edi, esp, esi, ebx);
    PUSH32(esp, 0); sub_00063F30(); /* call 0x00063F30 */
""",
    ),
    GeneratedPatch(
        "AI common initialization stack checkpoint 1",
        """loc_0006BA7C: ;
    eax = MEM32(edi);
""",
        """loc_0006BA7C: ;
    recomp_ai_init_stack_checkpoint(1u, edi, esp, esi, ebx);
    eax = MEM32(edi);
""",
    ),
    GeneratedPatch(
        "AI common initialization stack checkpoint 2",
        """loc_0006BB33: ;
    recomp_xmm_loadss(xmm0v, 0x2DC340); /* movss */
""",
        """loc_0006BB33: ;
    recomp_ai_init_stack_checkpoint(2u, edi, esp, esi, ebx);
    recomp_xmm_loadss(xmm0v, 0x2DC340); /* movss */
""",
    ),
    GeneratedPatch(
        "AI common initialization stack checkpoint 3",
        """loc_0006BC55: ;
    (void)0; /* test eax, eax - flags set for next jcc */
""",
        """loc_0006BC55: ;
    recomp_ai_init_stack_checkpoint(3u, edi, esp, esi, ebx);
    (void)0; /* test eax, eax - flags set for next jcc */
""",
    ),
    GeneratedPatch(
        "AI common initialization stack checkpoint 4",
        """loc_0006BD16: ;
    if (TEST_Z(eax, eax)) goto loc_0006BD35; /* je: equal / zero */
""",
        """loc_0006BD16: ;
    recomp_ai_init_stack_checkpoint(4u, edi, esp, esi, ebx);
    if (TEST_Z(eax, eax)) goto loc_0006BD35; /* je: equal / zero */
""",
    ),
    GeneratedPatch(
        "AI common initialization stack checkpoint 5",
        """loc_0006BDF3: ;
    if (TEST_Z(eax, eax)) goto loc_0006BE0C; /* je: equal / zero */
""",
        """loc_0006BDF3: ;
    recomp_ai_init_stack_checkpoint(5u, edi, esp, esi, ebx);
    if (TEST_Z(eax, eax)) goto loc_0006BE0C; /* je: equal / zero */
""",
    ),
    GeneratedPatch(
        "AI common initialization stack checkpoint 6",
        """loc_0006BEC5: ;
    if (TEST_Z(eax, eax)) goto loc_0006BEE4; /* je: equal / zero */
""",
        """loc_0006BEC5: ;
    recomp_ai_init_stack_checkpoint(6u, edi, esp, esi, ebx);
    if (TEST_Z(eax, eax)) goto loc_0006BEE4; /* je: equal / zero */
""",
    ),
    GeneratedPatch(
        "AI common initialization stack checkpoint 7",
        """loc_0006BF86: ;
    edx = MEM32(edi);
""",
        """loc_0006BF86: ;
    recomp_ai_init_stack_checkpoint(7u, edi, esp, esi, ebx);
    edx = MEM32(edi);
""",
    ),
    GeneratedPatch(
        "AI common initialization stack checkpoint 8",
        """loc_0006BFB9: ;
    (void)0; /* test ebp, ebp - flags set for next jcc */
""",
        """loc_0006BFB9: ;
    recomp_ai_init_stack_checkpoint(8u, edi, esp, esi, ebx);
    (void)0; /* test ebp, ebp - flags set for next jcc */
""",
    ),
    GeneratedPatch(
        "AI common initialization stack checkpoint 9",
        """    POP32(esp, esi);
    POP32(esp, ebx);
    if (TEST_Z(ebp, ebp)) goto loc_0006BFF4; /* je: equal / zero */
""",
        """    POP32(esp, esi);
    POP32(esp, ebx);
    recomp_ai_init_stack_checkpoint(9u, edi, esp, esi, ebx);
    if (TEST_Z(ebp, ebp)) goto loc_0006BFF4; /* je: equal / zero */
""",
    ),
    GeneratedPatch(
        "AI common initialization stack checkpoint 10",
        """loc_0006BFFB: ;
    edx = MEM32(edi);
""",
        """loc_0006BFFB: ;
    recomp_ai_init_stack_checkpoint(10u, edi, esp, esi, ebx);
    edx = MEM32(edi);
""",
    ),
    GeneratedPatch(
        "AI common initialization stack checkpoint 11",
        """loc_0006C00D: ;
    ecx = edi;
""",
        """loc_0006C00D: ;
    recomp_ai_init_stack_checkpoint(11u, edi, esp, esi, ebx);
    ecx = edi;
""",
    ),
    GeneratedPatch(
        "AI common initialization stack checkpoint 12",
        """loc_0006C014: ;
    POP32(esp, edi);
""",
        """loc_0006C014: ;
    recomp_ai_init_stack_checkpoint(12u, edi, esp, esi, ebx);
    POP32(esp, edi);
""",
    ),
    GeneratedPatch(
        "AI base initialization factory-call entry diagnostics",
        """loc_00068BF1: ;
    eax = MEM32(esi);
    ecx = esi + 0x330;
    { uint32_t _icall_esp = g_esp;
""",
        """loc_00068BF1: ;
    eax = MEM32(esi);
    ecx = esi + 0x330;
    recomp_ai_base_init_checkpoint(0u, esi, eax, edx, esp);
    { uint32_t _icall_esp = g_esp;
""",
    ),
    GeneratedPatch(
        "AI base initialization factory result diagnostics",
        """loc_00068C03: ;
    MEM32(esi + 0x2E0) = eax;
    edx = MEM32(eax);
    ecx = eax;
    { uint32_t _icall_esp = g_esp;
""",
        """loc_00068C03: ;
    recomp_ai_base_init_checkpoint(1u, esi, eax, 0u, esp);
    MEM32(esi + 0x2E0) = eax;
    edx = MEM32(eax);
    ecx = eax;
    recomp_ai_base_init_checkpoint(2u, esi, ecx, edx, esp);
    { uint32_t _icall_esp = g_esp;
""",
    ),
    GeneratedPatch(
        "AI base initialization child-call return diagnostics",
        """loc_00068C10: ;
    ecx = esi + 0x54;
""",
        """loc_00068C10: ;
    recomp_ai_base_init_checkpoint(3u, esi, eax, ecx, esp);
    ecx = esi + 0x54;
""",
    ),
    GeneratedPatch(
        "Derived actor initialization receiver diagnostics",
        """loc_0008A4C0: ;
    eax = MEM32(esp + 8);
""",
        """loc_0008A4C0: ;
    recomp_actor_init_checkpoint(0u, ecx, MEM32(esp + 4), MEM32(esp + 8));
    eax = MEM32(esp + 8);
""",
    ),
    GeneratedPatch(
        "Base actor initialization entry diagnostics",
        """loc_0007EE80: ;
    eax = MEM32(0);
""",
        """loc_0007EE80: ;
    recomp_actor_init_checkpoint(1u, ecx, MEM32(esp + 4), MEM32(esp + 8));
    eax = MEM32(0);
""",
    ),
    GeneratedPatch(
        "Base actor initialization post-common diagnostics",
        """loc_0007EEAC: ;
    eax = MEM32(esi + 0x264);
""",
        """loc_0007EEAC: ;
    recomp_actor_init_checkpoint(2u, esi, MEM32(esp + 0x58),
                                 MEM32(esp + 0x5C));
    eax = MEM32(esi + 0x264);
""",
    ),
    GeneratedPatch(
        "Actor dispatcher invalid-object caller diagnostics",
        """loc_00173A60: ;
    PUSH32(esp, ebp);
    ebp = esp;
    esp = esp & 0xFFFFFFF0u;
    esp = esp - 0x54;
    PUSH32(esp, ebx);
    PUSH32(esp, esi);
    esi = MEM32(ebp + 8);
    eax = MEM32(esi);
""",
        """loc_00173A60: ;
    PUSH32(esp, ebp);
    ebp = esp;
    esp = esp & 0xFFFFFFF0u;
    esp = esp - 0x54;
    PUSH32(esp, ebx);
    PUSH32(esp, esi);
    esi = MEM32(ebp + 8);
    recomp_actor_dispatch_checkpoint(MEM32(ebp + 4), ecx, esi, esp);
    eax = MEM32(esi);
""",
    ),
    GeneratedPatch(
        "Havok callback collection corruption guard",
        """loc_001C6B00: ;
    PUSH32(esp, ecx);
    PUSH32(esp, esi);
    PUSH32(esp, edi);
    edi = ecx;
    eax = MEM32(edi + 8);
    esi = MEM32(edi + 0xC4);
    esi--;
""",
        """loc_001C6B00: ;
    PUSH32(esp, ecx);
    PUSH32(esp, esi);
    PUSH32(esp, edi);
    edi = ecx;
    eax = MEM32(edi + 8);
    esi = MEM32(edi + 0xC4);
    esi = recomp_havok_callback_count_checkpoint(
        edi, MEM32(edi + 0xC0), esi, MEM32(eax + 0xCC), esp);
    esi--;
""",
    ),
    GeneratedPatch(
        "RedWorld hibernation predicate diagnostics",        """loc_001EC2B3: ;
    ecx = MEM32(esp + 0xC);
    recomp_xmm_loadss(xmm0v, ecx); /* movss */
""",
        """loc_001EC2B3: ;
    ecx = MEM32(esp + 0xC);
    recomp_spore_predicate_checkpoint(1u, esi, ecx, eax);
    recomp_xmm_loadss(xmm0v, ecx); /* movss */
""",
    ),
    GeneratedPatch(
        "RedWorld preloading predicate diagnostics",
        """loc_001EC333: ;
    ecx = MEM32(esp + 0xC);
    recomp_xmm_loadss(xmm0v, ecx); /* movss */
""",
        """loc_001EC333: ;
    ecx = MEM32(esp + 0xC);
    recomp_spore_predicate_checkpoint(2u, esi, ecx, eax);
    recomp_xmm_loadss(xmm0v, ecx); /* movss */
""",
    ),
    GeneratedPatch(
        "Movie config extension CMOVE flag preservation",
        """loc_0010D780: ;
    (void)0; /* cmp LO8(edx), 0x2E - flags set for next jcc */
    SET_LO8(edx, MEM8(ecx + 1));
    if (CMP_EQ(LO8(edx), 0x2E)) esi = ecx; /* cmove */
""",
        """loc_0010D780: ;
    (void)0; /* cmp LO8(edx), 0x2E - flags set for next jcc */
    _flags = (((uint8_t)(LO8(edx)) == (uint8_t)(0x2E))); /* preserve cmp flags across 1 instruction(s) */
    SET_LO8(edx, MEM8(ecx + 1));
    if (_flags != 0) esi = ecx; /* cmove */
""",
    ),
    GeneratedPatch(
        "Texture init-level merged comparison",
        """loc_00205CA8: ;
    if (CMP_LE(eax, esi)) goto loc_00205D17; /* jle: less or equal (signed <=) */
""",
        """loc_00205CA8: ;
    /* This block has two live predecessors. The direct JGE path compares the
     * manager field, while the loop-exit path has the same value in EAX.
     * Re-read the authoritative field so regeneration cannot reuse stale EAX
     * from the linear predecessor selected by the translator. */
    if (CMP_LE(MEM32(edi), esi)) goto loc_00205D17; /* jle: less or equal (signed <=) */
""",
    ),
    GeneratedPatch(
        "DirectSound passthrough mailbox completion",
        """    MEM32(ebx) = eax;

loc_002A0BF1: ;
    if (CMP_NE(MEM32(ebx), 0)) goto loc_002A0BF1; /* jne: not equal / not zero */
""",
        """    MEM32(ebx) = eax;

    /* xemu executes the downloaded GP DSP program, which consumes this
     * command and clears the mailbox. The standalone passthrough DSP does not
     * execute GP instructions, so acknowledge the completed image download. */
    MEM32(ebx) = 0;
""",
    ),
    GeneratedPatch(
        "D3D PFB WBC flush completion",
        """loc_0028E740: ;
    if (TEST_NZ(MEM32(eax + 0x100410), 0x10000)) goto loc_0028E740; /* jne: not equal / not zero */
""",
        """loc_0028E740: ;
    /* xemu models NV_PFB_WBC reads as zero (flush not pending). The
     * standalone renderer consumes submissions synchronously, so the XDK
     * write-buffer-cache flush is complete as soon as it is requested. */
""",
    ),
    GeneratedPatch(
        "D3D PFB WBC flush completion (secondary path)",
        """loc_0028E7F0: ;
    if (TEST_NZ(MEM32(eax + 0x100410), 0x10000)) goto loc_0028E7F0; /* jne: not equal / not zero */
""",
        """loc_0028E7F0: ;
    /* Match xemu's immediately completed NV_PFB_WBC flush. */
""",
    ),
    GeneratedPatch(
        "D3D PFB WBC request elision (secondary path)",
        """loc_0028E7C7: ;
    eax = MEM32(0x299378);
    eax = MEM32(eax + 0x1C28);
    /* TODO: sfence  */
    edx = MEM32(eax + 0x100410);
    edx = edx | 0x10000;
    MEM32(eax + 0x100410) = edx;
    goto loc_0028E7F0;
""",
        """loc_0028E7C7: ;
    /* Xemu reports NV_PFB_WBC as immediately idle and the write merely
     * stores the request bit. The host renderer is synchronous here, so
     * avoid entering the VEH MMIO bridge for this no-op flush request. */
    goto loc_0028E7F0;
""",
    ),
    GeneratedPatch(
        "D3D primary flush phase checkpoints",
        """    eax = MEM32(0x299378);
    eax = MEM32(eax + 0x1C28);
    /* TODO: sfence  */
    ecx = MEM32(eax + 0x100410);
    ecx = ecx | 0x10000;
    MEM32(eax + 0x100410) = ecx;
    /* nop */

loc_0028E740: ;
    /* xemu models NV_PFB_WBC reads as zero (flush not pending). The
     * standalone renderer consumes submissions synchronously, so the XDK
     * write-buffer-cache flush is complete as soon as it is requested. */

loc_0028E74C: ;
    ecx = MEM32(esi + 0x1C20);
    edx = edx & 0xFFFFFFF;
    MEM32(ecx + 0x40) = edx;
    eax = MEM32(0x298ED0);
    if (TEST_Z(eax, eax)) goto loc_0028E7AE; /* je: equal / zero */

loc_0028E764: ;
    PUSH32(esp, 0); sub_0028A060(); /* call 0x0028A060 */

loc_0028E769: ;
""",
        """    /* Xemu reports NV_PFB_WBC as immediately idle and the write
     * has no synchronization side effect. The standalone renderer consumes
     * submissions synchronously, so the physical MMIO request is redundant. */
    recomp_d3d_sync_checkpoint(1u);
    /* nop */

loc_0028E740: ;
    /* xemu models NV_PFB_WBC reads as zero (flush not pending). The
     * standalone renderer consumes submissions synchronously, so the XDK
     * write-buffer-cache flush is complete as soon as it is requested. */

loc_0028E74C: ;
    ecx = MEM32(esi + 0x1C20);
    edx = edx & 0xFFFFFFF;
    MEM32(ecx + 0x40) = edx;
    recomp_d3d_sync_checkpoint(2u);
    eax = MEM32(0x298ED0);
    recomp_d3d_sync_checkpoint(3u);
    if (TEST_Z(eax, eax)) goto loc_0028E7AE; /* je: equal / zero */

loc_0028E764: ;
    recomp_d3d_sync_checkpoint(4u);
    PUSH32(esp, 0); sub_0028A060(); /* call 0x0028A060 */

loc_0028E769: ;
    recomp_d3d_sync_checkpoint(5u);
""",
    ),
    GeneratedPatch(
        "D3D SetStreamSource renderer bridge",
        """    MEM32(esi + 0x297FB0) = eax;
    MEM32(esi + 0x297FB8) = edi;
    if (_flags != 0) goto loc_00288BAE; /* jne: not equal / not zero */
""",
        """    MEM32(esi + 0x297FB0) = eax;
    MEM32(esi + 0x297FB8) = edi;
    recomp_d3d_set_stream_source(esi / 12u, edi, eax);
    if (_flags != 0) goto loc_00288BAE; /* jne: not equal / not zero */
""",
    ),
    GeneratedPatch(
        "D3D SelectVertexShaderDirect declaration checkpoint",
        """loc_002888A0: ;
    PUSH32(esp, esi);
""",
        """loc_002888A0: ;
    recomp_d3d_select_vertex_shader_direct(MEM32(esp + 4),
                                            MEM32(esp + 8));
    PUSH32(esp, esi);
""",
    ),
    GeneratedPatch(
        "D3D SetVertexShader FVF bridge",
        """loc_00288F80: ;
    PUSH32(esp, ecx);
    PUSH32(esp, ebx);
    ebx = MEM32(esp + 0xC);
    (void)0; /* test LO8(ebx), 1 - flags set for next jcc */
""",
        """loc_00288F80: ;
    PUSH32(esp, ecx);
    PUSH32(esp, ebx);
    ebx = MEM32(esp + 0xC);
    recomp_d3d_set_vertex_shader(ebx);
    (void)0; /* test LO8(ebx), 1 - flags set for next jcc */
""",
    ),
    GeneratedPatch(
        "Lua invertjump TEST/POP flag preservation",
        """    SET_LO8(ebx, MEM8(edx + 0x2FC144));
    (void)0; /* test LO8(ebx), LO8(ebx) - flags set for next jcc */
    POP32(esp, ebx);
    if (((int32_t)(LO8(ebx) & LO8(ebx)) >= 0)) goto loc_001E8DC7; /* jns: not sign (positive) */
""",
        """    SET_LO8(ebx, MEM8(edx + 0x2FC144));
    (void)0; /* test LO8(ebx), LO8(ebx) - flags set for next jcc */
    _flags = (((int8_t)(uint8_t)(LO8(ebx) & LO8(ebx)) >= 0)); /* preserve test flags across 1 instruction(s) */
    POP32(esp, ebx);
    if (_flags != 0) goto loc_001E8DC7; /* jns: not sign (positive) */
""",
    ),
    GeneratedPatch(
        "RedShader LoadShader iLoadFlags preservation",
        """loc_00212FA0: ;
    (void)0; /* test MEM8(esp + 0x178), 1 - flags set for next jcc */
    POP32(esp, esi);
    if (TEST_NZ(MEM8(esp + 0x178), 1)) goto loc_00212FDC; /* jne: not equal / not zero */
""",
        """loc_00212FA0: ;
    (void)0; /* test MEM8(esp + 0x178), 1 - flags set for next jcc */
    _flags = (TEST_NZ(MEM8(esp + 0x178), 1)); /* preserve test flags across 1 instruction(s) */
    POP32(esp, esi);
    if (_flags != 0) goto loc_00212FDC; /* jne: not equal / not zero */
""",
    ),
    GeneratedPatch(
        "XPhysicalFree stack-sensitive address-space dispatch",
        """loc_002293E3: ;
    (void)0; /* test MEM8(esp + 0xB), 0x80 - flags set for next jcc */
    PUSH32(esp, MEM32(esp + 4));
    if (TEST_Z(MEM8(esp + 0xB), 0x80)) goto loc_002293F5; /* je: equal / zero */
""",
        """loc_002293E3: ;
    (void)0; /* test MEM8(esp + 0xB), 0x80 - flags set for next jcc */
    _flags = (TEST_Z(MEM8(esp + 0xB), 0x80)); /* preserve test flags across 1 instruction(s) */
    PUSH32(esp, MEM32(esp + 4));
    if (_flags != 0) goto loc_002293F5; /* je: equal / zero */
""",
    ),
    GeneratedPatch(
        "developer import reuse of completed streaming buffers",
        """loc_002228F0: ;
    if (CMP_NE(MEM32(ecx + 0x48), 3)) goto loc_00222947; /* jne: not equal / not zero */
""",
        """loc_002228F0: ;
    /* Developer chapter reloads can request a range whose pinned buffer was
     * completed earlier. Fulfil new reads from it with the retail reference
     * accounting; READ_DONE requests below are skipped, so no double-use. */
    if (MEM32(ecx + 0x48) != 3 &&
        !(MEM32(ecx + 0x48) == 4 && recomp_dev_region_import_active())) goto loc_00222947;
""",
    ),
    GeneratedPatch(
        "streaming idle budget deadlock recovery",
        """loc_002221CD: ;
    ebx = MEM32(esi + 0x3B4);
    ebx = ebx + edi;
    if (CMP_GE(ebx, 0x7D000)) goto loc_00222236; /* jge: greater or equal (signed >=) */
""",
        """loc_002221CD: ;
    ebx = MEM32(esi + 0x3B4);
    ebx = ebx + edi;
    /* Permit one bounded read when pinned completed buffers leave disk idle. */
    if (CMP_GE(ebx, 0x7D000) && !recomp_stream_admit_idle(esi, edi)) goto loc_00222236;
""",
    ),
    GeneratedPatch(
        "RedCounted RequestAsset boolean result",
        """    MEM32(esi + 0x10) = eax;
    (void)0; /* test eax, eax - flags set for next jcc */
    _flags = (TEST_NZ(eax, eax)); /* preserve test flags across 2 instruction(s) */
    POP32(esp, edi);
    SET_LO8(eax, 1);
""",
        """    MEM32(esi + 0x10) = eax;
    /* The retail instruction is SETNE AL. Preserve the tested request
     * pointer instead of treating a failed virtual-disk lookup as success. */
    (void)0; /* test eax, eax - flags set for next jcc */
    _flags = (TEST_NZ(eax, eax)); /* preserve TEST flags across POP/SETNE */
    POP32(esp, edi);
    SET_LO8(eax, (_flags != 0) ? 1 : 0);
""",
    ),
    GeneratedPatch(
        "RedAssetRequest completed-read flag preservation",
        """    edx = MEM32(ecx + 0x14);
    PUSH32(esp, esi);
    esi = MEM32(edx + 0x20);
    eax = 2;
    (void)0; /* cmp esi, eax - flags set for next jcc */
    POP32(esp, esi);
    if (CMP_NE(esi, eax)) goto loc_002181DA; /* jne: not equal / not zero */
""",
        """    edx = MEM32(ecx + 0x14);
    PUSH32(esp, esi);
    esi = MEM32(edx + 0x20);
    eax = 2;
    (void)0; /* cmp esi, eax - flags set for next jcc */
    _flags = (CMP_NE(esi, eax)); /* preserve cmp flags across 1 instruction(s) */
    POP32(esp, esi);
    if (_flags != 0) goto loc_002181DA; /* jne: not equal / not zero */
""",
    ),
    GeneratedPatch(
        "DirectSound zero routing-count guard",
        """loc_002A33FD: ;
    edi = ZX8(MEM8(ecx + 0x64));
    eax = MEM32(ebx + 0x24);
    edx = 0; /* xor self */
    { uint64_t _dividend = ((uint64_t)edx << 32) | eax;
      eax = (uint32_t)(_dividend / (uint32_t)edi);
      edx = (uint32_t)(_dividend % (uint32_t)edi); }
    esi = 0; /* xor self */
""",
        """loc_002A33FD: ;
    edi = ZX8(MEM8(ecx + 0x64));
    /* The XDK routine has a zero-count exit immediately after this divide.
     * Take it before DIV as well when an incompletely initialized voice has
     * no routes. Xemu identifies the downstream writes as VOICE_CFG_* and
     * Cxbx likewise treats unused routes as silent. Nonzero retail voices
     * retain their original arithmetic and MCPX register programming. */
    if (edi == 0u) {
        goto loc_002A3555;
    }
    eax = MEM32(ebx + 0x24);
    edx = 0; /* xor self */
    { uint64_t _dividend = ((uint64_t)edx << 32) | eax;
      eax = (uint32_t)(_dividend / (uint32_t)edi);
      edx = (uint32_t)(_dividend % (uint32_t)edi); }
    esi = 0; /* xor self */
""",
    ),
    GeneratedPatch(
        "RsMain platform-table CMOVE flag preservation",
        """loc_00187CD7: ;
    esp = esp + 8;
    (void)0; /* cmp eax, 0xE02DEBB6u - flags set for next jcc */
    eax = MEM32(0x42E3E4);
    if (CMP_EQ(eax, 0xE02DEBB6u)) ebx = esi; /* cmove */
""",
        """loc_00187CD7: ;
    esp = esp + 8;
    (void)0; /* cmp eax, 0xE02DEBB6u - flags set for next jcc */
    _flags = (CMP_EQ(eax, 0xE02DEBB6u)); /* preserve cmp flags across 1 instruction(s) */
    eax = MEM32(0x42E3E4);
    if (_flags != 0) ebx = esi; /* cmove */
""",
    ),
    GeneratedPatch(
        "MSVC x87 FRNDINT semantics",
        """    fp_push(MEMD(esp + 0xC)); /* fld double */
    /* FPU: frndint  */
    MEMD(esp) = fp_top(); fp_pop(); /* fstp */
""",
        """    fp_push(MEMD(esp + 0xC)); /* fld double */
    /* FRNDINT obeys the x87 rounding-control field and records precision
     * loss even when the default Xbox/MSVC control word masks it. */
    {
        double source = fp_top();
        double rounded;
        switch ((g_x87_control_word >> 10) & 3u) {
        case 1u: rounded = floor(source); break;
        case 2u: rounded = ceil(source); break;
        case 3u: rounded = trunc(source); break;
        default: rounded = nearbyint(source); break;
        }
        if (rounded != source)
            g_x87_status_word |= 0x20u;
        fp_top() = rounded;
    }
    MEMD(esp) = fp_top(); fp_pop(); /* fstp */
""",
    ),
    GeneratedPatch(
        "RedBlockPool current-block flag preservation",
        """    PUSH32(esp, ebp);
    ebp = MEM32(edx + eax * 4 + -4);
    edi = edi + ebx;
    (void)0; /* cmp edi, ebp - flags set for next jcc */
    POP32(esp, ebp);
    if (CMP_G(edi, ebp)) goto loc_0012107F; /* jg: greater (signed >) */
""",
        """    PUSH32(esp, ebp);
    ebp = MEM32(edx + eax * 4 + -4);
    edi = edi + ebx;
    (void)0; /* cmp edi, ebp - flags set for next jcc */
    _flags = (CMP_G(edi, ebp)); /* preserve cmp flags across 1 instruction(s) */
    POP32(esp, ebp);
    if (_flags != 0) goto loc_0012107F; /* jg: greater (signed >) */
""",
    ),
    GeneratedPatch(
        "RedMemory SmallBlockPool minimum-free flag preservation",
        """    edx--;
    (void)0; /* cmp esi, edx - flags set for next jcc */
    MEM32(ecx + 0x14) = edx;
    POP32(esp, esi);
    if (CMP_LE(esi, edx)) goto loc_001F66DF; /* jle: less or equal (signed <=) */
""",
        """    edx--;
    (void)0; /* cmp esi, edx - flags set for next jcc */
    _flags = (CMP_LE(esi, edx)); /* preserve cmp flags across 2 instruction(s) */
    MEM32(ecx + 0x14) = edx;
    POP32(esp, esi);
    if (_flags != 0) goto loc_001F66DF; /* jle: less or equal (signed <=) */
""",
    ),
    GeneratedPatch(
        "XG texture dimension BSF",
        """loc_0029B952: ;
    PUSH32(esp, ecx);
    MEM32(esp) = ecx;
    /* TODO: bsf eax, dword ptr [esp] */
    POP32(esp, ecx);
""",
        """loc_0029B952: ;
    PUSH32(esp, ecx);
    MEM32(esp) = ecx;
    /* Retail BSF returns the log2 exponent for power-of-two texture
     * dimensions. Leaving EAX stale made 512x512 DXT1 report 16 bytes. */
    {
        uint32_t bsf_value = MEM32(esp);
        eax = 0;
        if (bsf_value != 0u) {
            while ((bsf_value & 1u) == 0u) {
                ++eax;
                bsf_value >>= 1;
            }
        }
    }
    POP32(esp, ecx);
""",
    ),
    GeneratedPatch(
        "PblConfig line-reader position ICALL restore point",
        """loc_001EF410: ;
    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, ecx);
    PUSH32(esp, esi);
    esi = ecx;
    ecx = MEM32(esi + 0x418);
    eax = 0; /* xor self */
    MEM32(esi + 4) = eax;
    MEM32(esi + 8) = eax;
    eax = MEM32(ecx);
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(eax + 0x1C), _icall_esp); /* indirect call */
    }
""",
        """loc_001EF410: ;
    PUSH32(esp, ecx);
    PUSH32(esp, esi);
    esi = ecx;
    ecx = MEM32(esi + 0x418);
    eax = 0; /* xor self */
    MEM32(esi + 4) = eax;
    MEM32(esi + 8) = eax;
    eax = MEM32(ecx);
    /* Preserve the line-reader's saved ECX/ESI frame when a virtual file
     * position call cannot be dispatched. The generic ICALL scan mistook
     * these prologue saves for arguments and restored ESP above them. */
    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(eax + 0x1C), _icall_esp); /* indirect call */
    }
""",
    ),
    GeneratedPatch(
        "PblConfig line-reader read ICALL restore point",
        """    ecx = MEM32(esi + 0x418);
    edx = MEM32(ecx);
    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, ebx);
    PUSH32(esp, 1);
    eax = esp + 0xF;
""",
        """    ecx = MEM32(esi + 0x418);
    edx = MEM32(ecx);
    PUSH32(esp, ebx);
    /* EBX is a delayed callee-save, not an argument to Read. Keep it below
     * the failure restore point so the matching POP cannot advance ESP. */
    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, 1);
    eax = esp + 0xF;
""",
    ),
    GeneratedPatch(
        "retail XInputGetState host bridge",
        """void sub_002D54AC(void)
{
    int _flags = 0; /* fallback flag var */
    RECOMP_TRACE_FUNC(0x002D54ACu);

loc_002D54AC: ;
    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, ebx);
    PUSH32(esp, esi);
    ebx = 0; /* xor self */
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(0x2DBDC0), _icall_esp); /* indirect call */
    }

loc_002D54B6: ;
    edx = MEM32(esp + 0xC);
    ecx = MEM32(edx + 0xA3);
    if (TEST_Z(MEM8(ecx + 0x28), 0x10)) goto loc_002D54CB; /* je: equal / zero */

loc_002D54C6: ;
    PUSH32(esp, 0x57);
    POP32(esp, esi);
    goto loc_002D5510;

loc_002D54CB: ;
    ecx = MEM32(edx);
    if (TEST_Z(ecx, ecx)) goto loc_002D54D7; /* je: equal / zero */

loc_002D54D1: ;
    if (TEST_Z(MEM8(ecx + 4), 2)) goto loc_002D54DC; /* je: equal / zero */

loc_002D54D7: ;
    ebx = 0x48F;

loc_002D54DC: ;
    ecx = MEM32(edx + 8);
    PUSH32(esp, edi);
    edi = MEM32(esp + 0x14);
    MEM32(edi) = ecx;
    MEM8(edx + 0xA2) = MEM8(edx + 0xA2) & 0xEF;
    ecx = MEM32(edx + 0xA3);
    ecx = MEM32(ecx + 8);
    ecx = ZX8(MEM8(ecx));
    esi = edx + 0x14;
    edx = ecx;
    edi = edi + 4;
    ecx = ecx >> 2;
    XBOX_MEMCPY(edi, esi, ecx * 4);
    esi += ecx * 4; edi += ecx * 4; ecx = 0; /* rep movsd */
    ecx = edx;
    ecx = ecx & 3;
    XBOX_MEMCPY(edi, esi, ecx);
    esi += ecx; edi += ecx; ecx = 0; /* rep movsb */
    esi = ebx;
    POP32(esp, edi);

loc_002D5510: ;
    SET_LO8(ecx, LO8(eax));
    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(0x2DBCEC), _icall_esp); /* indirect call */
    }

loc_002D5518: ;
    eax = esi;
    POP32(esp, esi);
    POP32(esp, ebx);
    esp += 12; return; /* ret 8 */

}""",
        """void sub_002D54AC(void)
{
    RECOMP_TRACE_FUNC(0x002D54ACu);
    eax = recomp_xinput_get_state(MEM32(esp + 4u), MEM32(esp + 8u));
    esp += 12u;
    return;
}""",
    ),
    GeneratedPatch(
        "retail XInputOpen host bridge",
        """void sub_002D5272(void)
{
    uint32_t ebp;
    int _flags = 0; /* fallback flag var */
    ebp = g_seh_ebp; /* inherit guest caller frame */
    RECOMP_TRACE_FUNC(0x002D5272u);

loc_002D5272: ;
    PUSH32(esp, ebp);
    ebp = esp;
    PUSH32(esp, ecx);
    ecx = MEM32(ebp + 8);
    MEM32(ebp + -4) = MEM32(ebp + -4) & 0;
    PUSH32(esp, 0); sub_002D563F(); /* call 0x002D563F */

loc_002D5282: ;
    if (TEST_NZ(eax, eax)) goto loc_002D5291; /* jne: not equal / not zero */

loc_002D5286: ;
    PUSH32(esp, 0x57);
    PUSH32(esp, 0); sub_0022B178(); /* call 0x0022B178 */

loc_002D528D: ;
    eax = 0; /* xor self */
    goto loc_002D52C4;

loc_002D5291: ;
    PUSH32(esp, esi);
    esi = MEM32(ebp + 0x14);
    if (TEST_NZ(esi, esi)) goto loc_002D529C; /* jne: not equal / not zero */

loc_002D5299: ;
    esi = MEM32(eax + 0x10);

loc_002D529C: ;
    (void)0; /* cmp MEM32(ebp + 0x10), 1 - flags set for next jcc */
    edx = MEM32(ebp + 0xC);
    if (CMP_NE(MEM32(ebp + 0x10), 1)) goto loc_002D52A8; /* jne: not equal / not zero */

loc_002D52A5: ;
    edx = edx + 0x10;

loc_002D52A8: ;
    PUSH32(esp, esi);
    ecx = ebp + -4;
    PUSH32(esp, ecx);
    ecx = eax;
    PUSH32(esp, 0); sub_002D7239(); /* call 0x002D7239 */

loc_002D52B4: ;
    (void)0; /* cmp MEM32(ebp + -4), 0 - flags set for next jcc */
    POP32(esp, esi);
    if (CMP_NE(MEM32(ebp + -4), 0)) goto loc_002D52C1; /* jne: not equal / not zero */

loc_002D52BB: ;
    PUSH32(esp, eax);
    PUSH32(esp, 0); sub_0022B178(); /* call 0x0022B178 */

loc_002D52C1: ;
    eax = MEM32(ebp + -4);

loc_002D52C4: ;
    esp = ebp;
    POP32(esp, ebp); /* leave */
    esp += 20; return; /* ret 16 */

}""",
        """void sub_002D5272(void)
{
    RECOMP_TRACE_FUNC(0x002D5272u);
    eax = recomp_xinput_open(MEM32(esp + 8u));
    esp += 20u;
    return;
}""",
    ),
    GeneratedPatch(
        "retail XInputClose host bridge",
        """void sub_002D52C8(void)
{
    RECOMP_TRACE_FUNC(0x002D52C8u);

loc_002D52C8: ;
    ecx = MEM32(esp + 4);
    PUSH32(esp, 0); sub_002D6EA5(); /* call 0x002D6EA5 */

loc_002D52D1: ;
    esp += 8; return; /* ret 4 */

}""",
        """void sub_002D52C8(void)
{
    RECOMP_TRACE_FUNC(0x002D52C8u);
    recomp_xinput_close(MEM32(esp + 4u));
    esp += 8u;
    return;
}""",
    ),
    GeneratedPatch(
        "retail XInputGetCapabilities host bridge",
        """void sub_002D52D4(void)
{
    uint32_t ebp;
    int _flags = 0; /* fallback flag var */
    ebp = g_seh_ebp; /* inherit guest caller frame */
    RECOMP_TRACE_FUNC(0x002D52D4u);

loc_002D52D4: ;
    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, ebp);
    ebp = esp;
    esp = esp - 0x48;
    MEM32(ebp + -8) = MEM32(ebp + -8) & 0;
    PUSH32(esp, ebx);
    ebx = MEM32(0x2DBDC0);
    PUSH32(esp, esi);
    PUSH32(esp, edi);
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(ebx, _icall_esp); /* indirect call */
    }

loc_002D52E9: ;
    MEM8(ebp + -1) = LO8(eax);
    eax = MEM32(ebp + 8);
    esi = MEM32(eax);
    if (TEST_Z(esi, esi)) goto loc_002D548A; /* je: equal / zero */

loc_002D52F9: ;
    if (TEST_NZ(MEM8(esi + 4), 2)) goto loc_002D548A; /* jne: not equal / not zero */

loc_002D5303: ;
    edx = MEM32(ebp + 0xC);
    eax = 0; /* xor self */
    PUSH32(esp, 6);
    POP32(esp, ecx);
    edi = edx;
    { uint32_t _i; for (_i = 0; _i < ecx; _i++) MEM32(edi + _i*4) = eax; }
    edi += ecx * 4; ecx = 0; /* rep stosd */
    MEM8(edi) = LO8(eax); edi++; /* stosb */
    SET_LO8(eax, MEM8(esi + 0xB));
    MEM8(edx) = LO8(eax);
    eax = MEM32(esi + 0xE);
    if (TEST_Z(MEM8(eax + 0x28), 1)) goto loc_002D532A; /* je: equal / zero */

loc_002D531E: ;
    MEM32(ebp + -8) = 5;
    goto loc_002D5491;

loc_002D532A: ;
    eax = MEM32(eax + 0xC);
    eax = ZX8(MEM8(eax));
    MEM32(ebp + -20) = MEM32(ebp + -20) & 0;
    MEM32(ebp + -56) = MEM32(ebp + -56) & 0;
    ecx = ebp + -16;
    MEM32(ebp + -12) = ecx;
    MEM32(ebp + -16) = ecx;
    ecx = ebp + -24;
    MEM32(ebp + -60) = ecx;
    ecx = eax + 2;
    edx = edx + 0x13;
    MEM8(ebp + -24) = 0;
    MEM8(ebp + -22) = 4;
    MEM8(ebp + -72) = 0x30;
    MEM8(ebp + -71) = 0x40;
    edi = 0x2D75DD;
    MEM32(ebp + -64) = edi;
    MEM32(ebp + -48) = edx;
    MEM32(ebp + -52) = ecx;
    MEM8(ebp + -44) = 2;
    MEM8(ebp + -43) = 1;
    MEM8(ebp + -42) = 0;
    MEM8(ebp + -32) = 0xC1;
    MEM8(ebp + -31) = 1;
    MEM16(ebp + -30) = 0x200;
    SET_LO16(ecx, ZX8(MEM8(esi + 5)));
    eax = eax + 2;
    MEM16(ebp + -26) = LO16(eax);
    eax = ebp + -72;
    MEM16(ebp + -28) = LO16(ecx);
    ecx = MEM32(esi);
    PUSH32(esp, eax);
    PUSH32(esp, 0); sub_002D79B6(); /* call 0x002D79B6 */

loc_002D53A0: ;
    SET_LO8(ecx, MEM8(ebp + -1));
    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(0x2DBCEC), _icall_esp); /* indirect call */
    }

loc_002D53A9: ;
    ecx = MEM32(esi);
    eax = ebp + -24;
    PUSH32(esp, eax);
    edx = ebp + -72;
    PUSH32(esp, 0); sub_002D6B3E(); /* call 0x002D6B3E */

loc_002D53B7: ;
    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(ebx, _icall_esp); /* indirect call */
    }

loc_002D53B9: ;
    MEM8(ebp + -1) = LO8(eax);
    eax = MEM32(ebp + 8);
    ecx = 0; /* xor self */
    if (CMP_EQ(MEM32(eax), ecx)) goto loc_002D548A; /* je: equal / zero */

loc_002D53C9: ;
    if (TEST_NZ(MEM8(esi + 4), 2)) goto loc_002D548A; /* jne: not equal / not zero */

loc_002D53D3: ;
    if (CMP_L(MEM32(ebp + -68), ecx)) goto loc_002D547D; /* jl: less (signed <) */

loc_002D53DC: ;
    eax = MEM32(esi + 0xE);
    eax = MEM32(eax + 8);
    eax = ZX8(MEM8(eax));
    edx = ebp + -24;
    MEM32(ebp + -60) = edx;
    edx = MEM32(ebp + 0xC);
    edx++;
    MEM32(ebp + -48) = edx;
    edx = eax + 2;
    eax = eax + 2;
    MEM8(ebp + -72) = 0x30;
    MEM8(ebp + -71) = 0x40;
    MEM32(ebp + -64) = edi;
    MEM32(ebp + -56) = ecx;
    MEM32(ebp + -52) = edx;
    MEM8(ebp + -44) = 2;
    MEM8(ebp + -43) = 1;
    MEM8(ebp + -42) = LO8(ecx);
    MEM8(ebp + -32) = 0xC1;
    MEM8(ebp + -31) = 1;
    MEM16(ebp + -30) = 0x100;
    SET_LO16(edx, ZX8(MEM8(esi + 5)));
    MEM16(ebp + -26) = LO16(eax);
    eax = ebp + -16;
    MEM32(ebp + -12) = eax;
    MEM32(ebp + -16) = eax;
    eax = ebp + -72;
    MEM16(ebp + -28) = LO16(edx);
    MEM8(ebp + -24) = LO8(ecx);
    MEM8(ebp + -22) = 4;
    MEM32(ebp + -20) = ecx;
    ecx = MEM32(esi);
    PUSH32(esp, eax);
    PUSH32(esp, 0); sub_002D79B6(); /* call 0x002D79B6 */

loc_002D544D: ;
    SET_LO8(ecx, MEM8(ebp + -1));
    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(0x2DBCEC), _icall_esp); /* indirect call */
    }

loc_002D5456: ;
    ecx = MEM32(esi);
    eax = ebp + -24;
    PUSH32(esp, eax);
    edx = ebp + -72;
    PUSH32(esp, 0); sub_002D6B3E(); /* call 0x002D6B3E */

loc_002D5464: ;
    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(ebx, _icall_esp); /* indirect call */
    }

loc_002D5466: ;
    MEM8(ebp + -1) = LO8(eax);
    eax = MEM32(ebp + 8);
    if (CMP_EQ(MEM32(eax), 0)) goto loc_002D548A; /* je: equal / zero */

loc_002D5471: ;
    if (TEST_NZ(MEM8(esi + 4), 2)) goto loc_002D548A; /* jne: not equal / not zero */

loc_002D5477: ;
    if (CMP_GE(MEM32(ebp + -68), 0)) goto loc_002D5491; /* jge: greater or equal (signed >=) */

loc_002D547D: ;
    PUSH32(esp, MEM32(ebp + -68));
    PUSH32(esp, 0); sub_002D7620(); /* call 0x002D7620 */

loc_002D5485: ;
    MEM32(ebp + -8) = eax;
    goto loc_002D5491;

loc_002D548A: ;
    MEM32(ebp + -8) = 0x48F;

loc_002D5491: ;
    eax = MEM32(ebp + 0xC);
    SET_LO8(ecx, MEM8(ebp + -1));
    MEM16(eax + 1) = MEM16(eax + 1) & 0;
    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(0x2DBCEC), _icall_esp); /* indirect call */
    }

loc_002D54A2: ;
    eax = MEM32(ebp + -8);
    POP32(esp, edi);
    POP32(esp, esi);
    POP32(esp, ebx);
    esp = ebp;
    POP32(esp, ebp); /* leave */
    esp += 12; return; /* ret 8 */

}""",
        """void sub_002D52D4(void)
{
    RECOMP_TRACE_FUNC(0x002D52D4u);
    eax = recomp_xinput_get_capabilities(MEM32(esp + 4u), MEM32(esp + 8u));
    esp += 12u;
    return;
}""",
    ),
    GeneratedPatch(
        "retail XInputSetState host bridge",
        """void sub_002D551F(void)
{
    int _flags = 0; /* fallback flag var */
    RECOMP_TRACE_FUNC(0x002D551Fu);

loc_002D551F: ;
    ecx = MEM32(esp + 4);
    eax = ecx + 0xA3;
    edx = MEM32(eax);
    if (TEST_Z(MEM8(edx + 0x28), 0x20)) goto loc_002D5536; /* je: equal / zero */

loc_002D5531: ;
    PUSH32(esp, 0x57);
    POP32(esp, eax);
    goto loc_002D554F;

loc_002D5536: ;
    edx = MEM32(esp + 8);
    MEM8(edx + 0x40) = 0;
    eax = MEM32(eax);
    eax = MEM32(eax + 0xC);
    SET_LO8(eax, MEM8(eax));
    SET_LO8(eax, LO8(eax) + 2);
    MEM8(edx + 0x41) = LO8(eax);
    PUSH32(esp, 0); sub_002D6F39(); /* call 0x002D6F39 */

loc_002D554F: ;
    esp += 12; return; /* ret 8 */

}""",
        """void sub_002D551F(void)
{
    RECOMP_TRACE_FUNC(0x002D551Fu);
    eax = recomp_xinput_set_state(MEM32(esp + 4u), MEM32(esp + 8u));
    esp += 12u;
    return;
}""",
    ),
    GeneratedPatch(
        "retail XGetDevices host bridge",
        """void sub_002D5579(void)
{
    RECOMP_TRACE_FUNC(0x002D5579u);

loc_002D5579: ;
    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, esi);
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(0x2DBDC0), _icall_esp); /* indirect call */
    }

loc_002D5580: ;
    edx = MEM32(esp + 8);
    esi = MEM32(edx);
    MEM32(edx + 4) = MEM32(edx + 4) & 0;
    SET_LO8(ecx, LO8(eax));
    MEM32(edx + 8) = esi;
    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(0x2DBCEC), _icall_esp); /* indirect call */
    }

loc_002D5595: ;
    eax = esi;
    POP32(esp, esi);
    esp += 8; return; /* ret 4 */

}""",
        """void sub_002D5579(void)
{
    RECOMP_TRACE_FUNC(0x002D5579u);
    eax = recomp_xinput_get_devices();
    esp += 8u;
    return;
}""",
    ),
    GeneratedPatch(
        "retail XGetDeviceChanges host bridge",
        """void sub_002D559B(void)
{
    uint32_t ebp;
    int _flags = 0; /* fallback flag var */
    int _cf = 0; /* carry flag */
    ebp = g_seh_ebp; /* inherit guest caller frame */
    RECOMP_TRACE_FUNC(0x002D559Bu);

loc_002D559B: ;
    PUSH32(esp, ebp);
    ebp = esp;
    PUSH32(esp, esi);
    esi = MEM32(ebp + 8);
    eax = 0; /* xor self */
    if (CMP_NE(MEM32(esi + 4), eax)) goto loc_002D55B5; /* jne: not equal / not zero */

loc_002D55A9: ;
    ecx = MEM32(ebp + 0xC);
    MEM32(ecx) = eax;
    ecx = MEM32(ebp + 0x10);
    MEM32(ecx) = eax;
    goto loc_002D5603;

loc_002D55B5: ;
    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, ebx);
    PUSH32(esp, edi);
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(0x2DBDC0), _icall_esp); /* indirect call */
    }

loc_002D55BD: ;
    ecx = MEM32(esi + 8);
    ebx = MEM32(ebp + 0xC);
    ecx = ~ecx;
    ecx = ecx & MEM32(esi);
    MEM32(ebx) = ecx;
    edx = MEM32(esi);
    ecx = MEM32(ebp + 0x10);
    edx = ~edx;
    edx = edx & MEM32(esi + 8);
    MEM32(ecx) = edx;
    edi = MEM32(esi + 4);
    edi = edi & MEM32(esi + 8);
    edi = edi & MEM32(esi);
    edx = edx | edi;
    MEM32(ecx) = edx;
    MEM32(ebx) = MEM32(ebx) | edi;
    ecx = MEM32(esi);
    MEM32(esi + 4) = MEM32(esi + 4) & 0;
    MEM32(esi + 8) = ecx;
    SET_LO8(ecx, LO8(eax));
    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(0x2DBCEC), _icall_esp); /* indirect call */
    }

loc_002D55F4: ;
    eax = MEM32(ebx);
    ecx = MEM32(ebp + 0x10);
    eax = eax | MEM32(ecx);
    POP32(esp, edi);
    { uint32_t _neg_value = (uint32_t)(eax);
      _cf = (_neg_value != 0);
    eax = (uint32_t)(-(int32_t)_neg_value); }
    eax = _cf ? 0xFFFFFFFF : 0; /* sbb self (CF extend) */
    { uint32_t _neg_value = (uint32_t)(eax);
      _cf = (_neg_value != 0);
    eax = (uint32_t)(-(int32_t)_neg_value); }
    POP32(esp, ebx);

loc_002D5603: ;
    POP32(esp, esi);
    POP32(esp, ebp);
    esp += 16; return; /* ret 12 */

}""",
        """void sub_002D559B(void)
{
    RECOMP_TRACE_FUNC(0x002D559Bu);
    eax = recomp_xinput_get_device_changes(MEM32(esp + 8u), MEM32(esp + 12u));
    esp += 16u;
    return;
}""",
    ),
    GeneratedPatch(
        "PblJoystick logical input checkpoint",
        """loc_0020F0D6: ;
    POP32(esp, esi);""",
        """loc_0020F0D6: ;
    recomp_input_logical_checkpoint(esi);
    POP32(esp, esi);""",
    ),

    GeneratedPatch(
        "RsActorVehicleHumanPlayer inventory-clear ICALL restore point",
        """    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(edx + 0x3B0), _icall_esp); /* indirect call */
    }

loc_0005CD49: ;""",
        """    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(edx + 0x3B0), _icall_esp); /* indirect call */
    g_esp = _icall_esp;
    }

loc_0005CD49: ;""",
    ),
    GeneratedPatch(
        "RsBriefing reloads Lua-state ESI before sFaction",
        """loc_00175EE6: ;
    PUSH32(esp, 0x2F0A48);""",
        """loc_00175EE6: ;
    ecx = MEM32(0x403378);
    esi = MEM32(ecx + 0x1C);
    PUSH32(esp, 0x2F0A48);""",
    ),
    GeneratedPatch(
        "RsActorVehicleHumanPlayer init M4 create entry checkpoint",
        """loc_0005BD5C: ;
    edx = MEM32(esi);
    { uint32_t _icall_esp = g_esp;""",
        """loc_0005BD5C: ;
    edx = MEM32(esi);
    recomp_player_loadout_checkpoint(0u, esi, 0x2F27C138u,
                                     MEM32(edx + 0x3ACu));
    { uint32_t _icall_esp = g_esp;""",
    ),
    GeneratedPatch(
        "RsActorVehicleHumanPlayer init M4 create exit checkpoint",
        """loc_0005BD7B: ;
    eax = MEM32(esi);""",
        """loc_0005BD7B: ;
    recomp_player_loadout_checkpoint(1u, esi, 0x2F27C138u, eax);
    eax = MEM32(esi);""",
    ),
    GeneratedPatch(
        "RsActorVehicleHumanPlayer primary-item create entry checkpoint",
        """loc_00059733: ;
    eax = MEM32(edi + 0x7A0);""",
        """loc_00059733: ;
    recomp_player_loadout_checkpoint(10u, edi, MEM32(esp + 0x818u), 0u);
    eax = MEM32(edi + 0x7A0);""",
    ),
    GeneratedPatch(
        "RsActorVehicleHumanPlayer primary-item sprout checkpoint",
        """loc_000597BF: ;
    esi = eax;
    if (TEST_Z(esi, esi)) goto loc_000597E2; /* je: equal / zero */""",
        """loc_000597BF: ;
    esi = eax;
    recomp_player_loadout_checkpoint(11u, edi, MEM32(esp + 0x81Cu), esi);
    if (TEST_Z(esi, esi)) goto loc_000597E2; /* je: equal / zero */""",
    ),
    GeneratedPatch(
        "RsActorVehicleHumanPlayer primary-item cast checkpoint",
        """loc_000597E4: ;
    SET_LO8(ecx, MEM8(eax + 0x190));""",
        """loc_000597E4: ;
    recomp_player_loadout_checkpoint(12u, edi, MEM32(esp + 0x81Cu), eax);
    SET_LO8(ecx, MEM8(eax + 0x190));""",
    ),
    GeneratedPatch(
        "RsActorVehicleHumanPlayer primary-item add checkpoint",
        """loc_000597FC: ;
    POP32(esp, esi);""",
        """loc_000597FC: ;
    recomp_player_loadout_checkpoint(13u, edi, MEM32(esp + 0x81Cu), esi);
    POP32(esp, esi);""",
    ),
    GeneratedPatch(
        "RsActorVehicleHumanPlayer inventory restore entry checkpoint",
        """loc_0005CD10: ;
    PUSH32(esp, ebp);""",
        """loc_0005CD10: ;
    recomp_player_loadout_checkpoint(20u, ecx, 0u, 0u);
    PUSH32(esp, ebp);""",
    ),
    GeneratedPatch(
        "RsActorVehicleHumanPlayer inventory restore count checkpoint",
        """loc_0005CD3F: ;
    edx = MEM32(edi);""",
        """loc_0005CD3F: ;
    recomp_player_loadout_checkpoint(21u, edi, 0x372C1990u,
                                     MEM32(esp + 0x28u));
    edx = MEM32(edi);""",
    ),
    GeneratedPatch(
        "RsActorVehicleHumanPlayer inventory restore cleared checkpoint",
        """loc_0005CD49: ;
    eax = MEM32(esp + 0x28);""",
        """loc_0005CD49: ;
    recomp_player_loadout_checkpoint(22u, MEM32(esp + 0x24u),
                                     0x372C1990u, edi);
    eax = MEM32(esp + 0x28);""",
    ),
    GeneratedPatch(
        "RsActorVehicleHumanPlayer inventory restore template checkpoint",
        """loc_0005CE56: ;
    recomp_xmm_zero(xmm0v); /* xorps self = zero */""",
        """loc_0005CE56: ;
    recomp_player_loadout_checkpoint(23u, MEM32(esp + 0x24u),
                                     MEM32(esp + 0x3Cu), esi);
    recomp_xmm_zero(xmm0v); /* xorps self = zero */""",
    ),
    GeneratedPatch(
        "RsActorVehicleHumanPlayer inventory restore sprout checkpoint",
        """loc_0005CEDC: ;
    edi = eax;
    if (CMP_EQ(edi, ebx)) goto loc_0005CF1E; /* je: equal / zero */""",
        """loc_0005CEDC: ;
    edi = eax;
    recomp_player_loadout_checkpoint(24u, MEM32(esp + 0x24u),
                                     MEM32(esp + 0x3Cu), edi);
    if (CMP_EQ(edi, ebx)) goto loc_0005CF1E; /* je: equal / zero */""",
    ),
    GeneratedPatch(
        "RsActorVehicleHumanPlayer inventory restore cast checkpoint",
        """loc_0005CEE9: ;
    edx = MEM32(0x30C25C);""",
        """loc_0005CEE9: ;
    recomp_player_loadout_checkpoint(25u, MEM32(esp + 0x24u),
                                     MEM32(esp + 0x3Cu), eax);
    edx = MEM32(0x30C25C);""",
    ),
    GeneratedPatch(
        "RsActorVehicleHumanPlayer inventory restore re-add checkpoint",
        """loc_0005CEFB: ;
    eax = MEM32(esp + 0x20);""",
        """loc_0005CEFB: ;
    recomp_player_loadout_checkpoint(26u, MEM32(esp + 0x24u),
                                     MEM32(esp + 0x3Cu), edi);
    eax = MEM32(esp + 0x20);""",
    ),
    GeneratedPatch(
        "RsActorVehicleHumanPlayer inventory restore loop checkpoint",
        """loc_0005CF1E: ;
    eax = MEM32(esp + 0x28);""",
        """loc_0005CF1E: ;
    recomp_player_loadout_checkpoint(27u, MEM32(esp + 0x24u),
                                     MEM32(esp + 0x3Cu), esi);
    eax = MEM32(esp + 0x28);""",
    ),
    GeneratedPatch(
        "RsActorVehicleHumanPlayer inventory restore primary completion checkpoint",
        """loc_0005CF2B: ;
    edi = MEM32(esp + 0x24);""",
        """loc_0005CF2B: ;
    recomp_player_loadout_checkpoint(28u, MEM32(esp + 0x24u),
                                     0x372C1990u, esi);
    edi = MEM32(esp + 0x24);""",
    ),
    GeneratedPatch(
        "RsActorVehicleHumanPlayer clear-primary entry checkpoint",
        """loc_00057960: ;
    PUSH32(esp, ebx);""",
        """loc_00057960: ;
    recomp_player_loadout_checkpoint(30u, ecx, 0u, 0u);
    PUSH32(esp, ebx);""",
    ),
    GeneratedPatch(
        "RsActorVehicleHumanPlayer clear-primary eliminate entry checkpoint",
        """loc_00057977: ;
    ecx = MEM32(esi);
    eax = MEM32(ecx);""",
        """loc_00057977: ;
    ecx = MEM32(esi);
    eax = MEM32(ecx);
    recomp_player_loadout_checkpoint(31u, ebx, MEM32(eax + 0x10u), ecx);""",
    ),
    GeneratedPatch(
        "RsActorVehicleHumanPlayer clear-primary eliminate exit checkpoint",
        """loc_0005797E: ;
    MEM32(esi) = 0;""",
        """loc_0005797E: ;
    recomp_player_loadout_checkpoint(32u, ebx, 0u, MEM32(esi));
    MEM32(esi) = 0;""",
    ),
    GeneratedPatch(
        "RsActorVehicleHumanPlayer clear-primary zero-count checkpoint",
        """loc_00057993: ;
    PUSH32(esp, 0);""",
        """loc_00057993: ;
    recomp_player_loadout_checkpoint(33u, ebx, 0u, edi);
    PUSH32(esp, 0);""",
    ),
    GeneratedPatch(
        "RsActorVehicleHumanPlayer clear-primary reset exit checkpoint",
        """loc_000579A6: ;
    POP32(esp, edi);""",
        """loc_000579A6: ;
    recomp_player_loadout_checkpoint(34u, ebx, 0u, eax);
    POP32(esp, edi);""",
    ),
    GeneratedPatch(
        "RsActorVehicleHumanPlayer clear-primary function exit checkpoint",
        """    MEM32(ebx + 0xA8C) = 0xFFFFFFFFu;
    POP32(esp, ebx);
    esp += 4; return; /* ret */""",
        """    MEM32(ebx + 0xA8C) = 0xFFFFFFFFu;
    recomp_player_loadout_checkpoint(35u, ebx, 0u, 0u);
    POP32(esp, ebx);
    esp += 4; return; /* ret */""",
    ),
    GeneratedPatch(
        "RsActorVehicleHuman equipped-primary reset entry checkpoint",
        """loc_0004FC90: ;
    eax = MEM32(esp + 4);""",
        """loc_0004FC90: ;
    recomp_player_loadout_checkpoint(40u, ecx, 0u, MEM32(esp + 4u));
    eax = MEM32(esp + 4);""",
    ),
    GeneratedPatch(
        "RsActorVehicleHuman equipped-primary sound checkpoint",
        """loc_0004FCB4: ;
    edx = MEM32(esi);""",
        """loc_0004FCB4: ;
    recomp_player_loadout_checkpoint(41u, esi, 0u, eax);
    edx = MEM32(esi);""",
    ),
    GeneratedPatch(
        "RsActorVehicleHuman equipped-primary AI lookup checkpoint",
        """loc_0004FCBE: ;
    if (TEST_Z(eax, eax)) goto loc_0004FCD6; /* je: equal / zero */""",
        """loc_0004FCBE: ;
    recomp_player_loadout_checkpoint(42u, esi, 0u, eax);
    if (TEST_Z(eax, eax)) goto loc_0004FCD6; /* je: equal / zero */""",
    ),
    GeneratedPatch(
        "RsActorVehicleHuman equipped-primary AI notify checkpoint",
        """loc_0004FCCC: ;
    edx = MEM32(eax);""",
        """loc_0004FCCC: ;
    recomp_player_loadout_checkpoint(43u, esi, 0u, eax);
    edx = MEM32(eax);""",
    ),
    GeneratedPatch(
        "RsActorVehicleHuman equipped-primary exit checkpoint",
        """loc_0004FCD6: ;
    POP32(esp, esi);""",
        """loc_0004FCD6: ;
    recomp_player_loadout_checkpoint(44u, esi, 0u, eax);
    POP32(esp, esi);""",
    ),
    GeneratedPatch(
        "RsActorVehicleHuman weapon-render toggle entry checkpoint",
        """    PUSH32(esp, edi);
    edi = ecx;
    eax = MEM32(edi + 0x7B8);""",
        """    PUSH32(esp, edi);
    edi = ecx;
    recomp_weapon_render_toggle_checkpoint(0u, edi, MEM8(esp + 0xC));
    eax = MEM32(edi + 0x7B8);""",
    ),
    GeneratedPatch(
        "RsActorVehicleHuman weapon-render nonempty exit checkpoint",
        """    POP32(esp, esi);
    MEM8(edi + 0x810) = LO8(ebx);
    POP32(esp, ebx);""",
        """    POP32(esp, esi);
    MEM8(edi + 0x810) = LO8(ebx);
    recomp_weapon_render_toggle_checkpoint(1u, edi, LO8(ebx));
    POP32(esp, ebx);""",
    ),
    GeneratedPatch(
        "RsActorVehicleHuman weapon-render empty exit checkpoint",
        """    SET_LO8(eax, MEM8(esp + 0xC));
    MEM8(edi + 0x810) = LO8(eax);
    POP32(esp, edi);""",
        """    SET_LO8(eax, MEM8(esp + 0xC));
    MEM8(edi + 0x810) = LO8(eax);
    recomp_weapon_render_toggle_checkpoint(1u, edi, LO8(eax));
    POP32(esp, edi);""",
    ),
    GeneratedPatch(
        "RsActorVehicleHuman inventory owner checkpoint",
        """    MEM32(esp + 0x18) = ecx;
    MEM32(esp + 0x14) = 0;
    if (CMP_LE((eax & eax), 0)) goto loc_00052456;""",
        """    MEM32(esp + 0x18) = ecx;
    MEM32(esp + 0x14) = 0;
    recomp_inventory_item_checkpoint(0u, ecx, MEM32(ecx + 0x7A0u),
                                     0u, 0u, 0u);
    if (CMP_LE((eax & eax), 0)) goto loc_00052456;""",
    ),
    GeneratedPatch(
        "RsActorVehicleHuman inventory dock checkpoint",
        """loc_00052340: ;
    ebx = eax;
    if (TEST_Z(ebx, ebx)) goto loc_00052430;""",
        """loc_00052340: ;
    ebx = eax;
    recomp_inventory_item_checkpoint(1u, MEM32(esp + 0x18u), esi,
                                     ebx, 0u, 0u);
    if (TEST_Z(ebx, ebx)) goto loc_00052430;""",
    ),
    GeneratedPatch(
        "RsActorVehicleHuman inventory renderable checkpoint",
        """loc_00052360: ;
    ecx = MEM32(0x643844);""",
        """loc_00052360: ;
    recomp_inventory_item_checkpoint(2u, MEM32(esp + 0x18u), esi,
                                     ebx, eax, 0u);
    ecx = MEM32(0x643844);""",
    ),
    GeneratedPatch(
        "RsActorVehicleHuman inventory matrix checkpoint",
        """loc_00052428: ;
    edx = MEM32(esi);
    ecx = esi;
    { uint32_t _icall_esp = g_esp;""",
        """loc_00052428: ;
    edx = MEM32(esi);
    ecx = esi;
    recomp_inventory_item_checkpoint(3u, MEM32(esp + 0x18u), esi,
                                     ebx, 0u, eax);
    { uint32_t _icall_esp = g_esp;""",
    ),
    GeneratedPatch(
        "RsActorVehicleHuman inventory stored-matrix checkpoint",
        """loc_00052430: ;
    eax = MEM32(esp + 0x14);""",
        """loc_00052430: ;
    recomp_inventory_item_checkpoint(4u, MEM32(esp + 0x18u), esi,
                                     ebx, 0u, 0u);
    eax = MEM32(esp + 0x14);""",
    ),
    GeneratedPatch(
        "RsActorVehicleHuman EnableCollision checkpoint",
        """    esi = ecx;
    if (TEST_Z(LO8(eax), LO8(eax))) goto loc_000522CD; /* je: equal / zero */""",
        """    esi = ecx;
    recomp_human_collision_checkpoint(1u, esi, LO8(eax), MEM32(esi + 0x76C));
    if (TEST_Z(LO8(eax), LO8(eax))) goto loc_000522CD; /* je: equal / zero */""",
    ),
    GeneratedPatch(
        "RsActorVehicleHuman EnableCollision created checkpoint",
        """loc_000522A1: ;
    esp = esp + 0x18;
    (void)0; /* test eax, eax - flags set for next jcc */
    MEM32(esi + 0x76C) = eax;""",
        """loc_000522A1: ;
    esp = esp + 0x18;
    (void)0; /* test eax, eax - flags set for next jcc */
    MEM32(esi + 0x76C) = eax;
    recomp_human_collision_checkpoint(2u, esi, 1u, eax);""",
    ),
    GeneratedPatch(
        "RsActorVehicleHuman EnableCollision removed checkpoint",
        """loc_000522DF: ;
    esp = esp + 8;
    MEM32(esi + 0x76C) = 0;""",
        """loc_000522DF: ;
    esp = esp + 8;
    MEM32(esi + 0x76C) = 0;
    recomp_human_collision_checkpoint(3u, esi, 0u, 0u);""",
    ),
    GeneratedPatch(
        "retail XInitDevices host bridge and stdcall cleanup",
        """void sub_002D5574(void)
{
    uint32_t ebp;
    ebp = g_seh_ebp; /* inherit guest caller frame */
    RECOMP_TRACE_FUNC(0x002D5574u);

loc_002D5574: ;
    g_seh_ebp = ebp; sub_002D4876(); return; /* tail jmp 0x002D4876 */

}""",
        """void sub_002D5574(void)
{
    RECOMP_TRACE_FUNC(0x002D5574u);
    recomp_xinput_init_devices();
    esp += 12u;
    return;
}""",
    ),
    GeneratedPatch(
        "RsFrontEnd SetCurrentMenu transition checkpoint",
        """loc_000D3E60: ;
    PUSH32(esp, esi);
    esi = ecx;
    ecx = MEM32(esi + 0x3EB8);""",
        """loc_000D3E60: ;
    PUSH32(esp, esi);
    esi = ecx;
    recomp_frontend_menu_checkpoint(esi, MEM32(esp + 8u));
    ecx = MEM32(esi + 0x3EB8);""",
    ),
    GeneratedPatch(
        "MenuTitleScreen ProcessInput checkpoint",
        """loc_000D4970: ;
    if (CMP_NE(MEM32(esp + 4), 7)) goto loc_000D49A1; /* jne: not equal / not zero */""",
        """loc_000D4970: ;
    recomp_frontend_input_checkpoint(ecx, MEM32(esp + 4u), MEM32(esp + 8u));
    /* The title accepts the rebindable Menu Confirm as well as Start. */
    if (CMP_NE(MEM32(esp + 4), 7) && CMP_NE(MEM32(esp + 4), 5)) goto loc_000D49A1; /* jne: not equal / not zero */""",
    ),
    GeneratedPatch(
        "MenuMain ProcessInput checkpoint",
        """loc_000D8030: ;
    PUSH32(esp, ebx);""",
        """loc_000D8030: ;
    if(recomp_options_input_checkpoint(ecx,MEM32(esp+4u),MEM32(esp+8u))==1u){esp+=12u;return;}
    recomp_frontend_input_checkpoint(ecx, MEM32(esp + 4u), MEM32(esp + 8u));
    PUSH32(esp, ebx);""",
    ),
    GeneratedPatch(
        "MenuOptions ProcessInput recomp settings checkpoint",
        """loc_000D8400: ;
    PUSH32(esp, ebp);""",
        """loc_000D8400: ;
    eax = recomp_options_input_checkpoint(ecx, MEM32(esp + 4u),
                                          MEM32(esp + 8u));
    if (eax == 1u) {
        esp += 12u;
        return;
    }
    if (eax == 2u)
        MEM32(esp + 4u) = 4u;
    PUSH32(esp, ebp);""",
    ),
    GeneratedPatch(
        "RsLoadSaveGame state push checkpoint",
        """loc_00188AC0: ;
    eax = MEM32(ecx + 0x6C0);""",
        """loc_00188AC0: ;
    recomp_loadsave_state_checkpoint(1u, ecx, MEM32(esp + 4u));
    eax = MEM32(ecx + 0x6C0);""",
    ),
    GeneratedPatch(
        "RsLoadSaveGame state pop checkpoint",
        """loc_00188AF0: ;
    eax = MEM32(ecx + 0x6C0);""",
        """loc_00188AF0: ;
    recomp_loadsave_state_checkpoint(2u, ecx, 0u);
    eax = MEM32(ecx + 0x6C0);""",
    ),
    GeneratedPatch(
        "StateCheckCardValid update checkpoint",
        """loc_0018AEE0: ;
    PUSH32(esp, esi);""",
        """loc_0018AEE0: ;
    recomp_loadsave_state_checkpoint(3u, MEM32(0x4322F0u), ecx);
    PUSH32(esp, esi);""",
    ),
    GeneratedPatch(
        "StateCheckCardValid post-push ESI checkpoint",
        """loc_0018AF13: ;
    ecx = MEM32(esi + 4);""",
        """loc_0018AF13: ;
    recomp_loadsave_state_checkpoint(4u, MEM32(0x4322F0u), esi);
    ecx = MEM32(esi + 4);""",
    ),
    GeneratedPatch(
        "DirectSound inner-object construction checkpoint",
        """    MEM32(edi + 0x24) = eax;
    if (((int32_t)esi < 0)) goto loc_0029F972; /* js: sign (negative) */
""",
        """    MEM32(edi + 0x24) = eax;
    recomp_dsound_object_checkpoint(1u, edi, eax);
    if (((int32_t)esi < 0)) goto loc_0029F972; /* js: sign (negative) */
""",
    ),
    GeneratedPatch(
        "DirectSound inner-object use checkpoint",
        """loc_0029DA72: ;
    (void)0; /* cmp MEM32(ebp + 0x14), 0 - flags set for next jcc */
    eax = MEM32(ebp + 8);
    ecx = MEM32(eax + 0x24);
    PUSH32(esp, edi);
""",
        """loc_0029DA72: ;
    (void)0; /* cmp MEM32(ebp + 0x14), 0 - flags set for next jcc */
    eax = MEM32(ebp + 8);
    ecx = MEM32(eax + 0x24);
    recomp_dsound_object_checkpoint(2u, eax, ecx);
    PUSH32(esp, edi);
""",
    ),
    GeneratedPatch(
        "DirectSound release-interface checkpoint",
        """loc_0029D7E1: ;
    PUSH32(esp, esi);
""",
        """loc_0029D7E1: ;
    recomp_dsound_object_checkpoint(3u, MEM32(esp + 4), 0u);
    PUSH32(esp, esi);
""",
    ),
    GeneratedPatch(
        "CRT sprintf wrapper preserve guest EBX entry",
        """    RECOMP_TRACE_FUNC(0x002370B8u);

loc_002370B8: ;
""",
        """    RECOMP_TRACE_FUNC(0x002370B8u);
    const uint32_t recomp_saved_ebx = ebx;

loc_002370B8: ;
""",
    ),
    GeneratedPatch(
        "CRT sprintf wrapper preserve guest EBX exit",
        """loc_0023710A: ;
    eax = edi;
    POP32(esp, edi);
""",
        """loc_0023710A: ;
    eax = edi;
    ebx = recomp_saved_ebx;
    POP32(esp, edi);
""",
    ),
    GeneratedPatch(
        "Lua symbolic debug metadata validation",
        """loc_001DDE70: ;
    esp = esp - 0x18;
""",
        """loc_001DDE70: ;
    if (!recomp_lua_symbexec_args_valid(MEM32(esp + 4), MEM32(esp + 8))) {
        eax = 0x1B; /* OP_RETURN: neutral result for invalid debug metadata */
        esp += 4; return;
    }
    esp = esp - 0x18;
""",
    ),
    GeneratedPatch(
        "Lua concatenation operand diagnostics",
        """loc_001E09F4: ;
    eax = MEM32(esi + 0x1C);
""",
        """loc_001E09F4: ;
    recomp_lua_concat_trace(esi, MEM32(ebp + 0xC), MEM32(ebp + 0x10),
                            ebx, edi, ebx - 0x10u);
    eax = MEM32(esi + 0x1C);
""",
    ),
    GeneratedPatch(
        "Lua concatenation final failure diagnostics",
        """loc_001E0A56: ;
    PUSH32(esp, ebx);
""",
        """loc_001E0A56: ;
    recomp_lua_concat_trace(esi, 0xFFFFFFFFu, 0xFFFFFFFFu,
                            MEM32(esi + 8), edi, ebx);
    PUSH32(esp, ebx);
""",
    ),
    GeneratedPatch(
        "CRT x87 shared exception continuation",
        """void sub_0023C03C(void) { esp += 4; /* 0x0023C03C: not detected; minimal guest ret */ }
void sub_0023C10E(void) { esp += 4; /* 0x0023C10E: not detected; minimal guest ret */ }
void sub_0023C11B(void) { esp += 4; /* 0x0023C11B: not detected; minimal guest ret */ }
""",
        """void sub_0023C03C(void)
{
    uint32_t ebp = g_seh_ebp;
    MEMD(ebp + -8) = g_fp_stack[g_fp_top & 7u];
    g_fp_top++;
    MEM32(ebp + -28) = ecx;
    eax = MEM32(ebp + 0x10);
    ecx = MEM32(ebp + 0x14);
    MEM32(ebp + -24) = eax;
    MEM32(ebp + -20) = ecx;
    eax = ebp + 8;
    ecx = ebp + -32;
    PUSH32(esp, eax);
    PUSH32(esp, ecx);
    PUSH32(esp, edx);
    PUSH32(esp, 0); sub_0023F2E7();
    esp = esp + 0xC;
    g_fp_stack[--g_fp_top & 7u] = MEMD(ebp + -8);
    if (MEM16(ebp + 8) != 0x027Fu)
        g_x87_control_word = MEM16(ebp + 8);
    esp = ebp;
    POP32(esp, ebp);
    esp += 4;
}
void sub_0023C10E(void)
{
    if (MEM16(esp) != 0x027Fu)
        g_x87_control_word = MEM16(esp);
    POP32(esp, edx);
    esp += 4;
}
void sub_0023C11B(void)
{
    SET_LO16(eax, MEM16(esp));
    if (LO16(eax) != 0x027Fu) {
        SET_LO16(eax, LO16(eax) & 0x20u);
        if (LO16(eax) != 0u) {
            SET_LO16(eax, g_x87_status_word);
            SET_LO16(eax, LO16(eax) & 0x20u);
            if (LO16(eax) != 0u) {
                eax = 8u;
                PUSH32(esp, 0); sub_0023C033();
                POP32(esp, edx);
                esp += 4;
                return;
            }
        }
        g_x87_control_word = MEM16(esp);
    }
    POP32(esp, edx);
    esp += 4;
}""",
    ),
    GeneratedPatch(
        "CRT x87 arccos intrinsic",
        """loc_00238F40: ;
    esp = esp - 0xC;
    MEMD(esp) = fp_top(); /* fst */
    PUSH32(esp, 0); sub_0023C0F8(); /* call 0x0023C0F8 */

loc_00238F4B: ;
    PUSH32(esp, 0); sub_00238F5D(); /* call 0x00238F5D */

loc_00238F50: ;
    esp = esp + 0xC;
    esp += 4; return; /* ret */
""",
        """loc_00238F40: ;
    fp_top() = acos(fp_top()); /* native _CIacos */
    esp += 4; return; /* ret */
""",
    ),
    GeneratedPatch(
        "CRT logarithm fyl2x implementation",
        """loc_00239859: ;
    /* FPU: fyl2x  */
    PUSH32(esp, 0); sub_0023C070(); /* call 0x0023C070 */
""",
        """loc_00239859: ;
    fp_st(1) = fp_st(1) * log2(fp_top()); fp_pop(); /* fyl2x */
    PUSH32(esp, 0); sub_0023C070(); /* call 0x0023C070 */
""",
    ),
    GeneratedPatch(
        "CRT exponent x87 operations",
        """loc_0023C070: ;
    { double _fpu_t = fp_st(0); fp_push(_fpu_t); } /* fld st(0) */
    /* FPU: frndint  */
    fp_st(1) = fp_top() - fp_st(1); /* fsubr */
    { double _t = fp_top(); fp_top() = fp_st(1); fp_st(1) = _t; } /* fxch st(1) */
    fp_top() = -fp_top(); /* fchs */
    /* FPU: f2xm1  */
    fp_push(1.0); /* fld1 */
    fp_st(1) += fp_top(); fp_pop(); /* faddp */
    /* FPU: fscale  */
    fp_st(1) = fp_top(); fp_pop(); /* fstp st(1) */
""",
        """loc_0023C070: ;
    { double _fpu_t = fp_st(0); fp_push(_fpu_t); } /* fld st(0) */
    fp_top() = nearbyint(fp_top()); /* frndint */
    fp_st(1) = fp_top() - fp_st(1); /* fsubr */
    { double _t = fp_top(); fp_top() = fp_st(1); fp_st(1) = _t; } /* fxch st(1) */
    fp_top() = -fp_top(); /* fchs */
    fp_top() = exp2(fp_top()) - 1.0; /* f2xm1 */
    fp_push(1.0); /* fld1 */
    fp_st(1) += fp_top(); fp_pop(); /* faddp */
    fp_top() = ldexp(fp_top(), (int)trunc(fp_st(1))); /* fscale */
    fp_st(1) = fp_top(); fp_pop(); /* fstp st(1) */
""",
    ),
    GeneratedPatch(
        "CRT logarithm shared completion",
        """void sub_0023C159(void) { esp += 4; /* 0x0023C159: not detected; minimal guest ret */ }
""",
        """void sub_0023C159(void)
{
    int exception_pending;
    double value;

    esp -= 8u;
    MEMD(esp) = g_fp_stack[g_fp_top & 7u];
    eax = MEM32(esp + 4u) & 0x7FF00000u;
    esp += 8u;

    if (eax == 0x7FF00000u) {
        value = g_fp_stack[g_fp_top & 7u];
        value = ldexp(value, (int)trunc(MEMD(0x303A00u)));
        g_fp_stack[g_fp_top & 7u] = value;
        exception_pending = fabs(value) >= MEMD(0x3039F0u);
        eax = 4u;
        if (!exception_pending)
            g_fp_stack[g_fp_top & 7u] *= MEMD(0x303A10u);
        goto report_or_return;
    }

    if (MEM16(esp) != 0x027Fu) {
        if ((MEM16(esp) & 0x20u) == 0u &&
            (g_x87_status_word & 0x20u) != 0u) {
            eax = 8u;
            goto report_or_return;
        }
        g_x87_control_word = MEM16(esp);
    }
    POP32(esp, edx);
    esp += 4u;
    return;

report_or_return:
    if (edx == 0x1Du) {
        PUSH32(esp, 0); sub_0023C01C();
    } else {
        PUSH32(esp, 0); sub_0023C033();
    }
    POP32(esp, edx);
    esp += 4u;
}
""",
    ),
    GeneratedPatch(
        "Lua native-call add precall ESP declaration migration",
        """void sub_001DEE30(void)
{
    uint32_t ebp;
    uint32_t recomp_saved_lua_native_ebx;
    int _flags = 0; /* fallback flag var */
""",
        """void sub_001DEE30(void)
{
    uint32_t ebp;
    uint32_t recomp_saved_lua_native_ebx;
    uint32_t recomp_saved_lua_precall_esp;
    int _flags = 0; /* fallback flag var */
""",
    ),
    GeneratedPatch(
        "Lua native-call preserve guest EBX declaration",
        """void sub_001DEE30(void)
{
    uint32_t ebp;
    int _flags = 0; /* fallback flag var */
""",
        """void sub_001DEE30(void)
{
    uint32_t ebp;
    uint32_t recomp_saved_lua_native_ebx;
    uint32_t recomp_saved_lua_precall_esp;
    int _flags = 0; /* fallback flag var */
""",
    ),
    GeneratedPatch(
        "Lua native-call preserve guest EBX boundary",
        """loc_001DEFB2: ;
    eax = MEM32(esi + 0xC);
    ecx = MEM32(eax + -8);
    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, esi);
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(ecx + 0xC), _icall_esp); /* indirect call */
    }

loc_001DEFBC: ;
    esp = esp + 4;
""",
        """loc_001DEFB2: ;
    recomp_lua_callframe_checkpoint(1u, esi, edi, 0u, 0u);
    eax = MEM32(esi + 0xC);
    ecx = MEM32(eax + -8);
    recomp_saved_lua_native_ebx = ebx;
    { uint32_t _icall_esp = g_esp;
    const uint32_t _lua_native_target = MEM32(ecx + 0xC);
    PUSH32(esp, esi);
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(_lua_native_target, _icall_esp); /* indirect call */
    recomp_lua_cclosure_return_checkpoint(
        _lua_native_target, _icall_esp, g_esp, esi);
    }
loc_001DEFBC: ;
    recomp_lua_callframe_checkpoint(2u, esi, edi, eax, 0u);
    ebx = recomp_saved_lua_native_ebx;
    esp = esp + 4;
""",
    ),
    GeneratedPatch(
        "Lua precall target add saved ESP migration",
        """loc_001DEE30: ;
    recomp_lua_callframe_checkpoint(0u, MEM32(esp + 4), MEM32(esp + 8), 0u, 0u);
    PUSH32(esp, ebp);
""",
        """loc_001DEE30: ;
    recomp_saved_lua_precall_esp = g_esp;
    recomp_lua_callframe_checkpoint(0u, MEM32(esp + 4), MEM32(esp + 8), 0u, 0u);
    PUSH32(esp, ebp);
""",
    ),
    GeneratedPatch(
        "Lua precall target frame checkpoint",
        """loc_001DEE30: ;
    PUSH32(esp, ebp);
""",
        """loc_001DEE30: ;
    recomp_saved_lua_precall_esp = g_esp;
    recomp_lua_callframe_checkpoint(0u, MEM32(esp + 4), MEM32(esp + 8), 0u, 0u);
    PUSH32(esp, ebp);
""",
    ),
    GeneratedPatch(
        "Lua precall return stack checkpoint",
        """    POP32(esp, esi);
    eax = eax - edx;
    POP32(esp, ebp);
    esp += 4; return; /* ret */
""",
        """    POP32(esp, esi);
    eax = eax - edx;
    POP32(esp, ebp);
    recomp_lua_precall_return_checkpoint(recomp_saved_lua_precall_esp,
                                         g_esp, MEM32(g_esp + 4u), esi);
    esp += 4; return; /* ret */
""",
    ),
    GeneratedPatch(
        "Lua precall interpreted-function return stack checkpoint",
        """    MEM32(esi + 8) = eax;
    POP32(esp, esi);
    eax = 0; /* xor self */
    POP32(esp, ebp);
    esp += 4; return; /* ret */
""",
        """    MEM32(esi + 8) = eax;
    POP32(esp, esi);
    eax = 0; /* xor self */
    POP32(esp, ebp);
    recomp_lua_precall_return_checkpoint(recomp_saved_lua_precall_esp,
                                         g_esp, MEM32(g_esp + 4u), esi);
    esp += 4; return; /* ret */
""",
    ),
    GeneratedPatch(
        "Lua C callback entry frame checkpoint",
        """loc_001DEFB2: ;
    eax = MEM32(esi + 0xC);
""",
        """loc_001DEFB2: ;
    recomp_lua_callframe_checkpoint(1u, esi, edi, 0u, 0u);
    eax = MEM32(esi + 0xC);
""",
    ),
    GeneratedPatch(
        "Lua C callback return frame checkpoint",
        """loc_001DEFBC: ;
    ebx = recomp_saved_lua_native_ebx;
""",
        """loc_001DEFBC: ;
    recomp_lua_callframe_checkpoint(2u, esi, edi, eax, 0u);
    ebx = recomp_saved_lua_native_ebx;
""",
    ),
    GeneratedPatch(
        "Lua poscall pre-restore frame checkpoint",
        """    ebx = MEM32(esp + 0x14);
    edi = edi + 0xFFFFFFE8u;
""",
        """    ebx = MEM32(esp + 0x14);
    recomp_lua_callframe_checkpoint(3u, esi, 0u, eax, ebx);
    edi = edi + 0xFFFFFFE8u;
""",
    ),
    GeneratedPatch(
        "Lua poscall post-restore frame checkpoint",
        """    MEM32(esi + 0xC) = edx;
    if (TEST_Z(ebx, ebx)) goto loc_001DF09C; /* je: equal / zero */
""",
        """    MEM32(esi + 0xC) = edx;
    recomp_lua_callframe_checkpoint(4u, esi, 0u, eax, ebx);
    if (TEST_Z(ebx, ebx)) goto loc_001DF09C; /* je: equal / zero */
""",
    ),
    GeneratedPatch(
        "Lua VM hash lookup register diagnostics A",
        """    PUSH32(esp, ecx);
    PUSH32(esp, eax);
    PUSH32(esp, 0); sub_001E2FF0(); /* call 0x001E2FF0 */

loc_001E0F9E: ;
    ecx = MEM32(eax);
""",
        """    PUSH32(esp, ecx);
    PUSH32(esp, eax);
    recomp_lua_vm_register_trace(5u, esp, ebx, esi, edi);
    PUSH32(esp, 0); sub_001E2FF0(); /* call 0x001E2FF0 */

loc_001E0F9E: ;
    recomp_lua_vm_register_trace(6u, esp, ebx, esi, edi);
    ecx = MEM32(eax);
""",
    ),
    GeneratedPatch(
        "Lua VM hash lookup register diagnostics B",
        """    PUSH32(esp, eax);
    PUSH32(esp, edx);
    PUSH32(esp, 0); sub_001E2FF0(); /* call 0x001E2FF0 */

loc_001E12D8: ;
    ecx = MEM32(eax);
""",
        """    PUSH32(esp, eax);
    PUSH32(esp, edx);
    recomp_lua_vm_register_trace(7u, esp, ebx, esi, edi);
    PUSH32(esp, 0); sub_001E2FF0(); /* call 0x001E2FF0 */

loc_001E12D8: ;
    recomp_lua_vm_register_trace(8u, esp, ebx, esi, edi);
    ecx = MEM32(eax);
""",
    ),
    GeneratedPatch(
        "Lua VM call register diagnostics A",
        """    PUSH32(esp, esi);
    PUSH32(esp, ebx);
    PUSH32(esp, 0); sub_001DF030(); /* call 0x001DF030 */

loc_001E1995: ;
    esp = esp + 0xC;
""",
        """    PUSH32(esp, esi);
    PUSH32(esp, ebx);
    recomp_lua_vm_register_trace(1u, esp, ebx, esi, edi);
    recomp_lua_poscall_site_checkpoint(3u, ebx, esi, eax);
    PUSH32(esp, 0); sub_001DF030(); /* call 0x001DF030 */

loc_001E1995: ;
    recomp_lua_vm_register_trace(2u, esp, ebx, esi, edi);
    esp = esp + 0xC;
""",
    ),
    GeneratedPatch(
        "Lua VM call register diagnostics B",
        """    PUSH32(esp, esi);
    PUSH32(esp, ebx);
    PUSH32(esp, 0); sub_001DF030(); /* call 0x001DF030 */

loc_001E1DF5: ;
    esp = esp + 0xC;
""",
        """    PUSH32(esp, esi);
    PUSH32(esp, ebx);
    recomp_lua_vm_register_trace(3u, esp, ebx, esi, edi);
    recomp_lua_poscall_site_checkpoint(4u, ebx, esi, edi);
    PUSH32(esp, 0); sub_001DF030(); /* call 0x001DF030 */

loc_001E1DF5: ;
    recomp_lua_vm_register_trace(4u, esp, ebx, esi, edi);
    esp = esp + 0xC;
""",
    ),
    GeneratedPatch(
        "Lua poscall site ring luaD_call",
        """loc_001DF0EE: ;
    ecx = MEM32(esp + 0x10);
    PUSH32(esp, eax);
""",
        """loc_001DF0EE: ;
    ecx = MEM32(esp + 0x10);
    recomp_lua_poscall_site_checkpoint(0u, esi, ecx, eax);
    PUSH32(esp, eax);
""",
    ),
    GeneratedPatch(
        "Lua poscall site ring resume",
        """    PUSH32(esp, ecx);
    esi--;
    PUSH32(esp, esi);
    PUSH32(esp, edi);
    PUSH32(esp, 0); sub_001DF030(); /* call 0x001DF030 */
""",
        """    PUSH32(esp, ecx);
    esi--;
    recomp_lua_poscall_site_checkpoint(1u, edi, esi, ecx);
    PUSH32(esp, esi);
    PUSH32(esp, edi);
    PUSH32(esp, 0); sub_001DF030(); /* call 0x001DF030 */
""",
    ),
    GeneratedPatch(
        "Lua poscall site ring coroutine finalize",
        """loc_001DF1A7: ;
    PUSH32(esp, eax);
    PUSH32(esp, 0xFFFFFFFFu);
    PUSH32(esp, edi);
""",
        """loc_001DF1A7: ;
    recomp_lua_poscall_site_checkpoint(2u, edi, 0xFFFFFFFFu, eax);
    PUSH32(esp, eax);
    PUSH32(esp, 0xFFFFFFFFu);
    PUSH32(esp, edi);
""",
    ),
    GeneratedPatch(
        "Lua VM precall saved-result checkpoint",
        """    eax = eax & 0x1FF;
    eax--;
    PUSH32(esp, edi);
""",
        """    eax = eax & 0x1FF;
    eax--;
    recomp_lua_vm_precall_checkpoint(0u, ebx, edi, eax,
                                     esp, MEM32(esp + 0x28));
    PUSH32(esp, edi);
""",
    ),
    GeneratedPatch(
        "Lua VM precall callback-return checkpoint",
        """loc_001E1975: ;
    esp = esp + 8;
""",
        """loc_001E1975: ;
    recomp_lua_vm_precall_checkpoint(1u, ebx, edi, eax,
                                     esp, MEM32(esp + 0x30));
    esp = esp + 8;
""",
    ),
    GeneratedPatch(
        "Lua VM precall restored-local checkpoint",
        """loc_001E1989: ;
    esi = MEM32(esp + 0x28);
""",
        """loc_001E1989: ;
    recomp_lua_vm_precall_checkpoint(2u, ebx, edi, eax,
                                     esp, MEM32(esp + 0x28));
    esi = MEM32(esp + 0x28);
""",
    ),
    GeneratedPatch(
        "Lua oversized buffer-growth diagnostics",
        """loc_001E37DB: ;
    ecx = MEM32(esp + 0xC);
    PUSH32(esp, edi);
""",
        """loc_001E37DB: ;
    recomp_lua_buffer_grow_checkpoint(esi, edi, eax);
    ecx = MEM32(esp + 0xC);
    PUSH32(esp, edi);
""",
    ),
    GeneratedPatch(
        "Lua settable metamethod-chain diagnostics",
        """loc_001E053B: ;
    eax = MEM32(esp + 0x10);
    eax++;
""",
        """loc_001E053B: ;
    eax = MEM32(esp + 0x10);
    recomp_lua_settable_checkpoint(esi, eax, ebp, edi,
                                    MEM32(esp + 0x20), MEM32(esp + 0x24));
    eax++;
""",
    ),
    GeneratedPatch(
        "SHL Lua dobuffer activation checkpoint",
        """loc_001DC520: ;
    esp = esp - 8;
""",
        """loc_001DC520: ;
    recomp_shl_vm_checkpoint(0u, MEM32(esp + 4), MEM32(esp + 8),
                             MEM32(esp + 0xC));
    esp = esp - 8;
""",
    ),
    GeneratedPatch(
        "SHL Lua dobuffer result checkpoint",
        """loc_001DC56D: ;
    eax = edi;
    POP32(esp, edi);
""",
        """loc_001DC56D: ;
    eax = edi;
    recomp_shl_vm_checkpoint(3u, esi, eax, 0u);
    POP32(esp, edi);
""",
    ),
    GeneratedPatch(
        "SHL Lua SETGLOBAL checkpoint",
        """    PUSH32(esp, edi);
    esi = esi << 4;
    esi = esi + eax;
    PUSH32(esp, esi);
    edx = edx + 0x10;
""",
        """    PUSH32(esp, edi);
    esi = esi << 4;
    esi = esi + eax;
    recomp_shl_vm_checkpoint(2u, ebx, esi, edi);
    PUSH32(esp, esi);
    edx = edx + 0x10;
""",
    ),
    GeneratedPatch(
        "SHL Lua CLOSURE checkpoint",
        """loc_001E1D71: ;
    edx = MEM32(esp + 0x28);
    MEM32(edi) = 6;
    MEM32(edi + 8) = edx;
    goto loc_001E1254;
""",
        """loc_001E1D71: ;
    edx = MEM32(esp + 0x28);
    MEM32(edi) = 6;
    MEM32(edi + 8) = edx;
    recomp_shl_vm_checkpoint(1u, ebx, edi, edx);
    goto loc_001E1254;
""",
    ),
)

PATCHES += (
    GeneratedPatch(
        "temporary 8-byte array constructor checkpoint",
        """    esi = ecx;
    eax = 0; /* xor self */
    MEM32(esp + 8) = esi;
    MEM32(esi) = eax;
    MEM32(esi + 4) = eax;
    MEM32(esi + 8) = 0x80000000u;
    ebx = MEM32(esp + 0x1C);
    ecx = MEM32(0x4409AC);
    edx = MEM32(ecx + 0xC);
    MEM32(esp + 0x14) = eax;
    eax = ebx * 8 + 0xF;
""",
        """    esi = ecx;
    eax = 0; /* xor self */
    MEM32(esp + 8) = esi;
    MEM32(esi) = eax;
    MEM32(esi + 4) = eax;
    MEM32(esi + 8) = 0x80000000u;
    ebx = MEM32(esp + 0x1C);
    ecx = MEM32(0x4409AC);
    edx = MEM32(ecx + 0xC);
    recomp_temp_array_checkpoint(6u, esi, ecx, ebx, edx);
    MEM32(esp + 0x14) = eax;
    eax = ebx * 8 + 0xF;
""",
    ),
    GeneratedPatch(
        "temporary 8-byte array selected storage checkpoint",
        """loc_001BE497: ;
    ecx = ebx;
    MEM32(esi) = eax;
""",
        """loc_001BE497: ;
    recomp_temp_array_checkpoint(7u, esi, MEM32(0x4409AC), eax, ebx);
    ecx = ebx;
    MEM32(esi) = eax;
""",
    ),
    GeneratedPatch(
        "temporary 8-byte array destructor entry checkpoint",
        """    PUSH32(esp, ecx);
    PUSH32(esp, esi);
    esi = ecx;
    PUSH32(esp, edi);
    MEM32(esp + 8) = esi;
    eax = MEM32(esi + 0x10);
    edx = MEM32(esi + 0xC);
    ecx = MEM32(0x4409AC);
    eax = eax * 8 + 0xF;
""",
        """    PUSH32(esp, ecx);
    PUSH32(esp, esi);
    esi = ecx;
    recomp_temp_array_checkpoint(8u, esi, MEM32(0x4409AC), esp, 0u);
    PUSH32(esp, edi);
    MEM32(esp + 8) = esi;
    eax = MEM32(esi + 0x10);
    edx = MEM32(esi + 0xC);
    ecx = MEM32(0x4409AC);
    eax = eax * 8 + 0xF;
""",
    ),
    GeneratedPatch(
        "temporary 8-byte array heap free checkpoint",
        """loc_001BE661: ;
    ecx = MEM32(0x4409AC);
    edx = MEM32(ecx);
""",
        """loc_001BE661: ;
    ecx = MEM32(0x4409AC);
    recomp_temp_array_checkpoint(9u, esi, ecx, MEM32(esi),
                                 MEM32(esi + 8u));
    edx = MEM32(ecx);
""",
    ),
)

PATCHES += (
    GeneratedPatch(
        "scene builder temporary-array stable address declaration",
        """void sub_001BEE30(void)
{
    uint32_t ebp;
    int _flags = 0; /* fallback flag var */
""",
        """void sub_001BEE30(void)
{
    uint32_t ebp;
    uint32_t temp_array_object = 0u;
    int _flags = 0; /* fallback flag var */
""",
    ),
    GeneratedPatch(
        "scene builder temporary-array address capture",
        """    PUSH32(esp, eax);
    ecx = esp + 0x14;
    PUSH32(esp, 0); sub_001BE430(); /* call 0x001BE430 */

loc_001BEE5E: ;
""",
        """    PUSH32(esp, eax);
    ecx = esp + 0x14;
    temp_array_object = ecx;
    PUSH32(esp, 0); sub_001BE430(); /* call 0x001BE430 */

loc_001BEE5E: ;
    if (temp_array_object >= 0x008BF800u)
        recomp_temp_array_checkpoint(6u, temp_array_object, 0x001BE430u,
                                     esp, 0u);
""",
    ),
    GeneratedPatch(
        "scene builder first virtual call stack checkpoint",
        """loc_001BEE8A: ;
    eax = MEM32(esp + 0x14);
""",
        """loc_001BEE8A: ;
    if (temp_array_object >= 0x008BF800u)
        recomp_temp_array_checkpoint(6u, temp_array_object, 0x001BEE8Au,
                                     esp, 1u);
    eax = MEM32(esp + 0x14);
""",
    ),
    GeneratedPatch(
        "scene builder collection callback stack checkpoint",
        """loc_001BEEFA: ;
    esi = MEM32(esi + 0x138);
""",
        """loc_001BEEFA: ;
    if (temp_array_object >= 0x008BF800u)
        recomp_temp_array_checkpoint(6u, temp_array_object, 0x00230500u,
                                     esp, 2u);
    esi = MEM32(esi + 0x138);
""",
    ),
    GeneratedPatch(
        "scene builder list merge stack checkpoint",
        """loc_001BEF35: ;
    eax = MEM32(esi + 8);
""",
        """loc_001BEF35: ;
    if (temp_array_object >= 0x008BF800u)
        recomp_temp_array_checkpoint(6u, temp_array_object, 0x001C8F90u,
                                     esp, 3u);
    eax = MEM32(esi + 8);
""",
    ),
    GeneratedPatch(
        "scene builder list grow stack checkpoint",
        """loc_001BEF54: ;
    esp = esp + 0xC;

loc_001BEF57: ;
""",
        """loc_001BEF54: ;
    if (temp_array_object >= 0x008BF800u)
        recomp_temp_array_checkpoint(6u, temp_array_object, 0x0018D8C0u,
                                     esp, 4u);
    esp = esp + 0xC;

loc_001BEF57: ;
    if (temp_array_object >= 0x008BF800u)
        recomp_temp_array_checkpoint(6u, temp_array_object, 0x001BEF57u,
                                     esp, 5u);
""",
    ),
    GeneratedPatch(
        "scene builder temporary descriptor release stack checkpoint",
        """loc_001BEF70: ;
    goto loc_001BEF82;
""",
        """loc_001BEF70: ;
    if (temp_array_object >= 0x008BF800u)
        recomp_temp_array_checkpoint(6u, temp_array_object, 0x001BE970u,
                                     esp, 6u);
    goto loc_001BEF82;
""",
    ),
    GeneratedPatch(
        "scene builder temporary array pre-destructor stack checkpoint",
        """loc_001BEF82: ;
    ecx = esp + 0x10;
""",
        """loc_001BEF82: ;
    if (temp_array_object >= 0x008BF800u)
        recomp_temp_array_checkpoint(6u, temp_array_object, 0x001BE5F0u,
                                     esp, 7u);
    ecx = esp + 0x10;
""",
    ),
)

PATCHES += (
    GeneratedPatch(
        "temporary 16-byte array constructor checkpoint",
        """loc_001BE690: ;
    PUSH32(esp, 0xFFFFFFFFu);
    PUSH32(esp, 0x2455B8);
    eax = MEM32(0);
    PUSH32(esp, eax);
    MEM32(0) = esp;
    PUSH32(esp, ecx);
    PUSH32(esp, ebx);
    PUSH32(esp, esi);
    esi = ecx;
    eax = 0; /* xor self */
    MEM32(esp + 8) = esi;
    MEM32(esi) = eax;
    MEM32(esi + 4) = eax;
    MEM32(esi + 8) = 0x80000000u;
    ebx = MEM32(esp + 0x1C);
    ecx = MEM32(0x4409AC);
    edx = MEM32(ecx + 0xC);
    MEM32(esp + 0x14) = eax;
""",
        """loc_001BE690: ;
    PUSH32(esp, 0xFFFFFFFFu);
    PUSH32(esp, 0x2455B8);
    eax = MEM32(0);
    PUSH32(esp, eax);
    MEM32(0) = esp;
    PUSH32(esp, ecx);
    PUSH32(esp, ebx);
    PUSH32(esp, esi);
    esi = ecx;
    eax = 0; /* xor self */
    MEM32(esp + 8) = esi;
    MEM32(esi) = eax;
    MEM32(esi + 4) = eax;
    MEM32(esi + 8) = 0x80000000u;
    ebx = MEM32(esp + 0x1C);
    ecx = MEM32(0x4409AC);
    edx = MEM32(ecx + 0xC);
    recomp_temp_array_checkpoint(1u, esi, ecx, ebx, edx);
    MEM32(esp + 0x14) = eax;
""",
    ),
    GeneratedPatch(
        "temporary 16-byte array heap allocation result checkpoint",
        """loc_001BE6E2: ;
    goto loc_001BE6F8;
""",
        """loc_001BE6E2: ;
    recomp_temp_array_checkpoint(2u, esi, MEM32(0x4409AC), eax, ebx);
    goto loc_001BE6F8;
""",
    ),
    GeneratedPatch(
        "temporary 16-byte array selected storage checkpoint",
        """loc_001BE6F8: ;
    ecx = ebx;
    MEM32(esi) = eax;
""",
        """loc_001BE6F8: ;
    recomp_temp_array_checkpoint(3u, esi, MEM32(0x4409AC), eax, ebx);
    ecx = ebx;
    MEM32(esi) = eax;
""",
    ),
    GeneratedPatch(
        "temporary 16-byte array destructor entry checkpoint",
        """loc_001BE720: ;
    PUSH32(esp, 0xFFFFFFFFu);
    PUSH32(esp, 0x2455B8);
    eax = MEM32(0);
    PUSH32(esp, eax);
    MEM32(0) = esp;
    PUSH32(esp, ecx);
    PUSH32(esp, esi);
    esi = ecx;
    PUSH32(esp, edi);
    MEM32(esp + 8) = esi;
""",
        """loc_001BE720: ;
    PUSH32(esp, 0xFFFFFFFFu);
    PUSH32(esp, 0x2455B8);
    eax = MEM32(0);
    PUSH32(esp, eax);
    MEM32(0) = esp;
    PUSH32(esp, ecx);
    PUSH32(esp, esi);
    esi = ecx;
    recomp_temp_array_checkpoint(4u, esi, MEM32(0x4409AC), 0u, 0u);
    PUSH32(esp, edi);
    MEM32(esp + 8) = esi;
""",
    ),
    GeneratedPatch(
        "temporary 16-byte array heap free checkpoint",
        """loc_001BE790: ;
    ecx = MEM32(0x4409AC);
    edx = MEM32(ecx);
""",
        """loc_001BE790: ;
    ecx = MEM32(0x4409AC);
    recomp_temp_array_checkpoint(5u, esi, ecx, MEM32(esi), MEM32(esi + 8u));
    edx = MEM32(ecx);
""",
    ),
    GeneratedPatch(
        "temporary array merge post-construction stack checkpoint",
        """loc_001BFA18: ;
    eax = MEM32(esp + 0x2C);
""",
        """loc_001BFA18: ;
    recomp_temp_array_checkpoint(10u, esp + 0x24u, 0u, esp, esi);
    eax = MEM32(esp + 0x2C);
""",
    ),
    GeneratedPatch(
        "temporary array merge first grow pre-call stack checkpoint",
        """loc_001BFA36: ;
    PUSH32(esp, 0x10);
""",
        """loc_001BFA36: ;
    recomp_temp_array_checkpoint(20u, esp + 0x24u, 0x0018D8C0u, esp, esp);
    PUSH32(esp, 0x10);
""",
    ),
    GeneratedPatch(
        "temporary array merge first grow post-call stack checkpoint",
        """loc_001BFA43: ;
    esp = esp + 0xC;
""",
        """loc_001BFA43: ;
    recomp_temp_array_checkpoint(21u, esp + 0x30u, 0x0018D8C0u,
                                 esp + 0xCu, esp);
    esp = esp + 0xC;
""",
    ),
    GeneratedPatch(
        "temporary array merge second grow pre-call stack checkpoint",
        """loc_001BFAA6: ;
    PUSH32(esp, 0x10);
""",
        """loc_001BFAA6: ;
    recomp_temp_array_checkpoint(22u, esp + 0x24u, 0x0018D8C0u, esp, esp);
    PUSH32(esp, 0x10);
""",
    ),
    GeneratedPatch(
        "temporary array merge second grow post-call stack checkpoint",
        """loc_001BFAAF: ;
    edx = MEM32(esp + 0x34);
""",
        """loc_001BFAAF: ;
    recomp_temp_array_checkpoint(23u, esp + 0x30u, 0x0018D8C0u,
                                 esp + 0xCu, esp);
    edx = MEM32(esp + 0x34);
""",
    ),
    GeneratedPatch(
        "temporary array merge first release pre-call stack checkpoint",
        """    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(edx + 0x14), _icall_esp); /* indirect call */
    }

loc_001BFB89: ;
""",
        """    { uint32_t _icall_esp = g_esp;
    recomp_temp_array_checkpoint(11u, esp + 0x24u, MEM32(edx + 0x14),
                                 _icall_esp, esp);
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(edx + 0x14), _icall_esp); /* indirect call */
    }

loc_001BFB89: ;
    recomp_temp_array_checkpoint(12u, esp + 0x24u, 0u, 0u, esp);
""",
    ),
    GeneratedPatch(
        "temporary array merge second release pre-call stack checkpoint",
        """    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(eax + 0x10), _icall_esp); /* indirect call */
    }

loc_001BFB90: ;
""",
        """    { uint32_t _icall_esp = g_esp;
    recomp_temp_array_checkpoint(13u, esp + 0x24u, MEM32(eax + 0x10),
                                 _icall_esp, esp);
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(eax + 0x10), _icall_esp); /* indirect call */
    }

loc_001BFB90: ;
    recomp_temp_array_checkpoint(14u, esp + 0x24u, 0u, 0u, esp);
""",
    ),
    GeneratedPatch(
        "temporary array merge third grow pre-call stack checkpoint",
        """loc_001BFC16: ;
    PUSH32(esp, 0x10);
""",
        """loc_001BFC16: ;
    recomp_temp_array_checkpoint(24u, esp + 0x24u, 0x0018D8C0u, esp, esp);
    PUSH32(esp, 0x10);
""",
    ),
    GeneratedPatch(
        "temporary array merge third grow post-call stack checkpoint",
        """loc_001BFC1F: ;
    edx = MEM32(esp + 0x34);
""",
        """loc_001BFC1F: ;
    recomp_temp_array_checkpoint(25u, esp + 0x30u, 0x0018D8C0u,
                                 esp + 0xCu, esp);
    edx = MEM32(esp + 0x34);
""",
    ),
    GeneratedPatch(
        "temporary array merge pre-destruction stack checkpoint",
        """loc_001BFCB7: ;
    ecx = esp + 0x24;
""",
        """loc_001BFCB7: ;
    recomp_temp_array_checkpoint(30u, esp + 0x24u, 0u, 0u, esp);
    ecx = esp + 0x24;
""",
    ),
    GeneratedPatch(
        "scene-list temporary array post-construction checkpoint",
        """loc_001C8FEA: ;
    eax = MEM32(esi + 0x68);
""",
        """loc_001C8FEA: ;
    recomp_temp_array_checkpoint(35u, esp + 0x10u, 0u, 0u, esp);
    eax = MEM32(esi + 0x68);
""",
    ),
    GeneratedPatch(
        "scene-list merge pre-call stack checkpoint",
        """loc_001C9042: ;
    eax = MEM32(eax + 0x138);
""",
        """loc_001C9042: ;
    recomp_temp_array_checkpoint(40u, esp + 0x10u, 0x00230500u, esp, esp);
    eax = MEM32(eax + 0x138);
""",
    ),
    GeneratedPatch(
        "scene-list merge post-call stack checkpoint",
        """loc_001C9058: ;
    edx = MEM32(esi + 0x20);
""",
        """loc_001C9058: ;
    recomp_temp_array_checkpoint(41u, esp + 0x10u, 0u, 0u, esp);
    edx = MEM32(esi + 0x20);
""",
    ),
    GeneratedPatch(
        "scene-list temporary array pre-destruction checkpoint",
        """loc_001C9151: ;
    ecx = esp + 0x10;
""",
        """loc_001C9151: ;
    recomp_temp_array_checkpoint(36u, esp + 0x10u, 0u, 0u, esp);
    ecx = esp + 0x10;
""",
    ),
    GeneratedPatch(
        "scene-list merge diagnostic local declarations",
        """void sub_00230500(void)
{
    uint32_t ebp;
    int _flags = 0; /* fallback flag var */
""",
        """void sub_00230500(void)
{
    uint32_t ebp;
    uint32_t merge_left_base;
    uint32_t merge_right_base;
    uint32_t merge_left_items;
    uint32_t merge_right_items;
    int _flags = 0; /* fallback flag var */
""",
    ),
    GeneratedPatch(
        "scene-list merge entry argument checkpoint",
        """loc_00230500: ;
    PUSH32(esp, ecx);
""",
        """loc_00230500: ;
    merge_left_base = MEM32(esp + 4);
    merge_right_base = MEM32(esp + 0xC);
    merge_left_items = MEM32(esp + 8);
    merge_right_items = MEM32(esp + 0x10);
    PUSH32(esp, ecx);
""",
    ),
    GeneratedPatch(
        "scene-list merge compare input checkpoint",
        """loc_0023054D: ;
    ebx = MEM32(edi + 4);
""",
        """loc_0023054D: ;
    recomp_temp_array_checkpoint(59u, merge_left_base, merge_right_base,
                                 merge_left_items, merge_right_items);
    recomp_temp_array_checkpoint(56u, esi, edi, MEM32(esi + 4),
                                 MEM32(edi + 4));
    ebx = MEM32(edi + 4);
""",
    ),
    GeneratedPatch(
        "scene-list merge callback one stack checkpoint",
        """    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, edi);
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(edx + 8), _icall_esp); /* indirect call */
    }

loc_0023059B: ;
""",
        """    { uint32_t _icall_esp = g_esp;
    recomp_temp_array_checkpoint(42u, 0u, MEM32(edx + 8),
                                 _icall_esp, esp);
    PUSH32(esp, edi);
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(edx + 8), _icall_esp); /* indirect call */
    }

loc_0023059B: ;
    recomp_temp_array_checkpoint(43u, 0u, 0u, 0u, esp);
""",
    ),
    GeneratedPatch(
        "scene-list merge predicate one stack checkpoint",
        """    edx = MEM32(ebp);
    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, eax);
    PUSH32(esp, ecx);
    eax = esp + 0x20;
    PUSH32(esp, eax);
    ecx = ebp;
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(edx), _icall_esp); /* indirect call */
    }

loc_002305C9: ;
""",
        """    edx = MEM32(ebp);
    { uint32_t _icall_esp = g_esp;
    recomp_temp_array_checkpoint(44u, 0u, MEM32(edx),
                                 _icall_esp, esp);
    PUSH32(esp, eax);
    PUSH32(esp, ecx);
    eax = esp + 0x20;
    PUSH32(esp, eax);
    ecx = ebp;
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(edx), _icall_esp); /* indirect call */
    }

loc_002305C9: ;
    recomp_temp_array_checkpoint(45u, 0u, 0u, 0u, esp);
""",
    ),
    GeneratedPatch(
        "scene-list merge callback two stack checkpoint",
        """    eax = MEM32(ecx);
    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, esi);
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(eax + 4), _icall_esp); /* indirect call */
    }

loc_002305E5: ;
""",
        """    eax = MEM32(ecx);
    { uint32_t _icall_esp = g_esp;
    recomp_temp_array_checkpoint(46u, 0u, MEM32(eax + 4),
                                 _icall_esp, esp);
    PUSH32(esp, esi);
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(eax + 4), _icall_esp); /* indirect call */
    }

loc_002305E5: ;
    recomp_temp_array_checkpoint(47u, 0u, 0u, 0u, esp);
""",
    ),
    GeneratedPatch(
        "scene-list merge predicate two stack checkpoint",
        """    edx = MEM32(ebp);
    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, eax);
    PUSH32(esp, ecx);
    eax = esp + 0x30;
    PUSH32(esp, eax);
    ecx = ebp;
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(edx), _icall_esp); /* indirect call */
    }

loc_00230621: ;
""",
        """    edx = MEM32(ebp);
    { uint32_t _icall_esp = g_esp;
    recomp_temp_array_checkpoint(48u, 0u, MEM32(edx),
                                 _icall_esp, esp);
    PUSH32(esp, eax);
    PUSH32(esp, ecx);
    eax = esp + 0x30;
    PUSH32(esp, eax);
    ecx = ebp;
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(edx), _icall_esp); /* indirect call */
    }

loc_00230621: ;
    recomp_temp_array_checkpoint(49u, 0u, 0u, 0u, esp);
""",
    ),
    GeneratedPatch(
        "scene-list merge callback three stack checkpoint",
        """    eax = MEM32(ecx);
    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, esi);
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(eax + 4), _icall_esp); /* indirect call */
    }

loc_0023063D: ;
""",
        """    eax = MEM32(ecx);
    { uint32_t _icall_esp = g_esp;
    recomp_temp_array_checkpoint(50u, 0u, MEM32(eax + 4),
                                 _icall_esp, esp);
    PUSH32(esp, esi);
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(eax + 4), _icall_esp); /* indirect call */
    }

loc_0023063D: ;
    recomp_temp_array_checkpoint(51u, 0u, 0u, 0u, esp);
""",
    ),
    GeneratedPatch(
        "scene-list merge callback four stack checkpoint",
        """    eax = MEM32(ecx);
    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, edi);
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(eax + 8), _icall_esp); /* indirect call */
    }

loc_00230667: ;
""",
        """    eax = MEM32(ecx);
    { uint32_t _icall_esp = g_esp;
    recomp_temp_array_checkpoint(52u, 0u, MEM32(eax + 8),
                                 _icall_esp, esp);
    PUSH32(esp, edi);
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(eax + 8), _icall_esp); /* indirect call */
    }

loc_00230667: ;
    recomp_temp_array_checkpoint(53u, 0u, 0u, 0u, esp);
""",
    ),
    GeneratedPatch(
        "scene-list callback dispatch caller-cleanup checkpoint",
        """    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, esi);
    PUSH32(esp, edx);
    PUSH32(esp, ebx);
    PUSH32(esp, ebp);
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(eax, _icall_esp); /* indirect call */
    }

loc_001C8B75: ;
    ecx = MEM32(esp + 0x24);
    esp = esp + 0x10;
""",
        """    { uint32_t _icall_esp = g_esp;
    recomp_temp_array_checkpoint(54u, 0u, eax, _icall_esp, esp);
    PUSH32(esp, esi);
    PUSH32(esp, edx);
    PUSH32(esp, ebx);
    PUSH32(esp, ebp);
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(eax, _icall_esp); /* indirect call */
    }

loc_001C8B75: ;
    recomp_temp_array_checkpoint(55u, 0u, 0u, 0u, esp);
    ecx = MEM32(esp + 0x24);
    esp = esp + 0x10;
    recomp_temp_array_checkpoint(38u, 0u, 0u, 0u, esp);
""",
    ),
    GeneratedPatch(
        "game StatePlay prewarm camera checkpoint",
        """loc_0018173E: ;
    eax = eax + 0x30;
    edx = MEM32(eax);
    MEM32(esp + 0x1C) = edx;
    ecx = MEM32(eax + 4);
    MEM32(esp + 0x20) = ecx;
    edx = MEM32(eax + 8);
    eax = esp + 0x10;
""",
        """loc_0018173E: ;
    eax = eax + 0x30;
    edx = MEM32(eax);
    MEM32(esp + 0x1C) = edx;
    ecx = MEM32(eax + 4);
    MEM32(esp + 0x20) = ecx;
    edx = MEM32(eax + 8);
    recomp_shell_prewarm_checkpoint(esi, esp + 0x10u,
                                    MEM32(esp + 0x1Cu),
                                    MEM32(esp + 0x20u), edx);
    eax = esp + 0x10;
""",
    ),
    GeneratedPatch(
        "developer chapter import empty PPD head guard",
        """loc_001EB155: ;
    eax = MEM32(eax);
    (void)0; /* test eax, eax - flags set for next jcc */
""",
        """loc_001EB155: ;
    eax = MEM32(eax);
    /* A discarded zero-property import slot can still be the typed-table
     * head. _Reset has cleared it; linking that slot to itself never ends.
     * Keep this extension scoped to the developer's extra chapter import. */
    if (eax == esi && recomp_dev_region_import_active()) eax = 0;
    (void)0; /* test eax, eax - flags set for next jcc */
""",
    ),
    GeneratedPatch(
        "RsMain asset dispatch checkpoint",
        """loc_0017A302: ;
    edx = esp + 0x48;
    PUSH32(esp, edx);
""",
        """loc_0017A302: ;
    recomp_asset_dispatch_checkpoint(ebx, esi, esp + 0x48u);
    edx = esp + 0x48;
    PUSH32(esp, edx);
""",
    ),
    GeneratedPatch(
        "RedWorld permanent-property XFRM decode checkpoint",
        """    MEMF(esi + 0x28) = xmm1; /* movss */
    MEMF(esi + 0x3C) = xmm0; /* movss */
    goto loc_001EDB26;
""",
        """    MEMF(esi + 0x28) = xmm1; /* movss */
    MEMF(esi + 0x3C) = xmm0; /* movss */
    recomp_world_xfrm_checkpoint(esi, MEM32(esp + 0x7Cu));
    goto loc_001EDB26;
""",
    ),
    GeneratedPatch(
        "RedWorld composed permanent-property transform checkpoint",
        """loc_001EED67: ;
    ecx = esp + 0x180;
""",
        """loc_001EED67: ;
    recomp_world_composed_checkpoint(1u, esp + 0x140u, esp + 0x180u);
    ecx = esp + 0x180;
""",
    ),
    GeneratedPatch(
        "RedWorld composed spore transform checkpoint",
        """loc_001EEE30: ;
    eax = MEM32(esi);
""",
        """loc_001EEE30: ;
    recomp_world_composed_checkpoint(2u, esp + 0xC0u, esp + 0x100u);
    eax = MEM32(esi);
""",
    ),

    GeneratedPatch(
        "shell StatePlay prewarm camera checkpoint (retail shell)",
        """loc_0018762E: ;
    eax = eax + 0x30;
    edx = MEM32(eax);
    MEM32(esp + 0x1C) = edx;
    ecx = MEM32(eax + 4);
    MEM32(esp + 0x20) = ecx;
    edx = MEM32(eax + 8);
    eax = esp + 0x10;
""",
        """loc_0018762E: ;
    eax = eax + 0x30;
    edx = MEM32(eax);
    MEM32(esp + 0x1C) = edx;
    ecx = MEM32(eax + 4);
    MEM32(esp + 0x20) = ecx;
    edx = MEM32(eax + 8);
    recomp_shell_prewarm_checkpoint(esi, esp + 0x10u,
                                    MEM32(esp + 0x1Cu),
                                    MEM32(esp + 0x20u), edx);
    eax = esp + 0x10;
""",
    ),
    GeneratedPatch(
        "Havok heightfield temporary allocation size preservation",
        """loc_001A6249: ;
    ecx = MEM32(0x4409AC);
    eax = MEM32(ecx + 0xC);
    ebx = ebx + 0xF;
    ebx = ebx & 0xFFFFFFF0u;
    if (CMP_LE(ebx, eax)) goto loc_001A626A; /* jle: less or equal (signed <=) */
""",
        """loc_001A6249: ;
    ecx = MEM32(0x4409AC);
    eax = MEM32(ecx + 0xC);
    ebx = ebx + 0xF;
    ebx = ebx & 0xFFFFFFF0u;
    /* hkHeightFieldAgent allocates two equally-sized temporary arrays.  The
       original x86 keeps this aligned size in callee-saved EBX across the
       virtual shape/collector calls below.  Preserve it explicitly because
       recompiled indirect callees share the global guest register file. */
    MEM32(esp + 0x2Cu) = ebx;
    if (CMP_LE(ebx, eax)) goto loc_001A626A; /* jle: less or equal (signed <=) */
""",
    ),
    GeneratedPatch(
        "Havok heightfield temporary allocation size restore",
        """loc_001A63EA: ;
    (void)0; /* test ebx, ebx - flags set for next jcc */
    ecx = MEM32(0x4409AC);
""",
        """loc_001A63EA: ;
    /* Restore the allocation size before releasing both LIFO blocks. */
    ebx = MEM32(esp + 0x2Cu);
    (void)0; /* test ebx, ebx - flags set for next jcc */
    ecx = MEM32(0x4409AC);
""",
    ),
    GeneratedPatch(
        "RedTerrain EnableRendering request checkpoint",
        """loc_0011CC73: ;
    esp = esp + 0xC;
    (void)0; /* test LO8(eax), LO8(eax) - flags set for next jcc */
    POP32(esp, esi);
    ecx = 0x643370;
    if (TEST_Z(LO8(eax), LO8(eax))) goto loc_0011CC8A; /* je: equal / zero */
""",
        """loc_0011CC73: ;
    esp = esp + 0xC;
    (void)0; /* test LO8(eax), LO8(eax) - flags set for next jcc */
    POP32(esp, esi);
    ecx = 0x643370;
    recomp_terrain_rendering_request(LO8(eax) != 0u, MEM32(0x643384u));
    if (TEST_Z(LO8(eax), LO8(eax))) goto loc_0011CC8A; /* je: equal / zero */
""",
    ),
    GeneratedPatch(
        "RedScene renderable activation checkpoint",
        """loc_00203447: ;
    eax = eax | 1;
    MEM32(ecx + 0x14) = eax;
    eax = ecx + 4;
""",
        """loc_00203447: ;
    eax = eax | 1;
    MEM32(ecx + 0x14) = eax;
    recomp_redscene_checkpoint(1u, 0x692218u, ecx, 0u, 0u);
    eax = ecx + 4;
""",
    ),
    GeneratedPatch(
        "RedScene renderable deactivation checkpoint",
        """loc_00203469: ;
    eax = esi + 4;
    PUSH32(esp, eax);
""",
        """loc_00203469: ;
    recomp_redscene_checkpoint(3u, 0x692218u, esi, 0u, 0u);
    eax = esi + 4;
    PUSH32(esp, eax);
""",
    ),
    GeneratedPatch(
        "RedScene system start-begin checkpoint",
        """loc_002033E0: ;
    PUSH32(esp, 0x42800000);
""",
        """loc_002033E0: ;
    recomp_redscene_checkpoint(4u, 0x692218u, 0u, 0u, 0u);
    PUSH32(esp, 0x42800000);
""",
    ),
    GeneratedPatch(
        "RedScene system start-end checkpoint",
        """loc_00203419: ;
    esp += 4; return; /* ret */
""",
        """loc_00203419: ;
    recomp_redscene_checkpoint(5u, 0x692218u, 0u, 0u, 0u);
    esp += 4; return; /* ret */
""",
    ),
    GeneratedPatch(
        "RedScene system stop-begin checkpoint",
        """loc_00203420: ;
    ecx = 0x692218;
""",
        """loc_00203420: ;
    recomp_redscene_checkpoint(6u, 0x692218u, 0u, 0u, 0u);
    ecx = 0x692218;
""",
    ),
    GeneratedPatch(
        "RedScene spatial collection result checkpoint",
        """loc_00203551: ;
    esi = 0; /* xor self */
    (void)0; /* test eax, eax - flags set for next jcc */
""",
        """loc_00203551: ;
    recomp_redscene_checkpoint(2u, esi, esp + 8u, edi, eax);
    esi = 0; /* xor self */
    (void)0; /* test eax, eax - flags set for next jcc */
""",
    ),
    GeneratedPatch(
        "RedSpace paired-sort input checkpoint",
        """loc_00225029: ;
    eax = MEM32(0x85C5A0);
""",
        """loc_00225029: ;
    recomp_redscene_checkpoint(7u, 0x692218u, MEM32(0x85C59C),
                               MEM32(esp + 0x14), edi);
    eax = MEM32(0x85C5A0);
""",
    ),
    GeneratedPatch(
        "RedSpace collected item write checkpoint",
        """loc_00224277: ;
    edx = MEM32(ebx + 4);
    edx = MEM32(edx + edi * 4);
    esi = MEM32(0x85C5A4);
    MEM32(esi + eax * 4) = edx;
""",
        """loc_00224277: ;
    edx = MEM32(ebx + 4);
    edx = MEM32(edx + edi * 4);
    esi = MEM32(0x85C5A4);
    edx = recomp_redscene_collected_item_checkpoint(ebx, edi, edx, eax);
    recomp_redscene_checkpoint(8u, ebx, edi, edx, eax);
    MEM32(esi + eax * 4) = edx;
""",
    ),
    GeneratedPatch(
        "front-end BrushMenu paint checkpoint",
        """loc_000E6808: ;
    PUSH32(esp, 0x2EB484);
""",
        """loc_000E6808: ;
    recomp_menu_paint_checkpoint(1u, esi, 0u, 0u, 0u, ebx);
    PUSH32(esp, 0x2EB484);
""",
    ),
    GeneratedPatch(
        "front-end BrushMenu label lookup checkpoint",
        """loc_000E6A77: ;
    ecx = MEM32(0x30E558);
""",
        """loc_000E6A77: ;
    recomp_menu_paint_checkpoint(2u, esi, ebp,
                                 MEM32(esi + 0x40u + ebp * 4u), ebx, edi);
    ecx = MEM32(0x30E558);
""",
    ),
    GeneratedPatch(
        "front-end BrushMenu caption lookup checkpoint",
        """loc_000E690E: ;
    PUSH32(esp, edi);
""",
        """loc_000E690E: ;
    if (ebx != 0u)
        recomp_options_replace_label(MEM32(ebx + 0x54u), edi);
    PUSH32(esp, edi);
""",
    ),
    GeneratedPatch(
        "front-end BrushMenu custom caption localization style",
        """loc_000E68FC: ;
    if (TEST_Z(eax, eax)) goto loc_000E6918; /* je: equal / zero */
""",
        """loc_000E68FC: ;
    eax = recomp_options_localization_hash(eax);
    if (TEST_Z(eax, eax)) goto loc_000E6918; /* je: equal / zero */
""",
    ),
    GeneratedPatch(
        "front-end BrushMenu custom label localization style",
        """loc_000E6A5E: ;
    edx = MEM32(esp + 0x3C);
    eax = MEM32(edx);
    ecx = MEM32(esp + 0x48);
""",
        """loc_000E6A5E: ;
    edx = MEM32(esp + 0x3C);
    eax = MEM32(edx);
    eax = recomp_options_localization_hash(eax);
    ecx = MEM32(esp + 0x48);
""",
    ),
    GeneratedPatch(
        "front-end BrushMenu selected custom label localization style",
        """loc_000E6C4A: ;
    ecx = MEM32(esi + 0x38);
    edx = MEM32(esi + ecx * 4 + 0x40);
    ecx = MEM32(esp + 0x20);
""",
        """loc_000E6C4A: ;
    ecx = MEM32(esi + 0x38);
    edx = MEM32(esi + ecx * 4 + 0x40);
    edx = recomp_options_localization_hash(edx);
    ecx = MEM32(esp + 0x20);
""",
    ),
    GeneratedPatch(
        "front-end BrushMenu selected custom label replacement",
        """loc_000E6C61: ;
    PUSH32(esp, edi);
""",
        """loc_000E6C61: ;
    recomp_options_replace_label(
        MEM32(esi + 0x40u + MEM32(esi + 0x38u) * 4u), edi);
    PUSH32(esp, edi);
""",
    ),
    GeneratedPatch(
        "RedCamera SetDrawDistance recomp option",
        """loc_001F89F0: ;
    recomp_xmm_loadss(xmm0v, esp + 4); /* movss */
    recomp_xmm_loadss(xmm1v, 0x2DC08C); /* movss */
""",
        """loc_001F89F0: ;
    recomp_xmm_loadss(xmm0v, esp + 4); /* movss */
    xmm0 = recomp_options_scale_draw_distance(xmm0);
    recomp_xmm_loadss(xmm1v, 0x2DC08C); /* movss */
""",
    ),
    GeneratedPatch(
        "RsActor camera visibility preserves retail LOD under Hor+ aspect",
        """loc_00015243: ;
    ecx = MEM32(esp + 0x2C);
    MEMF(esi + 0x154) = xmm0; /* movss */
""",
        """loc_00015243: ;
    ecx = MEM32(esp + 0x2C);
    /* The title authored LOD/AI thresholds for its 4:3 horizontal field.
     * Hor+ expands only the horizontal view, so retain the equivalent
     * projected-size metric independently of internal resolution. */
    xmm0 = recomp_options_scale_camera_visibility(xmm0);
    MEMF(esi + 0x154) = xmm0; /* movss */
""",
    ),
    GeneratedPatch(
        "human AI LOD applies the user-selected front-camera wake distance",
        """loc_00050640: ;
    recomp_xmm_loadss(xmm1v, esi + 0x154); /* movss */
""",
        """loc_00050640: ;
    /* Recomp Options expresses wake range as a distance multiplier. The
     * retail projected-size hint remains the exact ORIGINAL setting. */
    xmm0 = recomp_options_scale_ai_visibility_threshold(xmm0);
    recomp_xmm_loadss(xmm1v, esi + 0x154); /* movss */
""",
    ),
    GeneratedPatch(
        "human AI LOD applies the user-selected behind-camera wake distance",
        """loc_0005071E: ;
    fp_push(MEMF(0x2DC344)); /* fld float */
""",
        """loc_0005071E: ;
    fp_push(recomp_options_scale_ai_behind_distance_squared(
        MEMF(0x2DC344))); /* retail squared distance, scaled by N^2 */
""",
    ),
    GeneratedPatch(
        "RedModel human far cull applies NPC draw distance option",
        """    recomp_xmm_loadss(xmm1v, ebp + 0x1C); /* movss */
    xmm0 = xmm0 + xmm3; /* addss */
""",
        """    recomp_xmm_loadss(xmm1v, ebp + 0x1C); /* movss */
    xmm1 = recomp_options_scale_npc_draw_distance(
        xmm1, (int)MEM32(edi + 0x2Cu));
    xmm0 = xmm0 + xmm3; /* addss */
""",
    ),
    GeneratedPatch(
        "ambient civilian spawn center applies NPC draw distance option",
        """loc_0016A8C3: ;
    recomp_xmm_loadss(xmm0v, esi + 0x22CAC); /* movss */
    goto loc_0016A8D5;

loc_0016A8CD: ;
    recomp_xmm_loadss(xmm0v, esi + 0x22CA8); /* movss */
""",
        """loc_0016A8C3: ;
    recomp_xmm_loadss(xmm0v, esi + 0x22CAC); /* movss */
    xmm0 = recomp_options_scale_ambient_civ_distance(xmm0);
    goto loc_0016A8D5;

loc_0016A8CD: ;
    recomp_xmm_loadss(xmm0v, esi + 0x22CA8); /* movss */
    xmm0 = recomp_options_scale_ambient_civ_distance(xmm0);
""",
    ),
    GeneratedPatch(
        "ambient civilian spawn radius applies NPC draw distance option",
        """loc_0016A95B: ;
    recomp_xmm_loadss(xmm0v, esi + 0x22CAC); /* movss */
    xmm0 = xmm0 + MEMF(esi + 0x22CB4); /* addss */
    xmm0 = xmm0 - MEMF(0x2DC34C); /* subss */
    MEMF(0x30EB54) = xmm0; /* movss */
    recomp_xmm_loadss(xmm0v, esi + 0x22CB4); /* movss */
    goto loc_0016A9AD;

loc_0016A985: ;
    recomp_xmm_loadss(xmm0v, esi + 0x22CA8); /* movss */
    xmm0 = xmm0 + MEMF(esi + 0x22CB0); /* addss */
    xmm0 = xmm0 - MEMF(0x2DC34C); /* subss */
    MEMF(0x30EB54) = xmm0; /* movss */
    recomp_xmm_loadss(xmm0v, esi + 0x22CB0); /* movss */
""",
        """loc_0016A95B: ;
    recomp_xmm_loadss(xmm0v, esi + 0x22CAC); /* movss */
    xmm0 = recomp_options_scale_ambient_civ_distance(xmm0);
    xmm0 = xmm0 + recomp_options_scale_ambient_civ_distance(
        MEMF(esi + 0x22CB4)); /* addss */
    xmm0 = xmm0 - MEMF(0x2DC34C); /* subss */
    MEMF(0x30EB54) = xmm0; /* movss */
    recomp_xmm_loadss(xmm0v, esi + 0x22CB4); /* movss */
    xmm0 = recomp_options_scale_ambient_civ_distance(xmm0);
    goto loc_0016A9AD;

loc_0016A985: ;
    recomp_xmm_loadss(xmm0v, esi + 0x22CA8); /* movss */
    xmm0 = recomp_options_scale_ambient_civ_distance(xmm0);
    xmm0 = xmm0 + recomp_options_scale_ambient_civ_distance(
        MEMF(esi + 0x22CB0)); /* addss */
    xmm0 = xmm0 - MEMF(0x2DC34C); /* subss */
    MEMF(0x30EB54) = xmm0; /* movss */
    recomp_xmm_loadss(xmm0v, esi + 0x22CB0); /* movss */
    xmm0 = recomp_options_scale_ambient_civ_distance(xmm0);
""",
    ),
    GeneratedPatch(
        "RedModel human LOD can remain high while preserving far cull",
        'loc_00220CA2: ;\n    eax = esp + 0x80;',
        'loc_00220CA2: ;\n    /* Preserve retail spawn fade/slide and actor opacity. Only substitute\n     * the visible low-detail human mesh after those decisions are made. */\n    if (recomp_options_force_high_npc_lod((int)MEM32(edi + 0x2Cu))) {\n        MEM32(esp + 0x24u) = recomp_options_high_npc_lod_mask(MEM32(esp + 0x24u));\n        ebx = recomp_options_high_npc_lod_mask(MEM32(esp + 0x10u));\n        MEM32(esp + 0x10u) = ebx & ~MEM32(esp + 0x24u);\n    }\n    eax = esp + 0x80;',
    ),
    GeneratedPatch(
        "RedCamera SetPerspective recomp aspect option",
        """loc_001F8F10: ;
    esp = esp - 0x10;
    fp_push(MEMF(esp + 0x1C)); /* fld float */
""",
        """loc_001F8F10: ;
    esp = esp - 0x10;
    { float recomp_options_camera_far_plane(uint32_t,float);
      MEMF(esp + 0x18u) = recomp_options_camera_far_plane(ecx,MEMF(esp + 0x18u)); }
    {
        const float _recomp_guest_aspect = MEMF(esp + 0x20);
        MEMF(esp + 0x1C) = recomp_options_perspective_fov(
            MEMF(esp + 0x1C), _recomp_guest_aspect);
        MEMF(esp + 0x20) = recomp_options_perspective_aspect(
            _recomp_guest_aspect);
    }
    fp_push(MEMF(esp + 0x1C)); /* fld float */
""",
    ),
    GeneratedPatch(
        "FilterShader Render authentic glow option",
        """loc_0015B0EE: ;
    PUSH32(esp, 0); sub_0015AB40(); /* call 0x0015AB40 */
""",
        """loc_0015B0EE: ;
    /* This is the retail FilterShader::RenderGlow call.  Keep the separate
     * monochrome/video-filter path below intact when authentic glow is off. */
    if (!recomp_options_authentic_haze()) goto loc_0015B0F3;
    PUSH32(esp, 0); sub_0015AB40(); /* call 0x0015AB40 */
""",
    ),
    GeneratedPatch(
        "RsFrontEnd movie update branch checkpoint",
        """loc_000D7DB9: ;
    eax = MEM32(esi + 0x3EC4);
    if (TEST_NZ(eax, eax)) goto loc_000D7DDB; /* jne: not equal / not zero */
""",
        """loc_000D7DB9: ;
    eax = MEM32(esi + 0x3EC4);
    recomp_movie_checkpoint(5u, esi, MEM32(esi + 0x3EB8u),
                            eax, MEM32(esi));
    if (TEST_NZ(eax, eax)) goto loc_000D7DDB; /* jne: not equal / not zero */
""",
    ),
    GeneratedPatch(
        "RsFrontEnd background movie selection checkpoint",
        """loc_000D7C5F: ;
    edi = eax;
    if (TEST_NZ(edi, edi)) goto loc_000D7C67; /* jne: not equal / not zero */
""",
        """loc_000D7C5F: ;
    edi = eax;
    recomp_movie_checkpoint(4u, esi, MEM32(esi + 0x3EB8u),
                            edi, MEM32(esi));
    if (TEST_NZ(edi, edi)) goto loc_000D7C67; /* jne: not equal / not zero */
""",
    ),
    GeneratedPatch(
        "RedMovie decoder frame-result checkpoint",
        """    PUSH32(esp, 0); sub_00256153(); /* call 0x00256153 */

loc_00217956: ;
""",
        """    PUSH32(esp, 0); sub_00256153(); /* call 0x00256153 */

loc_00217956: ;
    recomp_movie_checkpoint(6u, esi, MEM32(esi), MEM32(esp + 0xC), edi);
""",
    ),
    GeneratedPatch(
        "RedMovie brush texture setter checkpoint",
        """    MEM32(esi + 0x24) = eax;
    MEM32(esi + 0x44) = edi;
    PUSH32(esp, 0); sub_0028B0F0(); /* call 0x0028B0F0 */
""",
        """    MEM32(esi + 0x24) = eax;
    MEM32(esi + 0x44) = edi;
    recomp_movie_checkpoint(10u, esi, edi, eax,
                            MEM32(MEM32(esi) + 8u));
    PUSH32(esp, 0); sub_0028B0F0(); /* call 0x0028B0F0 */
""",
    ),
    GeneratedPatch(
        "RsFrontEnd movie brush commit checkpoint",
        """loc_0010DB64: ;
    eax = MEM32(ebp);
    ecx = ebp;
    { uint32_t _icall_esp = g_esp;
""",
        """loc_0010DB64: ;
    eax = MEM32(ebp);
    recomp_movie_checkpoint(11u, ebp, MEM32(ebp + 0x44u),
                            MEM32(ebp + 0x24u), MEM32(eax + 8u));
    ecx = ebp;
    { uint32_t _icall_esp = g_esp;
""",
    ),
    GeneratedPatch(
        "RedBrush2D movie renderer-context checkpoint",
        """    esi = ecx;
    eax = MEM32(esi);
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(eax + 0x10), _icall_esp); /* indirect call */
    }

loc_001F7628: ;
    SET_LO8(ecx, 7);
""",
        """    esi = ecx;
    eax = MEM32(esi);
    recomp_movie_checkpoint(12u, esi, MEM32(esi + 0x44u),
                            MEM32(esi + 0x24u), MEM32(eax + 0x10u));
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(eax + 0x10), _icall_esp); /* indirect call */
    }

loc_001F7628: ;
    recomp_movie_checkpoint(13u, esi, eax, MEM32(esi + 0x24u),
                            MEM32(MEM32(esi) + 0x10u));
    SET_LO8(ecx, 7);
""",
    ),
    GeneratedPatch(
        "RedBrush2D renderer queue insertion checkpoint",
        """    eax++;
    MEM32(esi + 0x4C) = eax;

loc_001F754E: ;
""",
        """    eax++;
    MEM32(esi + 0x4C) = eax;
    recomp_movie_checkpoint(14u, edi, esi, MEM32(edi + 0x24u), eax);

loc_001F754E: ;
""",
    ),
    GeneratedPatch(
        "RsFrontEnd movie paint-state checkpoint",
        """loc_0010DD20: ;
    PUSH32(esp, esi);
    esi = ecx;
    SET_LO8(ecx, MEM8(esi + 0xDC));
""",
        """loc_0010DD20: ;
    PUSH32(esp, esi);
    esi = ecx;
    recomp_movie_checkpoint(15u, esi, esi + 0x34u,
                            MEM8(esi + 0x30u), MEM8(esi + 0xDCu));
    SET_LO8(ecx, MEM8(esi + 0xDC));
""",
    ),
    GeneratedPatch(
        "Red UI movie brush lookup entry checkpoint",
        """loc_0020B900: ;
    eax = MEM32(esp + 4);
    esp = esp - 0x20;
""",
        """loc_0020B900: ;
    eax = MEM32(esp + 4);
    if (eax == 0x1D6FB11Au)
        recomp_movie_checkpoint(16u, ecx, eax, MEM32(0x793514u),
                                MEM32(0x7AC814u));
    esp = esp - 0x20;
""",
    ),
    GeneratedPatch(
        "Red UI movie brush lookup result checkpoints",
        """loc_0020BA52: ;
    eax = edi;
    POP32(esp, edi);
""",
        """loc_0020BA52: ;
    if (edi == 0x1D6FB11Au)
        recomp_movie_checkpoint(17u, eax, edi, MEM32(0x793514u),
                                MEM32(0x7AC814u));
    eax = edi;
    POP32(esp, edi);
""",
    ),
    GeneratedPatch(
        "Red UI movie brush lookup failure checkpoint",
        """loc_0020BA59: ;
    eax = 0; /* xor self */
    POP32(esp, edi);
""",
        """loc_0020BA59: ;
    if (edi == 0x1D6FB11Au)
        recomp_movie_checkpoint(18u, 0u, edi, MEM32(0x793514u),
                                MEM32(0x7AC814u));
    eax = 0; /* xor self */
    POP32(esp, edi);
""",
    ),
    GeneratedPatch(
        "Red UI movie command creation checkpoint",
        """loc_0020BB6B: ;
    ecx = MEM32(0x7AC818);
    MEM32(ecx + 8) = eax;
    edx = MEM32(0x7AC818);
""",
        """loc_0020BB6B: ;
    ecx = MEM32(0x7AC818);
    MEM32(ecx + 8) = eax;
    if (eax == 0x1D6FB11Au)
        recomp_movie_checkpoint(19u, ecx, eax, MEM32(ecx),
                                MEM32(0x7AC814u));
    edx = MEM32(0x7AC818);
""",
    ),
    GeneratedPatch(
        "Red UI movie command consumption checkpoint",
        """    if (CMP_EQ(edi, ebx)) goto loc_0020DDE4; /* je: equal / zero */
""",
        """    if (MEM32(esi + 8u) == 0x1D6FB11Au)
        recomp_movie_checkpoint(20u, edi, MEM32(esi + 8u),
                                MEM32(esi), MEM32(esi + 4u));
    if (CMP_EQ(edi, ebx)) goto loc_0020DDE4; /* je: equal / zero */
""",
    ),    GeneratedPatch(
        "Red renderer movie texture-cache checkpoint",
        """    PUSH32(esp, edi);
    edi = MEM32(esp + 0xC);
    if (CMP_EQ(MEM32(edi * 4 + 0x7AD310), esi)) goto loc_0021059B; /* je: equal / zero */
""",
        """    PUSH32(esp, edi);
    edi = MEM32(esp + 0xC);
    recomp_movie_checkpoint(21u, esi, MEM32(esi + 0x44u), edi,
                            MEM32(edi * 4u + 0x7AD310u));
    if (CMP_EQ(MEM32(edi * 4 + 0x7AD310), esi)) goto loc_0021059B; /* je: equal / zero */
""",
    ),
    GeneratedPatch(
        "Red renderer movie texture-bind checkpoint",
        """loc_00210577: ;
    eax = MEM32(esi + 0x44);
    PUSH32(esp, eax);
""",
        """loc_00210577: ;
    eax = MEM32(esi + 0x44);
    recomp_movie_checkpoint(22u, esi, eax, edi,
                            MEM32(edi * 4u + 0x7AD310u));
    PUSH32(esp, eax);
""",
    ),
    GeneratedPatch(
        "Retail movie SetTexture resource checkpoint",
        """    MEM32(esp + 8) = recomp_validate_texture_bind(
        g_recomp_current_func, MEM32(esp + 4), MEM32(esp + 8));
    RECOMP_TRACE_FUNC(0x00289E20u);
""",
        """    MEM32(esp + 8) = recomp_validate_texture_bind(
        g_recomp_current_func, MEM32(esp + 4), MEM32(esp + 8));
    if (MEM32(esp + 8u) != 0u &&
        (MEM32(MEM32(esp + 8u) + 0x0Cu) & 0x0000FF00u) == 0x00002400u)
        recomp_movie_checkpoint(23u, MEM32(esp + 8u), MEM32(esp + 4u),
                                MEM32(MEM32(esp + 8u) + 4u),
                                MEM32(MEM32(esp + 8u) + 0x0Cu));
    RECOMP_TRACE_FUNC(0x00289E20u);
""",
    ),
    GeneratedPatch(
        "Red renderer movie texture-bind completion checkpoint",
        """loc_00210581: ;
    MEM32(edi * 4 + 0x7AD310) = esi;
    POP32(esp, edi);
""",
        """loc_00210581: ;
    MEM32(edi * 4 + 0x7AD310) = esi;
    recomp_movie_checkpoint(24u, esi, MEM32(esi + 0x44u), edi,
                            MEM32(edi * 4u + 0x7AD310u));
    POP32(esp, edi);
""",
    ),    GeneratedPatch(
        "XMV motion-comp combine checkpoints",
        """loc_00258252: ;
    PUSH32(esp, 0); sub_0025AB55(); /* call 0x0025AB55 */
""",
        """loc_00258252: ;
    recomp_xmv_mc_checkpoint(0u, MEM32(esp), MEM32(esp + 4u),
                             MEM32(esp + 8u), MEM32(esp + 12u),
                             MEM32(esp + 16u), MEM32(esp + 20u),
                             MEM32(esp + 24u));
    PUSH32(esp, 0); sub_0025AB55(); /* call 0x0025AB55 */
    recomp_xmv_mc_checkpoint(1u, 0u, 0u, 0u, 0u, 0u, 0u, 0u);
""",
    ),
    GeneratedPatch(
        "XMV delayed frame-index selector flags",
        """    ecx = MEM32(esi + 0x4C);
    eax = edi + edi * 4;
    (void)0; /* cmp MEM32(ecx + eax * 4 + 0x10), 1 - flags set for next jcc */
    eax = edi + 3;
    if (CMP_EQ(MEM32(ecx + eax * 4 + 0x10), 1)) goto loc_00255E3C; /* je: equal / zero */
""",
        """    ecx = MEM32(esi + 0x4C);
    eax = edi + edi * 4;
    (void)0; /* cmp MEM32(ecx + eax * 4 + 0x10), 1 - flags set for next jcc */
    _flags = (CMP_EQ(MEM32(ecx + eax * 4 + 0x10), 1)); /* preserve cmp flags across 1 instruction(s) */
    eax = edi + 3;
    if (_flags != 0) goto loc_00255E3C; /* je: equal / zero */
""",
    ),
    GeneratedPatch(
        "XMV delayed decode-buffer selector flags",
        """    eax = MEM32(esi + 0x138);
    { uint32_t _shift_value = (uint32_t)(edi) & 0xFFFFFFFFu;
      uint32_t _shift_count = (uint32_t)(2) & 0x1Fu;
      while (_shift_count-- != 0u) {
      uint32_t _shift_next_cf = (_shift_value >> 31u) & 1u;
      _shift_value = (_shift_value << 1u) & 0xFFFFFFFFu;
      _cf = (int)_shift_next_cf;
      }
    edi = _shift_value; } /* shl */
    (void)0; /* cmp MEM32(edi + eax), ebx - flags set for next jcc */
    eax = MEM32(esi + 0x13C);
    if (CMP_EQ(MEM32(edi + eax), ebx)) goto loc_002562EE; /* je: equal / zero */
""",
        """    eax = MEM32(esi + 0x138);
    { uint32_t _shift_value = (uint32_t)(edi) & 0xFFFFFFFFu;
      uint32_t _shift_count = (uint32_t)(2) & 0x1Fu;
      while (_shift_count-- != 0u) {
      uint32_t _shift_next_cf = (_shift_value >> 31u) & 1u;
      _shift_value = (_shift_value << 1u) & 0xFFFFFFFFu;
      _cf = (int)_shift_next_cf;
      }
    edi = _shift_value; } /* shl */
    (void)0; /* cmp MEM32(edi + eax), ebx - flags set for next jcc */
    _flags = (CMP_EQ(MEM32(edi + eax), ebx)); /* preserve cmp flags across 1 instruction(s) */
    eax = MEM32(esi + 0x13C);
    if (_flags != 0) goto loc_002562EE; /* je: equal / zero */
""",
    ),
    GeneratedPatch(
        "XMV delayed prediction-selector flags",
        """loc_00258285: ;
    eax = MEM32(ebp + 8);
    (void)0; /* cmp MEM32(eax + 0x114), esi - flags set for next jcc */
    eax = MEM32(ebp + 0x1C);
    SET_LO8(ecx, MEM8(eax + 1));
    eax = (uint32_t)(int32_t)SMEM8(eax);
    if (CMP_EQ(MEM32(eax + 0x114), esi)) goto loc_002582B2; /* je: equal / zero */
""",
        """loc_00258285: ;
    eax = MEM32(ebp + 8);
    (void)0; /* cmp MEM32(eax + 0x114), esi - flags set for next jcc */
    _flags = (CMP_EQ(MEM32(eax + 0x114), esi)); /* preserve cmp flags across 3 instruction(s) */
    eax = MEM32(ebp + 0x1C);
    SET_LO8(ecx, MEM8(eax + 1));
    eax = (uint32_t)(int32_t)SMEM8(eax);
    if (_flags != 0) goto loc_002582B2; /* je: equal / zero */
""",
    ),
    GeneratedPatch(
        "XMV 8x8 transform checkpoint first half",
        """loc_00258155: ;
    PUSH32(esp, 0);
    eax = ebp + -128;
    PUSH32(esp, eax);
    eax = ebp + -256;
    PUSH32(esp, eax);
    PUSH32(esp, 0); sub_002598FC(); /* call 0x002598FC */

loc_00258167: ;
""",
        """loc_00258155: ;
    recomp_xmv_transform_checkpoint(0u, 0x002598FCu, ebp + -256,
                                    ebp + -128, 0u);
    PUSH32(esp, 0);
    eax = ebp + -128;
    PUSH32(esp, eax);
    eax = ebp + -256;
    PUSH32(esp, eax);
    PUSH32(esp, 0); sub_002598FC(); /* call 0x002598FC */
    recomp_xmv_transform_checkpoint(1u, 0x002598FCu, 0u, 0u, 0u);

loc_00258167: ;
""",
    ),
    GeneratedPatch(
        "XMV 8x8 transform checkpoint second half",
        """loc_00258188: ;
    PUSH32(esp, ebx);
    eax = ebp + -128;
    PUSH32(esp, eax);
    eax = ebp + -256;
    PUSH32(esp, eax);
    PUSH32(esp, 0); sub_002598FC(); /* call 0x002598FC */

loc_00258199: ;
""",
        """loc_00258188: ;
    recomp_xmv_transform_checkpoint(0u, 0x002598FCu, ebp + -256,
                                    ebp + -128, ebx);
    PUSH32(esp, ebx);
    eax = ebp + -128;
    PUSH32(esp, eax);
    eax = ebp + -256;
    PUSH32(esp, eax);
    PUSH32(esp, 0); sub_002598FC(); /* call 0x002598FC */
    recomp_xmv_transform_checkpoint(1u, 0x002598FCu, 0u, 0u, 0u);

loc_00258199: ;
""",
    ),
    GeneratedPatch(
        "XMV 4x8 transform checkpoint first half",
        """loc_002581BD: ;
    PUSH32(esp, 0);
    eax = ebp + -128;
    PUSH32(esp, eax);
    eax = ebp + -256;
    PUSH32(esp, eax);
    PUSH32(esp, 0); sub_00259B73(); /* call 0x00259B73 */

loc_002581CF: ;
""",
        """loc_002581BD: ;
    recomp_xmv_transform_checkpoint(0u, 0x00259B73u, ebp + -256,
                                    ebp + -128, 0u);
    PUSH32(esp, 0);
    eax = ebp + -128;
    PUSH32(esp, eax);
    eax = ebp + -256;
    PUSH32(esp, eax);
    PUSH32(esp, 0); sub_00259B73(); /* call 0x00259B73 */
    recomp_xmv_transform_checkpoint(1u, 0x00259B73u, 0u, 0u, 0u);

loc_002581CF: ;
""",
    ),
    GeneratedPatch(
        "XMV 4x8 transform checkpoint second half",
        """loc_002581EC: ;
    PUSH32(esp, ebx);
    eax = ebp + -128;
    PUSH32(esp, eax);
    eax = ebp + -256;
    PUSH32(esp, eax);
    PUSH32(esp, 0); sub_00259B73(); /* call 0x00259B73 */

loc_002581FD: ;
""",
        """loc_002581EC: ;
    recomp_xmv_transform_checkpoint(0u, 0x00259B73u, ebp + -256,
                                    ebp + -128, ebx);
    PUSH32(esp, ebx);
    eax = ebp + -128;
    PUSH32(esp, eax);
    eax = ebp + -256;
    PUSH32(esp, eax);
    PUSH32(esp, 0); sub_00259B73(); /* call 0x00259B73 */
    recomp_xmv_transform_checkpoint(1u, 0x00259B73u, 0u, 0u, 0u);

loc_002581FD: ;
""",
    ),
    GeneratedPatch(
        "XMV coefficient decoder checkpoints",
        """loc_002580CD: ;
    PUSH32(esp, MEM32(ebp + 0x4C));
    eax = ebp + -128;
    PUSH32(esp, MEM32(ebp + 0x48));
    PUSH32(esp, eax);
    PUSH32(esp, 0x25E108);
    PUSH32(esp, MEM32(ebp + 0x28));
    PUSH32(esp, MEM32(ebp + 0xC));
    PUSH32(esp, edi);
    PUSH32(esp, 0); sub_00257DB9(); /* call 0x00257DB9 */

loc_002580E8: ;
""",
        """loc_002580CD: ;
    recomp_xmv_coeff_checkpoint(0u, edi, MEM32(ebp + 0xC),
                                MEM32(ebp + 0x28), 0x25E108u,
                                ebp + -128, MEM32(ebp + 0x48),
                                MEM32(ebp + 0x4C), 0u);
    PUSH32(esp, MEM32(ebp + 0x4C));
    eax = ebp + -128;
    PUSH32(esp, MEM32(ebp + 0x48));
    PUSH32(esp, eax);
    PUSH32(esp, 0x25E108);
    PUSH32(esp, MEM32(ebp + 0x28));
    PUSH32(esp, MEM32(ebp + 0xC));
    PUSH32(esp, edi);
    PUSH32(esp, 0); sub_00257DB9(); /* call 0x00257DB9 */
    recomp_xmv_coeff_checkpoint(1u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, eax);

loc_002580E8: ;
""",
    ),
    GeneratedPatch(
        "XMV inter-frame IDCT checkpoints",
        """loc_002580E8: ;
    PUSH32(esp, eax);
    eax = ebp + -128;
    PUSH32(esp, eax);
    eax = ebp + -256;
    PUSH32(esp, eax);
    PUSH32(esp, 0); sub_0025927A(); /* call 0x0025927A */
""",
        """loc_002580E8: ;
    recomp_xmv_idct_checkpoint(0u, ebp + -256, ebp + -128, eax);
    PUSH32(esp, eax);
    eax = ebp + -128;
    PUSH32(esp, eax);
    eax = ebp + -256;
    PUSH32(esp, eax);
    PUSH32(esp, 0); sub_0025927A(); /* call 0x0025927A */
    recomp_xmv_idct_checkpoint(1u, ebp + -256, ebp + -128, 0u);
""",
    ),
    GeneratedPatch(
        "XMV full-frame decoder timing entry",
        """    RECOMP_TRACE_FUNC(0x002582FFu);

loc_002582FF: ;
""",
        """    RECOMP_TRACE_FUNC(0x002582FFu);
    recomp_xmv_frame_decode_checkpoint(0u, MEM32(esp + 4u));

loc_002582FF: ;
""",
    ),
    GeneratedPatch(
        "XMV full-frame decoder timing exit",
        """loc_00258D9A: ;
    POP32(esp, esi);
    { uint32_t _alu_dst = (uint32_t)(ebp);
      uint32_t _alu_src = (uint32_t)(0x74);
      _cf = ((uint64_t)_alu_dst + (uint64_t)_alu_src > 0xFFFFFFFFull);
    ebp = _alu_dst + _alu_src; }
    esp = ebp;
    POP32(esp, ebp); /* leave */
    esp += 8; return; /* ret 4 */
""",
        """loc_00258D9A: ;
    POP32(esp, esi);
    { uint32_t _alu_dst = (uint32_t)(ebp);
      uint32_t _alu_src = (uint32_t)(0x74);
      _cf = ((uint64_t)_alu_dst + (uint64_t)_alu_src > 0xFFFFFFFFull);
    ebp = _alu_dst + _alu_src; }
    esp = ebp;
    POP32(esp, ebp); /* leave */
    recomp_xmv_frame_decode_checkpoint(1u, 0u);
    esp += 8; return; /* ret 4 */
""",
    ),
    GeneratedPatch(
        "XMV planar-to-YUY2 conversion checkpoints",
        """loc_0025732A: ;
    PUSH32(esp, MEM32(ebp + -8));
    eax = MEM32(ebp + 8);
""",
        """loc_0025732A: ;
    recomp_movie_checkpoint(7u, MEM32(ebp + 8), MEM32(ebp + 0xC),
                            MEM32(ebp + -36), MEM32(ebp + -8));
    PUSH32(esp, MEM32(ebp + -8));
    eax = MEM32(ebp + 8);
""",
    ),
    GeneratedPatch(
        "XMV post-YUY2 conversion checkpoint",
        """    PUSH32(esp, MEM32(eax + 0xDC));
    PUSH32(esp, 0); sub_002569F3(); /* call 0x002569F3 */

loc_0025735C: ;
""",
        """    PUSH32(esp, MEM32(eax + 0xDC));
    PUSH32(esp, 0); sub_002569F3(); /* call 0x002569F3 */
    recomp_movie_checkpoint(8u, MEM32(ebp + 8), MEM32(ebp + 0xC),
                            MEM32(ebp + -4), MEM32(ebp + -8));

loc_0025735C: ;
""",
    ),
    GeneratedPatch(
        "RedMovie teardown checkpoint",
        """void sub_00217A10(void)
{
    int _flags = 0; /* fallback flag var */
    RECOMP_TRACE_FUNC(0x00217A10u);
""",
        """void sub_00217A10(void)
{
    int _flags = 0; /* fallback flag var */
    RECOMP_TRACE_FUNC(0x00217A10u);
    recomp_movie_checkpoint(9u, ecx, MEM32(ecx), 0u, 0u);
""",
    ),
    GeneratedPatch(
        "RedMovie init entry checkpoint",
        """void sub_00217B80(void)
{
    int _flags = 0; /* fallback flag var */
    int _cf = 0; /* carry flag */
    RECOMP_TRACE_FUNC(0x00217B80u);

loc_00217B80: ;
""",
        """void sub_00217B80(void)
{
    int _flags = 0; /* fallback flag var */
    int _cf = 0; /* carry flag */
    uint32_t movie_trace_path = MEM32(esp + 4);
    RECOMP_TRACE_FUNC(0x00217B80u);
    recomp_movie_checkpoint(1u, ecx, movie_trace_path,
                            MEM32(esp + 8), MEM32(esp + 12));

loc_00217B80: ;
""",
    ),
    GeneratedPatch(
        "RedMovie decoder creation result checkpoint",
        """loc_00217BC1: ;
    if (CMP_GE((eax & eax), 0)) goto loc_00217BCE; /* jge: greater or equal (signed >=) */
""",
        """loc_00217BC1: ;
    recomp_movie_checkpoint(2u, esi, movie_trace_path, eax, MEM32(esi));
    if (CMP_GE((eax & eax), 0)) goto loc_00217BCE; /* jge: greater or equal (signed >=) */
""",
    ),
    GeneratedPatch(
        "RedMovie first decoded frame checkpoint",
        """loc_00217BDC: ;
    ecx = esi;
    PUSH32(esp, 0); sub_00217A30(); /* call 0x00217A30 */

loc_00217BE3: ;
    SET_LO8(eax, 1);
""",
        """loc_00217BDC: ;
    if (!recomp_movie_audio_enabled(MEM8(esi + 0x14)))
        MEM8(esi + 0x14) = 0;
    ecx = esi;
    PUSH32(esp, 0); sub_00217A30(); /* call 0x00217A30 */

loc_00217BE3: ;
    recomp_movie_checkpoint(3u, esi, movie_trace_path,
                            MEM32(esi), MEM32(esi + 0x3Cu));
    SET_LO8(eax, 1);
""",
    ),
    GeneratedPatch(
        "Host-backed DirectSound reference clock",
        """    eax = MEM32(0x299378);
    edx = MEM32(eax + 0x1DE8);
    ecx = MEM32(esp + 4);
""",
        """    eax = MEM32(0x299378);
    edx = MEM32(eax + 0x1DE8);
    edx = recomp_dsound_clock_value(eax, edx);
    ecx = MEM32(esp + 4);
""",
    ),
    GeneratedPatch(
        "Brush3D lock checkpoint",
        """loc_0020BDAA: ;
    MEM32(0x7AC7C0) = eax;
""",
        """loc_0020BDAA: ;
    recomp_brush3d_checkpoint(1u, eax);
    MEM32(0x7AC7C0) = eax;
""",
    ),
    GeneratedPatch(
        "Brush3D paint checkpoint",
        """    MEM32(0x7AC7C4) = edx;
    MEM16(eax + 0x52) = MEM16(eax + 0x52) + 1;
    esp += 4; return; /* ret */
""",
        """    MEM32(0x7AC7C4) = edx;
    MEM16(eax + 0x52) = MEM16(eax + 0x52) + 1;
    recomp_brush3d_checkpoint(2u, edx - 0x18u);
    esp += 4; return; /* ret */
""",
    ),
    GeneratedPatch(
        "Brush3D render checkpoint",
        """loc_0020E2CD: ;
    eax = MEM32(0x7AC7D4);
""",
        """loc_0020E2CD: ;
    recomp_brush3d_checkpoint(3u, MEM32(0x7AC7C0));
    eax = MEM32(0x7AC7D4);
""",
    ),
)


PATCHES += (
    GeneratedPatch(
        "scene list merge stable entry stack declaration",
        """void sub_001C8F90(void)
{
    uint32_t ebp;
    int _flags = 0; /* fallback flag var */
""",
        """void sub_001C8F90(void)
{
    uint32_t ebp;
    uint32_t merge_entry_esp = esp;
    int _flags = 0; /* fallback flag var */
""",
    ),
    GeneratedPatch(
        "scene list merge first range call checkpoint",
        """loc_001C8FCD: ;
    eax = MEM32(esp + 0x44);
""",
        """loc_001C8FCD: ;
    if (merge_entry_esp >= 0x008BF800u)
        recomp_temp_array_checkpoint(6u, merge_entry_esp, 0x00230790u,
                                     esp, 10u);
    eax = MEM32(esp + 0x44);
""",
    ),
    GeneratedPatch(
        "scene list merge second range call checkpoint",
        """loc_001C8FD8: ;
    eax = MEM32(esi + 0x6C);
""",
        """loc_001C8FD8: ;
    if (merge_entry_esp >= 0x008BF800u)
        recomp_temp_array_checkpoint(6u, merge_entry_esp, 0x00230790u,
                                     esp, 11u);
    eax = MEM32(esi + 0x6C);
""",
    ),
    GeneratedPatch(
        "scene list merge temp constructor focused checkpoint",
        """    recomp_temp_array_checkpoint(35u, esp + 0x10u, 0u, 0u, esp);
    eax = MEM32(esi + 0x68);
    edx = MEM32(esp + 0x10);
""",
        """    recomp_temp_array_checkpoint(35u, esp + 0x10u, 0u, 0u, esp);
    eax = MEM32(esi + 0x68);
    if (merge_entry_esp >= 0x008BF800u)
        recomp_temp_array_checkpoint(6u, merge_entry_esp, 0x001BE690u,
                                     esp, 12u);
    edx = MEM32(esp + 0x10);
""",
    ),
    GeneratedPatch(
        "scene list merge collection callback focused checkpoint",
        """    recomp_temp_array_checkpoint(41u, esp + 0x10u, 0u, 0u, esp);
    edx = MEM32(esi + 0x20);
    eax = MEM32(edx + 0x138);
""",
        """    recomp_temp_array_checkpoint(41u, esp + 0x10u, 0u, 0u, esp);
    edx = MEM32(esi + 0x20);
    if (merge_entry_esp >= 0x008BF800u)
        recomp_temp_array_checkpoint(6u, merge_entry_esp, 0x00230500u,
                                     esp, 13u);
    eax = MEM32(edx + 0x138);
""",
    ),
    GeneratedPatch(
        "scene list merge old storage release focused checkpoint",
        """loc_001C9101: ;
    eax = MEM32(esp + 0x14);
""",
        """loc_001C9101: ;
    if (merge_entry_esp >= 0x008BF800u)
        recomp_temp_array_checkpoint(6u, merge_entry_esp, 0x001C9101u,
                                     esp, 14u);
    eax = MEM32(esp + 0x14);
""",
    ),
    GeneratedPatch(
        "scene list merge new storage allocation focused checkpoint",
        """loc_001C9116: ;
    MEM32(esi + 0x68) = eax;
""",
        """loc_001C9116: ;
    if (merge_entry_esp >= 0x008BF800u)
        recomp_temp_array_checkpoint(6u, merge_entry_esp, 0x001C9116u,
                                     esp, 15u);
    MEM32(esi + 0x68) = eax;
""",
    ),
    GeneratedPatch(
        "scene list merge pre-destructor focused checkpoint",
        """    recomp_temp_array_checkpoint(36u, esp + 0x10u, 0u, 0u, esp);
    ecx = esp + 0x10;
    MEM32(esp + 0x2C) = 0xFFFFFFFFu;
""",
        """    recomp_temp_array_checkpoint(36u, esp + 0x10u, 0u, 0u, esp);
    ecx = esp + 0x10;
    if (merge_entry_esp >= 0x008BF800u)
        recomp_temp_array_checkpoint(6u, merge_entry_esp, 0x001BE720u,
                                     esp, 16u);
    MEM32(esp + 0x2C) = 0xFFFFFFFFu;
""",
    ),
    GeneratedPatch(
        "scene list merge post-destructor focused checkpoint",
        """loc_001C9162: ;
    POP32(esp, ebp);

loc_001C9163: ;
""",
        """loc_001C9162: ;
    if (merge_entry_esp >= 0x008BF800u)
        recomp_temp_array_checkpoint(6u, merge_entry_esp, 0x001BE720u,
                                     esp, 17u);
    POP32(esp, ebp);

loc_001C9163: ;
    if (merge_entry_esp >= 0x008BF800u)
        recomp_temp_array_checkpoint(6u, merge_entry_esp, 0x001C9163u,
                                     esp, 18u);
""",
    ),
)

PATCHES += (
    GeneratedPatch(
        "retail scene pair-sort corruption checkpoint",
        """    PUSH32(esp, edi);
    edi = MEM32(esp + 0xC);
    if (TEST_Z(edi, edi)) goto loc_002307DE; /* je: equal / zero */
""",
        """    PUSH32(esp, edi);
    edi = MEM32(esp + 0xC);
    recomp_pair_sort_checkpoint(MEM32(esp + 8), edi, esp);
    if (TEST_Z(edi, edi)) goto loc_002307DE; /* je: equal / zero */
""",
    ),
    GeneratedPatch(
        "retail broadphase update invariant checkpoint",
        """loc_001258CF: ;
    eax = MEM32(ebp + 0xC);
""",
        """loc_001258CF: ;
    recomp_broadphase_invariant_checkpoint(ecx, MEM32(esp + 0x1C),
                                           MEM32(esp + 0x24),
                                           MEM32(ebp + 0xC));
    eax = MEM32(ebp + 0xC);
""",
    ),
    GeneratedPatch(
        "retail scene pair append checkpoint at 0x00125E91",
        """loc_00125E91: ;
""",
        """loc_00125E91: ;
    recomp_pair_append_checkpoint(0x00125E91u, esi, eax, MEM32(ebp + 0x14), esp, ebp);
""",
    ),
    GeneratedPatch(
        "retail scene pair append checkpoint at 0x00125F49",
        """loc_00125F49: ;
""",
        """loc_00125F49: ;
    recomp_pair_append_checkpoint(0x00125F49u, esi, eax, MEM32(ebp + 0x14), esp, ebp);
""",
    ),
    GeneratedPatch(
        "retail scene pair append checkpoint at 0x00125FDC",
        """loc_00125FDC: ;
""",
        """loc_00125FDC: ;
    recomp_pair_append_checkpoint(0x00125FDCu, esi, eax, MEM32(ebp + 0x18), esp, ebp);
""",
    ),
    GeneratedPatch(
        "retail scene pair append checkpoint at 0x00126070",
        """loc_00126070: ;
""",
        """loc_00126070: ;
    recomp_pair_append_checkpoint(0x00126070u, esi, eax, MEM32(ebp + 0x18), esp, ebp);
""",
    ),
    GeneratedPatch(
        "retail scene pair append checkpoint at 0x00126104",
        """loc_00126104: ;
""",
        """loc_00126104: ;
    recomp_pair_append_checkpoint(0x00126104u, esi, eax, MEM32(ebp + 0x14), esp, ebp);
""",
    ),
    GeneratedPatch(
        "retail scene pair append checkpoint at 0x001261B8",
        """loc_001261B8: ;
""",
        """loc_001261B8: ;
    recomp_pair_append_checkpoint(0x001261B8u, esi, eax, MEM32(ebp + 0x14), esp, ebp);
""",
    ),
    GeneratedPatch(
        "retail scene pair append checkpoint at 0x0012624C",
        """loc_0012624C: ;
""",
        """loc_0012624C: ;
    recomp_pair_append_checkpoint(0x0012624Cu, esi, eax, MEM32(ebp + 0x18), esp, ebp);
""",
    ),
    GeneratedPatch(
        "retail scene pair append checkpoint at 0x001262EB",
        """loc_001262EB: ;
""",
        """loc_001262EB: ;
    recomp_pair_append_checkpoint(0x001262EBu, esi, eax, MEM32(ebp + 0x18), esp, ebp);
""",
    ),
    GeneratedPatch(
        "retail float setter tail at 0x0010E540",
        """void sub_0010E540(void) { esp += 4; /* 0x0010E540: not detected; minimal guest ret */ }
""",
        """void sub_0010E540(void)
{
    /* Retail tail target: movss xmm0,[esp+4]; movss [ecx],xmm0; ret 4. */
    MEM32(ecx) = MEM32(esp + 4);
    esp += 8;
}
""",
    ),
    GeneratedPatch(
        "luaV_execute incomplete yield tail migration",
        """    if (CMP_A(eax, MEM32(ebx + 8))) { /* ja: above (unsigned >) */
        /* Retail 0x001E1EE4 is luaV_execute's internal yield epilogue.
         * Return NULL while restoring this function's guest frame. */
        eax = 0;
        POP32(esp, edi);
        POP32(esp, esi);
        POP32(esp, ebx);
        esp = ebp;
        POP32(esp, ebp);
        esp = esp + 4;
        return;
    }
""",
        """    if (CMP_A(eax, MEM32(ebx + 8))) { /* ja: above (unsigned >) */
        /* Retail 0x001E1EE4 is luaV_execute's internal yield epilogue.
         * Preserve the active CallInfo program counter/result state before
         * returning NULL, then restore this function's guest frame. */
        ecx = MEM32(ebx + 0x14);
        edx = MEM32(esp + 0x0C);
        MEM32(ecx - 0x0C) = edx;
        eax = MEM32(ebx + 0x14);
        MEM32(eax - 0x10) = 8;
        eax = 0;
        POP32(esp, edi);
        POP32(esp, esi);
        POP32(esp, ebx);
        esp = ebp;
        POP32(esp, ebp);
        esp = esp + 4;
        return;
    }
""",
    ),
    GeneratedPatch(
        "luaV_execute internal yield tail",
        """    if (CMP_A(eax, MEM32(ebx + 8))) { sub_001E1EE4(); return; } /* ja: above (unsigned >) */
""",
        """    if (CMP_A(eax, MEM32(ebx + 8))) { /* ja: above (unsigned >) */
        /* Retail 0x001E1EE4 is luaV_execute's internal yield epilogue.
         * Preserve the active CallInfo program counter/result state before
         * returning NULL, then restore this function's guest frame. */
        ecx = MEM32(ebx + 0x14);
        edx = MEM32(esp + 0x0C);
        MEM32(ecx - 0x0C) = edx;
        eax = MEM32(ebx + 0x14);
        MEM32(eax - 0x10) = 8;
        eax = 0;
        POP32(esp, edi);
        POP32(esp, esi);
        POP32(esp, ebx);
        esp = ebp;
        POP32(esp, ebp);
        esp = esp + 4;
        return;
    }
""",
    ),
    GeneratedPatch(
        "luaV_execute internal OP_RETURN tail",
        """    if (TEST_Z(MEM8(eax + 8), 4)) { sub_001E1F01(); return; } /* je: equal / zero */
""",
        """    if (TEST_Z(MEM8(eax + 8), 4)) { /* je: equal / zero */
        /* Retail 0x001E1F01 is an internal luaV_execute tail, not a
         * standalone function: return the first result and unwind this
         * function's frame exactly as the original epilogue does. */
        eax = edi;
        POP32(esp, edi);
        POP32(esp, esi);
        POP32(esp, ebx);
        esp = ebp;
        POP32(esp, ebp);
        esp = esp + 4;
        return;
    }
""",
    ),
    GeneratedPatch(
        "Xbox INT2D debug print consumes following INT3 marker",
        """    /* TODO: int 0x2d */
    __debugbreak(); /* int3 */
""",
        """    /*
     * Xbox debug service 1 consumes the byte following INT 2D.  The retail
     * CRT deliberately places INT3 there as that consumed service marker; it
     * is not a second breakpoint.  Surface the counted ANSI_STRING to the
     * host log and continue at the retail epilogue.
     */
    recomp_xbox_debug_print(MEM32(ebp + -4), ZX16(MEM16(ebp + -8)));
""",
    ),
    GeneratedPatch(
        "scene callback mismatched release target checkpoint",
        """loc_001C7DE6: ;
    eax = MEM32(ecx);
    esi = MEM32(ecx + 8);
    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(eax + 0x14), _icall_esp); /* indirect call */
    }
""",
        """loc_001C7DE6: ;
    eax = MEM32(ecx);
    esi = MEM32(ecx + 8);
    { uint32_t _icall_esp = g_esp;
    uint32_t _release_target = MEM32(eax + 0x14);
    if (_release_target == 0x00087D20u)
        recomp_temp_array_checkpoint(20u, ecx, eax, esi,
                                     _release_target);
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(_release_target, _icall_esp); /* indirect call */
    if (_release_target == 0x00087D20u && g_esp == _icall_esp + 4u)
        g_esp = _icall_esp;
    }
""",
    ),
    GeneratedPatch(
        "scene callback alternate mismatched release target checkpoint",
        """loc_001C7F54: ;
    eax = MEM32(ecx);
    ebp = MEM32(ecx + 8);
    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(eax + 0x14), _icall_esp); /* indirect call */
    }
""",
        """loc_001C7F54: ;
    eax = MEM32(ecx);
    ebp = MEM32(ecx + 8);
    { uint32_t _icall_esp = g_esp;
    uint32_t _release_target = MEM32(eax + 0x14);
    if (_release_target == 0x00087D20u)
        recomp_temp_array_checkpoint(20u, ecx, eax, ebp,
                                     _release_target);
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(_release_target, _icall_esp); /* indirect call */
    if (_release_target == 0x00087D20u && g_esp == _icall_esp + 4u)
        g_esp = _icall_esp;
    }
""",
    ),
)
PATCHES += (
    GeneratedPatch(
        "Retail actor update omitted 0x000401FE continuation",
        """loc_000401F4: ;
    SET_LO8(eax, MEM8(esi + 0x1AA));
    if (TEST_NZ(LO8(eax), LO8(eax))) { sub_00040260(); return; } /* jne: not equal / not zero */

}
""",
        """loc_000401F4: ;
    SET_LO8(eax, MEM8(esi + 0x1AA));
    if (TEST_NZ(LO8(eax), LO8(eax))) { sub_00040260(); return; } /* jne: not equal / not zero */

loc_000401FE: ;
    SET_LO8(eax, MEM8(esp + 0xB));
    if (TEST_NZ(LO8(eax), LO8(eax))) goto loc_00040255; /* jne: not equal / not zero */

loc_00040206: ;
    edx = MEM32(edi + 0x8BC);
    if (CMP_NE(MEM32(edx + 0x118), 6)) goto loc_00040255; /* jne: not equal / not zero */

loc_00040215: ;
    eax = MEM32(esi + 0xF4);
    PUSH32(esp, eax);
    ecx = esi;
    PUSH32(esp, 0); sub_0003FDB0(); /* call 0x0003FDB0 */

loc_00040223: ;
    if (TEST_NZ(LO8(eax), LO8(eax))) goto loc_00040255; /* jne: not equal / not zero */

loc_00040227: ;
    recomp_xmm_loadss(xmm0v, esi + 0x1AC); /* movss */
    xmm0 = xmm0 + MEMF(esp + 0x20); /* addss */
    MEMF(esi + 0x1AC) = xmm0; /* movss */
    if (((isnan((double)(xmm0)) || isnan((double)(MEMF(0x30C33C)))) || (xmm0 < MEMF(0x30C33C)))) { sub_00040260(); return; } /* jb: below (unsigned <) */

loc_00040246: ;
    ecx = esi;
    PUSH32(esp, 0); sub_0003FE00(); /* call 0x0003FE00 */
    sub_00040260(); return; /* retail shared epilogue */

loc_00040255: ;
    recomp_xmm_zero(xmm0v); /* xorps self = zero */
    MEMF(esi + 0x1AC) = xmm0; /* movss */
    sub_00040260(); return; /* retail shared epilogue */

}
""",
    ),
    GeneratedPatch(
        "Retail actor update shared 0x00040260 epilogue",
        """loc_00040260: ;
    POP32(esp, edi);
    POP32(esp, esi);

}
""",
        """loc_00040260: ;
    POP32(esp, edi);
    POP32(esp, esi);
    esp = esp + 0x14;
    esp += 8; return; /* ret 4 */

}
""",
    ),
)
PATCHES += (
    GeneratedPatch(
        "retail Havok MOPP long-ray return local declarations",
        """void sub_001B28E0(void)
{
    uint32_t ebp;
""",
        """void sub_001B28E0(void)
{
    uint32_t ebp;
    uint32_t mopp_saved_ebx;
    uint32_t mopp_saved_esi;
    uint32_t mopp_saved_edi;
""",
    ),
    GeneratedPatch(
        "retail Havok MOPP long-ray return register checkpoint",
        """    recomp_xmm_store(esp + 0x3C, xmm4v); /* movaps */
    recomp_xmm_store(esp + 0x4C, xmm1v); /* movaps */
    PUSH32(esp, 0); sub_001B1BA0(); /* call 0x001B1BA0 */

loc_001B29A2: ;
""",
        """    recomp_xmm_store(esp + 0x3C, xmm4v); /* movaps */
    recomp_xmm_store(esp + 0x4C, xmm1v); /* movaps */
    mopp_saved_ebx = ebx;
    mopp_saved_esi = esi;
    mopp_saved_edi = edi;
    PUSH32(esp, 0); sub_001B1BA0(); /* call 0x001B1BA0 */

loc_001B29A2: ;
    recomp_mopp_long_ray_return_checkpoint(
        mopp_saved_esi, MEM32(ebp + 8u), ebp, esp,
        mopp_saved_ebx, mopp_saved_esi, mopp_saved_edi,
        ebx, esi, edi);
""",
    ),
    GeneratedPatch(
        "retail camera ray broadphase begin checkpoint",
        """    PUSH32(esp, ebx);
    PUSH32(esp, esi);
    esi = MEM32(ebp + 8);
    recomp_xmm_loadss(xmm0v, esi + 0x70); /* movss */
""",
        """    PUSH32(esp, ebx);
    PUSH32(esp, esi);
    esi = MEM32(ebp + 8);
    MEM32(ebp + 0x14u) = recomp_camera_ray_begin_checkpoint(
        esi, MEM32(ebp + 0xCu), MEM32(ebp + 0x10u),
        MEM32(ebp + 0x14u), ebp);
    recomp_xmm_loadss(xmm0v, esi + 0x70); /* movss */
""",
    ),
    GeneratedPatch(
        "retail AI update entry vtable snapshot declaration",
        """void sub_0006B150(void)
{
    int _flags = 0; /* fallback flag var */
""",
        """void sub_0006B150(void)
{
    uint32_t ai_update_entry_vtable = 0u;
    uint32_t ai_update_pre_stimulus_depth = 0u;
    uint32_t ai_update_pre_stimulus_object = 0u;
    uint32_t ai_update_pre_stimulus_stack = 0u;
    int _flags = 0; /* fallback flag var */
""",
    ),
    GeneratedPatch(
        "retail AI update entry vtable snapshot",
        """    PUSH32(esp, esi);
    esi = ecx;
    MEMF(esp + 0x34) = xmm0; /* movss */
""",
        """    PUSH32(esp, esi);
    esi = ecx;
    ai_update_entry_vtable = recomp_ai_update_vtable_checkpoint(
        0x0006B150u, esi, 0u);
    if (ai_update_entry_vtable < 0x00200000u ||
        ai_update_entry_vtable > 0x0032FFF8u)
        goto loc_0006B584;
    MEMF(esp + 0x34) = xmm0; /* movss */
""",
    ),
    GeneratedPatch(
        "retail AI pre-stimulus object snapshot",
        """loc_0006B20F: ;
    ecx = esi;
""",
        """loc_0006B20F: ;
    ai_update_pre_stimulus_object = esi;
    ai_update_pre_stimulus_depth = MEM32(esi + 0x4B8u);
    recomp_ai_stimulus_boundary_checkpoint(
        0u, esi, ai_update_entry_vtable, ai_update_pre_stimulus_depth);
    ai_update_pre_stimulus_stack = esp;
    ecx = esi;
""",
    ),
    GeneratedPatch(
        "retail AI update post-stimulus vtable checkpoint",
        """loc_0006B216: ;
    ecx = esp + 0x1C;
""",
        """loc_0006B216: ;
    esp = recomp_ai_process_stimuli_esp_checkpoint(
        ai_update_pre_stimulus_stack, esp);
    esi = recomp_ai_process_stimuli_esi_checkpoint(
        ai_update_pre_stimulus_object, esi);
    if (!recomp_ai_stimulus_boundary_checkpoint(
            1u, esi, ai_update_entry_vtable, ai_update_pre_stimulus_depth)) {
        POP32(esp, edi);
        goto loc_0006B584;
    }
    recomp_ai_update_vtable_checkpoint(0x0006B216u, esi,
                                       ai_update_entry_vtable);
    ecx = esp + 0x1C;
""",
    ),
    GeneratedPatch(
        "retail AI update pre-anchor vtable checkpoint",
        """loc_0006B368: ;
    edx = MEM32(esi);
""",
        """loc_0006B368: ;
    recomp_ai_update_vtable_checkpoint(0x0006B368u, esi,
                                       ai_update_entry_vtable);
    edx = MEM32(esi);
""",
    ),
    GeneratedPatch(
        "retail AI perception ray count watch before increment",
        """loc_000699F9: ;
    eax = MEM32(esp + 0x18);
""",
        """loc_000699F9: ;
    recomp_ai_perception_ray_count_checkpoint(0x000699F9u, MEM32(esp + 0x18u), esp);
    eax = MEM32(esp + 0x18);
""",
    ),
    GeneratedPatch(
        "retail AI perception ray count watch at loop tail",
        """loc_00069A23: ;
    ecx = MEM32(esp + 0x30);
""",
        """loc_00069A23: ;
    recomp_ai_perception_ray_count_checkpoint(0x00069A23u, MEM32(esp + 0x18u), esp);
    ecx = MEM32(esp + 0x30);
""",
    ),
    GeneratedPatch(
        "retail AI perception ray count watch before batch",
        """loc_00069A3C: ;
    edi = MEM32(esp + 0x18);
""",
        """loc_00069A3C: ;
    recomp_ai_perception_ray_count_checkpoint(0x00069A3Cu, MEM32(esp + 0x18u), esp);
    edi = MEM32(esp + 0x18);
""",
    ),
    GeneratedPatch(
        "retail AI perception ray callsite checkpoint",
        """loc_00069A44: ;
    PUSH32(esp, 1);
    PUSH32(esp, edi);
    PUSH32(esp, 0x104);
""",
        """loc_00069A44: ;
    edi = recomp_camera_ray_callsite_checkpoint(0x00069A44u, ebp, edi, esp);
    MEM32(esp + 0x18u) = edi;
    if (CMP_LE((edi & edi), 0)) goto loc_00069A9C;
    PUSH32(esp, 1);
    PUSH32(esp, edi);
    PUSH32(esp, 0x104);
""",
    ),
    GeneratedPatch(
        "retail visibility batch ray callsite checkpoint",
        """loc_00156454: ;
    PUSH32(esp, 1);
    PUSH32(esp, esi);
    PUSH32(esp, 0x104);
""",
        """loc_00156454: ;
    esi = recomp_camera_ray_callsite_checkpoint(0x00156454u, edi, esi, esp);
    MEM32(esp + 0xCu) = esi;
    PUSH32(esp, 1);
    PUSH32(esp, esi);
    PUSH32(esp, 0x104);
""",
    ),
    GeneratedPatch(
        "retail camera ray broadphase candidate checkpoint",
        """loc_0012CDBF: ;
    edx = MEM32(ebp + 8);
    if (CMP_NE(MEM32(edx + 8), 1)) goto loc_0012CF45; /* jne: not equal / not zero */
""",
        """loc_0012CDBF: ;
    edx = MEM32(ebp + 8);
    recomp_camera_ray_candidate_checkpoint(edx);
    if (CMP_NE(MEM32(edx + 8), 1)) goto loc_0012CF45; /* jne: not equal / not zero */
""",
    ),
    GeneratedPatch(
        "retail camera ray closest-shape result checkpoint",
        """loc_0012CEF0: ;
    if (((uint8_t)(MEM8(eax)) == (uint8_t)(0))) goto loc_0012CF45; /* je: equal / zero */
""",
        """loc_0012CEF0: ;
    recomp_camera_ray_shape_result_checkpoint(
        MEM32(esp + 0x10u), eax, esp + 0x20u, ebx);
    if (((uint8_t)(MEM8(eax)) == (uint8_t)(0))) goto loc_0012CF45; /* je: equal / zero */
""",
    ),
    GeneratedPatch(
        "retail camera ray collision query checkpoint",
        """    PUSH32(esp, 0); sub_0012C650(); /* call 0x0012C650 */

loc_000988DA: ;
""",
        """    PUSH32(esp, 0); sub_0012C650(); /* call 0x0012C650 */
    recomp_camera_collision_query_checkpoint(1u, eax, esp + 0x2Cu);

loc_000988DA: ;
""",
    ),
    GeneratedPatch(
        "retail camera near-box collision query checkpoint",
        """    PUSH32(esp, 0); sub_0012E8C0(); /* call 0x0012E8C0 */

loc_00098B37: ;
""",
        """    PUSH32(esp, 0); sub_0012E8C0(); /* call 0x0012E8C0 */
    recomp_camera_collision_query_checkpoint(2u, eax, esp + 0x24u);

loc_00098B37: ;
""",
    ),
    GeneratedPatch(
        "retail camera box-cast collision query checkpoint",
        """    PUSH32(esp, 0); sub_0012CF60(); /* call 0x0012CF60 */

loc_00098D8E: ;
""",
        """    PUSH32(esp, 0); sub_0012CF60(); /* call 0x0012CF60 */
    recomp_camera_collision_query_checkpoint(3u, eax, esp + 0x20u);

loc_00098D8E: ;
""",
    ),
)
PATCHES += (
    GeneratedPatch(
        "retail camera collision result checkpoint at 0x0009E1D5",
        """    ecx = esi;
    PUSH32(esp, 0); sub_00098740(); /* call 0x00098740 */

loc_0009E1D5: ;
""",
        """    ecx = esi;
    PUSH32(esp, 0); sub_00098740(); /* call 0x00098740 */
    recomp_camera_collision_checkpoint(0x0009E1D5u, esi, eax);

loc_0009E1D5: ;
""",
    ),
    GeneratedPatch(
        "retail Mario64 camera collision result checkpoint",
        """    ecx = edi;
    PUSH32(esp, 0); sub_00098740(); /* call 0x00098740 */

loc_0009EAD3: ;
""",
        """    ecx = edi;
    PUSH32(esp, 0); sub_00098740(); /* call 0x00098740 */
    recomp_camera_collision_checkpoint(0x0009EAD3u, edi, eax);

loc_0009EAD3: ;
""",
    ),
    GeneratedPatch(
        "retail camera collision result checkpoint at 0x000A17AF",
        """    MEM32(edx + 8) = eax;
    PUSH32(esp, 0); sub_00098740(); /* call 0x00098740 */

loc_000A17AF: ;
""",
        """    MEM32(edx + 8) = eax;
    PUSH32(esp, 0); sub_00098740(); /* call 0x00098740 */
    recomp_camera_collision_checkpoint(0x000A17AFu, edi, eax);

loc_000A17AF: ;
""",
    ),
    GeneratedPatch(
        "retail camera collision result checkpoint at 0x000A2453",
        """    MEM32(edx + 8) = eax;
    PUSH32(esp, 0); sub_00098740(); /* call 0x00098740 */

loc_000A2453: ;
""",
        """    MEM32(edx + 8) = eax;
    PUSH32(esp, 0); sub_00098740(); /* call 0x00098740 */
    recomp_camera_collision_checkpoint(0x000A2453u, esi, eax);

loc_000A2453: ;
""",
    ),
)
PATCHES += (
    GeneratedPatch(
        "retail Mario64 post-collision stick checkpoint",
        """loc_0009E2A3: ;
    recomp_xmm_loadss(xmm3v, esi + 0xF8); /* movss */
""",
        """loc_0009E2A3: ;
    recomp_camera_post_collision_checkpoint(0x0009E1D5u, esi,
                                            esp + 0x1Cu, esp + 0x40u,
                                            MEM32(esp + 0x28u));
    recomp_xmm_loadss(xmm3v, esi + 0xF8); /* movss */
""",
    ),
)
PATCHES += (
    GeneratedPatch(
        "retail Mario64 repaired noisy look direction",
        """loc_0009DFE5: ;
    recomp_xmm_loadss(xmm0v, ebp + 0xC); /* movss */
""",
        """loc_0009DFE5: ;
    recomp_camera_repair_direction(esi, esp + 0x40u);
    recomp_xmm_loadss(xmm0v, ebp + 0xC); /* movss */
""",
    ),
)
PATCHES += (
    GeneratedPatch(
        "retail Mario64 pre-collision direction checkpoint",
        """loc_0009E183: ;
    ecx = MEM32(esi + 0xFC);
""",
        """loc_0009E183: ;
    recomp_camera_pre_collision_checkpoint(esi, esp + 0x10u,
                                           esp + 0x40u);
    ecx = MEM32(esi + 0xFC);
""",
    ),
)
PATCHES += (
    GeneratedPatch(
        "retail notification list iteration diagnostics",
        """    MEM32(0x64DAC8) = eax;
    ecx = MEM32(esi + 8);
""",
        """    recomp_notification_list_checkpoint(0u, esi, MEM32(esi + 8u), 0u);
    MEM32(0x64DAC8) = eax;
    ecx = MEM32(esi + 8);
""",
    ),

)
PATCHES += (
    GeneratedPatch(
        "retail RedPrimitive roadblock draw entry diagnostics",
        """loc_00216DA0: ;
    PUSH32(esp, esi);
    esi = ecx;
    eax = MEM32(esi + 0x10);
""",
        """loc_00216DA0: ;
    PUSH32(esp, esi);
    esi = ecx;
    recomp_redprimitive_draw_checkpoint(0u, esi);
    eax = MEM32(esi + 0x10);
""",
    ),
    GeneratedPatch(
        "retail RedPrimitive roadblock stream-bind diagnostics",
        """loc_00216DB5: ;
    eax = MEM32(esi);
""",
        """loc_00216DB5: ;
    recomp_redprimitive_draw_checkpoint(1u, esi);
    eax = MEM32(esi);
""",
    ),
    GeneratedPatch(
        "retail RedPrimitive roadblock indexed draw diagnostics",
        """loc_00216DBE: ;
    edx = MEM32(eax);
""",
        """loc_00216DBE: ;
    recomp_redprimitive_draw_checkpoint(2u, esi);
    edx = MEM32(eax);
""",
    ),
    GeneratedPatch(
        "retail RedPrimitive roadblock indexed return diagnostics",
        """loc_00216DCE: ;
    POP32(esp, esi);
""",
        """loc_00216DCE: ;
    recomp_redprimitive_draw_checkpoint(3u, esi);
    POP32(esp, esi);
""",
    ),
    GeneratedPatch(
        "retail RedPrimitive roadblock nonindexed draw diagnostics",
        """loc_00216DD0: ;
    eax = MEM32(esi + 8);
""",
        """loc_00216DD0: ;
    recomp_redprimitive_draw_checkpoint(4u, esi);
    eax = MEM32(esi + 8);
""",
    ),
    GeneratedPatch(
        "retail RedPrimitive roadblock draw return diagnostics",
        """loc_00216DF0: ;
    POP32(esp, esi);
""",
        """loc_00216DF0: ;
    recomp_redprimitive_draw_checkpoint(5u, esi);
    POP32(esp, esi);
""",
    ),
    GeneratedPatch(
        "retail player update lifetime entry diagnostics",
        """loc_000917C0: ;
    esp = esp - 0x10;
    PUSH32(esp, ebx);
    PUSH32(esp, ebp);
    ebp = MEM32(esp + 0x1C);
    PUSH32(esp, esi);
    PUSH32(esp, edi);
    PUSH32(esp, ebp);
    esi = ecx;
    PUSH32(esp, 0); sub_0006C420(); /* call 0x0006C420 */
""",
        """loc_000917C0: ;
    esp = esp - 0x10;
    PUSH32(esp, ebx);
    PUSH32(esp, ebp);
    ebp = MEM32(esp + 0x1C);
    PUSH32(esp, esi);
    PUSH32(esp, edi);
    PUSH32(esp, ebp);
    esi = ecx;
    recomp_player_update_lifetime_checkpoint(0u, esi);
    PUSH32(esp, 0); sub_0006C420(); /* call 0x0006C420 */
""",
    ),
    GeneratedPatch(
        "retail player update lifetime post-base diagnostics",
        """loc_000917D3: ;
    ecx = esi;
""",
        """loc_000917D3: ;
    recomp_player_update_lifetime_checkpoint(1u, esi);
    ecx = esi;
""",
    ),
    GeneratedPatch(
        "retail player update lifetime post-timing diagnostics",
        """loc_000917DA: ;
    MEMF(0x413FA8) = (float)fp_top(); fp_pop(); /* fstp */
""",
        """loc_000917DA: ;
    recomp_player_update_lifetime_checkpoint(2u, esi);
    MEMF(0x413FA8) = (float)fp_top(); fp_pop(); /* fstp */
""",
    ),
    GeneratedPatch(
        "retail player update lifetime post-event diagnostics",
        """loc_00091A27: ;
    esp = esp + 4;
""",
        """loc_00091A27: ;
    recomp_player_update_lifetime_checkpoint(3u, esi);
    esp = esp + 4;
""",
    ),
    GeneratedPatch(
        "retail player update lifetime post-state diagnostics",
        """loc_00091A32: ;
    PUSH32(esp, ebp);
""",
        """loc_00091A32: ;
    recomp_player_update_lifetime_checkpoint(4u, esi);
    PUSH32(esp, ebp);
""",
    ),
    GeneratedPatch(
        "retail player update lifetime post-action diagnostics",
        """loc_00091A3A: ;
    eax = MEM32(esi + 0x18);
""",
        """loc_00091A3A: ;
    recomp_player_update_lifetime_checkpoint(5u, esi);
    eax = MEM32(esi + 0x18);
""",
    ),
    GeneratedPatch(
        "retail player update lifetime pre-movement diagnostics",
        """loc_00091AAD: ;
    edi = MEM32(esp + 0x24);
""",
        """loc_00091AAD: ;
    recomp_player_update_lifetime_checkpoint(6u, esi);
    edi = MEM32(esp + 0x24);
""",
    ),
    GeneratedPatch(
        "retail player update lifetime post-movement diagnostics",
        """loc_00091AB9: ;
    PUSH32(esp, edi);
""",
        """loc_00091AB9: ;
    recomp_player_update_lifetime_checkpoint(7u, esi);
    PUSH32(esp, edi);
""",
    ),
    GeneratedPatch(
        "retail player update lifetime pre-actor diagnostics",
        """loc_00091AC1: ;
    ecx = MEM32(esi + 0x998);
""",
        """loc_00091AC1: ;
    recomp_player_update_lifetime_checkpoint(8u, esi);
    ecx = MEM32(esi + 0x998);
""",
    ),
    GeneratedPatch(
        "retail player action callback nonvolatile guard",
        """loc_00091A32: ;
    recomp_player_update_lifetime_checkpoint(4u, esi);
    PUSH32(esp, ebp);
    ecx = esi;
    PUSH32(esp, 0); sub_0008DEA0(); /* call 0x0008DEA0 */
""",
        """loc_00091A32: ;
    recomp_player_update_lifetime_checkpoint(4u, esi);
    { const uint32_t _player_action_saved_ebx = ebx;
    const uint32_t _player_action_saved_esi = esi;
    const uint32_t _player_action_saved_edi = edi;
    PUSH32(esp, ebp);
    ecx = esi;
    PUSH32(esp, 0); sub_0008DEA0(); /* call 0x0008DEA0 */
    recomp_update_esi_checkpoint(0x00091A32u,
                                 _player_action_saved_esi, esi);
    ebx = _player_action_saved_ebx;
    esi = _player_action_saved_esi;
    edi = _player_action_saved_edi;
    }
""",
    ),
    GeneratedPatch(
        "RedWorld allocation result preserves cmp carry",
        """    (void)0; /* cmp edx, eax - flags set for next jcc */
    MEM32(ecx) = eax;
    eax = _cf ? 0xFFFFFFFF : 0; /* sbb self (CF extend) */
""",
        """    _cf = ((uint32_t)(edx) < (uint32_t)(eax)); /* preserve cmp carry across 1 instruction(s) */
    MEM32(ecx) = eax;
    eax = _cf ? 0xFFFFFFFF : 0; /* sbb self (CF extend) */
""",
    ),
    GeneratedPatch(
        "RedWorld paired-container first-index carry",
        """    eax = ecx + eax + -16;
    (void)0; /* cmp eax, edx - flags set for next jcc */
    edi = _cf ? 0xFFFFFFFF : 0; /* sbb self (CF extend) */
    edi = edi & eax;
    goto loc_001DCC7C;
""",
        """    eax = ecx + eax + -16;
    _cf = ((uint32_t)(eax) < (uint32_t)(edx)); /* preserve cmp carry across 0 instruction(s) */
    edi = _cf ? 0xFFFFFFFF : 0; /* sbb self (CF extend) */
    edi = edi & eax;
    goto loc_001DCC7C;
""",
    ),
    GeneratedPatch(
        "RedWorld paired-container first-index carry (second helper)",
        """    eax = ecx + eax + -16;
    (void)0; /* cmp eax, edx - flags set for next jcc */
    edi = _cf ? 0xFFFFFFFF : 0; /* sbb self (CF extend) */
    edi = edi & eax;
    goto loc_001DCCEC;
""",
        """    eax = ecx + eax + -16;
    _cf = ((uint32_t)(eax) < (uint32_t)(edx)); /* preserve cmp carry across 0 instruction(s) */
    edi = _cf ? 0xFFFFFFFF : 0; /* sbb self (CF extend) */
    edi = edi & eax;
    goto loc_001DCCEC;
""",
    ),
    GeneratedPatch(
        "RedWorld paired-container second-index carry",
        """    ecx = edx + eax + -16;
    (void)0; /* cmp ecx, MEM32(esi + 8) - flags set for next jcc */
    eax = _cf ? 0xFFFFFFFF : 0; /* sbb self (CF extend) */
    eax = eax & ecx;
    goto loc_001DCC9E;
""",
        """    ecx = edx + eax + -16;
    _cf = ((uint32_t)(ecx) < (uint32_t)(MEM32(esi + 8))); /* preserve cmp carry across 0 instruction(s) */
    eax = _cf ? 0xFFFFFFFF : 0; /* sbb self (CF extend) */
    eax = eax & ecx;
    goto loc_001DCC9E;
""",
    ),
    GeneratedPatch(
        "RedWorld paired-container second-index carry (second helper)",
        """    ecx = edx + eax + -16;
    (void)0; /* cmp ecx, MEM32(esi + 8) - flags set for next jcc */
    eax = _cf ? 0xFFFFFFFF : 0; /* sbb self (CF extend) */
    eax = eax & ecx;
    goto loc_001DCD0E;
""",
        """    ecx = edx + eax + -16;
    _cf = ((uint32_t)(ecx) < (uint32_t)(MEM32(esi + 8))); /* preserve cmp carry across 0 instruction(s) */
    eax = _cf ? 0xFFFFFFFF : 0; /* sbb self (CF extend) */
    eax = eax & ecx;
    goto loc_001DCD0E;
""",
    ),
    GeneratedPatch(
        "Secondary engine critical-section predicate preserves cmp carry",
        """loc_0029CBB0: ;
    PUSH32(esp, esi);
    esi = ecx;
    eax = ZX8(MEM8(0x24));
    SET_LO8(ecx, 2);
    (void)0; /* cmp LO8(eax), LO8(ecx) - flags set for next jcc */
    eax = _cf ? 0xFFFFFFFF : 0; /* sbb self (CF extend) */
    { uint32_t _neg_value = (uint32_t)(eax);
      _cf = (_neg_value != 0);
    eax = (uint32_t)(-(int32_t)_neg_value); }
    MEM32(esi + 4) = eax;
""",
        """loc_0029CBB0: ;
    PUSH32(esp, esi);
    esi = ecx;
    eax = ZX8(MEM8(0x24));
    SET_LO8(ecx, 2);
    _cf = ((uint8_t)(LO8(eax)) < (uint8_t)(LO8(ecx))); /* preserve cmp carry across 0 instruction(s) */
    eax = _cf ? 0xFFFFFFFF : 0; /* sbb self (CF extend) */
    { uint32_t _neg_value = (uint32_t)(eax);
      _cf = (_neg_value != 0);
    eax = (uint32_t)(-(int32_t)_neg_value); }
    MEM32(esi + 4) = eax;
""",
    ),
    GeneratedPatch(
        "RsLoadSaveGame manager update checkpoint",
        """loc_001893B0: ;
    PUSH32(esp, esi);""",
        """loc_001893B0: ;
    recomp_loadsave_state_checkpoint(5u, ecx, 0u);
    PUSH32(esp, esi);""",
    ),
    GeneratedPatch(
        "RsLoadSaveGame optional callback pointer guard",
        """loc_00187494: ;
    eax = MEM32(esi + 0x14);
    if (TEST_Z(eax, eax)) goto loc_001874A6; /* je: equal / zero */""",
        """loc_00187494: ;
    eax = MEM32(esi + 0x14);
    eax = recomp_loadsave_callback_sanitize(esi, eax);
    if (TEST_Z(eax, eax)) goto loc_001874A6; /* je: equal / zero */""",
    ),
    GeneratedPatch(
        "retail player movement callback nonvolatile guard",
        """loc_00091AAD: ;
    recomp_player_update_lifetime_checkpoint(6u, esi);
    edi = MEM32(esp + 0x24);
    PUSH32(esp, edi);
    ecx = esi;
    PUSH32(esp, 0); sub_0008DCD0(); /* call 0x0008DCD0 */
""",
        """loc_00091AAD: ;
    recomp_player_update_lifetime_checkpoint(6u, esi);
    edi = MEM32(esp + 0x24);
    { const uint32_t _player_movement_saved_ebx = ebx;
    const uint32_t _player_movement_saved_esi = esi;
    const uint32_t _player_movement_saved_edi = edi;
    PUSH32(esp, edi);
    ecx = esi;
    PUSH32(esp, 0); sub_0008DCD0(); /* call 0x0008DCD0 */
    recomp_update_esi_checkpoint(0x00091AADu,
                                 _player_movement_saved_esi, esi);
    ebx = _player_movement_saved_ebx;
    esi = _player_movement_saved_esi;
    edi = _player_movement_saved_edi;
    }
""",
    ),)
PATCHES += (
    GeneratedPatch(
        "Havok symmetric linear-cast saved target declaration",
        """void sub_001A82A0(void)
{
    uint32_t ebp;
""",
        """void sub_001A82A0(void)
{
    uint32_t ebp;
    uint32_t havok_linear_target;
""",
    ),
    GeneratedPatch(
        "Havok symmetric linear-cast inner dispatch checkpoints",
        """    PUSH32(esp, edx);
    PUSH32(esp, eax);
    eax = esp + 0x1C;
    PUSH32(esp, edi);
    PUSH32(esp, eax);
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(ecx + esi * 4 + 0x318C), _icall_esp); /* indirect call */
    }

loc_001A8323: ;
""",
        """    PUSH32(esp, edx);
    PUSH32(esp, eax);
    eax = esp + 0x1C;
    PUSH32(esp, edi);
    PUSH32(esp, eax);
    havok_linear_target = MEM32(ecx + esi * 4 + 0x318C);
    recomp_havok_linear_cast_checkpoint(
        40u, MEM32(ebp + 8u), MEM32(ebp + 0xCu),
        MEM32(ebp + 0x10u), MEM32(ebp + 0x14u),
        MEM32(ebp + 0x18u), havok_linear_target);
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(havok_linear_target, _icall_esp); /* indirect call */
    }

loc_001A8323: ;
    recomp_havok_linear_cast_checkpoint(
        41u, MEM32(ebp + 8u), MEM32(ebp + 0xCu),
        MEM32(ebp + 0x10u), MEM32(ebp + 0x14u),
        MEM32(ebp + 0x18u), havok_linear_target);
""",
    ),
    GeneratedPatch(
        "Havok iterative linear-cast closest-point checkpoint",
        """loc_001B356C: ;
    SET_LO8(eax, MEM8(esp + 0x48));
    esp = esp + 0x10;
""",
        """loc_001B356C: ;
    recomp_havok_iterative_checkpoint(
        MEM32(ebp + 8u), MEM32(ebp + 0xCu), MEM32(ebp + 0x10u),
        MEM32(ebp + 0x14u), esp + 0x40u, ebx);
    SET_LO8(eax, MEM8(esp + 0x48));
    esp = esp + 0x10;
""",
    ),
    GeneratedPatch(
        "Havok SIMD ANDNPS support-point selector",
        """    /* TODO: andnps xmm3, xmm2 */
""",
        """    recomp_xmm_bitwise(xmm3v, xmm2v, 3u); /* andnps */
""",
    ),
    GeneratedPatch(
        "Havok SIMD ANDNPS box-face bias selector",
        """    /* TODO: andnps xmm4, xmm3 */
""",
        """    recomp_xmm_bitwise(xmm4v, xmm3v, 3u); /* andnps */
""",
    ),
    GeneratedPatch(
        "Havok SIMD ANDNPS box-edge bias selector",
        """    /* TODO: andnps xmm5, xmm4 */
""",
        """    recomp_xmm_bitwise(xmm5v, xmm4v, 3u); /* andnps */
""",
    ),
    GeneratedPatch(
        "Havok box-box feature-selection checkpoint",
        """loc_001B7465: ;
    if (CMP_NE(eax, 2)) goto loc_001B7504; /* jne: not equal / not zero */
""",
        """loc_001B7465: ;
    recomp_havok_box_feature_checkpoint(
        eax, esi, esp, esp + 0x90u, esp + 0x14u, esp + 0x50u);
    if (CMP_NE(eax, 2)) goto loc_001B7504; /* jne: not equal / not zero */
""",
    ),
    GeneratedPatch(
        "Havok world linear cast entry checkpoint",
        """loc_00191EF3: ;
    ecx = MEM32(ebp + 0x18);
    MEM32(esi + 0x18) = eax;
""",
        """loc_00191EF3: ;
    ecx = MEM32(ebp + 0x18);
    MEM32(esi + 0x18) = eax;
    recomp_mopp_linear_cast_checkpoint(
        10u, MEM32(ebp + 8u), eax, 0u, ebx, MEM32(esi), edi,
        MEM32(ebp + 0x1Cu), MEM32(ebp + 0x20u));
""",
    ),
    GeneratedPatch(
        "Havok world linear cast broadphase return checkpoint",
        """loc_00191FC4: ;
    POP32(esp, edi);
""",
        """loc_00191FC4: ;
    recomp_mopp_linear_cast_checkpoint(
        11u, MEM32(ebp + 8u), MEM32(esi + 0x18u), 0u,
        ebx, MEM32(esi), edi, MEM32(ebp + 0x1Cu),
        MEM32(ebp + 0x20u));
    POP32(esp, edi);
""",
    ),
    GeneratedPatch(
        "Havok world linear cast MOPP candidate checkpoint",
        """loc_0012BCC6: ;
    ebx = MEM32(esi + 0x10);
""",
        """loc_0012BCC6: ;
    recomp_mopp_linear_cast_checkpoint(
        20u, edi, MEM32(esi + 0x18u), eax,
        MEM32(esi + 0x14u),
        MEM32(MEM32(esi + 0x20u) +
              (((MEM32(esi + 0x18u) << 5u) + eax) * 4u) + 0x318Cu),
        esi + 0x20u, MEM32(esi + 0xCu), MEM32(esi + 0x10u));
    ebx = MEM32(esi + 0x10);
""",
    ),
    GeneratedPatch(
        "Havok MOPP box-cast leaf filter checkpoint",
        """loc_001BAB83: ;
    if (((uint8_t)(MEM8(eax)) == (uint8_t)(0))) goto loc_001BAC11; /* je: equal / zero */
""",
        """loc_001BAB83: ;
    {
        uint32_t _mopp_ctx = MEM32(esp + 0x28u);
        recomp_mopp_vm_hit_checkpoint(
            30u, _mopp_ctx, edi, MEM8(eax), 0u, 0u, 0u,
            MEM32(_mopp_ctx + 0x24u), MEM32(_mopp_ctx + 0x28u));
    }
    if (((uint8_t)(MEM8(eax)) == (uint8_t)(0))) goto loc_001BAC11; /* je: equal / zero */
""",
    ),
    GeneratedPatch(
        "Havok MOPP box-cast child dispatch checkpoint",
        """loc_001BABCC: ;
    ebx = MEM32(esi + 0x28);
""",
        """loc_001BABCC: ;
    {
        uint32_t _mopp_ctx = MEM32(esp + 0x28u);
        uint32_t _type_a = MEM32(_mopp_ctx + 0x18u);
        uint32_t _dispatcher = MEM32(MEM32(_mopp_ctx + 0x2Cu) + 0x30u);
        uint32_t _dispatch_table = MEM32(_dispatcher);
        uint32_t _target = MEM32(
            _dispatch_table + ((_type_a << 5u) + eax) * 4u + 0x318Cu);
        recomp_mopp_vm_hit_checkpoint(
            31u, _mopp_ctx, MEM32(esp + 0x84u), 1u,
            MEM32(esp + 0x80u), eax, _target,
            MEM32(_mopp_ctx + 0x24u), MEM32(_mopp_ctx + 0x28u));
    }
    ebx = MEM32(esi + 0x28);
""",
    ),
    GeneratedPatch(
        "Havok MOPP box-cast collector return checkpoint",
        """loc_001BABF8: ;
    edx = MEM32(esi + 0x24);
""",
        """loc_001BABF8: ;
    {
        uint32_t _mopp_ctx = esi;
        recomp_mopp_vm_hit_checkpoint(
            32u, _mopp_ctx, 0u, 2u, 0u, 0u, 0u,
            MEM32(_mopp_ctx + 0x24u), MEM32(_mopp_ctx + 0x28u));
    }
    edx = MEM32(esi + 0x24);
""",
    ),
    GeneratedPatch(
        "RedSpace collected item pointer integrity boundary",
        """loc_00224277: ;
    edx = MEM32(ebx + 4);
    edx = MEM32(edx + edi * 4);
    esi = MEM32(0x85C5A4);
    recomp_redscene_checkpoint(8u, ebx, edi, edx, eax);
    MEM32(esi + eax * 4) = edx;
""",
        """loc_00224277: ;
    edx = MEM32(ebx + 4);
    edx = MEM32(edx + edi * 4);
    esi = MEM32(0x85C5A4);
    edx = recomp_redscene_collected_item_checkpoint(ebx, edi, edx, eax);
    recomp_redscene_checkpoint(8u, ebx, edi, edx, eax);
    MEM32(esi + eax * 4) = edx;
""",
    ),
    GeneratedPatch(
        "retail actor query result pointer integrity boundary",
        """loc_0012E38A: ;
    eax = edx + edx * 4;
    eax = eax << 3;
    MEM32(eax + 0x378EF8) = ecx;
    ecx = MEM32(ecx + 0xA4);
    edx++;
    MEM32(eax + 0x378EFC) = ecx;
""",
        """loc_0012E38A: ;
    eax = edx + edx * 4;
    eax = eax << 3;
    MEM32(eax + 0x378EF8) = ecx;
    ecx = recomp_actor_query_entry_checkpoint(
        edx, ecx, MEM32(ecx + 0xA4));
    edx++;
    MEM32(eax + 0x378EFC) = ecx;
""",
    ),
    GeneratedPatch(
        "retail actor query consumer pointer integrity boundary",
        """loc_001707E0: ;
    esi = MEM32(ebp);
    if (TEST_Z(esi, esi)) goto loc_00170875; /* je: equal / zero */
""",
        """loc_001707E0: ;
    if (CMP_AE(ebp, 0x37A2FCu)) goto loc_00170887;
    esi = MEM32(ebp);
    esi = recomp_actor_query_entry_checkpoint(
        (ebp - 0x378EFCu) / 0x28u, MEM32(ebp - 4u), esi);
    if (TEST_Z(esi, esi)) goto loc_00170875; /* je: equal / zero */
""",
    ),
    GeneratedPatch(
        "retail human weapon update call nonvolatile guard",
        """loc_00059207: ;
    SET_LO8(ecx, MEM8(esi + 0x6BC));
    edx = MEM32(esp + 0x10);
    MEM8(esp + 0xB) = LO8(ecx);
    PUSH32(esp, edx);
    ecx = esi;
    PUSH32(esp, 0); sub_0004DDB0(); /* call 0x0004DDB0 */
""",
        """loc_00059207: ;
    SET_LO8(ecx, MEM8(esi + 0x6BC));
    { const uint32_t _human_update_ebx = ebx;
    const uint32_t _human_update_esi = esi;
    const uint32_t _human_update_edi = edi;
    const uint32_t _human_update_ebp = ebp;
    const uint32_t _human_update_esp = g_esp;
    edx = MEM32(esp + 0x10);
    MEM8(esp + 0xB) = LO8(ecx);
    PUSH32(esp, edx);
    ecx = esi;
    PUSH32(esp, 0); sub_0004DDB0(); /* call 0x0004DDB0 */
    recomp_nonvolatile_icall_checkpoint(
        0x0005921Du, _human_update_ebx, ebx,
        _human_update_esi, esi, _human_update_edi, edi,
        _human_update_ebp, ebp, _human_update_esp, g_esp);
    ebx = _human_update_ebx; esi = _human_update_esi;
    edi = _human_update_edi; ebp = _human_update_ebp;
    esp = _human_update_esp;
    }
""",
    ),
    GeneratedPatch(
        "retail projectile collision and damage diagnostics",
        """loc_0001FCF7: ;
    ecx = edi;
""",
        """loc_0001FCF7: ;
    recomp_bullet_hit_checkpoint(0u, esi, edi, ebx);
    ecx = edi;
""",
    ),
    GeneratedPatch(
        "retail projectile actor damage dispatch diagnostic",
        """loc_00020344: ;
    recomp_xmm_loadss(xmm0v, esi + 0x2B4); /* movss */
""",
        """loc_00020344: ;
    recomp_bullet_hit_checkpoint(1u, esi, edi, MEM32(esi + 0x1B4));
    recomp_xmm_loadss(xmm0v, esi + 0x2B4); /* movss */
""",
    ),
    GeneratedPatch(
        "retail projectile actor damage return diagnostic",
        """loc_00020414: ;
    fp_push(MEMF(0x2DC098)); /* fld float */
""",
        """loc_00020414: ;
    recomp_bullet_hit_checkpoint(2u, esi, edi, 0u);
    recomp_bullet_damage_result_checkpoint(esi, edi, (float)fp_top());
    fp_push(MEMF(0x2DC098)); /* fld float */
""",
    ),
    GeneratedPatch(
        "player firing vertical-aim diagnostic",
        """loc_000515AC: ;
    eax = MEM32(ebx + 0x6B0);
    MEMF(eax + 0x2944) = (float)fp_top(); fp_pop(); /* fstp */
""",
        """loc_000515AC: ;
    eax = MEM32(ebx + 0x6B0);
    recomp_human_fire_anim_checkpoint(
        ebx, eax, (float)fp_top(), MEM32(ebx + 0x6C4),
        MEM32(ebx + 0x6C8));
    MEMF(eax + 0x2944) = (float)fp_top(); fp_pop(); /* fstp */
""",
    ),
    GeneratedPatch(
        "player firing upper-body Play diagnostic",
        """    PUSH32(esp, eax);
    PUSH32(esp, 0); sub_00061E50(); /* call 0x00061E50 */

loc_0005162E: ;
""",
        """    { const uint32_t _human_fire_play_handle = eax;
    const uint32_t _human_fire_play_params = MEM32(esp + 4);
    recomp_human_fire_play_checkpoint(
        0u, ebx, MEM32(ebx + 0x6B0), _human_fire_play_handle,
        _human_fire_play_params, MEM32(ebx + 0x6C4),
        MEM32(ebx + 0x6C8), 0u);
    PUSH32(esp, eax);
    PUSH32(esp, 0); sub_00061E50(); /* call 0x00061E50 */
    recomp_human_fire_play_checkpoint(
        1u, ebx, MEM32(ebx + 0x6B0), _human_fire_play_handle,
        _human_fire_play_params, MEM32(ebx + 0x6C4),
        MEM32(ebx + 0x6C8), eax);
    }

loc_0005162E: ;
""",
    ),
    GeneratedPatch(
        "player weapon animation-type diagnostic",
        """loc_0004DFAD: ;
    if (TEST_Z(LO8(eax), 0x40)) goto loc_0004E476; /* je: equal / zero */
""",
        """loc_0004DFAD: ;
    recomp_human_weapon_update_checkpoint(10u, esi, edi, eax, MEM32(esi + 0x6B0));
    if (TEST_Z(LO8(eax), 0x40)) goto loc_0004E476; /* je: equal / zero */
""",
    ),
    GeneratedPatch(
        "player weapon heading diagnostic",
        """loc_0004E014: ;
    fp_top() = fabs(fp_top()); /* fabs */
""",
        """loc_0004E014: ;
    recomp_human_weapon_update_checkpoint(11u, esi, edi, MEM32(esp + 0x18), 0u);
    fp_top() = fabs(fp_top()); /* fabs */
""",
    ),
    GeneratedPatch(
        "player weapon hardpoint diagnostic",
        """loc_0004E048: ;
    if (TEST_Z(LO8(eax), LO8(eax))) goto loc_0004E476; /* je: equal / zero */
""",
        """loc_0004E048: ;
    recomp_human_weapon_update_checkpoint(12u, esi, edi, eax, 0u);
    if (TEST_Z(LO8(eax), LO8(eax))) goto loc_0004E476; /* je: equal / zero */
""",
    ),
    GeneratedPatch(
        "player weapon hardpoint direction diagnostic",
        """loc_0004E050: ;
    recomp_xmm_loadss(xmm0v, esp + 0x84); /* movss */
""",
        """loc_0004E050: ;
    recomp_human_weapon_update_checkpoint(13u, esi, edi, MEM32(esp + 0x84), MEM32(esp + 0x30));
    recomp_xmm_loadss(xmm0v, esp + 0x84); /* movss */
""",
    ),
    GeneratedPatch(
        "player weapon fire-event diagnostic",
        """loc_0004E096: ;
    SET_LO8(eax, MEM8(edi + 0x1E5));
""",
        """loc_0004E096: ;
    recomp_human_weapon_update_checkpoint(14u, esi, edi, MEM8(edi + 0x1E5), MEM8(esi + 0x6BE));
    SET_LO8(eax, MEM8(edi + 0x1E5));
""",
    ),
    GeneratedPatch(
        "player weapon use entry diagnostic",
        """loc_0004E2C3: ;
""",
        """loc_0004E2C3: ;
    recomp_human_weapon_update_checkpoint(15u, esi, edi, MEM32(esp + 0x20), MEM32(esi + 0x6C4));
""",
    ),
    GeneratedPatch(
        "player weapon use result diagnostic",
        """loc_0004E2E2: ;
    if (((uint8_t)(LO8(eax)) != (uint8_t)(1))) goto loc_0004E3FA; /* jne: not equal / not zero */
""",
        """loc_0004E2E2: ;
    recomp_human_weapon_update_checkpoint(16u, esi, edi, eax, 0u);
    if (((uint8_t)(LO8(eax)) != (uint8_t)(1))) goto loc_0004E3FA; /* jne: not equal / not zero */
""",
    ),
    GeneratedPatch(
        "player weapon success-state diagnostic",
        """loc_0004E3FA: ;
    MEM32(esi + 0x6C4) = 2;
""",
        """loc_0004E3FA: ;
    recomp_human_weapon_update_checkpoint(17u, esi, edi, eax, 0u);
    MEM32(esi + 0x6C4) = 2;
""",
    ),
    GeneratedPatch(
        "player must-aim weapon Use nonvolatile guard",
        """loc_0004E2C3: ;
    recomp_human_weapon_update_checkpoint(15u, esi, edi, MEM32(esp + 0x20), MEM32(esi + 0x6C4));
    ecx = MEM32(esp + 0x20);
    edx = MEM32(edi);
    eax = esp + 0x60;
    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, eax);
    PUSH32(esp, ecx);
    eax = esp + 0x38;
    PUSH32(esp, eax);
    ecx = esp + 0x30;
    PUSH32(esp, ecx);
    PUSH32(esp, esi);
    ecx = edi;
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(edx + 0x1C8), _icall_esp); /* indirect call */
    }
""",
        """loc_0004E2C3: ;
    recomp_human_weapon_update_checkpoint(15u, esi, edi, MEM32(esp + 0x20), MEM32(esi + 0x6C4));
    { const uint32_t _weapon_use_ebx = ebx;
    const uint32_t _weapon_use_esi = esi;
    const uint32_t _weapon_use_edi = edi;
    const uint32_t _weapon_use_ebp = ebp;
    const uint32_t _weapon_use_esp = g_esp;
    uint32_t _weapon_use_result;
    ecx = MEM32(esp + 0x20);
    edx = MEM32(edi);
    eax = esp + 0x60;
    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, eax);
    PUSH32(esp, ecx);
    eax = esp + 0x38;
    PUSH32(esp, eax);
    ecx = esp + 0x30;
    PUSH32(esp, ecx);
    PUSH32(esp, esi);
    ecx = edi;
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(edx + 0x1C8), _icall_esp); /* indirect call */
    }
    _weapon_use_result = eax;
    recomp_nonvolatile_icall_checkpoint(
        0x0004E2E2u, _weapon_use_ebx, ebx,
        _weapon_use_esi, esi, _weapon_use_edi, edi,
        _weapon_use_ebp, ebp, _weapon_use_esp, g_esp);
    ebx = _weapon_use_ebx; esi = _weapon_use_esi;
    edi = _weapon_use_edi; ebp = _weapon_use_ebp;
    esp = _weapon_use_esp; eax = _weapon_use_result;
    }
""",
    ),
    GeneratedPatch(
        "player simple weapon Use nonvolatile guard",
        """loc_0004E42E: ;
    eax = MEM32(edi);
    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, ebx);
    PUSH32(esp, ebx);
    PUSH32(esp, ebx);
    PUSH32(esp, ebx);
    PUSH32(esp, esi);
    ecx = edi;
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(eax + 0x1C8), _icall_esp); /* indirect call */
    }
""",
        """loc_0004E42E: ;
    { const uint32_t _weapon_use_ebx = ebx;
    const uint32_t _weapon_use_esi = esi;
    const uint32_t _weapon_use_edi = edi;
    const uint32_t _weapon_use_ebp = ebp;
    const uint32_t _weapon_use_esp = g_esp;
    uint32_t _weapon_use_result;
    eax = MEM32(edi);
    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, ebx);
    PUSH32(esp, ebx);
    PUSH32(esp, ebx);
    PUSH32(esp, ebx);
    PUSH32(esp, esi);
    ecx = edi;
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(eax + 0x1C8), _icall_esp); /* indirect call */
    }
    _weapon_use_result = eax;
    recomp_nonvolatile_icall_checkpoint(
        0x0004E43Du, _weapon_use_ebx, ebx,
        _weapon_use_esi, esi, _weapon_use_edi, edi,
        _weapon_use_ebp, ebp, _weapon_use_esp, g_esp);
    ebx = _weapon_use_ebx; esi = _weapon_use_esi;
    edi = _weapon_use_edi; ebp = _weapon_use_ebp;
    esp = _weapon_use_esp; eax = _weapon_use_result;
    }
""",
    ),
    GeneratedPatch(
        "weapon fire cue lookup nonvolatile guard",
        """loc_00150C73: ;
    edx = MEM32(edi);
    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, eax);
    eax = esi + 0x10;
    PUSH32(esp, eax);
    eax = esp + 0x10;
    PUSH32(esp, eax);
    ecx = edi;
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(edx + 0x34), _icall_esp); /* indirect call */
    }
""",
        """loc_00150C73: ;
    edx = MEM32(edi);
    { const uint32_t _weapon_fire_ebx = ebx;
    const uint32_t _weapon_fire_esi = esi;
    const uint32_t _weapon_fire_edi = edi;
    uint32_t _icall_esp = g_esp;
    PUSH32(esp, eax);
    eax = esi + 0x10;
    PUSH32(esp, eax);
    eax = esp + 0x10;
    PUSH32(esp, eax);
    ecx = edi;
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(edx + 0x34), _icall_esp); /* indirect call */
    recomp_nonvolatile_icall_checkpoint(
        0x00150C84u, _weapon_fire_ebx, ebx,
        _weapon_fire_esi, esi, _weapon_fire_edi, edi,
        0u, 0u, _icall_esp - 8u, g_esp);
    ebx = _weapon_fire_ebx; esi = _weapon_fire_esi;
    edi = _weapon_fire_edi; esp = _icall_esp - 8u;
    }
""",
    ),
    GeneratedPatch(
        "weapon PlayManaged call nonvolatile guard and trace",
        """loc_00150C84: ;
    ecx = MEM32(esi + 0x20);
    PUSH32(esp, eax);
    PUSH32(esp, ecx);
    PUSH32(esp, 0); sub_001FFB30(); /* call 0x001FFB30 */
""",
        """loc_00150C84: ;
    ecx = MEM32(esi + 0x20);
    recomp_weapon_fire_sound_checkpoint(
        0u, esi, MEM32(esi + 8), edi, MEM32(esp + 4), ecx, g_esp);
    { const uint32_t _weapon_play_ebx = ebx;
    const uint32_t _weapon_play_esi = esi;
    const uint32_t _weapon_play_edi = edi;
    const uint32_t _weapon_play_esp = g_esp;
    PUSH32(esp, eax);
    PUSH32(esp, ecx);
    PUSH32(esp, 0); sub_001FFB30(); /* call 0x001FFB30 */
    recomp_nonvolatile_icall_checkpoint(
        0x00150C8Eu, _weapon_play_ebx, ebx,
        _weapon_play_esi, esi, _weapon_play_edi, edi,
        0u, 0u, _weapon_play_esp - 8u, g_esp);
    ebx = _weapon_play_ebx; esi = _weapon_play_esi;
    edi = _weapon_play_edi; esp = _weapon_play_esp - 8u;
    recomp_weapon_fire_sound_checkpoint(
        1u, esi, MEM32(esi + 8), edi, eax,
        MEM32(esi + 0x20), g_esp);
    }
""",
    ),
    GeneratedPatch(
        "retail gameplay interpolation transform call nonvolatile guard",
        """    edx = MEM32(esi);
    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, eax);
    ecx = esi;
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(edx + 0x408), _icall_esp); /* indirect call */
    }

loc_0004E5EC: ;
""",
        """    edx = MEM32(esi);
    { const uint32_t _saved_ebx = ebx;
    const uint32_t _saved_esi = esi;
    const uint32_t _saved_edi = edi;
    const uint32_t _saved_ebp = ebp;
    uint32_t _icall_esp = g_esp;
    PUSH32(esp, eax);
    ecx = esi;
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(edx + 0x408), _icall_esp); /* indirect call */
    recomp_nonvolatile_icall_checkpoint(
        0x0004E5ECu, _saved_ebx, ebx, _saved_esi, esi,
        _saved_edi, edi, _saved_ebp, ebp, _icall_esp, g_esp);
    ebx = _saved_ebx; esi = _saved_esi; edi = _saved_edi;
    ebp = _saved_ebp; esp = _icall_esp;
    }

loc_0004E5EC: ;
""",
    ),
    GeneratedPatch(
        "retail gameplay interpolation factor call nonvolatile guard",
        """    edx = MEM32(esi);
    ecx = esi;
    MEM32(esp + 4) = eax;
    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(edx + 0x40C), _icall_esp); /* indirect call */
    }

loc_0004E62E: ;
""",
        """    edx = MEM32(esi);
    ecx = esi;
    MEM32(esp + 4) = eax;
    { const uint32_t _saved_ebx = ebx;
    const uint32_t _saved_esi = esi;
    const uint32_t _saved_edi = edi;
    const uint32_t _saved_ebp = ebp;
    uint32_t _icall_esp = g_esp;
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(edx + 0x40C), _icall_esp); /* indirect call */
    recomp_nonvolatile_icall_checkpoint(
        0x0004E62Eu, _saved_ebx, ebx, _saved_esi, esi,
        _saved_edi, edi, _saved_ebp, ebp, _icall_esp, g_esp);
    ebx = _saved_ebx; esi = _saved_esi; edi = _saved_edi;
    ebp = _saved_ebp; esp = _icall_esp;
    }

loc_0004E62E: ;
""",
    ),
    GeneratedPatch(
        "retail gameplay interpolation dispatch nonvolatile guard",
        """    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, 0);
    MEMF(esp + 0x10) = xmm1; /* movss */
    recomp_xmm_loadss(xmm1v, esp + 0x14); /* movss */
    PUSH32(esp, 0);
    xmm1 = xmm1 * xmm0; /* mulss */
    edx = esp + 0x14;
    PUSH32(esp, edx);
    MEMF(esp + 0x1C) = xmm1; /* movss */
    recomp_xmm_loadss(xmm1v, esp + 0x20); /* movss */
    edx = esp + 0x24;
    xmm1 = xmm1 * xmm0; /* mulss */
    PUSH32(esp, edx);
    MEMF(esp + 0x24) = xmm1; /* movss */
    eax = MEM32(ecx);
    PUSH32(esp, esi);
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(eax + 0x1C8), _icall_esp); /* indirect call */
    }

loc_0004E693: ;
""",
        """    { const uint32_t _saved_ebx = ebx;
    const uint32_t _saved_esi = esi;
    const uint32_t _saved_edi = edi;
    const uint32_t _saved_ebp = ebp;
    uint32_t _icall_esp = g_esp;
    PUSH32(esp, 0);
    MEMF(esp + 0x10) = xmm1; /* movss */
    recomp_xmm_loadss(xmm1v, esp + 0x14); /* movss */
    PUSH32(esp, 0);
    xmm1 = xmm1 * xmm0; /* mulss */
    edx = esp + 0x14;
    PUSH32(esp, edx);
    MEMF(esp + 0x1C) = xmm1; /* movss */
    recomp_xmm_loadss(xmm1v, esp + 0x20); /* movss */
    edx = esp + 0x24;
    xmm1 = xmm1 * xmm0; /* mulss */
    PUSH32(esp, edx);
    MEMF(esp + 0x24) = xmm1; /* movss */
    eax = MEM32(ecx);
    PUSH32(esp, esi);
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(eax + 0x1C8), _icall_esp); /* indirect call */
    recomp_nonvolatile_icall_checkpoint(
        0x0004E693u, _saved_ebx, ebx, _saved_esi, esi,
        _saved_edi, edi, _saved_ebp, ebp, _icall_esp, g_esp);
    ebx = _saved_ebx; esi = _saved_esi; edi = _saved_edi;
    ebp = _saved_ebp; esp = _icall_esp;
    }

loc_0004E693: ;
""",
    ),
    GeneratedPatch(
        "retail actor query type call nonvolatile guard",
        """loc_001707EB: ;
    edx = MEM32(esi);
    ecx = esi;
    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(edx + 0x28), _icall_esp); /* indirect call */
    }
""",
        """loc_001707EB: ;
    edx = MEM32(esi);
    ecx = esi;
    { const uint32_t _expected_ebx = ebx;
    const uint32_t _expected_esi = esi;
    const uint32_t _expected_edi = edi;
    const uint32_t _expected_ebp = ebp;
    uint32_t _icall_esp = g_esp;
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(edx + 0x28), _icall_esp); /* indirect call */
    recomp_nonvolatile_icall_checkpoint(
        0x001707F2u, _expected_ebx, ebx, _expected_esi, esi,
        _expected_edi, edi, _expected_ebp, ebp, _icall_esp, g_esp);
    ebx = _expected_ebx; esi = _expected_esi; edi = _expected_edi;
    ebp = _expected_ebp; esp = _icall_esp;
    }
""",
    ),
    GeneratedPatch(
        "retail actor query eligibility call nonvolatile guard",
        """loc_00170801: ;
    eax = MEM32(esi);
    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, ebx);
    ecx = esi;
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(eax + 0x15C), _icall_esp); /* indirect call */
    }
""",
        """loc_00170801: ;
    eax = MEM32(esi);
    { const uint32_t _expected_ebx = ebx;
    const uint32_t _expected_esi = esi;
    const uint32_t _expected_edi = edi;
    const uint32_t _expected_ebp = ebp;
    uint32_t _icall_esp = g_esp;
    PUSH32(esp, ebx);
    ecx = esi;
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(eax + 0x15C), _icall_esp); /* indirect call */
    recomp_nonvolatile_icall_checkpoint(
        0x0017080Cu, _expected_ebx, ebx, _expected_esi, esi,
        _expected_edi, edi, _expected_ebp, ebp, _icall_esp, g_esp);
    ebx = _expected_ebx; esi = _expected_esi; edi = _expected_edi;
    ebp = _expected_ebp; esp = _icall_esp;
    }
""",
    ),
    GeneratedPatch(
        "retail actor query owner-position call nonvolatile guard",
        """loc_00170810: ;
    edx = MEM32(ebx);
    eax = esp + 0x18;
    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, eax);
    ecx = ebx;
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(edx + 0x34), _icall_esp); /* indirect call */
    }
""",
        """loc_00170810: ;
    edx = MEM32(ebx);
    eax = esp + 0x18;
    { const uint32_t _expected_ebx = ebx;
    const uint32_t _expected_esi = esi;
    const uint32_t _expected_edi = edi;
    const uint32_t _expected_ebp = ebp;
    uint32_t _icall_esp = g_esp;
    PUSH32(esp, eax);
    ecx = ebx;
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(edx + 0x34), _icall_esp); /* indirect call */
    recomp_nonvolatile_icall_checkpoint(
        0x0017081Cu, _expected_ebx, ebx, _expected_esi, esi,
        _expected_edi, edi, _expected_ebp, ebp, _icall_esp, g_esp);
    ebx = _expected_ebx; esi = _expected_esi; edi = _expected_edi;
    ebp = _expected_ebp; esp = _icall_esp;
    }
""",
    ),
    GeneratedPatch(
        "retail actor query candidate position nonvolatile guard",
        """loc_0017081C: ;
    edx = MEM32(esi);
    edi = eax;
    eax = esp + 0x24;
    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, eax);
    ecx = esi;
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(edx + 0x34), _icall_esp); /* indirect call */
    }
""",
        """loc_0017081C: ;
    edx = MEM32(esi);
    edi = eax;
    eax = esp + 0x24;
    { const uint32_t _actor_query_candidate = esi;
    const uint32_t _actor_query_owner = ebx;
    const uint32_t _actor_query_position = edi;
    const uint32_t _actor_query_table = ebp;
    uint32_t _icall_esp = g_esp;
    PUSH32(esp, eax);
    ecx = esi;
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(edx + 0x34), _icall_esp); /* indirect call */
    recomp_nonvolatile_icall_checkpoint(
        0x0017082Au, _actor_query_owner, ebx,
        _actor_query_candidate, esi, _actor_query_position, edi,
        _actor_query_table, ebp, _icall_esp, g_esp);
    esi = _actor_query_candidate;
    ebx = _actor_query_owner;
    edi = _actor_query_position;
    ebp = _actor_query_table;
    esp = _icall_esp;
    }
""",
    ),
    GeneratedPatch(
        "retail actor query best-result pointer boundary",
        """loc_0008DEFE: ;
    ebp = eax;
    esp = esp + 8;
""",
        """loc_0008DEFE: ;
    ebp = recomp_actor_query_entry_checkpoint(0xFFFFFFFFu, 0u, eax);
    esp = esp + 8;
""",
    ),
    GeneratedPatch(
        "retail actor query returned count capacity boundary",
        """loc_001707A8: ;
    esp = esp + 0x14;
""",
        """loc_001707A8: ;
    eax = recomp_actor_query_count_checkpoint(0x001707A8u, eax);
    esp = esp + 0x14;
""",
    ),
    GeneratedPatch(
        "retail actor query consumer table capacity boundary",
        """loc_001707E0: ;
    esi = MEM32(ebp);
    esi = recomp_actor_query_entry_checkpoint(
        (ebp - 0x378EFCu) / 0x28u, MEM32(ebp - 4u), esi);
""",
        """loc_001707E0: ;
    if (CMP_AE(ebp, 0x37A2FCu)) goto loc_00170887;
    esi = MEM32(ebp);
    esi = recomp_actor_query_entry_checkpoint(
        (ebp - 0x378EFCu) / 0x28u, MEM32(ebp - 4u), esi);
""",
    ),
    GeneratedPatch(
        "retail actor query cleanup nonvolatile result guard",
        """    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, 0x12);
    eax = eax << 3;
    PUSH32(esp, eax);
    PUSH32(esp, edi);
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(edx + 0x14), _icall_esp); /* indirect call */
    }

loc_0012E3E0: ;
    eax = esi;
""",
        """    { const uint32_t _actor_query_result_count = esi;
    const uint32_t _actor_query_saved_ebx = ebx;
    const uint32_t _actor_query_saved_edi = edi;
    const uint32_t _actor_query_saved_ebp = ebp;
    uint32_t _icall_esp = g_esp;
    PUSH32(esp, 0x12);
    eax = eax << 3;
    PUSH32(esp, eax);
    PUSH32(esp, edi);
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(edx + 0x14), _icall_esp); /* indirect call */
    recomp_update_esi_checkpoint(
        0x0012E3E0u, _actor_query_result_count, esi);
    esi = _actor_query_result_count;
    ebx = _actor_query_saved_ebx;
    edi = _actor_query_saved_edi;
    ebp = _actor_query_saved_ebp;
    }

loc_0012E3E0: ;
    eax = esi;
""",
    ),
    GeneratedPatch(
        "retail Havok OBB MOPP query callback nonvolatile guard",
        """loc_00129C9F: ;
    ecx = MEM32(esp + 0x10);
    edx = MEM32(ecx);
    ecx = MEM32(esp + 0x14);
    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, ecx);
    ecx = MEM32(esp + 0x14);
    PUSH32(esp, eax);
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(edx + 4), _icall_esp); /* indirect call */
    }

loc_00129CB2: ;
""",
        """loc_00129C9F: ;
    ecx = MEM32(esp + 0x10);
    edx = MEM32(ecx);
    ecx = MEM32(esp + 0x14);
    { const uint32_t _mopp_query_saved_ebx = ebx;
    const uint32_t _mopp_query_saved_esi = esi;
    const uint32_t _mopp_query_saved_edi = edi;
    uint32_t _icall_esp = g_esp;
    PUSH32(esp, ecx);
    ecx = MEM32(esp + 0x14);
    PUSH32(esp, eax);
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(edx + 4), _icall_esp); /* indirect call */
    recomp_update_esi_checkpoint(
        0x00129CB2u, _mopp_query_saved_esi, esi);
    ebx = _mopp_query_saved_ebx;
    esi = _mopp_query_saved_esi;
    edi = _mopp_query_saved_edi;
    }

loc_00129CB2: ;
""",
    ),
    GeneratedPatch(
        "retail global notification callback nonvolatile guard",
        """    { uint32_t _icall_esp = g_esp;
    PUSH32(esp, edi);
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(eax + 4), _icall_esp); /* indirect call */
    }

loc_001FA110: ;
""",
        """    { const uint32_t _notification_esi = esi;
    const uint32_t _notification_edi = edi;
    const uint32_t _notification_ebp = ebp;
    uint32_t _icall_esp = g_esp;
    PUSH32(esp, edi);
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(eax + 4), _icall_esp); /* indirect call */
    recomp_update_esi_checkpoint(0x001FA110u, _notification_esi, esi);
    esi = _notification_esi;
    edi = _notification_edi;
    ebp = _notification_ebp;
    }

loc_001FA110: ;
""",
    ),
    GeneratedPatch(
        "retail AI action shared epilogue interior entries",
        """void sub_00087B87(void) { esp += 4; /* 0x00087B87: not detected; minimal guest ret */ }
""",
        """void sub_00087B87(void)
{
    RECOMP_TRACE_FUNC(0x00087B87u);
    ecx = esi;
    PUSH32(esp, 0); sub_00069D90();
    sub_00087B8E(); return;
}
""",
    ),
)
PATCHES += (
    GeneratedPatch(
        "retail notification callback mutation diagnostics",
        """    { const uint32_t _notification_esi = esi;
    const uint32_t _notification_edi = edi;
    const uint32_t _notification_ebp = ebp;
    uint32_t _icall_esp = g_esp;
    PUSH32(esp, edi);
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(eax + 4), _icall_esp); /* indirect call */
    recomp_update_esi_checkpoint(0x001FA110u, _notification_esi, esi);
""",
        """    { const uint32_t _notification_esi = esi;
    const uint32_t _notification_edi = edi;
    const uint32_t _notification_ebp = ebp;
    const uint32_t _notification_entry = ecx;
    const uint32_t _notification_target = MEM32(eax + 4u);
    uint32_t _icall_esp = g_esp;
    recomp_notification_list_checkpoint(
        1u, _notification_esi, _notification_entry, _notification_target);
    PUSH32(esp, edi);
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(_notification_target, _icall_esp); /* indirect call */
    if (g_esp != _icall_esp) {
        recomp_icall_stack_mismatch_trace(
            _notification_target, __FUNCTION__, __LINE__,
            _icall_esp, g_esp, g_seh_ebp);
        g_esp = _icall_esp;
    }
    recomp_notification_list_checkpoint(
        2u, _notification_esi, _notification_entry, _notification_target);
    recomp_update_esi_checkpoint(0x001FA110u, _notification_esi, esi);
""",
    ),
)
PATCHES += (
    GeneratedPatch(
        "retail notification callback cursor lifetime guard",
        """void sub_001FA0F0(void)
{
    uint32_t ebp;
    int _flags = 0; /* fallback flag var */
""",
        """void sub_001FA0F0(void)
{
    uint32_t ebp;
    uint32_t notification_next = 0u;
    int _flags = 0; /* fallback flag var */
""",
    ),
    GeneratedPatch(
        "retail notification callback snapshot next cursor",
        """loc_001FA102: ;
    recomp_notification_list_checkpoint(0u, esi, MEM32(esi + 8u), 0u);
""",
        """loc_001FA102: ;
    /* Keep a fallback link for an unexpectedly unlinked current PblThread.
     * The normal path must advance through the live post-Update link. */
    notification_next = MEM32(esi);
    recomp_notification_list_checkpoint(0u, esi, MEM32(esi + 8u), 0u);
""",
    ),
    GeneratedPatch(
        "retail PblThread callback advances validated live cursor",
        """    (void)0; /* test LO8(ecx), 1 - flags set for next jcc */
    esi = MEM32(esi);
    if (TEST_Z(LO8(ecx), 1)) goto loc_001FA14E; /* je: equal / zero */
""",
        """    (void)0; /* test LO8(ecx), 1 - flags set for next jcc */
    notification_next =
        recomp_pbl_thread_next_after_update(esi, notification_next);
    esi = notification_next;
    if (TEST_Z(LO8(ecx), 1)) goto loc_001FA14E; /* je: equal / zero */
""",
    ),
    GeneratedPatch(
        "retail notification callback validates saved successor",
        """loc_001FA14E: ;
    eax = MEM32(esi + 8);
""",
        """loc_001FA14E: ;
    esi = recomp_notification_cursor_sanitize(esi);
    if (esi == 0u) goto loc_001FA155;
    eax = MEM32(esi + 8);
""",
    ),
    GeneratedPatch(
        "retail FindCulprit empty-result diagnostics",
        """loc_000146D6: ;
    POP32(esp, edi);
    eax = 0; /* xor self */
""",
        """loc_000146D6: ;
    eax = 0; /* xor self */
    recomp_find_culprit_checkpoint(
        ebp, MEM32(esp + 0x7Cu), MEM32(esp + 0x20u), eax,
        MEM32(esp + 0x10u));
    POP32(esp, edi);
""",
    ),
    GeneratedPatch(
        "retail FindCulprit winner diagnostics",
        """loc_000146E0: ;
    eax = MEM32(esp + 0x1C);
    POP32(esp, edi);
""",
        """loc_000146E0: ;
    eax = MEM32(esp + 0x1C);
    recomp_find_culprit_checkpoint(
        ebp, MEM32(esp + 0x7Cu), MEM32(esp + 0x20u), eax,
        MEM32(esp + 0x10u));
    POP32(esp, edi);
""",
    ),
    GeneratedPatch(
        "retail vehicle reward damage attribution diagnostics",
        """    MEM8(esi + 0x124) = LO8(ecx);

loc_00014837: ;
""",
        """    MEM8(esi + 0x124) = LO8(ecx);
    recomp_vehicle_reward_checkpoint(
        0u, esi, ebp, MEM8(esi + 0x124u) & 2u, 0u, 0u);

loc_00014837: ;
""",
    ),
    GeneratedPatch(
        "retail vehicle reward kill entry diagnostics",
        """    esi = ecx;
    PUSH32(esp, 0); sub_00113700(); /* call 0x00113700 */

loc_00014BD7: ;
""",
        """    esi = ecx;
    recomp_vehicle_reward_checkpoint(
        1u, esi, 0u, MEM8(esi + 0x124u) & 2u, 0u, 0u);
    PUSH32(esp, 0); sub_00113700(); /* call 0x00113700 */

loc_00014BD7: ;
""",
    ),
    GeneratedPatch(
        "retail vehicle reward CashValue diagnostics",
        """    MEMF(esp + 0x14) = (float)fp_top(); fp_pop(); /* fstp */
    esp = esp + 4;

loc_00014C79: ;
""",
        """    MEMF(esp + 0x14) = (float)fp_top(); fp_pop(); /* fstp */
    esp = esp + 4;
    recomp_vehicle_reward_checkpoint(
        2u, esi, ebp, MEM8(esi + 0x124u) & 2u,
        MEM32(esp + 0x14u), MEM32(ebp + 0xAC4u));

loc_00014C79: ;
""",
    ),
    GeneratedPatch(
        "retail vehicle reward SetMoney diagnostics",
        """loc_00014EAE: ;
    PUSH32(esp, ecx);
""",
        """loc_00014EAE: ;
    recomp_vehicle_reward_checkpoint(
        3u, esi, ebp, MEM8(esi + 0x124u) & 2u,
        MEM32(esp + 0x14u), MEM32(ebp + 0xAC4u));
    PUSH32(esp, ecx);
""",
    ),
    GeneratedPatch(
        "retail vehicle reward post-SetMoney diagnostics",
        """loc_00014EC9: ;
    edx = MEM32(esi);
""",
        """loc_00014EC9: ;
    recomp_vehicle_reward_checkpoint(
        4u, esi, ebp, MEM8(esi + 0x124u) & 2u, 0u, 0u);
    edx = MEM32(esi);
""",
    ),
)
PATCHES += (
    GeneratedPatch(
        "retail effect update snapshots nonvolatile ESI",
        """    SET_HI8(eax, (uint32_t)_flags); /* lahf from snapshotted ucomiss */
    (void)0; /* test HI8(eax), 0x44 - flags set for next jcc */
    PUSH32(esp, esi);
    PUSH32(esp, edi);
    esi = ecx;
    if (!EVEN_PARITY8((uint8_t)(HI8(eax) & 0x44))) goto loc_00096E85; /* jnp: not parity */
""",
        """    SET_HI8(eax, (uint32_t)_flags); /* lahf from snapshotted ucomiss */
    (void)0; /* test HI8(eax), 0x44 - flags set for next jcc */
    PUSH32(esp, esi);
    PUSH32(esp, edi);
    esi = ecx;
    const uint32_t _effect_saved_esi = esi;
    if (!EVEN_PARITY8((uint8_t)(HI8(eax) & 0x44))) goto loc_00096E85; /* jnp: not parity */
""",
    ),
    GeneratedPatch(
        "retail effect tail call 00096DC9 nonvolatile ESI guard",
        """loc_00096DC9: ;
    ecx = MEM32(esi);
""",
        """loc_00096DC9: ;
    recomp_update_esi_checkpoint(0x00096DC9u, _effect_saved_esi, esi);
    esi = _effect_saved_esi;
    ecx = MEM32(esi);
""",
    ),
    GeneratedPatch(
        "retail effect tail call 00096DF3 nonvolatile ESI guard",
        """loc_00096DF3: ;
    edx = MEM32(ebp);
""",
        """loc_00096DF3: ;
    recomp_update_esi_checkpoint(0x00096DF3u, _effect_saved_esi, esi);
    esi = _effect_saved_esi;
    edx = MEM32(ebp);
""",
    ),
    GeneratedPatch(
        "retail effect tail call 00096E02 nonvolatile ESI guard",
        """loc_00096E02: ;
    ecx = ebp;
""",
        """loc_00096E02: ;
    recomp_update_esi_checkpoint(0x00096E02u, _effect_saved_esi, esi);
    esi = _effect_saved_esi;
    ecx = ebp;
""",
    ),
    GeneratedPatch(
        "retail effect tail call 00096E0D nonvolatile ESI guard",
        """loc_00096E0D: ;
    ecx = esi;
""",
        """loc_00096E0D: ;
    recomp_update_esi_checkpoint(0x00096E0Du, _effect_saved_esi, esi);
    esi = _effect_saved_esi;
    ecx = esi;
""",
    ),
    GeneratedPatch(
        "retail effect tail call 00096E18 nonvolatile ESI guard",
        """loc_00096E18: ;
    eax = MEM32(esi + 0x54);
""",
        """loc_00096E18: ;
    recomp_update_esi_checkpoint(0x00096E18u, _effect_saved_esi, esi);
    esi = _effect_saved_esi;
    eax = MEM32(esi + 0x54);
""",
    ),
    GeneratedPatch(
        "retail effect tail call 00096E4B nonvolatile ESI guard",
        """loc_00096E4B: ;
    PUSH32(esp, 0x70F7A8F7);
""",
        """loc_00096E4B: ;
    recomp_update_esi_checkpoint(0x00096E4Bu, _effect_saved_esi, esi);
    esi = _effect_saved_esi;
    PUSH32(esp, 0x70F7A8F7);
""",
    ),
)
PATCHES += (
    GeneratedPatch(
        "retail Audio Options initializes all live volume fields",
        """    ecx = esi;
    MEMF(esi + 0x88) = xmm0; /* movss */
    PUSH32(esp, 0); sub_000D9710(); /* call 0x000D9710 */
""",
        """    ecx = esi;
    MEMF(esi + 0x88) = xmm0; /* movss */
    /* MenuOptionsAudio::Update applies all three live values after any one
     * slider changes.  Retail Enter snapshots only the previous values,
     * while Init zeroes the live fields, so the first adjustment mutes the
     * two untouched channels.  Seed the live triplet from the current
     * options at the same lifecycle point as the previous-value snapshot. */
    MEM32(esi + 0x8C) = MEM32(esi + 0x80);
    MEM32(esi + 0x90) = MEM32(esi + 0x84);
    MEM32(esi + 0x94) = MEM32(esi + 0x88);
    PUSH32(esp, 0); sub_000D9710(); /* call 0x000D9710 */
""",
    ),
    GeneratedPatch(
        "retail actor animation update preserves nonvolatile registers",
        """loc_000471B4: ;
    PUSH32(esp, ebx);
    ecx = esi + 0x1204;
    PUSH32(esp, 0); sub_00168A10(); /* call 0x00168A10 */
""",
        """loc_000471B4: ;
    /* This call can enter animation callbacks through shared interior
     * targets.  The original x86 ABI preserves EBX/ESI/EDI/EBP, but those
     * callbacks share the recompiler's register bank and can leak their
     * working values back to this actor update.  Preserve the caller's
     * nonvolatile state explicitly before the next virtual dispatch. */
    { const uint32_t _actor_update_saved_ebx = ebx;
    const uint32_t _actor_update_saved_esi = esi;
    const uint32_t _actor_update_saved_edi = edi;
    const uint32_t _actor_update_saved_ebp = ebp;
    PUSH32(esp, ebx);
    ecx = esi + 0x1204;
    PUSH32(esp, 0); sub_00168A10(); /* call 0x00168A10 */
    recomp_update_esi_checkpoint(0x000471C0u,
                                 _actor_update_saved_esi, esi);
    ebx = _actor_update_saved_ebx;
    esi = _actor_update_saved_esi;
    edi = _actor_update_saved_edi;
    ebp = _actor_update_saved_ebp;
    }
""",
    ),
    GeneratedPatch(
        "retail object getter invalid-pointer checkpoint",
        """loc_0003EDC0: ;
    eax = MEM32(ecx + 0x200);
""",
        """loc_0003EDC0: ;
    recomp_object_getter_checkpoint(0x0003EDC0u, ecx, MEM32(esp));
    eax = MEM32(ecx + 0x200);
""",
    ),
    GeneratedPatch(
        "retail MenuDialog previous-menu shared target restores its frame",
        "void sub_000D92F2(void) { esp += 4; /* 0x000D92F2: not detected; minimal guest ret */ }",
        '''void sub_000D92F2(void)
{
    /* Interior target of MenuDialogBase::ProcessInput's pressed-event jump
     * table. The parent saved EBX, ESI, EDI and EBP before dispatch. */
    RECOMP_TRACE_FUNC(0x000D92F2u);
    eax = MEM32(esi + 0x64);
    if (eax != 0u) {
        ecx = MEM32(esi + 0x4C);
        PUSH32(esp, eax);
        PUSH32(esp, 0); sub_000D3E60();
    }
    sub_000D9302(); return;
}
''',
    ),
    GeneratedPatch(
        "retail MenuDialog pressed-event shared epilogue restores its frame",
        "void sub_000D9302(void) { esp += 4; /* 0x000D9302: not detected; minimal guest ret */ }",
        '''void sub_000D9302(void)
{
    /* Retail bytes 5D 5F 5E 5B C2 08 00. */
    RECOMP_TRACE_FUNC(0x000D9302u);
    POP32(esp, g_seh_ebp);
    POP32(esp, edi);
    POP32(esp, esi);
    POP32(esp, ebx);
    esp += 12; return; /* ret 8 */
}
''',
    ),
    GeneratedPatch(
        "retail MenuDialog control-sound shared target restores its frame",
        "void sub_000D950E(void) { esp += 4; /* 0x000D950E: not detected; minimal guest ret */ }",
        '''void sub_000D950E(void)
{
    /* Shared control-selection sound tail. The virtual control call leaves
     * its boolean result in AL; both branches then restore the parent frame. */
    RECOMP_TRACE_FUNC(0x000D950Eu);
    PUSH32(esp, TEST_Z(LO8(eax), LO8(eax)) ? 0xFB3B6989u : 0x8A52018Cu);
    PUSH32(esp, 0);
    PUSH32(esp, 0); sub_001FFA60();
    esp += 8;
    POP32(esp, g_seh_ebp);
    POP32(esp, edi);
    MEM32(esi + 0x58) = eax;
    POP32(esp, esi);
    POP32(esp, ebx);
    esp += 12; return; /* ret 8 */
}
''',
    ),
    GeneratedPatch(
        "retail MenuDialog non-pressed dispatcher preserves its frame",
        "void sub_000D9582(void) { esp += 4; /* 0x000D9582: not detected; minimal guest ret */ }",
        '''void sub_000D9582(void)
{
    /* MenuDialogBase dispatches button-down/released/repeat events here
     * before saving EBP. Returning from the generic stub leaked EBX, ESI,
     * EDI, the caller return address and both arguments on every event. */
    RECOMP_TRACE_FUNC(0x000D9582u);
    if (edi != 3u || ebx > edi) {
        sub_000D9303(); return;
    }
    RECOMP_ITAIL(MEM32(ebx * 4u + 0xD96C0u)); return;
}
''',
    ),
    GeneratedPatch(
        "retail load save invalid operation shared epilogue restores its frame",
        "void sub_0018D354(void) { esp += 4; /* 0x0018D354: not detected; minimal guest ret */ }",
        '''void sub_0018D354(void)
{
    /* 0018D354 is the shared epilogue reached before the valid-operation
     * dispatch pushes EBP.  RsLoadSaveGameFile::Sync has already saved ECX,
     * ESI and EDI, so the generic unresolved stub leaked twelve bytes on
     * every invalid-operation poll.  Preserve the retail bytes
     * (8B C7 5F 5E 59 C3) and leave EBP untouched. */
    RECOMP_TRACE_FUNC(0x0018D354u);
    eax = edi;
    POP32(esp, edi);
    POP32(esp, esi);
    POP32(esp, ecx);
    esp += 4;
    return;
}
''',
    ),
    GeneratedPatch(
        "retail load save pending and error shared epilogue restores its frame",
        "void sub_0018D3FE(void) { esp += 4; /* 0x0018D3FE: not detected; minimal guest ret */ }",
        '''void sub_0018D3FE(void)
{
    /* 0018D3FE is an interior branch target in the asynchronous write state.
     * Status 7 completes with failure and clears the active operation; every
     * other non-success status returns the current result (EDI, normally the
     * processing value) without clearing it.  Both paths share the parent
     * frame created at 0018D2D0.  The generic stub leaked ECX, ESI, EDI and
     * EBP on each pending poll, eventually moving ESP into kernel data. */
    RECOMP_TRACE_FUNC(0x0018D3FEu);
    if (eax == 7u) {
        edi = 3u;
        POP32(esp, g_seh_ebp);
        eax = edi;
        POP32(esp, edi);
        MEM32(esi) = 0u;
        POP32(esp, esi);
        POP32(esp, ecx);
        esp += 4;
        return;
    }
    POP32(esp, g_seh_ebp);
    eax = edi;
    POP32(esp, edi);
    POP32(esp, esi);
    POP32(esp, ecx);
    esp += 4;
    return;
}
''',
    ),
    GeneratedPatch(
        "retail release Lua callstack walker has no observable side effects",
        "/* generated PrintCallstack body is replaced as a whole */",
        '''void sub_00112930(void)
{
    /* RsLuaState::PrintCallstack only feeds debug_printf calls, which the
     * retail release XBE compiled out.  Walking Lua's internal CallInfo chain
     * therefore has no observable title-side effect.  Keep the release
     * contract directly: leave registers and Lua state untouched and perform
     * the guest RET.  This also avoids relying on compiler-private Lua 5.0
     * register conventions solely for discarded diagnostics. */
    RECOMP_TRACE_FUNC(0x00112930u);
    esp += 4;
    return;
}
''',
    ),
    GeneratedPatch(
        "retail save completion shared epilogue restores its full frame",
        "void sub_0018D435(void) { esp += 4; /* 0x0018D435: not detected; minimal guest ret */ }",
        '''void sub_0018D435(void)
{
    /* 0018D435 is an interior branch target in RsLoadSaveGameFile's async
     * close state.  It is not a callable leaf: the parent at 0018D2D0 has
     * already saved ECX, ESI, EDI and EBP.  The generic unresolved stub only
     * popped a return address, leaving that frame and the operation state in
     * place.  A completed close consequently retried forever and consumed
     * sixteen guest-stack bytes per poll.  Preserve the retail epilogue
     * (BF 02 00 00 00 5D 8B C7 5F C7 06 00 00 00 00 5E 59 C3). */
    RECOMP_TRACE_FUNC(0x0018D435u);
    edi = 2;
    POP32(esp, g_seh_ebp);
    eax = edi;
    POP32(esp, edi);
    MEM32(esi) = 0;
    POP32(esp, esi);
    POP32(esp, ecx);
    esp += 4;
    return;
}
''',
    ),
)
PATCHES += (
    GeneratedPatch(
        "retail Zephyr animation handle setter trace",
        """loc_002277BE: ;
    ecx = MEM32(edi);
    MEM32(esi + 0xB00) = ecx;
    ecx = MEM32(esp + 0x18);
""",
        """loc_002277BE: ;
    ecx = MEM32(edi);
    MEM32(esi + 0xB00) = ecx;
    /* Opt-in integrity trace derived from ZephyrAnimInst::SetAnim. */
    recomp_zephyr_anim_checkpoint(0u, esi, edi, ecx);
    ecx = MEM32(esp + 0x18);
""",
    ),
    GeneratedPatch(
        "retail Zephyr animation sampler handle trace",
        """loc_00207AFB: ;
    ecx = MEM32(esp + 0x18);
    eax = ZX16(LO16(edi));
""",
        """loc_00207AFB: ;
    ecx = MEM32(esp + 0x18);
    /* Observe the instance immediately before the joint mapping and
     * compressed-translation sampler consume it. */
    recomp_zephyr_anim_checkpoint(1u, ecx, 0u, 0u);
    eax = ZX16(LO16(edi));
""",
    ),
)
PATCHES += (
    GeneratedPatch(
        "retail target manager removal lifecycle trace",
        """loc_00153D22: ;
    MEM32(esi + 0x10) = 0;
""",
        """loc_00153D22: ;
    MEM32(esi + 0x10) = 0;
    recomp_target_manager_checkpoint(2u, 0u, esi);
""",
    ),
    GeneratedPatch(
        "retail target manager 3fe insertion lifecycle trace",
        """loc_00154334: ;
    MEM32(esi + 0x10) = edi;
    POP32(esp, edi);
""",
        """loc_00154334: ;
    MEM32(esi + 0x10) = edi;
    recomp_target_manager_checkpoint(1u, edi, esi);
    POP32(esp, edi);
""",
    ),
    GeneratedPatch(
        "retail target manager 010 insertion lifecycle trace",
        """loc_0015435C: ;
    MEM32(esi + 0x10) = edi;
    POP32(esp, edi);
""",
        """loc_0015435C: ;
    MEM32(esi + 0x10) = edi;
    recomp_target_manager_checkpoint(1u, edi, esi);
    POP32(esp, edi);
""",
    ),
    GeneratedPatch(
        "retail target manager 3ff insertion lifecycle trace",
        """loc_00154384: ;
    MEM32(esi + 0x10) = edi;
    POP32(esp, edi);
""",
        """loc_00154384: ;
    MEM32(esi + 0x10) = edi;
    recomp_target_manager_checkpoint(1u, edi, esi);
    POP32(esp, edi);
""",
    ),
    GeneratedPatch(
        "retail target manager 400 insertion lifecycle trace",
        """loc_001543B4: ;
    MEM32(esi + 0x10) = edi;

loc_001543B7: ;
""",
        """loc_001543B4: ;
    MEM32(esi + 0x10) = edi;
    recomp_target_manager_checkpoint(1u, edi, esi);

loc_001543B7: ;
""",
    ),
)
PATCHES += (
    GeneratedPatch(
        "retail callback manager validates entries before dispatch",
        """loc_0016D2F0: ;
    PUSH32(esp, 0xFFFFFFFFu);
""",
        """loc_0016D2F0: ;
    recomp_collision_manager_checkpoint(ecx);
    PUSH32(esp, 0xFFFFFFFFu);
""",
    ),
)
PATCHES += (
    GeneratedPatch(
        "retail 2f0 object allocation retires recycled callback watches",
        """loc_0003AF80: ;
    PUSH32(esp, esi);
    esi = ecx;
""",
        """loc_0003AF80: ;
    PUSH32(esp, esi);
    esi = ecx;
    recomp_collision_agent_recycle_range(esi, 0x2F0u, 0x0003AF80u);
""",
    ),
)
PATCHES += (
    GeneratedPatch(
        "retail collision-agent pointer entry trace",
        """loc_001C98D0: ;
    eax = MEM32(esp + 4);
    PUSH32(esp, esi);
""",
        """loc_001C98D0: ;
    recomp_collision_agent_checkpoint(0u, ecx, MEM32(esp + 4),
                                      MEM32(esp + 8));
    eax = MEM32(esp + 4);
    PUSH32(esp, esi);
""",
    ),
    GeneratedPatch(
        "retail collision-agent process entry trace",
        """loc_001C9180: ;
    PUSH32(esp, 0xFFFFFFFFu);
    eax = MEM32(0);
""",
        """loc_001C9180: ;
    recomp_collision_agent_checkpoint(1u, ecx, MEM32(esp + 4),
                                      MEM32(esp + 8));
    PUSH32(esp, 0xFFFFFFFFu);
    eax = MEM32(0);
""",
    ),
    GeneratedPatch(
        "retail collision-agent owner array trace",
        """    eax = MEM32(esi + 0xC4);
    ecx = MEM32(esi + 8);
    ecx = MEM32(ecx + edi * 4);
    PUSH32(esp, ebx);
""",
        """    eax = MEM32(esi + 0xC4);
    ecx = MEM32(esi + 8);
    ecx = MEM32(ecx + edi * 4);
    recomp_collision_agent_checkpoint(2u, ecx, esi, edi);
    PUSH32(esp, ebx);
""",
    ),
)
PATCHES += (
    GeneratedPatch(
        "retail collision callback setter argument trace",
        """loc_0016CBA0: ;
    PUSH32(esp, esi);
    esi = ecx;
    ecx = MEM32(esi + 0x20);
""",
        """loc_0016CBA0: ;
    recomp_collision_agent_checkpoint(10u, ecx, MEM32(esp + 4), 0u);
    PUSH32(esp, esi);
    esi = ecx;
    collision_callback_target = esi;
    ecx = MEM32(esi + 0x20);
""",
    ),
    GeneratedPatch(
        "retail collision callback setter result trace",
        """    MEM32(esi + 0x20) = ecx;
    POP32(esp, esi);
    if (TEST_Z(ecx, ecx)) goto loc_0016CBBF; /* je: equal / zero */
""",
        """    MEM32(esi + 0x20) = ecx;
    recomp_collision_agent_checkpoint(11u, esi, ecx, 0u);
    POP32(esp, esi);
    if (TEST_Z(ecx, ecx)) goto loc_0016CBBF; /* je: equal / zero */
""",
    ),
)
PATCHES += (
    GeneratedPatch(
        "retail collision callback setter declares preserved target",
        """void sub_0016CBA0(void)
{
    int _flags = 0; /* fallback flag var */
""",
        """void sub_0016CBA0(void)
{
    uint32_t collision_callback_target = 0u;
    int _flags = 0; /* fallback flag var */
""",
    ),
    GeneratedPatch(
        "retail collision callback setter preserves target across vcall",
        """loc_0016CBA0: ;
    recomp_collision_agent_checkpoint(10u, ecx, MEM32(esp + 4), 0u);
    PUSH32(esp, esi);
    esi = ecx;
    ecx = MEM32(esi + 0x20);
""",
        """loc_0016CBA0: ;
    recomp_collision_agent_checkpoint(10u, ecx, MEM32(esp + 4), 0u);
    PUSH32(esp, esi);
    esi = ecx;
    collision_callback_target = esi;
    ecx = MEM32(esi + 0x20);
""",
    ),
    GeneratedPatch(
        "retail collision callback setter restores target after vcall",
        """loc_0016CBAF: ;
    ecx = MEM32(esp + 8);
""",
        """loc_0016CBAF: ;
    /* The original x86 callback preserves ESI. Generated callbacks share the
     * register bank, so restore the setter target before writing +0x20. */
    esi = collision_callback_target;
    ecx = MEM32(esp + 8);
""",
    ),
    GeneratedPatch(
        "retail collision callback constructor watch",
        """loc_0016CB80: ;
    eax = ecx + 8;
    MEM32(ecx + 0x20) = eax;
""",
        """loc_0016CB80: ;
    eax = ecx + 8;
    MEM32(ecx + 0x20) = eax;
    recomp_collision_agent_checkpoint(9u, ecx, eax, 0u);
""",
    ),
    GeneratedPatch(
        "retail collision callback consumer declares preserved object",
        """void sub_0016D000(void)
{
    int _flags = 0; /* fallback flag var */
""",
        """void sub_0016D000(void)
{
    uint32_t collision_callback_object = 0u;
    int _flags = 0; /* fallback flag var */
""",
    ),
    GeneratedPatch(
        "retail collision callback consumer preserves object across vcall",
        """loc_0016D000: ;
    PUSH32(esp, esi);
    esi = ecx;
    eax = MEM32(esi + 0x20);
""",
        """loc_0016D000: ;
    PUSH32(esp, esi);
    esi = ecx;
    collision_callback_object = esi;
    eax = MEM32(esi + 0x20);
""",
    ),
    GeneratedPatch(
        "retail collision callback validates embedded state dispatch",
        """    collision_callback_object = esi;
    eax = MEM32(esi + 0x20);
    if (TEST_Z(eax, eax)) goto loc_0016D033; /* je: equal / zero */
""",
        """    collision_callback_object = esi;
    eax = MEM32(esi + 0x20);
    eax = recomp_collision_dispatch_state(esi, eax);
    if (TEST_Z(eax, eax)) goto loc_0016D033; /* je: equal / zero */
""",
    ),
    GeneratedPatch(
        "retail collision callback consumer restores object after vcall",
        """loc_0016D016: ;
    ecx = MEM32(esi + 0x70);
""",
        """loc_0016D016: ;
    /* The callback must preserve ESI under the retail ABI. */
    esi = collision_callback_object;
    ecx = MEM32(esi + 0x70);
""",
    ),
)
PATCHES += (
    GeneratedPatch(
        "retail collision callback setter snapshots all nonvolatiles",
        """void sub_0016CBA0(void)
{
    uint32_t collision_callback_target = 0u;
    int _flags = 0; /* fallback flag var */
""",
        """void sub_0016CBA0(void)
{
    uint32_t collision_callback_target = 0u;
    const uint32_t collision_callback_saved_ebx = ebx;
    const uint32_t collision_callback_saved_esi = esi;
    const uint32_t collision_callback_saved_edi = edi;
    const uint32_t collision_callback_saved_ebp = g_seh_ebp;
    int _flags = 0; /* fallback flag var */
""",
    ),
    GeneratedPatch(
        "retail collision callback setter restores all nonvolatiles",
        """loc_0016CBBF: ;
    esp += 8; return; /* ret 4 */
""",
        """loc_0016CBBF: ;
    /* The final AddRef is a retail tail call. Its x86 implementation must
     * preserve every nonvolatile register for our caller. Translated calls
     * share a register bank, so enforce that ABI at this wrapper boundary. */
    ebx = collision_callback_saved_ebx;
    esi = collision_callback_saved_esi;
    edi = collision_callback_saved_edi;
    g_seh_ebp = collision_callback_saved_ebp;
    esp += 8; return; /* ret 4 */
""",
    ),
    GeneratedPatch(
        "retail collision callback consumer snapshots all nonvolatiles",
        """void sub_0016D000(void)
{
    uint32_t collision_callback_object = 0u;
    int _flags = 0; /* fallback flag var */
""",
        """void sub_0016D000(void)
{
    uint32_t collision_callback_object = 0u;
    const uint32_t collision_callback_saved_ebx = ebx;
    const uint32_t collision_callback_saved_esi = esi;
    const uint32_t collision_callback_saved_edi = edi;
    const uint32_t collision_callback_saved_ebp = g_seh_ebp;
    int _flags = 0; /* fallback flag var */
""",
    ),
    GeneratedPatch(
        "retail collision callback consumer restores all nonvolatiles",
        """loc_0016D033: ;
    POP32(esp, esi);
    esp += 8; return; /* ret 4 */
""",
        """loc_0016D033: ;
    POP32(esp, esi);
    ebx = collision_callback_saved_ebx;
    esi = collision_callback_saved_esi;
    edi = collision_callback_saved_edi;
    g_seh_ebp = collision_callback_saved_ebp;
    esp += 8; return; /* ret 4 */
""",
    ),
)
PATCHES += (
    GeneratedPatch(
        "retail vehicle door closing preserves branch-local COMISS operands",
        """loc_0016D23A: ;
    if (((isnan((double)(xmm1)) || isnan((double)(MEMF(eax + 0x74)))) || (xmm1 < MEMF(eax + 0x74)))) goto loc_0016D250; /* jb: below (unsigned <) */
""",
        """loc_0016D23A: ;
    /* 0x0016D224 and 0x0016D236 reach the shared JB with different COMISS
     * operand order.  Reading the authored fields here preserves the retail
     * sign-dependent clamp instead of reusing the negative-speed expression
     * for positive-speed (left-side) doors. */
    if (MEMF(eax + 0x80) > 0.0f) {
        if (MEMF(eax + 0x74) < 0.0f) goto loc_0016D250;
    } else {
        if (MEMF(eax + 0x74) > 0.0f) goto loc_0016D250;
    }
""",
    ),
)
PATCHES += (
    GeneratedPatch(
        "retail actor reconstruction retires embedded collision watch",
        """loc_00054540: ;
    PUSH32(esp, 0xFFFFFFFFu);
    PUSH32(esp, 0x241400);
    eax = MEM32(0);
    PUSH32(esp, eax);
    MEM32(0) = esp;
    PUSH32(esp, ecx);
    PUSH32(esp, ebx);
    PUSH32(esp, esi);
    esi = ecx;
    MEM32(esp + 8) = esi;
""",
        """loc_00054540: ;
    PUSH32(esp, 0xFFFFFFFFu);
    PUSH32(esp, 0x241400);
    eax = MEM32(0);
    PUSH32(esp, eax);
    MEM32(0) = esp;
    PUSH32(esp, ecx);
    PUSH32(esp, ebx);
    PUSH32(esp, esi);
    esi = ecx;
    recomp_collision_agent_recycle_range(esi, 0x830u, 0x00054540u);
    MEM32(esp + 8) = esi;
""",
    ),
)
PATCHES += (
    GeneratedPatch(
        "retail secondary-ammo ResetTime recovered body",
        """void sub_00101AA0(void) { esp += 4; /* 0x00101AA0: not detected; minimal guest ret */ }
""",
        """void sub_00101AA0(void)
{
    uint32_t ebp;
    ebp = g_seh_ebp; /* inherit guest caller frame */
    RECOMP_TRACE_FUNC(0x00101AA0u);

loc_00101AA0: ;
    edx = MEM32(0x30E6BC);
    eax = ecx;
    ecx = MEM32(0x30E6B0);
    PUSH32(esp, ecx);
    PUSH32(esp, edx);
    ecx = eax + 0x44;
    PUSH32(esp, 0); sub_000F2950(); /* call 0x000F2950 */

loc_00101AB8: ;
    edx = MEM32(0x30E6B0);
    recomp_xmm_loadss(xmm0v, 0x2DC08C); /* movss */
    xmm0 = xmm0 / MEMF(0x30E6B0); /* divss */
    xmm0 = xmm0 + MEMF(0x30E6BC); /* addss */
    ecx = eax + 0x50;
    PUSH32(esp, edx);
    PUSH32(esp, ecx);
    MEMF(esp) = xmm0; /* movss */
    PUSH32(esp, 0); sub_000F2950(); /* call 0x000F2950 */

loc_00101AE5: ;
    MEM32(ecx + 0xC) = 0;
    MEM32(ecx + 0x10) = 0x80808080u;
    ecx = eax + 0x64;
    g_seh_ebp = ebp; sub_000F2A50(); return; /* tail jmp 0x000F2A50 */

}
""",
    ),
    GeneratedPatch(
        "retail DataPod Status shared-tail interior entries",
        """void sub_000CB444(void) { esp += 4; /* 0x000CB444: not detected; minimal guest ret */ }
void sub_000CB449(void) { esp += 4; /* 0x000CB449: not detected; minimal guest ret */ }
void sub_000CB451(void) { esp += 4; /* 0x000CB451: not detected; minimal guest ret */ }
void sub_000CB470(void) { esp += 4; /* 0x000CB470: not detected; minimal guest ret */ }
""",
        """void sub_000CB444(void)
{
    RECOMP_TRACE_FUNC(0x000CB444u);
    eax = esp + 0x10;
    PUSH32(esp, eax);
    sub_000CB449(); return;
}

void sub_000CB449(void)
{
    RECOMP_TRACE_FUNC(0x000CB449u);
    PUSH32(esp, 0); sub_002370B8();
    esp = esp + 8;
    sub_000CB451(); return;
}

void sub_000CB451(void)
{
    RECOMP_TRACE_FUNC(0x000CB451u);
    ecx = esp + 0xC;
    PUSH32(esp, ecx);
    PUSH32(esp, 0); sub_001F29F0();
    edx = MEM32(esi + 0x3B8);
    esp = esp + 4;
    PUSH32(esp, edx);
    PUSH32(esp, eax);
    ecx = 0x414150;
    PUSH32(esp, 0); sub_00121DA0();
    sub_000CB470(); return;
}

void sub_000CB470(void)
{
    RECOMP_TRACE_FUNC(0x000CB470u);
    recomp_xmm_loadss(xmm0v, esi + 0x3C4);
    recomp_xmm_zero(xmm1v);
    if (!((isnan((double)xmm0) || isnan((double)xmm1)) || xmm0 <= xmm1)) {
        SET_LO8(eax, MEM8(esi + 0x3B5));
        if (TEST_NZ(LO8(eax), LO8(eax))) {
            eax = MEM32(esi + 0x24);
            MEM8(eax + 0xE360) = 0;
            recomp_xmm_loadss(xmm0v, esi + 0x3C4);
            xmm0 = xmm0 - MEMF(esp + 0x70);
            MEM8(esi + 0x3C0) = 1;
            MEMF(esi + 0x3C4) = xmm0;
            if (!((isnan((double)xmm1) || isnan((double)xmm0)) || xmm1 < xmm0)) {
                ecx = MEM32(esi + 0x24);
                MEM8(esi + 0x3C0) = 0;
                MEM8(ecx + 0xE360) = 1;
            }
        }
    }
    edx = MEM32(esp + 0x70);
    ecx = MEM32(esi + 0x24);
    PUSH32(esp, edx);
    PUSH32(esp, 0); sub_000C9700();
    POP32(esp, edi);
    POP32(esp, esi);
    esp = esp + 0x64;
    esp += 8; return; /* ret 4 */
}
""",
    ),
    GeneratedPatch(
        "retail DataPod flash timer diagnostic",
        """    xmm1 = xmm1 + xmm0; /* addss */
    MEMF(esi + 0xDB3C) = xmm1; /* movss */
    if (((uint8_t)(MEM8(esi + 0xE36C)) != (uint8_t)(1))) goto loc_000CE84D; /* jne: not equal / not zero */
""",
        """    xmm1 = xmm1 + xmm0; /* addss */
    MEMF(esi + 0xDB3C) = xmm1; /* movss */
    recomp_datapod_timer_checkpoint(esi, xmm0, xmm1);
    if (((uint8_t)(MEM8(esi + 0xE36C)) != (uint8_t)(1))) goto loc_000CE84D; /* jne: not equal / not zero */
""",
    ),
    GeneratedPatch(
        "retail secondary-ammo update diagnostic",
        """loc_00105141: ;
    ecx = MEM32(esp + 0x14);
""",
        """loc_00105141: ;
    recomp_secondary_ammo_checkpoint(0u, esi, 0u);
    ecx = MEM32(esp + 0x14);
""",
    ),
    GeneratedPatch(
        "retail secondary-ammo paint diagnostic",
        """loc_0010560E: ;
    ecx = MEM32(esi + 0x2C);
""",
        """loc_0010560E: ;
    recomp_secondary_ammo_checkpoint(1u, esi, 0u);
    ecx = MEM32(esi + 0x2C);
""",
    ),
    GeneratedPatch(
        "retail secondary-ammo computed text color diagnostic",
        """loc_001055C9: ;
    PUSH32(esp, eax);
""",
        """loc_001055C9: ;
    recomp_secondary_ammo_checkpoint(2u, esi, eax);
    PUSH32(esp, eax);
""",
    ),)
PATCHES += (
    GeneratedPatch(
        "retail human move selection forced-walk diagnostic",
        """loc_0013E086: ;
    MEMF(esp + 0x3C) = xmm0; /* movss */

loc_0013E08C: ;
""",
        """loc_0013E086: ;
    MEMF(esp + 0x3C) = xmm0; /* movss */
    recomp_human_move_selection_checkpoint(esi, xmm1);

loc_0013E08C: ;
""",
    ),
)
PATCHES += (
    GeneratedPatch(
        "retail human jump request diagnostic",
        """loc_00053E70: ;
    PUSH32(esp, 0); sub_0013C470(); /* call 0x0013C470 */
""",
        """loc_00053E70: ;
    recomp_human_jump_checkpoint(0u, esi, eax, 0u);
    PUSH32(esp, 0); sub_0013C470(); /* call 0x0013C470 */
""",
    ),
    GeneratedPatch(
        "retail human jump result diagnostic",
        """loc_00053E80: ;
    POP32(esp, edi);
""",
        """loc_00053E80: ;
    recomp_human_jump_checkpoint(1u, esi, 0u, eax);
    POP32(esp, edi);
""",
    ),
)
PATCHES += (
    GeneratedPatch(
        "retail rider seat query snapshots nonvolatile registers",
        """void sub_001624B0(void)
{
    int _flags = 0; /* fallback flag var */
""",
        """void sub_001624B0(void)
{
    /* The nested type query and virtual predicate are ordinary x86 calls.
     * Their translated implementations share the guest register bank, so
     * enforce the retail callee-save contract at this public boundary. */
    const uint32_t rider_query_saved_ebx = ebx;
    const uint32_t rider_query_saved_esi = esi;
    const uint32_t rider_query_saved_edi = edi;
    const uint32_t rider_query_saved_ebp = g_seh_ebp;
    int _flags = 0; /* fallback flag var */
""",
    ),
    GeneratedPatch(
        "retail rider seat query restores nonvolatile registers on success",
        """loc_001624DF: ;
    eax = esi;
    POP32(esp, esi);
    esp += 4; return; /* ret */
""",
        """loc_001624DF: ;
    eax = esi;
    POP32(esp, esi);
    ebx = rider_query_saved_ebx;
    esi = rider_query_saved_esi;
    edi = rider_query_saved_edi;
    g_seh_ebp = rider_query_saved_ebp;
    esp += 4; return; /* ret */
""",
    ),
    GeneratedPatch(
        "retail rider seat query restores nonvolatile registers on failure",
        """loc_001624E3: ;
    eax = 0; /* xor self */
    POP32(esp, esi);
    esp += 4; return; /* ret */
""",
        """loc_001624E3: ;
    eax = 0; /* xor self */
    POP32(esp, esi);
    ebx = rider_query_saved_ebx;
    esi = rider_query_saved_esi;
    edi = rider_query_saved_edi;
    g_seh_ebp = rider_query_saved_ebp;
    esp += 4; return; /* ret */
""",
    ),
)
for _run_state_site, _run_state_next, _run_state_before, _run_state_pre, _run_state_post, _run_state_vtable_reg in (
    (
        "00055AC3", "00055AD3",
        "    edx = MEM32(esi);\n    SET_LO8(ebx, MEM8(esi + 0x817));\n    ecx = esi;",
        "    edx = MEM32(esi);\n    SET_LO8(ebx, MEM8(esi + 0x817));",
        "    ecx = esi;", "edx",
    ),
    (
        "00055AFB", "00055B05",
        "    eax = MEM32(esi);\n    ecx = esi;", "",
        "    eax = MEM32(esi);\n    ecx = esi;", "eax",
    ),
    ("00055B0E", "00055B16", "    edx = MEM32(esi);", "", "    edx = MEM32(esi);", "edx"),
    ("00055B21", "00055B29", "    eax = MEM32(esi);", "", "    eax = MEM32(esi);", "eax"),
    ("00055B43", "00055B4B", "    edx = MEM32(esi);", "", "    edx = MEM32(esi);", "edx"),
):
    PATCHES += (
        GeneratedPatch(
            f"retail human submersion vcall {_run_state_site} preserves run state",
            f"""loc_{_run_state_site}: ;
{_run_state_before}
    {{ uint32_t _icall_esp = g_esp;
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32({_run_state_vtable_reg} + 0x3BC), _icall_esp); /* indirect call */
    }}

loc_{_run_state_next}: ;
""",
            f"""loc_{_run_state_site}: ;
    /* _UpdateSubmergedState keeps the actor and authored run permission in
     * nonvolatile registers across this normal x86 virtual call. Translated
     * callees share the guest register bank, so preserve them explicitly. */
{_run_state_pre + chr(10) if _run_state_pre else ""}    {{ const uint32_t human_run_saved_ebx = ebx;
    const uint32_t human_run_saved_esi = esi;
    const uint32_t human_run_saved_edi = edi;
    const uint32_t human_run_saved_ebp = g_seh_ebp;
{_run_state_post}
    {{ uint32_t _icall_esp = g_esp;
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32({_run_state_vtable_reg} + 0x3BC), _icall_esp); /* indirect call */
    }}
    ebx = human_run_saved_ebx;
    esi = human_run_saved_esi;
    edi = human_run_saved_edi;
    g_seh_ebp = human_run_saved_ebp;
    }}

loc_{_run_state_next}: ;
""",
        ),
    )
for _gather_site, _gather_vtable_reg, _gather_next in (
    ("001066CD", "eax", "001066D7"),
    ("001066FC", "edx", "00106706"),
    ("0010672B", "eax", "00106735"),
    ("0010675A", "edx", "00106764"),
    ("00106789", "eax", "00106793"),
):
    PATCHES += (
        GeneratedPatch(
            f"retail actor gather vcall {_gather_site} preserves loop registers",
            f"""loc_{_gather_site}: ;
    {_gather_vtable_reg} = MEM32(edi);
    ecx = edi;
    {{ uint32_t _icall_esp = g_esp;
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32({_gather_vtable_reg} + 0xBC), _icall_esp); /* indirect call */
    }}

loc_{_gather_next}: ;
""",
            f"""loc_{_gather_site}: ;
    /* The predicate is a normal x86 virtual call. Preserve this gather
     * loop's node, count, current object, and output pointer explicitly
     * because translated callees share the guest register bank. */
    {{ const uint32_t actor_gather_saved_ebx = ebx;
    const uint32_t actor_gather_saved_esi = esi;
    const uint32_t actor_gather_saved_edi = edi;
    const uint32_t actor_gather_saved_ebp = ebp;
    {_gather_vtable_reg} = MEM32(edi);
    ecx = edi;
    {{ uint32_t _icall_esp = g_esp;
    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32({_gather_vtable_reg} + 0xBC), _icall_esp); /* indirect call */
    }}
    ebx = actor_gather_saved_ebx;
    esi = actor_gather_saved_esi;
    edi = actor_gather_saved_edi;
    ebp = actor_gather_saved_ebp;
    }}

loc_{_gather_next}: ;
""",
        ),
    )
PATCHES += (
    GeneratedPatch(
        "retail rider seat update snapshots caller nonvolatiles",
        """void sub_001683D0(void)
{
    uint32_t ebp;
    int _flags = 0; /* fallback flag var */
""",
        """void sub_001683D0(void)
{
    const uint32_t rider_update_saved_ebx = ebx;
    const uint32_t rider_update_saved_esi = esi;
    const uint32_t rider_update_saved_edi = edi;
    const uint32_t rider_update_saved_ebp = g_seh_ebp;
    uint32_t ebp;
    int _flags = 0; /* fallback flag var */
""",
    ),
    GeneratedPatch(
        "retail rider seat update restores caller nonvolatiles",
        """loc_001688A0: ;
    POP32(esp, esi);
    POP32(esp, ebx);
    esp += 8; return; /* ret 4 */
""",
        """loc_001688A0: ;
    POP32(esp, esi);
    POP32(esp, ebx);
    ebx = rider_update_saved_ebx;
    esi = rider_update_saved_esi;
    edi = rider_update_saved_edi;
    g_seh_ebp = rider_update_saved_ebp;
    esp += 8; return; /* ret 4 */
""",
    ),
)
OPTIONAL_PATCHES = {
    # A pristine lift has neither diagnostic declaration.  The following
    # declaration patch adds both in one step; this narrower migration exists
    # only for an older partially patched tree which already preserved EBX.
    "Lua native-call add precall ESP declaration migration",
    # Likewise, pristine generated C is handled by the full entry-checkpoint
    # patch below; this migration only upgrades an intermediate diagnostic tree.
    "Lua precall target add saved ESP migration",
    # The pristine lift still contains the direct interior-target call and is
    # handled by the complete retail yield-tail replacement.
    "luaV_execute incomplete yield tail migration",
    # The current lifter emits these x87 operations directly. Keep the old
    # anchors for older known-good generated snapshots.
    "MSVC x87 FRNDINT semantics",
    "CRT logarithm fyl2x implementation",
    "CRT exponent x87 operations",
    # The corrected vtable scanner no longer promotes memmove's embedded
    # jump-table cases to standalone functions. Interior RECOMP_ITAIL targets
    # are completed by mercenaries_memmove_finish in recomp_manual.c.
    "CRT memmove backward MOVSD path 238E3C",
    "CRT memmove backward MOVSD path 238E6E",
    # Diagnostic-only allocation trace; its label disappeared after retail
    # function discovery converged with the coalesced Havok range.
    "XACT sound activation event-build diagnostics",
    # Superseded by the translator-wide entry-prologue clamp. Retain the old
    # generated anchors so historical snapshots can still be patched safely.
    "Havok wheel constraint keeps SEH prologue outside ICALL cleanup",
    "Havok rigid-body motion setter keeps aligned SEH prologue outside ICALL cleanup",
    "PblConfig line-reader position ICALL restore point",
    # The player action guard composes the stage-4 diagnostic body.
    "retail player update lifetime post-state diagnostics",
    # The later notification mutation patch composes this guard's body.
    "retail global notification callback nonvolatile guard",
    # These declaration transforms are intentionally superseded by the
    # full nonvolatile snapshots below. Keep them for pristine lifted trees,
    # while allowing a second patch pass to recognize the composed result.
    "retail collision callback setter declares preserved target",
    "retail collision callback consumer declares preserved object",
}

FUNCTION_REPLACEMENT_PATCHES = {
    "retail release Lua callstack walker has no observable side effects":
        "sub_00112930",
    "retail save completion shared epilogue restores its full frame":
        "sub_0018D435",
    "retail XInputGetState host bridge": "sub_002D54AC",
    "retail XInputOpen host bridge": "sub_002D5272",
    "retail XInputClose host bridge": "sub_002D52C8",
    "retail XInputGetCapabilities host bridge": "sub_002D52D4",
    "retail XInputSetState host bridge": "sub_002D551F",
    "retail XGetDevices host bridge": "sub_002D5579",
    "retail XGetDeviceChanges host bridge": "sub_002D559B",
    "retail XInitDevices host bridge and stdcall cleanup": "sub_002D5574",
}

# Only SetupGPSTopDownScope's center panel, surrounding fill boundaries,
# grid exclusion mask and four corner brackets. Leave grid spacing, crosshair,
# distance boxes, progress bar, outer frame and text at their retail coordinates.
SATELLITE_CENTER_PATCHES = (
    GeneratedPatch(
        'satellite center proportions 000FA6C1',
        """loc_000FA6C1: ;
    PUSH32(esp, 0x3F800000);
    PUSH32(esp, 0x3F800000);
    PUSH32(esp, 0);
    PUSH32(esp, 0);
    PUSH32(esp, 0x43120000);
    PUSH32(esp, 0x431F8000);
    PUSH32(esp, 0x43370000);
    PUSH32(esp, 0);
    ecx = esi;
    PUSH32(esp, 0); sub_0020C980(); /* call 0x0020C980 */
""",
        """loc_000FA6C1: ;
    PUSH32(esp, 0x3F800000);
    PUSH32(esp, 0x3F800000);
    PUSH32(esp, 0);
    PUSH32(esp, 0);
    PUSH32(esp, 0x43120000);
    PUSH32(esp, 0x431F8000);
    PUSH32(esp, 0x43370000);
    PUSH32(esp, 0);
    ecx = esi;
    MEMF(esp + 8u) = recomp_options_satellite_center_x(MEMF(esp + 8u));
    PUSH32(esp, 0); sub_0020C980(); /* call 0x0020C980 */
""",
    ),
    GeneratedPatch(
        'satellite center proportions 000FA6E7',
        """loc_000FA6E7: ;
    PUSH32(esp, 0x3F800000);
    PUSH32(esp, 0x3F800000);
    PUSH32(esp, 0);
    PUSH32(esp, 0);
    PUSH32(esp, 0x43120000);
    PUSH32(esp, 0x431F8000);
    PUSH32(esp, 0x43370000);
    PUSH32(esp, 0x43B04000);
    ecx = esi;
    PUSH32(esp, 0); sub_0020C980(); /* call 0x0020C980 */
""",
        """loc_000FA6E7: ;
    PUSH32(esp, 0x3F800000);
    PUSH32(esp, 0x3F800000);
    PUSH32(esp, 0);
    PUSH32(esp, 0);
    PUSH32(esp, 0x43120000);
    PUSH32(esp, 0x431F8000);
    PUSH32(esp, 0x43370000);
    PUSH32(esp, 0x43B04000);
    ecx = esi;
    MEMF(esp) = recomp_options_satellite_center_x(MEMF(esp));
    MEMF(esp + 8u) = 512.0f - MEMF(esp);
    PUSH32(esp, 0); sub_0020C980(); /* call 0x0020C980 */
""",
    ),
    GeneratedPatch(
        'satellite center proportions 000FA76F',
        """loc_000FA76F: ;
    PUSH32(esp, 0x3F800000);
    PUSH32(esp, 0x3F800000);
    PUSH32(esp, 0);
    PUSH32(esp, 0);
    PUSH32(esp, 0x43120000);
    PUSH32(esp, 0x43410000);
    PUSH32(esp, 0x43370000);
    PUSH32(esp, 0x431F8000);
    ecx = esi;
    PUSH32(esp, 0); sub_0020C980(); /* call 0x0020C980 */
""",
        """loc_000FA76F: ;
    PUSH32(esp, 0x3F800000);
    PUSH32(esp, 0x3F800000);
    PUSH32(esp, 0);
    PUSH32(esp, 0);
    PUSH32(esp, 0x43120000);
    PUSH32(esp, 0x43410000);
    PUSH32(esp, 0x43370000);
    PUSH32(esp, 0x431F8000);
    ecx = esi;
    MEMF(esp) = recomp_options_satellite_center_x(MEMF(esp));
    MEMF(esp + 8u) = recomp_options_satellite_center_width(MEMF(esp + 8u));
    PUSH32(esp, 0); sub_0020C980(); /* call 0x0020C980 */
""",
    ),
    GeneratedPatch(
        'satellite center proportions 000FA736',
        """loc_000FA736: ;
    esp = esp + 0x14;
    PUSH32(esp, eax);
    ecx = esi;
    PUSH32(esp, 0); sub_000FA110(); /* call 0x000FA110 */
""",
        """loc_000FA736: ;
    esp = esp + 0x14;
    PUSH32(esp, eax);
    ecx = esi;
    MEMF(esp + 8u) = recomp_options_satellite_center_width(MEMF(esp + 8u));
    PUSH32(esp, 0); sub_000FA110(); /* call 0x000FA110 */
""",
    ),
    GeneratedPatch(
        'satellite center proportions 000FA8BD',
        """loc_000FA8BD: ;
    PUSH32(esp, 0x433D0000);
    PUSH32(esp, 0x431B8000);
    ecx = esi;
    PUSH32(esp, 0); sub_0020C160(); /* call 0x0020C160 */
""",
        """loc_000FA8BD: ;
    PUSH32(esp, 0x433D0000);
    PUSH32(esp, 0x431B8000);
    ecx = esi;
    MEMF(esp) = recomp_options_satellite_center_x(MEMF(esp));
    PUSH32(esp, 0); sub_0020C160(); /* call 0x0020C160 */
""",
    ),
    GeneratedPatch(
        'satellite center proportions 000FA8CE',
        """loc_000FA8CE: ;
    PUSH32(esp, 0x43330000);
    PUSH32(esp, 0x431B8000);
    ecx = esi;
    PUSH32(esp, 0); sub_0020C160(); /* call 0x0020C160 */
""",
        """loc_000FA8CE: ;
    PUSH32(esp, 0x43330000);
    PUSH32(esp, 0x431B8000);
    ecx = esi;
    MEMF(esp) = recomp_options_satellite_center_x(MEMF(esp));
    PUSH32(esp, 0); sub_0020C160(); /* call 0x0020C160 */
""",
    ),
    GeneratedPatch(
        'satellite center proportions 000FA8DF',
        """loc_000FA8DF: ;
    PUSH32(esp, 0x43330000);
    PUSH32(esp, 0x43258000);
    ecx = esi;
    PUSH32(esp, 0); sub_0020C160(); /* call 0x0020C160 */
""",
        """loc_000FA8DF: ;
    PUSH32(esp, 0x43330000);
    PUSH32(esp, 0x43258000);
    ecx = esi;
    MEMF(esp) = recomp_options_satellite_center_x(MEMF(esp));
    PUSH32(esp, 0); sub_0020C160(); /* call 0x0020C160 */
""",
    ),
    GeneratedPatch(
        'satellite center proportions 000FA904',
        """loc_000FA904: ;
    PUSH32(esp, 0x433D0000);
    PUSH32(esp, 0x43B24000);
    ecx = esi;
    PUSH32(esp, 0); sub_0020C160(); /* call 0x0020C160 */
""",
        """loc_000FA904: ;
    PUSH32(esp, 0x433D0000);
    PUSH32(esp, 0x43B24000);
    ecx = esi;
    MEMF(esp) = recomp_options_satellite_center_x(MEMF(esp));
    PUSH32(esp, 0); sub_0020C160(); /* call 0x0020C160 */
""",
    ),
    GeneratedPatch(
        'satellite center proportions 000FA915',
        """loc_000FA915: ;
    PUSH32(esp, 0x43330000);
    PUSH32(esp, 0x43B24000);
    ecx = esi;
    PUSH32(esp, 0); sub_0020C160(); /* call 0x0020C160 */
""",
        """loc_000FA915: ;
    PUSH32(esp, 0x43330000);
    PUSH32(esp, 0x43B24000);
    ecx = esi;
    MEMF(esp) = recomp_options_satellite_center_x(MEMF(esp));
    PUSH32(esp, 0); sub_0020C160(); /* call 0x0020C160 */
""",
    ),
    GeneratedPatch(
        'satellite center proportions 000FA926',
        """loc_000FA926: ;
    PUSH32(esp, 0x43330000);
    PUSH32(esp, 0x43AD4000);
    ecx = esi;
    PUSH32(esp, 0); sub_0020C160(); /* call 0x0020C160 */
""",
        """loc_000FA926: ;
    PUSH32(esp, 0x43330000);
    PUSH32(esp, 0x43AD4000);
    ecx = esi;
    MEMF(esp) = recomp_options_satellite_center_x(MEMF(esp));
    PUSH32(esp, 0); sub_0020C160(); /* call 0x0020C160 */
""",
    ),
    GeneratedPatch(
        'satellite center proportions 000FA94B',
        """loc_000FA94B: ;
    PUSH32(esp, 0x43A18000);
    PUSH32(esp, 0x431B8000);
    ecx = esi;
    PUSH32(esp, 0); sub_0020C160(); /* call 0x0020C160 */
""",
        """loc_000FA94B: ;
    PUSH32(esp, 0x43A18000);
    PUSH32(esp, 0x431B8000);
    ecx = esi;
    MEMF(esp) = recomp_options_satellite_center_x(MEMF(esp));
    PUSH32(esp, 0); sub_0020C160(); /* call 0x0020C160 */
""",
    ),
    GeneratedPatch(
        'satellite center proportions 000FA95C',
        """loc_000FA95C: ;
    PUSH32(esp, 0x43A68000);
    PUSH32(esp, 0x431B8000);
    ecx = esi;
    PUSH32(esp, 0); sub_0020C160(); /* call 0x0020C160 */
""",
        """loc_000FA95C: ;
    PUSH32(esp, 0x43A68000);
    PUSH32(esp, 0x431B8000);
    ecx = esi;
    MEMF(esp) = recomp_options_satellite_center_x(MEMF(esp));
    PUSH32(esp, 0); sub_0020C160(); /* call 0x0020C160 */
""",
    ),
    GeneratedPatch(
        'satellite center proportions 000FA96D',
        """loc_000FA96D: ;
    PUSH32(esp, 0x43A68000);
    PUSH32(esp, 0x43258000);
    ecx = esi;
    PUSH32(esp, 0); sub_0020C160(); /* call 0x0020C160 */
""",
        """loc_000FA96D: ;
    PUSH32(esp, 0x43A68000);
    PUSH32(esp, 0x43258000);
    ecx = esi;
    MEMF(esp) = recomp_options_satellite_center_x(MEMF(esp));
    PUSH32(esp, 0); sub_0020C160(); /* call 0x0020C160 */
""",
    ),
    GeneratedPatch(
        'satellite center proportions 000FA992',
        """loc_000FA992: ;
    PUSH32(esp, 0x43A18000);
    PUSH32(esp, 0x43B24000);
    ecx = esi;
    PUSH32(esp, 0); sub_0020C160(); /* call 0x0020C160 */
""",
        """loc_000FA992: ;
    PUSH32(esp, 0x43A18000);
    PUSH32(esp, 0x43B24000);
    ecx = esi;
    MEMF(esp) = recomp_options_satellite_center_x(MEMF(esp));
    PUSH32(esp, 0); sub_0020C160(); /* call 0x0020C160 */
""",
    ),
    GeneratedPatch(
        'satellite center proportions 000FA9A3',
        """loc_000FA9A3: ;
    PUSH32(esp, 0x43A68000);
    PUSH32(esp, 0x43B24000);
    ecx = esi;
    PUSH32(esp, 0); sub_0020C160(); /* call 0x0020C160 */
""",
        """loc_000FA9A3: ;
    PUSH32(esp, 0x43A68000);
    PUSH32(esp, 0x43B24000);
    ecx = esi;
    MEMF(esp) = recomp_options_satellite_center_x(MEMF(esp));
    PUSH32(esp, 0); sub_0020C160(); /* call 0x0020C160 */
""",
    ),
    GeneratedPatch(
        'satellite center proportions 000FA9B4',
        """loc_000FA9B4: ;
    PUSH32(esp, 0x43A68000);
    PUSH32(esp, 0x43AD4000);
    ecx = esi;
    PUSH32(esp, 0); sub_0020C160(); /* call 0x0020C160 */
""",
        """loc_000FA9B4: ;
    PUSH32(esp, 0x43A68000);
    PUSH32(esp, 0x43AD4000);
    ecx = esi;
    MEMF(esp) = recomp_options_satellite_center_x(MEMF(esp));
    PUSH32(esp, 0); sub_0020C160(); /* call 0x0020C160 */
""",
    ),
)
PATCHES += SATELLITE_CENTER_PATCHES

# The retail release strips ASSERTF after CreateDevice. A fragmented guest heap
# can therefore leave a null device and reach D3DSwap's buffer-chain walk. Retry
# the renderer's existing non-AA configuration only after allocation failure;
# its later buffer/resolve setup reads iUsedAntialiasType at 0x30F270.
DEVICE_CREATION_RECOVERY_PATCHES = (
    GeneratedPatch(
        "Opt-in renderer allocation-failure regression injection",
        """    PUSH32(esp, 0); sub_0028CAA0(); /* call 0x0028CAA0 */

loc_0020FC02: ;""",
        """    if (recomp_renderer_test_oom(MEM32(MEM32(esp + 0x10) + 0x10))) {
        /* Test only: fail before the retail CreateDevice owns any resources. */
        esp += 24u;
        eax = 0x8007000Eu;
        MEM32(0x7AD604) = 0u;
    } else {
        PUSH32(esp, 0); sub_0028CAA0(); /* call 0x0028CAA0 */
    }

loc_0020FC02: ;""",
    ),
    GeneratedPatch(
        "Recover renderer creation after guest allocation failure",
        """loc_0020FC02: ;
    ecx = MEM32(0x7AD604);""",
        """loc_0020FC02: ;
    if ((eax == 0x8007000Eu || eax == 0x8876017Cu) &&
        MEM32(0x7AD604) == 0u && MEM32(esp + 0x44) != 0x11u) {
        /* Device creation has already released its partial allocations.
         * Use retail non-AA presentation and its matching resolve path. */
        recomp_renderer_recovery_report(eax, 1u);
        MEM32(0x30F270) = 0x11u;
        MEM32(esp + 0x40) = 1u;
        MEM32(esp + 0x44) = 0x11u;
        PUSH32(esp, 0x7AD604);
        eax = esp + 0x38;
        PUSH32(esp, eax);
        PUSH32(esp, 0x40);
        PUSH32(esp, ebp);
        PUSH32(esp, edi);
        PUSH32(esp, ebp);
        PUSH32(esp, 0); sub_0028CAA0(); /* call 0x0028CAA0 */
        recomp_renderer_recovery_report(eax, 2u);
    }
    if ((int32_t)eax < 0 || MEM32(0x7AD604) == 0u)
        recomp_renderer_recovery_report(eax, 3u);
    ecx = MEM32(0x7AD604);""",
    ),
)
PATCHES += DEVICE_CREATION_RECOVERY_PATCHES

BOUNDARY_COMPARE_PATCHES = (
    GeneratedPatch(
        "retail boundary vertical-edge branch-local comparison",
        'loc_0014979B: ;\n    /* comiss xmm1, xmm4 - sets EFLAGS */\n    goto loc_001497BA;',
        'loc_0014979B: ;\n    /* Retail vertical polygon edge: this predecessor compares its X offset,\n     * not the delta-X in xmm3 used by the sloped-edge predecessor. */\n    if (isnan(xmm1) || isnan(xmm4) || xmm1 < xmm4) goto loc_001497C1;\n    goto loc_001497BC;',
    ),
)
PATCHES += BOUNDARY_COMPARE_PATCHES



def patch_generated(    generated_root: Path, *, allow_missing: bool = False
) -> list[tuple[str, Path]]:
    documents: dict[Path, tuple[str, str]] = {}
    for path in sorted(generated_root.glob("recomp_*.c")):
        raw = path.read_bytes()
        newline = "\r\n" if b"\r\n" in raw else "\n"
        documents[path] = (raw.decode("utf-8").replace("\r\n", "\n"), newline)

    if not documents:
        raise RuntimeError(f"no generated C files found in {generated_root}")

    applied: list[tuple[str, Path]] = []

    # A cached function map preserves every interior jump-table entry, but its
    # generic name for the retail XBE entry point must still match main.c and
    # the public generated declaration. Apply this as a token rename so the
    # function body, dispatch entry, and any direct references stay coherent.
    entry_generic = "sub_0022964A"
    entry_public = "xbe_entry_point"
    entry_matches = [
        path for path, (text, _) in documents.items()
        if entry_generic in text
    ]
    entry_public_matches = [
        path for path, (text, _) in documents.items()
        if entry_public in text
    ]
    header_path = generated_root / "recomp_funcs.h"
    header_raw = header_path.read_bytes()
    header_text = header_raw.decode("utf-8")
    if entry_matches:
        for path in entry_matches:
            text, newline = documents[path]
            documents[path] = (
                text.replace(entry_generic, entry_public), newline
            )
        if entry_generic in header_text:
            header_text = header_text.replace(entry_generic, entry_public)
            header_path.write_bytes(header_text.encode("utf-8"))
        elif entry_public not in header_text:
            raise RuntimeError(
                "retail entry point: generated C used the generic name but "
                "recomp_funcs.h did not"
            )
        for path in entry_matches:
            applied.append(("Retail XBE entry-point public name", path))
    elif not entry_public_matches or entry_public not in header_text:
        raise RuntimeError("retail entry point: generated symbol not found")


    # Keep the game-specific retail-XMV hot helpers available when a fresh
    # recompile regenerates recomp_types.h, then inline every direct call site.
    types_path = generated_root.parent / "recomp_types.h"
    types_raw = types_path.read_bytes()
    types_newline = "\r\n" if b"\r\n" in types_raw else "\n"
    types_text = types_raw.decode("utf-8").replace("\r\n", "\n")
    light_refresh_declaration = "int recomp_static_light_refresh_needed(uint32_t light);\n"
    if light_refresh_declaration not in types_text:
        types_text = types_text.replace("#include <stdint.h>\n", "#include <stdint.h>\n" + light_refresh_declaration, 1)
    types_changed = False
    controls_declarations = (
        "int recomp_controls_aim_assist(void);",
        "int recomp_controls_mouse_look_pending(void);",
        "float recomp_controls_mouse_delta(unsigned axis, float original);",
        "float recomp_controls_mouse_axis(uint32_t hash, float original, float dt);",
    )
    for declaration in controls_declarations:
        # Compare by function name: maintained headers may use other argument names.
        if declaration.split("(")[0].split()[-1] + "(" not in types_text:
            types_text = types_text.replace("#include <stdint.h>\n", "#include <stdint.h>\n" + declaration + "\n", 1)
            types_changed = True
    # Synchronize this maintained helper into disposable runtime headers.
    template_path = Path(__file__).resolve().parents[3] / "templates/runtime/recomp_types.h"
    template_types = template_path.read_text(encoding="utf-8")
    rep_pattern = r"static __forceinline void XBOX_REP_MOVS\(.*?^\}\n"
    rep_helper = re.search(rep_pattern, template_types, re.MULTILINE | re.DOTALL)
    if rep_helper is None:
        raise RuntimeError("Maintained REP MOVS helper missing")
    existing_rep = re.search(rep_pattern, types_text, re.MULTILINE | re.DOTALL)
    if existing_rep is None:
        rep_anchor = "/** Apply the guest x87 rounding-control field for FIST/FISTP. */"
        if rep_anchor not in types_text:
            raise RuntimeError("REP MOVS runtime header anchor missing")
        types_text = types_text.replace(rep_anchor, rep_helper[0] + rep_anchor, 1)
        types_changed = True
    elif existing_rep[0] != rep_helper[0]:
        types_text = types_text[:existing_rep.start()] + rep_helper[0] + types_text[existing_rep.end():]
        types_changed = True
    turbulence_declaration = "float recomp_turbulence_damping_dt(float dt);\n"
    if turbulence_declaration not in types_text:
        declaration_anchor = "#include <stdint.h>\n"
        if declaration_anchor not in types_text:
            raise RuntimeError("Turbulence damping declaration anchor not found")
        types_text = types_text.replace(declaration_anchor, declaration_anchor + turbulence_declaration, 1)
        types_changed = True
    recovery_declaration = "uint32_t recomp_renderer_test_oom(uint32_t multisample);\n"
    if recovery_declaration not in types_text:
        recovery_anchor = "float recomp_options_perspective_aspect(float guest_aspect);\n"
        if recovery_anchor not in types_text:
            raise RuntimeError("Renderer recovery declaration anchor not found")
        types_text = types_text.replace(recovery_anchor, recovery_anchor + recovery_declaration, 1)
        if "void recomp_renderer_recovery_report(uint32_t result, uint32_t stage);" not in types_text:
            types_text = types_text.replace(recovery_anchor, recovery_anchor + "void recomp_renderer_recovery_report(uint32_t result, uint32_t stage);\n", 1)
        types_changed = True
    satellite_declarations = 'float recomp_options_satellite_center_x(float x);\nfloat recomp_options_satellite_center_width(float width);\nfloat recomp_options_ui_x(float x, float anchor);\nvoid recomp_ui_record_position(uint32_t brush, float x, uint32_t screen_ref);\nvoid recomp_ui_begin_brush(uint32_t brush);\nvoid recomp_ui_end_brush(void);\n'
    if satellite_declarations not in types_text:
        satellite_anchor = "float recomp_options_perspective_aspect(float guest_aspect);\n"
        if satellite_anchor not in types_text:
            raise RuntimeError("Satellite center option declaration anchor not found")
        types_text = types_text.replace(satellite_anchor,
            satellite_anchor + satellite_declarations, 1)
        types_changed = True
    options_localization_declaration = (
        "uint32_t recomp_options_localization_hash(uint32_t hash);"
    )
    if options_localization_declaration not in types_text:
        options_declaration_anchor = (
            "uint32_t recomp_options_input_checkpoint(uint32_t menu_address,\n"
            "                                         uint32_t input, uint32_t event);\n"
        )
        if options_declaration_anchor not in types_text:
            raise RuntimeError("Recomp Options declaration anchor not found")
        types_text = types_text.replace(
            options_declaration_anchor,
            options_declaration_anchor + options_localization_declaration + "\n",
            1,
        )
        types_changed = True
    options_haze_declaration = "int recomp_options_authentic_haze(void);"
    if options_haze_declaration not in types_text:
        options_haze_anchor = options_localization_declaration + "\n"
        if options_haze_anchor not in types_text:
            raise RuntimeError("Recomp Options haze declaration anchor not found")
        types_text = types_text.replace(
            options_haze_anchor,
            options_haze_anchor + options_haze_declaration + "\n",
            1,
        )
        types_changed = True
    vehicle_door_declaration = (
        "void recomp_vehicle_door_checkpoint(uint32_t stage, uint32_t object,\n"
        "                                    uint32_t context, uint32_t detail);"
    )
    if vehicle_door_declaration not in types_text:
        vehicle_door_anchor = (
            "void recomp_collision_agent_checkpoint(uint32_t stage, uint32_t object,\n"
            "                                       uint32_t context, uint32_t detail);\n"
        )
        if vehicle_door_anchor not in types_text:
            raise RuntimeError("vehicle door declaration anchor not found")
        types_text = types_text.replace(
            vehicle_door_anchor,
            vehicle_door_anchor + vehicle_door_declaration + "\n",
            1,
        )
        types_changed = True
    helper_include = '#include "xmv_fast_helpers.h"'
    if helper_include not in types_text:
        helper_anchor = "#define mm7 g_mm7\n"
        if helper_anchor not in types_text:
            raise RuntimeError("XMV fast helper include anchor not found")
        types_text = types_text.replace(
            helper_anchor, helper_anchor + helper_include + "\n", 1
        )
        types_changed = True
    ai_vtable_declaration = (
        "uint32_t recomp_ai_process_stimuli_esi_checkpoint(uint32_t expected,\n"
        "                                                  uint32_t actual);\n"
        "uint32_t recomp_ai_process_stimuli_esp_checkpoint(uint32_t expected,\n"
        "                                                  uint32_t actual);\n"
        "uint32_t recomp_ai_process_stimuli_frame_checkpoint(uint32_t site,\n"
        "                                                    uint32_t expected,\n"
        "                                                    uint32_t actual);\n"
        "uint32_t recomp_ai_update_vtable_checkpoint(uint32_t site, uint32_t object,\n"
        "                                            uint32_t expected_vtable);\n"
        "uint32_t recomp_ai_stimulus_boundary_checkpoint(uint32_t stage, uint32_t object,\n"
        "                                            uint32_t expected_vtable,\n"
        "                                            uint32_t expected_depth);"
    )
    if ai_vtable_declaration not in types_text:
        ai_vtable_anchor = (
            "void recomp_ai_base_init_checkpoint(uint32_t stage, uint32_t owner,\n"
            "                                    uint32_t value1, uint32_t value2,\n"
            "                                    uint32_t guest_stack);\n"
        )
        if ai_vtable_anchor not in types_text:
            raise RuntimeError("AI vtable checkpoint declaration anchor not found")
        types_text = types_text.replace(
            ai_vtable_anchor,
            ai_vtable_anchor + ai_vtable_declaration + "\n",
            1,
        )
        types_changed = True
    spatial_integrity_declarations = (
        "void recomp_terrain_rendering_request(uint32_t requested_enable,\n"
        "                                      uint32_t renderable_flags);\n"
        "uint32_t recomp_redscene_collected_item_checkpoint(\n"
        "    uint32_t level, uint32_t source_index, uint32_t item,\n"
        "    uint32_t result_index);\n"
        "uint32_t recomp_actor_query_entry_checkpoint(uint32_t result_index,\n"
        "                                             uint32_t spatial_item,\n"
        "                                             uint32_t actor);\n"
        "uint32_t recomp_actor_query_count_checkpoint(uint32_t site, "
        "uint32_t count);"
    )
    if spatial_integrity_declarations not in types_text:
        spatial_declaration_anchor = (
            "void recomp_redscene_checkpoint(uint32_t stage, uint32_t scene,\n"
            "                                uint32_t subject, uint32_t result_buffer,\n"
            "                                uint32_t count);\n"
        )
        if spatial_declaration_anchor not in types_text:
            raise RuntimeError("spatial integrity declaration anchor not found")
        types_text = types_text.replace(
            spatial_declaration_anchor,
            spatial_declaration_anchor + spatial_integrity_declarations + "\n",
            1,
        )
        types_changed = True
    camera_checkpoint = "void recomp_camera_mode_checkpoint(uint32_t setter);"
    if camera_checkpoint not in types_text:
        declaration_anchor = (
            "void recomp_camera_pre_collision_checkpoint(uint32_t state,\n"
            "                                            uint32_t camera_position,\n"
            "                                            uint32_t direction);\n"
        )
        if declaration_anchor not in types_text:
            raise RuntimeError("camera mode checkpoint declaration anchor not found")
        types_text = types_text.replace(
            declaration_anchor,
            declaration_anchor + camera_checkpoint + "\n",
            1,
        )
        types_changed = True
    camera_post_checkpoint = (
        "void recomp_camera_mode_post_checkpoint(uint32_t setter);"
    )
    if camera_post_checkpoint not in types_text:
        if camera_checkpoint not in types_text:
            raise RuntimeError("camera post checkpoint declaration anchor not found")
        types_text = types_text.replace(
            camera_checkpoint + "\n",
            camera_checkpoint + "\n" + camera_post_checkpoint + "\n",
            1,
        )
        types_changed = True
    lua_frame_declarations = (
        "void recomp_lua_callframe_checkpoint(uint32_t stage, uint32_t state,\n"
        "                                     uint32_t function_object,\n"
        "                                     uint32_t aux0, uint32_t aux1);\n"
        "void recomp_lua_poscall_site_checkpoint(uint32_t site, uint32_t state,\n"
        "                                        uint32_t wanted,\n"
        "                                        uint32_t first_result);\n"
        "void recomp_lua_vm_precall_checkpoint(uint32_t stage, uint32_t state,\n"
        "                                      uint32_t function_object,\n"
        "                                      uint32_t result_or_wanted,\n"
        "                                      uint32_t stack, uint32_t saved_wanted);\n"
    )
    if "void recomp_lua_callframe_checkpoint(" not in types_text:
        if camera_post_checkpoint not in types_text:
            raise RuntimeError("Lua frame declaration anchor not found")
        types_text = types_text.replace(
            camera_post_checkpoint + "\n",
            camera_post_checkpoint + "\n" + lua_frame_declarations,
            1,
        )
        types_changed = True
    find_culprit_declaration = (
        "void recomp_find_culprit_checkpoint(uint32_t victim, "
        "uint32_t damage_type,\n"
        "                                    uint32_t suspect_mask, "
        "uint32_t culprit,\n"
        "                                    uint32_t best_value_bits);"
    )
    if find_culprit_declaration not in types_text:
        reward_anchor = (
            "void recomp_vehicle_reward_checkpoint(uint32_t stage, "
            "uint32_t actor,\n"
            "                                      uint32_t source, "
            "uint32_t credited,\n"
            "                                      uint32_t cash_bits,\n"
            "                                      uint32_t money_bits);\n"
        )
        if reward_anchor not in types_text:
            raise RuntimeError("FindCulprit declaration anchor not found")
        types_text = types_text.replace(
            reward_anchor,
            reward_anchor + find_culprit_declaration + "\n",
            1,
        )
        types_changed = True
    elif "void recomp_lua_poscall_site_checkpoint(" not in types_text:
        callframe_end = "                                     uint32_t aux0, uint32_t aux1);\n"
        if callframe_end not in types_text:
            raise RuntimeError("Lua poscall declaration anchor not found")
        poscall_declaration = (
            "void recomp_lua_poscall_site_checkpoint(uint32_t site, uint32_t state,\n"
            "                                        uint32_t wanted,\n"
            "                                        uint32_t first_result);\n"
        )
        types_text = types_text.replace(
            callframe_end, callframe_end + poscall_declaration, 1
        )
        types_changed = True
    if types_changed:
        if types_newline == "\r\n":
            types_text = types_text.replace("\n", "\r\n")
        types_path.write_bytes(types_text.encode("utf-8"))

    for before, after in XMV_DIRECT_CALL_REPLACEMENTS.items():
        before_matches = [
            path for path, (text, _) in documents.items() if before in text
        ]
        after_matches = [
            path for path, (text, _) in documents.items() if after in text
        ]
        if before_matches and not after_matches:
            for path in before_matches:
                source, newline = documents[path]
                documents[path] = (source.replace(before, after), newline)
                applied.append(("Retail XMV hot leaf direct calls", path))
        elif not before_matches and after_matches:
            for path in after_matches:
                applied.append(
                    ("Retail XMV hot leaf direct calls (already applied)", path)
                )
        else:
            raise RuntimeError(
                "Retail XMV hot leaf calls: expected unpatched or patched "
                f"sites for {before}; found {len(before_matches)} unpatched "
                f"files and {len(after_matches)} patched files"
            )
    # Upgrade earlier private intrinsic candidates before the canonical patch.
    for path, (text, newline) in list(documents.items()):
        documents[path] = (text.replace('loc_00238146: ;\n    /* _CIfmod receives dividend in ST(1), divisor in ST(0), and returns\n       one remainder. The generic CRT dispatcher relies on shared EBP,\n       FXAM and XLAT; lifting it corrupts the caller frame and leaks ST(1).\n       Match the native _CIacos bridge rather than changing HUD colors. */\n    {\n        const double divisor = g_fp_stack[g_fp_top & 7u];\n        const double dividend = g_fp_stack[(g_fp_top + 1u) & 7u];\n        if (isinf(dividend) || divisor == 0.0)\n            g_x87_status_word |= 1u; /* masked invalid operation */\n        /* Some Windows CRTs return NaN for a finite dividend and infinity. */\n        g_fp_stack[(g_fp_top + 1u) & 7u] =\n            isfinite(dividend) && isinf(divisor) ? dividend : fmod(dividend, divisor);\n        ++g_fp_top;\n    }\n    esp += 4; return; /* ret */\n', 'loc_00238146: ;\n    /* _CIfmod receives dividend in ST(1), divisor in ST(0), and returns\n       one remainder. The generic CRT dispatcher relies on shared EBP,\n       FXAM and XLAT; lifting it corrupts the caller frame and leaks ST(1).\n       Match the native _CIacos bridge rather than changing HUD colors. */\n    {\n        const double divisor = g_fp_stack[g_fp_top & 7u];\n        const double dividend = g_fp_stack[(g_fp_top + 1u) & 7u];\n        if (isinf(dividend) || divisor == 0.0)\n            g_x87_status_word |= 1u; /* masked invalid operation */\n        double remainder = isfinite(dividend) && isinf(divisor)\n            ? dividend : fmod(dividend, divisor);\n        /* Preserve the dividend sign for zero (legacy Windows CRTs lose it). */\n        if (remainder == 0.0) remainder = copysign(0.0, dividend);\n        g_fp_stack[(g_fp_top + 1u) & 7u] = remainder;\n        ++g_fp_top;\n    }\n    esp += 4; return; /* ret */\n').replace('loc_00238146: ;\n    /* _CIfmod receives dividend in ST(1), divisor in ST(0), and returns\n       one remainder. The generic CRT dispatcher relies on shared EBP,\n       FXAM and XLAT; lifting it corrupts the caller frame and leaks ST(1).\n       Match the native _CIacos bridge rather than changing HUD colors. */\n    {\n        const double divisor = g_fp_stack[g_fp_top & 7u];\n        const double dividend = g_fp_stack[(g_fp_top + 1u) & 7u];\n        if (isinf(dividend) || divisor == 0.0)\n            g_x87_status_word |= 1u; /* masked invalid operation */\n        g_fp_stack[(g_fp_top + 1u) & 7u] = fmod(dividend, divisor);\n        ++g_fp_top;\n    }\n    esp += 4; return; /* ret */\n', 'loc_00238146: ;\n    /* _CIfmod receives dividend in ST(1), divisor in ST(0), and returns\n       one remainder. The generic CRT dispatcher relies on shared EBP,\n       FXAM and XLAT; lifting it corrupts the caller frame and leaks ST(1).\n       Match the native _CIacos bridge rather than changing HUD colors. */\n    {\n        const double divisor = g_fp_stack[g_fp_top & 7u];\n        const double dividend = g_fp_stack[(g_fp_top + 1u) & 7u];\n        if (isinf(dividend) || divisor == 0.0)\n            g_x87_status_word |= 1u; /* masked invalid operation */\n        double remainder = isfinite(dividend) && isinf(divisor)\n            ? dividend : fmod(dividend, divisor);\n        /* Preserve the dividend sign for zero (legacy Windows CRTs lose it). */\n        if (remainder == 0.0) remainder = copysign(0.0, dividend);\n        g_fp_stack[(g_fp_top + 1u) & 7u] = remainder;\n        ++g_fp_top;\n    }\n    esp += 4; return; /* ret */\n'), newline)
    # Reapply label checkpoints after the functional patches whose anchors they split.
    for checkpoint in PATCHES:
        if checkpoint.name.startswith("satellite color checkpoint "):
            for path, (text, newline) in list(documents.items()):
                documents[path] = (text.replace(checkpoint.after, checkpoint.before), newline)
    for patch in PATCHES + (GeneratedPatch("recover static light fade after camera relocation", 'loc_00177ADB: ;\n    eax = MEM32(esi + 0x1D0);\n    if (TEST_Z(eax, eax)) goto loc_00177E2B; /* je: equal / zero */', 'loc_00177ADB: ;\n    eax = MEM32(esi + 0x1D0);\n    if (TEST_Z(eax, eax)) {\n        if (recomp_static_light_refresh_needed(esi)) {\n            ecx = esi;\n            PUSH32(esp, 0); sub_00177300(); /* original distance-fade recalculation */\n        }\n        goto loc_00177E2B;\n    }'),
    GeneratedPatch(
        "preserve retail turbulence damping above 30 FPS",
        'loc_0009C197: ;\n    fp_top() *= MEMF(0x2DC884); /* fmul memory */\n    recomp_xmm_loadss(xmm0v, 0x2E7048); /* movss */\n    xmm0 = xmm0 / MEMF(esp + 0x28); /* divss */',
        'loc_0009C197: ;\n    fp_top() *= MEMF(0x2DC884); /* fmul memory */\n    recomp_xmm_loadss(xmm0v, 0x2E7048); /* movss */\n    xmm0 = xmm0 / recomp_turbulence_damping_dt(MEMF(esp + 0x28)); /* preserve 30 FPS noise response */',
    ),):
        before_matches = [
            path for path, (text, _) in documents.items()
            if patch.before in text.replace(patch.after, "")
        ]
        after_matches = [
            path for path, (text, _) in documents.items()
            if patch.after in text
        ]
        if len(before_matches) == 1 and not after_matches:
            path = before_matches[0]
            text, newline = documents[path]
            documents[path] = (text.replace(patch.before, patch.after, 1), newline)
            applied.append((patch.name, path))
        elif not before_matches and len(after_matches) == 1:
            applied.append((patch.name + " (already applied)", after_matches[0]))
        elif (not before_matches and not after_matches and
              patch.name in FUNCTION_REPLACEMENT_PATCHES):
            function_name = FUNCTION_REPLACEMENT_PATCHES[patch.name]
            pattern = re.compile(
                rf"^void {function_name}\(void\)\n\{{.*?^\}}\n(?=\n/\*\*)",
                re.MULTILINE | re.DOTALL,
            )
            function_matches = [
                path for path, (text, _) in documents.items()
                if pattern.search(text)
            ]
            if len(function_matches) == 1:
                path = function_matches[0]
                text, newline = documents[path]
                documents[path] = (
                    pattern.sub(patch.after, text, count=1), newline)
                applied.append(
                    (patch.name + " (function replacement)", path))
            elif (not function_matches and
                  patch.name in TRANSLATOR_INLINED_PATCHES):
                inline = TRANSLATOR_INLINED_PATCHES[patch.name]
                inline_matches = [
                    path for path, (text, _) in documents.items()
                    if inline in text
                ]
                if len(inline_matches) != 1:
                    raise RuntimeError(
                        f"{patch.name}: expected one generated "
                        f"{function_name} or translator-inlined retail body; "
                        f"found {len(function_matches)} functions and "
                        f"{len(inline_matches)} inline bodies"
                    )
                applied.append((
                    patch.name + " (translator-inlined)",
                    inline_matches[0]))
            else:
                raise RuntimeError(
                    f"{patch.name}: expected one generated {function_name}; "
                    f"found {len(function_matches)}"
                )
        elif (not before_matches and not after_matches and
              patch.name in TRANSLATOR_INLINED_PATCHES):
            inline = TRANSLATOR_INLINED_PATCHES[patch.name]
            inline_matches = [
                path for path, (text, _) in documents.items()
                if inline in text
            ]
            if len(inline_matches) != 1:
                raise RuntimeError(
                    f"{patch.name}: expected one translator-inlined retail "
                    f"epilogue; found {len(inline_matches)}"
                )
            applied.append((patch.name + " (translator-inlined)",
                            inline_matches[0]))
        elif (not before_matches and not after_matches and
              patch.name in OPTIONAL_PATCHES):
            continue
        elif allow_missing and not before_matches and not after_matches:
            continue
        else:
            raise RuntimeError(
                f"{patch.name}: expected exactly one unpatched or patched site; "
                f"found {len(before_matches)} unpatched and "
                f"{len(after_matches)} patched"
            )

    # ProcessStimuli is a large retail function whose translated direct callees
    # can violate the x86 frame contract on a rare post-gate AI path. Keep its
    # repair scoped to stable post-call labels: every checkpoint is a point at
    # which the original function's ESP is back at the fixed local-frame base.
    ai_process_paths = [
        path for path, (text, _) in documents.items()
        if "void sub_00069550(void)" in text
    ]
    if len(ai_process_paths) != 1:
        raise RuntimeError(
            "ProcessStimuli frame guards: expected one generated function; "
            f"found {len(ai_process_paths)}"
        )
    ai_process_path = ai_process_paths[0]
    ai_text, ai_newline = documents[ai_process_path]
    ai_start = ai_text.index("void sub_00069550(void)")
    ai_end = ai_text.index("\n/**", ai_start + 1)
    ai_body = ai_text[ai_start:ai_end]
    ai_changed = False
    if "uint32_t ai_process_frame_stack = 0u;" not in ai_body:
        ai_body = ai_body.replace(
            "    uint32_t ebp;\n",
            "    uint32_t ebp;\n"
            "    uint32_t ai_process_frame_stack = 0u;\n",
            1,
        )
        frame_anchor = (
            "    PUSH32(esp, ebx);\n"
            "    PUSH32(esp, ebp);\n"
            "    PUSH32(esp, esi);\n"
            "    PUSH32(esp, edi);\n"
            "    PUSH32(esp, 0x2E4234);"
        )
        if frame_anchor not in ai_body:
            raise RuntimeError("ProcessStimuli frame-base anchor not found")
        ai_body = ai_body.replace(
            frame_anchor,
            frame_anchor.replace(
                "    PUSH32(esp, 0x2E4234);",
                "    ai_process_frame_stack = esp;\n"
                "    PUSH32(esp, 0x2E4234);",
            ),
            1,
        )
        ai_changed = True
    ai_frame_labels = (
        "0006959E", "000695E0", "000695FE", "00069672", "000696B7",
        "000696F5", "00069707", "00069729", "0006975E", "0006976F",
        "000697B2", "000697C0", "000697D5", "000697DF", "000697EF",
        "00069800", "00069811", "00069821", "0006982D", "00069886",
        "000698D4", "000698E2", "000698FF", "0006990F", "00069921",
        "0006993E", "00069955", "00069966", "000699A8", "000699B9",
        "000699CC", "000699DE", "000699F5", "00069A73", "00069A8C",
        "00069B4F", "00069B72",
    )
    for site in ai_frame_labels:
        anchor = f"loc_{site}: ;\n"
        marker = (
            anchor + "    esp = recomp_ai_process_stimuli_frame_checkpoint(\n"
        )
        if marker in ai_body:
            continue
        if anchor not in ai_body:
            raise RuntimeError(f"ProcessStimuli checkpoint label missing: {site}")
        ai_body = ai_body.replace(
            anchor,
            marker + f"        0x{site}u, ai_process_frame_stack, esp);\n",
            1,
        )
        ai_changed = True
    ai_cleanup_labels = {
        "0006957A": "    esp = esp + 0xC;",
        "000695B9": "    esp = esp + 0xC;",
        "00069612": "    esp = esp + 4;",
        "00069A54": "    esp = esp + 0x14;",
        "00069B57": "    esp = esp + 4;",
        "00069B78": "    esp = esp + 4;",
    }
    for site, cleanup in ai_cleanup_labels.items():
        anchor = f"loc_{site}: ;\n{cleanup}\n"
        marker = (
            anchor + "    esp = recomp_ai_process_stimuli_frame_checkpoint(\n"
        )
        if marker in ai_body:
            continue
        if anchor not in ai_body:
            raise RuntimeError(f"ProcessStimuli cleanup label missing: {site}")
        ai_body = ai_body.replace(
            anchor,
            marker + f"        0x{site}u, ai_process_frame_stack, esp);\n",
            1,
        )
        ai_changed = True
    ai_percept_changed = False
    percept_locals = (
        "    uint32_t ai_process_percept_esi = 0u;\n"
        "    uint32_t ai_process_percept_edi = 0u;\n"
        "    uint32_t ai_process_percept_ebx = 0u;\n"
        "    uint32_t ai_process_percept_ebp = 0u;\n"
        "    uint32_t ai_process_percept_call_active = 0u;\n"
    )
    if "uint32_t ai_process_percept_call_active = 0u;" not in ai_body:
        local_anchor = "    uint32_t ai_process_frame_stack = 0u;\n"
        if local_anchor not in ai_body:
            raise RuntimeError("ProcessStimuli percept-call local anchor missing")
        ai_body = ai_body.replace(
            local_anchor,
            local_anchor + percept_locals,
            1,
        )
        ai_percept_changed = True
    percept_call_anchor = "loc_00069A81: ;\n    eax = MEM32(esi);"
    percept_call_marker = (
        "loc_00069A81: ;\n"
        "    ai_process_percept_esi = esi;\n"
        "    ai_process_percept_edi = edi;\n"
        "    ai_process_percept_ebx = ebx;\n"
        "    ai_process_percept_ebp = ebp;\n"
        "    ai_process_percept_call_active = 1u;\n"
        "    eax = MEM32(esi);"
    )
    if percept_call_marker not in ai_body:
        if percept_call_anchor not in ai_body:
            raise RuntimeError("ProcessStimuli percept-call save anchor missing")
        ai_body = ai_body.replace(
            percept_call_anchor,
            percept_call_marker,
            1,
        )
        ai_percept_changed = True
    percept_return_anchor = (
        "loc_00069A8C: ;\n"
        "    esp = recomp_ai_process_stimuli_frame_checkpoint(\n"
        "        0x00069A8Cu, ai_process_frame_stack, esp);\n"
        "    ebx = ebx + 0x88;"
    )
    percept_return_marker = (
        "loc_00069A8C: ;\n"
        "    esp = recomp_ai_process_stimuli_frame_checkpoint(\n"
        "        0x00069A8Cu, ai_process_frame_stack, esp);\n"
        "    if (ai_process_percept_call_active) {\n"
        "        esi = ai_process_percept_esi;\n"
        "        edi = ai_process_percept_edi;\n"
        "        ebx = ai_process_percept_ebx;\n"
        "        ebp = ai_process_percept_ebp;\n"
        "        ai_process_percept_call_active = 0u;\n"
        "    }\n"
        "    ebx = ebx + 0x88;"
    )
    if percept_return_marker not in ai_body:
        if percept_return_anchor not in ai_body:
            raise RuntimeError("ProcessStimuli percept-call restore anchor missing")
        ai_body = ai_body.replace(
            percept_return_anchor,
            percept_return_marker,
            1,
        )
        ai_percept_changed = True
    if ai_changed or ai_percept_changed:
        ai_text = ai_text[:ai_start] + ai_body + ai_text[ai_end:]
        documents[ai_process_path] = (ai_text, ai_newline)
    if ai_changed:
        applied.append(("ProcessStimuli fixed-frame ABI guards", ai_process_path))
    else:
        applied.append((
            "ProcessStimuli fixed-frame ABI guards (already applied)",
            ai_process_path,
        ))
    if ai_percept_changed:
        applied.append((
            "ProcessStimuli percept-call nonvolatile guard",
            ai_process_path,
        ))
    else:
        applied.append((
            "ProcessStimuli percept-call nonvolatile guard (already applied)",
            ai_process_path,
        ))

    # The shell state update keeps its owner in x86 nonvolatile EBX and its
    # channel loop in ESI/EDI across renderer callbacks. A translated callback
    # can legitimately use the global emulated registers internally, so retain
    # the retail call-boundary values in native locals. Without this, EBX can
    # become zero and the next [owner+0x3EB8] vtable dispatch dereferences a
    # garbage target during frontend/logo rendering.
    frontend_shell_paths = [
        path for path, (text, _) in documents.items()
        if "void sub_000D51F0(void)" in text
    ]
    if len(frontend_shell_paths) != 1:
        raise RuntimeError(
            "frontend shell callback ABI guards: expected one generated "
            f"function; found {len(frontend_shell_paths)}"
        )
    frontend_shell_path = frontend_shell_paths[0]
    frontend_shell_text, frontend_shell_newline = documents[frontend_shell_path]
    frontend_shell_start = frontend_shell_text.index("void sub_000D51F0(void)")
    frontend_shell_end = frontend_shell_text.index(
        "\n/**", frontend_shell_start + 1
    )
    frontend_shell_body = frontend_shell_text[
        frontend_shell_start:frontend_shell_end
    ]
    frontend_shell_changed = False
    frontend_shell_locals = (
        "    uint32_t frontend_shell_this = 0u;\n"
        "    uint32_t frontend_loop_saved_ebx = 0u;\n"
        "    uint32_t frontend_loop_saved_esi = 0u;\n"
        "    uint32_t frontend_loop_saved_edi = 0u;\n"
    )
    if "uint32_t frontend_shell_this = 0u;" not in frontend_shell_body:
        local_anchor = "    RECOMP_TRACE_FUNC(0x000D51F0u);\n"
        if local_anchor not in frontend_shell_body:
            raise RuntimeError("frontend shell ABI local anchor missing")
        frontend_shell_body = frontend_shell_body.replace(
            local_anchor, frontend_shell_locals + local_anchor, 1
        )
        frontend_shell_changed = True
    entry_anchor = "    ebx = ecx;\n"
    entry_marker = (
        "    ebx = ecx;\n"
        "    frontend_shell_this = ebx;\n"
    )
    if entry_marker not in frontend_shell_body:
        if entry_anchor not in frontend_shell_body:
            raise RuntimeError("frontend shell ABI entry anchor missing")
        frontend_shell_body = frontend_shell_body.replace(
            entry_anchor, entry_marker, 1
        )
        frontend_shell_changed = True
    loop_anchor = "loc_000D53E4: ;\n"
    loop_marker = (
        "loc_000D53E4: ;\n"
        "    ebx = frontend_shell_this;\n"
    )
    if loop_marker not in frontend_shell_body:
        if loop_anchor not in frontend_shell_body:
            raise RuntimeError("frontend shell ABI loop-owner anchor missing")
        frontend_shell_body = frontend_shell_body.replace(
            loop_anchor, loop_marker, 1
        )
        frontend_shell_changed = True
    frontend_callback_sites = (
        ("000D53FE", "000D540C"),
        ("000D541D", "000D542B"),
        ("000D5460", "000D5473"),
        ("000D547E", "000D548C"),
    )
    for call_site, return_site in frontend_callback_sites:
        call_anchor = f"loc_{call_site}: ;\n"
        call_marker = (
            f"loc_{call_site}: ;\n"
            "    frontend_loop_saved_ebx = ebx;\n"
            "    frontend_loop_saved_esi = esi;\n"
            "    frontend_loop_saved_edi = edi;\n"
        )
        if call_marker not in frontend_shell_body:
            if call_anchor not in frontend_shell_body:
                raise RuntimeError(
                    f"frontend shell ABI call anchor missing: {call_site}"
                )
            frontend_shell_body = frontend_shell_body.replace(
                call_anchor, call_marker, 1
            )
            frontend_shell_changed = True
        legacy_return_marker = (
            f"loc_{return_site}: ;\n"
            "    ebx = frontend_loop_saved_ebx;\n"
            "    esi = frontend_loop_saved_esi;\n"
            "    edi = frontend_loop_saved_edi;\n"
        )
        if legacy_return_marker in frontend_shell_body:
            frontend_shell_body = frontend_shell_body.replace(
                legacy_return_marker, f"loc_{return_site}: ;\n", 1
            )
            frontend_shell_changed = True
        return_anchor = f"\n\nloc_{return_site}: ;\n"
        return_marker = (
            "\n    ebx = frontend_loop_saved_ebx;\n"
            "    esi = frontend_loop_saved_esi;\n"
            "    edi = frontend_loop_saved_edi;\n"
            f"\nloc_{return_site}: ;\n"
        )
        if return_marker not in frontend_shell_body:
            if return_anchor not in frontend_shell_body:
                raise RuntimeError(
                    f"frontend shell ABI return anchor missing: {return_site}"
                )
            frontend_shell_body = frontend_shell_body.replace(
                return_anchor, return_marker, 1
            )
            frontend_shell_changed = True
    frontend_shell_checkpoint_sites = (
        (
            "    frontend_shell_this = ebx;\n",
            "    frontend_shell_this = ebx;\n"
            "    recomp_frontend_shell_loop_checkpoint(0u, "
            "frontend_shell_this, 0u);\n",
        ),
        (
            "loc_000D53F0: ;\n",
            "loc_000D53F0: ;\n"
            "    recomp_frontend_shell_loop_checkpoint(1u, "
            "frontend_shell_this, esi);\n",
        ),
        (
            "loc_000D54A7: ;\n",
            "loc_000D54A7: ;\n"
            "    recomp_frontend_shell_loop_checkpoint(2u, "
            "frontend_shell_this, esi);\n",
        ),
    )
    for checkpoint_anchor, checkpoint_marker in frontend_shell_checkpoint_sites:
        if checkpoint_marker not in frontend_shell_body:
            if checkpoint_anchor not in frontend_shell_body:
                raise RuntimeError(
                    "frontend shell loop checkpoint anchor missing: "
                    f"{checkpoint_anchor.strip()}"
                )
            frontend_shell_body = frontend_shell_body.replace(
                checkpoint_anchor, checkpoint_marker, 1
            )
            frontend_shell_changed = True
    if frontend_shell_changed:
        frontend_shell_text = (
            frontend_shell_text[:frontend_shell_start] + frontend_shell_body +
            frontend_shell_text[frontend_shell_end:]
        )
        documents[frontend_shell_path] = (
            frontend_shell_text, frontend_shell_newline
        )
        applied.append((
            "frontend shell callback ABI guards", frontend_shell_path
        ))
    else:
        applied.append((
            "frontend shell callback ABI guards (already applied)",
            frontend_shell_path,
        ))
    # Retail 0x90295..0x902AB clamps the step count to 1..6; direct execution
    # is checked by test_retail_loop_limits.py. The translated loop keeps its
    # remaining count in nonvolatile EDI, but a nested
    # 60 Hz update can corrupt the emulated save slot and turn the loop
    # infinite. Snapshot the x86 call-boundary state in native locals.
    camera_tilt_paths = [
        path for path, (text, _) in documents.items()
        if "void sub_00090180(void)" in text
    ]
    if len(camera_tilt_paths) != 1:
        raise RuntimeError(
            "camera tilt loop ABI guard: expected one generated function; "
            f"found {len(camera_tilt_paths)}"
        )
    camera_tilt_path = camera_tilt_paths[0]
    camera_tilt_text, camera_tilt_newline = documents[camera_tilt_path]
    camera_tilt_start = camera_tilt_text.index("void sub_00090180(void)")
    camera_tilt_end = camera_tilt_text.index("\n/**", camera_tilt_start + 1)
    camera_tilt_body = camera_tilt_text[camera_tilt_start:camera_tilt_end]
    camera_tilt_changed = False
    camera_tilt_locals = (
        "    uint32_t camera_tilt_saved_esi = 0u;\n"
        "    uint32_t camera_tilt_saved_edi = 0u;\n"
        "    uint32_t camera_tilt_saved_ebx = 0u;\n"
        "    uint32_t camera_tilt_saved_ebp = 0u;\n"
        "    uint32_t camera_tilt_saved_esp = 0u;\n"
    )
    if "uint32_t camera_tilt_saved_edi = 0u;" not in camera_tilt_body:
        local_anchor = "    uint32_t ebp;\n"
        if local_anchor not in camera_tilt_body:
            raise RuntimeError("camera tilt loop ABI local anchor missing")
        camera_tilt_body = camera_tilt_body.replace(
            local_anchor, local_anchor + camera_tilt_locals, 1
        )
        camera_tilt_changed = True
    call_anchor = (
        "loc_000902D0: ;\n"
        "    PUSH32(esp, ebp);\n"
        "    PUSH32(esp, ebx);"
    )
    call_marker = (
        "loc_000902D0: ;\n"
        "    camera_tilt_saved_esi = esi;\n"
        "    camera_tilt_saved_edi = edi;\n"
        "    camera_tilt_saved_ebx = ebx;\n"
        "    camera_tilt_saved_ebp = ebp;\n"
        "    camera_tilt_saved_esp = esp;\n"
        "    PUSH32(esp, ebp);\n"
        "    PUSH32(esp, ebx);"
    )
    if call_marker not in camera_tilt_body:
        if call_anchor not in camera_tilt_body:
            raise RuntimeError("camera tilt loop ABI call anchor missing")
        camera_tilt_body = camera_tilt_body.replace(
            call_anchor, call_marker, 1
        )
        camera_tilt_changed = True
    return_anchor = "loc_000902D9: ;\n    edi--;"
    return_marker = (
        "loc_000902D9: ;\n"
        "    recomp_camera_tilt_loop_checkpoint(\n"
        "        0x000902D9u, camera_tilt_saved_edi, edi,\n"
        "        camera_tilt_saved_esp, esp);\n"
        "    esi = camera_tilt_saved_esi;\n"
        "    edi = camera_tilt_saved_edi;\n"
        "    ebx = camera_tilt_saved_ebx;\n"
        "    ebp = camera_tilt_saved_ebp;\n"
        "    esp = camera_tilt_saved_esp;\n"
        "    edi--;"
    )
    if return_marker not in camera_tilt_body:
        if return_anchor not in camera_tilt_body:
            raise RuntimeError("camera tilt loop ABI return anchor missing")
        camera_tilt_body = camera_tilt_body.replace(
            return_anchor, return_marker, 1
        )
        camera_tilt_changed = True
    if camera_tilt_changed:
        camera_tilt_text = (
            camera_tilt_text[:camera_tilt_start] + camera_tilt_body +
            camera_tilt_text[camera_tilt_end:]
        )
        documents[camera_tilt_path] = (
            camera_tilt_text, camera_tilt_newline
        )
        applied.append(("camera tilt loop ABI guard", camera_tilt_path))
    else:
        applied.append((
            "camera tilt loop ABI guard (already applied)", camera_tilt_path
        ))
    # Merge the retail bullet routine's 0x20100 continuation back into its
    # parent. The lifter otherwise emits internal branches as calls to
    # unresolved minimal-return stubs (notably 0x20235), which skips
    # ApplyModifiedDamage for actors without Havok physics.
    projectile_paths = [
        path for path, (text, _) in documents.items()
        if "void sub_0001FBB0(void)" in text
    ]
    if len(projectile_paths) != 1:
        raise RuntimeError(
            "projectile continuation repair: expected one generated file; "
            f"found {len(projectile_paths)}"
        )
    projectile_path = projectile_paths[0]
    projectile_text, projectile_newline = documents[projectile_path]
    projectile_marker = "/* retail projectile continuation repair */"
    if projectile_marker not in projectile_text:
        parent_start = projectile_text.index("void sub_0001FBB0(void)")
        parent_end = projectile_text.index("\n/**", parent_start + 1)
        parent_body = projectile_text[parent_start:parent_end]
        continuation_start = projectile_text.index("void sub_00020100(void)")
        continuation_end = projectile_text.index(
            "\n/**", continuation_start + 1
        )
        continuation_function = projectile_text[
            continuation_start:continuation_end
        ]
        continuation = continuation_function[
            continuation_function.index("loc_00020100: ;"):
            continuation_function.index("\n    #undef fp_push")
        ]
        continuation = continuation.replace(
            "    /* comiss xmm1, xmm2 - sets EFLAGS */",
            "loc_00020122: ;\n"
            "    /* comiss xmm1, xmm2 - sets EFLAGS */",
            1,
        ).replace(
            "    /* comiss xmm2, xmm0 - sets EFLAGS */",
            "loc_00020137: ;\n"
            "    /* comiss xmm2, xmm0 - sets EFLAGS */",
            1,
        ).replace(
            "    recomp_xmm_zero(xmm0v); /* xorps self = zero */\n"
            "    recomp_xmm_copy(xmm1v, xmm0v); /* movaps */\n"
            "    recomp_xmm_copy(xmm2v, xmm0v); /* movaps */\n\n"
            "loc_00020834: ;",
            "loc_0002082B: ;\n"
            "    recomp_xmm_zero(xmm0v); /* xorps self = zero */\n"
            "    recomp_xmm_copy(xmm1v, xmm0v); /* movaps */\n"
            "    recomp_xmm_copy(xmm2v, xmm0v); /* movaps */\n\n"
            "loc_00020834: ;",
            1,
        ).replace(
            "{ sub_00020095(); return; }", "goto loc_00020095;"
        )
        parent_body = parent_body.replace(
            "loc_00020093: ;\n    edi = eax;\n    PUSH32(esp, 0);",
            "loc_00020093: ;\n    edi = eax;\n\n"
            "loc_00020095: ;\n    PUSH32(esp, 0);",
            1,
        )
        branch_replacements = {
            "{ sub_00020122(); return; }": "goto loc_00020122;",
            "{ sub_00020137(); return; }": "goto loc_00020137;",
            "g_seh_ebp = ebp; sub_00020154(); return;":
                "goto loc_00020154;",
            "{ sub_00020235(); return; }": "goto loc_00020235;",
            "{ sub_0002082B(); return; }": "goto loc_0002082B;",
            "{ sub_00020885(); return; }": "goto loc_00020885;",
        }
        for before, after in branch_replacements.items():
            parent_body = parent_body.replace(before, after)
        tail = (
            "    g_seh_ebp = ebp; sub_00020100(); return; "
            "/* fallthrough 0x00020100 */"
        )
        if tail not in parent_body:
            raise RuntimeError("projectile continuation tail anchor missing")
        parent_body = parent_body.replace(
            tail,
            f"    {projectile_marker}\n\n{continuation}",
            1,
        )
        projectile_text = (
            projectile_text[:parent_start] + parent_body +
            projectile_text[parent_end:]
        )
        documents[projectile_path] = (
            projectile_text, projectile_newline
        )
        applied.append(("retail projectile continuation repair", projectile_path))
    else:
        applied.append((
            "retail projectile continuation repair (already applied)",
            projectile_path,
        ))
    traffic_matches = [
        path for path, (text, _) in documents.items()
        if "loc_0016AF22: ;" in text and "loc_0016BD2C: ;" in text
    ]
    if len(traffic_matches) != 1:
        raise RuntimeError(
            "retail traffic owner: expected one generated function shard; "
            f"found {len(traffic_matches)}"
        )
    traffic_path = traffic_matches[0]
    traffic_text, traffic_newline = documents[traffic_path]
    ambient_intersection_marker = (
        "/* preserve first ambient intersection COMISS before xmm0 reload */"
    )
    ambient_intersection_native_marker = (
        "/* preserve comiss flags across 17 instruction(s) */"
    )
    ambient_intersection_compare_anchor = (
        "loc_0016AF22: ;\n"
        "    fp_push(MEMF(esi + 8)); /* fld float */\n"
        "    recomp_xmm_loadss(xmm0v, ecx + edi * 4); /* movss */\n"
        "    fp_top() -= MEMF(esi + -4); /* fsub memory */\n"
        "    recomp_xmm_zero(xmm1v); /* xorps self = zero */\n"
        "    /* comiss xmm0, xmm1 - sets EFLAGS */\n"
        "    recomp_xmm_loadss(xmm0v, 0x2DC08C); /* movss */"
    )
    ambient_intersection_compare_replacement = (
        "loc_0016AF22: ;\n"
        "    fp_push(MEMF(esi + 8)); /* fld float */\n"
        "    recomp_xmm_loadss(xmm0v, ecx + edi * 4); /* movss */\n"
        "    fp_top() -= MEMF(esi + -4); /* fsub memory */\n"
        "    recomp_xmm_zero(xmm1v); /* xorps self = zero */\n"
        f"    {ambient_intersection_marker}\n"
        "    const int _ambient_first_intersection_nonpositive =\n"
        "        (isnan((double)(xmm0)) || isnan((double)(xmm1)) ||\n"
        "         xmm0 <= xmm1);\n"
        "    recomp_xmm_loadss(xmm0v, 0x2DC08C); /* movss */"
    )
    ambient_intersection_branch_anchor = (
        "    if (((isnan((double)(xmm0)) || isnan((double)(xmm1))) || "
        "(xmm0 <= xmm1))) goto loc_0016B105; "
        "/* jbe: below or equal (unsigned <=) */"
    )
    ambient_intersection_branch_replacement = (
        "    if (_ambient_first_intersection_nonpositive) "
        "goto loc_0016B105; "
        "/* jbe: below or equal (unsigned <=) */"
    )
    if ambient_intersection_native_marker in traffic_text:
        applied.append((
            "retail ambient first-intersection COMISS preservation "
            "(native translator output)", traffic_path
        ))
    elif ambient_intersection_marker not in traffic_text:
        if traffic_text.count(ambient_intersection_compare_anchor) != 1:
            raise RuntimeError(
                "ambient first-intersection COMISS anchor missing or ambiguous"
            )
        if traffic_text.count(ambient_intersection_branch_anchor) != 1:
            raise RuntimeError(
                "ambient first-intersection JBE anchor missing or ambiguous"
            )
        traffic_text = traffic_text.replace(
            ambient_intersection_compare_anchor,
            ambient_intersection_compare_replacement, 1
        )
        traffic_text = traffic_text.replace(
            ambient_intersection_branch_anchor,
            ambient_intersection_branch_replacement, 1
        )
        documents[traffic_path] = (traffic_text, traffic_newline)
        applied.append((
            "retail ambient first-intersection COMISS preservation", traffic_path
        ))
    else:
        applied.append((
            "retail ambient first-intersection COMISS preservation (already applied)",
            traffic_path,
        ))

    traffic_text, traffic_newline = documents[traffic_path]
    traffic_marker = "/* recomp traffic random-spawn target diagnostic */"
    traffic_anchor = (
        "    MEM32(esp + 0x6C) = eax;\n"
        "    MEM32(esp + 0xC) = 0;\n"
        "    if (CMP_GE(edx, eax)) goto loc_0016BD70; "
        "/* jge: greater or equal (signed >=) */"
    )
    traffic_replacement = (
        "    MEM32(esp + 0x6C) = eax;\n"
        "    MEM32(esp + 0xC) = 0;\n"
        f"    {traffic_marker}\n"
        "    eax = recomp_traffic_random_path_target(\n"
        "        MEM32(esp + 0x28), esi, eax);\n"
        "    MEM32(esp + 0x6C) = eax;\n"
        "    if (CMP_GE(edx, eax)) goto loc_0016BD70; "
        "/* jge: greater or equal (signed >=) */"
    )
    if traffic_marker not in traffic_text:
        if traffic_text.count(traffic_anchor) != 1:
            raise RuntimeError("traffic random-spawn target anchor missing or ambiguous")
        traffic_text = traffic_text.replace(
            traffic_anchor, traffic_replacement, 1
        )
        documents[traffic_path] = (traffic_text, traffic_newline)
        applied.append((
            "retail traffic random-spawn target diagnostic", traffic_path
        ))
    else:
        applied.append((
            "retail traffic random-spawn target diagnostic (already applied)",
            traffic_path,
        ))

    traffic_text, traffic_newline = documents[traffic_path]
    traffic_spawn_marker = "/* recomp traffic random-spawn actor diagnostic */"
    traffic_spawn_anchor = (
        "loc_0016BD2C: ;\n"
        "    ecx = MEM32(edi);\n"
        "    eax = MEM32(ecx + eax * 4 + 0xC);\n"
        "    ecx = MEM32(esp + 0x28);\n"
        "    edx = esp + 0x130;\n"
        "    PUSH32(esp, edx);\n"
        "    PUSH32(esp, eax);\n"
        "    eax = MEM32(esp + 0x58);\n"
        "    PUSH32(esp, eax);\n"
        "    PUSH32(esp, esi);\n"
        "    PUSH32(esp, 0); sub_0016A520(); /* call 0x0016A520 */"
    )
    traffic_spawn_replacement = (
        "loc_0016BD2C: ;\n"
        f"    {traffic_spawn_marker}\n"
        "    { const uint32_t _traffic_manager = MEM32(esp + 0x28);\n"
        "    const uint32_t _traffic_record = esi;\n"
        "    const uint32_t _traffic_point = MEM32(esp + 0x50);\n"
        "    const uint32_t _traffic_matrix = esp + 0x130;\n"
        "    ecx = MEM32(edi);\n"
        "    eax = MEM32(ecx + eax * 4 + 0xC);\n"
        "    { const uint32_t _traffic_type = eax;\n"
        "    recomp_traffic_spawn_checkpoint(\n"
        "        0u, _traffic_manager, _traffic_record, _traffic_type,\n"
        "        _traffic_point, _traffic_matrix, 0u);\n"
        "    ecx = _traffic_manager;\n"
        "    edx = _traffic_matrix;\n"
        "    PUSH32(esp, edx);\n"
        "    PUSH32(esp, eax);\n"
        "    eax = MEM32(esp + 0x58);\n"
        "    PUSH32(esp, eax);\n"
        "    PUSH32(esp, esi);\n"
        "    PUSH32(esp, 0); sub_0016A520(); /* call 0x0016A520 */\n"
        "    { const uint32_t _traffic_result = eax;\n"
        "    recomp_traffic_spawn_checkpoint(\n"
        "        1u, _traffic_manager, _traffic_record, _traffic_type,\n"
        "        _traffic_point, _traffic_matrix, _traffic_result);\n"
        "    eax = _traffic_result;\n"
        "    }}\n"
        "    }"
    )
    if traffic_spawn_marker not in traffic_text:
        if traffic_text.count(traffic_spawn_anchor) != 1:
            raise RuntimeError(
                "traffic random-spawn actor anchor missing or ambiguous"
            )
        traffic_text = traffic_text.replace(
            traffic_spawn_anchor, traffic_spawn_replacement, 1
        )
        documents[traffic_path] = (traffic_text, traffic_newline)
        applied.append((
            "retail traffic random-spawn actor diagnostic", traffic_path
        ))
    else:
        applied.append((
            "retail traffic random-spawn actor diagnostic (already applied)",
            traffic_path,
        ))

    traffic_text, traffic_newline = documents[traffic_path]
    traffic_normal_marker = "/* recomp traffic normal-spawn actor diagnostic */"
    traffic_normal_anchor = (
        "loc_0016C970: ;\n"
        "    eax = MEM32(esp + 0x1C);\n"
        "    ecx = MEM32(eax + edi * 4);\n"
        "    edx = esp + 0x180;\n"
        "    PUSH32(esp, edx);\n"
        "    PUSH32(esp, ebx);\n"
        "    PUSH32(esp, ecx);\n"
        "    ecx = MEM32(esp + 0x6C);\n"
        "    PUSH32(esp, esi);\n"
        "    PUSH32(esp, 0); sub_0016A520(); /* call 0x0016A520 */"
    )
    traffic_normal_replacement = (
        "loc_0016C970: ;\n"
        f"    {traffic_normal_marker}\n"
        "    { const uint32_t _traffic_manager = MEM32(esp + 0x60);\n"
        "    const uint32_t _traffic_record = esi;\n"
        "    const uint32_t _traffic_matrix = esp + 0x180;\n"
        "    eax = MEM32(esp + 0x1C);\n"
        "    ecx = MEM32(eax + edi * 4);\n"
        "    { const uint32_t _traffic_type = ebx;\n"
        "    const uint32_t _traffic_point = ecx;\n"
        "    recomp_traffic_spawn_checkpoint(\n"
        "        2u, _traffic_manager, _traffic_record, _traffic_type,\n"
        "        _traffic_point, _traffic_matrix, 0u);\n"
        "    edx = _traffic_matrix;\n"
        "    PUSH32(esp, edx);\n"
        "    PUSH32(esp, ebx);\n"
        "    PUSH32(esp, ecx);\n"
        "    ecx = MEM32(esp + 0x6C);\n"
        "    PUSH32(esp, esi);\n"
        "    PUSH32(esp, 0); sub_0016A520(); /* call 0x0016A520 */\n"
        "    { const uint32_t _traffic_result = eax;\n"
        "    recomp_traffic_spawn_checkpoint(\n"
        "        3u, _traffic_manager, _traffic_record, _traffic_type,\n"
        "        _traffic_point, _traffic_matrix, _traffic_result);\n"
        "    eax = _traffic_result;\n"
        "    }}\n"
        "    }"
    )
    if traffic_normal_marker in traffic_text:
        old_normal_labels = (
            "    const uint32_t _traffic_point = ebx;\n"
            "    const uint32_t _traffic_matrix = esp + 0x180;\n"
            "    eax = MEM32(esp + 0x1C);\n"
            "    ecx = MEM32(eax + edi * 4);\n"
            "    { const uint32_t _traffic_type = ecx;"
        )
        new_normal_labels = (
            "    const uint32_t _traffic_point = ecx;\n"
            "    const uint32_t _traffic_matrix = esp + 0x180;\n"
            "    eax = MEM32(esp + 0x1C);\n"
            "    ecx = MEM32(eax + edi * 4);\n"
            "    { const uint32_t _traffic_type = ebx;"
        )
        if old_normal_labels in traffic_text:
            traffic_text = traffic_text.replace(
                old_normal_labels, new_normal_labels, 1
            )
            documents[traffic_path] = (traffic_text, traffic_newline)
        early_point_labels = (
            "    const uint32_t _traffic_record = esi;\n"
            "    const uint32_t _traffic_point = ecx;\n"
            "    const uint32_t _traffic_matrix = esp + 0x180;\n"
            "    eax = MEM32(esp + 0x1C);\n"
            "    ecx = MEM32(eax + edi * 4);\n"
            "    { const uint32_t _traffic_type = ebx;"
        )
        late_point_labels = (
            "    const uint32_t _traffic_record = esi;\n"
            "    const uint32_t _traffic_matrix = esp + 0x180;\n"
            "    eax = MEM32(esp + 0x1C);\n"
            "    ecx = MEM32(eax + edi * 4);\n"
            "    { const uint32_t _traffic_type = ebx;\n"
            "    const uint32_t _traffic_point = ecx;"
        )
        if early_point_labels in traffic_text:
            traffic_text = traffic_text.replace(
                early_point_labels, late_point_labels, 1
            )
            documents[traffic_path] = (traffic_text, traffic_newline)
    if traffic_normal_marker not in traffic_text:
        if traffic_text.count(traffic_normal_anchor) != 1:
            raise RuntimeError(
                "traffic normal-spawn actor anchor missing or ambiguous"
            )
        traffic_text = traffic_text.replace(
            traffic_normal_anchor, traffic_normal_replacement, 1
        )
        documents[traffic_path] = (traffic_text, traffic_newline)
        applied.append((
            "retail traffic normal-spawn actor diagnostic", traffic_path
        ))
    else:
        applied.append((
            "retail traffic normal-spawn actor diagnostic (already applied)",
            traffic_path,
        ))

    traffic_text, traffic_newline = documents[traffic_path]
    traffic_detach_marker = "/* recomp traffic detach diagnostic */"
    traffic_detach_anchor = (
        "loc_00169E9C: ;\n"
        "    esi = eax;\n"
        "    edx = MEM32(esi + 0xC);"
    )
    traffic_detach_replacement = (
        "loc_00169E9C: ;\n"
        "    esi = eax;\n"
        f"    {traffic_detach_marker}\n"
        "    recomp_traffic_detach_checkpoint(\n"
        "        0u, esi, MEM32(esp + 0x10), MEM8(esp + 0x14));\n"
        "    edx = MEM32(esi + 0xC);"
    )
    traffic_detach_end_anchor = (
        "loc_00169F29: ;\n"
        "    POP32(esp, edi);"
    )
    traffic_detach_end_replacement = (
        "loc_00169F29: ;\n"
        "    recomp_traffic_detach_checkpoint(\n"
        "        1u, esi, MEM32(esp + 0x10), MEM8(esp + 0x14));\n"
        "    POP32(esp, edi);"
    )
    if traffic_detach_marker not in traffic_text:
        if traffic_text.count(traffic_detach_anchor) != 1:
            raise RuntimeError("traffic detach entry anchor missing or ambiguous")
        if traffic_text.count(traffic_detach_end_anchor) != 1:
            raise RuntimeError("traffic detach exit anchor missing or ambiguous")
        traffic_text = traffic_text.replace(
            traffic_detach_anchor, traffic_detach_replacement, 1
        ).replace(
            traffic_detach_end_anchor, traffic_detach_end_replacement, 1
        )
        documents[traffic_path] = (traffic_text, traffic_newline)
        applied.append(("retail traffic detach diagnostic", traffic_path))
    else:
        applied.append((
            "retail traffic detach diagnostic (already applied)", traffic_path
        ))

    traffic_text, traffic_newline = documents[traffic_path]
    traffic_update_marker = "/* recomp traffic refill-entry diagnostic */"
    traffic_update_anchor = (
        "    xmm6 = xmm6 + xmm1; /* addss */\n"
        "    /* comiss xmm6, MEMF(0x2DC424) - sets EFLAGS */\n"
        "    if (((isnan((double)(xmm6)) || "
        "isnan((double)(MEMF(0x2DC424)))) || "
        "(xmm6 <= MEMF(0x2DC424)))) goto loc_0016CB26; "
        "/* jbe: below or equal (unsigned <=) */"
    )
    traffic_update_replacement = (
        "    xmm6 = xmm6 + xmm1; /* addss */\n"
        f"    {traffic_update_marker}\n"
        "    recomp_traffic_update_checkpoint(\n"
        "        esi, xmm6, xmm3, xmm4, xmm5,\n"
        "        MEMF(esi + 0x1C), MEMF(esi + 0x20), MEMF(esi + 0x24));\n"
        "    /* comiss xmm6, MEMF(0x2DC424) - sets EFLAGS */\n"
        "    if (((isnan((double)(xmm6)) || "
        "isnan((double)(MEMF(0x2DC424)))) || "
        "(xmm6 <= MEMF(0x2DC424)))) goto loc_0016CB26; "
        "/* jbe: below or equal (unsigned <=) */"
    )
    if traffic_update_marker not in traffic_text:
        if traffic_text.count(traffic_update_anchor) != 1:
            raise RuntimeError(
                "traffic refill-entry diagnostic anchor missing or ambiguous"
            )
        traffic_text = traffic_text.replace(
            traffic_update_anchor, traffic_update_replacement, 1
        )
        documents[traffic_path] = (traffic_text, traffic_newline)
        applied.append((
            "retail traffic refill-entry diagnostic", traffic_path
        ))
    else:
        applied.append((
            "retail traffic refill-entry diagnostic (already applied)",
            traffic_path,
        ))

    registry_matches = [
        path for path, (text, _) in documents.items()
        if ("MEM32(0x690AC4)" in text and "MEM32(0x690AC8)" in text)
        or "/* retail scalar shader-key matcher fast path */" in text
    ]
    if len(registry_matches) != 1:
        raise RuntimeError(
            "retail shader-key registry owner: expected one generated function "
            f"shard; found {len(registry_matches)}"
        )
    registry_path = registry_matches[0]
    registry_text, registry_newline = documents[registry_path]
    registry_marker = "/* retail scalar shader-key matcher fast path */"
    registry_sites = (
        (
            "    eax = MEM32(0x690AC4);\n"
            "    PUSH32(esp, edi);\n"
            "    ecx = esi + eax;\n"
            "    PUSH32(esp, 0); sub_00206280(); /* call 0x00206280 */",
            "    eax = MEM32(0x690AC4);\n"
            "    ecx = esi + eax;\n"
            f"    {registry_marker}\n"
            "    if (MEM32(ecx) == 0x00300EB0u) {\n"
            "        eax = (MEM32(ecx + 0x24u) == MEM32(edi)) ? 1u : 0u;\n"
            "    } else {\n"
            "        PUSH32(esp, edi);\n"
            "        PUSH32(esp, 0); sub_00206280(); /* call 0x00206280 */\n"
            "    }",
        ),
        (
            "    edx = MEM32(0x690AC8);\n"
            "    PUSH32(esp, edi);\n"
            "    ecx = esi + edx;\n"
            "    PUSH32(esp, 0); sub_00206280(); /* call 0x00206280 */",
            "    edx = MEM32(0x690AC8);\n"
            "    ecx = esi + edx;\n"
            f"    {registry_marker}\n"
            "    if (MEM32(ecx) == 0x00300EB0u) {\n"
            "        eax = (MEM32(ecx + 0x24u) == MEM32(edi)) ? 1u : 0u;\n"
            "    } else {\n"
            "        PUSH32(esp, edi);\n"
            "        PUSH32(esp, 0); sub_00206280(); /* call 0x00206280 */\n"
            "    }",
        ),
    )
    if registry_marker not in registry_text:
        for before, after in registry_sites:
            if registry_text.count(before) != 1:
                raise RuntimeError(
                    "shader-key matcher fast-path anchor missing or ambiguous"
                )
            registry_text = registry_text.replace(before, after, 1)
        documents[registry_path] = (registry_text, registry_newline)
        applied.append((
            "retail scalar shader-key matcher fast path", registry_path
        ))
    else:
        applied.append((
            "retail scalar shader-key matcher fast path (already applied)",
            registry_path,
        ))
    for path, (text, newline) in list(documents.items()):
        fixed = patch_traffic_vehicle_teardown(patch_apu_voice_parameter_mmio(patch_crt_memmove_entry(text)))
        fixed = patch_apu_voice_update_mmio(fixed)
        # Migrate older generated snapshots too; fresh translation emits the
        # same width-aware helper. Apply after function/patch matching above.
        for width, length in ((1, "ecx"), (2, "ecx * 2"), (4, "ecx * 4")):
            fixed = fixed.replace(f"XBOX_MEMCPY(edi, esi, {length});",
                                  f"XBOX_REP_MOVS(edi, esi, ecx, {width}u);")
        fixed = patch_hud_brush_proportions(fixed)
        fixed = remove_experimental_traffic_admission(fixed)
        fixed = patch_reticle_proportions(fixed)
        fixed = patch_homing_reticle_proportions(fixed)
        fixed = patch_simple_scalar_delete_flags(fixed)
        if fixed != text:
            applied.append(("simple scalar-delete stack flags", path))
        browser_fixed = patch_save_browser_stack_flags(fixed)
        if browser_fixed != fixed:
            applied.append(("retail save browser event stack flags", path))
        fixed = browser_fixed
        save_fixed = patch_save_directory_stack_flags(fixed)
        if save_fixed != fixed:
            applied.append(("retail save directory stack flags", path))
        fixed = save_fixed
        pose_fixed = patch_pose_traversal_capacity(fixed)
        if pose_fixed != fixed:
            applied.append(("RedPose traversal stack capacity guard", path))
        fixed = pose_fixed
        path_spawn_fixed = patch_path_spawn_death_accounting(fixed)
        if path_spawn_fixed != fixed:
            applied.append(("RsPath single-departure accounting", path))
        fixed = path_spawn_fixed
        store_traced = trace_survivor_state(
            trace_screen_flash_setter(trace_pda_store_renderer(fixed)))
        if store_traced != fixed:
            applied.append(("opt-in PDA Store row traversal trace", path))
        fixed = store_traced
        weapon_fixed = patch_weapon_fire_continuation(fixed)
        if weapon_fixed != fixed:
            applied.append(("retail weapon fire continuation repair", path))
        fixed = weapon_fixed
        traced = trace_property_slot_lookup(
            trace_transition_pool(
                trace_extraction_use(
                    trace_extraction_flight(
                        trace_vehicle_audio_update(
                            trace_ai_update_calls(trace_event_abi_calls(trace_teardown_calls(fixed)))
                        )
                    )
                )
            )
        )
        traced = patch_deferred_voice_calls(traced)
        if traced != text:
            documents[path] = (traced, newline)
            applied.append(("opt-in teardown call stack trace", path))
    changed_paths = {path for _, path in applied}
    for path in changed_paths:
        text, newline = documents[path]
        if newline == "\r\n":
            text = text.replace("\n", "\r\n")
        path.write_bytes(text.encode("utf-8"))
    return applied



# Observe actual retail HUD relation queries without replacing faction policy.
PATCHES += (
    GeneratedPatch(
        "retail faction HUD query begin",
        """loc_000F76F0: ;
    PUSH32(esp, ebp);""",
        """loc_000F76F0: ;
    void recomp_preview_faction_query(uint32_t phase, uint32_t target,
                                      uint32_t detail, uint32_t value);
    recomp_preview_faction_query(0u, MEM32(esp + 4u), 0u, 0u);
    PUSH32(esp, ebp);""",
    ),
    GeneratedPatch(
        "retail faction HUD player vehicle",
        """loc_000F7731: ;
    PUSH32(esp, eax);""",
        """loc_000F7731: ;
    recomp_preview_faction_query(1u, edi, esi, eax);
    PUSH32(esp, eax);""",
    ),
    GeneratedPatch(
        "retail faction HUD personal result",
        """loc_000F7739: ;
    if (CMP_EQ(eax, 3))""",
        """loc_000F7739: ;
    recomp_preview_faction_query(2u, edi, 0u, eax);
    if (CMP_EQ(eax, 3))""",
    ),
    GeneratedPatch(
        "retail faction HUD global result",
        """loc_000F777B: ;
    { uint32_t _neg_value""",
        """loc_000F777B: ;
    recomp_preview_faction_query(3u, edi, esi, eax);
    { uint32_t _neg_value""",
    ),
)

# Bounded read-only evidence for rare HQ admission / faction event failures.
PATCHES += (
    GeneratedPatch(
        "retail faction event dispatch observation",
        """loc_0010FD90: ;
    PUSH32(esp, esi);""",
        """loc_0010FD90: ;
    void recomp_preview_faction_event(uint32_t phase, uint32_t event);
    recomp_preview_faction_event(1u, ecx);
    PUSH32(esp, esi);""",
    ),
    GeneratedPatch(
        "retail faction event cancel observation",
        """loc_0010FAA3: ;
    MEM32(eax + 0xA8) = 0xFFFFFFFFu;""",
        """loc_0010FAA3: ;
    void recomp_preview_faction_event(uint32_t phase, uint32_t event);
    recomp_preview_faction_event(2u, eax);
    MEM32(eax + 0xA8) = 0xFFFFFFFFu;""",
    ),
    GeneratedPatch(
        "retail faction event registration observation",
        """loc_00119920: ;
    fp_push((double)SMEM32(edi + 0xA8)); /* fild */""",
        """loc_00119920: ;
    void recomp_preview_faction_event(uint32_t phase, uint32_t event);
    recomp_preview_faction_event(3u, edi);
    fp_push((double)SMEM32(edi + 0xA8)); /* fild */""",
    ),
)

# Keep the original 30 Hz turret roll, without variable-frame feedback bias.
PATCHES += (
    GeneratedPatch(
        "turret camera reference-cadence roll",
        """loc_000A18E5: ;
    fp_push(MEMF(esp + 0x24)); /* fld float */
    fp_top() -= MEMF(esp + 0x20); /* fsub memory */
    PUSH32(esp, ecx);
    eax = esp + 0x154;
    fp_top() *= MEMF(esp + 0x20); /* fmul memory */
    fp_top() += MEMF(esp + 0x24); /* fadd memory */
    MEMF(esp) = (float)fp_top(); fp_pop(); /* fstp */""",
        """loc_000A18E5: ;
    {
        float recomp_turret_camera_roll(uint32_t state, float target,
                                       float old_pitch, float dt);
        float _turret_roll = recomp_turret_camera_roll(
            edi, MEMF(esp + 0x24), MEMF(esp + 0x20), MEMF(ebp + 8));
        PUSH32(esp, ecx);
        eax = esp + 0x154;
        MEMF(esp) = _turret_roll;
    }""",
    ),
)


# Capture physics callback ownership before and after each removal stage.
PATCHES += (
    GeneratedPatch("entity removal history 001C04D0",
        'loc_001C04D0: ;\n    PUSH32(esp, esi);',
        'loc_001C04D0: ;\n    void recomp_entity_removal_checkpoint(uint32_t, uint32_t, uint32_t, uint32_t);\n    recomp_entity_removal_checkpoint(0u, MEM32(esp + 8), ecx, esp);\n    PUSH32(esp, esi);'),
    GeneratedPatch("entity removal history 001C053C",
        'loc_001C053C: ;\n    PUSH32(esp, esi);',
        'loc_001C053C: ;\n    void recomp_entity_removal_checkpoint(uint32_t, uint32_t, uint32_t, uint32_t);\n    recomp_entity_removal_checkpoint(1u, esi, edi, esp);\n    PUSH32(esp, esi);'),
    GeneratedPatch("entity removal history 001C0543",
        'loc_001C0543: ;\n    PUSH32(esp, esi);',
        'loc_001C0543: ;\n    void recomp_entity_removal_checkpoint(uint32_t, uint32_t, uint32_t, uint32_t);\n    recomp_entity_removal_checkpoint(2u, esi, edi, esp);\n    PUSH32(esp, esi);'),
    GeneratedPatch("entity removal history 001C0549",
        'loc_001C0549: ;\n    esp = esp + 0xC;',
        'loc_001C0549: ;\n    void recomp_entity_removal_checkpoint(uint32_t, uint32_t, uint32_t, uint32_t);\n    recomp_entity_removal_checkpoint(3u, esi, edi, esp);\n    esp = esp + 0xC;'),
    GeneratedPatch("entity removal history 001C0554",
        'loc_001C0554: ;\n    MEM16(esi + 6) = MEM16(esi + 6) - 1;',
        'loc_001C0554: ;\n    void recomp_entity_removal_checkpoint(uint32_t, uint32_t, uint32_t, uint32_t);\n    recomp_entity_removal_checkpoint(4u, esi, edi, esp);\n    MEM16(esi + 6) = MEM16(esi + 6) - 1;'),
)

# A canceled proximity event must not signal a mission callback. Valid near/far
# comparisons retain their original strict threshold and horizontal-distance rules.
PATCHES += (
    GeneratedPatch(
        "suppress canceled actor proximity callback",
        "loc_0011096C: ;\n    esp = esp + 4;\n\nloc_0011096F: ;",
        """loc_0011096C: ;
    esp = esp + 4;
    /* Invalid actor: cancellation is not a distance signal. */
    eax = 0;
    POP32(esp, esi);
    esp = ebp;
    POP32(esp, ebp);
    esp += 8; return; /* ret 4 */

loc_0011096F: ;""",
    ),
)

# Distinguish original vehicle deactivation from AI destruction at their call sites.
# Lifted calls push synthetic return addresses, so guest-stack caller inference is invalid.
PATCHES += (
    GeneratedPatch("traffic AI destruction provenance",
        "loc_000676D9: ;\n    PUSH32(esp, esi);",
        "loc_000676D9: ;\n    recomp_traffic_release_checkpoint(0x000676D9u, esi, MEM8(esp));\n    PUSH32(esp, esi);"),
)

# Preserve the raw recenter angle for convergence and bounded opt-in timing evidence.
PATCHES += (
    GeneratedPatch("vehicle recenter raw angle local",
        "loc_0009C610: ;\n    { uint32_t _alu_dst",
        "loc_0009C610: ;\n    float _recomp_reset_raw_angle = 0.0f;\n    { uint32_t _alu_dst"),
    GeneratedPatch("vehicle recenter raw angle capture",
        "loc_0009C6DF: ;\n    recomp_xmm_zero(xmm0v);",
        "loc_0009C6DF: ;\n    _recomp_reset_raw_angle = (float)fp_top();\n    recomp_xmm_zero(xmm0v);"),
    GeneratedPatch("vehicle recenter timing checkpoint",
        "loc_0009C711: ;\n    edx = MEM32(esp + 0x44);",
        """loc_0009C711: ;
    {
        void recomp_vehicle_recenter_checkpoint(uint32_t, float, float, float);
        recomp_vehicle_recenter_checkpoint(esi, MEMF(esp + 8),
            _recomp_reset_raw_angle, MEMF(esp + 0x44));
    }
    edx = MEM32(esp + 0x44);"""),
)

# A tiny elapsed update is not evidence that the camera reached the rear.
# Preserve the original .005-radian tolerance and positive-time guard.
PATCHES += (
    GeneratedPatch("vehicle recenter finishes from remaining angle",
        "loc_0009C725: ;\n    fp_push(MEMF(esp + 0x50)); /* fld float */",
        "loc_0009C725: ;\n    fp_push(_recomp_reset_raw_angle); /* finish from remaining angle, not frame step */"),
)

# Observe the final overlay state after competing setters, without changing it.
PATCHES += (
    GeneratedPatch("screen dimmer paint checkpoint",
        "loc_000F2740: ;\n    esp = esp - 8;",
        """loc_000F2740: ;
    {
        void recomp_screen_dimmer_checkpoint(uint32_t);
        recomp_screen_dimmer_checkpoint(ecx);
    }
    esp = esp - 8;"""),
)


# Identify helicopter births, initialization and path transfers in private traces.
PATCHES += (
    GeneratedPatch("traffic attachment identity 0007255A",
        'loc_0007255A: ;\n    eax = MEM32(edi + 0xC);\n    PUSH32(esp, 1);\n    PUSH32(esp, eax);\n    PUSH32(esp, ebx);\n    ecx = 0x3DC970;\n    PUSH32(esp, 0); sub_00169E10(); /* call 0x00169E10 */',
        'loc_0007255A: ;\n    eax = MEM32(edi + 0xC);\n    PUSH32(esp, 1);\n    PUSH32(esp, eax);\n    PUSH32(esp, ebx);\n    ecx = 0x3DC970;\n    {\n        void recomp_traffic_attach_checkpoint(uint32_t, uint32_t, uint32_t);\n        recomp_traffic_attach_checkpoint(0x0007255Au, MEM32(esp), MEM32(esp + 4));\n    }\n    PUSH32(esp, 0); sub_00169E10(); /* call 0x00169E10 */'),
    GeneratedPatch("traffic attachment identity 00074BC5",
        'loc_00074BC5: ;\n    PUSH32(esp, 1);\n    PUSH32(esp, esi);\n    PUSH32(esp, edi);\n    ecx = 0x3DC970;\n    PUSH32(esp, 0); sub_00169E10(); /* call 0x00169E10 */',
        'loc_00074BC5: ;\n    PUSH32(esp, 1);\n    PUSH32(esp, esi);\n    PUSH32(esp, edi);\n    ecx = 0x3DC970;\n    {\n        void recomp_traffic_attach_checkpoint(uint32_t, uint32_t, uint32_t);\n        recomp_traffic_attach_checkpoint(0x00074BC5u, MEM32(esp), MEM32(esp + 4));\n    }\n    PUSH32(esp, 0); sub_00169E10(); /* call 0x00169E10 */'),
    GeneratedPatch("traffic attachment identity 0007ACB7",
        'loc_0007ACB7: ;\n    ecx = MEM32(esi + 0x40);\n    PUSH32(esp, 1);\n    PUSH32(esp, esi);\n    PUSH32(esp, ecx);\n    ecx = 0x3DC970;\n    PUSH32(esp, 0); sub_00169E10(); /* call 0x00169E10 */',
        'loc_0007ACB7: ;\n    ecx = MEM32(esi + 0x40);\n    PUSH32(esp, 1);\n    PUSH32(esp, esi);\n    PUSH32(esp, ecx);\n    ecx = 0x3DC970;\n    {\n        void recomp_traffic_attach_checkpoint(uint32_t, uint32_t, uint32_t);\n        recomp_traffic_attach_checkpoint(0x0007ACB7u, MEM32(esp), MEM32(esp + 4));\n    }\n    PUSH32(esp, 0); sub_00169E10(); /* call 0x00169E10 */'),
    GeneratedPatch("traffic attachment identity 0007F7F0",
        'loc_0007F7F0: ;\n    PUSH32(esp, 1);\n    PUSH32(esp, ecx);\n    PUSH32(esp, edx);\n    ecx = 0x3DC970;\n    PUSH32(esp, 0); sub_00169E10(); /* call 0x00169E10 */',
        'loc_0007F7F0: ;\n    PUSH32(esp, 1);\n    PUSH32(esp, ecx);\n    PUSH32(esp, edx);\n    ecx = 0x3DC970;\n    {\n        void recomp_traffic_attach_checkpoint(uint32_t, uint32_t, uint32_t);\n        recomp_traffic_attach_checkpoint(0x0007F7F0u, MEM32(esp), MEM32(esp + 4));\n    }\n    PUSH32(esp, 0); sub_00169E10(); /* call 0x00169E10 */'),
    GeneratedPatch("traffic attachment identity 0007F95A",
        'loc_0007F95A: ;\n    eax = MEM32(esi + 8);\n    PUSH32(esp, 1);\n    PUSH32(esp, eax);\n    PUSH32(esp, edi);\n    ecx = 0x3DC970;\n    PUSH32(esp, 0); sub_00169E10(); /* call 0x00169E10 */',
        'loc_0007F95A: ;\n    eax = MEM32(esi + 8);\n    PUSH32(esp, 1);\n    PUSH32(esp, eax);\n    PUSH32(esp, edi);\n    ecx = 0x3DC970;\n    {\n        void recomp_traffic_attach_checkpoint(uint32_t, uint32_t, uint32_t);\n        recomp_traffic_attach_checkpoint(0x0007F95Au, MEM32(esp), MEM32(esp + 4));\n    }\n    PUSH32(esp, 0); sub_00169E10(); /* call 0x00169E10 */'),
    GeneratedPatch("traffic attachment identity 00088BAC",
        'loc_00088BAC: ;\n    ecx = MEM32(esi + 0x44);\n    PUSH32(esp, 0);\n    PUSH32(esp, ecx);\n    PUSH32(esp, ebp);\n    ecx = 0x3DC970;\n    PUSH32(esp, 0); sub_00169E10(); /* call 0x00169E10 */',
        'loc_00088BAC: ;\n    ecx = MEM32(esi + 0x44);\n    PUSH32(esp, 0);\n    PUSH32(esp, ecx);\n    PUSH32(esp, ebp);\n    ecx = 0x3DC970;\n    {\n        void recomp_traffic_attach_checkpoint(uint32_t, uint32_t, uint32_t);\n        recomp_traffic_attach_checkpoint(0x00088BACu, MEM32(esp), MEM32(esp + 4));\n    }\n    PUSH32(esp, 0); sub_00169E10(); /* call 0x00169E10 */'),
    GeneratedPatch("traffic attachment identity 00089260",
        'loc_00089260: ;\n    PUSH32(esp, 0);\n    PUSH32(esp, esi);\n    PUSH32(esp, edi);\n    ecx = 0x3DC970;\n    PUSH32(esp, 0); sub_00169E10(); /* call 0x00169E10 */',
        'loc_00089260: ;\n    PUSH32(esp, 0);\n    PUSH32(esp, esi);\n    PUSH32(esp, edi);\n    ecx = 0x3DC970;\n    {\n        void recomp_traffic_attach_checkpoint(uint32_t, uint32_t, uint32_t);\n        recomp_traffic_attach_checkpoint(0x00089260u, MEM32(esp), MEM32(esp + 4));\n    }\n    PUSH32(esp, 0); sub_00169E10(); /* call 0x00169E10 */'),
    GeneratedPatch("traffic attachment identity 0009266A",
        'loc_0009266A: ;\n    eax = MEM32(edi + 8);\n    PUSH32(esp, 1);\n    PUSH32(esp, eax);\n    PUSH32(esp, ebx);\n    ecx = 0x3DC970;\n    PUSH32(esp, 0); sub_00169E10(); /* call 0x00169E10 */',
        'loc_0009266A: ;\n    eax = MEM32(edi + 8);\n    PUSH32(esp, 1);\n    PUSH32(esp, eax);\n    PUSH32(esp, ebx);\n    ecx = 0x3DC970;\n    {\n        void recomp_traffic_attach_checkpoint(uint32_t, uint32_t, uint32_t);\n        recomp_traffic_attach_checkpoint(0x0009266Au, MEM32(esp), MEM32(esp + 4));\n    }\n    PUSH32(esp, 0); sub_00169E10(); /* call 0x00169E10 */'),
    GeneratedPatch("traffic attachment identity 00094279",
        'loc_00094279: ;\n    PUSH32(esp, 1);\n    PUSH32(esp, esi);\n    PUSH32(esp, edi);\n    ecx = 0x3DC970;\n    PUSH32(esp, 0); sub_00169E10(); /* call 0x00169E10 */',
        'loc_00094279: ;\n    PUSH32(esp, 1);\n    PUSH32(esp, esi);\n    PUSH32(esp, edi);\n    ecx = 0x3DC970;\n    {\n        void recomp_traffic_attach_checkpoint(uint32_t, uint32_t, uint32_t);\n        recomp_traffic_attach_checkpoint(0x00094279u, MEM32(esp), MEM32(esp + 4));\n    }\n    PUSH32(esp, 0); sub_00169E10(); /* call 0x00169E10 */'),
    GeneratedPatch("traffic attachment identity 0016A18C",
        'loc_0016A18C: ;\n    PUSH32(esp, 0);\n    PUSH32(esp, esi);\n    PUSH32(esp, ebx);\n    ecx = ebp;\n    PUSH32(esp, 0); sub_00169E10(); /* call 0x00169E10 */',
        'loc_0016A18C: ;\n    PUSH32(esp, 0);\n    PUSH32(esp, esi);\n    PUSH32(esp, ebx);\n    ecx = ebp;\n    {\n        void recomp_traffic_attach_checkpoint(uint32_t, uint32_t, uint32_t);\n        recomp_traffic_attach_checkpoint(0x0016A18Cu, MEM32(esp), MEM32(esp + 4));\n    }\n    PUSH32(esp, 0); sub_00169E10(); /* call 0x00169E10 */'),
    GeneratedPatch("traffic attachment identity 0016A6A1",
        'loc_0016A6A1: ;\n    ecx = MEM32(esp + 0x10);\n    PUSH32(esp, 1);\n    PUSH32(esp, edi);\n    PUSH32(esp, ebx);\n    PUSH32(esp, 0); sub_00169E10(); /* call 0x00169E10 */',
        'loc_0016A6A1: ;\n    ecx = MEM32(esp + 0x10);\n    PUSH32(esp, 1);\n    PUSH32(esp, edi);\n    PUSH32(esp, ebx);\n    {\n        void recomp_traffic_attach_checkpoint(uint32_t, uint32_t, uint32_t);\n        recomp_traffic_attach_checkpoint(0x0016A6A1u, MEM32(esp), MEM32(esp + 4));\n    }\n    PUSH32(esp, 0); sub_00169E10(); /* call 0x00169E10 */'),
)



# Remove the rejected private traffic-admission experiment on patch replay.
# Retail junction selection is retained, including cross-zone convergence.
def remove_experimental_traffic_admission(text: str) -> str:
    return text.replace("""    {
        int recomp_traffic_helicopter_transfer_allowed(uint32_t, uint32_t, uint32_t);
        if (!recomp_traffic_helicopter_transfer_allowed(esi, ebp, eax))
            goto loc_0009260B;
    }
""", "")


# Opt-in satellite text-color provenance; does not alter guest state.
PATCHES += (
    GeneratedPatch("satellite color checkpoint 000FA646", 'loc_000FA646: ;\n', 'loc_000FA646: ;\n    {\n        void recomp_satellite_color_checkpoint(uint32_t, uint32_t);\n        recomp_satellite_color_checkpoint(0x000FA646u, eax);\n    }\n'),
    GeneratedPatch("satellite color checkpoint 000FA663", 'loc_000FA663: ;\n', 'loc_000FA663: ;\n    {\n        void recomp_satellite_color_checkpoint(uint32_t, uint32_t);\n        recomp_satellite_color_checkpoint(0x000FA663u, edi);\n    }\n'),
    GeneratedPatch("satellite color checkpoint 000FA736", 'loc_000FA736: ;\n', 'loc_000FA736: ;\n    {\n        void recomp_satellite_color_checkpoint(uint32_t, uint32_t);\n        recomp_satellite_color_checkpoint(0x000FA736u, edi);\n    }\n'),
    GeneratedPatch("satellite color checkpoint 000FA741", 'loc_000FA741: ;\n', 'loc_000FA741: ;\n    {\n        void recomp_satellite_color_checkpoint(uint32_t, uint32_t);\n        recomp_satellite_color_checkpoint(0x000FA741u, edi);\n    }\n'),
    GeneratedPatch("satellite color checkpoint 000FA798", 'loc_000FA798: ;\n', 'loc_000FA798: ;\n    {\n        void recomp_satellite_color_checkpoint(uint32_t, uint32_t);\n        recomp_satellite_color_checkpoint(0x000FA798u, edi);\n    }\n'),
    GeneratedPatch("satellite color checkpoint 000FA7A2", 'loc_000FA7A2: ;\n', 'loc_000FA7A2: ;\n    {\n        void recomp_satellite_color_checkpoint(uint32_t, uint32_t);\n        recomp_satellite_color_checkpoint(0x000FA7A2u, edi);\n    }\n'),
    GeneratedPatch("satellite color checkpoint 000FA7DC", 'loc_000FA7DC: ;\n', 'loc_000FA7DC: ;\n    {\n        void recomp_satellite_color_checkpoint(uint32_t, uint32_t);\n        recomp_satellite_color_checkpoint(0x000FA7DCu, edi);\n    }\n'),
    GeneratedPatch("satellite color checkpoint 000FA819", 'loc_000FA819: ;\n', 'loc_000FA819: ;\n    {\n        void recomp_satellite_color_checkpoint(uint32_t, uint32_t);\n        recomp_satellite_color_checkpoint(0x000FA819u, edi);\n    }\n'),
    GeneratedPatch("satellite color checkpoint 000FA8A9", 'loc_000FA8A9: ;\n', 'loc_000FA8A9: ;\n    {\n        void recomp_satellite_color_checkpoint(uint32_t, uint32_t);\n        recomp_satellite_color_checkpoint(0x000FA8A9u, edi);\n    }\n'),
    GeneratedPatch("satellite color checkpoint 000FA9C5", 'loc_000FA9C5: ;\n', 'loc_000FA9C5: ;\n    {\n        void recomp_satellite_color_checkpoint(uint32_t, uint32_t);\n        recomp_satellite_color_checkpoint(0x000FA9C5u, edi);\n    }\n'),
    GeneratedPatch("satellite color checkpoint 000FA9F6", 'loc_000FA9F6: ;\n', 'loc_000FA9F6: ;\n    {\n        void recomp_satellite_color_checkpoint(uint32_t, uint32_t);\n        recomp_satellite_color_checkpoint(0x000FA9F6u, edi);\n    }\n'),
)


PATCHES += (
    GeneratedPatch("CRT x87 remainder intrinsic", 'loc_00238146: ;\n    edx = 0x313630;\n    g_seh_ebp = ebp; sub_0023C1FC(); return; /* tail jmp 0x0023C1FC */\n', 'loc_00238146: ;\n    /* _CIfmod receives dividend in ST(1), divisor in ST(0), and returns\n       one remainder. The generic CRT dispatcher relies on shared EBP,\n       FXAM and XLAT; lifting it corrupts the caller frame and leaks ST(1).\n       Match the native _CIacos bridge rather than changing HUD colors. */\n    {\n        const double divisor = g_fp_stack[g_fp_top & 7u];\n        const double dividend = g_fp_stack[(g_fp_top + 1u) & 7u];\n        if (isinf(dividend) || divisor == 0.0)\n            g_x87_status_word |= 1u; /* masked invalid operation */\n        double remainder = isfinite(dividend) && isinf(divisor)\n            ? dividend : fmod(dividend, divisor);\n        /* Preserve the dividend sign for zero (legacy Windows CRTs lose it). */\n        if (remainder == 0.0) remainder = copysign(0.0, dividend);\n        g_fp_stack[(g_fp_top + 1u) & 7u] = remainder;\n        ++g_fp_top;\n    }\n    esp += 4; return; /* ret */\n'),
)




# GetActorTypeFromSpore has three CMP predecessors sharing one JNE. The
# generated join reused only the last (mobilesam) comparison, so tank and
# homingturret hashes returned TYPE_UNDEFINED. Resolve each incoming compare
# at its own predecessor, retaining the retail default and tank enum paths.
PATCHES += (
    GeneratedPatch(
        "actor type homing turret shared comparison",
        "loc_00011808: ;\n    (void)0; /* cmp eax, 0x7254ACB - flags set for next jcc */\n    goto loc_00011A0B;",
        "loc_00011808: ;\n    if (CMP_NE(eax, 0x7254ACB)) goto loc_0001194D;\n    goto loc_00011A11;",
    ),
    GeneratedPatch(
        "actor type tank shared comparison",
        "loc_0001181F: ;\n    (void)0; /* cmp eax, 0xC285A89 - flags set for next jcc */\n    goto loc_00011A0B;",
        "loc_0001181F: ;\n    if (CMP_NE(eax, 0xC285A89)) goto loc_0001194D;\n    goto loc_00011A11;",
    ),
)


# Resolve prompt regions from live device/context bindings at paint time.
PATCHES += (
    GeneratedPatch('dynamic prompt region input','void sub_0020B900(void)\n{','void sub_0020B900(void)\n{\n    const uint32_t prompt_region = MEM32(esp + 4u);'),
    GeneratedPatch('dynamic prompt region texture','loc_0020B9B4: ;\n','loc_0020B9B4: ;\n    {\n        uint32_t recomp_prompts_region(uint32_t,uint32_t,uint32_t*);\n        uint32_t prompt_hash=edi;\n        uint32_t prompt_texture=recomp_prompts_region(prompt_region,eax,&prompt_hash);\n        if(prompt_texture){\n            edi=prompt_hash;eax=prompt_texture;\n            MEMF(esp+4)=0.0f;MEMF(esp+8)=0.0f;\n            MEMF(esp+0xC)=1.0f;MEMF(esp+0x10)=0.0f;\n            MEMF(esp+0x14)=0.0f;MEMF(esp+0x18)=1.0f;\n        }\n    }\n'),
    GeneratedPatch('dynamic prompt canvas lookup','loc_0020DCE8: ;\n    edi = eax;','loc_0020DCE8: ;\n    edi = eax;\n    {\n        uint32_t recomp_prompts_find_texture(uint32_t,uint32_t);\n        edi=recomp_prompts_find_texture(MEM32(esi+8u),edi);\n    }'),
)


PATCHES += (
    GeneratedPatch('tutorial gameplay body scope begin','loc_001010BB: ;','loc_001010BB: ;\n    {void recomp_controls_prompt_gameplay_body(int);recomp_controls_prompt_gameplay_body(1);}'),
    GeneratedPatch('tutorial gameplay body scope end','loc_00101129: ;','loc_00101129: ;\n    {void recomp_controls_prompt_gameplay_body(int);recomp_controls_prompt_gameplay_body(0);}'),
    GeneratedPatch('movement prompt scope begin','loc_000F6FE5: ;','loc_000F6FE5: ;\n    {void recomp_prompts_movement_scope(int);recomp_prompts_movement_scope(1);}'),
    GeneratedPatch('movement prompt scope end','loc_000F70FF: ;','loc_000F70FF: ;\n    {void recomp_prompts_movement_scope(int);recomp_prompts_movement_scope(0);}'),
)

PATCHES += (
    GeneratedPatch('support prompt scope begin','loc_000F6B6A: ;','loc_000F6B6A: ;\n    {void recomp_prompts_support_scope(int);recomp_prompts_support_scope(1);}'),
    GeneratedPatch('support prompt scope end','loc_000F6B7D: ;','loc_000F6B7D: ;\n    {void recomp_prompts_support_scope(int);recomp_prompts_support_scope(0);}'),
)


# Dynamic editor icons and artwork-proportional menu pulse.
PATCHES += (
    GeneratedPatch('menu prompt glow proportions','    PUSH32(esp, 0); sub_0020A1C0(); /* call 0x0020A1C0 */\n\nloc_000E6D71: ;','    {void recomp_prompts_menu_glow(uint32_t);recomp_prompts_menu_glow(esp);}\n    PUSH32(esp, 0); sub_0020A1C0(); /* call 0x0020A1C0 */\n\nloc_000E6D71: ;'),
    GeneratedPatch('binding icon text column 000E6A96','    PUSH32(esp, 0); sub_0020A260(); /* call 0x0020A260 */\n\nloc_000E6AC8: ;','    {int recomp_controls_row_binding(uint32_t,int*,unsigned short*);\n     if(recomp_controls_row_binding(MEM32(esi+0x40u+(ebp)*4u),0,0))MEMF(esp)+=22.f;}\n    PUSH32(esp, 0); sub_0020A260(); /* call 0x0020A260 */\n\nloc_000E6AC8: ;'),
    GeneratedPatch('binding icon text column 000E6AF1','    PUSH32(esp, 0); sub_0020A260(); /* call 0x0020A260 */\n\nloc_000E6B04: ;','    {int recomp_controls_row_binding(uint32_t,int*,unsigned short*);\n     if(recomp_controls_row_binding(MEM32(esi+0x40u+(ebp)*4u),0,0))MEMF(esp)+=22.f;}\n    PUSH32(esp, 0); sub_0020A260(); /* call 0x0020A260 */\n\nloc_000E6B04: ;'),
    GeneratedPatch('binding icon text column 000E6C37','    PUSH32(esp, 0); sub_0020A260(); /* call 0x0020A260 */\n\nloc_000E6C4A: ;','    {int recomp_controls_row_binding(uint32_t,int*,unsigned short*);\n     if(recomp_controls_row_binding(MEM32(esi+0x40u+(MEM32(esi+0x38u))*4u),0,0))MEMF(esp)+=22.f;}\n    PUSH32(esp, 0); sub_0020A260(); /* call 0x0020A260 */\n\nloc_000E6C4A: ;'),
    GeneratedPatch('binding menu icon rows','loc_000E6E00: ;\n    ecx = esi;','loc_000E6E00: ;\n    {void recomp_controls_paint_binding_icons(uint32_t,float,float);\n     recomp_controls_paint_binding_icons(esi,binding_start_y,binding_row_height);}\n    ecx = esi;'),
)


PATCHES += (
    GeneratedPatch('binding menu saved geometry','void sub_000E67E0(void)\n{','void sub_000E67E0(void)\n{\n    float binding_start_y=0.f,binding_row_height=0.f;'),
    GeneratedPatch('binding menu save row layout','loc_000E69E0: ;','loc_000E69E0: ;\n    binding_start_y=MEMF(esp+0x18u);binding_row_height=MEMF(esp+0x10u);'),
)

PATCHES += (GeneratedPatch('empty host cache volume','loc_0022C5C6: ;','loc_0022C5C6: ;\n    {int recomp_prepare_empty_cache_volume(void);\n     if(recomp_prepare_empty_cache_volume())return;}'),)

PATCHES += (
    GeneratedPatch("Free Cam retains both world loading observers",
        '    RECOMP_TRACE_FUNC(0x001EE610u);\n\nloc_001EE610: ;',
        '    RECOMP_TRACE_FUNC(0x001EE610u);\n    { int recomp_freecam_world_update(uint32_t,uint32_t,uint32_t,uint32_t);\n      if(recomp_freecam_world_update(ecx,MEM32(esp+4u),MEM32(esp+8u),MEM32(esp+12u)))return; }\n\nloc_001EE610: ;'),
    GeneratedPatch("Free Cam hibernation uses both observers",
        'loc_001EC316: ;\n    SET_LO8(eax, 1);',
        'loc_001EC316: ;\n    { int recomp_freecam_stream_keep(uint32_t,int);\n      SET_LO8(eax, recomp_freecam_stream_keep(MEM32(esp+4u),0)?0:1); }'),
    GeneratedPatch("Free Cam preloading uses both observers",
        'loc_001EC391: ;\n    SET_LO8(eax, 0); /* xor self */',
        'loc_001EC391: ;\n    { int recomp_freecam_stream_keep(uint32_t,int);\n      SET_LO8(eax, recomp_freecam_stream_keep(MEM32(esp+4u),1)?1:0); }'),
    GeneratedPatch("Free Cam does not double population maintenance",
        'loc_001EE84D: ;\n    esp = esp + 4;\n    ecx = 0x63DCD0;',
        'loc_001EE84D: ;\n    esp = esp + 4;\n    { int recomp_freecam_world_secondary(void);\n      if(recomp_freecam_world_secondary())goto loc_001EE85A; }\n    ecx = 0x63DCD0;'),
)

PATCHES += (GeneratedPatch("Free Cam final rendered matrix",'loc_00099429: ;\n    ecx = MEM32(esi + 0x1C);','loc_00099429: ;\n    { void recomp_freecam_camera(uint32_t,uint32_t,float);\n      recomp_freecam_camera(esi,esp + 0x20u,MEMF(ebp + 8u)); }\n    ecx = MEM32(esi + 0x1C);'),)

PATCHES += (GeneratedPatch("Discard stale mapped joystick events after context changes",'    RECOMP_TRACE_FUNC(0x000A3AD0u);\n\nloc_000A3AD0: ;','    RECOMP_TRACE_FUNC(0x000A3AD0u);\n    { int recomp_controls_event_current(uint32_t,uint32_t,uint32_t,uint32_t);\n      if(!recomp_controls_event_current(MEM32(0x413F6Cu),MEM32(0x413F68u),MEM32(0x323ACCu),MEM32(0x323AD0u))){\n          eax=0;esp+=12;return; /* consume stale event, ret 8 */\n      } }\n\nloc_000A3AD0: ;'),)

PATCHES += (GeneratedPatch("F10 HUD render-only visibility",'loc_0020B820: ;\n    ecx = MEM32(eax + 8);','loc_0020B820: ;\n    { int recomp_controls_hide_hud_brush(uint32_t,uint32_t);\n      if(recomp_controls_hide_hud_brush(eax,MEM32(eax + 4u)))goto loc_0020B851; }\n    ecx = MEM32(eax + 8);'),)

PATCHES += (
    GeneratedPatch('Cargo helicopter reachable Mi26 dock','    g_seh_ebp = ebp; sub_00164680(); return; /* tail jmp 0x00164680 */','    { uint32_t cargo_seat=ecx,cargo_matrix=MEM32(esp+4u),cargo_dock=MEM32(esp+8u);\n      g_seh_ebp = ebp; sub_00164680();\n      if(LO8(eax) && MEM32(cargo_seat+0xD8u)==cargo_dock){\n          void recomp_cargo_ground_dock(uint32_t,uint32_t);\n          recomp_cargo_ground_dock(cargo_seat,cargo_matrix);\n      }\n      return;\n    }'),
    GeneratedPatch('AI helicopter operate dock height','loc_000847D3: ;','loc_000847D3: ;\n    { int recomp_ai_helicopter_dock_reachable(uint32_t,uint32_t,float);\n      if(!recomp_ai_helicopter_dock_reachable(ebx,eax,MEMF(esp+0x1C))){ebp=2;goto loc_0008480E;} }'),
    GeneratedPatch('AI helicopter entry state dock height','loc_000866EC: ;','loc_000866EC: ;\n    { int recomp_ai_helicopter_dock_reachable(uint32_t,uint32_t,float);\n      if(!recomp_ai_helicopter_dock_reachable(edi,eax,MEMF(esp+0x14))){ebp=2;goto loc_0008671B;} }'),
    GeneratedPatch('Cargo gunner completion after enter update','loc_00166284: ;\n    ecx = esi;','loc_00166284: ;\n    { int recomp_cargo_defer_gunner_completion(uint32_t,uint32_t);\n      if(recomp_cargo_defer_gunner_completion(esi,ebp))MEM8(esp+0xFu)=0; }\n    ecx = esi;'),
    GeneratedPatch('Cargo player gunner camera visibility','loc_001667B1: ;\n    MEMF(esi + 0x14) = (float)fp_top(); fp_pop(); /* fstp */','loc_001667B1: ;\n    { void recomp_cargo_hide_gunner(uint32_t,uint32_t);\n      recomp_cargo_hide_gunner(MEM32(esi+4u),ebx); }\n    MEMF(esi + 0x14) = (float)fp_top(); fp_pop(); /* fstp */'),
    GeneratedPatch('Cargo Pave Low existing gunner stations','loc_00163EBF: ;\n    POP32(esp, edi);','loc_00163EBF: ;\n    { void recomp_cargo_configure_seat(uint32_t); recomp_cargo_configure_seat(esi); }\n    POP32(esp, edi);'),
    GeneratedPatch('Cargo helicopter authored boarding points','loc_00163BB9: ;\n    PUSH32(esp, ebp);','loc_00163BB9: ;\n    { uint32_t recomp_cargo_dock(uint32_t,uint32_t); eax=recomp_cargo_dock(esi,eax); }\n    PUSH32(esp, ebp);'),
    GeneratedPatch('Cargo helicopter immediate player boarding','loc_00166130: ;\n    PUSH32(esp, ecx);','loc_00166130: ;\n    { uint32_t recomp_cargo_entry_type(uint32_t,uint32_t,uint32_t);\n      MEM32(esp+8u)=recomp_cargo_entry_type(ecx,MEM32(esp+4u),MEM32(esp+8u)); }\n    PUSH32(esp, ecx);'),
    GeneratedPatch('Cargo helicopter hidden player pilot','loc_00162A65: ;\n    eax = MEM32(ebx);','loc_00162A65: ;\n    { int recomp_cargo_player(uint32_t,uint32_t);\n      if(recomp_cargo_player(MEM32(ebp+4u),ebx))goto loc_00162AAF; }\n    eax = MEM32(ebx);'),
    GeneratedPatch('Cargo helicopter player exit without animation','loc_00166905: ;\n    ecx = MEM32(ebp + 0x6B0);','loc_00166905: ;\n    { int recomp_cargo_skip_door(uint32_t,uint32_t);\n      if(recomp_cargo_skip_door(ebx,ebp))goto loc_00166923; }\n    ecx = MEM32(ebp + 0x6B0);'),
)

PATCHES += (GeneratedPatch('FMV streams follow Music volume before decoding', 'loc_00217910: ;', 'loc_00217910: ;\n    { void recomp_movie_apply_volume(uint32_t); recomp_movie_apply_volume(ecx); }'),)

PATCHES += (
    GeneratedPatch('Scenery draw distance range 001700FD', 'loc_001700FD: ;\n    fp_push(MEMF(0x2FC664)); /* fld float */', 'loc_001700FD: ;\n    fp_push(recomp_options_scale_object_distance(MEMF(0x2FC664), eax));'),
    GeneratedPatch('Scenery draw distance range 00170124', 'loc_00170124: ;\n    fp_push(MEMF(0x2FC658)); /* fld float */', 'loc_00170124: ;\n    fp_push(recomp_options_scale_object_distance(MEMF(0x2FC658), eax));'),
    GeneratedPatch('Scenery draw distance range 00170134', 'loc_00170134: ;\n    fp_push(MEMF(0x2FC660)); /* fld float */', 'loc_00170134: ;\n    fp_push(recomp_options_scale_object_distance(MEMF(0x2FC660), eax));'),
    GeneratedPatch('Scenery draw distance range 0017015D', 'loc_0017015D: ;\n    fp_push(MEMF(0x2FC658)); /* fld float */', 'loc_0017015D: ;\n    fp_push(recomp_options_scale_object_distance(MEMF(0x2FC658), eax));'),
    GeneratedPatch('Scenery draw distance range 00170182', 'loc_00170182: ;\n    fp_push(MEMF(0x2FC658)); /* fld float */', 'loc_00170182: ;\n    fp_push(recomp_options_scale_object_distance(MEMF(0x2FC658), eax));'),
    GeneratedPatch('Scenery draw distance range 0017018B', 'loc_0017018B: ;\n    fp_push(MEMF(0x2FC65C)); /* fld float */', 'loc_0017018B: ;\n    fp_push(recomp_options_scale_object_distance(MEMF(0x2FC65C), eax));'),
    GeneratedPatch('Scenery streaming grid covers selected radius', 'loc_001ED240: ;\n    PUSH32(esp, 0x43A00000);', 'loc_001ED240: ;\n    esp -= 4u;\n    MEMF(esp) = 300.0f * recomp_options_object_distance_multiplier() + 20.0f; /* largest scenery radius + grid margin */'),
)

PATCHES += (
    GeneratedPatch('Scenery distance commits at full world initialization',
        'loc_001ED160: ;\n    PUSH32(esp, ebx);',
        'loc_001ED160: ;\n    { int recomp_options_begin_world_load(void);\n      void recomp_options_refresh_retail_camera_projection(void);\n      if (recomp_options_begin_world_load())\n          recomp_options_refresh_retail_camera_projection(); }\n    PUSH32(esp, ebx);'),
    GeneratedPatch('Reused model LOD refreshes changed spawn distance',
        'loc_00220020: ;\n    eax = MEM32(esp + 4);\n    recomp_xmm_loadss(xmm0v, esp + 8); /* movss */\n    MEM32(ecx + 0x2C) = eax;',
        'loc_00220020: ;\n    eax = MEM32(esp + 4);\n    recomp_xmm_loadss(xmm0v, esp + 8); /* movss */\n    /* Cached models can survive a world reload. Recalculate their original\n     * LOD table on the next render when the authored inputs change. */\n    if (MEM32(ecx + 0x2C) != eax || MEM32(ecx + 0x30) != MEM32(esp + 8))\n        MEM8(ecx + 0x28) &= (uint8_t)~8u;\n    MEM32(ecx + 0x2C) = eax;'),
)

# Support tooltip semantics. Tank driver death retains retail latched controls.
PATCHES += (
    GeneratedPatch('Support carousel equipped Fire binding during fade', 'loc_000FCC54: ;', 'loc_000FCC54: ;\n    {void recomp_prompts_support_fire_scope(int);recomp_prompts_support_fire_scope(1);}'),
    GeneratedPatch('Support carousel equipped Fire binding scope end', 'loc_000FCC64: ;', 'loc_000FCC64: ;\n    {void recomp_prompts_support_fire_scope(int);recomp_prompts_support_fire_scope(0);}'),
)


# Audio failure is completion after the calling script, never nested reentry.
# These replacements depend on trace_event_abi_calls, so they run after the
# wrapper pass rather than in the initial patch table.
DEFERRED_VOICE_PATCHES = tuple(
    GeneratedPatch(
        f"Deferred voice fallback {site}",
        f"loc_{site}: ;\n"
        "    PUSH32(esp, ebx);\n"
        f"    ecx = {owner};\n"
        "    { const uint32_t _event_call_esp = esp;\n"
        "      const uint32_t _event_call_esi = esi;\n"
        "      const uint32_t _event_call_edi = edi;\n"
        "      void recomp_event_abi_checkpoint(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);\n"
        "      PUSH32(esp, 0); sub_00113A50(); /* call 0x00113A50 */\n"
        "      recomp_event_abi_checkpoint(0x0011EA30u, 0x00113A50u, _event_call_esp + 4u, _event_call_esi, _event_call_edi); }\n\n",
        f"loc_{site}: ;\n"
        "    PUSH32(esp, ebx);\n"
        f"    ecx = {owner};\n"
        "    { const uint32_t _event_call_esp = esp;\n"
        "      const uint32_t _event_call_esi = esi;\n"
        "      const uint32_t _event_call_edi = edi;\n"
        "      void recomp_event_abi_checkpoint(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);\n"
        "      void recomp_voice_fallback_call(void);\n"
        "      PUSH32(esp, 0); recomp_voice_fallback_call(); /* deferred failure, ret 4 */\n"
        "      recomp_event_abi_checkpoint(0x0011EA30u, 0x00113A50u, _event_call_esp + 4u, _event_call_esi, _event_call_edi); }\n\n",
    )
    for site, owner in (("0011EB50", "edi"), ("0011EB5E", "eax"))
)


def patch_deferred_voice_calls(text: str) -> str:
    if "void sub_0011EA30(void)" not in text:
        return text
    for patch in DEFERRED_VOICE_PATCHES:
        before_count = text.count(patch.before)
        after_count = text.count(patch.after)
        if before_count == 1 and after_count == 0:
            text = text.replace(patch.before, patch.after, 1)
        elif before_count != 0 or after_count != 1:
            raise RuntimeError(f"{patch.name}: expected one ABI-wrapped call")
    return text


PATCHES += (GeneratedPatch("Private HQ silent-request injection", 'loc_0011EA77: ;\n    esp = esp + 8;', 'loc_0011EA77: ;\n    esp = esp + 8;\n    { int recomp_test_fail_hq_voice(uint32_t);\n      if (recomp_test_fail_hq_voice(ebp)) { eax=0; goto loc_0011EA97; } }'),)

PATCHES += (GeneratedPatch("HQ entry callback evidence", 'loc_00113A9B: ;\n    esp = esp + 0x20;', 'loc_00113A9B: ;\n    { void recomp_hq_callback_checkpoint(uint32_t,uint32_t,uint32_t);\n      recomp_hq_callback_checkpoint(esi,edi,eax); }\n    esp = esp + 0x20;'),)

PATCHES += (
    GeneratedPatch('HQ briefing ScriptInit begin evidence','loc_00175860: ;','loc_00175860: ;\n    { void recomp_hq_init_checkpoint(uint32_t,uint32_t); recomp_hq_init_checkpoint(0,esi); }'),
    GeneratedPatch('HQ briefing ScriptInit return evidence','loc_0017586F: ;','loc_0017586F: ;\n    { void recomp_hq_init_checkpoint(uint32_t,uint32_t); recomp_hq_init_checkpoint(1,esi); }'),
)

# Capture parse/top-level script errors before lua_dobuffer's callalert removes them.
PATCHES += (GeneratedPatch('Lua script load error preview evidence',
    'loc_001DC566: ;\n    eax = edi;',
    'loc_001DC566: ;\n    { void recomp_lua_pcall_checkpoint(uint32_t,uint32_t,uint32_t);\n      recomp_lua_pcall_checkpoint(esi,edi,0); }\n    eax = edi;'),)


# Reading a dossier is not acknowledgement of the card's capture animation.
# The retail getter mutates this transient flag even for a still-free card;
# capture then skips its intel animation until a save reload rebuilds the flags.
PATCHES += (
    GeneratedPatch("PDA dossier reads preserve pending capture intel",
        "    edi = edi << 3;\n    MEM8(edx + edi + 0xDB48) = 1;\n    ecx = MEM32(esi + 0x24);",
        "    edi = edi << 3;\n    /* Reading intel must not consume the capture animation. */\n    ecx = MEM32(esi + 0x24);"),
    GeneratedPatch("PDA intel entry evidence",
        "loc_000CDEE2: ;\n    POP32(esp, edi);",
        "loc_000CDEE2: ;\n    { void recomp_datapod_intel_checkpoint(uint32_t, uint32_t); recomp_datapod_intel_checkpoint(esi, 1u); }\n    POP32(esp, edi);"),
    GeneratedPatch("PDA intel award evidence",
        "    MEMF(esi + 0x3BC) = xmm1; /* movss */\n    goto loc_000CB470;",
        "    MEMF(esi + 0x3BC) = xmm1; /* movss */\n    { void recomp_datapod_intel_checkpoint(uint32_t, uint32_t); recomp_datapod_intel_checkpoint(esi, 2u); }\n    goto loc_000CB470;"),
    GeneratedPatch("PDA intel exit evidence",
        "loc_000CB0FC: ;\n    ecx = esi;",
        "loc_000CB0FC: ;\n    { void recomp_datapod_intel_checkpoint(uint32_t, uint32_t); recomp_datapod_intel_checkpoint(esi, 3u); }\n    ecx = esi;"),
)


# Start each gameplay scene with initialized color. Water blends before the sky;
# if no seabed is drawn, retaining color exposes an unrelated previous frame.
# Keep this at Game::Enter so only the existing scene-start clear changes:
# partial target clears, sky/depth copies and satellite passes remain untouched.
PATCHES += (GeneratedPatch("Initialize gameplay color before translucent water",
    "loc_001848E7: ;\n    PUSH32(esp, edi);\n    PUSH32(esp, 0); sub_0020FAA0(); /* call 0x0020FAA0 */",
    "loc_001848E7: ;\n    /* Water can cover pixels without an opaque seabed behind it. */\n    PUSH32(esp, 1); /* RedRenderer::SetTargetClear(true) */\n    PUSH32(esp, 0); sub_0020FAA0(); /* call 0x0020FAA0 */"),)



# Free-camera time control changes only the game clock, never real/UI time.
PATCHES += (GeneratedPatch("Free camera simulation tick scale",
    "loc_0018047F: ;\n    esi = eax;\n    PUSH32(esp, esi);",
    "loc_0018047F: ;\n    esi = eax;\n    { uint32_t recomp_freecam_game_ticks(uint32_t);\n      PUSH32(esp, recomp_freecam_game_ticks(esi)); }"),)


PATCHES += (GeneratedPatch("Acknowledgements retail body text scale",
    "loc_000E6A96: ;\n    xmm0 = (float)(int32_t)ebp;",
    "loc_000E6A96: ;\n    { void recomp_ack_text_scale(uint32_t,uint32_t,int); recomp_ack_text_scale(esi,ebp,0); }\n    xmm0 = (float)(int32_t)ebp;"),
    GeneratedPatch("Acknowledgements retail foreground text scale",
    "loc_000E6AF1: ;\n    eax = MEM32(esp + 0x24);",
    "loc_000E6AF1: ;\n    { void recomp_ack_text_scale(uint32_t,uint32_t,int); recomp_ack_text_scale(esi,ebp,1); }\n    eax = MEM32(esp + 0x24);"),)




# Canonicalize the optional mission alias before PlayManaged compares a live
# handle's config name. The DJ retains the authored request, so applying the
# setting also takes effect on subsequent updates of an already silent mission.
PATCHES += (GeneratedPatch("Optional NW Mafia 2 managed music cue alias",
    "    recomp_xact_managed_checkpoint(1u, MEM32(esp + 8), MEM32(esp + 0xC), 0u);\n    ecx = MEM32(0x690AC0);",
    "    recomp_xact_managed_checkpoint(1u, MEM32(esp + 8), MEM32(esp + 0xC), 0u);\n"
    "    { uint32_t recomp_original_bug_cue_alias(uint32_t);\n"
    "      MEM32(esp + 0xCu) = recomp_original_bug_cue_alias(MEM32(esp + 0xCu)); }\n"
    "    ecx = MEM32(0x690AC0);"),)



# Retail Ui_DisplayDateline restarts its timer but retains the sound counter.
PATCHES += (GeneratedPatch("Optional dateline typing sound reset",
    "    MEM32(0x363EE4) = eax;\n    MEMF(0x363EE8) = xmm0; /* movss */",
    "    MEM32(0x363EE4) = eax;\n    MEMF(0x363EE8) = xmm0; /* movss */\n"
    "    { uint32_t recomp_original_bug_dateline_chars(uint32_t);\n"
    "      MEM32(0x363EECu) = recomp_original_bug_dateline_chars(MEM32(0x363EECu)); }"),)



# getobjname returns through an interior epilogue when a local name is found.
PATCHES += (GeneratedPatch("Restore Lua local-name error epilogue",
    "void sub_001DE320(void) { esp += 4; /* 0x001DE320: not detected; minimal guest ret */ }",
    """void sub_001DE320(void)
{
    /* Retail 5F 5E B8 DC B9 2F 00 5D C3. Restore the caller's Lua state
     * in ESI before luaG_typeerror constructs the error message. */
    RECOMP_TRACE_FUNC(0x001DE320u);
    POP32(esp, edi);
    POP32(esp, esi);
    eax = 0x002FB9DCu; /* "local" */
    POP32(esp, g_seh_ebp);
    esp += 4;
    return;
}
"""),)



# Patch only the fingerprinted NW Allies 1 source at the protected parser boundary.
PATCHES += (
    GeneratedPatch("Optional WMD inspector script preparation",
        "    RECOMP_TRACE_FUNC(0x001DC520u);\n\nloc_001DC520: ;",
        "    RECOMP_TRACE_FUNC(0x001DC520u);\n"
        "    const uint32_t script_stack = esp;\n"
        "    const uint32_t script_source = MEM32(esp + 8u);\n"
        "    const uint32_t script_size = MEM32(esp + 12u);\n"
        "    uint32_t patched_size = script_size;\n"
        "    uint32_t recomp_prepare_mission_script(uint32_t, uint32_t, uint32_t *);\n"
        "    void recomp_title_heap_free(uint32_t);\n"
        "    const uint32_t script_patch = recomp_prepare_mission_script(script_source, script_size, &patched_size);\n"
        "    if (script_patch) {\n"
        "        MEM32(esp + 8u) = script_patch;\n"
        "        MEM32(esp + 12u) = patched_size;\n"
        "    }\n\nloc_001DC520: ;"),
    GeneratedPatch("Release optional WMD script after protected parse",
        "loc_001DC54E: ;\n    edi = eax;",
        "loc_001DC54E: ;\n"
        "    if (script_patch) {\n"
        "        MEM32(script_stack + 8u) = script_source;\n"
        "        MEM32(script_stack + 12u) = script_size;\n"
        "        recomp_title_heap_free(script_patch);\n"
        "    }\n    edi = eax;"),
)


PATCHES += (GeneratedPatch("Boid simulation world tick",
    '    RECOMP_TRACE_FUNC(0x001702D0u);\n\nloc_001702D0: ;',
    '    RECOMP_TRACE_FUNC(0x001702D0u);\n    { void recomp_boids_tick(uint32_t,uint32_t,uint32_t);\n      recomp_boids_tick(MEM32(esp+4u),MEM32(esp+8u),MEM32(esp+12u)); }\n\nloc_001702D0: ;'),)


# Developer battle controls override runtime threat decisions, not faction standings.
PATCHES += (
    GeneratedPatch("Developer battle relationship policy",
        "    RECOMP_TRACE_FUNC(0x00067C10u);\n\nloc_00067C10: ;",
        "    RECOMP_TRACE_FUNC(0x00067C10u);\n"
        "    { int recomp_dev_battle_protected(uint32_t,uint32_t);\n"
        "      if(recomp_dev_battle_protected(ecx,MEM32(esp+8u))){fp_push(1.0);esp+=12u;return;} }\n\nloc_00067C10: ;"),
    GeneratedPatch("Developer battle innate attitude policy",
        "    RECOMP_TRACE_FUNC(0x00066560u);\n\nloc_00066560: ;",
        "    RECOMP_TRACE_FUNC(0x00066560u);\n"
        "    { int recomp_dev_battle_protected(uint32_t,uint32_t);\n"
        "      if(recomp_dev_battle_protected(ecx,MEM32(esp+4u))){eax=3u;esp+=8u;return;} }\n\nloc_00066560: ;"),
    GeneratedPatch("Developer battle forced attack policy",
        "loc_0006C0A8: ;\n    if (CMP_EQ(edi, MEM32(esi + 0x3B4)))",
        "loc_0006C0A8: ;\n"
        "    { int recomp_dev_battle_protected(uint32_t,uint32_t);\n"
        "      if(recomp_dev_battle_protected(esi,edi))goto loc_0006C0FE; }\n"
        "    if (CMP_EQ(edi, MEM32(esi + 0x3B4)))"),
    GeneratedPatch("Developer battle retained target policy",
        "loc_0006C175: ;\n    ecx = MEM32(esi + 0x3FC);",
        "loc_0006C175: ;\n    ecx = MEM32(esi + 0x3FC);\n"
        "    { int recomp_dev_battle_protected(uint32_t,uint32_t);\n"
        "      if(recomp_dev_battle_protected(esi,edi))ecx=0x3F800000u; }"),
)


PATCHES += (GeneratedPatch("Developer spawn world-thread checkpoint",
    '    RECOMP_TRACE_FUNC(0x001702D0u);\n    { void recomp_boids_tick',
    '    RECOMP_TRACE_FUNC(0x001702D0u);\n    { void recomp_dev_spawn_tick(void); recomp_dev_spawn_tick(); }\n    { void recomp_boids_tick'),)

# The legal-credit movement mode (7) branches into the default arm, immediately
# before the shared AddName commit/epilogue. It is not a standalone empty return.
PATCHES += (GeneratedPatch("Credits stationary name placement continuation",
    "void sub_000E0B58(void) { esp += 4; /* 0x000E0B58: not detected; minimal guest ret */ }",
    "void sub_000E0B58(void)\n{\n"
    "    /* Retail F30F114E14: commit zero movement, then position/text and unwind. */\n"
    "    MEMF(esi + 0x14) = xmm1;\n"
    "    sub_000E0B5D(); return;\n}"),)


TRANSLATOR_INLINED_PATCHES["Boid simulation world tick"] = (
    '    RECOMP_TRACE_FUNC(0x001702D0u);\n'
    '    { void recomp_dev_spawn_tick(void); recomp_dev_spawn_tick(); }\n'
    '    { void recomp_boids_tick(uint32_t,uint32_t,uint32_t);\n'
    '      recomp_boids_tick(MEM32(esp+4u),MEM32(esp+8u),MEM32(esp+12u)); }\n\nloc_001702D0: ;')


# A failed Prepare owns a wrapper/source even though it never returned a handle.
# Release it through the manager's normal pool path before returning failure.
PATCHES += (GeneratedPatch("XACT failed prepare releases positional source",
    "loc_00225923: ;\n    POP32(esp, edi);",
    "loc_00225923: ;\n"
    "    { void recomp_xact_prepare_failure(uint32_t,uint32_t,uint32_t);\n"
    "      recomp_xact_prepare_failure(edi,esi,eax); }\n"
    "    PUSH32(esp, esi);\n    ecx = edi;\n"
    "    PUSH32(esp, 0); sub_00226830(); /* release failed preparation */\n"
    "    POP32(esp, edi);"),)


# Recover only after the real queue is empty, while its existing lock is held.
# Unregistration and manager retirement follow their ordinary guest paths.
PATCHES += (GeneratedPatch("XACT terminal cue stop reconciliation",
    "loc_0027EA74: ;\n    MEM32(ebp + -4) = 0x80004005u;",
    "loc_0027EA74: ;\n"
    "    { int recomp_xact_recover_stop_notification(uint32_t,uint32_t,uint32_t);\n"
    "      if (recomp_xact_recover_stop_notification(MEM32(ebp + 8),\n"
    "              MEM32(ebp + 12), MEM32(ebp + 16))) {\n"
    "          PUSH32(esp, MEM32(ebp + 16));\n"
    "          PUSH32(esp, MEM32(ebp + 8));\n"
    "          PUSH32(esp, 0); sub_0027E9C4();\n"
    "          goto loc_0027EA7B;\n"
    "      } }\n"
    "    MEM32(ebp + -4) = 0x80004005u;"),)


# Both network totals outlive the per-world path table in the retail singleton.
PATCHES += (GeneratedPatch("Traffic world shutdown clears discarded path budgets",
    "loc_0016A346: ;\n    eax = 0; /* xor self */",
    "loc_0016A346: ;\n"
    "    { void recomp_traffic_stop_network_totals(uint32_t);\n"
    "      recomp_traffic_stop_network_totals(esi); }\n"
    "    eax = 0; /* xor self */"),)


# A free card cannot have had its future capture animation acknowledged.
PATCHES += (
    GeneratedPatch("PDA previous chapters do not acknowledge unresolved cards",
        "loc_000CDEB2: ;\n    esp = esp + 0x14;\n    PUSH32(esp, 1);",
        "loc_000CDEB2: ;\n    esp = esp + 0x14;\n    /* Only a resolved card has a capture/death to acknowledge. */\n    ecx = MEM32(esi + 0x24);\n    PUSH32(esp, MEM32(ecx + ebp + 0xDAFC) != 1u);"),
    GeneratedPatch("PDA previous chapter transient acknowledgement matches persistence",
        "    MEM8(ecx + ebp + 0xDAF8) = 1;",
        "    MEM8(ecx + ebp + 0xDAF8) = MEM32(ecx + ebp + 0xDAFC) != 1u;"),
    GeneratedPatch("PDA unresolved cards clear stale acknowledgement",
        "loc_000C97EA: ;\n    SET_LO8(eax, MEM8(esi + 4));",
        "loc_000C97EA: ;\n    /* Repair acknowledgements inherited by an unresolved card (including NG+).\n     * Already captured/killed cards keep their saved animation state. */\n    if (MEM32(esi + 8) == 1u && MEM8(esi + 4)) {\n        MEM8(esi + 4) = 0;\n        PUSH32(esp, edi);\n        eax = esp + 0x18;\n        PUSH32(esp, eax);\n        eax = esp + 0x2C;\n        PUSH32(esp, 0x2E86A8);\n        PUSH32(esp, eax);\n        PUSH32(esp, 0); sub_002370B8();\n        eax = esp + 0x34;\n        PUSH32(esp, eax);\n        PUSH32(esp, 0); sub_001F29F0();\n        esp += 0x14;\n        PUSH32(esp, 0);\n        PUSH32(esp, eax);\n        ecx = 0x414150;\n        PUSH32(esp, 0); sub_00121E00();\n        ecx = 0;\n    }\n    SET_LO8(eax, MEM8(esi + 4));"),
)


PATCHES += (GeneratedPatch("Canvas outline physical pixel coverage",
    '    xmm0 = xmm0 * xmm2; /* mulss */\n    MEMF(esp + 0x64) = xmm0; /* movss */\n    ecx = MEM32(esp + 0x64);\n    xmm1 = xmm1 * xmm2; /* mulss */\n    recomp_xmm_loadss(xmm2v, esi + eax * 8); /* movss */',
    '    xmm0 = xmm0 * xmm2; /* mulss */\n    /* Keep canvas outlines at least one physical pixel after brush scaling. */\n    { float recomp_ui_outline_scale(float,float,float);\n      xmm0 *= recomp_ui_outline_scale(MEMF(esp + 0xB8) * 2.0f,\n          MEMF(0x7AB614), MEMF(0x7AB618)); }\n    MEMF(esp + 0x64) = xmm0; /* movss */\n    ecx = MEM32(esp + 0x64);\n    xmm1 = xmm1 * xmm2; /* mulss */\n    { float recomp_ui_outline_scale(float,float,float);\n      xmm1 *= recomp_ui_outline_scale(MEMF(esp + 0xB8) * 2.0f,\n          MEMF(0x7AB61C), MEMF(0x7AB620)); }\n    recomp_xmm_loadss(xmm2v, esi + eax * 8); /* movss */'),)


# Keep the original weather weight for the optional sky_clouds/empty fade.
PATCHES += (
    GeneratedPatch("Optional cloud transition remembers outgoing weather weight",
        "loc_00158570: ;\n    recomp_xmm_loadss(xmm1v, 0x30EA34);",
        "loc_00158570: ;\n"
        "    { void recomp_original_bug_sky_blend(float);\n"
        "      recomp_original_bug_sky_blend(MEMF(esp + 0xC)); }\n"
        "    recomp_xmm_loadss(xmm1v, 0x30EA34);"),
    GeneratedPatch("Optional cloud transition retains fading texture",
        "loc_00158390: ;\n    recomp_xmm_loadss(xmm0v, esp + 0x10);",
        "loc_00158390: ;\n"
        "    { void recomp_original_bug_cloud_params(volatile uint32_t *, volatile uint32_t *);\n"
        "      recomp_original_bug_cloud_params(&MEM32(esp + 4), &MEM32(esp + 8)); }\n"
        "    recomp_xmm_loadss(xmm0v, esp + 0x10);"),
    GeneratedPatch("Optional cloud transition fades layer coverage",
        "    MEM32(esp + 0x130) = eax;\n    PUSH32(esp, 0); sub_00288AF0(); /* call 0x00288AF0 */\n\nloc_00159AE2: ;",
        "    MEM32(esp + 0x130) = eax;\n"
        "    { void recomp_original_bug_cloud_colors(volatile float *);\n"
        "      recomp_original_bug_cloud_colors(&MEMF(edx)); }\n"
        "    PUSH32(esp, 0); sub_00288AF0(); /* call 0x00288AF0 */\n\nloc_00159AE2: ;"),
)

# Keep the retail CPU heap intact while giving large mods a separate GPU bank.
PATCHES += (
    GeneratedPatch("Optional extended graphics pool allocation",
        "loc_0020F9C0: ;\n    PUSH32(esp, 0x404);",
        "loc_0020F9C0: ;\n"
        "    { extern uint32_t xbox_GetExtendedGraphicsPool(void);\n"
        "      eax = xbox_GetExtendedGraphicsPool();\n"
        "      if (eax) goto loc_0020F9D3; }\n"
        "    PUSH32(esp, 0x404);"),
    GeneratedPatch("Optional extended graphics pool capacity",
        "loc_0020F9D3: ;\n    PUSH32(esp, 0xFA0);\n"
        "    PUSH32(esp, 0x7AD608);\n    PUSH32(esp, 0x1680000);",
        "loc_0020F9D3: ;\n"
        "    /* Reserve the upper bank's last 64 KiB for 8192 block descriptors. */\n"
        "    PUSH32(esp, eax == 0x04000000u ? 8192u : 0xFA0u);\n"
        "    PUSH32(esp, eax == 0x04000000u ? 0x07FF0000u : 0x7AD608u);\n"
        "    PUSH32(esp, eax == 0x04000000u ? 0x03FF0000u : 0x1680000u);"),
)


# Extend the permanent-property array only when the mod property option is enabled.
PATCHES += (
    GeneratedPatch('Mod permanent-property address 001EB11A',
        'loc_001EB11A: ;\n    eax = MEM32(0x30EE60);\n    ebx = MEM32(esp + 0x10);\n    ecx = eax * 8 + 0x4434E0;',
        'loc_001EB11A: ;\n    eax = MEM32(0x30EE60);\n    ebx = MEM32(esp + 0x10);\n    { uint32_t recomp_world_property_address(uint32_t);\n      ecx = recomp_world_property_address(eax); }'),
    GeneratedPatch('Mod permanent-property address 001EE238',
        'loc_001EE238: ;\n    eax = MEM32(0x30EE60);\n    edx = MEM32(esp + 0xC);\n    ecx = eax * 8 + 0x4434E0;',
        'loc_001EE238: ;\n    eax = MEM32(0x30EE60);\n    edx = MEM32(esp + 0xC);\n    { uint32_t recomp_world_property_address(uint32_t);\n      ecx = recomp_world_property_address(eax); }'),
    GeneratedPatch('Mod permanent-property address 001EEC2C',
        'loc_001EEC2C: ;\n    eax = MEM32(0x30EE60);\n    eax = eax * 8 + 0x4434E0;',
        'loc_001EEC2C: ;\n    eax = MEM32(0x30EE60);\n    { uint32_t recomp_world_property_address(uint32_t);\n      eax = recomp_world_property_address(eax); }'),
    GeneratedPatch('Permanent-property write bounds',
        '    ecx = ecx + eax * 8;\n    PUSH32(esp, 0); sub_001EA730(); /* call 0x001EA730 */\n\nloc_001EC8E3: ;',
        '    ecx = ecx + eax * 8;\n    { void recomp_world_property_check_write(uint32_t);\n      recomp_world_property_check_write(ecx); }\n    PUSH32(esp, 0); sub_001EA730(); /* call 0x001EA730 */\n\nloc_001EC8E3: ;'),
)

PATCHES += (GeneratedPatch("Acknowledgements compact row spacing", 'loc_000E6835: ;\n    MEMF(esp + 0x10) = (float)fp_top(); fp_pop(); /* fstp */', 'loc_000E6835: ;\n    MEMF(esp + 0x10) = (float)fp_top(); fp_pop(); /* fstp */\n    /* Keep sixteen acknowledgement rows within the original credits area. */\n    { int recomp_acknowledgements_active(void);\n      if (recomp_acknowledgements_active()) MEMF(esp + 0x10) *= 12.0f / 13.0f; }'),)

# The 64th normal shop entry overflows the shared config index with its sentinel.
PATCHES += (
    GeneratedPatch('Merchant shop parser private line storage',
        'loc_000CE672: ;\n    ecx = esp + 0x18;',
        'loc_000CE672: ;\n    { void recomp_shop_config_storage(uint32_t, uint32_t, uint32_t);\n      recomp_shop_config_storage(esp + 0x150, MEM32(esp + 0x1C), MEM32(esp + 0x20)); }\n    ecx = esp + 0x18;'),
    GeneratedPatch('Merchant shop parser storage release',
        'loc_000CE6A6: ;\n    PUSH32(esp, edi);',
        'loc_000CE6A6: ;\n    { void recomp_shop_config_release(uint32_t);\n      recomp_shop_config_release(esp + 0x150); }\n    PUSH32(esp, edi);'),
)

# Route only the shell theme through Music before the bank creates any voices.
PATCHES += (
    GeneratedPatch('Main menu theme uses the Music volume category',
        '    RECOMP_TRACE_FUNC(0x00280141u);\n',
        '    RECOMP_TRACE_FUNC(0x00280141u);\n'
        '    { void recomp_fix_menu_music_bank(uint32_t, uint32_t);\n'
        '      recomp_fix_menu_music_bank(MEM32(esp + 4), MEM32(esp + 8)); }\n'),
)

PATCHES += (
    GeneratedPatch('Custom menu background fits rendered labels',
        '    MEMF(esp + 0x4C) = xmm0; /* movss */\n    PUSH32(esp, 0); sub_0020A1C0(); /* call 0x0020A1C0 */\n',
        '    MEMF(esp + 0x4C) = xmm0; /* movss */\n'
        '    { float recomp_custom_menu_width(uint32_t); MEMF(esp+8u)=recomp_custom_menu_width(esi); }\n'
        '    PUSH32(esp, 0); sub_0020A1C0(); /* call 0x0020A1C0 */\n'),
    GeneratedPatch('Custom menu highlight fits rendered labels',
        '    PUSH32(esp, 0xC0800000u);\n    ecx = esi;\n    PUSH32(esp, 0); sub_0020A1C0(); /* call 0x0020A1C0 */\n\nloc_000E6BB5:',
        '    PUSH32(esp, 0xC0800000u);\n    ecx = esi;\n'
        '    { float recomp_custom_menu_width(uint32_t); MEMF(esp+8u)=recomp_custom_menu_width(esi); }\n'
        '    PUSH32(esp, 0); sub_0020A1C0(); /* call 0x0020A1C0 */\n\nloc_000E6BB5:'),
)

PATCHES += (
    GeneratedPatch('Observe crouch blend before carry pickup replaces the animation',
        'void sub_0004CFA0(void)\n{\n    RECOMP_TRACE_FUNC(0x0004CFA0u);',
        'void sub_0004CFA0(void)\n{\n'
        '    int recomp_crouch_pickup_transition(uint32_t,uint32_t);\n'
        '    const int crouch_pickup=recomp_crouch_pickup_transition(ecx,MEM32(esp+4u));\n'
        '    RECOMP_TRACE_FUNC(0x0004CFA0u);'),
    GeneratedPatch('Complete crouch pickup through the retail carry-state exit',
        'loc_0004CFC3: ;\n    POP32(esp, esi);',
        'loc_0004CFC3: ;\n'
        '    if(crouch_pickup){\n'
        '        uint32_t _icall_esp=g_esp;\n'
        '        uint32_t normal_state=MEM32(MEM32(esi)+0x334u);\n'
        '        ecx=esi; PUSH32(esp,0); PUSH32(esp,0);\n'
        '        RECOMP_ICALL_SAFE(normal_state,_icall_esp);\n'
        '    }\n'
        '    POP32(esp, esi);'),
)


# Keep failed-voice continuations scoped to the actual protected Lua call.
# Defining the queue alone is insufficient: a regenerated wrapper must enter,
# finish and cancel that scope before any bouncer fallback can use it.
PATCHES += (
    GeneratedPatch("Lua protected call opens voice continuation scope",
        "loc_001135E0: ;\n    eax = MEM32(esp + 0xC);",
        "loc_001135E0: ;\n"
        "    uint64_t recomp_voice_call_begin(void);\n"
        "    void recomp_voice_call_end(uint64_t, int);\n"
        "    void recomp_lua_pcall_checkpoint(uint32_t, uint32_t, uint32_t);\n"
        "    const uint64_t _voice_mark = recomp_voice_call_begin();\n"
        "    const uint32_t _voice_description = MEM32(esp + 0x10);\n"
        "    int _voice_success = 0;\n"
        "    eax = MEM32(esp + 0xC);"),
    GeneratedPatch("Lua protected call records result before error cleanup",
        "loc_001135F7: ;\n    esp = esp + 0x10;",
        "loc_001135F7: ;\n"
        "    _voice_success = eax == 0;\n"
        "    recomp_lua_pcall_checkpoint(esi, eax, _voice_description);\n"
        "    esp = esp + 0x10;"),
    GeneratedPatch("Lua protected call drains voice continuations after cleanup",
        "loc_00113619: ;\n    POP32(esp, esi);",
        "loc_00113619: ;\n"
        "    recomp_voice_call_end(_voice_mark, _voice_success);\n"
        "    POP32(esp, esi);"),
    GeneratedPatch("Lua teardown cancels deferred voice continuations",
        "loc_001138F0: ;\n    PUSH32(esp, esi);",
        "loc_001138F0: ;\n"
        "    { void recomp_voice_cancel_owner(uint32_t);\n"
        "      recomp_voice_cancel_owner(ecx); }\n"
        "    PUSH32(esp, esi);"),
    GeneratedPatch("HQ voice request records managed handle",
        "loc_0011EB66: ;\n    (void)0; /* test edi, edi - flags set for next jcc */",
        "loc_0011EB66: ;\n"
        "    { void recomp_voice_request_checkpoint(uint32_t,uint32_t,uint32_t,uint32_t);\n"
        "      recomp_voice_request_checkpoint(esi, edi, ebp, ebx); }\n"
        "    (void)0; /* test edi, edi - flags set for next jcc */"),
)


PATCHES += (
    GeneratedPatch("Recover failed HQ initialization after its thread deactivates",
        "loc_00175892: ;\n    POP32(esp, edi);",
        "loc_00175892: ;\n"
        "    { void recomp_hq_recover_script_error(void); recomp_hq_recover_script_error(); }\n"
        "    POP32(esp, edi);"),
    GeneratedPatch("Recover failed HQ event setup after event iteration",
        "loc_00111E27: ;\n    POP32(esp, edi);",
        "loc_00111E27: ;\n"
        "    { void recomp_hq_recover_script_error(void); recomp_hq_recover_script_error(); }\n"
        "    POP32(esp, edi);"),
)



# The shared JLE at 0x285EA6 consumes CMP eax,8 on the stop path,
# but CMP eax,5 on the preparation path. Keep each predecessor's predicate.
PATCHES += (
    GeneratedPatch("Preserve XACT stopping-state comparison at shared branch",
        "loc_00285E77: ;\n"
        "    (void)0; /* cmp eax, 8 - flags set for next jcc */\n"
        "    goto loc_00285EA6;",
        "loc_00285E77: ;\n"
        "    if (CMP_LE(eax, 8)) goto loc_00285E96; /* already stopping/stopped */\n"
        "    goto loc_00285EA8;"),
)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("generated_root", type=Path)
    parser.add_argument(
        "--allow-missing",
        action="store_true",
        help="skip patch sites absent from an older known-good generated snapshot",
    )
    args = parser.parse_args()
    for name, path in patch_generated(
        args.generated_root, allow_missing=args.allow_missing
    ):
        print(f"Applied generated patch: {name}: {path}")



PATCHES += (
    GeneratedPatch('Track failed XACT stop registration', 'loc_00226D68: ;\n    eax = MEM32(esi + 4);', 'loc_00226D68: ;\n    { void recomp_xact_stop_subscription(uint32_t,uint32_t,uint32_t);\n      recomp_xact_stop_subscription(esi, esp + 0x24u, eax); }\n    eax = MEM32(esi + 4);'),
    GeneratedPatch('Clear failed subscription before wrapper release', 'loc_00226830: ;\n    eax = MEM32(esp + 4);', 'loc_00226830: ;\n    { void recomp_xact_forget_subscription(uint32_t); recomp_xact_forget_subscription(MEM32(esp + 4)); }\n    eax = MEM32(esp + 4);'),
    GeneratedPatch('Private guard notification allocation failure', 'loc_00283E62: ;\n    PUSH32(esp, 0x64848011);', 'loc_00283E62: ;\n    { int recomp_test_fail_guard_notification(uint32_t);\n      if (recomp_test_fail_guard_notification(ebx)) { eax=0; goto loc_00283E6E; } }\n    PUSH32(esp, 0x64848011);'),
)



PATCHES += (
    GeneratedPatch("XMem audio heap fallback with original allocator first",
        "loc_001787C0: ;\n    g_seh_ebp = ebp; sub_00229343(); return; /* tail jmp 0x00229343 */",
        "loc_001787C0: ;\n"
        "    { uint32_t recomp_audio_pool_fallback(uint32_t,uint32_t);\n"
        "      int recomp_test_audio_alloc_failure(uint32_t);\n"
        "      const uint32_t size=MEM32(esp+4), attributes=MEM32(esp+8);\n"
        "      g_seh_ebp = ebp;\n"
        "      if (recomp_test_audio_alloc_failure(attributes)) { eax=0; esp+=12; }\n"
        "      else sub_00229343();\n"
        "      if (!eax) eax=recomp_audio_pool_fallback(size,attributes);\n"
        "      return; }"),
    GeneratedPatch("XMem free returns audio fallback to its owning pool",
        "loc_001787D0: ;\n    g_seh_ebp = ebp; sub_002293E3(); return; /* tail jmp 0x002293E3 */",
        "loc_001787D0: ;\n"
        "    { int recomp_audio_pool_free(uint32_t);\n"
        "      if (recomp_audio_pool_free(MEM32(esp+4))) { esp+=12; return; } }\n"
        "    g_seh_ebp = ebp; sub_002293E3(); return; /* tail jmp 0x002293E3 */"),
)

PATCHES += (
    GeneratedPatch("Jennifer fringe before transparent vehicle glass",
        "    MEM32(0x7ACDD0) = ebp;\n    MEM32(esp + 0x10) = eax;",
        "    MEM32(0x7ACDD0) = ebp;\n"
        "    if (ebp == 2u) { void recomp_render_hair_before_glass(void); recomp_render_hair_before_glass(); }\n"
        "    MEM32(esp + 0x10) = eax;"),
)

PATCHES += (
    GeneratedPatch("Recheck prepared XACT pointer after queue drain",
        "loc_00226CBC: ;\n    edx = MEM32(esi + 0x18);",
        "loc_00226CBC: ;\n"
        "    ebx = MEM32(esi + 8u) ? 4u : 0u; /* DoWork may retire the prepared cue */\n"
        "    edx = MEM32(esi + 0x18);"),
    GeneratedPatch("Invalidate retired prepared XACT cues before allocation reuse",
        "loc_00284658: ;\n    if (TEST_Z(MEM8(esp + 8), 1))",
        "loc_00284658: ;\n"
        "    { void recomp_xact_retire_prepared_cue(uint32_t); recomp_xact_retire_prepared_cue(esi); }\n"
        "    if (TEST_Z(MEM8(esp + 8), 1))"),
)


PATCHES += (GeneratedPatch("Carry fractional main-loop clock ticks",
    '    fp_top() *= MEMD(0x30ECC0); /* fmul memory */\n    PUSH32(esp, 0); sub_002375B4(); /* call 0x002375B4 */',
    '    fp_top() *= MEMD(0x30ECC0); /* fmul memory */\n    { double recomp_frame_ticks_with_remainder(double);\n      fp_top() = recomp_frame_ticks_with_remainder(fp_top()); }\n    PUSH32(esp, 0); sub_002375B4(); /* call 0x002375B4 */'),)


PATCHES += (
    GeneratedPatch('Recomp FOV on gameplay camera only',
        '    fp_push(MEMF(esp + 0x1C)); /* fld float */\n    recomp_xmm_loadss(xmm0v, esp + 0x14);',
        '    { float recomp_options_camera_fov(uint32_t,float);\n      MEMF(esp + 0x1Cu) = recomp_options_camera_fov(ecx, MEMF(esp + 0x1Cu)); }\n    fp_push(MEMF(esp + 0x1C)); /* fld float */\n    recomp_xmm_loadss(xmm0v, esp + 0x14);'),
    GeneratedPatch('Recomp FOV slider after menu highlight',
        '     recomp_controls_paint_binding_icons(esi,binding_start_y,binding_row_height);}\n    ecx = esi;',
        '     recomp_controls_paint_binding_icons(esi,binding_start_y,binding_row_height);}\n    { void recomp_options_paint_fov_slider(uint32_t,float,float);\n      recomp_options_paint_fov_slider(esi,binding_start_y,binding_row_height); }\n    ecx = esi;'),
)


PATCHES += (
    GeneratedPatch('Preserve alternate Jennifer backpack visibility 00057626',
        'loc_00057626: ;\n    eax = MEM32(ecx);\n    { uint32_t _icall_esp = g_esp;\n    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(eax + 0x1D0), _icall_esp); /* indirect call */\n    }\n',
        'loc_00057626: ;\n    eax = MEM32(ecx);\n    { uint32_t recomp_original_bug_accessory_show_slot(uint32_t,uint32_t);\n      uint32_t show_slot = recomp_original_bug_accessory_show_slot(MEM32(ebp + 0x58u), MEM32(ecx + 0x58u));\n      uint32_t _icall_esp = g_esp;\n    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(eax + show_slot), _icall_esp); /* indirect call */\n    }\n'),
    GeneratedPatch('Preserve alternate Jennifer backpack visibility 00057B0D',
        'loc_00057B0D: ;\n    ecx = eax;\n    eax = MEM32(ecx);\n    { uint32_t _icall_esp = g_esp;\n    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(eax + 0x1D0), _icall_esp); /* indirect call */\n    }\n',
        'loc_00057B0D: ;\n    ecx = eax;\n    eax = MEM32(ecx);\n    { uint32_t recomp_original_bug_accessory_show_slot(uint32_t,uint32_t);\n      uint32_t show_slot = recomp_original_bug_accessory_show_slot(MEM32(ebx + 0x58u), MEM32(ecx + 0x58u));\n      uint32_t _icall_esp = g_esp;\n    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(eax + show_slot), _icall_esp); /* indirect call */\n    }\n'),
    GeneratedPatch('Preserve alternate Jennifer backpack visibility 00059D98',
        'loc_00059D98: ;\n    edx = MEM32(edi);\n    ecx = edi;\n    { uint32_t _icall_esp = g_esp;\n    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(edx + 0x1D0), _icall_esp); /* indirect call */\n    }\n',
        'loc_00059D98: ;\n    edx = MEM32(edi);\n    ecx = edi;\n    { uint32_t recomp_original_bug_accessory_show_slot(uint32_t,uint32_t);\n      uint32_t show_slot = recomp_original_bug_accessory_show_slot(MEM32(esi + 0x58u), MEM32(edi + 0x58u));\n      uint32_t _icall_esp = g_esp;\n    PUSH32(esp, 0); RECOMP_ICALL_SAFE(MEM32(edx + show_slot), _icall_esp); /* indirect call */\n    }\n'),
)


PATCHES += (
    GeneratedPatch('Money HUD counter cadence locals',
        'void sub_000F5300(void)\n{\n    int _flags = 0;',
        'void sub_000F5300(void)\n{\n    float money_counter_dt;\n    int money_counter_cue;\n    int _flags = 0;'),
    GeneratedPatch('Money HUD counter cadence gate',
        '    eax = MEM32(esi + 0x44);\n    esp = esp + 4;\n    if (CMP_EQ(eax, edi)) goto loc_000F5381;',
        '    eax = MEM32(esi + 0x44);\n    esp = esp + 4;\n    { float recomp_money_counter_dt(uint32_t, uint32_t, float, float, int *);\n      money_counter_dt = recomp_money_counter_dt(esi, MEM32(esi + 0x58),\n          MEMF(0x413F98), MEMF(esp + 0x14), &money_counter_cue); }\n    if (money_counter_dt <= 0.0f) goto loc_000F5435;\n    if (CMP_EQ(eax, edi)) goto loc_000F5381;'),
    GeneratedPatch('Money HUD accumulated counter delta',
        '    xmm0 = (float)(int32_t)eax; /* cvtsi2ss */\n    xmm0 = xmm0 * MEMF(esp + 0x14); /* mulss */',
        '    xmm0 = (float)(int32_t)eax; /* cvtsi2ss */\n    xmm0 = xmm0 * money_counter_dt; /* accumulated counter delta */'),
    GeneratedPatch('Money HUD timed cue 000F53AE',
        'loc_000F53AE: ;\n    edx = MEM32(esi + 0x58);\n    edx = edx & 0x80000001u;\n    if (((int32_t)edx >= 0)) goto loc_000F53BE; /* jns: not sign (positive) */\n\nloc_000F53B9: ;\n    edx--;\n    edx = edx | 0xFFFFFFFEu;\n    edx++;\n\nloc_000F53BE: ;\n    if ((edx != 0)) goto loc_000F53CF; /* jne: not equal / not zero */\n\n',
        'loc_000F53AE: ;\n    if (!money_counter_cue) goto loc_000F53CF;\n\n'),
    GeneratedPatch('Money HUD timed cue 000F53FB',
        'loc_000F53FB: ;\n    eax = MEM32(esi + 0x58);\n    eax = eax & 0x80000001u;\n    if (((int32_t)eax >= 0)) goto loc_000F540A; /* jns: not sign (positive) */\n\nloc_000F5405: ;\n    eax--;\n    eax = eax | 0xFFFFFFFEu;\n    eax++;\n\nloc_000F540A: ;\n    if ((eax != 0)) goto loc_000F541B; /* jne: not equal / not zero */\n\n',
        'loc_000F53FB: ;\n    if (!money_counter_cue) goto loc_000F541B;\n\n'),
)

# Keep opt-in mod extensions together; the common patcher validates each site.
import runpy as _runpy
PATCHES += tuple(GeneratedPatch(*entry) for entry in _runpy.run_path(
    str(Path(__file__).with_name("Mod-Extensions-Patches.py")))["PATCHES"])

if __name__ == "__main__":
    main()
