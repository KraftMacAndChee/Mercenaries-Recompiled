# Xbox Static Recompilation Toolchain
# Usage: py -3 -m tools.<module> <args>
#   tools.xbe_parser    - Parse XBE header and sections
#   tools.disasm        - Disassemble and detect functions
#   tools.func_id       - Identify library functions (CRT, RenderWare, etc.)
#   tools.debug_symbols - Recover source-file names from a debug build's __FILE__ strings
#   tools.recomp        - Translate x86 to C (static recompiler)
