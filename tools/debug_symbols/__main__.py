"""
CLI entry point for debug-build source-file symbol recovery.

Usage:
    py -3 -m tools.debug_symbols --functions build/disasm/functions.json \
                                 --strings   build/disasm/strings.json \
                                 -o build/disasm/debug_symbols.json -v
    py -3 -m tools.debug_symbols ... --apply
"""

import argparse
import sys

from .recover import run


def main():
    parser = argparse.ArgumentParser(
        description="Recover function source-file attribution from a debug "
                    "build's __FILE__ assert strings"
    )
    parser.add_argument(
        "--functions",
        default="tools/disasm/output/functions.json",
        help="Path to functions.json (default: tools/disasm/output/functions.json)",
    )
    parser.add_argument(
        "--strings",
        default="tools/disasm/output/strings.json",
        help="Path to strings.json (default: tools/disasm/output/strings.json)",
    )
    parser.add_argument(
        "--output", "-o",
        help="Write the recovered name/attribution map here",
    )
    parser.add_argument(
        "--apply",
        action="store_true",
        help="Merge recovered names into functions.json in place (keeps a .bak)",
    )
    parser.add_argument(
        "--include-headers",
        action="store_true",
        help="Also attribute from .h/.hpp paths (asserts inside inline functions)",
    )
    parser.add_argument(
        "--no-interpolate",
        action="store_true",
        help="Only use direct string references; skip object-contiguity fill",
    )
    parser.add_argument(
        "--verbose", "-v",
        action="store_true",
        help="Print detailed progress",
    )

    args = parser.parse_args()

    if not args.output and not args.apply:
        parser.error("nothing to do: pass --output, --apply, or both")

    try:
        summary = run(
            functions_path=args.functions,
            strings_path=args.strings,
            output_path=args.output,
            apply_to=args.functions if args.apply else None,
            include_headers=args.include_headers,
            interpolate=not args.no_interpolate,
            verbose=args.verbose,
        )
    except FileNotFoundError as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)

    print(
        "Attributed {attributed_total}/{total_functions} functions "
        "({coverage_percent}%) across {source_paths} source files "
        "[{attributed_direct} direct, {attributed_interpolated} interpolated]".format(
            **summary
        )
    )


if __name__ == "__main__":
    main()
