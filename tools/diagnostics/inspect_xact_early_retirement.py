"""Resolve early-retired managed XACT cues without changing audio/game state.

A positive remaining timer is evidence to investigate, not proof of a bug:
authored interruptions and delayed starts must still be checked separately.
"""
import argparse
from pathlib import Path
import re


def name_hash(name):
    value = 2166136261
    for byte in name.lower().encode("ascii"):
        value = ((value ^ (byte | 0x20)) * 16777619) & 0xFFFFFFFF
    return value


def load_names(directory):
    names = {}
    for path in sorted(directory.glob("xbox*.sfx")):
        for name in re.findall(r'name\("([^"\r\n]+)"\)', path.read_text(errors="replace")):
            names.setdefault(name_hash(name), set()).add(name.lower())
    return names


def early_retirements(lines, minimum_remaining):
    for number, line in enumerate(lines, 1):
        if "[XACT-MANAGED-STALE]" not in line:
            continue
        fields = dict(re.findall(r"([\w-]+)=([^\s]+)", line))
        if fields.get("low-level") != "0":
            continue
        remaining = float(fields["remaining"])
        if remaining >= minimum_remaining:
            yield number, int(fields["name"], 16), remaining, fields


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--sound-source", required=True, type=Path)
    parser.add_argument("--minimum-remaining", type=float, default=0.5)
    args = parser.parse_args()
    names = load_names(args.sound_source)
    with args.log.open(errors="replace") as stream:
        for number, key, remaining, fields in early_retirements(stream, args.minimum_remaining):
            label = "/".join(sorted(names.get(key, {"<unresolved>"})))
            print(f"line={number} name={key:08X} {label} remaining={remaining:.6g}s "
                  f"cue={fields['cue']} handle={fields['sound-handle']} "
                  f"called={fields.get('called', '?')} stream={fields.get('stream', '?')}")


if __name__ == "__main__":
    main()
