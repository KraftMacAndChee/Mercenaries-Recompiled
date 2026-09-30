# Export Xbox XBE analysis from Ghidra (headless post-script).
#
# Exports up to three JSON files into an output directory:
#   functions.json   - every function: address/name/signature/calling_convention/
#                       param_count/is_thunk/namespace
#   symbols.json      - symbol table: address/name/type/namespace/source
#   decompiled.json   - per-function decompiled C (OPTIONAL, bounded)
#
# This is a Jython (Python 2.7) GhidraScript. Run via analyzeHeadless -postScript.
#
# Script args (all optional, positional):
#   arg0  outDir        : output directory (default: program executable dir or cwd)
#   arg1  doDecompile   : "decompile" to enable decompilation, "nodecompile" to skip
#                         (default: nodecompile)
#   arg2  decompLimit   : max functions to decompile this run, integer or "all"
#                         (default: 4000)
#   arg3  decompTimeout : per-function decompiler timeout seconds (default: 30)
#
# Decompilation is resumable: it appends to decompiled.json and records progress
# in decompiled_progress.json (set of addresses already done). Re-running with the
# same outDir continues where it left off until all functions are decompiled.
#
# @category XboxRecomp
# @runtime Jython

import json
import os

from ghidra.program.model.symbol import SourceType, SymbolType
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor


def hexaddr(addr):
    # Ghidra Address -> "0x%08X" matching recompiler functions.json `start`.
    try:
        return "0x%08X" % addr.getOffset()
    except Exception:
        return str(addr)


def get_namespace_str(symbol_or_func):
    try:
        ns = symbol_or_func.getParentNamespace()
        if ns is None:
            return ""
        name = ns.getName(True)  # fully qualified
        if name in ("Global", None):
            return ""
        return name
    except Exception:
        return ""


def export_functions(program, out_dir):
    fm = program.getFunctionManager()
    funcs = fm.getFunctions(True)  # forward order
    out = []
    count = 0
    for f in funcs:
        try:
            entry = f.getEntryPoint()
            sig = ""
            try:
                sig = f.getPrototypeString(False, False)
            except Exception:
                sig = f.getSignature().getPrototypeString()
            cc = ""
            try:
                cc = f.getCallingConventionName()
            except Exception:
                cc = ""
            try:
                pcount = f.getParameterCount()
            except Exception:
                pcount = -1
            rec = {
                "address": hexaddr(entry),
                "name": f.getName(),
                "signature": sig,
                "calling_convention": cc if cc else "",
                "param_count": pcount,
                "is_thunk": bool(f.isThunk()),
                "namespace": get_namespace_str(f),
            }
            out.append(rec)
            count += 1
        except Exception as e:
            print("  [func export warn] %s" % str(e))
    path = os.path.join(out_dir, "functions.json")
    fh = open(path, "w")
    try:
        json.dump(out, fh, indent=1)
    finally:
        fh.close()
    print("Exported %d functions -> %s" % (count, path))
    return count


SYMTYPE_NAMES = {}


def symbol_type_name(sym):
    try:
        st = sym.getSymbolType()
        return str(st)  # e.g. "Function", "Label", "Class", "Parameter", ...
    except Exception:
        return "Unknown"


def export_symbols(program, out_dir):
    st = program.getSymbolTable()
    it = st.getAllSymbols(True)  # include dynamic? True = include primary; iterate all
    out = []
    count = 0
    for sym in it:
        try:
            addr = sym.getAddress()
            if addr is None:
                continue
            rec = {
                "address": hexaddr(addr),
                "name": sym.getName(),
                "type": symbol_type_name(sym),
                "namespace": get_namespace_str(sym),
                "source": str(sym.getSource()),  # USER_DEFINED, IMPORTED, ANALYSIS, DEFAULT
                "primary": bool(sym.isPrimary()),
            }
            out.append(rec)
            count += 1
        except Exception as e:
            pass
    path = os.path.join(out_dir, "symbols.json")
    fh = open(path, "w")
    try:
        json.dump(out, fh, indent=1)
    finally:
        fh.close()
    print("Exported %d symbols -> %s" % (count, path))
    return count


def load_progress(out_dir):
    p = os.path.join(out_dir, "decompiled_progress.json")
    if os.path.exists(p):
        try:
            fh = open(p, "r")
            try:
                done = set(json.load(fh))
            finally:
                fh.close()
            return done
        except Exception:
            return set()
    return set()


def save_progress(out_dir, done):
    p = os.path.join(out_dir, "decompiled_progress.json")
    fh = open(p, "w")
    try:
        json.dump(sorted(list(done)), fh)
    finally:
        fh.close()


def append_decompiled(out_dir, records):
    # Append records to decompiled.json (kept as a JSON array, rewritten each
    # checkpoint for validity). We hold all in memory + on disk progress so
    # restart is safe.
    path = os.path.join(out_dir, "decompiled.json")
    existing = []
    if os.path.exists(path):
        try:
            fh = open(path, "r")
            try:
                existing = json.load(fh)
            finally:
                fh.close()
        except Exception:
            existing = []
    existing.extend(records)
    fh = open(path, "w")
    try:
        json.dump(existing, fh, indent=1)
    finally:
        fh.close()
    return len(existing)


def export_decompiled(program, out_dir, limit, timeout_secs):
    monitor = ConsoleTaskMonitor()
    decomp = DecompInterface()
    try:
        decomp.openProgram(program)
    except Exception as e:
        print("Decompiler open failed: %s" % str(e))
        return 0

    fm = program.getFunctionManager()
    done = load_progress(out_dir)
    print("Decompile: %d already done (resuming)" % len(done))

    batch = []
    processed = 0
    checkpoint_every = 200

    funcs = fm.getFunctions(True)
    for f in funcs:
        if limit is not None and processed >= limit:
            print("Decompile: reached limit %d for this run." % limit)
            break
        entry = hexaddr(f.getEntryPoint())
        if entry in done:
            continue
        if f.isThunk():
            # Thunks decompile to trivial bodies; still record cheaply.
            done.add(entry)
            batch.append({"address": entry, "name": f.getName(),
                          "decompiled_c": "/* thunk */"})
            processed += 1
            continue
        try:
            res = decomp.decompileFunction(f, timeout_secs, monitor)
            c = ""
            if res is not None and res.decompileCompleted():
                df = res.getDecompiledFunction()
                if df is not None:
                    c = df.getC()
            if not c:
                c = "/* decompile failed: %s */" % (
                    res.getErrorMessage() if res is not None else "no result")
            batch.append({"address": entry, "name": f.getName(),
                          "decompiled_c": c})
            done.add(entry)
            processed += 1
        except Exception as e:
            batch.append({"address": entry, "name": f.getName(),
                          "decompiled_c": "/* exception: %s */" % str(e)})
            done.add(entry)
            processed += 1

        if len(batch) >= checkpoint_every:
            total = append_decompiled(out_dir, batch)
            save_progress(out_dir, done)
            print("  decompiled checkpoint: +%d (total on disk %d)" %
                  (len(batch), total))
            batch = []

    if batch:
        total = append_decompiled(out_dir, batch)
        save_progress(out_dir, done)
        print("  decompiled final flush: +%d (total on disk %d)" %
              (len(batch), total))

    decomp.dispose()
    print("Decompile: processed %d this run; %d total done." %
          (processed, len(done)))
    return processed


def run():
    program = getCurrentProgram()
    args = getScriptArgs()

    # Defaults
    out_dir = None
    do_decompile = False
    decomp_limit = 4000
    decomp_timeout = 30

    if len(args) >= 1 and args[0]:
        out_dir = args[0]
    if len(args) >= 2 and args[1]:
        do_decompile = (args[1].lower() == "decompile")
    if len(args) >= 3 and args[2]:
        if args[2].lower() == "all":
            decomp_limit = None
        else:
            try:
                decomp_limit = int(args[2])
            except Exception:
                decomp_limit = 4000
    if len(args) >= 4 and args[3]:
        try:
            decomp_timeout = int(args[3])
        except Exception:
            decomp_timeout = 30

    if not out_dir:
        # Fall back to CWD-based default.
        out_dir = os.path.join(os.getcwd(), "ghidra_export")
    if not os.path.exists(out_dir):
        os.makedirs(out_dir)

    print("=== ExportXbeNames ===")
    print("Program:   %s" % program.getName())
    print("Out dir:   %s" % out_dir)
    print("Decompile: %s (limit=%s, timeout=%ss)" %
          (do_decompile, "all" if decomp_limit is None else decomp_limit,
           decomp_timeout))

    export_functions(program, out_dir)
    export_symbols(program, out_dir)

    if do_decompile:
        export_decompiled(program, out_dir, decomp_limit, decomp_timeout)
    else:
        print("Decompilation skipped (pass 'decompile' as arg1 to enable).")

    print("=== Export complete ===")


run()
