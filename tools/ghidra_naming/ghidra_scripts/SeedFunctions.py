# Seed Ghidra with the recompiler's known function entry points BEFORE analysis.
#
# The flat raw image has no symbol table, so Ghidra's auto-analysis only finds
# functions reachable from the entry point / references / prologue patterns.
# The project's own disassembler already identified 22,178 function starts in
# tools/disasm/output/functions.json. Seeding those exact addresses as functions
# lets Function ID + the decompiler operate on properly-bounded bodies at the
# addresses the recompiler keys on, maximizing name-transfer overlap.
#
# Run as a -preScript (BEFORE auto-analysis). Jython.
#
# Arg0 (optional): path to functions.json
#                  (default: <repo>/tools/disasm/output/functions.json)
#
# @category XboxRecomp
# @runtime Jython
import json
import os

from ghidra.program.model.address import AddressSet


def run():
    args = getScriptArgs()
    if len(args) >= 1 and args[0]:
        fj = args[0]
    else:
        # repo/tools/ghidra_naming/ghidra_scripts/.. -> repo/tools/disasm/...
        try:
            _here = os.path.dirname(os.path.abspath(__file__))
            _repo = os.path.dirname(os.path.dirname(os.path.dirname(_here)))
            fj = os.path.join(_repo, "tools", "disasm", "output", "functions.json")
        except NameError:
            fj = ""  # __file__ unavailable under headless; pass functions.json as arg0

    if not os.path.exists(fj):
        print("[seed] functions.json not found: %s (skipping seed)" % fj)
        return

    program = getCurrentProgram()
    mem = program.getMemory()
    af = program.getAddressFactory()
    space = af.getDefaultAddressSpace()
    listing = program.getListing()

    fh = open(fj, "r")
    try:
        funcs = json.load(fh)
    finally:
        fh.close()
    print("[seed] read %d function records" % len(funcs))

    created = 0
    already = 0
    skipped_mem = 0
    disas = 0
    n = 0
    for rec in funcs:
        n += 1
        s = rec.get("start")
        if not s:
            continue
        try:
            off = int(s, 16)
        except Exception:
            continue
        addr = space.getAddress(off)
        block = mem.getBlock(addr)
        if block is None or not block.isExecute() and not block.isInitialized():
            skipped_mem += 1
            continue
        # Only seed in initialized memory.
        if not block.isInitialized():
            skipped_mem += 1
            continue
        existing = listing.getFunctionAt(addr)
        if existing is not None:
            already += 1
            continue
        # Disassemble at the entry, then create a function (let analyzer fix body).
        try:
            if listing.getInstructionAt(addr) is None:
                d = disassemble(addr)
                if d is not None and not d.isEmpty():
                    disas += 1
            f = createFunction(addr, None)  # default name FUN_xxxx; FidDb may rename
            if f is not None:
                created += 1
        except Exception:
            pass
        if n % 4000 == 0:
            print("[seed] progress %d/%d (created=%d, existing=%d)" %
                  (n, len(funcs), created, already))

    print("[seed] done: created=%d, pre-existing=%d, skipped(no-init-mem)=%d, disasd=%d"
          % (created, already, skipped_mem, disas))


run()
