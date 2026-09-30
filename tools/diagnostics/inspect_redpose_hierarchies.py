"""Audit RedPose/Zephyr skeleton hierarchies in a 64 MiB guest RAM snapshot.

This is read-only.  It recognizes retail RedPose by its three-field layout and
checks the int8 parent/child/sibling graph for invalid indices,
cycles, duplicate visits, and traversal-stack growth.
"""

import argparse
import struct
from pathlib import Path


GUEST_SIZE = 0x04000000
LOWEST_POINTER = 0x00010000
JOINT_SIZE = 0x28


def u32(memory, address):
    return struct.unpack_from("<I", memory, address)[0]


def s8(value):
    return value - 256 if value >= 128 else value


def valid_pointer(address, size=4):
    return LOWEST_POINTER <= address <= GUEST_SIZE - size


def inspect_hierarchy(memory, joints, count):
    relatives = []
    invalid = []
    for index in range(count):
        offset = joints + index * JOINT_SIZE
        parent, child, sibling = (
            s8(memory[offset + 0x24]),
            s8(memory[offset + 0x25]),
            s8(memory[offset + 0x26]),
        )
        relatives.append((parent, child, sibling))
        for field, value in (("parent", parent), ("child", child),
                             ("sibling", sibling)):
            if value < -1 or value >= count:
                invalid.append((index, field, value))

    if invalid:
        return dict(relatives=relatives, invalid=invalid, visits=0,
                    duplicate=None, max_stack=0, overflow=False)

    root_child = relatives[0][1]
    stack = [] if root_child < 0 else [root_child]
    visited = set()
    visits = 0
    duplicate = None
    max_stack = len(stack)
    while stack and visits < 4096:
        joint = stack.pop()
        visits += 1
        if joint in visited:
            duplicate = joint
            break
        visited.add(joint)
        _parent, child, sibling = relatives[joint]
        if child >= 0:
            stack.append(child)
        if sibling >= 0:
            stack.append(sibling)
        max_stack = max(max_stack, len(stack))
    return dict(relatives=relatives, invalid=[], visits=visits,
                duplicate=duplicate, max_stack=max_stack,
                overflow=max_stack > 256 or visits >= 4096)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("snapshot", type=Path)
    parser.add_argument("--show-valid", action="store_true")
    args = parser.parse_args()
    memory = args.snapshot.read_bytes()
    if len(memory) != GUEST_SIZE:
        raise ValueError(f"Expected {GUEST_SIZE} bytes, got {len(memory)}")
    nonzero_bytes = len(memory) - memory.count(0)
    if nonzero_bytes < 4096:
        raise ValueError(
            f"Snapshot is effectively empty ({nonzero_bytes} nonzero bytes)"
        )

    poses = []
    for pose in range(LOWEST_POINTER, GUEST_SIZE - 12, 4):
        matrix = u32(memory, pose)
        count = u32(memory, pose + 4)
        skeleton = u32(memory, pose + 8)
        if not 1 <= count <= 127:
            continue
        if not valid_pointer(matrix, count * 0x40):
            continue
        if not valid_pointer(skeleton, 12):
            continue
        joints = u32(memory, skeleton)
        skeleton_count = u32(memory, skeleton + 8)
        if skeleton_count != count:
            continue
        if not valid_pointer(joints, count * JOINT_SIZE):
            continue
        result = inspect_hierarchy(memory, joints, count)
        poses.append((pose, matrix, skeleton, joints, count, result))

    suspicious = [row for row in poses if row[5]["invalid"] or
                  row[5]["duplicate"] is not None or row[5]["overflow"]]
    print(f"snapshot={args.snapshot} poses={len(poses)} suspicious={len(suspicious)}")
    rows = poses if args.show_valid else suspicious
    for pose, matrix, skeleton, joints, count, result in rows:
        print(
            f"pose={pose:08X} matrices={matrix:08X} skeleton={skeleton:08X} "
            f"joints={joints:08X} count={count} visits={result['visits']} "
            f"max_stack={result['max_stack']} duplicate={result['duplicate']} "
            f"invalid={result['invalid'][:8]} overflow={result['overflow']}"
        )


if __name__ == "__main__":
    main()
