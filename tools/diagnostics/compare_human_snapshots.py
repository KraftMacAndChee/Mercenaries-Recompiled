#!/usr/bin/env python3
"""Compare retail human actor state in two guest-RAM snapshots.

Inputs may be raw 64 MiB guest snapshots or full Windows minidumps. This tool
is read-only and knows only the retail fields needed for update-stall diagnosis.
"""
from __future__ import annotations

import argparse
from collections import Counter
import math
import mmap
import struct
from pathlib import Path

GUEST_SIZE = 0x04000000
NATIVE_GUEST_BASE = 0x10000
HUMAN_VTABLES = {0x002E2CE0: "npc-human", 0x002E32B8: "player-human"}
ACTOR_COMPARE_SIZE = 0xB00
# Retail human-actor layout used for snapshot comparison.
# +0x79C is the carried-passenger pointer and
# +0x7A0 is the equipped-primary-item pointer; neither is animation state.
ANIM_POINTER_OFFSET = 0x6B0
FROZEN_COUNT_OFFSET = 0x6C0
FROZEN_STATE_OFFSET = 0x6D0
UPDATE_OFFSET_OFFSET = 0x6D4
UPPER_COUNTER_OFFSET = 0x6D8
LOWER_COUNTER_OFFSET = 0x6DC
AI_PHYSICS_DISABLED_OFFSET = 0x814


def u32(data, offset):
    return struct.unpack_from("<I", data, offset)[0]


def minidump_guest(path):
    with path.open("rb") as stream:
        data = mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ)
        try:
            if data[:4] != b"MDMP":
                raise ValueError(f"{path} is not a minidump")
            directory = u32(data, 12)
            memory64 = None
            for index in range(u32(data, 8)):
                kind, _size, rva = struct.unpack_from("<III", data, directory + index * 12)
                if kind == 9:
                    memory64 = rva
                    break
            if memory64 is None:
                raise ValueError(f"{path} has no Memory64ListStream")
            count = struct.unpack_from("<Q", data, memory64)[0]
            file_rva = struct.unpack_from("<Q", data, memory64 + 8)[0]
            output = bytearray(GUEST_SIZE)
            copied = 0
            for index in range(count):
                start, size = struct.unpack_from("<QQ", data, memory64 + 16 + index * 16)
                end = start + size
                overlap_start = max(start, NATIVE_GUEST_BASE)
                overlap_end = min(end, NATIVE_GUEST_BASE + GUEST_SIZE)
                if overlap_start < overlap_end:
                    source = file_rva + overlap_start - start
                    target = overlap_start - NATIVE_GUEST_BASE
                    amount = overlap_end - overlap_start
                    output[target:target + amount] = data[source:source + amount]
                    copied += amount
                file_rva += size
            if copied != GUEST_SIZE:
                raise ValueError(f"only {copied:#x} of guest RAM is present in {path}")
            return bytes(output)
        finally:
            data.close()


def load_guest(path):
    return path.read_bytes() if path.stat().st_size == GUEST_SIZE else minidump_guest(path)


def census(memory):
    result = {}
    for actor in range(0x00800000, GUEST_SIZE - ACTOR_COMPARE_SIZE, 16):
        vtable = u32(memory, actor)
        if vtable not in HUMAN_VTABLES:
            continue
        position = struct.unpack_from("<3f", memory, actor + 0xE0)
        if not all(math.isfinite(value) and abs(value) < 1000000 for value in position):
            continue
        anim = u32(memory, actor + ANIM_POINTER_OFFSET)
        if anim and not 0x10000 <= anim < GUEST_SIZE:
            continue
        result[actor] = (vtable, u32(memory, actor + 4), position, anim)
    return result


def changed_words(before, after, address, size):
    return [offset for offset in range(0, size, 4)
            if before[address + offset:address + offset + 4]
            != after[address + offset:address + offset + 4]]


def offsets(values, limit=28):
    text = ",".join(f"+{value:03X}" for value in values[:limit])
    return text + (f",...(+{len(values)-limit} more)" if len(values) > limit else "") or "none"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("before", type=Path)
    parser.add_argument("after", type=Path)
    parser.add_argument("--anim-bytes", type=lambda value: int(value, 0), default=0x1000)
    parser.add_argument("--pointer-census", action="store_true",
                        help="summarize actor fields pointing at vtable-backed guest objects")
    parser.add_argument("--detail-actor", action="append", type=lambda value: int(value, 0),
                        default=[], help="print every changed actor word for this guest address")
    args = parser.parse_args()
    if not 0 <= args.anim_bytes <= 0x10000:
        raise SystemExit("--anim-bytes must be between 0 and 0x10000")
    print(f"Loading before: {args.before}")
    before = load_guest(args.before)
    print(f"Loading after:  {args.after}")
    after = load_guest(args.after)
    old, new = census(before), census(after)
    common = sorted(old.keys() & new.keys())
    print(f"Humans before={len(old)} after={len(new)} common={len(common)} "
          f"spawned={len(new.keys()-old.keys())} removed={len(old.keys()-new.keys())}")
    if args.pointer_census:
        rows = []
        for offset in range(0, ACTOR_COMPARE_SIZE, 4):
            targets = Counter()
            present = 0
            for actor in new:
                pointer = u32(after, actor + offset)
                if 0x10000 <= pointer <= GUEST_SIZE - 4:
                    vtable = u32(after, pointer)
                    if 0x002D0000 <= vtable <= 0x00310000:
                        present += 1
                        targets[vtable] += 1
            if present >= max(3, len(new) // 3):
                rows.append((present, offset, targets))
        print("Vtable-backed actor pointer fields:")
        for present, offset, targets in sorted(rows, reverse=True):
            summary = ",".join(f"{value:08X}:{count}" for value, count in targets.most_common(8))
            print(f"  offset=+{offset:03X} present={present}/{len(new)} targets={summary}")
    for actor in common:
        vtable, name_hash, position, anim = new[actor]
        old_position, old_anim = old[actor][2], old[actor][3]
        moved = math.sqrt(sum((a-b)**2 for a, b in zip(position, old_position)))
        actor_changes = changed_words(before, after, actor, ACTOR_COMPARE_SIZE)
        anim_changes = []
        if anim and anim == old_anim and anim + args.anim_bytes <= GUEST_SIZE:
            anim_changes = changed_words(before, after, anim, args.anim_bytes)
        old_frozen_count = before[actor + FROZEN_COUNT_OFFSET]
        frozen_count = after[actor + FROZEN_COUNT_OFFSET]
        old_frozen_state = u32(before, actor + FROZEN_STATE_OFFSET)
        frozen_state = u32(after, actor + FROZEN_STATE_OFFSET)
        update_offset = u32(after, actor + UPDATE_OFFSET_OFFSET)
        upper_counter = u32(after, actor + UPPER_COUNTER_OFFSET)
        lower_counter = u32(after, actor + LOWER_COUNTER_OFFSET)
        ai_disabled = after[actor + AI_PHYSICS_DISABLED_OFFSET]
        print(f"actor={actor:08X} kind={HUMAN_VTABLES[vtable]} hash={name_hash:08X} "
              f"pos=({position[0]:.2f},{position[1]:.2f},{position[2]:.2f}) moved={moved:.3f} "
              f"anim={old_anim:08X}->{anim:08X} "
              f"freeze={old_frozen_state}/{old_frozen_count}->{frozen_state}/{frozen_count} "
              f"ai_disabled={before[actor + AI_PHYSICS_DISABLED_OFFSET]}->{ai_disabled} "
              f"anim_counter={upper_counter}/{lower_counter} update_offset={update_offset} "
              f"actor_changes={len(actor_changes)} [{offsets(actor_changes)}] "
              f"anim_changes={len(anim_changes)} [{offsets(anim_changes)}]")
    for actor in sorted(new.keys() - old.keys()):
        vtable, name_hash, position, anim = new[actor]
        print(f"spawned actor={actor:08X} kind={HUMAN_VTABLES[vtable]} hash={name_hash:08X} "
              f"pos=({position[0]:.2f},{position[1]:.2f},{position[2]:.2f}) anim={anim:08X} "
              f"freeze={u32(after, actor + FROZEN_STATE_OFFSET)}/"
              f"{after[actor + FROZEN_COUNT_OFFSET]} "
              f"ai_disabled={after[actor + AI_PHYSICS_DISABLED_OFFSET]} "
              f"anim_counter={u32(after, actor + UPPER_COUNTER_OFFSET)}/"
              f"{u32(after, actor + LOWER_COUNTER_OFFSET)} "
              f"update_offset={u32(after, actor + UPDATE_OFFSET_OFFSET)}")
    for actor in args.detail_actor:
        if actor not in old or actor not in new:
            print(f"detail actor={actor:08X}: not present in both snapshots")
            continue
        print(f"detail actor={actor:08X}:")
        for offset in changed_words(before, after, actor, ACTOR_COMPARE_SIZE):
            old_value = u32(before, actor + offset)
            new_value = u32(after, actor + offset)
            print(f"  +{offset:03X}: {old_value:08X} -> {new_value:08X}")


if __name__ == "__main__":
    main()
