"""
CLI entry point for the XBE parser.

Usage:
    py -3 -m tools.xbe_parser game_files/default.xbe
    py -3 -m tools.xbe_parser game_files/default.xbe --json output/analysis.json
    py -3 -m tools.xbe_parser game_files/default.xbe --extract-sections output/sections/
"""

from .xbe_parser import main

main()
